# tst/kv_target —— KV 命令集的模拟 target 和 CPU 侧冒烟测试
#
# 在 target 机器上(只需要 SPDK,不需要 GPU):
#   ./apply_kv_patch.sh $SPDK_DIR && make -C $SPDK_DIR -j   # 一次
#   make                 # 编 kv_tgt 和 kv_smoke
#
# 顶层 Makefile 的 make kv-tgt / make kv-smoke 也会转到这里。

SPDK_DIR   ?= /home/metaxadmin/mx/spdk
SRC        := ../../src
BIN        ?= ../../bin

PKGCONF := PKG_CONFIG_PATH=$(SPDK_DIR)/build/lib/pkgconfig:$(SPDK_DIR)/dpdk/build/lib/pkgconfig pkg-config

# nvmf 的子系统依赖链:nvmf -> bdev keyring sock;bdev -> accel keyring vmd
# sock iobuf;accel -> iobuf。缺一个 spdk_app_start 就报依赖找不到。
# sock_posix 给 TCP 用(没有 RDMA 的机器上也能自测),env_dpdk_rpc 可选。
TGT_WANT := spdk_event spdk_event_iobuf spdk_event_accel spdk_event_keyring \
            spdk_event_vmd spdk_event_sock spdk_event_bdev spdk_event_nvmf \
            spdk_sock_posix spdk_env_dpdk_rpc
TGT_PCS  := $(foreach p,$(TGT_WANT),$(if $(wildcard $(SPDK_DIR)/build/lib/pkgconfig/$(p).pc),$(p)))

# 自注册(构造函数)的模块和子系统要 whole-archive,否则静态链接时被丢掉。
# DPDK 若是 .so,同理要 --no-as-needed:librte_mempool_ring 里只有构造函数,
# 被 --as-needed 丢掉后 mempool 没有 ring 驱动,启动时报
#   spdk_event_mempool creation failed
TGT_LIBS := $(shell $(PKGCONF) --libs $(TGT_PCS) 2>/dev/null)
SMK_LIBS := $(shell $(PKGCONF) --libs spdk_nvme spdk_sock_posix 2>/dev/null)
ENV_LIBS := $(shell $(PKGCONF) --libs spdk_env_dpdk 2>/dev/null)
SYS_LIBS := $(shell $(PKGCONF) --libs --static spdk_syslibs 2>/dev/null)

CFLAGS   := -O2 -g -Wall -Wno-unused-parameter -I$(SPDK_DIR)/include -I$(SRC)
# DPDK .so 之间的间接依赖(librte_power -> librte_timer)链接期也要找得到
RPATHLINK := -Wl,-rpath-link,$(SPDK_DIR)/dpdk/build/lib

.PHONY: all clean check-spdk

all: $(BIN)/kv_tgt $(BIN)/kv_smoke

check-spdk:
	@if [ -z "$(TGT_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	@grep -q "gpu-nvmeof kv_emu" $(SPDK_DIR)/lib/nvmf/ctrlr.c || { \
		echo "错误: $(SPDK_DIR) 没打 KV 透传补丁,先 ./apply_kv_patch.sh 再重编 SPDK"; \
		exit 1; }

$(BIN):
	@mkdir -p $(BIN)

$(BIN)/kv_tgt: kv_tgt.c bdev_kv_emu.c | check-spdk $(BIN)
	$(CC) $(CFLAGS) -o $@ kv_tgt.c bdev_kv_emu.c -pthread \
	      -Wl,--whole-archive $(TGT_LIBS) -Wl,--no-whole-archive \
	      -Wl,--no-as-needed $(ENV_LIBS) -Wl,--as-needed $(SYS_LIBS) $(RPATHLINK)
	@echo "OK: $@"

# CPU 侧冒烟:用和 kernel 同一个 nvme_build_kv_sqe 发 KV 命令,先把
# target 和胶囊构造验对,再上 GPU。不依赖 GPU,target 机器上也能跑。
$(BIN)/kv_smoke: kv_smoke.c $(SRC)/wqe_build.h $(SRC)/blk_pattern.h | $(BIN)
	@if [ -z "$(SMK_LIBS)" ]; then \
		echo "错误: pkg-config 找不到 SPDK,检查 SPDK_DIR=$(SPDK_DIR)"; exit 1; fi
	$(CC) $(CFLAGS) -o $@ kv_smoke.c -pthread \
	      -Wl,--whole-archive $(SMK_LIBS) -Wl,--no-whole-archive \
	      -Wl,--no-as-needed $(ENV_LIBS) -Wl,--as-needed $(SYS_LIBS) $(RPATHLINK)
	@echo "OK: $@"

clean:
	rm -f $(BIN)/kv_tgt $(BIN)/kv_smoke
