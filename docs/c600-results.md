# C600 实测:GPU-initiated NVMe-oF 方案与时延

2026-10-28,在两台 C600 机器上从零部署,验证 GPU-initiated 方案能否跑通,
量时延,顺带测 IOPS / 带宽。CPU-initiated 路径同机对照。

## 结论

- **方案在 C600 上成立。** GPU kernel 自己构造 NVMe 胶囊、敲门铃、收完成,
  数据经 RDMA 直接进出显存。读、写(另起进程回读)、多 QP 的逐块校验全部 PASS,
  所有扫描点 0 错误。
- **时延:4K 读 QD1 p50 14.6 µs**(`-X -O 0x18000160`)。同链路 CPU 直接发 I/O 是 8.2 µs,
  GPU 发起慢约 6 µs;但如果把"kernel 退出 → CPU 发 I/O → 再起 kernel"算进来,
  CPU 路径要 80 µs,GPU 发起省掉约 65 µs。
- **吞吐:4K 读最高约 80 万 IOPS**(4 QP × QD16),**128K 读最高 20.9 GB/s**。
  单 QP 下 GPU 发起与 CPU 发起的 128K 读带宽持平(9.6 vs 9.9 GB/s)。
- 和老机器(C500,mx-decode-02 → mx-prefill-02)的数基本一致:QD1 14.6 vs 14.4 µs,
  单 QP QD4 207k vs 230k IOPS。时延调优的结论可以直接沿用。

---

## 一、环境

| 角色 | 机器 | GPU | 网卡 |
|---|---|---|---|
| initiator | 10.0.82.30 | GPU#2(`26:00.0`) | mlx5_2 / ens46np0,RoCE 400G,10.190.0.30 |
| target | 10.0.82.31 | — | mlx5_2,10.190.0.31 |

- MetaX C600 × 16,MACA 3.8.3,GPU 架构 `xcore1502`
- Ubuntu 22.04 / 5.15,宿主机 MLNX rdma-core 58,**无 IOMMU**
- SPDK v24.09,target 为 `nvmf_tgt -m 0xFF`(8 核,NUMA0),后端 4 GiB malloc bdev
- 两边都在特权容器 `m01499-mega` 里以 root 跑(宿主机无 sudo)

**拓扑**:GPU#2 与 mlx5_2 之间是 PHB(跨 CPU host bridge),不是最优配对。
同 PCIe switch 的 mlx5_1 是 IB 模式、没有 IPv4,RDMA CM 用不了,所以这次没用它。
带宽数字因此偏保守。

---

## 二、功能验证

| 项目 | 结果 |
|---|---|
| CPU-initiated 读写显存(`gds_nvmeof -V`) | PASS,1 MiB 逐字节一致 |
| GPU-initiated 读,4K × 100 | PASS |
| GPU-initiated 写 4K × 200,另起进程回读 | PASS |
| GPU-initiated 读,4 QP × QD16,4K × 2000 | PASS |
| GPU-initiated 读,4 QP × QD8,128K × 500 | PASS |
| QD 扫描 64 个点(下文) | 读路径每点都逐块校验,全部 PASS;NVMe 出错 0 |
| 队列在显存(`-X`) | 可用,DevX umem 走 dma-buf |

---

## 三、时延(QD1,p50,µs)

GPU 侧是 kernel 内 `clock64` 量的单条总计;CPU 侧是 `latency_baseline -k 0`
(纯 I/O,不跑 kernel)。各 5000 条。

| | 4K 读 | 4K 写 | 128K 读 | 128K 写 |
|---|---|---|---|---|
| CPU 发起,纯 I/O | **8.2** | 9.1 | 23.6 | 35.2 |
| GPU 发起,`-X -O 0x18000160`(QD1 最佳) | **14.6** | — | — | — |
| GPU 发起,`-X -O 0x18000000` | 15.3 | 20.9 | 31.1 | 43.0 |
| GPU 发起,`-X`,`-O 0`(未优化) | 18.8 | — | — | — |
| GPU 发起,队列在主存,`-O 0` | 22.6 | 28.7 | 36.8 | 48.8 |

QD1 最佳配置跑了 3 次:14.7 / 14.6 / 14.7,p99 15.3–15.5。

### 放到实际场景里看

GPU 发起要省掉的是 kernel 的启停。`latency_baseline` 开 kernel 的模式模拟
"计算 kernel → 退出 → CPU 发 I/O → 起消费 kernel":

