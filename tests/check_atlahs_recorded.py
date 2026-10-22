"""Compare recorded Nsight input through pristine and explicit-field ATLAHS paths.

Unlike the small emitter fixtures, this executes get_traced_events.py on SQLite
exports and runs an independently built pinned backend. Logs and a JSON result
are retained, including on failure. No generated operations are substituted.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("traces", "generator", "reference", "converter", "atlahs-fields",
                 "simulator", "topology", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--nodes", type=int, required=True)
    parser.add_argument("--timeout", type=int, default=1200)
    args = parser.parse_args()
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    args.output = Path(tempfile.mkdtemp(prefix="run-", dir=args.output))
    print(f"Evidence directory: {args.output}", flush=True)
    inputs = sorted(args.traces.glob("*.sqlite"))
    if not inputs:
        parser.error("--traces must contain actual Nsight SQLite exports")
    result = {"status": "running", "inputs": {p.name: digest(p) for p in inputs},
              "upstream_revision": (args.reference / "revision.txt").read_text().strip(),
              "topology_sha256": digest(args.topology), "nodes": args.nodes,
              "commands": []}

    def run(name, command, cwd):
        command = [str(part) for part in command]
        result["commands"].append({"stage": name, "argv": command, "cwd": str(cwd)})
        print(f"Running {name}", flush=True)
        log = args.output / f"{name}.log"
        with log.open("w") as output:
            process = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                     env={**os.environ, "ATLAHS_TRACE_MATCHES": "1"},
                                     timeout=args.timeout)
        if process.returncode:
            raise RuntimeError(f"{name} exited {process.returncode}; see {log}")
        return log

    try:
        source = args.reference / "source"
        baseline_generator = source / "goal_gen/ai/nccl_goal_generator"
        baseline_simulator = source / "sim/htsim-backend/sim/datacenter/htsim_ndp"
        baseline_converter = source / "sim/LogGOPSim/txt2bin"
        outputs = {}
        for mode, generator, option in (("legacy", baseline_generator, "--unique-nic"),
                                        ("explicit", args.generator, "--explicit-fields")):
            directory = args.output / mode
            run(f"generate-{mode}", [sys.executable, generator / "get_traced_events.py",
                "-i", args.traces, "-o", directory, option, "--zero-red-copy", "-q"], generator)
            goal = directory / "InterNode_MicroEvents_Dependency.goal"
            ranks = sends = receives = 0
            with goal.open() as stream:
                for line in stream:
                    if line.startswith("num_ranks "):
                        ranks = int(line.split()[1])
                    sends += bool(re.match(r"l\d+: send ", line))
                    receives += bool(re.match(r"l\d+: recv ", line))
            if not ranks or not sends or sends != receives:
                raise RuntimeError(f"Invalid {mode} graph: {ranks=}, {sends=}, {receives=}")
            binary = directory / "trace.bin"
            converter = baseline_converter if mode == "legacy" else args.converter
            command = [converter, "-i", goal, "-o", binary]
            if mode == "explicit":
                command += ["--format", "self-describing", "--custom-fields", args.atlahs_fields]
            run(f"convert-{mode}", command, directory)
            before = digest(binary)
            simulator = baseline_simulator if mode == "legacy" else args.simulator
            log = run(f"simulate-{mode}", [simulator, "-goal", binary, "-topo", args.topology,
                "-nodes", args.nodes, "-strat", "ecmp_host", "-paths", "128",
                "-q", "1000000", "-seed", "4"], directory)
            text = log.read_text()
            times = dict(re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", text, re.MULTILINE))
            # Upstream prints progress dots without newlines, so an observation
            # can follow a dot or bar on the same line. Match the marker itself.
            pairs = Counter(tuple(map(int, match)) for match in re.findall(
                r"\bATLAHS_MATCH (\d+) (\d+) (\d+) (\d+)[ \t]*$", text, re.MULTILINE))
            if (len(times) != ranks or "LGS terminates!" not in text or
                    "queue on host" in text or sum(pairs.values()) != sends or
                    text.count("ATLAHS_MATCH") != sends):
                raise RuntimeError(f"Incomplete {mode} simulation: {len(times)} rank times, "
                                   f"{sum(pairs.values())}/{sends} matched messages; see {log}")
            if digest(binary) != before:
                raise RuntimeError(f"{mode} simulation changed its input binary")
            outputs[mode] = (times, pairs)
            result[mode] = {"ranks": ranks, "messages": sends, "finish_times": times,
                            "goal_sha256": digest(goal), "binary_sha256": before,
                            "simulator_sha256": digest(simulator),
                            "converter_sha256": digest(converter),
                            "pairs_sha256": hashlib.sha256(
                                repr(sorted(pairs.items())).encode()).hexdigest()}
        if outputs["legacy"][1] != outputs["explicit"][1]:
            raise RuntimeError("Explicit-field matching differs from the independent upstream reference")
        if outputs["legacy"][0] != outputs["explicit"][0]:
            raise RuntimeError("Explicit-field finish times differ from the independent upstream reference")
        result["status"] = "passed"
        print(f"PASS: recorded traces → generator CLI → converter → separate backends; "
              f"{result['explicit']['messages']} message pairs and all finish times agree")
    except Exception as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
