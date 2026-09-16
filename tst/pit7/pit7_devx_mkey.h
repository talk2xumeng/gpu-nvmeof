/*
 * pit7_devx_mkey.h  --  用 DEVX umem + CREATE_MKEY 给显存建 MR(-R 2/3)
 *
 * mxshmem / Mooncake 的 IBGDA 把网卡要读的显存(队列)用
 * mlx5dv_devx_umem_reg_ex(+DMABUF)注册,而不是 ibv_reg_dmabuf_mr。
 * 这里复现那条注册路径,再用 DEVX 发 CREATE_MKEY 把 umem 包成一个
 * 普通的 mkey —— 同一个 PD 下,SPDK 用标准 verbs 建的 QP 可以直接用它
 * 当 lkey / rkey。
 *
 *   -R 2  mlx5dv_devx_umem_reg_ex,comp_mask = DMABUF(mxshmem 的主路径)
 *   -R 3  mlx5dv_devx_umem_reg,传 GPU VA(peermem;mxshmem 的回退路径,
 *         Mooncake 的 GPU-VA control region)
 *
 * CREATE_MKEY 的字段布局和填法照 DPDK drivers/common/mlx5:
 *   mlx5_prm.h                mlx5_ifc_mkc_bits / create_mkey_in_bits
 *   mlx5_devx_cmds.c          mlx5_devx_cmd_mkey_create()
 * (BSD-3-Clause, Copyright 2016 6WIND S.A., Mellanox Technologies, Ltd)
 * 只摘了这里用到的结构。Mooncake 那份 mlx5_ifc.h 的 create_mkey_in
 * 没有 mkey_umem_id 字段(并进了 reserved),不能用。
 *
 * umem 页大小固定 4 KiB(pgsz_bitmap 只给 4K),和 mkey 的 log_page_size
 * 对齐。mxshmem 允许任意 >= 4K 的页,这一点和它不同 —— 页大小不太可能
 * 是可见性的决定因素,先求 MTT 不出错。
 */

#ifndef PIT7_DEVX_MKEY_H
#define PIT7_DEVX_MKEY_H

#include <endian.h>
#include <stdint.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

#define PIT7_OP_CREATE_MKEY		0x200
#define PIT7_MKC_ACCESS_MODE_MTT	0x1

/* ---- IFC 结构(位宽数组,只用来算偏移) ---- */
typedef uint8_t pit7_u8;

struct pit7_ifc_mkc_bits {
	pit7_u8 reserved_at_0[0x1];
	pit7_u8 free[0x1];
	pit7_u8 reserved_at_2[0x1];
	pit7_u8 access_mode_4_2[0x3];
	pit7_u8 reserved_at_6[0x7];
	pit7_u8 relaxed_ordering_write[0x1];
	pit7_u8 reserved_at_e[0x1];
	pit7_u8 small_fence_on_rdma_read_response[0x1];
	pit7_u8 umr_en[0x1];
	pit7_u8 a[0x1];
	pit7_u8 rw[0x1];
	pit7_u8 rr[0x1];
	pit7_u8 lw[0x1];
	pit7_u8 lr[0x1];
	pit7_u8 access_mode_1_0[0x2];
	pit7_u8 reserved_at_18[0x8];
	pit7_u8 qpn[0x18];
	pit7_u8 mkey_7_0[0x8];
	pit7_u8 reserved_at_40[0x20];
	pit7_u8 length64[0x1];
	pit7_u8 bsf_en[0x1];
	pit7_u8 sync_umr[0x1];
	pit7_u8 reserved_at_63[0x2];
	pit7_u8 expected_sigerr_count[0x1];
	pit7_u8 reserved_at_66[0x1];
	pit7_u8 en_rinval[0x1];
	pit7_u8 pd[0x18];
	pit7_u8 start_addr[0x40];
	pit7_u8 len[0x40];
	pit7_u8 bsf_octword_size[0x20];
	pit7_u8 reserved_at_120[0x80];
	pit7_u8 translations_octword_size[0x20];
	pit7_u8 reserved_at_1c0[0x16];
	pit7_u8 order_read_after_write[0x2];
	pit7_u8 reserved_at_1d8[0x1];
	pit7_u8 relaxed_ordering_read[0x1];
	pit7_u8 reserved_at_1da[0x1];
	pit7_u8 log_page_size[0x5];
	pit7_u8 reserved_at_1e0[0x3];
	pit7_u8 crypto_en[0x2];
	pit7_u8 reserved_at_1e5[0x1b];
};

