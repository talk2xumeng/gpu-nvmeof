/*
 * kv_smoke.c  --  CPU 侧 KV 命令冒烟测试
 *
 * 不碰 GPU。用 SPDK 的 io_raw 发 KV 命令,胶囊用和 kernel 同一个
 * nvme_build_kv_sqe(src/wqe_build.h)构造,SPDK 只覆盖 cid 和 dptr。
 * 所以这里全过,就说明 target(kv_emu + 补丁)和胶囊构造都对了,
 * GPU 侧再出问题只可能在 kernel/队列那一层。
 *
 *   ./kv_smoke -a <ip> -n <nqn> [-t rdma|tcp] [-s 端口] [-r 个数] [-b value 字节]
 *              [-S seed] [-L 起始序号] [-N 不用大页]
 *
 * 覆盖 KV 1.3 里 GPU 路径会用到、也是最容易实现错的语义:
 *   Exist/Store/Retrieve/Delete 基本往返,Retrieve DW0 = value 全长,
 *   截断 Retrieve,SINKE/SIKE 条件写,KL=0 / KL>16,零长度 value,List。
 */

#include "spdk/stdinc.h"
#include "spdk/env.h"
#include "spdk/nvme.h"

#include "wqe_build.h"
#include "blk_pattern.h"

#define KLEN	16

static struct spdk_nvme_ctrlr	*g_ctrlr;
static struct spdk_nvme_ns	*g_ns;
static struct spdk_nvme_qpair	*g_qpair;
static uint8_t			g_seed = 1;
static unsigned			g_pass, g_fail;

struct cpl {
	int		done;
	uint32_t	dw0;
	uint32_t	st;	/* (SCT << 8) | SC */
};

static void
cb(void *a, const struct spdk_nvme_cpl *c)
{
	struct cpl *p = a;

	p->dw0 = c->cdw0;
	p->st = ((uint32_t)c->status.sct << 8) | c->status.sc;
	p->done = 1;
}

/*
 * 发一条 KV 命令。klen_override >= 0 时在构造后改写 CDW11[7:0],
 * 用来造非法 key 长度(构造函数本身只接受 <= 16)。
 */
static uint32_t
kv(uint8_t opc, uint64_t idx, uint32_t opts, void *buf, uint32_t len,
   int klen_override, uint32_t *dw0)
{
	struct spdk_nvme_cmd cmd;
	struct nvme_sqe sqe;
	struct cpl c = { 0 };
	uint8_t key[KLEN];
	int has = nvme_kv_opc_has_data(opc) && len;

	blk_kv_key(key, g_seed, idx);
	nvme_build_kv_sqe(&sqe, opc, 0, spdk_nvme_ns_get_id(g_ns), key, KLEN,
			  opts, 0, 0, len);
	memcpy(&cmd, &sqe, sizeof(cmd));
	cmd.psdt = 0;
	memset(&cmd.dptr, 0, sizeof(cmd.dptr));
	if (klen_override >= 0) {
		cmd.cdw11 = (cmd.cdw11 & ~0xffu) | (uint32_t)klen_override;
	}
	if (spdk_nvme_ctrlr_cmd_io_raw(g_ctrlr, g_qpair, &cmd, has ? buf : NULL,
				       has ? len : 0, cb, &c) != 0) {
		fprintf(stderr, "io_raw 提交失败\n");
		exit(1);
	}
	while (!c.done) {
		spdk_nvme_qpair_process_completions(g_qpair, 0);
		/* 顺带发 keep-alive:几百个 key 的循环可能跑过 KATO,target 会断连 */
		spdk_nvme_ctrlr_process_admin_completions(g_ctrlr);
	}
	if (dw0) {
		*dw0 = c.dw0;
	}
	return c.st;
}

static void
expect(const char *what, uint32_t got, uint32_t want)
{
	if (got == want) {
		g_pass++;
		printf("  PASS  %-44s 0x%03x\n", what, got);
	} else {
		g_fail++;
		printf("  FAIL  %-44s 0x%03x,期望 0x%03x\n", what, got, want);
	}
}

/* 统计 n 个 key 里状态不等于 want 的,打一行 */
static void
expect_all(const char *what, unsigned bad, unsigned n)
{
	if (bad == 0) {
		g_pass++;
		printf("  PASS  %-44s %u/%u\n", what, n, n);
	} else {
		g_fail++;
		printf("  FAIL  %-44s %u/%u 不符\n", what, bad, n);
	}
}

static bool
probe_cb(void *c, const struct spdk_nvme_transport_id *t,
	 struct spdk_nvme_ctrlr_opts *o)
{
	return true;
}

