#!/usr/bin/env python3
"""Cross-socket multipath on the 2-switch dual-tree with an NTB
(tests/topo/dual2C_ntb.cfg: UPI at 32 GB/s, NTB at 64 GB/s).

G0-G3 (socket 0) each send 1.5 MiB to G4-G7 (socket 1), pinned per 4 KiB
request:

  1. shortest (default): every request takes the NTB (mp_units_by_key 1536).
  2. per_bridge + round_robin: NTB and UPI alternate, 768 / 768.
  3. per_bridge + weighted: 2:1 by link speed, exactly 1024 / 512, and the
     run beats the NTB-only run by at least 30% (ideal 1.5x: 64 + 32 GB/s).
  4. Every run completes, with credits back where the credit model is on.

    tests/dual_socket_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CFG = "tests/topo/dual2C_ntb.cfg"
FLOW_BYTES = 1536 * 1024


def run(args, tmp, name, overrides):
    trace = os.path.join(tmp, "cross.trace")
    with open(trace, "w") as f:
        for i in range(4):
            f.write("%d %d %d %d\n" % (i, i, i + 4, FLOW_BYTES))
    cmd = [args.bin, CFG, trace, "routing_function=pinned",
           "multipath_granularity=request"] + overrides
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=args.timeout)
    text = out.stdout
    if out.returncode != 0 or not re.search(r"^OK$", text, re.M):
        sys.exit("FAIL: %s did not complete\n%s" % (name, text[-2000:]))
    makespan = int(re.search(r"^makespan\s+(\d+)", text, re.M).group(1))
    keys = [int(x) for x in re.search(r"^mp_units_by_key\s+(.*)$", text, re.M).group(1).split()]
    print("%-28s makespan %7d  units by path key %s" % (name, makespan, keys))
    return makespan, keys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)

    fails = []
    units = 4 * FLOW_BYTES // 4096
    with tempfile.TemporaryDirectory() as tmp:
        ntb, k_ntb = run(args, tmp, "shortest (NTB only)", [])
        _, k_rr = run(args, tmp, "per_bridge, round_robin",
                      ["pcinet_cross_domain_paths=per_bridge"])
        w, k_w = run(args, tmp, "per_bridge, weighted",
                     ["pcinet_cross_domain_paths=per_bridge", "multipath_select=weighted"])
        _, k_lf = run(args, tmp, "per_bridge, weighted, loopfree",
                      ["pcinet_cross_domain_paths=per_bridge", "multipath_select=weighted",
                       "pcinet_deadlock_avoidance=loopfree", "loopfree_check_interval=50"])

    if k_ntb != [units]:
        fails.append("shortest: units by key %s, want [%d]" % (k_ntb, units))
    if k_rr != [units // 2, units // 2]:
        fails.append("round_robin: units by key %s, want an even split" % k_rr)
    if k_w != [units * 2 // 3, units // 3] or k_lf != k_w:
        fails.append("weighted: units by key %s / %s, want [%d, %d]"
                     % (k_w, k_lf, units * 2 // 3, units // 3))
    if not ntb >= 1.3 * w:
        fails.append("weighted multipath is only %.2fx faster than NTB only" % (ntb / float(w)))
    for f in fails:
        print("FAIL: " + f)
    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
