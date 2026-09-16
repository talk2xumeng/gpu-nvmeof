/*
 * pit7_io_kernel.cpp  --  坑 7 实验用的 kernel(tst/pit7)
 *
 * 从 src/gpu_io_kernel.cpp 分叉,加了:payload 由 kernel 写(-I)、
 * 填充微基准(-B)、轮间空转(-Y)、拆分 launch(-T 1..4)、每圈 fence(-Z)。
 * 背景和结论见同目录 README.md。
 *
 * 以下是原文件说明:
 *
 * gpu_io_kernel.cpp  --  kernel 内完成 NVMe-oF 读的全流程
 *
 * 稳态下 CPU 完全不参与:
 *   ① 构造 NVMe 命令胶囊(写显存,本地访问)
 *   ② 构造 mlx5 SEND WQE 写进 SQ
 *   ③ 更新 dbrec + 敲 UAR 门铃
 *   ④ 轮询 CQ 等 target 的响应
 *
 * ①② 用的是 wqe_build.h,那份布局已和 SPDK 实际提交的逐字节比对过。
 *
 * 一版只做单线程串行 —— 一个 warp 发一条,等完成,再发下一条。
 * 第 i 轮用显存第 i 块(data_step),LBA 同步递增,便于逐块校验。
 *
 * 三个约束:
 * 1. SQ/CQ/dbrec 在主机内存(SPDK 用 ibv_create_qp 建的),fence 必须
 *    是 system 级。以后用 DEVX 把队列建到显存才能降到 __threadfence()。
 * 2. 接管后这个 qpair 不能再调 spdk_nvme_ns_cmd_*,否则两边抢同一 slot。
 * 3. kernel 不补 recv buffer,所以单次 launch 轮数不能超过 RQ 深度。
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "pit7_io_ctx.h"

#define USE_MACA_DEVICE 1
#include "wqe_build.h"
#include "blk_pattern.h"

/*
 * 一次 I/O 产生两个 CQE:我们那条 SEND 的发送完成,以及 target 回的
 * 响应胶囊(RESP_SEND)。顺序不保证,收到后者才算真的完成。
 */
__device__ static int
poll_for_response(struct gpu_io_ctx *c, uint32_t *ci, uint8_t *phase,
		  uint64_t timeout_cycles, uint64_t *spins_out)
{
	uint64_t start = clock64();
	uint64_t spins = 0;

	for (;;) {
		/*
		 * 数圈数是为了区分"真的在等网络"和"自旋本身很贵"。
		 * CQ buffer 在主存,GPU 每转一圈都要走 PCIe 读 64 字节 ——
		 * 几百纳秒一次。转几十圈就能凑出几十微秒,那和网络无关。
		 * 转几万圈才说明确实在等对端。
		 */
		spins++;
		/*
		 * -Z:照 Tutti 在 MACA 下的写法,每圈都做一次 system fence
		 * (nvm_parallel_queue.h 的 cq_poll 系列)。
		 */
		if (c->poll_fence) {
			__threadfence_system();
		}
		uint32_t idx = *ci & (c->cq_cnt - 1);
		volatile uint8_t *cqe = c->cq_buf + (size_t)idx * c->cqe_size;
		uint8_t op_own = cqe[c->cqe_size - 1];
		uint8_t opcode = op_own >> 4;

		/*
		 * owner 和 opcode 两个条件缺一不可。空条目的 op_own=0xf0,
		 * owner 位恰好等于首圈的 phase(0) —— 只判 owner 的话,轮询
		 * 会在响应回来之前(不到 1 us)把整个 CQ 的空条目全部当成
		 * 有效 CQE 消费掉,绕回时 phase 翻转,真 CQE 落地后就再也
		 * 对不上了。表现就是"发包成功但永远等不到完成"。
		 */
		if ((op_own & 1) != (*phase & 1) ||
		    opcode == MLX5_CQE_INVALID) {
			if (clock64() - start > timeout_cycles) {
				*spins_out = spins;
				return -1;
			}
			continue;
		}

		/* owner 确认之后再读 CQE 其余字段 */
		__threadfence_system();

		(*ci)++;
		if ((*ci & (c->cq_cnt - 1)) == 0) {
			*phase ^= 1;
		}

		if (opcode == MLX5_CQE_REQ_ERR || opcode == MLX5_CQE_RESP_ERR) {
			*spins_out = spins;
			return -2;
		}
		if (opcode == MLX5_CQE_RESP_SEND) {
			*spins_out = spins;
			return 0;
		}
	}
}

