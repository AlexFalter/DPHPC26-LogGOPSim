#!/usr/bin/env python3
"""Profiles a running command and lists the functions it spends its time in.

usage: profile.py [--seconds S] [--delay S] [--top N] [--output DIR] -- COMMAND [ARGS...]

Starts COMMAND, waits --delay seconds to skip its startup, then samples it for --seconds with
the system profiler: `sample` on macOS (no Xcode needed), `perf` on Linux. Prints the functions
with the most samples at the top of the stack (their own time, without callees) and keeps the
raw profile, which also has the full call tree, in --output (default build/bench/profiles).

To profile one benchmark, let it run longer than the sampling:
  profile.py -- build/bench/decode_bench --benchmark_filter='^ReadNodes/bcast_64k/plan$' --benchmark_min_time=30s
"""

import argparse
import platform
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]


def sample_macos(pid, seconds, output):
    subprocess.run(["sample", str(pid), str(seconds), "1", "-mayDie", "-file", str(output)],
                   check=True, capture_output=True)
    return parse_sample(output.read_text())


def parse_sample(text):
    """(samples, function) pairs from the "Sort by top of stack" section of `sample`'s report."""
    section = text.split("Sort by top of stack, same collapsed", 1)
    if len(section) < 2:
        return []
    rows = []
    for line in section[1].splitlines()[1:]:
        match = re.match(r"\s+(.+?)\s+\(in ([^)]+)\)\s+(\d+)\s*$", line)
        if not match:
            if rows and not line.strip():
                break
            continue
        function, binary, samples = match.groups()
        rows.append((int(samples), f"{shorten(function)}  [{binary}]"))
    return rows


def sample_linux(pid, seconds, output):
    data = output.with_suffix(".data")
    subprocess.run(["perf", "record", "-F", "999", "-g", "-p", str(pid), "-o", str(data), "--", "sleep", str(seconds)],
                   check=True, capture_output=True)
    report = subprocess.run(["perf", "report", "-i", str(data), "--stdio", "--no-children", "--sort", "symbol"],
                            check=True, capture_output=True, text=True).stdout
    output.write_text(report)
    rows = []
    for line in report.splitlines():
        match = re.match(r"\s+([\d.]+)%\s+\S+\s+\[\.\]\s+(.+)$", line)
        if match:
            rows.append((float(match[1]), shorten(match[2])))
    return rows


def shorten(function):
    """Drops parameter lists and ABI tags, which make C++ names hard to read."""
    function = re.sub(r"\[abi:[^\]]*\]", "", function)
    depth, result = 0, []
    for character in function:
        if character == "(":
            depth += 1
            if depth == 1:
                result.append("(…)")
        elif character == ")":
            depth -= 1
        elif depth == 0:
            result.append(character)
    return "".join(result).replace("(…) const", "(…)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=float, default=5, help="sampling time (default 5)")
    parser.add_argument("--delay", type=float, default=1, help="seconds before sampling starts (default 1)")
    parser.add_argument("--top", type=int, default=25, help="functions to list (default 25)")
    parser.add_argument("--output", type=Path, default=REPOSITORY / "build" / "bench" / "profiles")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="-- COMMAND [ARGS...]")
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("give the command to profile after --")

    system = platform.system()
    if system == "Darwin":
        profiler = sample_macos
    elif system == "Linux" and shutil.which("perf"):
        profiler = sample_linux
    else:
        parser.error(f"no supported profiler on {system} (macOS sample or Linux perf)")
    args.output.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    output = args.output / f"{stamp}-{Path(command[0]).name}.txt"

    process = subprocess.Popen(command, stdout=subprocess.DEVNULL)
    try:
        time.sleep(args.delay)
        if process.poll() is not None:
            sys.exit(f"{command[0]} exited before sampling started; let it run longer")
        rows = profiler(process.pid, args.seconds, output)
    finally:
        process.terminate()
        process.wait()

    total = sum(samples for samples, _ in rows)
    print(f"Raw profile: {output}")
    if not total:
        sys.exit("No samples recorded")
    print(f"{'self':>7}  function  (own time, without callees)")
    for samples, function in sorted(rows, reverse=True)[:args.top]:
        share = samples if system == "Linux" else 100 * samples / total
        print(f"{share:6.1f}%  {function}")


if __name__ == "__main__":
    main()
