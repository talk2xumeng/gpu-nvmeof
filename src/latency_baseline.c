/*
 * latency_baseline.c
 *
 * 测 CPU-initiated 路径的端到端延迟,作为 GPU-initiated 和 DPU-offload
 * 的参照系。
 *
 * 测的不是吞吐。gds_nvmeof 那边 QD=32 流水线跑出的 686 us 是排队时间,
 * 不是延迟。这里 QD=1,而且刻意把 kernel 退出和重启算进去 ——
 * 因为那恰恰是 GPU-initiated 要省掉的部分。
 *
 * 每轮测五段:
 *
 *   t0 ──[计算 kernel + 退出]──> t1 ──[提交 I/O]──> t2
 *      ──[网络传输]──> t3 ──[消费 kernel 启动]──> t4
 *
 *   t1-t0  kernel launch + 执行 + sync,CPU 感知到"要数据了"
 *   t2-t1  构造并提交 NVMe 命令
 *   t3-t2  网络往返 + 数据传输        <- 物理下限,谁也省不掉
 *   t4-t3  kernel launch + 执行 + sync
 *
 *   (t1-t0) + (t2-t1) + (t4-t3) 就是 GPU-initiated 的优化空间。
 *
 * 用 -k 0 可以跳过 kernel,单独看纯 I/O 段,两者相减即 kernel 开销。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include <infiniband/verbs.h>
#include <math.h>

#include "gpu_backend.h"

/* kernels.cpp 提供,由 mxcc 编译 */
extern int  gpu_kernels_init(void **scratch, void **result);
extern void gpu_kernels_fini(void *scratch, void *result);
extern int  gpu_launch_compute(void *scratch, int iters);
extern int  gpu_launch_consume(const void *buf, size_t len,
			       unsigned long long *checksum);
extern int  gpu_launch_empty(void);

/* ================================================================== */

#define MAX_MR_CACHE	4096

struct config {
	char		traddr[64];
	char		trsvcid[16];
	char		nqn[224];
	int		gpu_id;
	uint32_t	block_sz;	/* 单次读取大小 */
	uint32_t	rounds;		/* 测量轮数 */
	uint32_t	warmup;
	int		kernel_iters;	/* 计算 kernel 的工作量,0 = 不跑 kernel */
	bool		use_host_mem;
};

static struct config g_cfg = {
	.trsvcid	= "4420",
	.gpu_id		= 0,
	.block_sz	= 4 * 1024 * 1024 + 608 * 1024,	/* 64 token MLA ≈ 4.5 MiB */
	.rounds		= 2000,
	.warmup		= 100,
	.kernel_iters	= 1000,
	.use_host_mem	= false,
};

struct mr_entry {
	uint64_t	addr;
	size_t		len;
	struct ibv_mr	*mr;
};

static struct {
	struct ibv_context	*verbs;
	struct ibv_pd		*pd;
	void			*gpu_buf;
	size_t			buf_len;
	int			dmabuf_fd;
	void			*host_buf;
	struct mr_entry		mr_cache[MAX_MR_CACHE];
	int			mr_count;
	pthread_mutex_t		mr_lock;
} g_ctx = {
	.dmabuf_fd	= -1,
	.mr_lock	= PTHREAD_MUTEX_INITIALIZER,
};

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;
static volatile int		g_io_done;
static volatile int		g_io_error;

/* 每轮的分段计时 */
struct sample {
	double	kernel_exit;	/* t1-t0 */
	double	submit;		/* t2-t1 */
	double	transfer;	/* t3-t2 */
	double	kernel_start;	/* t4-t3 */
	double	total;		/* t4-t0 */
};

static struct sample *g_samples;

/* 标定值:kernel launch+sync 固定开销,以及消费 kernel 的总耗时 */
static double g_launch_overhead;
static double g_consume_cost;

/* ================================================================== */
/* MR 管理(与 gds_nvmeof 相同的延迟注册策略)                          */
/* ================================================================== */

static struct mr_entry *
mr_lookup(uint64_t addr, size_t len)
{
	int i;

	for (i = 0; i < g_ctx.mr_count; i++) {
		struct mr_entry *e = &g_ctx.mr_cache[i];

		if (addr >= e->addr && addr + len <= e->addr + e->len) {
			return e;
		}
	}
	return NULL;
}

