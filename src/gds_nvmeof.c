/*
 * gds_nvmeof.c
 *
 * 验证目标:远端 NVMe SSD --RDMA--> 本地 GPU 显存,全程不经过主机内存。
 *
 * 路径:
 *     SPDK NVMe-oF initiator (用户态)
 *       -> RDMA QP (mlx5)
 *         -> payload buffer = GPU 显存 (通过 dma-buf 注册)
 *
 * 关键点:不需要给 SPDK 打 patch。SPDK 提供 spdk_nvme_rdma_hooks,
 * 允许调用方接管 PD 创建和内存注册。我们在 hook 里改用
 * ibv_reg_dmabuf_mr() 注册显存,SPDK 内部就只看到一个 rkey,
 * 完全不知道这块 buffer 是显存。
 *
 * 注意 SPDK 当前实现在 hooks 路径下是:
 *     _ctx->lkey = _ctx->rkey = (uint32_t)key;
 * 即 lkey 和 rkey 取同一个值。mlx5 的 MR 本来 lkey == rkey,
 * 所以 get_rkey() 直接返回 mr->rkey 即可,不需要打包成高低 32 位。
 *
 * 编译见 Makefile。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"

#include <infiniband/verbs.h>

#include "gpu_backend.h"

/* ================================================================== */
/* 配置                                                                */
/* ================================================================== */

#define MAX_MR_CACHE	4096
#define DEFAULT_BLOCK_SZ	(1u << 20)	/* 1 MiB */
#define DEFAULT_QD		32
#define DEFAULT_IO_COUNT	4096

struct config {
	char		traddr[64];	/* target IP */
	char		trsvcid[16];	/* target port */
	char		nqn[256];
	char		ib_dev[32];	/* mlx5_0 ... */
	int		gpu_id;
	uint32_t	block_sz;
	uint32_t	qd;
	uint32_t	io_count;
	bool		do_write;
	bool		verify;
	bool		use_host_mem;	/* 对照组:走主机内存 */
};

static struct config g_cfg = {
	.trsvcid	= "4420",
	.ib_dev		= "",	/* 留空:实际网卡由路由决定,-d 仅作校验提示 */
	.gpu_id		= 0,
	.block_sz	= DEFAULT_BLOCK_SZ,
	.qd		= DEFAULT_QD,
	.io_count	= DEFAULT_IO_COUNT,
	.do_write	= false,
	.verify		= false,
	.use_host_mem	= false,
};

/* ================================================================== */
/* 全局状态                                                            */
/* ================================================================== */

struct mr_entry {
	uint64_t	addr;
	size_t		len;
	struct ibv_mr	*mr;
	bool		is_gpu;
};

static struct {
	struct ibv_context	*verbs;
	struct ibv_pd		*pd;

	/* GPU 侧 */
	gpu_ptr_t		gpu_buf;
	size_t			gpu_buf_len;
	int			dmabuf_fd;

	/* 对照组用的主机内存 */
	void			*host_buf;

	/* MR 缓存:SPDK 会对它自己的内部 buffer 也调 get_rkey,
	 * 所以这里要同时能处理显存和主机内存两种情况 */
	struct mr_entry		mr_cache[MAX_MR_CACHE];
	int			mr_count;
	pthread_mutex_t		mr_lock;
} g_ctx = {
	.dmabuf_fd	= -1,
	.mr_lock	= PTHREAD_MUTEX_INITIALIZER,
};

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;

static volatile uint32_t	g_io_outstanding;
static volatile uint32_t	g_io_completed;
static volatile uint32_t	g_io_failed;

/* ================================================================== */
/* MR 管理                                                             */
/* ================================================================== */

static struct mr_entry *
mr_cache_lookup(uint64_t addr, size_t len)
{
	int i;

	for (i = 0; i < g_ctx.mr_count; i++) {
		struct mr_entry *e = &g_ctx.mr_cache[i];

		if (addr >= e->addr && addr + len <= e->addr + e->len) {
			return e;
		}
	}
	return NULL;
}

