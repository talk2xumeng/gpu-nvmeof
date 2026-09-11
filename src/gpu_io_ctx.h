/*
 * gpu_io_ctx.h  --  host 和 kernel 共享的 I/O 上下文
 *
 * 这个结构是按值传进 kernel 的(k_gpu_io 的第一个参数),所以两侧
 * 的字段顺序和类型必须严格一致 —— 早先 host 和 device 各抄了一份,
 * 改一处忘另一处就会静默错位,症状是发包之后永远等不到完成,和
 * CQE 判据写错长得一模一样,非常难查。放这儿是为了让它不可能不同步。
 *
 * 改这个文件必须重编 gpu_io_kernel.o,Makefile 里有对应的依赖。
 */

#ifndef GPU_IO_CTX_H
#define GPU_IO_CTX_H

#include <stdint.h>

struct gpu_io_ctx {
	volatile uint8_t	*sq_buf;
	volatile uint32_t	*qp_dbrec;	/* [0]=RQ [1]=SQ */
	volatile uint64_t	*bf_reg;	/* UAR,MMIO */
	volatile uint8_t	*cq_buf;
	volatile uint32_t	*cq_dbrec;
	uint32_t		sq_wqe_cnt;
	uint32_t		sq_stride;
	uint32_t		cq_cnt;
	uint32_t		cqe_size;
	uint32_t		qpn;
	volatile uint8_t	*capsule;
	uint32_t		capsule_lkey;
	uint64_t		capsule_addr;
	uint64_t		data_addr;
	uint32_t		data_rkey;
	uint32_t		nsid;
	uint32_t		sector_size;
	uint32_t		io_bytes;
	uint8_t			nvme_opc;	/* 0x02=READ 0x01=WRITE */
	uint16_t		sq_pi;	/* 接管时从 dbrec 读出的 SQ 生产者索引 */
	uint32_t		cq_ci;	/* 同上,CQ 消费者索引 */
	uint8_t			cq_phase;
};

#endif /* GPU_IO_CTX_H */
