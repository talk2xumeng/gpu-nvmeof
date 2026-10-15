/*
 * bdev_kv_emu.c  --  内存 KV bdev,在 SPDK target 上模拟 NVMe KV 命令集
 *
 * 用途:手头的盘不支持 KV(CSI 01h),但 GPU 侧要测 KV 胶囊的构造、
 * 收发和 CQE DW0 回收。把这个 bdev 挂到 nvmf subsystem 上,配合
 * nvmf_kv_passthru.patch,target 收到的 I/O 命令会原样(整条 SQE)
 * 交到这里,按 KV Command Set 1.3 的语义执行。
 *
 * 实现的命令(KV 1.3 §3.2,全部是 Mandatory):
 *
 *   01h Store     CDW10=VS  CDW11[7:0]=KL [8]SIKE [9]SINKE [10]NOCOMP(忽略)
 *   02h Retrieve  CDW10=HBS CDW11[7:0]=KL [8]RRD(忽略)  完成 DW0 = value 长度
 *   06h List      CDW10=HBS CDW11[7:0]=KL               返回 NRK + {KL,key,pad}
 *   10h Delete    CDW11[7:0]=KL  key 不存在时按 EDNEK 决定报 87h 还是成功
 *   14h Exist     CDW11[7:0]=KL  不存在报 87h
 *   00h Flush     直接成功
 *
 * key 在 CDW2/3(字节 0..7)和 CDW14/15(字节 8..15),直接按 SQE 偏移取。
 * 长度不同的 key 视为不同 key(spec §2.1)。
 *
 * 不做的:压缩、Key Per I/O(CDW13)、Identify 里的 KV 数据结构 ——
 * nvmf 仍按 NVM namespace 报 Identify,host 侧不读它,对 GPU 路径无影响。
 *
 * 并发:一把互斥锁保护整张表。Store 的数据在锁外收进新缓冲,锁内只换
 * 指针;Retrieve 在锁内拷出 —— 保证 Store/Delete 对单个 pair 原子(spec §2.1.4)。
 *
 * value 缓冲:按 2 的幂分级的池,底层是 hugepage(spdk_dma_malloc),
 * Delete / 覆盖时还回池里,不 free。早先直接用 glibc malloc,64–128 KiB
 * 的 value 每次 Store 都走 mmap + 首次触碰缺页 + munmap,KV vs NVM 对照
 * 里 Store 比 Write 多出 ~340 ns/KB —— 那是模拟器的代价,不是 KV 的。
 *
 * RPC:
 *   bdev_kv_emu_create   {name, capacity_mb?, max_value?, ednek?}
 *   bdev_kv_emu_stats    {name}   各 opcode 计数、key 数、已用字节
 *   bdev_kv_emu_delete   {name}
 */

#include "spdk/stdinc.h"
#include "spdk/bdev_module.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/jsonrpc.h"
#include "spdk/log.h"
#include "spdk/nvme_spec.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"

#define KV_EMU_MODULE		"kv_emu"	/* nvmf 补丁按这个名字认 namespace */
#define KV_EMU_MAX_KEY		16
#define KV_EMU_BLOCKLEN		4096		/* 只为满足 bdev 注册,不参与 KV 语义 */
#define KV_EMU_NBUCKET		(1u << 16)

/* KV 1.3 Figure 5 */
#define KV_OPC_FLUSH		0x00
#define KV_OPC_STORE		0x01
#define KV_OPC_RETRIEVE		0x02
#define KV_OPC_LIST		0x06
#define KV_OPC_DELETE		0x10
#define KV_OPC_EXIST		0x14

/* KV 1.3 Figure 4,SCT = Generic */
#define KV_SC_CAPACITY_EXCEEDED	0x81
#define KV_SC_INVALID_VALUE_SIZE 0x85
#define KV_SC_INVALID_KEY_SIZE	0x86
#define KV_SC_KEY_NOT_EXIST	0x87
#define KV_SC_KEY_EXISTS	0x89

#define KV_STORE_SIKE		(1u << 8)
#define KV_STORE_SINKE		(1u << 9)

struct kv_ent {
	struct kv_ent	*next;
	uint8_t		klen;
	uint8_t		key[KV_EMU_MAX_KEY];
	uint32_t	vlen;
	uint8_t		*val;
};

