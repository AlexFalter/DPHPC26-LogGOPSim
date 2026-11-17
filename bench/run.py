#!/usr/bin/env python3
"""Benchmark driver: runs a campaign's grid in randomized order and writes raw results.

usage: run.py --config bench/configs/full.json [--experiments a,b] [--budget-hours H]
              [--resume CAMPAIGN_DIR] [--skip-correctness | --correctness REPORT.json]
              [--cpus 2,3] [--traces-dir DIR] [--dry-run]

Each campaign gets a new directory bench/results/raw/<date>-<host>-<config>/ (raw data is never
overwritten) with
  metadata.json      machine (hostname, lscpu, RAM, kernel, governor/turbo, NUMA, cgroup, Slurm),
                     config, build manifests (commits, compiler versions, flags), trace manifest
  correctness/       the correctness gate's report (check_correctness.py), run first
  runs.jsonl         one JSON object per run (per repetition for micro), incl. all metadata
  status.json        per measurement cell: repetitions, median, CI, convergence
Rules (bench/README.md, "Methodology"): correctness gate before timing; >= 1 warm-up run per cell;
repetitions until the nonparametric 95% CI of the median is within +-ci_rel of the median (min..max
repetitions, lower caps for the largest traces); cells interleaved in a new random order every
round; runs pinned to one core (memory bound to its NUMA node); warm cache = trace resident before
the run, cold cache = evicted (drop_caches if root, else posix_fadvise) and verified with mincore;
load average logged and noisy runs flagged; --save-mem only on per-run copies.
"""
from __future__ import annotations

import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import benchlib as bl  # noqa: E402
import grid  # noqa: E402
import runner as rn  # noqa: E402

RESULTS = bl.results_dir() / "raw"


class Budget:
    def __init__(self, hours):
        self.deadline = time.monotonic() + hours * 3600 if hours else None

    def exceeded(self):
        return self.deadline is not None and time.monotonic() > self.deadline


