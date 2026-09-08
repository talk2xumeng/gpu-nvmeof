/*
 * gpu_initiated.c  --  GPU-initiated NVMe-oF 原型的 host 侧
 *
 * CPU 只做初始化:SPDK 建连、Fabric Connect、注册 MR、把队列映射给
 * GPU。稳态收发全在 kernel 内。
 *
 * 出错时 CPU 兜底 —— kernel 只报错误码,QP 恢复要 modify_qp,
 * 那是 host API,kernel 里做不了。原型阶段够用。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <endian.h>

/* 与 gpu_io_kernel.cpp 中的定义保持一致 */
struct gpu_io_ctx {
	volatile uint8_t	*sq_buf;
	volatile uint32_t	*qp_dbrec;
	volatile uint64_t	*bf_reg;
	volatile uint8_t	*cq_buf;
	volatile uint32_t	*cq_dbrec;
	uint32_t		sq_wqe_cnt;
	uint32_t		sq_stride;
	uint32_t		cq_cnt;
	uint32_t		cqe_size;
	uint32_t		qpn;
	volatile uint8_t	*capsule;
	uint32_t		capsule_lkey;
	uint64_t		capsule_addr;
	uint64_t		data_addr;
	uint32_t		data_rkey;
	uint32_t		nsid;
	uint32_t		sector_size;
	uint32_t		io_bytes;
	uint16_t		sq_pi;
	uint32_t		cq_ci;
	uint8_t			cq_phase;
};

extern int gpu_io_run(struct gpu_io_ctx *, unsigned int, unsigned long long,
		      unsigned long long *, int *, unsigned int *);
extern int gpu_ring_bench(void *, unsigned long long, unsigned int,
			  unsigned long long *);
extern int gpu_alloc_exportable(void **, int *, size_t);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_set_dev(int);
extern int gpu_clock_khz(int, int *);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_memset_dev(void *, int, size_t);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;
static struct ibv_pd		*g_pd;
static struct ibv_mr		*g_data_mr, *g_cap_mr;
static void	*g_data_gpu, *g_cap_gpu;
static size_t	g_data_len;
static int	g_data_fd = -1, g_cap_fd = -1;
static char	g_traddr[64], g_nqn[224], g_trsvcid[16] = "4420";
static int	g_gpu_id = 4;
static uint32_t	g_rounds = 100;
static uint32_t	g_io_bytes = 4096;

static struct ibv_pd *
hook_get_pd(const struct spdk_nvme_transport_id *t, struct ibv_context *v)
{
	if (!g_pd) {
		g_pd = ibv_alloc_pd(v);
		printf("[HOOK] 网卡 %s\n", ibv_get_device_name(v->device));
	}
	return g_pd;
}

static uint64_t
hook_get_rkey(struct ibv_pd *pd, void *buf, size_t size)
{
	static struct ibv_mr *mrs[256];
	static void *addrs[256];
	static size_t lens[256];
	static int n;
	int acc = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
		  IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING;
	uint64_t a = (uint64_t)buf;
	int i;

	if (g_data_gpu && a >= (uint64_t)g_data_gpu &&
	    a + size <= (uint64_t)g_data_gpu + g_data_len) {
		if (!g_data_mr) {
			g_data_mr = ibv_reg_dmabuf_mr(pd, 0, g_data_len,
						      (uint64_t)g_data_gpu,
						      g_data_fd, acc);
		}
		return g_data_mr ? g_data_mr->rkey : 0;
	}
	for (i = 0; i < n; i++) {
		if (a >= (uint64_t)addrs[i] &&
		    a + size <= (uint64_t)addrs[i] + lens[i]) {
			return mrs[i]->rkey;
		}
	}
	if (n >= 256) {
		return 0;
	}
	mrs[n] = ibv_reg_mr(pd, buf, size, acc);
	if (!mrs[n]) {
		return 0;
	}
	addrs[n] = buf;
	lens[n] = size;
	return mrs[n++]->rkey;
}