/*
 * 注册显存。这是整个程序的核心一步。
 *
 * ibv_reg_dmabuf_mr() 的语义:
 *   offset  - 在 dma_buf 内的偏移
 *   length  - 长度
 *   iova    - 希望网卡看到的虚拟地址。这里必须传 GPU VA,
 *             因为 SPDK 会把 GPU VA 直接填进 SGE 的 addr 字段。
 *             传错会导致 remote access error 或者静默读到垃圾数据。
 *   fd      - dma_buf fd
 */
static int
register_gpu_mr(struct ibv_pd *pd)
{
	struct mr_entry *e;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE |
		     IBV_ACCESS_RELAXED_ORDERING;

	if (g_ctx.mr_count >= MAX_MR_CACHE) {
		fprintf(stderr, "MR cache full\n");
		return -1;
	}

	e = &g_ctx.mr_cache[g_ctx.mr_count];

	e->mr = ibv_reg_dmabuf_mr(pd,
				  0,				/* offset */
				  g_ctx.gpu_buf_len,		/* length */
				  (uint64_t)g_ctx.gpu_buf,	/* iova = GPU VA */
				  g_ctx.dmabuf_fd,
				  access);
	if (!e->mr) {
		fprintf(stderr,
			"ibv_reg_dmabuf_mr failed: %s (errno=%d)\n"
			"  常见原因:\n"
			"    - 内核 < 5.12 或 ib_core 无 dma-buf 支持\n"
			"    - rdma-core 版本过老,没有该 verb\n"
			"    - GPU 驱动导出的 dma_buf 不支持 peer-to-peer 映射\n"
			"    - ACS 未关闭\n",
			strerror(errno), errno);
		return -1;
	}

	e->addr   = (uint64_t)g_ctx.gpu_buf;
	e->len    = g_ctx.gpu_buf_len;
	e->is_gpu = true;
	g_ctx.mr_count++;

	printf("[MR] GPU dmabuf MR registered: va=0x%lx len=%zu "
	       "lkey=0x%x rkey=0x%x\n",
	       e->addr, e->len, e->mr->lkey, e->mr->rkey);

	/* 顺手做个一致性检查:mlx5 上这两个应该相等。
	 * 如果不等,说明 SPDK hooks 那条 lkey=rkey 的假设不成立,
	 * 需要改用非 hooks 路径。 */
	if (e->mr->lkey != e->mr->rkey) {
		fprintf(stderr,
			"[WARN] lkey != rkey,SPDK hooks 路径会用错 key!\n");
	}

	return 0;
}

/* 主机内存的普通注册,用于 SPDK 内部 buffer 和对照组 */
static struct mr_entry *
register_host_mr(struct ibv_pd *pd, void *buf, size_t len)
{
	struct mr_entry *e;
	int access = IBV_ACCESS_LOCAL_WRITE |
		     IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE;

	if (g_ctx.mr_count >= MAX_MR_CACHE) {
		fprintf(stderr,
			"[MR] 缓存满 (%d 条)!后续注册会返回 rkey=0,\n"
			"     表现为 local protection error。请加大 MAX_MR_CACHE。\n",
			MAX_MR_CACHE);
		return NULL;
	}

	e = &g_ctx.mr_cache[g_ctx.mr_count];
	e->mr = ibv_reg_mr(pd, buf, len, access);
	if (!e->mr) {
		fprintf(stderr, "ibv_reg_mr(host) failed: %s\n", strerror(errno));
		return NULL;
	}

	e->addr   = (uint64_t)buf;
	e->len    = len;
	e->is_gpu = false;
	g_ctx.mr_count++;

	return e;
}

/* ================================================================== */
/* SPDK RDMA hooks                                                     */
/* ================================================================== */

static struct ibv_pd *
hook_get_ibv_pd(const struct spdk_nvme_transport_id *trid,
		struct ibv_context *verbs)
{
	/*
	 * 关键设计点。
	 *
	 * 早先的写法是自己 ibv_open_device + ibv_alloc_pd,然后在这里
	 * 校验 SPDK 传进来的 verbs 是不是同一个 —— 这是错的。
	 * SPDK 通过 rdma_cm 建连时会自己打开设备,即使是同一张物理卡,
	 * 拿到的 ibv_context 指针也和我们的不同。指针比较必然失败。
	 *
	 * 更根本的是:MR 和 QP 必须属于同一个 PD,而 PD 又绑定在
	 * 具体的 ibv_context 上。跨 context 混用 PD 的结果就是
	 * local protection error。
	 *
	 * 所以正确的方向是反过来 —— 不自己开设备,用 SPDK 给的
	 * context 建 PD,MR 延迟到 get_rkey 里再注册。
	 */
	pthread_mutex_lock(&g_ctx.mr_lock);