| 4K 读 | p50 µs |
|---|---|
| 计算 kernel | 28.4 |
| 提交 | 0.6 |
| 网络+传输 | 8.2 |
| 消费 kernel | 42.1 |
| **总计** | **80.1** |

GPU 在 kernel 内发起同一次读是 14.6 µs,**省约 65 µs**。128K 读同理:CPU 路径 98.9 µs,
GPU 发起 31.1 µs。

### 单条 I/O 比 CPU 慢的那 6 µs

和老机器(慢 4 µs)是同一个问题,`docs/latency-handoff.md` 里已经拆过:主要是
门铃之后网卡回显存取 WQE、GPU 侧串行读写。新机器 CPU 路径更快
(8.2 vs 10.4 µs),GPU 路径几乎没变,所以差距显得更大。

---

## 四、吞吐(`-X -O 0x18000000`)

IOPS = 条数 / kernel 时长,不含初始化和 launch。时延列是 kernel 内 p50。

### 4K 读

| QP × QD | IOPS | MB/s | 时延 p50 µs |
|---|---|---|---|
| 1 × 1 | 58,542 | 240 | 15.3 |
| 1 × 4 | 206,919 | 848 | 17.3 |
| 1 × 16 | 318,979 | 1,307 | 47.8 |
| 1 × 64 | 319,428 | 1,308 | 197.4 |
| 4 × 1 | 208,899 | 856 | 17.0 |
| 4 × 4 | 592,909 | 2,429 | 24.0 |
| **4 × 16** | **798,850** | 3,272 | 74.7 |
| 4 × 64 | 775,314 | 3,176 | 320.3 |

### 128K 读

| QP × QD | IOPS | GB/s | 时延 p50 µs |
|---|---|---|---|
| 1 × 1 | 30,231 | 3.96 | 31.1 |
| 1 × 4 | 63,902 | 8.38 | 64.8 |
| 1 × 16 | 72,690 | 9.53 | 211.7 |
| 1 × 32 | 73,217 | 9.60 | 423.8 |
| 4 × 1 | 101,817 | 13.35 | 36.7 |
| 4 × 4 | 149,354 | 19.58 | 98.5 |
| **4 × 16** | 159,681 | **20.93** | 381.0 |
| 4 × 32 | 95,465 | 12.51 | 2,616.5 |

### 写

| 大小 | QP × QD | IOPS | 带宽 | 时延 p50 µs |
|---|---|---|---|---|
| 4K | 1 × 1 | 43,694 | 179 MB/s | 20.9 |
| 4K | 1 × 16 | 318,026 | 1.30 GB/s | 47.8 |
| 4K | **4 × 16** | **777,726** | 3.19 GB/s | 78.6 |
| 128K | 1 × 1 | 22,304 | 2.92 GB/s | 43.0 |
| 128K | 1 × 32 | 49,355 | 6.47 GB/s | 643.4 |
| 128K | **4 × 16** | 109,063 | **14.30 GB/s** | 583.0 |

写路径只看 NVMe 状态,回读校验单独做过(见第二节)。

### 对照与上限

| 路径 | 128K 读 | 128K 写 |
|---|---|---|
| GPU 发起,1 QP,QD32 | 9.60 GB/s | 6.47 GB/s |
| CPU 发起(`gds_nvmeof`,1 qpair,QD64) | 9.92 GB/s | 6.92 GB/s |

单 QP 两者持平,瓶颈在 target 和链路,不在发起方。
链路上限参考:CPU 发起、1 MiB、null bdev(target 不做 memcpy)读 28.1 GB/s,
显存与主存同为 28 GB/s。

### 读数时注意

- **单 QP 约 32 万 IOPS 封顶**,QD 从 16 加到 64 不涨,只涨时延。要更高得加 QP。
- **4 QP × QD32 的 128K 读反而掉到 12.5 GB/s**,p50 2.6 ms。只跑了一次,原因没查,
  怀疑 target 8 核 + malloc memcpy 被压满后排队。需要更大 QD 时先复测这一点。
- 后端是 malloc,target 每条 I/O 都要做 memcpy;换真盘或 null bdev 数字会不同。
- 未优化配置(队列在主存、`-O 0`)的吞吐差很多:4K 读单 QP 只到 8.5 万 IOPS,
  4 QP 17 万。原始数据在 `sweep_1028_1818/`。

