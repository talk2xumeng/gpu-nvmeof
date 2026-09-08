/*
 * wqe_verify.c
 *
 * 把 wqe_build.h 构造的 WQE,和 SPDK 通过 ibv_post_send 实际写进 SQ 的
 * 内容逐字节比对。
 *
 * 这是搬进 kernel 之前的安全网。WQE 的位域填错一个,网卡要么静默丢弃,
 * 要么回一个含糊的 CQE error —— 在 kernel 里排查这种问题极其痛苦。
 * 先在 host 上比对,对上了再原样搬过去。
 *
 * 做法:
 *   1. 用 SPDK 建 NVMe-oF 连接(Fabric Connect 已完成)
 *   2. 打过 patch 的 spdk_nvme_qpair_get_ibv_qp 拿到底层 QP
 *   3. mlx5dv_init_obj 挖出 SQ buffer / dbrec / UAR
 *   4. 记下提交前的 dbrec,发一次读,再记下提交后的 dbrec
 *      —— 差值告诉我们 SPDK 用了哪个 slot
 *   5. dump 那个 slot,和自己构造的对比
 *
 * 有些字段本来就会不同(cid 由 SPDK 分配,胶囊地址是 SPDK 自己的
 * 命令 buffer),程序会区分"预期不同"和"真的错了"。
 */

#define _GNU_SOURCE

#include "spdk/stdinc.h"
#include "spdk/nvme.h"
#include "spdk/env.h"

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#include "gpu_backend.h"
#include "wqe_build.h"

/* ================================================================== */

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;

static struct ibv_pd		*g_pd;
static struct ibv_context	*g_verbs;
static struct ibv_mr		*g_gpu_mr;

static void			*g_gpu_buf;
static size_t			g_gpu_len;
static int			g_dmabuf_fd = -1;

static volatile int		g_io_done, g_io_err;

static char			g_traddr[64], g_nqn[224];
static char			g_trsvcid[16] = "4420";
static int			g_gpu_id = 4;

/* ================================================================== */
/* SPDK hooks(和 gds_nvmeof 相同的延迟注册策略)                       */
/* ================================================================== */

static struct ibv_pd *
hook_get_ibv_pd(const struct spdk_nvme_transport_id *trid,
		struct ibv_context *verbs)
{
	if (!g_pd) {
		g_verbs = verbs;
		g_pd = ibv_alloc_pd(verbs);
		printf("[HOOK] 网卡 %s\n", ibv_get_device_name(verbs->device));
	}
	return g_pd;
}

static uint64_t
hook_get_rkey(struct ibv_pd *pd, void *buf, size_t size)
{
	static struct ibv_mr *host_mrs[256];
	static void *host_addrs[256];
	static size_t host_lens[256];
	static int n;
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
		     IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_RELAXED_ORDERING;
	uint64_t addr = (uint64_t)buf;
	int i;

	if (g_gpu_buf && addr >= (uint64_t)g_gpu_buf &&
	    addr + size <= (uint64_t)g_gpu_buf + g_gpu_len) {
		if (!g_gpu_mr) {
			g_gpu_mr = ibv_reg_dmabuf_mr(pd, 0, g_gpu_len,
						     (uint64_t)g_gpu_buf,
						     g_dmabuf_fd, access);
			if (g_gpu_mr) {
				printf("[MR] 显存 rkey=0x%x\n", g_gpu_mr->rkey);
			}
		}
		return g_gpu_mr ? g_gpu_mr->rkey : 0;
	}

	for (i = 0; i < n; i++) {
		if (addr >= (uint64_t)host_addrs[i] &&
		    addr + size <= (uint64_t)host_addrs[i] + host_lens[i]) {
			return host_mrs[i]->rkey;
		}
	}
	if (n >= 256) {
		return 0;
	}
	host_mrs[n] = ibv_reg_mr(pd, buf, size, access);
	if (!host_mrs[n]) {
		return 0;
	}
	host_addrs[n] = buf;
	host_lens[n] = size;
	return host_mrs[n++]->rkey;
}

static void hook_put_rkey(uint64_t k) { }

static struct spdk_nvme_rdma_hooks g_hooks = {
	.get_ibv_pd = hook_get_ibv_pd,
	.get_rkey   = hook_get_rkey,
	.put_rkey   = hook_put_rkey,
};

/* ================================================================== */

static bool
probe_cb(void *c, const struct spdk_nvme_transport_id *t,
	 struct spdk_nvme_ctrlr_opts *o)
{
	o->num_io_queues = 2;
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
	if (spdk_nvme_cpl_is_error(cpl)) {
		g_io_err = 1;
	}
	g_io_done = 1;
}

/* ================================================================== */
/* dump 与对比                                                         */
/* ================================================================== */

static void
hexdump(const char *tag, const void *p, size_t len)
{
	const uint8_t *b = (const uint8_t *)p;
	size_t i;

	printf("  %s:\n", tag);
	for (i = 0; i < len; i++) {
		if (i % 16 == 0) {
			printf("    %04zx  ", i);
		}
		printf("%02x ", b[i]);
		if (i % 16 == 15) {
			printf("\n");
		}
	}
	if (len % 16) {
		printf("\n");
	}
}

