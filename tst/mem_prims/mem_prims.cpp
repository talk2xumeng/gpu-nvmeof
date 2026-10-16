/*
 * mem_prims.cpp  --  MACA 访存原语和 fence 的单次代价
 *
 * 背景见 README.md。一句话:我们 kernel 用 volatile + __threadfence_system,
 * DeepEP / mxshmem 的 MACA 分支用 __builtin_nontemporal_load/store +
 * __builtin_mxc_fence(..., "device"/"block")。剖析里显存上每次访问 ~0.5 µs、
 * 一条 I/O 十来次 fence,这里把每种写法单独量出来,决定 kernel 换不换。
 *
 * 两组测量,都是单线程,clock64 计时,换算用实测时钟:
 *
 *   单线程   每种原语 / 组合循环 n 次,取每次平均。读用依赖链
 *            (下一次地址取决于上一次读到的值),量的是时延不是吞吐
 *   乒乓     两个 block 各一个线程,轮流写对方要等的 flag,
 *            量跨 block 交接(poller → submitter 那一段)的往返时延;
 *            顺带验证 nontemporal / 不带 fence 的写法能不能看见对方的写
 *
 * 内存类型(-m,可多个):
 *   n  mcMalloc
 *   f  mcExtMallocWithFlags(1) Finegrained —— -X 队列、-H 胶囊用的就是它
 *   u  mcExtMallocWithFlags(4) MapPcieDefault
 *   h  主存,mcHostRegister(Mapped) —— 主存方案的 CQ/SQ/胶囊
 *
 * 用法:
 *   ./mem_prims -g 4 [-m nfuh] [-n 20000] [-r 5]
 */

#include <mcr/mc_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MP_BUF_BYTES	(64 * 1024)
#define MP_SPIN_MAX	(1ull << 22)	/* 乒乓一次等待的圈数上限(秒级),超了算看不见 */
#define MP_MAX_RUNS	15

/* ---- 原语:和 DeepEP utils.cuh 的 MACA 分支一一对应 ---- */

#define LD_V64(p)	(*(volatile uint64_t *)(p))
#define LD_NT64(p)	__builtin_nontemporal_load((uint64_t *)(p))
#define ST_V64(p, v)	(*(volatile uint64_t *)(p) = (uint64_t)(v))
#define ST_NT64(p, v)	__builtin_nontemporal_store((uint64_t)(v), (uint64_t *)(p))
#define ST_V32(p, v)	(*(volatile uint32_t *)(p) = (uint32_t)(v))
#define ST_NT32(p, v)	__builtin_nontemporal_store((uint32_t)(v), (uint32_t *)(p))

#define F_SYS()		__threadfence_system()
#define F_DEV()		__threadfence()
#define F_BLK()		__threadfence_block()
#define F_MXD()		__builtin_mxc_fence(__ATOMIC_ACQ_REL, "device")
#define F_MXDR()	__builtin_mxc_fence(__ATOMIC_RELEASE, "device")
#define F_MXDA()	__builtin_mxc_fence(__ATOMIC_ACQUIRE, "device")
#define F_MXB()		__builtin_mxc_fence(__ATOMIC_ACQ_REL, "block")
/* 只挡编译器:不带 fence 的 nontemporal 轮询不能被提到循环外 */
#define CBAR()		__atomic_signal_fence(__ATOMIC_SEQ_CST)

/* ================================================================== */
/* 单线程                                                              */
/* ================================================================== */

enum {
	T_LD_PLAIN,
	T_LD_V,
	T_LD_NT,
	T_LD_FSYS_V,
	T_LD_FDEV_V,
	T_LD_NT_MXDA,
	T_LD4_V,
	T_LD4_NT,
	T_ST_V,
	T_ST_NT,
	T_ST_V_FSYS,
	T_ST_V_FDEV,
	T_ST_V_FBLK,
	T_ST_MXDR_NT,
	T_ST_NT_MXD,
	T_ST_NT_MXB,
	T_F_SYS,
	T_F_DEV,
	T_F_BLK,
	T_F_MXD,
	T_F_MXB,
	T_LOCK_OURS,
	T_LOCK_EP,
	T_SUB_OURS,
	T_SUB_EP,
	T_HIT_OURS,
	T_HIT_EP,
	T_NUM
};

