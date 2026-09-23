/*
 * gpu_io_kernel.cpp  --  kernel 内完成 NVMe-oF 读写的全流程
 *
 * 稳态下 CPU 完全不参与。一次 launch,QD + 1 个 block,每个 block 只有
 * thread 0 干活,角色由 blockIdx 决定:
 *
 *   block 0       poller:独占 CQ。收 RESP → 从 recv buffer 读出 cid 和
 *                 NVMe status → 按 cid 找到 tag,置完成
 *   block 1..QD   submitter:领任务 → 写胶囊 → 抢锁写 WQE/dbrec/门铃 →
 *                 等自己的 tag 完成
 *
 * 角色不能放在同一个 block 的不同线程里:同一 block 的线程锁步执行,
 * 分支不同时会被串行化 —— submitter 会独占着空等到超时,poller 一圈
 * 都转不了。
 *
 * 设计见 docs/concurrency-design.md。WQE / 胶囊布局在 wqe_build.h,
 * 已和 SPDK 实际提交的逐字节比对过。
 *
 * 约束:
 * 1. SQ/CQ/dbrec/recv buffer 在主机内存(SPDK 建的),fence 用 system 级
 * 2. 接管后这个 qpair 不能再调 spdk_nvme_ns_cmd_*(坑 3)
 * 3. C1 阶段 kernel 不补 recv buffer,总 I/O 数不能超过已 post 的数量
 * 4. clock64() 是每线程各自的计数,跨线程的时间戳不能相减 ——
 *    等待时间只在 submitter 自己的线程里量
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>
#include <string.h>
#include "gpu_io_ctx.h"

#define USE_MACA_DEVICE 1
#include "wqe_build.h"

#define GIO_TIMEOUT	2000000000ULL	/* ~1.25 s @ 1.6 GHz */

/* ---- 线程间可见的读写(同 Mooncake maca_ops.cuh 的 ld_acquire/st_release) ---- */

__device__ static inline uint32_t
ld_acq32(uint32_t *p)
{
	__threadfence_system();
	return *(volatile uint32_t *)p;
}

__device__ static inline void
st_rel32(uint32_t *p, uint32_t v)
{
	*(volatile uint32_t *)p = v;
	__threadfence_system();
}

__device__ static void
set_abort(struct gio_shared *sh, int err)
{
	if (atomicCAS(&sh->abort, 0u, 1u) == 0u) {
		st_rel32((uint32_t *)&sh->err, (uint32_t)err);
	}
}

/* ================================================================== */
/* poller                                                              */
/* ================================================================== */

/* cqe64 offset 48:网卡的自由运行计数器,大端 */
__device__ static inline uint64_t
cqe64_ts(volatile uint8_t *cqe64)
{
	return wqe_hto_be64(*(volatile uint64_t *)(cqe64 + 48));
}

/*
 * 处理一个 RESP_SEND:取到响应胶囊 → 读 NVMe CQE → 置 tag 完成。
 *
 * 响应胶囊在哪(见 rdma-core providers/mlx5/cq.c handle_responder):
 *   op_own & 0x4  MLX5_INLINE_SCATTER_32:数据就在本条 cqe64 的开头
 *   op_own & 0x8  MLX5_INLINE_SCATTER_64:数据在 cqe64 前面那 64 字节
 *   否则          网卡 DMA 写进了 RQ 槽指向的 recv buffer
 * scatter-to-CQE 时网卡不写 recv buffer,是 libmlx5 在 poll_cq 里拷过去的
 * —— kernel 绕过 libmlx5,所以必须直接读 CQE。坑 11。
 *
 * NVMe CQE(16 字节,小端):
 *   [12..13] cid
 *   [14..15] status:bit0 = phase tag,bit1..8 = SC,bit9..11 = SCT
 */
