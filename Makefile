# gpu-nvmeof
#
#   make                  编译全部
#   make probe            只编 dmabuf_probe(不依赖 SPDK)
#   sudo make ldconfig    注册动态库路径,只做一次
#   make check            环境自检
#
# 测试:
#   make run-verify       显存数据校验
#   make run-bw           显存带宽
#   make run-latency      端到端延迟基线
#   make run-latency-noker  同上但不跑 kernel(相减得 kernel 开销)
#   make sweep-block      扫不同 KV block 粒度
#   make run-gpu-initiated  GPU 内发起的 NVMe-oF 读(kernel 直接收发)
#   make run-gpu-read-verify    GPU 读 + 逐块校验(进程内)
#   make run-gpu-write-verify   GPU 写 + 另起进程逐块回读
#
# KV 命令集(target 换成 tst/kv_target 的 kv_tgt,见那里的 README):
#   make kv-tgt           编 kv_tgt + kv_smoke(target 机器上,只要 SPDK)
#   make kv-smoke         只编 kv_smoke(initiator 机器上)
#   make run-kv-smoke     CPU 路径冒烟:验 target 和胶囊构造,不碰 GPU
#   make run-gpu-kv-retrieve   GPU Retrieve + 逐块校验 + DW0 校验
#   make run-gpu-kv-store      GPU Store + 另起进程逐块回读
#   make run-gpu-kv-exist      GPU Exist(只看状态)
#   make run-gpu-kv-delete     GPU Delete + 另起进程确认全部不存在
#   make qd-sweep              QD 扫描(IOPS/带宽/延迟),OPTS 对照,见 scripts/qd_sweep.sh
#   make kv-vs-nvm             KV vs NVM 读写延迟对照(同一 target,QD 1,
#                              SIZES/REPEAT 可调,见 tst/kv_target/kv_vs_nvm.sh)
#
# 实验(tst/,各目录有 README 记录背景和结论):
#   make pit7-matrix [MATRIX=1|2|3] 坑 7:kernel 写显存对网卡的可见性
#   make pit7-bench                 坑 7:kernel 填充微基准
#
# 注意:动态库路径必须先 make ldconfig 注册,否则 sudo 下会找不到
# librte_*.so —— gcc 生成的是 RUNPATH,sudo 提权后动态链接器进安全
# 模式会忽略 RUNPATH,同时清掉 LD_LIBRARY_PATH,两头都断。

SPDK_DIR   ?= /home/metaxadmin/mx/spdk
DPDK_LIB   ?= $(SPDK_DIR)/dpdk/build/lib
BACKEND    ?= maca

MACA_DIR   ?= /opt/maca
CUDA_DIR   ?= /usr/local/cuda

# GPU 编译器(实测路径)
MXCC       ?= $(MACA_DIR)/mxgpu_llvm/bin/mxcc

# 测试参数
TARGET_IP  ?= 172.16.3.8
NQN        ?= nqn.2024-01.io.test:cnode1
GPU_ID     ?= 4
NIC        ?= mlx5_1
BS         ?= 1048576
QD         ?= 32
COUNT      ?= 20000
ROUNDS     ?= 2000
# 64 token × 70272 B/token(DeepSeek MLA)≈ 4.5 MiB
KVBLOCK    ?= 4718592
# GPU-initiated 原型。kernel 自己补 recv(C2),轮数不受 RQ 深度限制
GPU_ROUNDS ?= 100
GPU_BS     ?= 4096
GPU_QD     ?= 1
GPU_NQ     ?= 1
GPU_LBA    ?= 0
# pit7-bench 注册 MR 用的网卡(和 GPU 同 NUMA / PCIe switch 的那张)
IBDEV      ?= mlx5_1
# pattern seed:每次 make 取一个新值,防止上一轮留在盘上的数据冒充本轮结果。
# 必须是立即展开(:=),?= 是递归展开,两行 recipe 跨秒会拿到不同的值。
ifeq ($(origin SEED),undefined)
SEED       := $(shell echo $$(( $$(date +%s) % 255 + 1 )))
endif

SRC        := src
BIN        := bin
TST        := tst

# ------------------------------------------------------------------

