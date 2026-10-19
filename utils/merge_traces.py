#!/usr/bin/env python3
"""Merge independently-generated collective traces into one, for running
concurrent collective mixes (e.g. a ring AllReduce alongside an all-to-all,
modeling a MoE step) through a single booksim_flowtest invocation.

booksim::Engine is one-per-process (see src/engine/booksim_engine.hpp) and
booksim_flowtest loads exactly one trace file, so this is the only way to
measure two collectives sharing the fabric: assign each trace a contiguous
id block, rewrite its flows' ids and internal after= references by that
block's offset (dependencies never cross trace boundaries -- collectives are
otherwise-independent programs sharing only the fabric), and concatenate.

--split does the reverse: given the manifest this produced and the --csv
output of running the merged trace (using the trace_id column
booksim_flowtest's per-flow CSV carries -- see src/tools/booksim_flowtest.cpp
and src/flowtrafficmanager.cpp), bucket completed flows back into their
original collective and report each one's own makespan/bytes/throughput
*within the shared run* (how much that collective was individually delayed
by contention), plus the whole-run combined makespan.

Note this is deliberately not each collective's isolated makespan (run
alone, nothing to contend with). Isolated makespans, and the resulting
combined/sum-of-isolated ratio, require running each constituent trace by
itself too, which is what utils/sweep.py's singleton mixes are for; this
module only knows about the one (possibly merged) run it was given.
"""

import argparse
import csv
import json
import sys

import trace_io


def merge(named_traces, out_trace, out_manifest):
    """named_traces: list of (name, path) in the order id blocks are
    assigned. Returns the manifest dict (also written to out_manifest)."""
    constituents = []
    all_flows = []
    offset = 0
    for name, path in named_traces:
        headers, flows = trace_io.parse(path)
        rewritten = []
        for f in flows:
            rewritten.append(trace_io.Flow(
                id=f.id + offset,
                src=f.src, dst=f.dst, bytes=f.bytes,
                after=[a + offset for a in f.after],
                at=f.at,
                pcie_class=f.pcie_class,
            ))
        all_flows.extend(rewritten)
        constituents.append({
            "name": name, "source": path,
            "id_lo": offset, "id_hi": offset + len(flows),
            "header": headers[0] if headers else "",
        })
        offset += len(flows)

    header_line = "merged trace: " + " ; ".join(
        "%s [%d,%d) <- %s" % (c["name"], c["id_lo"], c["id_hi"], c["source"])
        for c in constituents)
    with open(out_trace, "w") as f:
        trace_io.write(f, all_flows, header_line)

    manifest = {"constituents": constituents}
    with open(out_manifest, "w") as f:
        json.dump(manifest, f, indent=2)
    return manifest


def _bucket_for(trace_id, constituents):
    for c in constituents:
        if c["id_lo"] <= trace_id < c["id_hi"]:
            return c["name"]
    return None


def split(manifest_path, csv_path, out_prefix):
    with open(manifest_path) as f:
        manifest = json.load(f)
    constituents = manifest["constituents"]

    buckets = {c["name"]: [] for c in constituents}
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            trace_id = int(row["trace_id"])
            name = _bucket_for(trace_id, constituents)
            if name is None:
                continue  # shouldn't happen for a well-formed manifest/csv pair
            buckets[name].append(row)

    per_collective = {}
    for name, rows in buckets.items():
        if not rows:
            per_collective[name] = {"makespan": 0, "bytes": 0, "throughput": 0.0, "n_flows": 0}
            continue
        t_inject_min = min(int(r["t_inject"]) for r in rows)
        t_done_max = max(int(r["t_done"]) for r in rows)
        makespan = t_done_max - t_inject_min
        total_bytes = sum(int(r["bytes"]) for r in rows)
        throughput = (total_bytes / makespan) if makespan > 0 else 0.0
        per_collective[name] = {
            "makespan": makespan, "bytes": total_bytes,
            "throughput": throughput, "n_flows": len(rows),
        }

    all_rows = [r for rows in buckets.values() for r in rows]
    combined_makespan = max((int(r["t_done"]) for r in all_rows), default=0)

    # Deliberately no "vs isolated sum" here -- that needs each collective's
    # makespan from a separate solo run, which this function doesn't have
    # (see module docstring). sweep.py computes that, from its own singleton-
    # mix rows, once this per-collective/combined data is available.
    result = {
        "per_collective": per_collective,
        "combined_makespan": combined_makespan,
    }
    if out_prefix:
        with open(out_prefix + ".json", "w") as f:
            json.dump(result, f, indent=2)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--trace", action="append", default=[], metavar="NAME=PATH",
                    help="repeatable; id blocks are assigned in the order given")
    ap.add_argument("--out", help="merged trace output path")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--split", action="store_true",
                    help="split mode: read --manifest + --csv, write --out-prefix.json")
    ap.add_argument("--csv", help="split mode: the --csv output of running the merged trace")
    ap.add_argument("--out-prefix", help="split mode: output file prefix")
    args = ap.parse_args()

    if args.split:
        if not args.csv:
            sys.exit("--split requires --csv")
        result = split(args.manifest, args.csv, args.out_prefix)
        print(json.dumps(result, indent=2))
        return

    if not args.trace or not args.out:
        sys.exit("merge mode requires --trace NAME=PATH (repeatable) and --out")
    named = []
    for spec in args.trace:
        name, path = spec.split("=", 1)
        named.append((name, path))
    manifest = merge(named, args.out, args.manifest)
    sys.stderr.write("merged %d traces -> %s (%s)\n" % (len(named), args.out, args.manifest))
    for c in manifest["constituents"]:
        sys.stderr.write("  %s: ids [%d, %d)\n" % (c["name"], c["id_lo"], c["id_hi"]))


if __name__ == "__main__":
    main()
