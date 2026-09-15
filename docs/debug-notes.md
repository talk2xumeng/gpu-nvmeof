# GPU-initiated NVMe-oF 排查笔记

记的不是结论,是**会把人带沟里的坑**。这些坑每一个都产生过看起来很像
硬件问题的假象,合计浪费了一整天。下次再查这条路径,先把这一页过一遍。

## 现状

| 路径 | 状态 |
|---|---|
| 读(payload 落显存,胶囊在主存) | 通。100/100,4096/4096 落进 HBM |
| 写(payload 源在显存,胶囊在主存) | 通。target 侧 dump 确认收到完整 pattern |
| 胶囊放显存(`-H`) | 通。需用 `mcExtMallocWithFlags` 分配,见"坑 7" |

稳态收发全在 kernel 内:构造 NVMe 命令胶囊、写 mlx5 SEND WQE、更新
dbrec、敲 UAR 门铃、轮询 CQ,CPU 一条都不碰。

---

## 坑 1:快照时序(踩了两次)

`ctx.sq_pi / cq_ci / cq_phase` 必须在**所有 CPU 侧 I/O 之后**拍。

预热、涂色、任何 `spdk_nvme_ns_cmd_*` 都会推进 SQ/CQ。用过期的 pi:

- kernel 把 WQE 写进 SPDK 刚用过的 slot
- 写 dbrec 为 `pi+1`,而 dbrec 已经是这个值
- **网卡认为没有新工作,命令根本发不出去**

症状极具迷惑性:程序报"完成 N/N"、CQE 看着正常、延迟数也有——因为
kernel 收割的是**前一条 CPU 命令的 CQE**。而 target 侧日志里查无此命令。

判据:诊断里 `dbrec: SQ=x (接管时 SQ=y)`,`x - y` 应该等于轮数。
只涨 1 就是踩了。

## 坑 2:`mcMemset` 对 host 是异步的

`gpu_memset_dev()` 排进 stream 就返回。紧接着让网卡去读那块显存,
可能读到还没写完的旧内容。

症状是**同一条命令时而成功时而失败**——赢/输竞态。极易误判成
"显存作 DMA source 不可靠"这类硬件结论。

凡是 memset 之后要交给网卡的地方,都得 `gpu_sync()`。

## 坑 3:接管后不能再用 SPDK 轮询同一条 qpair

kernel 消费了 CQE、占了 RQ slot,SPDK 对这条 QP 的内部状态已经失同步。
之后再调 `spdk_nvme_ns_cmd_read` 回读:

- 要么静默读到全零 —— 看着像"数据没落盘",实际盘上是对的
- 要么直接在 `nvme_rdma_process_recv_completion` 空指针崩

这个假信号最贵:我们为此查了大半天"RDMA_READ 取到空数据",最后在
target 侧 dump `req->iov[0]` 才发现数据一直是完整的。

**写路径的校验必须另起进程**,用 `-P` 涂色 / `-V` 回读三段式。

## 坑 4:Makefile 漏目标 → 跑的是旧二进制

`wqe_verify` 不在 `all` 里,`make` 不编它;`gpu_initiated` 漏了
`wqe_build.h` 依赖,改头文件不重编。

三次拿旧二进制做判断,得出过"BlueFlame 内联才能成功"这种错误结论。

每次改完先确认:

```sh
ls -la bin/ && ./bin/gpu_initiated 2>&1 | head -2
```

## 坑 5:in-capsule 阈值会改变测试语义

target 的 `-c <size>`。当 I/O 长度 ≤ in-capsule size 时,SPDK 把数据
直接塞在胶囊后面 SEND 过去,**根本不走 RDMA_READ**。

要测 keyed SGL 路径必须让 I/O 长度 > in-capsule size。我们用
`-c 512` + 4 KiB I/O。改这个参数会让前后两轮实验不可比。

## 坑 7:GPU kernel 写普通显存,网卡读不到

**最贵的一个坑,查了一整天。**

普通 `mcMalloc` 的显存,GPU kernel 用普通 store 写完之后,网卡的
DMA read 取到的是**写入之前的旧数据**。写停在 L2 里,而 MACA 没有
kernel 内可用的 flush 原语。

症状极具迷惑性:WQE 正确、胶囊 CPU 读回来正确、dbrec 增量正确、
CQ 收到 SEND 完成 + 响应胶囊、往返时间真实、网卡计数器全 0 ——
一切正常,就是 target 收到的命令是 48 字节全零。