static struct ibv_pd *
hook_get_ibv_pd(const struct spdk_nvme_transport_id *trid,
		struct ibv_context *verbs)
{
	pthread_mutex_lock(&g_ctx.mr_lock);
	if (!g_ctx.pd) {
		g_ctx.verbs = verbs;
		g_ctx.pd = ibv_alloc_pd(verbs);
		if (g_ctx.pd) {
			printf("[HOOK] 网卡 %s, PD=%p\n",
			       ibv_get_device_name(verbs->device),
			       (void *)g_ctx.pd);
		}
	}
	pthread_mutex_unlock(&g_ctx.mr_lock);
	return g_ctx.pd;
}

static uint64_t
hook_get_rkey(struct ibv_pd *pd, void *buf, size_t size)
{
	struct mr_entry *e;
	uint64_t addr = (uint64_t)buf;
	uint64_t key = 0;
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING;

	pthread_mutex_lock(&g_ctx.mr_lock);

	e = mr_lookup(addr, size);
	if (!e && g_ctx.mr_count < MAX_MR_CACHE) {
		bool is_gpu = g_ctx.gpu_buf &&
			      addr >= (uint64_t)g_ctx.gpu_buf &&
			      addr + size <= (uint64_t)g_ctx.gpu_buf + g_ctx.buf_len;

		e = &g_ctx.mr_cache[g_ctx.mr_count];

		if (is_gpu) {
			e->mr = ibv_reg_dmabuf_mr(pd, 0, g_ctx.buf_len,
						  (uint64_t)g_ctx.gpu_buf,
						  g_ctx.dmabuf_fd, access);
			e->addr = (uint64_t)g_ctx.gpu_buf;
			e->len  = g_ctx.buf_len;
			if (e->mr) {
				printf("[MR] 显存 dmabuf MR: rkey=0x%x\n",
				       e->mr->rkey);
			}
		} else {
			e->mr = ibv_reg_mr(pd, buf, size, access);
			e->addr = addr;
			e->len  = size;
		}

		if (!e->mr) {
			fprintf(stderr, "MR 注册失败: %s\n", strerror(errno));
			e = NULL;
		} else {
			g_ctx.mr_count++;
		}
	}

	if (e) {
		key = e->mr->rkey;
	}

	pthread_mutex_unlock(&g_ctx.mr_lock);
	return key;
}

static void hook_put_rkey(uint64_t key) { }

static struct spdk_nvme_rdma_hooks g_hooks = {
	.get_ibv_pd	= hook_get_ibv_pd,
	.get_rkey	= hook_get_rkey,
	.put_rkey	= hook_put_rkey,
};

/* ================================================================== */

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	opts->num_io_queues = 2;
	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;

	g_ctrlr = ctrlr;
	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

		if (spdk_nvme_ns_is_active(ns) && !g_ns) {
			g_ns = ns;
			printf("[NVMe] ns %d: %lu MiB, sector %u B\n", nsid,
			       spdk_nvme_ns_get_size(ns) / (1024 * 1024),
			       spdk_nvme_ns_get_sector_size(ns));
		}
	}
}

static void
io_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_io_error = 1;
	}
	g_io_done = 1;
}

static inline double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ================================================================== */
/* 统计                                                                */
/* ================================================================== */

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void
report(const char *name, double *vals, uint32_t n)
{
	double sum = 0, mean, var = 0;
	uint32_t i;
	double *sorted;

	sorted = malloc(n * sizeof(double));
	memcpy(sorted, vals, n * sizeof(double));
	qsort(sorted, n, sizeof(double), cmp_double);

	for (i = 0; i < n; i++) {
		sum += vals[i];
	}
	mean = sum / n;
	for (i = 0; i < n; i++) {
		var += (vals[i] - mean) * (vals[i] - mean);
	}

	printf("  %-14s  %8.1f  %8.1f  %8.1f  %8.1f  %8.1f  %8.1f\n",
	       name, mean, sqrt(var / n),
	       sorted[0],
	       sorted[n / 2],
	       sorted[(uint32_t)(n * 0.99)],
	       sorted[n - 1]);

	free(sorted);
}

/* ================================================================== */

