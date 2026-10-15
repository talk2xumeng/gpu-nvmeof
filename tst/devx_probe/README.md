# devx_probe:队列进显存的可行性探针

主存方案(SPDK 建的 QP,SQ/CQ/dbrec 在主存)的 QD1 时延压到 18.3 µs 后
到底了:剖析显示大头是 GPU 跨 PCIe 访问主存 —— 轮询 CQ 每圈 ~1 µs、提交
时回读 SQ ~6 µs、写主存后的 system fence 每次 ~0.7 µs。根治是把队列放
显存,这是 IBGDA(NVSHMEM / mxshmem)的做法。

这个探针不碰 NVMe-oF,只验证两件事:

1. 网卡写进显存的 CQE,GPU 轮询多快能看到
2. 网卡从显存取 WQE,比从主存慢多少

## 做法

- `mlx5dv_open_device(DEVX)`,DevX 自己建 CQ 和 RC QP(无 RQ),QP 回环到
  自己(对端 GID / MAC 就是本口的)
- 队列内存注册成 umem:
  - 显存:`mcExtMallocWithFlags(-F)` 导出 dma-buf,
    `mlx5dv_devx_umem_reg_ex` + `MLX5DV_UMEM_MASK_DMABUF`(mxshmem 主路径)
  - 主存:`posix_memalign` + `mlx5dv_devx_umem_reg`,再 `mcHostRegister` 给 GPU
- 一块 umem 里切:CQ @0、CQ dbrec @4096、QP dbrec @4160、SQ @8192
- UAR 用 `mlx5dv_devx_alloc_uar(NC)`,映射给 GPU 敲普通门铃(8 字节)
- GPU 单线程每轮:写 8 字节 payload → 写 RDMA WRITE WQE → dbrec → 门铃 →
  轮询 CQE → 写 CQ dbrec。显存里的队列写完只 `__threadfence()`(同
  mxshmem 的 `IBGDA_MEMBAR`),主存的用 `__threadfence_system()`
- 结构体偏移来自 DPDK `mlx5_prm.h`(`src/devx_ifc.h`,主程序的 DevX 队列也用它),rdma-core 的
  `mlx5_ifc.h` 没有 cqc,内核那份没有 umem_id 字段

## 跑

```sh
make devx-probe                       # 四种组合,mlx5_1、GPU 4、GID 3
make devx-probe DEVX_GID=3 IBDEV=mlx5_1
sudo ./bin/devx_probe -d mlx5_1 -g 4 -x 3 -C g -W g -n 1000    # 单跑
```

GID 选 RoCE v2 那个(`show_gids mlx5_1`)。需要 root。

## 判读

| 输出 | 含义 |
|---|---|
| `[时延] 提交` | payload + WQE + dbrec + 门铃,GPU 侧 |
| `[时延] 门铃→看见 CQE` | 网卡取 WQE、做 RDMA WRITE(回环)、写 CQE、GPU 看见 |
| `[轮询] 每轮 x 圈` | 圈数 × 每圈代价 ≈ 等待 |
| `[校验] PASS` | dst 里是最后一轮的序号,数据真的走通了 |

- `-C g` 比 `-C h` 的"门铃→CQE"小、每圈代价小:CQE 进显存有效
- `-W g` 比 `-W h` 的"门铃→CQE"大很多:网卡读显存(P2P read)慢,SQ 留主存
- 失败时看打印的 `status / syndrome`,或错误 CQE 的 syndrome
