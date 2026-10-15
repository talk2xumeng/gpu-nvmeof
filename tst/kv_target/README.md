# tst/kv_target —— 不用 KV 盘也能测 GPU 发 KV 命令

> 只想照着命令跑:见 `docs/kv-runbook.md`。本文讲原理和验证记录。

手头的 NVMe 盘只支持 NVM 命令集,不支持 KV(CSI 01h)。这里提供一个
**target 侧的模拟**,让 GPU 侧的 KV 路径能在现有 RDMA 链路上跑通。

- GPU 侧代码在 `src/`,KV 支持属于主线:
  - `wqe_build.h` 的 `nvme_build_kv_sqe` 负责构造命令
  - `gpu_io_kernel.cpp` 负责发送命令,并回收 CQE DW0
  - `gpu_initiated -k` 是对应的入口
- 本目录只放测试设施,包括 target 和 CPU 侧冒烟测试。

规范依据:NVM Express Key Value Command Set Specification 1.3(2025-07)。

## 为什么不能直接用现有 target

KV 命令的外形和 NVM 读写完全一样(64 字节 SQE + keyed SGL),传输层不需要
改。卡住的地方在 target 的命令分发:

| | NVM | KV |
|---|---|---|
| 01h | Write | **Store** |
| 02h | Read | **Retrieve** |
| CDW10/11 | SLBA | value 长度 / key 长度 + 选项 |
| CDW2/3, 14/15 | 保留 | key |

SPDK 的 `nvmf_ctrlr_process_io_cmd` 先按 opcode 分发,01h/02h 会进
`nvmf_bdev_ctrlr_write_cmd/read_cmd`,并把 CDW10/11 当 SLBA 解析,命令根本
到不了 bdev。只有它不认识的 opcode(Delete/Exist/List)才会走 NVMe 透传。

所以需要两处配合:

1. **`apply_kv_patch.sh`**:改 SPDK 的 `lib/nvmf/ctrlr.c`。后端 bdev 的模块名
   是 `kv_emu` 时,所有 I/O 命令原样透传。补丁只靠模块名判断,其它 bdev 的
   行为不受影响,普通 `nvmf_tgt` 照常可用。
2. **`bdev_kv_emu.c`**:一个内存 KV bdev。透传过来的完整 SQE 交给它,按
   KV 1.3 的语义执行。

`kv_tgt` 就是 SPDK 的 `nvmf_tgt` 外加这个 bdev 模块。

## 用法

target 机器(mx-decode-03)上做一次准备:

```bash
cd tst/kv_target
./apply_kv_patch.sh /home/metaxadmin/mx/spdk
cd /home/metaxadmin/mx/spdk && make -j$(nproc)
cd -  && make                      # 或在仓库根目录 make kv-tgt
```

之后每次使用。先停掉普通的 `nvmf_tgt`,因为两者会抢同一个 RPC socket 和 4420 端口:

```bash
sudo ../../bin/kv_tgt -m 0x3 &
./setup_kv_target.sh               # 建 KVEmu0 + subsystem + RDMA listener
./setup_kv_target.sh stats         # 各 opcode 计数 / 出错数 / key 数
```

initiator 机器(mx-decode-02)上:

```bash
make run-kv-smoke                  # ① CPU 路径冒烟,不碰 GPU
make run-gpu-kv-retrieve           # ② GPU Retrieve:逐块校验 + DW0 = value 长度
make run-gpu-kv-store              # ③ GPU Store,另起进程回读
make run-gpu-kv-exist              # ④ GPU Exist
make run-gpu-kv-delete             # ⑤ GPU Delete,另起进程 -V -D 确认全部不存在
```

建议按顺序跑:

- **先跑 ①**。`kv_smoke` 用的胶囊构造函数和 kernel 是同一个
  `nvme_build_kv_sqe`,SPDK 只覆盖 cid 和 dptr。①全过,就说明 target 和胶囊
  构造都没问题;之后 GPU 侧再出错,只可能出在 kernel 或队列那一层。
- **再跑 ②**。它和现有的 `run-gpu-read-verify` 同形,只是把命令换成了 KV。

参数沿用读写模式的含义:`GPU_BS` 是 value 长度,`GPU_ROUNDS` 是 key 个数,
`GPU_QD` 是并发数,`GPU_LBA` 是起始 key 序号。

## KV vs NVM 延迟对照

`setup_kv_target.sh` 默认在同一个 kv_tgt 上建两个 subsystem:

- cnode1:KV(kv_emu)
- cnode2:NVM(malloc)

`kv_vs_nvm.sh` 在 QD 1 下把两边各跑一遍,对比:

- Read vs Retrieve
- Write vs Store
- Exist(不带数据)作为往返基线

同一个 target 进程、同一条链路、同一个 kernel,只换命令集,所以差值就是
KV 自己的代价。C1 那个小 I/O 回退两边都有,相减时就抵消了。

```bash
make kv-vs-nvm                                    # 默认 4K/64K/128K,各 3 次
make kv-vs-nvm SIZES="4096 16384 65536 131072" REPEAT=5
```

输出写到 `kv_vs_nvm_<时间>/`:

- 每次运行的完整日志
- `results.csv`:逐次的 p50
- `summary.md`:中位数表和差值表

NVMe 没有全部成功,或者校验 FAIL 的运行,不计入统计,并在最后单独列出。

`EXTRA` 会原样加到每条命令后面,比如 `EXTRA="-O 128"` 会打出 CQE 硬件时间戳。

## kv_emu 实现了什么

