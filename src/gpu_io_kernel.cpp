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

		nvme_build_rw_sqe((struct nvme_sqe *)cap, NVME_OPC_READ,
				  (uint16_t)(i & 0xffff), ctx.nsid,
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
		*ctx.bf_reg = db_val;
		WQE_FENCE_DB();

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
gpu_alloc_exportable(void **ptr, int *fd, size_t len)
{
	if (mcMalloc(ptr, len) != mcSuccess) {
		return -1;
	}
	if (mcMemGetHandleForAddressRange(fd, *ptr, len, 1, 0) != mcSuccess) {
		mcFree(*ptr);
		return -1;
	}
	return 0;
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

}