static int
run_baseline(void)
{
	uint32_t sector = spdk_nvme_ns_get_sector_size(g_ns);
	uint32_t lba_count = g_cfg.block_sz / sector;
	uint64_t max_lba = spdk_nvme_ns_get_num_sectors(g_ns);
	void *scratch = NULL, *result = NULL;
	void *payload;
	uint32_t r;
	double *col;
	bool use_kernel = (g_cfg.kernel_iters > 0) && !g_cfg.use_host_mem;

	if (g_cfg.block_sz % sector) {
		fprintf(stderr, "block_sz 必须是 sector(%u) 的整数倍\n", sector);
		return -1;
	}

	payload = g_cfg.use_host_mem ? g_ctx.host_buf : g_ctx.gpu_buf;

	if (use_kernel && gpu_kernels_init(&scratch, &result) != 0) {
		fprintf(stderr, "kernel 初始化失败\n");
		return -1;
	}

	g_samples = calloc(g_cfg.rounds, sizeof(*g_samples));

	printf("\n[基线] QD=1, bs=%u KiB, rounds=%u, kernel=%s\n",
	       g_cfg.block_sz / 1024, g_cfg.rounds,
	       use_kernel ? "开" : "关");

	/*
	 * 标定两个基准值。这一步很重要 —— 没有它,消费 kernel 那一格
	 * 里"真正在算"和"launch 开销"分不开,会把不可省的计算时间
	 * 误算成控制面开销。
	 */
	if (use_kernel) {
		double t0, t1, sum;
		int i;

		/* ① 空 kernel 的 launch+sync。前 20 次丢掉 —— 首次调用要
		 *    加载 module,几毫秒,会把平均值彻底带偏。 */
		for (i = 0; i < 20; i++) {
			gpu_launch_empty();
		}
		sum = 0;
		for (i = 0; i < 200; i++) {
			t0 = now_us();
			gpu_launch_empty();
			t1 = now_us();
			sum += t1 - t0;
		}
		g_launch_overhead = sum / 200;
		printf("[标定] 空 kernel launch+sync : %6.1f us\n",
		       g_launch_overhead);

		/* ② 消费 kernel 在数据已就位时的耗时。减去 ① 就是它
		 *    纯粹的计算时间,这部分 GPU-initiated 省不掉。 */
		for (i = 0; i < 10; i++) {
			gpu_launch_consume(payload, g_cfg.block_sz, NULL);
		}
		sum = 0;
		for (i = 0; i < 100; i++) {
			t0 = now_us();
			gpu_launch_consume(payload, g_cfg.block_sz, NULL);
			t1 = now_us();
			sum += t1 - t0;
		}
		g_consume_cost = sum / 100;
		printf("[标定] 消费 kernel 总耗时     : %6.1f us\n", g_consume_cost);
		printf("[标定]   其中计算部分         : %6.1f us  <- 不可省\n",
		       g_consume_cost - g_launch_overhead);
	}

	for (r = 0; r < g_cfg.warmup + g_cfg.rounds; r++) {
		double t0, t1, t2, t3, t4;
		uint64_t lba;
		struct sample *s;

		t0 = now_us();

		/* ① 计算 kernel,模拟"算到一半发现要数据" */
		if (use_kernel) {
			gpu_launch_compute(scratch, g_cfg.kernel_iters);
		}
		t1 = now_us();

		/* ② 提交读 */
		lba = ((uint64_t)r * lba_count) % (max_lba - lba_count);
		g_io_done = g_io_error = 0;
		if (spdk_nvme_ns_cmd_read(g_ns, g_qpair, payload, lba,
					  lba_count, io_cb, NULL, 0) != 0) {
			fprintf(stderr, "提交失败 @round %u\n", r);
			return -1;
		}
		t2 = now_us();

		/* ③ 等完成 */
		while (!g_io_done) {
			spdk_nvme_qpair_process_completions(g_qpair, 0);
		}
		if (g_io_error) {
			fprintf(stderr, "I/O 出错 @round %u\n", r);
			return -1;
		}
		t3 = now_us();

		/* ④ 消费 kernel */
		if (use_kernel) {
			gpu_launch_consume(payload, g_cfg.block_sz, NULL);
		}
		t4 = now_us();

		if (r < g_cfg.warmup) {
			continue;
		}

		s = &g_samples[r - g_cfg.warmup];
		s->kernel_exit  = t1 - t0;
		s->submit       = t2 - t1;
		s->transfer     = t3 - t2;
		s->kernel_start = t4 - t3;
		s->total        = t4 - t0;
	}

	/* ---- 报告 ---- */
	printf("\n===== 端到端延迟分解 (us) =====\n");
	printf("  %-14s  %8s  %8s  %8s  %8s  %8s  %8s\n",
	       "段", "mean", "stddev", "min", "p50", "p99", "max");

	col = malloc(g_cfg.rounds * sizeof(double));

#define REPORT_COL(field, label)					\
	do {								\
		uint32_t i;						\
		for (i = 0; i < g_cfg.rounds; i++)			\
			col[i] = g_samples[i].field;			\
		report(label, col, g_cfg.rounds);			\
	} while (0)

	if (use_kernel) {
		REPORT_COL(kernel_exit,  "计算kernel");
	}
	REPORT_COL(submit,       "提交");
	REPORT_COL(transfer,     "网络+传输");
	if (use_kernel) {
		REPORT_COL(kernel_start, "消费kernel");
	}
	REPORT_COL(total,        "总计");

#undef REPORT_COL

	/* 优化空间 —— 关键是把"不可省的计算"从控制面里剔出去 */
	{
		double k_exit = 0, submit = 0, xfer = 0, k_start = 0, total = 0;
		double compute, savable;
		uint32_t i;

		for (i = 0; i < g_cfg.rounds; i++) {
			k_exit  += g_samples[i].kernel_exit;
			submit  += g_samples[i].submit;
			xfer    += g_samples[i].transfer;
			k_start += g_samples[i].kernel_start;
			total   += g_samples[i].total;
		}
		k_exit  /= g_cfg.rounds;
		submit  /= g_cfg.rounds;
		xfer    /= g_cfg.rounds;
		k_start /= g_cfg.rounds;
		total   /= g_cfg.rounds;

		printf("\n===== 优化空间分析 =====\n");

		if (use_kernel) {
			/* 消费 kernel 里真正在算的部分,谁也省不掉 */
			compute = g_consume_cost - g_launch_overhead;
			if (compute < 0) {
				compute = 0;
			}

			/*
			 * GPU-initiated 能省的:
			 *   - 计算 kernel 退出后 CPU 才能感知(≈ 一次 launch+sync)
			 *   - I/O 提交(kernel 内直发,不用回 host)
			 *   - 消费 kernel 的启动(数据到了直接接着算,不用重启 kernel)
			 * 省不掉的:网络传输 + 消费 kernel 的实际计算
			 */
			savable = k_exit + submit + (k_start - compute);
			if (savable < 0) {
				savable = 0;
			}

			printf("  不可省:\n");
			printf("    网络传输          %6.1f us\n", xfer);
			printf("    消费kernel计算    %6.1f us\n", compute);
			printf("  可省(GPU-initiated):\n");
			printf("    计算kernel退出    %6.1f us\n", k_exit);
			printf("    I/O 提交          %6.1f us\n", submit);
			printf("    消费kernel启动    %6.1f us\n",
			       k_start - compute);
			printf("  ---------------------------------\n");
			printf("    总计              %6.1f us\n", total);
			printf("    可省              %6.1f us  (%.1f%%)\n",
			       savable, 100.0 * savable / total);
			printf("\n  注意:消费 kernel 那 %.1f us 的计算时间是真实工作量,\n",
			       compute);
			printf("       不是控制面开销,GPU-initiated 省不掉。\n");
		} else {
			printf("  提交              %6.1f us  <- 可省\n", submit);
			printf("  网络传输          %6.1f us  <- 物理下限\n", xfer);
			printf("  总计              %6.1f us\n", total);
			printf("\n  这是纯 I/O 路径。与开 kernel 的结果相减,\n");
			printf("  差值即 kernel launch/exit 的往返开销。\n");
		}
	}

	free(col);
	free(g_samples);
	if (use_kernel) {
		gpu_kernels_fini(scratch, result);
	}
	return 0;
}

