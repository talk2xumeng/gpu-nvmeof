/*
 * gpu_initiated.c  --  GPU-initiated NVMe-oF 原型的 host 侧
 *
 * CPU 只做初始化:SPDK 建连、Fabric Connect、注册 MR、把 SQ/CQ/dbrec/UAR
 * 和 recv buffer 映射给 GPU。稳态收发全在 kernel 内 —— 构造命令胶囊、
 * 写 WQE、敲门铃、轮询 CQ、按 cid 分发完成,一条都不经过 CPU。
 * 并发模型见 docs/concurrency-design.md。
 *
 * 出错时 CPU 兜底:kernel 只报错误码,QP 恢复要 modify_qp,那是 host API。
 *
 * ============ 用法 ============
 *
 *   -a <ip> -n <nqn>     必填
 *   -g <id>              GPU 编号
 *   -r <n> -b <bytes>    I/O 总数、每条字节数
 *   -Q <qd>              同时在飞的 I/O 数(submitter 线程数,默认 1)
 *   -W                   发 WRITE(默认 READ)
 *   -H                   命令胶囊放显存(默认主存),分配 flag 见 -F
 *   -F <flag>            胶囊区 mcExtMallocWithFlags 的 flag(默认 1)
 *   -L <lba>             起始 LBA(默认 0),第 i 轮用 L + i*nlb
 *   -S <seed>            pattern seed,0-255(默认 1)。每次运行都该换
 *   -K                   旧式对照:单块、单字节 pattern、独立 kernel 填充
 *   -P                   把 [L, L+r) 涂成 0xCC 然后退出
 *   -V                   回读 [L, L+r) 逐块校验然后退出(-S 要和写入时一致)
 *
 * 多块校验:第 i 轮读写显存第 i 块、LBA L+i*nlb,每块内容由
 * (seed, LBA) 决定(blk_pattern.h),漏块/串块/残留都能区分。
 *
 *   读:进程内先用 CPU 路径把 pattern 写到盘上,kernel 读回显存,
 *       结束后 mcMemcpy 拷回逐块比对 —— 不碰 qpair,可信。
 *   写:进程内先把盘涂成 0xCC,kernel 写;校验必须另起进程:
 *
 *   ./gpu_initiated ... -W -S 37     kernel 写
 *   ./gpu_initiated ... -V -S 37     逐块回读
 *
 * 不能在本进程里回读 —— 见 docs/debug-notes.md 坑 3。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>
#include <endian.h>

#include "gpu_io_ctx.h"
#include "blk_pattern.h"

extern int gpu_io_run(struct gpu_io_ctx *, unsigned long long,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned int *,
		      struct gio_shared *);
extern int gpu_alloc_exportable(void **, int *, size_t);
extern int gpu_alloc_exportable_ex(void **, int *, size_t, unsigned int);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_unmap_host(void *);
extern int gpu_peek(void *, unsigned char *, unsigned int);
extern int gpu_set_dev(int);
extern int gpu_clock_khz(int, int *);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_copy_to_dev(void *, const void *, size_t);
extern int gpu_memset_dev(void *, int, size_t);
extern int gpu_sync(void);
extern int gpu_fill_kernel(void *, int, size_t);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define CAP_AREA_LEN	(2 * 1024 * 1024)
#define RPAT		BLK_RPAT	/* 读:预填,被 target 覆盖才算落地 */
#define WPAT		BLK_WPAT	/* -K 旧式写 pattern */
#define CPAT		BLK_CPAT	/* 盘上涂色 */
#define HUGE_2M		(2UL * 1024 * 1024)

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
static uint32_t	g_qd = 1;
static uint32_t	g_io_bytes = 4096;
static int	g_write, g_cap_in_hbm, g_paint, g_verify;
static int	g_fill_by_kernel;	/* -K: 源数据由 kernel 写,不用 mcMemset */
static uint64_t	g_start_lba;
static uint8_t	g_seed = 1;

/*
 * -H 时胶囊区的分配 flag。默认 1 = mcDeviceMallocFinegrained。
 *
 * 普通 mcMalloc 的显存,GPU kernel 普通 store 写完之后网卡 DMA read
 * 取到的是旧值 —— 写停在 L2 里,而 MACA 没有 kernel 内可用的 flush。
 * 实测:flag 1(Finegrained)和 4(MapPcieDefault,uncache+映射 PCIe)
 * 都能让网卡读到最新值;3(WriteCoherence)和 5 无效;6 分配失败。
 * 选 1 因为它比完全 uncache 温和。-F 可覆盖。
 */
#define CAP_FLAGS_DEFAULT	1
static unsigned	g_cap_flags = CAP_FLAGS_DEFAULT;
static int	g_cap_fd = -1;

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
	printf("用法: %s -a <ip> -n <nqn> [-g gpu] [-r n_io] [-b bytes] [-Q qd]\n"
	       "      [-L start_lba] [-S seed] [-W 发写]\n"
	       "      [-H 胶囊放显存] [-F cap_flag]\n"
	       "      [-P 涂色] [-V 逐块回读] [-K 旧式单块对照]\n", p);
}

/*
 * 核对 host 注册映射:GPU 从 dev 读 len 字节,和主存 host 逐字节比。
 * 返回不一致的字节数(-1 = kernel 失败),first 带回首个不一致偏移。
 */
static long
map_selftest(void *host, void *dev, size_t len, size_t *first)
{
	unsigned char *g = malloc(len);
	long bad = 0;
	size_t k;

	*first = len;
	if (!g || gpu_peek(dev, g, (unsigned)len) != 0) {
		free(g);
		return -1;
	}
	for (k = 0; k < len; k++) {
		if (g[k] != ((unsigned char *)host)[k]) {
			if (bad == 0) {
				*first = k;
			}
			bad++;
		}
	}
	free(g);
	return bad;
}

/*
 * 把 SPDK 的 recv buffer 映射给 GPU,返回设备指针;失败返回 NULL。
 *
 * 响应一般内联在 CQE 里(坑 11),recv buffer 只是兜底 —— 失败不致命,
 * 只是碰到非内联响应时 kernel 会报 -4。
 *
 * recv buffer 在 DPDK 大页上。若 mcHostRegister 按大页边界对齐了注册
 * 范围,返回的设备指针可能不对应传入的起点(同坑 6 一类),所以映射后
 * 做 GPU/主存逐字节自检,不一致就按 2 MiB 对齐重注册再测。
 */
