#!/usr/bin/env python3
"""Rank placement search: which physical GPU should each logical rank sit on.

A placement is exactly gen_collective.py's --order string: a permutation
`order[logical_rank] -> physical_node_id`. The search operates at switch
granularity, not GPU granularity: which specific co-located GPU on a switch
is "rank i" vs "rank i+1" never changes which inter-switch link carries which
hop, so intra-switch order is fixed (ascending physical node id) and only the
switch-visiting sequence is searched. This is what collapses the search from
N! (over GPUs; 40,320 for 8 GPUs) to S! (over switches; 24 for 4 switches,
before rotations and reflections of the ring are folded away).

The proxy cost (sum of switch-graph shortest-path lengths between
consecutive ring stops) is a shortlist filter, not ground truth. On the
4-switch mesh_grid (2 GPUs per switch, 1 MiB ring AllReduce), the identity
order raises the mean switches per TLP only from 1.50 to 1.75 (17%) over the
order this module picks, but takes 2.0x as long (72,562 vs 35,958 cycles),
because one switch-to-switch link carries two ring transfers per step. Real
makespan from an actual booksim_flowtest run is always the final arbiter;
this module only narrows which orders are worth running.
"""

import argparse
import itertools
import json
import sys
from collections import deque


# ---------------------------------------------------------------------------
# PCINet topology parsing (switch adjacency + GPU attachment only -- not a
# full PCINet reader, just enough to recover the switch graph). Handles the
# one-clause-per-line convention gen_topology.py always emits; hand-authored
# files that chain multiple node clauses onto one line (e.g. the flat
# example) aren't fully parsed, but a flat (single-switch) topology has no
# placement decision to make anyway.
# ---------------------------------------------------------------------------

def _read_attrs(tok, i):
    attrs = {}
    while i < len(tok) and tok[i] not in ("node", "router"):
        key = tok[i]
        if key in ("port_type", "link_type"):
            attrs[key] = tok[i + 1]
            i += 2
        elif key == "domain":
            attrs["domain"] = int(tok[i + 1])
            i += 2
        elif key == "lat":
            attrs["lat"] = int(tok[i + 1])
            i += 2
        elif key in ("gen", "width", "bw_gbps", "cpf"):
            attrs[key] = tok[i + 1]
            i += 2
        else:
            break
    return attrs, i


class Topology:
    def __init__(self):
        self.switches = set()
        self.node_switch = {}      # node id -> switch id
        self.node_attrs = {}       # node id -> attrs dict
        self.edges = []            # (u, v, attrs)

    def switch_of(self, node_id):
        return self.node_switch[node_id]

    def gpu_nodes_by_switch(self):
        """Real GPU nodes only, grouped by switch, sorted ascending -- this
        is what placement search permutes over. Excludes the inert NVSwitch
        placeholder node (see runfiles/pcie_simai_flat.txt): every real GPU
        clause carries `port_type downstream`, the placeholder carries none,
        which is the marker gen_topology.py itself relies on."""
        by_switch = {s: [] for s in self.switches}
        for n, s in self.node_switch.items():
            if "port_type" in self.node_attrs.get(n, {}):
                by_switch[s].append(n)
        for s in by_switch:
            by_switch[s].sort()
        return by_switch

    def adjacency(self):
        """dict[frozenset({u,v})] -> (port_type, link_type) color, for
        automorphism/shortest-path use."""
        adj = {}
        for u, v, attrs in self.edges:
            adj[frozenset((u, v))] = (attrs.get("port_type"), attrs.get("link_type"))
        return adj


def parse_topology(path):
    t = Topology()
    with open(path) as f:
        for raw in f:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            tok = line.split()
            if tok[0] != "router":
                continue
            i = 1
            rid = int(tok[i])
            i += 1
            t.switches.add(rid)
            while i < len(tok):
                kind = tok[i]
                if kind == "domain" and i == 2:
                    i += 2  # bare "router <r> domain <d>" -- no attrs to record here
                    continue
                if kind == "node":
                    nid = int(tok[i + 1])
                    i += 2
                    attrs, i = _read_attrs(tok, i)
                    t.node_switch[nid] = rid
                    t.node_attrs[nid] = attrs
                    continue
                if kind == "router":
                    rid2 = int(tok[i + 1])
                    i += 2
                    attrs, i = _read_attrs(tok, i)
                    t.switches.add(rid2)
                    t.edges.append((rid, rid2, attrs))
                    continue
                break
    return t


# ---------------------------------------------------------------------------
# Switch-graph automorphisms (edge-colored, so NTB links never fold onto
# transparent ones). Exact backtracking search -- fine for the switch counts
# in scope here (single digits to low teens); pruning on color-consistency
# kills almost every branch on sparse graphs.
# ---------------------------------------------------------------------------

