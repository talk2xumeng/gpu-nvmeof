/*
 * gpu_backend.h  --  GPU 后端抽象层
 *
 * MACA 分支的符号已通过 dmabuf_probe 实测确认:
 *   库名        libmcruntime.so
 *   分配        mcMalloc / mcFree          ← runtime API 风格,非 driver API
 *   dma_buf     mcMemGetHandleForAddressRange, handle type = 1
 *
 * 注意:MACA 的分配接口只有 runtime 风格(mcMalloc),没有 mcMemAlloc。
 * 所以显存指针类型是 void*,不是 CUdeviceptr 那样的整数句柄。
 * CUDA 分支为了统一,也改用 runtime API。
 */

#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

#include <stdint.h>
#include <stdio.h>

/* 统一的显存指针类型:两个后端都用 void* */
typedef void *gpu_ptr_t;

/* ------------------------------------------------------------------ */

#ifdef USE_MACA

#include <mcr/mc_runtime_api.h>

typedef mcError_t gpu_res_t;

/*
 * 实测确认的签名:
 *   mcError_t mcMemGetHandleForAddressRange(void *handle, void *dptr,
 *                                           size_t size, int handleType,
 *                                           unsigned long long flags);
 * handleType 是普通 int,没有具名枚举。实测 1 = DMA_BUF_FD,
 * 与 CUDA 的 CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD 取值一致。
 */

#define GPU_SUCCESS                  mcSuccess
#define GPU_BACKEND_NAME             "MACA"

#define gpu_set_device(d)            mcSetDevice(d)
#define gpu_mem_alloc(pp, sz)        mcMalloc((void **)(pp), sz)
#define gpu_mem_free(p)              mcFree(p)
#define gpu_memset(p, v, n)          mcMemset(p, v, n)
#define gpu_memcpy_htod(d, h, n)     mcMemcpy(d, h, n, mcMemcpyHostToDevice)
#define gpu_memcpy_dtoh(h, d, n)     mcMemcpy(h, d, n, mcMemcpyDeviceToHost)
#define gpu_synchronize()            mcDeviceSynchronize()

/* dma_buf 导出。handle type = 1,实测确认与 CUDA 枚举值一致。 */
#define GPU_DMABUF_HANDLE_TYPE       1
#define gpu_mem_get_dmabuf_fd(fdp, p, sz) \
	mcMemGetHandleForAddressRange((void *)(fdp), p, sz, \
				      GPU_DMABUF_HANDLE_TYPE, 0)

#else	/* CUDA */

#include <cuda_runtime.h>
#include <cuda.h>

typedef cudaError_t gpu_res_t;

#define GPU_SUCCESS                  cudaSuccess
#define GPU_BACKEND_NAME             "CUDA"

#define gpu_set_device(d)            cudaSetDevice(d)
#define gpu_mem_alloc(pp, sz)        cudaMalloc((void **)(pp), sz)
#define gpu_mem_free(p)              cudaFree(p)
#define gpu_memset(p, v, n)          cudaMemset(p, v, n)
#define gpu_memcpy_htod(d, h, n)     cudaMemcpy(d, h, n, cudaMemcpyHostToDevice)
#define gpu_memcpy_dtoh(h, d, n)     cudaMemcpy(h, d, n, cudaMemcpyDeviceToHost)
#define gpu_synchronize()            cudaDeviceSynchronize()

#define GPU_DMABUF_HANDLE_TYPE       CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD
#define gpu_mem_get_dmabuf_fd(fdp, p, sz) \
	cuMemGetHandleForAddressRange((void *)(fdp), (CUdeviceptr)(p), sz, \
				      GPU_DMABUF_HANDLE_TYPE, 0)

#endif	/* USE_MACA */

/* ------------------------------------------------------------------ */

#define GPU_CHECK(expr)							\
	do {								\
		gpu_res_t _r = (expr);					\
		if (_r != GPU_SUCCESS) {				\
			fprintf(stderr,					\
				"[GPU] %s failed at %s:%d, res=%d\n",	\
				#expr, __FILE__, __LINE__, (int)_r);	\
			return -1;					\
		}							\
	} while (0)

#endif /* GPU_BACKEND_H */