/* value 池:第 c 级缓冲大小 = 1 << (KV_POOL_MIN_SHIFT + c) */
#define KV_POOL_MIN_SHIFT	12		/* 4 KiB 起 */
#define KV_POOL_NCLASS		21		/* 到 4 GiB,远超 max_value */

struct kv_pool {
	pthread_mutex_t		lock;
	void			*free[KV_POOL_NCLASS];	/* 空闲链,next 指针存在缓冲开头 */
	uint64_t		bytes;		/* 已从 hugepage 取出的总量(含空闲) */
	uint64_t		hits;		/* 从空闲链复用的次数 */
	uint64_t		misses;		/* 新分配的次数 */
};

struct kv_emu {
	struct spdk_bdev	bdev;
	pthread_mutex_t		lock;
	struct kv_pool		pool;
	struct kv_ent		**bucket;
	uint64_t		capacity;	/* 字节,key + value 都计入(NSZE/NUSE 语义) */
	uint64_t		used;
	uint64_t		nkeys;
	uint32_t		max_value;
	bool			ednek;		/* Feature 20h bit0:删不存在的 key 是否报错 */
	uint64_t		cnt[256];	/* 按 opcode 计数 */
	uint64_t		err[256];	/* 按 opcode 计非 0 状态 */
	TAILQ_ENTRY(kv_emu)	link;
};

static TAILQ_HEAD(, kv_emu) g_kv_emus = TAILQ_HEAD_INITIALIZER(g_kv_emus);

static int kv_emu_init(void);
static void kv_emu_fini(void);

static struct spdk_bdev_module kv_emu_if = {
	.name = KV_EMU_MODULE,
	.module_init = kv_emu_init,
	.module_fini = kv_emu_fini,
};

SPDK_BDEV_MODULE_REGISTER(kv_emu, &kv_emu_if)
SPDK_LOG_REGISTER_COMPONENT(kv_emu)

/* ================================================================== */
/* 表                                                                  */
/* ================================================================== */

static uint32_t
kv_hash(const uint8_t *key, uint8_t klen)
{
	uint32_t h = 2166136261u ^ klen;	/* FNV-1a,长度也参与 */
	int i;

	for (i = 0; i < klen; i++) {
		h = (h ^ key[i]) * 16777619u;
	}
	return h & (KV_EMU_NBUCKET - 1);
}

/* 返回指向命中项的那个指针(便于删除),没命中返回 NULL */
static struct kv_ent **
kv_find(struct kv_emu *d, const uint8_t *key, uint8_t klen)
{
	struct kv_ent **pp = &d->bucket[kv_hash(key, klen)];

	for (; *pp; pp = &(*pp)->next) {
		if ((*pp)->klen == klen && memcmp((*pp)->key, key, klen) == 0) {
			return pp;
		}
	}
	return NULL;
}

/* ================================================================== */
/* value 池                                                            */
/* ================================================================== */

static int
kv_pool_class(uint32_t len)
{
	int c = 0;

	while (((uint64_t)1 << (KV_POOL_MIN_SHIFT + c)) < len) {
		c++;
	}
	return c < KV_POOL_NCLASS ? c : -1;
}

/* len = 0 返回 NULL(零长度 value 不占缓冲),失败也返回 NULL,用 *ok 区分 */
static uint8_t *
kv_val_alloc(struct kv_emu *d, uint32_t len, bool *ok)
{
	int c;
	void *p;

	*ok = true;
	if (len == 0) {
		return NULL;
	}
	c = kv_pool_class(len);
	if (c < 0) {
		*ok = false;
		return NULL;
	}
	pthread_mutex_lock(&d->pool.lock);
	p = d->pool.free[c];
	if (p) {
		d->pool.free[c] = *(void **)p;
		d->pool.hits++;
	}
	pthread_mutex_unlock(&d->pool.lock);
	if (p) {
		return p;
	}
	/* hugepage 内存:已经映射、钉住,没有首次触碰缺页 */
	p = spdk_dma_malloc((size_t)1 << (KV_POOL_MIN_SHIFT + c), 64, NULL);
	if (!p) {
		*ok = false;
		return NULL;
	}
	pthread_mutex_lock(&d->pool.lock);
	d->pool.bytes += (uint64_t)1 << (KV_POOL_MIN_SHIFT + c);
	d->pool.misses++;
	pthread_mutex_unlock(&d->pool.lock);
	return p;
}