	if (!g_ctx.pd) {
		g_ctx.verbs = verbs;
		g_ctx.pd = ibv_alloc_pd(verbs);
		if (!g_ctx.pd) {
			fprintf(stderr, "[HOOK] ibv_alloc_pd 失败: %s\n",
				strerror(errno));
			pthread_mutex_unlock(&g_ctx.mr_lock);
			return NULL;
		}

		printf("[HOOK] SPDK 选定网卡 %s,PD=%p\n",
		       ibv_get_device_name(verbs->device), (void *)g_ctx.pd);

		/* -d 只作提示,实际用哪张卡由路由决定 */
		if (g_cfg.ib_dev[0] &&
		    strcmp(ibv_get_device_name(verbs->device),
			   g_cfg.ib_dev) != 0) {
			printf("[HOOK] 注意:-d 指定的是 %s,与实际不同。"
			       "以实际为准。\n", g_cfg.ib_dev);
		}
	}

	pthread_mutex_unlock(&g_ctx.mr_lock);
	return g_ctx.pd;
}

static uint64_t
hook_get_rkey(struct ibv_pd *pd, void *buf, size_t size)
{
	struct mr_entry *e;
	uint64_t key = 0;
	uint64_t addr = (uint64_t)buf;

	pthread_mutex_lock(&g_ctx.mr_lock);

	e = mr_cache_lookup(addr, size);

	if (!e) {
		bool is_gpu = g_ctx.gpu_buf &&
			      addr >= (uint64_t)g_ctx.gpu_buf &&
			      addr + size <= (uint64_t)g_ctx.gpu_buf +
					     g_ctx.gpu_buf_len;

		if (is_gpu) {
			/* 显存首次被用到,现在才有 PD 可用,延迟注册 */
			if (register_gpu_mr(pd) != 0) {
				pthread_mutex_unlock(&g_ctx.mr_lock);
				return 0;
			}
			e = mr_cache_lookup(addr, size);
		} else {
			/* SPDK 自己的内部 buffer(命令块、SGL 描述符等) */
			e = register_host_mr(pd, buf, size);
		}

		if (!e) {
			pthread_mutex_unlock(&g_ctx.mr_lock);
			return 0;
		}
	}

	/* mlx5: lkey == rkey,SPDK 会把这个值同时当 lkey 和 rkey 用 */
	key = e->mr->rkey;

	pthread_mutex_unlock(&g_ctx.mr_lock);
	return key;
}

static void
hook_put_rkey(uint64_t key)
{
	/* 我们全程缓存 MR,程序退出时统一 dereg,这里不做事 */
}

static struct spdk_nvme_rdma_hooks g_hooks = {
	.get_ibv_pd	= hook_get_ibv_pd,
	.get_rkey	= hook_get_rkey,
	.put_rkey	= hook_put_rkey,
};

/* ================================================================== */
/* GPU 显存准备                                                        */
/* ================================================================== */

