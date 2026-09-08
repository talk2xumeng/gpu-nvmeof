/*
 * dmabuf_probe.c
 *
 * 一个问题:沐曦 GPU 的显存能不能通过 dma-buf 注册成 RDMA MR?
 *
 * 程序把两条路都跑一遍,直接给结论:
 *   路径 A: ibv_reg_mr(GPU_VA)          —— peer_mem,对应用透明
 *   路径 B: ibv_reg_dmabuf_mr(fd)       —— dma-buf,需要显式 export
 *
 * 设计上刻意不 #include 任何 MACA 头文件,全部用 dlopen + dlsym 动态解析。
 * 这样接口名不确定时也能编过,运行时逐个试候选名字。
 *
 * 编译:
 *     gcc -O2 -o dmabuf_probe dmabuf_probe.c -libverbs -ldl
 *
 * 运行:
 *     ./dmabuf_probe mlx5_1
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <infiniband/verbs.h>

#define BUF_SIZE	(2 * 1024 * 1024)	/* 2 MiB */

/* ------------------------------------------------------------------ */
/* MACA 运行时:全部动态解析,不依赖头文件                             */
/* ------------------------------------------------------------------ */

/* CUDA driver API 的对应关系(用于猜 MACA 的符号名):
 *   cuInit                          -> mcInit
 *   cuDeviceGet                     -> mcDeviceGet
 *   cuCtxCreate_v2                  -> mcCtxCreate
 *   cuMemAlloc_v2                   -> mcMemAlloc / mcMalloc
 *   cuMemFree_v2                    -> mcMemFree / mcFree
 *   cuMemGetHandleForAddressRange   -> mcMemGetHandleForAddressRange
 */

static void *g_lib;

typedef int  (*fn_init_t)(unsigned int);
typedef int  (*fn_dev_get_t)(int *, int);
typedef int  (*fn_ctx_create_t)(void **, unsigned int, int);
typedef int  (*fn_malloc_t)(void **, size_t);
typedef int  (*fn_free_t)(void *);
typedef int  (*fn_get_handle_t)(void *, void *, size_t, int, unsigned long long);

static fn_init_t		mc_init;
static fn_dev_get_t		mc_dev_get;
static fn_ctx_create_t		mc_ctx_create;
static fn_malloc_t		mc_malloc;
static fn_free_t		mc_free;
static fn_get_handle_t		mc_get_handle;

static const char *g_handle_sym_used;

/* 逐个试候选符号名,返回第一个找到的 */
static void *
try_syms(const char *const *names, const char **found)
{
	int i;

	for (i = 0; names[i]; i++) {
		void *p = dlsym(g_lib, names[i]);

		if (p) {
			if (found) {
				*found = names[i];
			}
			return p;
		}
	}
	return NULL;
}

