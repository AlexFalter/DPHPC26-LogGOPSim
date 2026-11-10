"""The saved legacy binary and a fresh conversion of its schedule give the saved host times."""

import json

import pytest

from helpers import host_times, run


@pytest.mark.parametrize("trace", ["saved legacy binary", "current conversion"])
def test_matches_saved_host_times(trace, reference_case, converter, loggopsim, tmp_path):
    metadata = json.loads((reference_case / "metadata.json").read_text())
    expected = json.loads((reference_case / "expected_times.json").read_text())
    if trace == "saved legacy binary":
        # Never run on the preserved file: simulator options may modify a trace, and the
        # legacy reader opens even private mappings read-write.
        binary = tmp_path / "legacy.bin"
        binary.write_bytes((reference_case / "legacy.bin").read_bytes())
    else:
        binary = tmp_path / "converted.bin"
        converter(reference_case / "schedule.goal", binary)
    output = run([loggopsim, "-f", binary, *metadata["simulator_args"]]).stdout
    assert host_times(output) == expected
