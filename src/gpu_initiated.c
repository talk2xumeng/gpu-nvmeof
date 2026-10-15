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
 *   -Q <qd>              每个 QP 同时在飞的 I/O 数(submitter 线程数,默认 1)
 *   -q <n>               QP 数(默认 1,最多 GIO_MAX_QP)。每个 QP 一个 poller、
 *                        QD 个 submitter,总条数按 QP 切成连续段;总在飞 = n × QD
 *   -O <bits>            调优/诊断:bit0 submitter 稀疏 fence(每 16 圈),
 *                        bit1 poller 空转稀疏检查,bit2 配合 bit0 改为 1024 圈,
 *                        bit4(=16) 自轮询:不要独立 poller,
 *                        bit5(=32) 串行快路径(仅 QD=1),
 *                        bit6(=64) 不启动 poller block(配 16 或 32),
 *                        bit7(=128) 读 CQE 硬件时间戳,
 *                        bit8(=256) 精简提交(仅 QD=1,配 32):门铃后不做带
 *                        fence 的写,bit9(=512) 胶囊槽位轮换,
 *                        bit10(=1024) 旧式 cid 0xE000|j(仅 QD=1),
 *                        bit11(=2048) 不补 recv(C1 行为,对照用;
 *                        此时 -r 不能超过挂着的 recv 数 ~127)
 *                        poller 降本(C3 前置,可组合):
 *                        bit12(=0x1000) RQ 批量补,bit13(=0x2000) CQ ci 懒更新,
 *                        bit14(=0x4000) SEND 选择性 signal —— 这三位默认开,
 *                        -O 的值与 0x7000 按位或;
 *                        bit15(=0x8000) WQE 寄存器构造、不回读 SQ 槽(实测变慢);
 *                        bit16(=0x10000) 旧行为:不加默认的 0x7000(对照用)
 *                        bit17(=0x20000) 剖析:提交路径、poller 每圈分段计时
 *                        bit18(=0x40000) BlueFlame 只回读 1 次(WQE 在寄存器里构造)
 *                        bit19(=0x80000) BF 写满 64 字节,bit20(=0x100000) BF 两块轮换,
 *                        bit21(=0x200000) 写 BF 前空等 ~3.6 us(这三位配 0x8000/0x40000)
 *                        bit22(=0x400000) 默认路径的 4 次回读并发发出(不配 0x8000)
 *                        bit23(=0x800000) 只敲门铃(写 8 字节,不送 BF 数据),配 0x8000/0x40000
 *   -W                   发 WRITE(默认 READ)
 *   -H                   命令胶囊放显存(默认主存),分配 flag 见 -F
 *   -F <flag>            胶囊区 mcExtMallocWithFlags 的 flag(默认 1)
 *   -L <lba>             起始 LBA(默认 0),第 i 轮用 L + i*nlb
 *   -S <seed>            pattern seed,0-255(默认 1)。每次运行都该换
 *   -K                   旧式对照:单块、单字节 pattern、独立 kernel 填充
 *   -P                   把 [L, L+r) 涂成 0xCC 然后退出
 *   -V                   回读 [L, L+r) 逐块校验然后退出(-S 要和写入时一致)
 *
 * KV 命令集(target 的 namespace 必须是 KV,见 tst/kv_target):
 *
 *   -k                   KV 模式。第 i 条用 key blk_kv_key(seed, L+i)、
 *                        value = 显存第 i 块,长度 -b(8 的倍数即可,不受扇区约束)
 *                        默认 Retrieve;-W Store;-E Exist;-D Delete
 *   -k -P / -k -V        CPU 路径 Delete / Retrieve 这批 key(-V 逐块比对,
 *                        不存在的 key 记作 C)
 *   -k -V -D             回读,期望这批 key 全部不存在(验 Delete):
 *                        全是 C 才 PASS,还在的 key 逐个列出
 *
 *   Retrieve/Exist/Delete:进程内先用 CPU 路径 Store 好这批 key;
 *   Retrieve 额外核对每条 CQE DW0(value 长度)== -b。
 *   Store:进程内先 Delete 这批 key,校验同写,另起进程 -k -V。
 *   Delete:另起进程 -k -V -D。
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
#include "wqe_build.h"		/* KV 胶囊构造,CPU 路径和 kernel 共用 */

extern int gpu_io_run(struct gpu_io_ctx *, unsigned int, unsigned long long,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned int *,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, unsigned long long *,
		      unsigned long long *, double *,
		      unsigned int *, struct gio_shared *);
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
#define CAP_QP_STRIDE	(64 * 1024)	/* 多 QP:每个 QP 的胶囊槽区,1024 × 64 字节 */
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
static uint32_t	g_nq = 1;		/* -q: QP 数 */
static uint32_t	g_opt;
static uint32_t	g_io_bytes = 4096;
static int	g_write, g_cap_in_hbm, g_paint, g_verify;
static int	g_fill_by_kernel;	/* -K: 源数据由 kernel 写,不用 mcMemset */
static int	g_kv;			/* -k: KV 命令集 */
static int	g_kv_exist, g_kv_delete;	/* -E / -D */
#define KV_KLEN	16
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
	o->num_io_queues = g_nq + 1;
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

/*
 * KV 命令走 SPDK 的 CPU 路径(io_raw),同样只能在接管 QP 之前用。
 *
 * 胶囊用和 kernel 同一个 nvme_build_kv_sqe 构造,SPDK 只覆盖 cid 和
 * dptr —— 所以 CPU 路径先跑通,就同时验证了构造函数和 target。
 * 返回 NVMe status 的 (SCT << 8) | SC,0 为成功,-1 为提交失败;
 * dw0 带回 CQE DW0(Retrieve 的 value 长度)。
 */
struct kv_cpl {
	int		done;
	uint32_t	dw0;
	uint32_t	st;
};

static void
kv_cb(void *a, const struct spdk_nvme_cpl *cpl)
{
	struct kv_cpl *k = a;

	k->dw0 = cpl->cdw0;
	k->st = ((uint32_t)cpl->status.sct << 8) | cpl->status.sc;
	k->done = 1;
}

