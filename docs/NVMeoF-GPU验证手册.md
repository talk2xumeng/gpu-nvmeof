# NVMe-oF over RDMA → GPU 显存 验证手册

> **验证命题**:沐曦 GPU 的显存,能作为 RDMA MR 被远端 NVMe-oF target 正确读写,
> 数据全程不经过主机内存。
>
> 基于《NVMf-RDMA 配置指导》改写,修正了若干问题并扩展到显存路径。

---

## 0. 与原手册的差异说明

| 项 | 原手册 | 本手册 | 原因 |
|---|---|---|---|
| `addr_traddr` | `172.16.3.14` | target 自己的 IP | 原文填的是 host 侧地址,会导致监听失败 |
| NQN | `nvme-test` | `nqn.2024-01.io.test:cnode1` | 非标准格式在 SPDK 侧会有问题 |
| 端口检查 | `ss -ulpn \| grep 4420` | `dmesg` / `rdma resource show` | RDMA 不走内核 socket 栈,`ss` 看不到 |
| 后端设备 | 真实 NVMe | 一期用 ramdisk | 排除盘的瓶颈,单独看网络+显存路径 |
| Host 端 | 仅 `nvme connect` | 增加 SPDK + dma-buf | 内核态到不了显存 |

**最关键的一点**:原手册的 `nvme connect` 路径**永远到不了 GPU 显存**。
内核 block layer 只认 `struct page`,显存没有对应的 `struct page`。
它是基线测试,不是最终目标。

---

## 1. 整体路线

```
阶段 0  环境检查                     无硬件依赖
   ↓
阶段 1  target 配置 + 内核态基线      ← 原手册覆盖的范围
        nvme connect + fio
        目的:证明网络和盘本身没问题
   ↓
阶段 2  SPDK 用户态 + 主机内存        排除 SPDK 本身的问题
   ↓
阶段 3  SPDK + dma-buf → 显存         ★ 真正的验证目标
   ↓
阶段 4  确认真的走了 P2P              带宽好看 ≠ 走了直路
   ↓
阶段 5  性能矩阵 + 稳定性
```

**各阶段 target 配置完全相同,不用重配。**

---

## 2. 阶段 0:环境检查

### 2.1 dma-buf 能力(硬门槛)

```bash
uname -r                                    # 需要 >= 5.12
nm -D $(ldconfig -p | grep libibverbs.so | head -1 | awk '{print $NF}') \
   | grep ibv_reg_dmabuf_mr                 # 需要有这个符号
grep ibv_reg_dmabuf_mr /usr/include/infiniband/verbs.h
ls /sys/kernel/mm/memory_peers/ 2>/dev/null # 空 = 无 peer_mem,只能走 dma-buf
```

三项通过才有必要往下走。

### 2.2 RDMA 链路

```bash
rdma dev show
ibv_devinfo -d mlx5_0            # 确认 PORT_ACTIVE
ibstat | grep -E "State|Rate"
rdma link show
```

### 2.3 GID 选择(RoCEv2 必须选对)

```bash
rdma -dd show gid                # 新版
show_gids                        # 旧版
```

输出示例:
```
DEV     PORT  INDEX  GID                                      IPv4          VER
mlx5_0  1     0      fe80::966d:aeff:fee4:1a9e                              v1
mlx5_0  1     2      ::ffff:ac10:0308                         172.16.3.8    v1
mlx5_0  1     3      ::ffff:ac10:0308                         172.16.3.8    v2   ← 用这个
```

**RoCEv2 必须选 `VER=v2` 且带 IPv4 的那一条。**
原手册示例里只有 v1,如果实际环境也只有 v1,说明网卡配成了 RoCEv1,
需要检查 `/sys/class/infiniband/mlx5_0/ports/1/gid_attrs/types/`。

### 2.4 GPU 与网卡拓扑

```bash
lspci -D | grep -iE "mellanox|display|processing accelerator"
lspci -tv | grep -E "Mellanox|Display"
```

记录 GPU 和网卡的 BDF,确认是否在同一 PCIe switch 下或同一 NBIO。
**跨 socket 的组合只测功能,不测性能。**

