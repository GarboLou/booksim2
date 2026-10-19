#!/usr/bin/env python3
"""Sweep driver: enumerate topologies x placements x collectives/mixes x
message sizes, run each through booksim_flowtest, aggregate into a ranked
results table.

booksim::Engine is one-per-process (src/engine/booksim_engine.hpp), so each
combination runs booksim_flowtest as a separate subprocess -- this is a
requirement, not a convenience: running combinations in-process against
libbooksim.a directly would violate the one-Engine-per-process constraint
the moment two needed to coexist, and it also gives crash isolation for
free (one bad topology/config can't take the whole sweep down).

Sweep spec: JSON, see the example under `if __name__ == "__main__"` or
runfiles/generated/example_sweep.json once one has been run.

Optional spec keys:
  "routing": list of variants, each {"name": ..., <config key>: <value>, ...};
             every combination runs once per variant, with those keys passed
             to booksim_flowtest as overrides. Default: one variant, "default",
             with no overrides. Example:
               [{"name": "min", "routing_function": "min"},
                {"name": "pinned_req4k", "routing_function": "pinned",
                 "multipath_granularity": "request", "dma_request_bytes": 4096},
                {"name": "pinned_loopfree", "routing_function": "pinned",
                 "pcinet_deadlock_avoidance": "loopfree"}]
  "timeout_s": per-run wall-clock limit (default 600); a run that hits it is
             recorded with status HANG.
"""

import argparse
import itertools
import json
import os
import subprocess
import sys

import gen_topology
import merge_traces
import place_search

BOOKSIM_FLOWTEST = os.path.join(os.path.dirname(__file__), "..", "src", "tools", "booksim_flowtest")
GEN_DIR = "runfiles/generated"


def _cartesian(spec):
    """spec is a dict where some values are lists (swept) and others are
    scalars (fixed). Yields one dict per combination, list-valued fields
    replaced by one of their values."""
    list_fields = {k: v for k, v in spec.items() if isinstance(v, list)}
    fixed_fields = {k: v for k, v in spec.items() if not isinstance(v, list)}
    if not list_fields:
        yield dict(fixed_fields)
        return
    keys = list(list_fields)
    for combo in itertools.product(*(list_fields[k] for k in keys)):
        out = dict(fixed_fields)
        out.update(zip(keys, combo))
        yield out


def _topology_args(combo):
    ns = argparse.Namespace()
    ns.family = combo["family"]
    ns.switches = combo["switches"]
    ns.gpus_per_switch = combo["gpus_per_switch"]
    ns.switch_radix = combo.get("switch_radix", 64)
    ns.ntb = combo.get("ntb")
    ns.link_gen = combo.get("link_gen", 5)
    ns.link_width = combo.get("link_width", 16)
    ns.link_bw_gbps = combo.get("link_bw_gbps")
    ns.link_cpf = combo.get("link_cpf")
    ns.ntb_lat = combo.get("ntb_lat", 5)
    ns.link_lat = combo.get("link_lat", 2)
    ns.node_lat = combo.get("node_lat", 1)
    ns.grid = combo.get("grid")
    ns.domains = combo.get("domains", 2)
    ns.custom_graph = combo.get("custom_graph")
    return ns


def _slug(combo, index):
    bits = [combo["family"], "s%s" % combo["switches"], "p%s" % combo["gpus_per_switch"]]
    if combo.get("ntb"):
        bits.append("ntb-%s" % str(combo["ntb"]).replace(",", "_"))
    bits.append(str(index))
    return "_".join(str(b) for b in bits)


def enumerate_topologies(spec):
    """Yields (slug, combo, model, txt_path, cfg_path)."""
    seen = 0
    for topo_spec in spec["topologies"]:
        for combo in _cartesian(topo_spec):
            seen += 1
            slug = _slug(combo, seen)
            args = _topology_args(combo)
            model = gen_topology.build_topology(args)
            out_prefix = os.path.join(GEN_DIR, slug)
            txt_path, cfg_path = gen_topology.write_files(model, out_prefix, args.node_lat)
            yield slug, combo, model, txt_path, cfg_path


