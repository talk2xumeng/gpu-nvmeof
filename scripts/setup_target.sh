#!/bin/bash
# target 侧配置。在跑着 nvmf_tgt 的机器上执行。
#
#   ./setup_target.sh [null|malloc]
#     null   —— 不做 memcpy,测路径带宽上限(读出来是全零,不能做校验)
#     malloc —— 内存盘,数据真实,用于 -V 校验
set -e

SPDK_DIR=${SPDK_DIR:-/home/metaxadmin/mx/spdk}
TARGET_IP=${TARGET_IP:-172.16.3.3}
NQN=${NQN:-nqn.2024-01.io.test:cnode1}
MODE=${1:-null}
SIZE_MB=${SIZE_MB:-8192}

RPC="$SPDK_DIR/scripts/rpc.py"

if ! $RPC nvmf_get_subsystems >/dev/null 2>&1; then
    echo "nvmf_tgt 没在跑。先执行:"
    echo "  tmux new -s tgt"
    echo "  sudo $SPDK_DIR/build/bin/nvmf_tgt -m 0x3"
    exit 1
fi

$RPC nvmf_create_transport -t RDMA -u 8192 -i 131072 2>/dev/null || true

if [ "$MODE" = "null" ]; then
    $RPC bdev_null_create Null0 $SIZE_MB 512 2>/dev/null || true
    BDEV=Null0
else
    $RPC bdev_malloc_create -b Malloc0 $SIZE_MB 512 2>/dev/null || true
    BDEV=Malloc0
fi

$RPC nvmf_create_subsystem $NQN -a -s SPDK00000000000001 2>/dev/null || true
$RPC nvmf_subsystem_remove_ns $NQN 1 2>/dev/null || true
$RPC nvmf_subsystem_add_ns $NQN $BDEV
$RPC nvmf_subsystem_add_listener $NQN -t rdma -a $TARGET_IP -s 4420 2>/dev/null || true

echo "后端: $BDEV"
$RPC nvmf_get_subsystems | grep -A4 namespaces