static void
attach_cb(void *c, const struct spdk_nvme_transport_id *t,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *o)
{
	g_ctrlr = ctrlr;
	g_ns = spdk_nvme_ctrlr_get_ns(ctrlr,
				      spdk_nvme_ctrlr_get_first_active_ns(ctrlr));
}

int
main(int argc, char **argv)
{
	struct spdk_env_opts opts;
	struct spdk_nvme_transport_id trid = {};
	char traddr[64] = "", nqn[224] = "", port[16] = "4420", tr[8] = "rdma";
	uint32_t n = 16, vb = 4096, dw0, i, st, bad;
	uint64_t base = 0;
	bool no_huge = false;
	uint8_t *buf, *list;
	int op;

	while ((op = getopt(argc, argv, "a:n:t:s:r:b:S:L:N")) != -1) {
		switch (op) {
		case 'a': snprintf(traddr, sizeof(traddr), "%s", optarg); break;
		case 'n': snprintf(nqn, sizeof(nqn), "%s", optarg); break;
		case 't': snprintf(tr, sizeof(tr), "%s", optarg); break;
		case 's': snprintf(port, sizeof(port), "%s", optarg); break;
		case 'r': n = (uint32_t)atoi(optarg); break;
		case 'b': vb = (uint32_t)atoi(optarg); break;
		case 'S': g_seed = (uint8_t)strtoul(optarg, NULL, 0); break;
		case 'L': base = strtoull(optarg, NULL, 0); break;
		case 'N': no_huge = true; break;
		default:
			printf("用法: %s -a <ip> -n <nqn> [-t rdma|tcp] [-s port] "
			       "[-r n] [-b bytes] [-S seed] [-L idx] [-N]\n", argv[0]);
			return 1;
		}
	}
	if (!traddr[0] || !nqn[0] || n == 0 || vb < 16 || vb % 8) {
		fprintf(stderr, "-a -n 必填;-b 取 >=16 的 8 的倍数\n");
		return 1;
	}

	spdk_env_opts_init(&opts);
	opts.opts_size = sizeof(opts);
	opts.name = "kv_smoke";
	opts.no_pci = true;
	if (no_huge) {
		opts.no_huge = true;
		opts.mem_size = 512;
	}
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "spdk_env_init 失败\n");
		return 1;
	}

	if (spdk_nvme_transport_id_populate_trstring(&trid, tr) != 0 ||
	    spdk_nvme_transport_id_parse_trtype(&trid.trtype, tr) != 0) {
		fprintf(stderr, "传输 %s 不认识\n", tr);
		return 1;
	}
	trid.adrfam = SPDK_NVMF_ADRFAM_IPV4;
	snprintf(trid.traddr, sizeof(trid.traddr), "%s", traddr);
	snprintf(trid.trsvcid, sizeof(trid.trsvcid), "%s", port);
	snprintf(trid.subnqn, sizeof(trid.subnqn), "%s", nqn);
	if (spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL) || !g_ns) {
		fprintf(stderr, "连接 %s %s:%s 失败\n", tr, traddr, port);
		return 1;
	}
	g_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_ctrlr, NULL, 0);
	buf = spdk_dma_zmalloc(vb, 4096, NULL);
	list = spdk_dma_zmalloc(4096, 4096, NULL);
	if (!g_qpair || !buf || !list) {
		return 1;
	}

	printf("KV 冒烟:%s %s:%s nsid %u,%u 个 key,value %u 字节,seed %u\n\n",
	       tr, traddr, port, spdk_nvme_ns_get_id(g_ns), n, vb, g_seed);

	/* ---- 0. 清场 ---- */
	for (i = 0; i < n; i++) {
		kv(NVME_KV_OPC_DELETE, base + i, 0, NULL, 0, -1, NULL);
	}
	kv(NVME_KV_OPC_DELETE, base + n, 0, NULL, 0, -1, NULL);

	/* ---- 1. 基本往返 ---- */
	printf("[基本]\n");
	expect("Exist(未存)-> 87h", kv(NVME_KV_OPC_EXIST, base, 0, NULL, 0, -1, NULL),
	       NVME_KV_SC_KEY_NOT_EXIST);
	expect("Retrieve(未存)-> 87h",
	       kv(NVME_KV_OPC_RETRIEVE, base, 0, buf, vb, -1, NULL),
	       NVME_KV_SC_KEY_NOT_EXIST);

	for (bad = 0, i = 0; i < n; i++) {
		blk_fill(buf, vb, g_seed, base + i);
		bad += kv(NVME_KV_OPC_STORE, base + i, 0, buf, vb, -1, NULL) != 0;
	}
	expect_all("Store", bad, n);

	for (bad = 0, i = 0; i < n; i++) {
		bad += kv(NVME_KV_OPC_EXIST, base + i, 0, NULL, 0, -1, NULL) != 0;
	}
	expect_all("Exist(已存)", bad, n);

	for (bad = 0, i = 0; i < n; i++) {
		struct blk_result r;

		memset(buf, BLK_RPAT, vb);
		st = kv(NVME_KV_OPC_RETRIEVE, base + i, 0, buf, vb, -1, &dw0);
		if (st || dw0 != vb ||
		    blk_check(buf, vb, g_seed, base + i, &r) != BLK_OK) {
			if (!bad) {
				printf("        key %" PRIu64 ": st 0x%03x dw0 %u 块 %c\n",
				       base + i, st, dw0, blk_map_char(&r));
			}
			bad++;
		}
	}
	expect_all("Retrieve 全长:DW0 = value 长度,逐块一致", bad, n);

	/* ---- 2. 截断 Retrieve:只回前 HBS 字节,DW0 仍是全长 ---- */
	printf("[截断]\n");
	{
		uint32_t half = vb / 2;
		uint8_t *ref = malloc(vb);

		blk_fill(ref, vb, g_seed, base);
		memset(buf, BLK_RPAT, vb);
		st = kv(NVME_KV_OPC_RETRIEVE, base, 0, buf, half, -1, &dw0);
		expect("Retrieve HBS=value/2 状态", st, 0);
		expect("  DW0 仍是 value 全长", dw0, vb);
		expect("  前 HBS 字节一致", memcmp(buf, ref, half) != 0, 0);
		free(ref);
	}

	/* ---- 3. 条件写 ---- */
	printf("[条件写]\n");
	blk_fill(buf, vb, g_seed, base);
	expect("Store SINKE,key 已存在 -> 89h",
	       kv(NVME_KV_OPC_STORE, base, NVME_KV_STORE_SINKE, buf, vb, -1, NULL),
	       NVME_KV_SC_KEY_EXISTS);
	expect("Store SIKE,key 已存在 -> 0",
	       kv(NVME_KV_OPC_STORE, base, NVME_KV_STORE_SIKE, buf, vb, -1, NULL), 0);
	expect("Store SIKE,key 不存在 -> 87h",
	       kv(NVME_KV_OPC_STORE, base + n, NVME_KV_STORE_SIKE, buf, vb, -1, NULL),
	       NVME_KV_SC_KEY_NOT_EXIST);
	expect("  且没有被写入(Exist -> 87h)",
	       kv(NVME_KV_OPC_EXIST, base + n, 0, NULL, 0, -1, NULL),
	       NVME_KV_SC_KEY_NOT_EXIST);

	/* ---- 4. key 长度 ---- */
	printf("[key 长度]\n");
	expect("KL=17 -> Invalid Field 02h",
	       kv(NVME_KV_OPC_EXIST, base, 0, NULL, 0, 17, NULL),
	       SPDK_NVME_SC_INVALID_FIELD);
	expect("KL=0 -> Invalid Key Size 86h",
	       kv(NVME_KV_OPC_EXIST, base, 0, NULL, 0, 0, NULL),
	       NVME_KV_SC_INVALID_KEY_SIZE);
	expect("KL=8(前 8 字节相同)是另一个 key -> 87h",
	       kv(NVME_KV_OPC_EXIST, base, 0, NULL, 0, 8, NULL),
	       NVME_KV_SC_KEY_NOT_EXIST);

	/* ---- 5. List ---- */
	printf("[List]\n");
	{
		uint32_t nrk, off = 4, k, ok = 1;
		uint8_t key0[KLEN];

		blk_kv_key(key0, g_seed, base);
		st = kv(NVME_KV_OPC_LIST, base, 0, list, 4096, -1, NULL);
		memcpy(&nrk, list, 4);
		expect("List 状态", st, 0);
		for (k = 0; k < nrk && off + 2 <= 4096; k++) {
			uint32_t kl = list[off] | (list[off + 1] << 8);

			if (kl == 0 || kl > KLEN) {
				ok = 0;
				break;
			}
			if (k == 0 && (kl != KLEN || memcmp(list + off + 2, key0, KLEN))) {
				ok = 0;
			}
			off += (2 + kl + 3) & ~3u;
		}
		expect("  起始 key 存在时它排第一,条目格式合法", !ok, 0);
	}
	{
		/*
		 * 分页遍历全表:spec 只保证"没有增删时顺序稳定",不保证从哪开始,
		 * 所以从一个不存在的 key 起步(kv_emu 从头列),之后每页用上一页
		 * 最后一个 key 作起点,并丢掉新一页的第一项(就是它自己)。
		 * 本轮 n 个 key 必须恰好各出现一次。
		 */
		uint8_t start[KLEN], tag[8];
		uint8_t *seen = calloc(n, 1);
		uint32_t found = 0, dup = 0, pages = 0, nrk, k;
		int first_page = 1;

		blk_kv_key(start, g_seed, base + n);	/* 此时不存在 */
		memcpy(tag, start + 8, 8);	/* key 高 8 字节 = BLK_KV_TAG ^ seed */
		for (pages = 0; pages < 10000; pages++) {
			struct spdk_nvme_cmd cmd;
			struct nvme_sqe sqe;
			struct cpl c = { 0 };
			uint32_t off = 4, last_off = 0, got = 0;

			nvme_build_kv_sqe(&sqe, NVME_KV_OPC_LIST, 0,
					  spdk_nvme_ns_get_id(g_ns), start, KLEN,
					  0, 0, 0, 4096);
			memcpy(&cmd, &sqe, sizeof(cmd));
			cmd.psdt = 0;
			memset(&cmd.dptr, 0, sizeof(cmd.dptr));
			spdk_nvme_ctrlr_cmd_io_raw(g_ctrlr, g_qpair, &cmd, list, 4096,
						   cb, &c);
			while (!c.done) {
				spdk_nvme_qpair_process_completions(g_qpair, 0);
			}
			memcpy(&nrk, list, 4);
			for (k = 0; k < nrk; k++) {
				uint32_t kl = list[off] | (list[off + 1] << 8);
				const uint8_t *kp = list + off + 2;

				last_off = off;
				off += (2 + kl + 3) & ~3u;
				if (k == 0 && !first_page) {
					continue;	/* 上一页的最后一个 */
				}
				got++;
				if (kl == KLEN && memcmp(kp + 8, tag, 8) == 0) {
					uint64_t idx = 0;
					int b;

					for (b = 7; b >= 0; b--) {
						idx = (idx << 8) | kp[b];
					}
					if (idx >= base && idx < base + n) {
						if (seen[idx - base]++) {
							dup++;
						} else {
							found++;
						}
					}
				}
			}
			first_page = 0;
			if (got == 0) {
				break;
			}
			memcpy(start, list + last_off + 2, KLEN);
		}
		printf("        分页 %u 次\n", pages + 1);
		expect("  分页遍历:本轮 key 一个不少", n - found, 0);
		expect("  分页遍历:没有重复", dup, 0);
		free(seen);
	}

	/* ---- 6. 零长度 value ---- */
	printf("[零长度 value]\n");
	expect("Store VS=0", kv(NVME_KV_OPC_STORE, base + n, 0, buf, 0, -1, NULL), 0);
	expect("  Exist -> 0", kv(NVME_KV_OPC_EXIST, base + n, 0, NULL, 0, -1, NULL), 0);
	st = kv(NVME_KV_OPC_RETRIEVE, base + n, 0, buf, vb, -1, &dw0);
	expect("  Retrieve 状态", st, 0);
	expect("  DW0 = 0", dw0, 0);

	/* ---- 7. Delete ---- */
	printf("[Delete]\n");
	for (bad = 0, i = 0; i < n; i++) {
		bad += kv(NVME_KV_OPC_DELETE, base + i, 0, NULL, 0, -1, NULL) != 0;
	}
	expect_all("Delete", bad, n);
	for (bad = 0, i = 0; i < n; i++) {
		bad += kv(NVME_KV_OPC_EXIST, base + i, 0, NULL, 0, -1, NULL) !=
		       NVME_KV_SC_KEY_NOT_EXIST;
	}
	expect_all("  之后 Exist -> 87h", bad, n);
	st = kv(NVME_KV_OPC_DELETE, base, 0, NULL, 0, -1, NULL);
	printf("        再删一次不存在的 key: 0x%03x(EDNEK=0 时为 0,=1 时为 87h)\n", st);
	kv(NVME_KV_OPC_DELETE, base + n, 0, NULL, 0, -1, NULL);

	printf("\n%s:%u 通过,%u 失败\n", g_fail ? "*** FAIL ***" : "*** PASS ***",
	       g_pass, g_fail);

	spdk_dma_free(buf);
	spdk_dma_free(list);
	spdk_nvme_ctrlr_free_io_qpair(g_qpair);
	spdk_nvme_detach(g_ctrlr);
	return g_fail ? 2 : 0;
}
