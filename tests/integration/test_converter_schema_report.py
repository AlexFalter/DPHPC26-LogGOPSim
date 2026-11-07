"""The converter's --report-schema statistics and its range checks, through the command line."""

import re

import pytest

from helpers import run

SCHEDULE = """num_ranks 2
rank 0 {
l1: calc 1000 cpu 2
l2: send 8b to 1 tag 42 cpu 2 nic 1
l2 requires l1
}
rank 1 {
l1: recv 8b from -1 tag -1 cpu 1 nic 1
l2: calc 2000
l2 irequires l1
}
"""


def convert(converter, directory, text, report=True, name="output"):
    source = directory / f"{name}.goal"
    source.write_text(text)
    binary = directory / f"{name}.bin"
    result = converter(source, binary, *(["--report-schema"] if report else []), check=False)
    return result, binary


def report_rows(output):
    """name -> (range, wildcard, base, bits, offset) for every field row of the report."""
    rows = {}
    for line in output.splitlines():
        match = re.fullmatch(r"(\w+)\s+(\d+\.\.\d+|none)\s+(yes|no)\s+(\d+)\s+(\d+)\s+(\d+)", line)
        if match:
            name, numeric_range, wildcard, base, bits, offset = match.groups()
            rows[name] = (numeric_range, wildcard, int(base), int(bits), int(offset))
    return rows


def test_report_leaves_the_legacy_binary_unchanged(converter, tmp_path):
    plain, binary = convert(converter, tmp_path, SCHEDULE, report=False)
    assert plain.returncode == 0, plain.stderr
    original = binary.read_bytes()
    reported, binary = convert(converter, tmp_path, SCHEDULE)
    assert reported.returncode == 0, reported.stderr
    assert binary.read_bytes() == original


def test_report_lists_ranges_widths_and_offsets(converter, tmp_path):
    reported, _ = convert(converter, tmp_path, SCHEDULE)
    assert reported.returncode == 0, reported.stderr
    # (range, wildcard, base, bits); fields are packed in this order from offset 0.
    expected = {
        "depcnt": ("0..1", "no", 0, 1), "type": ("1..3", "no", 1, 2),
        "peer": ("1..1", "yes", 0, 2), "size": ("8..2000", "no", 8, 11),
        "tag": ("42..42", "yes", 42, 1), "cpu": ("0..2", "no", 0, 2),
        "nic": ("0..1", "no", 0, 1), "nDep": ("0..1", "no", 0, 1),
        "depIx": ("0..1", "no", 0, 1), "nSDep": ("0..1", "no", 0, 1),
        "sDepIx": ("0..1", "no", 0, 1),
    }
    offsets = [0]
    for row in expected.values():
        offsets.append(offsets[-1] + row[3])
    assert report_rows(reported.stdout) == {
        name: (*row, offset) for (name, row), offset in zip(expected.items(), offsets)}
    assert "Ranks: 2; nodes: 4; dependency edges: 2" in reported.stdout
    assert "Proposed node record: 24 bits / 3 bytes" in reported.stdout
    assert "Proposed root/dependency node indices: 1 bits each, shared file-wide; base 0" in reported.stdout


@pytest.mark.parametrize("report", [False, True], ids=["plain", "report"])
@pytest.mark.parametrize("operation, diagnostic", [
    ("send 1b to 1 tag 4294967296", "tag exceeds"),
    ("calc 1 cpu 256", "cpu exceeds"),
    ("send 1b to 1 nic 256", "nic exceeds"),
    ("send 1b to 4294967296", "Peer"),
    ("calc 18446744073709551616", "uint64_t"),
], ids=["tag", "cpu", "nic", "peer", "size"])
def test_rejects_values_beyond_the_legacy_ranges(converter, tmp_path, operation, diagnostic, report):
    result, _ = convert(converter, tmp_path, SCHEDULE.replace("calc 1000 cpu 2", operation), report)
    assert result.returncode != 0
    assert diagnostic in result.stderr


# Legal boundary values survive parsing without narrowing early.
def test_keeps_boundary_values(converter, tmp_path):
    boundary = (SCHEDULE.replace("calc 1000 cpu 2", "calc 18446744073709551615 cpu 255")
                .replace("tag 42 cpu 2 nic 1", "tag 4294967294 cpu 255 nic 255"))
    result, _ = convert(converter, tmp_path, boundary)
    assert result.returncode == 0, result.stderr
    assert re.search(r"^size\s+8\.\.18446744073709551615\s+no\s+8\s+64\s+", result.stdout, re.M), result.stdout
    assert re.search(r"^tag\s+4294967294\.\.4294967294\s+yes\s+4294967294\s+1\s+", result.stdout, re.M), result.stdout


@pytest.mark.parametrize("text, index_bits", [
    ("num_ranks 1\nrank 0 {\n}\n", 1),
    ("num_ranks 2\nrank 0 {\n" + "".join(f"l{i}: calc 1\n" for i in range(1, 10)) + "}\nrank 1 {\nl1: calc 1\n}\n", 4),
], ids=["empty rank", "nine nodes"])
def test_sizes_indices_for_the_largest_rank(converter, tmp_path, text, index_bits):
    result, _ = convert(converter, tmp_path, text)
    assert result.returncode == 0, result.stderr
    assert f"Proposed root/dependency node indices: {index_bits} bits each" in result.stdout


# Without --custom-fields, the converter loads the project configuration, also from another
# working directory. An empty rank works with any project declarations.
def test_uses_the_project_configuration_by_default(converter, tmp_path):
    source = tmp_path / "default_config.goal"
    source.write_text("num_ranks 1\nrank 0 {\n}\n")
    result = run([converter.executable, "-i", source, "-o", tmp_path / "default_config.bin", "--report-schema"],
                 cwd=tmp_path)
    assert "nodes: 0" in result.stdout


def test_requires_an_explicit_custom_field_file_to_exist(converter, tmp_path):
    source = tmp_path / "input.goal"
    source.write_text("num_ranks 1\nrank 0 {\n}\n")
    result = converter(source, tmp_path / "missing.bin", fields=tmp_path / "absent.toml", check=False)
    assert result.returncode != 0
    assert "Cannot open custom fields TOML" in result.stderr
