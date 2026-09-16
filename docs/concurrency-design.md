# 并发提交设计(status 第 3、4 项)

## 目标

把 kernel 从"发一条、等一条"改成**多条同时在飞**,并让 kernel 自己补
recv buffer,从而:

- 测出 QD > 1 下的真实带宽和延迟(MB 级 block)
- 单次 launch 的 I/O 数不再受 RQ 深度(~128)限制
- 为 PD 分离的批量 KV 搬运提供数据面

范围:**单 QP**、队列仍由 SPDK 建在主存、胶囊仍在主存。多 QP、队列进
显存、错误恢复不在本阶段。

## 约束(现状)

| 项 | 值 / 来源 |
|---|---|
| NVMe-oF queue size | `num_entries` = io_queue_size,当前 128(target `max_queue_depth` 128) |
| SPDK 预 post 的 recv | `num_entries` 个,buffer 是 `rsps->rsps[i].cpl`(16 字节,连续数组,`spdk_zmalloc` 大页) |
| CQ | `2 × num_entries` = 256,SEND 完成和 RESP 共用 |
| SQ | 512 × 64(mlx5 对齐后) |
| SPDK 自己的 cid | `rdma_req->id` = 0..num_entries-1 |
| QD 上限 | `num_entries - 1` = 127(留一个给安全余量) |
| 坑 3 | 接管后这条 qpair 只归 kernel |
| 坑 7 | kernel 写显存要到 host `mcDeviceSynchronize` 后网卡才可见;队列和胶囊在主存,不受影响 |

## 线程模型

一次 launch,`QD + 1` 个线程:

```
thread 0          poller:独占 CQ 和 RQ
thread 1..QD      submitter:每个线程同一时刻只有一条 I/O 在飞
```

- QD 就是 submitter 线程数,天然受控,不需要额外的 in-flight 计数
- CQ 只有 poller 一个消费者 —— 不需要 Tutti 那套 `head_mark`/`head_lock`
- RQ 补充也只在 poller 里做 —— 天然串行
- submitter 只做:领任务 → 写胶囊 → 抢锁提交 → 等完成 → (读路径)校验

参考:Tutti `nvm_parallel_queue.h` 是每个 submitter 自己扫 CQ;Mooncake
EP IBGDA(`ibgda_device.cuh`)是每 QP 一把 `atomicCAS` 自旋锁串行提交。
我们取 Mooncake 的提交方式 + 单 poller,先求简单可验证。

## 数据结构

全部在 host 注册给 GPU 的主存里(`mcHostRegister` MAPPED),和现在的
SQ/CQ 一样,避开坑 7。

```c
/* 每条 I/O 一项,host 填,kernel 只读 */
struct gio_job {
	uint64_t lba;
	uint64_t data_addr;	/* 显存地址 */
	uint32_t nlb;
	uint8_t  opc;
};

/* 每个 tag(= 在飞槽位,0..QD-1)一项 */
struct gio_tag {
	uint32_t job;		/* submitter 写 */
	uint32_t state;		/* 0 空闲 / 1 已提交 / 2 完成 / 3 出错,poller 写 2/3 */
	uint16_t nvme_status;	/* poller 写 */
	uint64_t t_post, t_done;
};

/* 共享计数,atomicAdd / atomicCAS */
uint32_t next_job;		/* submitter 领任务 */
uint32_t sq_lock;		/* 提交自旋锁 */
uint32_t done_cnt;		/* poller 累加,submitter 看到 == njobs 退出 */
uint32_t abort;			/* 任一方置位,全体退出 */
```

- **tag = submitter 线程号 - 1**。不需要分配器。
- **cid = 0x8000 | tag**,与 SPDK 的 0..127 不重叠。
- **胶囊槽 = tag**(主存胶囊区,64 字节一槽)。
- `gpu_io_ctx` 扩展:RQ 的 buf / wqe_cnt / stride / 接管时 head,
  rsp 数组的 host 基址、设备基址、元素大小,job/tag 表指针,njobs、QD。

## 提交路径(submitter)

```
j = atomicAdd(&next_job, 1) - 1;   if j >= njobs: 退出
tags[t].job = j
写胶囊(槽 t,cid = 0x8000|t)             ← 锁外,互不干扰
lock(sq_lock)                            ← atomicCAS 自旋
    pi = ctx.sq_pi++
    写 WQE 到 slot pi
    dbrec[SQ] = pi + 1
    BF 门铃
unlock
tags[t].state = 1
等 tags[t].state >= 2(__nanosleep 8→256 ns 退避,看 abort)
```

- 锁内只有 WQE + dbrec + 门铃,现测 ~3 µs。串行上限 ~30 万 IOPS,
  对 1 MiB block 远超网卡带宽,v1 够用
- 后续若锁成为瓶颈,换 Tutti 的 `tail_mark` + 批量推 tail/门铃

## 完成路径(poller)

