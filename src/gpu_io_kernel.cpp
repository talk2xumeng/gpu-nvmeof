/*
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
 *
 * 三个约束:
 * 1. SQ/CQ/dbrec 在主机内存(SPDK 用 ibv_create_qp 建的),fence 必须
 *    是 system 级。以后用 DEVX 把队列建到显存才能降到 __threadfence()。
 * 2. 接管后这个 qpair 不能再调 spdk_nvme_ns_cmd_*,否则两边抢同一 slot。
 * 3. kernel 不补 recv buffer,所以单次 launch 轮数不能超过 RQ 深度。
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>
#include "gpu_io_ctx.h"

#define USE_MACA_DEVICE 1
#include "wqe_build.h"

/*
 * 一次 I/O 产生两个 CQE:我们那条 SEND 的发送完成,以及 target 回的
 * 响应胶囊(RESP_SEND)。顺序不保证,收到后者才算真的完成。
 */
__device__ static int
poll_for_response(struct gpu_io_ctx *c, uint32_t *ci, uint8_t *phase,
		  uint64_t timeout_cycles)
{
	uint64_t start = clock64();

	for (;;) {
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
			return -2;
		}
		if (opcode == MLX5_CQE_RESP_SEND) {
			return 0;
		}
	}
}

__global__ void
k_gpu_io(struct gpu_io_ctx ctx, uint32_t rounds, uint64_t start_lba,
	 uint64_t *cycles, int *err_out, uint32_t *done_out)
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
		uint64_t t0, t1;
		uint32_t slot = pi & (ctx.sq_wqe_cnt - 1);
		volatile uint8_t *sq_slot = ctx.sq_buf +
					    (size_t)slot * ctx.sq_stride;
		uint32_t cap_idx = i & 0x3f;
		volatile uint8_t *cap = ctx.capsule + (size_t)cap_idx * 64;
		uint64_t cap_addr = ctx.capsule_addr + (uint64_t)cap_idx * 64;
		uint64_t lba = start_lba +
			       (uint64_t)i * (ctx.io_bytes / ctx.sector_size);
		uint64_t db_val;
		int rc;

		t0 = clock64();

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
		nvme_build_rw_sqe((struct nvme_sqe *)cap, ctx.nvme_opc,
				  (uint16_t)(0xE000u | (i & 0xfffu)), ctx.nsid,
				  lba, ctx.io_bytes / ctx.sector_size,
				  ctx.data_addr, ctx.data_rkey, ctx.io_bytes);

		/*
		 * ctrl seg 的 wqe_idx 和门铃里的索引用的是 16 位生产者
		 * 计数 pi,不是取模之后的 slot —— slot 只用来算 SQ 里的
		 * 地址。早先这里传 slot,pi 跑过 sq_wqe_cnt(512) 之后就会
		 * 和 dbrec 写进去的 pi+1 对不上。
		 */
		db_val = mlx5_build_send_wqe((void *)sq_slot, pi,
					     ctx.qpn, cap_addr,
					     ctx.capsule_lkey, 64);

		WQE_FENCE_QUEUE();


		ctx.qp_dbrec[1] = wqe_hto_be32((uint32_t)(pi + 1) & 0xffff);
		WQE_FENCE_DB();

		/*
		 * BlueFlame 内联:整个 WQE(ds=2,32 字节)写进 BF 寄存器,
		 * 不是只写 8 字节让网卡回 SQ 取。
		 *
		 * 这块网卡(bf.size=256)按内联解析 BF 写入。只写 8 字节时
		 * 读命令侥幸能通 —— SEND 靠 dbrec 也发得出去;但写命令要
		 * 网卡持有完整 WQE 才能服务 target 回来的 RDMA_READ,
		 * 残缺就取不到数据,表现为 SC=0 成功但盘上全零。
		 *
		 * host 侧对照(wqe_verify -B)实测:8 字节失败,内联成功。
		 */
		{
			volatile uint64_t *bfp = ctx.bf_reg;
			const uint64_t *src = (const uint64_t *)(void *)sq_slot;
			int w;

			for (w = 0; w < 4; w++) {
				bfp[w] = src[w];
			}
		}
		WQE_FENCE_DB();
		(void)db_val;

		rc = poll_for_response(&ctx, &ci, &phase, timeout);

		t1 = clock64();
		cycles[i] = t1 - t0;

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

extern "C" {

int
gpu_io_run(struct gpu_io_ctx *ctx, unsigned int rounds,
	   unsigned long long start_lba, unsigned long long *host_cycles,
	   int *host_err, unsigned int *host_done)
{
	uint64_t *d_cycles = NULL;
	int *d_err = NULL;
	uint32_t *d_done = NULL;
	mcError_t e;
	int rc = -1;

	if (mcMalloc((void **)&d_cycles, rounds * sizeof(uint64_t)) != mcSuccess ||
	    mcMalloc((void **)&d_err, sizeof(int)) != mcSuccess ||
	    mcMalloc((void **)&d_done, sizeof(uint32_t)) != mcSuccess) {
		goto out;
	}

	k_gpu_io<<<1, 32>>>(*ctx, rounds, start_lba, d_cycles, d_err, d_done);

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

	mcMemcpy(host_cycles, d_cycles, rounds * sizeof(uint64_t),
		 mcMemcpyDeviceToHost);
	mcMemcpy(host_err, d_err, sizeof(int), mcMemcpyDeviceToHost);
	mcMemcpy(host_done, d_done, sizeof(uint32_t), mcMemcpyDeviceToHost);
	rc = 0;
out:
	if (d_cycles) mcFree(d_cycles);
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
