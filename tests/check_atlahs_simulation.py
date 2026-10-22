"""Run the actual ATLAHS NDP scheduler on legacy and self-describing traces."""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def run(command, directory, env=None):
    result = subprocess.run(command, cwd=directory, text=True, capture_output=True, timeout=30, env=env)
    if result.returncode:
        raise RuntimeError(f"Command exited {result.returncode}: {command}\n{result.stdout}{result.stderr}")
    return result.stdout


def finishing_times(output, ranks=8):
    times = dict(re.findall(r"^Host\s+(\d+):\s*(\d+)\s*$", output, re.MULTILINE))
    if len(times) != ranks or "LGS terminates!" not in output or "queue on host" in output:
        raise RuntimeError(f"ATLAHS did not finish the {ranks}-rank workload:\n{output}")
    return times


def explicit_attributes(gpu=0, stream=0, peer_gpu=None, communicator=0, channel=0, sequence=0, kind=5, chunk=0):
    return (f" stream_id {stream} gpu_id {gpu} communicator_id {communicator} channel_id {channel}"
            f" sequence_id {sequence} message_kind {kind} chunk_id {chunk} peer_gpu_id {gpu if peer_gpu is None else peer_gpu}")


def explicit_schedule(schedule):
    lines = []
    for line in schedule.splitlines():
        if re.match(r"l\d+:", line):
            tag = re.search(r"\btag (\d+)", line)
            line = re.sub(r"\btag \d+", "tag 0", line)
            cpu = re.search(r"\bcpu (\d+)", line)
            stream = int(cpu[1]) if cpu else 0
            line = re.sub(r"\bcpu \d+", "cpu 0", line)
            line += explicit_attributes(stream=stream, chunk=int(tag[1]) if tag else 0)
        lines.append(line)
    return "\n".join(lines) + "\n"


