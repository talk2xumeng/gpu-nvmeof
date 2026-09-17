# GPU-initiated NVMe-oF 原型：现状与差距

## 一句话

远端 NVMe 的读写可以完全由 GPU kernel 发起，数据直落显存，稳态不经过
CPU。功能层面走通了，但离能用在 PD 分离的生产路径上还差几件事，最要紧
的是**并发**和**接收侧自持**。

---

## 一、跑通了什么

### 稳态全在 kernel 内

一次 kernel launch 跑完 N 轮，每轮做完整的一次 NVMe-oF I/O：

1. 构造 64 字节 NVMe 命令胶囊（opcode / nsid / keyed SGL / LBA / 长度）
2. 构造 mlx5 SEND WQE，写进 SQ
3. 更新 dbrec，敲 UAR 门铃
4. 自旋轮询 CQ，判 owner bit 和 opcode，收响应胶囊
5. 归还 CQ consumer index

CPU 在这期间一条命令都不发、一次完成都不轮询。

### 数据面是真 GDR 直出

payload 全程在 `mcMalloc` 的显存里：

- **读**：target 发 RDMA_WRITE，4096 字节直接落进 HBM
- **写**：target 发 RDMA_READ，直接从 HBM 取走

没有主存中转，没有 bounce buffer，CPU 不碰数据。

### 验证过的

| 项目 | 结果 |
|---|---|
| 读 100 轮 | 100/100，校验 4096/4096 落进显存 |
| 写 100 轮 | 100/100，独立进程回读 LBA 0 得到 pattern |
| 命令送达 | target 侧 dump 确认胶囊逐字节正确、SGL 解析正常 |
| 命令计数 | dbrec SQ 增量 == 轮数，一条不少 |
| target 后端 | 真盘（`0000:59:00.0` 上的 NVMe），不是内存盘 |
| 控制面进显存（`-H`） | 可行，需用 `mcExtMallocWithFlags` 分配 |

### 延迟分解（4 KiB 读，QD=1）

```
总计    22.4 us (p50)
  胶囊   0.4      写 64 字节命令
  提交   2.7      写 WQE + dbrec + 敲门铃
  等待  19.4      轮询到响应
```

「等待」这 19.4 us 里，**主要成分是轮询本身而不是网络**：

```
轮询圈数 mean 40，平均每圈 679 ns
40 × 679 ns ≈ 27 us
```

679 ns 是 GPU 走 PCIe 读一次主存 CQE 的延迟。CQ buffer 在主存，
每轮询一次就是一次 PCIe 往返。

所以这 19.4 us 是「轮询开销 + 网络往返 + target 处理 + 盘的读延迟」
揉在一起的数，轮询粒度把后面几项都盖住了 —— 单独分离没有做，
对当前阶段也不必要。target 挂的是真盘（`0000:59:00.0` 上的 NVMe），
每轮 LBA 顺序递增 8，盘大概率一直在预取。

对照 `latency_baseline` 的 CPU 路径：网络+传输 10.4 us，
整条链路 61.7 us（含两次 kernel 启停各约 20 us）。

---

## 二、CPU 还在做什么

全部集中在初始化，一次性：

| SPDK 负责 | 为什么没自己做 |
|---|---|
| RDMA CM 建连、QP/CQ 创建 | 要 rdma_cm，内核态 |
| NVMe-oF Fabric Connect | 协议握手，多轮 admin 命令 |
| Identify Controller / Namespace | 取 nsid、sector size |
| 注册 MR（经 hook） | `ibv_reg_dmabuf_mr` 是 host API |
| **预 post RQ recv buffer** | **这条是运行期依赖，见下** |

---

## 三、离正式方案的差距

按阻塞程度排。

### 1. 并发 —— 最大的一块

kernel 现在是**单线程串行**：发一条、等一条、再发下一条。QD 恒等于 1。

- 测不了带宽。4096 B / 22 us ≈ 180 MB/s，那是延迟的倒数。
- PD 分离要搬 MB 级 KV block，必须多条在飞才能压满 400G 链路。

要做的：

- 连续 post N 条 WQE 再统一轮询，而不是一条一等
- CQE 和请求用 cid 匹配（现在靠「收到一个 RESP_SEND 就算这轮完成」）
- 多 warp / 多 block 并行提交，`post_send_lock` 之类的互斥
  （mxshmem 的 `ibgda_post_send` 有现成范式）

