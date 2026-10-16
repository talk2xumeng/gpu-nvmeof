/*
 * gpu_io_ctx.h  --  host 和 kernel 共享的 I/O 上下文
 *
 * 这个结构每个 QP 一份,host 拷进显存数组交给 k_gpu_io,所以两侧
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

	/*
	 * 多块:第 i 轮用显存 data_addr + i * data_step 那一块。
	 * data_step = 0 是旧行为(每轮同一块),-K 对照实验用。
	 */
	uint64_t		data_step;

	/* ---- 并发(docs/concurrency-design.md) ---- */
	uint32_t		qd;		/* submitter 线程数 = 同时在飞的 I/O 数 */
	/*
	 * 等待/轮询的 fence 密度(-O,调优用):
	 *   bit0  submitter 等待时每 16 圈才 fence 一次
	 *   bit1  poller 空转时每 256 圈才检查一次 done/abort
	 *   bit2  配合 bit0,改成每 1024 圈(检验 system fence 是否全局同步点)
	 *   bit4  自轮询:不要独立 poller,submitter 等待时抢 CQ 锁自己收割
	 *   bit5  串行快路径(仅 QD=1):submitter 门铃后直接轮询 CQ,ci 在本地,
	 *         空转每圈只读一次主存 —— 和旧串行 kernel 同形
	 *   bit6  不启动 poller 的 block,只启动 QD 个 submitter block
	 *         (须配 bit4 或 bit5)
	 *   bit8  精简提交(仅 QD=1,须配 bit5):本地 pi,不抢锁、不写共享 sq_pi、
	 *         不写 t_post —— 门铃之后不做任何带 fence 的写,和旧串行 kernel
	 *         的提交路径一致。用来验证门铃后的 fence 是否拖慢网卡
	 *   bit9  胶囊槽位按 job 轮换(j & 0x3f),而不是固定用 tag 号 ——
	 *         旧串行 kernel 就是轮换的
	 *   bit10 cid 用 0xE000 | j(旧 kernel 的取值,仅 QD=1)
	 *   bit7  读 CQE 里的硬件时间戳(cqe64 offset 48,大端):网卡侧看到的
	 *         SEND 完成与响应到达的间隔,用来和 GPU 侧的发现时刻对比。
	 *         多一次主存读,默认不开
	 */
	uint32_t		opt;
	uint32_t		njobs;		/* 本次 launch 的 I/O 总数 */

	/*
	 * RQ 和 recv buffer。响应胶囊通常内联在 CQE 里(scatter-to-CQE,坑 11);
	 * 否则落在 SPDK 预先 post 的 recv buffer(spdk_nvme_rdma_rsp.cpl)。
	 * RESP CQE 的 wqe_counter 是 RQ 消费计数,& (rq_wqe_cnt-1) 即槽位。
	 */
	volatile uint8_t	*rq_buf;
	uint32_t		rq_wqe_cnt;
	uint32_t		rq_stride;
	uint32_t		rq_tail;	/* 接管时最老的已 post 槽的计数(诊断用) */
	uint64_t		rsp_host_base;	/* recv buffer 区间,网卡看到的地址 */
	uint64_t		rsp_len;
	volatile uint8_t	*rsp_dev;	/* 同一区间的 GPU 映射 */

	/*
	 * ---- KV 命令集(wqe_build.h 的 nvme_build_kv_sqe) ----
	 * kv = 1 时 nvme_opc 是 KV opcode,第 j 条用 key blk_kv_key(kv_seed,
	 * start + j)、显存第 j 块;Retrieve 时 poller 额外把响应的 CQE DW0
	 * (value 长度)收进 tag。kv = 0 时这几个字段不读,读写路径不变。
	 */
	uint8_t			kv;
	uint8_t			kv_klen;	/* 1..16 */
	uint8_t			kv_seed;
	uint32_t		kv_opts;	/* CDW11[15:8],NVME_KV_STORE_* / NVME_KV_RETRIEVE_* */

	/*
	 * ---- C2:RQ 自持(docs/concurrency-design.md) ----
	 * 收完一个响应就把那块 recv buffer 原样重新 post 到 RQ 头,单次 launch
	 * 的 I/O 数不再受 RQ 深度(~127)限制。槽内容从显存里的影子副本取,
	 * 不回读主存(每次 ~0.7 us);写 RQ 槽 + dbrec[0] 都是 posted 写。
	 *   rq_head     接管时 dbrec[0] 的值(已 post 计数)
	 *   rq_shadow   显存,rq_wqe_cnt 个槽、每槽 rq_stride 字节,接管时的 RQ 内容
	 *   rq_host_buf 主存里的 RQ(host 指针,只给 gpu_io_run 拷影子用)
	 * opt bit11(0x800)关掉,恢复 C1 行为(做对照用)。
	 */
	uint32_t		rq_head;
	volatile uint64_t	*rq_shadow;
	const void		*rq_host_buf;

	/*
	 * ---- C3:多 QP ----
	 * 每个 QP 一份 ctx、一个 poller、qd 个 submitter。总条数按 QP 切成
	 * 连续段:本 QP 的第 j 条(0..njobs-1)是全局第 job_base + j 条 ——
	 * LBA / key / 显存块 / 逐条统计数组都按全局序号取。单 QP 时 job_base = 0。
	 */
	uint32_t		job_base;

	/*
	 * BlueFlame 的另一半(rdma-core 每次 post 在 bf.reg 和 reg ^ bf.size
	 * 两块之间轮换)。-O 0x100000 时按 pi 奇偶轮换;映射失败为 NULL。
	 */
	volatile uint64_t	*bf_alt;

	/* 队列(SQ/CQ/RQ/dbrec)在显存(-X);胶囊在显存(-H)。配 GIO_OPT_DEVFENCE */
	uint8_t			q_in_gpu;
	uint8_t			cap_in_gpu;

	/* GIO_OPT_BFGAP:BF 的 4 个 8 字节写之间空等的时钟数(-G,默认 1000) */
	uint32_t		bf_gap;
};