/* ================================================================== */

static int
gpu_setup(size_t len)
{
	int fd = -1, rc;
	size_t reg_len;

	/*
	 * SPDK 的内存映射表以 2 MiB 为粒度管理,注册长度必须是整数倍,
	 * 否则 spdk_mem_register 返回 -EINVAL。
	 *
	 * KV block 尺寸(比如 64 token × 70272 B = 4.5 MiB)通常不是
	 * 2 MiB 的整数倍,所以分配时向上取整。多出来的部分不参与 I/O,
	 * 只是让注册能过。
	 */
	reg_len = (len + 0x1FFFFF) & ~(size_t)0x1FFFFF;

	GPU_CHECK(gpu_set_device(g_cfg.gpu_id));
	GPU_CHECK(gpu_mem_alloc(&g_ctx.gpu_buf, reg_len));
	g_ctx.buf_len = reg_len;

	printf("[GPU] dev=%d buf=%p len=%zu (I/O 用 %zu)\n",
	       g_cfg.gpu_id, g_ctx.gpu_buf, reg_len, len);

	GPU_CHECK(gpu_mem_get_dmabuf_fd(&fd, g_ctx.gpu_buf, reg_len));
	g_ctx.dmabuf_fd = fd;

	rc = spdk_mem_register(g_ctx.gpu_buf, reg_len);
	if (rc != 0) {
		fprintf(stderr,
			"spdk_mem_register 失败: %d\n"
			"  va=%p len=%zu\n"
			"  地址和长度都要按 2 MiB 对齐\n",
			rc, g_ctx.gpu_buf, reg_len);
		return -1;
	}
	return 0;
}

