#!/usr/bin/env python3
"""Generate collective communication schedules as booksim_flowtest traces.

The output is a dependency graph of point-to-point transfers, which is what a
collective actually is once the algorithm is fixed. Dependencies are on the
receive side: rank i cannot forward its step k+1 chunk until the step k chunk
from rank i-1 has arrived. That is what makes the trace a real critical path
rather than a bag of independent flows.

It covers the textbook algorithms, for running booksim_flowtest without SimAI.
Under SimAI, SimCCL supplies the same shape of schedule with NCCL's actual
channel and chunk decomposition.

Trace line format (see src/tools/booksim_flowtest.cpp):
    <id> <src> <dst> <bytes> [after=<id>[,<id>...]] [at=<ns>] [class=P|NP|Cpl]
"""

import argparse
import sys


def _dep(flow_id):
    """Wrap a single dependency. Flow ids start at 0, so `if flow_id` is wrong
    here -- it would drop every dependency on flow 0."""
    return None if flow_id is None else [flow_id]


class Trace:
    def __init__(self, perm=None):
        self.lines = []
        self.next_id = 0
        self.perm = perm  # perm[logical_rank] -> physical node id, or None

    def add(self, src, dst, nbytes, after=None, at=None, pcie_class=None):
        if self.perm:
            src, dst = self.perm[src], self.perm[dst]
        if src == dst or nbytes <= 0:
            return None
        fid = self.next_id
        self.next_id += 1
        parts = [str(fid), str(src), str(dst), str(int(nbytes))]
        if after:
            parts.append("after=" + ",".join(str(a) for a in sorted(set(after))))
        if at:
            parts.append("at=%d" % at)
        if pcie_class:
            parts.append("class=" + pcie_class)
        self.lines.append(" ".join(parts))
        return fid

    def emit(self, out, header):
        out.write("# %s\n" % header)
        out.write("# id src dst bytes [after=...] [at=ns] [class=P|NP|Cpl]\n")
        for l in self.lines:
            out.write(l + "\n")


def ring_allreduce(t, n, total_bytes):
    """Bandwidth-optimal ring: N-1 reduce-scatter steps then N-1 all-gather
    steps, each moving one 1/N chunk to the next rank. Moves 2(N-1)/N x S.
    Placement (--order) is applied uniformly at the Trace level, not here."""
    r = list(range(n))
    m = len(r)
    if m < 2:
        return []
    chunk = total_bytes // m
    prev = {}                      # rank -> flow it must wait on
    last = []
    for step in range(2 * (m - 1)):
        cur = {}
        for i in range(m):
            src, dst = r[i], r[(i + 1) % m]
            # rank i forwards only after receiving from rank i-1 last step
            dep = prev.get(r[i])
            cur[dst] = t.add(src, dst, chunk, after=_dep(dep))
        prev = cur
        last = [f for f in cur.values() if f is not None]
    return last


def hierarchical_ring_allreduce(t, n, total_bytes, group):
    """Two-level: ring inside each switch group, then a ring across one leader
    per group, then broadcast back down. Keeps most traffic off the inter-switch
    links, which is the whole point on a PCIe fabric."""
    groups = [list(range(g, min(g + group, n))) for g in range(0, n, group)]
    chunk = total_bytes // n

    # phase 1: reduce within each group toward its leader
    ready = []
    for g in groups:
        leader = g[0]
        deps = [f for f in (t.add(m, leader, chunk) for m in g[1:])
                if f is not None]
        ready.append((leader, deps))

    # phase 2: ring all-reduce across the leaders
    leaders = [g[0] for g in groups]
    inter = []
    if len(leaders) > 1:
        prev = {leader: deps for leader, deps in ready}
        for step in range(2 * (len(leaders) - 1)):
            cur = {}
            for i in range(len(leaders)):
                src, dst = leaders[i], leaders[(i + 1) % len(leaders)]
                dep = prev.get(src) or []
                f = t.add(src, dst, chunk, after=dep if dep else None)
                cur[dst] = [f] if f is not None else []
            prev = cur
        inter = [f for v in prev.values() for f in v if f is not None]

    # phase 3: leaders broadcast the result back to their group
    last = []
    for g in groups:
        leader = g[0]
        for m in g[1:]:
            f = t.add(leader, m, chunk * len(g), after=inter if inter else None)
            if f is not None:
                last.append(f)
    return last


def binomial_tree_allreduce(t, n, total_bytes):
    """Latency-optimal: binomial reduce to rank 0, then binomial broadcast.
    log2(N) steps each way, versus the ring's 2(N-1)."""
    # reduce
    done = {}
    dist = 1
    while dist < n:
        new = {}
        for i in range(0, n, dist * 2):
            partner = i + dist
            if partner < n:
                dep = [d for d in (done.get(i), done.get(partner))
                       if d is not None]
                new[i] = t.add(partner, i, total_bytes, after=dep if dep else None)
        done.update({k: v for k, v in new.items() if v is not None})
        dist *= 2
    root_dep = done.get(0)

    # broadcast
    have = {0: root_dep}
    last = []
    dist //= 2
    while dist >= 1:
        for i in range(0, n, dist * 2):
            partner = i + dist
            if partner < n and i in have:
                f = t.add(i, partner, total_bytes, after=_dep(have[i]))
                have[partner] = f
                if f is not None:
                    last.append(f)
        dist //= 2
    return last