__device__ static void
handle_resp(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags,
	    volatile uint8_t *cqe64, uint8_t op_own, uint16_t wqe_counter,
	    uint32_t seq)
{
	uint64_t w;		/* NVMe CQE 的第 8..15 字节:sqhd sqid cid status */
	uint16_t cid, status;
	uint32_t tag;

	/*
	 * 计数器只有 poller 写、host 在同步后读,不用 fence。
	 * 跨 block 可见的只有 tag 状态,最后统一 fence 一次。
	 */
	if (((c->rq_tail + seq) & 0xffff) != wqe_counter) {
		sh->wc_mismatch++;
	}

	/*
	 * 主存上每次 volatile 读都是一次 PCIe 往返(~0.7 us),逐字节读 6 次
	 * 就是 4 us。整字读:CQE、recv buffer、RQ 槽都按 8 字节对齐。
	 */
	if (op_own & 0x4) {
		w = *(volatile uint64_t *)(cqe64 + 8);
		sh->resp_inline++;
	} else if (op_own & 0x8) {
		w = *(volatile uint64_t *)(cqe64 - 64 + 8);
		sh->resp_inline++;
	} else {
		uint32_t k = wqe_counter & (c->rq_wqe_cnt - 1);
		volatile uint8_t *seg = c->rq_buf + (size_t)k * c->rq_stride;
		/* mlx5_wqe_data_seg:byte_count(4) lkey(4) addr(8),大端 */
		uint64_t addr = wqe_hto_be64(*(volatile uint64_t *)(seg + 8));

		if (c->rsp_len == 0 || addr < c->rsp_host_base ||
		    addr + 16 > c->rsp_host_base + c->rsp_len) {
			set_abort(sh, GIO_E_RSP_ADDR);
			return;
		}
		w = *(volatile uint64_t *)(c->rsp_dev +
					   (addr - c->rsp_host_base) + 8);
	}
	/* NVMe CQE 小端:[8]sqhd [10]sqid [12]cid [14]status */
	cid = (uint16_t)(w >> 32);
	status = (uint16_t)(w >> 48);

	tag = cid & ~GIO_CID_TAG;
	if ((cid & GIO_CID_TAG) == 0 || tag >= c->qd) {
		if (sh->cid_bad == 0) {
			sh->first_bad_cid = cid;
		}
		sh->cid_bad++;
		set_abort(sh, GIO_E_CID);
		return;
	}

	{
		volatile struct gio_tag *vt = (volatile struct gio_tag *)&tags[tag];
		uint64_t now = clock64();

		vt->det = now - vt->t_post;	/* t_post 早已 fence 过 */
		if (c->opt & 0x80) {
			vt->ts_resp = cqe64_ts(cqe64);
		}
		vt->status = status;
		if ((status >> 1) != 0) {
			if (sh->nvme_err == 0) {
				sh->first_status = status;
			}
			sh->nvme_err++;
			vt->state = GIO_ST_NVME_ERR;
		} else {
			vt->state = GIO_ST_DONE;
		}
		vt->t_done = clock64();
		__threadfence_system();
	}
	sh->done_cnt++;
}

/*
 * 处理一个已确认有效的 CQE(调用方已 fence)。返回处理响应的耗时。
 */
/*
 * cqe64 的最后 8 字节整字读出(小端装载):
 *   byte 56-59 sop_drop_qpn,60-61 wqe_counter(大端),62 signature,63 op_own
 */
#define TAIL_OP_OWN(t)	((uint8_t)((t) >> 56))
#define TAIL_WQE_CTR(t)	((uint16_t)((((t) >> 32) & 0xff) << 8 | (((t) >> 40) & 0xff)))

__device__ static inline uint64_t
cqe_tail(struct gpu_io_ctx *c, volatile uint8_t *cqe)
{
	return *(volatile uint64_t *)(cqe + c->cqe_size - 8);
}

__device__ static inline uint64_t
cqe_ts(struct gpu_io_ctx *c, volatile uint8_t *cqe)
{
	return cqe64_ts(cqe + c->cqe_size - 64);
}

__device__ static uint64_t
consume_one(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags,
	    volatile uint8_t *cqe, uint32_t *seq)
{
	/* 调用方 fence 过了;owner 确认前读到的尾部字不一定是终值,重读一次 */
	uint64_t tail = cqe_tail(c, cqe);
	uint8_t op_own = TAIL_OP_OWN(tail);
	uint8_t opcode = op_own >> 4;
	uint64_t r = 0;

	if (opcode == MLX5_CQE_RESP_SEND) {
		uint16_t wc = TAIL_WQE_CTR(tail);
		uint64_t r0 = clock64();

		sh->cqe_resp++;
		handle_resp(c, sh, tags, cqe + (c->cqe_size - 64), op_own,
			    wc, *seq);
		r = clock64() - r0;
		(*seq)++;
	} else if (opcode == MLX5_CQE_REQ) {
		sh->cqe_req++;
		if (c->opt & 0x80) {
			sh->last_req_ts = cqe_ts(c, cqe);
			sh->last_req_at = clock64();
		}
	} else if (opcode == MLX5_CQE_REQ_ERR ||
		   opcode == MLX5_CQE_RESP_ERR) {
		/* cqe64 在条目末尾;syndrome 在其 offset 55,vendor_err 在 54 */
		uint32_t base = c->cqe_size - 64;

		sh->err_cqe = ((uint32_t)cqe[base + 54] << 8) |
			      cqe[base + 55];
		set_abort(sh, GIO_E_CQE);
	}
	return r;
}