### 2.5 ACS(P2P 的前提)

```bash
sudo lspci -vvv | grep -A2 "Access Control Services" | grep ACSCtl
```

`SrcValid+` 表示 ACS 开着,P2P 会被强制上翻到 root complex。关掉:

```bash
for d in $(lspci -D | awk '{print $1}'); do
    sudo setpci -s $d ECAP_ACS+0x6.w=0000 2>/dev/null
done
```

> 重启后失效,需要写进启动脚本。

### 2.6 OFED 与沐曦驱动共存

原手册第 2 章的 `sed` 操作说明 **MOFED 与 maca 有包依赖**。
安装 OFED 时:

```bash
sudo rmmod metax                 # 先卸载 maca KMD
# ... 执行原手册的 sed 解耦操作 ...
./mlnxofedinstall --add-kernel-support --with-nvmf --with-nvmf_host --dkms
sudo /etc/init.d/openibd restart
```

装完必须两边都在:

```bash
lsmod | grep -E "metax|mlx5"
ibv_devinfo | head
mx-smi                           # 或 nvidia-smi
```

**后续任何 OFED 升级都要重复这套解耦步骤**,否则会把 GPU 驱动带走。

---

## 3. 阶段 1:Target 配置

### 3.1 后端设备选择

**一期强烈建议用 ramdisk**,把盘的性能瓶颈完全排除:

```bash
sudo modprobe brd rd_size=16777216 max_part=0 rd_nr=1    # 16 GiB
ls -l /dev/ram0
```

真盘验证放到阶段 5。单盘 7 GB/s 打不满 200G,要凑并发反而引入变量。

### 3.2 配置 nvmet

```bash
modprobe nvmet
modprobe nvmet-rdma

# ---- subsystem ----
NQN=nqn.2024-01.io.test:cnode1
mkdir -p /sys/kernel/config/nvmet/subsystems/$NQN
cd /sys/kernel/config/nvmet/subsystems/$NQN
echo 1 > attr_allow_any_host

mkdir -p namespaces/1
cd namespaces/1
echo -n /dev/ram0 > device_path        # 一期用 ramdisk
echo 1 > enable

# ---- port ----
mkdir -p /sys/kernel/config/nvmet/ports/1
cd /sys/kernel/config/nvmet/ports/1
echo 172.16.3.8 > addr_traddr          # ★ target 自己网卡的 IP
echo rdma       > addr_trtype
echo 4420       > addr_trsvcid
echo ipv4       > addr_adrfam

ln -s /sys/kernel/config/nvmet/subsystems/$NQN \
      /sys/kernel/config/nvmet/ports/1/subsystems/$NQN
```

> ★ `addr_traddr` 填 **target 本机**的 IP。原手册此处误填了 host 侧地址。

### 3.3 验证 target 就绪

```bash
cat /sys/kernel/config/nvmet/ports/1/addr_traddr
dmesg | grep -i nvmet | tail
```

期望看到 `nvmet_rdma: enabling port 1 (172.16.3.8:4420)`。

```bash
rdma resource show cm_id          # 有 host 连上后能看到
```

> **不要用 `ss -ulpn | grep 4420`。** RDMA 连接由网卡硬件处理,
> 不经过内核 socket 栈,`ss` 看不到任何东西,容易误判成 target 没起来。

### 3.4 重置(改配置时用)

顺序不能错:

```bash
# ① host 端先断开
nvme disconnect -n $NQN

# ② 解除 port 与 subsystem 的链接
rm /sys/kernel/config/nvmet/ports/1/subsystems/$NQN

# ③ 再改参数
```

---

## 4. 阶段 1(续):内核态基线

这一步的目的是**证明网络和盘没问题**,把变量降到最少。

```bash
modprobe nvme-rdma

nvme discover -t rdma -a 172.16.3.8 -s 4420

nvme connect -t rdma -n nqn.2024-01.io.test:cnode1 \
             -a 172.16.3.8 -s 4420 \
             --host-traddr 172.16.3.14 \
             -i 8                        # 队列数,默认太少打不满

nvme list
nvme list-subsys                         # 确认状态是 live
```