def allgather(t, n, total_bytes):
    """Ring all-gather: N-1 steps of one 1/N chunk each."""
    chunk = total_bytes // n
    prev = {}
    for step in range(n - 1):
        cur = {}
        for i in range(n):
            src, dst = i, (i + 1) % n
            dep = prev.get(i)
            cur[dst] = t.add(src, dst, chunk, after=_dep(dep))
        prev = cur
    return [f for f in prev.values() if f is not None]


def reducescatter(t, n, total_bytes):
    """Ring reduce-scatter: the first half of a ring all-reduce."""
    chunk = total_bytes // n
    prev = {}
    for step in range(n - 1):
        cur = {}
        for i in range(n):
            src, dst = i, (i + 1) % n
            dep = prev.get(i)
            cur[dst] = t.add(src, dst, chunk, after=_dep(dep))
        prev = cur
    return [f for f in prev.values() if f is not None]


def alltoall_destinations(i, n, order):
    """Destination order for rank i. Each source sends one flow at a time, so
    with `naive` (ascending) every source starts on rank 0 and the first steps
    are an incast; `shift` (i+1, i+2, ...) and `xor` (i^1, i^2, ...) make each
    step a permutation instead."""
    if order == "naive":
        return [j for j in range(n) if j != i]
    if order == "shift":
        return [(i + k) % n for k in range(1, n)]
    if order == "xor":
        return [i ^ k for k in range(1, n)]
    raise ValueError("unknown all-to-all order %r" % order)


def alltoall(t, n, total_bytes, order="shift"):
    """Direct all-to-all: every rank sends a distinct 1/N chunk to every other.
    No dependencies -- it is a pure fabric stress test."""
    chunk = total_bytes // n
    out = []
    for i in range(n):
        for j in alltoall_destinations(i, n, order):
            f = t.add(i, j, chunk)
            if f is not None:
                out.append(f)
    return out


ALGORITHMS = {
    "ring":         lambda t, a: ring_allreduce(t, a.nodes, a.size),
    "hier_ring":    lambda t, a: hierarchical_ring_allreduce(t, a.nodes, a.size, a.group),
    "tree":         lambda t, a: binomial_tree_allreduce(t, a.nodes, a.size),
    "allgather":    lambda t, a: allgather(t, a.nodes, a.size),
    "reducescatter":lambda t, a: reducescatter(t, a.nodes, a.size),
    "alltoall":     lambda t, a: alltoall(t, a.nodes, a.size, a.a2a_order),
}


def main():
    ap = argparse.ArgumentParser(
        description="Generate a collective schedule as a booksim_flowtest trace.")
    ap.add_argument("-n", "--nodes", type=int, required=True,
                    help="number of ranks (must match the topology's node count)")
    ap.add_argument("-s", "--size", type=int, default=1 << 20,
                    help="collective buffer size in bytes per rank (default 1 MiB)")
    ap.add_argument("-a", "--algo", choices=sorted(ALGORITHMS), default="ring")
    ap.add_argument("-g", "--group", type=int, default=4,
                    help="ranks per switch group, for hier_ring (default 4)")
    ap.add_argument("--order", default=None,
                    help="comma-separated physical node id for each logical rank, "
                         "e.g. 0,1,2,3,6,7,4,5 means logical rank 0 sits on node 0, "
                         "rank 1 on node 1, etc. Models GPU placement and applies to "
                         "every algorithm, not just the ring-shaped ones.")
    ap.add_argument("--a2a-order", choices=["shift", "xor", "naive"], default="shift",
                    help="destination order of each rank in alltoall (default shift). "
                         "naive is ascending, which makes every source start on rank 0 "
                         "(an incast); kept only to reproduce old numbers.")
    ap.add_argument("-o", "--out", default="-", help="output file, or - for stdout")
    args = ap.parse_args()

    if args.nodes < 2:
        sys.exit("need at least 2 ranks")
    if args.algo == "alltoall" and args.a2a_order == "xor" and args.nodes & (args.nodes - 1):
        sys.exit("--a2a-order xor needs a power-of-two rank count")
    if args.size < args.nodes:
        sys.exit("size must be at least one byte per rank")

    args.order_list = None
    if args.order:
        args.order_list = [int(x) for x in args.order.split(",")]
        if sorted(args.order_list) != list(range(args.nodes)):
            sys.exit("--order must be a permutation of 0..%d" % (args.nodes - 1))

    t = Trace(perm=args.order_list)
    ALGORITHMS[args.algo](t, args)
    if not t.lines:
        sys.exit("algorithm produced no transfers")

    header = ("%s%s, %d ranks, %d B per rank, %d flows%s"
              % (args.algo,
                 (" " + args.a2a_order) if args.algo == "alltoall" else "",
                 args.nodes, args.size, t.next_id,
                 (", order " + args.order) if args.order else ""))
    if args.out == "-":
        t.emit(sys.stdout, header)
    else:
        with open(args.out, "w") as f:
            t.emit(f, header)
        sys.stderr.write("%s -> %s\n" % (header, args.out))


if __name__ == "__main__":
    main()