static const char *t_name[T_NUM] = {
	"读 plain(可缓存)",
	"读 volatile",
	"读 nontemporal",
	"读 sys fence+volatile   [我们 ld_acq32]",
	"读 dev fence+volatile   [我们 0x4000000]",
	"读 nt+mxc acquire dev   [DeepEP ld_acquire]",
	"读 4 个互不依赖 volatile(每组)",
	"读 4 个互不依赖 nt(每组)",
	"写 volatile(只发出)",
	"写 nontemporal(只发出)",
	"写 volatile+sys fence   [我们 st_rel32]",
	"写 volatile+dev fence",
	"写 volatile+block fence",
	"写 mxc release dev+nt   [DeepEP st_na_release]",
	"写 nt+mxc acq_rel dev",
	"写 nt+mxc acq_rel block",
	"fence __threadfence_system",
	"fence __threadfence",
	"fence __threadfence_block",
	"fence mxc acq_rel device",
	"fence mxc acq_rel block",
	"锁 CAS+释放             [我们 sq_lock]",
	"锁 CAS+释放             [DeepEP]",
	"提交 WQE+dbrec          [我们 fence_q 默认]",
	"提交 WQE+dbrec          [DeepEP]",
	"CQE 命中后读字段        [我们]",
	"CQE 命中后读字段        [mxshmem]",
};

/*
 * mask 由 host 传 0:地址恒为 p,但编译器不知道,依赖链和每次访问都保得住。
 * lk 是锁字(32 位),和 p 分开放,避免和数据同一条线。
 */
__global__ void
k_single(int t, uint64_t *p, uint32_t *lk, uint64_t mask, uint32_t n,
	 uint64_t *out)
{
	uint64_t v = 0, t0, t1;
	uint32_t i;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}