/* 消费一个 CQE 后推进 ci,并立刻还给网卡(出错也要还,否则 CQ 会溢出) */
__device__ static void
advance_ci(struct gpu_io_ctx *c, uint32_t *ci, uint8_t *phase)
{
	(*ci)++;
	if ((*ci & (c->cq_cnt - 1)) == 0) {
		*phase ^= 1;
	}
	c->cq_dbrec[0] = wqe_hto_be32(*ci & 0xffffff);
	WQE_FENCE_DB();
}

__device__ static void
poller(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags)
{
	uint32_t ci = c->cq_ci;
	uint8_t phase = c->cq_phase;
	uint32_t seq = 0;
	uint64_t spins = 0, hits = 0, rcyc = 0;
	uint64_t idle_since = clock64();
	uint64_t pstart = idle_since;

	for (;;) {
		uint32_t idx = ci & (c->cq_cnt - 1);
		volatile uint8_t *cqe = c->cq_buf + (size_t)idx * c->cqe_size;
		uint64_t tail = cqe_tail(c, cqe);
		uint8_t op_own = TAIL_OP_OWN(tail);
		uint8_t opcode = op_own >> 4;

		spins++;

		/*
		 * owner 和 opcode 两个条件缺一不可。空条目 op_own=0xf0,owner 位
		 * 恰好等于首圈的 phase(0) —— 只判 owner 会把空条目当 CQE 消费掉。
		 */
		if ((op_own & 1) != (phase & 1) || opcode == MLX5_CQE_INVALID) {
			if ((c->opt & 2) && (spins & 0xff) != 0) {
				continue;
			}
			/* done_cnt 只有 poller 自己写,不用 fence */
			if (sh->done_cnt >= c->njobs || ld_acq32(&sh->abort)) {
				break;
			}
			if (clock64() - idle_since > GIO_TIMEOUT) {
				set_abort(sh, GIO_E_TIMEOUT);
				break;
			}
			continue;
		}

		/* owner 确认之后再读 CQE 其余字段 */
		__threadfence_system();
		hits++;
		idle_since = clock64();

		rcyc += consume_one(c, sh, tags, cqe, &seq);
		advance_ci(c, &ci, &phase);

		if (ld_acq32(&sh->abort)) {
			break;
		}
	}
	sh->poll_spins = spins;
	sh->poll_hits = hits;
	sh->poll_cycles = clock64() - pstart;
	sh->resp_cycles = rcyc;
}

/*
 * 自轮询(opt bit4):调用方已持 cq_lock。把已经到了的 CQE 最多收 32 个,
 * 别人的完成也按 cid 分发。ci/phase/seq 在共享区里,换人持锁时靠
 * 进出各一次 fence 交接;计数器也只有持锁者写,同样被这两次 fence 覆盖。
 * 不在这里 fence 收尾 —— 调用方释放锁时统一 fence。
 */
__device__ static void
poll_step(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags)
{
	uint32_t ci, seq, n;
	uint8_t phase;
	uint64_t rc = 0;

	__threadfence_system();
	ci = sh->sp_ci;
	phase = (uint8_t)sh->sp_phase;
	seq = sh->sp_seq;
	sh->poll_spins++;

	for (n = 0; n < 32; n++) {
		uint32_t idx = ci & (c->cq_cnt - 1);
		volatile uint8_t *cqe = c->cq_buf + (size_t)idx * c->cqe_size;
		uint64_t tail = cqe_tail(c, cqe);
		uint8_t op_own = TAIL_OP_OWN(tail);

		if ((op_own & 1) != (phase & 1) ||
		    (op_own >> 4) == MLX5_CQE_INVALID) {
			break;
		}
		__threadfence_system();
		sh->poll_hits++;
		rc += consume_one(c, sh, tags, cqe, &seq);
		advance_ci(c, &ci, &phase);
	}
	if (n) {
		*(volatile uint32_t *)&sh->sp_ci = ci;
		*(volatile uint32_t *)&sh->sp_phase = phase;
		*(volatile uint32_t *)&sh->sp_seq = seq;
		sh->resp_cycles += rc;
	}
}

/* ================================================================== */
/* submitter                                                           */
/* ================================================================== */

