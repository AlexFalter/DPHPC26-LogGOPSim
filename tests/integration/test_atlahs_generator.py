"""ATLAHS's NCCL event emitter: legacy output unchanged from upstream, explicit message fields
correct, and the converted traces simulate like legacy ones in NDP."""

import io
import re
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest

from atlahs import finishing_times, simulate_ndp
from helpers import is_operation, run

pytestmark = pytest.mark.atlahs

EMITTER = Path(__file__).with_name("atlahs_emitter.py")
EXPLICIT_FIELDS = ("gpu_id", "peer_gpu_id", "stream_id", "communicator_id", "channel_id", "sequence_id",
                   "message_kind", "chunk_id")


@pytest.fixture(scope="module")
def upstream_generator(atlahs_dir, atlahs_upstream_revision, tmp_path_factory):
    """The generator at the commit the fork branched from, not the edited checkout."""
    directory = tmp_path_factory.mktemp("upstream-generator")
    archive = subprocess.run(["git", "-C", atlahs_dir, "archive", "--format=tar",
                              f"{atlahs_upstream_revision}:goal_gen/ai/nccl_goal_generator"],
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as source:
        source.extractall(directory, filter="data")
    return directory


@pytest.fixture(scope="module")
def topology(htsim_dir):
    return htsim_dir / "datacenter" / "topologies" / "leaf_spine_tiny.topo"


def emit(generator, output, mode, scenario="p2p", pairing="aligned", check=True):
    """Runs atlahs_emitter.py in its own process, so each generator version imports its own modules."""
    return run([sys.executable, EMITTER, generator, output, mode, scenario, pairing], cwd=output.parent, check=check)


def operations(goal):
    return [line for line in goal.read_text().splitlines() if is_operation(line)]


@pytest.fixture
def message_fields(atlahs_generator, monkeypatch):
    monkeypatch.syspath_prepend(str(atlahs_generator))
    from generator_modules.data_dependency_modules.message_fields import format_message_fields
    return format_message_fields


# Minimum-width decimal padding merges distinct identities in legacy tags; explicit fields do not.
def test_explicit_fields_keep_identities_apart(message_fields):
    first, second = (1, 100, 0, 5, 0), (11, 0, 0, 5, 0)
    assert message_fields(*first, False) == message_fields(*second, False), "the legacy collision fixture is invalid"
    assert message_fields(*first, True) != message_fields(*second, True)


def test_keeps_full_width_components(message_fields):
    maximum = (1 << 64) - 1
    assert f"chunk_id {maximum}" in message_fields(maximum, "123", 1 << 40, 5, 1 << 48, True)[1]


@pytest.mark.parametrize("component", [-1, (1 << 64), 1.5, True, "1.5", "-1", ""])
def test_rejects_invalid_components(message_fields, component):
    with pytest.raises(ValueError):
        message_fields(component, 0, 0, 5, 0, True)


def test_rejects_unknown_message_kinds(message_fields):
    with pytest.raises(ValueError):
        message_fields(0, 0, 0, 6, 0, True)


@pytest.mark.parametrize("scenario, mode", [("p2p", "legacy"), ("p2p", "shared"), ("ring", "legacy"),
                                            ("tree", "legacy")])
def test_legacy_output_is_unchanged_from_upstream(upstream_generator, atlahs_generator, tmp_path, scenario, mode):
    upstream, edited = tmp_path / "upstream.goal", tmp_path / "edited.goal"
    emit(upstream_generator, upstream, mode, scenario)
    emit(atlahs_generator, edited, mode, scenario)
    assert edited.read_bytes() == upstream.read_bytes()


def test_cli_rejects_conflicting_gpu_modes(atlahs_generator, tmp_path):
    result = run([sys.executable, atlahs_generator / "get_traced_events.py", "-i", "unused", "-o", "unused",
                  "--explicit-fields", "--unique-nic"], cwd=tmp_path, check=False)
    assert result.returncode == 2
    assert "not allowed with" in result.stderr


def test_explicit_output_moves_metadata_into_fields(atlahs_generator, tmp_path):
    goal = tmp_path / "explicit.goal"
    emit(atlahs_generator, goal, "explicit")
    lines = operations(goal)
    for line in lines:
        assert " cpu 0 " in line, f"stream still encoded in the CPU: {line}"
        for field in EXPLICIT_FIELDS:
            assert len(re.findall(rf"\b{field} \d+", line)) == 1, f"not exactly one {field}: {line}"
        if ": calc " not in line:
            assert " tag 0 " in line, f"identity still hidden in the tag: {line}"
    for kind in ["send", "recv"]:
        messages = [line for line in lines if f": {kind} " in line]
        assert all(" nic 0 " in line for line in messages), f"GPU still encoded in the NIC for {kind}"
        assert {int(re.search(r"gpu_id (\d+)", line)[1]) for line in messages} == {0, 1, 2, 3}, kind


# Legacy finish times cannot show cross-GPU routing errors, so the peers are checked directly.
@pytest.mark.parametrize("scenario", ["p2p", "ring", "tree"])
def test_crossed_peers_name_the_remote_gpu(atlahs_generator, tmp_path, scenario):
    goal = tmp_path / "crossed.goal"
    emit(atlahs_generator, goal, "explicit", scenario, "crossed")
    directions = set()
    for line in operations(goal):
        peer = int(re.search(r"\bpeer_gpu_id (\d+)", line)[1])
        if ": calc " in line:
            assert peer == 0, line
            continue
        assert peer == (int(re.search(r"\bgpu_id (\d+)", line)[1]) + 2) % 4, line
        directions.add("send" if ": send " in line else "recv")
    assert directions == {"send", "recv"}


def test_rejects_an_unresolved_peer_gpu(atlahs_generator, tmp_path):
    result = emit(atlahs_generator, tmp_path / "missing-peer.goal", "explicit", "p2p", "missing", check=False)
    assert result.returncode != 0
    assert "Cannot resolve peer GPU" in result.stderr


# Point-to-point messages and the collectives, which the emitter handles separately: a
# multi-channel ring (Simple protocol) and a tree (LL).
@pytest.mark.parametrize("scenario", ["p2p", "ring", "tree"])
def test_explicit_traces_simulate_like_legacy(atlahs_generator, converter, atlahs_converter, htsim, topology,
                                              tmp_path, scenario):
    times = {}
    for mode in ["legacy", "explicit", "wide"]:
        goal = tmp_path / f"{mode}.goal"
        emit(atlahs_generator, goal, mode, scenario)
        binary = goal.with_suffix(".bin")
        if mode == "legacy":
            converter(goal, binary, format="legacy")
        else:
            atlahs_converter(goal, binary, format="self-describing")
        original = binary.read_bytes()
        output = simulate_ndp(htsim, topology, binary, cwd=tmp_path).stdout
        times[mode] = finishing_times(output, ranks=2)
        assert "HTSIM nodes=8" in output, f"wrong GPU endpoint mapping for {mode}"
        assert binary.read_bytes() == original, f"the {mode} simulation modified its input"
    assert times["explicit"] == times["legacy"]
    assert times["wide"] == times["legacy"]
