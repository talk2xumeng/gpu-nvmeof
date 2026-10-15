# KV 测试操作手册

只写命令和判据。原理见 `tst/kv_target/README.md`,现状见 `docs/status.md`。

## 机器

| 角色 | 机器 | 在哪跑 | 仓库 | SPDK |
|---|---|---|---|---|
| initiator | mx-decode-02 | 宿主机 | `~/mx/gpu-nvmeof` | `~/mx/spdk` |
| target | mx-prefill-02,172.16.3.8 | **容器里**(root) | `/workspace/gpu-nvmeof` | `/workspace/spdk` |

target 上的 SPDK 是在容器里 configure 的,路径写死成 `/workspace/...`。所以
target 上的编译、运行都要进容器做。在宿主机上 make 会报
`/workspace/spdk/lib/env_dpdk/env.mk: No such file`。

Makefile 里 `TARGET_IP` 默认就是 172.16.3.8。下文 initiator 侧的命令都不用
再带它。

---

## 一、一次性准备

### target(容器里)

```bash
cd /workspace/gpu-nvmeof && git pull

# 1. 给 SPDK 的 nvmf 打 KV 透传补丁,然后重编 SPDK
#    脚本可以重复执行,打过的部分会跳过
tst/kv_target/apply_kv_patch.sh /workspace/spdk
cd /workspace/spdk && make -j$(nproc) && cd -

# 2. 编 kv_tgt。只编这一个,顶层的 make 会去编 GPU 程序,这台机器编不了
make kv-tgt SPDK_DIR=/workspace/spdk

# 3. 注册 DPDK 动态库路径,否则启动报 librte_eal.so.24 找不到
echo /workspace/spdk/dpdk/build/lib > /etc/ld.so.conf.d/spdk-dpdk.conf && ldconfig
```

### initiator

```bash
cd ~/mx/gpu-nvmeof && git pull
make                # gpu_initiated 等。initiator 的 SPDK 不需要新补丁
make kv-smoke       # CPU 侧冒烟程序
```

仓库更新后:

- **target**:只有 `bdev_kv_emu.c` 或 `kv_tgt.c` 变了,才需要重做第 2 步。
- **initiator**:`src/` 变了,重新 `make`。

---

## 二、每次开测:启动 target

target 上的 `kv_tgt` 和普通 `nvmf_tgt` 不能同时跑:两个都占 4420 端口,
也都用 `/var/tmp/spdk.sock`。先停掉 nvmf_tgt。

```bash
# 容器里,开一个 tmux 窗口专门跑它
cd /workspace/gpu-nvmeof
./bin/kv_tgt -m 0x3

# 另开一个窗口
SPDK_DIR=/workspace/spdk TARGET_IP=172.16.3.8 tst/kv_target/setup_kv_target.sh
```

成功时最后列出两个 subsystem:

```
  nqn.2024-01.io.test:cnode1           KVEmu0     ← KV
  nqn.2024-01.io.test:cnode2           Malloc0    ← NVM 对照
```

- setup 脚本可以重复执行,已有的对象会跳过。kv_tgt 不用为它重启。
- 想查各 opcode 的计数:
  `SPDK_DIR=/workspace/spdk tst/kv_target/setup_kv_target.sh stats`

---

## 三、功能测试(initiator)

按顺序跑。前一步不过,就不要往下跑。

```bash
make run-kv-smoke           # ① CPU 发 KV,验 target 和胶囊构造
make run-gpu-kv-retrieve    # ② GPU Retrieve
make run-gpu-kv-store       # ③ GPU Store,另起进程回读
make run-gpu-kv-exist       # ④ GPU Exist
make run-gpu-kv-delete      # ⑤ GPU Delete,另起进程确认 key 已不存在
```

| 步骤 | 通过的样子 |
|---|---|
| ① | `*** PASS ***:25 通过,0 失败` |
| ② | `NVMe 成功 100`;`DW0 ... 不等于 4096 的: 0`;`*** PASS:逐块一致 ***` |
| ③ | 第二个进程输出 `*** PASS:逐块一致 ***` |
| ④ | `NVMe 成功 100` |
| ⑤ | 第二个进程输出 `*** PASS:全部已删除 ***` |

可调参数(GPU 侧四项通用):

| 参数 | 含义 |
|---|---|
| `GPU_BS` | value 字节数,8 的倍数,最大 131072 |
| `GPU_ROUNDS` | key 个数(C2 起不再受 127 限制) |
| `GPU_QD` | 并发数 |
| `GPU_LBA` | 起始 key 序号 |

示例:

