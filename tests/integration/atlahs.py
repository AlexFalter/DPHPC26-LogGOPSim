"""Running ATLAHS's NDP simulator and writing GOAL with ATLAHS message identities."""

from helpers import host_times, run


def simulate_ndp(htsim, topology, trace, *, nodes=8, cwd=None, env=None, check=True):
    """Runs htsim_ndp on the trace with the settings of ATLAHS's examples."""
    return run([htsim, "-goal", trace, "-topo", topology, "-nodes", nodes, "-strat", "ecmp_host",
                "-paths", "128", "-q", "1000000", "-seed", "4"], cwd=cwd, env=env, check=check)


def finishing_times(output, ranks=8):
    """The host finish times of a run that completed every rank's workload."""
    times = host_times(output)
    assert len(times) == ranks and "LGS terminates!" in output and "queue on host" not in output, (
        f"ATLAHS did not finish the {ranks}-rank workload:\n{output}")
    return times


def explicit_attributes(gpu=0, stream=0, peer_gpu=None, communicator=0, channel=0, sequence=0, kind=5, chunk=0):
    """The eight ATLAHS custom fields of one operation."""
    return (f" stream_id {stream} gpu_id {gpu} communicator_id {communicator} channel_id {channel}"
            f" sequence_id {sequence} message_kind {kind} chunk_id {chunk}"
            f" peer_gpu_id {gpu if peer_gpu is None else peer_gpu}")