# 注意:必须把路径内联进 shell 调用,不能用 export。
# export 只影响 recipe 里的子进程,而 $(shell) 是 make 解析阶段执行的,
# 那时导出还没生效,pkg-config 拿不到路径。
PKGCONF := PKG_CONFIG_PATH=$(SPDK_DIR)/build/lib/pkgconfig:$(DPDK_LIB)/pkgconfig pkg-config

SPDK_CFLAGS := $(shell $(PKGCONF) --cflags spdk_nvme spdk_env_dpdk 2>/dev/null)
SPDK_LIBS   := $(shell $(PKGCONF) --libs   spdk_nvme spdk_env_dpdk 2>/dev/null)
SYS_LIBS    := $(shell $(PKGCONF) --libs   spdk_syslibs 2>/dev/null)

CFLAGS     := -O2 -g -Wall -Wno-unused-parameter -Wno-format-truncation
CFLAGS     += -I$(SRC) $(SPDK_CFLAGS)

ifeq ($(BACKEND),cuda)
    CFLAGS   += -I$(CUDA_DIR)/include
    GPU_LIBS := -L$(CUDA_DIR)/lib64 -L$(CUDA_DIR)/lib64/stubs -lcuda -lcudart
    GPU_LIB_PATH := $(CUDA_DIR)/lib64
    GPUCC    := nvcc
    GPUCFLAGS := -O2 -I$(SRC)
else
    CFLAGS   += -DUSE_MACA -I$(MACA_DIR)/include
    # libmccompiler 提供 __mcPushCallConfiguration 等三尖括号 launch
    # 展开后需要的运行时符号
    GPU_LIBS := -L$(MACA_DIR)/lib -lmcruntime -lmccompiler
    GPU_LIB_PATH := $(MACA_DIR)/lib
    GPUCC    := $(MXCC)
    # -x maca 是关键:mxcc 靠 .maca 后缀或 -x maca 才进 GPU 编译模式,
    # 否则 .cpp 被当普通 C++,blockIdx/threadIdx 之类都不认识。
    # -fPIC 必需:Ubuntu 的 gcc 默认生成 PIE,mxcc 的 .o 不是位置无关的话链接会失败
    GPUCFLAGS := -O2 -fPIC -x maca -DUSE_MACA -I$(MACA_DIR)/include -I$(SRC)
endif

RDMA_LIBS  := -libverbs -lrdmacm
EXTRA_LIBS := -lssl -lcrypto -lpthread -lrt -lnuma -ldl -luuid -lm
LDFLAGS    := $(SPDK_LIBS) $(SYS_LIBS) $(GPU_LIBS) $(RDMA_LIBS) $(EXTRA_LIBS)

.PHONY: all probe clean check ldconfig gpucheck run-launch-probe run-wqe-verify \
        run-mmio-probe run-mmio-probe-w \
        run-verify run-bw run-host run-latency run-latency-noker sweep-block \
        run-gpu-initiated run-gpu-read-verify run-gpu-write-verify \
        tst pit7-matrix pit7-bench help kv-tgt kv-smoke run-kv-smoke \
        run-gpu-kv-retrieve run-gpu-kv-store run-gpu-kv-exist run-gpu-kv-delete \
        kv-vs-nvm qd-sweep

# 实验程序也在 all 里:src/ 改动若让它们编不过,当场就能发现,
# 而不是等回头复现时才知道(坑 4)。
all: $(BIN)/gds_nvmeof $(BIN)/latency_baseline $(BIN)/gpu_initiated \
     $(BIN)/wqe_verify tst

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/gds_nvmeof: $(SRC)/gds_nvmeof.c $(SRC)/gpu_backend.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "OK: $@"

# host 程序要链 mxcc 编的 kernel。kernels.o 给基线测试,
# gpu_io_kernel.o 给 GPU-initiated 原型。
$(BIN)/%.o: $(SRC)/%.cpp | $(BIN)
	@if [ ! -x "$(GPUCC)" ] && ! command -v $(GPUCC) >/dev/null; then \
		echo "错误: 找不到 GPU 编译器 $(GPUCC)"; \
		echo "  试试: find $(MACA_DIR) -name 'mxcc*' -maxdepth 3"; \
		echo "  然后: make MXCC=<实际路径>"; exit 1; fi
	$(GPUCC) $(GPUCFLAGS) -c -o $@ $<

