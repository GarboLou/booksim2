#!/usr/bin/env python3
"""Per-case fabric metrics from tests/run_matrix.py (or booksim_flowtest)
output directories: each <case>.csv (per-flow) with its <case>.log.

  utils/fabric_metrics.py DIR [DIR ...] [--tsv out.tsv]

Columns (1 cycle = 1 ns, so B/cycle = GB/s):
  agg_gbps      payload bytes of all flows / time the last byte arrived
  gpu_gbps_mean per sending GPU: its payload bytes / (its last delivery -
  gpu_gbps_min  its first flow's start); mean and slowest GPU
  switches      switches a TLP passes through, averaged over all TLPs
                (1 = sender and receiver on the same switch)
  lat_mean/p50/p99  per-TLP network latency (head injected to tail
                ejected), from the log's tlp_lat_net_* lines
"""

import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict


def log_stats(path):
    stats = {}
    status = "OK"
    try:
        text = open(path, errors="replace").read()
    except OSError:
        return {}, "NOLOG"
    for key, val in re.findall(r"^(tlp_\w+|makespan)\s+(\S+)", text, re.M):
        stats[key] = float(val)
    if "routing deadlock" in text:
        status = "DEADLOCK"
    elif re.search(r"^pcie credits\s+NOT RETURNED", text, re.M):
        status = "CREDITS"
    elif not re.search(r"^OK$", text, re.M):
        status = "FAIL"
    return stats, status


def case_metrics(csv_path):
    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    total = sum(int(r["bytes"]) for r in rows)
    end = max(int(r["t_done"]) for r in rows)
    per_src = defaultdict(lambda: [0, None, 0])     # bytes, first start, last done
    pk = sw = 0.0
    for r in rows:
        s = per_src[int(r["src"])]
        s[0] += int(r["bytes"])
        t0 = int(r["t_inject"])
        s[1] = t0 if s[1] is None else min(s[1], t0)
        s[2] = max(s[2], int(r["t_done"]))
        pk += int(r["packets"])
        sw += float(r["mean_hops"]) * int(r["packets"])
    gpu = [b / float(done - start) for b, start, done in per_src.values() if done > start]
    return {
        "flows": len(rows),
        "bytes": total,
        "makespan": end,
        "agg_gbps": total / float(end) if end else 0.0,
        "gpu_gbps_mean": sum(gpu) / len(gpu) if gpu else 0.0,
        "gpu_gbps_min": min(gpu) if gpu else 0.0,
        "switches": sw / pk if pk else 0.0,
    }


COLS = ["case", "status", "flows", "bytes", "makespan", "agg_gbps", "gpu_gbps_mean",
        "gpu_gbps_min", "switches", "lat_mean", "lat_p50", "lat_p99", "lat_max"]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--tsv", help="also write the table here")
    args = ap.parse_args()

    out = []
    for d in args.dirs:
        for log in sorted(glob.glob(os.path.join(d, "*.log"))):
            case = os.path.basename(log)[:-4]
            stats, status = log_stats(log)
            row = {"case": case, "status": status}
            csv_path = log[:-4] + ".csv"
            if status == "OK" and os.path.exists(csv_path):
                m = case_metrics(csv_path)
                if m:
                    row.update(m)
            for k in ("mean", "p50", "p99", "max"):
                if "tlp_lat_net_" + k in stats:
                    row["lat_" + k] = stats["tlp_lat_net_" + k]
            out.append(row)

    def fmt(v):
        return "%.2f" % v if isinstance(v, float) else str(v)

    lines = ["\t".join(COLS)]
    for row in out:
        lines.append("\t".join(fmt(row.get(c, "-")) for c in COLS))
    text = "\n".join(lines) + "\n"
    sys.stdout.write(text)
    if args.tsv:
        with open(args.tsv, "w") as f:
            f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
