/*
 * devx_qp.c  --  NVMe-oF IO 队列:DevX 自建 QP,队列放显存(见 devx_qp.h)
 *
 * 显存 umem 布局(2 MiB,一次分配,dma-buf offset 0 就是起点,坑 6):
 *   0x00000  CQ        cq_cnt × 64
 *   0x10000  CQ dbrec  8 字节
 *   0x10040  QP dbrec  [0]=RQ [1]=SQ
 *   0x20000  WQ        RQ(rq_cnt × 16)紧跟 SQ(sq_cnt × 64)
 * recv buffer、连接数据在主存(DevX 的 PD 上注册);胶囊区、数据区沿用
 * 主程序分配的那两块,在 DevX 的 PD 上重注册一份。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <rdma/rdma_cma.h>

#include "spdk/nvme.h"
#include "gpu_io_ctx.h"
#include "wqe_build.h"
#include "devx_ifc.h"
#include "devx_qp.h"

extern int gpu_alloc_exportable_ex(void **, int *, size_t, unsigned int);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_copy_to_dev(void *, const void *, size_t);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_sync(void);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define DQ_MEM_LEN	(2u * 1024 * 1024)
#define DQ_OFF_CQ	0x00000u
#define DQ_OFF_CQ_DBR	0x10000u
#define DQ_OFF_QP_DBR	0x10040u
#define DQ_OFF_WQ	0x20000u
#define DQ_RSP_STRIDE	64u
#define DQ_CONN_CID	0x7fffu

/* 只有一条 DevX 队列,状态放静态区 */
static struct {
	struct ibv_context		*ctx;
	struct ibv_pd			*pd;
	struct rdma_event_channel	*ech;
	struct rdma_cm_id		*cm;
	struct mlx5dv_devx_uar		*uar;
	struct mlx5dv_devx_umem		*umem;
	struct mlx5dv_devx_obj		*cq, *qp;
	uint8_t				*mem;		/* 显存 */
	int				mem_fd;
	uint32_t			cqn, qpn;
	uint32_t			cq_cnt, sq_cnt, rq_cnt;
	/* 接管时(Connect 之后)的计数,devx_q_check 用 */
	uint32_t			sq0, cq0, rq0;
} g;

static int
ilog2u(uint32_t v)
{
	int l = 0;

	while ((1u << l) < v) {
		l++;
	}
	return l;
}

static void
cmd_err(const char *what, const void *out)
{
	fprintf(stderr, "[DevX] %s 失败: errno=%d status=0x%x syndrome=0x%x\n",
		what, errno, DEVX_GET(create_cq_out, out, status),
		DEVX_GET(create_cq_out, out, syndrome));
}

static int
cm_wait(enum rdma_cm_event_type want, struct rdma_cm_event *copy)
{
	struct rdma_cm_event *ev;

	if (rdma_get_cm_event(g.ech, &ev)) {
		fprintf(stderr, "[CM] rdma_get_cm_event: %s\n", strerror(errno));
		return -1;
	}
	if (ev->event != want) {
		fprintf(stderr, "[CM] 等 %s,来的是 %s (status %d)\n",
			rdma_event_str(want), rdma_event_str(ev->event), ev->status);
		if (ev->event == RDMA_CM_EVENT_REJECTED &&
		    ev->param.conn.private_data_len >= 4) {
			const uint16_t *p = ev->param.conn.private_data;

			fprintf(stderr, "[CM] target 拒绝: recfmt %u sts %u "
				"(3 qid 不对,4/5 队列大小不对,6 没资源)\n", p[0], p[1]);
		}
		rdma_ack_cm_event(ev);
		return -1;
	}
	if (copy) {
		*copy = *ev;
	}
	rdma_ack_cm_event(ev);
	return 0;
}

