#!/usr/bin/env python3
"""Run a topology x routing x workload matrix through booksim_flowtest.

Each case runs as its own subprocess (booksim::Engine is one per process) with
a timeout, so a deadlocked case can never be left running. Results land in
--out as <case>.log / <case>.csv plus summary.tsv, which tests/compare.py
diffs against another run.

Examples (from the booksim2 root):
  tests/run_matrix.py --bin src/tools/booksim_flowtest --out /tmp/m_new
  tests/run_matrix.py --bin src/tools/booksim_flowtest --out /tmp/m_pin \
      --routings pinned --set multipath_granularity=flow
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GEN = os.path.join(ROOT, "utils", "gen_collective.py")

TOPOS = ["flat", "dual_tree", "ntb_tree", "mesh_grid", "ring_torus"]
ROUTINGS = ["min", "ecmp", "adaptive"]
WORKLOADS = ["ring", "a2a", "a2a_shift", "a2a_xor"]

# mesh_grid's GPU numbering differs from the ring's neighbor order, so its ring
# runs with the placement that keeps every ring hop on a direct link.
MESH_RING_ORDER = "0,1,2,3,6,7,4,5"


def config_for(topo):
    """A topology name maps to runfiles/simai_<topo>.cfg; anything ending in
    .cfg is taken as a path."""
    if topo.endswith(".cfg"):
        return os.path.abspath(topo)
    return os.path.join(ROOT, "runfiles", "simai_%s.cfg" % topo)


def topo_label(topo):
    return os.path.splitext(os.path.basename(topo))[0] if topo.endswith(".cfg") else topo


def make_traces(trace_dir, ranks, size):
    os.makedirs(trace_dir, exist_ok=True)
    specs = {
        "ring": ["-a", "ring"],
        "ring_mesh": ["-a", "ring", "--order", MESH_RING_ORDER],
        "a2a": ["-a", "alltoall", "--a2a-order", "naive"],
        "a2a_shift": ["-a", "alltoall", "--a2a-order", "shift"],
        "a2a_xor": ["-a", "alltoall", "--a2a-order", "xor"],
    }
    paths = {}
    for name, extra in specs.items():
        if name == "ring_mesh" and ranks != 8:
            continue
        path = os.path.join(trace_dir, name + ".trace")
        subprocess.run([sys.executable, GEN, "-n", str(ranks), "-s", str(size),
                        "-o", path] + extra, check=True, stderr=subprocess.DEVNULL)
        paths[name] = path
    return paths


def trace_for(paths, topo, workload):
    if workload == "ring" and topo_label(topo).endswith("mesh_grid") and "ring_mesh" in paths:
        return paths["ring_mesh"]
    return paths[workload]


def run_case(args, topo, routing, workload, trace):
    case = "%s.%s.%s" % (topo_label(topo), routing, workload)
    if args.tag:
        case += "." + args.tag
    log = os.path.join(args.out, case + ".log")
    csv = os.path.join(args.out, case + ".csv")
    cmd = [args.bin, config_for(topo), trace, "--csv", csv,
           "routing_function=" + routing] + args.set
    try:
        with open(log, "w") as fh:
            proc = subprocess.run(cmd, cwd=ROOT, stdout=fh, stderr=subprocess.STDOUT,
                                  timeout=args.timeout)
        rc = proc.returncode
    except subprocess.TimeoutExpired:
        rc = "timeout"
    text = open(log, errors="replace").read()
    m = re.search(r"^makespan\s+(\d+)", text, re.M)
    makespan = m.group(1) if m else "-"
    if rc == "timeout":
        status = "HANG"
    elif "routing deadlock" in text:
        status = "DEADLOCK"
    elif rc != 0:
        status = "FAIL(rc=%s)" % rc
    else:
        status = "OK"
    m = re.search(r"graph is (\w+)", text)
    cdg = m.group(1) if m else "-"
    extra = {}
    if re.search(r"^pcie credits\s+NOT RETURNED", text, re.M):
        status = "CREDITS"
    for key in ("lf_units", "lf_waited_units", "lf_wait_cycles", "lf_max_wait",
                "lf_alt_path"):
        m = re.search(r"^%s\s+(\d+)" % key, text, re.M)
        if m:
            extra[key] = m.group(1)
    return case, status, makespan, cdg, extra


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True, help="booksim_flowtest binary")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--topos", default=",".join(TOPOS),
                    help="comma list of runfiles/simai_<name>.cfg names or .cfg paths")
    ap.add_argument("--routings", default=",".join(ROUTINGS))
    ap.add_argument("--workloads", default=",".join(WORKLOADS))
    ap.add_argument("--set", action="append", default=[],
                    help="config override key=value, repeatable")
    ap.add_argument("--tag", default="", help="suffix for case names")
    ap.add_argument("--ranks", type=int, default=8)
    ap.add_argument("--size", type=int, default=1 << 20, help="bytes per rank")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 2))
    ap.add_argument("--timeout", type=int, default=300, help="seconds per case")
    args = ap.parse_args()

    args.bin = os.path.abspath(args.bin)
    args.out = os.path.abspath(args.out)
    os.makedirs(args.out, exist_ok=True)
    paths = make_traces(os.path.join(args.out, "traces"), args.ranks, args.size)

    cases = [(t, r, w) for t in args.topos.split(",") for r in args.routings.split(",")
             for w in args.workloads.split(",")]
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_case, args, t, r, w, trace_for(paths, t, w))
                for t, r, w in cases]
        for fut in futs:
            results.append(fut.result())

    bad = 0
    with open(os.path.join(args.out, "summary.tsv"), "a") as fh:
        for case, status, makespan, cdg, extra in results:
            ex = " ".join("%s=%s" % kv for kv in sorted(extra.items()))
            line = "%-40s %-10s %10s %-8s %s" % (case, status, makespan, cdg, ex)
            print(line.rstrip())
            fh.write("\t".join([case, status, makespan, cdg, ex]) + "\n")
            if status != "OK":
                bad += 1
    print("%d cases, %d not OK" % (len(results), bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
