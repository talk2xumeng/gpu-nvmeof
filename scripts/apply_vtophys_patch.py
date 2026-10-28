#!/usr/bin/env python3
# SPDK vtophys 补丁:注册 CPU 不可访问的地址(GPU VA)时不 touch、不失败。
#
#   python3 scripts/apply_vtophys_patch.py <SPDK_DIR>/lib/env_dpdk/memory.c
#   cd <SPDK_DIR> && make -j
#
# 为什么要打:没有 IOMMU 时 spdk_mem_register 会走 pagemap 求物理地址,
# 求不到就用 CPU 读一下这页把它 fault 进来。C600 + MACA 3.8.3 上 mcMalloc
# 的 GPU VA 不对 CPU 映射,这一读直接 SIGSEGV。RDMA 路径只用 rkey 那张映射表,
# 不需要 vtophys,所以对这类地址跳过 vtophys 即可。
# C500 那套环境(老机器)不打也能跑。脚本可重复执行。
import sys

p = sys.argv[1]
s = open(p).read()
if "gpu-nvmeof: safe touch" in s:
    print("已经打过补丁,跳过。"); sys.exit(0)

old_touch = "\t\trte_atomic64_read((rte_atomic64_t *)vaddr);\n"
new_touch = (
    "\t\t/* gpu-nvmeof: safe touch —— CPU 不可访问时返回 EFAULT 而不是 SIGSEGV */\n"
    "\t\tuint64_t tmp;\n"
    "\t\tstruct iovec lio = { &tmp, sizeof(tmp) }, rio = { (void *)vaddr, sizeof(tmp) };\n"
    "\t\tif (process_vm_readv(getpid(), &lio, 1, &rio, 1, 0) != (ssize_t)sizeof(tmp)) {\n"
    "\t\t\treturn SPDK_VTOPHYS_ERROR;\n"
    "\t\t}\n"
)
assert s.count(old_touch) == 1, "找不到 touch 那一行,SPDK 版本不对?"
s = s.replace(old_touch, new_touch)

old_fail = (
    "\t\t\t\tpaddr = vtophys_get_paddr_pagemap((uint64_t)vaddr);\n"
    "\t\t\t\tif (paddr == SPDK_VTOPHYS_ERROR) {\n"
    "\t\t\t\t\tDEBUG_PRINT(\"could not get phys addr for %p\\n\", vaddr);\n"
    "\t\t\t\t\treturn -EFAULT;\n"
    "\t\t\t\t}\n"
)
new_fail = (
    "\t\t\t\tpaddr = vtophys_get_paddr_pagemap((uint64_t)vaddr);\n"
    "\t\t\t\tif (paddr == SPDK_VTOPHYS_ERROR) {\n"
    "\t\t\t\t\t/* gpu-nvmeof: 设备内存(GPU VA)没有物理页,跳过 vtophys,\n"
    "\t\t\t\t\t * 其它 map(如 RDMA rkey)照常注册 */\n"
    "\t\t\t\t\tSPDK_NOTICELOG(\"vtophys: %p 无 CPU 物理页,跳过(设备内存)\\n\", vaddr);\n"
    "\t\t\t\t\treturn 0;\n"
    "\t\t\t\t}\n"
)
assert s.count(old_fail) == 1, "找不到 pagemap 失败分支,SPDK 版本不对?"
s = s.replace(old_fail, new_fail)
if "#include <sys/uio.h>" not in s:
    s = s.replace('#include "spdk/stdinc.h"\n', '#include "spdk/stdinc.h"\n#include <sys/uio.h>\n', 1)
open(p, "w").write(s)
print("memory.c 已打补丁")