/* RoCE 的对端 MAC:地址解析后内核邻居表里就有(/proc/net/arp) */
static int
arp_mac(const char *ip, uint8_t mac[6])
{
	char line[256], a[64], hw[64];
	unsigned v[6];
	FILE *f = fopen("/proc/net/arp", "r");
	int ok = -1;

	if (!f) {
		return -1;
	}
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%63s %*s %*s %63s", a, hw) == 2 &&
		    strcmp(a, ip) == 0 &&
		    sscanf(hw, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3],
			   &v[4], &v[5]) == 6) {
			for (int k = 0; k < 6; k++) {
				mac[k] = (uint8_t)v[k];
			}
			ok = (v[0] | v[1] | v[2] | v[3] | v[4] | v[5]) ? 0 : -1;
			break;
		}
	}
	fclose(f);
	return ok;
}

static int
gid_is_v2(struct ibv_context *ctx, uint8_t port, int idx)
{
	char path[256], buf[64] = "";
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/class/infiniband/%s/ports/%u/gid_attrs/types/%d",
		 ibv_get_device_name(ctx->device), port, idx);
	f = fopen(path, "r");
	if (f) {
		if (!fgets(buf, sizeof(buf), f)) {
			buf[0] = 0;
		}
		fclose(f);
	}
	return strstr(buf, "v2") != NULL;
}

/* CPU 往显存里的 SQ / dbrec 写:拷过去再同步,网卡才看得到(坑 2) */
static void
dev_put(uint32_t off, const void *src, size_t len)
{
	gpu_copy_to_dev(g.mem + off, src, len);
	gpu_sync();
}

static void
dev_get(void *dst, uint32_t off, size_t len)
{
	gpu_copy_to_host(dst, g.mem + off, len);
}

static uint32_t
sq_off(uint32_t pi)
{
	return DQ_OFF_WQ + g.rq_cnt * 16 + (pi & (g.sq_cnt - 1)) * 64;
}

int
devx_q_create(struct spdk_nvme_ctrlr *ctrlr, const struct devx_q_cfg *cfg,
	      struct gpu_io_ctx *ctx)
{
	struct mlx5dv_context_attr cattr = { .flags = MLX5DV_CONTEXT_FLAGS_DEVX };
	struct mlx5dv_pd dvpd;
	struct mlx5dv_obj obj;
	struct addrinfo *ai = NULL;
	struct ibv_qp_attr qa;
	struct ibv_device_attr dattr;
	struct ibv_mr *rsp_mr, *cap_mr, *data_mr, *conn_mr;
	const struct spdk_nvme_ctrlr_opts *copts = spdk_nvme_ctrlr_get_opts(ctrlr);
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	const struct spdk_nvme_transport_id *trid =
		spdk_nvme_ctrlr_get_transport_id(ctrlr);
	uint8_t *init, *rsp_host, *conn, mac[6];
	uint64_t *rq_host;
	void *d_uar, *rsp_dev;
	uint32_t eqn = 0, k;
	int mask, v2;

	memset(&g, 0, sizeof(g));
	g.rq_cnt = 1u << ilog2u(cfg->qdepth);
	g.sq_cnt = 512;
	g.cq_cnt = 512;

	/* ---- 寻址:rdma_cm,不建 QP ---- */
	g.ech = rdma_create_event_channel();
	if (!g.ech || rdma_create_id(g.ech, &g.cm, NULL, RDMA_PS_TCP) ||
	    getaddrinfo(cfg->traddr, cfg->trsvcid, NULL, &ai) ||
	    rdma_resolve_addr(g.cm, NULL, ai->ai_addr, 2000) ||
	    cm_wait(RDMA_CM_EVENT_ADDR_RESOLVED, NULL) ||
	    rdma_resolve_route(g.cm, 2000) ||
	    cm_wait(RDMA_CM_EVENT_ROUTE_RESOLVED, NULL)) {
		fprintf(stderr, "[CM] 寻址失败\n");
		return -1;
	}
	freeaddrinfo(ai);