# WQE/CQE 的字节布局全在 wqe_build.h 里,改它必须重编 —— 否则只动
# header 时 make 认为 .o 还是新的,你会拿着旧二进制查一晚上。
$(BIN)/gpu_io_kernel.o: $(SRC)/wqe_build.h $(SRC)/gpu_io_ctx.h $(SRC)/blk_pattern.h

# GPU-initiated 原型。host 侧直接用了 mlx5dv 拿 SQ/CQ/UAR,要显式
# 链 -lmlx5;kernel 那个 .o 是 C++ 编出来的,要 -lstdc++。
$(BIN)/gpu_initiated: $(SRC)/gpu_initiated.c $(BIN)/gpu_io_kernel.o \
                      $(SRC)/gpu_io_ctx.h $(SRC)/wqe_build.h \
                      $(SRC)/blk_pattern.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ $(SRC)/gpu_initiated.c $(BIN)/gpu_io_kernel.o \
	      $(LDFLAGS) -lmlx5 -lstdc++
	@echo "OK: $@"

# 纯 host 侧:WQE/胶囊布局比对 + host 手工提交。不链 GPU kernel。
$(BIN)/wqe_verify: $(SRC)/wqe_verify.c $(SRC)/wqe_build.h \
                   $(SRC)/gpu_backend.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ $(SRC)/wqe_verify.c $(LDFLAGS) -lmlx5 -lstdc++
	@echo "OK: $@"

$(BIN)/latency_baseline: $(SRC)/latency_baseline.c $(BIN)/kernels.o $(SRC)/gpu_backend.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/latency_baseline.c $(BIN)/kernels.o \
	      $(LDFLAGS) -lstdc++
	@echo "OK: $@"

$(BIN)/dmabuf_probe: $(SRC)/dmabuf_probe.c | $(BIN)
	$(CC) -O2 -Wall -o $@ $< -libverbs -ldl
	@echo "OK: $@"

# 纯 GPU 程序,不依赖 SPDK,mxcc 一步编完
$(BIN)/launch_probe: $(SRC)/launch_probe.cpp | $(BIN)
	$(GPUCC) $(GPUCFLAGS) -o $@ $<
	@echo "OK: $@"

# GPU 访问网卡队列/门铃的探测。需要 mlx5dv,不依赖 SPDK。
$(BIN)/mmio_probe: $(SRC)/mmio_probe.cpp | $(BIN)
	$(GPUCC) $(GPUCFLAGS) -o $@ $< -libverbs -lmlx5
	@echo "OK: $@"

probe: $(BIN)/dmabuf_probe $(BIN)/launch_probe $(BIN)/mmio_probe

run-wqe-verify: $(BIN)/wqe_verify
	sudo ./$(BIN)/wqe_verify -a $(TARGET_IP) -n $(NQN) -g $(GPU_ID)

# ---- 环境 ----

ldconfig:
	@echo "$(DPDK_LIB)"     > /etc/ld.so.conf.d/spdk-dpdk.conf
	@echo "$(GPU_LIB_PATH)" > /etc/ld.so.conf.d/gpu-backend.conf
	@ldconfig
	@ldconfig -p | grep -E "librte_eal|libmcruntime|libcudart" | head

gpucheck:
	@echo "=== GPU 编译器 ==="
	@ls -l $(GPUCC) 2>/dev/null || command -v $(GPUCC) || \
	   (echo "未找到,搜索中..."; find $(MACA_DIR) -name 'mxcc*' -maxdepth 3 2>/dev/null)

check:
	@echo "=== 内核 (需 >= 5.12) ==="; uname -r
	@echo "=== ibv_reg_dmabuf_mr ==="
	@nm -D $$(ldconfig -p | grep 'libibverbs.so ' | head -1 | awk '{print $$NF}') \
	   2>/dev/null | grep -q ibv_reg_dmabuf_mr && echo OK || echo 缺失
	@echo "=== hugepage ==="; grep -E "HugePages_(Total|Free)" /proc/meminfo
	@echo "=== SPDK ==="
	@test -f $(SPDK_DIR)/build/lib/libspdk_nvme.a && echo OK || echo "未找到"
	@echo "=== 动态库注册 ==="
	@ldconfig -p | grep -q librte_eal && echo "DPDK OK" || echo "请 sudo make ldconfig"
	@$(MAKE) --no-print-directory gpucheck

