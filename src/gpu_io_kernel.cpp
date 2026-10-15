/*
 * gpu_io_kernel.cpp  --  kernel 内完成 NVMe-oF 读写的全流程
 *
 * 稳态下 CPU 完全不参与。一次 launch,每个 QP 占 QD + 1 个 block(多 QP
 * 时按 QP 依次排开),每个 block 只有 thread 0 干活,角色由 QP 内序号决定:
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
 * 3. C2:poller 每收一个响应就把那块 recv buffer 重新 post(rq_repost),
 *    总 I/O 数不再受 RQ 深度限制;-O 2048 关掉
 * 4. clock64() 是每线程各自的计数,跨线程的时间戳不能相减 ——
 *    等待时间只在 submitter 自己的线程里量
 */

#include <mcr/mc_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gpu_io_ctx.h"

#define USE_MACA_DEVICE 1
#include "wqe_build.h"
#include "blk_pattern.h"

#define GIO_TIMEOUT	2000000000ULL	/* ~1.25 s @ 1.6 GHz */

/*
 * ---- fence 级别(-O 0x4000000) ----
 *
 * 默认全用 __threadfence_system()。共享状态(gio_shared / tag)在显存,
 * 跨 block 可见只需要 device 级;-X 时队列也在显存,网卡读写显存同样只要
 * device 级(mxshmem 的 IBGDA_MEMBAR、tst/devx_probe 已验证)。胶囊在
 * finegrained 显存(-H)时同理。写 MMIO 门铃之后的那次 fence 始终是 system。
 *
 * 级别放在 block 的 shared memory 里,k_gpu_io 开头由本 block 的线程 0 写。
 * 最早放在 __device__ 全局变量里,每次 fence 都多一次显存读(~0.3–0.5 µs),
 * 一条 I/O 十来次 fence,实测提交 +1.3 µs、总计 +1.8 µs —— 连不开 0x4000000
 * 的默认路径也一起变慢。
 */
static __shared__ uint32_t gio_f_sh, gio_f_q, gio_f_cap;

__device__ static __forceinline__ void
fence_sh(void)
{
	if (gio_f_sh) {
		__threadfence();
	} else {
		__threadfence_system();
	}
}

__device__ static __forceinline__ void
fence_q(void)
{
	if (gio_f_q) {
		__threadfence();
	} else {
		__threadfence_system();
	}
}

__device__ static __forceinline__ void
fence_cap(void)
{
	if (gio_f_cap) {
		__threadfence();
	} else {
		__threadfence_system();
	}
}

/* ---- 线程间可见的读写(同 Mooncake maca_ops.cuh 的 ld_acquire/st_release) ---- */

__device__ static inline uint32_t
ld_acq32(uint32_t *p)
{
	fence_sh();
	return *(volatile uint32_t *)p;
}

