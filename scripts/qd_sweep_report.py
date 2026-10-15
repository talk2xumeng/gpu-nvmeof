#!/usr/bin/env python3
"""汇总 qd_sweep.sh 的日志:qd_sweep_report.py <日志目录>"""

import csv
import glob
import os
import re
import statistics
import sys


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    r = {"ok": False}
    m = re.search(r"^\[运行\] .*? (\d+) 条, 每条 (\d+) 字节, QD (\d+)", text, re.M)
    if m:
        r["n"], r["bs"], r["qd_eff"] = int(m.group(1)), int(m.group(2)), int(m.group(3))
    m = re.search(r"^\[完成\] 响应 (\d+) / (\d+), NVMe 成功 (\d+)", text, re.M)
    if m:
        r["ok"] = m.group(1) == m.group(2) == m.group(3)
    if "*** FAIL ***" in text or "对不上" in text or "没补齐" in text:
        r["ok"] = False
    m = re.search(r"kernel \d+ cycles / (\d+) us", text)
    if m and r.get("n"):
        us = int(m.group(1))
        r["kernel_us"] = us
        r["iops"] = r["n"] / us * 1e6
        r["mbps"] = r["iops"] * r["bs"] / 1e6
    for line in text.splitlines():
        p = line.split()
        if len(p) == 6 and p[0] in ("总计", "等待", "提交"):
            try:
                r[{"总计": "total", "等待": "wait", "提交": "sub"}[p[0]]] = float(p[3])
            except ValueError:
                pass
    m = re.search(r"处理一条响应 (\d+) ns", text)
    if m:
        r["resp_ns"] = int(m.group(1))
    m = re.search(r"每圈 (\d+) ns, 处理一条响应", text)
    if m:
        r["spin_ns"] = int(m.group(1))
    return r


def main():
    out = sys.argv[1]
    rows = []
    for path in sorted(glob.glob(os.path.join(out, "*.log"))):
        m = re.match(r"o(\w+)_b(\d+)_q(\d+)_(\d+)\.log$", os.path.basename(path))
        if not m:
            continue
        r = parse(path)
        r.update(opt=m.group(1), size=int(m.group(2)), qd=int(m.group(3)),
                 rep=int(m.group(4)))
        rows.append(r)

    keys = ["opt", "size", "qd", "qd_eff", "rep", "ok", "kernel_us", "iops",
            "mbps", "total", "sub", "wait", "resp_ns", "spin_ns"]
    with open(os.path.join(out, "results.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    groups = {}
    for r in rows:
        groups.setdefault((r["opt"], r["size"], r["qd"]), []).append(r)

    def med(rs, k):
        v = [x[k] for x in rs if x["ok"] and k in x]
        return statistics.median(v) if v else None

    def f(x, nd=1):
        return "-" if x is None else f"{x:,.{nd}f}"

    print("# QD 扫描\n")
    print("多次运行取中位数;延迟为 kernel 内 p50(us),处理一条响应为 ns。\n")
    print("| -O | 大小 | QD | 实际 QD | 有效 | IOPS | MB/s | 总计 p50 | 提交 p50 | 等待 p50 | 处理一条响应 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for (opt, size, qd) in sorted(groups, key=lambda k: (k[0], k[1], k[2])):
        rs = groups[(opt, size, qd)]
        good = sum(1 for x in rs if x["ok"])
        qe = rs[0].get("qd_eff", "-")
        print(f"| {opt} | {size} | {qd} | {qe} | {good}/{len(rs)} | "
              f"{f(med(rs, 'iops'), 0)} | {f(med(rs, 'mbps'), 0)} | "
              f"{f(med(rs, 'total'))} | {f(med(rs, 'sub'))} | {f(med(rs, 'wait'))} | "
              f"{f(med(rs, 'resp_ns'), 0)} |")
    bad = [r for r in rows if not r["ok"]]
    if bad:
        print("\n失败的运行(不计入):")
        for r in bad:
            print(f"- -O {r['opt']} {r['size']} QD {r['qd']} #{r['rep']}")


if __name__ == "__main__":
    main()