static void
kv_val_free(struct kv_emu *d, uint8_t *p, uint32_t len)
{
	int c;

	if (!p) {
		return;
	}
	c = kv_pool_class(len);
	pthread_mutex_lock(&d->pool.lock);
	*(void **)p = d->pool.free[c];
	d->pool.free[c] = p;
	pthread_mutex_unlock(&d->pool.lock);
}

static void
kv_pool_destroy(struct kv_pool *pool)
{
	int c;

	for (c = 0; c < KV_POOL_NCLASS; c++) {
		void *p = pool->free[c], *n;

		for (; p; p = n) {
			n = *(void **)p;
			spdk_dma_free(p);
		}
	}
	pthread_mutex_destroy(&pool->lock);
}

/* ================================================================== */
/* iovec 拷贝                                                          */
/* ================================================================== */

static size_t
iov_to_buf(uint8_t *dst, size_t len, const struct iovec *iov, int cnt)
{
	size_t done = 0;
	int i;

	for (i = 0; i < cnt && done < len; i++) {
		size_t n = spdk_min(iov[i].iov_len, len - done);

		memcpy(dst + done, iov[i].iov_base, n);
		done += n;
	}
	return done;
}

/* 拷 len 字节进去,iovec 剩下的部分清零(target 会把整段 SGL 写给 host) */
static void
buf_to_iov(struct iovec *iov, int cnt, const uint8_t *src, size_t len)
{
	size_t done = 0;
	int i;

	for (i = 0; i < cnt; i++) {
		size_t n = spdk_min(iov[i].iov_len, len > done ? len - done : 0);

		if (n) {
			memcpy(iov[i].iov_base, src + done, n);
		}
		if (n < iov[i].iov_len) {
			memset((uint8_t *)iov[i].iov_base + n, 0, iov[i].iov_len - n);
		}
		done += n;
	}
}

/* ================================================================== */
/* 命令                                                                */
/* ================================================================== */

struct kv_req {
	uint8_t		opc;
	uint8_t		klen;
	uint8_t		key[KV_EMU_MAX_KEY];
	uint32_t	cdw10;
	uint32_t	cdw11;
	struct iovec	*iov;
	int		iovcnt;
	size_t		nbytes;		/* 本条命令实际的数据传输长度(SGL 长度) */
};

static int
kv_store(struct kv_emu *d, struct kv_req *r)
{
	uint32_t vs = r->cdw10;
	struct kv_ent **pp, *e;
	uint8_t *val, *old = NULL;
	uint32_t old_len = 0;
	uint64_t need;
	bool ok;

	if (vs > d->max_value) {
		return KV_SC_INVALID_VALUE_SIZE;
	}
	if (vs > r->nbytes) {
		/* 声称的 value 比 SGL 给的数据长 */
		return SPDK_NVME_SC_INVALID_FIELD;
	}
	/* 锁外先把数据收下来;锁内只做替换,保证原子 */
	val = kv_val_alloc(d, vs, &ok);
	if (!ok) {
		return KV_SC_CAPACITY_EXCEEDED;	/* hugepage 用完 */
	}
	iov_to_buf(val, vs, r->iov, r->iovcnt);

	pthread_mutex_lock(&d->lock);
	pp = kv_find(d, r->key, r->klen);
	if (pp && (r->cdw11 & KV_STORE_SINKE)) {
		pthread_mutex_unlock(&d->lock);
		kv_val_free(d, val, vs);
		return KV_SC_KEY_EXISTS;
	}
	if (!pp && (r->cdw11 & KV_STORE_SIKE)) {
		pthread_mutex_unlock(&d->lock);
		kv_val_free(d, val, vs);
		return KV_SC_KEY_NOT_EXIST;
	}
	need = (uint64_t)r->klen + vs;
	if (d->used - (pp ? (*pp)->klen + (*pp)->vlen : 0) + need > d->capacity) {
		pthread_mutex_unlock(&d->lock);
		kv_val_free(d, val, vs);
		return KV_SC_CAPACITY_EXCEEDED;
	}
	if (pp) {
		e = *pp;
		d->used -= e->vlen;
		old = e->val;		/* 锁外还给池 */
		old_len = e->vlen;
	} else {
		e = calloc(1, sizeof(*e));
		if (!e) {
			pthread_mutex_unlock(&d->lock);
			kv_val_free(d, val, vs);
			return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}
		e->klen = r->klen;
		memcpy(e->key, r->key, r->klen);
		e->next = d->bucket[kv_hash(r->key, r->klen)];
		d->bucket[kv_hash(r->key, r->klen)] = e;
		d->used += e->klen;
		d->nkeys++;
	}
	e->val = val;
	e->vlen = vs;
	d->used += vs;
	pthread_mutex_unlock(&d->lock);
	kv_val_free(d, old, old_len);
	return 0;
}

