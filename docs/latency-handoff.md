# QD1 时延调优：交接

接着做 GPU-initiated NVMe-oF 时延优化的人，从这里开始读。
本文只记 2026-10-16 这一轮(对照 DeepEP 之后)的结论、坑和下一步。
逐轮的原始数据在 `docs/concurrency-design.md`「访存原语和胶囊内联」一节，
微基准在 `tst/mem_prims/README.md`。

## 现状

4K 读,`-X`(DevX 队列在显存),cnode2(malloc bdev),mx-decode-02 → mx-prefill-02,p50:

| 配置 | `-O` | QD1 总计 | 单 QP QD4 |
|---|---|---|---|
| 优化前(`-X` 默认) | 0 | 19.4 µs | 183k IOPS |
| NT + 胶囊内联，独立 poller | `0x18000000` | 15.4 µs | **230k IOPS** |
| NT + 胶囊内联，串行快路径 | `0x18000160` | **14.4 µs** | 仅 QD1 |
| 参照:CPU 发起(`latency_baseline`) | — | 10.4 µs | — |

QD1 还比 CPU 路径慢 4 µs。

## 14.4 µs 花在哪

| 段 | µs | 依据 |
|---|---|---|
| 写胶囊 + 提交 | 1.9 | kernel 分段计时 |
| 门铃 → 看到 SEND 完成 | 6.0 | `-O 0x18013080` 网卡时间戳(poller 模式) |
| SEND 完成 → 响应到达 | 3.4 | 同上，网卡侧时间戳;所有配置都一样 |
| 发现响应 + 处理一条响应 | ≈3 | 处理一条响应 1.7,其余是轮询粒度 |

几条推断:

1. **"门铃 → SEND 完成"含一次网络往返**(RC 的 SEND 要等对端 ACK)。
   主存方案用 BlueFlame 时这一段只要 2.5 µs,所以往返 ≤ 2.5 µs。
   多出来的约 3.5 µs 在门铃之后、发包之前：网卡要通过 PCIe 回显存取 WQE
   (DevX 的 UAR 是 NC 类型，只敲 8 字节门铃，没有 BF)。
2. **"SEND 完成 → 响应"主要是 target 的处理**:收命令、RDMA WRITE 4K、发响应。
   两个时间戳都在我方网卡上，各自都走了一次往返，相减后往返基本抵消。GPU 侧动不了。
3. CPU 路径走同一个网络、同一个 target,所以比 CPU 慢的 4 µs 不在网络上。
4. 粗算下限：往返 ≤2.5 + target 3.4 + GPU 侧最少 1–2 ≈ **7–8 µs**。

网络往返还没单独量过，见"下一步"第 1 项。

## 已确认的结论

### MACA 访存特性(`tst/mem_prims`)

| 现象 | 数据(finegrained 显存) | 含义 |
|---|---|---|
| fence 本身不花钱 | 任何级别单独 fence ≈ 0 | 慢的是等之前的写完成;system / device 级没区别 |
| volatile 写要等完成 | 348 ns / 次 | 连写 N 个就是 N 倍 |
| nontemporal 写发出即走 | 68 ns,可叠加 | 一组写完只 fence 一次 |
| volatile 读串行 | 4 个 1380 ns | |
| nontemporal 读可并发 | 4 个 415 ns | 同一处要读的字段一起发出 |
| 单次读没有捷径 | 都是 ~350 ns(mcMalloc ~190,主存 ~1070) | 只能减少读的次数 |
| finegrained 上的原子操作 | CAS 4.7 µs | 共享状态、锁必须留在 `mcMalloc` 上 |
| 跨 block 交接 | ~0.6 µs(mcMalloc),各种写法一样 | |
| `clock64()` | 29 ns | 剖析开销可以忽略 |

`mxc_fence("block")` 不等写完成，跨 block / 给网卡的 release 不能用。

### 两个开关(代码在 `src/gpu_io_kernel.cpp`)

- **`0x8000000` NT**:热路径读写换成 nontemporal(同 DeepEP MACA 分支)。
  - 写:WQE、dbrec、RQ 补充、tag、`sq_pi`、解锁、CQ dbrec,每组末尾一次
    `mxc_fence(RELEASE,"device")`;敲门铃后不 fence。
  - 读:响应字段、RQ 影子、tag 收尾字段一起发出。
  - 效果：提交 −3 µs,处理一条响应 −3 µs。
- **`0x10000000` 胶囊内联**:胶囊在寄存器里拼进 SEND WQE。
  - WQE 96 字节，占 2 个 WQEBB,pi 每条 +2。
  - 只支持 `-X`,不配 `-G` / `-k`;跑完 dump 第一条 WQE 供核对。
  - 必须配 NT:单开时多出的 volatile 写把收益吃掉了。配 NT 后再省约 2 µs。
- 两个开关读写都验过：逐块校验 PASS,写路径另起进程回读 PASS,dbrec 核对一致。

### 结构选择

- QD1 用串行快路径(`0x160` = 0x20 自己轮询 + 0x100 精简提交 + 0x40 不起 poller block),
  比独立 poller 快约 1 µs。
- QD>1 用独立 poller。自轮询(`0x50`)QD4 只有 169k IOPS,p90 29 µs,原因是抢 `cq_lock`。

## 踩过的坑(别再踩)

