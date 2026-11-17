#!/usr/bin/env python3
"""Parametric GOAL workload generator for the benchmark harness.

usage: goalgen.py WORKLOAD --ranks P --records N [--schema inferred|custom|wide] -o OUT.goal
                  [--jobs J] [--plain-out OUT2.goal] [--seed S] [--param key=value ...]
       goalgen.py --list

Writes a GOAL text schedule with about N records (operations) on P ranks: the workload's
per-iteration pattern is repeated, and the iteration count is chosen from N, so trace size and
rank count are independent axes. Every send has exactly one matching receive of the same size and
tag (so the eager/rendezvous bug fixed in NEW is never triggered and BASE and NEW must agree).
Output is deterministic for given arguments. --jobs J writes rank ranges in parallel processes.

Schemas are produced through the data, because txt2bin always infers the narrowest field widths:
  inferred  realistic values: the writer narrows every field ("narrowed" schema)
  custom    inferred + four custom fields: comm_id (64-bit NCCL-like communicator hash),
            gpu_id, stream_id, channel (declare them with bench/workloads/custom_fields.toml)
  wide      standard fields spread over nearly their legacy widths: compute sizes over 40 bits,
            message sizes over 24 bits, tags over 31 bits, cpu and nic over 8 bits
--plain-out additionally writes the same schedule without custom fields (BASE cannot parse them).

Workloads (motivation in bench/README.md, "Workloads"):
  stencil3d      7-point halo exchange on a periodic 3D process grid (HPC proxy apps)
  allreduce_ring bandwidth-optimal ring allreduce (reduce-scatter + allgather), MPI/NCCL large msgs
  allreduce_rd   recursive-doubling allreduce (latency-optimal, power-of-two P)
  alltoall       linear all-to-all, all operations posted at once (widest frontier)
  dp_nccl        data-parallel training step: per-layer backward compute overlapped with
                 bucketed, channel-split NCCL ring allreduces (ATLAHS-style AI trace)
  pipeline_1f1b  pipeline-parallel 1F1B schedule over P stages (long dependency chains)
  moe_alltoall   mixture-of-experts layers: skewed all-to-all dispatch/combine around experts
"""
import argparse
import math
import os
import shutil
import sys
from concurrent.futures import ProcessPoolExecutor

# Bump when generated output changes: gen_traces.py regenerates traces from an older version.
VERSION = 3

MASK64 = (1 << 64) - 1
PRIME32 = 4294967291  # largest prime < 2^32; tags stay below ANY (2^32 - 1)


def mix(x):
    x = (x + 0x9E3779B97F4A7C15) & MASK64
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & MASK64
    return x ^ (x >> 31)