static int
kv_retrieve(struct kv_emu *d, struct kv_req *r, uint32_t *dw0)
{
	struct kv_ent **pp;
	size_t n;

	pthread_mutex_lock(&d->lock);
	pp = kv_find(d, r->key, r->klen);
	if (!pp) {
		pthread_mutex_unlock(&d->lock);
		return KV_SC_KEY_NOT_EXIST;
	}
	/* 只回 min(value, HBS, SGL) 字节;DW0 永远是 value 全长(spec §3.2.3.1) */
	n = spdk_min((size_t)(*pp)->vlen, spdk_min((size_t)r->cdw10, r->nbytes));
	buf_to_iov(r->iov, r->iovcnt, (*pp)->val, n);
	*dw0 = (*pp)->vlen;
	pthread_mutex_unlock(&d->lock);
	return 0;
}

static int
kv_exist(struct kv_emu *d, struct kv_req *r)
{
	int found;

	pthread_mutex_lock(&d->lock);
	found = kv_find(d, r->key, r->klen) != NULL;
	pthread_mutex_unlock(&d->lock);
	return found ? 0 : KV_SC_KEY_NOT_EXIST;
}

static int
kv_delete(struct kv_emu *d, struct kv_req *r)
{
	struct kv_ent **pp, *e;

	pthread_mutex_lock(&d->lock);
	pp = kv_find(d, r->key, r->klen);
	if (!pp) {
		pthread_mutex_unlock(&d->lock);
		return d->ednek ? KV_SC_KEY_NOT_EXIST : 0;
	}
	e = *pp;
	*pp = e->next;
	d->used -= e->klen + e->vlen;
	d->nkeys--;
	pthread_mutex_unlock(&d->lock);
	kv_val_free(d, e->val, e->vlen);
	free(e);
	return 0;
}

/*
 * List:顺序是 桶号 -> 链表序,没有增删时稳定(spec §2.1.6.2 只要求这个)。
 * 起始 key 存在就从它开始(含它本身);不存在就从头 —— spec 允许厂商自定。
 * KL = 0 宽松处理为"从头列"。
 */
static int
kv_list(struct kv_emu *d, struct kv_req *r)
{
	size_t hbs = spdk_min((size_t)r->cdw10, r->nbytes);
	uint8_t *out;
	size_t off = 4;
	uint32_t nrk = 0, b = 0;
	struct kv_ent *e = NULL, **pp = NULL;

	if (hbs < 4) {
		return SPDK_NVME_SC_INVALID_FIELD;
	}
	out = calloc(1, hbs);
	if (!out) {
		return SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
	}

	pthread_mutex_lock(&d->lock);
	if (r->klen) {
		pp = kv_find(d, r->key, r->klen);
	}
	if (pp) {
		e = *pp;
		b = kv_hash(r->key, r->klen);
	}
	for (; b < KV_EMU_NBUCKET; b++, e = NULL) {
		if (!e) {
			e = d->bucket[b];
		}
		for (; e; e = e->next) {
			size_t sz = (2 + e->klen + 3) & ~(size_t)3;

			if (off + sz > hbs) {
				goto full;
			}
			out[off] = e->klen;
			out[off + 1] = 0;
			memcpy(out + off + 2, e->key, e->klen);
			off += sz;
			nrk++;
		}
	}
full:
	pthread_mutex_unlock(&d->lock);

	memcpy(out, &nrk, 4);		/* 小端 */
	buf_to_iov(r->iov, r->iovcnt, out, hbs);
	free(out);
	return 0;
}

