"""Check that legacy and self-describing traces simulate identically.

Covers a saved reference, both message arrival orders and protocols, custom fields,
randomized schedules, the eager/rendezvous regression and the mmap modes.
"""

import argparse
import json
import random
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, success=True):
    result = subprocess.run(command, text=True, capture_output=True, timeout=30)
    require((result.returncode == 0) == success,
            f"Command exited {result.returncode}: {command}\n{result.stdout}{result.stderr}")
    return result


def host_times(output):
    return dict(re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", output, re.MULTILINE))


def random_schedule(rng, ranks, operations):
    """Acyclic random schedule: dependencies only point to earlier operations of the same rank."""
    ops = {rank: [] for rank in range(ranks)}
    deps = {rank: [] for rank in range(ranks)}

    def add(rank, body, calc=False):
        label = f"l{len(ops[rank]) + 1}"
        earlier = [existing for existing, _ in ops[rank]]
        for target in rng.sample(earlier, k=min(len(earlier), rng.choice([0, 0, 1, 1, 2]))):
            deps[rank].append(f"{label} {'irequires' if rng.random() < 0.3 else 'requires'} {target}")
        attributes = f" cpu {rng.randrange(4)}" if rng.random() < 0.5 else ""
        if not calc and rng.random() < 0.5:
            attributes += f" nic {rng.randrange(4)}"
        ops[rank].append((label, f"{label}: {body}{attributes}"))

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
        lines += [f"rank {rank} {{", *(text for _, text in ops[rank]), *deps[rank], "}"]
    return "\n".join(lines) + "\n"


def with_custom_fields(text, rng):
    return "\n".join(f"{line} gpu_id {rng.randrange(8)} priority {rng.randrange(-5, 6)}"
                     if re.match(r"l\d+:", line) else line for line in text.splitlines()) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", required=True)
    parser.add_argument("--plain-fields", required=True, help="Custom-field TOML for plain traces")
    parser.add_argument("--custom-fields", required=True, help="Custom-field TOML declaring gpu_id and priority")
    parser.add_argument("--simulator", required=True)
    args = parser.parse_args()
    reference = Path(__file__).parent / "reference" / "binomialtreebcast_8"
    with tempfile.TemporaryDirectory(prefix="new-simulation-") as temporary:
        directory = Path(temporary)
        source = directory / "input.goal"

        def convert(text, name, format="self-describing", fields=args.plain_fields):
            source.write_text(text)
            output = directory / name
            run([args.converter, "-i", str(source), "-o", str(output), "--format", format,
                 "--custom-fields", fields])
            return output

        baseline = convert((reference / "schedule.goal").read_text(), "reference.sd.bin")
        metadata = json.loads((reference / "metadata.json").read_text())
        expected = json.loads((reference / "expected_times.json").read_text())
        result = run([args.simulator, "-f", str(baseline), *metadata["simulator_args"]])
        require(host_times(result.stdout) == expected, "New reference trace changed expected host times")

        for delay in [0, 1000000]:
            schedule = f"""num_ranks 3
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
            legacy = convert(schedule, f"legacy-{delay}.bin", "legacy")
            packed = convert(schedule, f"packed-{delay}.bin")
            custom_text = "\n".join(line + " priority -2 gpu_id 1" if re.match(r"l\d+:", line)
                                     else line for line in schedule.splitlines()) + "\n"
            custom = convert(custom_text, f"custom-{delay}.bin", fields=args.custom_fields)
            for threshold in [1, 65535]:
                sim_args = ["-S", str(threshold)]
                expected_times = host_times(run([args.simulator, "-f", str(legacy), *sim_args]).stdout)
                require(len(expected_times) == 3, "Legacy test did not report all ranks")
                for file in [packed, custom]:
                    original = file.read_bytes()
                    for _ in range(2):
                        actual = host_times(run([args.simulator, "-f", str(file), *sim_args]).stdout)
                        require(actual == expected_times, f"Changed host times for delay={delay}, S={threshold}")
                        require(file.read_bytes() == original, "Private mmap modified the source file")

            # Mapping a read-only file privately must still allow counter updates.
            packed.chmod(0o444)
            run([args.simulator, "-f", str(packed)])
            packed.chmod(0o644)
            for file in [legacy, packed]:
                destructive = directory / (file.name + ".destructive")
                destructive.write_bytes(file.read_bytes())
                expected_times = host_times(run([args.simulator, "-f", str(file)]).stdout)
                result = run([args.simulator, "-f", str(destructive), "--save-mem"])
                require(host_times(result.stdout) == expected_times, "Destructive mmap changed execution times")
                require(struct.unpack_from("<Q", destructive.read_bytes())[0] == 2342,
                        "Destructive run did not invalidate the trace")
                failed = run([args.simulator, "-f", str(destructive)], success=False)
                require("invalidated" in failed.stderr, failed.stderr)

        for text in ["num_ranks 2\nrank 0 {\n}\nrank 1 {\n}\n",
                     "num_ranks 1\nrank 0 {\nl1: calc 10 cpu 255 nic 255\n}\n"]:
            legacy = convert(text, "resource-legacy.bin", "legacy")
            packed = convert(text, "resource-packed.bin")
            legacy_result = run([args.simulator, "-f", str(legacy)])
            packed_result = run([args.simulator, "-f", str(packed)])
            require(host_times(legacy_result.stdout) == host_times(packed_result.stdout),
                    "Empty ranks or maximum resource IDs changed timing")
            if "cpu 255" in text:
                require("256 CPUs, 256 NICs" in packed_result.stdout, "Resource count wrapped")

        # Regression: an eager send matched by a larger (rendezvous-sized) receive used to be
        # completed twice when the message arrived before the receive was posted.
        for delay in [100000, 0]:
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
                times = [host_times(run([args.simulator, "-f", str(convert(text, f"mixed.{format}", format))]).stdout)
                         for format in ["legacy", "self-describing"]]
                require(times[0] == times[1], "Mixed message sizes simulate differently per format")
                sender_times.append(times[0]["0"])
            require(sender_times[0] == sender_times[1],
                    f"A larger receive buffer changed the eager sender's finish time: {sender_times}")

        # Randomized schedules: every format and parameter set must give identical output.
        rng = random.Random(20261022)
        parameter_sets = [[], ["-S", "100"], ["-L", "0", "-o", "0", "-g", "0", "-G", "0"],
                          ["-L", "7", "-o", "3", "-g", "11", "-G", "2", "-O", "1"]]
        for case in range(60):
            text = random_schedule(rng, rng.choice([1, 2, 3, 5, 8]), rng.choice([0, 5, 20, 60]))
            files = [convert(text, "random.legacy", "legacy"), convert(text, "random.sd"),
                     convert(with_custom_fields(text, rng), "random.custom", fields=args.custom_fields)]
            for parameters in parameter_sets:
                outputs = [[line for line in run([args.simulator, "-f", str(file), *parameters]).stdout.splitlines()
                            if not line.startswith("PERFORMANCE")] for file in files]
                require(outputs[0] == outputs[1] == outputs[2],
                        f"Random schedule {case} differs between formats with {parameters}:\n{text}")

        for length in [0, 1, 7, 8, 20, 42]:
            truncated = directory / "truncated.bin"
            truncated.write_bytes(baseline.read_bytes()[:length])
            result = run([args.simulator, "-f", str(truncated)], success=False)
            require("LogGOPSim:" in result.stderr, "Malformed trace did not produce a useful diagnostic")
        print("PASS: reference times, arrival orders, protocols, custom fields, mixed sizes, random schedules and mmap modes")


if __name__ == "__main__":
    main()
