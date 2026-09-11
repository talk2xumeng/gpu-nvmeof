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
TARGET_IP  ?= 172.16.3.3
NQN        ?= nqn.2024-01.io.test:cnode1
GPU_ID     ?= 4
NIC        ?= mlx5_1
BS         ?= 1048576
QD         ?= 32
COUNT      ?= 20000
ROUNDS     ?= 2000
# 64 token × 70272 B/token(DeepSeek MLA)≈ 4.5 MiB
KVBLOCK    ?= 4718592
# GPU-initiated 原型:单线程串行发,轮数受 RQ 深度限制
GPU_ROUNDS ?= 100
GPU_BS     ?= 4096

SRC        := src
BIN        := bin

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

.PHONY: all probe clean check ldconfig gpucheck run-launch-probe \
        run-mmio-probe run-mmio-probe-w \
        run-verify run-bw run-host run-latency run-latency-noker sweep-block \
        run-gpu-initiated run-wqe-verify help

all: $(BIN)/gds_nvmeof $(BIN)/latency_baseline $(BIN)/gpu_initiated \
     $(BIN)/wqe_verify

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
$(BIN)/gpu_io_kernel.o: $(SRC)/wqe_build.h $(SRC)/gpu_io_ctx.h

# GPU-initiated 原型。host 侧直接用了 mlx5dv 拿 SQ/CQ/UAR,要显式
# 链 -lmlx5;kernel 那个 .o 是 C++ 编出来的,要 -lstdc++。
$(BIN)/gpu_initiated: $(SRC)/gpu_initiated.c $(BIN)/gpu_io_kernel.o \
                      $(SRC)/gpu_io_ctx.h $(SRC)/wqe_build.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ $(SRC)/gpu_initiated.c $(BIN)/gpu_io_kernel.o \
	      $(LDFLAGS) -lmlx5 -lstdc++
	@echo "OK: $@"

$(BIN)/wqe_verify: $(SRC)/wqe_verify.c $(SRC)/wqe_build.h \
                   $(SRC)/gpu_backend.h | $(BIN)
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
# 轮数别超过 RQ 深度 —— kernel 不补 recv buffer,超了会死等。
run-gpu-initiated: $(BIN)/gpu_initiated
	sudo ./$(BIN)/gpu_initiated -a $(TARGET_IP) -n $(NQN) \
	     -g $(GPU_ID) -r $(GPU_ROUNDS) -b $(GPU_BS)

# 扫 KV block 粒度:16/32/64/128 token @70272 B
sweep-block: $(BIN)/latency_baseline
	@for tok in 16 32 64 128; do \
		b=$$(( tok * 70272 )); \
		b=$$(( (b + 4095) / 4096 * 4096 )); \
		echo "=== $$tok token = $$b bytes ==="; \
		sudo $(LAT) -g $(GPU_ID) -b $$b -r 500 2>/dev/null | \
		  grep -E "总计|控制面|数据传输"; \
	done

clean:
	rm -rf $(BIN)

help:
	@sed -n '1,25p' Makefile