class Emitter:
    """Writes one rank's operations and dependencies as GOAL text to out, in chunks (one rank of a
    1e9-record trace has tens of millions of lines, too many to hold in memory)."""

    FLUSH_LINES = 1 << 16

    def __init__(self, rank, schema, seed, out):
        self.out = out
        out.write(f"rank {rank} {{\n")
        self.rank = rank
        self.schema = schema
        self.seed = seed
        self.label = 0
        self.lines = []
        self.records = 0
        self.edges = 0
        self.wide = schema == "wide"
        self.custom = schema == "custom"
        self.gpu = rank % 8

    def _extra(self, stream, channel, comm, calc=False):
        # Upstream's grammar: "calc SIZE [cpu C]" and "send|recv SIZEb to|from P [tag T] [cpu C] [nic N]",
        # in this order; custom fields (NEW only) follow.
        parts = ""
        if self.wide:
            h = mix(self.seed ^ (self.rank << 32) ^ self.label)
            # 0..254: upstream returns max_cpu+1 / max_nic+1 as uint8_t, so 255 overflows to 0 CPUs/NICs
            # and BASE crashes (a bug NEW fixes); staying below keeps BASE as the reference.
            cpu, nic = h % 255, (h >> 8) % 255
            parts += f" cpu {cpu}" if calc else f" cpu {cpu} nic {nic}"
        if self.custom:
            parts += f" comm_id {comm} gpu_id {self.gpu} stream_id {stream} channel {channel}"
        return parts

    def _next(self):
        self.label += 1
        self.records += 1
        return self.label

    def calc(self, size, stream=0, comm=0):
        label = self._next()
        if self.wide:
            size = (mix(self.seed ^ 0xC0FFEE ^ (self.rank << 24) ^ label) & ((1 << 40) - 1)) + 1
        self.lines.append(f"l{label}: calc {size}{self._extra(stream, 0, comm, calc=True)}")
        return label

    def _message(self, size, tag, it):
        if self.wide:  # a function of (tag, iteration) only, so sender and receiver agree
            key = mix(self.seed ^ (tag << 20) ^ it)
            size = (key & ((1 << 24) - 1)) + 1
            tag = (tag * 2654435761 + it * 40503 + 12345) % PRIME32
        return size, tag

    def send(self, size, peer, tag, it, channel=0, comm=None, stream=1):
        comm = COMM_WORLD if comm is None else comm
        size, tag = self._message(size, tag, it)
        label = self._next()
        self.lines.append(f"l{label}: send {size}b to {peer} tag {tag}{self._extra(stream, channel, comm)}")
        return label

    def recv(self, size, peer, tag, it, channel=0, comm=None, stream=1):
        comm = COMM_WORLD if comm is None else comm
        size, tag = self._message(size, tag, it)
        label = self._next()
        self.lines.append(f"l{label}: recv {size}b from {peer} tag {tag}{self._extra(stream, channel, comm)}")
        return label

    def requires(self, a, b):  # a starts after b finished
        if a and b:
            self.lines.append(f"l{a} requires l{b}")
            self.edges += 1
            if len(self.lines) >= self.FLUSH_LINES:
                self.flush()

    def irequires(self, a, b):  # a starts after b started
        if a and b:
            self.lines.append(f"l{a} irequires l{b}")
            self.edges += 1
            if len(self.lines) >= self.FLUSH_LINES:
                self.flush()

    def flush(self):
        if self.lines:
            self.out.write("\n".join(self.lines))
            self.out.write("\n")
            self.lines.clear()

    def close(self):
        self.flush()
        self.out.write("}\n\n")


# ---------------------------------------------------------------- workloads
# Each workload: ops(P, params) = operations per rank and iteration, and gen(e, P, iters, params).

COMM_WORLD = 0x8F3B2A1C9D4E5F60  # NCCL-style 64-bit communicator hashes (custom schema)
COMM_PP = 0x1D2C3B4A59687706
COMM_EP = 0xF0E1D2C3B4A59687


def grid3(P):
    """Most cubic factorization px*py*pz = P."""
    best = (P, 1, 1)
    for x in range(1, int(round(P ** (1 / 3))) + 2):
        if P % x:
            continue
        rest = P // x
        for y in range(x, int(math.isqrt(rest)) + 1):
            if rest % y == 0:
                z = rest // y
                cand = tuple(sorted((x, y, z)))
                if max(cand) - min(cand) < max(best) - min(best):
                    best = cand
    return best


def stencil3d_ops(P, p):
    return 1 + 4 * sum(1 for n in grid3(P) if n > 1)


