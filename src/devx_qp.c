/*
 * devx_qp.c  --  NVMe-oF IO 队列:DevX 自建 QP,队列放显存(见 devx_qp.h)
 *
 * 多条队列(-X -q N):PD、胶囊区和数据区的 MR、event channel 各队列共用,
 * 第一条建的时候顺带建;每条队列自己的 cm_id / UAR / umem / CQ / QP /
 * recv buffer。队列显存由调用方一次分配好,每条 2 MiB 一段(坑 6)。
 *
 * 每条队列的显存 umem 布局(2 MiB):
 *   0x00000  CQ        cq_cnt × 64
 *   0x10000  CQ dbrec  8 字节
 *   0x10040  QP dbrec  [0]=RQ [1]=SQ
 *   0x20000  WQ        RQ(rq_cnt × 16)紧跟 SQ(sq_cnt × 64)
 * recv buffer、连接数据在主存(DevX 的 PD 上注册,每条队列一份);胶囊区、
 * 数据区沿用主程序分配的那两块,在 DevX 的 PD 上重注册一份(各队列共用,
 * 胶囊区由主程序按 QP 切开)。
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

extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_copy_to_dev(void *, const void *, size_t);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_sync(void);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define DQ_MEM_LEN	DEVX_Q_MEM_LEN
#define DQ_OFF_CQ	0x00000u
#define DQ_OFF_CQ_DBR	0x10000u
#define DQ_OFF_QP_DBR	0x10040u
#define DQ_OFF_WQ	0x20000u
#define DQ_RSP_STRIDE	64u
#define DQ_CONN_CID	0x7fffu

/* 每条队列 */
struct dq {
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
};

/* 各队列共用 */
static struct {
	struct ibv_context		*ctx;
	struct ibv_pd			*pd;
	struct rdma_event_channel	*ech;
	uint32_t			pdn, eqn;
	struct ibv_mr			*cap_mr, *data_mr;
	struct dq			q[GIO_MAX_QP];
	/* 已映射给 GPU 的 UAR 页:几条队列的 UAR 可能在同一页(reg 偏移不同),
	 * 同一页再 mcHostRegister 会报 AlreadyRegistered */
	void				*uar_page[GIO_MAX_QP], *uar_dev[GIO_MAX_QP];
	unsigned			n_uar;
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
dev_put(struct dq *d, uint32_t off, const void *src, size_t len)
{
	gpu_copy_to_dev(d->mem + off, src, len);
	gpu_sync();
}

static void
dev_get(const struct dq *d, void *dst, uint32_t off, size_t len)
{
	gpu_copy_to_host(dst, d->mem + off, len);
}

static uint32_t
sq_off(const struct dq *d, uint32_t pi)
{
	return DQ_OFF_WQ + d->rq_cnt * 16 + (pi & (d->sq_cnt - 1)) * 64;
}

static int
uar_map(void *page, void **dev)
{
	unsigned i;

	for (i = 0; i < g.n_uar; i++) {
		if (g.uar_page[i] == page) {
			*dev = g.uar_dev[i];
			return 0;
		}
	}
	if (g.n_uar >= GIO_MAX_QP ||
	    gpu_map_host(page, 4096, MC_HOST_REGISTER_IO_MEMORY |
			 MC_HOST_REGISTER_MAPPED, dev) != 0) {
		return -1;
	}
	g.uar_page[g.n_uar] = page;
	g.uar_dev[g.n_uar] = *dev;
	g.n_uar++;
	return 0;
}

/* 第一条队列建的时候:DevX 上下文、PD、EQ,胶囊区和数据区的 MR */
static int
devx_shared_init(struct ibv_device *dev, const struct devx_q_cfg *cfg)
{
	struct mlx5dv_context_attr cattr = { .flags = MLX5DV_CONTEXT_FLAGS_DEVX };
	struct mlx5dv_pd dvpd;
	struct mlx5dv_obj obj;

	g.ctx = mlx5dv_open_device(dev, &cattr);
	if (!g.ctx) {
		fprintf(stderr, "[DevX] 打开 %s 失败: %s\n",
			ibv_get_device_name(dev), strerror(errno));
		return -1;
	}
	g.pd = ibv_alloc_pd(g.ctx);
	obj.pd.in = g.pd;
	obj.pd.out = &dvpd;
	if (!g.pd || mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD) ||
	    mlx5dv_devx_query_eqn(g.ctx, 0, &g.eqn)) {
		fprintf(stderr, "[DevX] PD / EQ 失败: %s\n", strerror(errno));
		return -1;
	}
	g.pdn = dvpd.pdn;
	if (cfg->cap_fd >= 0) {
		/* -H:胶囊区是单独一块可导出的显存,offset 0 就是起点 */
		g.cap_mr = ibv_reg_dmabuf_mr(g.pd, 0, cfg->cap_len,
					     (uint64_t)cfg->cap_host, cfg->cap_fd,
					     IBV_ACCESS_LOCAL_WRITE);
	} else {
		g.cap_mr = ibv_reg_mr(g.pd, cfg->cap_host, cfg->cap_len,
				      IBV_ACCESS_LOCAL_WRITE);
	}
	g.data_mr = ibv_reg_dmabuf_mr(g.pd, 0, cfg->data_len,
				      (uint64_t)cfg->data_gpu, cfg->data_fd,
				      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				      IBV_ACCESS_REMOTE_WRITE |
				      IBV_ACCESS_RELAXED_ORDERING);
	if (!g.cap_mr || !g.data_mr) {
		fprintf(stderr, "[DevX] 胶囊 / 数据区 MR 注册失败: %s\n",
			strerror(errno));
		return -1;
	}
	return 0;
}