```
loop:
    cqe = CQ[ci]
    if 无新 CQE: 退避, 检查 done_cnt / abort / 超时, continue
    switch opcode:
      REQ(SEND 完成):         忽略(可计数)
      RESP_SEND:
          k    = rq_tail & rq_mask             ← FIFO;同时和 wqe_counter 核对
          seg  = RQ[k] 的 data seg → addr
          cpl  = rsp 设备映射[addr - rsp_host_base]
          cid  = cpl.cid;  tag = cid & 0x7fff   ← 校验高位是 0x8000
          tags[tag].nvme_status = cpl.status
          tags[tag].state = status ? 3 : 2
          done_cnt++
          RQ[rq_head & rq_mask] = RQ[k] 整个 stride   ← 同一块 buffer 重新 post
          rq_head++, rq_tail++
          dbrec[RQ] = be32(rq_head & 0xffff)
      REQ_ERR / RESP_ERR:     abort = 1,记 syndrome
    ci++, cq_dbrec = be32(ci)
```

- **RQ 自持**:RC 的 recv 按 FIFO 消费,槽内容(地址、lkey、终止段)
  消费后网卡不清。处理完响应后把同一段原样写到 head,等于把这块
  buffer 还回去。RQ 只靠 dbrec 推进,不用敲门铃。
- 接管时 `rq_head = be32(dbrec[RQ])`,`rq_tail = rq_head - num_entries`。
- 必须**先读完 cpl 再重新 post**,否则 buffer 可能被下一条响应覆盖。

## 校验

沿用 `blk_pattern.h` 多块校验,job j 用显存第 j 块、LBA `L + j*nlb`。

- **读**:
  - host 侧:结束后 `mcDeviceSynchronize` + DtoH 逐块比对(已验证可信)
  - **kernel 内**:submitter 收到完成后立刻读该块比对,记 mismatch 数。
    这是**反方向可见性**(网卡写、kernel 立刻读)的第一次测试 ——
    decode 侧的真实用法。若 host 侧对、kernel 内错,说明读路径也要
    批次同步
- **写**:另起进程 `-V`(坑 3);payload 由 host HtoD 准备(坑 7 已知)
- **一致性**:`done_cnt == njobs`;dbrec SQ 增量 == njobs;RQ 增量 == njobs;
  每个 tag 的完成数之和 == njobs;cid 高位校验失败计数 == 0

## 计时与指标

- 每条 I/O:`t_post`(门铃后)、`t_done`(poller 置完成时)→ 延迟分布
- 整体:launch 内第一条提交到最后一条完成 → IOPS、带宽
- poller:轮询圈数、每圈耗时、空转比例
- 锁:抢锁自旋次数(看是否成为瓶颈)

## 分阶段与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| C1 | poller/submitter 拆分,QD=1 | 结果与现在一致(块图、延迟) |
| C2 | RQ 自持 | QD=1 跑 1000 条,dbrec RQ +1000,全 `.` |
| C3 | QD 扫描 1/4/16/64/127,4 KiB | 读写全 `.`;kernel 内读比对结果;延迟/IOPS 曲线 |
| C4 | 1 MiB block,QD 扫描 | 带宽对照 `gds_nvmeof`(CPU 路径,同盘同配置) |
| C5(视需要) | 无锁批量门铃 | 锁自旋占比显著时再做 |

C1–C3 进 `src/`(替换现有串行 kernel,保留 `-K` 等旧开关的语义);
不再需要的旧路径删掉,不留两套。

## 上机前要确认的

1. 接管时打印 `dbrec[RQ]`、`rq.wqe_cnt`、`rq.stride`,并从 RQ 槽里读出
   recv buffer 地址,确认它们落在一段连续区间、间隔 = `sizeof(rsp)`
2. 该区间是 DPDK 大页,`mcHostRegister` 能否注册(不能就换成我们自己的
   recv pool:第一轮消费完 SPDK 的 buffer 后改 post 自己的)
3. 响应 CQE 的 `wqe_counter` 是否等于 RQ 索引(和 FIFO 推算核对)
4. target `max_queue_depth` 与 `num_entries` 的实际值
5. `atomicCAS` / `atomicAdd` / `__nanosleep` 在 mxcc 下可用
   (Mooncake / Tutti 的 MACA 分支都在用)

这些做成 C1 的第一个提交里的诊断输出,不单独做实验程序。

## 附:坑 7 给驱动组的问题

Mooncake(`ibgda_device.cuh`)在 MUSA 上写 GPU 显存里的 doorbell record 时,
用 `__threadfence_system_noflush()` + `asm volatile("LSU.ATOM_32.XCHG %0, %1,
slc=byp;")`,注释说明这条 bypass 指令"让写入对网卡可见,而不是留在 GPU
缓存层级里" —— 与坑 7 同一现象。MACA 分支用的是普通 volatile store +
`__threadfence_system()`,也就是我们已证明不够的做法。

问题:**MACA 是否有等价于 `slc=byp` 的 store 指令,或 kernel 内可用的
flush 原语?** 附 `tst/pit7/README.md` 的证据链(同步能刷新;kernel 退出、
fence、sleep、MR 注册方式都不行)。
