#!/usr/bin/env python3
"""Correctness gate (TASK.md 6.1): every (trace, config) must reproduce BASE's results exactly.

usage: check_correctness.py --config CFG [--experiments a,b] [--out DIR] [--jobs N]

For each trace a campaign uses and each runtime config used with it:
  * LogGOPSim's deterministic output (the size/parameter line, per-host finish times or, for more
    than 16 ranks, the maximum finishing time, and any leftover queue entries) must be identical
    to BASE on the equivalent legacy trace;
  * for traces up to check.viz_max_records records, the -V timeline (start/end time of every
    operation on every host; unchanged upstream code) must be byte-identical too, which covers the
    per-host times LogGOPSim does not print for > 16 ranks;
  * the decoder that actually ran (LOGGOPSIM_TRACE_DECODER) must be the requested one, otherwise
    the config is "unavailable" for that trace (e.g. templated_schema without a compiled-in schema,
    orc without LLVM) and gets no numbers;
  * phase timing must print exactly when requested.
Plus the validation-failure tests (missing required field, values too wide for their field,
corrupt and invalidated traces, --save-mem destroying its input) and the byte identity of the
legacy files written by BASE, master and omp (from the trace manifest).
Writes DIR/correctness.json and DIR/correctness.md; exit status 1 if anything failed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import benchlib as bl  # noqa: E402
import grid  # noqa: E402
import runner as rn  # noqa: E402


def sha_file(path):
    return bl.sha256_file(path) if path and Path(path).exists() else None


def simulate(ctx, config, tid, workdir, viz):
    run = ctx.sim_run(config, tid, viz=workdir / f"{config}.viz" if viz else None)
    try:
        p = subprocess.run(run["cmd"], env=run["env"], capture_output=True, text=True, timeout=7200)
        parsed = bl.parse_loggopsim(p.stdout, p.stderr)
        res = {"exit_code": p.returncode, "result_sha256": parsed["result_sha256"], "events": parsed.get("events"),
               "max_time": parsed.get("max_time"), "decoder_actual": parsed.get("decoder_actual"),
               "fallback": parsed.get("decoder_fallback"), "has_phases": "phases" in parsed,
               "error": parsed.get("error") or (p.stderr.strip()[-500:] if p.returncode else None),
               "viz_sha256": sha_file(workdir / f"{config}.viz") if viz else None,
               "expected_decoder": rn.expected_decoder(run), "phase_timing": run["phase_timing"],
               "result_lines": parsed["result_lines"][:20]}
    finally:
        ctx.cleanup(run)
        if viz and (workdir / f"{config}.viz").exists():
            (workdir / f"{config}.viz").unlink()
    return res


def judge(res, ref):
    if res["exit_code"] != 0:
        return "error", f"exit {res['exit_code']}: {res['error']}"
    if res["expected_decoder"] and res["decoder_actual"] != res["expected_decoder"]:
        return "unavailable", f"requested {res['expected_decoder']}, ran {res['decoder_actual']}"
    if res["phase_timing"] != res["has_phases"]:
        return "fail", f"phase timing requested={res['phase_timing']} printed={res['has_phases']}"
    if ref is None or ref["exit_code"] != 0:
        return "no_reference", "BASE did not produce a reference"
    if res["result_sha256"] != ref["result_sha256"] or res["events"] != ref["events"]:
        return "fail", f"result differs from BASE (max_time {res['max_time']} vs {ref['max_time']})"
    if ref.get("viz_sha256") and res.get("viz_sha256") and res["viz_sha256"] != ref["viz_sha256"]:
        return "fail", "per-operation timeline (-V) differs from BASE"
    return "pass", "identical to BASE" + (" incl. -V timeline" if res.get("viz_sha256") else "")


# ------------------------------------------------------------------ validation-failure tests

def run_cmd(cmd, **kw):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, timeout=600, **kw)
    return p.returncode, (p.stdout + p.stderr).strip()


def write(path, text):
    Path(path).write_text(text)
    return path


def validation_tests(ctx, tmp: Path, sd_plain: Path | None, sd_custom: Path | None, legacy: Path | None):
    master, base = ctx.build("master"), ctx.build("base")
    tests = []

    def expect(name, cmd, should_fail, must_contain=None, informational=False, env=None):
        code, out = run_cmd(cmd, env=env)
        ok = (code != 0) == should_fail and (must_contain is None or must_contain.lower() in out.lower())
        tests.append({"name": name, "cmd": " ".join(map(str, cmd)), "exit_code": code, "output": out[-600:],
                      "expected": "failure" if should_fail else "success", "passed": ok or informational,
                      "informational": informational})

    micro = master["binaries"].get("bench_decode_micro")
    if micro and sd_plain:
        expect("missing required custom field gpu_id is rejected at load time",
               [micro, "--trace", sd_plain, "--require", "gpu_id"], True, "gpu_id")
    if micro and legacy:
        expect("legacy trace has no custom fields: gpu_id rejected", [micro, "--trace", legacy, "--require", "gpu_id"],
               True, "gpu_id")
    if micro and sd_custom:
        expect("present custom field gpu_id is accepted", [micro, "--trace", sd_custom, "--require", "gpu_id"], False)

    toml = bl.BENCH / "workloads" / "custom_fields.toml"
    none = bl.BENCH / "workloads" / "no_custom_fields.toml"
    t2b = master["binaries"]["txt2bin"]
    two = "num_ranks 2\n\nrank 0 {\n%s\n}\n\nrank 1 {\n%s\n}\n"
    custom = " comm_id 1 gpu_id %s stream_id 0 channel 0"
    cases = [
        ("cpu 256 exceeds the 8-bit cpu field", two % ("l1: calc 100 cpu 256", "l1: calc 1"), none, "cpu"),
        ("tag 2^32 exceeds the 32-bit tag field",
         two % ("l1: send 8b to 1 tag 4294967296", "l1: recv 8b from 0 tag 4294967296"), none, "tag"),
        ("peer outside num_ranks", two % ("l1: send 8b to 5 tag 0", "l1: calc 1"), none, "num_ranks"),
        ("custom unsigned value 2^64 exceeds uint64",
         two % ("l1: calc 1" + custom % "18446744073709551616", "l1: calc 1" + custom % "0"), toml, "gpu_id"),
        ("negative value for an unsigned custom field",
         two % ("l1: calc 1" + custom % "-1", "l1: calc 1" + custom % "0"), toml, "gpu_id"),
        ("declared custom field missing on an operation",
         two % ("l1: calc 1 comm_id 1 stream_id 0 channel 0", "l1: calc 1" + custom % "0"), toml, "gpu_id"),
    ]
    for i, (name, text, fields, word) in enumerate(cases):
        goal = write(tmp / f"invalid{i}.goal", text)
        for fmt in ("legacy", "self-describing"):
            if fields == toml and fmt == "legacy":
                continue
            expect(f"txt2bin --format {fmt} rejects: {name}",
                   [t2b, "-i", goal, "-o", tmp / f"invalid{i}.{fmt}.bin", "--format", fmt, "--custom-fields", fields],
                   True, word)
        if fields == none:  # what upstream does with the same input (informational: it has no checks)
            expect(f"(upstream txt2bin, informational) {name}", [base["binaries"]["txt2bin"], "-i", goal, "-o",
                   tmp / f"invalid{i}.base.bin"], False, informational=True)

    sim_new, sim_base = master["binaries"]["LogGOPSim"], base["binaries"]["LogGOPSim"]
    if sd_plain:
        cut = tmp / "truncated.sd.bin"
        data = sd_plain.read_bytes()
        cut.write_bytes(data[: max(64, len(data) // 2)])
        expect("NEW rejects a truncated self-describing trace", [sim_new, "-f", cut], True)
        garbage = write(tmp / "garbage.bin", "this is not a trace" * 10)
        expect("NEW rejects an unknown format", [sim_new, "-f", garbage], True, "format")
    if legacy:
        for label, sim in (("BASE", sim_base), ("NEW", sim_new)):
            copy = tmp / f"savemem-{label}.bin"
            shutil.copyfile(legacy, copy)
            expect(f"{label} --save-mem run on a copy succeeds", [sim, "-f", copy, "--save-mem"], False)
            expect(f"{label}: a trace consumed by --save-mem is rejected afterwards", [sim, "-f", copy], True,
                   "invalid")
            copy.unlink()
    return tests


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", required=True)
    ap.add_argument("--experiments")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--traces-dir", type=Path)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    a = ap.parse_args()
    report = check(bl.load_config(a.config), a.experiments.split(",") if a.experiments else None,
                   a.out or bl.new_campaign_dir(bl.results_dir() / "correctness", Path(a.config).stem),
                   a.traces_dir, a.jobs)
    sys.exit(0 if report["ok"] else 1)


def check(cfg, only, out: Path, traces_dir=None, jobs=1) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix="correctness-", dir=out))
    ctx = rn.Context(cfg, tmp / "scratch", traces_dir)
    viz_max = float(cfg.get("check", {}).get("viz_max_records", 1e6))
    traces = grid.all_traces(cfg, only)
    results, started = {}, time.time()

    def check_trace(t):
        tid = t["id"]
        entry = ctx.trace_entry(tid)
        if entry is None:
            return tid, {"status": "missing_trace"}
        configs = [c for c in t["configs"] if c != "BASE"]
        if not configs:
            return tid, {"status": "no_simulator_configs"}
        work = tmp / tid
        work.mkdir(exist_ok=True)
        viz = t["records"] <= viz_max
        ref = simulate(ctx, "BASE", tid, work, viz)
        per = {"BASE": {"status": "pass" if ref["exit_code"] == 0 else "error", "detail": "reference",
                        **{k: ref[k] for k in ("events", "max_time", "result_sha256", "viz_sha256", "exit_code")}}}
        for config in configs:
            res = simulate(ctx, config, tid, work, viz)
            status, detail = judge(res, ref)
            per[config] = {"status": status, "detail": detail,
                           **{k: res[k] for k in ("events", "max_time", "result_sha256", "viz_sha256",
                                                  "decoder_actual", "exit_code")}}
        shutil.rmtree(work, ignore_errors=True)
        return tid, {"status": "checked", "records": t["records"], "viz_compared": viz, "configs": per,
                     "byte_identity": entry.get("byte_identity")}

    with ThreadPoolExecutor(max(1, jobs)) as pool:
        for tid, res in pool.map(check_trace, sorted(traces.values(), key=lambda t: t["records"])):
            results[tid] = res
            if res.get("configs"):
                summary = {c: r["status"] for c, r in res["configs"].items()}
                print(f"{tid}: {summary}", flush=True)

    def pick(schema, kind):  # the smallest available trace of this schema
        cands = [tid for tid, t in traces.items() if t["schema"] == schema and ctx.trace_entry(tid)]
        if not cands:
            return None
        return ctx.trace_file(min(cands, key=lambda tid: ctx.trace_entry(tid)["files"][kind]["bytes"]), kind)

    tests = validation_tests(ctx, tmp, pick("inferred", "sd"), pick("custom", "sd"), pick("inferred", "legacy"))
    failed_pairs = [(tid, c) for tid, r in results.items() for c, v in r.get("configs", {}).items()
                    if v["status"] in ("fail", "error")]
    identity = [(tid, k) for tid, r in results.items() for k, v in (r.get("byte_identity") or {}).items()
                if not v["identical"]]
    report = {"ok": not failed_pairs and all(t["passed"] for t in tests), "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "duration_s": time.time() - started, "traces": results, "validation_tests": tests,
              "failed_pairs": failed_pairs, "byte_identity_differences": identity,
              "machine": bl.machine_info()["hostname"]}
    bl.write_json(out / "correctness.json", report)
    write_markdown(out / "correctness.md", report)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"correctness: {'OK' if report['ok'] else 'FAILURES'} -> {out / 'correctness.md'}")
    return report


def write_markdown(path, report):
    lines = ["# Correctness report", "", f"Created {report['created']} on {report['machine']}; "
             f"overall: **{'pass' if report['ok'] else 'FAIL'}**.", "", "## Simulator results vs BASE", "",
             "| trace | config | status | detail |", "|---|---|---|---|"]
    for tid, r in sorted(report["traces"].items()):
        for c, v in sorted(r.get("configs", {}).items()):
            lines.append(f"| {tid} | {c} | {v['status']} | {v['detail']} |")
        if not r.get("configs"):
            lines.append(f"| {tid} | – | {r['status']} | |")
    lines += ["", "## Byte identity of legacy/self-describing files written by NEW/omp vs BASE/master", "",
              "| trace | writer | identical to |", "|---|---|---|"]
    for tid, r in sorted(report["traces"].items()):
        for k, v in sorted((r.get("byte_identity") or {}).items()):
            lines.append(f"| {tid} | {k} | {v['reference']}: {'yes' if v['identical'] else '**NO**'} |")
    lines += ["", "## Validation-failure tests", "", "| test | expected | exit | result | output |", "|---|---|---|---|---|"]
    for t in report["validation_tests"]:
        res = "info" if t["informational"] else ("pass" if t["passed"] else "**FAIL**")
        out = t["output"].replace("\n", " ").replace("|", "\\|")[:160]
        lines.append(f"| {t['name']} | {t['expected']} | {t['exit_code']} | {res} | `{out}` |")
    Path(path).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
