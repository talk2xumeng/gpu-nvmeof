#!/bin/bash
# QD 扫描:按 (opt, 大小, QD) 跑 gpu_initiated,汇总 IOPS / 带宽 / 延迟。
# 仓库根目录执行:
#
#   scripts/qd_sweep.sh                                  # 默认:4K 读,QD 1/4/16/64,旧行为(0x10000)和默认(0)对照
#   QDS="1 8 32 96" OPTS="0x10000 0x11000 0x13000 0" scripts/qd_sweep.sh
#
# -O 的值会与默认的 0x7000 按位或;带 0x10000 时不加默认位(旧行为)。
# 所以 0 = 0x7000,0x10000 = 全关,0x14000 = 只开 0x4000。
#   EXTRA="-k" NQN=nqn.2024-01.io.test:cnode1 scripts/qd_sweep.sh   # KV Retrieve
#   EXTRA="-W" scripts/qd_sweep.sh                       # 写
#   NQPS="1 2 4" QDS="4 16" scripts/qd_sweep.sh          # 多 QP:QP 数 × QD 组合
#
# IOPS = 条数 / kernel 时长(kernel 自己量的 clock64,按实测频率换算),
# 不含 host 准备和 launch。读路径每次都逐块校验,FAIL 的不计入汇总。
# 写(-W)的回读要另起进程,这里不做,只看 NVMe 状态。
#
# QD 是每个 QP 的;总在飞 = QP 数 × QD。
#
# 环境变量:TARGET_IP NQN GPU_ID ROUNDS SIZES QDS NQPS OPTS REPEAT EXTRA OUT BIN
set -e

TARGET_IP=${TARGET_IP:-172.16.3.8}
NQN=${NQN:-nqn.2024-01.io.test:cnode2}
GPU_ID=${GPU_ID:-4}
ROUNDS=${ROUNDS:-5000}
SIZES=${SIZES:-4096}
QDS=${QDS:-1 4 16 64}
NQPS=${NQPS:-1}
OPTS=${OPTS:-0x10000 0}
REPEAT=${REPEAT:-1}
EXTRA=${EXTRA:-}
OUT=${OUT:-qd_sweep_$(date +%m%d_%H%M%S)}
BIN=${BIN:-./bin/gpu_initiated}

[ -x "$BIN" ] || { echo "找不到 $BIN,先在仓库根目录 make"; exit 1; }
mkdir -p "$OUT"
echo "$*  NQN=$NQN ROUNDS=$ROUNDS SIZES=$SIZES QDS=$QDS NQPS=$NQPS OPTS=$OPTS EXTRA=$EXTRA" > "$OUT/params.txt"

for opt in $OPTS; do
  for np in $NQPS; do
    for size in $SIZES; do
        for qd in $QDS; do
            for rep in $(seq 1 "$REPEAT"); do
                seed=$(( (RANDOM % 254) + 1 ))
                log="$OUT/o${opt}_p${np}_b${size}_q${qd}_${rep}.log"
                cmd="$BIN -a $TARGET_IP -n $NQN -g $GPU_ID -r $ROUNDS -b $size -q $np -Q $qd -L 0 -S $seed -O $opt $EXTRA"
                echo "$cmd" > "$log"
                sudo $cmd >> "$log" 2>&1 || true
                printf "  -O %-7s %2s QP %7s  QD %-3s #%d  %s\n" "$opt" "$np" "$size" "$qd" "$rep" \
                    "$(grep -m1 '^\[完成\]' "$log" || echo '没有 [完成] 行,看日志')"
            done
        done
    done
  done
done

python3 "$(dirname "$0")/qd_sweep_report.py" "$OUT" | tee "$OUT/summary.md"
echo
echo "日志、results.csv、summary.md 在 $OUT/"