static int
gpu_buffer_setup(size_t len)
{
	int fd = -1;
	int rc;

	/* runtime API 不需要显式建 context,setDevice 即可 */
	GPU_CHECK(gpu_set_device(g_cfg.gpu_id));

	GPU_CHECK(gpu_mem_alloc(&g_ctx.gpu_buf, len));
	g_ctx.gpu_buf_len = len;

	printf("[GPU] backend=%s dev=%d buf=%p len=%zu\n",
	       GPU_BACKEND_NAME, g_cfg.gpu_id, g_ctx.gpu_buf, len);

	/*
	 * 导出 dma_buf fd。
	 *
	 * 实测确认:普通 mcMalloc 分配的显存即可导出,
	 * 不需要走 VMM 三段式(mcMemCreate + mcMemMap)。
	 */
	GPU_CHECK(gpu_mem_get_dmabuf_fd(&fd, g_ctx.gpu_buf, len));

	g_ctx.dmabuf_fd = fd;
	printf("[GPU] dma_buf exported, fd=%d\n", fd);

	/*
	 * 关键一步:把显存登记进 SPDK 的内存映射表。
	 *
	 * SPDK 内部维护一张 spdk_mem_map,I/O 提交时按虚拟地址查表
	 * 拿翻译结果。只有登记过的地址范围才会触发 rdma_utils 的
	 * notify 回调,进而调用我们的 hooks->get_rkey。
	 *
	 * spdk_dma_zmalloc 的主机内存是自动登记的,所以 -H 模式能跑;
	 * 而 mcMalloc 出来的显存 SPDK 一无所知,查表落空后 SGL 里的
	 * key 就是 0,表现为 target 侧 local protection error。
	 *
	 * 要求地址和长度按 2 MiB 对齐 —— mcMalloc 的返回值通常已满足。
	 */
	rc = spdk_mem_register(g_ctx.gpu_buf, len);
	if (rc != 0) {
		fprintf(stderr,
			"spdk_mem_register 失败: %d (%s)\n"
			"  va=%p len=%zu\n"
			"  地址或长度可能未按 2 MiB 对齐\n",
			rc, spdk_strerror(-rc), g_ctx.gpu_buf, len);
		return -1;
	}
	printf("[GPU] 已登记进 SPDK 内存映射表\n");

	return 0;
}


/* ================================================================== */
/* SPDK probe / attach                                                 */
/* ================================================================== */

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	printf("[NVMe] probing %s (nqn=%s)\n", trid->traddr, trid->subnqn);

	/* 队列数给足,默认值打不满 200G */
	opts->num_io_queues = 8;

	return true;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr,
	  const struct spdk_nvme_ctrlr_opts *opts)
{
	int nsid;

	g_ctrlr = ctrlr;

	for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr);
	     nsid != 0;
	     nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
		struct spdk_nvme_ns *ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);

		if (!spdk_nvme_ns_is_active(ns)) {
			continue;
		}

		printf("[NVMe] ns %d: size=%lu MiB, sector=%u B\n",
		       nsid,
		       spdk_nvme_ns_get_size(ns) / (1024 * 1024),
		       spdk_nvme_ns_get_sector_size(ns));

		if (!g_ns) {
			g_ns = ns;
		}
	}
}

/* ================================================================== */
/* I/O                                                                 */
/* ================================================================== */