__device__ static int
claim_job(struct gio_shared *sh, uint32_t njobs, uint32_t *job)
{
	for (;;) {
		uint32_t j = ld_acq32(&sh->next_job);

		if (j >= njobs || ld_acq32(&sh->abort)) {
			return 0;
		}
		if (atomicCAS(&sh->next_job, j, j + 1) == j) {
			*job = j;
			return 1;
		}
	}
}

__device__ static uint64_t
sq_lock(struct gio_shared *sh)
{
	uint64_t spins = 0;

	while (atomicCAS(&sh->sq_lock, 0u, 1u) != 0u) {
		spins++;
	}
	return spins;
}

__device__ static void
sq_unlock(struct gio_shared *sh)
{
	st_rel32(&sh->sq_lock, 0u);
}

/*
 * 锁内:写 WQE → dbrec → BlueFlame 门铃。
 *
 * ctrl seg 的 wqe_idx 和门铃里的索引用 16 位生产者计数 pi,不是取模后
 * 的 slot —— slot 只用来算地址。pi 跑过 sq_wqe_cnt 之后两者就不同了。
 *
 * BlueFlame 内联:整个 WQE(32 字节)写进 BF 寄存器。只写 8 字节也行
 * (网卡回 SQ 取),这里沿用跑通时的形态。
 */
/*
 * 写 WQE → dbrec → BlueFlame 门铃。pi 由调用方给:
 *   正常路径 锁内从共享 sq_pi 取,用完写回(带 fence)
 *   精简路径(opt bit8) 本地算,门铃之后不再写任何东西
 */
__device__ static void
post_wqe(struct gpu_io_ctx *c, uint16_t pi, uint64_t cap_addr)
{
	uint32_t slot = pi & (c->sq_wqe_cnt - 1);
	volatile uint8_t *sq_slot = c->sq_buf + (size_t)slot * c->sq_stride;
	volatile uint64_t *bfp = c->bf_reg;
	const uint64_t *src;
	int w;

	(void)mlx5_build_send_wqe((void *)sq_slot, pi, c->qpn, cap_addr,
				  c->capsule_lkey, 64);
	WQE_FENCE_QUEUE();

	c->qp_dbrec[1] = wqe_hto_be32((uint32_t)(pi + 1) & 0xffff);
	WQE_FENCE_DB();

	src = (const uint64_t *)(void *)sq_slot;
	for (w = 0; w < 4; w++) {
		bfp[w] = src[w];
	}
	WQE_FENCE_DB();

}

__device__ static void
post_locked(struct gpu_io_ctx *c, struct gio_shared *sh, uint64_t cap_addr)
{
	uint16_t pi = (uint16_t)ld_acq32(&sh->sq_pi);

	post_wqe(c, pi, cap_addr);
	st_rel32(&sh->sq_pi, (uint32_t)pi + 1);
}