	/* ---- 同一块网卡,另开 DevX 上下文 ---- */
	g.ctx = mlx5dv_open_device(g.cm->verbs->device, &cattr);
	if (!g.ctx) {
		fprintf(stderr, "[DevX] 打开 %s 失败: %s\n",
			ibv_get_device_name(g.cm->verbs->device), strerror(errno));
		return -1;
	}
	g.pd = ibv_alloc_pd(g.ctx);
	obj.pd.in = g.pd;
	obj.pd.out = &dvpd;
	if (!g.pd || mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD) ||
	    mlx5dv_devx_query_eqn(g.ctx, 0, &eqn)) {
		fprintf(stderr, "[DevX] PD / EQ 失败: %s\n", strerror(errno));
		return -1;
	}
	g.uar = mlx5dv_devx_alloc_uar(g.ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
	if (!g.uar) {
		g.uar = mlx5dv_devx_alloc_uar(g.ctx, MLX5DV_UAR_ALLOC_TYPE_BF);
	}
	if (!g.uar || gpu_map_host(g.uar->base_addr, 4096,
				   MC_HOST_REGISTER_IO_MEMORY |
				   MC_HOST_REGISTER_MAPPED, &d_uar) != 0) {
		fprintf(stderr, "[DevX] UAR 分配 / 映射失败\n");
		return -1;
	}

	/* ---- 显存 umem ---- */
	if (gpu_alloc_exportable_ex((void **)&g.mem, &g.mem_fd, DQ_MEM_LEN,
				    cfg->gpu_flag) != 0) {
		fprintf(stderr, "[DevX] 队列显存分配失败(flag %u)\n", cfg->gpu_flag);
		return -1;
	}
	{
		struct mlx5dv_devx_umem_in ui;

		memset(&ui, 0, sizeof(ui));
		ui.addr = 0;
		ui.size = DQ_MEM_LEN;
		ui.access = IBV_ACCESS_LOCAL_WRITE;
		ui.pgsz_bitmap = 1ULL << 12;
		ui.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		ui.dmabuf_fd = g.mem_fd;
		g.umem = mlx5dv_devx_umem_reg_ex(g.ctx, &ui);
		if (!g.umem) {
			fprintf(stderr, "[DevX] umem_reg_ex(DMABUF) 失败: %s\n",
				strerror(errno));
			return -1;
		}
	}

	/* ---- MR:都在 DevX 的 PD 上 ---- */
	rsp_host = aligned_alloc(4096, g.rq_cnt * DQ_RSP_STRIDE);
	conn = aligned_alloc(4096, 4096);
	if (!rsp_host || !conn) {
		return -1;
	}
	memset(rsp_host, 0, g.rq_cnt * DQ_RSP_STRIDE);
	memset(conn, 0, 4096);
	rsp_mr = ibv_reg_mr(g.pd, rsp_host, g.rq_cnt * DQ_RSP_STRIDE,
			    IBV_ACCESS_LOCAL_WRITE);
	conn_mr = ibv_reg_mr(g.pd, conn, 4096, IBV_ACCESS_LOCAL_WRITE |
			     IBV_ACCESS_REMOTE_READ);
	if (cfg->cap_fd >= 0) {
		/* -H:胶囊区是单独一块可导出的显存,offset 0 就是起点 */
		cap_mr = ibv_reg_dmabuf_mr(g.pd, 0, cfg->cap_len,
					   (uint64_t)cfg->cap_host, cfg->cap_fd,
					   IBV_ACCESS_LOCAL_WRITE);
	} else {
		cap_mr = ibv_reg_mr(g.pd, cfg->cap_host, cfg->cap_len,
				    IBV_ACCESS_LOCAL_WRITE);
	}
	data_mr = ibv_reg_dmabuf_mr(g.pd, 0, cfg->data_len,
				    (uint64_t)cfg->data_gpu, cfg->data_fd,
				    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				    IBV_ACCESS_REMOTE_WRITE |
				    IBV_ACCESS_RELAXED_ORDERING);
	if (!rsp_mr || !conn_mr || !cap_mr || !data_mr ||
	    gpu_map_host(rsp_host, g.rq_cnt * DQ_RSP_STRIDE,
			 MC_HOST_REGISTER_MAPPED, &rsp_dev) != 0) {
		fprintf(stderr, "[DevX] MR 注册失败: %s\n", strerror(errno));
		return -1;
	}

	/* ---- 显存初值:CQE 置 INVALID,RQ 挂满 recv,dbrec ---- */
	init = calloc(1, DQ_OFF_WQ + g.rq_cnt * 16 + g.sq_cnt * 64);
	rq_host = (uint64_t *)(init + DQ_OFF_WQ);
	for (k = 0; k < g.cq_cnt; k++) {
		init[DQ_OFF_CQ + k * 64 + 63] = 0xf0;
	}
	for (k = 0; k < g.rq_cnt; k++) {
		uint32_t *seg = (uint32_t *)(init + DQ_OFF_WQ + k * 16);
		uint64_t addr = (uint64_t)(rsp_host + k * DQ_RSP_STRIDE);

		seg[0] = htobe32(16);		/* NVMe CQE 16 字节 */
		seg[1] = htobe32(rsp_mr->lkey);
		*(uint64_t *)&seg[2] = htobe64(addr);
	}
	((uint32_t *)(init + DQ_OFF_QP_DBR))[0] = htobe32(g.rq_cnt);
	dev_put(0, init, DQ_OFF_WQ + g.rq_cnt * 16 + g.sq_cnt * 64);

	/* ---- CREATE_CQ ---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = { 0 };
		void *cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);

		DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		DEVX_SET(cqc, cqc, log_cq_size, ilog2u(g.cq_cnt));
		DEVX_SET(cqc, cqc, uar_page, g.uar->page_id);
		DEVX_SET(cqc, cqc, c_eqn, eqn);
		DEVX_SET(cqc, cqc, cqe_sz, 0);
		DEVX_SET(cqc, cqc, log_page_size, 0);
		DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
		DEVX_SET(cqc, cqc, dbr_umem_id, g.umem->umem_id);
		DEVX_SET64(cqc, cqc, dbr_addr, DQ_OFF_CQ_DBR);
		DEVX_SET(create_cq_in, in, cq_umem_valid, 1);
		DEVX_SET(create_cq_in, in, cq_umem_id, g.umem->umem_id);
		DEVX_SET64(create_cq_in, in, cq_umem_offset, DQ_OFF_CQ);
		g.cq = mlx5dv_devx_obj_create(g.ctx, in, sizeof(in), out, sizeof(out));
		if (!g.cq) {
			cmd_err("CREATE_CQ", out);
			return -1;
		}
		g.cqn = DEVX_GET(create_cq_out, out, cqn);
	}

	/* ---- CREATE_QP:RC,RQ 16 字节一槽,响应 scatter 到 CQE ---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(create_qp_in, in, qpc);

		DEVX_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);
		DEVX_SET(qpc, qpc, st, MLX5_QPC_ST_RC);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, pd, dvpd.pdn);
		DEVX_SET(qpc, qpc, uar_page, g.uar->page_id);
		DEVX_SET(qpc, qpc, cqn_snd, g.cqn);
		DEVX_SET(qpc, qpc, cqn_rcv, g.cqn);
		DEVX_SET(qpc, qpc, log_sq_size, ilog2u(g.sq_cnt));
		DEVX_SET(qpc, qpc, log_rq_size, ilog2u(g.rq_cnt));
		DEVX_SET(qpc, qpc, log_rq_stride, 0);		/* 16 字节 */
		DEVX_SET(qpc, qpc, rq_type, 0);
		DEVX_SET(qpc, qpc, cs_res, 1);			/* 响应 ≤32B 进 CQE(坑 11) */
		DEVX_SET(qpc, qpc, ts_format, 1);
		DEVX_SET(qpc, qpc, log_page_size, 0);
		DEVX_SET(qpc, qpc, dbr_umem_valid, 1);
		DEVX_SET(qpc, qpc, dbr_umem_id, g.umem->umem_id);
		DEVX_SET64(qpc, qpc, dbr_addr, DQ_OFF_QP_DBR);
		DEVX_SET(create_qp_in, in, wq_umem_valid, 1);
		DEVX_SET(create_qp_in, in, wq_umem_id, g.umem->umem_id);
		DEVX_SET64(create_qp_in, in, wq_umem_offset, DQ_OFF_WQ);
		g.qp = mlx5dv_devx_obj_create(g.ctx, in, sizeof(in), out, sizeof(out));
		if (!g.qp) {
			cmd_err("CREATE_QP", out);
			return -1;
		}
		g.qpn = DEVX_GET(create_qp_out, out, qpn);
	}
	printf("[DevX] %s qid %u  CQN 0x%x QPN 0x%x  显存 %p umem %u  uar %u\n",
	       ibv_get_device_name(g.ctx->device), cfg->qid, g.cqn, g.qpn,
	       g.mem, g.umem->umem_id, g.uar->page_id);

	/* ---- RST2INIT(参数来自 rdma_cm)---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_INIT;
	if (rdma_init_qp_attr(g.cm, &qa, &mask)) {
		fprintf(stderr, "[CM] init_qp_attr(INIT): %s\n", strerror(errno));
		return -1;
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(rst2init_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rst2init_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);

		DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
		DEVX_SET(rst2init_qp_in, in, qpn, g.qpn);
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, qa.port_num);
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index, qa.pkey_index);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, rre, 1);
		DEVX_SET(qpc, qpc, rwe, 1);
		if (mlx5dv_devx_obj_modify(g.qp, in, sizeof(in), out, sizeof(out))) {
			cmd_err("RST2INIT", out);
			return -1;
		}
	}

	/* ---- CM 握手:私有数据同 SPDK nvme_rdma_connect ---- */
	{
		struct rdma_conn_param p;
		struct rdma_cm_event ev;
		uint16_t pd16[16];

		memset(&p, 0, sizeof(p));
		memset(pd16, 0, sizeof(pd16));
		pd16[0] = 0;				/* recfmt */
		pd16[1] = cfg->qid;
		pd16[2] = (uint16_t)cfg->qdepth;	/* hrqsize */
		pd16[3] = (uint16_t)(cfg->qdepth - 1);	/* hsqsize */
		pd16[4] = cdata->cntlid;
		if (ibv_query_device(g.cm->verbs, &dattr)) {
			return -1;
		}
		p.responder_resources = dattr.max_qp_rd_atom;
		p.private_data = pd16;
		p.private_data_len = 32;
		p.retry_count = 7;
		p.rnr_retry_count = 7;
		p.srq = 0;
		p.qp_num = g.qpn;
		if (rdma_connect(g.cm, &p)) {
			fprintf(stderr, "[CM] rdma_connect: %s\n", strerror(errno));
			return -1;
		}
		if (cm_wait(RDMA_CM_EVENT_CONNECT_RESPONSE, &ev)) {
			return -1;
		}
		if (ev.param.conn.private_data_len >= 4) {
			printf("[CM] target 接受,crqsize %u\n",
			       ((const uint16_t *)ev.param.conn.private_data)[1]);
		}
	}

	/* ---- INIT2RTR ---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_RTR;
	if (rdma_init_qp_attr(g.cm, &qa, &mask) ||
	    arp_mac(inet_ntoa(((struct sockaddr_in *)
			       rdma_get_peer_addr(g.cm))->sin_addr), mac)) {
		fprintf(stderr, "[CM] init_qp_attr(RTR) 或查对端 MAC 失败"
			"(先 ping 一下 target)\n");
		return -1;
	}
	v2 = gid_is_v2(g.cm->verbs, qa.ah_attr.port_num, qa.ah_attr.grh.sgid_index);
	printf("[CM] 对端 QPN 0x%x  mtu %u  sgid[%u] %s  mac %02x:%02x:%02x:%02x:%02x:%02x"
	       "  rd_atomic %u\n", qa.dest_qp_num, 128u << qa.path_mtu,
	       qa.ah_attr.grh.sgid_index, v2 ? "v2" : "v1", mac[0], mac[1],
	       mac[2], mac[3], mac[4], mac[5], qa.max_dest_rd_atomic);
	{
		uint32_t in[DEVX_ST_SZ_DW(init2rtr_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(init2rtr_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);

		DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
		DEVX_SET(init2rtr_qp_in, in, qpn, g.qpn);
		DEVX_SET(qpc, qpc, mtu, qa.path_mtu);
		DEVX_SET(qpc, qpc, log_msg_max, 30);
		DEVX_SET(qpc, qpc, remote_qpn, qa.dest_qp_num);
		DEVX_SET(qpc, qpc, next_rcv_psn, qa.rq_psn);
		DEVX_SET(qpc, qpc, min_rnr_nak, qa.min_rnr_timer);
		DEVX_SET(qpc, qpc, log_rra_max, ilog2u(qa.max_dest_rd_atomic ?
						       qa.max_dest_rd_atomic : 1));
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num,
			 qa.ah_attr.port_num);
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
		       qa.ah_attr.grh.dgid.raw, 16);
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
		       mac, 6);
		DEVX_SET(qpc, qpc, primary_address_path.src_addr_index,
			 qa.ah_attr.grh.sgid_index);
		DEVX_SET(qpc, qpc, primary_address_path.hop_limit,
			 qa.ah_attr.grh.hop_limit ? qa.ah_attr.grh.hop_limit : 64);
		DEVX_SET(qpc, qpc, primary_address_path.tclass,
			 qa.ah_attr.grh.traffic_class);
		DEVX_SET(qpc, qpc, primary_address_path.flow_label,
			 qa.ah_attr.grh.flow_label);
		if (v2) {
			DEVX_SET(qpc, qpc, primary_address_path.udp_sport, 0xc000 |
				 (g.qpn & 0x3fff));
		}
		if (mlx5dv_devx_obj_modify(g.qp, in, sizeof(in), out, sizeof(out))) {
			cmd_err("INIT2RTR", out);
			return -1;
		}
	}

	/* ---- RTR2RTS ---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_RTS;
	if (rdma_init_qp_attr(g.cm, &qa, &mask)) {
		fprintf(stderr, "[CM] init_qp_attr(RTS): %s\n", strerror(errno));
		return -1;
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);

		DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
		DEVX_SET(rtr2rts_qp_in, in, qpn, g.qpn);
		DEVX_SET(qpc, qpc, retry_count, qa.retry_cnt);
		DEVX_SET(qpc, qpc, rnr_retry, qa.rnr_retry);
		DEVX_SET(qpc, qpc, next_send_psn, qa.sq_psn);
		DEVX_SET(qpc, qpc, log_sra_max, ilog2u(qa.max_rd_atomic ?
						       qa.max_rd_atomic : 1));
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, 1);
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qa.timeout ? qa.timeout : 14);
		if (mlx5dv_devx_obj_modify(g.qp, in, sizeof(in), out, sizeof(out))) {
			cmd_err("RTR2RTS", out);
			return -1;
		}
	}
	if (rdma_establish(g.cm)) {
		fprintf(stderr, "[CM] rdma_establish: %s\n", strerror(errno));
		return -1;
	}

	/* ---- Fabrics Connect:CPU 在这条 QP 上发一条 ---- */
	{
		uint8_t *cap_slot = (uint8_t *)cfg->cap_host + cfg->cap_len - 64;
		uint8_t capbuf[64], *cap = capbuf;
		uint8_t wqe[64], cqe[64];
		uint32_t ci = 0, got_resp = 0, polls = 0;
		uint16_t st = 0xffff;
		uint64_t db;

		/* 连接数据:hostid / cntlid / subnqn / hostnqn */
		memcpy(conn, copts->extended_host_id, 16);
		memcpy(conn + 16, &cdata->cntlid, 2);
		snprintf((char *)conn + 256, 256, "%s", trid->subnqn);
		snprintf((char *)conn + 512, 256, "%s", copts->hostnqn);

		nvme_build_rw_sqe((struct nvme_sqe *)cap, 0x7f, DQ_CONN_CID, 0, 0, 1,
				  (uint64_t)conn, conn_mr->rkey, 1024);
		memset(cap + 4, 0, 20);
		cap[4] = 0x01;					/* fctype CONNECT */
		memset(cap + 40, 0, 24);
		*(uint16_t *)(cap + 42) = cfg->qid;
		*(uint16_t *)(cap + 44) = (uint16_t)(cfg->qdepth - 1);
		*(uint32_t *)(cap + 48) = copts->keep_alive_timeout_ms;
		if (cfg->cap_fd >= 0) {
			gpu_copy_to_dev(cap_slot, cap, 64);
			gpu_sync();
		} else {
			memcpy(cap_slot, cap, 64);
		}

		memset(wqe, 0, sizeof(wqe));
		(void)mlx5_build_send_wqe(wqe, 0, g.qpn, (uint64_t)cap_slot,
					  cap_mr->lkey, 64);
		dev_put(sq_off(0), wqe, 64);
		{
			uint32_t one = htobe32(1);

			dev_put(DQ_OFF_QP_DBR + 4, &one, 4);
		}
		memcpy(&db, wqe, 8);
		__sync_synchronize();
		*(volatile uint64_t *)g.uar->reg_addr = db;
		__sync_synchronize();

		/* 收 SEND 完成和响应,共 2 个 CQE */
		while (ci < 2 && polls < 200000) {
			uint8_t oo;

			dev_get(cqe, DQ_OFF_CQ + (ci & (g.cq_cnt - 1)) * 64, 64);
			oo = cqe[63];
			polls++;
			if ((oo & 1) != 0 || (oo >> 4) == 0xf) {
				continue;
			}
			if ((oo >> 4) == 2) {		/* RESP_SEND */
				const uint8_t *n = (oo & 0x4) ? cqe :
					rsp_host + ((be16toh(*(uint16_t *)(cqe + 60))) &
						    (g.rq_cnt - 1)) * DQ_RSP_STRIDE;

				st = (uint16_t)((n[14] | (n[15] << 8)) >> 1);
				got_resp = 1;
			} else if ((oo >> 4) != 0) {
				fprintf(stderr, "[Connect] 错误 CQE opcode %u syndrome 0x%02x "
					"vendor 0x%02x\n", oo >> 4, cqe[55], cqe[54]);
				return -1;
			}
			ci++;
		}
		{
			uint32_t v = htobe32(ci);

			dev_put(DQ_OFF_CQ_DBR, &v, 4);
		}
		if (!got_resp || st != 0) {
			fprintf(stderr, "[Connect] %s (status 0x%x,轮询 %u 次)\n",
				got_resp ? "target 拒绝" : "等不到响应", st, polls);
			return -1;
		}
		printf("[Connect] qid %u sqsize %u kato %u ms:成功\n", cfg->qid,
		       cfg->qdepth - 1, copts->keep_alive_timeout_ms);
		g.sq0 = 1;
		g.cq0 = ci;
		g.rq0 = g.rq_cnt;
	}

	/* ---- 交给 kernel:ctx 里的队列和 MR 全换成这一套 ---- */
	ctx->sq_buf	= (volatile uint8_t *)(g.mem + DQ_OFF_WQ + g.rq_cnt * 16);
	ctx->sq_wqe_cnt	= g.sq_cnt;
	ctx->sq_stride	= 64;
	ctx->qp_dbrec	= (volatile uint32_t *)(g.mem + DQ_OFF_QP_DBR);
	ctx->bf_reg	= (volatile uint64_t *)((uint8_t *)d_uar +
					((uint8_t *)g.uar->reg_addr -
					 (uint8_t *)g.uar->base_addr));
	ctx->bf_alt	= NULL;
	ctx->cq_buf	= (volatile uint8_t *)(g.mem + DQ_OFF_CQ);
	ctx->cq_cnt	= g.cq_cnt;
	ctx->cqe_size	= 64;
	ctx->cq_dbrec	= (volatile uint32_t *)(g.mem + DQ_OFF_CQ_DBR);
	ctx->qpn	= g.qpn;
	ctx->capsule_lkey = cap_mr->lkey;
	ctx->data_rkey	= data_mr->rkey;
	ctx->rq_buf	= (volatile uint8_t *)(g.mem + DQ_OFF_WQ);
	ctx->rq_wqe_cnt	= g.rq_cnt;
	ctx->rq_stride	= 16;
	ctx->rsp_host_base = (uint64_t)rsp_host;
	ctx->rsp_len	= g.rq_cnt * DQ_RSP_STRIDE;
	ctx->rsp_dev	= (volatile uint8_t *)rsp_dev;
	ctx->sq_pi	= (uint16_t)g.sq0;
	ctx->cq_ci	= g.cq0;
	ctx->cq_phase	= (uint8_t)((g.cq0 / g.cq_cnt) & 1);
	ctx->rq_tail	= 1;		/* Connect 的响应用掉 1 个 recv */
	ctx->rq_head	= g.rq0;
	ctx->rq_host_buf = rq_host;	/* gpu_io_run 拷成显存影子;init 不释放 */
	ctx->q_in_gpu	= 1;
	printf("[接管] SQ pi=%u  CQ ci=%u  RQ head=%u tail=1(挂着 %u 个 recv)"
	       "  队列在显存\n", g.sq0, g.cq0, g.rq0, g.rq0 - 1);
	return 0;
}