### 2. 接收侧自持 —— 硬约束

kernel 不补 RQ recv buffer，每轮响应消耗一个 slot。RQ 深度约 128，
**单次 launch 的轮数上限就是 128**。超了会死等，现象和 CQE 判错一样。

要跑长稳态，得把 RQ 也映射给 GPU，让 kernel 自己补 recv WQE。

### 3. 测试覆盖率 —— 现在很弱

kernel 每轮读写的都是**显存偏移 0 那一块 4 KiB**，只有 LBA 在变。所以：

- 读 100 轮，只验证了最后一轮（前面互相覆盖）
- 写 100 轮，只验证了第 0 轮（`-V` 读 LBA 0）
- 漏块、串块、LBA 和数据对不上，**全都查不出来**

要补：显存偏移跟着轮次走，每块填不同 pattern，跑完逐块比对。

### 4. 延迟优化 —— 方向已经明确

CQ 在主存，轮询每圈 679 ns。挪进显存后每圈降到几十纳秒，
「等待」能从 19 us 降到个位数。

做法是自己建 CQ：`mlx5dv_devx_obj_create` 发 `CREATE_CQ`，
`cq_umem_id` 指向显存 umem。`mlx5_ifc.h` 里字段现成，
mxshmem 的 `ibgda_mlx5.cpp:841-855` 可以照抄。

代价是 CQ 换了，QP 得绑新 CQ，SPDK 建的那套就用不上了 ——
牵动比较大，但这是延迟上唯一的大头。

### 5. 错误恢复

kernel 遇到错误 CQE 只能返回错误码退出，QP 恢复要 `ibv_modify_qp`，
那是 host API。生产环境需要想清楚：一条命令失败之后怎么办，
是整个 kernel 退出让 CPU 收拾，还是 kernel 内跳过继续。

---

## 四、绕过去但没根治的

### GPU 写显存，网卡读不到

普通 `mcMalloc` 的显存，GPU kernel 用普通 store 写完之后，网卡的
DMA read 取到的是**写入之前的旧数据** —— 写停在 L2 里，而 MACA 没有
kernel 内可用的 flush 原语（`CanFlushRemoteWrites` 和
`HdpMemFlushCntl` 都返回 0）。

现在用 `mcExtMallocWithFlags(flag=1)` 分配绕开（flag 1 Finegrained 和
4 MapPcieDefault 有效，3/5 无效，6 分配失败）。

**这个约束对生产场景有直接影响**：PD 分离里 KV block 是 attention
kernel 算出来的。如果 attention 和发送在同一个 kernel 里、中间没有
设备同步，payload 会踩同一个坑。那时候要么整块用 uncached（MB 级
数据，带宽代价未知），要么在两个 kernel 之间插同步点。

**同一 kernel 先写 payload 再发命令**：已在 `tst/pit7` 测过，普通显存上
确定性失败（100/100 取到旧值），flag 1/4 可修复。进一步实验表明，写入只有
在 host `mcDeviceSynchronize` 之后才可见；fence、sleep、kernel 退出都无效。
详见 `tst/pit7/README.md`。

理想的解法是驱动组提供 kernel 内的 flush 原语，那样就能回到
「普通 `mcMalloc` + cached 写 + 显式 flush」—— 写得快，网卡也读得到。

---

## 五、下一步建议顺序

1. ~~**多块校验**~~ 已完成：读写各 100 块逐块一致
2. **同一 kernel 内写 payload + 发命令** —— 已定论（`tst/pit7`）：普通显存
   上写入要到 host `mcDeviceSynchronize` 后才对网卡可见，拆 kernel 本身无效。
   生产形态二选一：批量「写 kernel → 同步 → 发送 kernel」，或网卡侧 buffer
   用 uncached 分配；取决于 `make pit7-bench` 测出的 uncached 读写代价
3. **并发提交** —— 暂停中。C1 功能已通过（QD=1/4 读 PASS），但 QD=1 延迟
   23 us，比串行版本（8.6 us）高，原因未定位；排查记录和恢复步骤见
   `docs/concurrency-design.md`「C1 状态」
4. **RQ 自持** —— 和 3 一起做，否则轮数上不去
5. CQ 进显存 —— 延迟优化，可以晚做
6. 错误恢复 —— 交付前

前四项做完，才算有个能拿去接 PD 分离的东西。
