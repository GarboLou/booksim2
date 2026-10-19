#!/usr/bin/env python3
"""booksim_flowtest --csv publishing rules. Rows stream into <csv>.partial; only a successful run renames it
to <csv>.

  1. Success: <csv> holds the header and one row per flow; no .partial is left.
  2. Failure (a flow dependency cycle; one flow completes, the frontend
     reports FAILED, exit 1): an existing <csv> is left byte-for-byte
     unchanged, and <csv>.partial holds the completed flow's row.
  3. Abort (routing deadlock -> Error()): an existing <csv> is unchanged.

    tests/csv_output_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def run(args, cfg, trace, csv, extra=()):
    cmd = [args.bin, cfg, trace, "--csv", csv] + list(extra)
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=args.timeout)
    return out.returncode, out.stdout + out.stderr


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=120)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)
    fails = []

    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.trace")
        with open(good, "w") as f:
            f.write("1 0 4 4096\n2 1 5 4096 after=1\n3 2 6 4096\n")
        cyc = os.path.join(tmp, "cycle.trace")
        with open(cyc, "w") as f:                      # 2 and 3 wait on each other
            f.write("1 0 4 16\n2 1 5 16 after=3\n3 2 6 16 after=2\n")
        csv = os.path.join(tmp, "out.csv")
        partial = csv + ".partial"

        # 1. success publishes
        rc, text = run(args, "runfiles/simai_flat.cfg", good, csv)
        lines = read(csv).decode().splitlines() if os.path.exists(csv) else []
        ok1 = (rc == 0 and len(lines) == 4 and lines[0].startswith("flow,")
               and not os.path.exists(partial))
        print("success: %s" % ("published, header + 3 rows, no .partial" if ok1
                               else "WRONG (rc %s, %d lines)" % (rc, len(lines))))
        if not ok1:
            fails.append("successful run did not publish exactly the CSV")
        complete = read(csv) if os.path.exists(csv) else b""

        # 2. a failed run keeps the existing CSV and its rows in .partial
        rc, text = run(args, "runfiles/simai_flat.cfg", cyc, csv)
        kept = os.path.exists(csv) and read(csv) == complete
        prows = read(partial).decode().splitlines() if os.path.exists(partial) else []
        ok2 = (rc != 0 and "FAILED" in text and kept and len(prows) == 2
               and prows[1].split(",")[1:3] == ["0", "4"])
        print("failure: %s" % ("existing CSV unchanged; .partial has the completed flow"
                               if ok2 else "WRONG (rc %s, kept %s, partial rows %d)"
                               % (rc, kept, len(prows))))
        if not ok2:
            fails.append("failed run replaced the CSV or lost the partial rows")

        # 3. an abort (Error) keeps the existing CSV
        if os.path.exists(partial):
            os.remove(partial)
        dl = os.path.join(tmp, "guard.trace")
        with open(dl, "w") as f:
            fid = 0
            pairs = [(0, 2), (1, 3), (2, 0), (3, 1)]
            for _ in range(2):
                for s, d in pairs:
                    f.write("%d %d %d 1%s\n" % (fid, s, d, " after=%d" % (fid - 1) if fid else ""))
                    fid += 1
            for s, d in pairs:
                f.write("%d %d %d 65536 after=%d\n" % (fid, s, d, 7))
                fid += 1
        rc, text = run(args, "tests/topo/guard_mesh4.cfg", dl, csv,
                       ["routing_function=pinned", "pcinet_cross_domain_paths=per_bridge",
                        "multipath_granularity=flow", "pcinet_deadlock_avoidance=none",
                        "deadlock_abort_cycles=20000"])
        ok3 = rc != 0 and "routing deadlock" in text and read(csv) == complete
        print("abort:   %s" % ("existing CSV unchanged" if ok3 else "WRONG (rc %s)" % rc))
        if not ok3:
            fails.append("aborted run changed the CSV")

    for f in fails:
        print("FAIL: " + f)
    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
