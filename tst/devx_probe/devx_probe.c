/*
 * devx_probe.c  --  队列进显存的可行性探针(IBGDA 的做法,不需要 target)
 *
 * 用 DevX 自己建 CQ 和 RC QP,队列内存用 umem 注册,QP 回环到自己;
 * GPU 单线程发 RDMA WRITE、轮询 CQE。对照队列放主存 / 显存:
 *
 *   -C h|g   CQ(+CQ dbrec)放主存 / 显存
 *   -W h|g   SQ(+QP dbrec)放主存 / 显存
 *
 * 显存:mcExtMallocWithFlags(-F,默认 1 finegrained)导出 dma-buf,
 * mlx5dv_devx_umem_reg_ex + MLX5DV_UMEM_MASK_DMABUF 注册(mxshmem 的
 * 主路径)。主存:posix_memalign + mlx5dv_devx_umem_reg,再
 * mcHostRegister 给 GPU。
 *
 * 要回答的两个问题(docs/concurrency-design.md「时延调优」):
 *   1. 网卡写进显存的 CQE,GPU 轮询多快能看到  → -C g 的 comp
 *   2. 网卡从显存取 WQE 比从主存慢多少        → -W g 的 comp
 *
 * 用法(root):
 *   devx_probe -d mlx5_1 -g 4 -x 3 -C g -W g -n 1000
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <endian.h>
#include <dirent.h>
#include <inttypes.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include "devx_ifc.h"
#include "devx_probe.h"

extern int gpu_set_dev(int);
extern int gpu_alloc_exportable_ex(void **, int *, size_t, unsigned int);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_copy_to_dev(void *, const void *, size_t);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_sync(void);
extern int dp_run(struct dp_args *, struct dp_out *, unsigned long long *,
		  unsigned long long *, double *);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define Q_CNT		64		/* CQE 数 = SQ WQE 数 */
#define OFF_CQ		0
#define OFF_CQ_DBR	4096
#define OFF_QP_DBR	(4096 + 64)
#define OFF_WQ		8192
#define HOST_REG_LEN	(64 * 1024)
#define GPU_REG_LEN	(2 * 1024 * 1024)

/* 一块队列内存:主存或显存,注册成一个 umem */
struct region {
	int			in_gpu;
	void			*host;	/* CPU 能访问的地址(主存时) */
	void			*dev;	/* GPU 访问的地址 */
	size_t			len;
	int			fd;
	struct mlx5dv_devx_umem	*umem;
};

static const char *g_dev = "mlx5_1";
static int g_gpu = 4, g_gid = 3, g_flag = 1;
static uint32_t g_n = 1000;
static int g_cq_gpu = 1, g_wq_gpu = 1;

static int
cmp_u64(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a;
	unsigned long long y = *(const unsigned long long *)b;

	return (x > y) - (x < y);
}

static void
usage(const char *p)
{
	printf("用法: %s [-d ibdev] [-g gpu] [-x gid_idx] [-n 轮数]\n"
	       "          [-C h|g] [-W h|g] [-F 显存分配 flag]\n", p);
}

/* devx 命令失败时把 status / syndrome 打出来 */
static void
cmd_err(const char *what, const void *out)
{
	fprintf(stderr, "%s 失败: errno=%d status=0x%x syndrome=0x%x\n", what,
		errno, DEVX_GET(create_cq_out, out, status),
		DEVX_GET(create_cq_out, out, syndrome));
}

