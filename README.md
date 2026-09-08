# gpu-nvmeof

验证 GPU 显存经 RDMA 直接访问远端 NVMe 存储,并评估进一步 bypass CPU 的两条路径。

```
远端 NVMe ── nvmf ── RDMA ──► CX-7 ──► MetaX C500 显存
                                ▲
                      ibv_reg_dmabuf_mr()
```

## 背景

推理场景下 KV cache 需要在 GPU 显存和远端存储之间搬运。传统路径是
`SSD → 主机内存 → 显存`,两跳、占 DRAM 带宽、CPU 全程参与。
目标是压到一跳,并逐步把控制面也从 host CPU 上移走。

分三个阶段:

| 阶段 | 数据面 | 控制面 | 状态 |
|---|---|---|---|
| CPU-initiated | 网卡直接 DMA 显存 | host CPU | ✅ 已验证 |
| GPU-initiated | 同上 | GPU kernel 内直发 | 🔬 可行性已确认 |
| DPU-offload | 同上 | DPU ARM 核 | ⬜ 待评估 |

## 已验证的结论

**功能**

- 沐曦显存可导出 dma_buf(`mcMemGetHandleForAddressRange`,handle type = 1)
- 普通 `mcMalloc` 的显存即可导出,不需要 VMM 三段式
- dma-buf 和 peer_mem 两条注册路径都可用,`lkey == rkey`
- 端到端数据校验 PASS:host → 显存 → 远端盘 → 显存 → host,1 MiB 逐字节一致

**性能**(CX-7 400G,SPDK null bdev 后端)

| 路径 | GB/s | Gb/s |
|---|---|---|
| perftest 主机内存 | 45.86 | 366.88 |
| perftest 显存(peer_mem) | 48.91 | 391.27 |
| **NVMe-oF → 显存(dma-buf)** | **48.86** | **390.9** |
| NVMe-oF → 显存,跨 PCIe switch | 40.55 | 324.4 |

两个值得注意的点:

1. **NVMe-oF 协议栈几乎零开销** —— 390.9 vs 裸 RDMA 391.27,差 0.1%
2. **显存路径比主机内存快 6.6%** —— 数据不经过 CPU 内存子系统,
   不占 DRAM 带宽、不污染 LLC、不受 NUMA 影响。这个反直觉的结果在
   perftest 和本工具上独立复现了两次,本身就是 P2P 生效的证据
3. **拓扑影响 17%** —— GPU 与网卡必须按 BDF 邻接关系配对

## GPU-initiated 可行性

调研结论:**硬件和底层软件无阻塞项**。

| 能力 | 现状 |
|---|---|
| GPU 写网卡 doorbell | ✅ `ibgda_ring_db` 是 `__device__` 函数 |
| QP/CQ 置于显存、device 端构造 WQE | ✅ `mxshmem_mlx_ibgda_device.h`(139 KB 实现) |
| mlx5 provider | ✅ `src/transport/ibgda/mlx5/ibgda_mlx5.cpp` |
| MMIO BAR 映射给 GPU | ✅ `mcHostRegisterIoMemory` |
| 实机验证 | ✅ DeepEP low-latency 模式已跑通 |

剩余工作是应用层拼装:device 端构造 NVMe SQE(公开标准,~100 行),
host 端做 Fabric Connect 握手(一次性控制面,不在数据路径上)。

## 目录

```
src/
  gds_nvmeof.c        CPU-initiated 功能与带宽验证
  latency_baseline.c  端到端延迟基线(含 kernel 退出/重启)
  kernels.cpp         基线用的 GPU kernel,mxcc 编译
  gpu_backend.h       MACA/CUDA 后端抽象
  dmabuf_probe.c      显存注册路径能力探测
docs/
  NVMeoF-GPU验证手册.md
scripts/
```

## 快速开始

```bash
sudo make ldconfig     # 只做一次
make check
make

# target 侧(另一台机器)
sudo ./build/bin/nvmf_tgt -m 0x3 &
scripts/setup_target.sh

# 验证
make run-verify        # 数据正确性
make run-bw            # 带宽
make run-latency       # 延迟基线
```

## 环境

- Ubuntu 22.04 / 5.15 内核
- MLNX_OFED,ConnectX-7 400G
- MetaX C500 × 8,MACA 3.8.1.3
- SPDK v24.09

## 坑

**SPDK 的 hooks 只管"怎么翻译",不管"翻译谁"**。外部内存(GPU、CXL)
必须先 `spdk_mem_register` 进入 SPDK 的内存映射表,`get_rkey` 回调才会
被触发。少了这一步现象是静默的 rkey=0,没有任何报错,很难反推。

**PD 必须建在 SPDK 自己的 verbs context 上**。自己 `ibv_open_device`
拿到的 context 和 rdma_cm 建连时创建的不是同一个,MR 和 QP 跨 context
就是 local protection error。正确做法是在 `get_ibv_pd` 里用传进来的
context 建 PD,MR 延迟到 `get_rkey` 首次调用时注册。

**RUNPATH 在 sudo 下失效**。gcc 默认生成 RUNPATH,而 setuid 场景下动态
链接器进安全模式会忽略它,同时清掉 `LD_LIBRARY_PATH`。用 ldconfig 注册
到 `/etc/ld.so.conf.d/` 是唯一干净的解法。