static void
kv_emu_exec(struct kv_emu *d, struct spdk_bdev_io *io)
{
	struct spdk_bdev_io_nvme_passthru_params *p = &io->u.nvme_passthru;
	const uint8_t *raw = (const uint8_t *)&p->cmd;
	struct iovec one;
	struct kv_req r;
	uint32_t dw0 = 0;
	int sc;

	memset(&r, 0, sizeof(r));
	r.opc = p->cmd.opc;
	r.cdw10 = p->cmd.cdw10;
	r.cdw11 = p->cmd.cdw11;
	r.klen = (uint8_t)(r.cdw11 & 0xff);
	/* SQE 偏移 8..15 = CDW2/3 = key[7:0];56..63 = CDW14/15 = key[15:8] */
	memcpy(r.key, raw + 8, 8);
	memcpy(r.key + 8, raw + 56, 8);
	if (r.klen < KV_EMU_MAX_KEY) {
		memset(r.key + r.klen, 0, KV_EMU_MAX_KEY - r.klen);
	}
	if (p->iovs) {
		r.iov = p->iovs;
		r.iovcnt = p->iovcnt;
	} else {
		one.iov_base = p->buf;
		one.iov_len = p->nbytes;
		r.iov = &one;
		r.iovcnt = p->buf ? 1 : 0;
	}
	r.nbytes = p->nbytes;

	/* key 长度:> 16 是 Invalid Field(spec §2.1.5);0 是 Invalid Key Size */
	if (r.opc != KV_OPC_FLUSH && r.klen > KV_EMU_MAX_KEY) {
		sc = SPDK_NVME_SC_INVALID_FIELD;
	} else if (r.klen == 0 && r.opc != KV_OPC_FLUSH && r.opc != KV_OPC_LIST) {
		sc = KV_SC_INVALID_KEY_SIZE;
	} else {
		switch (r.opc) {
		case KV_OPC_FLUSH:	sc = 0; break;
		case KV_OPC_STORE:	sc = kv_store(d, &r); break;
		case KV_OPC_RETRIEVE:	sc = kv_retrieve(d, &r, &dw0); break;
		case KV_OPC_LIST:	sc = kv_list(d, &r); break;
		case KV_OPC_DELETE:	sc = kv_delete(d, &r); break;
		case KV_OPC_EXIST:	sc = kv_exist(d, &r); break;
		default:		sc = SPDK_NVME_SC_INVALID_OPCODE; break;
		}
	}

	/* 计数器不求精确,不加锁 */
	d->cnt[r.opc]++;
	if (sc) {
		d->err[r.opc]++;
		SPDK_DEBUGLOG(kv_emu, "opc 0x%02x klen %u -> sc 0x%02x\n",
			      r.opc, r.klen, sc);
	}
	spdk_bdev_io_complete_nvme_status(io, dw0, SPDK_NVME_SCT_GENERIC, sc);
}

/* ================================================================== */
/* bdev 接口                                                           */
/* ================================================================== */

static void
kv_emu_free(struct kv_emu *d)
{
	uint32_t b;

	if (d->bucket) {
		for (b = 0; b < KV_EMU_NBUCKET; b++) {
			struct kv_ent *e = d->bucket[b], *n;

			for (; e; e = n) {
				n = e->next;
				if (e->val) {
					spdk_dma_free(e->val);
				}
				free(e);
			}
		}
		free(d->bucket);
	}
	kv_pool_destroy(&d->pool);
	pthread_mutex_destroy(&d->lock);
	free(d->bdev.name);
	free(d);
}

static void
kv_emu_unregister_cb(void *ctx)
{
	kv_emu_free(ctx);
}

static int
kv_emu_destruct(void *ctx)
{
	struct kv_emu *d = ctx;

	TAILQ_REMOVE(&g_kv_emus, d, link);
	spdk_io_device_unregister(d, kv_emu_unregister_cb);
	return 0;
}