static void
io_complete_cb(void *arg, const struct spdk_nvme_cpl *cpl)
{
	g_io_outstanding--;

	if (spdk_nvme_cpl_is_error(cpl)) {
		g_io_failed++;
		if (g_io_failed < 5) {
			fprintf(stderr, "[IO] error: sct=%d sc=%d\n",
				cpl->status.sct, cpl->status.sc);
		}
	} else {
		g_io_completed++;
	}
}

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static int
run_io(void)
{
	uint32_t sector_sz  = spdk_nvme_ns_get_sector_size(g_ns);
	uint32_t lba_count  = g_cfg.block_sz / sector_sz;
	uint64_t max_lba    = spdk_nvme_ns_get_num_sectors(g_ns);
	uint32_t submitted  = 0;
	uint64_t t0, t1;
	void *payload;
	uint32_t slots;

	if (g_cfg.block_sz % sector_sz != 0) {
		fprintf(stderr, "block_sz 必须是 sector_size(%u) 的整数倍\n",
			sector_sz);
		return -1;
	}

	payload = g_cfg.use_host_mem ?
		  g_ctx.host_buf : g_ctx.gpu_buf;

	/* buffer 里能放下几个 QD 槽位 */
	slots = g_ctx.gpu_buf_len / g_cfg.block_sz;
	if (slots < g_cfg.qd) {
		fprintf(stderr,
			"buffer 太小,只够 %u 个槽位,qd=%u。加大 buffer 或降 qd\n",
			slots, g_cfg.qd);
		return -1;
	}

	printf("\n[IO] %s, bs=%u KiB, qd=%u, count=%u, payload=%s\n",
	       g_cfg.do_write ? "WRITE" : "READ",
	       g_cfg.block_sz / 1024, g_cfg.qd, g_cfg.io_count,
	       g_cfg.use_host_mem ? "HOST DRAM" : "GPU HBM");

	t0 = now_ns();

	while (g_io_completed + g_io_failed < g_cfg.io_count) {

		while (g_io_outstanding < g_cfg.qd &&
		       submitted < g_cfg.io_count) {
			uint64_t lba;
			void *buf;
			int rc;

			/* 每个 in-flight I/O 用 buffer 里不同的槽位,
			 * 避免多个 I/O 写同一块显存导致校验失效 */
			buf = (char *)payload +
			      (uint64_t)(submitted % slots) * g_cfg.block_sz;

			lba = ((uint64_t)submitted * lba_count) %
			      (max_lba - lba_count);

			if (g_cfg.do_write) {
				rc = spdk_nvme_ns_cmd_write(g_ns, g_qpair, buf,
							    lba, lba_count,
							    io_complete_cb,
							    NULL, 0);
			} else {
				rc = spdk_nvme_ns_cmd_read(g_ns, g_qpair, buf,
							   lba, lba_count,
							   io_complete_cb,
							   NULL, 0);
			}

			if (rc != 0) {
				if (rc == -ENOMEM) {
					break;	/* 队列满,先收割 */
				}
				fprintf(stderr, "submit failed: %d\n", rc);
				return -1;
			}

			g_io_outstanding++;
			submitted++;
		}

		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}

	t1 = now_ns();

	{
		double sec  = (t1 - t0) / 1e9;
		double bytes = (double)g_io_completed * g_cfg.block_sz;
		double gbps = bytes / sec / (1000.0 * 1000.0 * 1000.0);
		double iops = g_io_completed / sec;

		printf("\n===== 结果 =====\n");
		printf("  完成       : %u\n", g_io_completed);
		printf("  失败       : %u\n", g_io_failed);
		printf("  耗时       : %.3f s\n", sec);
		printf("  带宽       : %.2f GB/s\n", gbps);
		printf("  IOPS       : %.0f\n", iops);
		printf("  平均延迟   : %.1f us\n",
		       sec * 1e6 / g_io_completed * g_cfg.qd);
		printf("  已注册 MR  : %d / %d\n", g_ctx.mr_count, MAX_MR_CACHE);
	}

	return g_io_failed ? -1 : 0;
}

/* ================================================================== */
/* 数据正确性校验                                                      */
/* ================================================================== */

/*
 * 写-读回校验。这一步很重要:带宽跑起来不代表数据是对的。
 * 如果 iova 传错、或者 dma_buf 映射有偏移问题,带宽照样好看,
 * 但读回来的是垃圾。
 */