static int
region_init(struct region *r, struct ibv_context *ctx, int in_gpu)
{
	memset(r, 0, sizeof(*r));
	r->in_gpu = in_gpu;
	r->fd = -1;

	if (in_gpu) {
		struct mlx5dv_devx_umem_in ui;
		uint8_t *z;

		r->len = GPU_REG_LEN;
		if (gpu_alloc_exportable_ex(&r->dev, &r->fd, r->len,
					    g_flag) != 0) {
			fprintf(stderr, "显存分配/导出失败(flag=%d)\n", g_flag);
			return -1;
		}
		memset(&ui, 0, sizeof(ui));
		ui.addr = 0;		/* dma-buf 模式下是 fd 内偏移(坑 6) */
		ui.size = r->len;
		ui.access = IBV_ACCESS_LOCAL_WRITE;
		ui.pgsz_bitmap = 1ULL << 12;
		ui.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		ui.dmabuf_fd = r->fd;
		r->umem = mlx5dv_devx_umem_reg_ex(ctx, &ui);
		if (!r->umem) {
			fprintf(stderr, "mlx5dv_devx_umem_reg_ex(DMABUF) 失败: %s\n",
				strerror(errno));
			return -1;
		}
		/* 清零,再把 CQE 的 op_own 置成 INVALID(0xf0) */
		z = calloc(1, OFF_WQ + Q_CNT * 64);
		for (int k = 0; k < Q_CNT; k++) {
			z[OFF_CQ + k * 64 + 63] = 0xf0;
		}
		gpu_copy_to_dev(r->dev, z, OFF_WQ + Q_CNT * 64);
		gpu_sync();
		free(z);
		printf("[显存] %p 2 MiB flag=%d fd=%d umem_id=%u\n", r->dev,
		       g_flag, r->fd, r->umem->umem_id);
		return 0;
	}

	r->len = HOST_REG_LEN;
	if (posix_memalign(&r->host, 4096, r->len) != 0) {
		return -1;
	}
	memset(r->host, 0, r->len);
	for (int k = 0; k < Q_CNT; k++) {
		((uint8_t *)r->host)[OFF_CQ + k * 64 + 63] = 0xf0;
	}
	r->umem = mlx5dv_devx_umem_reg(ctx, r->host, r->len,
				       IBV_ACCESS_LOCAL_WRITE);
	if (!r->umem) {
		fprintf(stderr, "mlx5dv_devx_umem_reg 失败: %s\n", strerror(errno));
		return -1;
	}
	if (gpu_map_host(r->host, r->len, MC_HOST_REGISTER_MAPPED,
			 &r->dev) != 0) {
		fprintf(stderr, "主存映射给 GPU 失败\n");
		return -1;
	}
	printf("[主存] %p 64 KiB umem_id=%u -> GPU %p\n", r->host,
	       r->umem->umem_id, r->dev);
	return 0;
}

/* 网卡口的 MAC:/sys/class/infiniband/<dev>/device/net/<if>/address */
static int
read_mac(const char *ibdev, uint8_t mac[6])
{
	char path[256], buf[64];
	struct dirent *de;
	DIR *d;
	FILE *f;
	unsigned v[6];

	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/net",
		 ibdev);
	d = opendir(path);
	if (!d) {
		return -1;
	}
	while ((de = readdir(d)) && de->d_name[0] == '.') {
	}
	if (!de) {
		closedir(d);
		return -1;
	}
	snprintf(path, sizeof(path), "/sys/class/infiniband/%s/device/net/%s/address",
		 ibdev, de->d_name);
	closedir(d);
	f = fopen(path, "r");
	if (!f || !fgets(buf, sizeof(buf), f)) {
		if (f) fclose(f);
		return -1;
	}
	fclose(f);
	if (sscanf(buf, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3],
		   &v[4], &v[5]) != 6) {
		return -1;
	}
	for (int k = 0; k < 6; k++) {
		mac[k] = (uint8_t)v[k];
	}
	return 0;
}

static int
gid_is_v2(const char *ibdev, int idx)
{
	char path[256], buf[64] = "";
	FILE *f;

	snprintf(path, sizeof(path),
		 "/sys/class/infiniband/%s/ports/1/gid_attrs/types/%d", ibdev, idx);
	f = fopen(path, "r");
	if (f) {
		if (!fgets(buf, sizeof(buf), f)) {
			buf[0] = 0;
		}
		fclose(f);
	}
	return strstr(buf, "v2") != NULL;
}

