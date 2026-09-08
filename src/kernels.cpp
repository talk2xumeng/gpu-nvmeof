/*
 * kernels.cpp  --  基线测试用的 GPU kernel
 *
 * 用 mxcc 编译。对外导出 extern "C" 包装函数,让 C 代码(gcc 编译)
 * 能调用,避免整个工程都被迫用 mxcc。
 *
 * 这里的 kernel 刻意做得很轻 —— 我们要测的是 kernel launch 和
 * 退出的固定开销,不是计算本身。
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* kernel                                                              */
/* ------------------------------------------------------------------ */

/*
 * 模拟"算到一半发现需要数据"。
 * 做一点无法被编译器优化掉的整数运算,然后退出。
 */
__global__ void
k_compute(unsigned int *scratch, int iters)
{
	unsigned int idx = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int v = scratch[idx];
	int i;

	for (i = 0; i < iters; i++) {
		v = v * 1664525u + 1013904223u;
	}
	scratch[idx] = v;
}

/*
 * 模拟"数据到了,开始用"。
 *
 * 这个 kernel 有个额外作用:它验证 SM 能真正读到 RDMA 写进来的数据。
 * 拷贝引擎和 SM 的一致性路径不同,memcpy_dtoh 能读对不代表 SM 也能 ——
 * L2 里可能还有陈旧数据。所以让 kernel 自己算 checksum 比在 host
 * 侧比对更严格。
 *
 * 实现上要够快,否则测出来的是 kernel 自己的低效而不是 launch 开销。
 * 早期版本用逐字节访问 + per-thread atomicAdd,只跑到 30 GB/s,
 * 比网络还慢,把延迟分解彻底带偏了。现在改成:
 *   - uint4 向量化访问,一次 16 字节
 *   - block 内先做 shared memory 归约,只有每 block 一次 atomicAdd
 *   - grid 开大,让 SM 填满
 */
__global__ void
k_consume(const uint4 *buf, size_t nvec, unsigned long long *out)
{
	__shared__ unsigned long long smem[256];

	size_t idx = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
	size_t stride = (size_t)gridDim.x * blockDim.x;
	unsigned long long sum = 0;
	unsigned int tid = threadIdx.x;
	unsigned int s;
	size_t i;

	for (i = idx; i < nvec; i += stride) {
		uint4 v = buf[i];

		sum += v.x + v.y + v.z + v.w;
	}

	smem[tid] = sum;
	__syncthreads();

	for (s = blockDim.x / 2; s > 0; s >>= 1) {
		if (tid < s) {
			smem[tid] += smem[tid + s];
		}
		__syncthreads();
	}

	if (tid == 0) {
		atomicAdd(out, smem[0]);
	}
}

/* 空 kernel,用来单独测 launch + sync 的固定开销 */
__global__ void
k_empty(void)
{
}

/* ------------------------------------------------------------------ */
/* C 接口                                                              */
/* ------------------------------------------------------------------ */

extern "C" {

int
gpu_kernels_init(void **scratch_out, void **result_out)
{
	void *scratch = NULL;
	void *result = NULL;

	if (mcMalloc(&scratch, 1024 * sizeof(unsigned int)) != mcSuccess) {
		return -1;
	}
	if (mcMalloc(&result, sizeof(unsigned long long)) != mcSuccess) {
		mcFree(scratch);
		return -1;
	}
	mcMemset(scratch, 0, 1024 * sizeof(unsigned int));
	mcMemset(result, 0, sizeof(unsigned long long));

	*scratch_out = scratch;
	*result_out = result;
	return 0;
}

void
gpu_kernels_fini(void *scratch, void *result)
{
	if (scratch) {
		mcFree(scratch);
	}
	if (result) {
		mcFree(result);
	}
}

/* 启动计算 kernel 并等它结束。iters 控制 kernel 内的工作量。 */
int
gpu_launch_compute(void *scratch, int iters)
{
	k_compute<<<4, 256>>>((unsigned int *)scratch, iters);
	if (mcGetLastError() != mcSuccess) {
		return -1;
	}
	return mcDeviceSynchronize() == mcSuccess ? 0 : -1;
}

/* 消费数据。返回 checksum,供正确性校验。 */
int
gpu_launch_consume(const void *buf, size_t len, unsigned long long *checksum)
{
	static unsigned long long *d_out;
	unsigned long long zero = 0;
	size_t nvec = len / sizeof(uint4);
	int blocks;

	if (!d_out) {
		if (mcMalloc((void **)&d_out, sizeof(*d_out)) != mcSuccess) {
			return -1;
		}
	}
	mcMemcpy(d_out, &zero, sizeof(zero), mcMemcpyHostToDevice);

	/* grid 开够大让 SM 填满,但每线程至少处理几个向量,
	 * 否则归约的开销盖过读取本身 */
	blocks = (int)((nvec + 255) / 256);
	if (blocks > 2048) {
		blocks = 2048;
	}
	if (blocks < 1) {
		blocks = 1;
	}

	k_consume<<<blocks, 256>>>((const uint4 *)buf, nvec, d_out);
	if (mcGetLastError() != mcSuccess) {
		return -1;
	}
	if (mcDeviceSynchronize() != mcSuccess) {
		return -1;
	}

	if (checksum) {
		mcMemcpy(checksum, d_out, sizeof(*checksum), mcMemcpyDeviceToHost);
	}
	return 0;
}

/* 空 kernel 的 launch + sync,用来标定固定开销 */
int
gpu_launch_empty(void)
{
	k_empty<<<1, 32>>>();
	if (mcGetLastError() != mcSuccess) {
		return -1;
	}
	return mcDeviceSynchronize() == mcSuccess ? 0 : -1;
}

}  /* extern "C" */
