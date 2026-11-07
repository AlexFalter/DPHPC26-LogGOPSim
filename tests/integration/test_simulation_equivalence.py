"""Legacy and self-describing traces simulate identically in LogGOPSim.

Covers the saved reference, both message arrival orders and protocols, custom fields,
randomized schedules, the eager/rendezvous regression and the mmap modes.
"""

import json
import random
import struct

import pytest

from helpers import append_to_operations, host_times, run

INVALIDATED_MAGIC = 2342

# A message from rank 0 to 1 and a reply. The delay before rank 1's receive decides whether
# the message or the receive arrives first.
def ping_pong(delay):
    return f"""num_ranks 3
rank 0 {{
l1: calc 1000
l2: send 8b to 1 tag 42 cpu 1 nic 1
l3: recv 8b from 1 tag 43 cpu 1 nic 1
l2 requires l1
l3 requires l2
l3 irequires l2
}}
rank 1 {{
l1: calc {delay}
l2: recv 8b from 0 tag 42
l3: calc 500
l4: send 8b to 0 tag 43
l2 requires l1
l3 requires l2
l4 requires l3
}}
rank 2 {{
}}
"""


def random_schedule(rng, ranks, operations):
    """An acyclic random schedule: dependencies only point to earlier operations of the same rank."""
    operations_of = {rank: [] for rank in range(ranks)}
    dependencies = {rank: [] for rank in range(ranks)}

    def add(rank, body, calc=False):
        label = f"l{len(operations_of[rank]) + 1}"
        earlier = [existing for existing, _ in operations_of[rank]]
        for target in rng.sample(earlier, k=min(len(earlier), rng.choice([0, 0, 1, 1, 2]))):
            dependencies[rank].append(f"{label} {'irequires' if rng.random() < 0.3 else 'requires'} {target}")
        attributes = f" cpu {rng.randrange(4)}" if rng.random() < 0.5 else ""
        if not calc and rng.random() < 0.5:
            attributes += f" nic {rng.randrange(4)}"
        operations_of[rank].append((label, f"{label}: {body}{attributes}"))

    for _ in range(operations):
        if ranks == 1 or rng.random() < 0.3:
            add(rng.randrange(ranks), f"calc {rng.choice([0, 1, 500, 10**6, 2**40])}", calc=True)
            continue
        source, destination = rng.sample(range(ranks), 2)
        size = rng.choice([1, 8, 1024, 65535, 65536, 10**6])
        tag = rng.choice([0, 1, 42, 2**31, 2**32 - 2])
        add(source, f"send {size}b to {destination} tag {tag}")
        # Wildcards and differing receive sizes may pair messages and buffers of different sizes.
        receive_size = size if rng.random() < 0.7 else rng.choice([8, 10**6])
        add(destination, f"recv {receive_size}b from {-1 if rng.random() < 0.15 else source} "
                         f"tag {-1 if rng.random() < 0.15 else tag}")
    lines = [f"num_ranks {ranks}"]
    for rank in range(ranks):
        lines += [f"rank {rank} {{", *(text for _, text in operations_of[rank]), *dependencies[rank], "}"]
    return "\n".join(lines) + "\n"


@pytest.fixture
def simulate(loggopsim):
    def simulate(trace, *options, check=True):
        return run([loggopsim, "-f", trace, *options], check=check)
    return simulate


def test_reference_trace_keeps_its_host_times(converter, simulate, reference_case, tmp_path):
    trace = converter.convert_text((reference_case / "schedule.goal").read_text(), tmp_path, "reference.sd.bin",
                                   format="self-describing")
    metadata = json.loads((reference_case / "metadata.json").read_text())
    expected = json.loads((reference_case / "expected_times.json").read_text())
    assert host_times(simulate(trace, *metadata["simulator_args"]).stdout) == expected


# Both protocols, eager and rendezvous, for both arrival orders. Each self-describing trace
# runs twice from a private mapping, which must leave the file unchanged.
@pytest.mark.parametrize("threshold", [1, 65535], ids=["rendezvous", "eager"])
@pytest.mark.parametrize("delay", [0, 1000000], ids=["receive first", "message first"])
def test_formats_give_the_same_host_times(converter, simulate, custom_fields, tmp_path, delay, threshold):
    schedule = ping_pong(delay)
    legacy = converter.convert_text(schedule, tmp_path, "legacy.bin", format="legacy")
    packed = converter.convert_text(schedule, tmp_path, "packed.bin", format="self-describing")
    custom = converter.convert_text(append_to_operations(schedule, " priority -2 gpu_id 1"), tmp_path,
                                    "custom.bin", format="self-describing", fields=custom_fields)
    expected = host_times(simulate(legacy, "-S", threshold).stdout)
    assert len(expected) == 3
    for trace in [packed, custom]:
        original = trace.read_bytes()
        for _ in range(2):
            assert host_times(simulate(trace, "-S", threshold).stdout) == expected, trace.name
            assert trace.read_bytes() == original, "the private mapping modified the file"


