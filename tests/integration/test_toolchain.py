"""Each generator/converter/simulator chain owns a fresh temporary directory."""

import sys

import pytest

from conftest import REPOSITORY
from helpers import host_times, run


@pytest.mark.parametrize("generator", ["schedgen", "schedgen2"])
def test_generated_broadcast_converts_and_simulates(generator, schedgen, converter, loggopsim, tmp_path):
    goal = tmp_path / "schedule.goal"
    if generator == "schedgen":
        command = [schedgen, "--commsize", "8", "--ptrn", "binomialtreebcast", "-o", goal]
    else:
        command = [sys.executable, REPOSITORY / "src/Schedgen2/schedgen.py", "bcast", "--output", goal]
    run(command, cwd=tmp_path)
    assert goal.is_file() and "num_ranks" in goal.read_text()
    outputs = []
    for format in ["legacy", "self-describing"]:
        trace = tmp_path / f"{format}.bin"
        converter(goal, trace, format=format)
        times = host_times(run([loggopsim, "-f", trace], cwd=tmp_path).stdout)
        assert len(times) >= 2, "the simulator did not report every rank of a broadcast"
        ranks = int(goal.read_text().split("num_ranks", 1)[1].split()[0])
        assert set(times) == {str(rank) for rank in range(ranks)}
        outputs.append(times)
    assert outputs[0] == outputs[1]