#define LOOP(body)						\
	do {							\
		t0 = clock64();					\
		for (i = 0; i < n; i++) {			\
			body;					\
		}						\
		t1 = clock64();					\
	} while (0)

	switch (t) {
	case T_LD_PLAIN:
		LOOP(v = p[v & mask]);
		break;
	case T_LD_V:
		LOOP(v = LD_V64(p + (v & mask)));
		break;
	case T_LD_NT:
		LOOP(v = LD_NT64(p + (v & mask)));
		break;
	case T_LD_FSYS_V:
		LOOP(F_SYS(); v = LD_V64(p + (v & mask)));
		break;
	case T_LD_FDEV_V:
		LOOP(F_DEV(); v = LD_V64(p + (v & mask)));
		break;
	case T_LD_NT_MXDA:
		LOOP(v = LD_NT64(p + (v & mask)); F_MXDA());
		break;
	case T_LD4_V:
		/*
		 * 一组 4 个读彼此不依赖(只依赖上一组),看硬件能不能并发发出。
		 * consume_one 读 CQE 各字段就是这种形态。
		 */
		LOOP(uint64_t m = v & mask;
		     uint64_t a0 = LD_V64(p + 8 + m);
		     uint64_t a1 = LD_V64(p + 9 + m);
		     uint64_t a2 = LD_V64(p + 10 + m);
		     uint64_t a3 = LD_V64(p + 11 + m);
		     v = a0 + a1 + a2 + a3);
		break;
	case T_LD4_NT:
		LOOP(uint64_t m = v & mask;
		     uint64_t a0 = LD_NT64(p + 8 + m);
		     uint64_t a1 = LD_NT64(p + 9 + m);
		     uint64_t a2 = LD_NT64(p + 10 + m);
		     uint64_t a3 = LD_NT64(p + 11 + m);
		     v = a0 + a1 + a2 + a3);
		break;
	case T_ST_V:
		LOOP(ST_V64(p + (i & mask), i));
		break;
	case T_ST_NT:
		LOOP(ST_NT64(p + (i & mask), i));
		break;
	case T_ST_V_FSYS:
		LOOP(ST_V64(p + (i & mask), i); F_SYS());
		break;
	case T_ST_V_FDEV:
		LOOP(ST_V64(p + (i & mask), i); F_DEV());
		break;
	case T_ST_V_FBLK:
		LOOP(ST_V64(p + (i & mask), i); F_BLK());
		break;
	case T_ST_MXDR_NT:
		LOOP(F_MXDR(); ST_NT64(p + (i & mask), i));
		break;
	case T_ST_NT_MXD:
		LOOP(ST_NT64(p + (i & mask), i); F_MXD());
		break;
	case T_ST_NT_MXB:
		LOOP(ST_NT64(p + (i & mask), i); F_MXB());
		break;
	case T_F_SYS:
		LOOP(F_SYS());
		break;
	case T_F_DEV:
		LOOP(F_DEV());
		break;
	case T_F_BLK:
		LOOP(F_BLK());
		break;
	case T_F_MXD:
		LOOP(F_MXD());
		break;
	case T_F_MXB:
		LOOP(F_MXB());
		break;
	case T_LOCK_OURS:
		/* gpu_io_kernel.cpp sq_lock / sq_unlock(st_rel32) */
		LOOP(while (atomicCAS(lk, 0u, 1u) != 0u) {}
		     ST_V32(lk, 0u); F_SYS());
		break;
	case T_LOCK_EP:
		/* ep_ibgda_device_common.h ibgda_lock_acquire / release */
		LOOP(while (atomicCAS(lk, 0u, 1u) != 0u) {}
		     F_MXB(); F_MXB(); ST_NT32(lk, 0u));
		break;
	case T_SUB_OURS:
		/*
		 * post_wqe 寄存器构造路径(不含门铃 MMIO):4 个 8 字节 volatile
		 * 写 WQE,fence_q,写 dbrec,fence_q。fence_q 默认 system。
		 */
		LOOP(ST_V64(p + 8 + (i & mask), i); ST_V64(p + 9, i);
		     ST_V64(p + 10, i); ST_V64(p + 11, i); F_SYS();
		     ST_V32((uint32_t *)(p + 16), i); F_SYS());
		break;
	case T_SUB_EP:
		/*
		 * ep_ibgda_device_mlx.h:WQE 用 st_na_relaxed,submit_requests 里
		 * __threadfence,update_dbr WRITE_ONCE,ring_db 的 st_na_release
		 * 先 mxc release fence(门铃那次写本身不计)
		 */
		LOOP(ST_NT64(p + 8 + (i & mask), i); ST_NT64(p + 9, i);
		     ST_NT64(p + 10, i); ST_NT64(p + 11, i); F_DEV();
		     ST_V32((uint32_t *)(p + 16), i); F_MXDR());
		break;
	case T_HIT_OURS:
		/* poller:读 CQE 尾 8 字节,命中后 fence_q,再读 3 个 8 字节字段 */
		LOOP(v = LD_V64(p + 8 + (v & mask)); F_SYS();
		     v += LD_V64(p + 9 + (v & mask));
		     v += LD_V64(p + 10 + (v & mask));
		     v += LD_V64(p + 11 + (v & mask)));
		break;
	case T_HIT_EP:
		/* mxshmem ibgda_poll_cq:读 op_own,IBGDA_MFENCE(block),再读字段 */
		LOOP(v = LD_NT64(p + 8 + (v & mask)); F_BLK();
		     v += LD_NT64(p + 9 + (v & mask));
		     v += LD_NT64(p + 10 + (v & mask));
		     v += LD_NT64(p + 11 + (v & mask)));
		break;
	default:
		t0 = t1 = 0;
		break;
	}
#undef LOOP
	out[0] = t1 - t0;
	out[1] = v;
}

/* ================================================================== */
/* 乒乓:两个 block 的线程 0 轮流交接                                   */
/* ================================================================== */

enum {
	P_V_FSYS,
	P_V_FDEV,
	P_NT_MXD,
	P_V_NOF,
	P_NT_NOF,
	P_NUM
};

static const char *p_name[P_NUM] = {
	"volatile + sys fence      [我们默认]",
	"volatile + dev fence      [我们 0x4000000]",
	"nt + mxc dev rel/acq      [DeepEP]",
	"volatile,不 fence",
	"nontemporal,不 fence",
};

template <int V>
__device__ static __forceinline__ void
pp_send(uint64_t *f, uint64_t x)
{
	if (V == P_V_FSYS) {
		ST_V64(f, x);
		F_SYS();
	} else if (V == P_V_FDEV) {
		ST_V64(f, x);
		F_DEV();
	} else if (V == P_NT_MXD) {
		F_MXDR();
		ST_NT64(f, x);
	} else if (V == P_V_NOF) {
		ST_V64(f, x);
	} else {
		ST_NT64(f, x);
	}
}