# Mapping a read-only file privately still allows the counter updates.
def test_simulates_read_only_files(converter, simulate, tmp_path):
    packed = converter.convert_text(ping_pong(0), tmp_path, "packed.bin", format="self-describing")
    packed.chmod(0o444)
    try:
        simulate(packed)
    finally:
        packed.chmod(0o644)


# --save-mem maps the file shared, updates it in place and marks it as used up.
@pytest.mark.parametrize("format", ["legacy", "self-describing"])
def test_save_mem_invalidates_the_trace(converter, simulate, tmp_path, format):
    trace = converter.convert_text(ping_pong(0), tmp_path, "trace.bin", format=format)
    destructive = tmp_path / "destructive.bin"
    destructive.write_bytes(trace.read_bytes())
    expected = host_times(simulate(trace).stdout)
    assert host_times(simulate(destructive, "--save-mem").stdout) == expected
    assert struct.unpack_from("<Q", destructive.read_bytes())[0] == INVALIDATED_MAGIC
    assert "invalidated" in simulate(destructive, check=False).stderr


@pytest.mark.parametrize("text", [
    "num_ranks 2\nrank 0 {\n}\nrank 1 {\n}\n",
    "num_ranks 1\nrank 0 {\nl1: calc 10 cpu 255 nic 255\n}\n",
], ids=["empty ranks", "maximum resource IDs"])
def test_empty_ranks_and_maximum_resources(converter, simulate, tmp_path, text):
    legacy = simulate(converter.convert_text(text, tmp_path, "legacy.bin", format="legacy"))
    packed = simulate(converter.convert_text(text, tmp_path, "packed.bin", format="self-describing"))
    assert host_times(legacy.stdout) == host_times(packed.stdout)
    if "cpu 255" in text:
        assert "256 CPUs, 256 NICs" in packed.stdout, "the resource count wrapped"


# Regression: an eager send matched by a larger, rendezvous-sized receive used to be
# completed twice when the message arrived before the receive was posted.
@pytest.mark.parametrize("delay", [100000, 0], ids=["message first", "receive first"])
def test_eager_send_into_a_larger_receive(converter, simulate, tmp_path, delay):
    sender_times = []
    for receive_size in [8, 100000]:
        text = f"""num_ranks 2
rank 0 {{
l1: send 8b to 1 tag 5
l2: calc 100
l3: calc 100
l3 requires l1
l3 requires l2
}}
rank 1 {{
l1: calc {delay}
l2: recv {receive_size}b from 0 tag 5
l2 requires l1
}}
"""
        times = [host_times(simulate(converter.convert_text(text, tmp_path, f"mixed.{format}", format=format)).stdout)
                 for format in ["legacy", "self-describing"]]
        assert times[0] == times[1], f"the formats differ for a {receive_size}-byte receive"
        sender_times.append(times[0]["0"])
    assert sender_times[0] == sender_times[1], "a larger receive buffer changed the eager sender's finish time"


PARAMETER_SETS = [[], ["-S", "100"], ["-L", "0", "-o", "0", "-g", "0", "-G", "0"],
                  ["-L", "7", "-o", "3", "-g", "11", "-G", "2", "-O", "1"]]


# Every format and parameter set gives the same output, except for the wall-clock line.
@pytest.mark.parametrize("case", range(60))
def test_random_schedules_simulate_identically(converter, simulate, custom_fields, tmp_path, case):
    rng = random.Random(20261022 + case)
    text = random_schedule(rng, rng.choice([1, 2, 3, 5, 8]), rng.choice([0, 5, 20, 60]))
    custom_text = append_to_operations(text, lambda _: f" gpu_id {rng.randrange(8)} priority {rng.randrange(-5, 6)}")
    traces = [converter.convert_text(text, tmp_path, "random.legacy", format="legacy"),
              converter.convert_text(text, tmp_path, "random.sd", format="self-describing"),
              converter.convert_text(custom_text, tmp_path, "random.custom", format="self-describing",
                                     fields=custom_fields)]
    for parameters in PARAMETER_SETS:
        outputs = [[line for line in simulate(trace, *parameters).stdout.splitlines() if not line.startswith("PERFORMANCE")]
                   for trace in traces]
        assert outputs[0] == outputs[1] == outputs[2], f"parameters {parameters}, schedule:\n{text}"


@pytest.mark.parametrize("length", [0, 1, 7, 8, 20, 42])
def test_truncated_traces_fail_with_a_diagnostic(converter, simulate, reference_case, tmp_path, length):
    trace = converter.convert_text((reference_case / "schedule.goal").read_text(), tmp_path, "reference.sd.bin",
                                   format="self-describing")
    truncated = tmp_path / "truncated.bin"
    truncated.write_bytes(trace.read_bytes()[:length])
    result = simulate(truncated, check=False)
    assert result.returncode != 0
    assert "LogGOPSim:" in result.stderr
