#!/usr/bin/env python3
"""Credit-loop throughput rule.

One flow crosses one switch-to-switch link of latency L under the VC0 model
with PCIe credits. In steady state the sender keeps at most `window` flits in
the link's credit loop, so

    throughput = min(1, window / RTT),   RTT = 2 L + switch_latency + c

with c a few cycles of pipeline, the same for every point. `window` is the
whole TLPs the binding credit admits times flits per TLP: data credits with
full 256 B TLPs, or header credits with 64 B TLPs.
Throughput is measured as extra flits / extra cycles between a 1 MiB and a
2 MiB flow, which removes the startup latency.

    tests/credit_rule.py --bin src/tools/booksim_flowtest
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

BASE = """topology = pcinet;
network_file = {topo};
routing_function = min;
sim_type = flow;
output_buffer_size = 4;
flit_bytes = 64;
pcie_mps = {mps};
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
speculative = 1;
noq = 1;
noq_vcs_per_output = 1;
num_vcs = 6;
write_request_begin_vc = 0; write_request_end_vc = 1;
read_request_begin_vc = 2; read_request_end_vc = 3;
read_reply_begin_vc = 4; read_reply_end_vc = 5;
pcie_default_class = P;
buffer_policy = shared;
pcie_credits = 1;
pcie_ph = {ph}; pcie_pd_bytes = {pd};
pcinet_switch_latency = {sw};
"""


def run_point(args, tmp, latency, ph, pd, mps, sw):
    tag = "L%d_ph%d_pd%d_mps%d_sw%d" % (latency, ph, pd, mps, sw)
    topo = os.path.join(tmp, tag + ".txt")
    with open(topo, "w") as f:
        f.write("router 0 node 0 lat 1\nrouter 1 node 1 lat 1\n"
                "router 0 router 1 lat %d gen 5 width 16\n" % latency)
    cfg = os.path.join(tmp, tag + ".cfg")
    with open(cfg, "w") as f:
        f.write(BASE.format(topo=topo, mps=mps, ph=ph, pd=pd, sw=sw))
    times = []
    for mib in (1, 2):
        trace = os.path.join(tmp, "flow%d.trace" % mib)
        out = subprocess.run([args.bin, cfg, trace], capture_output=True, text=True,
                             timeout=args.timeout, cwd=ROOT).stdout
        m = re.search(r"^makespan\s+(\d+)", out, re.M)
        if not m or "all returned" not in out or not out.rstrip().endswith("OK"):
            return tag, None
        times.append(int(m.group(1)))
    flits_per_tlp = (mps + 24 + 63) // 64
    extra_flits = (1 << 20) // mps * flits_per_tlp
    return tag, extra_flits / float(times[1] - times[0])


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join(ROOT, "src", "tools", "booksim_flowtest"))
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--tolerance", type=float, default=0.03)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)

    points = []   # (latency, ph, pd, mps, sw, window flits)
    for lat in (2, 25, 100, 250):
        for tlps in (6, 25, 51, 102):          # ~32/128/256/512 flits of window
            points.append((lat, 0, tlps * 256, 256, 0, tlps * 5))
    for sw in (25, 100):                       # switch latency on the data channel
        for tlps in (6, 25, 51):
            points.append((100, 0, tlps * 256, 256, sw, tlps * 5))
    for hdr in (8, 32, 64, 128):               # header-bound: 64 B TLPs, 2 flits each
        points.append((100, hdr, 0, 64, 0, hdr * 2))

    with tempfile.TemporaryDirectory() as tmp:
        for mib in (1, 2):
            with open(os.path.join(tmp, "flow%d.trace" % mib), "w") as f:
                f.write("0 0 1 %d\n" % (mib << 20))
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
            futs = [ex.submit(run_point, args, tmp, *p[:5]) for p in points]
            results = [f.result() for f in futs]

    bad = [tag for tag, thr in results if thr is None]
    if bad:
        print("runs failed:", bad)
        return 1

    # One pipeline constant for all credit-bound points.
    cs = sorted(w / thr - (2 * lat + sw)
                for (lat, ph, pd, mps, sw, w), (_, thr) in zip(points, results) if thr < 0.9)
    c = cs[len(cs) // 2]
    print("fitted pipeline constant c = %.1f cycles (RTT = 2 L + switch latency + c)" % c)
    fail = 0
    print("%-28s %7s %9s %9s" % ("point", "window", "measured", "predicted"))
    for (lat, ph, pd, mps, sw, w), (tag, thr) in zip(points, results):
        pred = min(1.0, w / (2.0 * lat + sw + c))
        ok = abs(thr - pred) <= args.tolerance * pred
        fail += not ok
        print("%-28s %7d %8.1f%% %8.1f%%  %s" % (tag, w, 100 * thr, 100 * pred,
                                                "ok" if ok else "OFF"))
    if not 0 <= c <= 20:
        print("pipeline constant outside 0..20 cycles")
        fail += 1
    print("%d points, %d off by more than %.0f%%" % (len(points), fail, 100 * args.tolerance))
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