struct pit7_ifc_create_mkey_in_bits {
	pit7_u8 opcode[0x10];
	pit7_u8 reserved_at_10[0x10];
	pit7_u8 reserved_at_20[0x10];
	pit7_u8 op_mod[0x10];
	pit7_u8 reserved_at_40[0x20];
	pit7_u8 pg_access[0x1];
	pit7_u8 reserved_at_61[0x1f];
	struct pit7_ifc_mkc_bits memory_key_mkey_entry;
	pit7_u8 reserved_at_280[0x80];
	pit7_u8 translations_octword_actual_size[0x20];
	pit7_u8 mkey_umem_id[0x20];
	pit7_u8 mkey_umem_offset[0x40];
	pit7_u8 reserved_at_380[0x500];
};

struct pit7_ifc_create_mkey_out_bits {
	pit7_u8 status[0x8];
	pit7_u8 reserved_at_8[0x18];
	pit7_u8 syndrome[0x20];
	pit7_u8 reserved_at_40[0x8];
	pit7_u8 mkey_index[0x18];
	pit7_u8 reserved_at_60[0x20];
};

/* ---- 访问宏(同 mlx5 PRM:大端,按 32 位字) ---- */
#define PIT7_BIT_OFF(typ, fld) \
	((unsigned)(uintptr_t)&(((struct pit7_ifc_##typ##_bits *)0)->fld))
#define PIT7_BIT_SZ(typ, fld) \
	((unsigned)sizeof(((struct pit7_ifc_##typ##_bits *)0)->fld))
#define PIT7_SZ_BYTES(typ)	(sizeof(struct pit7_ifc_##typ##_bits) / 8)

static inline void
pit7_set(uint8_t *p, unsigned off, unsigned sz, uint32_t v)
{
	uint32_t w, mask, sh;

	if (sz == 32) {
		w = htobe32(v);
		memcpy(p + off / 8, &w, 4);
		return;
	}
	memcpy(&w, p + (off / 32) * 4, 4);
	w = be32toh(w);
	sh = 32 - sz - (off & 0x1f);
	mask = ((1u << sz) - 1) << sh;
	w = (w & ~mask) | ((v << sh) & mask);
	w = htobe32(w);
	memcpy(p + (off / 32) * 4, &w, 4);
}

static inline void
pit7_set64(uint8_t *p, unsigned off, uint64_t v)
{
	uint64_t w = htobe64(v);

	memcpy(p + off / 8, &w, 8);
}

static inline uint32_t
pit7_get(const uint8_t *p, unsigned off, unsigned sz)
{
	uint32_t w;

	memcpy(&w, p + (off / 32) * 4, 4);
	w = be32toh(w);
	if (sz == 32) {
		return w;
	}
	return (w >> (32 - sz - (off & 0x1f))) & ((1u << sz) - 1);
}

#define PIT7_SET(typ, p, fld, v) \
	pit7_set((uint8_t *)(p), PIT7_BIT_OFF(typ, fld), PIT7_BIT_SZ(typ, fld), (v))
#define PIT7_SET_MKC(p, fld, v) \
	pit7_set((uint8_t *)(p), \
		 PIT7_BIT_OFF(create_mkey_in, memory_key_mkey_entry) + \
		 PIT7_BIT_OFF(mkc, fld), PIT7_BIT_SZ(mkc, fld), (v))
#define PIT7_SET64_MKC(p, fld, v) \
	pit7_set64((uint8_t *)(p), \
		   PIT7_BIT_OFF(create_mkey_in, memory_key_mkey_entry) + \
		   PIT7_BIT_OFF(mkc, fld), (v))
#define PIT7_GET(typ, p, fld) \
	pit7_get((const uint8_t *)(p), PIT7_BIT_OFF(typ, fld), PIT7_BIT_SZ(typ, fld))

/* ---- 对外 ---- */

struct pit7_devx_mr {
	struct ibv_mr		mr;	/* 只填 lkey/rkey/addr/length/pd/context */
	struct mlx5dv_devx_umem	*umem;
	struct mlx5dv_devx_obj	*mkey;
};

/*
 * dmabuf_fd >= 0:umem_reg_ex + DMABUF,fd 的 offset 0 必须就是 va(坑 6)
 * dmabuf_fd <  0:umem_reg(GPU VA),走 peermem
 * 返回的 ibv_mr 只能读 key,不能交给 ibv_dereg_mr,用 pit7_devx_dereg_mr。
 */
static inline struct ibv_mr *
pit7_devx_reg_mr(struct ibv_pd *pd, void *va, size_t len, int dmabuf_fd,
		 int acc, const char **why)
{
	const size_t pg = 4096;
	struct pit7_devx_mr *m;
	struct mlx5dv_obj obj;
	struct mlx5dv_pd dvpd;
	uint8_t in[PIT7_SZ_BYTES(create_mkey_in)];
	uint8_t out[PIT7_SZ_BYTES(create_mkey_out)];
	uint32_t umem_id, tsize, idx;
	size_t alen = (len + pg - 1) & ~(pg - 1);

	*why = "";
	m = (struct pit7_devx_mr *)calloc(1, sizeof(*m));
	if (!m) {
		*why = "calloc";
		return NULL;
	}

	memset(&obj, 0, sizeof(obj));
	obj.pd.in = pd;
	obj.pd.out = &dvpd;
	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD)) {
		*why = "mlx5dv_init_obj(PD)";
		goto fail;
	}

	if (dmabuf_fd >= 0) {
		struct mlx5dv_devx_umem_in ui;

		memset(&ui, 0, sizeof(ui));
		ui.addr = 0;			/* dma-buf 内的偏移 */
		ui.size = alen;
		ui.access = acc;
		ui.pgsz_bitmap = pg;
		ui.comp_mask = MLX5DV_UMEM_MASK_DMABUF;
		ui.dmabuf_fd = dmabuf_fd;
		m->umem = mlx5dv_devx_umem_reg_ex(pd->context, &ui);
		if (!m->umem) {
			*why = "mlx5dv_devx_umem_reg_ex(DMABUF)";
			goto fail;
		}
	} else {
		m->umem = mlx5dv_devx_umem_reg(pd->context, va, alen, acc);
		if (!m->umem) {
			*why = "mlx5dv_devx_umem_reg(VA)";
			goto fail;
		}
	}
	umem_id = m->umem->umem_id;

	/* 照 DPDK mlx5_devx_cmd_mkey_create(),direct MTT、无 KLM */
	memset(in, 0, sizeof(in));
	memset(out, 0, sizeof(out));
	tsize = (uint32_t)((alen * 8) / 16);
	PIT7_SET(create_mkey_in, in, opcode, PIT7_OP_CREATE_MKEY);
	PIT7_SET_MKC(in, access_mode_1_0, PIT7_MKC_ACCESS_MODE_MTT);
	PIT7_SET_MKC(in, log_page_size, 12);
	PIT7_SET(create_mkey_in, in, translations_octword_actual_size, tsize);
	PIT7_SET(create_mkey_in, in, mkey_umem_id, umem_id);
	PIT7_SET(create_mkey_in, in, pg_access, 0);
	PIT7_SET_MKC(in, lw, 1);
	PIT7_SET_MKC(in, lr, 1);
	PIT7_SET_MKC(in, rw, 1);
	PIT7_SET_MKC(in, rr, 1);
	PIT7_SET_MKC(in, qpn, 0xffffff);
	PIT7_SET_MKC(in, pd, dvpd.pdn);
	PIT7_SET_MKC(in, mkey_7_0, umem_id & 0xff);
	PIT7_SET_MKC(in, translations_octword_size, tsize);
	PIT7_SET64_MKC(in, start_addr, (uint64_t)(uintptr_t)va);
	PIT7_SET64_MKC(in, len, alen);

	m->mkey = mlx5dv_devx_obj_create(pd->context, in, sizeof(in),
					 out, sizeof(out));
	if (!m->mkey) {
		static char buf[96];

		snprintf(buf, sizeof(buf),
			 "CREATE_MKEY status=0x%x syndrome=0x%x",
			 PIT7_GET(create_mkey_out, out, status),
			 PIT7_GET(create_mkey_out, out, syndrome));
		*why = buf;
		goto fail;
	}
	idx = PIT7_GET(create_mkey_out, out, mkey_index);

	m->mr.context = pd->context;
	m->mr.pd = pd;
	m->mr.addr = va;
	m->mr.length = alen;
	m->mr.lkey = (idx << 8) | (umem_id & 0xff);
	m->mr.rkey = m->mr.lkey;
	return &m->mr;

fail:
	if (m->umem) {
		mlx5dv_devx_umem_dereg(m->umem);
	}
	free(m);
	return NULL;
}

static inline void
pit7_devx_dereg_mr(struct ibv_mr *mr)
{
	struct pit7_devx_mr *m = (struct pit7_devx_mr *)mr;	/* mr 是首成员 */

	if (!m) {
		return;
	}
	if (m->mkey) {
		mlx5dv_devx_obj_destroy(m->mkey);	/* 先拆 mkey 再拆 umem */
	}
	if (m->umem) {
		mlx5dv_devx_umem_dereg(m->umem);
	}
	free(m);
}

#endif /* PIT7_DEVX_MKEY_H */