static void *
map_recv_buffers(uint64_t base, uint64_t end, uint64_t lo, uint64_t hi)
{
	uint64_t hb = lo & ~(HUGE_2M - 1);
	uint64_t he = (hi + HUGE_2M - 1) & ~(HUGE_2M - 1);
	void *d, *d2;
	size_t first;
	long bad;

	if (gpu_map_host((void *)base, end - base,
			 MC_HOST_REGISTER_MAPPED, &d) != 0) {
		printf("[映射] recv buffer 映射失败,只支持 CQE 内联响应\n");
		return NULL;
	}
	bad = map_selftest((void *)base, d, end - base, &first);
	printf("[映射] recv buffer 0x%" PRIx64 " +%" PRIu64 " -> %p,"
	       "自检不一致 %ld 字节\n", base, end - base, d, bad);
	if (bad == 0) {
		return d;
	}

	gpu_unmap_host((void *)base);
	if (gpu_map_host((void *)hb, he - hb, MC_HOST_REGISTER_MAPPED,
			 &d2) != 0) {
		printf("[映射] 按 2 MiB 对齐重注册失败,只支持 CQE 内联响应\n");
		return NULL;
	}
	d = (char *)d2 + (base - hb);
	bad = map_selftest((void *)base, d, end - base, &first);
	printf("[映射] 按 2 MiB 对齐重注册 0x%" PRIx64 " -> %p,"
	       "自检不一致 %ld 字节\n", hb, d2, bad);
	if (bad != 0) {
		printf("[映射] recv buffer 的 GPU 视图和主存对不上,"
		       "只支持 CQE 内联响应\n");
		return NULL;
	}
	return d;
}

/*
 * 逐块比对并打印。buf 是 n 块连续数据,第 i 块期望 (seed, lba0 + i*nlb)。
 * 返回不一致的块数。
 *
 * 块图每字符一块:
 *   .  一致          X  串块(本轮 pattern,别的 LBA)
 *   R  0xBB 没落地   O  上一轮残留(seed 不同)
 *   C  0xCC 没写到   S  0xEE 显存旧值(坑 7)
 *   W  0x5A 旧式     Z  全零
 *   F  其他单字节    ?  部分写入/乱码
 */