static int
cpu_kv(uint8_t opc, void *buf, uint32_t len, uint64_t idx, uint32_t *dw0)
{
	struct spdk_nvme_cmd cmd;
	struct nvme_sqe sqe;
	struct kv_cpl k = { 0 };
	uint8_t key[KV_KLEN];
	int has_data = nvme_kv_opc_has_data(opc);

	SPDK_STATIC_ASSERT(sizeof(sqe) == sizeof(cmd), "SQE 必须是 64 字节");
	blk_kv_key(key, g_seed, idx);
	nvme_build_kv_sqe(&sqe, opc, 0, spdk_nvme_ns_get_id(g_ns), key,
			  KV_KLEN, 0, 0, 0, len);
	memcpy(&cmd, &sqe, sizeof(cmd));
	cmd.psdt = 0;
	memset(&cmd.dptr, 0, sizeof(cmd.dptr));

	if (spdk_nvme_ctrlr_cmd_io_raw(g_ctrlr, g_qpair, &cmd,
				       has_data ? buf : NULL,
				       has_data ? len : 0, kv_cb, &k) != 0) {
		return -1;
	}
	while (!k.done) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
		/* 准备阶段要逐个 Store/Delete 一批 key,顺带发 keep-alive,
		 * 免得跑过 KATO 被 target 断连 */
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}
	if (dw0) {
		*dw0 = k.dw0;
	}
	return (int)k.st;
}

static const char *
kv_opc_name(uint8_t opc)
{
	switch (opc) {
	case NVME_KV_OPC_STORE:		return "KV STORE";
	case NVME_KV_OPC_RETRIEVE:	return "KV RETRIEVE";
	case NVME_KV_OPC_DELETE:	return "KV DELETE";
	case NVME_KV_OPC_EXIST:		return "KV EXIST";
	case NVME_KV_OPC_LIST:		return "KV LIST";
	}
	return "KV ?";
}

/* status 字(含 phase 位)里的 SCT=0 SC,给 KV 状态码一个名字 */
static const char *
kv_sc_name(uint16_t status)
{
	if (((status >> 9) & 7) != 0) {
		return "";
	}
	switch ((status >> 1) & 0xff) {
	case 0x01: return " Invalid Opcode —— target 不认 KV,namespace 不是 KV?";
	case 0x02: return " Invalid Field —— key 长度 > 16 或超过 max_io_size";
	case NVME_KV_SC_CAPACITY_EXCEEDED: return " Capacity Exceeded";
	case NVME_KV_SC_INVALID_VALUE_SIZE: return " Invalid Value Size";
	case NVME_KV_SC_INVALID_KEY_SIZE: return " Invalid Key Size";
	case NVME_KV_SC_KEY_NOT_EXIST: return " KV Key Does Not Exist";
	case NVME_KV_SC_KEY_EXISTS: return " Key Exists";
	}
	return "";
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
	       "      [-P 涂色] [-V 逐块回读] [-K 旧式单块对照]\n"
	       "      [-q QP 数,默认 1;总在飞 = QP 数 × QD]\n"
	       "      [-k KV 模式:默认 Retrieve,-W Store,-E Exist,-D Delete]\n"
	       "      [-O opt 位,与默认 0x7000 按位或;-O 0x10000 = 不加默认位]\n", p);
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

/* "[运行]" 行里的 QP 数,单 QP 时不显示 */
static const char *
qp_desc(unsigned nq)
{
	static char b[32];

	if (nq <= 1) {
		return "";
	}
	snprintf(b, sizeof(b), " × %u QP", nq);
	return b;
}

/*
 * 多 QP 的共享区合计成一份,下文的统计输出照旧用。计数器相加;首个错误
 * 取最先出现的 QP;时钟起止取 QP 0(时钟只能在同一 block 内相减)。
 */
static void
sh_sum(struct gio_shared *o, const struct gio_shared *s, unsigned nq)
{
	unsigned q, i;

	*o = s[0];
	for (q = 1; q < nq; q++) {
		const struct gio_shared *x = &s[q];

		if (!o->err) o->err = x->err;
		if (!o->nvme_err) o->first_status = x->first_status;
		if (!o->cid_bad) o->first_bad_cid = x->first_bad_cid;
		if (!o->err_cqe) o->err_cqe = x->err_cqe;
		o->done_cnt += x->done_cnt;
		o->nvme_err += x->nvme_err;
		o->cid_bad += x->cid_bad;
		o->wc_mismatch += x->wc_mismatch;
		o->cqe_req += x->cqe_req;
		o->cqe_resp += x->cqe_resp;
		o->resp_inline += x->resp_inline;
		o->poll_spins += x->poll_spins;
		o->poll_hits += x->poll_hits;
		o->poll_cycles += x->poll_cycles;
		o->resp_cycles += x->resp_cycles;
		o->rq_reposted += x->rq_reposted;
		o->rq_flushes += x->rq_flushes;
		o->cq_db_writes += x->cq_db_writes;
		for (i = 0; i < 16; i++) {
			o->prof[i] += x->prof[i];
		}
	}
}

/* ================================================================== */
/* 把一个 SPDK qpair 交给 GPU(C3 起每个 QP 各做一遍)                */
/* ================================================================== */

/* 每个 QP 在 host 侧的硬件对象 */
struct qp_hw {
	struct spdk_nvme_qpair	*qpair;
	struct ibv_qp		*qp;
	struct ibv_cq		*cq;
	struct mlx5dv_qp	dvq;
	struct mlx5dv_cq	dvc;
	uint32_t		rq_posted;
	uint64_t		rsp_lo, rsp_gap;
};

/*
 * 拿到 qpair 底下的 mlx5 QP/CQ,把 RQ/SQ/CQ/dbrec/UAR 和 recv buffer 映射给
 * GPU,填 ctx 里和硬件有关的字段。q 只用来打印;q == 0 时输出和单 QP 一样。
 */
