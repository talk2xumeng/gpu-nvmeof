# tst/pit7 —— kernel 写显存,网卡什么时候能读到

对应 `docs/debug-notes.md` 坑 7。本目录是一套**独立的实验程序**:
host 侧 `pit7_initiated.c`、kernel `pit7_io_kernel.cpp`、上下文
`pit7_io_ctx.h`,从 `src/` 分叉而来,参数是 `src/gpu_initiated` 的超集。
和 `src/` 不共享 ctx 结构,互不影响;只复用 `src/wqe_build.h` 和
`src/blk_pattern.h`。`make` 默认会编它,编不过说明共用头文件变了。

## 问题

普通 `mcMalloc` 显存上,GPU kernel 用普通 store 写入的内容,网卡 DMA
读到的是旧值。这对 PD 分离是硬约束:KV block 由 attention kernel 写出,
随后要交给网卡发走。

## 已有结论

| 现象 | 证据 |
|---|---|
| 同一 kernel 内,写入对网卡**始终不可见** | R0 / R3 / W2 全失败;历史上门铃前 sleep 1 µs~1 ms 无效 |
| `__threadfence_system()` 不触发回写,每圈 fence 也不行 | R3 = R0,W2 = 100 `S` |
| 不是竞态,是确定性的 | 100/100 块一致失败;单 kernel 跑 ~0.8 ms 后复用槽位仍读到全零 |
| 写入 kernel 退出(或随后的设备同步)之后,内容可见 | R1/R2/R4 块图见下 |
| uncached 分配(flag 1 / 4)可以绕开 | payload 100/100 正确,但 4 KiB 填充贵 ~35% |
| Tutti(本地 NVMe,C500)同样把门铃放在写入 kernel 内,实测可用 | 推测是 SSD 取 SQE 晚于 kernel 退出,赢了竞态;未验证 |

R1/R2/R4(门铃在写入 kernel 内部,之后退出)的块图:

```
....................................RRRRRRRRRRRRRRRRRRRRRRRRRRRR
RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR
```

胶囊槽位按 `i & 0x3f` 复用。第 0–63 轮首次用槽位,网卡读到初始的全零
→ 全 `R`;第 64–99 轮复用槽位 0–35,网卡读到的是**已经刷下去的**第 0–35
轮胶囊,于是执行了"读 LBA 0–35 到块 0–35" → 块 0–35 正确。
R0(单 kernel 从不退出)是 100 `R`:复用槽位时读到的仍是全零。

R1 每轮 poll 之后还有一次 host 同步,所以"刷新点是 kernel 退出还是
设备同步"第一轮分不开 —— 第二轮的 P3 专门回答这个。

## 待验证(第二轮,`make pit7-matrix`)

把门铃挪到写入 kernel **退出之后**的另一个 kernel,不依赖竞态:

| 用例 | 配置 | 判读 |
|---|---|---|
| C3 | 主存胶囊,`-T 3` | 自检,必须全 `.` |
| P3 | 普通显存胶囊,写 \| 门铃,中间不同步 | **全 `.` → kernel 退出即刷新点** |
| P4 | 同上,中间同步 | 对照 |
| Q3 | kernel 写 payload,写 \| 门铃 | 生产形态 |
| Q4 | 同上,中间同步 | 对照 |
| Q3H | 同 Q3,胶囊也在普通显存 | 全程普通 `mcMalloc` |

- P3/Q3 成功:正式方案是"写显存的 kernel"与"敲门铃的 kernel"分开,
  不需要 uncached,不需要 host 同步,代价是每批一次 launch
- P3 失败、P4 成功:刷新点在设备同步,每批要付一次同步
- 都失败:与 debug-notes 里"拆 kernel + 同步有效"矛盾,先查拆分实现

结果出来后:结论回填到本文件和 `docs/debug-notes.md` 坑 7,
正式机制在 `src/` 里按正式代码实现。

## Tutti 侧建议的反向实验

在 `snvme_smoke_gpu.cu` 里把 `k_poll_one` 的轮询并进 `k_submit_rw`
(敲完门铃不退出,原地轮询)。按上面的模型,SSD 应读到旧 SQE,表现为
cid 不匹配或超时。若仍然成功,说明 snvme 的 P2P 映射和 dma-buf MR
不是同一类页,值得单独查。