### 4.1 基线性能

```bash
# 大块顺序读 —— 看带宽上限
fio --name=bw --filename=/dev/nvme1n1 \
    --rw=read --bs=1M --iodepth=32 --numjobs=4 \
    --ioengine=libaio --direct=1 --runtime=60 --time_based

# 小块随机读 —— 看 IOPS 和延迟
fio --name=iops --filename=/dev/nvme1n1 \
    --rw=randread --bs=4k --iodepth=32 --numjobs=4 \
    --ioengine=libaio --direct=1 --runtime=60 --time_based
```

> 设备名以 `nvme list` 实际输出为准,**别照抄 nvme0n1**,
> 那通常是本地盘,测错对象。

**记录这组数据作为基线。** 后面显存路径的带宽要和它对比。

### 4.2 进入阶段 2 前必须断开

```bash
nvme disconnect -n nqn.2024-01.io.test:cnode1
```

内核 nvme-rdma 和 SPDK 同时连一个 subsystem 会有资源冲突。

---

## 5. 阶段 2:SPDK + 主机内存

先不上显存,排除 SPDK 本身的问题。

```bash
./gds_nvmeof -a 172.16.3.8 -n nqn.2024-01.io.test:cnode1 \
             -d mlx5_0 -H -b 1048576 -q 32 -c 8192
```

`-H` 是主机内存对照组。带宽应该和阶段 1 的 fio 基线接近
(SPDK 通常还略高,因为省了内核开销)。

这一步跑通说明:SPDK 编译正确、hooks 装好了、target 连接正常。

---

## 6. 阶段 3:SPDK + dma-buf → 显存 ★

### 6.1 先做数据校验,不看带宽

```bash
./gds_nvmeof -a 172.16.3.8 -n nqn.2024-01.io.test:cnode1 \
             -d mlx5_0 -g 0 -V -c 64
```

校验流程:

```
① host 生成 pattern
      ↓ memcpy_htod
② 显存持有 pattern
      ↓ NVMe Write   —— 网卡【读】显存,被测路径
③ 远端盘存下 pattern
      ↓
④ 显存 memset 成 0xAA  —— 关键:把证据擦掉
      ↓ NVMe Read    —— 网卡【写】显存,被测路径
⑤ 显存应重新变回 pattern
      ↓ memcpy_dtoh
⑥ 比对 ① 和 ⑤
```

> **第 ④ 步是设计要点**。不擦的话,显存里本来就有 pattern,
> 比对"一致"证明不了数据真的从盘回来了。

**读回全是 0xAA** = RDMA 没报错但数据没落到显存
→ 查 `iova` 参数和 ACS。

### 6.2 两个方向都要测

| 操作 | 数据流 | 网卡对显存 | 业务对应 |
|---|---|---|---|
| NVMe Read | 远端盘 → 显存 | **写** | ★ 主场景:权重/KV cache 加载 |
| NVMe Write | 显存 → 远端盘 | **读** | KV cache 落盘 |

```bash
./gds_nvmeof ... -b 1048576 -q 32 -c 8192        # 读(默认)
./gds_nvmeof ... -b 1048576 -q 32 -c 8192 -w     # 写
```

> ⚠️ 写测试会从 LBA 0 开始覆盖 target 盘。**确保是 ramdisk 或空盘。**

### 6.3 机制说明

NVMe-oF over RDMA 里,**发起 RDMA 操作的是 target 侧网卡,不是 initiator**:

1. initiator 发 NVMe 命令,SGL 里带 `{显存地址, rkey, 长度}`
2. target 侧网卡据此发起 RDMA Read/Write
3. 本地网卡响应,对显存做 DMA

这解释了为什么 MR 权限必须带 `REMOTE_READ | REMOTE_WRITE`,
也解释了为什么 **target 完全不需要知道那是显存**。

---

## 7. 阶段 4:确认真的走了 P2P

**带宽好看不等于走了直路。** 静默 fallback 到 bounce buffer 是最常见的坑。
三个交叉验证,至少做两个:

### 7.1 主机内存带宽(最直接)