static int
qp_attach(struct qp_hw *h, struct gpu_io_ctx *ctx, unsigned q)
{
	struct mlx5dv_obj obj;
	struct spdk_nvme_io_qpair_opts qopts;
	void *d_sq, *d_rq, *d_q, *d_dbrec, *d_bf, *d_cq, *d_cqdb, *d_rsp;
	int v = (q == 0);

	memset(&h->dvq, 0, sizeof(h->dvq));
	memset(&h->dvc, 0, sizeof(h->dvc));
	h->qp = spdk_nvme_qpair_get_ibv_qp(h->qpair);
	h->cq = spdk_nvme_qpair_get_ibv_cq(h->qpair);
	if (!h->qp || !h->cq) {
		fprintf(stderr, "拿不到 ibv_qp/ibv_cq,SPDK 打过 patch 吗?\n");
		return -1;
	}
	h->dvq.comp_mask = MLX5DV_QP_MASK_UAR_MMAP_OFFSET;
	obj.qp.in = h->qp; obj.qp.out = &h->dvq;
	obj.cq.in = h->cq; obj.cq.out = &h->dvc;
	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ)) {
		fprintf(stderr, "mlx5dv_init_obj 失败\n");
		return -1;
	}
	if (v) {
		printf("\n[QP] qpn=0x%x sq %ux%u  cq %ux%u\n",
		       h->qp->qp_num, h->dvq.sq.wqe_cnt, h->dvq.sq.stride,
		       h->dvc.cqe_cnt, h->dvc.cqe_size);
	} else {
		printf("[QP%u] qpn=0x%x sq %ux%u  cq %ux%u\n", q,
		       h->qp->qp_num, h->dvq.sq.wqe_cnt, h->dvq.sq.stride,
		       h->dvc.cqe_cnt, h->dvc.cqe_size);
	}

	/*
	 * mlx5 的 RQ 和 SQ 在同一块 buffer 里(RQ 在前),SQ 起点往往不对齐页,
	 * 两者可能共用一页。整段一起注册,再按偏移算出各自的设备指针。
	 */
	{
		char *rq_lo = (char *)h->dvq.rq.buf;
		char *rq_hi = rq_lo + (size_t)h->dvq.rq.wqe_cnt * h->dvq.rq.stride;
		char *sq_lo = (char *)h->dvq.sq.buf;
		char *sq_hi = sq_lo + (size_t)h->dvq.sq.wqe_cnt * h->dvq.sq.stride;
		char *lo = rq_lo < sq_lo ? rq_lo : sq_lo;
		char *hi = rq_hi > sq_hi ? rq_hi : sq_hi;

		if ((size_t)(hi - lo) > (size_t)((rq_hi - rq_lo) +
						 (sq_hi - sq_lo)) + 8192) {
			fprintf(stderr, "RQ %p 与 SQ %p 不相邻,和预期的 mlx5 "
				"布局不同\n", rq_lo, sq_lo);
			return -1;
		}
		if (gpu_map_host(lo, (size_t)(hi - lo),
				 MC_HOST_REGISTER_MAPPED, &d_q) != 0) {
			fprintf(stderr, "映射 RQ+SQ 给 GPU 失败\n");
			return -1;
		}
		d_sq = (char *)d_q + (sq_lo - lo);
		d_rq = (char *)d_q + (rq_lo - lo);
	}
	if (gpu_map_host((void *)h->dvq.dbrec, 64,
			 MC_HOST_REGISTER_MAPPED, &d_dbrec) ||
	    gpu_map_host(h->dvc.buf, (size_t)h->dvc.cqe_cnt * h->dvc.cqe_size,
			 MC_HOST_REGISTER_MAPPED, &d_cq) ||
	    gpu_map_host((void *)h->dvc.dbrec, 64,
			 MC_HOST_REGISTER_MAPPED, &d_cqdb) ||
	    gpu_map_host(h->dvq.bf.reg, h->dvq.bf.size ? h->dvq.bf.size : 4096,
			 MC_HOST_REGISTER_IO_MEMORY | MC_HOST_REGISTER_MAPPED,
			 &d_bf)) {
		fprintf(stderr, "QP %u:映射队列给 GPU 失败\n", q);
		return -1;
	}
	if (v) {
		printf("[映射] RQ/SQ/CQ/dbrec/UAR 均已交给 GPU\n");
	}
	/*
	 * BlueFlame 另一半(-O 0x100000 用):页内偏移和 bf.size 异或。单独映射,
	 * 失败就不轮换,不影响默认路径。
	 */
	ctx->bf_alt = NULL;
	if ((g_opt & GIO_OPT_BFTOG) && h->dvq.bf.size) {
		uintptr_t r = (uintptr_t)h->dvq.bf.reg;
		void *alt = (void *)((r & ~(uintptr_t)4095) |
				     ((r & 4095) ^ h->dvq.bf.size));
		void *d_alt = NULL;

		if (gpu_map_host(alt, h->dvq.bf.size, MC_HOST_REGISTER_IO_MEMORY |
				 MC_HOST_REGISTER_MAPPED, &d_alt) == 0) {
			ctx->bf_alt = (volatile uint64_t *)d_alt;
		}
		printf("[BF] reg=%p size=%u 另一半=%p -> %p%s\n", h->dvq.bf.reg,
		       h->dvq.bf.size, alt, d_alt,
		       ctx->bf_alt ? "" : "(映射失败,不轮换)");
	}

	ctx->sq_buf       = (volatile uint8_t *)d_sq;
	ctx->qp_dbrec     = (volatile uint32_t *)d_dbrec;
	ctx->bf_reg       = (volatile uint64_t *)d_bf;
	ctx->cq_buf       = (volatile uint8_t *)d_cq;
	ctx->cq_dbrec     = (volatile uint32_t *)d_cqdb;
	ctx->sq_wqe_cnt   = h->dvq.sq.wqe_cnt;
	ctx->sq_stride    = h->dvq.sq.stride;
	ctx->cq_cnt       = h->dvc.cqe_cnt;
	ctx->cqe_size     = h->dvc.cqe_size;
	ctx->qpn          = h->qp->qp_num;

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
		volatile uint32_t *qdb = (volatile uint32_t *)h->dvq.dbrec;
		uint32_t head = be32toh(qdb[0]) & 0xffff;
		uint32_t mask = h->dvq.rq.wqe_cnt - 1;
		uint64_t amin = ~0ULL, amax = 0, gap = ~0ULL, prev = 0;
		uint32_t lkey0 = 0, bad = 0, k;
		uint64_t base, end;

		spdk_nvme_ctrlr_get_default_io_qpair_opts(g_ctrlr, &qopts,
							  sizeof(qopts));
		h->rq_posted = qopts.io_queue_size;
		if (h->rq_posted > h->dvq.rq.wqe_cnt) {
			h->rq_posted = h->dvq.rq.wqe_cnt;
		}
		for (k = 0; k < h->rq_posted; k++) {
			uint8_t *seg = (uint8_t *)h->dvq.rq.buf +
				       (size_t)((head - h->rq_posted + k) & mask) *
				       h->dvq.rq.stride;
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
		if (v) {
			printf("[RQ] wqe_cnt=%u stride=%u head(准备前)=%u posted=%u "
			       "(io_queue_size=%u) lkey=0x%x 异常槽=%u\n",
			       h->dvq.rq.wqe_cnt, h->dvq.rq.stride, head,
			       h->rq_posted, qopts.io_queue_size, lkey0, bad);
			printf("[RQ] recv buffer 0x%" PRIx64 " .. 0x%" PRIx64
			       " (跨 %" PRIu64 " 字节, 最小间隔 %" PRIu64 ")\n",
			       amin, amax + 16, amax + 16 - amin,
			       gap == ~0ULL ? 0 : gap);
		}
		if (bad) {
			fprintf(stderr, "QP %u:RQ 槽内容和预期不符(len 应为 16、"
				"lkey 一致),不能继续\n", q);
			return -1;
		}

		h->rsp_lo = amin;
		h->rsp_gap = (gap == ~0ULL) ? 0 : gap;
		base = amin & ~4095ULL;
		end = (amax + 16 + 4095) & ~4095ULL;
		d_rsp = map_recv_buffers(base, end, amin, amax + 16);

		ctx->rq_buf        = (volatile uint8_t *)d_rq;
		ctx->rq_wqe_cnt    = h->dvq.rq.wqe_cnt;
		ctx->rq_stride     = h->dvq.rq.stride;
		ctx->rsp_host_base = base;
		ctx->rsp_len       = d_rsp ? end - base : 0;
		ctx->rsp_dev       = (volatile uint8_t *)d_rsp;
	}
	return 0;
}

