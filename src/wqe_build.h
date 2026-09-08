/*
 * wqe_build.h  --  mlx5 SEND WQE 与 NVMe-oF 命令胶囊的构造
 *
 * 一份代码,host 和 device 都能调。
 *
 * 这么做是为了先在 host 上把字节布局验对 —— kernel 里调这个太痛苦:
 * 不能 printf 到有用的地方,不能 gdb,不能 dump 内存。而 WQE 的位域
 * 填错一个,网卡要么静默丢弃,要么回一个含糊的 CQE error。
 *
 * 先让 host 构造一条,和 SPDK 通过 ibv_post_send 实际产出的逐字节比对,
 * 对上了再原样搬进 kernel。
 *
 * ============ 一个必须注意的差异 ============
 *
 * mxshmem 的 IBGDA_MEMBAR() 用的是 __threadfence(),因为它的队列在显存里
 * (源码里被注释掉的原始逻辑写着:nic_buf_on_gpumem ? threadfence :
 *  threadfence_system)。
 *
 * 而我们这条路径上,SQ 和 dbrec 是 SPDK 用 ibv_create_qp 建的,
 * 在主机内存里 —— 必须用 __threadfence_system(),否则网卡可能
 * 看不到 WQE,或者看到的顺序不对(门铃响了但 WQE 还没落地)。
 *
 * 照抄 mxshmem 的 fence 会踩这个坑。等以后把队列搬进显存(真 IBGDA
 * 形态),再换回轻量的 __threadfence()。
 */

#ifndef WQE_BUILD_H
#define WQE_BUILD_H

#include <stdint.h>
#include <string.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

/* ------------------------------------------------------------------ */
/* host / device 双端修饰                                              */
/* ------------------------------------------------------------------ */

#if defined(__MACACC__) || defined(__CUDACC__) || defined(USE_MACA_DEVICE)
  #define WQE_FN	__host__ __device__ static inline
  #define WQE_DEVICE	1
#else
  #define WQE_FN	static inline
  #define WQE_DEVICE	0
#endif

/*
 * 内存序。
 *
 * WQE_FENCE_QUEUE : 保证 WQE 写入对网卡可见,用在更新 dbrec 之前
 * WQE_FENCE_DB    : 保证 dbrec 更新先于门铃,用在敲铃之前
 *
 * 队列在主机内存 -> system 级;在显存 -> device 级即可。
 * 默认按主机内存处理,这是当前 SPDK 路径的实际情况。
 */
#ifndef WQE_QUEUE_ON_GPUMEM
#define WQE_QUEUE_ON_GPUMEM 0
#endif

#if WQE_DEVICE
  #if WQE_QUEUE_ON_GPUMEM
    #define WQE_FENCE_QUEUE()	__threadfence()
    #define WQE_FENCE_DB()	__threadfence()
  #else
    #define WQE_FENCE_QUEUE()	__threadfence_system()
    #define WQE_FENCE_DB()	__threadfence_system()
  #endif
#else
  #define WQE_FENCE_QUEUE()	__sync_synchronize()
  #define WQE_FENCE_DB()	__sync_synchronize()
#endif

/* 字节序。device 端没有 htobe32,自己实现。 */
WQE_FN uint32_t wqe_hto_be32(uint32_t x)
{
	return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
	       ((x & 0x00ff0000u) >> 8)  | ((x & 0xff000000u) >> 24);
}

WQE_FN uint64_t wqe_hto_be64(uint64_t x)
{
	return ((uint64_t)wqe_hto_be32((uint32_t)(x & 0xffffffffu)) << 32) |
	       wqe_hto_be32((uint32_t)(x >> 32));
}

/* ------------------------------------------------------------------ */
/* NVMe 命令胶囊(64 字节 SQE)                                        */
/* ------------------------------------------------------------------ */

/*
 * 只定义我们要填的字段,避免依赖 SPDK 头文件 —— device 端包不进来。
 * 布局照 NVMe spec,和 struct spdk_nvme_cmd 一致。
 */
