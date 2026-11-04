"""Reuse ATLAHS's NDP regression test and its unchanged stored reference output."""

import argparse
import contextlib
import importlib.util
import io
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tests-dir", type=Path, required=True)
    args = parser.parse_args()
    directory = args.tests_dir.resolve()
    if not (directory / "htsim_dumbell_ndp").is_file():
        parser.error("Build the atlahs_htsim target before running upstream checks")
    references = list((directory / "htsim-tests/dumbell_ndp").glob("*.ref"))
    if not references:
        parser.error("The upstream NDP reference files are missing")

    spec = importlib.util.spec_from_file_location("atlahs_upstream_tests", directory / "tests.py")
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    os.chdir(directory)
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        upstream.run_test("dumbell_ndp")
    result = output.getvalue()
    print(result, end="")
    # The upstream runner prints failures but does not return a failing exit code.
    if result.count("Test passed\n") != len(references) or "Test failed" in result:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