static void
kv_emu_submit(struct spdk_io_channel *ch, struct spdk_bdev_io *io)
{
	switch (io->type) {
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IOV_MD:
		kv_emu_exec(io->bdev->ctxt, io);
		return;
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return;
	default:
		/* READ/WRITE 不该到这里:补丁让 nvmf 把所有 I/O 都走透传 */
		SPDK_ERRLOG("kv_emu 收到非透传 I/O type %d —— nvmf 补丁没打?\n",
			    io->type);
		spdk_bdev_io_complete(io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}
}

static bool
kv_emu_io_type_supported(void *ctx, enum spdk_bdev_io_type t)
{
	return t == SPDK_BDEV_IO_TYPE_NVME_IO || t == SPDK_BDEV_IO_TYPE_RESET ||
	       t == SPDK_BDEV_IO_TYPE_FLUSH;
}

static int
kv_emu_ch_create(void *io_device, void *ctx_buf)
{
	return 0;
}

static void
kv_emu_ch_destroy(void *io_device, void *ctx_buf)
{
}

static struct spdk_io_channel *
kv_emu_get_io_channel(void *ctx)
{
	return spdk_get_io_channel(ctx);
}

static int
kv_emu_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct kv_emu *d = ctx;

	spdk_json_write_named_object_begin(w, "kv_emu");
	spdk_json_write_named_uint64(w, "capacity", d->capacity);
	spdk_json_write_named_uint32(w, "max_value", d->max_value);
	spdk_json_write_named_bool(w, "ednek", d->ednek);
	spdk_json_write_object_end(w);
	return 0;
}

static const struct spdk_bdev_fn_table kv_emu_fn_table = {
	.destruct		= kv_emu_destruct,
	.submit_request		= kv_emu_submit,
	.io_type_supported	= kv_emu_io_type_supported,
	.get_io_channel		= kv_emu_get_io_channel,
	.dump_info_json		= kv_emu_dump_info_json,
};

static int
kv_emu_create(const char *name, uint64_t capacity, uint32_t max_value,
	      bool ednek)
{
	struct kv_emu *d;
	int rc;

	d = calloc(1, sizeof(*d));
	if (!d) {
		return -ENOMEM;
	}
	pthread_mutex_init(&d->lock, NULL);
	pthread_mutex_init(&d->pool.lock, NULL);
	d->bucket = calloc(KV_EMU_NBUCKET, sizeof(*d->bucket));
	d->bdev.name = strdup(name);
	if (!d->bucket || !d->bdev.name) {
		kv_emu_free(d);
		return -ENOMEM;
	}
	d->capacity = capacity;
	d->max_value = max_value;
	d->ednek = ednek;

	d->bdev.product_name = "KV emulator";
	d->bdev.module = &kv_emu_if;
	d->bdev.ctxt = d;
	d->bdev.fn_table = &kv_emu_fn_table;
	d->bdev.blocklen = KV_EMU_BLOCKLEN;
	d->bdev.blockcnt = spdk_max(capacity / KV_EMU_BLOCKLEN, 1);
	d->bdev.write_cache = 0;

	spdk_io_device_register(d, kv_emu_ch_create, kv_emu_ch_destroy, 0, name);
	rc = spdk_bdev_register(&d->bdev);
	if (rc) {
		spdk_io_device_unregister(d, NULL);
		kv_emu_free(d);
		return rc;
	}
	TAILQ_INSERT_TAIL(&g_kv_emus, d, link);
	SPDK_NOTICELOG("kv_emu %s: capacity %" PRIu64 " B, max value %u B, EDNEK %d\n",
		       name, capacity, max_value, ednek);
	return 0;
}

static struct kv_emu *
kv_emu_by_name(const char *name)
{
	struct kv_emu *d;

	TAILQ_FOREACH(d, &g_kv_emus, link) {
		if (strcmp(d->bdev.name, name) == 0) {
			return d;
		}
	}
	return NULL;
}

static int
kv_emu_init(void)
{
	return 0;
}

static void
kv_emu_fini(void)
{
}

/* ================================================================== */
/* RPC                                                                 */
/* ================================================================== */

struct rpc_kv_emu {
	char		*name;
	uint64_t	capacity_mb;
	uint32_t	max_value;
	bool		ednek;
};

static const struct spdk_json_object_decoder rpc_kv_emu_create_dec[] = {
	{"name", offsetof(struct rpc_kv_emu, name), spdk_json_decode_string},
	{"capacity_mb", offsetof(struct rpc_kv_emu, capacity_mb), spdk_json_decode_uint64, true},
	{"max_value", offsetof(struct rpc_kv_emu, max_value), spdk_json_decode_uint32, true},
	{"ednek", offsetof(struct rpc_kv_emu, ednek), spdk_json_decode_bool, true},
};

static const struct spdk_json_object_decoder rpc_kv_emu_name_dec[] = {
	{"name", offsetof(struct rpc_kv_emu, name), spdk_json_decode_string},
};