static unsigned
report_blocks(const char *tag, const uint8_t *buf, uint32_t n, uint32_t bs,
	      uint32_t nlb, uint64_t lba0, uint8_t seed)
{
	unsigned cnt[128] = { 0 };
	unsigned bad = 0, shown = 0, i;
	struct blk_result r;

	printf("\n[%s] %u 块 × %u 字节, LBA %" PRIu64 " 起, seed %u\n  ",
	       tag, n, bs, lba0, seed);
	for (i = 0; i < n; i++) {
		blk_check(buf + (size_t)i * bs, bs, seed,
			  lba0 + (uint64_t)i * nlb, &r);
		putchar(blk_map_char(&r));
		cnt[(unsigned char)blk_map_char(&r)]++;
		if (r.v != BLK_OK) {
			bad++;
		}
		if (i % 64 == 63 && i + 1 < n) {
			printf("\n  ");
		}
	}
	printf("\n  ");
	for (i = 0; i < 128; i++) {
		if (cnt[i]) {
			printf("%c=%u ", (char)i, cnt[i]);
		}
	}
	printf("\n");

	for (i = 0; i < n && shown < 8; i++) {
		uint64_t lba = lba0 + (uint64_t)i * nlb;

		if (blk_check(buf + (size_t)i * bs, bs, seed, lba, &r) == BLK_OK) {
			continue;
		}
		shown++;
		printf("  块 %u (LBA %" PRIu64 "): ", i, lba);
		switch (r.v) {
		case BLK_FILL:
			printf("整块 0x%02x\n", r.fill);
			break;
		case BLK_CROSS:
			printf("串块,内容属于 LBA %" PRIu64 " (块 %" PRId64 ")\n",
			       r.lba, (int64_t)(r.lba - lba0) / (int64_t)nlb);
			break;
		case BLK_OLDSEED:
			printf("残留,seed %u LBA %" PRIu64 "\n", r.seed, r.lba);
			break;
		default:
			printf("首个不一致在偏移 %zu (%02x",
			       r.bad_off, buf[(size_t)i * bs + r.bad_off]);
			printf(" 期望 %02x)\n", (unsigned)
			       ((blk_word(seed, lba, (uint32_t)(r.bad_off / 8)) >>
				 ((r.bad_off % 8) * 8)) & 0xff));
			break;
		}
	}
	if (bad > shown) {
		printf("  ...另有 %u 块\n", bad - shown);
	}
	printf("  %s\n", bad ? "*** FAIL ***" : "*** PASS:逐块一致 ***");
	return bad;
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
	unsigned long long *cycles, *c_cap, *c_sub, *c_wait, *c_spin, *c_lock;
	unsigned int *c_stat;
	struct gio_shared sh;
	struct spdk_nvme_io_qpair_opts qopts;
	void *d_rsp = NULL;
	uint32_t rq_posted = 0;
	uint64_t rsp_lo = 0, rsp_gap = 0;
	double *us;
	uint32_t sector, nlb, nblk;
	int err = 0, clk_khz = 0, op;
	unsigned int done = 0, i;
	void *d_sq, *d_rq, *d_q, *d_dbrec, *d_bf, *d_cq, *d_cqdb;

	memset(&dvq, 0, sizeof(dvq));
	memset(&dvc, 0, sizeof(dvc));
	memset(&ctx, 0, sizeof(ctx));

	while ((op = getopt(argc, argv, "a:s:n:g:r:b:Q:WHPVKF:L:S:")) != -1) {
		switch (op) {
		case 'a': snprintf(g_traddr, sizeof(g_traddr), "%s", optarg); break;
		case 's': snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_nqn, sizeof(g_nqn), "%s", optarg); break;
		case 'g': g_gpu_id = atoi(optarg); break;
		case 'r': g_rounds = (unsigned)atoi(optarg); break;
		case 'b': g_io_bytes = (unsigned)atoi(optarg); break;
		case 'Q': g_qd = (unsigned)atoi(optarg); break;
		case 'W': g_write = 1; break;
		case 'H': g_cap_in_hbm = 1; break;
		case 'P': g_paint = 1; break;
		case 'V': g_verify = 1; break;
		case 'K': g_fill_by_kernel = 1; break;
		case 'F': g_cap_flags = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'L': g_start_lba = strtoull(optarg, NULL, 0); break;
		case 'S': g_seed = (uint8_t)strtoul(optarg, NULL, 0); break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (!g_traddr[0] || !g_nqn[0]) {
		usage(argv[0]);
		return 1;
	}
	if (g_io_bytes % 8 || g_rounds == 0) {
		fprintf(stderr, "-b 必须是 8 的倍数,-r 必须 > 0\n");
		return 1;
	}
	if (g_qd == 0 || g_qd > 127) {
		fprintf(stderr, "-Q 取 1..127\n");
		return 1;
	}
	if (g_qd > g_rounds) {
		g_qd = g_rounds;
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

	/*
	 * ---- 显存:一块分配,-H 时把胶囊切在后半段 ----
	 * 多块模式下每轮一块,数据区 = 轮数 × 块大小,按 2 MiB 取整。
	 * -P/-V 不碰显存,只留最小的一块给 hook 用。
	 */
	nblk = (g_fill_by_kernel || g_paint || g_verify) ? 1 : g_rounds;
	g_data_len = (size_t)nblk * g_io_bytes;
	g_data_len = (g_data_len + HUGE_2M - 1) / HUGE_2M * HUGE_2M;
	g_total_len = g_data_len +
		      ((g_cap_in_hbm && !g_cap_flags) ? CAP_AREA_LEN : 0);
	if (gpu_alloc_exportable(&g_base_gpu, &g_base_fd, g_total_len) != 0) {
		fprintf(stderr, "显存分配失败\n");
		return 1;
	}
	g_data_gpu = g_base_gpu;
	spdk_mem_register(g_base_gpu, g_total_len);

	if (g_cap_in_hbm && g_cap_flags) {
		/* 胶囊区单独分配,带 uncache / 一致性 flag。
		 * 留意 MACA 的 dma-buf 导出 offset 0 是底层分配的基址 ——
		 * 两次普通 mcMalloc 落同一 slab 会让第二个 MR 错位。
		 * ExtMalloc 是另一套分配器,但 lkey 得和数据区不同才算真分开,
		 * 启动时会打出来核对。 */
		if (gpu_alloc_exportable_ex(&g_cap_hbm, &g_cap_fd,
					    CAP_AREA_LEN, g_cap_flags) != 0) {
			fprintf(stderr, "胶囊区 ExtMalloc(flag=0x%x) 失败\n",
				g_cap_flags);
			return 1;
		}
		gpu_memset_dev(g_cap_hbm, 0, CAP_AREA_LEN);
		gpu_sync();
		printf("[GPU] data=%p (%zu)  capsule(HBM flag=0x%x)=%p\n",
		       g_data_gpu, g_data_len, g_cap_flags, g_cap_hbm);
	} else if (g_cap_in_hbm) {
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

	if (g_io_bytes % sector) {
		fprintf(stderr, "-b %u 不是扇区 %u 的整数倍\n", g_io_bytes, sector);
		return 1;
	}

	/*
	 * kernel 一次发一条 keyed SGL,不会像 SPDK 主机驱动那样按最大传输
	 * 长度自动拆分。超过 target 的 max_io_size,命令会被拒,而 kernel
	 * 目前不看响应里的 NVMe 状态 —— 表现为"完成 N/N"但数据一块没动。
	 * 见 debug-notes 坑 9。-P/-V 走 SPDK 路径,会自动拆,不受限。
	 */
	if (!g_paint && !g_verify) {
		uint32_t mx = spdk_nvme_ctrlr_get_max_xfer_size(g_ctrlr);

		if (g_io_bytes > mx) {
			fprintf(stderr,
				"-b %u 超过控制器最大传输长度 %u。\n"
				"target 需要更大的 max_io_size,例如:\n"
				"  rpc.py nvmf_create_transport -t RDMA -c 512 "
				"-u 131072 -i 1048576\n", g_io_bytes, mx);
			return 1;
		}
	}

	/* ---- -P / -V:纯 CPU 路径,做完就退。范围 [L, L+r) ---- */
	if (g_paint || g_verify) {
		void *b = spdk_dma_zmalloc(g_io_bytes, 4096, NULL);
		uint8_t *all = NULL;
		unsigned nfail = 0, bad = 0;

		if (!b) {
			return 1;
		}
		if (g_verify) {
			all = malloc((size_t)g_rounds * g_io_bytes);
			if (!all) {
				return 1;
			}
		}
		memset(b, CPAT, g_io_bytes);
		for (i = 0; i < g_rounds; i++) {
			uint64_t lba = g_start_lba + (uint64_t)i * nlb;

			if (g_paint) {
				nfail += cpu_io(1, b, lba, nlb) != 0;
				continue;
			}
			if (cpu_io(0, b, lba, nlb) != 0) {
				nfail++;
				memset(b, 0, g_io_bytes);
			}
			memcpy(all + (size_t)i * g_io_bytes, b, g_io_bytes);
		}
		if (g_paint) {
			printf("[涂色] LBA %" PRIu64 " 起 %u 块填 0x%02x,失败 %u\n",
			       g_start_lba, g_rounds, CPAT, nfail);
		} else {
			if (nfail) {
				printf("[回读] %u 块 I/O 失败(按全零计)\n", nfail);
			}
			bad = report_blocks("回读", all, g_rounds, g_io_bytes,
					    nlb, g_start_lba, g_seed);
			free(all);
		}
		spdk_dma_free(b);
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
		spdk_nvme_detach(g_ctrlr);
		return (nfail || bad) ? 2 : 0;
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
	if (g_cap_in_hbm && g_cap_flags) {
		g_cap_mr = ibv_reg_dmabuf_mr(g_pd, 0, CAP_AREA_LEN,
					     (uint64_t)g_cap_hbm, g_cap_fd,
					     IBV_ACCESS_LOCAL_WRITE |
					     IBV_ACCESS_REMOTE_READ |
					     IBV_ACCESS_REMOTE_WRITE);
		if (!g_cap_mr) {
			fprintf(stderr, "胶囊注册失败: %s\n", strerror(errno));
			return 1;
		}
		g_cap_dev = g_cap_hbm;
		printf("[MR] 胶囊 lkey=0x%x (独立 MR,数据区 rkey=0x%x)%s\n",
		       g_cap_mr->lkey, g_data_mr->rkey,
		       g_cap_mr->lkey == g_data_mr->rkey ?
		       "  *** 两者相同,可能落在同一 slab ***" : "");
	} else if (g_cap_in_hbm) {
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

	/*
	 * mlx5 的 RQ 和 SQ 在同一块 buffer 里(RQ 在前),SQ 起点往往不对齐页,
	 * 两者可能共用一页。整段一起注册,再按偏移算出各自的设备指针。
	 */
	{
		char *rq_lo = (char *)dvq.rq.buf;
		char *rq_hi = rq_lo + (size_t)dvq.rq.wqe_cnt * dvq.rq.stride;
		char *sq_lo = (char *)dvq.sq.buf;
		char *sq_hi = sq_lo + (size_t)dvq.sq.wqe_cnt * dvq.sq.stride;
		char *lo = rq_lo < sq_lo ? rq_lo : sq_lo;
		char *hi = rq_hi > sq_hi ? rq_hi : sq_hi;

		if ((size_t)(hi - lo) > (size_t)((rq_hi - rq_lo) +
						 (sq_hi - sq_lo)) + 8192) {
			fprintf(stderr, "RQ %p 与 SQ %p 不相邻,和预期的 mlx5 "
				"布局不同\n", rq_lo, sq_lo);
			return 1;
		}
		if (gpu_map_host(lo, (size_t)(hi - lo),
				 MC_HOST_REGISTER_MAPPED, &d_q) != 0) {
			fprintf(stderr, "映射 RQ+SQ 给 GPU 失败\n");
			return 1;
		}
		d_sq = (char *)d_q + (sq_lo - lo);
		d_rq = (char *)d_q + (rq_lo - lo);
	}
	if (gpu_map_host((void *)dvq.dbrec, 64,
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
	printf("[映射] RQ/SQ/CQ/dbrec/UAR 均已交给 GPU\n");

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
	ctx.data_step    = g_fill_by_kernel ? 0 : g_io_bytes;

	/*
	 * ---- RQ 与 recv buffer ----
	 *
	 * 响应胶囊落在 SPDK 预先 post 的 recv buffer 里(rsps[i].cpl,连续
	 * 数组,spdk_zmalloc 大页)。poller 要从里面读 cid 和 NVMe status,
	 * 所以这段也得映射给 GPU。地址从 RQ 槽里现读,不依赖 SPDK 私有结构。
	 *
	 * rdma-core 把 RQ 生产者计数写在 dbrec[0](大端,低 16 位)。SPDK
	 * 每消费一个就立刻补一个,所以接管时已 post 的数量恒为 queue size,
	 * 最老那个的计数 = head - posted。RESP CQE 的 wqe_counter 应该从这里
	 * 开始逐个递增 —— kernel 会核对(wc_mismatch)。
	 */
	{
		volatile uint32_t *qdb = (volatile uint32_t *)dvq.dbrec;
		uint32_t head = be32toh(qdb[0]) & 0xffff;
		uint32_t mask = dvq.rq.wqe_cnt - 1;
		uint64_t amin = ~0ULL, amax = 0, gap = ~0ULL, prev = 0;
		uint32_t lkey0 = 0, bad = 0, k;
		uint64_t base, end;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts,
							  sizeof(qopts));
		rq_posted = qopts.io_queue_size;
		if (rq_posted > dvq.rq.wqe_cnt) {
			rq_posted = dvq.rq.wqe_cnt;
		}
		for (k = 0; k < rq_posted; k++) {
			uint8_t *seg = (uint8_t *)dvq.rq.buf +
				       (size_t)((head - rq_posted + k) & mask) *
				       dvq.rq.stride;
			uint32_t len, lkey;
			uint64_t addr;

			memcpy(&len, seg, 4);
			memcpy(&lkey, seg + 4, 4);
			memcpy(&addr, seg + 8, 8);
			len = be32toh(len);
			lkey = be32toh(lkey);
			addr = be64toh(addr);
			if (k == 0) {
				lkey0 = lkey;
			}
			if (len != 16 || lkey != lkey0) {
				bad++;
			}
			if (addr < amin) amin = addr;
			if (addr > amax) amax = addr;
			if (k && addr > prev && addr - prev < gap) {
				gap = addr - prev;
			}
			prev = addr;
		}
		printf("[RQ] wqe_cnt=%u stride=%u head(准备前)=%u posted=%u "
		       "(io_queue_size=%u) lkey=0x%x 异常槽=%u\n",
		       dvq.rq.wqe_cnt, dvq.rq.stride, head, rq_posted,
		       qopts.io_queue_size, lkey0, bad);
		printf("[RQ] recv buffer 0x%" PRIx64 " .. 0x%" PRIx64
		       " (跨 %" PRIu64 " 字节, 最小间隔 %" PRIu64 ")\n",
		       amin, amax + 16, amax + 16 - amin,
		       gap == ~0ULL ? 0 : gap);
		if (bad) {
			fprintf(stderr, "RQ 槽内容和预期不符(len 应为 16、lkey 一致),"
				"不能继续\n");
			return 1;
		}

		rsp_lo = amin;
		rsp_gap = (gap == ~0ULL) ? 0 : gap;
		base = amin & ~4095ULL;
		end = (amax + 16 + 4095) & ~4095ULL;
		d_rsp = map_recv_buffers(base, end, amin, amax + 16);

		ctx.rq_buf        = (volatile uint8_t *)d_rq;
		ctx.rq_wqe_cnt    = dvq.rq.wqe_cnt;
		ctx.rq_stride     = dvq.rq.stride;
		ctx.rsp_host_base = base;
		ctx.rsp_len       = d_rsp ? end - base : 0;
		ctx.rsp_dev       = (volatile uint8_t *)d_rsp;
	}

	/*
	 * C1 不补 recv buffer:每条 I/O 的响应消耗一个,总数不能超过已 post 的。
	 * 超出的那条响应无处安放,表现是超时。
	 */
	if (rq_posted < 2) {
		fprintf(stderr, "已 post 的 recv 数 %u 异常\n", rq_posted);
		return 1;
	}
	if (g_qd >= rq_posted) {
		g_qd = rq_posted - 1;
		printf("[QD] 超过 queue size,降为 %u\n", g_qd);
	}
	ctx.qd = g_qd;
	ctx.njobs = g_rounds;

	/* ---- 准备源数据 / 盘上底色 ---- */
	if (g_fill_by_kernel) {
		/*
		 * 旧式对照(-K):单块,pattern 由独立 kernel 的普通 store 写入,
		 * 之后 mcDeviceSynchronize。配 -W 用 —— 取到 pattern 说明
		 * "独立 kernel + 设备同步"这一形态对网卡可见。
		 */
		gpu_memset_dev(g_data_gpu, 0, g_io_bytes);
		gpu_sync();		/* 先清零,底色要和 pattern 不同 */
		gpu_fill_kernel(g_data_gpu, g_write ? WPAT : RPAT, g_io_bytes);
		gpu_sync();		/* 等 kernel 跑完,但不做额外 flush */
		printf("[填充] 源数据由独立 kernel 写入 (普通 store)\n");
	} else if (!g_write) {
		/* 读:显存预填 0xBB,盘上写本轮 pattern */
		void *b = spdk_dma_zmalloc(g_io_bytes, 4096, NULL);
		unsigned nfail = 0;

		gpu_memset_dev(g_data_gpu, RPAT, (size_t)nblk * g_io_bytes);
		gpu_sync();	/* 必须:mcMemset 对 host 是异步的 */
		if (!b) {
			return 1;
		}
		for (i = 0; i < nblk; i++) {
			uint64_t lba = g_start_lba + (uint64_t)i * nlb;

			blk_fill(b, g_io_bytes, g_seed, lba);
			nfail += cpu_io(1, b, lba, nlb) != 0;
		}
		spdk_dma_free(b);
		printf("[准备] 显存 %u 块预填 0x%02x;盘上 %u 块写入 pattern"
		       " (seed %u),失败 %u\n", nblk, RPAT, nblk, g_seed, nfail);
		if (nfail) {
			return 1;
		}
	} else {
		/* 写:host 生成 pattern 拷进显存 */
		uint8_t *h = malloc((size_t)nblk * g_io_bytes);

		if (!h) {
			return 1;
		}
		for (i = 0; i < nblk; i++) {
			blk_fill(h + (size_t)i * g_io_bytes, g_io_bytes, g_seed,
				 g_start_lba + (uint64_t)i * nlb);
		}
		if (gpu_copy_to_dev(g_data_gpu, h,
				    (size_t)nblk * g_io_bytes) != 0) {
			fprintf(stderr, "HtoD 失败\n");
			return 1;
		}
		gpu_sync();
		free(h);
		printf("[准备] 显存 %u 块写入 pattern (seed %u, HtoD)\n",
		       nblk, g_seed);
	}

	if (g_write) {
		/* 目标 LBA 全部涂成 0xCC,-V 时就能区分"没写"和"写了别的" */
		void *b = spdk_dma_zmalloc(g_io_bytes, 4096, NULL);
		unsigned nfail = 0;

		if (!b) {
			return 1;
		}
		memset(b, CPAT, g_io_bytes);
		for (i = 0; i < nblk; i++) {
			nfail += cpu_io(1, b, g_start_lba + (uint64_t)i * nlb,
					nlb) != 0;
		}
		spdk_dma_free(b);
		printf("[涂色] LBA %" PRIu64 " 起 %u 块已填 0x%02x,失败 %u\n",
		       g_start_lba, nblk, CPAT, nfail);
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

		uint32_t rq_head = be32toh(qdb[0]) & 0xffff;

		ctx.sq_pi = (uint16_t)sq_pi;
		ctx.cq_ci = cq_ci;
		ctx.cq_phase = (uint8_t)((cq_ci / dvc.cqe_cnt) & 1);
		/*
		 * RQ 的快照也必须在这里拍:上面的预写/涂色会推进 RQ head
		 * (SPDK 每消费一个 recv 就补一个)。
		 */
		/*
		 * 已消费的 recv 数 = 已收到的响应数 = 已发的命令数 = sq_pi
		 * (接管时所有 CPU 侧命令都已完成)。不能用 head - io_queue_size:
		 * SPDK 补 recv 是批量延迟 flush 的,最后消费的那个常常还没
		 * post 回去,实际挂着的比 queue size 少一个。
		 */
		ctx.rq_tail = sq_pi & 0xffff;
		rq_posted = (rq_head - sq_pi) & 0xffff;
		printf("[接管] SQ pi=%u  CQ ci=%u phase=%u  RQ head=%u tail=%u "
		       "(挂着 %u 个 recv)\n", sq_pi, cq_ci, ctx.cq_phase,
		       rq_head, ctx.rq_tail, rq_posted);
		if (g_rounds > rq_posted) {
			fprintf(stderr, "-r %u 超过挂着的 recv 数 %u(C1 尚不补充)\n",
				g_rounds, rq_posted);
			return 1;
		}
	}

	/* ---- 跑 ---- */
	cycles = calloc(g_rounds, sizeof(*cycles));
	c_cap  = calloc(g_rounds, sizeof(*c_cap));
	c_sub  = calloc(g_rounds, sizeof(*c_sub));
	c_wait = calloc(g_rounds, sizeof(*c_wait));
	c_spin = calloc(g_rounds, sizeof(*c_spin));
	c_lock = calloc(g_rounds, sizeof(*c_lock));
	c_stat = calloc(g_rounds, sizeof(*c_stat));
	us = calloc(g_rounds, sizeof(*us));
	printf("\n[运行] %s %u 条, 每条 %u 字节, QD %u, %s\n",
	       g_write ? "WRITE" : "READ", g_rounds, g_io_bytes, g_qd,
	       ctx.data_step ? "每条一块" : "单块复用");

	if (gpu_io_run(&ctx, g_start_lba, c_cap, c_sub, c_wait, c_spin,
		       c_lock, c_stat, &sh) != 0) {
		fprintf(stderr, "kernel 执行失败\n");
		return 1;
	}

	/*
	 * done = 等到了响应的条数(不论 NVMe status);ok = status 为 0 的。
	 * 这是坑 9 的根治:target 拒掉的命令不再算成功。
	 */
	{
		unsigned ok = 0;

		for (i = 0; i < g_rounds; i++) {
			if (c_stat[i] != 0xffffffffu) {
				cycles[done] = c_cap[i] + c_sub[i] + c_wait[i];
				done++;
				ok += (c_stat[i] >> 1) == 0;
			}
		}
		err = sh.err;
		printf("[完成] 响应 %u / %u, NVMe 成功 %u, NVMe 出错 %u",
		       done, g_rounds, ok, sh.nvme_err);
		if (sh.nvme_err) {
			printf(" (首个 status=0x%04x: SCT=%u SC=0x%02x%s)",
			       sh.first_status, (sh.first_status >> 9) & 7,
			       (sh.first_status >> 1) & 0xff,
			       ((sh.first_status >> 1) & 0xff) == 0x02 &&
			       ((sh.first_status >> 9) & 7) == 0 ?
			       " Invalid Field —— 常见于超过 max_io_size" : "");
		}
		printf("\n[poller] CQE: SEND 完成 %u, 响应 %u;cid 异常 %u",
		       sh.cqe_req, sh.cqe_resp, sh.cid_bad);
		if (sh.cid_bad) {
			printf(" (首个 0x%04x)", sh.first_bad_cid);
		}
		printf(";wqe_counter 不连续 %u;响应内联在 CQE %u/%u"
		       ";轮询 %llu 圈, 命中 %llu\n",
		       sh.wc_mismatch, sh.resp_inline, sh.cqe_resp,
		       (unsigned long long)sh.poll_spins,
		       (unsigned long long)sh.poll_hits);
		if (sh.err_cqe) {
			printf("[poller] 错误 CQE syndrome=0x%02x vendor=0x%02x\n",
			       sh.err_cqe & 0xff, (sh.err_cqe >> 8) & 0xff);
		}
		if (ok != g_rounds && !err) {
			err = -5;
		}
	}

	/*
	 * 响应落在哪块 recv buffer:host 直接扫 rsp 数组找 kernel 的 cid,
	 * 再从 GPU 视角读同一处核对。只在出了 cid 异常时打印。
	 */
	if (sh.cid_bad && rsp_gap) {
		uint64_t hb = rsp_lo & ~(HUGE_2M - 1);
		uint64_t a2;
		uint32_t k, mask = dvq.rq.wqe_cnt - 1;
		unsigned shown = 0;
		unsigned *seen = calloc(512, sizeof(*seen));

		/* ① 整个 2 MiB 大页里找 kernel 的响应:cid 高位 0x8000、sqid=1 */
		printf("[诊断] 大页 0x%" PRIx64 " 内 cid&0x8000 且 sqid=1 的记录:\n",
		       hb);
		for (a2 = hb; a2 + 16 <= hb + HUGE_2M; a2 += 8) {
			uint8_t *c = (uint8_t *)a2;
			uint16_t sqid = (uint16_t)(c[10] | (c[11] << 8));
			uint16_t cid = (uint16_t)(c[12] | (c[13] << 8));

			if (sqid == 1 && (cid & 0xff00) == 0x8000 && shown < 6) {
				printf("  @0x%" PRIx64 " (相对 rsp 起点 %+" PRId64
				       ") cid=0x%04x status=0x%04x sqhd=%u\n",
				       a2, (int64_t)(a2 - rsp_lo), cid,
				       (unsigned)(c[14] | (c[15] << 8)),
				       (unsigned)(c[8] | (c[9] << 8)));
				shown++;
			}
		}
		if (!shown) {
			printf("  (没有)\n");
		}

		/* ② 128 个槽 -> rsp 下标(相对最小地址 / 间隔) */
		printf("[诊断] RQ 槽 -> rsp 下标 (tail=%u, 槽 %u 起):\n  ",
		       ctx.rq_tail, ctx.rq_tail & mask);
		for (k = 0; k < dvq.rq.wqe_cnt; k++) {
			uint8_t *seg = (uint8_t *)dvq.rq.buf +
				       (size_t)k * dvq.rq.stride;
			uint64_t addr;
			int64_t idx;

			memcpy(&addr, seg + 8, 8);
			addr = be64toh(addr);
			idx = (int64_t)(addr - rsp_lo) / (int64_t)rsp_gap;
			printf("%3" PRId64 "%s", idx,
			       k == (ctx.rq_tail & mask) ? "*" : " ");
			if (idx >= 0 && idx < 512) {
				seen[idx]++;
			}
			if (k % 16 == 15) {
				printf("\n  ");
			}
		}
		printf("\n  重复的下标:");
		for (k = 0; k < 512; k++) {
			if (seen[k] > 1) {
				printf(" %u(x%u)", k, seen[k]);
			}
		}
		printf("  缺的下标(0..%u):", dvq.rq.wqe_cnt - 1);
		for (k = 0; k < dvq.rq.wqe_cnt; k++) {
			if (!seen[k]) {
				printf(" %u", k);
			}
		}
		printf("\n");
		free(seen);

		/* ③ 响应 CQE 全量(cqe64:byte_cnt @44,大端) */
		for (k = 0; k < 2; k++) {
			uint32_t idx = (ctx.cq_ci + k) & (dvc.cqe_cnt - 1);
			uint8_t *c = (uint8_t *)dvc.buf +
				     (size_t)idx * dvc.cqe_size;
			uint32_t base = dvc.cqe_size - 64, q;

			if ((c[dvc.cqe_size - 1] >> 4) != 2) {
				continue;
			}
			printf("[诊断] RESP CQE[%u] byte_cnt=%u:\n  ", idx,
			       (unsigned)((c[base + 44] << 24) |
					  (c[base + 45] << 16) |
					  (c[base + 46] << 8) | c[base + 47]));
			for (q = 0; q < 64; q++) {
				printf("%02x%s", c[base + q],
				       q % 16 == 15 ? "\n  " : " ");
			}
			printf("\n");
		}
	}

	/*
	 * dbrec 增量是判断"命令到底有没有发出去"最直接的指标。
	 *
	 * SQ 增量应该等于轮数。只涨 CPU 侧那几条的量,就说明 kernel 写的
	 * WQE 网卡没取走 —— 多半是接管快照过期(pi 和 dbrec 已经相等,
	 * 网卡认为没有新工作)。这种情况下 kernel 收割的是前一条 CPU 命令
	 * 的 CQE,照样报"完成 N/N",但 target 侧查无此命令。
	 */
	{
		volatile uint32_t *qdb = (volatile uint32_t *)dvq.dbrec;
		volatile uint32_t *cdb = (volatile uint32_t *)dvc.dbrec;
		uint32_t sq_now = be32toh(qdb[1]) & 0xffff;
		uint32_t rq_now = be32toh(qdb[0]) & 0xffff;
		uint32_t cq_now = be32toh(cdb[0]) & 0xffffff;

		printf("[dbrec] SQ %u -> %u (+%u, 期望 +%u)   CQ %u -> %u (+%u)"
		       "   RQ head %u\n",
		       ctx.sq_pi, sq_now, (sq_now - ctx.sq_pi) & 0xffff, g_rounds,
		       ctx.cq_ci, cq_now, cq_now - ctx.cq_ci, rq_now);
		if (((sq_now - ctx.sq_pi) & 0xffff) != g_rounds) {
			printf("  *** SQ 增量不等于轮数:kernel 的命令没发出去 ***\n");
		}
	}

	/*
	 * kernel 写完之后,CPU 读回 SQ slot 和胶囊,确认它到底写了什么。
	 *
	 * dbrec 正常、CQ 也涨,不代表网卡执行的是我们这条命令 —— 如果
	 * kernel 的 WQE 没落到网卡看得到的地方,门铃可能让网卡重新处理了
	 * slot 里的旧内容(上一条 SPDK 命令留下的),现象一模一样,但
	 * target 侧看到的是另一条命令。
	 *
	 * 判据:
	 *   WQE offset 3    = 0x0a (MLX5_OPCODE_SEND)
	 *   WQE offset 1-2  = 接管时的 sq_pi(大端)
	 *   WQE offset 16-31 = data seg,addr 应指向胶囊
	 *   胶囊 offset 0   = 0x02 READ / 0x01 WRITE
	 *   胶囊 offset 2-3 = cid,小端。kernel 的是 0x8000 | tag
	 */
	{
		uint32_t slot = ctx.sq_pi & (dvq.sq.wqe_cnt - 1);
		uint8_t *w = (uint8_t *)dvq.sq.buf +
			     (size_t)slot * dvq.sq.stride;
		uint8_t cap[64];
		int k;

		printf("\n[SQ] slot %u (pi=%u):\n  ", slot, ctx.sq_pi);
		for (k = 0; k < 32; k++) {
			printf("%02x ", w[k]);
			if (k % 16 == 15) {
				printf("\n  ");
			}
		}

		if (g_cap_in_hbm) {
			gpu_copy_to_host(cap, g_cap_hbm, 64);
		} else {
			memcpy(cap, g_cap_host, 64);
		}
		printf("\n[胶囊] ");
		for (k = 0; k < 16; k++) {
			printf("%02x ", cap[k]);
		}
		printf("\n  opc=0x%02x cid=0x%04x %s\n", cap[0],
		       (unsigned)(cap[2] | (cap[3] << 8)),
		       (w[3] == 0x0a) ? "" : "*** WQE opcode 不是 SEND ***");
	}

	/*
	 * CQE dump。op_own 的高 4 位是 opcode:
	 *   0  MLX5_CQE_REQ        我们发的 SEND 完成了
	 *   2  MLX5_CQE_RESP_SEND  收到对端的 SEND(响应胶囊)
	 *   13 MLX5_CQE_REQ_ERR / 14 MLX5_CQE_RESP_ERR
	 *   15 MLX5_CQE_INVALID    这一格还没被网卡写过
	 * 低位是 owner bit,要和 cq_phase 一致才算新 CQE。
	 *
	 * 后 16 字节里有 s_wqe_opcode_qpn 和 wqe_counter,能看出这个
	 * CQE 对应哪条 WQE。错误 CQE 的 syndrome 在 offset 55,
	 * vendor_err 在 54 —— syndrome 0x05 是 WR_FLUSH_ERR,只是
	 * 后果,真凶在更早的 CQE 里。
	 */
	{
		unsigned k;

		printf("[CQE] ci=%u 起 4 个 (phase=%u):\n",
		       ctx.cq_ci, ctx.cq_phase);
		for (k = 0; k < 4; k++) {
			uint32_t idx = (ctx.cq_ci + k) & (dvc.cqe_cnt - 1);
			uint8_t *c = (uint8_t *)dvc.buf +
				     (size_t)idx * dvc.cqe_size;
			uint8_t oo = c[dvc.cqe_size - 1];
			uint8_t opc = oo >> 4;
			unsigned q;

			printf("  [%u] op_own=0x%02x opcode=%u owner=%u",
			       idx, oo, opc, oo & 1);
			if (opc == 13 || opc == 14) {
				printf("  syndrome=0x%02x vendor=0x%02x",
				       c[55], c[54]);
			}
			printf("\n      尾16: ");
			for (q = dvc.cqe_size - 16; q < dvc.cqe_size; q++) {
				printf("%02x ", c[q]);
			}
			printf("\n");
		}
	}

	printf("[结果] 完成 %u / %u", done, g_rounds);
	if (err) {
		printf(", 错误码 %d (%s)", err,
		       err == GIO_E_TIMEOUT ? "poller 等不到新 CQE" :
		       err == GIO_E_WAIT ? "submitter 等不到自己的完成" :
		       err == GIO_E_CQE ? "CQE 报错" :
		       err == GIO_E_CID ? "响应 cid 对不上" :
		       err == GIO_E_RSP_ADDR ? "recv buffer 地址越界" :
		       "有 NVMe 出错");
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

	/*
	 * 分段统计(只统计等到了响应的 I/O)。总时间拆成三段:
	 *   胶囊  写那 64 字节。-H 时胶囊在 uncached 显存,这一段会明显变贵
	 *   提交  抢锁 + 写 WQE + dbrec + 敲门铃。QD 大时抢锁会占比上升
	 *   等待  网络往返 + target 处理 + poller 收割 + submitter 看到完成
	 *
	 * p99 常比 p50 高一两个数量级,别当网络问题:kernel 单线程自旋轮询,
	 * 会被 GPU 调度打断,clock64() 在长自旋下也未必可靠。比较配置时看 p50。
	 */
	{
		static const char *names[4] = { "总计", "  胶囊", "  提交", "  等待" };
		unsigned long long *srcs[4];
		unsigned long long *compact[4];
		unsigned n = 0;
		int k;

		/* 按"等到了响应"压紧,cycles 已经压过 */
		for (k = 1; k < 4; k++) {
			compact[k] = calloc(done ? done : 1, sizeof(**compact));
		}
		for (i = 0; i < g_rounds; i++) {
			if (c_stat[i] == 0xffffffffu) {
				continue;
			}
			compact[1][n] = c_cap[i];
			compact[2][n] = c_sub[i];
			compact[3][n] = c_wait[i];
			n++;
		}
		srcs[0] = cycles;
		srcs[1] = compact[1];
		srcs[2] = compact[2];
		srcs[3] = compact[3];

		printf("\n===== kernel 内延迟分段 (us) =====\n");
		printf("  %-6s %8s %8s %8s %8s %8s\n",
		       "段", "mean", "min", "p50", "p90", "p99");
		for (k = 0; k < 4; k++) {
			double sum = 0;

			for (i = 0; i < done; i++) {
				us[i] = srcs[k][i] / (clk_khz / 1000.0);
				sum += us[i];
			}
			qsort(us, done, sizeof(double), cmp_d);
			printf("  %-6s %8.1f %8.1f %8.1f %8.1f %8.1f\n",
			       names[k], sum / done, us[0], us[done / 2],
			       us[(unsigned)(done * 0.90)],
			       us[(unsigned)(done * 0.99)]);
		}

		/*
		 * submitter 等待圈数:每圈是一次跨线程读 tag 状态。
		 * 锁自旋:抢 SQ 锁失败的次数,QD 大时看它判断锁是不是瓶颈。
		 */
		{
			unsigned long long smin = ~0ULL, smax = 0, ssum = 0;
			double wsum = 0;

			unsigned long long lsum = 0, lmax = 0;

			for (i = 0; i < g_rounds; i++) {
				if (c_stat[i] == 0xffffffffu) {
					continue;
				}
				if (c_spin[i] < smin) smin = c_spin[i];
				if (c_spin[i] > smax) smax = c_spin[i];
				ssum += c_spin[i];
				wsum += c_wait[i] / (clk_khz / 1000.0);
				lsum += c_lock[i];
				if (c_lock[i] > lmax) lmax = c_lock[i];
			}
			printf("\n  锁自旋: mean %llu  max %llu", lsum / done, lmax);
			printf("\n  等待圈数: mean %llu  min %llu  max %llu",
			       ssum / done, smin, smax);
			if (ssum) {
				printf("   平均每圈 %.0f ns",
				       wsum * 1000.0 / (double)ssum);
			}
			printf("\n");
		}

		/* 逐轮等待时间,看双峰是怎么分布的:
		 * 前几轮快之后稳定 -> 冷启动
		 * 快慢交替有周期   -> target poll group 轮转或 GPU 调度
		 * 完全随机         -> 别的 */
		printf("  逐条等待(us): ");
		for (i = 0; i < g_rounds && i < 40; i++) {
			if (c_stat[i] == 0xffffffffu) {
				printf("- ");
			} else {
				printf("%.0f ", c_wait[i] / (clk_khz / 1000.0));
			}
		}
		printf("%s\n", g_rounds > 40 ? "..." : "");
		for (k = 1; k < 4; k++) {
			free(compact[k]);
		}
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
		printf("  %s -a %s -n %s -g %d -b %u -r %u -L %" PRIu64
		       " -S %u -V\n", argv[0], g_traddr, g_nqn, g_gpu_id,
		       g_io_bytes, done, g_start_lba, g_seed);
		if (g_fill_by_kernel) {
			printf("  -K 是单块旧式 pattern:期望第 0 块 W,其余 C\n");
		}
	} else if (g_fill_by_kernel) {
		unsigned char *h = malloc(g_io_bytes);
		unsigned nz = 0;

		gpu_copy_to_host(h, g_data_gpu, g_io_bytes);
		for (i = 0; i < g_io_bytes; i++) {
			nz += h[i] != RPAT;
		}
		printf("\n[校验] -K 单块:%u/%u 字节不再是 0x%02x\n",
		       nz, g_io_bytes, RPAT);
		free(h);
	} else {
		/*
		 * 读的校验走 gpu_copy_to_host,不碰 qpair,可信。
		 * 只比对完成了的轮数;未完成的块应该还是 0xBB。
		 */
		uint8_t *h = malloc((size_t)nblk * g_io_bytes);

		if (!h || gpu_copy_to_host(h, g_data_gpu,
					   (size_t)nblk * g_io_bytes) != 0) {
			fprintf(stderr, "DtoH 失败\n");
			return 1;
		}
		if (report_blocks("校验", h, nblk, g_io_bytes, nlb,
				  g_start_lba, g_seed)) {
			err = err ? err : -3;
		}
		free(h);
	}

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	return (err || done != g_rounds) ? 2 : 0;
}