template <int V>
__device__ static __forceinline__ uint64_t
pp_load(uint64_t *f)
{
	uint64_t x;

	if (V == P_V_FSYS) {
		F_SYS();
		x = LD_V64(f);
	} else if (V == P_V_FDEV) {
		F_DEV();
		x = LD_V64(f);
	} else if (V == P_NT_MXD) {
		x = LD_NT64(f);
		F_MXDA();
	} else if (V == P_V_NOF) {
		x = LD_V64(f);
	} else {
		x = LD_NT64(f);
		CBAR();
	}
	return x;
}

/* 等到 *f == x;超时返回 0 */
template <int V>
__device__ static __forceinline__ int
pp_wait(uint64_t *f, uint64_t x, uint64_t *spins)
{
	uint64_t k = 0;

	while (pp_load<V>(f) != x) {
		if (++k > MP_SPIN_MAX) {
			return 0;
		}
	}
	*spins += k;
	return 1;
}

/*
 * a:block 0 写、block 1 等;b:反过来。out[0] 往返总时钟,out[1] block 0
 * 的等待圈数,out[2]/out[3] 各 block 超时时卡在第几轮(0 = 没超时)。
 */
template <int V>
__global__ void
k_pingpong(uint64_t *a, uint64_t *b, uint32_t n, uint64_t *out)
{
	uint64_t t0, spins = 0;
	uint32_t i;

	if (threadIdx.x != 0) {
		return;
	}
	t0 = clock64();
	for (i = 1; i <= n; i++) {
		if (blockIdx.x == 0) {
			pp_send<V>(a, i);
			if (!pp_wait<V>(b, i, &spins)) {
				out[2] = i;
				return;
			}
		} else {
			if (!pp_wait<V>(a, i, &spins)) {
				out[3] = i;
				return;
			}
			pp_send<V>(b, i);
		}
	}
	if (blockIdx.x == 0) {
		out[0] = clock64() - t0;
		out[1] = spins;
	}
}

/* 时钟校准:自旋固定时钟数,host 量墙钟 */
__global__ void
k_spin(uint64_t cycles, uint64_t *out)
{
	uint64_t t0 = clock64(), t;

	do {
		t = clock64();
	} while (t - t0 < cycles);
	out[0] = t - t0;
}

/* ================================================================== */
/* host                                                                */
/* ================================================================== */

