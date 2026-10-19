#!/usr/bin/env python3
"""utils/gen_dual_socket.py --mesh and --gen/--width, end to end through
booksim_flowtest (4 switches, placement B, UPI cpf 2):

  1. With --gen 4 --width 8 every PCIe link parses at 4 cycles/flit
     (15.75 GB/s rounds to 16), and UPI keeps its own cpf 2.
  2. One TLP crosses the expected switches (RCs count): G2 -> G6 takes
     SW2-RC0... in the plain tree (6), SW2-SW0-SW1-SW3 with the NTB (4), and
     only SW2-SW3 in the mesh (2, the lower NTB); G0 -> G4 takes 4 / 2 / 2.
  3. A 1 MiB flow on one switch runs at 12.8 B/cycle under Gen4 x8 (256 B
     per 5-flit TLP at 4 cycles/flit) and at 51.2 without --gen.
  4. Bad combinations are refused.
  5. pcinet_min_tiebreak on the mesh, with G0 -> G6 and G2 -> G4 (1 MiB each)
     at once: port (default) sends both diagonals over the upper NTB, so they
     share it and take twice as long; cross_first crosses each at its own
     level (SW0-SW1 and SW2-SW3), so both run at link rate, and the min
     dependency graph stays acyclic; G0 -> G6 then shares SW0-SW1 with
     G1 -> G4. Bad settings are refused.
  6. --fabric-width 16 with --gen 4 --width 8: GPU ports stay at cpf 4, root
     ports, switch-to-switch links and NTBs parse at cpf 2 (31.5 GB/s rounds
     to 32). G0 -> G4 and G1 -> G5 (1 MiB each) share the upper NTB, so they
     take twice as long as one alone with an x8 fabric and as long as one
     alone with x16. --fabric-width without --gen/--width is refused.
  7. --gpu-switches: 0,0,2,2,1,1,3,3 writes placement B's topology and
     2,2,2,2,3,3,3,3 placement A's (all but the header). With
     0,1,2,3,0,1,2,3 on the mesh, G0 -> G4 stays on SW0 (1 switch), G0 -> G1
     crosses the upper NTB (2) and G0 -> G3 is a diagonal (3). Wrong counts,
     out-of-range switches and --placement with it are refused.

    tests/gen_dual_socket_check.py --bin src/tools/booksim_flowtest
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GEN = os.path.join(ROOT, "utils", "gen_dual_socket.py")


def generate(tmp, name, extra, placement="B"):
    cmd = [sys.executable, GEN, "--switches", "4", "--upi-cpf", "2", "-o", tmp, "--name", name]
    cmd += ["--placement", placement] if placement else []
    cmd += extra
    subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True, text=True)
    return os.path.join(tmp, name + ".cfg")


def flowtest(args, tmp, cfg, flows, overrides=(), expect_ok=True):
    trace = os.path.join(tmp, "t.trace")
    with open(trace, "w") as f:
        for i, (s, d, b) in enumerate(flows):
            f.write("%d %d %d %d\n" % (i, s, d, b))
    out = subprocess.run([args.bin, cfg, trace] + list(overrides), cwd=ROOT, capture_output=True,
                         text=True, timeout=args.timeout)
    if not expect_ok:
        return out
    if out.returncode != 0 or not re.search(r"^OK$", out.stdout, re.M):
        sys.exit("FAIL: %s %s did not complete\n%s" % (cfg, flows, out.stdout[-2000:]))
    return out.stdout


def link_cpfs(text):
    """{(a, b): cpf} for router-router links and {('n', node): cpf} for GPU ports,
    from pcinet's parse listing."""
    cpf, head = {}, None
    for line in text.splitlines():
        m = re.match(r"^Router (\d+)", line)
        if m:
            head = int(m.group(1))
            continue
        m = re.match(r"^\s+Router (\d+) lat \d+ port_type \d+ cpf (\d+)", line)
        if m and head is not None:
            cpf[(head, int(m.group(1)))] = int(m.group(2))
        m = re.match(r"^\s+Node (\d+) lat \d+ cpf (\d+)", line)
        if m:
            cpf[("n", int(m.group(1)))] = int(m.group(2))
        if line.startswith("====") and "Node to Router" in line:
            break
    return cpf


