"""Recorded Nsight traces through the pristine upstream pipeline and through ours.

Unlike the emitter fixtures, this runs get_traced_events.py on real SQLite exports and compares
with an independently built pinned upstream backend (the atlahs_reference target). Logs and a
result.json (including each stage's wall-clock time) are kept in a new run directory under
build/atlahs-recorded-run, also on failure.
Needs --recorded-traces; see src/LogGOPSim/simulators/atlahs/README.md.
"""

import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from collections import Counter
from pathlib import Path

import pytest

from helpers import existing

pytestmark = [pytest.mark.atlahs, pytest.mark.recorded]

COMMAND_TIMEOUT = 1200


def digest(path):
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


@pytest.fixture
def traces(request):
    directory = request.config.getoption("--recorded-traces")
    if directory is None:
        pytest.skip("pass --recorded-traces with Nsight SQLite exports")
    inputs = sorted(directory.resolve().glob("*.sqlite"))
    assert inputs, f"{directory} has no Nsight SQLite exports"
    return inputs


def test_explicit_fields_match_the_upstream_pipeline(request, traces, build_dir, htsim, htsim_dir, atlahs_generator,
                                                     atlahs_fields, converter):
    reference = existing(build_dir / "atlahs-reference", "build the atlahs_reference target")
    topology = htsim_dir / "datacenter" / "topologies" / "leaf_spine_16_1os.topo"
    nodes = request.config.getoption("--recorded-nodes")
    runs = build_dir / "atlahs-recorded-run"
    runs.mkdir(parents=True, exist_ok=True)
    run_directory = Path(tempfile.mkdtemp(prefix="run-", dir=runs))
    print(f"Run directory: {run_directory}")
    result = {"status": "running", "inputs": {path.name: digest(path) for path in traces},
              "upstream_revision": (reference / "revision.txt").read_text().strip(),
              "topology_sha256": digest(topology), "nodes": nodes, "commands": []}

    def run(stage, command, cwd):
        command = [str(part) for part in command]
        entry = {"stage": stage, "argv": command, "cwd": str(cwd)}
        result["commands"].append(entry)
        log = run_directory / f"{stage}.log"
        start = time.perf_counter()
        with log.open("w") as output:
            process = subprocess.run(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT,
                                     env={**os.environ, "ATLAHS_TRACE_MATCHES": "1"}, timeout=COMMAND_TIMEOUT)
        entry["seconds"] = round(time.perf_counter() - start, 3) # wall-clock time of the stage
        print(f"{stage}: {entry['seconds']:.1f} s")
        assert process.returncode == 0, f"{stage} exited {process.returncode}; see {log}"
        return log

    try:
        source = reference / "source"
        pipelines = {
            "legacy": dict(generator=source / "goal_gen/ai/nccl_goal_generator", option="--unique-nic",
                           converter=source / "sim/LogGOPSim/txt2bin",
                           simulator=source / "sim/htsim-backend/sim/datacenter/htsim_ndp"),
            "explicit": dict(generator=atlahs_generator, option="--explicit-fields",
                             converter=converter.executable, simulator=htsim),
        }
        outputs = {}
        for mode, pipeline in pipelines.items():
            directory = run_directory / mode
            run(f"generate-{mode}", [sys.executable, pipeline["generator"] / "get_traced_events.py",
                                     "-i", traces[0].parent, "-o", directory, pipeline["option"], "--zero-red-copy",
                                     "-q"], pipeline["generator"])
            goal = directory / "InterNode_MicroEvents_Dependency.goal"
            ranks = sends = receives = 0
            with goal.open() as stream:
                for line in stream:
                    if line.startswith("num_ranks "):
                        ranks = int(line.split()[1])
                    sends += bool(re.match(r"l\d+: send ", line))
                    receives += bool(re.match(r"l\d+: recv ", line))
            assert ranks and sends and sends == receives, f"invalid {mode} graph: {ranks=}, {sends=}, {receives=}"

            binary = directory / "trace.bin"
            command = [pipeline["converter"], "-i", goal, "-o", binary]
            if mode == "explicit":
                command += ["--format", "self-describing", "--custom-fields", atlahs_fields]
            run(f"convert-{mode}", command, directory)
            before = digest(binary)
            log = run(f"simulate-{mode}", [pipeline["simulator"], "-goal", binary, "-topo", topology, "-nodes", nodes,
                                           "-strat", "ecmp_host", "-paths", "128", "-q", "1000000", "-seed", "4"],
                      directory)
            text = log.read_text()
            times = dict(re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", text, re.MULTILINE))
            # Upstream prints progress dots without newlines, so a match can follow a dot or bar
            # on the same line; match the marker itself.
            pairs = Counter(tuple(map(int, match)) for match in re.findall(
                r"\bATLAHS_MATCH (\d+) (\d+) (\d+) (\d+)[ \t]*$", text, re.MULTILINE))
            assert (len(times) == ranks and "LGS terminates!" in text and "queue on host" not in text and
                    sum(pairs.values()) == sends and text.count("ATLAHS_MATCH") == sends), (
                f"incomplete {mode} simulation: {len(times)} rank times, {sum(pairs.values())}/{sends} "
                f"matched messages; see {log}")
            assert digest(binary) == before, f"the {mode} simulation changed its input binary"
            outputs[mode] = (times, pairs)
            result[mode] = {"ranks": ranks, "messages": sends, "finish_times": times, "goal_sha256": digest(goal),
                            "binary_sha256": before, "simulator_sha256": digest(pipeline["simulator"]),
                            "converter_sha256": digest(pipeline["converter"]),
                            "pairs_sha256": hashlib.sha256(repr(sorted(pairs.items())).encode()).hexdigest()}
        assert outputs["explicit"][1] == outputs["legacy"][1], "explicit-field matching differs from upstream"
        # Finish times are recorded in result.json but not compared: the explicit fields route each
        # message to its partner GPU and schedule by stream, where upstream reuses the sender's GPU
        # index, so the times legitimately differ (by about 0.02% on the Llama recording).
        result["status"] = "passed"
    except BaseException as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        (run_directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