def identity_schedule(component, delay, explicit):
    first = dict(communicator=1 << 48, channel=123, sequence=1 << 40, kind=4, chunk=(1 << 64) - 2)
    second = dict(first)
    second[component] += 1
    def operation(label, text, tag, identity):
        return (f"l{label}: {text} tag {0 if explicit else tag}"
                + (explicit_attributes(**identity) if explicit else ""))
    # The first receive deliberately requests B. If A wrongly matches it, the
    # acknowledgement needed to send B is never released and the test fails.
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", type=Path, required=True)
    parser.add_argument("--plain-fields", type=Path, required=True, help="Custom-field TOML for plain traces")
    parser.add_argument("--atlahs-fields", type=Path, required=True, help="Custom-field TOML for ATLAHS traces")
    parser.add_argument("--simulator", type=Path, required=True)
    parser.add_argument("--topology", type=Path, required=True)
    args = parser.parse_args()
    plain = [str(args.converter.resolve()), "--custom-fields", str(args.plain_fields.resolve())]
    atlahs = [str(args.converter.resolve()), "--custom-fields", str(args.atlahs_fields.resolve())]
    simulator = args.simulator.resolve()
    if not simulator.is_file():
        parser.error("Build the atlahs_htsim target before running this integration test")
    topology = args.topology.resolve()
    reference = Path(__file__).parent / "reference/binomialtreebcast_8/schedule.goal"
    schedules = [reference.read_text()]
    for delay in [0, 1000000]:
        # Exercise both receive/message arrival orders and start/completion lists.
        schedules.append(f"""num_ranks 8
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
""" + "".join(f"rank {rank} {{\n}}\n" for rank in range(2, 8)))

    with tempfile.TemporaryDirectory(prefix="atlahs-adapter-") as temporary:
        directory = Path(temporary)
        simulator_args = [str(simulator), "-topo", str(topology), "-nodes", "8",
                          "-strat", "ecmp_host", "-paths", "128", "-q", "1000000", "-seed", "4"]
        for case, schedule in enumerate(schedules):
            source = directory / "input.goal"
            source.write_text(schedule)
            legacy = directory / f"legacy-{case}.bin"
            packed = directory / f"packed-{case}.bin"
            custom = directory / f"custom-{case}.bin"
            for format_name, output in [("legacy", legacy), ("self-describing", packed)]:
                run([*plain, "-i", str(source), "-o", str(output),
                     "--format", format_name], directory)
            source.write_text(explicit_schedule(schedule))
            run([*atlahs, "-i", str(source), "-o", str(custom),
                 "--format", "self-describing"], directory)
            expected = finishing_times(run([*simulator_args, "-goal", str(legacy)], directory))
            missing_gpu = subprocess.run([*simulator_args, "-goal", str(packed)],
                                         cwd=directory, text=True, capture_output=True, timeout=30)
            if missing_gpu.returncode == 0 or "gpu_id" not in missing_gpu.stderr:
                raise RuntimeError("ATLAHS accepted a packed GPU trace without gpu_id")
            trace = custom
            original = trace.read_bytes()
            trace.chmod(0o444)
            try:
                for _ in range(2):
                    actual = finishing_times(run([*simulator_args, "-goal", str(trace)], directory))
                    if actual != expected:
                        raise RuntimeError(f"ATLAHS finish times changed in case {case}: {actual} != {expected}")
                    if trace.read_bytes() != original:
                        raise RuntimeError("ATLAHS private mapping modified its input trace")
            finally:
                trace.chmod(0o644)
        # Two ranks, each with four GPUs. Standard NIC is deliberately different
        # from GPU in the packed input, so copying/ignoring NIC cannot pass.
        def gpu_schedule(custom_fields, collapse=False, rotate=False):
            lines = ["num_ranks 2"]
            for rank in range(2):
                lines.append(f"rank {rank} {{")
                for gpu in range(4):
                    operation = (f"send 1000000b to 1" if rank == 0
                                 else f"recv 1000000b from 0")
                    # Rotation receives each message on a different local GPU than
                    # it was sent from, as on NCCL ring hops between hosts.
                    local = (gpu + 1) % 4 if rotate and rank == 1 else gpu
                    nic = 7 if custom_fields else local
                    remote = ((gpu + 1) % 4 if rotate else gpu) if rank == 0 else gpu
                    suffix = explicit_attributes(gpu=0 if collapse else local, peer_gpu=0 if collapse else remote, stream=gpu, chunk=gpu+42) if custom_fields else ""
                    lines.append(f"l{gpu+1}: {operation} tag {0 if custom_fields else gpu+42} cpu {7 if custom_fields else gpu} nic {nic}{suffix}")
                # Keep resource count fixed even when all communications use GPU 0.
                suffix = explicit_attributes(gpu=3, stream=4) if custom_fields else ""
                lines.append(f"l5: calc 0 cpu {7 if custom_fields else 4} nic {7 if custom_fields else 3}{suffix}")
                lines.append("}")
            return "\n".join(lines) + "\n"

        gpu_traces = []
        for name, custom_fields, collapse, rotate in [("gpu-legacy", False, False, False),
                                                       ("gpu-explicit", True, False, False),
                                                       ("gpu-shared", True, True, False),
                                                       ("gpu-legacy-crossed", False, False, True),
                                                       ("gpu-explicit-crossed", True, False, True)]:
            source = directory / f"{name}.goal"
            source.write_text(gpu_schedule(custom_fields, collapse, rotate))
            trace = source.with_suffix(".bin")
            converter = atlahs if custom_fields else plain
            run([*converter, "-i", str(source), "-o", str(trace), "--format",
                 "self-describing" if custom_fields else "legacy"], directory)
            gpu_traces.append(trace)
        outputs = [run([*simulator_args, "-goal", str(trace)], directory) for trace in gpu_traces]
        expected, explicit, shared, crossed_legacy, crossed_explicit = [
            finishing_times(output, ranks=2) for output in outputs]
        if explicit != expected:
            raise RuntimeError(f"Explicit GPU selection changed legacy GPU behavior: {explicit} != {expected}")
        # Cross-GPU routing intentionally fixes upstream's same-index assumption;
        # completion is required, but upstream timing is not its routing oracle.
        if explicit == shared:
            raise RuntimeError("Changing gpu_id did not affect multi-GPU simulation")
        print(f"Multi-GPU finish times: legacy={expected}, explicit={explicit}, shared={shared}")
        for output in outputs:
            if "HTSIM nodes=8" not in output:
                raise RuntimeError(f"GPU endpoint mapping is incorrect:\n{output}")
        for component in ("communicator", "channel", "sequence", "kind", "chunk"):
            for delay in (0, 1000000):
                times = []
                for explicit in (False, True):
                    source = directory / "identity.goal"
                    source.write_text(identity_schedule(component, delay, explicit))
                    trace = directory / "identity.bin"
                    converter = atlahs if explicit else plain
                    run([*converter, "-i", str(source), "-o", str(trace), "--format",
                         "self-describing" if explicit else "legacy"], directory)
                    times.append(finishing_times(run([*simulator_args, "-goal", str(trace)], directory), ranks=2))
                if times[0] != times[1]:
                    raise RuntimeError(f"Matching changed for {component}, delay={delay}: {times}")
        # Independent endpoint oracle: four GPUs/host imply A0=0 and B2=6.
        # Exercise both receive-first and message-first paths with different streams.
        for source_gpu, receive_gpu in ((0, 0), (0, 2), (2, 0)):
            for delay in (0, 1000000):
                source = directory / "route-audit.goal"
                source.write_text("\n".join([
                    "num_ranks 2", "rank 0 {",
                    "l1: send 4096b to 1 tag 0 cpu 0 nic 0" + explicit_attributes(gpu=source_gpu, peer_gpu=receive_gpu, stream=17, chunk=42),
                    "l2: calc 0 cpu 0 nic 0" + explicit_attributes(gpu=3),
                    "}", "rank 1 {",
                    "l1: recv 4096b from 0 tag 0 cpu 0 nic 0" + explicit_attributes(gpu=receive_gpu, peer_gpu=source_gpu, stream=3, chunk=42),
                    "l2: calc 0 cpu 0 nic 0" + explicit_attributes(gpu=3),
                    f"l3: calc {delay} cpu 0 nic 0" + explicit_attributes(stream=3),
                    "l1 requires l3", "}", ""]))
                trace = source.with_suffix(".bin")
                run([*atlahs, "-i", str(source), "-o", str(trace), "--format", "self-describing"], directory)
                output = run([*simulator_args, "-goal", str(trace)], directory,
                             env={**os.environ, "ATLAHS_TRACE_ROUTES": "1"})
                finishing_times(output, ranks=2)
                routes = [tuple(map(int, entry)) for entry in re.findall(
                    r"^ATLAHS_ROUTE (\d+) (\d+) (\d+) (\d+) (\d+)$", output, re.MULTILINE)]
                expected_route = (0, 1, source_gpu, source_gpu, 4 + receive_gpu)
                if routes != [expected_route]:
                    raise RuntimeError(f"NDP routed to the wrong GPU: {routes} != {[expected_route]}")
                print(f"Routing audit: A{source_gpu} -> B{receive_gpu}, receive delay={delay}: "
                      f"actual endpoints {source_gpu} -> {4 + receive_gpu} (correct)")
        # A receive on the wrong local GPU must not consume a routed message.
        wrong_gpu = source.read_text().replace("peer_gpu_id 2", "peer_gpu_id 1", 1)
        source.write_text(wrong_gpu)
        run([*atlahs, "-i", str(source), "-o", str(trace), "--format", "self-describing"], directory)
        result = subprocess.run([*simulator_args, "-goal", str(trace)], cwd=directory,
                                text=True, capture_output=True, timeout=30)
        if result.returncode == 0 or "unmatched" not in result.stdout + result.stderr:
            raise RuntimeError("Mismatched peer-GPU endpoints were silently accepted")
        # Stream selection is independent of standard CPU, and IDs above 255
        # survive the adapter and scheduler queues without narrowing.
        def stream_schedule(explicit, collapse=False):
            lines = ["num_ranks 2"]
            for rank in range(2):
                lines.append(f"rank {rank} {{")
                for i in range(2):
                    suffix = explicit_attributes(gpu=3, stream=1024 + (0 if collapse else i)) if explicit else ""
                    lines.append(f"l{i+1}: calc 100000 cpu {7 if explicit else i} nic {0 if explicit else 3}{suffix}")
                lines.append("}")
            return "\n".join(lines) + "\n"
        stream_times = []
        for explicit_stream, collapse in [(False, False), (True, False), (True, True)]:
            source = directory / "streams.goal"
            source.write_text(stream_schedule(explicit_stream, collapse))
            trace = source.with_suffix(".bin")
            run([*(atlahs if explicit_stream else plain), "-i", str(source), "-o", str(trace),
                 "--format", "self-describing" if explicit_stream else "legacy"], directory)
            stream_times.append(finishing_times(run([*simulator_args, "-goal", str(trace)], directory), ranks=2))
        if stream_times[0] != stream_times[1] or stream_times[1] == stream_times[2]:
            raise RuntimeError(f"Explicit stream scheduling or wide stream IDs failed: {stream_times}")
        # A missing partner must fail the process, rather than report success
        # after consuming the event queue with pending unmatched receives.
        source = directory / "unmatched.goal"
        schedule = identity_schedule("communicator", 0, True)
        schedule = schedule.replace("communicator_id 281474976710656", "communicator_id 281474976710658", 1)
        source.write_text(schedule)
        trace = directory / "unmatched.bin"
        run([*atlahs, "-i", str(source), "-o", str(trace),
             "--format", "self-describing"], directory)
        result = subprocess.run([*simulator_args, "-goal", str(trace)], cwd=directory,
                                text=True, capture_output=True, timeout=30)
        if result.returncode == 0 or "unmatched messages or receives" not in result.stdout + result.stderr:
            raise RuntimeError("An unmatched explicit identity did not fail the simulation")
    print("PASS: ATLAHS matching, GPU/stream selection, explicit peer-GPU routing, legacy equivalence and private mappings")


if __name__ == "__main__":
    main()