定位它靠的是两件事:给 kernel 发的命令 cid 打标记(`0xE000 | i`),
以及在 target 的 `RDMA_REQUEST_STATE_NEW` 就 dump 收到的胶囊 ——
`parse_sgl` 里那个 dump 太晚,格式非法的命令走不到那儿。

### 已实测无效

- `__threadfence_system()` —— 排序原语,不保证回写
- `mcPointerSetAttribute(SyncMemops)` —— 返回 ok,无效果
- 门铃前空转 1 µs / 10 µs / 1 ms
- 去掉 `IBV_ACCESS_RELAXED_ORDERING`
- BlueFlame 门铃内联 vs 8 字节;两种门铃值格式
- `mcDeviceAttributeHdpMemFlushCntl` / `CanFlushRemoteWrites` ——
  C500 上都返回 0,没有 HDP flush 寄存器可用

### 有效的两条

**① 拆 kernel + 设备同步**(验证用,性能不划算)

把写胶囊拆到独立 kernel,中间 `mcDeviceSynchronize()`。通了 ——
这证明缺的就是一次刷新,设备同步能做到,kernel 内的 fence 不能。

**② `mcExtMallocWithFlags` 分配(正式解法)**

```c
mcExtMallocWithFlags(&ptr, len, flag);   /* 而不是 mcMalloc */
```

实测(`mcr/mc_runtime_types.h` 的取值):

| flag | 名字 | 结果 |
|---|---|---|
| 1 | `mcDeviceMallocFinegrained` | **有效** |
| 3 | `mcDeviceMallocWriteCoherence` | 无效 |
| 4 | `mcDeviceMallocMapPcieDefault` | **有效**(uncache + 映射 PCIe) |
| 5 | `mcDeviceMallocMapPcieCoherence` | 无效 |
| 6 | `mcDeviceMallocFixedMemDefault` | 分配失败 |

代码里默认用 1,`-F` 可覆盖。

**payload 上同样成立**(实验分支 `exp/pit7`:kernel 写 4 KiB payload
后立即发 WRITE,普通显存 100/100 块取到旧值,flag 1/4 全部正确)。
现象是确定性的,不是竞态。

**只给命令胶囊用。** uncached 显存的访问延迟高得多 —— 64 字节的
胶囊就让每轮往返从 ~50 µs 涨到 ~110 µs(p50)。MB 级的 payload
必须走普通 `mcMalloc`,否则带宽没法看。

## 坑 6:MACA 的 dma-buf 导出粒度

`mcMemGetHandleForAddressRange` 导出的 dma-buf,**offset 0 是底层分配的
基址,不是你传进去那个子区间的起点**。

两次相邻的 `mcMalloc` 很可能落在同一个 slab。各自导出、各自
`ibv_reg_dmabuf_mr(fd, offset=0)`,第二个 MR 就映射到了第一块的物理页。

定位线索很典型:target 报

```
Invalid NVMf I/O Command SGL:  Type 0xa, Subtype 0xa
```

`Type/Subtype` 两个 nibble 合起来正是另一块的 memset 值。把填充换成
0xBB 会变成 `0xb/0xb`,换成 0x5A 变成 `0x5/0xa` —— 这个对应关系一出来
就实锤了。

解法:一次分配、自己切、只注册一个 MR 覆盖全部。

---

## 坑 8(预防):单字节 pattern + 固定 seed 会让旧数据冒充新结果

盘上的块会保留上一轮写入的内容。如果两轮用同一个 pattern,这一轮
一条都没写成,回读也照样 PASS。

- 写之前进程内把目标 LBA 全部涂成 0xCC,回读见 `C` 就是没写到
- seed 每次换(`make` 按时间取),回读见 `O` 就是上一轮残留
- `-V` 必须带和写入时相同的 `-S`、`-L`、`-r`、`-b`;写进程结束时
  会把完整命令行打出来,照抄即可

块图字符:`.` 一致,`R` 0xBB 未落地,`C` 0xCC 未写,`W` 0x5A 旧式,
`Z` 全零,`F` 其他单字节,`X` 串块,`O` 残留,`?` 乱码。

## 有用的观测手段

**target 侧**(SPDK 需 `./configure --enable-debug` 重编):

```c
/* nvmf_rdma_request_parse_sgl 里 */
SPDK_ERRLOG("CAPSULE opc=0x%02x: <64 字节 hex>\n", ...);
SPDK_ERRLOG("PARSE opc=%02x cid=%04x sgl_type=%x subtype=%x "
            "keyed_len=%u req_len=%u iovcnt=%u xfer=%d\n", ...);

/* nvmf_rdma_request_process,state == READY_TO_EXECUTE 且 xfer 是
 * HOST_TO_CONTROLLER 时,dump req->iov[0].iov_base 前几字节 ——
 * 这是唯一能看到"target 到底收到什么数据"的地方 */
```

