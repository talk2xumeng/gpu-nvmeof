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
BS         ?= 1048576
QD         ?= 32
COUNT      ?= 20000
ROUNDS     ?= 2000
# 64 token × 70272 B/token(DeepSeek MLA)≈ 4.5 MiB
KVBLOCK    ?= 4718592

SRC        := src
BIN        := bin

# ------------------------------------------------------------------

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
    GPU_LIBS := -L$(MACA_DIR)/lib -lmcruntime -lmccompiler
    GPU_LIB_PATH := $(MACA_DIR)/lib
    GPUCC    := $(MXCC)
    GPUCFLAGS := -O2 -fPIC -x maca -DUSE_MACA -I$(MACA_DIR)/include -I$(SRC)
endif

RDMA_LIBS  := -libverbs -lrdmacm
EXTRA_LIBS := -lssl -lcrypto -lpthread -lrt -lnuma -ldl -luuid -lm
LDFLAGS    := $(SPDK_LIBS) $(SYS_LIBS) $(GPU_LIBS) $(RDMA_LIBS) $(EXTRA_LIBS)

.PHONY: all probe clean check ldconfig gpucheck \
        run-verify run-bw run-host run-latency run-latency-noker sweep-block help

all: $(BIN)/gds_nvmeof $(BIN)/latency_baseline

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/gds_nvmeof: $(SRC)/gds_nvmeof.c $(SRC)/gpu_backend.h | $(BIN)
	@if [ -z "$(SPDK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "OK: $@"

# 基线程序要链 mxcc 编的 kernel
$(BIN)/kernels.o: $(SRC)/kernels.cpp | $(BIN)
	@if [ ! -x "$(GPUCC)" ] && ! command -v $(GPUCC) >/dev/null; then \
		echo "错误: 找不到 GPU 编译器 $(GPUCC)"; \
		echo "  试试: find $(MACA_DIR) -name 'mxcc*' -maxdepth 3"; \
		echo "  然后: make MXCC=<实际路径>"; exit 1; fi
	$(GPUCC) $(GPUCFLAGS) -c -o $@ $<

$(BIN)/latency_baseline: $(SRC)/latency_baseline.c $(BIN)/kernels.o $(SRC)/gpu_backend.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/latency_baseline.c $(BIN)/kernels.o \
	      $(LDFLAGS) -lstdc++
	@echo "OK: $@"

$(BIN)/dmabuf_probe: $(SRC)/dmabuf_probe.c | $(BIN)
	$(CC) -O2 -Wall -o $@ $< -libverbs -ldl
	@echo "OK: $@"

probe: $(BIN)/dmabuf_probe

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

run-latency: $(BIN)/latency_baseline
	sudo $(LAT) -g $(GPU_ID) -b $(KVBLOCK) -r $(ROUNDS)

run-latency-noker: $(BIN)/latency_baseline
	sudo $(LAT) -g $(GPU_ID) -b $(KVBLOCK) -r $(ROUNDS) -k 0

# 扫 KV block 粒度:16/32/64/128 token @70272 B
sweep-block: $(BIN)/latency_baseline
	@for tok in 8 16 32 64 128; do \
		b=$$(( tok * 70272 )); \
		b=$$(( (b + 4095) / 4096 * 4096 )); \
		echo "=== $$tok token = $$b bytes ==="; \
		sudo $(LAT) -g $(GPU_ID) -b $$b -r 500 2>/dev/null | \
		  sed -n '/优化空间/,$$p'; \
		echo; \
	done

clean:
	rm -rf $(BIN)

help:
	@sed -n '1,25p' Makefile