/*
 * kernel 内线程共享的状态,放显存。字段只由注释里的一方写。
 * 布局 host 和 kernel 共用,host 同步后整块拷回。
 */
struct gio_shared {
	uint32_t	next_job;	/* submitter, atomicCAS */
	uint32_t	sq_lock;	/* submitter, atomicCAS / release store */
	uint32_t	sq_pi;		/* submitter,锁内 */
	uint32_t	done_cnt;	/* poller */
	uint32_t	abort;		/* 任一方 */
	int32_t		err;		/* 置 abort 的一方写,第一个错误码 */
	uint32_t	nvme_err;	/* poller:status 非 0 的响应数 */
	uint16_t	first_status;	/* poller:第一个非 0 status */
	uint16_t	first_bad_cid;	/* poller:第一个对不上的 cid */
	uint32_t	cid_bad;	/* poller */
	uint32_t	wc_mismatch;	/* poller:wqe_counter 与顺序推算不符 */
	uint32_t	cqe_req;	/* poller:SEND 完成数 */
	uint32_t	cqe_resp;	/* poller:响应数 */
	uint32_t	err_cqe;	/* poller:错误 CQE 的 (vendor << 8) | syndrome */
	uint32_t	resp_inline;	/* poller:scatter-to-CQE 的响应数 */
	uint64_t	poll_spins;	/* poller */
	uint64_t	poll_hits;	/* poller:读到有效 CQE 的圈数 */
	uint64_t	poll_cycles;	/* poller:自己时钟下的总运行时间 */
	/* 自轮询(opt bit4):持 cq_lock 的 submitter 才能读写下面四个 */
	uint32_t	cq_lock;
	uint32_t	sp_ci;
	uint32_t	sp_phase;
	uint32_t	sp_seq;
	uint64_t	last_req_ts;	/* opt bit7:最近一个 SEND 完成 CQE 的网卡时间戳 */
	uint64_t	last_req_at;	/* 同上,发现它时的 GPU 时钟 */
	/*
	 * 实测 GPU 时钟用:kernel 自己的起止 clock64()。
	 * mcGetDeviceProperties 给的是标称最大频率,实际频率随负载变,
	 * 用标称值换算微秒会系统性偏(两个二进制之间实测差过 1.67 倍)。
	 */
	uint64_t	k_start;
	uint64_t	k_end;
	uint64_t	resp_cycles;	/* poller:处理响应(handle_resp)的总耗时 */
	/* C2:当前消费者(poller / 持 cq_lock 者 / 串行快路径)独占读写 */
	uint32_t	rq_head;
	uint32_t	rq_reposted;
	uint32_t	rq_pending;	/* opt 0x1000:已写槽、还没推 dbrec 的个数 */
	uint32_t	rq_flushes;	/* 推 dbrec[0] 的次数 */
	uint32_t	cq_ci_written;	/* opt 0x2000:最近一次写进 CQ dbrec 的 ci */
	uint32_t	cq_db_writes;	/* 写 CQ dbrec 的次数 */
	/* opt GIO_OPT_PROF:分段计时累计(cycles),下标见 GIO_PF_* */
	uint64_t	prof[16];
};

/*
 * 剖析(-O 0x20000):提交路径和 poller 每圈拆段计时,累计进 sh->prof。
 * 提交段由各 submitter atomicAdd,poller 段只有 poller 写。
 */