def stencil3d(e, P, iters, p):
    """Per iteration: compute, then send each face to its neighbour (after the compute) and receive
    the opposite neighbour's face (posted when the compute starts); the next compute needs all of
    them. Dimensions of extent 1 exchange nothing (no messages to self)."""
    dims = grid3(P)
    px, py, pz = dims
    r = e.rank
    x, y, z = r % px, (r // px) % py, r // (px * py)

    def at(i, j, k):
        return (i % px) + (j % py) * px + (k % pz) * px * py

    nbrs = [at(x + 1, y, z), at(x - 1, y, z), at(x, y + 1, z), at(x, y - 1, z), at(x, y, z + 1), at(x, y, z - 1)]
    face = [p["halo_x"], p["halo_x"], p["halo_y"], p["halo_y"], p["halo_z"], p["halo_z"]]
    opposite = [1, 0, 3, 2, 5, 4]
    directions = [d for d in range(6) if dims[d // 2] > 1]
    prev = []
    for it in range(iters):
        c = e.calc(p["compute"])
        for b in prev:
            e.requires(c, b)
        cur = []
        for d in directions:
            # My neighbour in direction opposite[d] sends its direction-d face to me with tag d.
            s = e.send(face[d], nbrs[d], d, it, channel=d)
            e.requires(s, c)
            q = e.recv(face[d], nbrs[opposite[d]], d, it, channel=d)
            e.irequires(q, c)
            cur += [s, q]
        prev = cur


def allreduce_ring_ops(P, p):
    return 5 * (P - 1) + 1


def allreduce_ring(e, P, iters, p):
    r = e.rank
    right, left = (r + 1) % P, (r - 1) % P
    chunk = max(1, p["bytes"] // P)
    last = 0
    for it in range(iters):
        c = e.calc(p["compute"])
        e.requires(c, last)
        dep = c
        for k in range(2 * (P - 1)):
            s = e.send(chunk, right, k, it)
            e.requires(s, dep)
            q = e.recv(chunk, left, k, it)
            e.requires(q, dep)
            if k < P - 1:  # reduce-scatter: reduce the received chunk before forwarding it
                red = e.calc(p["reduce"])
                e.requires(red, q)
                e.requires(red, s)
                dep = red
            else:
                dep = q
        last = dep


def allreduce_rd_ops(P, p):
    return 3 * int(math.log2(P)) + 1


def allreduce_rd(e, P, iters, p):
    if P & (P - 1):
        raise ValueError("allreduce_rd needs a power-of-two rank count")
    r = e.rank
    last = 0
    steps = int(math.log2(P))
    for it in range(iters):
        c = e.calc(p["compute"])
        e.requires(c, last)
        dep = c
        for k in range(steps):
            peer = r ^ (1 << k)
            s = e.send(p["bytes"], peer, k, it)
            q = e.recv(p["bytes"], peer, k, it)
            e.requires(s, dep)
            e.requires(q, dep)
            red = e.calc(p["reduce"])
            e.requires(red, s)
            e.requires(red, q)
            dep = red
        last = dep


def alltoall_ops(P, p):
    return 2 * (P - 1) + 1


def alltoall(e, P, iters, p):
    r = e.rank
    prev = []
    for it in range(iters):
        c = e.calc(p["compute"])
        for b in prev:
            e.requires(c, b)
        cur = []
        for k in range(1, P):
            s = e.send(p["bytes"], (r + k) % P, 7, it)
            q = e.recv(p["bytes"], (r - k) % P, 7, it)
            e.requires(s, c)
            e.irequires(q, c)
            cur += [s, q]
        prev = cur


def dp_nccl_ops(P, p):
    L, B, C = p["layers"], p["buckets"], p["channels"]
    return 2 * L + B * C * (5 * (P - 1)) + 1


def dp_nccl(e, P, iters, p):
    """One training step per iteration: forward over L layers, backward in reverse; after every
    L/B layers of backward a gradient bucket is allreduced with C parallel NCCL ring channels
    (each a chunked reduce-scatter + allgather), overlapping the remaining backward compute."""
    r = e.rank
    L, B, C = p["layers"], p["buckets"], p["channels"]
    right, left = (r + 1) % P, (r - 1) % P
    chunk = max(1, p["bucket_bytes"] // (P * C))
    step_end = []
    for it in range(iters):
        dep = 0
        for layer in range(L):
            f = e.calc(p["fwd"], stream=0)
            if layer == 0:
                for b in step_end:
                    e.requires(f, b)
            e.requires(f, dep)
            dep = f
        ends = []
        per_bucket = max(1, L // B)
        for layer in range(L):
            g = e.calc(p["bwd"], stream=0)
            e.requires(g, dep)
            dep = g
            if (layer + 1) % per_bucket == 0 and len(ends) < B * C:
                bucket = len(ends) // C
                for ch in range(C):
                    tag_base = (bucket * C + ch) * 2 * P
                    cd = g
                    for k in range(2 * (P - 1)):
                        s = e.send(chunk, right, tag_base + k, it, channel=ch, comm=COMM_WORLD)
                        q = e.recv(chunk, left, tag_base + k, it, channel=ch, comm=COMM_WORLD)
                        e.requires(s, cd)
                        e.requires(q, cd)
                        if k < P - 1:
                            red = e.calc(p["reduce"], stream=1, comm=COMM_WORLD)
                            e.requires(red, q)
                            e.requires(red, s)
                            cd = red
                        else:
                            cd = q
                    ends.append(cd)
        opt = e.calc(p["optimizer"], stream=0)
        e.requires(opt, dep)
        for b in ends:
            e.requires(opt, b)
        step_end = [opt]


def pipeline_1f1b_ops(P, p):
    return 6 * p["microbatches"]


def pipeline_1f1b(e, P, iters, p):
    """Stage s = rank. 1F1B: P-s-1 warm-up forwards, then alternate one forward/one backward,
    then drain the backwards. Every operation of a stage requires the previous one."""
    s, M = e.rank, p["microbatches"]
    act, grad = p["activation_bytes"], p["activation_bytes"]
    last = 0

    def chain(label):
        nonlocal last
        e.requires(label, last)
        last = label

    for it in range(iters):
        def forward(m):
            if s > 0:
                chain(e.recv(act, s - 1, 2 * m, it, comm=COMM_PP, stream=2))
            c = e.calc(p["fwd"])
            chain(c)
            if s < P - 1:  # asynchronous (isend): the stage goes on; a chained rendezvous send deadlocks 1F1B
                e.requires(e.send(act, s + 1, 2 * m, it, comm=COMM_PP, stream=2), c)

        def backward(m):
            if s < P - 1:
                chain(e.recv(grad, s + 1, 2 * m + 1, it, comm=COMM_PP, stream=2))
            c = e.calc(p["bwd"])
            chain(c)
            if s > 0:
                e.requires(e.send(grad, s - 1, 2 * m + 1, it, comm=COMM_PP, stream=2), c)

        warm = min(P - s - 1, M)
        f = b = 0
        for _ in range(warm):
            forward(f); f += 1
        while f < M:
            forward(f); f += 1
            backward(b); b += 1
        while b < M:
            backward(b); b += 1


def moe_alltoall_ops(P, p):
    return 2 + 4 * (P - 1)


def moe_alltoall(e, P, iters, p):
    """One MoE layer per iteration: attention compute, skewed all-to-all token dispatch, expert
    compute, all-to-all combine (same sizes back). Sizes depend on (src, dst, layer) only."""
    r = e.rank
    prev = 0

    def tokens(src, dst, it):  # skewed (Zipf-like) routing load, identical on both ends
        h = mix((src << 40) ^ (dst << 20) ^ it ^ 0xE0E)
        return p["token_bytes"] * (1 + (h % 64) ** 2 // 64)

    for it in range(iters):
        a = e.calc(p["attention"])
        e.requires(a, prev)
        disp = []
        for k in range(1, P):
            dst, src = (r + k) % P, (r - k) % P
            s = e.send(tokens(r, dst, it), dst, 11, it, comm=COMM_EP, stream=1)
            q = e.recv(tokens(src, r, it), src, 11, it, comm=COMM_EP, stream=1)
            e.requires(s, a)
            e.irequires(q, a)
            disp += [s, q]
        x = e.calc(p["expert"])
        for b in disp:
            e.requires(x, b)
        comb = []
        for k in range(1, P):
            dst, src = (r + k) % P, (r - k) % P
            s = e.send(tokens(dst, r, it), dst, 12, it, comm=COMM_EP, stream=1)
            q = e.recv(tokens(r, src, it), src, 12, it, comm=COMM_EP, stream=1)
            e.requires(s, x)
            e.irequires(q, x)
            comb += [s, q]
        j = e.calc(p["combine"])
        for b in comb:
            e.requires(j, b)
        prev = j


WORKLOADS = {
    "stencil3d": (stencil3d, stencil3d_ops,
                  dict(compute=50000, halo_x=32768, halo_y=32768, halo_z=131072)),
    "allreduce_ring": (allreduce_ring, allreduce_ring_ops, dict(compute=200000, reduce=2000, bytes=64 << 20)),
    "allreduce_rd": (allreduce_rd, allreduce_rd_ops, dict(compute=200000, reduce=2000, bytes=8192)),
    "alltoall": (alltoall, alltoall_ops, dict(compute=100000, bytes=4096)),
    "dp_nccl": (dp_nccl, dp_nccl_ops, dict(layers=24, buckets=4, channels=2, fwd=300000, bwd=600000,
                                           reduce=5000, optimizer=100000, bucket_bytes=100 << 20)),
    "pipeline_1f1b": (pipeline_1f1b, pipeline_1f1b_ops,
                      dict(microbatches=16, fwd=400000, bwd=800000, activation_bytes=8 << 20)),
    "moe_alltoall": (moe_alltoall, moe_alltoall_ops,
                     dict(attention=300000, expert=500000, combine=50000, token_bytes=4096)),
}


def iterations_for(workload, P, records, params):
    _, ops, _ = WORKLOADS[workload]
    return max(1, round(records / (P * ops(P, params))))


def _write_ranks(job):
    """Worker: writes ranks [lo, hi) to part files; returns (records, edges)."""
    workload, P, iters, params, schema, seed, lo, hi, out, plain_out = job
    gen = WORKLOADS[workload][0]
    records = edges = 0
    targets = [(out, schema)] + ([(plain_out, "inferred")] if plain_out else [])
    files = [open(path, "w", buffering=1 << 22) for path, _ in targets]
    try:
        for r in range(lo, hi):
            for index, (f, (_, sch)) in enumerate(zip(files, targets)):
                e = Emitter(r, sch, seed, f)
                gen(e, P, iters, params)
                e.close()
                if index == 0:
                    records += e.records
                    edges += e.edges
    finally:
        for f in files:
            f.close()
    return records, edges


def generate(workload, P, records, out, schema="inferred", jobs=1, plain_out=None, seed=20261112, params=None):
    if workload not in WORKLOADS:
        raise SystemExit(f"unknown workload {workload}; use --list")
    p = dict(WORKLOADS[workload][2])
    p.update(params or {})
    iters = iterations_for(workload, P, records, p)
    jobs = max(1, min(jobs, P))
    bounds = [P * j // jobs for j in range(jobs + 1)]
    parts = []
    for j in range(jobs):
        part = f"{out}.part{j}"
        plain_part = f"{plain_out}.part{j}" if plain_out else None
        parts.append((workload, P, iters, p, schema, seed, bounds[j], bounds[j + 1], part, plain_part))
    if jobs == 1:
        results = [_write_ranks(parts[0])]
    else:
        with ProcessPoolExecutor(jobs) as pool:
            results = list(pool.map(_write_ranks, parts))
    for target, index in [(out, 8)] + ([(plain_out, 9)] if plain_out else []):
        with open(target, "w") as f:
            f.write(f"num_ranks {P}\n\n")
            for job in parts:
                part = job[index]
                with open(part) as src:
                    shutil.copyfileobj(src, f, 1 << 24)
                os.remove(part)
    total_records = sum(r for r, _ in results)
    total_edges = sum(e for _, e in results)
    return {"workload": workload, "ranks": P, "iterations": iters, "records": total_records,
            "edges": total_edges, "schema": schema, "params": p, "seed": seed}


def parse_params(items):
    params = {}
    for item in items or []:
        key, _, value = item.partition("=")
        params[key] = int(value)
    return params


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("workload", nargs="?")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--ranks", type=int)
    ap.add_argument("--records", type=float)
    ap.add_argument("--schema", default="inferred", choices=["inferred", "custom", "wide"])
    ap.add_argument("-o", "--output")
    ap.add_argument("--plain-out")
    ap.add_argument("--jobs", type=int, default=1)
    ap.add_argument("--seed", type=int, default=20261112)
    ap.add_argument("--param", action="append", help="override a workload parameter, key=int")
    a = ap.parse_args()
    if a.list:
        for name, (_, ops, defaults) in WORKLOADS.items():
            print(f"{name:15s} ops/rank/iter(P=64)={ops(64, defaults):6d}  {defaults}")
        return
    if not (a.workload and a.ranks and a.records and a.output):
        ap.error("WORKLOAD, --ranks, --records and -o are required")
    info = generate(a.workload, a.ranks, int(a.records), a.output, a.schema, a.jobs, a.plain_out, a.seed,
                    parse_params(a.param))
    import json
    print(json.dumps(info))


if __name__ == "__main__":
    main()
