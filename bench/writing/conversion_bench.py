#!/usr/bin/env python3
"""Measure complete GOAL-to-binary conversion for the two main benchmark traces.

Uses schedgen inputs, warms both conversion paths, and interleaves formats over five
rounds by default. Times include txt2bin startup, parsing and writing to the OS cache;
schedule generation is excluded. Existing I/O policies are retained: legacy calls
system-wide sync() at finish, while the self-describing writer closes its stream.
"""

import argparse
import json
import random
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
SCENARIOS = {"bcast_64k": ("binomialtreebcast", 65536), "dissem_16k": ("dissemination", 16384)}
FORMATS = {"legacy": "legacy", "self_describing": "self-describing"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be positive")
    build = args.build_dir.resolve()
    output = (args.output or build / "bench/results/writing/conversion.json").resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    fields = REPOSITORY / "tests/data/empty_custom_fields.toml"
    results = {name: {kind: {"samples_ms": []} for kind in FORMATS} for name in SCENARIOS}
    with tempfile.TemporaryDirectory(prefix="conversion-", dir=output.parent) as directory:
        scratch = Path(directory)
        inputs = {}
        for name, (pattern, ranks) in SCENARIOS.items():
            inputs[name] = scratch / f"{name}.goal"
            subprocess.run([build / "schedgen", "--ptrn", pattern, "--commsize", str(ranks),
                            "-o", inputs[name]], check=True, stdout=subprocess.DEVNULL)

        def convert(name, kind):
            destination = scratch / f"{name}.{kind}.bin"
            start = time.perf_counter_ns()
            subprocess.run([build / "txt2bin", "-i", inputs[name], "-o", destination,
                            "--format", FORMATS[kind], "--custom-fields", fields],
                           check=True, stdout=subprocess.DEVNULL)
            elapsed_ms = (time.perf_counter_ns() - start) / 1e6
            return elapsed_ms, destination.stat().st_size

        cases = [(name, kind) for name in SCENARIOS for kind in FORMATS]
        for name, kind in cases:
            _, size = convert(name, kind)
            results[name][kind]["output_bytes"] = size
        randomizer = random.Random(20261107)
        for _ in range(args.repetitions):
            order = cases.copy()
            randomizer.shuffle(order)
            for name, kind in order:
                elapsed_ms, size = convert(name, kind)
                if size != results[name][kind]["output_bytes"]:
                    raise RuntimeError(f"Output size changed for {name}/{kind}")
                results[name][kind]["samples_ms"].append(elapsed_ms)
        for name in SCENARIOS:
            for kind in FORMATS:
                result = results[name][kind]
                result["median_ms"] = statistics.median(result["samples_ms"])
                print(f"{name}/{kind}: {result['median_ms']:.1f} ms; {result['output_bytes']} bytes")
    output.write_text(json.dumps({"repetitions": args.repetitions,
                                 "scope": "whole txt2bin subprocess; warm input cache; legacy sync() and stream-close policies retained",
                                 "order": "seeded interleaving after one warm-up per case",
                                 "results": results}, indent=2) + "\n")
    print(f"Saved {output}")


if __name__ == "__main__":
    main()
