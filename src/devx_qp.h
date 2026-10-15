/*
 * devx_qp.h  --  NVMe-oF IO 队列:DevX 自建 QP,队列放显存(-X)
 *
 * SPDK 照旧负责 admin 连接、预热、准备数据(用它自己的 qpair 1)。
 * 这里另建一条 IO 队列(qid = 空出来的那个):
 *   rdma_cm 只负责寻址和 CM 握手,不绑 verbs QP(conn_param.qp_num 填
 *   DevX QP 的号);QP 状态迁移用 rdma_init_qp_attr 拿参数、DevX 命令下发;
 *   CQ / SQ / RQ / dbrec 都在一块显存 umem 里(dma-buf);
 *   Fabrics Connect 由 CPU 在这条 QP 上发(往显存 SQ 拷 WQE,CPU 敲门铃)。
 * 之后把队列交给现有 kernel,ctx 里的指针换成显存的。
 *
 * 依据 tst/devx_probe 的回环实测:CQ、SQ 都放显存,单条比全主存快 35%,
 * 网卡从显存取 WQE 不慢。
 */

#ifndef DEVX_QP_H
#define DEVX_QP_H

#include <stdint.h>
#include <stddef.h>

struct spdk_nvme_ctrlr;
struct gpu_io_ctx;

struct devx_q_cfg {
	const char	*traddr;
	const char	*trsvcid;
	uint16_t	qid;
	uint32_t	qdepth;		/* hrqsize;SQ/RQ/CQ 按它定大小 */
	unsigned	gpu_flag;	/* 显存分配 flag(mcExtMallocWithFlags) */
	/* 已有的胶囊区和数据区(显存 + dma-buf fd),在 DevX 的 PD 上重注册 */
	void		*cap_host;	/* 胶囊区:主存地址,或 -H 时的显存地址 */
	size_t		cap_len;
	int		cap_fd;		/* -H:胶囊区的 dma-buf fd;主存为 -1 */
	void		*data_gpu;
	size_t		data_len;
	int		data_fd;
};

/* 失败返回非 0 并打印原因;成功时把 ctx 里和队列、MR 有关的字段全部填好 */
int devx_q_create(struct spdk_nvme_ctrlr *ctrlr, const struct devx_q_cfg *cfg,
		  struct gpu_io_ctx *ctx);

/* kernel 跑完后核对 dbrec 增量(显存里读),打印一行;返回不一致的项数 */
int devx_q_check(const struct gpu_io_ctx *ctx, uint32_t njobs,
		 uint32_t cqe_req, uint32_t cqe_resp, int no_repost);

#endif /* DEVX_QP_H */
