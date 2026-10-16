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

一次 launch,`QD + 1` 个 block(每个 block 只用 thread 0):

```
block 0           poller:独占 CQ 和 RQ
block 1..QD       submitter:每个 block 同一时刻只有一条 I/O 在飞
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
          k    = wqe_counter & rq_mask         ← RESP 的 wqe_counter 是 RQ 消费计数;
                                                  同时和顺序推算核对
          cpl  = op_own & 0x4 ? CQE 开头                ← scatter-to-CQE(坑 11,实际总是)
               : op_own & 0x8 ? CQE 前 64 字节
               : rsp 设备映射[RQ[k].addr - rsp_host_base]
          cid  = cpl[12..13];  status = cpl[14..15]   ← 小端
          tag  = cid & 0x7fff                         ← 校验高位是 0x8000
          NVMe 出错 = (status >> 1) != 0              ← bit0 是 phase tag
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
- 必须**先读完 cpl 再重新 post**,否则 buffer 可能被下一条响应覆盖
  (内联时无此问题,数据在 CQE 里)。

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

`clock64()` 是每线程各自的计数(Tutti 的 MACA 原语说明里写明是
per-SM counter),**跨线程的时间戳不能相减**。所以每条 I/O 的等待时间
在 submitter 自己的线程里量(门铃后 → 看到完成),poller 只统计自己的
圈数。

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

## C1 实现要点(已落地)

- 共享状态和 tag 表放显存(只有 kernel 线程访问,不涉及坑 7);队列、
  胶囊、recv buffer 在主存
- 跨线程读写照 Mooncake `maca_ops.cuh`:读前 / 写后 `__threadfence_system()`;
  锁和领任务用 `atomicCAS`(返回旧值)
- mlx5 的 RQ 和 SQ 在同一块 buffer 里(RQ 在前),SQ 起点通常不对齐页,
  两者整段一起 `mcHostRegister`
- `rq_tail = sq_pi`(已消费 recv = 已收响应 = 已发命令),挂着的 recv =
  `head - sq_pi`,**都在接管快照处读**。不能用 `head - io_queue_size`:SPDK
  补 recv 是批量延迟 flush 的,最后消费的那个常常没 post 回去,实测挂着
  127 个而不是 128 个
- **响应内联在 CQE 里**(mlx5 scatter-to-CQE,坑 11),网卡不写 recv
  buffer。kernel 按 `op_own` 的 0x4/0x8 直接读 CQE;recv buffer 映射只是
  兜底(DPDK 大页,映射后自检,不一致按 2 MiB 对齐重注册,仍失败则放弃)
- 因此 C2 的 RQ 自持不需要关心 buffer 内容,只需把槽原样重新 post
- **角色按 block 分,不按线程分**:`<<<QD+1, 32>>>`,blockIdx 0 = poller,
  1..QD = submitter,每个 block 只有 thread 0 干活。第一版把角色放在同一
  block 的不同线程里,结果 submitter 独占着空等到超时,poller 一圈没转 ——
  同一 block 的线程锁步执行,分支不同时被串行化
- 结果区分"等到响应"和"NVMe 成功",坑 9 根治

## C2 实现要点(2026-10-15 验收通过)

- **在哪做**:`consume_one` 处理完 RESP_SEND 后调 `rq_repost`。独立 poller、
  自轮询(`-O 16`,持 `cq_lock`)、串行快路径(`-O 32`)三条消费路径共用
  这一处。它们在同一时刻只有一个消费者,所以 `sh->rq_head` 用普通的
  volatile 读写就够;自轮询换人时,靠 `cq_lock` 进出的 fence 交接。
- **怎么补**:
  1. 取刚消费的槽 `k = wqe_counter & mask`。
  2. 把 `k` 的描述符写到 `rq_head & mask`。
  3. `rq_head++`,`dbrec[0] = be32(rq_head & 0xffff)`。

  写的顺序是:先写槽,`__threadfence_system`,再写 dbrec,再 fence。
  和 SQ 一样,不敲门铃(rdma-core 的 `mlx5_post_recv` 也是只推 dbrec)。
- **描述符从显存影子取**:`gpu_io_run` 在 launch 前把接管时的整个 RQ
  拷进显存。补的时候从影子读,写到主存的 RQ 槽,同时更新影子的 head 槽。
  这样每条响应只多出两次 posted 写(16 B 槽 + 4 B dbrec)和一次 fence,
  不需要回读主存(回读一次约 0.7 µs)。
- **接管时少挂的那一个 recv**:SPDK 补 recv 是延迟 flush 的,接管时挂着的
  是 127 个,不是 128 个。第一个补进去的会写到那个"已消费、未重挂"的位置,
  那块 buffer 从此不再使用,无害。在飞的 recv 数始终保持 127。
- **对照开关**:`-O 2048` 不补 recv,恢复 C1 行为(`-r` 上限约 127)。可以
  用它测补 recv 本身的开销。
- **检查项**:结束时打印 `RQ a -> b (+n, 期望 +响应数)`;`sh.rq_reposted`
  必须等于响应数,对不上会醒目提示。

### C2 验收记录(mx-decode-02 → mx-prefill-02,cnode2 malloc,4 KiB 读)

| 用例 | 结果 |
|---|---|
| QD 1 × 1000,读 / 写(另起进程回读)/ KV Retrieve | 全部 1000/1000,RQ +1000,块图全 `.` |
| QD 16 × 5000 读 | 5000/5000,RQ +5000,cid / wqe_counter 无异常 |
| `-O 16`(自轮询)QD 4 × 1000、`-O 32`(串行快路径)QD 1 × 1000 | 都 PASS,RQ 增量正确 |

补 recv 的代价:poller 处理一条响应 3.3 → 6.8 µs(`-O 2048` 对照),几乎全是
`rq_repost` 里那两次 `__threadfence_system`。QD 1 下被网络往返盖住,总计 p50
19.4 → 20.1 µs。

### 基线 QD 扫描(C3 前,全关,5000 条)

| QD | IOPS | 总计 p50 (µs) | 处理一条响应 (ns) |
|---|---|---|---|
| 1 | 47.6k | 18.9 | 6301 |
| 4 | 73.3k | 52.4 | 7135 |
| 16 | 73.8k | 214.6 | 7043 |
| 64 | 78.0k | 818.2 | 6658 |

QD 4 就已经饱和,约 75k IOPS(4 KiB 约 300 MB/s)。QD 再往上加,只是让请求
在 poller 前面排队更长。瓶颈是 poller 串行处理每条 I/O 约 13 µs:一条 I/O 有
两个 CQE,每个都要做 fence 和写 dbrec;响应 CQE 还要读主存、补 recv。

`-O 16` 自轮询的平台也一样(QD 4 约 76k),所以瓶颈不在 poller 是一个
独立 block,而在每个 CQE 的处理成本。降本的开关见 `gpu_io_ctx.h`:
0x1000、0x2000、0x4000、0x8000。

### poller 降本:选默认组合(C3)

4 KiB 读,cnode2(malloc),5000 条,每组 3 次取中位数。下表的 `-O` 是
**改默认之前**的含义(位直接生效,0 = 全关)。

| -O | QD1 IOPS | QD1 总计 p50 | QD4 | QD16 | QD64 | 处理一条响应 (ns, QD4) |
|---|---|---|---|---|---|---|
| 0(全关) | 46.3k | 19.3 | 64.3k | 63.5k | 64.2k | 8103 |
| 0x4000 | 46.6k | 18.9 | 85.8k | 85.1k | 84.0k | 8088 |
| 0x6000 | 46.4k | 19.2 | 96.3k | 91.4k | 91.0k | 7560 |
| **0x7000** | 46.5k | 19.3 | **106.0k** | **102.9k** | **103.7k** | 6667 |
| 0xF000 | 42.3k | 21.3 | 102.7k | 101.8k | 102.0k | 6964 |

单项(QD4,1 次):0x1000 +2%,0x2000 +10%,0x4000 +27%,0x8000 −5%。

- 0x4000(SEND 每 32 条才要一个 CQE)收益最大:poller 要处理的 CQE 从每条
  I/O 2 个降到约 1.03 个。
- 0x1000 单开几乎没用,和 0x6000 叠起来再 +10%:fence 次数降下来以后,
  RQ 补槽的 fence 才成为大头。
- 0x8000 提交快 0.7–1.2 µs,等待却多 2.7 µs,QD1 慢 2 µs。去掉"门铃前从
  主存 SQ 槽回读"后,网卡看到 WQE 反而晚了。猜测回读把之前的 posted write
  推了出去;这可能和 C1 的"检测下限"有关,留给 C1 排查。
- 0x7000 在 QD4–64 比全关高 61–65%,QD1 持平。饱和点从约 64k 到约 104k
  IOPS(4 KiB 约 425 MB/s)。

**结论**:0x7000 设为默认,`-O` 的值与它按位或;`-O 0x10000` 不加默认位,
保留旧行为做对照。0x8000 不开。

QD4 起仍然饱和,瓶颈还是单 poller 串行处理 CQE(约 6.7 µs/条,其中主存
读响应约 0.7 µs、fence 若干)。再往上要么多 QP(每 QP 一个 poller,扩带宽),
要么 CQ 放显存(去掉 PCIe 读,降单条成本)。

回归(0x7000 默认,2026-10-15):读 QD1/QD16、写 QD4 + 回读、`-O 16`、
`-O 32`、`-O 2048`、QD 127(自动降为 111)、KV Retrieve/Store/Delete 全部
PASS。`-O 16` 自轮询每约 32 条有一次 ~300 µs 的等待尖刺,改默认前就有,
自轮询只做对照,未处理。

### 多 QP(C3)

单 poller 每条约 6.9 µs,单 QP 上限约 104k IOPS(4 KiB)。最终目标是
GPU 发起的 GDS,要的是大块带宽,单 QP 不够,所以先横向扩:

- `-q N` 建 N 个 io qpair。QP 0 是预热 / 准备用的那个,其余接管前一条
  CPU I/O 都不走。每个 QP 各自一份 ctx(SQ/RQ/CQ/dbrec/UAR/recv buffer
  映射、接管快照)、一个 `gio_shared`、`qd` 个 tag,QP 之间不共享状态,
  不加任何跨 QP 的锁。
- launch `N × (QD + 1)` 个 block,block b 属于 QP `b / (QD + 1)`。ctx 数组
  放显存,每个线程拷一份到本地,之后和单 QP 一样。
- 总条数切成 N 段连续区间:QP q 的第 j 条是全局第 `job_base + j` 条,
  LBA、key、显存块、逐条统计都按全局序号,所以校验、统计代码不用改。
- 胶囊区每个 QP 64 KiB(1024 槽);`-O 512` 的轮换槽位不跨 QP 隔离,
  多 QP 时禁用。
- 统计:共享区计数器合计;dbrec 逐 QP 核对;CQE / SQ dump 只看 QP 0。

约束:
- 所有 block 必须同时常驻(都在自旋)。放不下时没被调度的 poller 会让同
  QP 的 submitter 超时(`GIO_E_WAIT`)。
- target 侧每个 QP 落到一个 reactor,kv_tgt `-m 0x3` 只有 2 个。
- recv buffer 映射可能和别的 QP 落在同一页而失败,这时那个 QP 只支持
  CQE 内联响应 —— 目前响应 100% 内联,不影响。

#### 验收与扩展性(2026-10-15)

功能:`-q 2 -Q 4`、`-q 4 -Q 16` 读,`-q 2` KV Retrieve 全部 PASS,每个 QP
的 SQ/CQ/RQ dbrec 增量都对得上,单 QP 回归不变。

4 KiB 读,cnode2(malloc),kv_tgt `-m 0x3`,3 次中位数:

| QP | QD4 IOPS | QD16 IOPS | QD4 总计 p50 (µs) |
|---|---|---|---|
| 1 | 99k | 101k | 37.8 |
| 2 | 198k | 202k | 37.4 |
| 4 | 401k | 395k | 37.3 |
| 8 | **775k** | 752k | 37.8 |

线性扩展,每个 QP 约 97k(单 poller 约 7 µs/条)。到 8 QP 还没饱和。

128 KiB 读(target 的 max_io_size 上限),QD4:

| QP | kv_tgt `-m 0x3` | kv_tgt `-m 0xF` |
|---|---|---|
| 1 | 9.5 GB/s | - |
| 2 | 18.3 GB/s | 17.9 GB/s |
| 4 | 19.9 GB/s | **24.2 GB/s** |
| 8 | 18.9 GB/s | 24.0 GB/s |

- 2 核时卡在约 20 GB/s:malloc 后端每次读要在 target CPU 上 memcpy 128K,
  单核约 10 GB/s。换 4 核后上限到 24.2 GB/s(≈ 193 Gb/s 净荷),是 200G
  RoCE 的线速量级。
- 也就是说,GPU 发起的这条路径在 4 QP × QD4 × 128K 下已经能跑满网卡,
  initiator 侧不是带宽瓶颈。poller 每条约 6.9 µs,128K 下单 QP 能处理
  约 145k 条/s,用到的不到一半。
- 换真盘后,带宽上限看盘和 target 的读路径,不再是 malloc 的 memcpy。

## 时延调优(C1 续,2026-10-15)

`-O 0x20000` 剖析,QD1 4K 读(cnode2),每条均值。剖析本身每段约多 0.25 µs。

| 提交路径 | ns |
|---|---|
| 写 tag(job/state,两次 fence) | 485 |
| 抢 SQ 锁 | 625 |
| 读共享 sq_pi | 653 |
| 写 WQE 到主存 + fence | 977 |
| 写 dbrec + fence | 717 |
| **回读 SQ 槽 4 次 + 写 BlueFlame + fence** | **5994** |
| 写 sq_pi + 解锁 | 753 |
| 写 t_post + fence | 528 |

| poller | ns |
|---|---|
| 读 CQE 尾(主存) | 1073 |
| 空转:idle_flush | 1284 |
| 空转:done/abort 检查 | 635 |
| 命中:consume_one | 7569–8343 |

网卡时间戳(`-O 0x33080`):SEND 完成 → 响应到达 3.5 µs;GPU 门铃 →
发现 SEND 完成 2.9 µs。网络 + target 一共只占等待的一小半。

结论:
- 提交路径 6 µs 在 BlueFlame 前的 4 次主存回读(每次 ~1 µs)。0x8000 一次
  不读反而等待变长,说明回读同时起到"把 WQE/dbrec 写推到主存"的作用。
  → `-O 0x40000`:寄存器构造 WQE,只回读 1 次。
- 显存上每次 volatile 访问 ~0.5 µs(全局内存延迟)。poller 空转的
  idle_flush 是两次显存读;处理一条响应碰 gio_shared 十几次,另有一次
  多余的 CQE 尾重读(主存 ~1 µs)。
  → 消费者状态和计数器放进寄存器(`struct pstate`,热路径函数全部
  内联),只在开始/结束时和 gio_shared 交换;consume_one 用判 owner 时
  读到的尾部字,不再重读;空转时每 64 圈才查一次全局 abort。

实测(QD1,剖析开着):

| | 改前 | poller 进寄存器 | + `0x40000` |
|---|---|---|---|
| 总计 p50 | 21.1 | **19.4** | 21.2 |
| 提交 p50 | 10.4 | 10.3 | **9.0** |
| 等待 p50 | 9.4 | **8.3** | 11.1 |
| 门铃→poller 发现 | 7.8 | **6.5** | 9.5 |
| poller 空转一圈 | 3.0 µs | **1.6 µs** | 1.6 µs |
| 处理一条响应(consume_one) | 8.3 µs | **5.0 µs** | 5.1 µs |

- poller 进寄存器:全面变好,保留(默认生效)。
- `0x40000`:提交快 1.3 µs,门铃→完成却慢 2.8 µs,和 0x8000 一模一样。
  说明不是"推 posted 写"那么简单 —— 只读 1 次不够,要读满 4 次才快。
- QD4 + `0x40000`:单 QP 123k IOPS(改前 99k),但抢 SQ 锁平均 8.4 µs,
  成了新瓶颈:锁内要做 WQE + dbrec + BF(+回读),约 7–9 µs,单 QP
  提交上限 ≈ 1 / 锁内时间。

BF 写法对照(QD1,0015/0016 合入后重测;之前一轮 0015 没合入,C–F 实际都是 B,作废):

| -O | 写法 | 总计 | 提交 | 等待 | 门铃→发现 |
|---|---|---|---|---|---|
| 0x20000 | 默认:读 1 写 1 交替 4 次 | **20.0** | 10.8 | **8.1** | 6.5 |
| 0x128000 | 不读,32 字节,两块轮换 | 22.1 | 9.9 | 11.4 | 9.6 |
| 0xA8000 | 不读,写满 64 字节 | 21.9 | 11.5 | 9.3 | 7.7 |
| 0x228000 | 不读,写前空等 3.6 µs | 25.6 | 13.8 | 11.2 | 9.6 |
| 0x420000 | 读 4 次后集中写 | 22.4 | 10.2 | 11.1 | 9.5 |

- QP 有 BlueFlame:`[BF] reg=... size=256`。
- 回读并发(0x400000)没有变快,回读那段还是 ~6 µs —— GPU 读主存在这台
  机器上就是串行的;而且等待变长到 11 µs。
- 规律:4 个 8 字节 BF 写**挨着发**时都慢(~11),**被回读隔开**时快
  (8.1),写满 64 字节居中(9.3)。像是网卡收到不完整的 BF 块后要等一段
  再回退去 SQ 取 WQE;默认路径里第一个 8 字节单独到达,网卡把它当普通
  门铃立刻去取。
- `-O 0x800000` 只敲门铃(只写 8 字节):不回读 22.2 / 等待 11.0,回读 1 次
  20.9 / 11.4。网卡回 SQ 取 WQE 比用 BF 数据慢 ~3 µs,默认路径的 BF 数据
  确实被用上了。
- BF 写隔开(0018):fence 隔开 19.9 / 等待 10.4;每个之间空等 0.9 µs
  20.3 / **7.6**。等待能回到默认水平,机制就是"4 个 8 字节写要隔开到达"
  (推测 GPU 连发的 MMIO 写在 PCIe 上合并/乱序,网卡收不到完整 BF 块就
  回退取 SQ);但隔开本身的代价和回读差不多,总计没有收益。
- 结论:主存方案的 QD1 到底了,默认路径不变(回读 4 次)。剩下的大头都是
  GPU 跨 PCIe 访问主存。

主存方案最终基线(不开剖析,4K 读,cnode2,3 次中位数):

| QP × QD | IOPS | 总计 p50 | 提交 p50 | 等待 p50 | 处理一条响应 |
|---|---|---|---|---|---|
| 1 × 1 | 48.0k | **18.3** | 8.8 | 8.6 | 4.8 µs |
| 1 × 4 | 124.8k | 24.6 | 14.0 | 9.1 | 5.0 µs |
| 1 × 16 | 127.2k | 90.7 | 80.7 | 9.0 | 4.8 µs |
| 4 × 1 | 192.6k | 18.3 | 8.9 | 8.3 | 4.8 µs |
| 4 × 4 | 477.0k | 27.3 | 16.9 | 9.3 | 4.9 µs |
| 4 × 16 | 477.5k | 97.5 | 86.8 | 9.2 | 5.1 µs |

对比本节开头(0x7000 默认,poller 未进寄存器):QD1 19.3 → 18.3,单 QP
QD4 106k → 125k,4 QP QD4 401k → 477k。单 QP 的瓶颈从 poller 换成了 SQ 锁:
QD16 时提交 80 µs、等待不变 —— 锁内 fence + 回读 ~8 µs/条。

### 队列进显存(C3,DevX)

回环探针(`make devx-probe`,8 字节 RDMA WRITE 回环,QD1,普通门铃,µs):

| CQ / SQ | 提交 | 门铃→看见 CQE | 合计 | 每轮轮询圈数 |
|---|---|---|---|---|
| 主存 / 主存 | 6.31 | 5.65 | 12.0 | 2.7 |
| 主存 / 显存 | 3.83 | 4.46 | 8.3 | 3.0 |
| 显存 / 主存 | 5.01 | 3.56 | 8.6 | 6.0 |
| 显存 / 显存 | **4.09** | **3.72** | **7.8** | 6.3 |

- 网卡从显存取 WQE 不慢,反而比从主存取快 1.2 µs —— 主存方案里 GPU 写进
  主存的 WQE 网卡取得慢,正是要靠回读 / BF 绕的那个问题
- CQE 进显存:每圈 ~0.6 µs(主存 ~2 µs),门铃→看见快 ~2 µs
- 两者都在显存:比全主存快 35%,只用普通门铃,不需要 BF、不需要回读
- dma-buf umem(`mcExtMallocWithFlags` flag 1)注册、DevX CREATE_CQ/QP 一次通过

`-X`:NVMe-oF 的 IO 队列换成 DevX 自建 QP(`src/devx_qp.c`):

- SPDK 照旧做 admin 连接和准备(qpair 1);DevX 队列用 qid 2
  (`num_io_queues` 多要一个)
- rdma_cm 只做寻址和 CM 握手:不建 verbs QP,`conn_param.qp_num` 填 DevX
  QP 号,私有数据同 SPDK(qid / hrqsize 128 / hsqsize 127 / cntlid);收到
  `CONNECT_RESPONSE` 后用 `rdma_init_qp_attr` 拿 RTR/RTS 参数、DevX 下发,
  再 `rdma_establish`。RoCE 的对端 MAC 从 `/proc/net/arp` 取
- 一块 2 MiB 显存 umem:CQ 512、CQ dbrec、QP dbrec、RQ 128 × 16、SQ 512 × 64;
  `cs_res = 1`,响应 scatter 到 CQE
- 胶囊区、数据区在 DevX 的 PD 上重注册;recv buffer、连接数据在主存
- Fabrics Connect 由 CPU 在这条 QP 上发:往显存 SQ 拷 WQE、CPU 敲门铃、
  拷回 CQE 看结果
- 之后交给现有 kernel:强制寄存器构造 WQE + 只敲 8 字节门铃(DevX 的 UAR
  是 NC,没有 BF)。fence 仍是 system 级,下一步按队列在显存改成 device 级

`-X` 首测(4K 读,cnode2,全部 PASS,dbrec 核对一致):

| | 主存方案 | `-X` |
|---|---|---|
| QD1 总计 / 提交 / 等待 p50 | 18.3 / 8.8 / 8.6 | 18.1 / **4.8** / 12.0 |
| QD1 门铃→poller 发现 | 6.6 | 10.5 |
| 单 QP QD4 IOPS | 124.8k | **182.8k**(+46%) |
| 处理一条响应 | 4.8 µs | 4.1 µs |

- 提交少 4 µs(不再回读 / BF,写 SQ / dbrec 是本地写),SQ 锁不再是瓶颈
- 等待多 3.4 µs:门铃→发现变长。嫌疑:网卡要从显存取 WQE(无 BF)、
  收响应时还要从显存取 RQ WQE;fence 仍是 system 级(poller 每圈 1.3 µs,
  探针里 ~0.6 µs)
- 下一步 `-O 0x4000000` 把共享状态 / 显存队列 / 显存胶囊的 fence 降成 device
  级;`-X -H` 胶囊也进显存

0021 实测作废:fence 级别放在 `__device__` 全局变量里,每次 fence 多一次显存
读,`-X` 和主存方案都变慢(QD1 总计 +1.8 µs,开不开 0x4000000 都一样)。
0022 改成 block 的 shared memory。

0022 实测(QD1 总计 p50):`-X` 18.2;`-X -O 0x4000000` 19.4;主存方案
`-O 0x4000000` 19.1(不开 18.3)。**device 级 fence 在 MACA 上反而更慢**,
0x4000000 保留作对照,不默认开。

`-X -H` 时 Connect 收不到任何 CQE(Connect 胶囊改放主存后仍然如此,
op_own 一直是 0xf0):坑 6。胶囊区和队列显存各一次 ExtMalloc,第二次的
dma-buf offset 0 仍是底层分配的基址,队列 umem 实际指到了胶囊区,网卡把
CQE 写到了别处。0023 改成胶囊区和队列显存同一次分配,umem 用偏移注册。

0023 实测:`-X -H` 能连上、PASS,QD1 19.0(不比 `-X` 好)。网卡硬件时间戳
(`-O 0x13080`,QD1):

| | 网卡侧 SEND 完成→响应到达 | GPU 门铃→发现 SEND 完成 |
|---|---|---|
| 主存方案(BF) | 3.3 | **2.5** |
| `-X`(只敲门铃) | 3.3 | **5.8** |

多出来的 ~3.3 µs 全在门铃到 SEND 完成之间,网络和 target 一样。主存方案
用上了 BF(4 个 8 字节写被回读隔开),网卡直接拿到 WQE;`-X` 只敲门铃,网卡
要回 SQ 取。主存 SQ 不用 BF 时同样慢 ~3 µs(0017 的 `0x800000`),和 SQ 在哪
无关。0024:`-X -G <cycles>` 用 BF 类型的 UAR,4 个 8 字节写之间空等,扫空等
长度找最小能让 BF 生效的间隔。

0025:`-X -q N` 建 N 条 DevX 队列(qid 2..N+1)。DevX 上下文、PD、胶囊区和
数据区的 MR、CM event channel 各队列共用;每条队列有自己的 cm_id、UAR 页、
umem、CQ/QP、recv buffer。所有队列的显存一次分配(`-H` 时连同胶囊区),
每条 2 MiB 一段,umem 按偏移注册(坑 6)。胶囊区和主存方案一样按 QP 切开。
建连按队列顺序串行,每条 CPU 发一次 Connect。

0026:NC 类型的 UAR 几条队列可能在同一页(实测 16 条全是 uar 262、页内
0x800,同一个门铃寄存器),同一页只 mcHostRegister 一次。只敲 8 字节门铃时
共用寄存器没问题(门铃里带 QPN);`-X -G` 多 QP 时 BF 区会互相覆盖,要先改成
每条队列单独的 UAR。

多 QP 实测(4K 读,QD4 / QP,cnode2,全部 PASS,每条队列 dbrec 核对一致;
IOPS = 条数 / kernel 时间):

| QP 数 | 条数 | kernel 时间 | `-X` IOPS | 主存方案 | 单条总计 p50 |
|---|---|---|---|---|---|
| 1 | 5000 | 27.4 ms | 183k | 125k | 19.2 µs |
| 4 | 20000 | 28.4 ms | 704k | 477k | 20.1 µs |
| 8 | 100000 | 72.1 ms | 1.39M | — | 19.9 µs |
| 16 | 100000 | 39.1 ms | 2.56M(10.5 GB/s) | — | 21.9 µs |

吞吐随 QP 数近似线性,16 QP 时单条时延只涨 2 µs,target 还没饱和。
待测:32 QP 以上(GIO_MAX_QP 目前 16)、128K 带宽。

### 访存原语和胶囊内联(C3,DeepEP 对照)

对照 DeepEP / mxshmem 的 IBGDA(`C:\Code\muxi`)后，用 `tst/mem_prims` 量了 MACA 上
各种原语的单次代价(数据和结论见该目录 README)。要点:

- fence 级别无所谓，单独一个 fence 不花钱;花钱的是等之前的写完成
- volatile 写每次都要等写完成(finegrained 显存 ~350 ns),nontemporal 写 ~70 ns 发出、可叠加
- volatile 读串行，nontemporal 读能并发(4 个:1380 → 415 ns)

据此加了两个开关(0028),待实机:

- `-O 0x8000000`(GIO_OPT_NT):热路径读写换成 nontemporal。
  - **写**:WQE、dbrec、RQ 补充、tag、`sq_pi`、解锁、CQ dbrec,每组末尾一次
    `mxc_fence(RELEASE, "device")`;敲门铃后不 fence。
  - **读**:响应的 NVMe CQE / DW0、RQ 影子、submitter 收尾的 tag 字段，一起发出。
  - QD1 串行快路径(0x20)的 `poll_hits` / `resp_cycles` 留在寄存器，退出时写回。
- `-O 0x10000000`(GIO_OPT_INLCAP):胶囊在寄存器里构造，内联进 SEND WQE。
  - WQE 96 字节、2 个 WQEBB,pi 每条 +2;SQ dbrec 核对按 2 倍算。
  - 网卡收到门铃后取一次 WQE 就能发包，不用再去主存取胶囊。
  - 只支持 `-X`,不配 `-G` / `-k`;第 0 条队列跑完 dump 第一条 WQE 并解出胶囊字段。

预期:提交 4.8 → ~3 µs,处理一条响应 4.1 → ~2 µs,门铃 → SEND 完成 5.8 → 4 µs 左右。

实测(2026-10-16,`-X`,cnode2,4K 读，各 1000 条，全部 PASS,dbrec 核对一致;µs p50):

| -O | 总计 | 提交 | 等待 | 处理一条响应 |
|---|---|---|---|---|
| 0(基线) | 19.4 | 5.9 | 12.2 | 4.7 |
| 0x8000000(NT) | 16.4 | **2.9** | 12.2 | **1.7** |
| 0x10000000(内联) | 20.4 | 8.4 | 11.4 | 4.8 |
| 0x18000000(NT + 内联) | 15.4 | 2.8 | 12.3 | 1.7 |
| **0x18000160**(+ 串行快路径、精简提交、无 poller block) | **9.5** | **1.0** | **8.2** | 1.1 |

- NT 符合预期：提交 −3 µs,处理一条响应 −3 µs。
- 内联不配 NT 时提交更慢：要多写 8 个 volatile 字。等待 −0.8 µs,配 NT 之后看不出来。
- **串行快路径 9.5 µs**:低于 CPU 路径(10.4),接近旧串行 kernel(8.6)。
  poller 模式的等待多了 4 µs,构成是：发现得晚(每圈 1.1 µs,串行 0.57 µs)、
  处理一条响应(1.7 vs 1.1)、交接(1.2)。这就是 C1 以来"QD1 比串行版本慢"的那部分。
- 单 QP QD4(NT + 内联):5000 条 22.5 ms,**222k IOPS**(之前 183k),单条 15.8。
- 写(NT + 内联,100 条):总计 27.4,等待 22.7,另起进程回读 PASS。`-X` 下写还没有基线。
- `-O 0x18000080` 那次时间戳作废：默认的 0x4000(每 32 条才要一次 SEND CQE)
  让时间戳配错了对。host 侧现在拒绝 0x80 配 0x4000,要用 `0x10000` 去掉默认位。

第二轮(同一天，同配置):

串行快路径(0x160 基础上)拆开两个开关(µs p50):

| | 总计 | 胶囊 | 提交 | 等待 | 处理一条响应 |
|---|---|---|---|---|---|
| 都不开 | 14.2 | 0.8 | 2.8 | 10.6 | 2.8 |
| NT | 10.9 | 0.9 | 1.1 | 8.9 | 1.2 |
| 内联 | 14.6 | 0.1 | 4.4 | 10.1 | 2.8 |
| NT + 内联 | **9.5** | 0.3 | 1.0 | 8.2 | 1.1 |

- NT 贡献 3.3 µs。
- 内联在 NT 之上再省 1.4 µs:胶囊不用写主存，等待 −0.7 µs。不配 NT 时，多出来的 volatile 写把收益吃掉了。

网卡时间戳(poller 模式，全部 SEND 要 CQE,`-O 0x8013080` / `0x18013080`):

- 网卡侧 SEND 完成 → 响应到达：都是 3.4 µs。
- GPU 门铃 → 发现 SEND 完成:6.6 → 6.0 µs,内联省 0.6。
  这一段还包含一个网络往返(RC 的 SEND 要等对端 ACK 才完成)。

poller 模式剖析(`-O 0x18020000`):

- 空转一圈：读 CQE 375 + idle_flush 267 + done/abort 检查 209 ns。
  后两段在没活干时本应几乎不花钱，各约一次显存读。
- 0029 猜是 ctx 落在 local memory,加了 `-O 0x20000000`(ctx 副本放 shared memory)。
  **猜错了，而且起了反作用**:
  - 开不开这一位都全面变慢:QD1 串行快路径 9.5 → 17.8 µs,空转一圈 0.57 → 2.2 µs;
    poller 模式 15.5 → 20.2 µs。
  - 解释：原来 poller / submitter 全部内联进 kernel,局部的 ctx 已经被编译器拆进寄存器。
    改成运行时在 `&ctx` 和 shared 副本之间二选一的指针后，编译器没法再这样做,
    每次 `c->` 都成了地址空间不确定的访存。
  - 0030 撤掉了这一位，只保留 k_end 的修复。
  - 那些"空段"的 200 ns 更可能是剖析自身的 `clock64()` 开销,mem_prims 加了一行来量。
  - 教训:**ctx 只能以 `&ctx` 传，不要让它的地址在运行时可变**。
- 提交(NT + 内联，含剖析开销):抢锁 459 / 读 sq_pi 639 / 写 WQE+fence 989 /
  dbrec+fence 584 / 门铃 308 / 写 sq_pi+解锁 895 / 写 t_post 297 ns。

QD4 自轮询(`-O 0x18000050`):总计 p50 14.0 / mean 14.7 µs,p90 21 µs(抢 cq_lock 的尾巴)。
0x40 时 k_end 从没记过，打不出 IOPS;0029 一并修了。

(历史)原计划:找出"不回读就慢 2.8 µs"的原因(`-O 0x80000/0x100000/0x200000`
三个对照:BF 写满 64 字节、BF 两块轮换、空等代替读)。找到后去掉回读,
锁内时间能从 ~9 µs 降到 ~3 µs,QD1 时延和单 QP 吞吐一起受益。

## C1 状态(2026-09-17 暂停)

### 已完成

- 功能:4 KiB QD=1 读 100/100、NVMe 全成功、cid / `wqe_counter` 无异常、
  响应 100% 内联在 CQE、块图全 `.`;QD=4(`-O 16`)读也 PASS,锁自旋均值 2
- 过程中定位并修掉的问题:坑 9(不看 NVMe status)、坑 10(角色放在同一
  block 被串行化)、坑 11(scatter-to-CQE)、RQ tail 快照时机与算法
  (`rq_tail = sq_pi`)、RQ 与 SQ 同一块 buffer 须整段注册
- 验收全部通过(独立 poller,QD=1,各 100 条):4 KiB 读/写、1 MiB 读/写,
  响应与 NVMe 成功均 100/100,块图全 `.`

### 未解决:QD=1 延迟比串行版本高 ~2.7 倍

同一 target 配置下(`-u 131072 -i 1048576`),4 KiB 读 p50:

| 版本 | 总计 | 等待 | 门铃→发现 | 处理一条响应 |
|---|---|---|---|---|
| 旧串行(`bin/pit7_initiated`) | 8.6 | 5.3 | — | — |
| C1 独立 poller(`-O 0`),逐字节读 | 26.3 | 20.1 | 19.1 | 4.0 |
| C1 独立 poller,按字读 | 23.1 | 17.3 | 16.4 | 1.9 |
| 自轮询(`-O 16`) | 26.3 | 20.0 | 16.9 | 2.1 |
| 串行快路径,2 block(`-O 32`) | 23.6 | 17.7 | 16.3 | 1.9 |
| 串行快路径,1 block(`-O 96`) | 24.2 | 18.0 | 16.3 | 2.1 |

已排除:

- target 配置(旧串行 kernel 在同配置下仍是 8.6 us)
- 跨 block 交接(poller 置完成 → submitter 看到只要 0.2–1 us)
- 多启动一个 block(`-O 96` 与 `-O 32` 相同)
- 等待循环里的 fence 密度(`-O 1/2/3/5` 全无变化)
- system fence 是全局同步点(`-O 5`:submitter 每 ~250 us 才 fence 一次,
  门铃→发现不变)

**关键线索:1 MiB 下没有回归。**

| | 串行版本 等待 p50 | C1 等待 p50 |
|---|---|---|
| 4 KiB 读 | 5.3 | 17.3 |
| 4 KiB 写 | — | 15.7 |
| 1 MiB 读 | 248.3 | 247.4 |
| 1 MiB 写 | 213.2 | 215.8 |

若新代码给每条 I/O 加了固定开销(包括"命令发出得晚"),1 MiB 也该多
出 ~12 us,实际没有。更像一个**检测下限**:门铃后 ~15 us 内,poller 看不到
已经到达的 CQE;响应本身要 200 us 以上时,下限被掩盖。

已确认的开销:主存上每次 volatile 访问 ~0.7 us(PCIe 往返)。逐字节读
响应要 7 次,改整字读后 3 次,处理一条响应 4.0 → 1.9 us。

**剩下的嫌疑**都在门铃附近,是旧 kernel 没有、新代码有的步骤:

1. 胶囊写完后的 `__threadfence_system()`
2. `sq_pi` 改为显存共享变量,锁内 fence 读、写后 fence
3. 锁的 `atomicCAS` 获取与 release store + fence
4. 门铃后写 `t_post` + fence;门铃前写 tag 的 `job`/`state` + fence
5. 每条 I/O 开头 `claim_job` 的 fence 读 + CAS

### 恢复调试时的第一步

**验证检测下限**:在 `-O 96` 路径里,分别记下 SEND 完成(REQ CQE)和
RESP 被发现的时刻(相对门铃)。SEND 完成本应 ~1 us 就到。

- REQ 也要 ~15 us 才被发现 → 检测下限成立:门铃后一段时间内读主存 CQ
  读到的是旧内容。嫌疑是门铃之后新增的 fence(`sq_pi` store、锁 release、
  `t_post`)触发的刷新,逐个去掉二分;对照旧 kernel 门铃后只有一次 fence
- REQ ~1 us 被发现、RESP 仍 ~15 us → 与 1 MiB 无回归的现象矛盾,
  再回头看 target 侧日志和胶囊字节

之后再决定并发结构:若多 block 本身无害(上表已基本说明),保留自轮询
(`-O 16`)作为默认,删掉独立 poller 与 `-O` 诊断开关,整理 C1 的中间提交。

### 仓库状态说明

`main` 远端停在 C1 fix 3。其后的 C1 diag / perf 提交(等待拆分、fence
合并与 `-O`、自轮询、按字读、串行快路径)只在本地,**整理之前不要推送**。

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
