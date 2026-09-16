#!/bin/bash
#
# pit7_matrix.sh  --  坑 7 的对照矩阵:kernel 写普通显存,网卡能不能读到
#
# 背景:Tutti 的 snvme_smoke_gpu.cu 在 C500 上用普通 cudaMalloc 的 SQ ring,
# kernel 写 SQE + __threadfence_system() + 敲门铃,本地 NVMe 能正确读到。
# 我们同样的动作(胶囊/payload)网卡却读到旧值。差别在:
#   - Tutti 的 submit kernel 敲完门铃就退出,poll 是另一次 launch    -> -T 1
#   - Tutti 在 MACA 下轮询每圈都做 system fence                       -> -Z
#
# 每个用例一个新 seed。完整日志写到 $LOG,屏幕只打摘要。
#
# 用法:  make pit7-matrix              第三轮(默认)
#        make pit7-matrix MATRIX=1|2   前两轮
# 参数从 Makefile 传进来;日志默认写到 tst/pit7/logs/

BIN=${BIN:-bin}
TARGET_IP=${TARGET_IP:-172.16.3.3}
NQN=${NQN:-nqn.2024-01.io.test:cnode1}
GPU_ID=${GPU_ID:-4}
ROUNDS=${ROUNDS:-100}
BS=${BS:-4096}
LBA=${LBA:-0}
LOGDIR=${LOGDIR:-tst/pit7/logs}
mkdir -p "$LOGDIR"
LOG=${LOG:-$LOGDIR/pit7_$(date +%Y%m%d_%H%M%S).log}

G="./$BIN/pit7_initiated -a $TARGET_IP -n $NQN -g $GPU_ID -r $ROUNDS -b $BS -L $LBA"
seed=$(( $(date +%s) % 200 + 20 ))

summary() {
	# 块图统计行、PASS/FAIL、结果、分段 p50、墙钟
	grep -E '^\[结果\]|^  [.RCSWZFXO?]+$|^  [.RCSWZFXO?]=|PASS|FAIL|^  总计|^    填充|^    提交|^    等待|墙钟|轮询圈数|\*\*\*' "$1" |
		grep -vE '写成功|期望看到' | sed 's/^/    /'
}

run_read() {		# $1 = 名字, 其余 = 额外参数
	local name=$1; shift
	local tmp; tmp=$(mktemp)
	seed=$((seed + 1))
	echo "=== $name  [读 $*]"
	{ echo "===== $name: $G -S $seed $*"; $G -S $seed "$@"; } >"$tmp" 2>&1
	summary "$tmp"
	cat "$tmp" >>"$LOG"
	rm -f "$tmp"
}

run_write() {
	local name=$1; shift
	local tmp; tmp=$(mktemp)
	seed=$((seed + 1))
	echo "=== $name  [写 $*]"
	{
		echo "===== $name: $G -S $seed -W $*"
		$G -S $seed -W "$@"
		echo "----- 回读"
		$G -S $seed -V
	} >"$tmp" 2>&1
	summary "$tmp"
	cat "$tmp" >>"$LOG"
	rm -f "$tmp"
}

echo "日志: $LOG"
echo

# 第二轮:门铃挪到写显存的 kernel 退出之后
# 第一轮结论:同一 kernel 内写入对网卡始终不可见(-Z 无效);kernel 退出后
# 可见,但 -T 1/2 的门铃在写入 kernel 内部,网卡读得比退出早。
#
# 读块图时注意:胶囊槽位按 i&0x3f 复用,"块 0-35 对、其余 R"说明网卡
# 读到的是上一次刷下去的旧胶囊。
# MATRIX=1 / 2 / 3 选轮次,默认 3
MATRIX=${MATRIX:-3}
[ -n "$MATRIX_V1" ] && MATRIX=1

case "$MATRIX" in
1)
	run_read  "C0 拆分自检"            -T 1
	run_read  "R0 普通显存胶囊/单kernel" -H -F 0
	run_read  "R1 +拆分,不同步 (A)"     -H -F 0 -T 1
	run_read  "R2 +拆分,同步   (B)"     -H -F 0 -T 2
	run_read  "R3 +轮询 fence  (C)"     -H -F 0 -Z
	run_read  "R4 +拆分+fence  (Tutti)" -H -F 0 -T 1 -Z
	run_write "W1 kernel写payload+拆分 (A)"  -I 1 -D 0 -T 1
	run_write "W2 kernel写payload+fence (C)" -I 1 -D 0 -Z
	run_write "W3 kernel写payload+拆分+fence" -I 1 -D 0 -T 1 -Z
	;;
2)
	# 第二轮:刷新点是 kernel 退出还是 host 同步
	run_read  "C3 自检:主存胶囊 -T 3"       -T 3
	run_read  "P3 普通显存胶囊, 写|门铃"     -H -F 0 -T 3
	run_read  "P4 普通显存胶囊, 写|同步|门铃" -H -F 0 -T 4
	run_write "Q3 kernel写payload, 写|门铃"     -I 1 -D 0 -T 3
	run_write "Q4 kernel写payload, 写|同步|门铃" -I 1 -D 0 -T 4
	run_write "Q3H 同 Q3 + 胶囊也在普通显存"    -I 1 -D 0 -T 3 -H -F 0
	;;
3)
	# 第三轮:MR 注册路径会不会改变坑 7。全部单 kernel、无同步
	run_read  "G0 peer_mem 自检:主存胶囊"          -R 1
	run_read  "G1 dma-buf,普通显存胶囊(=R0)"      -H -F 0
	run_read  "G2 peer_mem,普通显存胶囊"           -H -F 0 -R 1
	run_write "G3 dma-buf,kernel写payload"         -I 1 -D 0
	run_write "G4 peer_mem,kernel写payload"        -I 1 -D 0 -R 1
	run_write "G5 peer_mem,payload+胶囊都在普通显存" -I 1 -D 0 -H -F 0 -R 1
	;;
esac

echo
echo "完整日志: $LOG"
