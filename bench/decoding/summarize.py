#!/usr/bin/env python3
"""Tables of decode_bench results: per benchmark kind, one row per trace and one column per runner,
with the fastest runner of each row marked.

usage: summarize.py [--exclude RUNNER]... RESULTS.json

RESULTS.json is decode_bench's JSON output (--benchmark_out=RESULTS.json --benchmark_out_format=json).
With repetitions, the medians are used. Setup and ColdStart show the time per run; the other kinds
show the time per node. A final table lists the fastest runner for every kind and trace; runners
within 3% of the fastest count as tied (a=b), since smaller differences are within the noise.
--exclude leaves a runner out of the ranking (it is still shown), for example templated_schema,
which only exists for the benchmark traces themselves.
"""

import argparse
import json
from collections import defaultdict

PER_RUN = {"Setup", "ColdStart"}
TIE = 1.03
KINDS = ["Setup", "ColdStart", "ReadNodes", "RandomReads", "Replay"]


def load(path):
    """{(kind, trace): {runner: (value, unit)}} from the medians, or the single runs without repetitions."""
    rows = json.load(open(path))["benchmarks"]
    has_medians = any(row.get("aggregate_name") == "median" for row in rows)
    results = defaultdict(dict)
    for row in rows:
        if row.get("error_occurred") or (has_medians and row.get("aggregate_name") != "median"):
            continue
        if not has_medians and row.get("run_type") != "iteration":
            continue
        kind, trace, runner = row["run_name"].split("/")[:3]
        if kind in PER_RUN:
            results[(kind, trace)][runner] = (row["real_time"], row["time_unit"])
        else:
            results[(kind, trace)][runner] = (row["time_per_node"] * 1e9, "ns/node")
    return results


def format_value(value):
    return f"{value:.3g}" if value < 100 else f"{value:.0f}"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exclude", action="append", default=[], help="runner to leave out of the ranking")
    parser.add_argument("results")
    args = parser.parse_args()
    excluded = {"legacy", *args.exclude} # the legacy format is a reference, not a runner to choose
    results = load(args.results)
    runners = []
    for values in results.values():
        runners += [runner for runner in values if runner not in runners]
    traces = sorted({trace for _, trace in results})
    winners = defaultdict(dict)
    for kind in KINDS:
        kind_traces = [trace for trace in traces if (kind, trace) in results]
        if not kind_traces:
            continue
        columns = [runner for runner in runners if any(runner in results[(kind, trace)] for trace in kind_traces)]
        unit = next(iter(results[(kind, kind_traces[0])].values()))[1]
        print(f"\n{kind} ({unit}; * fastest" + (f", not counting {', '.join(args.exclude)}" if args.exclude else "") + ")")
        width = max(len(trace) for trace in kind_traces)
        print(f"{'':{width}}  " + "  ".join(f"{runner:>16}" for runner in columns))
        for trace in kind_traces:
            values = results[(kind, trace)]
            candidates = {runner: value for runner, (value, _) in values.items() if runner not in excluded}
            fastest = min(candidates.values())
            best = [runner for runner in columns if runner in candidates and candidates[runner] <= fastest * TIE]
            winners[kind][trace] = "=".join(best)
            cells = []
            for runner in columns:
                cell = format_value(values[runner][0]) + ("*" if runner in best else " ") if runner in values else "-"
                cells.append(f"{cell:>16}")
            print(f"{trace:{width}}  " + "  ".join(cells))
    print("\nFastest runner")
    kinds = [kind for kind in KINDS if kind in winners]
    width = max(len(trace) for trace in traces)
    print(f"{'':{width}}  " + "  ".join(f"{kind:>26}" for kind in kinds))
    for trace in traces:
        print(f"{trace:{width}}  " + "  ".join(f"{winners[kind].get(trace, '-'):>26}" for kind in kinds))


if __name__ == "__main__":
    main()