/*
 * 一轮的提交部分,单 kernel 模式和拆分模式(-T)共用。
 * tt[0..3] = 填充前 / 胶囊前 / WQE 前 / 门铃后 的 clock64。
 *
 * 拆成两半,-T 3/4 要把它们放进不同的 kernel:
 *   prep_round: 空转 → 写 payload → 写胶囊       (写显存的部分)
 *   post_round: 写 WQE → dbrec → 门铃            (让网卡来读的部分)
 */
__device__ static void
prep_round(struct gpu_io_ctx *c, uint32_t i, uint64_t start_lba, uint64_t *tt)
{
	uint32_t cap_idx = i & 0x3f;
	volatile uint8_t *cap = c->capsule + (size_t)cap_idx * 64;
	uint64_t lba = start_lba + (uint64_t)i * (c->io_bytes / c->sector_size);
	uint64_t blk_addr = c->data_addr + (uint64_t)i * c->data_step;

	if (c->gap_cycles) {
		uint64_t g0 = clock64();

		while (clock64() - g0 < c->gap_cycles) {
		}
	}

	tt[0] = clock64();

	/*
	 * -I:payload 由本 kernel 写,紧接着发命令,中间没有任何设备
	 * 同步。这就是 PD 分离里"attention 算完直接发"的样子。
	 *
	 * 普通 mcMalloc 的显存上,预期会踩 debug-notes 坑 7:网卡
	 * RDMA_READ 取到的是写之前的旧值(host 预填的 0xEE)。
	 * -D 换分配 flag 可以对比 uncached payload 的效果和代价。
	 */
	if (c->fill_mode) {
		volatile uint64_t *d =
			(volatile uint64_t *)(uintptr_t)blk_addr;
		uint32_t nw = c->io_bytes / 8;
		uint32_t k;

		for (k = 0; k < nw; k++) {
			d[k] = blk_word(c->seed, lba, k);
		}
	}

	tt[1] = clock64();

	/*
	 * READ  : target 发 RDMA_WRITE 写我们显存(显存作 DMA target)
	 * WRITE : target 发 RDMA_READ  读我们显存(显存作 DMA source)
	 * 后者和 -H 失败的是同一类动作,正好用来二分。
	 */
	/*
	 * cid 高位打上 0xE 作标记,方便在 target 日志里把 kernel
	 * 发的命令和 SPDK(预热、涂色)发的区分开 —— 两边的 SGL
	 * 地址和 rkey 可能完全一样,只有 cid 能分。
	 * 定位完可以改回 (uint16_t)(i & 0xffff)。
	 */
	nvme_build_rw_sqe((struct nvme_sqe *)cap, c->nvme_opc,
			  (uint16_t)(0xE000u | (i & 0xfffu)), c->nsid,
			  lba, c->io_bytes / c->sector_size,
			  blk_addr, c->data_rkey, c->io_bytes);

	tt[2] = clock64();	/* 胶囊耗时 = tt[2] - tt[1] */
}

