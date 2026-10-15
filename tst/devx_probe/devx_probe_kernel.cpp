/*
 * devx_probe_kernel.cpp  --  GPU 单线程:写 RDMA WRITE WQE → dbrec → 门铃 → 轮询 CQE
 *
 * QP 回环到自己,每轮一条,QD=1。逐轮记两个时间(都在同一线程里量):
 *   post  写 payload + WQE + dbrec + 门铃
 *   comp  门铃之后到看见 CQE
 * SQ/CQ 在主存还是显存由 host 决定;在显存时写完只用 __threadfence(),
 * 和 mxshmem 的 IBGDA_MEMBAR 一样,在主存时用 __threadfence_system()。
 */

#include <mcr/mc_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "devx_probe.h"

#define DP_TIMEOUT	2000000000ULL

__device__ static inline uint32_t
be32(uint32_t x)
{
	return ((x & 0xffu) << 24) | ((x & 0xff00u) << 8) |
	       ((x >> 8) & 0xff00u) | (x >> 24);
}

__device__ static inline void
fence_q(int in_gpu)
{
	if (in_gpu) {
		__threadfence();
	} else {
		__threadfence_system();
	}
}

__global__ void
k_devx_probe(struct dp_args a, struct dp_out *o, uint64_t *t_post,
	     uint64_t *t_comp)
{
	uint32_t ci = 0, i;
	uint8_t phase = 0;
	uint64_t spins = 0;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	o->k_start = clock64();
	o->err = 0;

	for (i = 0; i < a.n; i++) {
		uint16_t pi = (uint16_t)i;
		volatile uint64_t *w = (volatile uint64_t *)
			(a.sq + (size_t)(pi & (a.sq_cnt - 1)) * 64);
		uint64_t ctrl0, t0, t1, t2;
		volatile uint8_t *cqe;
		uint8_t op_own;

		t0 = clock64();

		/* payload:本轮序号,回读 dst 校验最后一轮 */
		*a.src_dev = (uint64_t)(i + 1);
		__threadfence_system();

		/*
		 * RDMA WRITE,ds = 3:ctrl(16) + raddr(16) + data(16)。
		 * 小端机器上按字节序拼成 64 位字,布局同 wqe_build.h。
		 */
		ctrl0 = (uint64_t)be32(((uint32_t)pi << 8) | 0x08) |
			((uint64_t)be32((a.qpn << 8) | 3) << 32);
		w[0] = ctrl0;
		w[1] = (uint64_t)((uint32_t)0x08 << 24);	/* fm_ce_se = CQ_UPDATE */
		w[2] = (uint64_t)be32((uint32_t)(a.raddr >> 32)) |
		       ((uint64_t)be32((uint32_t)a.raddr) << 32);
		w[3] = (uint64_t)be32(a.rkey);
		w[4] = (uint64_t)be32(a.len) | ((uint64_t)be32(a.lkey) << 32);
		w[5] = (uint64_t)be32((uint32_t)(a.laddr >> 32)) |
		       ((uint64_t)be32((uint32_t)a.laddr) << 32);
		fence_q(a.sq_in_gpu);

		a.qp_dbr[1] = be32((uint32_t)(pi + 1));
		fence_q(a.sq_in_gpu);

		/* 普通门铃:控制段前 8 字节写 UAR(MMIO 一律 system fence) */
		*a.db = ctrl0;
		__threadfence_system();
		t1 = clock64();

		cqe = a.cq + (size_t)(ci & (a.cq_cnt - 1)) * 64;
		for (;;) {
			uint64_t tail = *(volatile uint64_t *)(cqe + 56);

			op_own = (uint8_t)(tail >> 56);
			spins++;
			if ((op_own & 1) == phase && (op_own >> 4) != 0xf) {
				break;
			}
			if ((spins & 0x3ff) == 0 &&
			    clock64() - t1 > DP_TIMEOUT) {
				o->err = DP_E_TIMEOUT;
				o->err_iter = i;
				goto out;
			}
		}
		fence_q(a.cq_in_gpu);
		t2 = clock64();

		if ((op_own >> 4) != 0) {	/* 0 = MLX5_CQE_REQ */
			o->err = DP_E_CQE;
			o->err_iter = i;
			o->syndrome = ((uint32_t)cqe[54] << 8) | cqe[55];
			goto out;
		}
		ci++;
		if ((ci & (a.cq_cnt - 1)) == 0) {
			phase ^= 1;
		}
		a.cq_dbr[0] = be32(ci & 0xffffff);
		fence_q(a.cq_in_gpu);

		t_post[i] = t1 - t0;
		t_comp[i] = t2 - t1;
		o->done = i + 1;
	}
out:
	o->spins = spins;
	o->k_end = clock64();
	__threadfence_system();
}

extern "C" int
dp_run(struct dp_args *a, struct dp_out *out, unsigned long long *post,
       unsigned long long *comp, double *wall_us)
{
	struct dp_out *d_o = NULL;
	uint64_t *d_post = NULL, *d_comp = NULL;
	size_t sz = (size_t)a->n * sizeof(uint64_t);
	struct timespec t0, t1;
	mcError_t e;
	int rc = -1;

	if (mcMalloc((void **)&d_o, sizeof(*d_o)) != mcSuccess ||
	    mcMalloc((void **)&d_post, sz) != mcSuccess ||
	    mcMalloc((void **)&d_comp, sz) != mcSuccess) {
		goto done;
	}
	mcMemset(d_o, 0, sizeof(*d_o));
	mcMemset(d_post, 0, sz);
	mcMemset(d_comp, 0, sz);
	mcDeviceSynchronize();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	k_devx_probe<<<1, 32>>>(*a, d_o, d_post, d_comp);
	e = mcGetLastError();
	if (e == mcSuccess) {
		e = mcDeviceSynchronize();
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (e != mcSuccess) {
		fprintf(stderr, "kernel 失败: %s\n", mcGetErrorString(e));
		goto done;
	}
	*wall_us = (t1.tv_sec - t0.tv_sec) * 1e6 +
		   (t1.tv_nsec - t0.tv_nsec) / 1e3;
	mcMemcpy(out, d_o, sizeof(*out), mcMemcpyDeviceToHost);
	mcMemcpy(post, d_post, sz, mcMemcpyDeviceToHost);
	mcMemcpy(comp, d_comp, sz, mcMemcpyDeviceToHost);
	rc = 0;
done:
	if (d_o) mcFree(d_o);
	if (d_post) mcFree(d_post);
	if (d_comp) mcFree(d_comp);
	return rc;
}
