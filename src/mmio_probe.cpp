/*
 * mmio_probe.cpp
 *
 * 一个问题:沐曦 GPU 能不能直接访问 mlx5 网卡的队列和门铃寄存器?
 *
 * 这是 GPU-initiated NVMe-oF 的地基。整条路需要 kernel 能做到:
 *   ① 往 SQ buffer 里写 WQE          (主机内存)
 *   ② 更新 doorbell record            (主机内存)
 *   ③ 写 UAR/BlueFlame 寄存器敲门铃   (MMIO!)   <- 最关键
 *   ④ 轮询 CQ 读完成                  (主机内存)
 *
 * ③ 是唯一涉及 MMIO 的,也是唯一可能不支持的。CUDA 上靠
 * cuMemHostRegister(..., IOMEMORY) 实现,MACA 有对应的
 * mcHostRegisterIoMemory,但没实测过。
 *
 * 本程序不碰 SPDK,不建 NVMe-oF 连接。只用裸 verbs 建一个 RC QP,
 * 把它的各个 buffer 映射给 GPU,然后从 kernel 里读写验证。
 * QP 停在 INIT 状态,不会真的发包 —— 敲门铃是空操作,安全。
 *
 * 编译:
 *   mxcc -O2 -x maca -I/opt/maca/include -o mmio_probe mmio_probe.cpp \
 *        -libverbs -lmlx5
 *
 * 运行:
 *   ./mmio_probe mlx5_1 4
 */

#include <mcr/mc_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <infiniband/mlx5dv.h>

/* MACA 的 IoMemory 标志。头文件里通过 hip_to_maca_adaptor.h 暴露,
 * 直接用可能找不到,所以这里给个兜底定义。实际值需与驱动一致。 */
#ifndef mcHostRegisterIoMemory
#define mcHostRegisterIoMemory 0x04
#endif

#define SQ_WQE_CNT	64
#define CQ_CQE_CNT	64

/* ================================================================== */
/* kernel                                                              */
/* ================================================================== */

/* 往普通内存写一个 pattern,再读回 —— 验证 GPU 能读写这块映射 */
__global__ void
k_rw_test(volatile unsigned int *p, unsigned int val, unsigned int *readback)
{
	if (threadIdx.x == 0) {
		p[0] = val;
		__threadfence_system();
		readback[0] = p[0];
	}
}

/* 只读 —— 用于 CQ 这类不该被 GPU 写的区域 */
__global__ void
k_read_test(volatile unsigned int *p, unsigned int *out)
{
	if (threadIdx.x == 0) {
		out[0] = p[0];
	}
}

/*
 * 写 MMIO 门铃。
 *
 * 关键点:必须是 volatile 写 + system 级 fence。GPU 的写会被
 * 缓存或重排,不加 fence 的话网卡可能永远看不到,或者看到的顺序
 * 和预期不符(WQE 还没写完门铃就响了)。
 */
__global__ void
k_write_mmio(volatile unsigned long long *db, unsigned long long val)
{
	if (threadIdx.x == 0) {
		__threadfence_system();		/* 确保之前的写先落地 */
		*db = val;
		__threadfence_system();
	}
}

/* ================================================================== */
/* 工具                                                                */
/* ================================================================== */

static const char *
mc_err(mcError_t e)
{
	const char *s = mcGetErrorString(e);

	return s ? s : "?";
}

/*
 * 把一段 host 虚拟地址映射给 GPU,返回 device 侧指针。
 * flags 里带 mcHostRegisterIoMemory 时用于 MMIO 区域。
 */
static void *
map_to_gpu(void *hptr, size_t len, unsigned int flags, const char *what)
{
	mcError_t e;
	void *dptr = NULL;

	e = mcHostRegister(hptr, len, flags);
	if (e != mcSuccess) {
		printf("    ✗ mcHostRegister(%s, %zu, 0x%x) 失败: %s\n",
		       what, len, flags, mc_err(e));
		return NULL;
	}

	e = mcHostGetDevicePointer(&dptr, hptr, 0);
	if (e != mcSuccess) {
		printf("    ✗ mcHostGetDevicePointer(%s) 失败: %s\n",
		       what, mc_err(e));
		mcHostUnregister(hptr);
		return NULL;
	}

	printf("    ✓ %s: host=%p -> dev=%p\n", what, hptr, dptr);
	return dptr;
}