---

## 五、部署要点(和老机器不同的地方)

按踩到的顺序列。每一条不处理都跑不起来。

1. **SPDK 注册显存段错误。** 没有 IOMMU 时 `spdk_mem_register` 会用 CPU 去读
   这段地址,C600 + MACA 3.8.3 上 GPU VA 对 CPU 不可见,直接 SIGSEGV。
   打 `scripts/apply_vtophys_patch.py` 后重编 SPDK。老机器上不需要。
2. **容器里的 rdma-core 太旧。** 镜像自带 39,`mlx5dv_devx_umem_in` 没有 `dmabuf_fd`,
   `gpu_initiated` 编不过。把宿主机 MLNX rdma-core 58 的 `include/` 和
   `libibverbs/libmlx5/librdmacm` 拷到一个目录,用它编译和运行:

   ```bash
   O=~/mx/ofed58
   make SPDK_DIR=~/mx/spdk CC="gcc -I$O/include -L$O/lib -Wl,-rpath,$O/lib" \
        MXCC="/opt/maca/mxgpu_llvm/bin/mxcc --offload-arch=xcore1502"
   export LD_LIBRARY_PATH=$O/lib
   ```

   拷的时候 `libmlx5-rdmav57.so` 是指向 `../libmlx5.so.1.25.58.0` 的相对链接,
   要改成指向同目录,否则没有直接链 `-lmlx5` 的程序(`latency_baseline`)找不到网卡。
3. **GPU kernel 要按 C600 编。** 不加 `--offload-arch=xcore1502` 报
   `mcErrorNoKernelImageForDevice`。
4. **hugepage 要分在 node0。** 只写 `nr_hugepages` 会均分到 8 个 NUMA 节点,
   node0 只有 1 GiB,malloc bdev 建不出来:

   ```bash
   echo 4096 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
   ```

   容器里还要自己 `mount -t hugetlbfs nodev /dev/hugepages`。
5. **停 target 要按 PID。** SPDK 把进程名改成 `reactor_0`,`pkill nvmf_tgt` 找不到它,
   新起的 target 会因抢不到核 0 退出。
6. 无外网时 SPDK 子模块(dpdk、isa-l、isa-l-crypto)要在有网的机器上拉好再传。
   从 Windows 打包会丢可执行位,按 `git ls-files -s` 里的 100755 恢复。

### 复现命令(initiator,容器内)

```bash
GI="./bin/gpu_initiated -a 10.190.0.31 -n nqn.2024-01.io.test:cnode1 -g 2 -b 4096 -X -S 7"
$GI -r 5000 -Q 1 -O 0x18000160                    # QD1 最佳时延
$GI -r 5000 -Q 4 -O 0x18000000                    # 单 QP 吞吐
$GI -r 200 -Q 4 -L 2000000 -W && $GI -r 200 -Q 4 -L 2000000 -V   # 写 + 另起进程回读

# 扫描(容器里没有 sudo,PATH 里放一个 exec "$@" 的 sudo 垫片)
TARGET_IP=10.190.0.31 NQN=nqn.2024-01.io.test:cnode1 GPU_ID=2 \
OPTS=0x18000000 NQPS="1 4" SIZES=4096 QDS="1 4 16 64" EXTRA="-X" scripts/qd_sweep.sh

# CPU 对照
./bin/latency_baseline -a 10.190.0.31 -n nqn.2024-01.io.test:cnode1 -g 2 -b 4096 -r 5000 -k 0
```

原始日志在 10.0.82.30 的 `~/mx/sweepX_1028_1821/`(优化配置)和
`~/mx/sweep_1028_1818/`(未优化配置)。

---

## 六、没做完的

- **metax_rdma(GPU 片上网口)。** `metax_rdma_N` ↔ GPU#N ↔ `metax_eth_N` ↔ 11.22.(100+N).x。
  宿主机上(普通用户)provider `libmrdma-rdmav57.so` 能加载,uverbs 也能打开,
  但 `ibv_devinfo` 报 Failed to open device;容器里没有这个 provider。还没查。
- **最优拓扑。** GPU 和网卡同 switch(PXB)的配对要么是 IB 口没 IP,要么要换 GPU;
  给 IB 口配 IPoIB 地址后可以再测一次带宽。
- **4 QP × QD32 掉速**,见第四节。
- KV 命令集(`tst/kv_target`)这次没测。