#define CK(x)								\
	do {								\
		mcError_t e_ = (x);					\
		if (e_ != mcSuccess) {					\
			fprintf(stderr, "%s:%d %s: %s\n", __FILE__,	\
				__LINE__, #x, mcGetErrorString(e_));	\
			exit(1);					\
		}							\
	} while (0)

static double
now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

struct mem {
	char	type;
	void	*host;		/* 'h' 时的主存指针 */
	uint8_t	*dev;		/* kernel 用的地址 */
};

static const char *
mem_desc(char t)
{
	switch (t) {
	case 'n': return "mcMalloc";
	case 'f': return "ExtMalloc(1) Finegrained";
	case 'u': return "ExtMalloc(4) MapPcieDefault";
	case 'h': return "主存 HostRegister";
	default:  return "?";
	}
}

static int
mem_alloc(struct mem *m, char t)
{
	void *p = NULL;

	memset(m, 0, sizeof(*m));
	m->type = t;
	switch (t) {
	case 'n':
		CK(mcMalloc(&p, MP_BUF_BYTES));
		break;
	case 'f':
		CK(mcExtMallocWithFlags(&p, MP_BUF_BYTES, 1));
		break;
	case 'u':
		CK(mcExtMallocWithFlags(&p, MP_BUF_BYTES, 4));
		break;
	case 'h':
		if (posix_memalign(&m->host, 4096, MP_BUF_BYTES) != 0) {
			return -1;
		}
		memset(m->host, 0, MP_BUF_BYTES);
		CK(mcHostRegister(m->host, MP_BUF_BYTES,
				  mcHostRegisterPortable | mcHostRegisterMapped));
		CK(mcHostGetDevicePointer(&p, m->host, 0));
		break;
	default:
		fprintf(stderr, "未知内存类型 '%c'\n", t);
		return -1;
	}
	m->dev = (uint8_t *)p;
	return 0;
}

static void
mem_clear(struct mem *m)
{
	if (m->type == 'h') {
		memset(m->host, 0, MP_BUF_BYTES);
	} else {
		CK(mcMemset(m->dev, 0, MP_BUF_BYTES));
	}
	CK(mcDeviceSynchronize());
}

static void
mem_free(struct mem *m)
{
	if (m->type == 'h') {
		mcHostUnregister(m->host);
		free(m->host);
	} else {
		mcFree(m->dev);
	}
}

/* 实测有效时钟(kHz)。标称值是最大频率,见 gpu_initiated.c 的说明 */
static double
calib_khz(uint64_t *d_out)
{
	const uint64_t cyc = 300000000ull;
	uint64_t got;
	double t0, t1;

	k_spin<<<1, 1>>>(1000000, d_out);	/* 预热 */
	CK(mcDeviceSynchronize());
	t0 = now_us();
	k_spin<<<1, 1>>>(cyc, d_out);
	CK(mcDeviceSynchronize());
	t1 = now_us();
	CK(mcMemcpy(&got, d_out, sizeof(got), mcMemcpyDeviceToHost));
	return got / (t1 - t0) * 1000.0;
}

static uint64_t
run_single(int t, struct mem *m, uint32_t n, int runs, uint64_t *d_out)
{
	uint64_t r[MP_MAX_RUNS], h[2];
	uint64_t *p = (uint64_t *)m->dev;
	uint32_t *lk = (uint32_t *)(m->dev + 4096);
	int k;

	mem_clear(m);
	k_single<<<1, 1>>>(t, p, lk, 0, n / 10 + 1, d_out);	/* 预热 */
	CK(mcDeviceSynchronize());
	for (k = 0; k < runs; k++) {
		k_single<<<1, 1>>>(t, p, lk, 0, n, d_out);
		CK(mcDeviceSynchronize());
		CK(mcGetLastError());
		CK(mcMemcpy(h, d_out, sizeof(h), mcMemcpyDeviceToHost));
		r[k] = h[0];
	}
	qsort(r, runs, sizeof(r[0]), cmp_u64);
	return r[runs / 2];
}

static void
pp_launch(int v, uint64_t *a, uint64_t *b, uint32_t n, uint64_t *o)
{
	switch (v) {
	case P_V_FSYS: k_pingpong<P_V_FSYS><<<2, 1>>>(a, b, n, o); break;
	case P_V_FDEV: k_pingpong<P_V_FDEV><<<2, 1>>>(a, b, n, o); break;
	case P_NT_MXD: k_pingpong<P_NT_MXD><<<2, 1>>>(a, b, n, o); break;
	case P_V_NOF:  k_pingpong<P_V_NOF><<<2, 1>>>(a, b, n, o); break;
	default:       k_pingpong<P_NT_NOF><<<2, 1>>>(a, b, n, o); break;
	}
}

/* 返回往返时钟中位数;超时返回 0 并在 *stuck 里给出卡住的轮次 */
static uint64_t
run_pingpong(int v, struct mem *m, uint32_t n, int runs, uint64_t *d_out,
	     double *spins, uint64_t *stuck)
{
	uint64_t r[MP_MAX_RUNS], h[4];
	uint64_t *a = (uint64_t *)m->dev;
	uint64_t *b = (uint64_t *)(m->dev + 512);	/* 不同缓存行 */
	double sp = 0;
	int k;

	*stuck = 0;
	for (k = 0; k < runs; k++) {
		mem_clear(m);
		CK(mcMemset(d_out, 0, 4 * sizeof(uint64_t)));
		CK(mcDeviceSynchronize());
		pp_launch(v, a, b, n, d_out);
		CK(mcDeviceSynchronize());
		CK(mcGetLastError());
		CK(mcMemcpy(h, d_out, sizeof(h), mcMemcpyDeviceToHost));
		if (h[2] || h[3]) {
			*stuck = h[2] ? h[2] : h[3];
			return 0;
		}
		r[k] = h[0];
		sp += (double)h[1] / n;
	}
	*spins = sp / runs;
	qsort(r, runs, sizeof(r[0]), cmp_u64);
	return r[runs / 2];
}

/* 左对齐打印到 w 列。中文 UTF-8 每字 3 字节、显示 2 列 */
static void
print_padded(const char *s, int w)
{
	const unsigned char *q;
	int cols = 0;

	for (q = (const unsigned char *)s; *q; q++) {
		if (*q < 0x80) {
			cols++;
		} else if ((*q & 0xe0) == 0xe0) {
			cols += 2;
		}
	}
	printf("%s%*s", s, cols < w ? w - cols : 1, "");
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"用法: %s -g <gpu> [-m nfuh] [-n 次数] [-r 重复] [-P 乒乓次数]\n"
		"  -m  内存类型,可多个:n mcMalloc,f Finegrained,u MapPcieDefault,h 主存\n"
		"      默认 nfh\n"
		"  -n  单线程每项循环次数(默认 20000)\n"
		"  -r  每项重复次数,取中位数(默认 5,最大 %d)\n"
		"  -P  乒乓往返次数(默认 5000)\n",
		prog, MP_MAX_RUNS);
	exit(1);
}