static int
load_maca(void)
{
	static const char *const libs[] = {
		"libmcruntime.so",
		"libmcr.so",
		"libmaca.so",
		"libmcruntime.so.1",
		"/opt/maca/lib/libmcruntime.so",
		"/opt/maca/lib/libmcr.so",
		NULL
	};
	static const char *const s_init[]   = { "mcInit", "macaInit", NULL };
	static const char *const s_devget[] = { "mcDeviceGet", "macaDeviceGet", NULL };
	static const char *const s_ctx[]    = { "mcCtxCreate", "mcCtxCreate_v2",
						"macaCtxCreate", NULL };
	static const char *const s_malloc[] = { "mcMalloc", "mcMemAlloc",
						"mcMemAlloc_v2", "macaMalloc", NULL };
	static const char *const s_free[]   = { "mcFree", "mcMemFree",
						"mcMemFree_v2", "macaFree", NULL };
	static const char *const s_handle[] = {
		"mcMemGetHandleForAddressRange",
		"mcMemGetHandleForAddressRange_v2",
		"macaMemGetHandleForAddressRange",
		"mcMemGetDmabufFd",
		"mcMemExportToDmabuf",
		NULL
	};
	int i;

	for (i = 0; libs[i]; i++) {
		g_lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
		if (g_lib) {
			printf("  已加载 MACA 库: %s\n", libs[i]);
			break;
		}
	}
	if (!g_lib) {
		printf("  ✗ 找不到 MACA 运行时库。试过:\n");
		for (i = 0; libs[i]; i++) {
			printf("      %s\n", libs[i]);
		}
		printf("    请用 LD_LIBRARY_PATH 指定,或告诉我实际库名。\n");
		return -1;
	}

	mc_init       = (fn_init_t)       try_syms(s_init,   NULL);
	mc_dev_get    = (fn_dev_get_t)    try_syms(s_devget, NULL);
	mc_ctx_create = (fn_ctx_create_t) try_syms(s_ctx,    NULL);
	mc_malloc     = (fn_malloc_t)     try_syms(s_malloc, NULL);
	mc_free       = (fn_free_t)       try_syms(s_free,   NULL);
	mc_get_handle = (fn_get_handle_t) try_syms(s_handle, &g_handle_sym_used);

	printf("  符号解析:\n");
	printf("    init            : %s\n", mc_init       ? "OK" : "缺失");
	printf("    device_get      : %s\n", mc_dev_get    ? "OK" : "缺失");
	printf("    ctx_create      : %s\n", mc_ctx_create ? "OK" : "缺失");
	printf("    malloc          : %s\n", mc_malloc     ? "OK" : "缺失");
	printf("    get_handle      : %s%s%s\n",
	       mc_get_handle ? "OK (" : "缺失",
	       mc_get_handle ? g_handle_sym_used : "",
	       mc_get_handle ? ")" : "");

	if (!mc_malloc) {
		printf("  ✗ 连显存分配接口都找不到,库可能不对。\n");
		return -1;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* RDMA                                                                */
/* ------------------------------------------------------------------ */

static struct ibv_context *g_verbs;
static struct ibv_pd      *g_pd;

static int
rdma_setup(const char *devname)
{
	struct ibv_device **list;
	struct ibv_device *dev = NULL;
	int num, i;

	list = ibv_get_device_list(&num);
	if (!list) {
		printf("  ✗ ibv_get_device_list 失败\n");
		return -1;
	}

	for (i = 0; i < num; i++) {
		if (strcmp(ibv_get_device_name(list[i]), devname) == 0) {
			dev = list[i];
			break;
		}
	}
	if (!dev) {
		printf("  ✗ 找不到设备 %s。可用设备:", devname);
		for (i = 0; i < num; i++) {
			printf(" %s", ibv_get_device_name(list[i]));
		}
		printf("\n");
		ibv_free_device_list(list);
		return -1;
	}

	g_verbs = ibv_open_device(dev);
	ibv_free_device_list(list);
	if (!g_verbs) {
		printf("  ✗ ibv_open_device 失败\n");
		return -1;
	}

	g_pd = ibv_alloc_pd(g_verbs);
	if (!g_pd) {
		printf("  ✗ ibv_alloc_pd 失败\n");
		return -1;
	}

	printf("  设备 %s 打开成功\n", devname);
	return 0;
}

/* ------------------------------------------------------------------ */
/* 主流程                                                              */
/* ------------------------------------------------------------------ */

static const int ACCESS_FLAGS =
	IBV_ACCESS_LOCAL_WRITE  |
	IBV_ACCESS_REMOTE_READ  |
	IBV_ACCESS_REMOTE_WRITE |
	IBV_ACCESS_RELAXED_ORDERING;

int
main(int argc, char **argv)
{
	const char *devname = (argc > 1) ? argv[1] : "mlx5_1";
	int gpu_id = (argc > 2) ? atoi(argv[2]) : 0;
	void *gpu_buf = NULL;
	void *ctx = NULL;
	int dev = 0;
	int fd = -1;
	struct ibv_mr *mr_peer = NULL;
	struct ibv_mr *mr_dmabuf = NULL;
	int rc;

	printf("=========================================================\n");
	printf(" 沐曦显存 RDMA 注册路径探测\n");
	printf(" 设备=%s  GPU=%d  size=%d MiB\n",
	       devname, gpu_id, BUF_SIZE / (1024 * 1024));
	printf("=========================================================\n\n");

	/* ---- 1. 加载 MACA ---- */
	printf("[1] 加载 MACA 运行时\n");
	if (load_maca() != 0) {
		return 1;
	}
	printf("\n");

	/* ---- 2. 初始化 GPU ---- */
	printf("[2] 初始化 GPU\n");
	if (mc_init) {
		rc = mc_init(0);
		printf("  init: rc=%d\n", rc);
	}
	if (mc_dev_get && mc_ctx_create) {
		rc = mc_dev_get(&dev, gpu_id);
		printf("  device_get(%d): rc=%d\n", gpu_id, rc);
		rc = mc_ctx_create(&ctx, 0, dev);
		printf("  ctx_create: rc=%d\n", rc);
	}

	rc = mc_malloc(&gpu_buf, BUF_SIZE);
	if (rc != 0 || !gpu_buf) {
		printf("  ✗ 显存分配失败 rc=%d\n", rc);
		printf("    如果 ctx_create 没跑(runtime API 不需要),这里也可能失败,\n");
		printf("    试试 mcSetDevice 之类。\n");
		return 1;
	}
	printf("  ✓ 显存分配成功 va=%p len=%d\n\n", gpu_buf, BUF_SIZE);

	/* ---- 3. RDMA ---- */
	printf("[3] 打开 RDMA 设备\n");
	if (rdma_setup(devname) != 0) {
		return 1;
	}
	printf("\n");

	/* ---- 4. 路径 A: peer_mem ---- */
	printf("[4] 路径 A —— peer_mem (ibv_reg_mr 直接传 GPU VA)\n");
	errno = 0;
	mr_peer = ibv_reg_mr(g_pd, gpu_buf, BUF_SIZE, ACCESS_FLAGS);
	if (mr_peer) {
		printf("  ✓ 成功  lkey=0x%x rkey=0x%x\n",
		       mr_peer->lkey, mr_peer->rkey);
		if (mr_peer->lkey != mr_peer->rkey) {
			printf("  ! lkey != rkey,SPDK hooks 那条 lkey=rkey 假设不成立\n");
		}
	} else {
		printf("  ✗ 失败: %s (errno=%d)\n", strerror(errno), errno);
	}
	printf("\n");

	/* ---- 5. 路径 B: dma-buf ---- */
	printf("[5] 路径 B —— dma-buf\n");

	if (!mc_get_handle) {
		printf("  ✗ 找不到 dma_buf 导出接口\n");
		printf("    → 沐曦 runtime 未提供该能力,或符号名不在候选列表里\n");
		printf("    需要向驱动组确认接口名称\n");
	} else {
		/* CUDA 的 CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD = 1,
		 * MACA 大概率沿用同一个值。不行的话试 0 和 2。 */
		int type;
		int ok = 0;

		for (type = 1; type <= 2 && !ok; type++) {
			errno = 0;
			rc = mc_get_handle(&fd, gpu_buf, BUF_SIZE, type, 0);
			printf("  get_handle(type=%d): rc=%d fd=%d\n", rc, type, fd);
			if (rc == 0 && fd > 0) {
				ok = 1;
			}
		}

		if (!ok) {
			printf("  ✗ dma_buf 导出失败\n");
			printf("    可能原因:\n");
			printf("      - handle type 枚举值不对(试过 1、2)\n");
			printf("      - 普通 malloc 的显存不支持导出,\n");
			printf("        必须走 VMM API (mcMemCreate + mcMemMap)\n");
		} else {
			printf("  ✓ dma_buf 导出成功 fd=%d\n", fd);

			errno = 0;
			mr_dmabuf = ibv_reg_dmabuf_mr(g_pd, 0, BUF_SIZE,
						      (uint64_t)gpu_buf, fd,
						      ACCESS_FLAGS);
			if (mr_dmabuf) {
				printf("  ✓ ibv_reg_dmabuf_mr 成功  "
				       "lkey=0x%x rkey=0x%x\n",
				       mr_dmabuf->lkey, mr_dmabuf->rkey);
			} else {
				printf("  ✗ ibv_reg_dmabuf_mr 失败: %s (errno=%d)\n",
				       strerror(errno), errno);
				if (errno == EOPNOTSUPP) {
					printf("    → 内核或 rdma-core 不支持\n");
				} else if (errno == EINVAL) {
					printf("    → 导出成功但网卡无法映射,\n");
					printf("      通常是 GPU 驱动的 dma_buf 不支持 P2P,\n");
					printf("      或 ACS 未关闭。这条值得反馈给驱动组。\n");
				}
			}
		}
	}
	printf("\n");

	/* ---- 6. 结论 ---- */
	printf("=========================================================\n");
	printf(" 结论\n");
	printf("---------------------------------------------------------\n");
	printf("  peer_mem  : %s\n", mr_peer   ? "可用" : "不可用");
	printf("  dma-buf   : %s\n", mr_dmabuf ? "可用" : "不可用");
	printf("---------------------------------------------------------\n");

	if (mr_dmabuf) {
		printf("  → SPDK 程序按原样使用 ibv_reg_dmabuf_mr,不用改。\n");
	} else if (mr_peer) {
		printf("  → dma-buf 不可用,SPDK 程序改走 peer_mem:\n");
		printf("     register_gpu_mr() 里换成\n");
		printf("       ibv_reg_mr(pd, (void *)gpu_buf, len, access)\n");
		printf("     其余(hooks、get_rkey、I/O、校验)完全不用动。\n");
	} else {
		printf("  → 两条路都不通,先解决显存注册再谈 NVMe-oF。\n");
		printf("     检查 metax 驱动是否加载、ACS 是否关闭。\n");
	}
	printf("=========================================================\n");

	/* ---- 清理 ---- */
	if (mr_dmabuf) {
		ibv_dereg_mr(mr_dmabuf);
	}
	if (mr_peer) {
		ibv_dereg_mr(mr_peer);
	}
	if (fd > 0) {
		close(fd);
	}
	if (g_pd) {
		ibv_dealloc_pd(g_pd);
	}
	if (g_verbs) {
		ibv_close_device(g_verbs);
	}
	if (gpu_buf && mc_free) {
		mc_free(gpu_buf);
	}

	return mr_dmabuf ? 0 : (mr_peer ? 2 : 1);
}