__device__ static void
post_round(struct gpu_io_ctx *c, uint32_t i, uint16_t pi, uint64_t *tt)
{
	uint32_t slot = pi & (c->sq_wqe_cnt - 1);
	volatile uint8_t *sq_slot = c->sq_buf + (size_t)slot * c->sq_stride;
	uint32_t cap_idx = i & 0x3f;
	uint64_t cap_addr = c->capsule_addr + (uint64_t)cap_idx * 64;
	uint64_t db_val;

	tt[2] = clock64();

	/*
	 * ctrl seg 的 wqe_idx 和门铃里的索引用的是 16 位生产者
	 * 计数 pi,不是取模之后的 slot —— slot 只用来算 SQ 里的
	 * 地址。早先这里传 slot,pi 跑过 sq_wqe_cnt(512) 之后就会
	 * 和 dbrec 写进去的 pi+1 对不上。
	 */
	db_val = mlx5_build_send_wqe((void *)sq_slot, pi,
				     c->qpn, cap_addr,
				     c->capsule_lkey, 64);

	WQE_FENCE_QUEUE();

	c->qp_dbrec[1] = wqe_hto_be32((uint32_t)(pi + 1) & 0xffff);
	WQE_FENCE_DB();

	/*
	 * BlueFlame 内联:整个 WQE(32 字节)写进 BF 寄存器。
	 *
	 * host 侧对照(wqe_verify 加/不加 -B)实测两种都能工作 ——
	 * 只写 8 字节(ctrl seg 前半)也行,网卡会回 SQ 取。这里用
	 * 内联是因为它是跑通时的形态,没有性能上的理由。
	 */
	{
		volatile uint64_t *bfp = c->bf_reg;
		const uint64_t *src = (const uint64_t *)(void *)sq_slot;
		int w;

		for (w = 0; w < 4; w++) {
			bfp[w] = src[w];
		}
	}
	WQE_FENCE_DB();
	(void)db_val;

	tt[3] = clock64();	/* 提交耗时 = tt[3] - tt[2] */
}

__device__ static void
submit_round(struct gpu_io_ctx *c, uint32_t i, uint16_t pi,
	     uint64_t start_lba, uint64_t *tt)
{
	uint64_t t2;

	prep_round(c, i, start_lba, tt);
	t2 = tt[2];
	post_round(c, i, pi, tt);
	tt[2] = t2;
}

__global__ void
k_gpu_io(struct gpu_io_ctx ctx, uint32_t rounds, uint64_t start_lba,
	 uint64_t *cycles, uint64_t *c_fill, uint64_t *c_cap, uint64_t *c_sub,
	 uint64_t *c_wait, uint64_t *c_spin, int *err_out, uint32_t *done_out)
{
	uint32_t i;
	uint16_t pi;
	uint32_t ci;
	uint8_t phase;
	const uint64_t timeout = 2000000000ULL;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}

	pi = ctx.sq_pi;
	ci = ctx.cq_ci;
	phase = ctx.cq_phase;
	*err_out = 0;
	*done_out = 0;

	for (i = 0; i < rounds; i++) {
		uint64_t tt[4];
		uint64_t tf, t0, t1, t2, t3;
		uint64_t spins = 0;
		int rc;

		submit_round(&ctx, i, pi, start_lba, tt);
		tf = tt[0]; t0 = tt[1]; t1 = tt[2]; t2 = tt[3];

		rc = poll_for_response(&ctx, &ci, &phase, timeout, &spins);
		t3 = clock64();		/* 等待耗时 = t3 - t2 */

		cycles[i] = t3 - tf;
		c_fill[i] = t0 - tf;	/* -I:kernel 写 payload */
		c_cap[i]  = t1 - t0;	/* 写 64 字节胶囊 */
		c_sub[i]  = t2 - t1;	/* 写 WQE + dbrec + 门铃 */
		c_wait[i] = t3 - t2;	/* 网络往返 + target 处理 + 轮询 */
		c_spin[i] = spins;	/* 轮询圈数 */

		/*
		 * 不管这一轮成功还是出错,已经消费掉的 CQE 都要把 ci 还给
		 * 网卡,否则下次跑起来 CQ 会溢出。
		 */
		ctx.cq_dbrec[0] = wqe_hto_be32(ci & 0xffffff);
		WQE_FENCE_DB();

		if (rc != 0) {
			*err_out = rc;
			*done_out = i;
			return;
		}

		pi++;
		*done_out = i + 1;
	}
}

