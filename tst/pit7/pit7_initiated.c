/*
 * pit7_initiated.c  --  坑 7 实验程序(tst/pit7)
 *
 * 从 src/gpu_initiated.c 分叉,参数是它的超集。背景、结论、复现方法
 * 见同目录 README.md。以下是原文件说明(用法里的 -I/-D/-B/-Y/-T/-Z
 * 只在本程序里有)。
 *
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
 *   -H                   命令胶囊放显存(默认主存),分配 flag 见 -F
 *   -F <flag>            胶囊区 mcExtMallocWithFlags 的 flag(默认 1)
 *   -L <lba>             起始 LBA(默认 0),第 i 轮用 L + i*nlb
 *   -S <seed>            pattern seed,0-255(默认 1)。每次运行都该换
 *   -I <mode>            payload 由 kernel 写(配 -W),显存底色 0xEE:
 *                          1 = 发命令的同一个 kernel 边写边发,无同步
 *                          2 = 独立 kernel 先写完全部块 + 设备同步,再发
 *   -Y <us>              每轮发命令前空转(不访存),模拟 -I 的轮间间隔
 *   -B                   填充微基准:计算/写/读 × 分配 flag 0/1/4,然后退出
 *   -T <mode>            拆分 launch(Tutti snvme_smoke_gpu 形态),每轮:
 *                          1 = k_submit 退出 → k_poll → 同步
 *                          2 = k_submit 退出 → 同步 → k_poll → 同步
 *                          3 = k_prep(写显存)退出 → k_post_poll(门铃+轮询)
 *                          4 = 同 3,两者之间同步
 *   -Z                   轮询每圈 __threadfence_system()(Tutti 的 MACA 写法)
 *   -R <mode>            显存 MR 注册路径:
 *                          0 = ibv_reg_dmabuf_mr(默认)
 *                          1 = ibv_reg_mr(GPU VA),peer_mem —— Mooncake IBGDA
 *                              数据 buffer、MACA perftest 的做法
 *                          2 = devx umem_reg_ex(DMABUF)+ CREATE_MKEY
 *                              —— mxshmem IBGDA 队列内存的做法
 *                          3 = devx umem_reg(GPU VA)+ CREATE_MKEY
 *   -X                   写完显存后 __threadfence()(IBGDA_MEMBAR 的级别)
 *   -N <ibdev>           -B 注册 MR 用的网卡(默认 mlx5_1)
 *   -D <flag>            数据区用 mcExtMallocWithFlags 分配(默认 0=mcMalloc)
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

#include "pit7_io_ctx.h"
#include "pit7_devx_mkey.h"
#include "blk_pattern.h"

extern int gpu_io_run(struct gpu_io_ctx *, unsigned int, unsigned long long,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned long long *,
		      int *, unsigned int *);
extern int gpu_alloc_exportable(void **, int *, size_t);
extern int gpu_alloc_exportable_ex(void **, int *, size_t, unsigned int);
extern int gpu_map_host(void *, size_t, unsigned int, void **);
extern int gpu_set_dev(int);
extern int gpu_clock_khz(int, int *);
extern int gpu_copy_to_host(void *, const void *, size_t);
extern int gpu_copy_to_dev(void *, const void *, size_t);
extern int gpu_fill_blocks(void *, unsigned int, unsigned int, unsigned int,
			   unsigned long long, unsigned int);
extern int gpu_bench(void *, unsigned int, unsigned int, unsigned int,
		     unsigned long long *);
extern int gpu_free(void *);
extern int gpu_io_run_split(struct gpu_io_ctx *, unsigned int,
			    unsigned long long, int,
			    unsigned long long *, unsigned long long *,
			    unsigned long long *, unsigned long long *,
			    unsigned long long *, double *, int *,
			    unsigned int *);
extern int gpu_memset_dev(void *, int, size_t);
extern int gpu_sync(void);
extern int gpu_fill_kernel(void *, int, size_t);

#define MC_HOST_REGISTER_MAPPED		0x02
#define MC_HOST_REGISTER_IO_MEMORY	0x04

#define CAP_AREA_LEN	(2 * 1024 * 1024)
#define RPAT		BLK_RPAT	/* 读:预填,被 target 覆盖才算落地 */
#define WPAT		BLK_WPAT	/* -K 旧式写 pattern */
#define CPAT		BLK_CPAT	/* 盘上涂色 */
#define EPAT		BLK_EPAT	/* -I 显存底色 */
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
static uint32_t	g_io_bytes = 4096;
static int	g_write, g_cap_in_hbm, g_paint, g_verify;
static int	g_fill_by_kernel;	/* -K: 源数据由 kernel 写,不用 mcMemset */
static int	g_fill_in_io;		/* -I: 1 同 kernel 写;2 拆分 kernel + 同步 */
static double	g_gap_us;		/* -Y */
static int	g_bench;		/* -B */
static int	g_split;		/* -T */
static int	g_poll_fence;		/* -Z */
static int	g_reg_mode;		/* -R */
static int	g_dev_fence;		/* -X */
static char	g_bench_dev[32] = "mlx5_1";	/* -N */
static uint64_t	g_start_lba;
static uint8_t	g_seed = 1;
static unsigned	g_data_flags;		/* -D */
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