`-L nvmf -L rdma` 打开状态机跳转日志,对照 CONNECT 那条的
`1→2→3→4→5` 当模板。

**initiator 侧**:

- 网卡硬件计数器 `/sys/class/infiniband/<dev>/ports/1/hw_counters/`,
  重点 `resp_remote_access_errors`、`resp_cqe_error`、`packet_seq_err`
- `ibv_query_qp` 的 `max_dest_rd_atomic`(入站 RDMA_READ 资源)、
  `qp_access_flags`、`sq_psn`/`rq_psn`(PSN 增量能证明包确实收发了)
- 错误 CQE 的 syndrome 在 offset 55,vendor_err 在 54。
  `0x05 = WR_FLUSH_ERR` 只是后果,真凶在更早的 CQE 里

**`ibdump` 在 CX-7 上不可用**(`Failed to set port sniffer1`),
要抓包得用 MFT 的 `mlx5_core_sniffer`。`tcpdump` 完全抓不到 RoCE。

---

## 已排除的假设(别重复走)

查 `-H`(胶囊放显存)失败时排除过的,每条都有实测:

- BlueFlame 门铃内联 vs 8 字节 —— host 侧对照两种都成功
- 门铃值格式(ctrl seg 原样 vs `{prod_idx<<8, qpn<<8}` 合成)——
  mxshmem 的 `ibgda_ring_db` 用的是后者,两种我们都试过
- `mcPointerSetAttribute(SyncMemops)` —— 调用返回 ok,无效果
- L2 写回延迟 —— 门铃前空转 1 µs / 10 µs / 1 ms 全无效
- MR 权限 / `max_dest_rd_atomic` —— `access=0xe` 带 REMOTE_READ,
  `max_dest_rd_atomic=16`
- 显存作 DMA source —— CPU 路径从同一块显存写,五次全成功
- 命令胶囊字节 —— target 侧 dump 与我们构造的逐字节相同
- fence 级别 —— 我们队列在主存必须用 `__threadfence_system()`;
  mxshmem 用 `__threadfence()` 是因为它队列在显存,不能照抄

## 关于 mxshmem 的 IBGDA

`/opt/maca/*/include/mxshmem/device/non_abi/transport/`

- `mxshmem_mlx_ibgda_device.h:1266` `ibgda_ring_db` —— 门铃写的是
  合成值,opcode/ds 位留空
- `ibgda_post_send` 顺序:`MEMBAR → 更新 dbrec → MEMBAR → 敲门铃`,
  和我们一致
- `need_strong_flush` 是多 CTA 共享 QP 时的跨块可见性保护,单线程用不上
- `IBGDA_MEMBAR()` 硬编成 `__threadfence()`,因为
  `MXSHMEM_IBGDA_SUPPORT_GPUMEM_ONLY` —— 它的队列全在显存
- 队列用 `mlx5dv_devx_umem_reg_ex` + `MLX5DV_UMEM_MASK_DMABUF` 建,
  和我们"接管 SPDK 建的标准 verbs QP"是两条路
- `MEM_GRANULARITY` 默认 2 GiB:它一次向 MACA 要 2 GB 自己切分,
  天然绕开了坑 6

## 还没做的

- ~~**多块校验**~~ 已实现(`blk_pattern.h`,待实机确认)。第 i 轮用
  显存第 i 块、LBA `L+i*nlb`,块内容由 `(seed, LBA)` 决定,读回后
  每块归类成 一致 / 底色 / 串块 / 残留 / 乱码,打印一行块图。
  `-K` 保留旧式单块行为作对照。
- **RQ 补充**。kernel 不补 recv buffer,每轮响应消耗一个,所以单次
  launch 的轮数上限就是 RQ 深度(约 128)。要跑长稳态得把 RQ 也
  映射给 GPU,让 kernel 自己补。
- **真盘验证**。现在 target 是 `bdev_malloc`,纯内存盘。端到端落盘
  还没测过。
- **队列进显存**。SQ/CQ/dbrec 仍在主存(SPDK 建 QP 时分配的)。
  要挪得换 `mlx5dv_devx_umem_reg_ex` 建队列,那是另一个工程 ——
  不过 mxshmem 的 IBGDA 就是这么做的,可以参考。
