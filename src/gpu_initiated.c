/*
 * gpu_initiated.c  --  GPU-initiated NVMe-oF 原型的 host 侧
 *
 * CPU 只做初始化:SPDK 建连、Fabric Connect、注册 MR、把 SQ/CQ/dbrec/UAR
 * 映射给 GPU。稳态收发全在 kernel 内 —— 构造命令胶囊、写 WQE、敲门铃、
 * 轮询 CQ,一条都不经过 CPU。
 *
 * 出错时 CPU 兜底:kernel 只报错误码,QP 恢复要 modify_qp,那是 host API。
 *
 * ============ 用法 ============
 *
 *   -a <ip> -n <nqn>     必填
 *   -g <id>              GPU 编号
 *   -r <n> -b <bytes>    轮数、每轮字节数
 *   -W                   发 WRITE(默认 READ)
 *   -H                   命令胶囊放显存(默认主存)。见下面的已知问题。
 *   -P                   只把 LBA 0 涂成 0xCC 然后退出
 *   -V                   只回读 LBA 0 然后退出
 *
 * 写路径的校验必须用 -P / -V 分三个进程做:
 *
 *   ./gpu_initiated ... -P        涂 0xCC
 *   ./gpu_initiated ... -W -r 1   kernel 写
 *   ./gpu_initiated ... -V        看是不是变成 pattern
 *
 * 不能在本进程里回读 —— 见 docs/debug-notes.md 第 4 条。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <endian.h>

#include "gpu_io_ctx.h"

extern int gpu_io_run(struct gpu_io_ctx *, unsigned int, unsigned long long,
		      unsigned long long *, int *, unsigned int *);
extern int gpu_alloc_exportable(void **, int *, size_t);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_set_dev(int);
extern int gpu_clock_khz(int, int *);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_memset_dev(void *, int, size_t);
extern int gpu_sync(void);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define CAP_AREA_LEN	(2 * 1024 * 1024)
#define RPAT		0xbb		/* 读:预填,被 target 覆盖才算落地 */
#define WPAT		0x5a		/* 写:pattern */
#define CPAT		0xcc		/* -P 涂色 */

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;
static struct ibv_pd		*g_pd;
static struct ibv_mr		*g_data_mr, *g_cap_mr;

static void	*g_base_gpu, *g_data_gpu;
static void	*g_cap_host, *g_cap_dev, *g_cap_hbm;
static size_t	g_data_len, g_total_len;
static int	g_base_fd = -1;

static char	g_traddr[64], g_nqn[224], g_trsvcid[16] = "4420";
static int	g_gpu_id = 4;
static uint32_t	g_rounds = 100;
static uint32_t	g_io_bytes = 4096;
static int	g_write, g_cap_in_hbm, g_paint, g_verify;