/*
 * 显存 MR 的两条注册路径(-R)。
 *   0 dma-buf :mcMemGetHandleForAddressRange 导出 fd + ibv_reg_dmabuf_mr
 *   1 peer_mem:ibv_reg_mr 直接传 GPU VA,由 MACA 的 peer memory 模块 pin 页
 * 要回答的问题:注册路径会不会改变这段显存的缓存行为(坑 7)。Tutti 的
 * snvme 走 P2P pin,mxshmem IBGDA 的注册方式也和我们不同,而它们不需要
 * 显式同步。
 */
static struct ibv_mr *
reg_gpu_mr(struct ibv_pd *pd, void *va, size_t len, int fd, int acc)
{
	struct ibv_mr *mr;

	if (g_reg_mode >= 2) {
		const char *why;

		mr = pit7_devx_reg_mr(pd, va, len, g_reg_mode == 2 ? fd : -1,
				      acc & ~IBV_ACCESS_RELAXED_ORDERING, &why);
		if (!mr) {
			fprintf(stderr, "[MR] %s 失败: %s (errno=%d %s)\n",
				g_reg_mode == 2 ? "devx-dmabuf" : "devx-va",
				why, errno, strerror(errno));
		} else {
			printf("[MR] %s  va=%p len=%zu key=0x%x\n",
			       g_reg_mode == 2 ? "devx-dmabuf" : "devx-va",
			       va, len, mr->lkey);
		}
		return mr;
	}
	if (g_reg_mode == 1) {
		mr = ibv_reg_mr(pd, va, len, acc);
		if (!mr) {
			fprintf(stderr, "[MR] peer_mem 注册失败: %s "
				"(peer memory 模块加载了吗?)\n", strerror(errno));
		} else {
			printf("[MR] peer_mem  va=%p len=%zu rkey=0x%x\n",
			       va, len, mr->rkey);
		}
		return mr;
	}
	mr = ibv_reg_dmabuf_mr(pd, 0, len, (uint64_t)va, fd, acc);
	if (!mr) {
		fprintf(stderr, "[MR] dma-buf 注册失败: %s\n", strerror(errno));
	}
	return mr;
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
			g_data_mr = reg_gpu_mr(pd, g_base_gpu, g_total_len,
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
	       "      [-L start_lba] [-S seed] [-W 发写] [-I 1|2 kernel 写 payload]\n"
	       "      [-Y gap_us] [-B 填充微基准] [-T 1..4 拆分 launch] [-Z 轮询 fence]\n"
	       "      [-R 0..3 MR 注册: dmabuf|peer|devx-dmabuf|devx-va] [-X]\n"
	       "      [-N ibdev(-B 用)]\n"
	       "      [-H 胶囊放显存] [-F cap_flag] [-D data_flag]\n"
	       "      [-P 涂色] [-V 逐块回读] [-K 旧式单块对照]\n", p);
}