## 复现

```sh
make                                   # 同时编出 bin/pit7_initiated
make pit7-matrix                       # 第二轮
make pit7-matrix MATRIX_V1=1           # 第一轮
make pit7-bench                        # 填充微基准,不需要 target
```

日志落在 `tst/pit7/logs/`(不入库)。单个用例手动跑:

```sh
G="sudo ./bin/pit7_initiated -a 172.16.3.3 -n nqn.2024-01.io.test:cnode1 -g 4 -r 100 -b 4096"
$G -S 37 -H -F 0 -T 3                  # 读
$G -S 38 -W -I 1 -D 0 -T 3             # 写 ...
$G -S 38 -V                            # ... 另起进程回读
```

## 参数(相对 `src/gpu_initiated` 新增)

| 参数 | 含义 |
|---|---|
| `-I 1` | 发命令的同一个 kernel 边写 payload 边发(显存底色 0xEE) |
| `-I 2` | 独立 kernel 预写全部块 + 设备同步,再由 io kernel 发 |
| `-D <flag>` | 数据区用 `mcExtMallocWithFlags(flag)` 分配 |
| `-T 1` | 每轮:`k_submit`(写+门铃)退出 → `k_poll` → 同步 |
| `-T 2` | 同上,submit 和 poll 之间同步 |
| `-T 3` | 每轮:`k_prep`(只写显存)退出 → `k_post_poll`(门铃+轮询)→ 同步 |
| `-T 4` | 同上,两个 kernel 之间同步 |
| `-Z` | 轮询每圈 `__threadfence_system()`(Tutti 的 MACA 写法) |
| `-Y <us>` | 每轮发命令前空转,不访存 |
| `-B` | 填充微基准:只算 / 只写 / 算+写 / 只读 × flag 0/1/4 |

块图字符:`.` 一致,`R` 0xBB 未落地,`C` 0xCC 未写,`S` 0xEE 显存旧值,
`W` 0x5A,`Z` 全零,`F` 其他单字节,`X` 串块,`O` 残留,`?` 乱码。

## 原始数据

### 第一轮(2026-09-15,4 KiB × 100,真盘)

| 用例 | 配置 | 块图 | 总计 p50 | 等待 p50 | host 墙钟 p50 |
|---|---|---|---|---|---|
| C0 | 主存胶囊 `-T 1` | 100 `.` | 10.9 | 6.1 | 57 |
| R0 | 普通显存胶囊,单 kernel | 100 `R` | 8.2 | 3.1 | — |
| R1 | + `-T 1` | 36 `.` 64 `R` | 8.7 | 2.4 | 41 |
| R2 | + `-T 2` | 36 `.` 64 `R` | 8.5 | 2.3 | 50 |
| R3 | + `-Z` | 100 `R` | 8.2 | 3.1 | — |
| R4 | + `-T 1 -Z` | 36 `.` 64 `R` | 8.7 | 2.4 | 40 |
| W1 | kernel 写 payload `-T 1` | 100 `S` | 131.0 | 9.8 | 222 |
| W2 | kernel 写 payload `-Z` | 100 `S` | 130.8 | 13.6 | — |
| W3 | kernel 写 payload `-T 1 -Z` | 100 `S` | 130.6 | 9.7 | 220 |

### payload 分配 flag(`-I 1`,单 kernel)

| 数据区 | 块图 | 填充 p50 | 等待 p50 | 总计 p50 |
|---|---|---|---|---|
| host HtoD(对照,`src/`) | 100 `.` | — | 5.2 | 8.2 |
| flag 0 | 100 `S` | 114.8 | 13.6 | 129.0 |
| flag 1 | 100 `.` | 152.3 | 13.1 | 171.1 |
| flag 4 | 100 `.` | 146.8 | 13.7 | 165.7 |

`-I 1` 下等待从第 4~8 轮起由 5 µs 爬到 14 µs,每圈轮询 640→900 ns,
uncached 下同样出现(所以不是 L2 积压)。原因未查,`-Y` 空转对照待跑。