static void
usage(const char *p)
{
	printf("用法: %s -a <ip> -n <nqn> [选项]\n\n", p);
	printf("  -a <ip>     target 地址\n");
	printf("  -n <nqn>    subsystem NQN\n");
	printf("  -s <port>   端口,默认 4420\n");
	printf("  -g <id>     GPU 编号\n");
	printf("  -b <bytes>  单次读取大小,默认 4718592 (64 token MLA)\n");
	printf("  -r <n>      测量轮数,默认 2000\n");
	printf("  -k <iters>  计算 kernel 工作量,0 = 不跑 kernel\n");
	printf("  -H          对照组:主机内存\n");
	printf("\n典型用法:\n");
	printf("  # 完整基线\n");
	printf("  %s -a 172.16.3.3 -n nqn.xxx -g 4\n", p);
	printf("  # 纯 I/O,不含 kernel,两者相减即 kernel 开销\n");
	printf("  %s -a 172.16.3.3 -n nqn.xxx -g 4 -k 0\n", p);
	printf("  # 扫不同 KV block 粒度\n");
	printf("  for b in 1179648 2359296 4718592 9437184; do \\\n");
	printf("      %s -a ... -b $b; done\n", p);
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	int op, rc = 0;

	while ((op = getopt(argc, argv, "a:s:n:g:b:r:k:Hh")) != -1) {
		switch (op) {
		case 'a': snprintf(g_cfg.traddr, sizeof(g_cfg.traddr), "%s", optarg); break;
		case 's': snprintf(g_cfg.trsvcid, sizeof(g_cfg.trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_cfg.nqn, sizeof(g_cfg.nqn), "%s", optarg); break;
		case 'g': g_cfg.gpu_id = atoi(optarg); break;
		case 'b': g_cfg.block_sz = (uint32_t)atoll(optarg); break;
		case 'r': g_cfg.rounds = (uint32_t)atoi(optarg); break;
		case 'k': g_cfg.kernel_iters = atoi(optarg); break;
		case 'H': g_cfg.use_host_mem = true; break;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!g_cfg.traddr[0] || !g_cfg.nqn[0]) {
		usage(argv[0]);
		return 1;
	}

	spdk_env_opts_init(&opts);
	opts.opts_size = sizeof(opts);
	opts.name = "latency_baseline";
	opts.no_pci = true;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init 失败\n");
		return 1;
	}
	spdk_log_set_print_level(SPDK_LOG_NOTICE);

	if (g_cfg.use_host_mem) {
		g_ctx.host_buf = spdk_dma_zmalloc(g_cfg.block_sz, 4096, NULL);
		g_ctx.buf_len = g_cfg.block_sz;
		printf("[HOST] 对照组:主机内存\n");
	} else if (gpu_setup(g_cfg.block_sz) != 0) {
		return 1;
	}

	spdk_nvme_rdma_init_hooks(&g_hooks);

	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr,  sizeof(trid.traddr),  "%s", g_cfg.traddr);
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", g_cfg.trsvcid);
	snprintf(trid.subnqn,  sizeof(trid.subnqn),  "%s", g_cfg.nqn);

	if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) != 0 || !g_ns) {
		fprintf(stderr, "连接 target 失败\n");
		return 1;
	}

	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (!g_qpair) {
		fprintf(stderr, "alloc_io_qpair 失败\n");
		return 1;
	}

	if (run_baseline() != 0) {
		rc = 1;
	}

	if (g_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	}
	if (g_ctrlr) {
		spdk_nvme_detach(g_ctrlr);
	}
	if (g_ctx.dmabuf_fd >= 0) {
		close(g_ctx.dmabuf_fd);
	}
	if (g_ctx.gpu_buf) {
		spdk_mem_unregister(g_ctx.gpu_buf, g_ctx.buf_len);
		gpu_mem_free(g_ctx.gpu_buf);
	}

	return rc;
}
