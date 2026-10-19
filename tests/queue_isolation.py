#!/usr/bin/env python3
"""Queue isolation vs. shared-credit backpressure.

Switch S holds GPUs A1, A2; switch R holds B and C. A1 -> B and A2 -> C share
the S->R link and its Posted credits. B's link runs at 1/16 rate, so egress B
backs up at R.

  1. Pool available (B sends less than S->R's Posted pool holds): with one
     queue per egress at R's ingress, C is not held up by B; with one
     FIFO per type, C waits behind B's TLPs at the head of the queue.
  2. Pool exhausted (B sends far more): B's backlog takes the shared Posted
     credits of S->R, so new C TLPs wait too, even with C's queue empty. This
     is expected shared-credit backpressure, as in real PCIe. C must still
     complete, and every credit must come back.
  3. Recovery: with a B flow that exhausts the pool but ends early, C's
     admission resumes once credits return: one more MiB of C, sent after B
     has drained, takes line-rate time.

    tests/queue_isolation.py --bin src/tools/booksim_flowtest
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

TOPO = """router 0 node 0 lat 1
router 0 node 3 lat 1
router 1 node 1 lat 1 cpf 16
router 1 node 2 lat 1
router 0 router 1 lat 2
"""

CFG = """topology = pcinet;
network_file = {topo};
routing_function = min;
sim_type = flow;
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
speculative = 1;
pcie_default_class = P;
buffer_policy = shared;
pcie_credits = 1;
pcie_ph = {ph}; pcie_pd_bytes = 0;
"""

# Per-egress queues: one queue per (type, egress of the next switch); radix 3.
PER_EGRESS = """noq = 1;
noq_vcs_per_output = 1;
num_vcs = 9;
write_request_begin_vc = 0; write_request_end_vc = 2;
read_request_begin_vc = 3; read_request_end_vc = 5;
read_reply_begin_vc = 6; read_reply_end_vc = 8;
"""
# One FIFO per type.
FIFO = """num_vcs = 3;
write_request_begin_vc = 0; write_request_end_vc = 0;
read_request_begin_vc = 1; read_request_end_vc = 1;
read_reply_begin_vc = 2; read_reply_end_vc = 2;
"""


def run(args, tmp, name, model, ph, flows):
    topo = os.path.join(tmp, "topo.txt")
    cfg = os.path.join(tmp, name + ".cfg")
    with open(cfg, "w") as f:
        f.write(CFG.format(topo=topo, ph=ph) + model)
    trace = os.path.join(tmp, name + ".trace")
    with open(trace, "w") as f:
        for i, (src, dst, size) in enumerate(flows):
            f.write("%d %d %d %d\n" % (i, src, dst, size))
    csv_path = os.path.join(tmp, name + ".csv")
    out = subprocess.run([args.bin, cfg, trace, "--csv", csv_path], capture_output=True,
                         text=True, timeout=args.timeout, cwd=ROOT).stdout
    if not out.rstrip().endswith("OK") or "all returned" not in out:
        print(out[-2000:])
        sys.exit("%s: run failed" % name)
    done = {}
    for row in csv.DictReader(open(csv_path)):
        done[(int(row["src"]), int(row["dst"]))] = int(row["t_done"])
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", default=os.path.join(ROOT, "src", "tools", "booksim_flowtest"))
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)

    ph = 32                       # S->R Posted pool: 32 TLPs
    c_flow = (3, 2, 1 << 20)      # A2 -> C, 1 MiB
    small_b = (0, 1, 24 * 256)    # A1 -> B, 24 TLPs: fits in the pool
    big_b = (0, 1, 1 << 20)       # A1 -> B, 4096 TLPs: exhausts it
    fails = 0

    with tempfile.TemporaryDirectory() as tmp:
        with open(os.path.join(tmp, "topo.txt"), "w") as f:
            f.write(TOPO)
        alone = run(args, tmp, "alone", PER_EGRESS, ph, [c_flow])[(3, 2)]
        b_alone = run(args, tmp, "b_alone", PER_EGRESS, ph, [small_b])[(0, 1)]

        a_small = run(args, tmp, "a_small", PER_EGRESS, ph, [small_b, c_flow])
        f_small = run(args, tmp, "f_small", FIFO, ph, [small_b, c_flow])
        a_big = run(args, tmp, "a_big", PER_EGRESS, ph, [big_b, c_flow])
        mid_b = (0, 1, 256 * 256)     # 256 TLPs: exhausts the pool, drains in ~20k cycles
        r1 = run(args, tmp, "rec1", PER_EGRESS, ph, [mid_b, c_flow])
        r2 = run(args, tmp, "rec2", PER_EGRESS, ph, [mid_b, (3, 2, 2 << 20)])

    print("C alone (1 MiB):                        %7d cycles" % alone)
    print("B alone (24 TLPs at 1/16 rate):          %7d cycles" % b_alone)
    print("1. pool available, per-egress: C done at %7d (+%.1f%%)"
          % (a_small[(3, 2)], 100.0 * (a_small[(3, 2)] - alone) / alone))
    print("   pool available, one FIFO:  C done at %7d (+%.1f%%)"
          % (f_small[(3, 2)], 100.0 * (f_small[(3, 2)] - alone) / alone))
    print("2. pool exhausted, per-egress: C done at %7d (+%.1f%%), B done at %d"
          % (a_big[(3, 2)], 100.0 * (a_big[(3, 2)] - alone) / alone, a_big[(0, 1)]))

    extra = r2[(3, 2)] - r1[(3, 2)]
    print("3. recovery: B (256 TLPs) done at %d; C 1 MiB done at %d, 2 MiB at %d: "
          "the second MiB took %d cycles (line rate 20480)"
          % (r1[(0, 1)], r1[(3, 2)], r2[(3, 2)], extra))

    # 1: per-egress queues keep C within 3% of running alone; the FIFO delays C by at
    # least half of B's drain time.
    if a_small[(3, 2)] > 1.03 * alone:
        print("FAIL: per-egress queues delayed C while pool space was available")
        fails += 1
    if f_small[(3, 2)] - alone < 0.5 * b_alone:
        print("FAIL: the FIFO did not show head-of-line blocking")
        fails += 1
    # 2: shared credits throttle C well below its own rate while B's backlog
    # holds the pool, and C finishes before B (it recovers once it can get
    # credits, which B returns only at 1/16 rate).
    if a_big[(3, 2)] < 1.5 * alone:
        print("FAIL: no shared-credit backpressure on C")
        fails += 1
    if a_big[(3, 2)] >= a_big[(0, 1)]:
        print("FAIL: C did not finish before the slow B flow")
        fails += 1
    # 3: after B has drained, C runs at line rate again.
    if r1[(3, 2)] <= r1[(0, 1)] or abs(extra - 20480) > 0.03 * 20480:
        print("FAIL: C did not recover full rate after B drained")
        fails += 1
    print("OK" if not fails else "%d checks failed" % fails)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