def total_gpus(model):
    return model.total_gpus()


def make_traces(model, txt_path, collective, candidates_per_topology, trace_dir):
    """Returns a list of (order_label, trace_path) for the given collective
    spec on the given topology, one per placement candidate (or a single
    entry for placement-insensitive algos)."""
    algo = collective["algo"]
    n = total_gpus(model)
    size = collective["size"]
    candidates = place_search.search(txt_path, algo, candidates_per_topology)

    out = []
    for i, cand in enumerate(candidates):
        order = cand["order"]
        trace_path = os.path.join(trace_dir, "%s_%s_o%d.trace" % (collective["name"], algo, i))
        os.makedirs(trace_dir, exist_ok=True)
        _gen_collective_to_file(algo, n, size, order, trace_path,
                                collective.get("a2a_order", "shift"))
        out.append((("order%d" % i, cand.get("proxy_cost")), trace_path))
    return out


def _gen_collective_to_file(algo, n, size, order, out_path, a2a_order="shift"):
    import gen_collective
    order_list = [int(x) for x in order.split(",")]
    t = gen_collective.Trace(perm=order_list)
    args = argparse.Namespace(nodes=n, size=size, group=4, order_list=order_list,
                              a2a_order=a2a_order)
    gen_collective.ALGORITHMS[algo](t, args)
    with open(out_path, "w") as f:
        header = "%s, %d ranks, %d B per rank, %d flows, order %s" % (algo, n, size, t.next_id, order)
        t.emit(f, header)


def run_booksim_flowtest(cfg_path, trace_path, csv_path, overrides=None, timeout_s=600):
    cmd = [BOOKSIM_FLOWTEST, cfg_path, trace_path, "--csv", csv_path]
    for key, value in (overrides or {}).items():
        cmd.append("%s=%s" % (key, value))
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
    except subprocess.TimeoutExpired as e:
        out = e.stdout.decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        return None, out, "timeout after %ss" % timeout_s
    return proc.returncode, proc.stdout, proc.stderr


# Settings booksim_flowtest echoes on its "multipath:" line, recorded per row.
MULTIPATH_KEYS = ["routing_function", "multipath_granularity", "dma_request_bytes",
                  "multipath_select", "pcinet_deadlock_avoidance"]
# Loop-free admission statistics (pcinet_deadlock_avoidance = loopfree).
LOOPFREE_STATS = ["lf_units", "lf_waited_units", "lf_wait_cycles", "lf_max_wait",
                  "lf_alt_path"]


def parse_stdout(stdout):
    out = {"status": "FAILED"}
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("flows completed"):
            done, total = line.split()[-3], line.split()[-1]
            out["flows_completed"] = int(done)
            out["flows_total"] = int(total)
        elif line.startswith("makespan"):
            out["makespan"] = int(line.split()[1])
        elif line.startswith("total bytes moved"):
            out["total_bytes"] = int(line.split()[-1])
        elif line.startswith("aggregate throughput"):
            out["throughput_bcyc"] = float(line.split()[-2])
        elif line.startswith("engine idle"):
            out["engine_idle"] = line.split()[-1] == "yes"
        elif line.startswith("flows outstanding"):
            out["flows_outstanding"] = int(line.split()[-1])
        elif line in ("OK", "FAILED"):
            out["status"] = line
        elif line.startswith("Error in") and "routing deadlock" in line:
            # FlowTrafficManager's watchdog aborted the run before any results
            out["status"] = "DEADLOCK"
        elif line.startswith("multipath:"):
            for tok in line.split()[1:]:
                key, _, value = tok.partition("=")
                if key in MULTIPATH_KEYS:
                    out[key] = value
        elif line.split() and line.split()[0] in LOOPFREE_STATS:
            out[line.split()[0]] = int(line.split()[1])
    return out