struct nvme_sqe {
	uint8_t		opc;		/* opcode */
	uint8_t		fuse_psdt;	/* [1:0] fuse, [7:6] psdt */
	uint16_t	cid;
	uint32_t	nsid;
	uint32_t	cdw2;
	uint32_t	cdw3;
	uint64_t	mptr;
	/*
	 * dptr: Keyed SGL Data Block descriptor,16 字节。
	 * 布局由实测比对确定(NVMe spec Figure "SGL Descriptor"):
	 *   [0..7]   address   8 字节
	 *   [8..10]  length    3 字节 —— 24 位,故单 SGL 上限 16 MB
	 *   [11..14] key       4 字节
	 *   [15]     identifier: [7:4] type, [3:0] subtype
	 *
	 * 早先写成 length 4 字节 + key 3 字节,导致从 key 起整体错位
	 * 一个字节。target 会把 rkey 解析错,I/O 静默失败。
	 */
	uint64_t	sgl_addr;
	uint8_t		sgl_length[3];
	uint8_t		sgl_key[4];
	uint8_t		sgl_type;
	uint32_t	cdw10;		/* slba 低 32 位 */
	uint32_t	cdw11;		/* slba 高 32 位 */
	uint32_t	cdw12;		/* [15:0] nlb-1 */
	uint32_t	cdw13;
	uint32_t	cdw14;
	uint32_t	cdw15;
} __attribute__((packed));

/* NVMe SGL:keyed data block,地址型 */
#define NVME_SGL_TYPE_KEYED_DATA_BLOCK	0x4
#define NVME_SGL_SUBTYPE_ADDRESS	0x0
#define NVME_PSDT_SGL_MPTR_CONTIG	0x1	/* psdt 值,写在 [7:6] */

#define NVME_OPC_READ			0x02
#define NVME_OPC_WRITE			0x01

/*
 * 构造一条 NVMe 读/写命令。
 *
 * gpu_addr / rkey 就是你显存 MR 的地址和 rkey —— target 侧网卡拿着
 * 它们直接读写显存,这正是已经验证过的那条路。
 */
WQE_FN void
nvme_build_rw_sqe(struct nvme_sqe *cmd, uint8_t opc, uint16_t cid,
		  uint32_t nsid, uint64_t slba, uint32_t nlb,
		  uint64_t gpu_addr, uint32_t rkey, uint32_t byte_len)
{
	uint32_t key_le;
	int i;

	for (i = 0; i < (int)sizeof(*cmd); i++) {
		((uint8_t *)cmd)[i] = 0;
	}

	cmd->opc       = opc;
	cmd->fuse_psdt = (uint8_t)(NVME_PSDT_SGL_MPTR_CONTIG << 6);
	cmd->cid       = cid;
	cmd->nsid      = nsid;

	/* SGL descriptor。注意这些字段是小端(NVMe 是小端协议),
	 * 和 mlx5 WQE 的大端不同,别搞混。 */
	cmd->sgl_addr = gpu_addr;

	cmd->sgl_length[0] = (uint8_t)(byte_len & 0xff);
	cmd->sgl_length[1] = (uint8_t)((byte_len >> 8) & 0xff);
	cmd->sgl_length[2] = (uint8_t)((byte_len >> 16) & 0xff);

	key_le = rkey;
	cmd->sgl_key[0] = (uint8_t)(key_le & 0xff);
	cmd->sgl_key[1] = (uint8_t)((key_le >> 8) & 0xff);
	cmd->sgl_key[2] = (uint8_t)((key_le >> 16) & 0xff);
	cmd->sgl_key[3] = (uint8_t)((key_le >> 24) & 0xff);

	cmd->sgl_type = (uint8_t)((NVME_SGL_TYPE_KEYED_DATA_BLOCK << 4) |
				  NVME_SGL_SUBTYPE_ADDRESS);

	cmd->cdw10 = (uint32_t)(slba & 0xffffffffu);
	cmd->cdw11 = (uint32_t)(slba >> 32);
	cmd->cdw12 = (nlb - 1) & 0xffff;
}

/* ------------------------------------------------------------------ */
/* mlx5 SEND WQE                                                       */
/* ------------------------------------------------------------------ */

/*
 * NVMe-oF 的命令 WQE 比 mxshmem 里的 RDMA_WRITE 简单:
 * 数据搬运由 target 侧发起,我们只要把 64 字节胶囊 SEND 过去。
 *
 *   [ctrl_seg 16B][data_seg 16B]  -> ds = 2
 *
 * ctrl segment 的前 8 字节(opmod_idx_opcode + qpn_ds)后面敲门铃时
 * 还要用一次,所以单独返回。
 */