#define GIO_OPT_PROF	0x20000u
/*
 * BlueFlame 只回读 1 次(-O 0x40000):WQE 在寄存器里构造,写 SQ 槽和
 * BlueFlame 都用寄存器里的值,中间只做一次 8 字节主存读来推出前面的
 * posted 写。默认路径回读 4 次(~6 us);0x8000 不读(等待变长)。
 */
#define GIO_OPT_BF1	0x40000u
/*
 * BlueFlame 写法对照(须配 0x8000 或 0x40000,即寄存器构造 WQE 的路径)。
 * 0x8000 / 0x40000 提交快了,门铃→完成却慢 ~2.8 us,下面三位用来找原因:
 *   0x80000   BF 写满 64 字节(后 32 字节补 0)。mlx5 的 BF 以 64 字节为
 *             单位,rdma-core 的 mlx5_bf_copy 也是按 64 字节写;我们只写 32
 *   0x100000  BF 两块缓冲按 pi 奇偶轮换,同 rdma-core
 *   0x200000  写 BF 前空等 ~4096 个时钟(~3.6 us),不读主存 —— 看慢的
 *             原因是"读"还是"时间"
 */
#define GIO_OPT_BF64	0x80000u
#define GIO_OPT_BFTOG	0x100000u
#define GIO_OPT_BFDLY	0x200000u
/*
 * 0x400000:4 次回读先全部发出再写门铃(实测回读耗时不变,等待反而变长)。
 * 0x800000:普通门铃 —— 只往 bf.reg 写控制段前 8 字节(rdma-core 不用 BF
 *           时的写法),网卡从主存 SQ 取 WQE。配 0x8000(不回读)或
 *           0x40000(回读 1 次)。
 *
 * BF 对照(QD1,2026-10-15,QP 有 BF:bf.size = 256):回读 4 次时 4 个 8 字节
 * BF 写之间各隔 ~1.5 us,网卡最快(等待 8.1);4 个写紧挨着发(不回读 /
 * 回读后集中写)等待 ~11;写满 64 字节 9.3;轮换、空等无效。像是网卡在
 * 等一个不完整的 BF 块,超时后才回退去 SQ 取 —— 所以试只敲门铃。
 */
#define GIO_OPT_RBPAR	0x400000u
#define GIO_OPT_DB8	0x800000u
/*
 * 0x800000 实测(QD1):不回读 / 回读 1 次,等待都是 ~11 us —— 网卡回 SQ 取
 * WQE 比用 BF 数据慢 ~3 us。默认路径快,应是 BF 数据真被用上了,而且 4 个
 * 8 字节写是被回读隔开到达的。下面两位验证"隔开"能否代替回读:
 *   0x1000000  每个 BF 8 字节写之后 system fence
 *   0x2000000  BF 8 字节写之间空等 ~0.9 us
 */
#define GIO_OPT_BFSEP	0x1000000u
#define GIO_OPT_BFGAP	0x2000000u
/*
 * 0x4000000:fence 降级。共享状态(gio_shared / tag,都在显存)用
 * __threadfence();队列在显存(q_in_gpu)、胶囊在显存(cap_in_gpu)时,写它们
 * 之后也用 __threadfence()。写 MMIO 门铃后的 fence 仍是 system。
 */
#define GIO_OPT_DEVFENCE	0x4000000u
/*
 * 0x8000000:热路径读写换成 nontemporal(同 DeepEP MACA 分支)。
 * tst/mem_prims 实测:MACA 上每个 volatile 写都要等写完成(finegrained
 * 显存 ~350 ns),nontemporal 写 ~70 ns 发出、可叠加;volatile 读串行,
 * nontemporal 读能并发(4 个 1380 → 415 ns);fence 本身不花钱,花的是
 * 等之前的写完成。所以:一组写全用 nontemporal,末尾 fence 一次;同一
 * 处要读的几个字段一起发出;敲门铃之后不再 fence(DeepEP 也不)。
 */
#define GIO_OPT_NT		0x8000000u
/*
 * 0x10000000:胶囊内联进 SEND WQE(仅 -X、NVM 读写)。胶囊在寄存器里
 * 构造,不写胶囊区;WQE = ctrl 16 + inline 头 4 + 胶囊 64,补齐到 96 字节
 * (ds = 6),占 2 个 WQEBB,pi 每条 +2。网卡收到门铃后只取一次 WQE 就能
 * 发包,省掉取胶囊那次 DMA 读(-X 胶囊在主存)。
 */
