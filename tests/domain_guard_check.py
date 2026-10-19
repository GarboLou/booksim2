#!/usr/bin/env python3
"""Regression for the per-bridge deadlock guard, on tests/topo/guard_mesh4.cfg: a 4-switch full mesh split into
domains {0,1} and {2,3}, one GPU per switch, one header and 256 B of data
credit per type.

Two 1-byte warm-up flows per pair (G0->G2, G1->G3, G2->G0, G3->G1), each after
the previous, advance every pair's round-robin selector to path key 2. Then
four 64 KiB flows start together on the key-2 routes 0->1->2, 1->2->3,
2->3->0, 3->0->1, which close a cycle of pass-throughs.

  1. auto and vc_classes are rejected at start-up: distance classes cannot
     order non-shortest routes, even though the class count here is 1.
  2. none (explicit opt-out) deadlocks: the deadlock case is real.
  3. loopfree completes, with every credit back and no live route reference.

    tests/domain_guard_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CFG = "tests/topo/guard_mesh4.cfg"
PAIRS = [(0, 2), (1, 3), (2, 0), (3, 1)]


def trace_lines():
    lines, fid = [], 0
    for _ in range(2):
        for s, d in PAIRS:
            lines.append("%d %d %d 1%s" % (fid, s, d, " after=%d" % (fid - 1) if fid else ""))
            fid += 1
    last = fid - 1
    for s, d in PAIRS:
        lines.append("%d %d %d 65536 after=%d" % (fid, s, d, last))
        fid += 1
    return lines


def run(args, trace, mode):
    cmd = [args.bin, CFG, trace, "routing_function=pinned",
           "pcinet_cross_domain_paths=per_bridge", "multipath_granularity=flow",
           "multipath_select=round_robin", "pcinet_deadlock_avoidance=" + mode,
           "deadlock_abort_cycles=20000"]
    if mode == "loopfree":
        cmd.append("loopfree_check_interval=97")
    try:
        out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                             timeout=args.timeout)
        return out.returncode, out.stdout + out.stderr
    except subprocess.TimeoutExpired:
        return "timeout", ""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)

    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        trace = os.path.join(tmp, "guard.trace")
        with open(trace, "w") as f:
            f.write("\n".join(trace_lines()) + "\n")

        for mode in ("auto", "vc_classes"):
            rc, text = run(args, trace, mode)
            rejected = rc not in (0, "timeout") and "only order shortest routes" in text
            print("%-10s %s" % (mode, "rejected at start-up" if rejected else "NOT rejected (rc %s)" % rc))
            if not rejected:
                fails.append("%s was not rejected" % mode)

        rc, text = run(args, trace, "none")
        dl = re.search(r"routing deadlock: .* of (\d+) flows outstanding", text)
        ok_dl = rc not in (0, "timeout") and dl and dl.group(1) == "4"
        print("%-10s %s" % ("none", "deadlocked as expected (4 flows stuck)" if ok_dl
                            else "did NOT deadlock as expected (rc %s)" % rc))
        if not ok_dl:
            fails.append("none: the four-flow deadlock case did not deadlock")

        rc, text = run(args, trace, "loopfree")
        ok = (rc == 0 and re.search(r"^OK$", text, re.M)
              and re.search(r"^pcie credits\s+all returned", text, re.M)
              and re.search(r"^lf_live_units\s+0$", text, re.M)
              and re.search(r"^lf_live_turn_refs\s+0$", text, re.M))
        m = re.search(r"^makespan\s+(\d+)", text, re.M)
        print("%-10s %s" % ("loopfree", "completed, drained (makespan %s)" % m.group(1)
                            if ok else "FAILED (rc %s)" % rc))
        if not ok:
            fails.append("loopfree did not complete and drain")

    for f in fails:
        print("FAIL: " + f)
    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
