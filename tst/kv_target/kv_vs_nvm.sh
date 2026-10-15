#!/bin/bash
# KV 比 NVM 读写多花多少 —— 同一个 kv_tgt、同一条链路、同一个 GPU kernel,
# 只换命令集。在 initiator 上、仓库根目录执行:
#
#   tst/kv_target/kv_vs_nvm.sh
#   TARGET_IP=172.16.3.8 SIZES="4096 65536 131072" REPEAT=5 tst/kv_target/kv_vs_nvm.sh
#
# target 侧先跑 setup_kv_target.sh(默认同时建 KV 的 cnode1 和 malloc 的 cnode2)。
#
# 每个 value 大小跑:
#   read     NVM 读      vs  retrieve  KV Retrieve
#   write    NVM 写      vs  store     KV Store
#   exist    KV Exist(无数据,往返基线;只在第一个大小上跑)
# 全部 QD 1,每次 ROUNDS 条,重复 REPEAT 次,取各次 p50 的中位数。
#
# 为什么相减有意义:C1 那个 QD=1 回退(小 I/O 多 ~12 us)两边都有,差值里
# 抵消;剩下的是 KV 自己的代价 —— poller 多读一次 DW0、target 侧 kv_emu
# 的哈希 + 拷贝(对照的 malloc bdev 也拷贝,所以主要是哈希和锁)。
#
# 输出:OUT 目录下每次运行的完整日志、results.csv、summary.md(同时打到屏幕)。
# 环境变量:TARGET_IP KV_NQN NVM_NQN GPU_ID ROUNDS SIZES REPEAT OUT EXTRA
#   EXTRA 原样加到每条命令后面,比如 EXTRA="-O 128" 带上 CQE 硬件时间戳
set -e

TARGET_IP=${TARGET_IP:-172.16.3.8}
KV_NQN=${KV_NQN:-nqn.2024-01.io.test:cnode1}
NVM_NQN=${NVM_NQN:-nqn.2024-01.io.test:cnode2}
GPU_ID=${GPU_ID:-4}
ROUNDS=${ROUNDS:-100}
SIZES=${SIZES:-4096 65536 131072}
REPEAT=${REPEAT:-3}
OUT=${OUT:-kv_vs_nvm_$(date +%m%d_%H%M%S)}
EXTRA=${EXTRA:-}
BIN=${BIN:-./bin/gpu_initiated}

[ -x "$BIN" ] || { echo "找不到 $BIN,先在仓库根目录 make"; exit 1; }
mkdir -p "$OUT"

run() {		# op size rep nqn args...
    local op=$1 size=$2 rep=$3 nqn=$4
    shift 4
    local seed=$(( (RANDOM % 254) + 1 ))
    local log="$OUT/${op}_${size}_${rep}.log"
    local cmd="$BIN -a $TARGET_IP -n $nqn -g $GPU_ID -r $ROUNDS -b $size -Q 1 -L 0 -S $seed $* $EXTRA"

    echo "$cmd" > "$log"
    sudo $cmd >> "$log" 2>&1 || true	# 成败由解析判断,不在这里中断
    printf "  %-8s %7s  #%d  %s\n" "$op" "$size" "$rep" \
           "$(grep -m1 '^\[完成\]' "$log" || echo '没有 [完成] 行,看日志')"
}

first=1
for size in $SIZES; do
    echo "== value $size 字节"
    for rep in $(seq 1 "$REPEAT"); do
        run read     "$size" "$rep" "$NVM_NQN"
        run retrieve "$size" "$rep" "$KV_NQN" -k
        run write    "$size" "$rep" "$NVM_NQN" -W
        run store    "$size" "$rep" "$KV_NQN" -k -W
        if [ $first = 1 ]; then
            run exist "$size" "$rep" "$KV_NQN" -k -E
        fi
    done
    first=0
done

python3 "$(dirname "$0")/kv_vs_nvm_report.py" "$OUT" "$ROUNDS" | tee "$OUT/summary.md"
echo
echo "日志、results.csv、summary.md 在 $OUT/"