static void hook_put_rkey(uint64_t k) { (void)k; }

static struct spdk_nvme_rdma_hooks g_hooks = {
	.get_ibv_pd = hook_get_pd,
	.get_rkey   = hook_get_rkey,
	.put_rkey   = hook_put_rkey,
};

static bool
probe_cb(void *c, const struct spdk_nvme_transport_id *t,
	 struct spdk_nvme_ctrlr_opts *o)
{
	o->num_io_queues = 2;
	return true;
}

static void
attach_cb(void *c, const struct spdk_nvme_transport_id *t,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *o)
{
	int nsid;

	g_ctrlr = ctrlr;
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

		if (spdk_nvme_ns_is_active(ns) && !g_ns) {
			g_ns = ns;
		}
	}
}

static void
warm_cb(void *a, const struct spdk_nvme_cpl *cpl)
{
	*(int *)a = spdk_nvme_cpl_is_error(cpl) ? -1 : 1;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	struct ibv_qp *qp;
	struct ibv_cq *cq;
	struct mlx5dv_qp dvq;
	struct mlx5dv_cq dvc;
	struct mlx5dv_obj obj;
	struct gpu_io_ctx ctx;
	unsigned long long *cycles;
	double *us;
	int err = 0, clk_khz = 0, op;
	unsigned int done = 0, i;
	void *d_sq, *d_dbrec, *d_bf, *d_cq, *d_cqdb;

	memset(&dvq, 0, sizeof(dvq));
	memset(&dvc, 0, sizeof(dvc));
	memset(&ctx, 0, sizeof(ctx));

	while ((op = getopt(argc, argv, "a:s:n:g:r:b:")) != -1) {
		switch (op) {
		case 'a': snprintf(g_traddr, sizeof(g_traddr), "%s", optarg); break;
		case 's': snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_nqn, sizeof(g_nqn), "%s", optarg); break;
		case 'g': g_gpu_id = atoi(optarg); break;
		case 'r': g_rounds = (unsigned)atoi(optarg); break;
		case 'b': g_io_bytes = (unsigned)atoi(optarg); break;
		default:
			printf("用法: %s -a <ip> -n <nqn> [-g gpu] [-r rounds] [-b bytes]\n", argv[0]);
			return 1;
		}
	}
	if (!g_traddr[0] || !g_nqn[0]) {
		printf("用法: %s -a <ip> -n <nqn> [-g gpu] [-r rounds] [-b bytes]\n", argv[0]);
		return 1;
	}

	printf("=========================================================\n");
	printf(" GPU-initiated NVMe-oF 原型\n");
	printf(" CPU 只做初始化,稳态收发全在 kernel 内\n");
	printf("=========================================================\n");

	spdk_env_opts_init(&opts);
	opts.opts_size = sizeof(opts);
	opts.name = "gpu_initiated";
	opts.no_pci = true;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init 失败\n");
		return 1;
	}

	if (gpu_set_dev(g_gpu_id) != 0) {
		fprintf(stderr, "选 GPU %d 失败\n", g_gpu_id);
		return 1;
	}
	gpu_clock_khz(g_gpu_id, &clk_khz);
	printf("\n[GPU] dev=%d clock=%d kHz\n", g_gpu_id, clk_khz);

	g_data_len = (size_t)g_io_bytes * 2;
	if (g_data_len < 2 * 1024 * 1024) {
		g_data_len = 2 * 1024 * 1024;
	}
	if (gpu_alloc_exportable(&g_data_gpu, &g_data_fd, g_data_len) != 0) {
		fprintf(stderr, "数据区分配失败\n");
		return 1;
	}
	if (gpu_alloc_exportable(&g_cap_gpu, &g_cap_fd, 2 * 1024 * 1024) != 0) {
		fprintf(stderr, "胶囊区分配失败\n");
		return 1;
	}
	spdk_mem_register(g_data_gpu, g_data_len);
	printf("[GPU] data=%p (%zu) capsule=%p\n", g_data_gpu, g_data_len, g_cap_gpu);

	spdk_nvme_rdma_init_hooks(&g_hooks);
	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", g_traddr);
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", g_trsvcid);
	snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", g_nqn);

	if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) || !g_ns) {
		fprintf(stderr, "连接失败\n");
		return 1;
	}
	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (!g_qpair) {
		fprintf(stderr, "alloc_io_qpair 失败\n");
		return 1;
	}

	/*
	 * 先用 SPDK 正常发一条:触发 hooks 注册显存 MR 拿 rkey,
	 * 同时让 recv buffer 池进入稳定状态。之后交给 GPU。
	 */
	{
		int st = 0;

		if (spdk_nvme_ns_cmd_read(g_ns, g_qpair, g_data_gpu, 0,
					  g_io_bytes / spdk_nvme_ns_get_sector_size(g_ns),
					  warm_cb, &st, 0) != 0) {
			fprintf(stderr, "预热提交失败\n");
			return 1;
		}
		while (!st) {
			spdk_nvme_qpair_process_completions(g_qpair, 0);
		}
		if (st < 0) {
			fprintf(stderr, "预热 I/O 出错\n");
			return 1;
		}
		printf("[预热] SPDK 路径正常,显存 rkey=0x%x\n",
		       g_data_mr ? g_data_mr->rkey : 0);
	}

	g_cap_mr = ibv_reg_dmabuf_mr(g_pd, 0, 2 * 1024 * 1024,
				     (uint64_t)g_cap_gpu, g_cap_fd,
				     IBV_ACCESS_LOCAL_WRITE |
				     IBV_ACCESS_REMOTE_READ |
				     IBV_ACCESS_RELAXED_ORDERING);
	if (!g_cap_mr) {
		fprintf(stderr, "胶囊区注册失败: %s\n", strerror(errno));
		return 1;
	}
	printf("[MR] 胶囊 lkey=0x%x\n", g_cap_mr->lkey);

	qp = spdk_nvme_qpair_get_ibv_qp(g_qpair);
	cq = spdk_nvme_qpair_get_ibv_cq(g_qpair);
	if (!qp || !cq) {
		fprintf(stderr, "拿不到 ibv_qp/ibv_cq,SPDK 打过 patch 吗?\n");
		return 1;
	}
	dvq.comp_mask = MLX5DV_QP_MASK_UAR_MMAP_OFFSET;
	obj.qp.in = qp; obj.qp.out = &dvq;
	obj.cq.in = cq; obj.cq.out = &dvc;
	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ)) {
		fprintf(stderr, "mlx5dv_init_obj 失败\n");
		return 1;
	}
	printf("\n[QP] qpn=0x%x sq %ux%u  cq %ux%u\n",
	       qp->qp_num, dvq.sq.wqe_cnt, dvq.sq.stride,
	       dvc.cqe_cnt, dvc.cqe_size);

	if (gpu_map_host(dvq.sq.buf, (size_t)dvq.sq.wqe_cnt * dvq.sq.stride,
			 MC_HOST_REGISTER_MAPPED, &d_sq) ||
	    gpu_map_host((void *)dvq.dbrec, 64,
			 MC_HOST_REGISTER_MAPPED, &d_dbrec) ||
	    gpu_map_host(dvc.buf, (size_t)dvc.cqe_cnt * dvc.cqe_size,
			 MC_HOST_REGISTER_MAPPED, &d_cq) ||
	    gpu_map_host((void *)dvc.dbrec, 64,
			 MC_HOST_REGISTER_MAPPED, &d_cqdb) ||
	    gpu_map_host(dvq.bf.reg, dvq.bf.size ? dvq.bf.size : 4096,
			 MC_HOST_REGISTER_IO_MEMORY | MC_HOST_REGISTER_MAPPED,
			 &d_bf)) {
		fprintf(stderr, "映射队列给 GPU 失败\n");
		return 1;
	}
	printf("[映射] SQ/CQ/dbrec/UAR 均已交给 GPU\n");

	ctx.sq_buf       = (volatile uint8_t *)d_sq;
	ctx.qp_dbrec     = (volatile uint32_t *)d_dbrec;
	ctx.bf_reg       = (volatile uint64_t *)d_bf;
	ctx.cq_buf       = (volatile uint8_t *)d_cq;
	ctx.cq_dbrec     = (volatile uint32_t *)d_cqdb;
	ctx.sq_wqe_cnt   = dvq.sq.wqe_cnt;
	ctx.sq_stride    = dvq.sq.stride;
	ctx.cq_cnt       = dvc.cqe_cnt;
	ctx.cqe_size     = dvc.cqe_size;
	ctx.qpn          = qp->qp_num;
	ctx.capsule      = (volatile uint8_t *)g_cap_gpu;
	ctx.capsule_lkey = g_cap_mr->lkey;
	ctx.capsule_addr = (uint64_t)g_cap_gpu;
	ctx.data_addr    = (uint64_t)g_data_gpu;
	ctx.data_rkey    = g_data_mr->rkey;
	ctx.nsid         = spdk_nvme_ns_get_id(g_ns);
	ctx.sector_size  = spdk_nvme_ns_get_sector_size(g_ns);
	ctx.io_bytes     = g_io_bytes;

	{
		volatile uint32_t *qdb = (volatile uint32_t *)dvq.dbrec;
		volatile uint32_t *cdb = (volatile uint32_t *)dvc.dbrec;
		uint32_t sq_pi = be32toh(qdb[1]) & 0xffff;
		uint32_t cq_ci = be32toh(cdb[0]) & 0xffffff;

		ctx.sq_pi = (uint16_t)sq_pi;
		ctx.cq_ci = cq_ci;
		ctx.cq_phase = (uint8_t)((cq_ci / dvc.cqe_cnt) & 1);
		printf("[接管] SQ pi=%u  CQ ci=%u phase=%u\n",
		       sq_pi, cq_ci, ctx.cq_phase);
	}

	gpu_memset_dev(g_data_gpu, 0xAA, g_io_bytes);

	{
		unsigned long long rc_[64];

		if (gpu_ring_bench(d_bf, 0, 64, rc_) == 0 && clk_khz) {
			double sum = 0;
			int k;

			for (k = 1; k < 64; k++) {
				sum += rc_[k];
			}
			printf("\n[门铃] kernel 内写一次 UAR: %.2f us\n",
			       sum / 63.0 / (clk_khz / 1000.0));
		}
	}

	cycles = calloc(g_rounds, sizeof(*cycles));
	us = calloc(g_rounds, sizeof(*us));
	printf("\n[运行] %u 轮, 每轮 %u 字节\n", g_rounds, g_io_bytes);

	if (gpu_io_run(&ctx, g_rounds, 0, cycles, &err, &done) != 0) {
		fprintf(stderr, "kernel 执行失败\n");
		return 1;
	}

	printf("[结果] 完成 %u / %u", done, g_rounds);
	if (err) {
		printf(", 错误码 %d (%s)", err,
		       err == -1 ? "轮询超时" : "CQE 报错");
	}
	printf("\n");

	if (done == 0) {
		unsigned char cap[64];
		volatile uint32_t *qdb = (volatile uint32_t *)dvq.dbrec;
		volatile uint32_t *cdb = (volatile uint32_t *)dvc.dbrec;
		uint8_t *slot = (uint8_t *)dvq.sq.buf +
				(size_t)(ctx.sq_pi & (dvq.sq.wqe_cnt - 1)) *
				dvq.sq.stride;
		uint32_t k;

		fprintf(stderr, "\n===== 诊断 =====\n");

		/* kernel 写进 SQ 的 WQE。和 wqe_verify 里 SPDK 的对比:
		 * 前 8 字节应是 opmod_idx_opcode + qpn_ds,
		 * offset 11 应为 0x08(CQ_UPDATE)。 */
		fprintf(stderr, "kernel 写的 WQE @slot %u:\n  ",
			ctx.sq_pi & (dvq.sq.wqe_cnt - 1));
		for (k = 0; k < 32; k++) {
			fprintf(stderr, "%02x ", slot[k]);
			if (k % 16 == 15) fprintf(stderr, "\n  ");
		}
		fprintf(stderr, "\n");

		/* 胶囊在显存,拷回来看 */
		gpu_copy_to_host(cap, g_cap_gpu, 64);
		fprintf(stderr, "kernel 写的胶囊:\n  ");
		for (k = 0; k < 64; k++) {
			fprintf(stderr, "%02x ", cap[k]);
			if (k % 16 == 15) fprintf(stderr, "\n  ");
		}
		fprintf(stderr, "\n");

		fprintf(stderr, "dbrec: RQ=%u SQ=%u (接管时 SQ=%u)\n",
			be32toh(qdb[0]) & 0xffff, be32toh(qdb[1]) & 0xffff,
			ctx.sq_pi);
		fprintf(stderr, "CQ dbrec ci=%u (接管时 %u)\n",
			be32toh(cdb[0]) & 0xffffff, ctx.cq_ci);

		/* CQ 里 ci 附近几个条目的 op_own,看有没有新 CQE */
		fprintf(stderr, "CQ op_own (ci=%u 起 4 个):\n", ctx.cq_ci);
		for (k = 0; k < 4; k++) {
			uint32_t idx = (ctx.cq_ci + k) & (dvc.cqe_cnt - 1);
			uint8_t *c8 = (uint8_t *)dvc.buf +
				      (size_t)idx * dvc.cqe_size;
			uint8_t oo = c8[dvc.cqe_size - 1];

			fprintf(stderr, "  [%u] op_own=0x%02x opcode=%u owner=%u"
				"  期望 owner=%u\n",
				idx, oo, oo >> 4, oo & 1, ctx.cq_phase);
		}

		/* 顺带看看 QP 状态,error 说明网卡拒绝了我们的 WQE */
		{
			struct ibv_qp_attr a;
			struct ibv_qp_init_attr ia;

			if (ibv_query_qp(qp, &a, IBV_QP_STATE, &ia) == 0) {
				fprintf(stderr, "QP state=%d (3=RTS 正常, "
					"6=ERROR)\n", a.qp_state);
			}
		}
		return 1;
	}

	for (i = 0; i < done; i++) {
		us[i] = cycles[i] / (clk_khz / 1000.0);
	}
	qsort(us, done, sizeof(double), cmp_d);
	{
		double sum = 0;

		for (i = 0; i < done; i++) {
			sum += us[i];
		}
		printf("\n===== kernel 内往返延迟 (us) =====\n");
		printf("  mean %.1f   min %.1f   p50 %.1f   p99 %.1f   max %.1f\n",
		       sum / done, us[0], us[done / 2],
		       us[(unsigned)(done * 0.99)], us[done - 1]);
	}

	{
		unsigned char *h = malloc(g_io_bytes);
		unsigned nz = 0;

		gpu_copy_to_host(h, g_data_gpu, g_io_bytes);
		for (i = 0; i < g_io_bytes; i++) {
			if (h[i] != 0xAA) {
				nz++;
			}
		}
		printf("\n[校验] %u/%u 字节不再是 0xAA\n", nz, g_io_bytes);
		printf("  (null bdev 读回全零,此数应接近 %u;若为 0 说明 DMA 没落到显存)\n",
		       g_io_bytes);
		free(h);
	}

	printf("\n---------------------------------------------------------\n");
	printf("和 latency_baseline 的 CPU 路径对比,差值即真实收益。\n");

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	return 0;
}
