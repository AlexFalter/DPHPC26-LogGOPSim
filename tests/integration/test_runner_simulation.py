"""Correctness of complete simulations for each decoder, separate from bench/ timings."""

import os
import re

import pytest

from helpers import host_times, run
from test_simulation_equivalence import ping_pong


def simulate_with_runner(executable, trace, runner, tmp_path, *options, require_orc=False):
    result = run([executable, "-f", trace, *options], env={
        **os.environ, "LOGGOPSIM_DECODER": runner, "LOGGOPSIM_TRACE_DECODER": "1",
        "LOGGOPSIM_JIT_CACHE": str(tmp_path / "jit-cache")})
    actual = re.findall(r"^Trace decoder: (\w+)$", result.stderr, re.MULTILINE)
    assert len(actual) == 1, result.stderr
    if actual[0] != runner:
        if runner == "orc" and actual == ["plan"] and not require_orc:
            pytest.skip("LLVM ORC is not available; simulator reported the plan fallback")
        pytest.fail(f"requested {runner}, but the simulator used {actual[0]}: {result.stderr}")
    return result


@pytest.mark.parametrize("runner", ["plan", "jit", "orc", "templated_node"])
@pytest.mark.parametrize("delay", [0, 1000000], ids=["receive first", "message first"])
@pytest.mark.parametrize("threshold", [1, 65535], ids=["rendezvous", "eager"])
def test_runners_reproduce_legacy_simulation(runner, delay, threshold, converter, loggopsim, tmp_path, request):
    text = ping_pong(delay)
    legacy = converter.convert_text(text, tmp_path, "legacy.bin", format="legacy")
    packed = converter.convert_text(text, tmp_path, "packed.bin", format="self-describing")
    expected = host_times(run([loggopsim, "-f", legacy, "-S", threshold]).stdout)
    assert len(expected) == 3
    original = packed.read_bytes()
    result = simulate_with_runner(loggopsim, packed, runner, tmp_path, "-S", threshold,
                                  require_orc=request.config.getoption("--require-orc"))
    assert host_times(result.stdout) == expected
    assert packed.read_bytes() == original


def test_templated_schema_reproduces_legacy_broadcast(schedgen, converter, loggopsim, tmp_path):
    # This is an existing production TemplatedSchemaRunner instantiation. Generate it independently;
    # correctness must not depend on bench_traces having been built.
    goal = tmp_path / "bcast.goal"
    run([schedgen, "--ptrn", "binomialtreebcast", "--commsize", "65536", "-o", goal], cwd=tmp_path)
    paths = []
    for format in ["legacy", "self-describing"]:
        trace = tmp_path / f"{format}.bin"
        converter(goal, trace, format=format)
        paths.append(trace)
    output = run([loggopsim, "-f", paths[0]], timeout=60).stdout
    # For more than sixteen ranks the CLI reports the slowest host, not every host.
    maximum = re.findall(r"^Maximum finishing time at host (\d+): (\d+) ", output, re.MULTILINE)
    assert len(maximum) == 1, output
    original = paths[1].read_bytes()
    result = simulate_with_runner(loggopsim, paths[1], "templated_schema", tmp_path)
    assert re.findall(r"^Maximum finishing time at host (\d+): (\d+) ", result.stdout, re.MULTILINE) == maximum
    assert paths[1].read_bytes() == original