| 命令 | 行为 |
|---|---|
| Store 01h | 支持 VS=0;支持 SINKE(只插入,已存在回 89h)和 SIKE(只更新,不存在回 87h);超过 `max_value` 回 85h,超过容量回 81h |
| Retrieve 02h | 只返回 min(value, HBS) 字节,SGL 剩余部分清零;**DW0 永远是 value 全长** |
| List 06h | 返回 NRK + {KL, key, pad 到 4B};顺序在无增删时稳定;起始 key 存在时它排第一 |
| Delete 10h | 删除不存在的 key:默认成功;`KV_EDNEK=1` 时回 87h |
| Exist 14h | 存在回 0,不存在回 87h |

共同规则:

- KL > 16 回 Invalid Field(02h),KL = 0 回 86h。
- 长度不同的 key 视为不同的 key。
- Store 和 Delete 对单个 pair 是原子的。

不做的部分:

- 压缩(NOCOMP/RRD 被忽略)。
- Key Per I/O(CDW13)。
- Identify 里的 KV 数据结构:nvmf 仍按 NVM namespace 报 Identify。GPU 路径
  不读 Identify,所以不受影响;但 Linux 内核 nvme 驱动连上来时会把它当
  块设备,别对它发块 I/O。
- 持久化:进程退出,数据就没了。

value 大小受传输层 `max_io_size` 限制。`setup_kv_target.sh` 与
`setup_target.sh` 一致,上限是 128 KiB。需要更大的 value(比如 MB 级的
KV block)时,同时调大 `nvmf_create_transport -i` 和 `KV_MAXVAL`。

## 验证记录

### 2026-10-15,构建环境(无 RDMA 网卡)

- 环境:SPDK v24.09,打补丁后构建,NVMe/TCP 回环。
- 结果:`kv_smoke` 全过(25/25)。
- 覆盖的配置:3×16 B、16×4 KiB、64×64 KiB、200×8 KiB、500×4 KiB。
- 覆盖的检查项:
  - 基本往返和逐块校验
  - 截断 Retrieve
  - SINKE / SIKE
  - KL=0 / KL=17,以及前缀相同但长度不同的 key
  - 零长度 value
  - List 分页遍历,无漏、无重复
  - Delete 后 Exist

TCP 和 RDMA 走的是同一段 `nvmf_ctrlr_process_io_cmd`,补丁和 bdev 与传输层
无关。没有验证过的只有 GPU kernel 发出的 keyed SGL 那一段,而那段与现有读写
路径完全相同,已在硬件上验证过。

### 2026-10-15,上机(GPU 路径)

- initiator:mx-decode-02,GPU 4,mlx5_1
- target:mx-prefill-02(172.16.3.8),在容器里跑 kv_tgt,后端是 KVEmu0
- 胶囊放在主存;响应 100/100 都内联在 CQE 里,cid 异常为 0,wqe_counter 连续

| 测试 | 结果 | 等待 p50 | 总计 p50 |
|---|---|---|---|
| Retrieve 4 KiB × 100,QD 1 | 100/100;DW0 全部 = 4096;逐块一致 | 11.7 µs | 21.6 µs |
| Store 4 KiB × 100,QD 1 | 100/100;另起进程回读逐块一致 | 17.3 µs | 27.1 µs |
| Exist × 100 | 100/100 | 10.4 µs | 19.9 µs |
| Delete × 100 | 100/100;另起进程回读全部不存在 | 10.8 µs | 20.5 µs |
| Retrieve 64 KiB × 100,QD 4 | 100/100;DW0 全部 = 65536;逐块一致 | 33.4 µs | 43.5 µs |

胶囊逐字段核对过,均符合 KV 1.3:

- key 位于 CDW2/3 和 CDW14/15
- CDW10 是 HBS/VS
- CDW11 是 KL=16
- Exist/Delete 的 SGL 长度为 0

怎么读这些延迟:

- **基线**:Exist/Delete 不带数据,约 10.5 µs 的等待就是一次往返的基线。
- **Retrieve**:在基线上多约 1 µs,用于 target 发 RDMA WRITE 写进 HBM。
- **Store**:多约 7 µs。target 要先发 RDMA READ 把 value 从 HBM 取回,再执行,
  比 Retrieve 多一个往返。这和 NVM 写比读慢是同一个原因。
- **"提交" 约 8.5 µs**:这是 C1 那个 QD=1 回退,和 KV 无关。
- **DW0 的代价**:读 DW0 会让每条响应多一次主存读,poller 处理一条响应约 4.5 µs。
- **QD 4 × 64 KiB**:100 条用了 1182 µs,约 5.5 GB/s。QD 和单 poller 都是限制
  因素,这个数不代表上限。

## 踩到的坑

**keep-alive 超时**:CPU 侧逐个 Store 几百个 key 时,如果只轮询 I/O qpair,
admin 队列上的 keep-alive 发不出去。target 会在 KATO(默认 10 s)后断开连接,
现象是 `CQ transport error -6`,后面的命令全部提交失败。`kv_smoke` 和
`gpu_initiated` 的 `cpu_kv` 现在会在等待时顺带调用
`spdk_nvme_ctrlr_process_admin_completions`。

**SPDK 单元测试链接失败**:`ctrlr_ut` / `tcp_ut` 把 `ctrlr.c` 直接 include
进去、bdev 函数全靠桩,补丁新用的 `spdk_bdev_get_module_name` 没有桩,
`make` 在 test/unit 报 undefined reference(库和 nvmf_tgt 其实已经编好)。
`apply_kv_patch.sh` 现在会顺带给这两个测试加桩,两者链接通过、全部用例通过。

**`spdk_event_mempool creation failed`**:DPDK 是 .so 时,
`librte_mempool_ring` 里只有构造函数。链接时如果开着 `--as-needed`,它会被
丢掉,mempool 就没有 ring 驱动。解决办法是在 Makefile 里用 `--no-as-needed`
包住 DPDK 库。