__device__ static inline void
st_rel32(uint32_t *p, uint32_t v)
{
	*(volatile uint32_t *)p = v;
	fence_sh();
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

/*
 * 消费者(poller / 持 cq_lock 者 / 串行快路径)独占的状态和计数器。
 *
 * 剖析(-O 0x20000)显示,显存上每次 volatile 读写约 0.5 us(全局内存
 * 延迟),poller 每处理一条响应要碰十几次 gio_shared,空转一圈也有两次
 * —— 处理一条响应 7 us、空转一圈 3 us 大半花在这里。现在消费者把这些
 * 放在寄存器里(ps 是消费者线程的局部变量,热路径函数全部内联),
 * 只在进出时和 gio_shared 交换:独立 poller 和串行快路径在开始 / 结束时,
 * 自轮询每一步持锁进出时。跨 block 真正需要的只有 tag 状态和 abort。
 */
struct pstate {
	uint32_t	done_cnt, nvme_err, cid_bad, wc_mismatch;
	uint32_t	cqe_req, cqe_resp, resp_inline, err_cqe;
	uint16_t	first_status, first_bad_cid;
	uint32_t	rq_head, rq_reposted, rq_pending, rq_flushes;
	uint32_t	cq_ci_written, cq_db_writes;
	uint32_t	aborted;	/* 本消费者置过 abort(不回写共享区) */
};

#define PS_FIELDS(X) X(done_cnt) X(nvme_err) X(cid_bad) X(wc_mismatch) \
	X(cqe_req) X(cqe_resp) X(resp_inline) X(err_cqe) X(first_status) \
	X(first_bad_cid) X(rq_head) X(rq_reposted) X(rq_pending) X(rq_flushes) \
	X(cq_ci_written) X(cq_db_writes)

__device__ static __forceinline__ void
ps_load(struct pstate *ps, struct gio_shared *sh)
{
#define PS_LD(f) ps->f = *(volatile __typeof__(sh->f) *)&sh->f;
	PS_FIELDS(PS_LD)
#undef PS_LD
}

__device__ static __forceinline__ void
ps_store(struct gio_shared *sh, const struct pstate *ps)
{
#define PS_ST(f) *(volatile __typeof__(sh->f) *)&sh->f = ps->f;
	PS_FIELDS(PS_ST)
#undef PS_ST
}

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
 *   [0..3]   DW0,命令相关 —— KV Retrieve 是 value 长度(只在 ctx.kv 时读)
 *   [12..13] cid
 *   [14..15] status:bit0 = phase tag,bit1..8 = SC,bit9..11 = SCT
 */
__device__ static __forceinline__ void
handle_resp(struct gpu_io_ctx *c, struct gio_shared *sh, struct pstate *ps,
	    struct gio_tag *tags,
	    volatile uint8_t *cqe64, uint8_t op_own, uint16_t wqe_counter,
	    uint32_t seq)
{
	volatile uint8_t *ncqe;	/* NVMe CQE 起点,在 CQE 内或 recv buffer 里 */
	uint64_t w;		/* NVMe CQE 的第 8..15 字节:sqhd sqid cid status */
	uint32_t dw0 = 0;
	uint16_t cid, status;
	uint32_t tag;

	/*
	 * 计数器只有 poller 写、host 在同步后读,不用 fence。
	 * 跨 block 可见的只有 tag 状态,最后统一 fence 一次。
	 */
	if (((c->rq_tail + seq) & 0xffff) != wqe_counter) {
		ps->wc_mismatch++;
	}

	/*
	 * 主存上每次 volatile 读都是一次 PCIe 往返(~0.7 us),逐字节读 6 次
	 * 就是 4 us。整字读:CQE、recv buffer、RQ 槽都按 8 字节对齐。
	 */
	if (op_own & 0x4) {
		ncqe = cqe64;
		ps->resp_inline++;
	} else if (op_own & 0x8) {
		ncqe = cqe64 - 64;
		ps->resp_inline++;
	} else {
		uint32_t k = wqe_counter & (c->rq_wqe_cnt - 1);
		volatile uint8_t *seg = c->rq_buf + (size_t)k * c->rq_stride;
		/* mlx5_wqe_data_seg:byte_count(4) lkey(4) addr(8),大端 */
		uint64_t addr = wqe_hto_be64(*(volatile uint64_t *)(seg + 8));

		if (c->rsp_len == 0 || addr < c->rsp_host_base ||
		    addr + 16 > c->rsp_host_base + c->rsp_len) {
			ps->aborted = 1;
			set_abort(sh, GIO_E_RSP_ADDR);
			return;
		}
		ncqe = c->rsp_dev + (addr - c->rsp_host_base);
	}
	w = *(volatile uint64_t *)(ncqe + 8);
	/*
	 * DW0 多一次主存读,实测每条响应 +0.65~1.1 us(KV vs NVM 对照)。只有
	 * Retrieve 的 DW0 有内容(value 长度),Store/Exist/Delete 不读。
	 * 没有并成一次 16 字节读:recv buffer 按 40 字节间隔排,不保证 16 对齐。
	 */
	if (c->kv && c->nvme_opc == NVME_KV_OPC_RETRIEVE) {
		dw0 = *(volatile uint32_t *)ncqe;
	}
	/* NVMe CQE 小端:[8]sqhd [10]sqid [12]cid [14]status */
	cid = (uint16_t)(w >> 32);
	status = (uint16_t)(w >> 48);

	if (c->opt & 0x400) {
		tag = 0;	/* -O 1024:旧式 cid,只在 QD=1 下用 */
	} else {
		tag = cid & ~GIO_CID_TAG;
	}
	if ((c->opt & 0x400) ? ((cid & 0xf000) != 0xE000) :
	    ((cid & GIO_CID_TAG) == 0 || tag >= c->qd)) {
		if (ps->cid_bad == 0) {
			ps->first_bad_cid = cid;
		}
		ps->cid_bad++;
		ps->aborted = 1;
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
		vt->dw0 = dw0;
		vt->status = status;
		if ((status >> 1) != 0) {
			if (ps->nvme_err == 0) {
				ps->first_status = status;
			}
			ps->nvme_err++;
			vt->state = GIO_ST_NVME_ERR;
		} else {
			vt->state = GIO_ST_DONE;
		}
		vt->t_done = clock64();
		fence_sh();
	}
	ps->done_cnt++;
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

__device__ static __forceinline__ void rq_repost(struct gpu_io_ctx *c,
						 struct pstate *ps,
						 uint16_t wqe_counter);

/* 把已写好的 RQ 槽一次性交给网卡:槽内容先落地,再推 dbrec[0] */
__device__ static __forceinline__ void
rq_flush(struct gpu_io_ctx *c, struct pstate *ps)
{
	uint32_t head;

	if (ps->rq_pending == 0) {
		return;
	}
	head = ps->rq_head;
	fence_q();
	c->qp_dbrec[0] = wqe_hto_be32(head & 0xffff);
	fence_q();
	ps->rq_pending = 0;
	ps->rq_flushes += 1;
}

/* opt 0x2000:把 CQ dbrec 补到当前 ci(空闲 / 退出时调) */
__device__ static __forceinline__ void
cq_flush(struct gpu_io_ctx *c, struct pstate *ps, uint32_t ci)
{
	if (!(c->opt & 0x2000) ||
	    ps->cq_ci_written == ci) {
		return;
	}
	c->cq_dbrec[0] = wqe_hto_be32(ci & 0xffffff);
	fence_q();
	ps->cq_ci_written = ci;
	ps->cq_db_writes += 1;
}

/* 消费者空闲时调:把攒着的 RQ 补充和 CQ ci 都交出去 */
__device__ static __forceinline__ void
idle_flush(struct gpu_io_ctx *c, struct pstate *ps, uint32_t ci)
{
	rq_flush(c, ps);
	cq_flush(c, ps, ci);
}

__device__ static __forceinline__ uint64_t
consume_one(struct gpu_io_ctx *c, struct gio_shared *sh, struct pstate *ps,
	    struct gio_tag *tags,
	    volatile uint8_t *cqe, uint64_t tail, uint32_t *seq)
{
	/*
	 * tail 是调用方判 owner 时读到的那个 8 字节:对齐的 8 字节读是一次
	 * 完整的 PCIe 读,不会读到半新半旧,不必再读一次(主存读 ~1 us)。
	 * CQE 其余字段要在调用方 fence 之后再读。
	 */
	uint8_t op_own = TAIL_OP_OWN(tail);
	uint8_t opcode = op_own >> 4;
	uint64_t r = 0;

	if (opcode == MLX5_CQE_RESP_SEND) {
		uint16_t wc = TAIL_WQE_CTR(tail);
		uint64_t r0 = clock64();

		ps->cqe_resp++;
		handle_resp(c, sh, ps, tags, cqe + (c->cqe_size - 64), op_own,
			    wc, *seq);
		if (!(c->opt & 0x800) && c->rq_shadow) {
			rq_repost(c, ps, wc);
		}
		r = clock64() - r0;
		(*seq)++;
	} else if (opcode == MLX5_CQE_REQ) {
		ps->cqe_req++;
		if (c->opt & 0x80) {
			sh->last_req_ts = cqe_ts(c, cqe);
			sh->last_req_at = clock64();
		}
	} else if (opcode == MLX5_CQE_REQ_ERR ||
		   opcode == MLX5_CQE_RESP_ERR) {
		/* cqe64 在条目末尾;syndrome 在其 offset 55,vendor_err 在 54 */
		uint32_t base = c->cqe_size - 64;

		ps->err_cqe = ((uint32_t)cqe[base + 54] << 8) |
			      cqe[base + 55];
		ps->aborted = 1;
		set_abort(sh, GIO_E_CQE);
	}
	return r;
}

/*
 * C2:把刚消费的 recv 槽 k 原样重新 post 到 RQ 头。
 *
 * RC 的 recv 按 FIFO 消费,网卡消费后不清槽内容;把同一个描述符写到
 * head 处,等于把那块 buffer 还回去。和 rdma-core mlx5_post_recv 一样,
 * RQ 只推 dbrec[0](大端,低 16 位),不敲门铃。
 *
 * 描述符从显存影子取,不回读主存;影子跟着搬,保证下一圈取到的还是
 * 这块 buffer。调用方保证此刻只有自己在消费 CQ(poller / 持 cq_lock /
 * QD=1 串行快路径),所以 ps->rq_head 不用原子操作。
 *
 * 必须在 handle_resp 读完响应之后调:非内联响应的数据在这块 buffer 里。
 */
__device__ static __forceinline__ void
rq_repost(struct gpu_io_ctx *c, struct pstate *ps, uint16_t wqe_counter)
{
	uint32_t mask = c->rq_wqe_cnt - 1;
	uint32_t k = wqe_counter & mask;
	uint32_t head = ps->rq_head;
	uint32_t h = head & mask;
	uint32_t words = c->rq_stride / 8, i;
	volatile uint64_t *dst = (volatile uint64_t *)(c->rq_buf +
						       (size_t)h * c->rq_stride);
	volatile uint64_t *src = c->rq_shadow + (size_t)k * words;
	volatile uint64_t *shd = c->rq_shadow + (size_t)h * words;

	for (i = 0; i < words; i++) {
		uint64_t v = src[i];

		dst[i] = v;
		shd[i] = v;
	}
	head++;
	ps->rq_head = head;
	ps->rq_reposted += 1;
	ps->rq_pending += 1;

	/*
	 * 默认每个都推 dbrec;opt 0x1000 攒够一批再推。两次 system fence
	 * 实测每条响应 ~3.5 us,是 poller 的大头。批量期间在飞的 recv 最少
	 * 少 GIO_RQ_BATCH 个,host 侧相应压低 QD 上限。
	 */
	if (!(c->opt & 0x1000) ||
	    ps->rq_pending >= GIO_RQ_BATCH) {
		rq_flush(c, ps);
	}
}

/* 消费一个 CQE 后推进 ci,并立刻还给网卡(出错也要还,否则 CQ 会溢出) */
__device__ static __forceinline__ void
advance_ci(struct gpu_io_ctx *c, struct pstate *ps, uint32_t *ci,
	   uint8_t *phase)
{
	(*ci)++;
	if ((*ci & (c->cq_cnt - 1)) == 0) {
		*phase ^= 1;
	}
	if (c->opt & 0x2000) {
		/*
		 * 懒更新:网卡只用 CQ dbrec 判断溢出。每 GIO_CQ_BATCH 个写一次、
		 * 不 fence(posted 写迟早可见);空闲 / 退出时 cq_flush 补齐。
		 * 溢出余量由 host 压 QD 保证:2*QD + GIO_CQ_BATCH < cq_cnt。
		 */
		if ((*ci & (GIO_CQ_BATCH - 1)) == 0) {
			c->cq_dbrec[0] = wqe_hto_be32(*ci & 0xffffff);
			ps->cq_ci_written = *ci;
			ps->cq_db_writes += 1;
		}
		return;
	}
	c->cq_dbrec[0] = wqe_hto_be32(*ci & 0xffffff);
	fence_q();
	ps->cq_db_writes += 1;
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
	/* 空转时每 chk 圈才查一次全局 abort(fence + 显存读 ~0.6 us) */
	uint32_t chk = (c->opt & 2) ? 0xff : 0x3f;
	struct pstate ps;
	/* -O 0x20000 剖析:本地累计,退出时写进 sh->prof */
	int prof = (c->opt & GIO_OPT_PROF) != 0;
	uint64_t pf[16] = { 0 }, p0 = 0, p1;

	ps_load(&ps, sh);
	ps.aborted = 0;

	for (;;) {
		uint32_t idx = ci & (c->cq_cnt - 1);
		volatile uint8_t *cqe = c->cq_buf + (size_t)idx * c->cqe_size;
		uint64_t tail;
		uint8_t op_own, opcode;

		if (prof) {
			p0 = clock64();
		}
		tail = cqe_tail(c, cqe);
		op_own = TAIL_OP_OWN(tail);
		opcode = op_own >> 4;
		if (prof) {
			p1 = clock64();
			pf[GIO_PF_IREAD] += p1 - p0;
			p0 = p1;
		}

		spins++;

		/*
		 * owner 和 opcode 两个条件缺一不可。空条目 op_own=0xf0,owner 位
		 * 恰好等于首圈的 phase(0) —— 只判 owner 会把空条目当 CQE 消费掉。
		 */
		if ((op_own & 1) != (phase & 1) || opcode == MLX5_CQE_INVALID) {
			idle_flush(c, &ps, ci);
			if (prof) {
				p1 = clock64();
				pf[GIO_PF_IFLUSH] += p1 - p0;
				p0 = p1;
				pf[GIO_PF_NIDLE]++;
			}
			/* done_cnt 在寄存器里,每圈查也不花钱 */
			if (ps.done_cnt >= c->njobs) {
				break;
			}
			if ((spins & chk) == 0) {
				if (ld_acq32(&sh->abort)) {
					break;
				}
				if (clock64() - idle_since > GIO_TIMEOUT) {
					set_abort(sh, GIO_E_TIMEOUT);
					break;
				}
			}
			if (prof) {
				pf[GIO_PF_ICHK] += clock64() - p0;
			}
			continue;
		}

		/* owner 确认之后再读 CQE 其余字段 */
		fence_q();
		hits++;
		idle_since = clock64();
		if (prof) {
			pf[GIO_PF_HFENCE] += idle_since - p0;
			p0 = idle_since;
			pf[GIO_PF_NHIT]++;
		}

		rcyc += consume_one(c, sh, &ps, tags, cqe, tail, &seq);
		if (prof) {
			p1 = clock64();
			pf[GIO_PF_HCONS] += p1 - p0;
			p0 = p1;
		}
		advance_ci(c, &ps, &ci, &phase);
		if (prof) {
			pf[GIO_PF_HCI] += clock64() - p0;
		}

		/* 自己置的 abort 立刻退;别人置的(submitter 超时)按圈数查 */
		if (ps.aborted || ((hits & chk) == 0 && ld_acq32(&sh->abort))) {
			break;
		}
	}
	idle_flush(c, &ps, ci);
	ps_store(sh, &ps);
	sh->poll_spins = spins;
	sh->poll_hits = hits;
	sh->poll_cycles = clock64() - pstart;
	sh->resp_cycles = rcyc;
	if (prof) {
		int k;

		/* 提交段(0..7)由 submitter atomicAdd,poller 只写自己的 8..15 */
		for (k = GIO_PF_IREAD; k < 16; k++) {
			sh->prof[k] = pf[k];
		}
	}
}

/*
 * 自轮询(opt bit4):调用方已持 cq_lock。把已经到了的 CQE 最多收 32 个,
 * 别人的完成也按 cid 分发。ci/phase/seq 和消费者状态都在共享区里,换人
 * 持锁时靠进出各一次 fence 交接。不在这里 fence 收尾 —— 调用方释放锁时
 * 统一 fence。
 */
__device__ static void
poll_step(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags)
{
	uint32_t ci, seq, n;
	uint8_t phase;
	uint64_t rc = 0;
	struct pstate ps;

	fence_sh();
	ci = sh->sp_ci;
	phase = (uint8_t)sh->sp_phase;
	seq = sh->sp_seq;
	ps_load(&ps, sh);
	ps.aborted = 0;
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
		fence_q();
		sh->poll_hits++;
		rc += consume_one(c, sh, &ps, tags, cqe, tail, &seq);
		advance_ci(c, &ps, &ci, &phase);
	}
	/* 每步结束都交出去:下一个持锁者未必马上来 */
	idle_flush(c, &ps, ci);
	ps_store(sh, &ps);
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
/*
 * 剖析:pf 非空时把 [now - *t] 累计进 pf[i],并把 *t 推到 now。
 * pf 是 submitter 线程本地的数组,退出时才一次性加进 sh->prof,
 * 免得原子操作本身混进被测的段里。
 */
__device__ static __forceinline__ void
pf_mark(uint64_t *pf, int i, uint64_t *t)
{
	if (pf) {
		uint64_t now = clock64();

		pf[i] += now - *t;
		*t = now;
	}
}

__device__ static __forceinline__ void
post_wqe(struct gpu_io_ctx *c, uint16_t pi, uint64_t cap_addr,
	 uint64_t *pf, uint64_t *pt)
{
	uint32_t slot = pi & (c->sq_wqe_cnt - 1);
	volatile uint8_t *sq_slot = c->sq_buf + (size_t)slot * c->sq_stride;
	volatile uint64_t *bfp = c->bf_reg;
	const uint64_t *src;
	int w;

	/* opt 0x4000:只有每 GIO_SIG_EVERY 条要 CQE(fm_ce_se 在第 2 个字的最高字节) */
	int unsig = (c->opt & 0x4000) && ((pi & (GIO_SIG_EVERY - 1)) !=
					  (GIO_SIG_EVERY - 1));

	if (c->opt & (0x8000 | GIO_OPT_BF1)) {
		/*
		 * 在寄存器里构造,分别写 SQ 槽和 BlueFlame,不再从主存回读 4 次
		 * (剖析:回读 + BF + fence 约 6 us,是提交路径的大头)。按 32 位
		 * 构造、按 32 位拼 64 位,避免坑 wqe_build.h 里那个严格别名问题。
		 *
		 * 0x8000 一次都不读,实测提交快 ~1 us、等待却慢 ~2.7 us:回读不只是
		 * 取数据,还把之前写主存的 WQE/dbrec 推了出去(PCIe 上读不能越过
		 * 之前的 posted 写)。GIO_OPT_BF1 保留这个作用,只读 1 次 8 字节。
		 */
		/*
		 * 纯寄存器构造(不取地址,不落 local memory):和
		 * mlx5_build_send_wqe 逐字一致,按小端拼成 4 个 64 位字。
		 * 之前经 w32[8] 数组构造,剖析里"写 WQE"要 2.3~4.3 us,
		 * 多半是数组落在 local memory(显存)上的读写。
		 */
		uint64_t w64[4];

		w64[0] = (uint64_t)wqe_hto_be32(((uint32_t)pi << 8) |
						MLX5_OPCODE_SEND) |
			 ((uint64_t)wqe_hto_be32((c->qpn << 8) | 2) << 32);
		w64[1] = unsig ? 0 :
			 (uint64_t)(((uint32_t)MLX5_WQE_CTRL_CQ_UPDATE) << 24);
		w64[2] = (uint64_t)wqe_hto_be32(64) |
			 ((uint64_t)wqe_hto_be32(c->capsule_lkey) << 32);
		w64[3] = (uint64_t)wqe_hto_be32((uint32_t)(cap_addr >> 32)) |
			 ((uint64_t)wqe_hto_be32((uint32_t)cap_addr) << 32);
		{
			volatile uint64_t *d64 = (volatile uint64_t *)sq_slot;

			d64[0] = w64[0];
			d64[1] = w64[1];
			d64[2] = w64[2];
			d64[3] = w64[3];
		}
		fence_q();
		pf_mark(pf, GIO_PF_WQE, pt);
		c->qp_dbrec[1] = wqe_hto_be32((uint32_t)(pi + 1) & 0xffff);
		fence_q();
		pf_mark(pf, GIO_PF_DBR, pt);
		if (c->opt & GIO_OPT_BF1) {
			/* 一次主存读,把前面的 WQE / dbrec 写推到主存 */
			(void)*(volatile uint64_t *)sq_slot;
		}
		if (c->opt & GIO_OPT_BFDLY) {
			uint64_t t0 = clock64();

			while (clock64() - t0 < 4096) {
			}
		}
		if ((c->opt & GIO_OPT_BFTOG) && (pi & 1) && c->bf_alt) {
			bfp = c->bf_alt;
		}
		if (c->opt & GIO_OPT_DB8) {
			/* 普通门铃:只写控制段前 8 字节,网卡从 SQ 取 WQE */
			bfp[0] = w64[0];
		} else if (c->opt & (GIO_OPT_BFSEP | GIO_OPT_BFGAP)) {
			/*
			 * 默认路径里 4 个 BF 写被回读隔开(各 ~1.5 us)时网卡最快。
			 * BFSEP:每写一个 8 字节就 system fence;BFGAP:每个之间
			 * 空等 ~1000 时钟(~0.9 us)。看"隔开"本身能不能代替回读。
			 */
			for (w = 0; w < 4; w++) {
				bfp[w] = w64[w];
				if (c->opt & GIO_OPT_BFSEP) {
					__threadfence_system();
				}
				if ((c->opt & GIO_OPT_BFGAP) && w < 3) {
					uint64_t t0 = clock64();

					while (clock64() - t0 < 1000) {
					}
				}
			}
		} else {
			bfp[0] = w64[0];
			bfp[1] = w64[1];
			bfp[2] = w64[2];
			bfp[3] = w64[3];
		}
		if (!(c->opt & GIO_OPT_DB8) && (c->opt & GIO_OPT_BF64)) {
			for (w = 4; w < 8; w++) {
				bfp[w] = 0;
			}
		}
		WQE_FENCE_DB();
		pf_mark(pf, GIO_PF_BF, pt);
		return;
	}

	(void)mlx5_build_send_wqe((void *)sq_slot, pi, c->qpn, cap_addr,
				  c->capsule_lkey, 64);
	if (unsig) {
		((volatile uint32_t *)sq_slot)[2] = 0;
	}
	WQE_FENCE_QUEUE();
	pf_mark(pf, GIO_PF_WQE, pt);

	c->qp_dbrec[1] = wqe_hto_be32((uint32_t)(pi + 1) & 0xffff);
	WQE_FENCE_DB();
	pf_mark(pf, GIO_PF_DBR, pt);

	src = (const uint64_t *)(void *)sq_slot;
	if (c->opt & GIO_OPT_RBPAR) {
		/*
		 * 4 次回读先全部发出、再写门铃。原写法每个读后面紧跟一个
		 * volatile 写,编译器不能把后面的读提到写之前(可能别名),
		 * 4 次主存读(~1 us 一次)只能串行。
		 */
		uint64_t v0 = ((volatile const uint64_t *)src)[0];
		uint64_t v1 = ((volatile const uint64_t *)src)[1];
		uint64_t v2 = ((volatile const uint64_t *)src)[2];
		uint64_t v3 = ((volatile const uint64_t *)src)[3];

		bfp[0] = v0;
		bfp[1] = v1;
		bfp[2] = v2;
		bfp[3] = v3;
	} else {
		for (w = 0; w < 4; w++) {
			bfp[w] = src[w];
		}
	}
	WQE_FENCE_DB();
	pf_mark(pf, GIO_PF_BF, pt);

}

__device__ static __forceinline__ void
post_locked(struct gpu_io_ctx *c, struct gio_shared *sh, uint64_t cap_addr,
	    uint64_t *pf, uint64_t *pt)
{
	uint16_t pi = (uint16_t)ld_acq32(&sh->sq_pi);

	pf_mark(pf, GIO_PF_PI, pt);
	post_wqe(c, pi, cap_addr, pf, pt);
	st_rel32(&sh->sq_pi, (uint32_t)pi + 1);
}

__device__ static void
submitter(struct gpu_io_ctx *c, struct gio_shared *sh, struct gio_tag *tags,
	  uint32_t tag, uint64_t start_lba, uint64_t *c_cap, uint64_t *c_sub,
	  uint64_t *c_wait, uint64_t *c_spin, uint64_t *c_lock,
	  uint32_t *c_stat, uint64_t *c_det, uint64_t *c_hand,
	  uint64_t *c_tsreq, uint64_t *c_tsresp, uint64_t *c_reqat,
	  uint32_t *c_dw0)
{
	uint32_t nlb = c->io_bytes / c->sector_size;
	uint32_t j;
	/* 串行快路径(opt bit5)的本地 CQ 状态 */
	uint32_t my_ci = c->cq_ci, my_seq = 0;
	uint8_t my_phase = c->cq_phase;
	struct pstate sps;		/* 串行快路径的消费者状态,退出时写回 */
	/* -O 0x20000 剖析:提交路径分段先本地累计,退出时加进 sh->prof */
	uint64_t lpf[GIO_PF_POST + 1] = { 0 };
	uint64_t *pf = (c->opt & GIO_OPT_PROF) ? lpf : NULL;
	uint64_t pt = 0;

	if (c->opt & 0x20) {
		ps_load(&sps, sh);
		sps.aborted = 0;
	}
	while (claim_job(sh, c->njobs, &j)) {
		/* 多 QP:j 是本 QP 内的序号,J 是全局序号(LBA/key/块/统计数组) */
		uint32_t J = c->job_base + j;
		/* -O 512:槽位按 job 轮换,同旧 kernel;默认固定用 tag 号 */
		uint32_t cslot = (c->opt & 0x200) ? (j & 0x3f) : tag;
		volatile uint8_t *cap = c->capsule + (size_t)cslot * 64;
		uint64_t cap_addr = c->capsule_addr + (uint64_t)cslot * 64;
		/* -O 1024:cid 用旧 kernel 的取值 */
		uint16_t cid = (c->opt & 0x400) ?
			       (uint16_t)(0xE000 | (j & 0xfff)) :
			       (uint16_t)(GIO_CID_TAG | tag);
		uint64_t lba = start_lba + (uint64_t)J * nlb;
		uint64_t blk = c->data_addr + (uint64_t)J * c->data_step;
		uint64_t t0, t1, t2, t3, lspins, spins = 0;
		uint32_t st;

		t0 = clock64();
		if (c->kv) {
			/* KV:第 J 条的 key 由 (seed, start + J) 决定,和 host 侧一致 */
			uint8_t key[NVME_KV_MAX_KEY];

			blk_kv_key(key, c->kv_seed, start_lba + J);
			nvme_build_kv_sqe((struct nvme_sqe *)cap, c->nvme_opc, cid,
					  c->nsid, key, c->kv_klen, c->kv_opts,
					  blk, c->data_rkey, c->io_bytes);
		} else {
			nvme_build_rw_sqe((struct nvme_sqe *)cap, c->nvme_opc, cid,
					  c->nsid,
					  lba, nlb, blk, c->data_rkey, c->io_bytes);
		}
		fence_cap();		/* 网卡要读胶囊(主存:system;-H 显存:可 device) */
		t1 = clock64();

		/* 必须在门铃之前置 POSTED,否则 poller 置的 DONE 会被覆盖 */
		st_rel32(&tags[tag].job, j);
		st_rel32(&tags[tag].state, GIO_ST_POSTED);
		pt = t1;
		pf_mark(pf, GIO_PF_TAG, &pt);

		if (c->opt & 0x100) {
			/* 门铃之后一个带 fence 的写都没有,同旧串行 kernel */
			post_wqe(c, (uint16_t)(c->sq_pi + j), cap_addr, pf, &pt);
			lspins = 0;
			t2 = clock64();
		} else {
			lspins = sq_lock(sh);
			pf_mark(pf, GIO_PF_LOCK, &pt);
			post_locked(c, sh, cap_addr, pf, &pt);
			sq_unlock(sh);
			pf_mark(pf, GIO_PF_REL, &pt);
			t2 = clock64();
			tags[tag].t_post = t2;
			fence_sh();
			pf_mark(pf, GIO_PF_POST, &pt);
		}

		for (;;) {
			spins++;
			if (c->opt & 0x20) {
				uint32_t idx = my_ci & (c->cq_cnt - 1);
				volatile uint8_t *cqe = c->cq_buf +
					(size_t)idx * c->cqe_size;
				uint64_t tl = cqe_tail(c, cqe);
				uint8_t oo = TAIL_OP_OWN(tl);

				if ((oo & 1) != (my_phase & 1) ||
				    (oo >> 4) == MLX5_CQE_INVALID) {
					idle_flush(c, &sps, my_ci);
					if ((spins & 0x3ff) == 0 &&
					    clock64() - t2 > GIO_TIMEOUT) {
						set_abort(sh, GIO_E_WAIT);
						st = GIO_ST_POSTED;
						break;
					}
					continue;
				}
				fence_q();
				sh->poll_hits++;
				sh->resp_cycles += consume_one(c, sh, &sps, tags,
							       cqe, tl, &my_seq);
				advance_ci(c, &sps, &my_ci, &my_phase);
				/* handle_resp 在本 block 里已 fence 过,直接读 */
				st = *(volatile uint32_t *)&tags[tag].state;
				if (st >= GIO_ST_DONE) {
					break;
				}
				if (sps.aborted || ld_acq32(&sh->abort)) {
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

			fence_sh();
			td = tags[tag].t_done;
			c_det[J] = tags[tag].det;
			c_hand[J] = t3 - td;
			c_dw0[J] = tags[tag].dw0;
		}

		if (c->opt & 0x80) {
			c_tsreq[J] = sh->last_req_ts;
			c_reqat[J] = sh->last_req_at - t2;	/* 门铃→发现 SEND 完成 */
			c_tsresp[J] = tags[tag].ts_resp;
		}

		c_cap[J]  = t1 - t0;	/* 写 64 字节胶囊 */
		c_sub[J]  = t2 - t1;	/* 抢锁 + WQE + dbrec + 门铃 */
		c_wait[J] = t3 - t2;	/* 网络往返 + target + poller 转交 */
		c_spin[J] = spins;
		c_lock[J] = lspins;
		c_stat[J] = (st >= GIO_ST_DONE) ?
			    ld_acq32((uint32_t *)&tags[tag].status) & 0xffff :
			    0xffffffffu;	/* 没等到 */

		st_rel32(&tags[tag].state, GIO_ST_IDLE);
		if (st < GIO_ST_DONE) {
			break;
		}
	}
	/* 串行快路径自己消费 CQ:退出前把攒着的 RQ / CQ dbrec 交出去 */
	if (c->opt & 0x20) {
		idle_flush(c, &sps, my_ci);
		ps_store(sh, &sps);
	}
	if (pf) {
		int k;

		for (k = 0; k <= GIO_PF_POST; k++) {
			atomicAdd((unsigned long long *)&sh->prof[k],
				  (unsigned long long)lpf[k]);
		}
	}
}

/*
 * 多 QP:block b 属于 QP b / per,QP 内序号 b % per(per = 每 QP 的 block 数)。
 * 每个 QP 的 ctx / 共享区 / tag 各自独立,QP 之间不共享任何状态。
 * ctx 从显存数组拷到本线程,之后和单 QP 时按值传参的用法一样。
 */
__global__ void
k_gpu_io(const struct gpu_io_ctx *ctxs, struct gio_shared *shs,
	 struct gio_tag *tags_all, uint32_t per,
	 uint64_t start_lba, uint64_t *c_cap, uint64_t *c_sub,
	 uint64_t *c_wait, uint64_t *c_spin, uint64_t *c_lock,
	 uint32_t *c_stat, uint64_t *c_det, uint64_t *c_hand,
	 uint64_t *c_tsreq, uint64_t *c_tsresp, uint64_t *c_reqat,
	 uint32_t *c_dw0)
{
	uint32_t q = blockIdx.x / per;
	uint32_t t = blockIdx.x % per;
	struct gpu_io_ctx ctx;
	struct gio_shared *sh;
	struct gio_tag *tags;

	if (threadIdx.x != 0) {
		return;
	}
	ctx = ctxs[q];
	sh = shs + q;
	tags = tags_all + (size_t)q * ctx.qd;
	{
		uint32_t dev = (ctx.opt & GIO_OPT_DEVFENCE) ? 1u : 0u;

		gio_f_sh = dev;
		gio_f_q = dev && ctx.q_in_gpu;
		gio_f_cap = dev && ctx.cap_in_gpu;
	}

	/* 记 kernel 的起止时钟:由本 QP 实际干活的第一个 block 记 */
	if (t == 0) {
		sh->k_start = clock64();
	}
	if (ctx.opt & 0x40) {
		t++;		/* 没有 poller block:block i 就是 submitter i */
	}
	if (t > ctx.qd) {
		return;
	}
	if (t == 0) {
		if ((ctx.opt & 0x30) == 0) {
			poller(&ctx, sh, tags);
		}
	} else {
		submitter(&ctx, sh, tags, t - 1, start_lba, c_cap, c_sub,
			  c_wait, c_spin, c_lock, c_stat, c_det, c_hand,
			  c_tsreq, c_tsresp, c_reqat, c_dw0);
	}
	if (t == (((ctx.opt & 0x30) && !(ctx.opt & 0x40)) ? 1u : 0u)) {
		sh->k_end = clock64();
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
 * ctxs 是 nq 个 QP 的上下文,sh_out 同样 nq 个。各 QP 的 qd / opt 必须
 * 相同;njobs / job_base 把总条数切成连续段。逐 I/O 数组长度 = 总条数;
 * c_stat 是 NVMe status 字,0xffffffff 表示没等到完成。host_dw0 是
 * CQE DW0,只在 kv 时有意义,可以传 NULL。
 */
int
gpu_io_run(struct gpu_io_ctx *ctxs, unsigned int nq,
	   unsigned long long start_lba,
	   unsigned long long *host_cap, unsigned long long *host_sub,
	   unsigned long long *host_wait, unsigned long long *host_spin,
	   unsigned long long *host_lock, unsigned int *host_stat,
	   unsigned long long *host_det, unsigned long long *host_hand,
	   unsigned long long *host_tsreq, unsigned long long *host_tsresp,
	   unsigned long long *host_reqat, double *wall_us_out,
	   unsigned int *host_dw0, struct gio_shared *sh_out)
{
	uint64_t *d_cap = NULL, *d_sub = NULL, *d_wait = NULL;
	uint64_t *d_spin = NULL, *d_lock = NULL;
	uint64_t *d_det = NULL, *d_hand = NULL;
	uint64_t *d_tsreq = NULL, *d_tsresp = NULL, *d_reqat = NULL;
	uint32_t *d_stat = NULL, *d_dw0 = NULL;
	uint64_t *d_rqs[GIO_MAX_QP] = { NULL };	/* C2:每个 QP 的 RQ 显存影子 */
	struct gio_shared *d_sh = NULL, *sh0 = NULL;
	struct gio_tag *d_tags = NULL;
	struct gpu_io_ctx *d_ctx = NULL;
	uint32_t qd = ctxs[0].qd, opt = ctxs[0].opt, njobs = 0;
	size_t sz, tsz = (size_t)nq * qd * sizeof(struct gio_tag);
	/* 每个 QP:block 0 = poller;opt bit6 时不启动它 */
	unsigned int per = (opt & 0x40) ? qd : qd + 1;
	unsigned int q;
	mcError_t e;
	int rc = -1;

	if (nq == 0 || nq > GIO_MAX_QP) {
		fprintf(stderr, "QP 数 %u 超出 1..%d\n", nq, GIO_MAX_QP);
		return -1;
	}
	for (q = 0; q < nq; q++) {
		if (ctxs[q].qd != qd || ctxs[q].opt != opt) {
			fprintf(stderr, "各 QP 的 qd/opt 必须相同\n");
			return -1;
		}
		if (ctxs[q].job_base + ctxs[q].njobs > njobs) {
			njobs = ctxs[q].job_base + ctxs[q].njobs;
		}
	}
	sz = (size_t)njobs * sizeof(uint64_t);
	sh0 = (struct gio_shared *)calloc(nq, sizeof(*sh0));

	if (!sh0 ||
	    mcMalloc((void **)&d_cap, sz) != mcSuccess ||
	    mcMalloc((void **)&d_sub, sz) != mcSuccess ||
	    mcMalloc((void **)&d_wait, sz) != mcSuccess ||
	    mcMalloc((void **)&d_spin, sz) != mcSuccess ||
	    mcMalloc((void **)&d_lock, sz) != mcSuccess ||
	    mcMalloc((void **)&d_det, sz) != mcSuccess ||
	    mcMalloc((void **)&d_hand, sz) != mcSuccess ||
	    mcMalloc((void **)&d_tsreq, sz) != mcSuccess ||
	    mcMalloc((void **)&d_tsresp, sz) != mcSuccess ||
	    mcMalloc((void **)&d_reqat, sz) != mcSuccess ||
	    mcMalloc((void **)&d_stat, njobs * sizeof(uint32_t)) != mcSuccess ||
	    mcMalloc((void **)&d_dw0, njobs * sizeof(uint32_t)) != mcSuccess ||
	    mcMalloc((void **)&d_sh, nq * sizeof(*d_sh)) != mcSuccess ||
	    mcMalloc((void **)&d_ctx, nq * sizeof(*d_ctx)) != mcSuccess ||
	    mcMalloc((void **)&d_tags, tsz) != mcSuccess) {
		fprintf(stderr, "kernel 工作区分配失败\n");
		goto out;
	}

	for (q = 0; q < nq; q++) {
		struct gpu_io_ctx *c = &ctxs[q];
		size_t rqs_len = (size_t)c->rq_wqe_cnt * c->rq_stride;

		sh0[q].sq_pi = c->sq_pi;
		sh0[q].sp_ci = c->cq_ci;
		sh0[q].sp_phase = c->cq_phase;
		sh0[q].rq_head = c->rq_head;

		/* C2:接管时的 RQ 内容拷进显存,kernel 补 recv 时从这里取描述符 */
		c->rq_shadow = NULL;
		if (!(opt & 0x800) && c->rq_host_buf && rqs_len &&
		    c->rq_stride % 8 == 0) {
			if (mcMalloc((void **)&d_rqs[q], rqs_len) != mcSuccess ||
			    mcMemcpy(d_rqs[q], c->rq_host_buf, rqs_len,
				     mcMemcpyHostToDevice) != mcSuccess) {
				fprintf(stderr, "QP %u 的 RQ 影子分配/拷贝失败\n", q);
				goto out;
			}
			c->rq_shadow = (volatile uint64_t *)d_rqs[q];
		}
	}
	mcMemcpy(d_ctx, ctxs, nq * sizeof(*d_ctx), mcMemcpyHostToDevice);
	mcMemcpy(d_sh, sh0, nq * sizeof(*d_sh), mcMemcpyHostToDevice);
	mcMemset(d_tags, 0, tsz);
	mcMemset(d_stat, 0xff, njobs * sizeof(uint32_t));
	mcMemset(d_dw0, 0, njobs * sizeof(uint32_t));
	mcDeviceSynchronize();

	mcMemset(d_det, 0, sz);
	mcMemset(d_hand, 0, sz);
	mcMemset(d_tsreq, 0, sz);
	mcMemset(d_tsresp, 0, sz);
	mcMemset(d_reqat, 0, sz);
	mcDeviceSynchronize();

	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		*wall_us_out = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
	}
	k_gpu_io<<<nq * per, 32>>>(d_ctx, d_sh, d_tags, per, start_lba, d_cap,
				   d_sub, d_wait, d_spin, d_lock, d_stat, d_det,
				   d_hand, d_tsreq, d_tsresp, d_reqat, d_dw0);

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

	{
		struct timespec ts;

		clock_gettime(CLOCK_MONOTONIC, &ts);
		*wall_us_out = ts.tv_sec * 1e6 + ts.tv_nsec / 1e3 - *wall_us_out;
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
	mcMemcpy(host_stat, d_stat, njobs * sizeof(uint32_t),
		 mcMemcpyDeviceToHost);
	if (host_dw0) {
		mcMemcpy(host_dw0, d_dw0, njobs * sizeof(uint32_t),
			 mcMemcpyDeviceToHost);
	}
	mcMemcpy(sh_out, d_sh, nq * sizeof(*sh_out), mcMemcpyDeviceToHost);
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
	if (d_dw0)  mcFree(d_dw0);
	if (d_sh)   mcFree(d_sh);
	if (d_ctx)  mcFree(d_ctx);
	for (q = 0; q < nq && q < GIO_MAX_QP; q++) {
		if (d_rqs[q]) {
			mcFree(d_rqs[q]);
		}
		ctxs[q].rq_shadow = NULL;
	}
	if (d_tags) mcFree(d_tags);
	free(sh0);
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