/*
 * 接管快照。必须在所有 CPU 侧 I/O 之后拍 —— 预热和涂色都会推进
 * SQ/CQ/RQ。用过期的 pi,kernel 会写进 SPDK 刚用过的 slot,而且 dbrec
 * 已经等于 pi+1,网卡认为没有新工作,命令根本发不出去,表现成
 * "完成 N/N"但 target 侧查无此命令。这个坑踩过两次。
 */
static int
qp_snapshot(struct qp_hw *h, struct gpu_io_ctx *ctx, unsigned q)
{
	volatile uint32_t *qdb = (volatile uint32_t *)h->dvq.dbrec;
	volatile uint32_t *cdb = (volatile uint32_t *)h->dvc.dbrec;
	uint32_t sq_pi = be32toh(qdb[1]) & 0xffff;
	uint32_t cq_ci = be32toh(cdb[0]) & 0xffffff;
	uint32_t rq_head = be32toh(qdb[0]) & 0xffff;

	ctx->sq_pi = (uint16_t)sq_pi;
	ctx->cq_ci = cq_ci;
	ctx->cq_phase = (uint8_t)((cq_ci / h->dvc.cqe_cnt) & 1);
	/*
	 * 已消费的 recv 数 = 已收到的响应数 = 已发的命令数 = sq_pi
	 * (接管时所有 CPU 侧命令都已完成)。不能用 head - io_queue_size:
	 * SPDK 补 recv 是批量延迟 flush 的,最后消费的那个常常还没
	 * post 回去,实际挂着的比 queue size 少一个。
	 */
	ctx->rq_tail = sq_pi & 0xffff;
	ctx->rq_head = rq_head;
	ctx->rq_host_buf = h->dvq.rq.buf;	/* gpu_io_run 拷成显存影子 */
	h->rq_posted = (rq_head - sq_pi) & 0xffff;
	printf("[接管%s%.0u] SQ pi=%u  CQ ci=%u phase=%u  RQ head=%u tail=%u "
	       "(挂着 %u 个 recv)\n", q ? " QP" : "", q, sq_pi, cq_ci,
	       ctx->cq_phase, rq_head, ctx->rq_tail, h->rq_posted);
	if ((g_opt & 0x800) && ctx->njobs > h->rq_posted) {
		fprintf(stderr, "QP %u:%u 条超过挂着的 recv 数 %u"
			"(-O 2048 不补 recv)\n", q, ctx->njobs, h->rq_posted);
		return -1;
	}
	if (!(g_opt & 0x800) && h->dvq.rq.stride % 8) {
		fprintf(stderr, "RQ stride %u 不是 8 的倍数,补 recv 不支持\n",
			h->dvq.rq.stride);
		return -1;
	}
	return 0;
}

