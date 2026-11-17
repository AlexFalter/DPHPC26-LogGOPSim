"""Turns a runtime config + trace into the exact command and environment of one simulator run.
check_correctness.py and run.py both use it, so the correctness gate checks precisely what is timed."""
from __future__ import annotations

import os
import random
import shutil
import string
import tempfile
from pathlib import Path

import benchlib as bl

# Only these variables reach the measured process (plus what a config sets), so that runs do not
# depend on the caller's shell environment.
# XDG_RUNTIME_DIR / DBUS_SESSION_BUS_ADDRESS: systemd-run --user (memory_budget) needs the session bus.
PASS_ENV = ("PATH", "HOME", "USER", "LOGNAME", "TMPDIR", "LD_LIBRARY_PATH", "SLURM_JOB_ID", "XDG_RUNTIME_DIR",
            "DBUS_SESSION_BUS_ADDRESS")


class Context:
    def __init__(self, cfg: dict, scratch: Path, traces_dir: Path | None = None):
        self.cfg = cfg
        self.catalog = cfg.get("configs", {})
        self.params = cfg.get("params", {})
        self.scratch = Path(scratch)
        self.scratch.mkdir(parents=True, exist_ok=True)
        self.traces_dir = traces_dir or bl.traces_dir()
        self.trace_manifest = bl.load_trace_manifest(self.traces_dir)["traces"]
        self._builds = {}
        self.env_padding = cfg.get("environment", {}).get("env_padding", False)
        self.rng = random.Random(20261112)

    def build(self, name):
        if name not in self._builds:
            self._builds[name] = bl.load_build_manifest(name)
        return self._builds[name]

    def trace_entry(self, tid):
        e = self.trace_manifest.get(tid)
        if not e or e.get("status") != "ok":
            return None
        return e

    def trace_file(self, tid, kind):
        e = self.trace_entry(tid)
        if not e or kind not in e["files"]:
            return None
        return self.traces_dir / e["files"][kind]["name"]

    def warm_jit_dir(self, build):
        d = self.scratch / f"jit-warm-{build}"
        d.mkdir(exist_ok=True)
        return d

    def base_env(self):
        env = {k: os.environ[k] for k in PASS_ENV if k in os.environ}
        env["LANG"] = env["LC_ALL"] = "C"
        return env

    def sim_run(self, config: str, tid: str, viz: Path | None = None) -> dict:
        """Command, environment and bookkeeping for one run of a runtime config on a trace."""
        c = self.catalog[config]
        build = self.build(c["build"])
        trace = self.trace_file(tid, c["file"])
        if trace is None:
            raise FileNotFoundError(f"trace {tid} has no {c['file']} file")
        env = self.base_env()
        cleanup = []
        new = build["kind"] == "new"
        if new:
            env["LOGGOPSIM_TRACE_DECODER"] = "1"
            env["LOGGOPSIM_JIT_CC"] = build["jit_cc"]
            if c.get("decoder"):
                env["LOGGOPSIM_DECODER"] = c["decoder"]
            if c.get("jit_cache") == "cold":
                d = Path(tempfile.mkdtemp(prefix="jit-cold-", dir=self.scratch))
                env["LOGGOPSIM_JIT_CACHE"] = str(d)
                cleanup.append(d)
            else:  # warm, or not a JIT config: one persistent directory per build
                env["LOGGOPSIM_JIT_CACHE"] = str(self.warm_jit_dir(c["build"]))
            if c.get("phase_timing") and build.get("instrumented"):
                env["GOAL_PHASE_TIMING"] = "1"
        env.update(c.get("env", {}))
        pad = 0
        if self.env_padding:  # Mytkowicz et al. (ASPLOS'09): environment size shifts the stack
            pad = self.rng.randrange(0, 4096)
            env["BENCH_ENV_PAD"] = "".join(self.rng.choice(string.ascii_letters) for _ in range(pad))
        run_trace = trace
        if c.get("copy_trace"):  # --save-mem destroys its input: never on the shared trace
            run_trace = self.scratch / f"copy-{os.getpid()}-{self.rng.randrange(1 << 30)}-{trace.name}"
            shutil.copyfile(trace, run_trace)
            cleanup.append(run_trace)
        cmd = [build["binaries"]["LogGOPSim"], "-f", str(run_trace)] + bl.loggopsim_args(self.params)
        cmd += list(c.get("args", []))
        if viz:
            cmd += ["-V", str(viz)]
        entry = self.trace_entry(tid)
        return {
            "cmd": cmd, "env": env, "cleanup": cleanup, "trace_path": str(trace), "run_trace": str(run_trace),
            "env_pad": pad, "build": c["build"], "commit": build["commit"], "decoder": c.get("decoder"),
            "jit_cache": c.get("jit_cache"), "phase_timing": bool(c.get("phase_timing") and build.get("instrumented")),
            "file_kind": c["file"],
            "trace_file": {"name": trace.name, **entry["files"][c["file"]]},
            "header_sha256": entry.get("header_sha256") if c["file"] == "sd" else None,
        }

    @staticmethod
    def cleanup(run):
        for p in run.get("cleanup", []):
            p = Path(p)
            if p.is_dir():
                shutil.rmtree(p, ignore_errors=True)
            elif p.exists():
                p.unlink()


def expected_decoder(run):
    """What LOGGOPSIM_TRACE_DECODER must report for the config to count as measured."""
    if run["file_kind"] == "legacy":
        return "legacy" if bl.load_build_manifest(run["build"])["kind"] == "new" else None
    return run["decoder"] or "plan"