def switch_automorphisms(num_switches, adj, budget=20000):
    """Returns a list of permutations (tuples) of range(num_switches) that
    preserve the edge-colored adjacency. Bails out (returns just the
    identity) if the search would exceed `budget` node visits -- folding
    automorphisms is an optimization, not a correctness requirement."""
    S = num_switches
    perm = [None] * S
    used = [False] * S
    result = []
    visits = [0]

    def color(u, v):
        return adj.get(frozenset((u, v)))

    def consistent(k):
        for u in range(k + 1):
            for v in range(u + 1, k + 1):
                if color(u, v) != color(perm[u], perm[v]):
                    return False
        return True

    def backtrack(k):
        if visits[0] > budget:
            return
        visits[0] += 1
        if k == S:
            result.append(tuple(perm))
            return
        for cand in range(S):
            if not used[cand]:
                perm[k] = cand
                used[cand] = True
                if consistent(k):
                    backtrack(k + 1)
                used[cand] = False
        perm[k] = None

    backtrack(0)
    if visits[0] > budget:
        return [tuple(range(S))]
    return result


def shortest_paths(num_switches, adj):
    """All-pairs shortest path length (hop count; the proxy's pluggable edge
    weight -- becomes bandwidth-aware once the NTB derate is calibrated)."""
    dist = [[None] * num_switches for _ in range(num_switches)]
    neighbors = {s: [] for s in range(num_switches)}
    for key in adj:
        u, v = tuple(key)
        neighbors[u].append(v)
        neighbors[v].append(u)
    for src in range(num_switches):
        dist[src][src] = 0
        q = deque([src])
        while q:
            u = q.popleft()
            for v in neighbors[u]:
                if dist[src][v] is None:
                    dist[src][v] = dist[src][u] + 1
                    q.append(v)
    return dist


def necklace_cost(necklace, dist):
    n = len(necklace)
    return sum(dist[necklace[i]][necklace[(i + 1) % n]] for i in range(n))


def canonical_necklace(seq, automorphisms):
    n = len(seq)
    variants = []
    for rot in range(n):
        rotated = seq[rot:] + seq[:rot]
        variants.append(rotated)
        variants.append(tuple(reversed(rotated)))
    best = None
    for v in variants:
        for auto in automorphisms:
            relabeled = tuple(auto[x] for x in v)
            if best is None or relabeled < best:
                best = relabeled
    return best


# Above this many switches, exact S!-enumeration is not attempted; fall back
# to structured + sampled candidates instead. Automorphism folding is capped
# separately (switch_automorphisms' own `budget`) since a dense graph's
# automorphism group can be large even for small S (e.g. full_mesh's is S!).
_EXACT_SWITCH_LIMIT = 8
_MAX_CANDIDATES_MID = 200000   # cap for the 8 < S <= ~11 tier
_MID_SWITCH_LIMIT = 11
_SAMPLE_COUNT = 2000


def _greedy_tour(num_switches, dist):
    unvisited = set(range(num_switches))
    cur = 0
    unvisited.remove(cur)
    tour = [cur]
    while unvisited:
        nxt = min(unvisited, key=lambda v: dist[cur][v])
        tour.append(nxt)
        unvisited.remove(nxt)
        cur = nxt
    return tuple(tour)


def switch_necklaces(num_switches, adj, dist, max_results=8):
    """Returns up to max_results switch-visiting-order tuples, ranked by the
    hop-count proxy ascending, deduplicated by ring rotation/reflection (and,
    when cheap, topology automorphism) symmetry."""
    automorphisms = switch_automorphisms(num_switches, adj)

    if num_switches <= 2:
        return [tuple(range(num_switches))]

    if num_switches <= _EXACT_SWITCH_LIMIT:
        seen_canon = set()
        candidates = []
        for p in itertools.permutations(range(num_switches)):
            if p[0] != 0:
                continue  # rotation symmetry: fix the start
            c = canonical_necklace(p, automorphisms)
            if c in seen_canon:
                continue
            seen_canon.add(c)
            candidates.append(p)
    elif num_switches <= _MID_SWITCH_LIMIT:
        seen_canon = set()
        candidates = []
        count = 0
        for p in itertools.permutations(range(num_switches)):
            if p[0] != 0:
                continue
            count += 1
            if count > _MAX_CANDIDATES_MID:
                break
            c = canonical_necklace(p, automorphisms) if len(automorphisms) <= 5000 else p
            if c in seen_canon:
                continue
            seen_canon.add(c)
            candidates.append(p)
    else:
        # Heuristic shortlist: structured "obviously good" candidates plus
        # random sampling. Not exhaustive above this point, and that's an
        # explicit, honest trade-off -- the real arbiter is the actual
        # booksim_flowtest run on whatever gets shortlisted here.
        import random
        rng = random.Random(0)
        candidates = [_greedy_tour(num_switches, dist), tuple(range(num_switches))]
        for _ in range(_SAMPLE_COUNT):
            perm = list(range(num_switches))
            rng.shuffle(perm)
            candidates.append(tuple(perm))
        # de-dup cheaply (ring rotation/reflection only -- automorphism
        # folding is skipped here since we're not exhaustive anyway)
        seen_canon = set()
        deduped = []
        for p in candidates:
            c = canonical_necklace(p, [tuple(range(num_switches))])
            if c in seen_canon:
                continue
            seen_canon.add(c)
            deduped.append(p)
        candidates = deduped

    candidates.sort(key=lambda p: necklace_cost(p, dist))
    return candidates[:max_results]


