"""ATLAHS's NDP simulator on legacy traces and on self-describing traces with message identities."""

import os
import re

import pytest

from atlahs import explicit_attributes, finishing_times, simulate_ndp
from helpers import is_operation

pytestmark = pytest.mark.atlahs


@pytest.fixture(scope="module")
def topology(htsim_dir):
    return htsim_dir / "datacenter" / "topologies" / "leaf_spine_tiny.topo"


@pytest.fixture
def ndp(htsim, topology, tmp_path):
    def ndp(trace, **options):
        return simulate_ndp(htsim, topology, trace, cwd=tmp_path, **options)
    return ndp


def explicit_schedule(schedule):
    """Moves the tag into chunk_id and the CPU into stream_id, as the explicit-field generator does."""
    lines = []
    for line in schedule.splitlines():
        if is_operation(line):
            tag = re.search(r"\btag (\d+)", line)
            cpu = re.search(r"\bcpu (\d+)", line)
            line = re.sub(r"\bcpu \d+", "cpu 0", re.sub(r"\btag \d+", "tag 0", line))
            line += explicit_attributes(stream=int(cpu[1]) if cpu else 0, chunk=int(tag[1]) if tag else 0)
        lines.append(line)
    return "\n".join(lines) + "\n"


def ping_pong(delay):
    """Both receive and message arrival orders, and both kinds of dependency lists."""
    return f"""num_ranks 8
rank 0 {{
l1: calc 1000
l2: send 4096b to 1 tag 42
l3: recv 4096b from 1 tag 43
l2 requires l1
l3 requires l2
l3 irequires l2
}}
rank 1 {{
l1: calc {delay}
l2: recv 4096b from 0 tag 42
l3: calc 500
l4: send 4096b to 0 tag 43
l2 requires l1
l3 requires l2
l4 requires l3
}}
""" + "".join(f"rank {rank} {{\n}}\n" for rank in range(2, 8))


def identity_schedule(component, delay, explicit):
    """Two messages that differ in one identity field. The first receive asks for the second
    message; if the first matches it instead, the reply that releases the second message is
    never sent and the run does not finish."""
    first = dict(communicator=1 << 48, channel=123, sequence=1 << 40, kind=4, chunk=(1 << 64) - 2)
    second = dict(first)
    second[component] += 1

    def operation(label, text, tag, identity):
        return f"l{label}: {text} tag {0 if explicit else tag}" + (explicit_attributes(**identity) if explicit else "")
    return "\n".join([
        "num_ranks 2", "rank 0 {",
        operation(1, "send 4096b to 1", 42, first),
        operation(2, "recv 4096b from 1", 99, dict(chunk=99)),
        operation(3, "send 4096b to 1", 43, second),
        "l3 requires l2", "}", "rank 1 {",
        operation(1, "recv 4096b from 0", 43, second),
        operation(2, "recv 4096b from 0", 42, first),
        operation(3, "send 4096b to 0", 99, dict(chunk=99)),
        f"l4: calc {delay}" + (explicit_attributes() if explicit else ""),
        "l1 requires l4", "l2 requires l4", "l3 requires l2", "}", ""])


def gpu_schedule(explicit, collapse=False, rotate=False):
    """Two ranks with four GPUs each. The explicit trace uses NIC 7 for every operation, so a
    reader that takes the GPU from the NIC fails. Rotation receives each message on a
    different local GPU than it was sent from, as on NCCL ring hops between hosts."""
    lines = ["num_ranks 2"]
    for rank in range(2):
        lines.append(f"rank {rank} {{")
        for gpu in range(4):
            operation = "send 1000000b to 1" if rank == 0 else "recv 1000000b from 0"
            local = (gpu + 1) % 4 if rotate and rank == 1 else gpu
            remote = ((gpu + 1) % 4 if rotate else gpu) if rank == 0 else gpu
            suffix = (explicit_attributes(gpu=0 if collapse else local, peer_gpu=0 if collapse else remote,
                                          stream=gpu, chunk=gpu + 42) if explicit else "")
            lines.append(f"l{gpu + 1}: {operation} tag {0 if explicit else gpu + 42} "
                         f"cpu {7 if explicit else gpu} nic {7 if explicit else local}{suffix}")
        # Keep the resource count fixed even when all communication uses GPU 0.
        suffix = explicit_attributes(gpu=3, stream=4) if explicit else ""
        lines.append(f"l5: calc 0 cpu {7 if explicit else 4} nic {7 if explicit else 3}{suffix}")
        lines.append("}")
    return "\n".join(lines) + "\n"


