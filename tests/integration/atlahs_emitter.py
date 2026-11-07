"""A tiny NCCL trace through ATLAHS's event emitter, for test_atlahs_generator.py.

Each call runs in its own process, `python atlahs_emitter.py GENERATOR OUTPUT MODE [SCENARIO
[PAIRING]]`, because the upstream and the edited generator have the same module names.
"""

import sys


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
    # each host's four global trace IDs, so guessing the peer's local index fails.
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


if __name__ == "__main__":
    emit(*sys.argv[1:])
