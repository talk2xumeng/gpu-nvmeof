/*
 * devx_probe.h  --  host 和 kernel 共享的参数(按值传进 kernel)
 */

#ifndef DEVX_PROBE_H
#define DEVX_PROBE_H

#include <stdint.h>

struct dp_args {
	/* SQ:wqe_cnt 个 64 字节槽,QP dbrec([0]=RQ [1]=SQ),门铃(UAR) */
	volatile uint8_t	*sq;
	uint32_t		sq_cnt;
	volatile uint32_t	*qp_dbr;
	volatile uint64_t	*db;
	uint32_t		qpn;

	/* CQ:cq_cnt 个 64 字节 CQE,CQ dbrec */
	volatile uint8_t	*cq;
	uint32_t		cq_cnt;
	volatile uint32_t	*cq_dbr;

	/* RDMA WRITE:本地 src(GPU 能写的映射)→ 远端 dst(回环到自己) */
	volatile uint64_t	*src_dev;
	uint64_t		laddr;
	uint32_t		lkey;
	uint64_t		raddr;
	uint32_t		rkey;
	uint32_t		len;

	uint32_t		n;		/* 轮数 */
	uint32_t		sq_in_gpu;	/* 1:写 SQ/dbrec 后只用 __threadfence() */
	uint32_t		cq_in_gpu;	/* 1:写 CQ dbrec 后只用 __threadfence() */
};

/* kernel 写回 */
struct dp_out {
	uint64_t	k_start, k_end;
	int32_t		err;		/* 0 / -1 超时 / -2 错误 CQE */
	uint32_t	err_iter;
	uint32_t	syndrome;	/* 错误 CQE:vendor << 8 | syndrome */
	uint32_t	done;
	uint64_t	spins;		/* 轮询总圈数 */
};

#define DP_E_TIMEOUT	(-1)
#define DP_E_CQE	(-2)

#endif /* DEVX_PROBE_H */
