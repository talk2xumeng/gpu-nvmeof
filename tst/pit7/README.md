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

## 结论

> kernel 写普通显存,**只有 host 调 `mcDeviceSynchronize()` 之后**才对网卡
> 可见。kernel 内的 fence、sleep,kernel 退出,新的 launch,都不触发刷新。

| 现象 | 证据 |
|---|---|
| 同一 kernel 内始终不可见 | R0 / R3 / W2;历史上门铃前 sleep 1 µs~1 ms 无效 |
| 每圈 `__threadfence_system()` 无效 | R3 = R0,W2 = 100 `S` |
| 写入 kernel 退出、另一个 kernel 敲门铃,仍不可见 | P3 / Q3 |
| 两个 kernel 之间插 `mcDeviceSynchronize`,可见 | P4 / Q4 全 `.` |
| 确定性,不是竞态 | 所有失败用例块图完全可预测 |
| uncached 分配(flag 1 / 4)可以绕开 | payload 100/100 正确,4 KiB 填充贵 ~35% |

块图的解读(R1/R2/R4/P3 相同):

```
....................................RRRRRRRRRRRRRRRRRRRRRRRRRRRR
RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR
```

胶囊槽位按 `i & 0x3f` 复用。第 0–63 轮首次用槽位,网卡读到初始全零,
命令非法 → 全 `R`。每轮末尾的 host 同步把本轮胶囊刷下去;第 64–99 轮
复用槽位 0–35 时,网卡读到的是这些**已刷下去的旧胶囊**,执行了"读 LBA
0–35 到块 0–35" → 块 0–35 正确。R0(单 kernel,期间无同步)是 100 `R`。

Q3H(胶囊和 payload 都在普通显存)是 36 `.` + 64 `C`:前 64 轮命令非法,
盘上保持涂色;后 36 轮执行旧胶囊,对应的旧 payload 也已刷下,所以写对了。

**读失败用例的延迟时注意**:非法命令被 target 快速拒绝,P3/Q3H 的
"等待" p50 只有 2.2 µs,不是真实 I/O。

### 代价

同步本身约 13 µs/轮:P4 host 墙钟 67 µs vs C3 54 µs;Q4 235 µs vs W1 222 µs。

### 对 Tutti 的推论

P3 否定了"SSD 取 SQE 晚于 kernel 退出"。按上述模型,Tutti 的 SQE 要到
poll 之后的同步才可见,poll 会死等 —— 而它实测可用,所以**它那条路的
内存本身不受这个缓存影响**。最可能是 snvme 对显存做 P2P pin/映射后,
这段内存进入了 uncached / PCIe 映射模式,相当于我们的 flag 4
`MapPcieDefault`。待在 Tutti 侧确认(cu-bridge 的 `cudaMalloc` 实际 flag,
或 snvme 注册前后的内存属性)。

### 生产形态的选项

1. 批量流水线:attention kernel → `mcDeviceSynchronize` → 发送 kernel。
   每批一次同步,host 每批参与一次调用,不碰数据
2. 面向网卡的 buffer 用 uncached 分配:无需同步;写贵 ~35%,**读代价
   未测**(`make pit7-bench`),attention 读 KV 对此敏感
3. 驱动组提供 kernel 内 flush 原语 —— 现在能给出明确证据:同步能刷新,
   kernel 退出 / fence / sleep 不能

## 待做

- `make pit7-bench`:uncached 的读/写代价,决定选项 1 还是 2
- Tutti 侧:确认 SQ ring 的实际分配 flag / P2P 注册后的内存属性
- `-I 1` 下等待从 5 µs 爬到 14 µs 的原因(`-Y` 空转对照)

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

### 第二轮(2026-09-16,4 KiB × 100,真盘)

| 用例 | 配置 | 块图 | 总计 p50 | 等待 p50 | host 墙钟 p50 |
|---|---|---|---|---|---|
| C3 | 主存胶囊 `-T 3` | 100 `.` | 17.8 | 12.0 | 54 |
| P3 | 普通显存胶囊,写 \| 门铃 | 36 `.` 64 `R` | 8.5 | 2.2 | 41 |
| P4 | 普通显存胶囊,写 \| 同步 \| 门铃 | 100 `.` | 19.4 | 13.5 | 67 |
| Q3 | kernel 写 payload,写 \| 门铃 | 100 `S` | 132.6 | 11.3 | 319 |
| Q4 | kernel 写 payload,写 \| 同步 \| 门铃 | 100 `.` | 132.9 | 11.9 | 235 |
| Q3H | Q3 + 普通显存胶囊 | 36 `.` 64 `C` | 125.7 | 2.2 | 214 |