#define GIO_OPT_INLCAP		0x10000000u
/*
 * 0x20000000:ctx 副本放 block 的 shared memory。默认 ctx 是 k_gpu_io 的
 * 局部变量、地址传给 poller / submitter,落在 local memory(显存);每次
 * fence 之后编译器都要重读 c->opt / c->njobs / c->cq_buf …… 剖析里 poller
 * 空转一圈"done 检查"209 ns、"idle_flush"267 ns,各约一次显存读。
 */
#define GIO_OPT_SHCTX		0x20000000u
#define GIO_PF_TAG	0	/* 写 tag 的 job/state(两次 fence) */
#define GIO_PF_LOCK	1	/* 抢 SQ 锁 */
#define GIO_PF_PI	2	/* 读共享 sq_pi(fence) */
#define GIO_PF_WQE	3	/* 写 WQE 到主存 SQ 槽 + FENCE_QUEUE */
#define GIO_PF_DBR	4	/* 写 SQ dbrec + FENCE_DB */
#define GIO_PF_BF	5	/* 回读 SQ 槽 + 写 BlueFlame + FENCE_DB */
#define GIO_PF_REL	6	/* 写共享 sq_pi + 解锁(两次 fence) */
#define GIO_PF_POST	7	/* 门铃后写 t_post + fence */
#define GIO_PF_IREAD	8	/* poller 空转:读 CQE 尾(主存) */
#define GIO_PF_IFLUSH	9	/* poller 空转:idle_flush */
#define GIO_PF_ICHK	10	/* poller 空转:done/abort 检查 */
#define GIO_PF_HFENCE	11	/* poller 命中:owner 之后的 fence */
#define GIO_PF_HCONS	12	/* poller 命中:consume_one(含补 recv) */
#define GIO_PF_HCI	13	/* poller 命中:advance_ci */
#define GIO_PF_NIDLE	14	/* poller 空转圈数 */
#define GIO_PF_NHIT	15	/* poller 命中次数 */

/*
 * poller 降本(C3 前置)。各自一个 opt 位,便于逐项对照:
 *   0x1000  RQ 批量补:攒 GIO_RQ_BATCH 个或空闲时才 fence + 推 dbrec[0]
 *   0x2000  CQ ci 懒更新:每 GIO_CQ_BATCH 个 CQE 或空闲时才写 CQ dbrec,不 fence
 *   0x4000  SEND 选择性 signal:每 GIO_SIG_EVERY 条才要 CQE
 *   0x8000  WQE 在寄存器里构造,不从主存 SQ 槽回读再写 BlueFlame
 *
 * 0x7000 默认开(-O 的值与它按位或)。QD 扫描(4K 读,3 次中位数)里
 * 0x7000 在 QD4-64 比全关高 60%+,QD1 持平;0x8000 让等待变长,不开。
 * 要旧行为做对照,给 -O 带 GIO_OPT_LEGACY(0x10000),默认位就不加。
 */
#define GIO_OPT_DEFAULT	0x7000u
#define GIO_OPT_LEGACY	0x10000u

/* 多 QP 上限(-q) */
#define GIO_MAX_QP	16

#define GIO_RQ_BATCH	16
#define GIO_CQ_BATCH	32
#define GIO_SIG_EVERY	32

/* 每个 tag(= submitter 线程)一项,放显存 */
struct gio_tag {
	uint32_t	state;		/* 0 空闲 1 已提交(submitter) 2 完成 3 NVMe 出错(poller) */
	uint32_t	job;		/* submitter */
	uint16_t	status;		/* poller:NVMe status 字(含 phase tag 位) */
	uint64_t	ts_resp;	/* opt bit7:响应 CQE 的网卡时间戳 */
	uint16_t	rsvd;
	uint32_t	dw0;		/* poller:NVMe CQE DW0(仅 ctx.kv) */
	/*
	 * 等待拆分(诊断)。clock64() 可能是每个 block 各自的计数,跨 block
	 * 相减的结果若离谱,说明不能这么比。
	 */
	uint64_t	t_post;		/* submitter:门铃后 */
	uint64_t	det;		/* poller:发现响应时 now - t_post */
	uint64_t	t_done;		/* poller:置完成之后 */
};

#define GIO_ST_IDLE	0
#define GIO_ST_POSTED	1
#define GIO_ST_DONE	2
#define GIO_ST_NVME_ERR	3

#define GIO_CID_TAG	0x8000u		/* kernel 的 cid = 0x8000 | tag,和 SPDK 的 0..N-1 分开 */

/* kernel 错误码 */
#define GIO_E_TIMEOUT	(-1)		/* poller 长时间没有新 CQE */
#define GIO_E_CQE	(-2)
#define GIO_E_CID	(-3)
#define GIO_E_RSP_ADDR	(-4)
#define GIO_E_WAIT	(-6)		/* submitter 等自己的完成超时 */

#endif /* GPU_IO_CTX_H */