def run_sweep(spec, out_dir):
    trace_dir = os.path.join(GEN_DIR, "traces")
    results_dir = os.path.join(out_dir, "results")
    os.makedirs(results_dir, exist_ok=True)

    collectives_by_name = {c["name"]: c for c in spec["collectives"]}
    variants = spec.get("routing") or [{"name": "default"}]
    timeout_s = spec.get("timeout_s", 600)
    placement_cfg = spec.get("placement", {"strategy": "search", "candidates_per_topology": 4})
    k = placement_cfg.get("candidates_per_topology", 4)

    rows = []
    for slug, combo, model, txt_path, cfg_path in enumerate_topologies(spec):
        n_topo = total_gpus(model)

        # For each size variant of each collective, build its candidate traces.
        # A collective may pin an explicit "nodes" rank count (useful when a
        # spec sweeps topologies of different total GPU counts); if it does
        # and this topology doesn't match, skip and log rather than silently
        # generating a trace for the wrong rank count.
        collective_traces = {}   # (name, size) -> list of (order_label, path)
        for cspec in spec["collectives"]:
            required_n = cspec.get("nodes")
            if required_n is not None and required_n != n_topo:
                sys.stderr.write(
                    "sweep: skipping collective '%s' for topology %s "
                    "(needs %d GPUs, topology has %d)\n"
                    % (cspec["name"], slug, required_n, n_topo))
                continue
            if n_topo < 2:
                continue
            if cspec["algo"] not in place_search.RING_LIKE_ALGOS | {"tree", "alltoall"}:
                continue
            for size in (cspec["size"] if isinstance(cspec["size"], list) else [cspec["size"]]):
                c = dict(cspec, size=size)
                traces = make_traces(model, txt_path, c, k, trace_dir)
                collective_traces[(c["name"], size)] = traces

        for mix in spec["mixes"]:
            sizes_product = itertools.product(*[
                (collectives_by_name[name]["size"]
                 if isinstance(collectives_by_name[name]["size"], list)
                 else [collectives_by_name[name]["size"]])
                for name in mix
            ])
            for sizes in sizes_product:
                mix_key = list(zip(mix, sizes))
                # best (first, i.e. lowest proxy-cost) candidate trace per constituent
                constituent_traces = []
                skip = False
                for name, size in mix_key:
                    key = (name, size)
                    if key not in collective_traces or not collective_traces[key]:
                        skip = True
                        break
                    label, path = collective_traces[key][0]
                    constituent_traces.append((name, path))
                if skip:
                    sys.stderr.write("sweep: skipping %s / %s (GPU count mismatch)\n" % (slug, mix))
                    continue

                mix_name = "+".join(mix)
                mix_slug = "%s__%s" % (slug, mix_name)

                if len(constituent_traces) == 1:
                    trace_path = constituent_traces[0][1]
                    manifest = None
                else:
                    trace_path = os.path.join(trace_dir, mix_slug + "_merged.trace")
                    manifest_path = os.path.join(trace_dir, mix_slug + ".manifest.json")
                    manifest = merge_traces.merge(constituent_traces, trace_path, manifest_path)

                for variant in variants:
                    overrides = {k: v for k, v in variant.items() if k != "name"}
                    run_slug = mix_slug
                    if len(variants) > 1 or variant["name"] != "default":
                        run_slug += "__" + variant["name"]
                    csv_path = os.path.join(results_dir, run_slug + ".csv")

                    rc, stdout, stderr = run_booksim_flowtest(cfg_path, trace_path, csv_path,
                                                              overrides, timeout_s)
                    parsed = parse_stdout(stdout)
                    if rc is None:
                        parsed["status"] = "HANG"
                    if parsed.get("status") != "OK":
                        sys.stderr.write("sweep: %s %s: %s\n" % (run_slug, parsed.get("status"),
                                                                  stderr.strip()[:200]))

                    per_constituent = None
                    if manifest is not None and parsed.get("status") == "OK":
                        split_result = merge_traces.split(
                            os.path.join(trace_dir, mix_slug + ".manifest.json"), csv_path, None)
                        per_constituent = split_result["per_collective"]

                    row = {
                        "topology_slug": slug,
                        "family": combo["family"],
                        "switches": combo["switches"],
                        "gpus_per_switch": combo["gpus_per_switch"],
                        "ntb_config": combo.get("ntb"),
                        "switch_radix": combo.get("switch_radix"),
                        "collective_or_mix": mix_name,
                        "routing_variant": variant["name"],
                        "message_sizes": dict(mix_key),
                        "makespan": parsed.get("makespan"),
                        "total_bytes": parsed.get("total_bytes"),
                        "throughput_bcyc": parsed.get("throughput_bcyc"),
                        "status": parsed.get("status"),
                        "per_constituent": per_constituent,
                    }
                    for key in MULTIPATH_KEYS + LOOPFREE_STATS:
                        row[key] = parsed.get(key)
                    rows.append(row)

    # vs_sum for multi-collective mixes: combined makespan / sum of each
    # constituent's own isolated (singleton-mix) makespan, per the same
    # topology. Isolated rows are the ones whose collective_or_mix has no "+".
    isolated_makespan = {}
    for r in rows:
        if "+" not in r["collective_or_mix"] and r["status"] == "OK":
            isolated_makespan[(r["topology_slug"], r["routing_variant"],
                               r["collective_or_mix"])] = r["makespan"]
    for r in rows:
        if "+" in r["collective_or_mix"] and r["status"] == "OK":
            names = r["collective_or_mix"].split("+")
            iso = [isolated_makespan.get((r["topology_slug"], r["routing_variant"], n))
                   for n in names]
            if all(v is not None for v in iso) and sum(iso) > 0:
                r["vs_sum"] = r["makespan"] / sum(iso)
            else:
                r["vs_sum"] = None
        else:
            r["vs_sum"] = None

    results_csv = os.path.join(results_dir, "sweep_results.csv")
    _write_results_csv(rows, results_csv)
    summary_txt = os.path.join(results_dir, "sweep_summary.txt")
    _write_summary(rows, summary_txt)
    return rows, results_csv, summary_txt