WQE_FN uint64_t
mlx5_build_send_wqe(void *sq_slot, uint16_t wqe_idx, uint32_t qpn,
		    uint64_t capsule_addr, uint32_t capsule_lkey,
		    uint32_t capsule_len)
{
	/*
	 * 直接按 32 位字构造,不经过 struct mlx5_wqe_ctrl_seg。
	 *
	 * 早先的写法是先填结构体成员再用 uint32_t* 拷出去,结果
	 * fm_ce_se 那个 uint8_t 写入被 -O2 优化掉了 —— 通过 uint32_t*
	 * 读一个 uint8_t 成员是严格别名违例,编译器认为两者不别名,
	 * 可以把那次写消除。表现是 WQE 里 offset 11 恒为 0,
	 * 而 fm_ce_se=0 意味着不产生 CQE,I/O 完成了 CQ 里却空空如也。
	 *
	 * 布局(mlx5dv.h,packed):
	 *   [0..3]   opmod_idx_opcode
	 *   [4..7]   qpn_ds
	 *   [8]      signature
	 *   [9..10]  dci_stream_channel_id
	 *   [11]     fm_ce_se
	 *   [12..15] imm
	 */
	uint32_t *dst = (uint32_t *)sq_slot;
	const uint32_t ds = 2;	/* ctrl + data,各 16B */

	dst[0] = wqe_hto_be32(((uint32_t)wqe_idx << 8) | MLX5_OPCODE_SEND);
	dst[1] = wqe_hto_be32((qpn << 8) | ds);
	/* 小端机器上,offset 11 落在这个字的最高字节 */
	dst[2] = ((uint32_t)MLX5_WQE_CTRL_CQ_UPDATE) << 24;
	dst[3] = 0;			/* imm */

	/* data segment: byte_count / lkey / addr */
	dst[4] = wqe_hto_be32(capsule_len);
	dst[5] = wqe_hto_be32(capsule_lkey);
	dst[6] = wqe_hto_be32((uint32_t)(capsule_addr >> 32));
	dst[7] = wqe_hto_be32((uint32_t)(capsule_addr & 0xffffffffu));

	/* 门铃要写的 8 字节:只带下一个 index 和 qpn,opcode 位留空 */
	{
		uint32_t db[2];

		db[0] = wqe_hto_be32((uint32_t)(wqe_idx + 1) << 8);
		db[1] = wqe_hto_be32(qpn << 8);
		return ((uint64_t)db[1] << 32) | db[0];
	}
}

/* ------------------------------------------------------------------ */
/* 提交:更新 dbrec + 敲门铃                                           */
/* ------------------------------------------------------------------ */

/*
 * 顺序和 fence 照抄 ibgda_post_send:
 *   WQE 落地 -> fence -> 更新 dbrec -> fence -> 敲铃
 * 两个 fence 一个都不能省。
 */
WQE_FN void
mlx5_submit(volatile uint32_t *dbrec, volatile uint64_t *bf_reg,
	    uint16_t next_prod_idx, uint64_t db_val)
{
	WQE_FENCE_QUEUE();

	/* dbrec 里放的是下一个空闲 WQEBB 的索引,大端 16 位 */
	*dbrec = wqe_hto_be32((uint32_t)next_prod_idx & 0xffff);

	WQE_FENCE_DB();

	/* BlueFlame:8 字节 MMIO 写 */
	*bf_reg = db_val;

	WQE_FENCE_DB();
}

/* ------------------------------------------------------------------ */
/* CQE 解析                                                            */
/* ------------------------------------------------------------------ */

/*
 * mlx5 CQE 是 64 字节,最后一个字节的 bit0 是 owner bit。
 * 软件维护一个 phase,phase 对上就说明这个 CQE 是新的。
 */
#define MLX5_CQE_OWNER_MASK	0x1
#define MLX5_CQE_OPCODE_SHIFT	4

WQE_FN int
mlx5_cqe_is_valid(const void *cqe, uint32_t cqe_size, uint8_t phase)
{
	uint8_t op_own = ((const uint8_t *)cqe)[cqe_size - 1];

	return (op_own & MLX5_CQE_OWNER_MASK) == (phase & 1);
}

WQE_FN uint8_t
mlx5_cqe_opcode(const void *cqe, uint32_t cqe_size)
{
	uint8_t op_own = ((const uint8_t *)cqe)[cqe_size - 1];

	return op_own >> MLX5_CQE_OPCODE_SHIFT;
}

#define MLX5_CQE_RESP_SEND		2
#define MLX5_CQE_REQ			0
#define MLX5_CQE_REQ_ERR		13
#define MLX5_CQE_RESP_ERR		14

#endif /* WQE_BUILD_H */
