"""ATLAHS's own NDP regression test, with its unchanged stored reference output, on our htsim build."""

import contextlib
import importlib.util
import io

import pytest

from helpers import existing

pytestmark = pytest.mark.atlahs


def test_ndp_matches_the_upstream_reference(htsim_dir, monkeypatch):
    directory = htsim_dir / "tests"
    existing(directory / "htsim_dumbell_ndp", "build the atlahs_htsim target")
    references = list((directory / "htsim-tests" / "dumbell_ndp").glob("*.ref"))
    assert references, "the upstream NDP reference files are missing"

    spec = importlib.util.spec_from_file_location("atlahs_upstream_tests", directory / "tests.py")
    upstream = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(upstream)
    monkeypatch.chdir(directory)
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        upstream.run_test("dumbell_ndp")
    result = output.getvalue()
    # The upstream runner prints failures but does not return them.
    assert "Test failed" not in result, result
    assert result.count("Test passed\n") == len(references), result
