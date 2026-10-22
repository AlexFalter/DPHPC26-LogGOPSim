"""Compare legacy reading and current conversion against saved host times."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--simulator", required=True)
    parser.add_argument("--converter", required=True)
    parser.add_argument("--plain-fields", required=True, help="Custom-field TOML for plain traces")
    args = parser.parse_args()
    case = Path(__file__).parent / "reference" / "binomialtreebcast_8"
    metadata = json.loads((case / "metadata.json").read_text())
    expected = json.loads((case / "expected_times.json").read_text())

    # Never run against the preserved binary directly: simulator options may
    # modify a trace, and the legacy reader opens even private mappings as r+.
    with tempfile.TemporaryDirectory(prefix="loggopsim-reference-") as directory:
        directory = Path(directory)
        legacy = directory / "legacy.bin"
        legacy.write_bytes((case / "legacy.bin").read_bytes())
        converted = directory / "converted.bin"
        run([args.converter, "-i", str(case / "schedule.goal"), "-o", str(converted),
             "--custom-fields", args.plain_fields])
        for name, trace in [("saved legacy binary", legacy), ("current converter", converted)]:
            output = run([args.simulator, "-f", str(trace)] + metadata["simulator_args"])
            rows = re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", output, re.MULTILINE)
            actual = dict(rows)
            if len(rows) != len(expected) or actual != expected:
                raise RuntimeError(
                    "{} differs from reference\nExpected: {}\nActual: {}\n{}".format(
                        name, expected, actual, output
                    )
                )
            print("PASS: {} ({} rank times)".format(name, len(actual)))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
