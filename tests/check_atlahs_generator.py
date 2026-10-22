"""Exercise ATLAHS's actual micro-event emitter, converter and NDP scheduler."""

import argparse
import io
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile


def emit(generator, output, mode, scenario="p2p", pairing="aligned"):
    sys.path.insert(0, generator)
    from generator_modules.data_dependency_modules.inter_node_dependency import (
        get_inter_node_microevents_dependency,
    )

    events = {0: {}, 1: {}}
    comms = {}
    counters = {0: {}, 1: {}}
    intervals = {}
    # Rotating communicator peers changes host-local GPU pairs without changing
    # each host's four global trace IDs. It catches local-index routing guesses.
    receivers = ["40", "42", "45", "50"]
    if pairing == "crossed":
        receivers = receivers[2:] + receivers[:2]
    # Sparse/global trace IDs must become dense GPU IDs local to each host.
    for gpu, (sender, receiver) in enumerate(zip(["10", "12", "15", "20"], receivers)):
        comm = f"comm-{gpu}"
        comms[comm] = {"gpuId_To_rank": {sender: "0", receiver: "1"},
                       "rank_To_rankInfo": {
                           "0": {"gpuId": sender, "goal_rank": 0},
                           "1": {"gpuId": receiver, "goal_rank": 1}}}
        if scenario != "p2p":
            comms[comm]["nranks"] = 2
            for rank in range(2):
                peer = str(1 - rank)
                comms[comm]["rank_To_rankInfo"][str(rank)]["channel_info"] = {
                    "Ring": [{"previous_rank": peer, "next_rank": peer}] * 2,
                    "Tree": [{"parent_rank": "-1" if rank == 0 else "0",
                              "child_1_rank": "1" if rank == 0 else "-1",
                              "child_2_rank": "-1", "child_3_rank": "-1"}] * 2}
        for rank, gpu_id, kind, peer in [(0, sender, "Send", "1"),
                                          (1, receiver, "Recv", "0")]:
            sequence = (1 << 40) + gpu if mode == "wide" else 0
            communicator = (1 << 48) + gpu if mode == "wide" else gpu
            event = {"event_type": kind, "commId": comm, "peer_rank": peer,
                     "seq": sequence, "protocol": "2", "chunkSize": 1000000,
                     "count": 1000000, "comm_index": communicator}
            if scenario != "p2p":
                event.update(event_type="AllReduce", algorithm="1" if scenario == "ring" else "0",
                             protocol="2" if scenario == "ring" else "0", type_size=4,
                             chunkSteps=1, sliceSteps=1, stepSize=2048,
                             elems=[dict(chunkCount=64, workOffset=channel * 128,
                                         workCount=128, lastChunkCount=64) for channel in range(2)])
            events[rank][gpu_id] = {"0": [{"ts_group_gpu_start": 100,
                                          "ts_group_gpu_end": 101,
                                          "events": [event]}]}
            counters[rank][gpu_id] = {comm: {kind: {peer: {sequence: {0: []}}}}}
            if scenario != "p2p":
                counters[rank][gpu_id] = {comm: {"AllReduce": {sequence: {
                    channel: {"send": {peer: []}, "recv": {peer: []}} for channel in range(2)}}}}
            intervals[gpu_id] = {"start": 0, "end": 1000}
    if pairing == "missing":
        comms["comm-0"]["rank_To_rankInfo"]["1"]["gpuId"] = "9999"
    options = {"zero_red_copy": True, "unique_nic": mode == "legacy"}
    if mode in ("explicit", "wide"):
        options["explicit_fields"] = True
    get_inter_node_microevents_dependency(events, {}, comms, counters,
                                          output, intervals, **options)