class Campaign:
    def __init__(self, a, cfg):
        self.a, self.cfg = a, cfg
        self.only = a.experiments.split(",") if a.experiments else None
        self.dir = Path(a.resume) if a.resume else bl.new_campaign_dir(RESULTS, cfg["name"])
        scratch_root = Path(os.environ.get("BENCH_SCRATCH", bl.work_dir() / "scratch"))
        self.scratch = scratch_root / self.dir.name
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.ctx = rn.Context(cfg, self.scratch, a.traces_dir)
        self.env_cfg = cfg.get("environment", {})
        self.reps = cfg.get("repetitions", {})
        self.rng = random.Random(int(time.time()))
        self.budget = Budget(a.budget_hours)
        self.machine = bl.machine_info()
        self.numa = len(self.machine["numa_nodes"]) > 1
        self.pin = self.env_cfg.get("pin", True)
        self.fixed_cpus = [int(c) for c in a.cpus.split(",")] if a.cpus else None
        self.runs_path = self.dir / "runs.jsonl"
        self.samples, self.started_cells, self.status = {}, set(), {}
        self.micro_done = {}  # micro cells: process indices already run (for --resume)
        self.lock = threading.Lock()
        self.reference = {}
        self.correct = {}
        self._load_resume()

    # ------------------------------------------------------------ bookkeeping
    def _load_resume(self):
        if not self.runs_path.exists():
            return
        for line in self.runs_path.read_text().splitlines():
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            self.started_cells.add(r["key"])
            if r.get("kind") == "micro":
                self.micro_done.setdefault(r["key"], set()).add(r.get("process"))
            elif not r.get("warmup") and r.get("status") == "ok" and r.get("metric") is not None:
                self.samples.setdefault(r["key"], []).append(r["metric"])
        print(f"resuming {self.dir}: {sum(map(len, self.samples.values()))} measured runs found")

    def record(self, rec):
        rec.setdefault("campaign", self.dir.name)
        rec.setdefault("timestamp", time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
        rec.setdefault("host", self.machine["hostname"])
        with self.lock, open(self.runs_path, "a") as f:
            f.write(json.dumps(rec, default=str) + "\n")

    def write_metadata(self):
        if (self.dir / "metadata.json").exists():
            return
        builds = {}
        for name in grid.builds_needed(self.cfg, self.only):
            try:
                builds[name] = bl.load_build_manifest(name)
            except SystemExit as e:
                builds[name] = {"missing": str(e)}
        flags = bl.work_dir() / "build" / "flags-check.json"
        used = grid.all_traces(self.cfg, self.only)
        meta = {
            "argv": sys.argv, "config": self.cfg, "machine": self.machine, "builds": builds,
            "flags_check": json.loads(flags.read_text()) if flags.exists() else None,
            "traces": {tid: self.ctx.trace_manifest.get(tid) for tid in used},
            "traces_dir": str(self.ctx.traces_dir), "traces_fs": bl.fs_type(self.ctx.traces_dir),
            "scratch": str(self.scratch), "scratch_fs": bl.fs_type(self.scratch),
            "repo": {"head": subprocess.run(["git", "-C", str(bl.REPO), "rev-parse", "HEAD"], capture_output=True,
                                            text=True).stdout.strip(),
                     "status": subprocess.run(["git", "-C", str(bl.REPO), "status", "--porcelain"], capture_output=True,
                                              text=True).stdout.splitlines()},
            "pinning": {"enabled": self.pin, "fixed_cpus": self.fixed_cpus, "membind": self.numa},
            "params": self.cfg.get("params"),
        }
        bl.write_json(self.dir / "metadata.json", meta)

    # ------------------------------------------------------------ correctness gate
    def gate(self):
        if self.a.skip_correctness:
            print("WARNING: correctness gate skipped (--skip-correctness); results are not gated", file=sys.stderr)
            return
        path = Path(self.a.correctness) if self.a.correctness else self.dir / "correctness" / "correctness.json"
        if not path.exists():
            import check_correctness
            check_correctness.check(self.cfg, self.only, path.parent, self.a.traces_dir,
                                    max(1, (os.cpu_count() or 2) // 2))
        report = json.loads(path.read_text())
        for tid, r in report["traces"].items():
            for config, v in r.get("configs", {}).items():
                self.correct[(tid, config)] = v["status"]
                if config == "BASE":
                    self.reference[tid] = v.get("result_sha256")
        bad = [k for k, v in self.correct.items() if v not in ("pass",)]
        if bad:
            print(f"correctness: {len(bad)} (trace, config) pairs are excluded from timing (see {path})")

    def allowed(self, tid, config):
        if self.a.skip_correctness or self.a.dry_run:
            return True, "not_checked"
        st = self.correct.get((tid, config))
        return st == "pass", st or "not_checked"

    # ------------------------------------------------------------ environment per run
    def cpus(self, count=1, core_class="fast"):
        if not self.pin:
            return None
        if self.fixed_cpus:
            return self.fixed_cpus[:count] if len(self.fixed_cpus) >= count else None
        if core_class == "slow":
            allowed = sorted(os.sched_getaffinity(0))
            def speed(c):
                cap = bl._read(f"/sys/devices/system/cpu/cpu{c}/cpu_capacity")
                return int(cap) if cap and cap.isdigit() else 0
            slowest = min(speed(c) for c in allowed)
            chosen = [c for c in allowed if speed(c) == slowest and c != 0]
            return chosen[:count] if len(chosen) >= count else None
        chosen = bl.pick_cpus(count)
        return chosen if len(chosen) >= count else None

    def env_snapshot(self, cpus):
        load = os.getloadavg()
        threshold = self.env_cfg.get("noisy_loadavg_per_cpu", 0.25)
        snap = {"loadavg": load, "noisy": load[0] / (os.cpu_count() or 1) > threshold, "cpus": cpus}
        if cpus:
            info = bl.cpu_freq_info(cpus[0])
            snap["governor"] = info.get("scaling_governor")
            snap["cur_freq_khz"] = info.get("scaling_cur_freq")
            snap["membind"] = bl.numa_node_of(cpus[0]) if (self.numa and self.env_cfg.get("membind", True)) else None
        return snap

    def cache_state(self, paths, cache):
        method = "none"
        if cache == "cold":
            method = bl.drop_page_cache(paths, self.env_cfg.get("cache_method", "auto"))
        else:
            if any((bl.resident_fraction(p) or 0) < 0.99 for p in paths):
                bl.warm_page_cache(paths)
                method = "read"
        return {"cache": cache, "cache_method": method,
                "resident_before": [bl.resident_fraction(p) for p in paths]}

    # ------------------------------------------------------------ adaptive repetition loop
    def adaptive(self, exp, cells, run_once, metric_name="wall_ns", fixed_reps=None):
        """cells: dicts with 'key' and 'records'. run_once(cell, rep, warmup, round) -> metric or None."""
        warmup = self.reps.get("warmup", 1)
        rel, conf = self.reps.get("ci_rel", 0.05), self.reps.get("confidence", 0.95)
        failures = {}
        for w in range(warmup):
            for c in self.rng.sample(cells, len(cells)):
                if c["key"] in self.started_cells or self.budget.exceeded():
                    continue
                run_once(c, -1 - w, True, -1)
        def still_sampling(c):
            """False once the cell converged, hit its cap or failed twice (and records why)."""
            xs = self.samples.get(c["key"], [])
            lo, hi = (fixed_reps, fixed_reps) if fixed_reps else grid.caps_for(self.cfg, c["records"])
            if failures.get(c["key"], 0) >= 2:
                self.status[c["key"]] = {"state": "failed", "n": len(xs)}
                return False
            if len(xs) >= hi or (len(xs) >= lo and bl.ci_converged(xs, rel, conf)):
                m, l, h, exact = bl.median_ci(xs, conf)
                self.status[c["key"]] = {"state": "converged" if bl.ci_converged(xs, rel, conf) else "max_reps",
                                         "n": len(xs), "median": m, "ci": [l, h], "metric": metric_name}
                return False
            return True

        active = [c for c in cells if still_sampling(c)]  # resumed cells may already be done
        rnd = 0
        while active and not self.budget.exceeded():
            for c in self.rng.sample(active, len(active)):
                if self.budget.exceeded():
                    break
                v = run_once(c, len(self.samples.get(c["key"], [])), False, rnd)
                if v is None:
                    failures[c["key"]] = failures.get(c["key"], 0) + 1
                else:
                    self.samples.setdefault(c["key"], []).append(v)
            rnd += 1
            active = [c for c in active if still_sampling(c)]
            if active:
                print(f"  [{exp['name']}] round {rnd}: {len(active)} cells still sampling", flush=True)
        for c in active:
            xs = self.samples.get(c["key"], [])
            self.status[c["key"]] = {"state": "budget_exhausted", "n": len(xs)}
        bl.write_json(self.dir / "status.json", self.status)

    # ------------------------------------------------------------ e2e
    def simulate_once(self, exp, cell, rep, warmup, rnd, cpus=None, prefix=None, record=True):
        ctx = self.ctx
        run = ctx.sim_run(cell["config"], cell["trace"])
        cpus = cpus if cpus is not None else self.cpus(1)
        snap = self.env_snapshot(cpus)
        cache = self.cache_state([run["run_trace"]], cell.get("cache", "warm"))
        out = self.scratch / f"run-{os.getpid()}-{threading.get_ident()}.out"
        err = out.with_suffix(".err")
        cmd = (prefix or []) + run["cmd"]
        try:
            res = bl.run_measured(cmd, env=run["env"], cpus=cpus, membind=snap.get("membind"),
                                  timeout=self.env_cfg.get("timeout_s"), stdout_path=out, stderr_path=err)
            parsed = bl.parse_loggopsim(out.read_text(errors="replace"), err.read_text(errors="replace"))
        finally:
            ctx.cleanup(run)
        expected = rn.expected_decoder(run)
        ok = res["exit_code"] == 0 and (expected is None or parsed.get("decoder_actual") == expected)
        ref = self.reference.get(cell["trace"])
        rec = {
            "key": cell["key"], "experiment": exp["name"], "kind": exp["kind"], "config": cell["config"],
            "trace": cell["trace"], "records": cell["records"], "ranks": cell["ranks"], "workload": cell["workload"],
            "schema": cell["schema"], "rep": rep, "warmup": warmup, "round": rnd,
            "status": "ok" if ok else ("decoder_mismatch" if res["exit_code"] == 0 else "error"),
            "metric": res["wall_ns"] if ok and not warmup else None,
            **res, **snap, **cache, "cmd": cmd, "env": {k: v for k, v in run["env"].items() if k != "BENCH_ENV_PAD"},
            "env_pad": run["env_pad"], "build": run["build"], "commit": run["commit"], "decoder": run["decoder"],
            "jit_cache": run["jit_cache"], "phase_timing": run["phase_timing"], "trace_file": run["trace_file"],
            "header_sha256": run["header_sha256"], "params": self.cfg.get("params"),
            "events": parsed.get("events"), "speed_evps_printed": parsed.get("speed_evps_printed"),
            "max_time": parsed.get("max_time"), "result_sha256": parsed.get("result_sha256"),
            "matches_reference": (parsed.get("result_sha256") == ref) if ref else None,
            "decoder_actual": parsed.get("decoder_actual"), "phases": parsed.get("phases"),
            "error": parsed.get("error") if res["exit_code"] else None,
            "correctness": self.allowed(cell["trace"], cell["config"])[1],
        }
        if record:
            self.record(rec)
        if not warmup:
            phases = (parsed.get("phases") or {}).get("phases", {})
            print(f"    {cell['config']:20s} {cell['trace']:38s} {cell.get('cache', 'warm'):4s} "
                  f"{res['wall_ns'] / 1e6:10.1f} ms  rss {res['maxrss_kb'] / 1024:8.1f} MB"
                  f"{'  sim ' + format(phases['simulate'] / 1e6, '.1f') + ' ms' if 'simulate' in phases else ''}"
                  f"{'' if ok else '  [' + rec['status'] + ']'}{'  NOISY' if snap['noisy'] else ''}", flush=True)
        return rec

    def cells_e2e(self, exp):
        cells = []
        for t in grid.experiment_traces(self.cfg, exp):
            if not self.ctx.trace_entry(t["id"]):
                print(f"  skip {t['id']}: trace missing (run gen_traces.py)")
                continue
            for config in exp["configs"]:
                ok, why = self.allowed(t["id"], config)
                if not ok:
                    self.status[f"{exp['name']}|{config}|{t['id']}"] = {"state": f"excluded:{why}"}
                    continue
                for cache in exp.get("cache", ["warm"]):
                    cells.append({"key": f"{exp['name']}|e2e|{config}|{t['id']}|{cache}", "config": config,
                                  "trace": t["id"], "cache": cache, "records": t["records"], "ranks": t["ranks"],
                                  "workload": t["workload"], "schema": t["schema"]})
        return cells

    def run_e2e(self, exp):
        cells = self.cells_e2e(exp)
        print(f"[{exp['name']}] {len(cells)} cells")
        if self.a.dry_run:
            return
        self.adaptive(exp, cells, lambda c, rep, w, r: self.simulate_once(exp, c, rep, w, r)["metric"])

    # ------------------------------------------------------------ memcap
    def run_memcap(self, exp):
        probe = subprocess.run(["systemd-run", "--user", "--scope", "--quiet", "-p", "MemoryMax=1G", "true"],
                               capture_output=True, text=True) if shutil.which("systemd-run") else None
        if not probe or probe.returncode != 0:
            print(f"[{exp['name']}] skipped: user cgroups (systemd-run --user --scope) unavailable")
            self.status[exp["name"]] = {"state": "skipped", "reason": "no systemd-run --user --scope"}
            return
        cells = []
        for c in self.cells_e2e({**exp, "cache": ["warm"]}):
            for g in exp["limits_gb"]:
                cells.append({**c, "key": c["key"] + f"|{g}G", "limit_gb": g})
        print(f"[{exp['name']}] {len(cells)} cells")
        if self.a.dry_run:
            return

        def once(c, rep, w, r):
            prefix = ["systemd-run", "--user", "--scope", "--quiet", "-p", f"MemoryMax={c['limit_gb']}G",
                      "-p", "MemorySwapMax=0", "--"]
            rec = self.simulate_once(exp, c, rep, w, r, prefix=prefix, record=False)
            rec["limit_gb"] = c["limit_gb"]
            rec["oom_killed"] = rec["exit_code"] in (-9, 137)
            if rec["oom_killed"]:
                rec["status"] = "oom"
            self.record(rec)
            return rec["wall_ns"] if not w else None  # failures are results here, not errors
        self.adaptive(exp, cells, once, fixed_reps=exp.get("reps", 3))

    # ------------------------------------------------------------ concurrent
    def run_concurrent(self, exp):
        cells = []
        for c in self.cells_e2e({**exp, "cache": ["warm"]}):
            for k in exp["k"]:
                if self.pin and self.cpus(k) is None:
                    self.status[c["key"] + f"|k{k}"] = {"state": "skipped", "reason": f"fewer than {k} cpus"}
                    continue
                cells.append({**c, "key": c["key"] + f"|k{k}", "k": k})
        print(f"[{exp['name']}] {len(cells)} cells")
        if self.a.dry_run:
            return

        def once(c, rep, w, r):
            cpus = self.cpus(c["k"]) or [None] * c["k"]
            results = [None] * c["k"]
            barrier = threading.Barrier(c["k"])

            def worker(i):
                barrier.wait()
                results[i] = self.simulate_once(exp, c, rep, True, r, cpus=[cpus[i]] if cpus[i] is not None else None,
                                                record=False)
            t0 = time.perf_counter_ns()
            threads = [threading.Thread(target=worker, args=(i,)) for i in range(c["k"])]
            for t in threads:
                t.start()
            for t in threads:
                t.join()
            makespan = time.perf_counter_ns() - t0
            ok = all(x and x["status"] == "ok" for x in results)
            rec = {"key": c["key"], "experiment": exp["name"], "kind": "concurrent", "config": c["config"],
                   "trace": c["trace"], "records": c["records"], "ranks": c["ranks"], "workload": c["workload"],
                   "schema": c["schema"], "k": c["k"], "rep": rep, "warmup": w, "round": r,
                   "status": "ok" if ok else "error", "metric": makespan if ok and not w else None,
                   "makespan_ns": makespan, "cpus": cpus,
                   "sum_peak_rss_anon_kb": sum((x or {}).get("peak_rss_anon_kb") or 0 for x in results),
                   "sum_maxrss_kb": sum((x or {}).get("maxrss_kb") or 0 for x in results),
                   "processes": [{k: (x or {}).get(k) for k in ("wall_ns", "maxrss_kb", "peak_rss_anon_kb",
                                                                "peak_rss_file_kb", "majflt", "minflt", "status",
                                                                "result_sha256", "phases")} for x in results],
                   "loadavg": os.getloadavg(), "build": results[0]["build"] if results[0] else None}
            self.record(rec)
            if not w:
                print(f"    {c['config']:20s} {c['trace']:38s} k={c['k']:<3d} makespan {makespan / 1e6:10.1f} ms", flush=True)
            return rec["metric"]
        self.adaptive(exp, cells, once, metric_name="makespan_ns")

    # ------------------------------------------------------------ micro
    def run_micro(self, exp):
        micro = self.ctx.build("master")["binaries"].get("bench_decode_micro")
        if not micro:
            print(f"[{exp['name']}] skipped: master has no bench_decode_micro")
            return
        cells = []
        for t in grid.experiment_traces(self.cfg, exp):
            if not self.ctx.trace_entry(t["id"]):
                continue
            for v in exp["variants"]:
                if v in ("orc",) and not self.ctx.build("master").get("llvm_enabled"):
                    self.status[f"{exp['name']}|{v}|{t['id']}"] = {"state": "unavailable", "reason": "no LLVM"}
                    continue
                for o in exp.get("orders", ["seq"]):
                    for f in exp.get("fields", ["sim"]):
                        for cache in exp.get("cache", ["warm"]):
                            cells.append({"key": f"{exp['name']}|micro|{v}|{t['id']}|{o}|{f}|{cache}|"
                                                 f"{int(exp.get('jit_cold', False))}",
                                          "variant": v, "trace": t["id"], "order": o, "fields": f, "cache": cache,
                                          "records": t["records"], "ranks": t["ranks"], "workload": t["workload"],
                                          "schema": t["schema"]})
        print(f"[{exp['name']}] {len(cells)} cells x {exp.get('processes', 3)} processes x {exp.get('reps', 10)} reps")
        if self.a.dry_run:
            return
        warm_jit = self.ctx.warm_jit_dir("micro")
        for proc_index in range(exp.get("processes", 3)):
            for c in self.rng.sample(cells, len(cells)):
                if self.budget.exceeded():
                    break
                if proc_index in self.micro_done.get(c["key"], set()):
                    continue
                kind = "legacy" if c["variant"] in ("upstream", "legacy") else "sd"
                trace = self.ctx.trace_file(c["trace"], kind)
                cpus = self.cpus(1, exp.get("core_class", "fast"))
                snap = self.env_snapshot(cpus)
                cmd = [micro, "--trace", str(trace), "--variant", c["variant"], "--order", c["order"], "--fields",
                       c["fields"], "--cache", c["cache"], "--reps", str(exp.get("reps", 10)), "--warmup", "1",
                       "--jit-cache", str(warm_jit), "--seed", str(20261112 + proc_index)]
                if exp.get("jit_cold"):
                    cmd.append("--jit-cold")
                env = self.ctx.base_env()
                env["LOGGOPSIM_JIT_CC"] = self.ctx.build("master")["jit_cc"]
                out = self.scratch / "micro.out"
                res = bl.run_measured(cmd, env=env, cpus=cpus, membind=snap.get("membind"),
                                      timeout=self.env_cfg.get("timeout_s"), stdout_path=out,
                                      stderr_path=out.with_suffix(".err"))
                base = {"key": c["key"], "experiment": exp["name"], "kind": "micro", **{k: c[k] for k in (
                    "variant", "trace", "order", "fields", "cache", "records", "ranks", "workload", "schema")},
                    "jit_cold": bool(exp.get("jit_cold")), "process": proc_index, "core_class": exp.get("core_class", "fast"),
                    **snap, "cmd": cmd, "process_wall_ns": res["wall_ns"], "process_maxrss_kb": res["maxrss_kb"],
                    "trace_file": trace.name, "trace_bytes": trace.stat().st_size}
                try:
                    data = json.loads(out.read_text())
                except (json.JSONDecodeError, OSError):
                    self.record({**base, "status": "error", "metric": None, "exit_code": res["exit_code"],
                                 "error": out.with_suffix(".err").read_text(errors="replace")[-1000:]})
                    continue
                summary = {k: data[k] for k in ("runner_actual", "ranks", "records", "edges", "record_bytes",
                                                "node_bytes", "file_bytes", "checksum", "custom_checksum", "eviction")}
                ok = c["variant"] in ("upstream", "legacy") or data["runner_actual"] == c["variant"]
                for rep in data["reps"]:
                    self.record({**base, **summary, "rep": rep["rep"], "warmup": rep["warmup"],
                                 "status": "ok" if ok else "decoder_mismatch",
                                 "metric": rep["decode_ns"] if ok and not rep["warmup"] else None,
                                 "decode_ns": rep["decode_ns"], "setup_ns": rep["setup_ns"],
                                 "resident_before": rep["resident_before"],
                                 "ns_per_record": rep["decode_ns"] / max(1, data["records"])})
                self.micro_done.setdefault(c["key"], set()).add(proc_index)
                meds = sorted(r["decode_ns"] for r in data["reps"] if not r["warmup"])
                print(f"    {c['variant']:16s} {c['trace']:38s} {c['order']:6s} {c['fields']:6s} {c['cache']:4s} "
                      f"{meds[len(meds) // 2] / data['records']:7.2f} ns/rec  setup {data['reps'][-1]['setup_ns'] / 1e6:7.2f} ms"
                      f"{'' if ok else '  [ran ' + data['runner_actual'] + ']'}", flush=True)

    # ------------------------------------------------------------ convert
    def run_convert(self, exp):
        tools = self.cfg["convert_tools"]
        cells = []
        for t in grid.experiment_traces(self.cfg, exp):
            e = self.ctx.trace_entry(t["id"])
            if not e or "goal" not in e["files"]:
                print(f"  skip {t['id']}: GOAL file not kept (run gen_traces.py for this config)")
                continue
            for tool in exp["tools"]:
                threads = tools[tool].get("threads", 1)
                if self.pin and self.cpus(threads) is None:
                    self.status[f"{exp['name']}|{tool}|{t['id']}"] = {"state": "skipped", "reason": f"needs {threads} cpus"}
                    continue
                cells.append({"key": f"{exp['name']}|convert|{tool}|{t['id']}", "tool": tool, "trace": t["id"],
                              "records": t["records"], "ranks": t["ranks"], "workload": t["workload"],
                              "schema": t["schema"], "threads": threads})
        print(f"[{exp['name']}] {len(cells)} cells")
        if self.a.dry_run:
            return

        def once(c, rep, w, r):
            spec = tools[c["tool"]]
            build = self.ctx.build(spec["build"])
            e = self.ctx.trace_entry(c["trace"])
            plain = e["files"].get("plain_goal")
            legacy_fmt = spec["format"] == "legacy"
            src = self.ctx.traces_dir / (plain["name"] if (plain and legacy_fmt) else e["files"]["goal"]["name"])
            out = self.scratch / f"convert-{c['tool']}.bin"
            cmd = [build["binaries"]["txt2bin"], "-i", str(src), "-o", str(out)]
            env = self.ctx.base_env()
            if build["kind"] == "new":
                toml = bl.BENCH / "workloads" / ("custom_fields.toml" if c["schema"] == "custom" and not legacy_fmt
                                                 else "no_custom_fields.toml")
                cmd += ["--format", spec["format"], "--custom-fields", str(toml)]
                if "threads" in spec:
                    cmd += ["--threads", str(spec["threads"])]
                    env["OMP_NUM_THREADS"] = str(spec["threads"])
            cpus = self.cpus(c["threads"])
            snap = self.env_snapshot(cpus)
            cache = self.cache_state([str(src)], "warm")
            res = bl.run_measured(cmd, env=env, cpus=cpus, membind=snap.get("membind"),
                                  timeout=self.env_cfg.get("timeout_s"), stdout_path=self.scratch / "convert.out",
                                  stderr_path=self.scratch / "convert.err")
            size = out.stat().st_size if out.exists() else None
            if out.exists():
                out.unlink()
            ok = res["exit_code"] == 0
            self.record({"key": c["key"], "experiment": exp["name"], "kind": "convert", "tool": c["tool"],
                         "build": spec["build"], "commit": build["commit"], "format": spec["format"],
                         "threads": c["threads"], "trace": c["trace"], "records": c["records"], "ranks": c["ranks"],
                         "workload": c["workload"], "schema": c["schema"], "rep": rep, "warmup": w, "round": r,
                         "status": "ok" if ok else "error", "metric": res["wall_ns"] if ok and not w else None,
                         "input_bytes": src.stat().st_size, "output_bytes": size, **res, **snap, **cache, "cmd": cmd})
            if not w:
                print(f"    {c['tool']:16s} {c['trace']:38s} {res['wall_ns'] / 1e6:10.1f} ms", flush=True)
            return res["wall_ns"] if ok and not w else None
        self.adaptive(exp, cells, once)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", required=True)
    ap.add_argument("--experiments")
    ap.add_argument("--budget-hours", type=float)
    ap.add_argument("--resume")
    ap.add_argument("--skip-correctness", action="store_true")
    ap.add_argument("--correctness", help="use an existing correctness.json instead of running the gate")
    ap.add_argument("--cpus", help="pin to these CPUs instead of choosing the fastest free ones")
    ap.add_argument("--traces-dir", type=Path)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    cfg = bl.load_config(a.config)
    if a.traces_dir:
        os.environ["BENCH_TRACES"] = str(a.traces_dir.resolve())
    camp = Campaign(a, cfg)
    print(f"campaign {camp.dir}")
    camp.write_metadata()
    if not a.dry_run:
        camp.gate()
    kinds = {"e2e": camp.run_e2e, "micro": camp.run_micro, "convert": camp.run_convert,
             "concurrent": camp.run_concurrent, "memcap": camp.run_memcap}
    for exp in grid.experiments(cfg, camp.only):
        if camp.budget.exceeded():
            camp.status[exp["name"]] = {"state": "budget_exhausted"}
            print(f"[{exp['name']}] skipped: time budget exhausted")
            continue
        kinds[exp["kind"]](exp)
        bl.write_json(camp.dir / "status.json", camp.status)
    bl.write_json(camp.dir / "status.json", camp.status)
    shutil.rmtree(camp.scratch, ignore_errors=True)
    print(f"done: {camp.dir}\nanalyze with: python3 bench/analyze.py {camp.dir}")


if __name__ == "__main__":
    main()