int
main(int argc, char **argv)
{
	const char *types = "nfh";
	struct mem mems[4];
	uint64_t *d_out;
	uint32_t n = 20000, pn = 5000;
	int gpu = -1, runs = 5, nm, i, t, c;
	double khz, ns_per_cyc;
	static uint64_t res[T_NUM][4];

	while ((c = getopt(argc, argv, "g:m:n:r:P:")) != -1) {
		switch (c) {
		case 'g': gpu = atoi(optarg); break;
		case 'm': types = optarg; break;
		case 'n': n = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'r': runs = atoi(optarg); break;
		case 'P': pn = (uint32_t)strtoul(optarg, NULL, 0); break;
		default: usage(argv[0]);
		}
	}
	nm = (int)strlen(types);
	if (gpu < 0 || nm < 1 || nm > 4 || runs < 1 || runs > MP_MAX_RUNS ||
	    n < 10 || pn < 10) {
		usage(argv[0]);
	}

	CK(mcSetDevice(gpu));
	CK(mcMalloc((void **)&d_out, 4096));
	khz = calib_khz(d_out);
	ns_per_cyc = 1e6 / khz;
	printf("[GPU] dev=%d 实测时钟 %.0f kHz(1 cycle = %.3f ns)\n", gpu, khz,
	       ns_per_cyc);
	printf("[参数] 单线程 %u 次/项,乒乓 %u 往返,各重复 %d 次取中位数\n",
	       n, pn, runs);

	for (i = 0; i < nm; i++) {
		if (mem_alloc(&mems[i], types[i]) != 0) {
			return 1;
		}
		printf("[内存] %c = %s\n", types[i], mem_desc(types[i]));
	}

	/* ---- 单线程 ---- */
	for (i = 0; i < nm; i++) {
		for (t = 0; t < T_NUM; t++) {
			res[t][i] = run_single(t, &mems[i], n, runs, d_out);
		}
	}
	printf("\n== 单线程,每次平均 ns ==\n%-44s", "");
	for (i = 0; i < nm; i++) {
		printf("%10c", types[i]);
	}
	printf("\n");
	for (t = 0; t < T_NUM; t++) {
		print_padded(t_name[t], 44);
		for (i = 0; i < nm; i++) {
			printf("%10.1f", (double)res[t][i] / n * ns_per_cyc);
		}
		printf("\n");
	}

	/* ---- 乒乓 ---- */
	printf("\n== 跨 block 乒乓,单程 ns(往返 / 2);括号里是每次等待的平均圈数 ==\n");
	for (i = 0; i < nm; i++) {
		printf("[%c %s]\n", types[i], mem_desc(types[i]));
		for (t = 0; t < P_NUM; t++) {
			uint64_t stuck, cyc;
			double spins = 0;

			cyc = run_pingpong(t, &mems[i], pn, runs, d_out,
					   &spins, &stuck);
			printf("  ");
			print_padded(p_name[t], 40);
			if (cyc == 0) {
				printf("看不见对方的写(第 %llu 轮超时)\n",
				       (unsigned long long)stuck);
			} else {
				printf("%8.1f   (%.1f 圈)\n",
				       (double)cyc / pn / 2 * ns_per_cyc, spins);
			}
		}
	}

	for (i = 0; i < nm; i++) {
		mem_free(&mems[i]);
	}
	mcFree(d_out);
	return 0;
}
