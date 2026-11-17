"""Expands a campaign config into traces and measurement cells (shared by gen_traces, check_correctness,
run and analyze, so that all of them agree on what a config means)."""
from __future__ import annotations

import itertools

from benchlib import trace_id

TRACE_KEYS = ("workload", "ranks", "records", "schema")


def expand_traces(spec, defaults: dict) -> list[dict]:
    """A dict is a cross product of its list values (missing keys from trace_defaults); a list is a
    sequence of such dicts. Returns trace specs with their id."""
    items = spec if isinstance(spec, list) else [spec or {}]
    out = []
    for item in items:
        merged = {**defaults, **item}
        values = [merged[k] if isinstance(merged[k], list) else [merged[k]] for k in TRACE_KEYS]
        for combo in itertools.product(*values):
            t = dict(zip(TRACE_KEYS, combo))
            t["ranks"] = int(t["ranks"])
            t["records"] = int(float(t["records"]))
            t["id"] = trace_id(t["workload"], t["ranks"], t["records"], t["schema"])
            out.append(t)
    return out


def experiments(cfg: dict, only=None) -> list[dict]:
    exps = cfg.get("experiments", [])
    if only:
        names = set(only)
        unknown = names - {e["name"] for e in exps}
        if unknown:
            raise SystemExit(f"unknown experiments: {sorted(unknown)}")
        exps = [e for e in exps if e["name"] in names]
    return exps


def experiment_traces(cfg: dict, exp: dict) -> list[dict]:
    return expand_traces(exp.get("traces"), cfg.get("trace_defaults", {}))


def simulator_configs(exp: dict) -> list[str]:
    """Runtime configs whose LogGOPSim runs need a correctness check."""
    return list(exp.get("configs", [])) if exp["kind"] in ("e2e", "concurrent", "memcap") else []


def all_traces(cfg: dict, only=None) -> dict:
    """Every trace any selected experiment needs, with what it is needed for."""
    traces = {}
    for exp in experiments(cfg, only):
        for t in experiment_traces(cfg, exp):
            entry = traces.setdefault(t["id"], {**t, "experiments": [], "needs_goal": False, "configs": set()})
            if exp["name"] not in entry["experiments"]:
                entry["experiments"].append(exp["name"])
            if exp["kind"] == "convert":
                entry["needs_goal"] = True
            entry["configs"].update(simulator_configs(exp))
    for entry in traces.values():
        entry["configs"] = sorted(entry["configs"])
    return traces


def builds_needed(cfg: dict, only=None) -> list[str]:
    names = set()
    catalog = cfg.get("configs", {})
    tools = cfg.get("convert_tools", {})
    for exp in experiments(cfg, only):
        for c in simulator_configs(exp):
            names.add(catalog[c]["build"])
        if exp["kind"] == "convert":
            names.update(tools[t]["build"] for t in exp["tools"])
        if exp["kind"] == "micro":
            names.add("master")
    names.update({"base", "master"})  # trace generation and the correctness reference
    return sorted(names)


def caps_for(cfg: dict, records: int) -> tuple[int, int]:
    reps = cfg.get("repetitions", {})
    lo, hi = reps.get("min", 10), reps.get("max", 50)
    for cap in sorted(reps.get("caps", []), key=lambda c: c["min_records"]):
        if records >= float(cap["min_records"]):
            lo, hi = cap.get("min", lo), cap.get("max", hi)
    return lo, hi
