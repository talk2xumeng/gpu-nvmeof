#!/bin/bash
# 给 target 侧 SPDK 的 nvmf 打补丁:kv_emu bdev 的 namespace,所有 I/O 命令原样透传。
#
# 为什么必须打:KV 的 Store=01h、Retrieve=02h 和 NVM 的 WRITE/READ 是同一个
# opcode,nvmf_ctrlr_process_io_cmd 会先按 READ/WRITE 分发,把 CDW10/11 当
# SLBA 解析 —— 根本到不了 bdev。只有不认识的 opcode(Delete/Exist/List)
# 才会走透传。补丁按 bdev 模块名判断,别的 bdev 的行为一字不变。
#
#   ./apply_kv_patch.sh [SPDK_DIR]
#   cd $SPDK_DIR && make -j$(nproc)
set -e
SPDK_DIR="${1:-${SPDK_DIR:-/home/metaxadmin/mx/spdk}}"
SRC="$SPDK_DIR/lib/nvmf/ctrlr.c"

[ -f "$SRC" ] || { echo "找不到 $SRC"; exit 1; }

if grep -q "gpu-nvmeof kv_emu" "$SRC"; then
    echo "已经打过补丁,跳过。"; exit 0
fi

cp "$SRC" "$SRC.orig.$(date +%s)"

python3 - "$SRC" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
anchor = '''	if (spdk_nvmf_request_using_zcopy(req)) {
		assert(req->zcopy_phase == NVMF_ZCOPY_PHASE_INIT);
		return nvmf_bdev_ctrlr_zcopy_start(bdev, desc, ch, req);'''
if s.count(anchor) != 1:
    sys.exit("找不到插入点(nvmf_ctrlr_process_io_cmd 的 zcopy 分支),SPDK 版本不同?本补丁按 v24.09 写")
patch = '''	/* gpu-nvmeof kv_emu:KV 模拟 namespace 的 I/O 全部原样透传,
	 * 不能按 READ/WRITE 解析(KV Store/Retrieve 与之同 opcode)。见 tst/kv_target */
	if (spdk_unlikely(strcmp(spdk_bdev_get_module_name(bdev), "kv_emu") == 0)) {
		return nvmf_bdev_ctrlr_nvme_passthru_io(bdev, desc, ch, req);
	}

'''
s = s.replace(anchor, patch + anchor)
open(path, 'w').write(s)
print("  ctrlr.c 已加 kv_emu 透传分支")
PY

echo
echo "接下来: cd $SPDK_DIR && make -j\$(nproc)"