int
devx_q_check(const struct gpu_io_ctx *ctx, uint32_t njobs, uint32_t cqe_req,
	     uint32_t cqe_resp, int no_repost)
{
	uint32_t qdb[2], cdb, sq, rq, cq;
	int bad = 0;

	dev_get(qdb, DQ_OFF_QP_DBR, 8);
	dev_get(&cdb, DQ_OFF_CQ_DBR, 4);
	sq = be32toh(qdb[1]) & 0xffff;
	rq = be32toh(qdb[0]) & 0xffff;
	cq = be32toh(cdb) & 0xffffff;
	printf("[dbrec] SQ %u -> %u (+%u, 期望 +%u)   CQ %u -> %u (+%u)"
	       "   RQ %u -> %u (+%u, 期望 +%u)\n", g.sq0, sq, (sq - g.sq0) & 0xffff,
	       njobs, g.cq0, cq, cq - g.cq0, g.rq0, rq, (rq - g.rq0) & 0xffff,
	       no_repost ? 0 : cqe_resp);
	if (((sq - g.sq0) & 0xffff) != (njobs & 0xffff)) {
		printf("  *** SQ 增量不等于轮数 ***\n");
		bad++;
	}
	if (!no_repost && ((rq - g.rq0) & 0xffff) != (cqe_resp & 0xffff)) {
		printf("  *** RQ 补充数和响应数对不上 ***\n");
		bad++;
	}
	if (cq != g.cq0 + cqe_req + cqe_resp) {
		printf("  *** CQ dbrec 没补齐:%u,期望 %u ***\n", cq,
		       g.cq0 + cqe_req + cqe_resp);
		bad++;
	}
	(void)ctx;
	return bad;
}