```bash
pcm-memory 1 &
./gds_nvmeof ... -b 1048576 -q 32 -c 8192
```

真 P2P 时 DRAM 流量应接近零。
如果 DRAM 带宽随存储带宽一比一上涨,数据在主机内存里中转了。

### 7.2 CPU 占用对照

```bash
mpstat 1 &
./gds_nvmeof ... -H      # 主机内存,CPU 明显忙
./gds_nvmeof ...         # 显存,CPU 应基本闲
```

### 7.3 PCIe switch 端口计数

GPU 和网卡挂在可管理 switch(如 PEX890xx)下时,直接读端口间流量计数。
**这个证据最硬。**

---

## 8. 阶段 5:性能矩阵

| 变量 | 取值 |
|---|---|
| block size | 4K / 128K / 1M / 4M |
| queue depth | 8 / 32 / 128 |
| 方向 | read / write |
| 并发 GPU | 1 / 2 / 4 |
| NIC-GPU 拓扑 | 同 switch / 同 die / 跨 die / 跨 socket |
| payload | 显存 / 主机内存(`-H`) |
| 后端 | ramdisk / 真实 NVMe |

**重点关注**:多卡并发时是否撞上 NBIO 天花板。
CPU 路径与 GDR 路径的上限差异,是本次验证最有价值的一组数据。

### 8.1 稳定性

```bash
# 24h 持续
./gds_nvmeof ... -c 100000000

# 监控
watch -n5 'cat /sys/class/infiniband/mlx5_0/ports/1/counters/*'
```

关注 RDMA 重传计数、CQE error、port_xmit_discards。

### 8.2 故障恢复

- target 侧 `rmmod nvmet-rdma` 后重新加载,看 initiator 行为
- 拔插光纤
- 混合负载:存储 I/O + MCCL 通信同跑,看相互干扰

---

## 9. 故障排查

| 现象 | 排查方向 |
|---|---|
| `nvme discover` 超时 | `addr_traddr` 填错(原手册的坑)、链路非 ACTIVE、GID 选了 v1 |
| `ibv_reg_dmabuf_mr` EOPNOTSUPP | rdma-core 或内核太老 |
| `ibv_reg_dmabuf_mr` EINVAL | GPU 驱动导出的 dma_buf 不支持 P2P,或 offset/length 未对齐 |
| 校验读回全 0xAA | DMA 没落到显存,查 iova 和 ACS |
| `transport RDMA not found` | Makefile 的 `--whole-archive` 掉了 |
| verbs context 不匹配 | target IP 配在了别的网卡上 |
| CQE remote access error | rkey 错,或 MR 权限位不全 |
| 带宽只有一半 | 队列数(`-i`)、RX ring size、跨 die |
| `ss` 看不到 4420 | 正常现象,RDMA 不走 socket 栈 |
| OFED 装完 GPU 驱动没了 | 漏了 `sed` 解耦步骤,重装 maca |

---

## 10. 待驱动组确认

1. `mcMemGetHandleForAddressRange` 等价接口是否存在?枚举值名称?
2. 普通 device malloc 能否直接导出 dma_buf,还是必须走 VMM 三段式?
3. 导出的 dma_buf 是否支持 partial mapping(offset + len)?
4. MACA 兼容层下 perftest 的 `--use_cuda_dmabuf` 能否直接工作?
5. 内核态 fs shim(cuFile 等价物)的规划?
   —— 决定内核态 GDS 路线能否走,与本手册的 SPDK 路线无关

---

## 附:快速自检清单

```
□ 内核 >= 5.12
□ libibverbs 有 ibv_reg_dmabuf_mr
□ RDMA 链路 ACTIVE,GID 选了 v2
□ ACS 已关闭
□ GPU 与网卡同 switch / 同 NBIO
□ MOFED 与 maca 驱动共存正常
□ target addr_traddr = target 自己的 IP
□ NQN 用标准格式
□ 后端用 ramdisk(一期)
□ 内核态基线已跑通并记录
□ 进 SPDK 前已 nvme disconnect
□ 显存路径先跑 -V 校验再测带宽
□ 已用至少两种手段确认 P2P
```
