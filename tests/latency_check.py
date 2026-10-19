#!/usr/bin/env python3
"""Check the per-TLP latency and switch-count statistics (tlp_* lines).

A chain of N = 1..4 switches carries one TLP from G0 (first switch) to G1
(last switch), alone in the network:

  1. tlp_count is 1, and tlp_switches_mean and the CSV's mean_hops are both N.
  2. Total latency (creation to tail ejection) differs from the flow's
     completion time (t_done - t_inject) by the same small constant for
     every N: two independent measurements of one event.
  3. Network latency grows by the same amount per added switch.
  4. A 256 B TLP (5 flits) takes exactly 4 cycles longer than a 40 B TLP
     (1 flit): network latency runs from the head's injection to the tail's
     ejection, so it includes the tail's serialization.

Then two GPUs send 64 KiB each to a third on one switch (incast): every TLP
is counted, and queueing shows (mean above zero-load, p99 >= p50 >= minimum).

    tests/latency_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

CFG = """topology = pcinet;
network_file = {topo};
routing_function = min;
sim_type = flow;
num_vcs = 4;
vc_buf_size = 32;
output_buffer_size = 4;
flit_bytes = 64;
pcie_mps = 256;
pcie_tlp_overhead_bytes = 24;
max_queued_packets = 8;
credit_delay = 2;
routing_delay = 0;
vc_alloc_delay = 1;
sw_alloc_delay = 1;
st_final_delay = 1;
vc_allocator = islip;
sw_allocator = islip;
alloc_iters = 2;
"""


def chain(n):
    lines = ["router 0 node 0 lat 1", "router %d node 1 lat 1" % (n - 1)]
    lines += ["router %d router %d lat 2" % (i, i + 1) for i in range(n - 1)]
    return "\n".join(lines) + "\n"


INCAST = """router 0 node 0 lat 1
router 0 node 1 lat 1
router 0 node 2 lat 1
"""


def run(args, tmp, name, topo, trace_lines):
    topo_path = os.path.join(tmp, name + ".txt")
    cfg_path = os.path.join(tmp, name + ".cfg")
    trace_path = os.path.join(tmp, name + ".trace")
    csv_path = os.path.join(tmp, name + ".csv")
    with open(topo_path, "w") as f:
        f.write(topo)
    with open(cfg_path, "w") as f:
        f.write(CFG.format(topo=topo_path))
    with open(trace_path, "w") as f:
        f.write("\n".join(trace_lines) + "\n")
    out = subprocess.run([args.bin, cfg_path, trace_path, "--csv", csv_path],
                         cwd=ROOT, capture_output=True, text=True, timeout=args.timeout)
    if out.returncode != 0:
        sys.exit("FAIL: %s exited %d\n%s" % (name, out.returncode, out.stdout[-2000:]))
    stats = {}
    for key, val in re.findall(r"^(tlp_\w+)\s+(\S+)", out.stdout, re.M):
        stats[key] = float(val)
    with open(csv_path, newline="") as f:
        rows = list(csv.DictReader(f))
    return stats, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=60)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)

    fails = []
    net = {}
    offsets = set()
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(1, 5):
            for nbytes in (40, 256):
                s, rows = run(args, tmp, "chain%d_%d" % (n, nbytes), chain(n),
                              ["0 0 1 %d" % nbytes])
                if s.get("tlp_count") != 1:
                    fails.append("chain %d, %d B: tlp_count %s" % (n, nbytes, s.get("tlp_count")))
                    continue
                if s["tlp_switches_mean"] != n or float(rows[0]["mean_hops"]) != n:
                    fails.append("chain %d: switches %s, CSV mean_hops %s"
                                 % (n, s["tlp_switches_mean"], rows[0]["mean_hops"]))
                fct = int(rows[0]["t_done"]) - int(rows[0]["t_inject"])
                offsets.add(fct - int(s["tlp_lat_total_max"]))
                net[(n, nbytes)] = int(s["tlp_lat_net_max"])
                print("chain of %d switch(es), %3d B TLP: network %3d, total %3d, flow %3d cycles"
                      % (n, nbytes, net[(n, nbytes)], s["tlp_lat_total_max"], fct))

        if len(offsets) != 1 or not offsets <= {0, 1}:
            fails.append("total latency vs flow completion offsets %s, want one value in {0, 1}"
                         % sorted(offsets))
        steps = set(net[(n, 256)] - net[(n - 1, 256)] for n in range(2, 5))
        if len(steps) != 1:
            fails.append("network latency per added switch not constant: %s" % sorted(steps))
        for n in range(1, 5):
            if net[(n, 256)] - net[(n, 40)] != 4:
                fails.append("chain %d: 256 B minus 40 B network latency is %d, want 4"
                             % (n, net[(n, 256)] - net[(n, 40)]))

        s, _ = run(args, tmp, "incast", INCAST, ["0 0 2 65536", "1 1 2 65536"])
        zero = net[(1, 256)]
        print("incast: %d TLPs, network mean %.1f, p50 %d, p99 %d, max %d (zero-load %d)"
              % (s["tlp_count"], s["tlp_lat_net_mean"], s["tlp_lat_net_p50"],
                 s["tlp_lat_net_p99"], s["tlp_lat_net_max"], zero))
        if s["tlp_count"] != 2 * 65536 // 256:
            fails.append("incast: tlp_count %s, want %d" % (s["tlp_count"], 2 * 65536 // 256))
        if not (s["tlp_lat_net_mean"] > zero and
                s["tlp_lat_net_max"] >= s["tlp_lat_net_p99"] >= s["tlp_lat_net_p50"] >= zero):
            fails.append("incast: latency order wrong")

    for f in fails:
        print("FAIL: " + f)
    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