# ---- 测试 ----

GDS := $(BIN)/gds_nvmeof -a $(TARGET_IP) -n $(NQN)
LAT := $(BIN)/latency_baseline -a $(TARGET_IP) -n $(NQN)

run-verify: $(BIN)/gds_nvmeof
	sudo $(GDS) -g $(GPU_ID) -V -c 64

run-bw: $(BIN)/gds_nvmeof
	sudo $(GDS) -g $(GPU_ID) -b $(BS) -q $(QD) -c $(COUNT)

run-host: $(BIN)/gds_nvmeof
	sudo $(GDS) -H -b $(BS) -q $(QD) -c $(COUNT)

run-launch-probe: $(BIN)/launch_probe
	./$(BIN)/launch_probe $(GPU_ID) 1000

# 只映射不写门铃,安全
run-mmio-probe: $(BIN)/mmio_probe
	sudo ./$(BIN)/mmio_probe $(NIC) $(GPU_ID)

# 加 -w 真的从 kernel 写门铃(QP 在 RESET 态,不会发包)
run-mmio-probe-w: $(BIN)/mmio_probe
	sudo ./$(BIN)/mmio_probe $(NIC) $(GPU_ID) -w

run-latency: $(BIN)/latency_baseline
	sudo $(LAT) -g $(GPU_ID) -b $(KVBLOCK) -r $(ROUNDS)

run-latency-noker: $(BIN)/latency_baseline
	sudo $(LAT) -g $(GPU_ID) -b $(KVBLOCK) -r $(ROUNDS) -k 0

# GPU-initiated:稳态收发全在 kernel 内,CPU 只做初始化。
# C2 起 kernel 自己补 recv,轮数不受 RQ 深度限制(-O 2048 关掉时恢复 ~127 上限)。
run-gpu-initiated: $(BIN)/gpu_initiated
	sudo ./$(BIN)/gpu_initiated -a $(TARGET_IP) -n $(NQN) \
	     -g $(GPU_ID) -r $(GPU_ROUNDS) -b $(GPU_BS) -Q $(GPU_QD)

# 逐块校验。显存按条数分配(每条一块),条数很大时注意显存和盘/malloc 容量。
GI := $(BIN)/gpu_initiated -a $(TARGET_IP) -n $(NQN) -g $(GPU_ID) \
      -r $(GPU_ROUNDS) -b $(GPU_BS) -q $(GPU_NQ) -Q $(GPU_QD) -L $(GPU_LBA) -S $(SEED)

run-gpu-read-verify: $(BIN)/gpu_initiated
	sudo ./$(GI)

# 写的回读必须另起进程(debug-notes 坑 3)。第一步失败也要回读看现场。
run-gpu-write-verify: $(BIN)/gpu_initiated
	-sudo ./$(GI) -W
	sudo ./$(GI) -V

# KV:key = (seed, L+i),value 长度 = GPU_BS(8 的倍数即可)。
# 写类(Store/Delete)的回读同样必须另起进程。
GKV := $(GI) -k

run-gpu-kv-retrieve: $(BIN)/gpu_initiated
	sudo ./$(GKV)

run-gpu-kv-store: $(BIN)/gpu_initiated
	-sudo ./$(GKV) -W
	sudo ./$(GKV) -V

run-gpu-kv-exist: $(BIN)/gpu_initiated
	sudo ./$(GKV) -E

# 回读用 -V -D:期望全部不存在,全是才 PASS
run-gpu-kv-delete: $(BIN)/gpu_initiated
	-sudo ./$(GKV) -D
	sudo ./$(GKV) -V -D

# QD 扫描。默认对 cnode2(malloc)跑 4K 读,旧行为(-O 0x10000)与默认(-O 0,即 0x7000)对照
QDS     ?= 1 4 16 64
NQPS    ?= 1
OPTS    ?= 0x10000 0
SWEEP_NQN ?= nqn.2024-01.io.test:cnode2
SWEEP_ROUNDS ?= 5000