/* 只敲门铃,用于单独测 MMIO 写的成本 */
__global__ void
k_ring_only(volatile uint64_t *bf_reg, uint64_t val, uint32_t n,
	    uint64_t *cycles)
{
	uint32_t i;

	if (threadIdx.x != 0) {
		return;
	}
	for (i = 0; i < n; i++) {
		uint64_t t0 = clock64();

		WQE_FENCE_DB();
		*bf_reg = val;
		WQE_FENCE_DB();
		cycles[i] = clock64() - t0;
	}
}

/*
 * 由 GPU kernel 自己往显存写 pattern —— 用普通 store,和
 * nvme_build_rw_sqe 写胶囊是同一类动作。
 *
 * 对照用:gpu_memset_dev 走的是 mcMemset,那是 CPU 发起的 DMA 填充,
 * 后面还跟 mcDeviceSynchronize,网卡当然读得到。真正要验的是
 * "kernel 内的 store 对网卡可见吗"。
 */
__global__ void
k_fill(volatile unsigned char *p, unsigned char v, unsigned int len)
{
	unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int stride = gridDim.x * blockDim.x;

	for (; i < len; i += stride) {
		p[i] = v;
	}
	__threadfence_system();
}

/*
 * 拆分模式(-T),照 Tutti snvme_smoke_gpu.cu 的形态:
 *   每轮 k_submit(写胶囊/WQE/门铃,然后 kernel 退出)
 *        [-T 2: mcDeviceSynchronize]
 *        k_poll(另一次 launch,等响应)
 *        mcDeviceSynchronize
 *
 * 要回答的问题:坑 7 里"kernel 写普通显存网卡读到旧值",Tutti 在同样
 * 的普通 cudaMalloc SQ ring 上却能工作。差别之一是它的 submit kernel
 * 敲完门铃就退出了。-T 1 检验"kernel 边界本身就是刷新点"。
 *
 * 跨 launch 的状态(ci/phase/rc)和逐轮计时放在 host 注册给 GPU 的主存
 * 里,kernel 写完做 system fence,host 同步后直接读,不用 memcpy。
 *
 * rec[i*6 + 0..5] = fill, cap, sub, wait, spins, 保留
 * st[0] = ci, st[1] = phase, st[2] = rc
 */
__global__ void
k_submit(struct gpu_io_ctx ctx, uint32_t i, uint16_t pi, uint64_t start_lba,
	 volatile uint64_t *rec)
{
	uint64_t tt[4];

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	submit_round(&ctx, i, pi, start_lba, tt);
	rec[0] = tt[1] - tt[0];
	rec[1] = tt[2] - tt[1];
	rec[2] = tt[3] - tt[2];
	__threadfence_system();
}

/* -T 3/4 前半:只写显存,然后退出 */
__global__ void
k_prep(struct gpu_io_ctx ctx, uint32_t i, uint64_t start_lba,
       volatile uint64_t *rec)
{
	uint64_t tt[4];

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	prep_round(&ctx, i, start_lba, tt);
	rec[0] = tt[1] - tt[0];
	rec[1] = tt[2] - tt[1];
	__threadfence_system();
}

/* -T 3/4 后半:WQE + 门铃 + 轮询,一个 kernel 做完 */
__global__ void
k_post_poll(struct gpu_io_ctx ctx, uint32_t i, uint16_t pi,
	    volatile uint32_t *st, volatile uint64_t *rec)
{
	uint64_t tt[4], spins = 0, t0;
	uint32_t ci;
	uint8_t phase;
	int rc;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	post_round(&ctx, i, pi, tt);
	rec[2] = tt[3] - tt[2];
	ci = st[0];
	phase = (uint8_t)st[1];
	t0 = clock64();
	rc = poll_for_response(&ctx, &ci, &phase, 2000000000ULL, &spins);
	rec[3] = clock64() - t0;
	rec[4] = spins;
	ctx.cq_dbrec[0] = wqe_hto_be32(ci & 0xffffff);
	WQE_FENCE_DB();
	st[0] = ci;
	st[1] = phase;
	st[2] = (uint32_t)rc;
	__threadfence_system();
}

