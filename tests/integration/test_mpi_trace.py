"""Validate fresh per-rank traces from both liballprof language wrappers."""

import re

import pytest

from helpers import existing, run

pytestmark = pytest.mark.mpi


@pytest.mark.parametrize("language", ["c", "f"])
def test_mpi_wrapper_records_every_rank(language, mpi_exec, build_dir, request, tmp_path):
    executable = existing(build_dir / f"test_lap2_{language}", f"build the test_lap2_{language} target")
    command = [mpi_exec, request.config.getoption("--mpi-numproc-flag"), "4",
               *request.config.getoption("--mpi-preflag"), executable,
               *request.config.getoption("--mpi-postflag")]
    run(command, cwd=tmp_path, timeout=60)
    assert {p.name for p in tmp_path.glob("lap2-trace-*.txt")} == {
        f"lap2-trace-rank-{rank}-of-4.txt" for rank in range(4)}
    for rank in range(4):
        trace = (tmp_path / f"lap2-trace-rank-{rank}-of-4.txt").read_text()
        calls = re.findall(r"^(MPI_\w+):", trace, re.MULTILINE)
        assert calls[0] == "MPI_Init" and calls[-1] == "MPI_Finalize", trace
        expected_calls = {"MPI_Init", "MPI_Comm_rank", "MPI_Comm_size", "MPI_Allreduce", "MPI_Finalize"}
        if language == "c":
            expected_calls |= {"MPI_Isend", "MPI_Irecv", "MPI_Waitall"}
        assert expected_calls <= set(calls), trace
        rank_call = next(line for line in trace.splitlines() if line.startswith("MPI_Comm_rank:"))
        assert int(rank_call.split(":")[3]) == rank, rank_call