__device__ static void
submitter(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags,
	  uint32_t tag, uint64_t start_lba, uint64_t *c_cap, uint64_t *c_sub,
	  uint64_t *c_wait, uint64_t *c_spin, uint64_t *c_lock,
	  uint32_t *c_stat, uint64_t *c_det, uint64_t *c_hand,
	  uint64_t *c_tsreq, uint64_t *c_tsresp, uint64_t *c_reqat)
{
	volatile uint8_t *cap = c->capsule + (size_t)tag * 64;
	uint64_t cap_addr = c->capsule_addr + (uint64_t)tag * 64;
	uint32_t nlb = c->io_bytes / c->sector_size;
	uint32_t j;
	/* 串行快路径(opt bit5)的本地 CQ 状态 */
	uint32_t my_ci = c->cq_ci, my_seq = 0;
	uint8_t my_phase = c->cq_phase;

	while (claim_job(sh, c->njobs, &j)) {
		uint64_t lba = start_lba + (uint64_t)j * nlb;
		uint64_t blk = c->data_addr + (uint64_t)j * c->data_step;
		uint64_t t0, t1, t2, t3, lspins, spins = 0;
		uint32_t st;

		t0 = clock64();
		nvme_build_rw_sqe((struct nvme_sqe *)cap, c->nvme_opc,
				  (uint16_t)(GIO_CID_TAG | tag), c->nsid,
				  lba, nlb, blk, c->data_rkey, c->io_bytes);
		__threadfence_system();		/* 胶囊在主存,网卡要读 */
		t1 = clock64();

		/* 必须在门铃之前置 POSTED,否则 poller 置的 DONE 会被覆盖 */
		st_rel32(&tags[tag].job, j);
		st_rel32(&tags[tag].state, GIO_ST_POSTED);

		if (c->opt & 0x100) {
			/* 门铃之后一个带 fence 的写都没有,同旧串行 kernel */
			post_wqe(c, (uint16_t)(c->sq_pi + j), cap_addr);
			lspins = 0;
			t2 = clock64();
		} else {
			lspins = sq_lock(sh);
			post_locked(c, sh, cap_addr);
			sq_unlock(sh);
			t2 = clock64();
			tags[tag].t_post = t2;
			__threadfence_system();
		}

		for (;;) {
			spins++;
			if (c->opt & 0x20) {
				uint32_t idx = my_ci & (c->cq_cnt - 1);
				volatile uint8_t *cqe = c->cq_buf +
					(size_t)idx * c->cqe_size;
				uint8_t oo = TAIL_OP_OWN(cqe_tail(c, cqe));

				if ((oo & 1) != (my_phase & 1) ||
				    (oo >> 4) == MLX5_CQE_INVALID) {
					if ((spins & 0x3ff) == 0 &&
					    clock64() - t2 > GIO_TIMEOUT) {
						set_abort(sh, GIO_E_WAIT);
						st = GIO_ST_POSTED;
						break;
					}
					continue;
				}
				__threadfence_system();
				sh->poll_hits++;
				sh->resp_cycles += consume_one(c, sh, tags, cqe,
							       &my_seq);
				advance_ci(c, &my_ci, &my_phase);
				/* handle_resp 在本 block 里已 fence 过,直接读 */
				st = *(volatile uint32_t *)&tags[tag].state;
				if (st >= GIO_ST_DONE) {
					break;
				}
				if (ld_acq32(&sh->abort)) {
					break;
				}
				continue;
			}
			if (c->opt & 0x10) {
				if (atomicCAS(&sh->cq_lock, 0u, 1u) == 0u) {
					poll_step(c, sh, tags);
					st_rel32(&sh->cq_lock, 0u);
				}
			} else if ((c->opt & 1) &&
				   (spins & ((c->opt & 4) ? 0x3ff : 0xf)) != 0) {
				/* 不 fence,直接读:可能读到旧值,每 16 圈 fence 一次兜底 */
				st = *(volatile uint32_t *)&tags[tag].state;
				if (st >= GIO_ST_DONE) {
					break;
				}
				continue;
			}
			st = ld_acq32(&tags[tag].state);
			if (st >= GIO_ST_DONE) {
				break;
			}
			if (ld_acq32(&sh->abort)) {
				break;
			}
			if (clock64() - t2 > GIO_TIMEOUT) {
				set_abort(sh, GIO_E_WAIT);
				break;
			}
		}
		t3 = clock64();
		if (st >= GIO_ST_DONE) {
			uint64_t td;

			__threadfence_system();
			td = tags[tag].t_done;
			c_det[j] = tags[tag].det;
			c_hand[j] = t3 - td;
		}

		if (c->opt & 0x80) {
			c_tsreq[j] = sh->last_req_ts;
			c_reqat[j] = sh->last_req_at - t2;	/* 门铃→发现 SEND 完成 */
			c_tsresp[j] = tags[tag].ts_resp;
		}

		c_cap[j]  = t1 - t0;	/* 写 64 字节胶囊 */
		c_sub[j]  = t2 - t1;	/* 抢锁 + WQE + dbrec + 门铃 */
		c_wait[j] = t3 - t2;	/* 网络往返 + target + poller 转交 */
		c_spin[j] = spins;
		c_lock[j] = lspins;
		c_stat[j] = (st >= GIO_ST_DONE) ?
			    ld_acq32((uint32_t *)&tags[tag].status) & 0xffff :
			    0xffffffffu;	/* 没等到 */

		st_rel32(&tags[tag].state, GIO_ST_IDLE);
		if (st < GIO_ST_DONE) {
			break;
		}
	}
}

__global__ void
k_gpu_io(struct gpu_io_ctx ctx, struct gio_shared *sh, struct gio_tag *tags,
	 uint64_t start_lba, uint64_t *c_cap, uint64_t *c_sub,
	 uint64_t *c_wait, uint64_t *c_spin, uint64_t *c_lock,
	 uint32_t *c_stat, uint64_t *c_det, uint64_t *c_hand,
	 uint64_t *c_tsreq, uint64_t *c_tsresp, uint64_t *c_reqat)
{
	uint32_t t = blockIdx.x;

	if (ctx.opt & 0x40) {
		t++;		/* 没有 poller block:block i 就是 submitter i */
	}
	if (threadIdx.x != 0 || t > ctx.qd) {
		return;
	}
	if (t == 0) {
		if ((ctx.opt & 0x30) == 0) {
			poller(&ctx, sh, tags);
		}
	} else {
		submitter(&ctx, sh, tags, t - 1, start_lba, c_cap, c_sub,
			  c_wait, c_spin, c_lock, c_stat, c_det, c_hand,
			  c_tsreq, c_tsresp, c_reqat);
	}
}

