#!/usr/bin/env python3
"""Compare the per-flow CSVs of two run_matrix.py output directories.

  tests/compare.py OLD_DIR NEW_DIR [--map old_routing=new_routing]
                   [--old-tag T] [--new-tag T] [--allow-new COLS]

Cases are matched by name. With --map, NEW_DIR/<topo>.<new>.<wl> is compared
against OLD_DIR/<topo>.<old>.<wl> (e.g. --map min=pinned for the single-path
equivalence check), and only old cases of the mapped routings are expected.

Every expected old case must exist in NEW_DIR. Files with identical headers
must be byte-identical. Otherwise columns are matched by name: every old
column must exist with identical values in every row, and the new file may
add only the columns named in --allow-new (default: paths_used, which
routing_function = pinned appends). Exit status 0 only if nothing differs
and nothing is missing.
"""

import argparse
import csv
import filecmp
import glob
import os
import sys


def case_files(d, tag):
    out = {}
    for path in glob.glob(os.path.join(d, "*.csv")):
        name = os.path.basename(path)[:-4]
        if tag:
            if not name.endswith("." + tag):
                continue
            name = name[:-(len(tag) + 1)]
        out[name] = path
    return out


def read(path):
    with open(path, newline="") as fh:
        rows = list(csv.reader(fh))
    return rows[0], rows[1:]


def same_by_schema(opath, npath, allow_new):
    """Byte-identical when the headers match; else column-by-name."""
    oh, orows = read(opath)
    nh, nrows = read(npath)
    if oh == nh:
        return filecmp.cmp(opath, npath, shallow=False), ""
    extra = [c for c in nh if c not in oh]
    if [c for c in extra if c not in allow_new]:
        return False, "unexpected new columns %s" % extra
    missing = [c for c in oh if c not in nh]
    if missing:
        return False, "missing columns %s" % missing
    if len(orows) != len(nrows):
        return False, "%d rows vs %d" % (len(orows), len(nrows))
    idx = [nh.index(c) for c in oh]
    for i, (o, n) in enumerate(zip(orows, nrows)):
        if o != [n[j] for j in idx]:
            return False, "row %d differs" % (i + 1)
    return True, "+" + ",".join(extra)


def makespan(path):
    h, rows = read(path)
    col = h.index("t_done")
    return max((int(r[col]) for r in rows), default=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("old")
    ap.add_argument("new")
    ap.add_argument("--map", action="append", default=[],
                    help="old_routing=new_routing, repeatable")
    ap.add_argument("--old-tag", default="")
    ap.add_argument("--new-tag", default="")
    ap.add_argument("--allow-new", default="paths_used",
                    help="comma list of columns the new CSVs may add")
    args = ap.parse_args()

    old = case_files(args.old, args.old_tag)
    new = case_files(args.new, args.new_tag)
    rmap = dict(m.split("=", 1) for m in args.map)
    allow_new = [c for c in args.allow_new.split(",") if c]

    expected = []          # (old name, new name)
    for oname in sorted(old):
        topo, routing, wl = oname.split(".", 2)
        if rmap and routing not in rmap:
            continue
        expected.append((oname, "%s.%s.%s" % (topo, rmap.get(routing, routing), wl)))
    if not expected:
        print("no expected cases")
        return 1

    diff = missing = 0
    for oname, nname in expected:
        if nname not in new:
            missing += 1
            print("%-36s %-36s MISSING" % (oname, nname))
            continue
        same, note = same_by_schema(old[oname], new[nname], allow_new)
        diff += not same
        print("%-36s %-36s %s  %d -> %d  %s" % (oname, nname, "SAME" if same else "DIFF",
                                               makespan(old[oname]), makespan(new[nname]),
                                               note))
    unexpected = sorted(set(new) - set(n for _, n in expected))
    for n in unexpected:
        print("%-36s %-36s EXTRA (not compared)" % ("", n))
    print("%d expected, %d differ, %d missing, %d extra"
          % (len(expected), diff, missing, len(unexpected)))
    return 1 if diff or missing else 0


if __name__ == "__main__":
    sys.exit(main())