def _write_results_csv(rows, path):
    import csv
    fields = ["topology_slug", "family", "switches", "gpus_per_switch", "ntb_config",
              "switch_radix", "collective_or_mix", "routing_variant", "message_sizes",
              "makespan", "total_bytes", "throughput_bcyc", "status", "vs_sum",
              "per_constituent"] + MULTIPATH_KEYS + LOOPFREE_STATS
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in rows:
            row = dict(r)
            row["message_sizes"] = json.dumps(row["message_sizes"])
            row["per_constituent"] = json.dumps(row["per_constituent"]) if row["per_constituent"] else ""
            w.writerow(row)


def _write_summary(rows, path):
    groups = {}
    for r in rows:
        groups.setdefault(r["collective_or_mix"], []).append(r)
    lines = []
    for mix_name in sorted(groups):
        lines.append("=== %s ===" % mix_name)
        ranked = sorted((r for r in groups[mix_name] if r["status"] == "OK"),
                         key=lambda r: r["makespan"])
        for r in ranked:
            lines.append("  %-30s %-20s makespan=%-10s vs_sum=%s"
                          % (r["topology_slug"], r["routing_variant"], r["makespan"],
                             r.get("vs_sum")))
        failed = [r for r in groups[mix_name] if r["status"] != "OK"]
        for r in failed:
            lines.append("  %-30s %-20s STATUS=%s" % (r["topology_slug"], r["routing_variant"],
                                                     r["status"]))
        lines.append("")
    with open(path, "w") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("spec", help="sweep spec JSON file")
    ap.add_argument("--out-dir", default=GEN_DIR)
    args = ap.parse_args()

    with open(args.spec) as f:
        spec = json.load(f)
    rows, results_csv, summary_txt = run_sweep(spec, args.out_dir)
    sys.stderr.write("%d runs -> %s, %s\n" % (len(rows), results_csv, summary_txt))


if __name__ == "__main__":
    main()