/* 从 GPU 视角读一段内存,用来核对 host 注册映射是否对得上 */
__global__ void
k_peek(volatile uint8_t *src, uint8_t *dst, uint32_t n)
{
	uint32_t i;

	if (threadIdx.x != 0 || blockIdx.x != 0) {
		return;
	}
	__threadfence_system();
	for (i = 0; i < n; i++) {
		dst[i] = src[i];
	}
}

/* 只敲门铃,用于单独测 MMIO 写的成本 */
__global__ void
k_ring_only(volatile uint64_t *bf_reg, uint64_t val, uint32_t n,
	    uint64_t *cycles)
{
	uint32_t i;

	if (threadIdx.x != 0) {
		return;
	}
	for (i = 0; i < n; i++) {
		uint64_t t0 = clock64();

		WQE_FENCE_DB();
		*bf_reg = val;
		WQE_FENCE_DB();
		cycles[i] = clock64() - t0;
	}
}

/*
 * 由 GPU kernel 自己往显存写 pattern —— 用普通 store,和
 * nvme_build_rw_sqe 写胶囊是同一类动作。
 *
 * 对照用:gpu_memset_dev 走的是 mcMemset,那是 CPU 发起的 DMA 填充,
 * 后面还跟 mcDeviceSynchronize,网卡当然读得到。真正要验的是
 * "kernel 内的 store 对网卡可见吗"。
 */
__global__ void
k_fill(volatile unsigned char *p, unsigned char v, unsigned int len)
{
	unsigned int i = blockIdx.x * blockDim.x + threadIdx.x;
	unsigned int stride = gridDim.x * blockDim.x;

	for (; i < len; i += stride) {
		p[i] = v;
	}
	__threadfence_system();
}

