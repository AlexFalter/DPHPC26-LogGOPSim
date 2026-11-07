"""Running the built programs and reading their output, shared by the integration tests."""

import re
import subprocess
from pathlib import Path

import pytest


def run(command, *, cwd=None, env=None, timeout=30, check=True):
    """Runs a command and captures its output. With check, a nonzero exit fails the test."""
    result = subprocess.run([str(part) for part in command], cwd=cwd, env=env, text=True,
                            capture_output=True, timeout=timeout)
    if check:
        assert result.returncode == 0, (
            f"{command} exited {result.returncode}\n{result.stdout}{result.stderr}")
    return result


def existing(path, hint, *, required=True):
    """Missing required inputs fail; unavailable optional integrations may skip."""
    if not path.exists():
        message = f"{path} is missing; {hint}"
        if required:
            pytest.fail(message, pytrace=False)
        pytest.skip(message)
    return path


def host_times(output):
    """The finish time of every host, as printed by LogGOPSim and ATLAHS: {"0": "1234", ...}."""
    return dict(re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", output, re.MULTILINE))


def is_operation(line):
    return re.match(r"l\d+:", line) is not None


def append_to_operations(text, attributes):
    """Appends the attributes, a string or a function of the line, to every operation."""
    def extend(line):
        if not is_operation(line):
            return line
        return line + (attributes(line) if callable(attributes) else attributes)
    return "\n".join(extend(line) for line in text.splitlines()) + "\n"


class Converter:
    """txt2bin with a custom-field declaration file."""

    def __init__(self, executable, fields):
        self.executable = executable
        self.fields = fields

    def __call__(self, source, output, *options, format=None, fields=None, check=True, cwd=None):
        command = [self.executable, "-i", source, "-o", output]
        if format:
            command += ["--format", format]
        command += [*options, "--custom-fields", fields or self.fields]
        return run(command, check=check, cwd=cwd)

    def convert_text(self, text, directory, name, **options):
        """Writes the GOAL text next to the output and converts it; returns the output path."""
        source = Path(directory) / f"{name}.goal"
        source.write_text(text)
        output = Path(directory) / name
        self(source, output, **options)
        return output