def route_schedule(source_gpu, receive_gpu, delay):
    """One message between two GPUs, on different streams; the delay orders receive and message."""
    return "\n".join([
        "num_ranks 2", "rank 0 {",
        "l1: send 4096b to 1 tag 0 cpu 0 nic 0"
        + explicit_attributes(gpu=source_gpu, peer_gpu=receive_gpu, stream=17, chunk=42),
        "l2: calc 0 cpu 0 nic 0" + explicit_attributes(gpu=3),
        "}", "rank 1 {",
        "l1: recv 4096b from 0 tag 0 cpu 0 nic 0"
        + explicit_attributes(gpu=receive_gpu, peer_gpu=source_gpu, stream=3, chunk=42),
        "l2: calc 0 cpu 0 nic 0" + explicit_attributes(gpu=3),
        f"l3: calc {delay} cpu 0 nic 0" + explicit_attributes(stream=3),
        "l1 requires l3", "}", ""])


def stream_schedule(explicit, collapse=False):
    """Two calculations per rank, on two streams, or on one if collapsed."""
    lines = ["num_ranks 2"]
    for rank in range(2):
        lines.append(f"rank {rank} {{")
        for i in range(2):
            suffix = explicit_attributes(gpu=3, stream=1024 + (0 if collapse else i)) if explicit else ""
            lines.append(f"l{i + 1}: calc 100000 cpu {7 if explicit else i} nic {0 if explicit else 3}{suffix}")
        lines.append("}")
    return "\n".join(lines) + "\n"


def convert(converter, atlahs_converter, text, directory, name, explicit):
    if explicit:
        return atlahs_converter.convert_text(text, directory, name, format="self-describing")
    return converter.convert_text(text, directory, name, format="legacy")


@pytest.mark.parametrize("schedule", ["reference", "receive first", "message first"])
def test_explicit_identities_finish_like_legacy_tags(converter, atlahs_converter, ndp, reference_case, tmp_path,
                                                     schedule):
    text = {"reference": lambda: (reference_case / "schedule.goal").read_text(),
            "receive first": lambda: ping_pong(0), "message first": lambda: ping_pong(1000000)}[schedule]()
    legacy = converter.convert_text(text, tmp_path, "legacy.bin", format="legacy")
    expected = finishing_times(ndp(legacy).stdout)

    packed = converter.convert_text(text, tmp_path, "packed.bin", format="self-describing")
    missing_gpu = ndp(packed, check=False)
    assert missing_gpu.returncode != 0 and "gpu_id" in missing_gpu.stderr, "accepted a trace without gpu_id"

    explicit = atlahs_converter.convert_text(explicit_schedule(text), tmp_path, "explicit.bin", format="self-describing")
    original = explicit.read_bytes()
    explicit.chmod(0o444)
    try:
        for _ in range(2):
            assert finishing_times(ndp(explicit).stdout) == expected
            assert explicit.read_bytes() == original, "the private mapping modified the trace"
    finally:
        explicit.chmod(0o644)


