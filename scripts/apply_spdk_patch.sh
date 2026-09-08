#!/bin/bash
set -e
SPDK_DIR="${1:-/home/metaxadmin/mx/spdk}"
HDR="$SPDK_DIR/include/spdk/nvme.h"
SRC="$SPDK_DIR/lib/nvme/nvme_rdma.c"

[ -f "$HDR" ] || { echo "找不到 $HDR"; exit 1; }
[ -f "$SRC" ] || { echo "找不到 $SRC"; exit 1; }

if grep -q "spdk_nvme_qpair_get_ibv_qp" "$HDR"; then
    echo "已经打过补丁,跳过。"; exit 0
fi

cp "$HDR" "$HDR.orig.$(date +%s)"
cp "$SRC" "$SRC.orig.$(date +%s)"

python3 - "$HDR" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
decl = '''
/* 以下由 gpu-nvmeof 添加:暴露底层 verbs 对象给 GPU-initiated 路径 */
struct ibv_qp;
struct ibv_cq;
struct ibv_qp *spdk_nvme_qpair_get_ibv_qp(struct spdk_nvme_qpair *qpair);
struct ibv_cq *spdk_nvme_qpair_get_ibv_cq(struct spdk_nvme_qpair *qpair);
'''
idx = s.rfind('#endif')
if idx == -1:
    sys.exit("找不到收尾 #endif")
open(path,'w').write(s[:idx] + decl + '\n' + s[idx:])
print("  头文件已加声明")
PY

cat >> "$SRC" <<'EOF'

/* ---- gpu-nvmeof 添加 ---- */

struct ibv_qp *
spdk_nvme_qpair_get_ibv_qp(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	if (qpair == NULL || qpair->trtype != SPDK_NVME_TRANSPORT_RDMA) {
		return NULL;
	}
	rqpair = nvme_rdma_qpair(qpair);
	/* cm_id->qp 是 rdma_cm 公开字段,比内部 rdma_qp 稳定 */
	if (rqpair->cm_id != NULL) {
		return rqpair->cm_id->qp;
	}
	return NULL;
}

struct ibv_cq *
spdk_nvme_qpair_get_ibv_cq(struct spdk_nvme_qpair *qpair)
{
	struct nvme_rdma_qpair *rqpair;

	if (qpair == NULL || qpair->trtype != SPDK_NVME_TRANSPORT_RDMA) {
		return NULL;
	}
	rqpair = nvme_rdma_qpair(qpair);
	return rqpair->cq;
}
EOF
echo "  nvme_rdma.c 已加实现"
echo
echo "接下来: cd $SPDK_DIR && make -j\$(nproc)"