qd-sweep: $(BIN)/gpu_initiated
	TARGET_IP=$(TARGET_IP) NQN=$(SWEEP_NQN) GPU_ID=$(GPU_ID) \
	ROUNDS=$(SWEEP_ROUNDS) SIZES="$(GPU_BS)" QDS="$(QDS)" NQPS="$(NQPS)" OPTS="$(OPTS)" \
	REPEAT=$(REPEAT) EXTRA="$(EXTRA)" BIN=./$(BIN)/gpu_initiated scripts/qd_sweep.sh

# 同一个 kv_tgt 上 KV(cnode1)和 malloc(cnode2)各跑一遍,算差值
KV_NQN  ?= $(NQN)
NVM_NQN ?= nqn.2024-01.io.test:cnode2
SIZES   ?= 4096 65536 131072
REPEAT  ?= 3

kv-vs-nvm: $(BIN)/gpu_initiated
	TARGET_IP=$(TARGET_IP) KV_NQN=$(KV_NQN) NVM_NQN=$(NVM_NQN) \
	GPU_ID=$(GPU_ID) ROUNDS=$(GPU_ROUNDS) SIZES="$(SIZES)" REPEAT=$(REPEAT) \
	BIN=./$(BIN)/gpu_initiated $(TST)/kv_target/kv_vs_nvm.sh

# ---- KV 模拟 target(tst/kv_target,target 机器上编)----
kv-tgt:
	$(MAKE) -C $(TST)/kv_target SPDK_DIR=$(SPDK_DIR) BIN=$(abspath $(BIN))

# initiator 侧只要 kv_smoke(kv_tgt 需要打过补丁的 SPDK,只在 target 上编)
kv-smoke:
	$(MAKE) -C $(TST)/kv_target SPDK_DIR=$(SPDK_DIR) BIN=$(abspath $(BIN)) \
	        $(abspath $(BIN))/kv_smoke

run-kv-smoke: kv-smoke
	sudo ./$(BIN)/kv_smoke -a $(TARGET_IP) -n $(NQN) -S $(SEED)

# 扫 KV block 粒度:16/32/64/128 token @70272 B
sweep-block: $(BIN)/latency_baseline
	@for tok in 16 32 64 128; do \
		b=$$(( tok * 70272 )); \
		b=$$(( (b + 4095) / 4096 * 4096 )); \
		echo "=== $$tok token = $$b bytes ==="; \
		sudo $(LAT) -g $(GPU_ID) -b $$b -r 500 2>/dev/null | \
		  grep -E "总计|控制面|数据传输"; \
	done

# ---- 实验:tst/pit7 ----
# 独立的一套 host + kernel,和 src/ 不共享 ctx 结构。
PIT7 := $(TST)/pit7

$(BIN)/pit7_io_kernel.o: $(PIT7)/pit7_io_kernel.cpp $(PIT7)/pit7_io_ctx.h \
                         $(SRC)/wqe_build.h $(SRC)/blk_pattern.h | $(BIN)
	$(GPUCC) $(GPUCFLAGS) -I$(PIT7) -c -o $@ $<

$(BIN)/pit7_initiated: $(PIT7)/pit7_initiated.c $(BIN)/pit7_io_kernel.o \
                       $(PIT7)/pit7_io_ctx.h $(PIT7)/pit7_devx_mkey.h \
                       $(SRC)/blk_pattern.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -I$(PIT7) -o $@ $(PIT7)/pit7_initiated.c \
	      $(BIN)/pit7_io_kernel.o $(LDFLAGS) -lmlx5 -lstdc++
	@echo "OK: $@"

tst: $(BIN)/pit7_initiated

pit7-matrix: $(BIN)/pit7_initiated
	sudo BIN=$(BIN) TARGET_IP=$(TARGET_IP) NQN=$(NQN) GPU_ID=$(GPU_ID) \
	     ROUNDS=$(GPU_ROUNDS) BS=$(GPU_BS) LBA=$(GPU_LBA) \
	     MATRIX=$(MATRIX) $(PIT7)/pit7_matrix.sh

pit7-bench: $(BIN)/pit7_initiated
	sudo ./$(BIN)/pit7_initiated -g $(GPU_ID) -b $(GPU_BS) -N $(IBDEV) -B

clean:
	rm -rf $(BIN)

help:
	@sed -n '1,35p' Makefile