int
devx_q_create(struct spdk_nvme_ctrlr *ctrlr, const struct devx_q_cfg *cfg,
	      struct gpu_io_ctx *ctx)
{
	struct dq *d;
	struct addrinfo *ai = NULL;
	struct ibv_qp_attr qa;
	struct ibv_device_attr dattr;
	struct ibv_mr *rsp_mr, *conn_mr;
	const struct spdk_nvme_ctrlr_opts *copts = spdk_nvme_ctrlr_get_opts(ctrlr);
	const struct spdk_nvme_ctrlr_data *cdata = spdk_nvme_ctrlr_get_data(ctrlr);
	const struct spdk_nvme_transport_id *trid =
		spdk_nvme_ctrlr_get_transport_id(ctrlr);
	uint8_t *init, *rsp_host, *conn, mac[6];
	uint64_t *rq_host;
	void *d_uar, *rsp_dev;
	uint32_t k;
	int mask, v2;

	if (cfg->slot >= GIO_MAX_QP || !cfg->q_mem) {
		fprintf(stderr, "[DevX] slot %u 越界或没给队列显存\n", cfg->slot);
		return -1;
	}
	d = &g.q[cfg->slot];
	memset(d, 0, sizeof(*d));
	d->rq_cnt = 1u << ilog2u(cfg->qdepth);
	d->sq_cnt = 512;
	d->cq_cnt = 512;

	/* ---- 寻址:rdma_cm,不建 QP;event channel 各队列共用,依次建连 ---- */
	if (!g.ech) {
		g.ech = rdma_create_event_channel();
	}
	if (!g.ech || rdma_create_id(g.ech, &d->cm, NULL, RDMA_PS_TCP) ||
	    getaddrinfo(cfg->traddr, cfg->trsvcid, NULL, &ai) ||
	    rdma_resolve_addr(d->cm, NULL, ai->ai_addr, 2000) ||
	    cm_wait(RDMA_CM_EVENT_ADDR_RESOLVED, NULL) ||
	    rdma_resolve_route(d->cm, 2000) ||
	    cm_wait(RDMA_CM_EVENT_ROUTE_RESOLVED, NULL)) {
		fprintf(stderr, "[CM] 寻址失败\n");
		return -1;
	}
	freeaddrinfo(ai);