def stat(text, key):
    return float(re.search(r"^%s\s+(\S+)" % key, text, re.M).group(1))


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
        g48 = ["--gen", "4", "--width", "8"]
        cfgs = {"plain": generate(tmp, "plain", g48),
                "ntb": generate(tmp, "ntb", ["--ntb"] + g48),
                "mesh": generate(tmp, "mesh", ["--mesh"] + g48)}
        mesh_full = generate(tmp, "mesh_full", ["--mesh"])

        # 1. link rates
        upi = {(4, 5), (5, 4)}
        for name, cfg in sorted(cfgs.items()):
            cpf = link_cpfs(flowtest(args, tmp, cfg, [(0, 1, 256)]))
            pcie = {k: v for k, v in cpf.items() if k not in upi}
            # one-way: 2 root ports + 2 lower links (+ 1 NTB, + 1 more NTB), and 9 GPU ports
            want_links = {"plain": 8 + 9, "ntb": 10 + 9, "mesh": 12 + 9}[name]
            check(len(pcie) == want_links and set(pcie.values()) == {4}
                  and cpf.get((4, 5)) == 2 and cpf.get((5, 4)) == 2,
                  "%s: %d one-way PCIe links at cpf 4, UPI at cpf 2" % (name, len(pcie)))

        # 2. switches crossed by one TLP
        want = {"plain": (6, 4), "ntb": (4, 2), "mesh": (2, 2)}
        for name, cfg in sorted(cfgs.items()):
            a = stat(flowtest(args, tmp, cfg, [(2, 6, 256)]), "tlp_switches_mean")
            b = stat(flowtest(args, tmp, cfg, [(0, 4, 256)]), "tlp_switches_mean")
            check((a, b) == want[name], "%s: G2->G6 crosses %g switches, G0->G4 %g (want %s)"
                  % (name, a, b, want[name]))

        # 3. bandwidth of one switch-local flow
        bw4 = stat(flowtest(args, tmp, cfgs["mesh"], [(0, 1, 1 << 20)]), "aggregate throughput")
        bw5 = stat(flowtest(args, tmp, mesh_full, [(0, 1, 1 << 20)]), "aggregate throughput")
        check(abs(bw4 - 12.8) < 0.13, "Gen4 x8: 1 MiB at %.3f B/cycle (want 12.8)" % bw4)
        check(abs(bw5 - 51.2) < 0.6, "full rate: 1 MiB at %.3f B/cycle (want 51.2)" % bw5)

        # 4. refused
        for bad in (["--switches", "2", "--mesh"], ["--switches", "4", "--gen", "4"],
                    ["--switches", "4", "--width", "8"]):
            out = subprocess.run([sys.executable, GEN] + bad + ["-o", tmp], cwd=ROOT,
                                 capture_output=True, text=True)
            check(out.returncode != 0, "refused: %s" % " ".join(bad))

        # 5. min tie-break on the mesh
        diag = [(0, 6, 1 << 20), (2, 4, 1 << 20)]
        alone = stat(flowtest(args, tmp, cfgs["mesh"], diag[:1]), "makespan")
        port = stat(flowtest(args, tmp, cfgs["mesh"], diag), "makespan")
        cf_text = flowtest(args, tmp, cfgs["mesh"], diag, ["pcinet_min_tiebreak=cross_first"])
        cross = stat(cf_text, "makespan")
        check(port > 1.9 * alone, "port: diagonals share the upper NTB (%d vs %d cycles alone)"
              % (port, alone))
        check(cross < 1.02 * alone and "channel dependency graph is acyclic" in cf_text,
              "cross_first: diagonals on separate NTBs (%d vs %d alone), graph acyclic" % (cross, alone))
        # ... and it crosses at SW0's own level: G0 -> G6 shares SW0-SW1 with G1 -> G4
        # (crossing low, SW0-SW2-SW3, would also separate the diagonals).
        level = stat(flowtest(args, tmp, cfgs["mesh"], [(0, 6, 1 << 20), (1, 4, 1 << 20)],
                              ["pcinet_min_tiebreak=cross_first"]), "makespan")
        check(level > 1.9 * alone, "cross_first: G0->G6 crosses SW0-SW1, shared with G1->G4 "
              "(%d vs %d alone)" % (level, alone))
        for bad in (["pcinet_min_tiebreak=lowest"],
                    ["pcinet_min_tiebreak=cross_first", "routing_function=pinned"]):
            out = flowtest(args, tmp, cfgs["mesh"], diag[:1], bad, expect_ok=False)
            check(out.returncode != 0 and "pcinet_min_tiebreak" in out.stdout,
                  "refused: %s" % " ".join(bad))

        # 6. fabric links wider than the GPU ports
        wide = generate(tmp, "wide", ["--mesh"] + g48 + ["--fabric-width", "16"])
        cpf = link_cpfs(flowtest(args, tmp, wide, [(0, 1, 256)]))
        gpu = {k: v for k, v in cpf.items() if k[0] == "n"}
        fabric = {k: v for k, v in cpf.items() if k[0] != "n" and k not in upi}
        check(len(gpu) == 9 and set(gpu.values()) == {4} and len(fabric) == 12
              and set(fabric.values()) == {2} and cpf.get((4, 5)) == 2,
              "fabric x16: 9 GPU ports at cpf 4, %d fabric links at cpf 2, UPI at cpf 2" % len(fabric))
        pair = [(0, 4, 1 << 20), (1, 5, 1 << 20)]
        narrow_pair = stat(flowtest(args, tmp, cfgs["mesh"], pair), "makespan")
        wide_alone = stat(flowtest(args, tmp, wide, pair[:1]), "makespan")
        wide_pair = stat(flowtest(args, tmp, wide, pair), "makespan")
        check(narrow_pair > 1.9 * alone and wide_pair < 1.02 * wide_alone,
              "fabric x16: G0->G4 + G1->G5 over the upper NTB take %d cycles (x8 fabric %d, "
              "one alone %d)" % (wide_pair, narrow_pair, wide_alone))
        out = subprocess.run([sys.executable, GEN, "--switches", "4", "--fabric-width", "16", "-o", tmp],
                             cwd=ROOT, capture_output=True, text=True)
        check(out.returncode != 0, "refused: --fabric-width without --gen/--width")

        # 7. any placement
        def body(cfg):
            return [l for l in open(cfg[:-4] + ".txt") if not l.startswith("#")]
        for spec, named in (("0,0,2,2,1,1,3,3", "B"), ("2,2,2,2,3,3,3,3", "A")):
            mine = generate(tmp, "p_" + named, ["--mesh", "--gpu-switches", spec], placement=None)
            ref = generate(tmp, "n_" + named, ["--mesh"], placement=named)
            check(body(mine) == body(ref), "--gpu-switches %s writes placement %s" % (spec, named))
        alt = generate(tmp, "alt", ["--mesh", "--gpu-switches", "0,1,2,3,0,1,2,3"] + g48, placement=None)
        got = tuple(stat(flowtest(args, tmp, alt, [(0, d, 256)]), "tlp_switches_mean") for d in (4, 1, 3))
        check(got == (1, 2, 3), "--gpu-switches 0,1,2,3,0,1,2,3: G0->G4, G0->G1, G0->G3 cross %s "
              "switches (want 1, 2, 3)" % (got,))
        for bad in (["--gpu-switches", "0,0,2,2,1,1,3"], ["--gpu-switches", "0,0,2,2,1,1,3,4"],
                    ["--placement", "B", "--gpu-switches", "0,0,2,2,1,1,3,3"]):
            out = subprocess.run([sys.executable, GEN, "--switches", "4"] + bad + ["-o", tmp], cwd=ROOT,
                                 capture_output=True, text=True)
            check(out.returncode != 0, "refused: %s" % " ".join(bad))

    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
