#!/usr/bin/env python3
"""Generates every trace a campaign config needs: GOAL text -> legacy .bin (BASE's txt2bin) and
self-describing .bin (NEW's txt2bin), plus a manifest with sizes, hashes and schemas.

usage: gen_traces.py --config bench/configs/full.json [--experiments a,b] [--traces-dir DIR]
                     [--jobs J] [--force] [--keep-goal] [--dry-run]

Per trace <id> = <workload>-p<ranks>-n<records>-<schema> in --traces-dir (default
$BENCH_WORK/traces, i.e. bench/.work/traces):
  <id>.goal           GOAL text (deleted after conversion unless a convert experiment needs it)
  <id>.plain.goal     custom schema only: the same schedule without custom fields (BASE input)
  <id>.legacy.bin     legacy format, written by BASE's (unmodified upstream) txt2bin
  <id>.sd.bin         self-describing format, written by master's txt2bin
  <id>.schema.txt     master's --report-schema output (field widths, record size)
  manifest.json       per trace: spec, record/edge counts, file sizes, SHA-256, header SHA-256
                      (the format has no schema hash; this stands in for it), conversion times,
                      byte-identity of NEW's/omp's legacy output with BASE's (small traces only)
Workloads: see bench/workloads/goalgen.py --list; also "schedgen:<pattern>" (master's schedgen,
--commsize = ranks; records are whatever the pattern produces) and "goal:<path>" (an existing GOAL
file, e.g. an ATLAHS AI trace converted without --explicit-fields).
Traces whose files and spec are unchanged are skipped, so the script can be re-run after a failure.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
sys.path.insert(0, str(Path(__file__).resolve().parent / "workloads"))
import benchlib as bl  # noqa: E402
import grid  # noqa: E402
import goalgen  # noqa: E402

CUSTOM_TOML = bl.BENCH / "workloads" / "custom_fields.toml"
NO_CUSTOM_TOML = bl.BENCH / "workloads" / "no_custom_fields.toml"


def estimate_bytes(t):
    """Rough disk need (GOAL text + both binaries), for the free-space check."""
    n = t["records"]
    return int(n * (80 + 50 + 15) * (1.4 if t["schema"] == "custom" else 1.0))


def run_tool(cmd, log_dir, name, env=None):
    out, err = log_dir / f"{name}.out", log_dir / f"{name}.err"
    res = bl.run_measured(cmd, env=env, stdout_path=out, stderr_path=err)
    res["cmd"] = [str(c) for c in cmd]
    if res["exit_code"] != 0:
        raise RuntimeError(f"{name} failed ({res['exit_code']}): {' '.join(res['cmd'])}\n"
                           f"{err.read_text(errors='replace')[-2000:]}")
    return res


def files_ok(entry, directory: Path, needs_goal: bool):
    if not entry or entry.get("status") != "ok":
        return False
    for kind in ("legacy", "sd"):
        f = entry["files"].get(kind)
        if not f or not (directory / f["name"]).exists() or (directory / f["name"]).stat().st_size != f["bytes"]:
            return False
    if needs_goal and not (directory / entry["files"].get("goal", {}).get("name", "-")).exists():
        return False
    return True


def generate_goal(t, goal: Path, plain: Path | None, jobs: int, builds):
    workload = t["workload"]
    if workload.startswith("schedgen:"):
        schedgen = builds["master"]["binaries"]["schedgen"]
        if not schedgen:
            raise RuntimeError("master's schedgen was not built")
        bl.run_measured([schedgen, "-p", workload.split(":", 1)[1], "-s", str(t["ranks"]), "-o", str(goal)])
        records = edges = 0
        with open(goal) as f:
            for line in f:
                s = line.strip()
                if ":" in s and s.startswith("l"):
                    records += 1
                elif " requires " in s or " irequires " in s:
                    edges += 1
        return {"generator": "schedgen", "records": records, "edges": edges}
    if workload.startswith("goal:"):
        shutil.copyfile(workload.split(":", 1)[1], goal)
        return {"generator": "import", "source": workload.split(":", 1)[1]}
    info = goalgen.generate(workload, t["ranks"], t["records"], str(goal), t["schema"], jobs,
                            str(plain) if plain else None)
    info["generator"] = "goalgen"
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", required=True)
    ap.add_argument("--experiments", help="comma-separated subset")
    ap.add_argument("--traces-dir", type=Path)
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--keep-goal", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    cfg = bl.load_config(a.config)
    only = a.experiments.split(",") if a.experiments else None
    traces = grid.all_traces(cfg, only)
    directory = (a.traces_dir or bl.traces_dir()).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    os.environ["BENCH_TRACES"] = str(directory)
    manifest_path = directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {"traces": {}}
    check = cfg.get("check", {})
    identity_max = float(check.get("byte_identity_max_records", 1e6))

    order = sorted(traces.values(), key=lambda t: (t["records"], t["id"]))
    total = sum(estimate_bytes(t) for t in order)
    print(f"{len(order)} traces in {directory} (fs {bl.fs_type(directory)}), "
          f"rough upper bound {total / 1e9:.1f} GB while converting")
    if str(directory).startswith("/mnt/") and bl.machine_info().get("is_wsl"):
        print("WARNING: traces on a Windows drive under WSL are slow and distort cache effects; "
              "set BENCH_TRACES/BENCH_WORK to a Linux path.", file=sys.stderr)
    if a.dry_run:
        for t in order:
            print(f"  {t['id']:45s} ~{estimate_bytes(t) / 1e9:7.2f} GB  {','.join(t['experiments'])}")
        return

    builds = {n: bl.load_build_manifest(n) for n in ("base", "master")}
    for optional in ("omp",):
        try:
            builds[optional] = bl.load_build_manifest(optional)
        except SystemExit:
            pass
    logs = directory / "logs"
    logs.mkdir(exist_ok=True)
    failures = 0
    for t in order:
        tid = t["id"]
        entry = manifest["traces"].get(tid)
        spec = {k: t[k] for k in grid.TRACE_KEYS}
        if not a.force and entry and entry.get("spec") == spec \
                and entry.get("generator_version") == goalgen.VERSION and files_ok(entry, directory, t["needs_goal"] or a.keep_goal):
            print(f"= {tid} (up to date)")
            entry["experiments"] = t["experiments"]
            continue
        free = shutil.disk_usage(directory).free
        if estimate_bytes(t) > free * 0.9:
            print(f"! {tid}: needs ~{estimate_bytes(t) / 1e9:.1f} GB, only {free / 1e9:.1f} GB free: skipped")
            manifest["traces"][tid] = {"spec": spec, "status": "skipped_disk", "experiments": t["experiments"]}
            failures += 1
            continue
        print(f"+ {tid}", flush=True)
        goal = directory / f"{tid}.goal"
        plain = directory / f"{tid}.plain.goal" if t["schema"] == "custom" else None
        legacy, sd = directory / f"{tid}.legacy.bin", directory / f"{tid}.sd.bin"
        new = {"spec": spec, "experiments": t["experiments"], "status": "running", "files": {}, "times": {},
               "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "generator_version": goalgen.VERSION}
        try:
            t0 = time.perf_counter()
            new["generation"] = generate_goal(t, goal, plain, a.jobs, builds)
            new["times"]["generate_s"] = time.perf_counter() - t0
            base_input = plain or goal
            toml = CUSTOM_TOML if t["schema"] == "custom" else NO_CUSTOM_TOML
            r = run_tool([builds["base"]["binaries"]["txt2bin"], "-i", base_input, "-o", legacy], logs, f"{tid}.base")
            new["times"]["txt2bin_base_legacy_s"] = r["wall_ns"] / 1e9
            r = run_tool([builds["master"]["binaries"]["txt2bin"], "-i", goal, "-o", sd, "--format", "self-describing",
                          "--custom-fields", toml, "--report-schema"], logs, f"{tid}.master-sd")
            new["times"]["txt2bin_master_sd_s"] = r["wall_ns"] / 1e9
            shutil.copyfile(logs / f"{tid}.master-sd.out", directory / f"{tid}.schema.txt")
            if t["records"] <= identity_max:
                ident = {}
                tmp = directory / f"{tid}.check.bin"
                # Legacy output is written from BASE's input (no custom fields), self-describing from the full GOAL.
                plain_fields = ["--custom-fields", NO_CUSTOM_TOML]
                checks = [("master-legacy", "master", ["--format", "legacy"] + plain_fields, legacy)]
                if "omp" in builds and builds["omp"]["binaries"].get("txt2bin"):
                    checks += [("omp-legacy-t4", "omp", ["--format", "legacy", "--threads", "4"] + plain_fields, legacy),
                               ("omp-sd-t4", "omp", ["--format", "self-describing", "--custom-fields", toml, "--threads", "4"], sd)]
                for label, build, extra, reference in checks:
                    src = goal if "self-describing" in extra else base_input
                    run_tool([builds[build]["binaries"]["txt2bin"], "-i", src, "-o", tmp] + extra, logs, f"{tid}.{label}")
                    ident[label] = {"reference": reference.name, "identical": tmp.read_bytes() == reference.read_bytes()}
                    tmp.unlink()
                new["byte_identity"] = ident
            for kind, path in (("legacy", legacy), ("sd", sd)):
                new["files"][kind] = {"name": path.name, "bytes": path.stat().st_size, "sha256": bl.sha256_file(path)}
            header = bl.self_describing_header(sd)
            if header:
                import hashlib
                new["header_sha256"] = hashlib.sha256(header[0]).hexdigest()
                new["header_bytes"] = len(header[0])
                new["record_bits"] = header[1]
            keep = t["needs_goal"] or a.keep_goal
            for kind, path in (("goal", goal), ("plain_goal", plain)):
                if path and path.exists():
                    if keep:
                        new["files"][kind] = {"name": path.name, "bytes": path.stat().st_size}
                    else:
                        new["goal_bytes" if kind == "goal" else "plain_goal_bytes"] = path.stat().st_size
                        path.unlink()
            new["status"] = "ok"
            os.sync()
            g = new.get("generation", {})
            print(f"  records={g.get('records')} edges={g.get('edges')} legacy={new['files']['legacy']['bytes'] / 1e6:.1f} MB "
                  f"sd={new['files']['sd']['bytes'] / 1e6:.1f} MB record_bits={new.get('record_bits')} "
                  f"identity={ {k: v['identical'] for k, v in new.get('byte_identity', {}).items()} }", flush=True)
        except Exception as e:  # keep going; the failure is recorded and reported
            new["status"] = "failed"
            new["error"] = str(e)
            failures += 1
            print(f"! {tid} failed: {e}", file=sys.stderr)
        manifest["traces"][tid] = new
        manifest["updated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        manifest["builds"] = {n: {"commit": b["commit"], "txt2bin": b["binaries"]["txt2bin"]} for n, b in builds.items()}
        bl.write_json(manifest_path, manifest)
    bl.write_json(manifest_path, manifest)
    print(f"manifest: {manifest_path} ({failures} failed/skipped)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