__global__ void
k_poll(struct gpu_io_ctx ctx, volatile uint32_t *st, volatile uint64_t *rec)
{
	uint32_t ci;
	uint8_t phase;
	uint64_t spins = 0, t0;
	int rc;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	ci = st[0];
	phase = (uint8_t)st[1];
	t0 = clock64();
	rc = poll_for_response(&ctx, &ci, &phase, 2000000000ULL, &spins);
	rec[3] = clock64() - t0;
	rec[4] = spins;
	ctx.cq_dbrec[0] = wqe_hto_be32(ci & 0xffffff);
	WQE_FENCE_DB();
	st[0] = ci;
	st[1] = phase;
	st[2] = (uint32_t)rc;
	__threadfence_system();
}

extern "C" {

static double
split_now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int
gpu_io_run_split(struct gpu_io_ctx *ctx, unsigned int rounds,
		 unsigned long long start_lba, int mode,
		 unsigned long long *host_fill, unsigned long long *host_cap,
		 unsigned long long *host_sub, unsigned long long *host_wait,
		 unsigned long long *host_spin, double *host_wall_us,
		 int *host_err, unsigned int *host_done)
{
	size_t rec_len = ((size_t)rounds * 6 * sizeof(uint64_t) + 4095) & ~4095UL;
	void *rec_h = NULL, *st_h = NULL, *rec_d = NULL, *st_d = NULL;
	uint32_t *st;
	uint64_t *rec;
	unsigned int i;
	int rc = -1;

	*host_err = 0;
	*host_done = 0;
	if (posix_memalign(&rec_h, 4096, rec_len) ||
	    posix_memalign(&st_h, 4096, 4096)) {
		goto out;
	}
	memset(rec_h, 0, rec_len);
	memset(st_h, 0, 4096);
	if (mcHostRegister(rec_h, rec_len, 0x02) != mcSuccess ||
	    mcHostGetDevicePointer(&rec_d, rec_h, 0) != mcSuccess ||
	    mcHostRegister(st_h, 4096, 0x02) != mcSuccess ||
	    mcHostGetDevicePointer(&st_d, st_h, 0) != mcSuccess) {
		fprintf(stderr, "split: 状态区注册失败\n");
		goto out;
	}
	st = (uint32_t *)st_h;
	rec = (uint64_t *)rec_h;
	st[0] = ctx->cq_ci;
	st[1] = ctx->cq_phase;

	for (i = 0; i < rounds; i++) {
		double w0 = split_now_us();
		mcError_t e;

		volatile uint64_t *r =
			(volatile uint64_t *)((uint64_t *)rec_d + (size_t)i * 6);
		uint16_t pi = (uint16_t)(ctx->sq_pi + i);

		if (mode <= 2) {
			/* 1/2:门铃在写显存的同一个 kernel 里 */
			k_submit<<<1, 32>>>(*ctx, i, pi, start_lba, r);
			if (mode == 2) {
				mcDeviceSynchronize();
			}
			k_poll<<<1, 32>>>(*ctx, (volatile uint32_t *)st_d, r);
		} else {
			/* 3/4:写显存的 kernel 退出之后,另一个 kernel 敲门铃 */
			k_prep<<<1, 32>>>(*ctx, i, start_lba, r);
			if (mode == 4) {
				mcDeviceSynchronize();
			}
			k_post_poll<<<1, 32>>>(*ctx, i, pi,
					       (volatile uint32_t *)st_d, r);
		}
		e = mcGetLastError();
		if (e == mcSuccess) {
			e = mcDeviceSynchronize();
		}
		host_wall_us[i] = split_now_us() - w0;
		if (e != mcSuccess) {
			fprintf(stderr, "split: 第 %u 轮 kernel 失败: %s\n",
				i, mcGetErrorString(e));
			goto out;
		}
		host_fill[i] = rec[i * 6 + 0];
		host_cap[i]  = rec[i * 6 + 1];
		host_sub[i]  = rec[i * 6 + 2];
		host_wait[i] = rec[i * 6 + 3];
		host_spin[i] = rec[i * 6 + 4];
		if ((int)st[2] != 0) {
			*host_err = (int)st[2];
			break;
		}
		*host_done = i + 1;
	}
	rc = 0;
out:
	if (rec_d) mcHostUnregister(rec_h);
	if (st_d) mcHostUnregister(st_h);
	free(rec_h);
	free(st_h);
	return rc;
}

}

