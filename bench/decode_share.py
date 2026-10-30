#!/usr/bin/env python3
"""Decode share of simulator runtime: replay time (decode_bench) vs full simulation.

usage: decode_share.py BUILD_DIR RUNS TRACE...
Expects BUILD_DIR/TRACE.legacy.bin and BUILD_DIR/TRACE.sd.bin; the self-describing file is
run with the generic decoder and with the decode plan (LOGGOPSIM_DECODER).
Medians with 95% nonparametric CIs (Le Boudec), raw samples in BUILD_DIR/decode_share.csv.
"""
import csv
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


CONFIGS = (("legacy", "legacy", "generic"), ("generic", "sd", "generic"), ("plan", "sd", "plan"))


def simulate(build, schedule, env):
    start = time.monotonic_ns()
    subprocess.run([build / "LogGOPSim", "-f", schedule], stdout=subprocess.DEVNULL, check=True, env=env)
    return time.monotonic_ns() - start


def replay(build, schedule, runs, env):
    out = subprocess.run([build / "decode_bench", schedule, str(runs + 1)],
                         capture_output=True, text=True, check=True, env=env).stdout
    rows = [line.split(",") for line in out.split()]
    return [int(r[1]) for r in rows[1:]], int(rows[0][2])


def main():
    build, runs, traces = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3:]
    raw = []
    print("trace,decoder,nodes,sim_ms,sim_ci,replay_ms,replay_ci,replay_ns_per_node,share")
    for trace in traces:
        for label, suffix, decoder in CONFIGS:
            schedule = build / f"{trace}.{suffix}.bin"
            env = dict(os.environ, LOGGOPSIM_DECODER=decoder)
            simulate(build, schedule, env)
            sim = [simulate(build, schedule, env) for _ in range(runs)]
            rep, nodes = replay(build, schedule, runs, env)
            raw += [(trace, label, "sim", i, v) for i, v in enumerate(sim)]
            raw += [(trace, label, "replay", i, v) for i, v in enumerate(rep)]
            sm, sl, sh = median_ci(sim)
            rm, rl, rh = median_ci(rep)
            print(f"{trace},{label},{nodes},{sm/1e6:.1f},[{sl/1e6:.1f};{sh/1e6:.1f}],"
                  f"{rm/1e6:.1f},[{rl/1e6:.1f};{rh/1e6:.1f}],{rm/nodes:.1f},{rm/sm:.2f}", flush=True)
    with open(build / "decode_share.csv", "w", newline="") as f:
        csv.writer(f).writerows([("trace", "decoder", "kind", "run", "ns")] + raw)


if __name__ == "__main__":
    main()