static void
compare(const char *tag, const void *a, const void *b, size_t len,
	const int *expected_diff, int n_exp)
{
	const uint8_t *x = (const uint8_t *)a;
	const uint8_t *y = (const uint8_t *)b;
	size_t i;
	int diffs = 0, unexpected = 0;

	printf("\n  --- %s 对比 ---\n", tag);
	for (i = 0; i < len; i++) {
		if (x[i] == y[i]) {
			continue;
		}
		diffs++;

		int is_exp = 0, j;

		for (j = 0; j < n_exp; j++) {
			if ((int)i == expected_diff[j]) {
				is_exp = 1;
				break;
			}
		}
		if (!is_exp) {
			unexpected++;
		}
		printf("    offset 0x%02zx: SPDK=%02x 我们=%02x  %s\n",
		       i, x[i], y[i], is_exp ? "(预期不同)" : "<<< 不一致");
	}

	if (diffs == 0) {
		printf("    完全一致\n");
	} else {
		printf("    共 %d 处不同,其中 %d 处非预期\n", diffs, unexpected);
	}
	printf("    结论: %s\n",
	       unexpected == 0 ? "布局正确,可搬入 kernel" : "布局有误,需修正");
}

/* ================================================================== */

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	struct ibv_qp *qp;
	struct ibv_cq *cq;
	struct mlx5dv_qp dv_qp = {};
	struct mlx5dv_cq dv_cq = {};
	struct mlx5dv_obj obj;
	uint32_t sector, lba_count;
	uint16_t pi_before, pi_after, slot;
	uint8_t spdk_wqe[64], our_wqe[64];
	void *slot_ptr;
	uint64_t db_val;
	int op;

	while ((op = getopt(argc, argv, "a:s:n:g:")) != -1) {
		switch (op) {
		case 'a': snprintf(g_traddr, sizeof(g_traddr), "%s", optarg); break;
		case 's': snprintf(g_trsvcid, sizeof(g_trsvcid), "%s", optarg); break;
		case 'n': snprintf(g_nqn, sizeof(g_nqn), "%s", optarg); break;
		case 'g': g_gpu_id = atoi(optarg); break;
		default:
			printf("用法: %s -a <ip> -n <nqn> [-g gpu]\n", argv[0]);
			return 1;
		}
	}
	if (!g_traddr[0] || !g_nqn[0]) {
		printf("用法: %s -a <ip> -n <nqn> [-g gpu]\n", argv[0]);
		return 1;
	}

	printf("=========================================================\n");
	printf(" WQE 布局验证:自建 vs SPDK 实际提交\n");
	printf("=========================================================\n");

	spdk_env_opts_init(&opts);
	opts.opts_size = sizeof(opts);
	opts.name = "wqe_verify";
	opts.no_pci = true;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init 失败\n");
		return 1;
	}

	/* ---- 显存 ---- */
	g_gpu_len = 2 * 1024 * 1024;
	if (gpu_set_device(g_gpu_id) != GPU_SUCCESS ||
	    gpu_mem_alloc(&g_gpu_buf, g_gpu_len) != GPU_SUCCESS) {
		fprintf(stderr, "显存分配失败\n");
		return 1;
	}
	if (gpu_mem_get_dmabuf_fd(&g_dmabuf_fd, g_gpu_buf, g_gpu_len)
	    != GPU_SUCCESS) {
		fprintf(stderr, "dma_buf 导出失败\n");
		return 1;
	}
	if (spdk_mem_register(g_gpu_buf, g_gpu_len) != 0) {
		fprintf(stderr, "spdk_mem_register 失败\n");
		return 1;
	}
	printf("[GPU] buf=%p len=%zu fd=%d\n", g_gpu_buf, g_gpu_len, g_dmabuf_fd);

	/* ---- 连接 ---- */
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

	/* ---- 挖出底层 verbs 对象(需要打过 patch)---- */
	qp = spdk_nvme_qpair_get_ibv_qp(g_qpair);
	cq = spdk_nvme_qpair_get_ibv_cq(g_qpair);
	if (!qp) {
		fprintf(stderr,
			"spdk_nvme_qpair_get_ibv_qp 返回 NULL。\n"
			"  SPDK 打过 patch 了吗?scripts/apply_spdk_patch.sh\n");
		return 1;
	}

	dv_qp.comp_mask = MLX5DV_QP_MASK_UAR_MMAP_OFFSET;
	obj.qp.in = qp;   obj.qp.out = &dv_qp;
	obj.cq.in = cq;   obj.cq.out = &dv_cq;
	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ)) {
		fprintf(stderr, "mlx5dv_init_obj 失败: %s\n", strerror(errno));
		return 1;
	}

	printf("\n[QP] qpn=0x%x\n", qp->qp_num);
	printf("  sq.buf=%p  wqe_cnt=%u stride=%u\n",
	       dv_qp.sq.buf, dv_qp.sq.wqe_cnt, dv_qp.sq.stride);
	printf("  dbrec=%p  bf.reg=%p (size %u)\n",
	       (void *)dv_qp.dbrec, dv_qp.bf.reg, dv_qp.bf.size);
	printf("  cq.buf=%p cqe_cnt=%u cqe_size=%u\n",
	       dv_cq.buf, dv_cq.cqe_cnt, dv_cq.cqe_size);

	/* ---- 让 SPDK 提交一次,抓它写的 WQE ---- */
	sector = spdk_nvme_ns_get_sector_size(g_ns);
	lba_count = 4096 / sector;

	/* dbrec[0]=RQ, dbrec[1]=SQ。读错会拿到接收队列的索引,
	 * 那个也在动(每完成一个 I/O 补一个 recv buffer),看着像对的,
	 * 但按它去 SQ 找 slot 只会读到全零。 */
	pi_before = (uint16_t)(wqe_hto_be32(((volatile uint32_t *)dv_qp.dbrec)[1]) & 0xffff);

	g_io_done = g_io_err = 0;
	if (spdk_nvme_ns_cmd_read(g_ns, g_qpair, g_gpu_buf, 0, lba_count,
				  io_cb, NULL, 0) != 0) {
		fprintf(stderr, "提交失败\n");
		return 1;
	}
	while (!g_io_done) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
	}
	if (g_io_err) {
		fprintf(stderr, "I/O 出错\n");
		return 1;
	}

	pi_after = (uint16_t)(wqe_hto_be32(((volatile uint32_t *)dv_qp.dbrec)[1]) & 0xffff);

	printf("\n[提交] dbrec: %u -> %u\n", pi_before, pi_after);

	slot = pi_before & (dv_qp.sq.wqe_cnt - 1);
	slot_ptr = (void *)((uintptr_t)dv_qp.sq.buf +
			    (size_t)slot * dv_qp.sq.stride);
	memcpy(spdk_wqe, slot_ptr, sizeof(spdk_wqe));

	printf("  SPDK 用了 slot %u @ %p\n", slot, slot_ptr);
	hexdump("SPDK 写的 WQE(前 64 字节)", spdk_wqe, 64);

	/* ---- 自己构造一条同样的 ---- */
	{
		struct mlx5_wqe_data_seg *sd =
			(struct mlx5_wqe_data_seg *)(spdk_wqe + 16);
		uint64_t cap_addr = wqe_hto_be64(sd->addr);
		uint32_t cap_lkey = wqe_hto_be32(sd->lkey);
		uint32_t cap_len  = wqe_hto_be32(sd->byte_count);

		printf("\n  从 SPDK 的 WQE 里读出胶囊位置:\n");
		printf("    addr=0x%lx lkey=0x%x len=%u\n",
		       cap_addr, cap_lkey, cap_len);

		memset(our_wqe, 0, sizeof(our_wqe));
		db_val = mlx5_build_send_wqe(our_wqe, slot, qp->qp_num,
					     cap_addr, cap_lkey, cap_len);
		hexdump("我们构造的 WQE", our_wqe, 32);

		{
			/* fm_ce_se 可能不同(SPDK 未必每条都要 CQE) */
			int exp[] = { 11 };

			compare("mlx5 SEND WQE", spdk_wqe, our_wqe, 32,
				exp, 1);
		}

		printf("\n  门铃值: 0x%016lx\n", db_val);

		/* ---- 顺带核对 NVMe 胶囊 ---- */
		if (cap_addr && cap_len >= 64) {
			struct nvme_sqe ours;
			const uint8_t *spdk_cap = (const uint8_t *)cap_addr;
			/* SPDK 分配的 cid 我们不知道,预期不同的偏移:
			 * 2-3 (cid),以及 sgl 里的地址/key(指向显存,应相同) */
			int exp[] = { 2, 3 };

			hexdump("\n  SPDK 的 NVMe 胶囊", spdk_cap, 64);

			nvme_build_rw_sqe(&ours, NVME_OPC_READ, 0,
					  spdk_nvme_ns_get_id(g_ns), 0,
					  lba_count,
					  (uint64_t)g_gpu_buf,
					  g_gpu_mr ? g_gpu_mr->rkey : 0,
					  4096);
			hexdump("  我们构造的胶囊", &ours, 64);
			compare("NVMe 命令胶囊", spdk_cap, &ours, 64, exp, 2);
		}
	}

	printf("\n=========================================================\n");
	printf(" 若两处都只有预期差异,布局即正确,可原样搬入 kernel。\n");
	printf("=========================================================\n");

	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	if (g_dmabuf_fd >= 0) {
		close(g_dmabuf_fd);
	}
	if (g_gpu_buf) {
		spdk_mem_unregister(g_gpu_buf, g_gpu_len);
		gpu_mem_free(g_gpu_buf);
	}
	return 0;
}