static double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/*
 * 填充微基准。回答两个问题:
 *   ① -I 的填充 112 us / 4 KiB 是慢在算 blk_word 还是慢在 store
 *   ② uncached(flag 1/4)对写和读各自的代价 —— attention 要在
 *      这块内存上读写 KV,读代价和写一样重要
 * 每项跑 reps 次 4 KiB,报 p50/min,单位 us,另给 ns/字。
 */
static int
bench_one(unsigned long long *cyc, double *us, unsigned reps, int clk_khz,
	  void *p, uint32_t bs, unsigned mode, double *p50)
{
	unsigned r;

	if (mode == 3 || mode == 5) {
		gpu_bench(p, bs / 8, 4, 1, cyc);	/* 读之前先写一遍 */
	}
	if (gpu_bench(p, bs / 8, reps, mode, cyc) != 0) {
		return -1;
	}
	for (r = 1; r < reps; r++) {			/* 丢掉第一次 */
		us[r - 1] = cyc[r] / (clk_khz / 1000.0);
	}
	qsort(us, reps - 1, sizeof(double), cmp_d);
	*p50 = us[(reps - 1) / 2];
	return 0;
}

/*
 * 填充微基准。每项 reps 次 × bs 字节,单线程,报 p50 的 ns/字。
 *
 *   ① 只算:blk_word 本身的代价(与内存无关,只跑一次)
 *   ② 写 / 读 × volatile / 普通指针:volatile 访存语义是不是慢的来源
 *   ③ × 分配 flag 0/1/4 × MR 注册(无 / dma-buf / peer_mem):
 *      注册路径会不会改变这段显存的访问代价 —— 如果某条路径让 flag 0
 *      的读写变得和 flag 4 一样慢,说明它把内存切到了 uncached 模式,
 *      也就是坑 7 在那条路径上不存在的原因
 */