1. **没有"[GPU 时钟]"那一行的输出不能比。** 那时 host 按标称 1.6 GHz 换算，实际约 1.06 GHz,
   数字小 1.5 倍。0x40 配置以前从不记 `k_end`,曾把 14.4 误记成 9.5,还据此得出错误结论。
   已修(`k_end` 由第一个干活的 block 记)。每次看结果先确认有这一行。
2. **ctx 只能以 `&ctx` 传下去，地址不能运行时可变。** 试过在 `&ctx` 和 shared memory 副本之间
   二选一，编译器就没法把 ctx 拆进寄存器，开不开那一位都全面变慢(14.4 → 17.8 µs)。已撤。
   同理，`pf = 剖析 ? lpf : NULL` 让 `lpf[8]` 落进 scratch:k_gpu_io 的
   `.private_memory_size` 是 72 B,不在热路径上。
3. **网卡时间戳 `0x80` 要关掉 `0x4000`**(SEND 选择性 signal),否则时间戳会配错对。
   写法是 `-O 0x13080` 再或上别的位。host 现在会拒绝错误组合。
4. 内联让 SQ dbrec 每条 +2,核对已按 2 倍算;以后改 WQE 大小时记得同步。
5. 改共用头文件后 `make` 全量重编(坑 4);跑 A/B 时用 `git worktree` 编旧版本对照。

## 下一步(按优先级)

1. **量网络往返**,把 14.4 µs 拆清楚(两台机器之间,`t_typical` 是往返的一半):
   ```sh
   # target 机器
   ib_send_lat -d <对端网卡> -x 3 -s 64 -n 10000
   ib_write_lat -d <对端网卡> -x 3 -s 4096 -n 10000
   # 本机
   ib_send_lat  -d mlx5_1 -x 3 -s 64   -n 10000 172.16.3.8
   ib_write_lat -d mlx5_1 -x 3 -s 4096 -n 10000 172.16.3.8
   ```
2. **门铃 → SEND 完成的 3.5 µs(最大单项)**:让 `-X` 用上 BlueFlame,网卡不用回显存取 WQE。
   - 已有 `-X -G <cycles>`:BF 类型 UAR,4 个 8 字节 BF 写之间空等。没有实测数据。
   - 之前在主存方案上发现：4 个 BF 写挨着发网卡不认，要隔开。
   - 先测(不配内联):
     ```sh
     for g in 0 500 1000 2000; do
       $GI -r 1000 -Q 1 -G $g -O 0x8013080 | grep -E "总计|门铃→发现 SEND|SEND 完成→响应"
     done
     ```
   - 对照：NT 单开时门铃 → SEND 完成 6.6 µs,主存 BF 方案 2.5 µs。
     有效的话，再让 BF 支持 96 字节内联 WQE,并处理多 QP 共用 UAR 页的问题(0026)。
3. **GPU 侧剩余约 3 µs**:
   - poller 剖析里空转一圈的"idle_flush 267 ns、done 检查 209 ns"原因不明，不是 `clock64`
     开销。可以先查 125 个寄存器有没有溢出(试 `__launch_bounds__`,看 `.private_memory_size`)。
   - 处理一条响应 1.7 µs:看能否再减少串行读。
4. **其余没测的**:多 QP + NT + 内联的吞吐(之前 16 QP 2.56M 是旧路径);`-X` 写路径基线
   (NT + 内联写 27.4 µs,没有对照);128K 带宽。
5. 定论之后，把 NT(以及 `-X` 下的内联)设为默认，删掉证明无用的诊断位，清理 `-O`。

## 命令模板

```sh
make
GI="sudo ./bin/gpu_initiated -a 172.16.3.8 -n nqn.2024-01.io.test:cnode2 -g 4 -b 4096 -X -S 7"

$GI -r 1000 -Q 1 -O 0x18000160     # QD1 最好配置
$GI -r 1000 -Q 1 -O 0x18020000     # poller 模式 + 分段剖析
$GI -r 1000 -Q 1 -O 0x18013080     # 网卡时间戳(全部 SEND 要 CQE)
$GI -r 5000 -Q 4 -O 0x18000000     # 单 QP 吞吐;IOPS = 条数 / "[GPU 时钟]" 行里的 kernel 时间
$GI -r 100 -Q 1 -W -O 0x18000000 && $GI -r 100 -Q 1 -V   # 写 + 另起进程回读
./bin/mem_prims -g 4 -m nf          # 访存原语微基准
```

同一配置至少跑 3 次看是否稳定(这一轮同配置三次在 ±0.1 µs 内)。

## 参考

- DeepEP / mxshmem IBGDA(`C:\Code\muxi`):
  - `mxdeepep/csrc/kernels/utils.cuh`:MACA 读写原语
  - `mxshmem/src/include/ep/ep_ibgda_device_mlx.h`:门铃 / 提交
  - `mxshmem/src/include/device/non_abi/transport/mxshmem_mlx_ibgda_device.h`:`ibgda_poll_cq`
- mxshmem 自带 IBGDA ping-pong:`mxshmem/perf_test/device/p2p/shmem_put_ping_pong_latency.cpp`。
  可以拿它的数作为"这台硬件上 IBGDA 本身能多快"的参照，还没跑。
- 相关代码:`src/gpu_io_kernel.cpp`(kernel)、`src/gpu_io_ctx.h`(开关定义)、
  `src/devx_qp.c`(DevX 队列、WQE dump)、`tst/mem_prims/`。
