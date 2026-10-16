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

(待上机)