/*
 * 拆分形态(-I 2):一个独立 kernel 把所有块写好,host 随后
 * mcDeviceSynchronize,再由 io kernel 发送。和 -I 1 用同一套 pattern、
 * 同样单线程,填充耗时可直接比较。
 */
__global__ void
k_fill_blocks(volatile uint64_t *base, uint32_t nblk, uint32_t bs,
	      uint8_t seed, uint64_t lba0, uint32_t nlb)
{
	uint32_t i, k, nw = bs / 8;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	for (i = 0; i < nblk; i++) {
		uint64_t lba = lba0 + (uint64_t)i * nlb;
		volatile uint64_t *d = base + (size_t)i * nw;

		for (k = 0; k < nw; k++) {
			d[k] = blk_word(seed, lba, k);
		}
	}
}

/*
 * 填充微基准(-B)。每次 rep 处理 words 个字:
 *   mode 0  只算 blk_word,结果丢进局部 volatile   -> 纯计算
 *   mode 1  store 常量(k)                         -> 纯写
 *   mode 2  算 + 写(= -I 1 的填充段)
 *   mode 3  只读(累加)                           -> 读,attention 读 KV 的样子
 *   mode 4  同 1,但普通指针(非 volatile)
 *   mode 5  同 3,但普通指针(非 volatile)
 * 4/5 用来判断 1/3 的慢是不是 volatile 访存语义造成的。
 */
__global__ void
k_bench(volatile uint64_t *buf, uint32_t words, uint32_t reps, uint32_t mode,
	uint64_t *out)
{
	uint64_t *pb = (uint64_t *)buf;
	uint32_t r, k;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	for (r = 0; r < reps; r++) {
		uint64_t t0 = clock64();
		volatile uint64_t acc = 0;

		if (mode == 4) {
			for (k = 0; k < words; k++) {
				pb[k] = k ^ r;
			}
			out[r] = clock64() - t0;
			continue;
		}
		if (mode == 5) {
			uint64_t sum = 0;

			for (k = 0; k < words; k++) {
				sum += pb[k];
			}
			acc = sum;
			out[r] = clock64() - t0;
			continue;
		}
		for (k = 0; k < words; k++) {
			switch (mode) {
			case 0:
				acc ^= blk_word(7, r, k);
				break;
			case 1:
				buf[k] = k;
				break;
			case 2:
				buf[k] = blk_word(7, r, k);
				break;
			default:
				acc += buf[k];
				break;
			}
		}
		(void)acc;
		out[r] = clock64() - t0;
	}
}

