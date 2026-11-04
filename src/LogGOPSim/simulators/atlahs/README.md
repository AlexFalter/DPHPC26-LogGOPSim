# ATLAHS adapter

This adapter lets the [ATLAHS htsim backend](https://github.com/spcl/atlahs/tree/main/sim/htsim-backend) read self-describing traces through the same reader as LogGOPSim. ATLAHS keeps its own GOAL scheduler and packet-level network simulation; only trace loading goes through the shared reader.

```text
                              ┌─ LogGOPSimAdapter → LogGOPSim
GOAL → txt2bin → packed trace ┤
                              └─ AtlahsAdapter → ATLAHS scheduler → htsim (NDP)
```

Each simulation maps the trace privately, so dependency-counter updates never reach the file and several simulators can read the same trace. Traces with explicit ATLAHS message identities (below) only run on this backend; LogGOPSim rejects them because it matches messages by numeric tag.

## Source

This folder holds only our side of the integration: the adapter, the build and reference scripts, an example and this README. ATLAHS itself is a separate repository, the fork [mgman03/atlahs](https://github.com/mgman03/atlahs/tree/packed-trace-integration), branch `packed-trace-integration`, forked from [spcl/atlahs](https://github.com/spcl/atlahs) at commit `fb51a99f908e550318056ebb3e084f3d2fff55bd`. The fork uses this repository's trace reader and adapter; it does not use the LogGOPSim simulator. Upstream licenses and documentation are unchanged.

From the LogGOPSim root, clone the fork next to this repository:

```sh
git clone --branch packed-trace-integration https://github.com/mgman03/atlahs.git ../atlahs
```

ATLAHS's own application submodules are not needed. The changes are in `goal_gen/ai/nccl_goal_generator/` (generator) and in `sim/htsim-backend/sim/lgs/` and `datacenter/` (loader and scheduler). To see all of them, look at the [comparison with upstream](https://github.com/spcl/atlahs/compare/main...mgman03:atlahs:packed-trace-integration), or run:

```sh
git -C ../atlahs diff fb51a99f908e550318056ebb3e084f3d2fff55bd
```

[`UPSTREAM_REVISION`](UPSTREAM_REVISION) records the fork point. The reference tests extract that commit from the checkout with `git archive`, so their baseline does not depend on our edits.

## Build

ATLAHS is optional and off by default. With the fork cloned at `../atlahs`:

```sh
cmake -S src -B build -DBUILD_ATLAHS=ON
cmake --build build --target txt2bin atlahs_htsim -j4
```

ATLAHS's Makefiles write objects next to the sources, so `integration/build.py` copies the backend to `build/atlahs-htsim` and builds it there. Edit the files in `../atlahs` and rebuild; the copy is refreshed each time. To use a checkout somewhere else, set `-DATLAHS_SOURCE_DIR=/path/to/atlahs`.

The backend is compiled as C++23 so that it can include the shared reader. This needed three small source fixes: a `std::string` passed to `printf`, an upstream variable named `print` that clashes with `std::print` (now `debug_print`), and a warning flag for the `register` keyword in the old random-number generator. The fork also removes the prebuilt executables and object files that upstream had committed.

## Running a trace

The converter must know the eight unsigned ATLAHS fields. Either copy these declarations into `config/custom_fields.toml`, or pass `--custom-fields tests/atlahs_custom_fields.toml` to `txt2bin`:

```toml
[[fields]]
name = "gpu_id"
type = "unsigned"

[[fields]]
name = "communicator_id"
type = "unsigned"

[[fields]]
name = "channel_id"
type = "unsigned"

[[fields]]
name = "sequence_id"
type = "unsigned"

[[fields]]
name = "message_kind"
type = "unsigned"

[[fields]]
name = "chunk_id"
type = "unsigned"

[[fields]]
name = "stream_id"
type = "unsigned"

[[fields]]
name = "peer_gpu_id"
type = "unsigned"
```

Every operation, including calculations, carries all eight. Sends and receives use `tag 0`, since the fields identify the message. The example has two host ranks with four GPUs each:

```sh
build/txt2bin \
  -i src/LogGOPSim/simulators/atlahs/examples/two_rank_four_gpu.goal \
  -o /tmp/gpu-example.bin --format self-describing

build/atlahs-htsim/datacenter/htsim_ndp \
  -goal /tmp/gpu-example.bin \
  -topo build/atlahs-htsim/datacenter/topologies/leaf_spine_tiny.topo \
  -nodes 8 -strat ecmp_host -paths 128 -q 1000000 -seed 4
```

For other traces, choose a topology with `ranks × GPUs per rank` endpoints and pass that number as `-nodes`. Legacy binaries still load through the original reader.

## Generating traces from Nsight profiles

The NCCL generator writes the explicit fields when given `--explicit-fields`. It needs `numpy`, `scipy`, `pyyaml` and `tqdm`:

```sh
(
  cd ../atlahs/goal_gen/ai/nccl_goal_generator
  python get_traced_events.py \
    -i /path/to/nsys-profiles -o /tmp/generated-goal \
    --explicit-fields --zero-red-copy
)

build/txt2bin \
  -i /tmp/generated-goal/InterNode_MicroEvents_Dependency.goal \
  -o /tmp/generated.bin --format self-describing \
  --custom-fields tests/atlahs_custom_fields.toml
```

`--zero-red-copy` sets reduction and copy times to zero, so the example needs no benchmark data; leave it out when you have upstream's benchmark inputs. Only the final `InterNode_MicroEvents_Dependency.goal` is in the new syntax; the intermediate files keep upstream's.

With `--explicit-fields`, each operation gets:

- `gpu_id`: the GPU's index within its host (0, 1, …), in numeric order of the trace's global GPU IDs.
- `peer_gpu_id`: for a send the destination GPU, for a receive the source GPU, as an index on the peer's host. It is resolved from the communicator's peer mapping for point-to-point, ring and tree messages.
- `communicator_id`, `channel_id`, `sequence_id`, `message_kind`, `chunk_id`: the parts of NCCL's message identity that upstream concatenates into one decimal tag. `message_kind` uses upstream's values 0–5.
- `stream_id`: the execution stream, which upstream stores in `cpu`.

The standard `cpu`, `nic` and `tag` fields are zero. Calculations use zero for the message fields and for `peer_gpu_id`. The start and end fences of each rank use GPU 0. The generator rejects negative, fractional or out-of-range values. `--explicit-fields` cannot be combined with `--unique-nic`, and `--explicit-gpu` is accepted as an alias. Without the flag, the output is byte-identical to upstream's.

## How the adapter works

`AtlahsAdapter` derives from `SimulatorAdapter<AtlahsNode>`, which wraps the shared `TraceReader`. `AtlahsNode` adds Type, Peer, Size, Tag, CPU and NIC; 32-bit `Gpu`, `PeerGpu` and `Stream`; and 64-bit `Communicator`, `Channel`, `Sequence`, `MessageKind` and `Chunk`. The reader fills the dependency lists. `start_time` is scheduler state and is not stored in the trace. The adapter declares fourteen field bindings and which values mean "any source".

**Loading.** The ATLAHS parser checks the file signature. For packed files it uses the adapter but keeps ATLAHS's mapping and ready queues, and its five dependency-release paths call the shared reader to decrement counters. For legacy files nothing changes: they keep the original reader, tag matching and GPU mapping, with the NIC value copied into `Gpu` and `PeerGpu` and the CPU value into `Stream`.

**Matching.** A receive matches a message if the source agrees (or either side is a wildcard) and all five identity fields are equal. For explicit traces the matcher also requires the GPU endpoints to agree at both ends. Legacy traces are matched by tag, so the two kinds never match each other. A run that ends with unmatched sends or receives fails.

**Routing.** Each rank is a host. A GPU endpoint is `rank × gpu_count + gpu_id`, where `gpu_count` is the largest `gpu_id` in the file plus one, so GPU IDs should be dense. Sends route from the sender's `gpu_id` to `peer_gpu_id`. Upstream used the sender's GPU index at both ends, which routed messages to the wrong GPU whenever the two indices differed. GPU clocks use `Gpu` and execution resources use `Stream`, with as many streams as the largest `stream_id` plus one.

**Limits.**
- htsim's message API takes an `int` byte count, so the adapter rejects sends and receives larger than `INT_MAX`; calculation durations keep the full 64 bits.
- `save_mem` (destructive mapping) is not supported.
- A packed trace missing any of the eight fields fails at load time. Other custom fields are ignored.
- The adapter runs inside htsim's own executable; LogGOPSim cannot select it at run time.
- Only the NDP transport has been tested; EQDS and UEC have not.
- Other upstream generators are unchanged.

## Tests

Pass `-DATLAHS_GENERATOR_PYTHON=/path/to/venv/bin/python` to CMake if the generator's dependencies are installed in a separate environment. Then:

```sh
cmake --build build --target atlahs_generator txt2bin test_trace_reader test_atlahs_matching -j4
ctest --test-dir build --output-on-failure -R '^(trace_reader|atlahs_matching|atlahs_generator|atlahs_simulation|atlahs_upstream_ndp)$'
```

- **`trace_reader`**: the adapter's field requirements, full-width values, invalid tags and message kinds, graph metadata and the message-size limit.
- **`atlahs_matching`**: the production matcher with identities that differ in one field each, source wildcards, legacy/explicit separation and receive order.
- **`atlahs_generator`**: runs the real event emitter on two-host, four-GPU point-to-point, ring and tree AllReduce inputs. It checks that default output is byte-identical to the pristine generator extracted from the fork point, checks the explicit fields, then converts and simulates the output. It also covers a case where upstream's decimal tags collide, and invalid field values.
- **`atlahs_simulation`**: simulates explicit-field traces and the equivalent unique-tag legacy traces and requires identical finish times. Cases: a broadcast, compute/reply workloads, message pairs differing in one identity field under both arrival orders, and messages received on a different GPU than they were sent from. It also checks that a trace with NIC 7 but `gpu_id` 0–3 gives the same times as the NIC-based legacy trace, and that moving all traffic to GPU 0 changes them. With `ATLAHS_TRACE_ROUTES=1` it reads the transport endpoints, for example 0→4, 0→6 and 2→4 for GPU pairs 0→0, 0→2 and 2→0 between two four-GPU hosts. Finally, it checks that inconsistent peer GPUs and unmatched messages fail and that input files stay unchanged.
- **`atlahs_upstream_ndp`**: runs upstream's own [test runner](https://github.com/spcl/atlahs/blob/fb51a99f908e550318056ebb3e084f3d2fff55bd/sim/htsim-backend/sim/tests/tests.py) against its stored `dumbell_ndp/default.ref`, to check that the NDP network code itself is unchanged. Our wrapper turns the runner's printed failures into a non-zero exit code.

Upstream's [Docker quick test](https://github.com/spcl/atlahs#running-a-quick-test) and `scripts/run_validation_exp.py` have not been run with this integration. Upstream's HPC demo uses UEC, which is untested here.

### Recorded-trace comparison

`tests/check_atlahs_recorded.py` compares the two pipelines on a real recording, upstream's four-host, sixteen-GPU Llama quick-test dataset:

- **Baseline:** the pinned upstream generator, converter and backend. Instrumentation prints the matches upstream makes, and the upstream converter needed an explicit `int` return type on `main` to compile with current compilers. Nothing else changes.
- **New:** our generator with `--explicit-fields`, the packed converter and the adapted backend.

Both use `--zero-red-copy`. The test requires identical matched send/receive pairs and per-rank finish times, and every send must be matched.

Download URLs, sizes and SHA-256 checksums are in `tests/atlahs_recorded_traces.json`. Inputs and outputs stay under `build/`; allow several GB of disk space. To export the reports to SQLite on Apple Silicon, build the pinned Nsight exporter image:

```sh
docker build --platform linux/arm64 -t dhpc-nsys-export:2026.3.1 \
  -f src/LogGOPSim/simulators/atlahs/integration/Dockerfile.nsys-export \
  src/LogGOPSim/simulators/atlahs/integration
docker run --rm dhpc-nsys-export:2026.3.1 --version
```

Download the four reports to `build/atlahs-recorded/nsys_reports`, then export them (no GPU needed):

```sh
mkdir -p build/atlahs-recorded/sqlite
docker run --rm --entrypoint sh \
  -v "$PWD/build/atlahs-recorded/nsys_reports:/input:ro" \
  -v "$PWD/build/atlahs-recorded/sqlite:/output" \
  dhpc-nsys-export:2026.3.1 -c '
    set -e
    for report in /input/*.nsys-rep; do
      name=$(basename "$report" .nsys-rep)
      nsys export --type=sqlite --force-overwrite=true --output="/output/$name.sqlite" "$report"
    done'
```

Build the reference and run the test; the topology matches the sixteen GPUs:

```sh
cmake -S src -B build -DBUILD_ATLAHS=ON \
  -DATLAHS_GENERATOR_PYTHON=/path/to/venv/bin/python \
  -DATLAHS_RECORDED_TRACE_DIR="$PWD/build/atlahs-recorded/sqlite"
cmake --build build --target atlahs_reference atlahs_htsim txt2bin -j4
ctest --test-dir build -R '^atlahs_recorded$' --output-on-failure
```

Each run writes its logs and a `result.json` (input and executable hashes, commands, match counts, finish times) to `build/atlahs-recorded-run/run-*`, also when it fails. The test fails if the exporter is missing or a run does not finish.

**Result.** The comparison passed: 5,680,774 operations, 475,452 identical matched pairs and identical finish times on all four hosts. The latest finish time was 2,067,317,935 ns in both, and the test took 364 seconds. The packed trace is 116,773,549 bytes against 252,330,728 for legacy, with 17-byte instead of 39-byte node records. This measures correctness and size, not speed.

In this recording, 825,184 sends and receives have decimal tags above `UINT32_MAX`. The upstream converter silently truncates them, while ours rejects them, so the baseline uses upstream's converter to reproduce the original results. The explicit-field path stores each identity field separately and needs no truncation. That truncation caused no mismatches in this recording does not show it is safe in general.

This run predates `stream_id` and has not been repeated since. Stream handling is covered by scheduler tests with stream IDs 1024 and 1025 and unrelated CPU IDs, which check legacy-equivalent timing and the effect of two operations sharing a stream.