extern "C" {

/*
 * 返回 0 表示 kernel 正常跑完(不代表 I/O 全成功,看 sh_out)。
 * 逐 I/O 数组长度 = ctx->njobs;c_stat 是 NVMe status 字,
 * 0xffffffff 表示没等到完成。
 */
int
gpu_io_run(struct gpu_io_ctx *ctx, unsigned long long start_lba,
	   unsigned long long *host_cap, unsigned long long *host_sub,
	   unsigned long long *host_wait, unsigned long long *host_spin,
	   unsigned long long *host_lock, unsigned int *host_stat,
	   unsigned long long *host_det, unsigned long long *host_hand,
	   unsigned long long *host_tsreq, unsigned long long *host_tsresp,
	   unsigned long long *host_reqat, struct gio_shared *sh_out)
{
	uint64_t *d_cap = NULL, *d_sub = NULL, *d_wait = NULL;
	uint64_t *d_spin = NULL, *d_lock = NULL;
	uint64_t *d_det = NULL, *d_hand = NULL;
	uint64_t *d_tsreq = NULL, *d_tsresp = NULL, *d_reqat = NULL;
	uint32_t *d_stat = NULL;
	struct gio_shared *d_sh = NULL, sh0;
	struct gio_tag *d_tags = NULL;
	size_t sz = ctx->njobs * sizeof(uint64_t);
	size_t tsz = ctx->qd * sizeof(struct gio_tag);
	/* block 0 = poller;opt bit6 时不启动它 */
	unsigned int nblk = (ctx->opt & 0x40) ? ctx->qd : ctx->qd + 1;
	mcError_t e;
	int rc = -1;

	if (mcMalloc((void **)&d_cap, sz) != mcSuccess ||
	    mcMalloc((void **)&d_sub, sz) != mcSuccess ||
	    mcMalloc((void **)&d_wait, sz) != mcSuccess ||
	    mcMalloc((void **)&d_spin, sz) != mcSuccess ||
	    mcMalloc((void **)&d_lock, sz) != mcSuccess ||
	    mcMalloc((void **)&d_det, sz) != mcSuccess ||
	    mcMalloc((void **)&d_hand, sz) != mcSuccess ||
	    mcMalloc((void **)&d_tsreq, sz) != mcSuccess ||
	    mcMalloc((void **)&d_tsresp, sz) != mcSuccess ||
	    mcMalloc((void **)&d_reqat, sz) != mcSuccess ||
	    mcMalloc((void **)&d_stat, ctx->njobs * sizeof(uint32_t)) != mcSuccess ||
	    mcMalloc((void **)&d_sh, sizeof(*d_sh)) != mcSuccess ||
	    mcMalloc((void **)&d_tags, tsz) != mcSuccess) {
		fprintf(stderr, "kernel 工作区分配失败\n");
		goto out;
	}

	memset(&sh0, 0, sizeof(sh0));
	sh0.sq_pi = ctx->sq_pi;
	sh0.sp_ci = ctx->cq_ci;
	sh0.sp_phase = ctx->cq_phase;
	mcMemcpy(d_sh, &sh0, sizeof(sh0), mcMemcpyHostToDevice);
	mcMemset(d_tags, 0, tsz);
	mcMemset(d_stat, 0xff, ctx->njobs * sizeof(uint32_t));
	mcDeviceSynchronize();

	mcMemset(d_det, 0, sz);
	mcMemset(d_hand, 0, sz);
	mcMemset(d_tsreq, 0, sz);
	mcMemset(d_tsresp, 0, sz);
	mcMemset(d_reqat, 0, sz);
	mcDeviceSynchronize();

	k_gpu_io<<<nblk, 32>>>(*ctx, d_sh, d_tags, start_lba, d_cap, d_sub,
			       d_wait, d_spin, d_lock, d_stat, d_det, d_hand,
			       d_tsreq, d_tsresp, d_reqat);

	e = mcGetLastError();
	if (e != mcSuccess) {
		fprintf(stderr, "kernel launch 失败: %s\n", mcGetErrorString(e));
		goto out;
	}
	e = mcDeviceSynchronize();
	if (e != mcSuccess) {
		fprintf(stderr, "kernel 执行失败: %s\n", mcGetErrorString(e));
		goto out;
	}

	mcMemcpy(host_cap,  d_cap,  sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_sub,  d_sub,  sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_wait, d_wait, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_spin, d_spin, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_lock, d_lock, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_det, d_det, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_hand, d_hand, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_tsreq, d_tsreq, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_tsresp, d_tsresp, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_reqat, d_reqat, sz, mcMemcpyDeviceToHost);
	mcMemcpy(host_stat, d_stat, ctx->njobs * sizeof(uint32_t),
		 mcMemcpyDeviceToHost);
	mcMemcpy(sh_out, d_sh, sizeof(*sh_out), mcMemcpyDeviceToHost);
	rc = 0;
out:
	if (d_cap)  mcFree(d_cap);
	if (d_sub)  mcFree(d_sub);
	if (d_wait) mcFree(d_wait);
	if (d_spin) mcFree(d_spin);
	if (d_lock) mcFree(d_lock);
	if (d_det)  mcFree(d_det);
	if (d_hand) mcFree(d_hand);
	if (d_tsreq) mcFree(d_tsreq);
	if (d_tsresp) mcFree(d_tsresp);
	if (d_reqat) mcFree(d_reqat);
	if (d_stat) mcFree(d_stat);
	if (d_sh)   mcFree(d_sh);
	if (d_tags) mcFree(d_tags);
	return rc;
}

int
gpu_ring_bench(void *bf_dev_ptr, unsigned long long val, unsigned int n,
	       unsigned long long *host_cycles)
{
	uint64_t *d_cycles = NULL;
	int rc = -1;

	if (mcMalloc((void **)&d_cycles, n * sizeof(uint64_t)) != mcSuccess) {
		return -1;
	}
	k_ring_only<<<1, 32>>>((volatile uint64_t *)bf_dev_ptr, val, n, d_cycles);
	if (mcGetLastError() == mcSuccess &&
	    mcDeviceSynchronize() == mcSuccess) {
		mcMemcpy(host_cycles, d_cycles, n * sizeof(uint64_t),
			 mcMemcpyDeviceToHost);
		rc = 0;
	}
	mcFree(d_cycles);
	return rc;
}

int
gpu_alloc_exportable_ex(void **ptr, int *fd, size_t len, unsigned int flags)
{
	mcError_t e;

	/*
	 * flags != 0 走 mcExtMallocWithFlags。MACA 的取值见
	 * mcr/mc_runtime_types.h:
	 *   0x1 Finegrained        细粒度区域
	 *   0x3 WriteCoherence     写一致
	 *   0x4 MapPcieDefault     uncache,映射到 PCIe 访问
	 *   0x5 MapPcieCoherence   写一致 + 映射到 PCIe
	 *   0x6 FixedMemDefault    uncache,固定内存区
	 *
	 * 为什么要它:普通 mcMalloc 的显存,GPU kernel 普通 store 写完之后,
	 * 网卡的 DMA read 取到的是写入前的旧值 —— 写停在 L2 里。已实测无效
	 * 的手段:__threadfence_system()、SyncMemops、门铃前空转 1 ms、
	 * 去掉 RELAXED_ORDERING。只有把那段写拆到独立 kernel 再做
	 * mcDeviceSynchronize 才正常,而 MACA 没暴露 kernel 内的 flush
	 * (CanFlushRemoteWrites=0, HdpMemFlushCntl=0)。
	 *
	 * uncached 显存的 GPU 侧带宽会掉不少,所以只给 64 字节的命令胶囊用,
	 * MB 级的 payload 仍然走普通 mcMalloc。
	 */
	e = flags ? mcExtMallocWithFlags(ptr, len, flags)
		  : mcMalloc(ptr, len);
	if (e != mcSuccess) {
		return -1;
	}
	if (mcMemGetHandleForAddressRange(fd, *ptr, len, 1, 0) != mcSuccess) {
		mcFree(*ptr);
		return -1;
	}
	return 0;
}

int
gpu_alloc_exportable(void **ptr, int *fd, size_t len)
{
	return gpu_alloc_exportable_ex(ptr, fd, len, 0);
}

int
gpu_map_host(void *hptr, size_t len, unsigned int flags, void **dptr)
{
	if (mcHostRegister(hptr, len, flags) != mcSuccess) {
		return -1;
	}
	if (mcHostGetDevicePointer(dptr, hptr, 0) != mcSuccess) {
		mcHostUnregister(hptr);
		return -1;
	}
	return 0;
}

int
gpu_peek(void *dev_ptr, unsigned char *out, unsigned int n)
{
	uint8_t *d = NULL;
	int rc = -1;

	if (mcMalloc((void **)&d, n) != mcSuccess) {
		return -1;
	}
	k_peek<<<1, 32>>>((volatile uint8_t *)dev_ptr, d, n);
	if (mcGetLastError() == mcSuccess &&
	    mcDeviceSynchronize() == mcSuccess &&
	    mcMemcpy(out, d, n, mcMemcpyDeviceToHost) == mcSuccess) {
		rc = 0;
	}
	mcFree(d);
	return rc;
}

int
gpu_unmap_host(void *hptr)
{
	return mcHostUnregister(hptr) == mcSuccess ? 0 : -1;
}

int gpu_set_dev(int id)
{
	return mcSetDevice(id) == mcSuccess ? 0 : -1;
}

int gpu_clock_khz(int dev, int *khz)
{
	mcDeviceProp_t prop;

	if (mcGetDeviceProperties(&prop, dev) != mcSuccess) {
		return -1;
	}
	*khz = prop.clockRate;
	return 0;
}

int gpu_copy_to_host(void *dst, const void *src, size_t len)
{
	return mcMemcpy(dst, src, len, mcMemcpyDeviceToHost) == mcSuccess ? 0 : -1;
}

/* 同步拷贝;调用方仍应在交给网卡之前 gpu_sync(),见坑 2 */
int gpu_copy_to_dev(void *dst, const void *src, size_t len)
{
	return mcMemcpy(dst, src, len, mcMemcpyHostToDevice) == mcSuccess ? 0 : -1;
}

int gpu_memset_dev(void *p, int v, size_t len)
{
	return mcMemset(p, v, len) == mcSuccess ? 0 : -1;
}

/*
 * mcMemset 对 host 是异步的 —— 排进 stream 就返回。紧接着让网卡去读
 * 这块显存,可能读到 memset 还没写完的旧内容。测试里表现为同一条命令
 * 时而通过时而失败(赢/输竞态),非常容易误判成硬件问题。
 * 凡是 memset 之后要交给网卡的地方,都得先同步。
 */
/* kernel 侧填充。不做 mcDeviceSynchronize 之外的任何 flush —— 就是要看
 * kernel 的普通 store 能不能被网卡看到。 */
int gpu_fill_kernel(void *p, int v, size_t len)
{
	k_fill<<<64, 256>>>((volatile unsigned char *)p,
			    (unsigned char)v, (unsigned int)len);
	return mcGetLastError() == mcSuccess ? 0 : -1;
}

int gpu_sync(void)
{
	return mcDeviceSynchronize() == mcSuccess ? 0 : -1;
}

}