def necklace_to_order(necklace, gpu_by_switch):
    order = []
    for s in necklace:
        order.extend(gpu_by_switch[s])
    return order


# ---------------------------------------------------------------------------
# Binomial tree placement: recursively bisect the switch set to minimize
# inter-switch edges crossing the cut, so binomial-tree partners at small
# `dist` land within the same half.
# ---------------------------------------------------------------------------

_MAX_BISECT_SWITCHES = 14


def _cut_size(a, b, adj):
    cnt = 0
    for key in adj:
        u, v = tuple(key)
        if (u in a and v in b) or (u in b and v in a):
            cnt += 1
    return cnt


def _bisect_order(switches, adj, gpu_by_switch):
    if len(switches) == 1:
        return list(gpu_by_switch[switches[0]])
    if len(switches) > _MAX_BISECT_SWITCHES:
        # too many balanced bipartitions to try exhaustively; fall back to
        # the natural (sorted) switch order for this subset
        out = []
        for s in sorted(switches):
            out.extend(gpu_by_switch[s])
        return out

    half = len(switches) // 2
    best_cut = None
    best_split = None
    for combo in itertools.combinations(switches, half):
        a = set(combo)
        b = set(switches) - a
        cut = _cut_size(a, b, adj)
        if best_cut is None or cut < best_cut:
            best_cut = cut
            best_split = (sorted(a), sorted(b))
    a, b = best_split
    return _bisect_order(a, adj, gpu_by_switch) + _bisect_order(b, adj, gpu_by_switch)


# ---------------------------------------------------------------------------
# Public entry point
# ---------------------------------------------------------------------------

RING_LIKE_ALGOS = {"ring", "hier_ring", "allgather", "reducescatter"}


def search(topology_path, algo, candidates=8):
    """Returns a list of {"order": "0,1,2,...", "proxy_cost": N} dicts,
    ranked ascending by the proxy (for ring-like algos) or a single no-op /
    bisection-derived entry (alltoall / tree)."""
    t = parse_topology(topology_path)
    S = len(t.switches)
    gpu_by_switch = t.gpu_nodes_by_switch()
    n_gpus = sum(len(v) for v in gpu_by_switch.values())
    identity = ",".join(str(i) for i in range(n_gpus))

    if algo == "alltoall":
        # Placement-insensitive on the vertex-transitive family shapes in
        # scope here -- every rank talks to every other rank in equal
        # volume, so searching this axis wastes sweep budget. Not a
        # universal truth for an arbitrary non-vertex-transitive custom
        # topology, but true for every family gen_topology.py builds.
        return [{"order": identity, "proxy_cost": 0}]

    if S <= 1:
        return [{"order": identity, "proxy_cost": 0}]

    adj = t.adjacency()
    dist = shortest_paths(S, adj)

    if algo == "tree":
        order = _bisect_order(sorted(t.switches), adj, gpu_by_switch)
        return [{"order": ",".join(str(n) for n in order), "proxy_cost": None}]

    if algo not in RING_LIKE_ALGOS:
        sys.exit("place_search: unknown algo %r" % algo)

    necklaces = switch_necklaces(S, adj, dist, max_results=candidates)
    results = []
    for necklace in necklaces:
        order = necklace_to_order(necklace, gpu_by_switch)
        results.append({
            "order": ",".join(str(n) for n in order),
            "proxy_cost": necklace_cost(necklace, dist),
            "switch_necklace": list(necklace),
        })
    return results


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--topology", required=True, help="a PCINet .txt file")
    ap.add_argument("--algo", required=True,
                    choices=sorted(RING_LIKE_ALGOS | {"tree", "alltoall"}))
    ap.add_argument("--candidates", type=int, default=8)
    ap.add_argument("--out", default="-")
    args = ap.parse_args()

    results = search(args.topology, args.algo, args.candidates)
    text = json.dumps(results, indent=2)
    if args.out == "-":
        print(text)
    else:
        with open(args.out, "w") as f:
            f.write(text)


if __name__ == "__main__":
    main()