def main():
    from check_atlahs_simulation import finishing_times, run

    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["generator", "upstream", "converter", "plain-fields", "atlahs-fields", "simulator", "topology"]:
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--upstream-revision", required=True)
    args = parser.parse_args()
    plain = [str(args.converter.resolve()), "--custom-fields", str(args.plain_fields.resolve())]
    atlahs = [str(args.converter.resolve()), "--custom-fields", str(args.atlahs_fields.resolve())]
    sys.path.insert(0, str(args.generator.resolve()))
    from generator_modules.data_dependency_modules.message_fields import format_message_fields
    # Minimum-width decimal padding could merge distinct component tuples.
    first = (1, 100, 0, 5, 0)
    second = (11, 0, 0, 5, 0)
    if format_message_fields(*first, False) != format_message_fields(*second, False):
        raise RuntimeError("The legacy collision fixture is invalid")
    if format_message_fields(*first, True) == format_message_fields(*second, True):
        raise RuntimeError("Distinct message identities were merged")
    maximum = (1 << 64) - 1
    if f"chunk_id {maximum}" not in format_message_fields(maximum, "123", 1 << 40, 5, 1 << 48, True)[1]:
        raise RuntimeError("The emitter lost a full-width identity component")
    for invalid in [-1, maximum + 1, 1.5, True, "1.5", "-1", ""]:
        try:
            format_message_fields(invalid, 0, 0, 5, 0, True)
        except ValueError:
            pass
        else:
            raise RuntimeError(f"The emitter accepted an invalid component: {invalid!r}")
    try:
        format_message_fields(0, 0, 0, 6, 0, True)
    except ValueError:
        pass
    else:
        raise RuntimeError("The emitter accepted an unknown message kind")
    with tempfile.TemporaryDirectory(prefix="atlahs-generator-") as temporary:
        directory = Path(temporary)
        # Read the pristine generator from the pinned commit, not the edited
        # working tree. This keeps the legacy comparison independent of our fork.
        baseline = directory / "upstream-generator"
        baseline.mkdir()
        archive = subprocess.run(
            ["git", "-C", str(args.upstream.resolve()), "archive", "--format=tar",
             f"{args.upstream_revision}:goal_gen/ai/nccl_goal_generator"],
            check=True, capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as source:
            source.extractall(baseline, filter="data")
        outputs = {}
        for name, generator, mode in [("upstream", baseline, "legacy"),
                                       ("legacy", args.generator, "legacy"),
                                       ("upstream-shared", baseline, "shared"),
                                       ("shared", args.generator, "shared"),
                                       ("explicit", args.generator, "explicit"),
                                       ("wide", args.generator, "wide")]:
            output = directory / f"{name}.goal"
            run([sys.executable, "-c",
                 "import runpy,sys; runpy.run_path(sys.argv[1])['emit'](*sys.argv[2:])",
                 str(Path(__file__).resolve()), str(generator.resolve()), str(output), mode], directory)
            outputs[name] = output
        if outputs["upstream"].read_bytes() != outputs["legacy"].read_bytes():
            raise RuntimeError("The generator edits changed legacy output")
        if outputs["upstream-shared"].read_bytes() != outputs["shared"].read_bytes():
            raise RuntimeError("The generator edits changed default shared-NIC output")
        incompatible = subprocess.run(
            [sys.executable, str(args.generator.resolve() / "get_traced_events.py"),
             "-i", "unused", "-o", "unused", "--explicit-fields", "--unique-nic"],
            cwd=directory, text=True, capture_output=True, timeout=30)
        if incompatible.returncode != 2 or "not allowed with" not in incompatible.stderr:
            raise RuntimeError("The generator CLI did not reject conflicting GPU encoding modes")
        operations = [line for line in outputs["explicit"].read_text().splitlines()
                      if re.match(r"l\d+:", line)]
        for line in operations:
            if " cpu 0 " not in line:
                raise RuntimeError(f"Stream metadata still encoded in CPU: {line}")
            for field in ("gpu_id", "peer_gpu_id", "stream_id", "communicator_id", "channel_id", "sequence_id", "message_kind", "chunk_id"):
                if len(re.findall(rf"\b{field} \d+", line)) != 1:
                    raise RuntimeError(f"Operation does not supply exactly one {field}: {line}")
            if ": calc " not in line and " tag 0 " not in line:
                raise RuntimeError(f"Message identity is still hidden in tag: {line}")
        for kind in ["send", "recv"]:
            gpu_ids = set()
            for line in operations:
                if f": {kind} " in line:
                    if " nic 0 " not in line:
                        raise RuntimeError(f"GPU metadata still encoded in NIC: {line}")
                    gpu_ids.add(int(re.search(r"gpu_id (\d+)", line)[1]))
            if gpu_ids != {0, 1, 2, 3}:
                raise RuntimeError(f"Generator lost local GPU IDs for {kind}: {gpu_ids}")
        # Check the real emitters' peer endpoints independently of historical
        # simulation times, which cannot validate cross-GPU routing.
        for scenario in ("p2p", "ring", "tree"):
            source = directory / f"crossed-{scenario}.goal"
            run([sys.executable, "-c",
                 "import runpy,sys; runpy.run_path(sys.argv[1])['emit'](*sys.argv[2:])",
                 str(Path(__file__).resolve()), str(args.generator.resolve()), str(source),
                 "explicit", scenario, "crossed"], directory)
            count = 0
            directions = set()
            for line in source.read_text().splitlines():
                if not re.match(r"l\d+:", line):
                    continue
                peer = int(re.search(r"\bpeer_gpu_id (\d+)", line)[1])
                if ": calc " in line:
                    if peer != 0:
                        raise RuntimeError(f"Calculation has nonzero peer GPU: {line}")
                    continue
                local = int(re.search(r"\bgpu_id (\d+)", line)[1])
                if peer != (local + 2) % 4:
                    raise RuntimeError(f"Incorrect cross-GPU peer in {scenario}: {line}")
                count += 1
                directions.add("send" if ": send " in line else "recv")
            if not count or directions != {"send", "recv"}:
                raise RuntimeError(f"Cross-GPU {scenario} did not exercise both directions")
        invalid = subprocess.run(
            [sys.executable, "-c",
             "import runpy,sys; runpy.run_path(sys.argv[1])['emit'](*sys.argv[2:])",
             str(Path(__file__).resolve()), str(args.generator.resolve()),
             str(directory / "missing-peer.goal"), "explicit", "p2p", "missing"],
            cwd=directory, text=True, capture_output=True, timeout=30)
        if invalid.returncode == 0 or "Cannot resolve peer GPU" not in invalid.stderr:
            raise RuntimeError("Unresolved peer GPU was not rejected clearly")
        times = {}
        for name in ["legacy", "explicit", "wide"]:
            binary = directory / f"{name}.bin"
            converter = plain if name == "legacy" else atlahs
            run([*converter, "-i", str(outputs[name]), "-o", str(binary),
                 "--format", "legacy" if name == "legacy" else "self-describing"], directory)
            original = binary.read_bytes()
            output = run([str(args.simulator.resolve()), "-goal", str(binary),
                          "-topo", str(args.topology.resolve()), "-nodes", "8",
                          "-strat", "ecmp_host", "-paths", "128", "-q", "1000000", "-seed", "4"],
                         directory)
            times[name] = finishing_times(output, ranks=2)
            if "HTSIM nodes=8" not in output or binary.read_bytes() != original:
                raise RuntimeError("Incorrect GPU endpoint mapping or modified binary input")
        if times["explicit"] != times["legacy"] or times["wide"] != times["legacy"]:
            raise RuntimeError(f"Generated explicit GPU workload changed behavior: {times}")
        # Collective emitters have separate paths from Send/Recv. Exercise a
        # multi-channel ring (Simple) and tree (LL), not only synthetic messages.
        for scenario in ("ring", "tree"):
            collective_times = {}
            legacy_text = None
            for name, generator, mode in [("upstream", baseline, "legacy"),
                                           ("legacy", args.generator, "legacy"),
                                           ("explicit", args.generator, "explicit"),
                                           ("wide", args.generator, "wide")]:
                source = directory / f"{scenario}-{name}.goal"
                run([sys.executable, "-c",
                     "import runpy,sys; runpy.run_path(sys.argv[1])['emit'](*sys.argv[2:])",
                     str(Path(__file__).resolve()), str(generator.resolve()), str(source), mode, scenario], directory)
                if name == "upstream":
                    legacy_text = source.read_bytes()
                    continue
                if name == "legacy" and source.read_bytes() != legacy_text:
                    raise RuntimeError(f"The generator edits changed legacy {scenario} output")
                binary = source.with_suffix(".bin")
                converter = plain if name == "legacy" else atlahs
                run([*converter, "-i", str(source), "-o", str(binary),
                     "--format", "legacy" if name == "legacy" else "self-describing"], directory)
                output = run([str(args.simulator.resolve()), "-goal", str(binary),
                              "-topo", str(args.topology.resolve()), "-nodes", "8",
                              "-strat", "ecmp_host", "-paths", "128", "-q", "1000000", "-seed", "4"], directory)
                collective_times[name] = finishing_times(output, ranks=2)
            if any(result != collective_times["legacy"] for result in collective_times.values()):
                raise RuntimeError(f"Explicit {scenario} identities changed behavior: {collective_times}")
        print(f"PASS: upstream emitter → explicit message fields → packed binary → htsim: {times['explicit']}")


if __name__ == "__main__":
    main()
