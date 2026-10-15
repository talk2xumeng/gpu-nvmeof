#!/bin/bash
# kv_tgt 的配置。在跑着 kv_tgt 的机器上执行(先停掉普通的 nvmf_tgt,
# 两者默认抢同一个 RPC socket 和 4420 端口)。
#
#   ./setup_kv_target.sh            建 KV bdev + subsystem + listener
#   ./setup_kv_target.sh stats      看各 opcode 计数、key 数
#
# 环境变量:
#   KV_MB       容量 MiB(key+value 都计入,默认 4096)
#   KV_MAXVAL   单个 value 上限字节(默认 4 MiB;实际还受传输层 -i 限制)
#   KV_EDNEK    1 = Delete 不存在的 key 报 87h(默认 0,spec 默认行为)
#   NVM_NQN     对照用的 NVM subsystem(默认 ...:cnode2,挂 malloc bdev;
#               设成 none 不建)。和 KV 同一个 target 进程、同一条链路,
#               kv_vs_nvm.sh 用它测"KV 比读写多花多少"
#   NVM_MB      malloc 盘大小 MiB(默认 1024)
set -e

SPDK_DIR=${SPDK_DIR:-/home/metaxadmin/mx/spdk}
TARGET_IP=${TARGET_IP:-172.16.3.3}
NQN=${NQN:-nqn.2024-01.io.test:cnode1}
SOCK=${RPC_SOCK:-/var/tmp/spdk.sock}
BDEV=${KV_BDEV:-KVEmu0}
KV_MB=${KV_MB:-4096}
KV_MAXVAL=${KV_MAXVAL:-4194304}
KV_EDNEK=${KV_EDNEK:-0}
NVM_NQN=${NVM_NQN:-nqn.2024-01.io.test:cnode2}
NVM_BDEV=${NVM_BDEV:-Malloc0}
NVM_MB=${NVM_MB:-1024}

RPC="$SPDK_DIR/scripts/rpc.py -s $SOCK"

# rpc.py 不认识自定义方法,直接发 JSON-RPC
kvrpc() {
    python3 - "$SOCK" "$1" "$2" <<'PY'
import json, socket, sys
sock, method, params = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
s = socket.socket(socket.AF_UNIX)
s.connect(sock)
s.sendall(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                      "params": params}).encode())
buf = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
    try:
        resp = json.loads(buf)
        break
    except ValueError:
        continue
if "error" in resp:
    print(json.dumps(resp["error"], ensure_ascii=False))
    sys.exit(1)
print(json.dumps(resp["result"], indent=2, ensure_ascii=False))
PY
}

if [ "$1" = "stats" ]; then
    kvrpc bdev_kv_emu_stats "{\"name\": \"$BDEV\"}"
    exit 0
fi

if ! $RPC rpc_get_methods 2>/dev/null | grep -q bdev_kv_emu_create; then
    echo "kv_tgt 没在跑(或跑的是普通 nvmf_tgt)。先执行:"
    echo "  tmux new -s tgt"
    echo "  sudo $(dirname "$(realpath "$0")")/kv_tgt -m 0x3"
    exit 1
fi

# -i 131072:单条命令最大 128 KiB,和 setup_target.sh 一致;
# value 更大时要同时调大这里和 initiator 侧的 -b
$RPC nvmf_create_transport -t RDMA -u 8192 -i 131072 >/dev/null 2>&1 || true

kvrpc bdev_kv_emu_create "{\"name\": \"$BDEV\", \"capacity_mb\": $KV_MB, \
\"max_value\": $KV_MAXVAL, \"ednek\": $([ "$KV_EDNEK" = 1 ] && echo true || echo false)}" \
    || echo "($BDEV 可能已存在)"

# 新建的 subsystem 里没有 ns 1,remove 会报 Invalid parameters(rpc.py 打在
# stdout 上),属正常 —— 一并静音
add_subsys() {	# nqn serial bdev
    $RPC nvmf_create_subsystem $1 -a -s $2 >/dev/null 2>&1 || true
    $RPC nvmf_subsystem_remove_ns $1 1 >/dev/null 2>&1 || true
    $RPC nvmf_subsystem_add_ns $1 $3
    $RPC nvmf_subsystem_add_listener $1 -t rdma -a $TARGET_IP -s 4420 >/dev/null 2>&1 || true
}

add_subsys $NQN SPDK00000000000001 $BDEV
echo "KV : $NQN -> $BDEV (kv_emu, ${KV_MB} MiB, value <= $KV_MAXVAL B)"

if [ "$NVM_NQN" != none ]; then
    $RPC bdev_malloc_create -b $NVM_BDEV $NVM_MB 512 >/dev/null 2>&1 || true
    add_subsys $NVM_NQN SPDK00000000000002 $NVM_BDEV
    echo "NVM: $NVM_NQN -> $NVM_BDEV (malloc, ${NVM_MB} MiB,对照用)"
fi
$RPC nvmf_get_subsystems | python3 -c '
import json, sys
for s in json.load(sys.stdin):
    ns = ", ".join(n["bdev_name"] for n in s.get("namespaces", []))
    if ns:
        print("  %-36s %s" % (s["nqn"], ns))
'