def test_explicit_gpus_route_like_legacy_gpus(converter, atlahs_converter, ndp, tmp_path):
    outputs = {}
    for name, explicit, collapse, rotate in [("legacy", False, False, False), ("explicit", True, False, False),
                                             ("shared", True, True, False), ("legacy crossed", False, False, True),
                                             ("explicit crossed", True, False, True)]:
        trace = convert(converter, atlahs_converter, gpu_schedule(explicit, collapse, rotate), tmp_path,
                        name.replace(" ", "-") + ".bin", explicit)
        outputs[name] = ndp(trace).stdout
        assert "HTSIM nodes=8" in outputs[name], f"wrong GPU endpoint mapping for {name}:\n{outputs[name]}"
    times = {name: finishing_times(output, ranks=2) for name, output in outputs.items()}
    assert times["explicit"] == times["legacy"]
    assert times["shared"] != times["explicit"], "changing gpu_id did not change the simulation"
    # Upstream routes to the same GPU index on both hosts, so its times are no reference for
    # cross-GPU traffic; finishing_times only requires the crossed runs to complete.


@pytest.mark.parametrize("delay", [0, 1000000], ids=["receive first", "message first"])
@pytest.mark.parametrize("component", ["communicator", "channel", "sequence", "kind", "chunk"])
def test_each_identity_field_separates_messages(converter, atlahs_converter, ndp, tmp_path, component, delay):
    times = [finishing_times(ndp(convert(converter, atlahs_converter, identity_schedule(component, delay, explicit),
                                         tmp_path, "identity.bin", explicit)).stdout, ranks=2)
             for explicit in (False, True)]
    assert times[0] == times[1]


# With four GPUs per host, GPU g of host A is endpoint g and of host B endpoint 4 + g.
@pytest.mark.parametrize("delay", [0, 1000000], ids=["receive first", "message first"])
@pytest.mark.parametrize("source_gpu, receive_gpu", [(0, 0), (0, 2), (2, 0)])
def test_routes_to_the_peer_gpu(atlahs_converter, ndp, tmp_path, source_gpu, receive_gpu, delay):
    trace = atlahs_converter.convert_text(route_schedule(source_gpu, receive_gpu, delay), tmp_path, "route.bin",
                                          format="self-describing")
    output = ndp(trace, env={**os.environ, "ATLAHS_TRACE_ROUTES": "1"}).stdout
    finishing_times(output, ranks=2)
    routes = [tuple(map(int, entry))
              for entry in re.findall(r"^ATLAHS_ROUTE (\d+) (\d+) (\d+) (\d+) (\d+)$", output, re.MULTILINE)]
    assert routes == [(0, 1, source_gpu, source_gpu, 4 + receive_gpu)]


def test_a_receive_on_another_gpu_does_not_match(atlahs_converter, ndp, tmp_path):
    text = route_schedule(2, 0, 1000000).replace("peer_gpu_id 2", "peer_gpu_id 1", 1)
    result = ndp(atlahs_converter.convert_text(text, tmp_path, "wrong-gpu.bin", format="self-describing"), check=False)
    assert result.returncode != 0
    assert "unmatched" in result.stdout + result.stderr


# Streams are independent of the standard CPU field, and IDs above 255 are not narrowed.
def test_streams_schedule_calculations(converter, atlahs_converter, ndp, tmp_path):
    times = [finishing_times(ndp(convert(converter, atlahs_converter, stream_schedule(explicit, collapse), tmp_path,
                                         "streams.bin", explicit)).stdout, ranks=2)
             for explicit, collapse in [(False, False), (True, False), (True, True)]]
    assert times[1] == times[0], "explicit streams schedule differently from CPUs"
    assert times[2] != times[1], "collapsing the streams did not serialize the calculations"


# A message without a matching receive fails the run instead of ending it normally.
def test_unmatched_identity_fails_the_run(atlahs_converter, ndp, tmp_path):
    text = identity_schedule("communicator", 0, True).replace(
        "communicator_id 281474976710656", "communicator_id 281474976710658", 1)
    result = ndp(atlahs_converter.convert_text(text, tmp_path, "unmatched.bin", format="self-describing"), check=False)
    assert result.returncode != 0
    assert "unmatched messages or receives" in result.stdout + result.stderr
