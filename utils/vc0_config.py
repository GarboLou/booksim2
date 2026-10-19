#!/usr/bin/env python3
"""Append the PCIe VC0 switch model to a BookSim config.

With --fifo, every ingress instead has one FIFO per type (num_vcs = 3).

Default: at every ingress, one internal queue (BookSim VC) per (transaction
type, egress of the next switch) via NOQ, all queues of a type sharing that
type's storage; whole TLPs admitted against PCIe header/data credits, and
sent whole on every link (whole_tlp = 1; --split-tlp turns that off).
BookSim VCs here are internal queues, not PCIe protocol VCs: the link runs
VC0 only.

num_vcs is set to 3 x the largest switch radix of the config's topology
(GPU ports count), one contiguous range per type: Posted, Non-Posted,
Completion.

    python3 utils/vc0_config.py runfiles/simai_ring_torus.cfg -o /tmp/vc0_ring.cfg
    python3 utils/vc0_config.py runfiles/simai_flat.cfg --no-credits --pool-flits 128,128,128

Run from the booksim2 root: network_file paths are relative to it.

Every on/off setting the model depends on (noq, whole_tlp, pcie_credits) is
written explicitly in every mode, so a value already in the base config (for
example the output of an earlier run of this script) cannot stay in force.
"""

import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import place_search  # noqa: E402

# Placeholders, TODO calibrate from switch datasheets:
# Posted 32 headers / 8 KiB, Non-Posted 32 / 1 KiB, Completions infinite
# (endpoints commonly advertise infinite completion credits).
DEFAULT_CREDITS = "32,8192,32,1024,0,0"


def network_file(cfg_text):
    m = None
    for m in re.finditer(r"^\s*network_file\s*=\s*([^;]+);", cfg_text, re.M):
        pass
    if not m:
        sys.exit("vc0_config: base config has no network_file")
    return m.group(1).strip()


def max_radix(topology_path):
    t = place_search.parse_topology(topology_path)
    neighbors = {s: set() for s in t.switches}
    for u, v, _ in t.edges:
        neighbors[u].add(v)
        neighbors[v].add(u)
    nodes = {s: 0 for s in t.switches}
    for n, s in t.node_switch.items():
        nodes[s] += 1
    return max(nodes[s] + len(neighbors[s]) for s in t.switches)


def vc0_block(radix, args):
    if args.fifo:
        lines = ["",
                 "// ---- PCIe VC0 switch, one FIFO per type (no per-egress queues),",
                 "// ---- appended by utils/vc0_config.py --fifo",
                 "speculative            = 1;   // back-to-back TLPs on one queue, no idle cycle",
                 "noq                    = 0;   // no per-egress queues",
                 "num_vcs                = 3;   // one queue per type"]
    else:
        lines = ["",
                 "// ---- PCIe VC0 switch, one queue per (type, egress of the next switch),",
                 "// ---- appended by utils/vc0_config.py",
                 "speculative            = 1;   // back-to-back TLPs on one queue, no idle cycle",
                 "noq                    = 1;   // sort by next egress on arrival",
                 "noq_vcs_per_output     = 1;   // exactly one queue per (type, egress)",
                 "num_vcs                = %d;  // 3 types x max switch radix %d" % (3 * radix, radix)]
    names = ["write_request", "read_request", "read_reply"]
    for i, name in enumerate(names):
        lines.append("%s_begin_vc = %d; %s_end_vc = %d;"
                     % (name, i * radix, name, (i + 1) * radix - 1))
    lines.append("pcie_default_class     = %s;   // untagged (SimAI) flows" % args.default_class)
    if args.cap:
        lines.append("buffer_policy          = limited;  // capped queues: optimistic variant")
        lines.append("max_held_slots         = %d;" % args.cap)
    else:
        lines.append("buffer_policy          = shared;   // one pool per type per ingress")
    if args.no_credits:
        pools = [int(x) for x in args.pool_flits.split(",")]
        if len(pools) != 3:
            sys.exit("vc0_config: --pool-flits needs three values")
        lines += ["pcie_credits           = 0;",
                  "whole_tlp              = 0;   // needs the credits",
                  "private_bufs           = 3;",
                  "private_buf_size       = {%d,%d,%d};" % tuple(pools),
                  "private_buf_start_vc   = {%d,%d,%d};" % (0, radix, 2 * radix),
                  "private_buf_end_vc     = {%d,%d,%d};" % (radix - 1, 2 * radix - 1, 3 * radix - 1),
                  "buf_size               = %d;   // no common pool" % sum(pools)]
    else:
        c = [int(x) for x in args.credits.split(",")]
        if len(c) != 6:
            sys.exit("vc0_config: --credits needs six values")
        lines += ["pcie_credits           = 1;   // storage derived from these; 0 = infinite",
                  "pcie_ph = %d; pcie_pd_bytes = %d;       // TODO calibrate" % (c[0], c[1]),
                  "pcie_nph = %d; pcie_npd_bytes = %d;     // TODO calibrate" % (c[2], c[3]),
                  "pcie_cplh = %d; pcie_cpld_bytes = %d;   // TODO calibrate" % (c[4], c[5])]
        # A link sends a TLP whole, as real PCIe does. Needs the credits: a TLP
        # starts only once the receiver has room for all of it, so it never
        # stalls half-sent while holding the link.
        if args.split_tlp:
            lines.append("whole_tlp              = 0;   // switches may split TLPs (--split-tlp)")
        else:
            lines.append("whole_tlp              = 1;   // every link sends whole TLPs")
    if args.switch_latency:
        lines.append("pcinet_switch_latency  = %d;   // TODO calibrate" % args.switch_latency)
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="BookSim config to extend")
    ap.add_argument("-o", "--out", default="-")
    ap.add_argument("--credits", default=DEFAULT_CREDITS,
                    help="ph,pd_bytes,nph,npd_bytes,cplh,cpld_bytes (0 = infinite); "
                         "default %s" % DEFAULT_CREDITS)
    ap.add_argument("--no-credits", action="store_true",
                    help="per-egress queues and storage only, no header/data credits")
    ap.add_argument("--pool-flits", default="128,128,128",
                    help="with --no-credits: flits of storage per type (P,NP,Cpl)")
    ap.add_argument("--cap", type=int, default=0,
                    help="cap each queue at N flits (buffer_policy = limited): an "
                         "optimistic variant, since a real sender sees only "
                         "aggregate credits, not per-egress occupancy")
    ap.add_argument("--fifo", action="store_true",
                    help="one FIFO per type at every ingress instead of "
                         "one queue per (type, egress)")
    ap.add_argument("--split-tlp", action="store_true",
                    help="let switches split TLPs on a link (stock BookSim; "
                         "without --no-credits a link otherwise sends whole TLPs)")
    ap.add_argument("--switch-latency", type=int, default=0)
    ap.add_argument("--default-class", choices=["P", "NP", "Cpl", "none"], default="P")
    args = ap.parse_args()

    text = open(args.base).read()
    radix = 1 if args.fifo else max_radix(network_file(text))
    out = text.rstrip("\n") + "\n" + vc0_block(radix, args)
    if args.out == "-":
        sys.stdout.write(out)
    else:
        with open(args.out, "w") as f:
            f.write(out)


if __name__ == "__main__":
    main()