	/* ---- 同一块网卡,另开 DevX 上下文(第一条队列时)---- */
	if (!g.ctx && devx_shared_init(d->cm->verbs->device, cfg) != 0) {
		return -1;
	}
	/*
	 * -X 时等待比主存方案多 ~3 µs,网卡时间戳显示全在"门铃→SEND 完成":
	 * 没有 BF,网卡要回 SQ 取 WQE。-G 时要一个 BF 类型的 UAR,kernel 把
	 * WQE 按 8 字节分 4 次、中间空等写进 BF(连着写网卡不认,见 0018)。
	 */
	if (cfg->want_bf) {
		d->uar = mlx5dv_devx_alloc_uar(g.ctx, MLX5DV_UAR_ALLOC_TYPE_BF);
		if (!d->uar) {
			fprintf(stderr, "[DevX] BF 类型 UAR 分配失败,退回 NC\n");
		}
	}
	if (!d->uar) {
		d->uar = mlx5dv_devx_alloc_uar(g.ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
	}
	if (!d->uar) {
		d->uar = mlx5dv_devx_alloc_uar(g.ctx, MLX5DV_UAR_ALLOC_TYPE_BF);
	}
	if (!d->uar || uar_map(d->uar->base_addr, &d_uar) != 0) {
		fprintf(stderr, "[DevX] UAR 分配 / 映射失败\n");
		return -1;
	}

	/* ---- 显存 umem:调用方给的那一段,addr 填它在 dma-buf 里的偏移 ---- */
	d->mem = cfg->q_mem;
	d->mem_fd = cfg->q_fd;
	{
		struct mlx5dv_devx_umem_in ui;

		memset(&ui, 0, sizeof(ui));
		ui.addr = (void *)(uintptr_t)cfg->q_off;
		ui.size = DQ_MEM_LEN;
		ui.access = IBV_ACCESS_LOCAL_WRITE;
		ui.pgsz_bitmap = 1ULL << 12;
		ui.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		ui.dmabuf_fd = d->mem_fd;
		d->umem = mlx5dv_devx_umem_reg_ex(g.ctx, &ui);
		if (!d->umem) {
			fprintf(stderr, "[DevX] umem_reg_ex(DMABUF, off 0x%zx) 失败: %s\n",
				cfg->q_off, strerror(errno));
			return -1;
		}
	}

	/* ---- 本队列的 MR(recv buffer、连接数据):都在 DevX 的 PD 上 ---- */
	rsp_host = aligned_alloc(4096, d->rq_cnt * DQ_RSP_STRIDE);
	conn = aligned_alloc(4096, 4096);
	if (!rsp_host || !conn) {
		return -1;
	}
	memset(rsp_host, 0, d->rq_cnt * DQ_RSP_STRIDE);
	memset(conn, 0, 4096);
	rsp_mr = ibv_reg_mr(g.pd, rsp_host, d->rq_cnt * DQ_RSP_STRIDE,
			    IBV_ACCESS_LOCAL_WRITE);
	conn_mr = ibv_reg_mr(g.pd, conn, 4096, IBV_ACCESS_LOCAL_WRITE |
			     IBV_ACCESS_REMOTE_READ);
	if (!rsp_mr || !conn_mr ||
	    gpu_map_host(rsp_host, d->rq_cnt * DQ_RSP_STRIDE,
			 MC_HOST_REGISTER_MAPPED, &rsp_dev) != 0) {
		fprintf(stderr, "[DevX] MR 注册失败: %s\n", strerror(errno));
		return -1;
	}

	/* ---- 显存初值:CQE 置 INVALID,RQ 挂满 recv,dbrec ---- */
	init = calloc(1, DQ_OFF_WQ + d->rq_cnt * 16 + d->sq_cnt * 64);
	rq_host = (uint64_t *)(init + DQ_OFF_WQ);
	for (k = 0; k < d->cq_cnt; k++) {
		init[DQ_OFF_CQ + k * 64 + 63] = 0xf0;
	}
	for (k = 0; k < d->rq_cnt; k++) {
		uint32_t *seg = (uint32_t *)(init + DQ_OFF_WQ + k * 16);
		uint64_t addr = (uint64_t)(rsp_host + k * DQ_RSP_STRIDE);

		seg[0] = htobe32(16);		/* NVMe CQE 16 字节 */
		seg[1] = htobe32(rsp_mr->lkey);
		*(uint64_t *)&seg[2] = htobe64(addr);
	}
	((uint32_t *)(init + DQ_OFF_QP_DBR))[0] = htobe32(d->rq_cnt);
	dev_put(d, 0, init, DQ_OFF_WQ + d->rq_cnt * 16 + d->sq_cnt * 64);

	/* ---- CREATE_CQ ---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = { 0 };
		void *cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);

		DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		DEVX_SET(cqc, cqc, log_cq_size, ilog2u(d->cq_cnt));
		DEVX_SET(cqc, cqc, uar_page, d->uar->page_id);
		DEVX_SET(cqc, cqc, c_eqn, g.eqn);
		DEVX_SET(cqc, cqc, cqe_sz, 0);
		DEVX_SET(cqc, cqc, log_page_size, 0);
		DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
		DEVX_SET(cqc, cqc, dbr_umem_id, d->umem->umem_id);
		DEVX_SET64(cqc, cqc, dbr_addr, DQ_OFF_CQ_DBR);
		DEVX_SET(create_cq_in, in, cq_umem_valid, 1);
		DEVX_SET(create_cq_in, in, cq_umem_id, d->umem->umem_id);
		DEVX_SET64(create_cq_in, in, cq_umem_offset, DQ_OFF_CQ);
		d->cq = mlx5dv_devx_obj_create(g.ctx, in, sizeof(in), out, sizeof(out));
		if (!d->cq) {
			cmd_err("CREATE_CQ", out);
			return -1;
		}
		d->cqn = DEVX_GET(create_cq_out, out, cqn);
	}

	/* ---- CREATE_QP:RC,RQ 16 字节一槽,响应 scatter 到 CQE ---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(create_qp_in, in, qpc);

		DEVX_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);
		DEVX_SET(qpc, qpc, st, MLX5_QPC_ST_RC);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, pd, g.pdn);
		DEVX_SET(qpc, qpc, uar_page, d->uar->page_id);
		DEVX_SET(qpc, qpc, cqn_snd, d->cqn);
		DEVX_SET(qpc, qpc, cqn_rcv, d->cqn);
		DEVX_SET(qpc, qpc, log_sq_size, ilog2u(d->sq_cnt));
		DEVX_SET(qpc, qpc, log_rq_size, ilog2u(d->rq_cnt));
		DEVX_SET(qpc, qpc, log_rq_stride, 0);		/* 16 字节 */
		DEVX_SET(qpc, qpc, rq_type, 0);
		DEVX_SET(qpc, qpc, cs_res, 1);			/* 响应 ≤32B 进 CQE(坑 11) */
		DEVX_SET(qpc, qpc, ts_format, 1);
		DEVX_SET(qpc, qpc, log_page_size, 0);
		DEVX_SET(qpc, qpc, dbr_umem_valid, 1);
		DEVX_SET(qpc, qpc, dbr_umem_id, d->umem->umem_id);
		DEVX_SET64(qpc, qpc, dbr_addr, DQ_OFF_QP_DBR);
		DEVX_SET(create_qp_in, in, wq_umem_valid, 1);
		DEVX_SET(create_qp_in, in, wq_umem_id, d->umem->umem_id);
		DEVX_SET64(create_qp_in, in, wq_umem_offset, DQ_OFF_WQ);
		d->qp = mlx5dv_devx_obj_create(g.ctx, in, sizeof(in), out, sizeof(out));
		if (!d->qp) {
			cmd_err("CREATE_QP", out);
			return -1;
		}
		d->qpn = DEVX_GET(create_qp_out, out, qpn);
	}
	printf("[DevX] %s qid %u  CQN 0x%x QPN 0x%x  显存 %p umem %u  uar %u(%s,reg 页内 0x%lx)\n",
	       ibv_get_device_name(g.ctx->device), cfg->qid, d->cqn, d->qpn,
	       d->mem, d->umem->umem_id, d->uar->page_id,
	       cfg->want_bf ? "要 BF" : "NC",
	       (unsigned long)((uintptr_t)d->uar->reg_addr & 4095));