/* ================================================================== */
/* RDMA 资源                                                           */
/* ================================================================== */

struct rdma_res {
	struct ibv_context	*ctx;
	struct ibv_pd		*pd;
	struct ibv_cq		*cq;
	struct ibv_qp		*qp;

	struct mlx5dv_qp	dv_qp;
	struct mlx5dv_cq	dv_cq;
};

static int
rdma_build(struct rdma_res *r, const char *devname)
{
	struct ibv_device **list;
	struct ibv_device *dev = NULL;
	struct ibv_qp_init_attr qp_attr;
	struct mlx5dv_obj obj;
	int num, i;

	list = ibv_get_device_list(&num);
	if (!list) {
		printf("  ✗ ibv_get_device_list 失败\n");
		return -1;
	}
	for (i = 0; i < num; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), devname)) {
			dev = list[i];
			break;
		}
	}
	if (!dev) {
		printf("  ✗ 找不到 %s。可用:", devname);
		for (i = 0; i < num; i++) {
			printf(" %s", ibv_get_device_name(list[i]));
		}
		printf("\n");
		ibv_free_device_list(list);
		return -1;
	}

	r->ctx = ibv_open_device(dev);
	ibv_free_device_list(list);
	if (!r->ctx) {
		printf("  ✗ ibv_open_device 失败\n");
		return -1;
	}

	if (!mlx5dv_is_supported(dev)) {
		printf("  ✗ %s 不支持 mlx5dv(不是 mlx5 设备?)\n", devname);
		return -1;
	}

	r->pd = ibv_alloc_pd(r->ctx);
	if (!r->pd) {
		printf("  ✗ ibv_alloc_pd 失败\n");
		return -1;
	}

	r->cq = ibv_create_cq(r->ctx, CQ_CQE_CNT, NULL, NULL, 0);
	if (!r->cq) {
		printf("  ✗ ibv_create_cq 失败\n");
		return -1;
	}

	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.send_cq = r->cq;
	qp_attr.recv_cq = r->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr  = SQ_WQE_CNT;
	qp_attr.cap.max_recv_wr  = SQ_WQE_CNT;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	r->qp = ibv_create_qp(r->pd, &qp_attr);
	if (!r->qp) {
		printf("  ✗ ibv_create_qp 失败: %s\n", strerror(errno));
		return -1;
	}

	/* QP 停在 RESET 状态,不做 modify。这样敲门铃不会真的发包。 */

	/* 用 DEVX/DV 挖出底层结构 */
	memset(&r->dv_qp, 0, sizeof(r->dv_qp));
	memset(&r->dv_cq, 0, sizeof(r->dv_cq));
	r->dv_qp.comp_mask = MLX5DV_QP_MASK_UAR_MMAP_OFFSET;

	obj.qp.in  = r->qp;
	obj.qp.out = &r->dv_qp;
	obj.cq.in  = r->cq;
	obj.cq.out = &r->dv_cq;

	if (mlx5dv_init_obj(&obj, MLX5DV_OBJ_QP | MLX5DV_OBJ_CQ)) {
		printf("  ✗ mlx5dv_init_obj 失败: %s\n", strerror(errno));
		return -1;
	}

	printf("  QP  qpn=0x%x\n", r->qp->qp_num);
	printf("    sq.buf   = %p  (%u wqe × %u B = %u B)\n",
	       r->dv_qp.sq.buf, r->dv_qp.sq.wqe_cnt, r->dv_qp.sq.stride,
	       r->dv_qp.sq.wqe_cnt * r->dv_qp.sq.stride);
	printf("    dbrec    = %p\n", (void *)r->dv_qp.dbrec);
	printf("    bf.reg   = %p  (size %u B)  <- UAR/门铃, MMIO\n",
	       r->dv_qp.bf.reg, r->dv_qp.bf.size);
	printf("  CQ  cqn=0x%x\n", r->dv_cq.cqn);
	printf("    cq.buf   = %p  (%u cqe × %u B)\n",
	       r->dv_cq.buf, r->dv_cq.cqe_cnt, r->dv_cq.cqe_size);
	printf("    cq.dbrec = %p\n", (void *)r->dv_cq.dbrec);

	return 0;
}

