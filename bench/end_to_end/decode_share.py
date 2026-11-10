#!/usr/bin/env python3
"""Decode share of simulator runtime: replay time (decode_bench's Replay) vs full simulation.

usage: decode_share.py BUILD_DIR RUNS TRACE...
Expects BUILD_DIR/bench/traces/TRACE.legacy.bin and TRACE.sd.bin (the bench_traces target, or
traces copied there); the self-describing file is run with each plan runner
(LOGGOPSIM_DECODER=plan|jit|orc|templated_node|templated_schema). If the loggopsim_original
target was built, the original LogGOPSim also simulates the legacy file ("original"); it has no
replay, so its replay columns are empty. The warm-up run fills the JIT cache, so measured
runs see a cache hit; setup_ms is the schedule load time. Medians with 95% nonparametric CIs
(Le Boudec), raw samples in BUILD_DIR/bench/decode_share.csv.
"""
import csv
import json
import math
import os
import subprocess
import sys
import time
from pathlib import Path


def median_ci(samples):
    s = sorted(samples)
    n = len(s)
    half = 1.96 * math.sqrt(n) / 2
    lo = max(int(math.floor(n / 2 - half)), 0)
    hi = min(int(math.ceil(n / 2 + half)), n - 1)
    m = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2
    return m, s[lo], s[hi]


CONFIGS = (("legacy", "legacy", "plan"), ("plan", "sd", "plan"),
           ("jit", "sd", "jit"), ("orc", "sd", "orc"), ("templated_node", "sd", "templated_node"), ("templated_schema", "sd", "templated_schema"))


def simulate(simulator, schedule, env):
    start = time.monotonic_ns()
    subprocess.run([simulator, "-f", schedule], stdout=subprocess.DEVNULL, check=True, env=env)
    return time.monotonic_ns() - start


def replay(build, trace, label, runs, env):
    """Replay times and setup times in ns, one per run, and the node count."""
    out = subprocess.run([build / "bench" / "decode_bench", f"--traces={build / 'bench' / 'traces' / trace}.sd.bin",
                          f"--benchmark_filter=^Replay/{trace}/{label}/", "--benchmark_min_time=1x",
                          f"--benchmark_repetitions={runs + 1}", "--benchmark_format=json"],
                         capture_output=True, text=True, check=True, env=env).stdout
    rows = [row for row in json.loads(out)["benchmarks"] if row["run_type"] == "iteration"][1:]
    assert rows and "error_occurred" not in rows[0], f"Replay/{trace}/{label} did not run"
    # The benchmark reports milliseconds; the warm-up repetition is dropped.
    return [row["real_time"] * 1e6 for row in rows], [row["setup"] * 1e9 for row in rows], int(rows[0]["nodes"])


def main():
    build, runs, traces = Path(sys.argv[1]).resolve(), int(sys.argv[2]), sys.argv[3:]
    raw = []
    print("trace,decoder,nodes,sim_ms,sim_ci,replay_ms,replay_ci,replay_ns_per_node,share,setup_ms")
    original = build / "bench" / "original" / "build" / "LogGOPSim"
    for trace in traces:
        if original.exists():
            schedule = build / "bench" / "traces" / f"{trace}.legacy.bin"
            simulate(original, schedule, os.environ)
            sim = [simulate(original, schedule, os.environ) for _ in range(runs)]
            raw += [(trace, "original", "sim", i, v) for i, v in enumerate(sim)]
            sm, sl, sh = median_ci(sim)
            print(f"{trace},original,,{sm/1e6:.1f},[{sl/1e6:.1f};{sh/1e6:.1f}],,,,,", flush=True)
        for label, suffix, decoder in CONFIGS:
            schedule = build / "bench" / "traces" / f"{trace}.{suffix}.bin"
            env = dict(os.environ, LOGGOPSIM_DECODER=decoder)
            simulate(build / "LogGOPSim", schedule, env)
            sim = [simulate(build / "LogGOPSim", schedule, env) for _ in range(runs)]
            rep, setup, nodes = replay(build, trace, label, runs, env)
            raw += [(trace, label, "sim", i, v) for i, v in enumerate(sim)]
            raw += [(trace, label, "replay", i, v) for i, v in enumerate(rep)]
            sm, sl, sh = median_ci(sim)
            rm, rl, rh = median_ci(rep)
            print(f"{trace},{label},{nodes},{sm/1e6:.1f},[{sl/1e6:.1f};{sh/1e6:.1f}],"
                  f"{rm/1e6:.1f},[{rl/1e6:.1f};{rh/1e6:.1f}],{rm/nodes:.1f},{rm/sm:.2f},"
                  f"{median_ci(setup)[0]/1e6:.1f}", flush=True)
    with open(build / "bench" / "decode_share.csv", "w", newline="") as f:
        csv.writer(f).writerows([("trace", "decoder", "kind", "run", "ns")] + raw)


if __name__ == "__main__":
    main()