static int
verify_data(void)
{
	size_t len = g_cfg.block_sz;
	uint32_t sector_sz = spdk_nvme_ns_get_sector_size(g_ns);
	uint32_t lba_count = len / sector_sz;
	unsigned char *host_src, *host_dst;
	size_t i;
	int rc = 0;

	printf("\n[VERIFY] 写入 pattern 再读回比对...\n");

	host_src = malloc(len);
	host_dst = malloc(len);
	if (!host_src || !host_dst) {
		free(host_src);
		free(host_dst);
		return -1;
	}

	for (i = 0; i < len; i++) {
		host_src[i] = (unsigned char)((i * 31 + 7) & 0xff);
	}

	/* host -> GPU */
	GPU_CHECK(gpu_memcpy_htod(g_ctx.gpu_buf, host_src, len));
	GPU_CHECK(gpu_synchronize());

	/* GPU -> 远端盘 */
	g_io_outstanding = 1;
	g_io_completed = g_io_failed = 0;
	if (spdk_nvme_ns_cmd_write(g_ns, g_qpair,
				   g_ctx.gpu_buf,
				   0, lba_count, io_complete_cb, NULL, 0) != 0) {
		fprintf(stderr, "verify write submit failed\n");
		rc = -1;
		goto out;
	}
	while (g_io_outstanding) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}
	if (g_io_failed) {
		fprintf(stderr, "verify write failed\n");
		rc = -1;
		goto out;
	}

	/* 把显存清掉,确保读回来的确实来自盘 */
	GPU_CHECK(gpu_memset(g_ctx.gpu_buf, 0xBB, len));
	GPU_CHECK(gpu_synchronize());

	/* 远端盘 -> GPU */
	g_io_outstanding = 1;
	g_io_completed = g_io_failed = 0;
	if (spdk_nvme_ns_cmd_read(g_ns, g_qpair,
				  g_ctx.gpu_buf,
				  0, lba_count, io_complete_cb, NULL, 0) != 0) {
		fprintf(stderr, "verify read submit failed\n");
		rc = -1;
		goto out;
	}
	while (g_io_outstanding) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}
	if (g_io_failed) {
		fprintf(stderr, "verify read failed\n");
		rc = -1;
		goto out;
	}

	/* GPU -> host,比对 */
	GPU_CHECK(gpu_memcpy_dtoh(host_dst, g_ctx.gpu_buf, len));
	GPU_CHECK(gpu_synchronize());

	if (memcmp(host_src, host_dst, len) == 0) {
		printf("[VERIFY] PASS —— %zu 字节完全一致\n", len);
	} else {
		size_t first_bad = 0;

		for (i = 0; i < len; i++) {
			if (host_src[i] != host_dst[i]) {
				first_bad = i;
				break;
			}
		}
		printf("[VERIFY] FAIL —— 首个不一致字节 offset=%zu "
		       "(期望 0x%02x, 实际 0x%02x)\n",
		       first_bad, host_src[first_bad], host_dst[first_bad]);

		if (host_dst[first_bad] == 0xBB) {
			printf("         读回的是 memset 的值,说明 DMA 根本没落到显存。\n"
			       "         检查 iova 是否传的 GPU VA、ACS 是否关闭。\n");
		}
		rc = -1;
	}

out:
	free(host_src);
	free(host_dst);
	return rc;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */

static void
usage(const char *prog)
{
	printf("用法: %s -a <target_ip> -n <nqn> [选项]\n\n", prog);
	printf("  -a <ip>      target 地址(必填)\n");
	printf("  -s <port>    target 端口,默认 4420\n");
	printf("  -n <nqn>     subsystem NQN(必填)\n");
	printf("  -d <dev>     IB 设备名,默认 mlx5_0\n");
	printf("  -g <id>      GPU 编号,默认 0\n");
	printf("  -b <bytes>   block size,默认 1048576\n");
	printf("  -q <qd>      queue depth,默认 32\n");
	printf("  -c <count>   I/O 次数,默认 4096\n");
	printf("  -w           写测试(默认读)\n");
	printf("  -V           跑数据校验\n");
	printf("  -H           对照组:用主机内存而非显存\n");
	printf("\n示例:\n");
	printf("  %s -a 192.168.1.100 -n nqn.2024-01.io.spdk:cnode1 \\\n", prog);
	printf("       -d mlx5_0 -g 0 -b 1048576 -q 32 -V\n");
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	size_t buf_len;
	int op, rc = 0;

	while ((op = getopt(argc, argv, "a:s:n:d:g:b:q:c:wVHh")) != -1) {
		switch (op) {
		case 'a': snprintf(g_cfg.traddr,  sizeof(g_cfg.traddr),  "%s", optarg); break;
		case 's': snprintf(g_cfg.trsvcid, sizeof(g_cfg.trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_cfg.nqn,     sizeof(g_cfg.nqn),     "%s", optarg); break;
		case 'd': snprintf(g_cfg.ib_dev,  sizeof(g_cfg.ib_dev),  "%s", optarg); break;
		case 'g': g_cfg.gpu_id   = atoi(optarg); break;
		case 'b': g_cfg.block_sz = (uint32_t)atoi(optarg); break;
		case 'q': g_cfg.qd       = (uint32_t)atoi(optarg); break;
		case 'c': g_cfg.io_count = (uint32_t)atoi(optarg); break;
		case 'w': g_cfg.do_write = true; break;
		case 'V': g_cfg.verify   = true; break;
		case 'H': g_cfg.use_host_mem = true; break;
		default:  usage(argv[0]); return 1;
		}
	}

	if (!g_cfg.traddr[0] || !g_cfg.nqn[0]) {
		usage(argv[0]);
		return 1;
	}

	/* ---- SPDK env ----
	 * 注意 no_pci:我们是 NVMe-oF initiator,不接管本地 PCIe 设备。
	 * 不加这个,DPDK EAL 会去扫 PCI,有把网卡从内核抢走的风险 ——
	 * 而我们还需要内核态 mlx5_ib 来做 RDMA。 */
	spdk_env_opts_init(&opts);
	opts.name    = "gds_nvmeof";
	opts.opts_size = sizeof(opts);
	opts.no_pci  = true;

	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init failed\n");
		return 1;
	}
	spdk_log_set_print_level(SPDK_LOG_NOTICE);

	/*
	 * 注意:这里不再自己 ibv_open_device / ibv_alloc_pd。
	 * PD 由 hook_get_ibv_pd 用 SPDK 的 verbs context 创建,
	 * MR 由 hook_get_rkey 首次用到时延迟注册。
	 * 见 hook_get_ibv_pd 里的注释。
	 */

	/* ---- buffer 准备(只分配,不注册)---- */
	buf_len = (size_t)g_cfg.block_sz * g_cfg.qd * 2;	/* 留一倍余量 */

	if (g_cfg.use_host_mem) {
		g_ctx.host_buf = spdk_dma_zmalloc(buf_len, 4096, NULL);
		if (!g_ctx.host_buf) {
			fprintf(stderr, "host buffer alloc failed\n");
			return 1;
		}
		g_ctx.gpu_buf_len = buf_len;	/* 槽位计算共用这个字段 */
		printf("[HOST] 对照组模式,payload 走主机内存\n");
	} else {
		if (gpu_buffer_setup(buf_len) != 0) {
			return 1;
		}
	}

	/* ---- 装 hooks。必须在 probe 之前 ---- */
	spdk_nvme_rdma_init_hooks(&g_hooks);

	/* ---- 连 target ---- */
	trid.trtype = SPDK_NVME_TRANSPORT_RDMA;
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr,  sizeof(trid.traddr),  "%s", g_cfg.traddr);
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", g_cfg.trsvcid);
	snprintf(trid.subnqn,  sizeof(trid.subnqn),  "%s", g_cfg.nqn);

	if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) != 0) {
		fprintf(stderr, "spdk_nvme_probe failed\n");
		return 1;
	}

	if (!g_ns) {
		fprintf(stderr, "未找到可用 namespace\n");
		return 1;
	}

	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	if (!g_qpair) {
		fprintf(stderr, "alloc_io_qpair failed\n");
		return 1;
	}

	/* ---- 跑 ---- */
	if (g_cfg.verify && !g_cfg.use_host_mem) {
		if (verify_data() != 0) {
			rc = 1;
			goto cleanup;
		}
	}

	g_io_outstanding = g_io_completed = g_io_failed = 0;
	if (run_io() != 0) {
		rc = 1;
	}

cleanup:
	/*
	 * 清理顺序:先 detach,再放 verbs 资源。
	 *
	 * spdk_nvme_detach 内部还会发一条 FABRIC PROPERTY GET 读 CC
	 * 寄存器做优雅关闭,那时 QP 还在用 PD。如果提前 dealloc_pd,
	 * 就会看到关闭阶段的 local protection error。
	 *
	 * detach 之后 verbs context 可能已被 rdma_cm 销毁,此时再
	 * dereg_mr / dealloc_pd 不安全。索性交给进程退出时内核回收 ——
	 * 这些是 verbs 资源,fd 关闭时一并释放,不会泄漏。
	 */
	if (g_qpair) {
		spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	}
	if (g_ctrlr) {
		spdk_nvme_detach(g_ctrlr);
	}
	if (g_ctx.dmabuf_fd >= 0) {
		close(g_ctx.dmabuf_fd);
	}
	if (g_ctx.gpu_buf) {
		spdk_mem_unregister(g_ctx.gpu_buf, g_ctx.gpu_buf_len);
		gpu_mem_free(g_ctx.gpu_buf);
	}

	return rc;
}