static void
rpc_bdev_kv_emu_create(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_kv_emu req = {
		.capacity_mb = 1024,
		.max_value = 4 * 1024 * 1024,
	};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_kv_emu_create_dec,
				    SPDK_COUNTOF(rpc_kv_emu_create_dec), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "参数解析失败");
		goto out;
	}
	rc = kv_emu_create(req.name, req.capacity_mb * 1024 * 1024, req.max_value,
			   req.ednek);
	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto out;
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_string(w, req.name);
	spdk_jsonrpc_end_result(request, w);
out:
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_kv_emu_create", rpc_bdev_kv_emu_create, SPDK_RPC_RUNTIME)

static void
rpc_bdev_kv_emu_stats(struct spdk_jsonrpc_request *request,
		      const struct spdk_json_val *params)
{
	static const struct {
		uint8_t		opc;
		const char	*name;
	} ops[] = {
		{ KV_OPC_STORE, "store" }, { KV_OPC_RETRIEVE, "retrieve" },
		{ KV_OPC_LIST, "list" }, { KV_OPC_DELETE, "delete" },
		{ KV_OPC_EXIST, "exist" }, { KV_OPC_FLUSH, "flush" },
	};
	struct rpc_kv_emu req = {};
	struct spdk_json_write_ctx *w;
	struct kv_emu *d;
	uint64_t other = 0;
	unsigned i;

	if (spdk_json_decode_object(params, rpc_kv_emu_name_dec,
				    SPDK_COUNTOF(rpc_kv_emu_name_dec), &req) ||
	    !(d = kv_emu_by_name(req.name))) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "没有这个 kv_emu bdev");
		free(req.name);
		return;
	}
	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	pthread_mutex_lock(&d->lock);
	spdk_json_write_named_uint64(w, "keys", d->nkeys);
	spdk_json_write_named_uint64(w, "used_bytes", d->used);
	pthread_mutex_unlock(&d->lock);
	spdk_json_write_named_uint64(w, "capacity", d->capacity);
	pthread_mutex_lock(&d->pool.lock);
	spdk_json_write_named_object_begin(w, "value_pool");
	spdk_json_write_named_uint64(w, "hugepage_bytes", d->pool.bytes);
	spdk_json_write_named_uint64(w, "reused", d->pool.hits);
	spdk_json_write_named_uint64(w, "allocated", d->pool.misses);
	spdk_json_write_object_end(w);
	pthread_mutex_unlock(&d->pool.lock);
	for (i = 0; i < SPDK_COUNTOF(ops); i++) {
		spdk_json_write_named_object_begin(w, ops[i].name);
		spdk_json_write_named_uint64(w, "count", d->cnt[ops[i].opc]);
		spdk_json_write_named_uint64(w, "errors", d->err[ops[i].opc]);
		spdk_json_write_object_end(w);
	}
	for (i = 0; i < 256; i++) {
		if (i != KV_OPC_STORE && i != KV_OPC_RETRIEVE && i != KV_OPC_LIST &&
		    i != KV_OPC_DELETE && i != KV_OPC_EXIST && i != KV_OPC_FLUSH) {
			other += d->cnt[i];
		}
	}
	spdk_json_write_named_uint64(w, "other_opcodes", other);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_kv_emu_stats", rpc_bdev_kv_emu_stats, SPDK_RPC_RUNTIME)

static void
rpc_kv_emu_delete_done(void *cb_arg, int rc)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (rc) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
	} else {
		spdk_jsonrpc_send_bool_response(request, true);
	}
}

static void
rpc_bdev_kv_emu_delete(struct spdk_jsonrpc_request *request,
		       const struct spdk_json_val *params)
{
	struct rpc_kv_emu req = {};

	if (spdk_json_decode_object(params, rpc_kv_emu_name_dec,
				    SPDK_COUNTOF(rpc_kv_emu_name_dec), &req) ||
	    !kv_emu_by_name(req.name)) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "没有这个 kv_emu bdev");
		free(req.name);
		return;
	}
	spdk_bdev_unregister_by_name(req.name, &kv_emu_if, rpc_kv_emu_delete_done, request);
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_kv_emu_delete", rpc_bdev_kv_emu_delete, SPDK_RPC_RUNTIME)

