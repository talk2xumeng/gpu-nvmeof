#!/usr/bin/env python3
"""
汇总 kv_vs_nvm.sh 的日志:每次运行取 kernel 内延迟分段的 p50,
同一 (操作, 大小) 的多次运行再取中位数,算出 KV - NVM 的差值。

    kv_vs_nvm_report.py <日志目录> <每次条数>

只解析 gpu_initiated 自己打印的几行,格式变了这里要跟着改:
    [完成] 响应 a / b, NVMe 成功 c, ...
      总计 / 胶囊 / 提交 / 等待   mean min p50 p90 p99   (us)
    poller: ... 处理一条响应 N ns   或   自轮询: ... 处理一条响应 N ns
    *** FAIL ***(校验失败)
"""

import csv
import glob
import os
import re
import statistics
import sys

SEGS = {"总计": "total", "胶囊": "cap", "提交": "sub", "等待": "wait"}
OPS = ["read", "retrieve", "write", "store", "exist"]
PAIRS = [("retrieve", "read"), ("store", "write")]


def parse(path, rounds):
    r = {"ok": False}
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    m = re.search(r"^\[完成\] 响应 (\d+) / (\d+), NVMe 成功 (\d+)", text, re.M)
    if m:
        r["ok"] = int(m.group(3)) == rounds and int(m.group(1)) == rounds
    if "*** FAIL ***" in text:
        r["ok"] = False
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 6 and parts[0] in SEGS:
            try:
                r[SEGS[parts[0]]] = float(parts[3])	# p50
            except ValueError:
                pass
    m = re.search(r"处理一条响应 (\d+) ns", text)
    if m:
        r["resp_ns"] = float(m.group(1))
    return r


def main():
    out, rounds = sys.argv[1], int(sys.argv[2])
    rows = []
    for path in sorted(glob.glob(os.path.join(out, "*.log"))):
        m = re.match(r"(\w+)_(\d+)_(\d+)\.log$", os.path.basename(path))
        if not m or m.group(1) not in OPS:
            continue
        r = parse(path, rounds)
        r.update(op=m.group(1), size=int(m.group(2)), rep=int(m.group(3)))
        rows.append(r)

    keys = ["op", "size", "rep", "ok", "total", "cap", "sub", "wait", "resp_ns"]
    with open(os.path.join(out, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    # (op, size) -> 各字段中位数;失败的运行不计入
    agg, bad = {}, {}
    for r in rows:
        k = (r["op"], r["size"])
        if not r["ok"] or "total" not in r:
            bad[k] = bad.get(k, 0) + 1
            continue
        agg.setdefault(k, []).append(r)

    def med(k, field):
        v = [r[field] for r in agg.get(k, []) if field in r]
        return statistics.median(v) if v else None

    def fmt(x, nd=1):
        return "-" if x is None else f"{x:.{nd}f}"

    sizes = sorted({r["size"] for r in rows})
    print(f"# KV vs NVM,QD 1,每次 {rounds} 条\n")
    print("各次运行 p50 的中位数,单位 us(处理一条响应为 ns)。\n")
    print("| 大小 | 操作 | 有效次数 | 总计 | 胶囊 | 提交 | 等待 | 处理一条响应 |")
    print("|---|---|---|---|---|---|---|---|")
    for s in sizes:
        for op in OPS:
            k = (op, s)
            n = len(agg.get(k, []))
            if n == 0 and k not in bad:
                continue
            nb = f"{n}" + (f"(失败 {bad[k]})" if k in bad else "")
            print(f"| {s} | {op} | {nb} | {fmt(med(k, 'total'))} | "
                  f"{fmt(med(k, 'cap'))} | {fmt(med(k, 'sub'))} | "
                  f"{fmt(med(k, 'wait'))} | {fmt(med(k, 'resp_ns'), 0)} |")

    print("\n## KV - NVM\n")
    print("| 大小 | 对比 | 总计 Δ | 等待 Δ | 处理一条响应 Δ (ns) |")
    print("|---|---|---|---|---|")
    for s in sizes:
        for kv, nvm in PAIRS:
            a, b = (kv, s), (nvm, s)
            if a not in agg or b not in agg:
                continue

            def d(field):
                x, y = med(a, field), med(b, field)
                return None if x is None or y is None else x - y

            print(f"| {s} | {kv} - {nvm} | {fmt(d('total'))} | "
                  f"{fmt(d('wait'))} | {fmt(d('resp_ns'), 0)} |")

    ex = [k for k in agg if k[0] == "exist"]
    if ex:
        k = ex[0]
        print(f"\nExist(无数据往返基线):总计 {fmt(med(k, 'total'))},"
              f"等待 {fmt(med(k, 'wait'))} us")
    if bad:
        print("\n失败的运行没有计入,日志里搜 `[完成]` 和 `FAIL`:")
        for (op, s), n in sorted(bad.items()):
            print(f"- {op} {s}: {n} 次")


if __name__ == "__main__":
    main()