/* ================================================================== */
/* SPDK hooks:让 SPDK 用我们的 PD,并把显存注册成 dma-buf MR          */
/* ================================================================== */

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

	/*
	 * 显存只有一块分配,一个 MR 覆盖全部(-H 时数据区和胶囊区都在里面)。
	 *
	 * 别拆成两次 mcMalloc 各自导出各自注册:MACA 的 dma-buf,offset 0
	 * 指的是底层分配的基址,不是你传进去那个子区间的起点。两块相邻的
	 * mcMalloc 很可能落在同一个 slab,于是第二个 MR 实际映射到第一块的
	 * 物理页,网卡读出来是另一块的内容。症状是 target 报
	 *   Invalid NVMf I/O Command SGL: Type 0xa, Subtype 0xa
	 * —— 0xAA 正是另一块的 memset 值,换成 0xBB 就变成 0xb/0xb,
	 * 换成 0x5A 就变成 0x5/0xa。这个对应关系是定位它的关键线索。
	 */
	if (g_base_gpu && a >= (uint64_t)g_base_gpu &&
	    a + size <= (uint64_t)g_base_gpu + g_total_len) {
		if (!g_data_mr) {
			g_data_mr = ibv_reg_dmabuf_mr(pd, 0, g_total_len,
						      (uint64_t)g_base_gpu,
						      g_base_fd, acc);
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

/* ================================================================== */

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
io_cb(void *a, const struct spdk_nvme_cpl *cpl)
{
	*(int *)a = spdk_nvme_cpl_is_error(cpl) ? -1 : 1;
}

/* 用 SPDK 的 CPU 路径发一条并等完成。接管 QP 之前用,之后不能再用。 */
static int
cpu_io(int is_write, void *buf, uint64_t lba, uint32_t nlb)
{
	int st = 0;
	int rc;

	rc = is_write ?
	     spdk_nvme_ns_cmd_write(g_ns, g_qpair, buf, lba, nlb, io_cb, &st, 0) :
	     spdk_nvme_ns_cmd_read(g_ns, g_qpair, buf, lba, nlb, io_cb, &st, 0);
	if (rc != 0) {
		return -1;
	}
	while (!st) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}
	return st > 0 ? 0 : -1;
}

static int cmp_d(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void usage(const char *p)
{
	printf("用法: %s -a <ip> -n <nqn> [-g gpu] [-r rounds] [-b bytes]\n"
	       "      [-W 发写] [-H 胶囊放显存] [-P 涂色] [-V 回读]\n", p);
}

/* ================================================================== */

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
	uint32_t sector, nlb;
	int err = 0, clk_khz = 0, op;
	unsigned int done = 0, i;
	void *d_sq, *d_dbrec, *d_bf, *d_cq, *d_cqdb;

	memset(&dvq, 0, sizeof(dvq));
	memset(&dvc, 0, sizeof(dvc));
	memset(&ctx, 0, sizeof(ctx));

	while ((op = getopt(argc, argv, "a:s:n:g:r:b:WHPV")) != -1) {
		switch (op) {
		case 'a': snprintf(g_traddr, sizeof(g_traddr), "%s", optarg); break;
		case 's': snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_nqn, sizeof(g_nqn), "%s", optarg); break;
		case 'g': g_gpu_id = atoi(optarg); break;
		case 'r': g_rounds = (unsigned)atoi(optarg); break;
		case 'b': g_io_bytes = (unsigned)atoi(optarg); break;
		case 'W': g_write = 1; break;
		case 'H': g_cap_in_hbm = 1; break;
		case 'P': g_paint = 1; break;
		case 'V': g_verify = 1; break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (!g_traddr[0] || !g_nqn[0]) {
		usage(argv[0]);
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

	/* ---- 显存:一块分配,-H 时把胶囊切在后半段 ---- */
	g_data_len = (size_t)g_io_bytes * 2;
	if (g_data_len < 2 * 1024 * 1024) {
		g_data_len = 2 * 1024 * 1024;
	}
	g_total_len = g_data_len + (g_cap_in_hbm ? CAP_AREA_LEN : 0);
	if (gpu_alloc_exportable(&g_base_gpu, &g_base_fd, g_total_len) != 0) {
		fprintf(stderr, "显存分配失败\n");
		return 1;
	}
	g_data_gpu = g_base_gpu;
	spdk_mem_register(g_base_gpu, g_total_len);

	if (g_cap_in_hbm) {
		g_cap_hbm = (char *)g_base_gpu + g_data_len;
		gpu_memset_dev(g_cap_hbm, 0, CAP_AREA_LEN);
		gpu_sync();
		printf("[GPU] data=%p (%zu)  capsule(HBM)=%p\n",
		       g_data_gpu, g_data_len, g_cap_hbm);
	} else {
		if (posix_memalign(&g_cap_host, 4096, CAP_AREA_LEN) != 0) {
			fprintf(stderr, "胶囊主存分配失败\n");
			return 1;
		}
		memset(g_cap_host, 0, CAP_AREA_LEN);
		printf("[GPU] data=%p (%zu)  capsule(host)=%p\n",
		       g_data_gpu, g_data_len, g_cap_host);
	}

	/* ---- 建连 ---- */
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
	sector = spdk_nvme_ns_get_sector_size(g_ns);
	nlb = g_io_bytes / sector;

	/* ---- -P / -V:纯 CPU 路径,做完就退 ---- */
	if (g_paint || g_verify) {
		void *b = spdk_dma_zmalloc(g_io_bytes, 4096, NULL);
		unsigned char *p8 = b;
		unsigned q, npat = 0, ncpat = 0;
		int rc;

		if (!b) {
			return 1;
		}
		if (g_paint) {
			memset(b, CPAT, g_io_bytes);
			rc = cpu_io(1, b, 0, nlb);
			printf("[涂色] LBA 0 填 0x%02x %s\n", CPAT,
			       rc == 0 ? "成功" : "失败");
		} else {
			rc = cpu_io(0, b, 0, nlb);
			if (rc != 0) {
				printf("[回读] 失败\n");
			} else {
				for (q = 0; q < g_io_bytes; q++) {
					if (p8[q] == WPAT) npat++;
					else if (p8[q] == CPAT) ncpat++;
				}
				printf("[回读] LBA 0: 0x%02x %u  0x%02x %u  前8字节 ",
				       WPAT, npat, CPAT, ncpat);
				for (q = 0; q < 8; q++) {
					printf("%02x ", p8[q]);
				}
				printf("\n  %s\n",
				       npat == g_io_bytes ? "*** 写成功了 ***" :
				       ncpat == g_io_bytes ? "*** 没写,还是涂色 ***" :
				       "*** 内容是别的东西 ***");
			}
		}
		spdk_dma_free(b);
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
		spdk_nvme_detach(g_ctrlr);
		return 0;
	}

	/*
	 * 预热:用 SPDK 正常发一条,触发 hook 注册显存 MR 拿 rkey,
	 * 同时让 recv buffer 池进入稳定状态。之后才能交给 GPU。
	 */
	if (cpu_io(0, g_data_gpu, 0, nlb) != 0) {
		fprintf(stderr, "预热 I/O 出错\n");
		return 1;
	}
	printf("[预热] SPDK 路径正常,显存 rkey=0x%x\n",
	       g_data_mr ? g_data_mr->rkey : 0);

	/* ---- 胶囊的 MR ---- */
	if (g_cap_in_hbm) {
		/* 预热已让 hook 把整块注册成一个 MR,直接复用 */
		g_cap_mr = g_data_mr;
		g_cap_dev = g_cap_hbm;
		if (!g_cap_mr) {
			fprintf(stderr, "预热未产生 MR\n");
			return 1;
		}
		printf("[MR] 胶囊 lkey=0x%x (与数据区同一 MR)\n",
		       g_cap_mr->lkey);
	} else {
		g_cap_mr = ibv_reg_mr(g_pd, g_cap_host, CAP_AREA_LEN,
				      IBV_ACCESS_LOCAL_WRITE);
		if (!g_cap_mr) {
			fprintf(stderr, "胶囊注册失败: %s\n", strerror(errno));
			return 1;
		}
		if (gpu_map_host(g_cap_host, CAP_AREA_LEN,
				 MC_HOST_REGISTER_MAPPED, &g_cap_dev) != 0) {
			fprintf(stderr, "胶囊映射给 GPU 失败\n");
			return 1;
		}
		printf("[MR] 胶囊 lkey=0x%x  host=%p dev=%p\n",
		       g_cap_mr->lkey, g_cap_host, g_cap_dev);
	}

	/* ---- 把队列交给 GPU ---- */
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
	/* capsule 是 GPU 写用的指针,capsule_addr 是网卡读用的地址。
	 * 胶囊在主存时两者不同(前者是 mcHostRegister 的 device 指针)。 */
	ctx.capsule      = (volatile uint8_t *)g_cap_dev;
	ctx.capsule_addr = g_cap_in_hbm ? (uint64_t)g_cap_hbm
					: (uint64_t)g_cap_host;
	ctx.capsule_lkey = g_cap_mr->lkey;
	ctx.data_addr    = (uint64_t)g_data_gpu;
	ctx.data_rkey    = g_data_mr->rkey;
	ctx.nsid         = spdk_nvme_ns_get_id(g_ns);
	ctx.sector_size  = sector;
	ctx.io_bytes     = g_io_bytes;
	ctx.nvme_opc     = g_write ? 0x01 : 0x02;

	/*
	 * kernel 不补 recv buffer,每轮响应消耗一个。RQ 里现成有多少就只能
	 * 跑多少轮 —— 超出的那轮响应无处安放,表现成和 CQE 判错一模一样的
	 * 死等,别混淆。
	 */
	if (g_rounds > dvq.rq.wqe_cnt) {
		printf("[警告] RQ 深度 %u < 轮数 %u,第 %u 轮之后会卡住\n",
		       dvq.rq.wqe_cnt, g_rounds, dvq.rq.wqe_cnt);
	}

	/* ---- 准备源数据 ---- */
	gpu_memset_dev(g_data_gpu, g_write ? WPAT : RPAT, g_io_bytes);
	gpu_sync();		/* 必须:mcMemset 对 host 是异步的 */

	if (g_write) {
		/* 目标 LBA 涂成 0xCC,-V 时就能区分"没写"和"写了零" */
		void *b = spdk_dma_zmalloc(g_io_bytes, 4096, NULL);

		if (b) {
			memset(b, CPAT, g_io_bytes);
			cpu_io(1, b, 0, nlb);
			spdk_dma_free(b);
			printf("[涂色] LBA 0 已填 0x%02x\n", CPAT);
		}
	}

	/*
	 * 快照必须在所有 CPU 侧 I/O 之后拍 —— 上面的预热和涂色都会推进
	 * SQ/CQ。用过期的 pi,kernel 会写进 SPDK 刚用过的 slot,而且 dbrec
	 * 已经等于 pi+1,网卡认为没有新工作,命令根本发不出去,表现成
	 * "完成 N/N"但 target 侧查无此命令。这个坑踩过两次。
	 */
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

	/* ---- 跑 ---- */
	cycles = calloc(g_rounds, sizeof(*cycles));
	us = calloc(g_rounds, sizeof(*us));
	printf("\n[运行] %s %u 轮, 每轮 %u 字节\n",
	       g_write ? "WRITE" : "READ", g_rounds, g_io_bytes);

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
		struct ibv_qp_attr a;
		struct ibv_qp_init_attr ia;

		if (ibv_query_qp(qp, &a, IBV_QP_STATE, &ia) == 0) {
			fprintf(stderr, "QP state=%d (3=RTS 正常, 6=ERROR)\n",
				a.qp_state);
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

	/* ---- 校验 ---- */
	if (g_write) {
		/*
		 * 写的校验不能在本进程做。kernel 接管过这条 qpair —— 消费了
		 * CQE、占了 RQ slot,SPDK 对它的内部状态已经失同步。再调
		 * spdk_nvme_ns_cmd_read 要么静默读到全零(看着像"数据没落盘",
		 * 白查一整天),要么直接在 nvme_rdma_process_recv_completion
		 * 里空指针崩。
		 */
		printf("\n[校验] 写路径请另起进程验证:\n");
		printf("  %s -a %s -n %s -g %d -V -b %u\n",
		       argv[0], g_traddr, g_nqn, g_gpu_id, g_io_bytes);
		printf("  期望看到 0x%02x %u\n", WPAT, g_io_bytes);
	} else {
		/* 读的校验走 gpu_copy_to_host,不碰 qpair,可信 */
		unsigned char *h = malloc(g_io_bytes);
		unsigned nz = 0;

		gpu_copy_to_host(h, g_data_gpu, g_io_bytes);
		for (i = 0; i < g_io_bytes; i++) {
			if (h[i] != RPAT) {
				nz++;
			}
		}
		printf("\n[校验] %u/%u 字节不再是 0x%02x  前8字节 ",
		       nz, g_io_bytes, RPAT);
		for (i = 0; i < 8; i++) {
			printf("%02x ", h[i]);
		}
		printf("\n  %s\n", nz == g_io_bytes ? "*** 数据落进显存了 ***" :
		       "*** DMA 没落到显存 ***");
		free(h);
	}

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	return 0;
}