	/* ---- RST2INIT(参数来自 rdma_cm)---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_INIT;
	if (rdma_init_qp_attr(d->cm, &qa, &mask)) {
		fprintf(stderr, "[CM] init_qp_attr(INIT): %s\n", strerror(errno));
		return -1;
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(rst2init_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rst2init_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);

		DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
		DEVX_SET(rst2init_qp_in, in, qpn, d->qpn);
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, qa.port_num);
		DEVX_SET(qpc, qpc, primary_address_path.pkey_index, qa.pkey_index);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, rre, 1);
		DEVX_SET(qpc, qpc, rwe, 1);
		if (mlx5dv_devx_obj_modify(d->qp, in, sizeof(in), out, sizeof(out))) {
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
		if (ibv_query_device(d->cm->verbs, &dattr)) {
			return -1;
		}
		p.responder_resources = dattr.max_qp_rd_atom;
		p.private_data = pd16;
		p.private_data_len = 32;
		p.retry_count = 7;
		p.rnr_retry_count = 7;
		p.srq = 0;
		p.qp_num = d->qpn;
		if (rdma_connect(d->cm, &p)) {
			fprintf(stderr, "[CM] rdma_connect: %s\n", strerror(errno));
			return -1;
		}
		if (cm_wait(RDMA_CM_EVENT_CONNECT_RESPONSE, &ev)) {
			return -1;
		}
		if (cfg->slot == 0 && ev.param.conn.private_data_len >= 4) {
			printf("[CM] target 接受,crqsize %u\n",
			       ((const uint16_t *)ev.param.conn.private_data)[1]);
		}
	}

	/* ---- INIT2RTR ---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_RTR;
	if (rdma_init_qp_attr(d->cm, &qa, &mask) ||
	    arp_mac(inet_ntoa(((struct sockaddr_in *)
			       rdma_get_peer_addr(d->cm))->sin_addr), mac)) {
		fprintf(stderr, "[CM] init_qp_attr(RTR) 或查对端 MAC 失败"
			"(先 ping 一下 target)\n");
		return -1;
	}
	v2 = gid_is_v2(d->cm->verbs, qa.ah_attr.port_num, qa.ah_attr.grh.sgid_index);
	if (cfg->slot == 0) {
		printf("[CM] 对端 QPN 0x%x  mtu %u  sgid[%u] %s  mac %02x:%02x:%02x:%02x:%02x:%02x"
		       "  rd_atomic %u\n", qa.dest_qp_num, 128u << qa.path_mtu,
		       qa.ah_attr.grh.sgid_index, v2 ? "v2" : "v1", mac[0], mac[1],
		       mac[2], mac[3], mac[4], mac[5], qa.max_dest_rd_atomic);
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(init2rtr_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(init2rtr_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);

		DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
		DEVX_SET(init2rtr_qp_in, in, qpn, d->qpn);
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
				 (d->qpn & 0x3fff));
		}
		if (mlx5dv_devx_obj_modify(d->qp, in, sizeof(in), out, sizeof(out))) {
			cmd_err("INIT2RTR", out);
			return -1;
		}
	}

	/* ---- RTR2RTS ---- */
	memset(&qa, 0, sizeof(qa));
	qa.qp_state = IBV_QPS_RTS;
	if (rdma_init_qp_attr(d->cm, &qa, &mask)) {
		fprintf(stderr, "[CM] init_qp_attr(RTS): %s\n", strerror(errno));
		return -1;
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);

		DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
		DEVX_SET(rtr2rts_qp_in, in, qpn, d->qpn);
		DEVX_SET(qpc, qpc, retry_count, qa.retry_cnt);
		DEVX_SET(qpc, qpc, rnr_retry, qa.rnr_retry);
		DEVX_SET(qpc, qpc, next_send_psn, qa.sq_psn);
		DEVX_SET(qpc, qpc, log_sra_max, ilog2u(qa.max_rd_atomic ?
						       qa.max_rd_atomic : 1));
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, 1);
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout,
			 qa.timeout ? qa.timeout : 14);
		if (mlx5dv_devx_obj_modify(d->qp, in, sizeof(in), out, sizeof(out))) {
			cmd_err("RTR2RTS", out);
			return -1;
		}
	}
	if (rdma_establish(d->cm)) {
		fprintf(stderr, "[CM] rdma_establish: %s\n", strerror(errno));
		return -1;
	}

	/* ---- Fabrics Connect:CPU 在这条 QP 上发一条 ---- */
	{
		/*
		 * Connect 的胶囊放连接数据那页的后半(主存,conn_mr),不放胶囊区:
		 * -H 时胶囊区在显存,CPU 拷进去之后网卡读到的不一定是新值,
		 * 实测 -X -H 时 Connect 收不到任何 CQE。
		 */
		uint8_t *cap = conn + 2048;
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

		memset(wqe, 0, sizeof(wqe));
		(void)mlx5_build_send_wqe(wqe, 0, d->qpn, (uint64_t)cap,
					  conn_mr->lkey, 64);
		dev_put(d, sq_off(d, 0), wqe, 64);
		{
			uint32_t one = htobe32(1);

			dev_put(d, DQ_OFF_QP_DBR + 4, &one, 4);
		}
		memcpy(&db, wqe, 8);
		__sync_synchronize();
		*(volatile uint64_t *)d->uar->reg_addr = db;
		__sync_synchronize();

		/* 收 SEND 完成和响应,共 2 个 CQE */
		while (ci < 2 && polls < 200000) {
			uint8_t oo;

			dev_get(d, cqe, DQ_OFF_CQ + (ci & (d->cq_cnt - 1)) * 64, 64);
			oo = cqe[63];
			polls++;
			if ((oo & 1) != 0 || (oo >> 4) == 0xf) {
				continue;
			}
			if ((oo >> 4) == 2) {		/* RESP_SEND */
				const uint8_t *n = (oo & 0x4) ? cqe :
					rsp_host + ((be16toh(*(uint16_t *)(cqe + 60))) &
						    (d->rq_cnt - 1)) * DQ_RSP_STRIDE;

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

			dev_put(d, DQ_OFF_CQ_DBR, &v, 4);
		}
		if (!got_resp || st != 0) {
			fprintf(stderr, "[Connect] %s (status 0x%x,轮询 %u 次,收到 CQE %u 个,"
				"当前 op_own 0x%02x)\n",
				got_resp ? "target 拒绝" : "等不到响应", st, polls, ci,
				cqe[63]);
			return -1;
		}
		printf("[Connect] qid %u sqsize %u kato %u ms:成功\n", cfg->qid,
		       cfg->qdepth - 1, copts->keep_alive_timeout_ms);
		d->sq0 = 1;
		d->cq0 = ci;
		d->rq0 = d->rq_cnt;
	}

	/* ---- 交给 kernel:ctx 里的队列和 MR 全换成这一套 ---- */
	ctx->sq_buf	= (volatile uint8_t *)(d->mem + DQ_OFF_WQ + d->rq_cnt * 16);
	ctx->sq_wqe_cnt	= d->sq_cnt;
	ctx->sq_stride	= 64;
	ctx->qp_dbrec	= (volatile uint32_t *)(d->mem + DQ_OFF_QP_DBR);
	ctx->bf_reg	= (volatile uint64_t *)((uint8_t *)d_uar +
					((uint8_t *)d->uar->reg_addr -
					 (uint8_t *)d->uar->base_addr));
	ctx->bf_alt	= NULL;
	ctx->cq_buf	= (volatile uint8_t *)(d->mem + DQ_OFF_CQ);
	ctx->cq_cnt	= d->cq_cnt;
	ctx->cqe_size	= 64;
	ctx->cq_dbrec	= (volatile uint32_t *)(d->mem + DQ_OFF_CQ_DBR);
	ctx->qpn	= d->qpn;
	ctx->capsule_lkey = g.cap_mr->lkey;
	ctx->data_rkey	= g.data_mr->rkey;
	ctx->rq_buf	= (volatile uint8_t *)(d->mem + DQ_OFF_WQ);
	ctx->rq_wqe_cnt	= d->rq_cnt;
	ctx->rq_stride	= 16;
	ctx->rsp_host_base = (uint64_t)rsp_host;
	ctx->rsp_len	= d->rq_cnt * DQ_RSP_STRIDE;
	ctx->rsp_dev	= (volatile uint8_t *)rsp_dev;
	ctx->sq_pi	= (uint16_t)d->sq0;
	ctx->cq_ci	= d->cq0;
	ctx->cq_phase	= (uint8_t)((d->cq0 / d->cq_cnt) & 1);
	ctx->rq_tail	= 1;		/* Connect 的响应用掉 1 个 recv */
	ctx->rq_head	= d->rq0;
	ctx->rq_host_buf = rq_host;	/* gpu_io_run 拷成显存影子;init 不释放 */
	ctx->q_in_gpu	= 1;
	printf("[接管] SQ pi=%u  CQ ci=%u  RQ head=%u tail=1(挂着 %u 个 recv)"
	       "  队列在显存\n", d->sq0, d->cq0, d->rq0, d->rq0 - 1);
	return 0;
}