```bash
make run-gpu-kv-retrieve GPU_BS=65536 GPU_QD=4
```

---

## 四、性能:KV vs NVM 延迟对照(initiator)

```bash
make kv-vs-nvm                                        # 4K/64K/128K,各 3 次
make kv-vs-nvm SIZES="4096 16384 65536 131072" REPEAT=5
```

- 结果目录:`kv_vs_nvm_<时间>/`,里面有 `summary.md`、`results.csv` 和每次运行的日志。
- 重点看 `summary.md` 里的「KV - NVM」差值表。

---

## 五、切回普通读写测试

```bash
# target 容器里:Ctrl-C 停 kv_tgt,然后
/workspace/spdk/build/bin/nvmf_tgt -m 0x3
#   再按原来的方式配置 nvmf_tgt(后端是真盘或 null/malloc)

# initiator
make run-gpu-read-verify
make run-gpu-write-verify
```

`nvmf_tgt` 和 kv_tgt 用的是同一份打过补丁的 SPDK,这没有问题:补丁只对
kv_emu 这个 bdev 生效,对其他 bdev 的行为一字不差。

---

## 六、target 重启之后

重启会丢掉三样东西,kv_tgt 起不来或者连不上,先把这三项补上。

```bash
# 1. hugepage(容器里是 root,没有 sudo)。kv_tgt 起不来、报
#    "No free 2048 kB hugepages" 就是缺这一项
echo 16384 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages

# 2. ARP(宿主机上)。target 有多张网卡,默认 arp_ignore=0 时任何口都会
#    回答本机任意 IP 的 ARP —— 172.16.3.8 可能被 ens100np0(mlx5_2)抢答。
#    IP 照样通(ping 的 ttl 会变成 63),RoCE 建连却超时(initiator 报
#    RDMA connect error -99,kv_tgt 一条日志都没有)
sudo sysctl -w net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2
#    持久化:
echo -e "net.ipv4.conf.all.arp_ignore=1\nnet.ipv4.conf.all.arp_announce=2" | \
    sudo tee /etc/sysctl.d/90-rdma-arp.conf

# 3. 容器如果是新建的,重做一次性准备里的 ldconfig
```

然后到 initiator 上清掉旧的 ARP 表项,并确认:

```bash
ip neigh del 172.16.3.8 dev ens3np0
ping -c2 172.16.3.8               # ttl 应为 64
ip neigh show 172.16.3.8          # MAC 应为 94:6d:ae:e4:1a:9e(ens10np0)
make run-kv-smoke
```

两边的 RoCE 模式和 GID 都检查过,重启后不会变:`cma_roce_mode` 都是 v2,
`show_gids` 里有 v2 的条目。如果以后又怀疑这里,可以用下面的命令看:
`sudo cma_roce_mode -d mlx5_0 -p 1`、`show_gids mlx5_0`。

---

## 七、出问题时先查

| 现象 | 原因 / 处理 |
|---|---|
| `/workspace/spdk/lib/env_dpdk/env.mk: No such file` | 在宿主机上编的,要进容器编 |
| `librte_eal.so.24: cannot open shared object file` | 没做一次性准备的第 3 步(ldconfig) |
| setup 脚本说「kv_tgt 没在跑」,可它明明在跑 | 脚本把 rpc.py 的报错吞掉了。直接执行 `/workspace/spdk/scripts/rpc.py rpc_get_methods \| grep kv_emu` 看真实错误 |
| SPDK 的 make 在 `ctrlr_ut` / `tcp_ut` 报 `undefined reference to spdk_bdev_get_module_name` | 补丁脚本是旧版。重新跑一次 `apply_kv_patch.sh`,它会补上单元测试的桩 |
| GPU 侧报 `SC=0x01 Invalid Opcode` | 命令没到 kv_emu。要么 SPDK 打完补丁没重编,要么 target 跑的是 nvmf_tgt 而不是 kv_tgt |
| GPU 侧报 `SC=0x02` 或 `0x86` | key 长度字段不对。把输出里的 `[胶囊]` dump 拿出来看 |
| CPU 侧准备阶段出现 `CQ transport error -6` | keep-alive 超时,target 把连接断了。确认 initiator 用的是最新代码 |
| `-r` 超过 127 时报「超过挂着的 recv 数」 | 用的是 C2 之前的二进制,或带了 `-O 2048`;重新 `make` |
| `RDMA connect error -99`,ping 却能通、ttl=63 | ARP 被别的网口抢答,见第六节 |
| kv_tgt 启动报 `No free 2048 kB hugepages` | 重启后 hugepage 没了,见第六节 |
