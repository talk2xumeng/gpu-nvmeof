/*
 * pit7_io_ctx.h  --  tst/pit7 实验程序的 host/kernel 共享上下文
 *
 * 从 src/gpu_io_ctx.h 分叉,多了实验字段(fill_mode/seed/gap_cycles/
 * poll_fence)。和 src/ 不共享,改 src/ 的结构不会影响这里,反之亦然。
 *
 * 以下是原文件说明:
 *
 *
 * 这个结构是按值传进 kernel 的(k_gpu_io 的第一个参数),所以两侧
 * 的字段顺序和类型必须严格一致 —— 早先 host 和 device 各抄了一份,
 * 改一处忘另一处就会静默错位,症状是发包之后永远等不到完成,和
 * CQE 判据写错长得一模一样,非常难查。放这儿是为了让它不可能不同步。
 *
 * 改这个文件必须重编 pit7_io_kernel.o,Makefile 里有对应的依赖。
 */

#ifndef PIT7_IO_CTX_H
#define PIT7_IO_CTX_H

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

	/*
	 * 多块:第 i 轮用显存 data_addr + i * data_step 那一块。
	 * data_step = 0 是旧行为(每轮同一块),-K 对照实验用。
	 */
	uint64_t		data_step;
	/*
	 * fill_mode = 1:每轮在发命令之前,由本 kernel 用普通 store 把
	 * blk_pattern 写进该块 —— 也就是"attention 算完直接发"的生产形态。
	 * 只对 WRITE 有意义。seed 同 blk_pattern.h。
	 */
	uint8_t			fill_mode;
	uint8_t			seed;
	/*
	 * 每轮发命令之前空转这么多 cycle(不访存)。用来模拟 -I 那 ~150 us
	 * 的轮间间隔,区分"等待变长"是间隔本身引起的还是填充访存引起的。
	 * 空转不计入任何分段。
	 */
	uint64_t		gap_cycles;
	/* -Z:轮询每圈做一次 __threadfence_system()(Tutti 的 MACA 写法) */
	uint8_t			poll_fence;
	/*
	 * -X:写完显存(payload / 胶囊)后做一次 __threadfence()。
	 * mxshmem IBGDA 的 IBGDA_MEMBAR 用的就是这一级 —— 它的队列在显存。
	 * 我们的 SQ/CQ 在主存,那边仍用 system 级;这里只加在写显存之后。
	 */
	uint8_t			dev_fence;
	/* -G:读 CQE 里的网卡时间戳(cqe64 offset 48),和 src/ 的 -O 128 同口径 */
	uint8_t			ts_diag;
};

/* 实测 GPU 时钟用:kernel 自己的起止 clock64()(标称频率是最大值,不准) */
struct gio_clock {
	unsigned long long	k_start;
	unsigned long long	k_end;
};

#endif /* PIT7_IO_CTX_H */