int
devx_q_check(unsigned slot, uint32_t njobs, uint32_t cqe_req,
	     uint32_t cqe_resp, int no_repost)
{
	const struct dq *d = &g.q[slot];
	uint32_t qdb[2], cdb, sq, rq, cq;
	int bad = 0;

	dev_get(d, qdb, DQ_OFF_QP_DBR, 8);
	dev_get(d, &cdb, DQ_OFF_CQ_DBR, 4);
	sq = be32toh(qdb[1]) & 0xffff;
	rq = be32toh(qdb[0]) & 0xffff;
	cq = be32toh(cdb) & 0xffffff;
	printf("[dbrec] QP %u  SQ %u -> %u (+%u, 期望 +%u)   CQ %u -> %u (+%u)"
	       "   RQ %u -> %u (+%u, 期望 +%u)\n", slot, d->sq0, sq,
	       (sq - d->sq0) & 0xffff, njobs, d->cq0, cq, cq - d->cq0, d->rq0, rq,
	       (rq - d->rq0) & 0xffff, no_repost ? 0 : cqe_resp);
	if (((sq - d->sq0) & 0xffff) != (njobs & 0xffff)) {
		printf("  *** SQ 增量不等于轮数 ***\n");
		bad++;
	}
	if (!no_repost && ((rq - d->rq0) & 0xffff) != (cqe_resp & 0xffff)) {
		printf("  *** RQ 补充数和响应数对不上 ***\n");
		bad++;
	}
	if (cq != d->cq0 + cqe_req + cqe_resp) {
		printf("  *** CQ dbrec 没补齐:%u,期望 %u ***\n", cq,
		       d->cq0 + cqe_req + cqe_resp);
		bad++;
	}
	return bad;
}