/* ================================================================== */
/* 测试项                                                              */
/* ================================================================== */

static int g_pass, g_fail;

static void
result(const char *name, int ok, const char *note)
{
	printf("  [%s] %-30s %s\n", ok ? "通过" : "失败", name,
	       note ? note : "");
	if (ok) {
		g_pass++;
	} else {
		g_fail++;
	}
}

/* 主机内存区域:映射 + kernel 读写 */
static int
test_hostmem(void *hptr, size_t len, const char *what, int do_write)
{
	void *dptr;
	unsigned int *d_out;
	unsigned int host_out = 0;
	unsigned int magic = 0xA5A50000u | (rand() & 0xffff);
	int ok = 0;

	printf("\n--- %s ---\n", what);

	dptr = map_to_gpu(hptr, len, mcHostRegisterMapped, what);
	if (!dptr) {
		result(what, 0, "映射失败");
		return -1;
	}

	if (mcMalloc((void **)&d_out, sizeof(*d_out)) != mcSuccess) {
		result(what, 0, "mcMalloc 失败");
		return -1;
	}

	if (do_write) {
		unsigned int saved = *(volatile unsigned int *)hptr;

		k_rw_test<<<1, 32>>>((volatile unsigned int *)dptr,
				     magic, d_out);
		if (mcDeviceSynchronize() != mcSuccess) {
			result(what, 0, "kernel 执行失败");
			goto out;
		}
		mcMemcpy(&host_out, d_out, sizeof(host_out),
			 mcMemcpyDeviceToHost);

		/* GPU 读回的值、以及 CPU 侧看到的值,都要对上 */
		ok = (host_out == magic) &&
		     (*(volatile unsigned int *)hptr == magic);

		printf("    GPU 写入 0x%08x, GPU 读回 0x%08x, CPU 看到 0x%08x\n",
		       magic, host_out, *(volatile unsigned int *)hptr);

		*(volatile unsigned int *)hptr = saved;	/* 还原 */
	} else {
		k_read_test<<<1, 32>>>((volatile unsigned int *)dptr, d_out);
		if (mcDeviceSynchronize() != mcSuccess) {
			result(what, 0, "kernel 执行失败");
			goto out;
		}
		mcMemcpy(&host_out, d_out, sizeof(host_out),
			 mcMemcpyDeviceToHost);
		printf("    GPU 读到 0x%08x, CPU 看到 0x%08x\n",
		       host_out, *(volatile unsigned int *)hptr);
		ok = (host_out == *(volatile unsigned int *)hptr);
	}

	result(what, ok, ok ? "" : "读写不一致");
out:
	mcFree(d_out);
	return ok ? 0 : -1;
}

/*
 * MMIO 门铃。这是整个探测的核心。
 *
 * 分两步:先看能不能映射(这一步失败就说明驱动不支持),
 * 再看 kernel 能不能写(可能映射成功但写的时候挂)。
 */
static int
test_mmio_doorbell(void *bf_reg, size_t len, int do_write)
{
	void *dptr;
	unsigned int flags[] = {
		mcHostRegisterIoMemory | mcHostRegisterMapped,
		mcHostRegisterIoMemory,
		mcHostRegisterMapped,
	};
	const char *fname[] = {
		"IoMemory|Mapped", "IoMemory", "Mapped(对照)",
	};
	int i;

	printf("\n--- UAR 门铃 (MMIO) ---\n");
	printf("    这是关键项。映射不了 = GPU-initiated 走不通。\n");

	dptr = NULL;
	for (i = 0; i < 3 && !dptr; i++) {
		printf("    尝试 flags=%s:\n", fname[i]);
		dptr = map_to_gpu(bf_reg, len, flags[i], "bf.reg");
		if (!dptr) {
			continue;
		}
		printf("    ^ 用 %s 成功\n", fname[i]);
	}

	if (!dptr) {
		result("UAR 门铃映射", 0,
		       "三种 flag 都失败 —— 需向驱动组确认");
		return -1;
	}
	result("UAR 门铃映射", 1, "");

	if (!do_write) {
		printf("    (跳过写测试,加 -w 启用)\n");
		return 0;
	}

	/*
	 * 写一个 0。QP 在 RESET 状态,SQ 是空的,网卡收到门铃后
	 * 找不到有效 WQE,不会发包。安全。
	 */
	printf("    kernel 写门铃...\n");
	k_write_mmio<<<1, 32>>>((volatile unsigned long long *)dptr, 0ULL);

	mcError_t e = mcDeviceSynchronize();

	if (e != mcSuccess) {
		result("UAR 门铃写入", 0, mc_err(e));
		return -1;
	}
	result("UAR 门铃写入", 1, "GPU 可对网卡 BAR 发起 MMIO 写");
	return 0;
}