/* ================================================================== */

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	/*
	 * 多 QP(-q):hw[q] / ctxs[q] / shs[q] 是第 q 个 QP 的。下文单 QP 时代
	 * 的诊断输出仍用 qp / dvq / dvc / ctx,它们是 QP 0 的别名;sh 是
	 * 各 QP 共享区计数器的合计。
	 */
	static struct qp_hw hw[GIO_MAX_QP];
	static struct gpu_io_ctx ctxs[GIO_MAX_QP];
	static struct gio_shared shs[GIO_MAX_QP];
	struct ibv_qp *qp;
	struct mlx5dv_qp dvq;
	struct mlx5dv_cq dvc;
	struct gpu_io_ctx ctx;
	unsigned long long *cycles, *c_cap, *c_sub, *c_wait, *c_spin, *c_lock;
	unsigned int *c_stat, *c_dw0;
	unsigned long long *c_det, *c_hand, *c_tsreq, *c_tsresp, *c_reqat;
	struct gio_shared sh;
	uint32_t rq_posted = 0;
	uint64_t rsp_lo = 0, rsp_gap = 0;
	double *us;
	double wall_us = 0;
	uint32_t sector, nlb, nblk;
	int err = 0, clk_khz = 0, op;
	unsigned int done = 0, i, q;

	memset(&dvq, 0, sizeof(dvq));
	memset(&dvc, 0, sizeof(dvc));
	memset(&ctx, 0, sizeof(ctx));

	while ((op = getopt(argc, argv, "a:s:n:g:r:b:Q:q:O:WHPVKF:L:S:kED")) != -1) {
		switch (op) {
		case 'a': snprintf(g_traddr, sizeof(g_traddr), "%s", optarg); break;
		case 's': snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_nqn, sizeof(g_nqn), "%s", optarg); break;
		case 'g': g_gpu_id = atoi(optarg); break;
		case 'r': g_rounds = (unsigned)atoi(optarg); break;
		case 'b': g_io_bytes = (unsigned)atoi(optarg); break;
		case 'Q': g_qd = (unsigned)atoi(optarg); break;
		case 'q': g_nq = (unsigned)atoi(optarg); break;
		case 'O': g_opt = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'W': g_write = 1; break;
		case 'H': g_cap_in_hbm = 1; break;
		case 'P': g_paint = 1; break;
		case 'V': g_verify = 1; break;
		case 'K': g_fill_by_kernel = 1; break;
		case 'F': g_cap_flags = (unsigned)strtoul(optarg, NULL, 0); break;
		case 'L': g_start_lba = strtoull(optarg, NULL, 0); break;
		case 'S': g_seed = (uint8_t)strtoul(optarg, NULL, 0); break;
		case 'k': g_kv = 1; break;
		case 'E': g_kv_exist = 1; break;
		case 'D': g_kv_delete = 1; break;
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
	if (g_nq == 0 || g_nq > GIO_MAX_QP) {
		fprintf(stderr, "-q 取 1..%d\n", GIO_MAX_QP);
		return 1;
	}
	if (g_nq > g_rounds) {
		g_nq = g_rounds;
	}
	if (g_qd > g_rounds / g_nq) {
		g_qd = g_rounds / g_nq;
	}
	if ((g_opt & (GIO_OPT_BF64 | GIO_OPT_BFTOG | GIO_OPT_BFDLY |
		      GIO_OPT_DB8)) &&
	    !(g_opt & (0x8000 | GIO_OPT_BF1))) {
		fprintf(stderr, "-O 0x80000/0x100000/0x200000/0x800000 要配 0x8000 或 0x40000\n");
		return 1;
	}
	if (g_nq > 1 && (g_opt & 0x200)) {
		fprintf(stderr, "-O 512(胶囊槽轮换)不支持多 QP\n");
		return 1;
	}
	/* 0x7000 默认开;带 0x10000 时保持 -O 原样,用来对照旧行为 */
	if (g_opt & GIO_OPT_LEGACY) {
		g_opt &= ~GIO_OPT_LEGACY;
	} else {
		g_opt |= GIO_OPT_DEFAULT;
	}
	if ((g_opt & 0x20) && g_qd != 1) {
		fprintf(stderr, "-O 32(串行快路径)只能配 -Q 1\n");
		return 1;
	}
	if ((g_opt & 0x400) && g_qd != 1) {
		fprintf(stderr, "-O 1024(旧式 cid)只能配 -Q 1\n");
		return 1;
	}
	if ((g_opt & 0x100) && (!(g_opt & 0x20) || g_qd != 1)) {
		fprintf(stderr, "-O 256(精简提交)要配 -O 32 且 -Q 1\n");
		return 1;
	}
	if ((g_opt & 0x40) && !(g_opt & 0x30)) {
		fprintf(stderr, "-O 64 必须配 16 或 32,否则没人收割 CQ\n");
		return 1;
	}
	if ((g_kv_exist || g_kv_delete) && !g_kv) {
		fprintf(stderr, "-E / -D 只用于 KV 模式(-k)\n");
		return 1;
	}
	if (g_kv && (g_fill_by_kernel ||
		     g_write + g_kv_exist + g_kv_delete > 1)) {
		fprintf(stderr, "-k 不能配 -K;-W/-E/-D 只能选一个\n");
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

	/* KV 的 value 是字节粒度,不受扇区约束;nlb 只给读写模式用 */
	if (!g_kv && g_io_bytes % sector) {
		fprintf(stderr, "-b %u 不是扇区 %u 的整数倍\n", g_io_bytes, sector);
		return 1;
	}
	if (g_kv) {
		nlb = 1;	/* KV 的 "LBA" 就是 key 序号,report_blocks 按步长 1 */
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
		unsigned absent = 0, shown_present = 0;	/* -k -V -D */

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
		for (i = 0; i < g_rounds && g_kv; i++) {
			/*
			 * KV:涂色 = Delete(key 不存在本来就算成功,EDNEK 默认 0);
			 * 回读 = Retrieve,不存在的 key 按 0xCC 记,块图上是 C。
			 */
			uint64_t idx = g_start_lba + i;
			uint32_t dw0 = 0;
			int st;

			if (g_paint) {
				st = cpu_kv(NVME_KV_OPC_DELETE, NULL, 0, idx, NULL);
				nfail += st != 0 && st != NVME_KV_SC_KEY_NOT_EXIST;
				continue;
			}
			st = cpu_kv(NVME_KV_OPC_RETRIEVE, b, g_io_bytes, idx, &dw0);
			if (g_kv_delete) {
				/* -V -D:期望不存在,只看状态,不比内容 */
				if (st == NVME_KV_SC_KEY_NOT_EXIST) {
					absent++;
				} else if (st != 0) {
					nfail++;
				} else if (shown_present++ < 8) {
					printf("  key %" PRIu64 " 仍存在 (value %u 字节)\n",
					       idx, dw0);
				}
				continue;
			}
			if (st == NVME_KV_SC_KEY_NOT_EXIST) {
				memset(b, CPAT, g_io_bytes);
			} else if (st != 0) {
				nfail++;
				memset(b, 0, g_io_bytes);
			} else if (dw0 != g_io_bytes) {
				printf("  key %" PRIu64 ": value 长度 %u,期望 %u\n",
				       idx, dw0, g_io_bytes);
				bad++;
			}
			memcpy(all + (size_t)i * g_io_bytes, b, g_io_bytes);
		}
		for (i = 0; i < g_rounds && !g_kv; i++) {
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
		if (g_paint && g_kv) {
			printf("[删除] key %" PRIu64 " 起 %u 个 (seed %u),失败 %u\n",
			       g_start_lba, g_rounds, g_seed, nfail);
		} else if (g_kv && g_kv_delete) {
			bad = g_rounds - absent - nfail;
			printf("\n[回读] key %" PRIu64 " 起 %u 个 (seed %u),期望全部不存在:\n"
			       "  不存在 %u,仍存在 %u,出错 %u\n  %s\n",
			       g_start_lba, g_rounds, g_seed, absent, bad, nfail,
			       (bad || nfail) ? "*** FAIL ***" :
			       "*** PASS:全部已删除 ***");
			free(all);
		} else if (g_paint) {
			printf("[涂色] LBA %" PRIu64 " 起 %u 块填 0x%02x,失败 %u\n",
			       g_start_lba, g_rounds, CPAT, nfail);
		} else {
			if (nfail) {
				printf("[回读] %u 块 I/O 失败(按全零计)\n", nfail);
			}
			bad += report_blocks("回读", all, g_rounds, g_io_bytes,
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
	if (g_kv) {
		/*
		 * KV namespace 上发 NVM READ 会被当成 Retrieve(同为 02h),
		 * CDW11 的 KL=0 会被拒。改发一条真 Retrieve,key 取一个本轮
		 * 用不到的序号:要的只是 SPDK 为显存建 SGL 时触发 hook 注册 MR,
		 * 回 87h(不存在)正常。回 01h 说明 target 不认 KV。
		 */
		int st = cpu_kv(NVME_KV_OPC_RETRIEVE, g_data_gpu, g_io_bytes,
				~0ULL, NULL);

		if (st != 0 && st != NVME_KV_SC_KEY_NOT_EXIST) {
			fprintf(stderr, "KV 预热返回 SCT/SC=0x%03x%s\n", st,
				st == 0x01 ? " (Invalid Opcode:namespace 不是 KV,"
					     "检查 target,见 tst/kv_target)" : "");
			return 1;
		}
	} else if (cpu_io(0, g_data_gpu, 0, nlb) != 0) {
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

	/*
	 * ---- 把队列交给 GPU ----
	 * 先填各 QP 共用的字段(胶囊、数据区、命令),再逐个 QP 填硬件相关的。
	 * QP 0 就是上面预热用的 g_qpair;其余 QP 现在才建,CPU 侧一条 I/O
	 * 都不走它们。胶囊区按 QP 切开,每个 QP 64 KiB(1024 个槽)。
	 */
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
	if (g_kv) {
		ctx.kv       = 1;
		ctx.kv_klen  = KV_KLEN;
		ctx.kv_seed  = g_seed;
		ctx.kv_opts  = 0;
		ctx.nvme_opc = g_write ? NVME_KV_OPC_STORE :
			       g_kv_exist ? NVME_KV_OPC_EXIST :
			       g_kv_delete ? NVME_KV_OPC_DELETE :
			       NVME_KV_OPC_RETRIEVE;
	}

	hw[0].qpair = g_qpair;
	for (q = 1; q < g_nq; q++) {
		hw[q].qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
		if (!hw[q].qpair) {
			fprintf(stderr, "第 %u 个 qpair 建不出来(target 的 "
				"max_qpairs_per_ctrlr?)\n", q);
			return 1;
		}
	}
	for (q = 0; q < g_nq; q++) {
		ctxs[q] = ctx;
		ctxs[q].capsule      += (size_t)q * CAP_QP_STRIDE;
		ctxs[q].capsule_addr += (uint64_t)q * CAP_QP_STRIDE;
		if (qp_attach(&hw[q], &ctxs[q], q) != 0) {
			return 1;
		}
	}
	qp = hw[0].qp;
	dvq = hw[0].dvq;
	dvc = hw[0].dvc;
	rq_posted = hw[0].rq_posted;
	rsp_lo = hw[0].rsp_lo;
	rsp_gap = hw[0].rsp_gap;

	/*
	 * QD 仍受 queue size 限制:同时在飞的命令数不能超过 target 端的
	 * 队列深度,也不能超过挂着的 recv 数。总条数(-r)从 C2 起不再受限 ——
	 * kernel 收一个响应就补一个 recv(-O 2048 关掉时才受限)。
	 */
	if (rq_posted < 2) {
		fprintf(stderr, "已 post 的 recv 数 %u 异常\n", rq_posted);
		return 1;
	}
	if (g_qd >= rq_posted) {
		g_qd = rq_posted - 1;
		printf("[QD] 超过 queue size,降为 %u\n", g_qd);
	}
	/*
	 * 批量补 recv 期间在飞的 recv 最少少 GIO_RQ_BATCH 个;CQ ci 懒更新时
	 * 网卡眼里的未消费 CQE 最多多 GIO_CQ_BATCH 个,每条 I/O 最多 2 个 CQE。
	 * 这两种情况都要再压 QD,否则响应无处安放 / CQ 溢出。
	 */
	if ((g_opt & 0x1000) && g_qd > rq_posted - 1 - GIO_RQ_BATCH) {
		g_qd = rq_posted - 1 - GIO_RQ_BATCH;
		printf("[QD] RQ 批量补(0x1000)要留余量,降为 %u\n", g_qd);
	}
	if ((g_opt & 0x2000) &&
	    2 * g_qd + GIO_CQ_BATCH >= dvc.cqe_cnt) {
		g_qd = (dvc.cqe_cnt - GIO_CQ_BATCH) / 2 - 1;
		printf("[QD] CQ ci 懒更新(0x2000)要留余量,降为 %u\n", g_qd);
	}
	/*
	 * 总条数按 QP 切成连续段。GPU 上同时常驻 QP 数 × (QD + 1) 个 block,
	 * 全部在自旋 —— 放不下时没被调度的 poller 会让同 QP 的 submitter 超时。
	 */
	for (q = 0; q < g_nq; q++) {
		uint32_t lo = (uint32_t)((uint64_t)g_rounds * q / g_nq);
		uint32_t hi = (uint32_t)((uint64_t)g_rounds * (q + 1) / g_nq);

		ctxs[q].qd = g_qd;
		ctxs[q].opt = g_opt;
		ctxs[q].job_base = lo;
		ctxs[q].njobs = hi - lo;
	}
	ctx = ctxs[0];

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
			/* KV:Retrieve/Exist/Delete 都要求 key 先存在 */
			nfail += g_kv ?
				 cpu_kv(NVME_KV_OPC_STORE, b, g_io_bytes, lba,
					NULL) != 0 :
				 cpu_io(1, b, lba, nlb) != 0;
		}
		spdk_dma_free(b);
		printf("[准备] 显存 %u 块预填 0x%02x;%s %u 块写入 pattern"
		       " (seed %u),失败 %u\n", nblk, RPAT,
		       g_kv ? "target 上 Store 了" : "盘上", nblk, g_seed, nfail);
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

	if (g_write && g_kv) {
		/* KV 的"涂色":先删掉这批 key,-k -V 时没写到的显示为 C */
		unsigned nfail = 0;
		int st;

		for (i = 0; i < nblk; i++) {
			st = cpu_kv(NVME_KV_OPC_DELETE, NULL, 0,
				    g_start_lba + i, NULL);
			nfail += st != 0 && st != NVME_KV_SC_KEY_NOT_EXIST;
		}
		printf("[删除] key %" PRIu64 " 起 %u 个已删除,失败 %u\n",
		       g_start_lba, nblk, nfail);
	} else if (g_write) {
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

	/* 快照必须在所有 CPU 侧 I/O 之后拍,见 qp_snapshot */
	for (q = 0; q < g_nq; q++) {
		if (qp_snapshot(&hw[q], &ctxs[q], q) != 0) {
			return 1;
		}
	}
	ctx = ctxs[0];
	rq_posted = hw[0].rq_posted;

	/* ---- 跑 ---- */
	cycles = calloc(g_rounds, sizeof(*cycles));
	c_cap  = calloc(g_rounds, sizeof(*c_cap));
	c_sub  = calloc(g_rounds, sizeof(*c_sub));
	c_wait = calloc(g_rounds, sizeof(*c_wait));
	c_spin = calloc(g_rounds, sizeof(*c_spin));
	c_lock = calloc(g_rounds, sizeof(*c_lock));
	c_stat = calloc(g_rounds, sizeof(*c_stat));
	c_det  = calloc(g_rounds, sizeof(*c_det));
	c_hand = calloc(g_rounds, sizeof(*c_hand));
	c_tsreq  = calloc(g_rounds, sizeof(*c_tsreq));
	c_tsresp = calloc(g_rounds, sizeof(*c_tsresp));
	c_reqat  = calloc(g_rounds, sizeof(*c_reqat));
	c_dw0    = calloc(g_rounds, sizeof(*c_dw0));
	us = calloc(g_rounds, sizeof(*us));
	printf("\n[运行] %s %u 条, 每条 %u 字节, QD %u%s, %s, %s, -O 0x%x\n",
	       g_kv ? kv_opc_name(ctx.nvme_opc) : g_write ? "WRITE" : "READ",
	       g_rounds, g_io_bytes, g_qd, qp_desc(g_nq),
	       ctx.data_step ? "每条一块" : "单块复用",
	       (g_opt & 0x800) ? "不补 recv" : "补 recv", g_opt);

	if (gpu_io_run(ctxs, g_nq, g_start_lba, c_cap, c_sub, c_wait, c_spin,
		       c_lock, c_stat, c_det, c_hand, c_tsreq, c_tsresp,
		       c_reqat, &wall_us, c_dw0, shs) != 0) {
		fprintf(stderr, "kernel 执行失败\n");
		return 1;
	}
	sh_sum(&sh, shs, g_nq);

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
			       g_kv ? kv_sc_name(sh.first_status) :
			       ((sh.first_status >> 1) & 0xff) == 0x02 &&
			       ((sh.first_status >> 9) & 7) == 0 ?
			       " Invalid Field —— 常见于超过 max_io_size" : "");
		}
		/* Retrieve 的 DW0 是 value 实际长度,应正好等于 -b */
		if (g_kv && ctx.nvme_opc == NVME_KV_OPC_RETRIEVE) {
			unsigned nbad = 0, first = 0;

			for (i = 0; i < g_rounds; i++) {
				if (c_stat[i] == 0 && c_dw0[i] != g_io_bytes) {
					if (nbad++ == 0) {
						first = i;
					}
				}
			}
			printf("\n[KV] Retrieve DW0(value 长度)不等于 %u 的: %u",
			       g_io_bytes, nbad);
			if (nbad) {
				printf(" (首个第 %u 条 = %u)", first, c_dw0[first]);
				if (!err) {
					err = -5;
				}
			}
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
	 * 实测 GPU 时钟。标称值是最大频率,实际频率随负载变 —— 用标称值
	 * 换算微秒会系统性偏(两个二进制之间实测差过 1.67 倍,足以把
	 * "谁更快"判反)。之后所有微秒换算都用实测值。
	 */
	if (sh.k_end > sh.k_start && wall_us > 0) {
		int khz = (int)((sh.k_end - sh.k_start) / wall_us * 1000.0);

		printf("[GPU 时钟] 标称 %d kHz, 实测 %d kHz (kernel %llu cycles / "
		       "%.0f us)\n", clk_khz, khz,
		       (unsigned long long)(sh.k_end - sh.k_start), wall_us);
		if (khz > 100000 && khz < 4000000) {
			clk_khz = khz;
		} else {
			printf("[GPU 时钟] 实测值不合理,仍用标称值\n");
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
	for (q = 0; q < g_nq; q++) {
		volatile uint32_t *qdb = (volatile uint32_t *)hw[q].dvq.dbrec;
		volatile uint32_t *cdb = (volatile uint32_t *)hw[q].dvc.dbrec;
		const struct gpu_io_ctx *c = &ctxs[q];
		const struct gio_shared *x = &shs[q];
		uint32_t sq_now = be32toh(qdb[1]) & 0xffff;
		uint32_t rq_now = be32toh(qdb[0]) & 0xffff;
		uint32_t cq_now = be32toh(cdb[0]) & 0xffffff;

		printf("[dbrec%s%.0u] SQ %u -> %u (+%u, 期望 +%u)   CQ %u -> %u (+%u)"
		       "   RQ %u -> %u (+%u, 期望 +%u)\n", q ? " QP" : "", q,
		       c->sq_pi, sq_now, (sq_now - c->sq_pi) & 0xffff, c->njobs,
		       c->cq_ci, cq_now, cq_now - c->cq_ci,
		       c->rq_head, rq_now, (rq_now - c->rq_head) & 0xffff,
		       (g_opt & 0x800) ? 0 : x->cqe_resp);
		if (((sq_now - c->sq_pi) & 0xffff) != (c->njobs & 0xffff)) {
			printf("  *** SQ 增量不等于轮数:kernel 的命令没发出去 ***\n");
		}
		if (!(g_opt & 0x800) &&
		    (((rq_now - c->rq_head) & 0xffff) != (x->cqe_resp & 0xffff) ||
		     x->rq_reposted != x->cqe_resp)) {
			printf("  *** RQ 补充数(%u)和响应数(%u)对不上 ***\n",
			       x->rq_reposted, x->cqe_resp);
		}
		if (cq_now != c->cq_ci + x->cqe_req + x->cqe_resp) {
			printf("  *** CQ dbrec 没补齐:%u,期望 %u ***\n", cq_now,
			       c->cq_ci + x->cqe_req + x->cqe_resp);
		}
	}
	printf("[dbrec] 推 RQ dbrec %u 次,写 CQ dbrec %u 次;SEND 要了 CQE 的 %u 条\n",
	       sh.rq_flushes, sh.cq_db_writes, sh.cqe_req);

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
		printf("\n[胶囊] 64 字节:\n  ");
		for (k = 0; k < 64; k++) {
			printf("%02x ", cap[k]);
			if (k % 16 == 15) {
				printf("\n  ");
			}
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

		/*
		 * 等待拆分:门铃 → poller 发现(poller 时钟减 submitter 时钟),
		 * poller 置完成 → submitter 看到(反过来)。两者之和应接近"等待"。
		 * 时钟若不能跨 block 比较,这里会出现离谱的值。
		 */
		{
			double *dv = calloc(done, sizeof(*dv));
			double *hv = calloc(done, sizeof(*hv));
			unsigned m = 0, weird = 0;
			double ps = clk_khz / 1000.0;

			for (i = 0; i < g_rounds; i++) {
				if (c_stat[i] == 0xffffffffu) {
					continue;
				}
				if (c_det[i] > (1ULL << 40) ||
				    c_hand[i] > (1ULL << 40)) {
					weird++;
					continue;
				}
				dv[m] = c_det[i] / ps;
				hv[m] = c_hand[i] / ps;
				m++;
			}
			if (g_opt & 0x100) {
				m = 0;		/* 精简提交不写 t_post,拆分无意义 */
			}
			if (m) {
				qsort(dv, m, sizeof(double), cmp_d);
				qsort(hv, m, sizeof(double), cmp_d);
				printf("\n  等待拆分(us, p50/p90): 门铃→poller 发现 "
				       "%.1f/%.1f   poller 置完成→submitter 看到 "
				       "%.1f/%.1f", dv[m / 2],
				       dv[(unsigned)(m * 0.9)], hv[m / 2],
				       hv[(unsigned)(m * 0.9)]);
			}
			if (weird) {
				printf("\n  等待拆分:%u 条值离谱 —— 时钟可能不能跨 "
				       "block 比较", weird);
			}
			free(dv);
			free(hv);
		}
		/*
		 * 网卡侧时间戳(-O 128)。tick 频率未知,用相邻两条 I/O 的 SEND
		 * 完成时间戳之差 <-> GPU 测到的单条总耗时来自校准。
		 *
		 * 这是区分"响应回来得晚"和"我们发现得晚"的关键:
		 *   网卡侧 SEND→响应 ≈ 5 us,而 GPU 门铃→发现 ≈ 15 us
		 *     -> 响应早就到了,是检测下限
		 *   两者都 ≈ 15 us -> 响应确实回来得晚
		 */
		if (g_opt & 0x80) {
			double ticks = 0, gpu_ns = 0;
			double *rr = calloc(done ? done : 1, sizeof(*rr));
			double *ra = calloc(done ? done : 1, sizeof(*ra));
			unsigned m = 0, n2 = 0;

			for (i = 0; i + 1 < g_rounds; i++) {
				if (c_stat[i] == 0xffffffffu ||
				    c_stat[i + 1] == 0xffffffffu ||
				    !c_tsreq[i] || !c_tsreq[i + 1] ||
				    c_tsreq[i + 1] <= c_tsreq[i]) {
					continue;
				}
				ticks += c_tsreq[i + 1] - c_tsreq[i];
				gpu_ns += (c_cap[i + 1] + c_sub[i + 1] +
					   c_wait[i + 1]) * 1e6 / clk_khz;
				n2++;
			}
			for (i = 0; i < g_rounds; i++) {
				if (c_stat[i] == 0xffffffffu || !c_tsreq[i] ||
				    c_tsresp[i] <= c_tsreq[i]) {
					continue;
				}
				rr[m] = (double)(c_tsresp[i] - c_tsreq[i]);
				ra[m] = c_reqat[i] * 1e6 / clk_khz / 1000.0;
				m++;
			}
			if (n2 && m) {
				double ns_tick = gpu_ns / ticks;

				qsort(rr, m, sizeof(double), cmp_d);
				qsort(ra, m, sizeof(double), cmp_d);
				printf("\n  网卡时间戳: 1 tick = %.2f ns(自校准 %u 对)"
				       "\n    网卡侧 SEND 完成→响应到达 p50 %.1f us"
				       "\n    GPU 门铃→发现 SEND 完成   p50 %.1f us",
				       ns_tick, n2, rr[m / 2] * ns_tick / 1000.0,
				       ra[m / 2]);
			} else {
				printf("\n  网卡时间戳: 取不到有效样本(可能没开 rt 时钟)");
			}
			free(rr);
			free(ra);
		}
		if (g_opt & 0x30) {
			printf("\n  自轮询: 收割 %llu 次, 命中 %llu, 处理一条响应 %.0f ns",
			       (unsigned long long)sh.poll_spins,
			       (unsigned long long)sh.poll_hits,
			       sh.cqe_resp ? sh.resp_cycles * 1e6 / clk_khz /
					     sh.cqe_resp : 0.0);
		} else if (sh.poll_spins) {
			printf("\n  poller: 每圈 %.0f ns, 处理一条响应 %.0f ns",
			       sh.poll_cycles * 1e6 / clk_khz / sh.poll_spins,
			       sh.cqe_resp ? sh.resp_cycles * 1e6 / clk_khz /
					     sh.cqe_resp : 0.0);
		}
			printf("\n  等待圈数: mean %llu  min %llu  max %llu",
			       ssum / done, smin, smax);
			if (ssum) {
				printf("   平均每圈 %.0f ns",
				       wsum * 1000.0 / (double)ssum);
			}
			printf("\n");
		}

		/*
		 * -O 0x20000 剖析:提交路径每条的平均分段,poller 空转一圈 /
		 * 命中一次的平均分段。都是均值(含少数长尾),单位 ns。
		 */
		if (g_opt & GIO_OPT_PROF) {
			double ns = 1e6 / clk_khz;
			double nj = done ? done : 1;
			double ni = sh.prof[GIO_PF_NIDLE] ? sh.prof[GIO_PF_NIDLE] : 1;
			double nh = sh.prof[GIO_PF_NHIT] ? sh.prof[GIO_PF_NHIT] : 1;

			printf("\n  [剖析] 提交每条(ns): 写 tag %.0f | 抢锁 %.0f | 读 sq_pi %.0f"
			       " | 写 WQE+fence %.0f | dbrec+fence %.0f | 回读+BF+fence %.0f"
			       " | 写 sq_pi+解锁 %.0f | 写 t_post+fence %.0f",
			       sh.prof[GIO_PF_TAG] * ns / nj,
			       sh.prof[GIO_PF_LOCK] * ns / nj,
			       sh.prof[GIO_PF_PI] * ns / nj,
			       sh.prof[GIO_PF_WQE] * ns / nj,
			       sh.prof[GIO_PF_DBR] * ns / nj,
			       sh.prof[GIO_PF_BF] * ns / nj,
			       sh.prof[GIO_PF_REL] * ns / nj,
			       sh.prof[GIO_PF_POST] * ns / nj);
			if (!(g_opt & 0x30)) {
				printf("\n  [剖析] poller 空转 %llu 圈,每圈(ns): 读 CQE %.0f"
				       " | idle_flush %.0f | done/abort 检查 %.0f",
				       (unsigned long long)sh.prof[GIO_PF_NIDLE],
				       sh.prof[GIO_PF_IREAD] * ns / (ni + nh),
				       sh.prof[GIO_PF_IFLUSH] * ns / ni,
				       sh.prof[GIO_PF_ICHK] * ns / ni);
				printf("\n  [剖析] poller 命中 %llu 次,每次(ns): 读 CQE %.0f"
				       " | fence %.0f | consume_one %.0f | advance_ci %.0f",
				       (unsigned long long)sh.prof[GIO_PF_NHIT],
				       sh.prof[GIO_PF_IREAD] * ns / (ni + nh),
				       sh.prof[GIO_PF_HFENCE] * ns / nh,
				       sh.prof[GIO_PF_HCONS] * ns / nh,
				       sh.prof[GIO_PF_HCI] * ns / nh);
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
	if (g_kv && (g_write || g_kv_delete)) {
		/* 同读写:接管过的 qpair 不能再用 CPU 路径,另起进程(坑 3) */
		printf("\n[校验] 请另起进程回读,%s:\n",
		       g_kv_delete ? "应全部不存在" : "应逐块一致");
		printf("  %s -a %s -n %s -g %d -b %u -r %u -L %" PRIu64
		       " -S %u -k -V%s\n", argv[0], g_traddr, g_nqn, g_gpu_id,
		       g_io_bytes, done, g_start_lba, g_seed,
		       g_kv_delete ? " -D" : "");
	} else if (g_kv && g_kv_exist) {
		printf("\n[校验] Exist 只看状态:NVMe 成功 == 条数即 PASS\n");
	} else if (g_write) {
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

	for (q = 0; q < g_nq; q++) {
		spdk_nvme_ctrlr_free_io_qpair(hw[q].qpair);
	}
	spdk_nvme_detach(g_ctrlr);
	return (err || done != g_rounds) ? 2 : 0;
}
