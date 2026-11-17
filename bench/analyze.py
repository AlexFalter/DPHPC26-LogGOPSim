#!/usr/bin/env python3
"""Turns raw campaign data into tables (CSV + Markdown) and plots (PDF + PNG).

usage: analyze.py [CAMPAIGN_DIR ...] [--out-name NAME] [--exclude-noisy] [--no-plots]

Default: the newest campaign in bench/results/raw/. Several campaigns (e.g. local + cluster) are
analysed together; each row keeps its host. Everything is computed from runs.jsonl (and the
campaign's metadata.json / correctness report) only. Outputs:
  bench/results/tables/<name>/  summary, phases, decode, breakeven, file_sizes, memory, convert,
                                concurrency, overhead, ranks, workloads, environment (.csv + .md)
  bench/results/plots/<name>/   e2e_vs_size, phases, decode_throughput, breakeven, file_size,
                                rss_vs_size, distributions, convert_scaling, concurrency,
                                rank_sweep, workloads (.pdf + .png; needs matplotlib)
Every number is a median with a 95% nonparametric CI (order statistics); speedups are ratios of
medians with a percentile-bootstrap CI, always reported with both absolute times and the exact
pair of configs and trace.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import benchlib as bl  # noqa: E402

RESULTS = bl.results_dir()

# Fixed visual identity per config (color = decoding path, line style = build). The categorical
# hues are the dataviz skill's validated default order; BASE is drawn in ink as the reference.
INK = "#0b0b0b"
SLOT = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
FAMILY_COLOR = {"base": INK, "legacy": SLOT[0], "plan": SLOT[1], "jit-cold": SLOT[2], "jit-warm": SLOT[3],
                "orc": SLOT[4], "tnode": SLOT[6], "tschema": SLOT[7], "upstream": INK, "jit": SLOT[3]}
BUILD_STYLE = {"base": "-", "master": "-", "master-plain": ":", "lazy": "--", "omp": "-."}
MARKERS = {"base": "s", "legacy": "o", "plan": "^", "jit-cold": "v", "jit-warm": "D", "orc": "P", "tnode": "X",
           "tschema": "*", "upstream": "s", "jit": "D"}


def family(config, catalog):
    c = catalog.get(config, {})
    if c.get("build") == "base":
        return "base"
    if c.get("file") == "legacy":
        return "legacy"
    d = c.get("decoder") or "plan"
    if d == "jit":
        return "jit-cold" if c.get("jit_cache") == "cold" else "jit-warm"
    return {"templated_node": "tnode", "templated_schema": "tschema"}.get(d, d)


def style(config, catalog):
    fam = family(config, catalog)
    build = catalog.get(config, {}).get("build", "master")
    ls = BUILD_STYLE.get(build, "-")
    if "notiming" in config:
        ls = (0, (1, 1))
    if "savemem" in config:
        ls = (0, (5, 1, 1, 1))
    return {"color": FAMILY_COLOR.get(fam, SLOT[5]), "linestyle": ls, "marker": MARKERS.get(fam, "o")}


# ------------------------------------------------------------------ loading

def load(campaigns, exclude_noisy):
    runs, meta, correctness = [], {}, {}
    for d in campaigns:
        d = Path(d)
        m = json.loads((d / "metadata.json").read_text()) if (d / "metadata.json").exists() else {}
        meta[d.name] = m
        c = d / "correctness" / "correctness.json"
        if c.exists():
            correctness[d.name] = json.loads(c.read_text())
        for line in (d / "runs.jsonl").read_text().splitlines() if (d / "runs.jsonl").exists() else []:
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if exclude_noisy and r.get("noisy"):
                continue
            runs.append(r)
    return runs, meta, correctness


def measured(runs, kind):
    return [r for r in runs if r.get("kind") == kind and not r.get("warmup") and r.get("status") == "ok"]


def ci(xs):
    m, lo, hi, exact = bl.median_ci(xs)
    return m, lo, hi, exact


def fmt(x, digits=3):
    if x is None or (isinstance(x, float) and math.isnan(x)):
        return "–"
    if abs(x) >= 1000:
        return f"{x:,.0f}"
    return f"{x:.{digits}g}"


def write_table(out_dir: Path, name, rows, columns, title, notes=""):
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(out_dir / f"{name}.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=columns, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    md = [f"# {title}", ""]
    if notes:
        md += [notes, ""]
    md += ["| " + " | ".join(columns) + " |", "|" + "---|" * len(columns)]
    for r in rows:
        md.append("| " + " | ".join(fmt(r.get(c)) if isinstance(r.get(c), float) else str(r.get(c, "–"))
                                    for c in columns) + " |")
    (out_dir / f"{name}.md").write_text("\n".join(md) + "\n")


# ------------------------------------------------------------------ end-to-end

def group(runs, keys):
    g = defaultdict(list)
    for r in runs:
        g[tuple(r.get(k) for k in keys)].append(r)
    return g


def e2e_summary(runs, catalog, correctness):
    e2e = measured(runs, "e2e")
    keys = ("host", "experiment", "trace", "workload", "records", "ranks", "schema", "cache", "config")
    groups = group(e2e, keys)
    status = {}
    for rep in correctness.values():
        for tid, t in rep.get("traces", {}).items():
            for cfg, v in t.get("configs", {}).items():
                status[(tid, cfg)] = v["status"]
    rows = []
    for k, rs in sorted(groups.items(), key=lambda kv: (kv[0][0], kv[0][1], kv[0][4], kv[0][2], kv[0][7], kv[0][8])):
        host, exp, tid, wl, n, p, sch, cache, cfg = k
        wall = [r["wall_ns"] / 1e6 for r in rs]
        m, lo, hi, exact = ci(wall)
        base = groups.get((host, exp, tid, wl, n, p, sch, cache, "BASE"))
        row = {"host": host, "experiment": exp, "trace": tid, "workload": wl, "records": n, "ranks": p, "schema": sch,
               "cache": cache, "config": cfg, "n": len(wall), "median_ms": m, "ci_lo_ms": lo, "ci_hi_ms": hi,
               "ci_exact": exact, "ci_halfwidth_pct": 100 * (hi - lo) / 2 / m if m else None,
               "correctness": status.get((tid, cfg), "not_checked"),
               "peak_rss_mb": bl.median([r["maxrss_kb"] / 1024 for r in rs]),
               "peak_anon_mb": bl.median([(r.get("peak_rss_anon_kb") or 0) / 1024 for r in rs]),
               "majflt": bl.median([r.get("majflt") or 0 for r in rs]),
               "noisy_runs": sum(1 for r in rs if r.get("noisy")),
               "matches_reference": all(r.get("matches_reference") is not False for r in rs)}
        sims = [r["phases"]["phases"].get("simulate") for r in rs if r.get("phases")]
        events = rs[0].get("events")
        if events:  # events/s of the whole process (comparable with BASE) and of the event loop alone
            row["events_per_s"] = events / (m / 1e3) if m else None
            if any(sims):
                row["loop_events_per_s"] = events / (bl.median([s for s in sims if s]) / 1e9)
        if base and cfg != "BASE":
            bw = [r["wall_ns"] / 1e6 for r in base]
            s, slo, shi = bl.bootstrap_ratio_ci(bw, wall)
            row.update(base_median_ms=bl.median(bw), speedup_vs_base=s, speedup_ci_lo=slo, speedup_ci_hi=shi)
        rows.append(row)
    return rows


def phase_rows(runs):
    rows = []
    groups = group([r for r in measured(runs, "e2e") if r.get("phases")], ("host", "experiment", "trace", "cache", "config"))
    for k, rs in sorted(groups.items()):
        names = sorted({p for r in rs for p in r["phases"]["phases"]})
        row = {"host": k[0], "experiment": k[1], "trace": k[2], "cache": k[3], "config": k[4], "n": len(rs),
               "wall_ms": bl.median([r["wall_ns"] / 1e6 for r in rs])}
        for p in names:
            vals = [r["phases"]["phases"].get(p, 0) / 1e6 for r in rs]
            m, lo, hi, _ = ci(vals)
            row[p + "_ms"], row[p + "_ci"] = m, f"[{fmt(lo)}, {fmt(hi)}]"
        row["process_overhead_ms"] = row["wall_ms"] - row.get("total_ms", row["wall_ms"])
        caches = {r["phases"].get("notes", {}).get("jit_cache") for r in rs} - {None}
        row["jit_cache"] = ",".join(sorted(caches)) if caches else ""
        rows.append(row)
    return rows


# ------------------------------------------------------------------ decode microbenchmark

def micro_rows(runs):
    rows, checks = [], defaultdict(set)
    groups = group(measured(runs, "micro"), ("host", "experiment", "trace", "variant", "order", "fields", "cache",
                                              "jit_cold", "core_class"))
    for k, rs in sorted(groups.items()):
        host, exp, tid, var, order, fields, cache, jit_cold, core = k
        ns = [r["ns_per_record"] for r in rs]
        m, lo, hi, exact = ci(ns)
        rb = rs[0]["record_bytes"]
        row = {"host": host, "experiment": exp, "trace": tid, "records": rs[0]["records"], "variant": var,
               "runner": rs[0].get("runner_actual"), "order": order, "fields": fields, "cache": cache,
               "jit_cold": jit_cold, "core_class": core, "n": len(ns), "processes": len({r["process"] for r in rs}),
               "ns_per_record": m, "ci_lo": lo, "ci_hi": hi, "mrecords_per_s": 1e3 / m if m else None,
               "gb_per_s": rb / m if m else None, "record_bytes": rb,
               "setup_ms": bl.median([r["setup_ns"] / 1e6 for r in rs if r.get("setup_ns")]) if any(
                   r.get("setup_ns") for r in rs) else None,
               "resident_before": bl.median([r["resident_before"] for r in rs if r.get("resident_before") is not None])
               if any(r.get("resident_before") is not None for r in rs) else None,
               "checksum": rs[0]["checksum"]}
        rows.append(row)
        checks[(host, tid.rsplit("-", 1)[0], fields if fields != "all" else "sim")].add(rs[0]["checksum"])
    agree = {f"{h}|{t}|{f}": len(v) == 1 for (h, t, f), v in checks.items()}
    return rows, agree


# ------------------------------------------------------------------ break-even

def breakeven(micro, phases, summary):
    """N* = T_jit_cold / (t_interp - t_spec), per host and access order (TASK.md section 2)."""
    out = []
    by_host = defaultdict(list)
    for r in micro:
        if r["experiment"].startswith("decode_micro") and not r["jit_cold"] and r["fields"] == "sim"                 and r["cache"] == "warm" and r.get("core_class", "fast") == "fast":
            by_host[r["host"]].append(r)
    jit_cost = defaultdict(list)
    for r in phases:
        if r["config"] == "NEW-spec-cold" and r.get("jit_compile_ms"):
            jit_cost[r["host"]].append(sum(r.get(p + "_ms", 0) or 0 for p in
                                           ("jit_generate", "jit_compile", "jit_load", "decoder_lookup")))
    for host, rs in by_host.items():
        T = bl.median(jit_cost[host]) if jit_cost[host] else None
        largest_trace = max((r for r in rs if r["variant"] == "jit"), key=lambda r: r["records"], default=None)
        if largest_trace is None:
            continue
        largest, ltid = largest_trace["records"], largest_trace["trace"]
        for order in sorted({r["order"] for r in rs}):
            sel = {r["variant"]: r for r in rs if r["order"] == order and r["trace"] == ltid}
            if "jit" not in sel or "plan" not in sel:
                continue
            t_spec, t_interp = sel["jit"]["ns_per_record"], sel["plan"]["ns_per_record"]
            row = {"host": host, "order": order, "trace": sel["jit"]["trace"], "records": largest,
                   "T_jit_cold_ms": T, "t_interp_ns": t_interp, "t_spec_ns": t_spec,
                   "t_legacy_ns": sel.get("legacy", {}).get("ns_per_record"),
                   "t_upstream_ns": sel.get("upstream", {}).get("ns_per_record"),
                   "t_tnode_ns": sel.get("templated_node", {}).get("ns_per_record")}
            for ref, t_ref in (("interp", t_interp), ("legacy", row["t_legacy_ns"]), ("upstream", row["t_upstream_ns"])):
                if T is None or t_ref is None:
                    row[f"N_star_vs_{ref}"] = None
                elif t_ref <= t_spec:
                    row[f"N_star_vs_{ref}"] = "never (spec not faster per record)"
                else:
                    row[f"N_star_vs_{ref}"] = T * 1e6 / (t_ref - t_spec)
            ns = row["N_star_vs_interp"]
            if isinstance(ns, float):
                row["N_star_bytes_legacy_equiv"] = ns * 39
                row["N_star_bytes_sd"] = ns * sel["jit"]["record_bytes"]
            out.append(row)
    # End-to-end check: where does NEW-spec-cold overtake NEW-interp?
    for host in sorted({r["host"] for r in summary}):
        pts = defaultdict(dict)
        for r in summary:
            if r["host"] == host and r["cache"] == "warm" and r["schema"] == "inferred" and r["workload"] == "stencil3d" \
                    and r["experiment"] in ("size_sweep", "breakeven") and r["config"] in ("NEW-interp", "NEW-spec-cold"):
                pts[r["records"]][r["config"]] = r["median_ms"]
        series = sorted((n, v["NEW-interp"] / v["NEW-spec-cold"]) for n, v in pts.items() if len(v) == 2)
        crossing = None
        for (n0, r0), (n1, r1) in zip(series, series[1:]):
            if (r0 - 1) * (r1 - 1) <= 0 and r0 != r1:
                f = (1 - r0) / (r1 - r0)
                crossing = 10 ** (math.log10(n0) + f * (math.log10(n1) - math.log10(n0)))
                break
        out.append({"host": host, "order": "end-to-end", "trace": "stencil3d-p64 size sweep",
                    "N_star_vs_interp": crossing if crossing else ("not crossed in measured range" if series else None),
                    "e2e_ratio_interp_over_specCold": "; ".join(f"{n:.0e}:{x:.3f}" for n, x in series)})
    return out


# ------------------------------------------------------------------ other tables

def file_size_rows(meta):
    rows, seen = [], set()
    for camp, m in meta.items():
        for tid, t in (m.get("traces") or {}).items():
            if not t or t.get("status") != "ok" or tid in seen:
                continue
            seen.add(tid)
            g, f = t.get("generation", {}), t["files"]
            n = g.get("records") or 0
            rows.append({"trace": tid, "workload": t["spec"]["workload"], "ranks": t["spec"]["ranks"],
                         "schema": t["spec"]["schema"], "records": n, "edges": g.get("edges"),
                         "legacy_bytes": f["legacy"]["bytes"], "sd_bytes": f["sd"]["bytes"],
                         "sd_over_legacy": f["sd"]["bytes"] / f["legacy"]["bytes"],
                         "legacy_bytes_per_record": f["legacy"]["bytes"] / n if n else None,
                         "sd_bytes_per_record": f["sd"]["bytes"] / n if n else None,
                         "sd_record_bits": t.get("record_bits"), "goal_bytes": t.get("goal_bytes"),
                         "header_sha256": (t.get("header_sha256") or "")[:16]})
    return sorted(rows, key=lambda r: (r["schema"], r["workload"], r["records"]))


def convert_rows(runs):
    rows = []
    for k, rs in sorted(group(measured(runs, "convert"), ("host", "trace", "tool")).items()):
        wall = [r["wall_ns"] / 1e6 for r in rs]
        m, lo, hi, _ = ci(wall)
        rows.append({"host": k[0], "trace": k[1], "tool": k[2], "build": rs[0]["build"], "format": rs[0]["format"],
                     "threads": rs[0]["threads"], "n": len(wall), "median_ms": m, "ci_lo_ms": lo, "ci_hi_ms": hi,
                     "peak_rss_mb": bl.median([r["maxrss_kb"] / 1024 for r in rs]),
                     "input_mb_per_s": rs[0]["input_bytes"] / 1e6 / (m / 1e3) if m else None,
                     "output_bytes": rs[0].get("output_bytes")})
    base = {(r["host"], r["trace"]): r["median_ms"] for r in rows if r["tool"] == "base-legacy"}
    for r in rows:
        b = base.get((r["host"], r["trace"]))
        r["speedup_vs_base_txt2bin"] = b / r["median_ms"] if b and r["median_ms"] else None
    return rows


def concurrency_rows(runs):
    rows = []
    for k, rs in sorted(group(measured(runs, "concurrent"), ("host", "trace", "config", "k")).items()):
        ms = [r["makespan_ns"] / 1e6 for r in rs]
        m, lo, hi, _ = ci(ms)
        rows.append({"host": k[0], "trace": k[1], "config": k[2], "k": k[3], "n": len(ms), "makespan_ms": m,
                     "ci_lo_ms": lo, "ci_hi_ms": hi, "simulations_per_s": k[3] / (m / 1e3) if m else None,
                     "sum_peak_anon_mb": bl.median([r["sum_peak_rss_anon_kb"] / 1024 for r in rs]),
                     "sum_maxrss_mb": bl.median([r["sum_maxrss_kb"] / 1024 for r in rs])})
    single = {(r["host"], r["trace"], r["config"]): r["makespan_ms"] for r in rows if r["k"] == 1}
    for r in rows:
        s = single.get((r["host"], r["trace"], r["config"]))
        r["parallel_efficiency"] = s / r["makespan_ms"] if s and r["makespan_ms"] else None
    return rows


def memcap_rows(runs):
    rows = []
    rs_all = [r for r in runs if r.get("experiment") and r.get("limit_gb") is not None and not r.get("warmup")]
    for k, rs in sorted(group(rs_all, ("host", "trace", "config", "limit_gb")).items()):
        ok = [r for r in rs if r["status"] == "ok"]
        rows.append({"host": k[0], "trace": k[1], "config": k[2], "limit_gb": k[3], "runs": len(rs),
                     "completed": len(ok), "oom_killed": sum(1 for r in rs if r.get("oom_killed")),
                     "median_ms": bl.median([r["wall_ns"] / 1e6 for r in ok]) if ok else None,
                     "majflt": bl.median([r.get("majflt") or 0 for r in ok]) if ok else None})
    return rows


def overhead_rows(summary):
    rows = []
    idx = {(r["host"], r["trace"], r["config"]): r for r in summary if r["experiment"] == "instrumentation_overhead"}
    for (host, tid, cfg), r in sorted(idx.items()):
        if cfg != "NEW-interp":
            continue
        for other in ("NEW-interp-notiming", "PLAIN-interp"):
            o = idx.get((host, tid, other))
            if o:
                rows.append({"host": host, "trace": tid, "timing_on": "NEW-interp", "compared_with": other,
                             "on_median_ms": r["median_ms"], "on_ci": f"[{fmt(r['ci_lo_ms'])}, {fmt(r['ci_hi_ms'])}]",
                             "other_median_ms": o["median_ms"], "other_ci": f"[{fmt(o['ci_lo_ms'])}, {fmt(o['ci_hi_ms'])}]",
                             "ratio_on_over_other": r["median_ms"] / o["median_ms"],
                             "cis_overlap": not (r["ci_lo_ms"] > o["ci_hi_ms"] or o["ci_lo_ms"] > r["ci_hi_ms"])})
    return rows


def environment_rows(runs, meta):
    rows = []
    for camp, m in meta.items():
        mach = m.get("machine", {})
        rs = [r for r in runs if r.get("campaign") == camp]
        cold = [r for r in rs if r.get("cache") == "cold" and r.get("resident_before")]
        res = [x for r in cold for x in (r["resident_before"] if isinstance(r["resident_before"], list) else [r["resident_before"]])
               if x is not None]
        govs = {r.get("governor") for r in rs} - {None}
        rows.append({"campaign": camp, "host": mach.get("hostname"), "kernel": mach.get("kernel"),
                     "cpu": next((l.split(":", 1)[1].strip() for l in (mach.get("lscpu") or "").splitlines()
                                  if l.startswith("Model name")), None),
                     "mem_gb": round(mach.get("mem_total_kb", 0) / 2 ** 20, 1), "wsl": mach.get("is_wsl"),
                     "root": mach.get("is_root"), "governors": ",".join(sorted(govs)) or "n/a (no cpufreq)",
                     "no_turbo": mach.get("intel_no_turbo"), "thp": mach.get("thp_enabled"),
                     "runs": len(rs), "noisy_runs": sum(1 for r in rs if r.get("noisy")),
                     "cold_eviction": ",".join(sorted({r.get("cache_method") for r in cold} - {None})),
                     "cold_resident_max": max(res) if res else None,
                     "compiler": next((b.get("compiler", {}).get("cxx_version") for b in (m.get("builds") or {}).values()
                                       if isinstance(b, dict) and b.get("compiler")), None),
                     "flags_identical": (m.get("flags_check") or {}).get("identical_flags")})
    return rows


# ------------------------------------------------------------------ plots

def plots(out, summary, phases, micro, be, sizes, conv, conc, catalog):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed: tables only (pip install matplotlib)")
        return
    out.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.size": 9, "axes.spines.top": False, "axes.spines.right": False,
                         "axes.edgecolor": "#52514e", "axes.labelcolor": INK, "xtick.color": "#52514e",
                         "ytick.color": "#52514e", "axes.grid": True, "grid.color": "#e4e3df", "grid.linewidth": 0.6,
                         "legend.frameon": False, "lines.linewidth": 1.5, "lines.markersize": 5,
                         "figure.facecolor": "#fcfcfb", "axes.facecolor": "#fcfcfb", "savefig.facecolor": "#fcfcfb"})

    def save(fig, name):
        fig.tight_layout()
        for ext in ("pdf", "png"):
            fig.savefig(out / f"{name}.{ext}", dpi=200)
        plt.close(fig)

    hosts = sorted({r["host"] for r in summary} | {r["host"] for r in micro})

    def line_plot(name, exp_filter, x_key, xlabel, ylabel, value=lambda r: r["median_ms"],
                  band=lambda r: (r["ci_lo_ms"], r["ci_hi_ms"]), logy=True):
        sel = [r for r in summary if exp_filter(r)]
        if not sel:
            return
        hs = sorted({r["host"] for r in sel})
        fig, axes = plt.subplots(1, len(hs), figsize=(5.6 * len(hs), 3.8), squeeze=False)
        for ax, h in zip(axes[0], hs):
            for cfg in sorted({r["config"] for r in sel if r["host"] == h}, key=lambda c: (c != "BASE", c)):
                pts = sorted((r for r in sel if r["host"] == h and r["config"] == cfg), key=lambda r: r[x_key])
                xs = [r[x_key] for r in pts]
                st = style(cfg, catalog)
                ax.plot(xs, [value(r) for r in pts], label=cfg, **st)
                if band:
                    lo, hi = zip(*(band(r) for r in pts))
                    ax.fill_between(xs, lo, hi, color=st["color"], alpha=0.15, linewidth=0)
            ax.set_xscale("log")
            if logy:
                ax.set_yscale("log")
            ax.set_xlabel(xlabel)
            ax.set_ylabel(ylabel)
            ax.set_title(h, loc="left", fontsize=9)
        axes[0][-1].legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.01, 1))
        save(fig, name)

    line_plot("e2e_vs_size", lambda r: r["experiment"] in ("size_sweep", "breakeven") and r["cache"] == "warm",
              "records", "trace size (records)", "end-to-end wall time (ms), median and 95% CI")
    line_plot("rss_vs_size", lambda r: r["experiment"] in ("size_sweep",) and r["cache"] == "warm", "records",
              "trace size (records)", "peak RSS (MB, median)", value=lambda r: r["peak_rss_mb"], band=None)
    line_plot("anon_vs_size", lambda r: r["experiment"] in ("size_sweep",) and r["cache"] == "warm", "records",
              "trace size (records)", "peak anonymous RSS (MB): not reclaimable", value=lambda r: max(r["peak_anon_mb"], 1e-3),
              band=None)
    line_plot("rank_sweep", lambda r: r["experiment"] == "rank_sweep", "ranks", "ranks (fixed total records)",
              "end-to-end wall time (ms)")

    # Phase breakdown at the largest size <= 1e7 of the size sweep.
    groups_ph = [("open + validate", ("header_parse", "rank_table", "legacy_open", "validate", "validate_deps", "plan_build")),
                 ("JIT (lookup, generate, compile, load)", ("decoder_lookup", "jit_generate", "jit_compile", "jit_load")),
                 ("root nodes", ("load_roots",)), ("simulate (incl. decoding)", ("simulate",))]
    sel = [r for r in phases if r["experiment"] == "size_sweep" and r["cache"] == "warm"]
    if sel:
        target = max((s for s in {r["trace"] for r in sel} if "n1e8" not in s and "n1e9" not in s), default=None,
                     key=lambda t: float(t.split("-n")[1].split("-")[0]))
        rows = [r for r in sel if r["trace"] == target]
        base_rows = [r for r in summary if r["trace"] == target and r["config"] == "BASE" and r["experiment"] == "size_sweep"]
        fig, ax = plt.subplots(figsize=(7.5, 0.45 * (len(rows) + len(base_rows)) + 1.2))
        labels = []
        for i, r in enumerate(sorted(rows, key=lambda r: r["config"])):
            left = 0.0
            known = 0.0
            for j, (label, keys) in enumerate(groups_ph):
                v = sum(r.get(k + "_ms", 0) or 0 for k in keys)
                ax.barh(i, v, left=left, color=SLOT[j], edgecolor="#fcfcfb", linewidth=1, label=label if i == 0 else None)
                left += v
                known += v
            rest = max(0.0, (r.get("total_ms") or 0) - known)
            ax.barh(i, rest, left=left, color=SLOT[4], edgecolor="#fcfcfb", linewidth=1, label="other in-process" if i == 0 else None)
            left += rest
            ax.barh(i, max(0.0, r["process_overhead_ms"]), left=left, color=SLOT[5], edgecolor="#fcfcfb", linewidth=1,
                    label="exec + exit (wall - total)" if i == 0 else None)
            labels.append(r["config"])
        for b in base_rows:
            ax.barh(len(labels), b["median_ms"], color="#c3c2b7", edgecolor="#fcfcfb", label="BASE (wall only, not instrumented)")
            labels.append("BASE")
        ax.set_yticks(range(len(labels)), labels)
        ax.set_xlabel(f"time (ms, medians) - {target}")
        ax.legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.01, 1))
        ax.grid(axis="y", visible=False)
        save(fig, "phases")

    # Decode throughput.
    sel = [r for r in micro if r["experiment"].startswith("decode_micro") and r["fields"] == "sim" and not r["jit_cold"]
           and r.get("core_class", "fast") == "fast"]
    if sel:
        for h in sorted({r["host"] for r in sel}):
            hs = [r for r in sel if r["host"] == h]
            largest = max(r["records"] for r in hs)
            ltid = max((r for r in hs if r["variant"] == "plan"), key=lambda r: r["records"], default=hs[0])["trace"]
            hs = [r for r in hs if r["trace"] == ltid]
            orders = sorted({r["order"] for r in hs})
            variants = [v for v in ("upstream", "legacy", "plan", "jit", "orc", "templated_node", "templated_schema")
                        if any(r["variant"] == v for r in hs)]
            fig, axes = plt.subplots(1, len(orders), figsize=(3.6 * len(orders), 3.4), squeeze=False, sharey=True)
            for ax, o in zip(axes[0], orders):
                for ci_, cache in enumerate(("warm", "cold")):
                    xs, ys, err = [], [], []
                    for vi, v in enumerate(variants):
                        r = next((r for r in hs if r["variant"] == v and r["order"] == o and r["cache"] == cache), None)
                        if r:
                            xs.append(vi + (ci_ - 0.5) * 0.38)
                            ys.append(r["mrecords_per_s"])
                            err.append((r["mrecords_per_s"] - 1e3 / r["ci_hi"], 1e3 / r["ci_lo"] - r["mrecords_per_s"]))
                    if xs:
                        ax.bar(xs, ys, width=0.36, color=SLOT[ci_], edgecolor="#fcfcfb", label=f"{cache} page cache",
                               yerr=list(zip(*err)), error_kw={"elinewidth": 0.8, "ecolor": INK, "capsize": 2})
                ax.set_xticks(range(len(variants)), variants, rotation=35, ha="right")
                ax.set_title(f"{o} order", loc="left", fontsize=9)
                ax.grid(axis="x", visible=False)
            axes[0][0].set_ylabel(f"decode throughput (M records/s)\n{largest:.0e} records, {h}")
            axes[0][-1].legend(fontsize=7)
            save(fig, f"decode_throughput_{h.split('.')[0]}")

    # Break-even model.
    rows = [r for r in be if r["order"] != "end-to-end" and isinstance(r.get("T_jit_cold_ms"), float)]
    if rows:
        fig, axes = plt.subplots(1, len(rows), figsize=(4.6 * len(rows), 3.6), squeeze=False)
        for ax, r in zip(axes[0], rows):
            n = [10 ** (k / 10) for k in range(30, 95)]
            T = r["T_jit_cold_ms"]
            models = [("NEW-interp (plan)", 0.0, r["t_interp_ns"], "NEW-interp"),
                      ("NEW-spec-cold (jit, empty cache)", T, r["t_spec_ns"], "NEW-spec-cold"),
                      ("NEW-spec-warm (jit, cached)", 0.0, r["t_spec_ns"], "NEW-spec-warm")]
            if r.get("t_legacy_ns"):
                models.append(("legacy reader (NEW-legacyfile)", 0.0, r["t_legacy_ns"], "NEW-legacyfile"))
            if r.get("t_upstream_ns"):
                models.append(("upstream reader (BASE)", 0.0, r["t_upstream_ns"], "BASE"))
            for label, setup, t, cfg in models:
                ax.plot(n, [setup + x * t / 1e6 for x in n], label=label, **{**style(cfg, catalog), "marker": None})
            ns = r.get("N_star_vs_interp")
            if isinstance(ns, float):
                ax.axvline(ns, color="#52514e", linewidth=0.8, linestyle=":")
                ax.annotate(f"N* = {ns:.2g}", (ns, T), textcoords="offset points", xytext=(4, 6), fontsize=8)
            ax.set_xscale("log")
            ax.set_yscale("log")
            ax.set_xlabel("records decoded")
            ax.set_ylabel("cumulative decode time (ms)")
            ax.set_title(f"{r['host']}, {r['order']} order", loc="left", fontsize=9)
        axes[0][-1].legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.01, 1))
        save(fig, "breakeven")

    # File size per schema relative to legacy.
    if sizes:
        sel = [r for r in sizes if r["records"] and r["workload"] == "stencil3d"]
        sel = sel or sizes
        labels = [f"{r['schema']}\n{r['records']:.0e}" for r in sel]
        fig, ax = plt.subplots(figsize=(max(4, 0.55 * len(sel) + 1.5), 3.2))
        ax.bar(range(len(sel)), [r["sd_over_legacy"] for r in sel], color=SLOT[0], edgecolor="#fcfcfb")
        ax.axhline(1, color=INK, linewidth=0.8)
        ax.set_xticks(range(len(sel)), labels, fontsize=7)
        ax.set_ylabel("self-describing / legacy file size")
        ax.grid(axis="x", visible=False)
        save(fig, "file_size")

    if conv:
        fig, ax = plt.subplots(figsize=(5.5, 3.6))
        for i, (fmt_, color) in enumerate((("legacy", SLOT[0]), ("self-describing", SLOT[1]))):
            pts = sorted((r for r in conv if r["build"] in ("omp", "lazy") and r["format"] == fmt_), key=lambda r: r["threads"])
            for tr in sorted({r["trace"] for r in pts}):
                p = [r for r in pts if r["trace"] == tr]
                ax.errorbar([r["threads"] for r in p], [r["median_ms"] for r in p],
                            yerr=[[r["median_ms"] - r["ci_lo_ms"] for r in p], [r["ci_hi_ms"] - r["median_ms"] for r in p]],
                            color=color, marker="o", label=f"omp {fmt_} ({tr})", capsize=2)
        for r in conv:
            if r["tool"] in ("base-legacy", "master-legacy", "master-sd"):
                ax.axhline(r["median_ms"], color={"base-legacy": INK, "master-legacy": SLOT[0], "master-sd": SLOT[1]}[r["tool"]],
                           linestyle=":", linewidth=1)
                ax.annotate(r["tool"], (1, r["median_ms"]), fontsize=7, textcoords="offset points", xytext=(2, 2))
        ax.set_xscale("log", base=2)
        ax.set_xlabel("txt2bin --threads")
        ax.set_ylabel("conversion wall time (ms)")
        ax.legend(fontsize=7)
        save(fig, "convert_scaling")
    if conc:
        fig, axes = plt.subplots(1, 2, figsize=(9, 3.4))
        for cfg in sorted({r["config"] for r in conc}):
            p = sorted((r for r in conc if r["config"] == cfg), key=lambda r: r["k"])
            st = style(cfg, catalog)
            axes[0].plot([r["k"] for r in p], [r["simulations_per_s"] for r in p], label=cfg, **st)
            axes[1].plot([r["k"] for r in p], [r["sum_peak_anon_mb"] for r in p], label=cfg, **st)
        axes[0].set_ylabel("simulations per second")
        axes[1].set_ylabel("sum of peak anonymous RSS (MB)")
        for ax in axes:
            ax.set_xlabel("concurrent simulations k")
        axes[1].legend(fontsize=7)
        save(fig, "concurrency")
    # Workloads: speedup vs BASE.
    sel = [r for r in summary if r["experiment"] == "workloads" and r.get("speedup_vs_base")]
    if sel:
        wls = sorted({r["workload"] for r in sel})
        cfgs = sorted({r["config"] for r in sel})
        fig, ax = plt.subplots(figsize=(1.2 * len(wls) + 2, 3.4))
        w = 0.8 / len(cfgs)
        for i, cfg in enumerate(cfgs):
            pts = [next((r for r in sel if r["workload"] == wl and r["config"] == cfg), None) for wl in wls]
            xs = [j + (i - len(cfgs) / 2 + 0.5) * w for j, p in enumerate(pts) if p]
            ys = [p["speedup_vs_base"] for p in pts if p]
            err = [[p["speedup_vs_base"] - p["speedup_ci_lo"] for p in pts if p], [p["speedup_ci_hi"] - p["speedup_vs_base"] for p in pts if p]]
            ax.bar(xs, ys, width=w * 0.95, color=style(cfg, catalog)["color"], edgecolor="#fcfcfb", label=cfg, yerr=err,
                   error_kw={"elinewidth": 0.8, "ecolor": INK, "capsize": 2})
        ax.axhline(1, color=INK, linewidth=0.8)
        ax.set_xticks(range(len(wls)), wls, rotation=25, ha="right")
        ax.set_ylabel("speedup vs BASE (median ratio, 95% CI)")
        ax.legend(fontsize=7, loc="upper left", bbox_to_anchor=(1.01, 1))
        ax.grid(axis="x", visible=False)
        save(fig, "workloads")


def distributions(out, runs, catalog):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    sel = [r for r in measured(runs, "e2e") if r["experiment"] == "size_sweep" and r.get("cache") == "warm"]
    if not sel:
        return
    for host in sorted({r["host"] for r in sel}):
        hs = [r for r in sel if r["host"] == host]
        sizes = sorted({r["records"] for r in hs})
        fig, axes = plt.subplots(1, len(sizes), figsize=(2.6 * len(sizes) + 1, 3.6), squeeze=False)
        for ax, n in zip(axes[0], sizes):
            cfgs = sorted({r["config"] for r in hs if r["records"] == n}, key=lambda c: (c != "BASE", c))
            data = [[r["wall_ns"] / 1e6 for r in hs if r["records"] == n and r["config"] == c] for c in cfgs]
            bp = ax.boxplot(data, vert=True, patch_artist=True, widths=0.6, showfliers=True,
                            medianprops={"color": INK, "linewidth": 1.2},
                            flierprops={"markersize": 3, "markeredgecolor": "#52514e"})
            for patch, c in zip(bp["boxes"], cfgs):
                patch.set_facecolor(style(c, catalog)["color"])
                patch.set_alpha(0.55)
                patch.set_edgecolor(INK)
            ax.set_xticks(range(1, len(cfgs) + 1), cfgs, rotation=60, ha="right", fontsize=6)
            ax.set_title(f"{n:.0e} records", loc="left", fontsize=9)
            ax.grid(axis="x", visible=False)
        axes[0][0].set_ylabel(f"wall time (ms), all repetitions\n{host}")
        fig.tight_layout()
        for ext in ("pdf", "png"):
            fig.savefig(out / f"distributions_{host.split('.')[0]}.{ext}", dpi=200)
        plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("campaigns", nargs="*")
    ap.add_argument("--out-name")
    ap.add_argument("--exclude-noisy", action="store_true")
    ap.add_argument("--no-plots", action="store_true")
    a = ap.parse_args()
    campaigns = [Path(c) for c in a.campaigns]
    if not campaigns:
        raw = sorted((RESULTS / "raw").glob("*/runs.jsonl"), key=lambda p: p.stat().st_mtime)
        if not raw:
            sys.exit("no campaigns in bench/results/raw")
        campaigns = [raw[-1].parent]
    name = a.out_name or "+".join(c.name for c in campaigns)
    runs, meta, correctness = load(campaigns, a.exclude_noisy)
    catalog = {}
    for m in meta.values():
        catalog.update((m.get("config") or {}).get("configs", {}))
    tables, plot_dir = RESULTS / "tables" / name, RESULTS / "plots" / name

    summary = e2e_summary(runs, catalog, correctness)
    write_table(tables, "summary", summary,
                ["host", "experiment", "workload", "records", "ranks", "schema", "cache", "config", "n", "median_ms",
                 "ci_lo_ms", "ci_hi_ms", "ci_halfwidth_pct", "base_median_ms", "speedup_vs_base", "speedup_ci_lo",
                 "speedup_ci_hi", "events_per_s", "loop_events_per_s", "peak_rss_mb", "peak_anon_mb", "majflt", "noisy_runs", "correctness",
                 "matches_reference"],
                "End-to-end summary (TASK.md 7, table 7)",
                "Wall time of the whole LogGOPSim process. speedup_vs_base = median(BASE) / median(config) on the same "
                "trace (BASE reads the equivalent legacy file), CI by percentile bootstrap. correctness = gate result "
                "for this (trace, config).")
    ph = phase_rows(runs)
    cols = ["host", "experiment", "trace", "cache", "config", "n", "wall_ms", "total_ms", "process_overhead_ms", "load_ms",
            "header_parse_ms", "rank_table_ms", "legacy_open_ms", "validate_ms", "validate_deps_ms", "plan_build_ms",
            "decoder_select_ms", "decoder_lookup_ms", "jit_generate_ms", "jit_compile_ms", "jit_compile_ci", "jit_load_ms",
            "load_roots_ms", "simulate_ms", "simulate_ci", "jit_cache"]
    write_table(tables, "phases", ph, cols, "Phase times (GOAL_PHASE_TIMING, medians)",
                "BASE has no instrumentation: only wall time, ev/s and RSS (see summary). JIT cost is never averaged "
                "into other phases.")
    mic, agree = micro_rows(runs)
    write_table(tables, "decode", mic, ["host", "experiment", "trace", "records", "variant", "runner", "order", "fields",
                                         "cache", "jit_cold", "core_class", "n", "processes", "ns_per_record", "ci_lo",
                                         "ci_hi", "mrecords_per_s", "gb_per_s", "record_bytes", "setup_ms",
                                         "resident_before", "checksum"],
                "Decode microbenchmark (bench/micro/decode_bench.cpp)",
                "ns_per_record: median over repetitions and processes; GB/s counts node-record bytes touched. "
                "Checksums agree across variants per trace: " + json.dumps(agree))
    be = breakeven(mic, ph, summary)
    write_table(tables, "breakeven", be, ["host", "order", "trace", "records", "T_jit_cold_ms", "t_interp_ns", "t_spec_ns",
                                          "t_legacy_ns", "t_upstream_ns", "t_tnode_ns", "N_star_vs_interp",
                                          "N_star_vs_legacy", "N_star_vs_upstream", "N_star_bytes_legacy_equiv",
                                          "N_star_bytes_sd", "e2e_ratio_interp_over_specCold"],
                "Break-even trace size N* = T_jit_cold / (t_ref - t_spec)",
                "T_jit_cold = median jit_generate+jit_compile+jit_load+decoder_lookup of NEW-spec-cold runs; t_* = "
                "median decode ns/record (warm cache, simulator fields) on the largest micro trace. 'end-to-end' rows: "
                "median(NEW-interp)/median(NEW-spec-cold) per size, crossing 1 at the measured N*.")
    sizes = file_size_rows(meta)
    write_table(tables, "file_sizes", sizes, ["trace", "workload", "ranks", "schema", "records", "edges", "legacy_bytes",
                                              "sd_bytes", "sd_over_legacy", "legacy_bytes_per_record",
                                              "sd_bytes_per_record", "sd_record_bits", "goal_bytes", "header_sha256"],
                "Trace file size per schema")
    mem = [r for r in summary if r["experiment"] in ("size_sweep", "save_mem", "xl")]
    write_table(tables, "memory", mem, ["host", "experiment", "records", "config", "peak_rss_mb", "peak_anon_mb", "majflt",
                                        "median_ms"], "Peak memory (ru_maxrss of the simulator; peak RssAnon sampled from /proc)")
    write_table(tables, "memory_budget", memcap_rows(runs), ["host", "trace", "config", "limit_gb", "runs", "completed",
                                                             "oom_killed", "median_ms", "majflt"],
                "Runs under a cgroup memory limit (swap off)")
    conv = convert_rows(runs)
    write_table(tables, "convert", conv, ["host", "trace", "tool", "build", "format", "threads", "n", "median_ms", "ci_lo_ms",
                                          "ci_hi_ms", "speedup_vs_base_txt2bin", "input_mb_per_s", "peak_rss_mb",
                                          "output_bytes"], "txt2bin conversion")
    conc = concurrency_rows(runs)
    write_table(tables, "concurrency", conc, ["host", "trace", "config", "k", "n", "makespan_ms", "ci_lo_ms", "ci_hi_ms",
                                              "simulations_per_s", "parallel_efficiency", "sum_peak_anon_mb",
                                              "sum_maxrss_mb"], "Concurrent simulations of one trace")
    write_table(tables, "overhead", overhead_rows(summary), ["host", "trace", "timing_on", "compared_with", "on_median_ms",
                                                             "on_ci", "other_median_ms", "other_ci",
                                                             "ratio_on_over_other", "cis_overlap"],
                "Phase-timing instrumentation overhead (TASK.md 4)")
    write_table(tables, "environment", environment_rows(runs, meta),
                ["campaign", "host", "cpu", "mem_gb", "kernel", "wsl", "root", "governors", "no_turbo", "thp", "compiler",
                 "flags_identical", "runs", "noisy_runs", "cold_eviction", "cold_resident_max"], "Measurement environment")
    for camp, rep in correctness.items():
        src = Path([c for c in campaigns if c.name == camp][0]) / "correctness" / "correctness.md"
        if src.exists():
            (tables / f"correctness_{camp}.md").write_text(src.read_text())
    if not a.no_plots:
        plots(plot_dir, summary, ph, mic, be, sizes, conv, conc, catalog)
        distributions(plot_dir, runs, catalog)
    print(f"tables: {tables}\nplots:  {plot_dir}")


if __name__ == "__main__":
    main()
