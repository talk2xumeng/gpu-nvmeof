# mem_prims:访存原语和 fence 的单次代价

## 为什么测

`-X`(队列进显存)的 QD1 还是 ~18 µs。剖析里的大头不在网络，而在 GPU 自己的访存：

- 显存上每次 volatile 访问约 0.5 µs
- poller 空转一圈 1.3 µs,处理一条响应 4.1 µs
- 提交 4.8 µs(DevX 探针光写 WQE + dbrec + 门铃就要 4 µs)

和 DeepEP / mxshmem 的 IBGDA(`C:\Code\muxi`)对照，队列位置、门铃写法都已经一样了。剩下的差别在原语上：

| | DeepEP MACA 分支(`mxdeepep/csrc/kernels/utils.cuh`) | 我们 |
|---|---|---|
| 读 | `__builtin_nontemporal_load` | `volatile` |
| 写 | `__builtin_nontemporal_store` | `volatile` |
| acquire / release | `__builtin_mxc_fence(ACQUIRE/RELEASE, "device")` | `__threadfence_system()` |
| 锁 | CAS + `mxc_fence("block")` | CAS + 释放后 system fence |
| CQE 命中后 | `__threadfence_block`(mxshmem `IBGDA_MFENCE`) | `fence_q()`,默认 system |
| 敲门铃后 | 不 fence | `__threadfence_system()` |

我们之前测的"device 级 fence 更慢",测的是 `__threadfence()`,不是 `__builtin_mxc_fence(..., "device")`,这个结论不能套到 DeepEP 的写法上。

这个程序把每种写法单独量出来，用来决定 kernel 要不要换。纯 GPU 程序，不需要网卡和 target。

## 测什么

**单线程**:每项循环 n 次取平均，重复 r 次取中位数。

- 读用依赖链(下一次的地址取决于上一次读到的值),量的是时延不是吞吐。
- 写只发出不 fence 的那两项，量的是发射代价，不代表写已经到达。
- "提交 / CQE 命中 / 锁"三组是 kernel 热路径的复刻(不含门铃 MMIO),一行我们的写法、一行 DeepEP 的写法，可以直接相减。

**跨 block 乒乓**:两个 block 各一个线程，轮流写对方等的 flag,报单程时延。

- 对应 poller → submitter 交接那一段。
- 同时是可见性检查：某种写法看不见对方的写，就报"超时"。

**内存类型**(`-m`):

| | 分配方式 | 用在哪里 |
|---|---|---|
| `n` | `mcMalloc` | |
| `f` | `mcExtMallocWithFlags(1)` Finegrained | `-X` 队列、`-H` 胶囊、mxshmem 的队列和 heap |
| `u` | `mcExtMallocWithFlags(4)` MapPcieDefault | |
| `h` | 主存 `mcHostRegister(Mapped)` | 主存方案的 CQ/SQ/胶囊 |

## 跑

```sh
make mem-prims                     # GPU_ID=4,MEMS=nfh
make mem-prims MEMS=fh GPU_ID=4
./bin/mem_prims -g 4 -m nfuh -n 20000 -r 5 -P 5000
```

不需要 root。全部跑完大约几秒到十几秒，主存那一列最慢。

## 判读

先看 `f` 列，这是 `-X` 队列实际用的内存。

1. **"读 nt+mxc acquire" 对比 "读 sys fence+volatile"**:poller 每圈、`ld_acq32` 每次都付这个差价。
2. **"写 mxc release+nt" 对比 "写 volatile+sys fence"**:`st_rel32` 每次的差价。一条 I/O 的提交路径上约 8 次。
3. **"提交 [DeepEP]" 对比 "提交 [我们]"**、**"CQE 命中 [mxshmem]" 对比 "CQE 命中 [我们]"**:差值直接对应剖析里的"提交"和"处理一条响应"。
4. **乒乓 DeepEP 行**:跨 block 能看见，且比我们默认那行快，交接也可以换。
5. `h` 列对照主存方案，应该能复现"每次 ~0.7 µs"。复现不了，说明这个程序和 kernel 里的测法有出入，先查这个。

要注意:**这里只验证 GPU 内部(跨 block)的可见性**。nontemporal 读能不能看见网卡 P2P 写进显存的 CQE,得上 `tst/devx_probe` 或 `-X` 实测。如果乒乓的结论支持换，下一步就是把 devx_probe 的 CQE 轮询换成 nontemporal 跑一遍。

## 结果

### 2026-10-16,mx-decode-02,GPU 4,实测时钟 1.11 GHz

单线程，每次平均 ns:

| | n | f | h |
|---|---|---|---|
| 读 volatile / nt / plain | 196 / 176 / 201 | 358 / 358 / 358 | 1074 |
| 读 sys fence+volatile [我们] | 203 | 358 | 1089 |
| 读 nt+mxc acquire [DeepEP] | 194 | 352 | 1076 |
| 写 volatile(只发出) | 223 | **348** | 481 |
| 写 nontemporal(只发出) | 38 | **68** | 38 |
| 写 volatile+sys fence [我们 st_rel32] | 266 | 340 | 520 |
| 写 mxc release+nt [DeepEP] | 252 | 360 | 514 |
| 写 nt+mxc block fence | 48 | 68 | 48 |
| 单独一个 fence(任何级别) | 0.1 | 0.1 | 0.1 |
| 锁 CAS+释放 [我们 / DeepEP] | 517 / 302 | **4760 / 4620** | 1572 / 1489 |
| 提交 WQE+dbrec [我们 / DeepEP] | 1000 / 458 | **1503 / 702** | 2256 / 960 |
| CQE 命中后读字段 [我们 / mxshmem] | 822 / 829 | 1432 / 1396 | 4354 / 4342 |

跨 block 乒乓，单程 ns:5 种写法之间没有差别。n 约 620,f 约 910,h 约 1840–2000。
不带 fence 的写法也都看得见对方的写。

**结论**

1. **fence 级别无关紧要。** 单独一个 fence 不花钱，带 fence 的写都等于"一次写完成"的时间。
   system、device、mxc device 三者一样，和之前"0x4000000 不快"一致。
   唯一例外是 mxc `"block"` 级，它不等写完成，所以跨 block 的 release 不能用它。
2. **真正贵的是 volatile 写。** 在 MACA 上，每个 volatile 写都要等写完成
   (f 上 348 ns),连发 4 个就是 4 倍。nontemporal 写只要 68 ns 就能发出，几个可以叠在一起，
   最后一次 fence 统一等。"提交"那组差 0.8 µs(f)、1.3 µs(h),全来自这一点。
3. **读没有捷径。** f 上任何读法都是 358 ns,主存都是 1.07 µs,换原语不省。
   能省的只有读的次数，或者让几个读并发发出(`T_LD4_*` 两行待测)。
4. **finegrained 上的原子操作要 4.7 µs。** `gio_shared`、tag 现在在 `mcMalloc` 上
   (CAS 约 0.3–0.5 µs),不要挪到 finegrained。
5. **跨 block 交接约 0.6 µs**(n)。换原语没有收益。

**对 `-X` 热路径的含义**:能拿到的是"写"这一侧。改法是把 WQE、dbrec、RQ 补充、tag 等连续写
都换成 nontemporal,每组只在末尾 fence 一次。提交预计省约 1 µs。
QD1 的其余部分(网卡取 WQE / 胶囊约 5.8 µs、网络加 target 3.3 µs、串行读)换原语都省不掉。