/* ================================================================== */

int
main(int argc, char **argv)
{
	const char *devname = (argc > 1) ? argv[1] : "mlx5_1";
	int gpu = (argc > 2) ? atoi(argv[2]) : 0;
	int do_write_db = 0;
	struct rdma_res r;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-w")) {
			do_write_db = 1;
		}
	}

	printf("=========================================================\n");
	printf(" GPU 访问 mlx5 队列与门铃 探测\n");
	printf(" 网卡=%s  GPU=%d\n", devname, gpu);
	printf("=========================================================\n");

	if (mcSetDevice(gpu) != mcSuccess) {
		printf("mcSetDevice(%d) 失败\n", gpu);
		return 1;
	}

	printf("\n[1] 建 RDMA 资源(QP 停在 RESET,不发包)\n");
	memset(&r, 0, sizeof(r));
	if (rdma_build(&r, devname) != 0) {
		return 1;
	}

	printf("\n[2] 逐项映射给 GPU\n");

	/* ① SQ buffer —— GPU 要往这里写 WQE */
	test_hostmem(r.dv_qp.sq.buf,
		     (size_t)r.dv_qp.sq.wqe_cnt * r.dv_qp.sq.stride,
		     "SQ buffer (写 WQE)", 1);

	/* ② QP doorbell record —— GPU 要更新它 */
	test_hostmem((void *)r.dv_qp.dbrec, 64,
		     "QP dbrec (doorbell record)", 1);

	/* ③ CQ buffer —— GPU 要读完成 */
	test_hostmem(r.dv_cq.buf,
		     (size_t)r.dv_cq.cqe_cnt * r.dv_cq.cqe_size,
		     "CQ buffer (读完成)", 0);

	/* ④ CQ dbrec */
	test_hostmem((void *)r.dv_cq.dbrec, 64,
		     "CQ dbrec", 1);

	/* ⑤ UAR 门铃 —— 关键项 */
	test_mmio_doorbell(r.dv_qp.bf.reg,
			   r.dv_qp.bf.size ? r.dv_qp.bf.size : 4096,
			   do_write_db);

	/* ---- 结论 ---- */
	printf("\n=========================================================\n");
	printf(" 结论:通过 %d 项,失败 %d 项\n", g_pass, g_fail);
	printf("---------------------------------------------------------\n");
	if (g_fail == 0) {
		printf("  GPU 可访问 mlx5 的全部队列结构和门铃寄存器。\n");
		printf("  GPU-initiated NVMe-oF 的硬件基础具备,可进入下一步:\n");
		printf("    - 从 SPDK 的 NVMe-oF QP 里挖出同样的结构\n");
		printf("    - kernel 内写 WQE + 敲门铃触发一次读\n");
	} else {
		printf("  有失败项。若失败的是 UAR 门铃,GPU-initiated 走不通,\n");
		printf("  需要向驱动组确认 MMIO 映射能力;若失败的是主机内存\n");
		printf("  映射,检查 mcHostRegister 的 flag 取值。\n");
	}
	printf("=========================================================\n");

	if (r.qp) {
		ibv_destroy_qp(r.qp);
	}
	if (r.cq) {
		ibv_destroy_cq(r.cq);
	}
	if (r.pd) {
		ibv_dealloc_pd(r.pd);
	}
	if (r.ctx) {
		ibv_close_device(r.ctx);
	}

	return g_fail ? 1 : 0;
}