static int
run_bench(int clk_khz, uint32_t bs)
{
	static const unsigned flags[3] = { 0, 1, 4 };
	static const char *rname[5] = { "无", "dmabuf", "peer", "devx-db", "devx-va" };
	static const unsigned modes[4] = { 1, 4, 3, 5 };
	const unsigned reps = 200;
	unsigned long long *cyc = calloc(reps, sizeof(*cyc));
	double *us = calloc(reps, sizeof(*us));
	struct ibv_device **dl = NULL;
	struct ibv_context *vc = NULL;
	struct ibv_pd *pd = NULL;
	int ndev = 0, d, rm;
	unsigned f, m;
	double v;

	if (!cyc || !us) {
		return 1;
	}

	dl = ibv_get_device_list(&ndev);
	for (d = 0; dl && d < ndev; d++) {
		if (strcmp(ibv_get_device_name(dl[d]), g_bench_dev) == 0) {
			vc = ibv_open_device(dl[d]);
			break;
		}
	}
	pd = vc ? ibv_alloc_pd(vc) : NULL;
	if (!pd) {
		printf("[bench] 打不开 %s,只测不注册的情况\n", g_bench_dev);
	}

	{
		void *p = NULL;
		int fd = -1;

		gpu_alloc_exportable_ex(&p, &fd, HUGE_2M, 0);
		if (p && bench_one(cyc, us, reps, clk_khz, p, bs, 0, &v) == 0) {
			printf("\n===== 填充微基准:%u 字节 × %u 次,单线程 =====\n",
			       bs, reps);
			printf("  只算 blk_word: %.1f us (%.1f ns/字)\n",
			       v, v * 1000.0 / (bs / 8));
		}
		if (fd >= 0) close(fd);
		if (p) gpu_free(p);
	}

	printf("\n  ns/字 (p50)          写vol   写plain   读vol   读plain\n");
	for (rm = 0; rm < 5; rm++) {
		if (rm > 0 && !pd) {
			break;
		}
		for (f = 0; f < 3; f++) {
			void *p = NULL;
			int fd = -1;
			struct ibv_mr *mr = NULL;
			int acc = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
				  IBV_ACCESS_REMOTE_WRITE;

			printf("  %-7s flag %u    ", rname[rm], flags[f]);
			if (gpu_alloc_exportable_ex(&p, &fd, HUGE_2M,
						    flags[f]) != 0) {
				printf("分配失败\n");
				continue;
			}
			gpu_memset_dev(p, 0, HUGE_2M);
			gpu_sync();
			const char *why = "";
			int devx = rm >= 3;

			if (rm == 1) {
				mr = ibv_reg_dmabuf_mr(pd, 0, HUGE_2M,
						       (uint64_t)p, fd, acc);
			} else if (rm == 2) {
				mr = ibv_reg_mr(pd, p, HUGE_2M, acc);
			} else if (devx) {
				mr = pit7_devx_reg_mr(pd, p, HUGE_2M,
						      rm == 3 ? fd : -1, acc, &why);
			}
			if (rm > 0 && !mr) {
				printf("注册失败: %s %s\n", why, strerror(errno));
			} else {
				for (m = 0; m < 4; m++) {
					if (bench_one(cyc, us, reps, clk_khz,
						      p, bs, modes[m], &v) != 0) {
						printf("   失败 ");
						continue;
					}
					printf("%8.0f", v * 1000.0 / (bs / 8));
				}
				printf("\n");
			}
			if (mr && devx) {
				pit7_devx_dereg_mr(mr);
			} else if (mr) {
				ibv_dereg_mr(mr);
			}
			if (fd >= 0) close(fd);
			gpu_free(p);
		}
	}

	if (pd) ibv_dealloc_pd(pd);
	if (vc) ibv_close_device(vc);
	if (dl) ibv_free_device_list(dl);
	free(cyc);
	free(us);
	return 0;
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
	unsigned long long *cycles, *c_cap, *c_sub, *c_wait, *c_spin;
	double *us;
	unsigned long long *c_fill;
	double *wall = NULL;
	uint32_t sector, nlb, nblk;
	int err = 0, clk_khz = 0, op;
	unsigned int done = 0, i;
	void *d_sq, *d_dbrec, *d_bf, *d_cq, *d_cqdb;

	memset(&dvq, 0, sizeof(dvq));
	memset(&dvc, 0, sizeof(dvc));
	memset(&ctx, 0, sizeof(ctx));

	while ((op = getopt(argc, argv, "a:s:n:g:r:b:WHPVKF:L:S:I:D:Y:BT:ZR:N:X")) != -1) {
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
		case 'K': g_fill_by_kernel = 1; break;
		case 'F': g_cap_flags = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'L': g_start_lba = strtoull(optarg, NULL, 0); break;
		case 'S': g_seed = (uint8_t)strtoul(optarg, NULL, 0); break;
		case 'I': g_fill_in_io = atoi(optarg); break;
		case 'Y': g_gap_us = atof(optarg); break;
		case 'B': g_bench = 1; break;
		case 'T': g_split = atoi(optarg); break;
		case 'Z': g_poll_fence = 1; break;
		case 'R': g_reg_mode = atoi(optarg); break;
		case 'X': g_dev_fence = 1; break;
		case 'N': snprintf(g_bench_dev, sizeof(g_bench_dev), "%s", optarg); break;
		case 'D': g_data_flags = (unsigned)strtoul(optarg, NULL, 0); break;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (!g_bench && (!g_traddr[0] || !g_nqn[0])) {
		usage(argv[0]);
		return 1;
	}
	if (g_reg_mode < 0 || g_reg_mode > 3) {
		fprintf(stderr, "-R 取 0..3\n");
		return 1;
	}
	if (g_split < 0 || g_split > 4) {
		fprintf(stderr, "-T 取 1..4\n");
		return 1;
	}
	if (g_fill_in_io < 0 || g_fill_in_io > 2) {
		fprintf(stderr, "-I 取 1 或 2\n");
		return 1;
	}
	if (g_io_bytes % 8 || g_rounds == 0) {
		fprintf(stderr, "-b 必须是 8 的倍数,-r 必须 > 0\n");
		return 1;
	}
	if (g_fill_in_io && (!g_write || g_fill_by_kernel)) {
		fprintf(stderr, "-I 只能配 -W,且不能和 -K 同用\n");
		return 1;
	}

	printf("=========================================================\n");
	printf(" GPU-initiated NVMe-oF 原型\n");
	printf(" CPU 只做初始化,稳态收发全在 kernel 内\n");
	printf("=========================================================\n");

	if (g_bench) {
		if (gpu_set_dev(g_gpu_id) != 0 ||
		    gpu_clock_khz(g_gpu_id, &clk_khz) != 0) {
			fprintf(stderr, "选 GPU %d 失败\n", g_gpu_id);
			return 1;
		}
		return run_bench(clk_khz, g_io_bytes);
	}

	spdk_env_opts_init(&opts);
	opts.opts_size = sizeof(opts);
	opts.name = "pit7_initiated";
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
	if (gpu_alloc_exportable_ex(&g_base_gpu, &g_base_fd, g_total_len,
				    g_data_flags) != 0) {
		fprintf(stderr, "显存分配失败 (data flag=0x%x)\n", g_data_flags);
		return 1;
	}
	if (g_data_flags) {
		printf("[GPU] 数据区用 mcExtMallocWithFlags(0x%x)\n",
		       g_data_flags);
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
		g_cap_mr = reg_gpu_mr(g_pd, g_cap_hbm, CAP_AREA_LEN, g_cap_fd,
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
	ctx.data_step    = g_fill_by_kernel ? 0 : g_io_bytes;
	ctx.fill_mode    = g_fill_in_io == 1;
	ctx.gap_cycles   = (uint64_t)(g_gap_us * clk_khz / 1000.0);
	ctx.poll_fence   = (uint8_t)g_poll_fence;
	ctx.dev_fence    = (uint8_t)g_dev_fence;
	ctx.seed         = g_seed;

	/*
	 * kernel 不补 recv buffer,每轮响应消耗一个。RQ 里现成有多少就只能
	 * 跑多少轮 —— 超出的那轮响应无处安放,表现成和 CQE 判错一模一样的
	 * 死等,别混淆。
	 */
	if (g_rounds > dvq.rq.wqe_cnt) {
		printf("[警告] RQ 深度 %u < 轮数 %u,第 %u 轮之后会卡住\n",
		       dvq.rq.wqe_cnt, g_rounds, dvq.rq.wqe_cnt);
	}

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
	} else if (g_fill_in_io == 1) {
		/* 写 + -I 1:显存底色 0xEE,pattern 留给 kernel 在发命令前写 */
		gpu_memset_dev(g_data_gpu, EPAT, (size_t)nblk * g_io_bytes);
		gpu_sync();
		printf("[准备] 显存 %u 块底色 0x%02x,payload 由 io kernel 写\n",
		       nblk, EPAT);
	} else if (g_fill_in_io == 2) {
		/*
		 * 写 + -I 2:拆分形态。独立 kernel 写完全部块,再设备同步。
		 * 分开计 launch 返回和同步完成的时间 —— launch 是异步的,
		 * 真正的填充耗时落在同步那一段。
		 */
		double t0, t1, t2;

		gpu_memset_dev(g_data_gpu, EPAT, (size_t)nblk * g_io_bytes);
		gpu_sync();
		t0 = now_us();
		if (gpu_fill_blocks(g_data_gpu, nblk, g_io_bytes, g_seed,
				    g_start_lba, nlb) != 0) {
			fprintf(stderr, "fill kernel launch 失败\n");
			return 1;
		}
		t1 = now_us();
		gpu_sync();
		t2 = now_us();
		printf("[准备] 拆分形态:fill kernel 写 %u 块 + 同步\n"
		       "       launch %.1f us, 至同步完成 %.1f us, "
		       "摊到每块 %.1f us\n",
		       nblk, t1 - t0, t2 - t0, (t2 - t0) / nblk);
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

		ctx.sq_pi = (uint16_t)sq_pi;
		ctx.cq_ci = cq_ci;
		ctx.cq_phase = (uint8_t)((cq_ci / dvc.cqe_cnt) & 1);
		printf("[接管] SQ pi=%u  CQ ci=%u phase=%u\n",
		       sq_pi, cq_ci, ctx.cq_phase);
	}

	/* ---- 跑 ---- */
	cycles = calloc(g_rounds, sizeof(*cycles));
	c_cap  = calloc(g_rounds, sizeof(*c_cap));
	c_sub  = calloc(g_rounds, sizeof(*c_sub));
	c_wait = calloc(g_rounds, sizeof(*c_wait));
	c_spin = calloc(g_rounds, sizeof(*c_spin));
	c_fill = calloc(g_rounds, sizeof(*c_fill));
	us = calloc(g_rounds, sizeof(*us));
	printf("\n[运行] %s %u 轮, 每轮 %u 字节, %s%s",
	       g_write ? "WRITE" : "READ", g_rounds, g_io_bytes,
	       ctx.data_step ? "每轮一块" : "单块复用",
	       g_fill_in_io == 1 ? ", payload 由 io kernel 写" :
	       g_fill_in_io == 2 ? ", payload 由独立 kernel 预写" : "");
	if (ctx.gap_cycles) {
		printf(", 轮间空转 %.0f us", g_gap_us);
	}
	if (g_reg_mode) {
		static const char *rn[4] = { "", "peer_mem", "devx-dmabuf",
					     "devx-va" };
		printf(", MR 走 %s", rn[g_reg_mode]);
	}
	if (g_dev_fence) {
		printf(", 写显存后 __threadfence()");
	}
	if (g_split) {
		static const char *sm[5] = { "",
			"submit+门铃 | poll",
			"submit+门铃 | 同步 | poll",
			"写显存 | 门铃+poll",
			"写显存 | 同步 | 门铃+poll" };
		printf(", 拆分 launch(%s)", sm[g_split]);
	}
	if (g_poll_fence) {
		printf(", 轮询每圈 fence");
	}
	printf("\n");

	if (g_split) {
		wall = calloc(g_rounds, sizeof(*wall));
		if (gpu_io_run_split(&ctx, g_rounds, g_start_lba, g_split,
				     c_fill, c_cap, c_sub, c_wait, c_spin,
				     wall, &err, &done) != 0) {
			fprintf(stderr, "拆分模式执行失败\n");
			return 1;
		}
		for (i = 0; i < done; i++) {
			cycles[i] = c_fill[i] + c_cap[i] + c_sub[i] + c_wait[i];
		}
	} else if (gpu_io_run(&ctx, g_rounds, g_start_lba, cycles, c_fill,
			      c_cap, c_sub, c_wait, c_spin, &err,
			      &done) != 0) {
		fprintf(stderr, "kernel 执行失败\n");
		return 1;
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
		uint32_t cq_now = be32toh(cdb[0]) & 0xffffff;

		printf("[dbrec] SQ %u -> %u (+%u, 期望 +%u)   CQ %u -> %u (+%u)\n",
		       ctx.sq_pi, sq_now, sq_now - ctx.sq_pi, g_rounds,
		       ctx.cq_ci, cq_now, cq_now - ctx.cq_ci);
		if (sq_now - ctx.sq_pi != g_rounds) {
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
	 *   胶囊 offset 2-3 = cid,小端。kernel 打了 0xE000 标记则是 00 e0
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

	/*
	 * 分段统计。总时间拆成三段:
	 *   胶囊  写那 64 字节。-H 时胶囊在 uncached 显存,这一段会明显变贵
	 *   提交  写 WQE + dbrec + 敲门铃,全在主存,应该很稳
	 *   等待  网络往返 + target 处理 + 轮询到 CQE。min 值最接近真实网络往返
	 *
	 * p99 常比 p50 高一两个数量级,别当网络问题:kernel 单线程自旋轮询,
	 * 会被 GPU 调度打断,clock64() 在长自旋下也未必可靠。比较配置时看 p50。
	 */
	{
		static const char *names[5] = { "总计", "  填充", "  胶囊",
						 "  提交", "  等待" };
		unsigned long long *srcs[5] = { cycles, c_fill, c_cap,
						c_sub, c_wait };
		int k;

		printf("\n===== kernel 内延迟分段 (us) =====\n");
		printf("  %-6s %8s %8s %8s %8s %8s\n",
		       "段", "mean", "min", "p50", "p90", "p99");
		for (k = 0; k < 5; k++) {
			double sum = 0;

			if (k == 1 && g_fill_in_io != 1) {
				continue;	/* 只有 -I 才有填充段 */
			}

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
		 * 轮询圈数。CQ buffer 在主存,GPU 每转一圈要走 PCIe 读
		 * 64 字节 —— 几百纳秒一次。
		 *   转几十圈    -> 等待时间不是花在等网络上,是自旋本身贵
		 *   转几万圈    -> 确实在等对端,那 45 us 就是真实往返
		 */
		{
			unsigned long long smin = ~0ULL, smax = 0, ssum = 0;
			double wsum = 0;

			for (i = 0; i < done; i++) {
				if (c_spin[i] < smin) smin = c_spin[i];
				if (c_spin[i] > smax) smax = c_spin[i];
				ssum += c_spin[i];
				wsum += c_wait[i] / (clk_khz / 1000.0);
			}
			printf("\n  轮询圈数: mean %llu  min %llu  max %llu",
			       ssum / done, smin, smax);
			if (ssum) {
				printf("   平均每圈 %.0f ns",
				       wsum * 1000.0 / (double)ssum);
			}
			printf("\n");
		}

		/*
		 * 拆分模式下 kernel 内分段不含 launch 开销;host 墙钟才是
		 * 一轮的真实代价(两次 launch + 同步)。
		 */
		if (wall) {
			for (i = 0; i < done; i++) {
				us[i] = wall[i];
			}
			qsort(us, done, sizeof(double), cmp_d);
			printf("  host 墙钟/轮(us): min %.0f  p50 %.0f  p90 %.0f\n",
			       us[0], us[done / 2], us[(unsigned)(done * 0.9)]);
		}

		/* 逐轮等待时间,看双峰是怎么分布的:
		 * 前几轮快之后稳定 -> 冷启动
		 * 快慢交替有周期   -> target poll group 轮转或 GPU 调度
		 * 完全随机         -> 别的 */
		printf("  逐轮等待(us): ");
		for (i = 0; i < done && i < 40; i++) {
			printf("%.0f ", c_wait[i] / (clk_khz / 1000.0));
		}
		printf("%s\n", done > 40 ? "..." : "");
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
		} else if (g_fill_in_io == 2) {
			printf("  -I 2:期望全 '.'(独立 kernel + 同步是已知可行形态)\n");
		} else if (g_fill_in_io) {
			printf("  -I 1:全 '.' = kernel 内写对网卡可见;"
			       "'S' = 取到了 0x%02x 旧值(坑 7)\n", EPAT);
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
		if (report_blocks("校验", h, done, g_io_bytes, nlb,
				  g_start_lba, g_seed)) {
			err = err ? err : -3;
		}
		free(h);
	}

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	return (err || done != g_rounds) ? 2 : 0;
}
