#!/usr/bin/env python3
"""Whole TLPs on every link (check_whole_tlp).

Real PCIe never splits a TLP on a link. Stock BookSim switches alternate
flits of different packets on one output (stock hold_switch_for_packet does
not prevent it); whole_tlp = 1, which utils/vc0_config.py sets with the
credit model, does. With
check_whole_tlp = 1 every channel aborts the run if it ever carries a flit of
one packet while another packet's tail has not crossed yet.

  1. The generator writes whole_tlp = 1 with credits, and whole_tlp = 0
     with --split-tlp or --no-credits. Each switch is written explicitly, so
     a value already in the base config cannot stay in force: --split-tlp, --no-credits and --fifo applied to an
     already generated VC0 config give the same whole_tlp setting and the
     same result as applied to the stock config.
  2. VC0 configs from the generator never split a TLP: an 8-GPU all-to-all on
     mesh_grid, ring_torus (pinned, loop-free) and dual4B (per_bridge,
     weighted, loop-free), with source_interleave = tlp.
  3. Negative controls: the same all-to-all with --split-tlp (also with
     stock hold_switch_for_packet = 1), and on the stock (non-VC0) config,
     is caught splitting a TLP. whole_tlp without credits is refused.

    tests/whole_tlp_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def gen_vc0(base, out, extra=()):
    subprocess.run([sys.executable, os.path.join(ROOT, "utils/vc0_config.py"), base, "-o", out]
                   + list(extra), cwd=ROOT, check=True, capture_output=True)
    return open(out).read()


def run(args, cfg, trace, extra=()):
    cmd = [args.bin, cfg, trace, "check_whole_tlp=1"] + list(extra)
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=args.timeout)
    return out.returncode, out.stdout + out.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)
    fails = []

    def check(ok, what):
        print("%s %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        trace = os.path.join(tmp, "a2a.trace")
        subprocess.run([sys.executable, os.path.join(ROOT, "utils/gen_collective.py"), "-n", "8",
                        "-s", str(512 << 10), "-a", "alltoall", "--a2a-order", "shift", "-o", trace],
                       check=True, capture_output=True)

        # 1. generator
        hold = "whole_tlp              = 1;"
        mesh = os.path.join(tmp, "mesh.cfg")
        # whole_tlp needs the credit model
        rc, text = run(args, "runfiles/simai_mesh_grid.cfg", trace, ["whole_tlp=1"])
        check(rc != 0 and "whole_tlp = 1 needs pcie_credits = 1" in text,
              "whole_tlp without credits is refused")
        check(hold in gen_vc0("runfiles/simai_mesh_grid.cfg", mesh), "generator: whole TLPs with credits")
        off = "whole_tlp              = 0;"
        split = os.path.join(tmp, "mesh_split.cfg")
        text = gen_vc0("runfiles/simai_mesh_grid.cfg", split, ["--split-tlp"])
        check(hold not in text and off in text, "generator: --split-tlp writes whole_tlp = 0")
        nocred = os.path.join(tmp, "mesh_nocred.cfg")
        text = gen_vc0("runfiles/simai_mesh_grid.cfg", nocred, ["--no-credits"])
        check(hold not in text and off in text, "generator: --no-credits writes whole_tlp = 0")

        # Each flag over an already generated VC0 config (which has noq = 1,
        # pcie_credits = 1 and whole_tlp = 1) must give exactly what it gives
        # over the stock config.
        for flag, whole in (("--split-tlp", "whole_tlp=0"), ("--no-credits", "whole_tlp=0"),
                            ("--fifo", "whole_tlp=1")):
            got = []
            for name, base in (("stock", "runfiles/simai_mesh_grid.cfg"), ("VC0", mesh)):
                cfg = os.path.join(tmp, "%s_from_%s.cfg" % (flag.strip("-"), name))
                gen_vc0(base, cfg, [flag])
                rc, text = run(args, cfg, trace, ["check_whole_tlp=0"])
                got.append((rc, whole in text, [l for l in text.splitlines() if l.startswith("makespan")]))
            check(got[0] == got[1] and got[0][0] == 0 and got[0][1] and got[0][2],
                  "generator: %s over a VC0 config equals %s over the stock one (%s)"
                  % (flag, flag, got[1][0] if got[1][0] else got[1][2]))

        # 2. no split TLP anywhere
        ring = os.path.join(tmp, "ring.cfg")
        gen_vc0("runfiles/simai_ring_torus.cfg", ring)
        dual = os.path.join(tmp, "dual.cfg")
        gen_vc0("tests/topo/dual4B_ntb.cfg", dual)
        lf = ["routing_function=pinned", "pcinet_deadlock_avoidance=loopfree"]
        cases = [("mesh_grid VC0, min", mesh, []),
                 ("ring_torus VC0, pinned loop-free", ring, lf),
                 ("dual4B VC0, per_bridge weighted loop-free", dual,
                  lf + ["pcinet_cross_domain_paths=per_bridge", "multipath_select=weighted"])]
        for name, cfg, extra in cases:
            rc, text = run(args, cfg, trace, ["source_interleave=tlp"] + extra)
            check(rc == 0 and "check_whole_tlp" not in text and "OK" in text,
                  "%s: every TLP sent whole (rc %d)" % (name, rc))

        # 3. negative controls
        for name, cfg, extra in (("VC0 with --split-tlp", split, []),
                                 ("VC0 with --split-tlp and stock hold_switch_for_packet", split,
                                  ["hold_switch_for_packet=1"]),
                                 ("stock config", "runfiles/simai_mesh_grid.cfg", [])):
            rc, text = run(args, cfg, trace, ["source_interleave=tlp"] + extra)
            check(rc != 0 and "check_whole_tlp:" in text and "split on the wire" in text,
                  "%s: a split TLP is caught (rc %d)" % (name, rc))

    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