extern "C" {

int
gpu_fill_blocks(void *base, unsigned int nblk, unsigned int bs,
		unsigned int seed, unsigned long long lba0, unsigned int nlb)
{
	k_fill_blocks<<<1, 32>>>((volatile uint64_t *)base, nblk, bs,
				 (uint8_t)seed, lba0, nlb);
	return mcGetLastError() == mcSuccess ? 0 : -1;
}

int
gpu_bench(void *buf, unsigned int words, unsigned int reps, unsigned int mode,
	  unsigned long long *host_cycles)
{
	uint64_t *d = NULL;
	int rc = -1;

	if (mcMalloc((void **)&d, reps * sizeof(uint64_t)) != mcSuccess) {
		return -1;
	}
	k_bench<<<1, 32>>>((volatile uint64_t *)buf, words, reps, mode, d);
	if (mcGetLastError() == mcSuccess &&
	    mcDeviceSynchronize() == mcSuccess) {
		mcMemcpy(host_cycles, d, reps * sizeof(uint64_t),
			 mcMemcpyDeviceToHost);
		rc = 0;
	}
	mcFree(d);
	return rc;
}

int
gpu_free(void *p)
{
	return mcFree(p) == mcSuccess ? 0 : -1;
}

int
gpu_io_run(struct gpu_io_ctx *ctx, unsigned int rounds,
	   unsigned long long start_lba, unsigned long long *host_cycles,
	   unsigned long long *host_fill,
	   unsigned long long *host_cap, unsigned long long *host_sub,
	   unsigned long long *host_wait, unsigned long long *host_spin,
	   int *host_err, unsigned int *host_done)
{
	uint64_t *d_cycles = NULL, *d_cap = NULL, *d_sub = NULL, *d_wait = NULL;
	uint64_t *d_spin = NULL, *d_fill = NULL;
	int *d_err = NULL;
	uint32_t *d_done = NULL;
	size_t sz = rounds * sizeof(uint64_t);
	mcError_t e;
	int rc = -1;

	if (mcMalloc((void **)&d_cycles, sz) != mcSuccess ||
	    mcMalloc((void **)&d_cap, sz) != mcSuccess ||
	    mcMalloc((void **)&d_sub, sz) != mcSuccess ||
	    mcMalloc((void **)&d_wait, sz) != mcSuccess ||
	    mcMalloc((void **)&d_spin, sz) != mcSuccess ||
	    mcMalloc((void **)&d_fill, sz) != mcSuccess ||
	    mcMalloc((void **)&d_err, sizeof(int)) != mcSuccess ||
	    mcMalloc((void **)&d_done, sizeof(uint32_t)) != mcSuccess) {
		goto out;
	}

	k_gpu_io<<<1, 32>>>(*ctx, rounds, start_lba, d_cycles, d_fill,
			    d_cap, d_sub, d_wait, d_spin, d_err, d_done);

	e = mcGetLastError();
	if (e != mcSuccess) {
		fprintf(stderr, "kernel launch 失败: %s\n", mcGetErrorString(e));
		goto out;
	}
	e = mcDeviceSynchronize();
	if (e != mcSuccess) {
		fprintf(stderr, "kernel 执行失败: %s\n", mcGetErrorString(e));
		goto out;
	}

	mcMemcpy(host_cycles, d_cycles, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_cap,  d_cap,  sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_sub,  d_sub,  sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_wait, d_wait, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_spin, d_spin, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_fill, d_fill, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_err, d_err, sizeof(int), mcMemcpyDeviceToHost);
	mcMemcpy(host_done, d_done, sizeof(uint32_t), mcMemcpyDeviceToHost);
	rc = 0;
out:
	if (d_cycles) mcFree(d_cycles);
	if (d_cap)    mcFree(d_cap);
	if (d_sub)    mcFree(d_sub);
	if (d_wait)   mcFree(d_wait);
	if (d_spin)   mcFree(d_spin);
	if (d_fill)   mcFree(d_fill);
	if (d_err)    mcFree(d_err);
	if (d_done)   mcFree(d_done);
	return rc;
}

int
gpu_ring_bench(void *bf_dev_ptr, unsigned long long val, unsigned int n,
	       unsigned long long *host_cycles)
{
	uint64_t *d_cycles = NULL;
	int rc = -1;

	if (mcMalloc((void **)&d_cycles, n * sizeof(uint64_t)) != mcSuccess) {
		return -1;
	}
	k_ring_only<<<1, 32>>>((volatile uint64_t *)bf_dev_ptr, val, n, d_cycles);
	if (mcGetLastError() == mcSuccess &&
	    mcDeviceSynchronize() == mcSuccess) {
		mcMemcpy(host_cycles, d_cycles, n * sizeof(uint64_t),
			 mcMemcpyDeviceToHost);
		rc = 0;
	}
	mcFree(d_cycles);
	return rc;
}

int
gpu_alloc_exportable_ex(void **ptr, int *fd, size_t len, unsigned int flags)
{
	mcError_t e;

	/*
	 * flags != 0 走 mcExtMallocWithFlags。MACA 的取值见
	 * mcr/mc_runtime_types.h:
	 *   0x1 Finegrained        细粒度区域
	 *   0x3 WriteCoherence     写一致
	 *   0x4 MapPcieDefault     uncache,映射到 PCIe 访问
	 *   0x5 MapPcieCoherence   写一致 + 映射到 PCIe
	 *   0x6 FixedMemDefault    uncache,固定内存区
	 *
	 * 为什么要它:普通 mcMalloc 的显存,GPU kernel 普通 store 写完之后,
	 * 网卡的 DMA read 取到的是写入前的旧值 —— 写停在 L2 里。已实测无效
	 * 的手段:__threadfence_system()、SyncMemops、门铃前空转 1 ms、
	 * 去掉 RELAXED_ORDERING。只有把那段写拆到独立 kernel 再做
	 * mcDeviceSynchronize 才正常,而 MACA 没暴露 kernel 内的 flush
	 * (CanFlushRemoteWrites=0, HdpMemFlushCntl=0)。
	 *
	 * uncached 显存的 GPU 侧带宽会掉不少,所以只给 64 字节的命令胶囊用,
	 * MB 级的 payload 仍然走普通 mcMalloc。
	 */
	e = flags ? mcExtMallocWithFlags(ptr, len, flags)
		  : mcMalloc(ptr, len);
	if (e != mcSuccess) {
		return -1;
	}
	if (mcMemGetHandleForAddressRange(fd, *ptr, len, 1, 0) != mcSuccess) {
		mcFree(*ptr);
		return -1;
	}
	return 0;
}

int
gpu_alloc_exportable(void **ptr, int *fd, size_t len)
{
	return gpu_alloc_exportable_ex(ptr, fd, len, 0);
}

int
gpu_map_host(void *hptr, size_t len, unsigned int flags, void **dptr)
{
	if (mcHostRegister(hptr, len, flags) != mcSuccess) {
		return -1;
	}
	if (mcHostGetDevicePointer(dptr, hptr, 0) != mcSuccess) {
		mcHostUnregister(hptr);
		return -1;
	}
	return 0;
}

int gpu_set_dev(int id)
{
	return mcSetDevice(id) == mcSuccess ? 0 : -1;
}

int gpu_clock_khz(int dev, int *khz)
{
	mcDeviceProp_t prop;

	if (mcGetDeviceProperties(&prop, dev) != mcSuccess) {
		return -1;
	}
	*khz = prop.clockRate;
	return 0;
}

int gpu_copy_to_host(void *dst, const void *src, size_t len)
{
	return mcMemcpy(dst, src, len, mcMemcpyDeviceToHost) == mcSuccess ? 0 : -1;
}

/* 同步拷贝;调用方仍应在交给网卡之前 gpu_sync(),见坑 2 */
int gpu_copy_to_dev(void *dst, const void *src, size_t len)
{
	return mcMemcpy(dst, src, len, mcMemcpyHostToDevice) == mcSuccess ? 0 : -1;
}

int gpu_memset_dev(void *p, int v, size_t len)
{
	return mcMemset(p, v, len) == mcSuccess ? 0 : -1;
}

/*
 * mcMemset 对 host 是异步的 —— 排进 stream 就返回。紧接着让网卡去读
 * 这块显存,可能读到 memset 还没写完的旧内容。测试里表现为同一条命令
 * 时而通过时而失败(赢/输竞态),非常容易误判成硬件问题。
 * 凡是 memset 之后要交给网卡的地方,都得先同步。
 */
/* kernel 侧填充。不做 mcDeviceSynchronize 之外的任何 flush —— 就是要看
 * kernel 的普通 store 能不能被网卡看到。 */
int gpu_fill_kernel(void *p, int v, size_t len)
{
	k_fill<<<64, 256>>>((volatile unsigned char *)p,
			    (unsigned char)v, (unsigned int)len);
	return mcGetLastError() == mcSuccess ? 0 : -1;
}

int gpu_sync(void)
{
	return mcDeviceSynchronize() == mcSuccess ? 0 : -1;
}

}