int
main(int argc, char **argv)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	struct mlx5dv_context_attr cattr = { .flags = MLX5DV_CONTEXT_FLAGS_DEVX };
	struct ibv_pd *pd;
	struct mlx5dv_pd dvpd;
	struct mlx5dv_obj obj;
	struct mlx5dv_devx_uar *uar;
	struct mlx5dv_devx_obj *cq_obj, *qp_obj;
	struct region rc_cq, rc_wq, *rcq, *rwq;
	struct ibv_mr *mr_src, *mr_dst;
	union ibv_gid gid;
	uint8_t mac[6];
	uint32_t eqn = 0, cqn, qpn;
	void *src, *dst, *d_src, *d_uar;
	struct dp_args a;
	struct dp_out o;
	unsigned long long *post, *comp;
	double wall = 0;
	int op, k, v2;

	while ((op = getopt(argc, argv, "d:g:x:n:C:W:F:h")) != -1) {
		switch (op) {
		case 'd': g_dev = optarg; break;
		case 'g': g_gpu = atoi(optarg); break;
		case 'x': g_gid = atoi(optarg); break;
		case 'n': g_n = (uint32_t)atoi(optarg); break;
		case 'C': g_cq_gpu = optarg[0] == 'g'; break;
		case 'W': g_wq_gpu = optarg[0] == 'g'; break;
		case 'F': g_flag = atoi(optarg); break;
		default: usage(argv[0]); return 1;
		}
	}
	if (g_n == 0) {
		usage(argv[0]);
		return 1;
	}
	printf("[配置] %s GPU %d gid %d,CQ 在%s,SQ 在%s,%u 轮\n", g_dev, g_gpu,
	       g_gid, g_cq_gpu ? "显存" : "主存", g_wq_gpu ? "显存" : "主存", g_n);

	if (gpu_set_dev(g_gpu) != 0) {
		fprintf(stderr, "选 GPU %d 失败\n", g_gpu);
		return 1;
	}

	/* ---- DevX 上下文、PD、EQ、UAR ---- */
	list = ibv_get_device_list(NULL);
	for (k = 0; list && list[k]; k++) {
		if (strcmp(ibv_get_device_name(list[k]), g_dev) == 0) {
			ctx = mlx5dv_open_device(list[k], &cattr);
			break;
		}
	}
	if (!ctx) {
		fprintf(stderr, "以 DevX 方式打开 %s 失败: %s\n", g_dev, strerror(errno));
		return 1;
	}
	pd = ibv_alloc_pd(ctx);
	obj.pd.in = pd;
	obj.pd.out = &dvpd;
	if (!pd || mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD)) {
		fprintf(stderr, "PD 失败\n");
		return 1;
	}
	if (mlx5dv_devx_query_eqn(ctx, 0, &eqn)) {
		fprintf(stderr, "query_eqn 失败: %s\n", strerror(errno));
		return 1;
	}
	uar = mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_NC);
	if (!uar) {
		uar = mlx5dv_devx_alloc_uar(ctx, MLX5DV_UAR_ALLOC_TYPE_BF);
	}
	if (!uar) {
		fprintf(stderr, "alloc_uar 失败: %s\n", strerror(errno));
		return 1;
	}
	if (gpu_map_host(uar->base_addr, 4096, MC_HOST_REGISTER_IO_MEMORY |
			 MC_HOST_REGISTER_MAPPED, &d_uar) != 0) {
		fprintf(stderr, "UAR 映射给 GPU 失败\n");
		return 1;
	}
	printf("[DevX] pdn=%u eqn=%u uar page_id=%u base=%p reg=%p\n", dvpd.pdn,
	       eqn, uar->page_id, uar->base_addr, uar->reg_addr);

	/* ---- 队列内存 ---- */
	if (region_init(&rc_cq, ctx, g_cq_gpu) != 0) {
		return 1;
	}
	if (g_wq_gpu == g_cq_gpu) {
		rwq = &rc_cq;
	} else {
		if (region_init(&rc_wq, ctx, g_wq_gpu) != 0) {
			return 1;
		}
		rwq = &rc_wq;
	}
	rcq = &rc_cq;

	/* ---- CREATE_CQ ---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = { 0 };
		void *cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);

		DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);
		DEVX_SET(cqc, cqc, log_cq_size, 6);
		DEVX_SET(cqc, cqc, uar_page, uar->page_id);
		DEVX_SET(cqc, cqc, c_eqn, eqn);
		DEVX_SET(cqc, cqc, cqe_sz, 0);		/* 64 字节 */
		DEVX_SET(cqc, cqc, log_page_size, 0);	/* 4 KiB,同 umem */
		DEVX_SET(cqc, cqc, dbr_umem_valid, 1);
		DEVX_SET(cqc, cqc, dbr_umem_id, rcq->umem->umem_id);
		DEVX_SET64(cqc, cqc, dbr_addr, OFF_CQ_DBR);
		DEVX_SET(create_cq_in, in, cq_umem_valid, 1);
		DEVX_SET(create_cq_in, in, cq_umem_id, rcq->umem->umem_id);
		DEVX_SET64(create_cq_in, in, cq_umem_offset, OFF_CQ);
		cq_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out,
						sizeof(out));
		if (!cq_obj) {
			cmd_err("CREATE_CQ", out);
			return 1;
		}
		cqn = DEVX_GET(create_cq_out, out, cqn);
	}

	/* ---- CREATE_QP(RC,无 RQ)---- */
	{
		uint32_t in[DEVX_ST_SZ_DW(create_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(create_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(create_qp_in, in, qpc);

		DEVX_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);
		DEVX_SET(qpc, qpc, st, MLX5_QPC_ST_RC);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, pd, dvpd.pdn);
		DEVX_SET(qpc, qpc, uar_page, uar->page_id);
		DEVX_SET(qpc, qpc, cqn_snd, cqn);
		DEVX_SET(qpc, qpc, cqn_rcv, cqn);
		DEVX_SET(qpc, qpc, log_sq_size, 6);
		DEVX_SET(qpc, qpc, rq_type, MLX5_QPC_RQ_TYPE_ZERO_LEN);
		DEVX_SET(qpc, qpc, ts_format, 1);
		DEVX_SET(qpc, qpc, log_page_size, 0);
		DEVX_SET(qpc, qpc, dbr_umem_valid, 1);
		DEVX_SET(qpc, qpc, dbr_umem_id, rwq->umem->umem_id);
		DEVX_SET64(qpc, qpc, dbr_addr, OFF_QP_DBR);
		DEVX_SET(create_qp_in, in, wq_umem_valid, 1);
		DEVX_SET(create_qp_in, in, wq_umem_id, rwq->umem->umem_id);
		DEVX_SET64(create_qp_in, in, wq_umem_offset, OFF_WQ);
		qp_obj = mlx5dv_devx_obj_create(ctx, in, sizeof(in), out,
						sizeof(out));
		if (!qp_obj) {
			cmd_err("CREATE_QP", out);
			return 1;
		}
		qpn = DEVX_GET(create_qp_out, out, qpn);
	}
	printf("[DevX] CQN=0x%x QPN=0x%x\n", cqn, qpn);

	/* ---- 回环:对端就是自己 ---- */
	if (ibv_query_gid(ctx, 1, g_gid, &gid) || read_mac(g_dev, mac)) {
		fprintf(stderr, "查 GID %d / MAC 失败\n", g_gid);
		return 1;
	}
	v2 = gid_is_v2(g_dev, g_gid);
	printf("[地址] gid[%d] %s mac %02x:%02x:%02x:%02x:%02x:%02x\n", g_gid,
	       v2 ? "RoCE v2" : "RoCE v1", mac[0], mac[1], mac[2], mac[3],
	       mac[4], mac[5]);

	{
		uint32_t in[DEVX_ST_SZ_DW(rst2init_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rst2init_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);

		DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
		DEVX_SET(rst2init_qp_in, in, qpn, qpn);
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, 1);
		DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
		DEVX_SET(qpc, qpc, rre, 1);
		DEVX_SET(qpc, qpc, rwe, 1);
		if (mlx5dv_devx_obj_modify(qp_obj, in, sizeof(in), out,
					   sizeof(out))) {
			cmd_err("RST2INIT", out);
			return 1;
		}
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(init2rtr_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(init2rtr_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);

		DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
		DEVX_SET(init2rtr_qp_in, in, qpn, qpn);
		DEVX_SET(qpc, qpc, mtu, 3);			/* 1024 */
		DEVX_SET(qpc, qpc, log_msg_max, 20);
		DEVX_SET(qpc, qpc, remote_qpn, qpn);
		DEVX_SET(qpc, qpc, next_rcv_psn, 0);
		DEVX_SET(qpc, qpc, min_rnr_nak, 12);
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, 1);
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
		       gid.raw, 16);
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
		       mac, 6);
		DEVX_SET(qpc, qpc, primary_address_path.src_addr_index, g_gid);
		DEVX_SET(qpc, qpc, primary_address_path.hop_limit, 64);
		if (v2) {
			DEVX_SET(qpc, qpc, primary_address_path.udp_sport, 0xdcba);
		}
		if (mlx5dv_devx_obj_modify(qp_obj, in, sizeof(in), out,
					   sizeof(out))) {
			cmd_err("INIT2RTR", out);
			return 1;
		}
	}
	{
		uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = { 0 };
		uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = { 0 };
		void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);

		DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
		DEVX_SET(rtr2rts_qp_in, in, qpn, qpn);
		DEVX_SET(qpc, qpc, retry_count, 7);
		DEVX_SET(qpc, qpc, rnr_retry, 7);
		DEVX_SET(qpc, qpc, next_send_psn, 0);
		DEVX_SET(qpc, qpc, log_sra_max, 0);
		DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, 1);
		DEVX_SET(qpc, qpc, primary_address_path.ack_timeout, 14);
		if (mlx5dv_devx_obj_modify(qp_obj, in, sizeof(in), out,
					   sizeof(out))) {
			cmd_err("RTR2RTS", out);
			return 1;
		}
	}
	printf("[QP] RTS\n");

	/* ---- payload:主存 src / dst,各 4 KiB ---- */
	if (posix_memalign(&src, 4096, 4096) || posix_memalign(&dst, 4096, 4096)) {
		return 1;
	}
	memset(src, 0, 4096);
	memset(dst, 0, 4096);
	mr_src = ibv_reg_mr(pd, src, 4096, IBV_ACCESS_LOCAL_WRITE);
	mr_dst = ibv_reg_mr(pd, dst, 4096, IBV_ACCESS_LOCAL_WRITE |
			    IBV_ACCESS_REMOTE_WRITE);
	if (!mr_src || !mr_dst ||
	    gpu_map_host(src, 4096, MC_HOST_REGISTER_MAPPED, &d_src) != 0) {
		fprintf(stderr, "payload MR / 映射失败\n");
		return 1;
	}

	memset(&a, 0, sizeof(a));
	a.sq = (volatile uint8_t *)rwq->dev + OFF_WQ;
	a.sq_cnt = Q_CNT;
	a.qp_dbr = (volatile uint32_t *)((uint8_t *)rwq->dev + OFF_QP_DBR);
	a.db = (volatile uint64_t *)((uint8_t *)d_uar +
				     ((uint8_t *)uar->reg_addr -
				      (uint8_t *)uar->base_addr));
	a.qpn = qpn;
	a.cq = (volatile uint8_t *)rcq->dev + OFF_CQ;
	a.cq_cnt = Q_CNT;
	a.cq_dbr = (volatile uint32_t *)((uint8_t *)rcq->dev + OFF_CQ_DBR);
	a.src_dev = (volatile uint64_t *)d_src;
	a.laddr = (uint64_t)src;
	a.lkey = mr_src->lkey;
	a.raddr = (uint64_t)dst;
	a.rkey = mr_dst->rkey;
	a.len = 8;
	a.n = g_n;
	a.sq_in_gpu = rwq->in_gpu;
	a.cq_in_gpu = rcq->in_gpu;

	post = calloc(g_n, sizeof(*post));
	comp = calloc(g_n, sizeof(*comp));
	if (dp_run(&a, &o, post, comp, &wall) != 0) {
		return 1;
	}

	/* ---- 结果 ---- */
	{
		double khz = (o.k_end > o.k_start && wall > 0) ?
			     (o.k_end - o.k_start) / wall * 1000.0 : 1.6e6;
		double us = 1000.0 / khz;	/* 每 cycle 多少 us */
		uint32_t n = o.done;
		uint64_t last = *(volatile uint64_t *)dst;

		printf("[运行] 完成 %u / %u,kernel %.0f us,实测时钟 %.0f kHz\n",
		       n, g_n, wall, khz);
		if (o.err) {
			printf("[错误] 第 %u 轮 %s", o.err_iter,
			       o.err == DP_E_TIMEOUT ? "等不到 CQE" : "错误 CQE");
			if (o.err == DP_E_CQE) {
				printf(" syndrome=0x%02x vendor=0x%02x",
				       o.syndrome & 0xff, o.syndrome >> 8);
			}
			printf("\n");
		}
		if (n) {
			qsort(post, n, sizeof(*post), cmp_u64);
			qsort(comp, n, sizeof(*comp), cmp_u64);
			printf("[时延] 提交(payload+WQE+dbrec+门铃) p50 %.2f us p90 %.2f\n",
			       post[n / 2] * us, post[n * 9 / 10] * us);
			printf("[时延] 门铃→看见 CQE              p50 %.2f us p90 %.2f\n",
			       comp[n / 2] * us, comp[n * 9 / 10] * us);
			printf("[轮询] 共 %" PRIu64 " 圈,每轮 %.1f 圈\n", o.spins,
			       (double)o.spins / n);
		}
		printf("[校验] dst = %" PRIu64 ",期望 %u  %s\n", last, n,
		       last == n && n == g_n && !o.err ? "*** PASS ***" :
		       "*** FAIL ***");
	}

	mlx5dv_devx_obj_destroy(qp_obj);
	mlx5dv_devx_obj_destroy(cq_obj);
	return 0;
}
