"""Exercise converter statistics and legacy range checks through the CLI."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def convert(converter, fields, directory, text, report=True):
    source = directory / "input.goal"
    binary = directory / "output.bin"
    source.write_text(text)
    command = [converter, "-i", str(source), "-o", str(binary), "--custom-fields", fields]
    if report:
        command.append("--report-schema")
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    return result, binary


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", required=True)
    parser.add_argument("--plain-fields", required=True, help="Custom-field TOML for plain traces")
    args = parser.parse_args()
    schedule = """num_ranks 2
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
    with tempfile.TemporaryDirectory(prefix="schema-report-") as temporary:
        directory = Path(temporary)
        plain, binary = convert(args.converter, args.plain_fields, directory, schedule, report=False)
        require(plain.returncode == 0, plain.stderr)
        original = binary.read_bytes()
        reported, binary = convert(args.converter, args.plain_fields, directory, schedule)
        require(reported.returncode == 0, reported.stderr)
        require(binary.read_bytes() == original, "Report changed the legacy binary")
        rows = {}
        for line in reported.stdout.splitlines():
            match = re.fullmatch(r"(\w+)\s+(\d+\.\.\d+|none)\s+(yes|no)\s+(\d+)\s+(\d+)\s+(\d+)", line)
            if match:
                name, numeric_range, wildcard, base, bits, offset = match.groups()
                rows[name] = (numeric_range, wildcard, int(base), int(bits), int(offset))
        expected = {
            "depcnt": ("0..1", "no", 0, 1), "type": ("1..3", "no", 1, 2),
            "peer": ("1..1", "yes", 0, 2), "size": ("8..2000", "no", 8, 11),
            "tag": ("42..42", "yes", 42, 1), "cpu": ("0..2", "no", 0, 2),
            "nic": ("0..1", "no", 0, 1), "nDep": ("0..1", "no", 0, 1),
            "depIx": ("0..1", "no", 0, 1), "nSDep": ("0..1", "no", 0, 1),
            "sDepIx": ("0..1", "no", 0, 1),
        }
        require(set(rows) == set(expected), reported.stdout)
        offset = 0
        for name, expected_row in expected.items():
            require(rows[name] == (*expected_row, offset), "Wrong statistics for " + name + "\n" + reported.stdout)
            offset += expected_row[3]
        require("Ranks: 2; nodes: 4; dependency edges: 2" in reported.stdout, reported.stdout)
        require("Proposed node record: 24 bits / 3 bytes" in reported.stdout, reported.stdout)

        require("Proposed root/dependency node indices: 1 bits each, shared file-wide; base 0" in
                reported.stdout, reported.stdout)

        for operation, diagnostic in [
            ("send 1b to 1 tag 4294967296", "tag exceeds"),
            ("calc 1 cpu 256", "cpu exceeds"),
            ("send 1b to 1 nic 256", "nic exceeds"),
            ("send 1b to 4294967296", "Peer"),
            ("calc 18446744073709551616", "uint64_t"),
        ]:
            invalid = schedule.replace("calc 1000 cpu 2", operation)
            for report in [False, True]:
                result, _ = convert(args.converter, args.plain_fields, directory, invalid, report)
                require(result.returncode != 0 and diagnostic in result.stderr, result.stdout + result.stderr)

        # Legal boundary values must survive parsing without narrowing early.
        boundary = schedule.replace("calc 1000 cpu 2", "calc 18446744073709551615 cpu 255")
        boundary = boundary.replace("tag 42 cpu 2 nic 1", "tag 4294967294 cpu 255 nic 255")
        result, _ = convert(args.converter, args.plain_fields, directory, boundary)
        require(result.returncode == 0, result.stderr)
        require(re.search(r"^size\s+8\.\.18446744073709551615\s+no\s+8\s+64\s+", result.stdout, re.M), result.stdout)
        require(re.search(r"^tag\s+4294967294\.\.4294967294\s+yes\s+4294967294\s+1\s+", result.stdout, re.M), result.stdout)

        empty = "num_ranks 1\nrank 0 {\n}\n"
        result, _ = convert(args.converter, args.plain_fields, directory, empty)
        require(result.returncode == 0 and "nodes: 0" in result.stdout, result.stderr)
        require("Proposed root/dependency node indices: 1 bits each" in result.stdout, result.stdout)
        larger = "num_ranks 2\nrank 0 {\n" + "".join(
            f"l{i}: calc 1\n" for i in range(1, 10)) + "}\nrank 1 {\nl1: calc 1\n}\n"
        result, _ = convert(args.converter, args.plain_fields, directory, larger)
        require(result.returncode == 0, result.stderr)
        require("Proposed root/dependency node indices: 4 bits each" in result.stdout, result.stdout)
        # Production converter loads the project configuration, even from another cwd.
        # An empty rank works with any project declarations, including a comment-only file.
        source = directory / "default_config.goal"
        source.write_text("num_ranks 1\nrank 0 {\n}\n")
        command = [args.converter, "-i", str(source),
                   "-o", str(directory / "default_config.bin"), "--report-schema"]
        result = subprocess.run(command, cwd=directory, text=True, capture_output=True, timeout=30)
        require(result.returncode == 0 and "nodes: 0" in result.stdout, result.stdout + result.stderr)
        # An explicitly requested custom-field file must exist.
        missing = subprocess.run([args.converter, "-i", str(source), "-o", str(directory / "missing.bin"),
                                  "--custom-fields", str(directory / "absent.toml")],
                                 text=True, capture_output=True, timeout=30)
        require(missing.returncode != 0 and "Cannot open custom fields TOML" in missing.stderr,
                missing.stdout + missing.stderr)
        print("PASS: schema report, unchanged legacy bytes, empty rank, and overflow diagnostics")


if __name__ == "__main__":
    main()
