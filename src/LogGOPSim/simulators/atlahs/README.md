# ATLAHS adapter

The adapter lets [ATLAHS's htsim backend](https://github.com/spcl/atlahs/tree/main/sim/htsim-backend) read self-describing binaries through the same shared reader as LogGOPSim. ATLAHS supplies the GOAL scheduler and htsim packet simulation. `AtlahsAdapter` inherits `SimulatorAdapter<AtlahsNode>`, which holds the shared `TraceReader` for decoding, validation and graph operations. The scheduler calls `read_node(rank, index)` and receives an `AtlahsNode`.

```text
                             ┌─ LogGOPSimAdapter → LogGOPSim
GOAL → txt2bin → packed trace ┤
                             └─ AtlahsAdapter → ATLAHS scheduler → htsim/NDP
```

Each simulation uses its own private mmap, so counter updates do not change the file or affect another consumer. A trace must also use a matching convention supported by its consumer: explicit ATLAHS identities currently require the adapted ATLAHS backend. LogGOPSim rejects these messages because its matcher only understands numeric tags.

## Build

The readable ATLAHS source lives in [`upstream/`](upstream/), beside the adapter. It is a submodule of [our ATLAHS fork](https://github.com/mgman03/atlahs), based on revision `fb51a99f908e550318056ebb3e084f3d2fff55bd`. See [UPSTREAM.md](UPSTREAM.md) for source navigation, the pin and Git review commands.

Enable the optional backend in the existing build:

```sh
git submodule update --init src/LogGOPSim/simulators/atlahs/upstream
cmake -S src -B build -DBUILD_ATLAHS=ON
cmake --build build --target txt2bin atlahs_htsim -j4
```

`ATLAHS_SOURCE_DIR` defaults to this `upstream/` checkout; override it only to use another fork with the same integration edits. The default build does not compile ATLAHS or download it. The `atlahs_generator` target simply prints the generator source location; there is no preparation or patch-application step.

ATLAHS's Makefiles put objects next to source files, so `integration/build.py` copies the edited backend to `build/atlahs-htsim` and compiles it there. Edit files under `upstream/`, then rebuild; the build copy is replaced from those sources. Executables and objects remain in `build/`. Prebuilt executables and objects tracked by the original upstream commit have been removed from the local fork.

The backend is compiled as C++23 to use the shared reader. Our source edits also fix a diagnostic passing `std::string` to `printf` and rename an upstream `print` variable that conflicts with `std::print`. Its old RNG's `register` declarations are accepted with a compatibility warning flag.

## Run

Declare the eight unsigned attributes in `config/custom_fields.toml`, which the converter loads automatically, or pass the ready-made declarations with `txt2bin --custom-fields tests/atlahs_custom_fields.toml`:

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

Every operation supplies all eight attributes, including calculations. Messages use `tag 0`; their identities are carried by the named fields. The example has two host ranks with four GPUs each; standard NIC values are not used to identify GPUs:

```sh
build/txt2bin \
  -i src/LogGOPSim/simulators/atlahs/examples/two_rank_four_gpu.goal \
  -o /tmp/gpu-example.bin --format self-describing

build/atlahs-htsim/datacenter/htsim_ndp \
  -goal /tmp/gpu-example.bin \
  -topo build/atlahs-htsim/datacenter/topologies/leaf_spine_tiny.topo \
  -nodes 8 -strat ecmp_host -paths 128 -q 1000000 -seed 4
```

For other traces, configure the topology and node count according to ATLAHS's rank/resource mapping. The original legacy binary reader remains available in the fork.

## Generate explicit attributes from ATLAHS profiles

Run the edited upstream generator directly.

Use an interpreter with the upstream dependencies (`numpy`, `scipy`, `pyyaml`, `tqdm`), then run:

```sh
(
  cd src/LogGOPSim/simulators/atlahs/upstream/goal_gen/ai/nccl_goal_generator
  python get_traced_events.py \
    -i /path/to/nsys-profiles -o /tmp/generated-goal \
    --explicit-fields --zero-red-copy
)

build/txt2bin \
  -i /tmp/generated-goal/InterNode_MicroEvents_Dependency.goal \
  -o /tmp/generated.bin --format self-describing
```

The converter needs the eight TOML declarations shown above. `--zero-red-copy` disables benchmark-based reduction/copy timing for this example; omit it when supplying the normal upstream benchmark inputs.

`--explicit-fields` emits the dense host-local `gpu_id`, the remote `peer_gpu_id`, and separate communicator, channel, sequence, message-kind and chunk values on every final operation. IDs follow the generator's numeric ordering of global GPU trace IDs within each host. Standard CPU IDs, NICs and message tags remain zero. `stream_id` selects the execution stream. For sends, `peer_gpu_id` identifies the destination GPU; for receives it identifies the source GPU. The generator resolves it from communicator peer mappings for point-to-point, ring and tree messages. Calculations and fences supply `peer_gpu_id 0`. Calculations supply zero-valued message components; rank-wide fences also use GPU 0. `--unique-nic` and `--explicit-fields` are mutually exclusive. The old `--explicit-gpu` spelling remains an alias, but now emits the full set of fields. Without the new flag, upstream output stays unchanged. Convert the final `InterNode_MicroEvents_Dependency.goal` file; intermediate diagnostic files retain upstream syntax.

## Adapter boundary

`AtlahsNode` extends `BaseDeserializedGraphNode` with Type, Peer, Size, Tag, CPU, NIC, 32-bit `Gpu`, `PeerGpu` and `Stream`, and unsigned 64-bit `Communicator`, `Channel`, `Sequence`, `MessageKind` and `Chunk` members. `start_time` is initialized runtime scheduler state; it is not stored in the trace. `AtlahsAdapter` declares fourteen bindings, including the eight custom attributes, and the numeric wildcard destinations. The shared reader supplies the inherited dependency fields automatically.

The edited upstream parser branches on the file signature and uses `AtlahsAdapter` for packed files. It reuses ATLAHS's mapping lifetime and ready queues. All five upstream dependency-release paths delegate packed counter updates to the shared reader. Packet scheduling and timing remain in ATLAHS. The backend carries the full message identity through scheduler queues and network callbacks. Both waiting-receive and unexpected-message matching compare source plus `(Communicator, Channel, Sequence, MessageKind, Chunk)`. The message tuple is separate from routing metadata; matching additionally checks that the local and peer GPU IDs agree at both endpoints. There is no concatenation, truncated integer token or hash-based identity. The existing source wildcard still works. Legacy files retain tag matching; explicit messages require `tag 0`, and unmatched messages fail the run. GPU resource clocks use `Gpu`; sends select the source endpoint from `Gpu` and the destination from `PeerGpu`. For packed input, each rank is a host and each `gpu_id` is local to it; the endpoint is `rank * gpu_count + gpu_id`, where `gpu_count` is the file-wide maximum GPU ID plus one. IDs should be dense and the topology needs that many endpoints. GPU count and rank mapping no longer come from NIC/CPU inference.

The upstream send API used the sender's GPU index at both endpoints. The explicit path fixes this with `peer_gpu_id`, carried through routing, matching queues and network completion callbacks. Each host's completion clocks use its own GPU and stream. A receive with inconsistent GPU endpoints cannot match. Legacy files retain their original routing. The 32-bit `Stream` member selects execution resources; stream count is the schema maximum plus one. Legacy loading copies CPU IDs into `Stream`. Legacy files retain the upstream mapping heuristic and copy the legacy NIC-as-GPU value into `Gpu` at the reader boundary.

The focused simulation test also checks actual transport endpoints using `ATLAHS_TRACE_ROUTES=1`. Its independent two-host/four-GPU fixture expects A0=0, B0=4 and B2=6. It checks GPU pairs 0→0, 0→2 and 2→0 under both receive-first and message-first arrival orders. All six transfers must complete and use the independently calculated endpoints (0→4, 0→6 and 2→4). Inconsistent peer GPU IDs must fail.

ATLAHS's present htsim message API uses an `int` byte count, so this adapter rejects send/receive sizes above `INT_MAX` when decoded. Calculation durations retain their 64-bit range. Packed input uses a private mapping; the destructive `save_mem` mode is not supported by this integration.

All eight custom fields are required for packed ATLAHS input; missing or incompatible fields fail during reader setup. The emitter rejects negative, non-integral and out-of-range components. Message kinds preserve upstream values 0–5. Other unbound named fields are skipped. Numeric-tag-only consumers need matching changes before they can run these traces; the LogGOPSim adapter rejects explicit ATLAHS messages to prevent incorrect matches.

This removes GPU-over-NIC and identity-over-tag conventions from the packed consumer path. Other upstream generators are unchanged. Execution-stream metadata is a separate `stream_id` attribute. The new path does not extract hidden components from standard fields.

This adapter is exercised through htsim's own executable. It is not a runtime-selectable plugin for the LogGOPSim executable. NDP is the backend built and tested here; EQDS and UEC have not been validated by this target.

## Checks

Set `-DATLAHS_GENERATOR_PYTHON=/path/to/venv/bin/python` during CMake configuration to select an interpreter with the generator dependencies. After building the backend and test converters:

```sh
cmake --build build --target atlahs_generator txt2bin test_trace_reader test_atlahs_matching -j4
ctest --test-dir build --output-on-failure -R '^(trace_reader|atlahs_matching|atlahs_generator|atlahs_simulation|atlahs_upstream_ndp)$'
```

`atlahs_generator` executes the real upstream event emitter on two-host/four-GPU point-to-point, ring and tree AllReduce fixtures, extracts the pristine generator from the pinned Git commit and compares legacy/default output, checks the explicit attributes, converts its output and runs the actual NDP scheduler. This covers the event-emission-to-simulation path; ingesting a full Nsight dataset is outside this focused test.

The reader test covers all required attributes, full-width values, invalid tags/kinds, graph metadata and the message-size limit. `atlahs_matching` tests the production matcher with identities differing in each component, source wildcards, legacy separation and receive ordering. Integration tests compare explicit-field and unique-tag legacy traces for a broadcast, compute/reply workloads and adversarial pairs differing in one identity component under both arrival orders. A crossed-GPU case, where each message is received on a different local GPU than it was sent from, must also reproduce legacy finish times. They also check unmatched-message failure and unchanged input files. Generator tests include a collision in the original decimal tag scheme and rejected invalid components.

A second comparison uses two ranks with four GPUs each. Its packed input deliberately uses NIC 7 while `gpu_id` selects GPUs 0–3. It must reproduce the original NIC-based GPU schedule's finish times. Moving all communication to GPU 0 must change the finish times while retaining the same topology and GPU count. This checks that the scheduler and routing actually consume the named attribute.

`atlahs_upstream_ndp` reuses the upstream [test runner](https://github.com/spcl/atlahs/blob/fb51a99f908e550318056ebb3e084f3d2fff55bd/sim/htsim-backend/sim/tests/tests.py) and its `dumbell_ndp/default.ref` output, without regenerating the reference. The build target compiles the corresponding `htsim_dumbell_ndp` executable. Our wrapper turns the runner's printed failures into a failing test exit code. This checks the original NDP network implementation; the separate `atlahs_simulation` test checks our GOAL reader integration. Both passed locally.

Upstream also provides a [Docker quick-test workflow](https://github.com/spcl/atlahs#running-a-quick-test) and `scripts/run_validation_exp.py` for AI/HPC application traces. That wider workflow has not been run here. The upstream HPC demo uses UEC, whereas this integration currently verifies NDP.

### Recorded traces and an independent backend

`tests/check_atlahs_recorded.py` runs both generator CLIs on actual Nsight SQLite exports. The baseline uses the pinned upstream generator, converter and backend. The other path uses our generator's explicit-field output, the packed converter and our adapted backend. It requires identical matched send/receive operation pairs and per-rank finish times, and requires every send to match. The reference instrumentation only prints matches already selected by upstream; no reader, matcher or scheduler fixes are applied. The original converter needs an explicit `int` return type on `main` to compile with modern C++; its conversion behavior is preserved.

This matters for the Llama recording: 825,184 send/receive operations have decimal tags above `UINT32_MAX`. The original converter silently narrows these tags; our legacy converter rejects them. Using the original converter reproduces the historical baseline, while the explicit-field path keeps each identity component separately. Matching agreement on this recording does not establish that narrowing is safe for other inputs.

The recorded comparison predates `stream_id` and has not been rerun after that change. Focused scheduler tests check stream IDs 1024 and 1025 with unrelated CPU IDs, legacy-equivalent timing, and the effect of sharing a stream.

The dataset is upstream's four-host, sixteen-GPU Llama quick-test recording. Download URLs, sizes and SHA-256 checksums are recorded in `tests/atlahs_recorded_traces.json`; large inputs stay in `build/`. Allow several GB of free disk space for exports, generated GOAL and binary files. The two paths both use `--zero-red-copy`, so this compares integration behavior without benchmark-based reduction/copy durations.

On Apple Silicon, install the pinned Nsight exporter in Docker:

```sh
docker build --platform linux/arm64 -t dhpc-nsys-export:2026.3.1 \
  -f src/LogGOPSim/simulators/atlahs/integration/Dockerfile.nsys-export \
  src/LogGOPSim/simulators/atlahs/integration
docker run --rm dhpc-nsys-export:2026.3.1 --version
```

After downloading the four reports to `build/atlahs-recorded/nsys_reports`, export them without GPU access:

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

Build the reference and enable the optional recorded test (the topology here matches this sixteen-GPU dataset):

```sh
git submodule update --init src/LogGOPSim/simulators/atlahs/upstream
cmake -S src -B build -DBUILD_ATLAHS=ON \
  -DATLAHS_GENERATOR_PYTHON=/path/to/venv/bin/python \
  -DATLAHS_RECORDED_TRACE_DIR="$PWD/build/atlahs-recorded/sqlite"
cmake --build build --target atlahs_reference atlahs_htsim txt2bin -j4
ctest --test-dir build -R '^atlahs_recorded$' --output-on-failure
```

Each run retains logs and a `result.json` under `build/atlahs-recorded-run/run-*`, including failures. The result includes input and executable hashes, commands, matched-pair counts and finish times. A missing exporter, incomplete run or disagreement is never treated as a passing comparison.

The full recorded comparison passed: 5,680,774 operations, 475,452 identical matched pairs, and identical finish times for all four host ranks. The `atlahs_recorded` CTest took 364 seconds. The longest predicted finish time was 2,067,317,935 ns in both paths. The packed binary is 116,773,549 bytes versus 252,330,728 bytes for legacy, with 17-byte versus 39-byte node records. These are correctness and size results; the test does not establish a runtime speedup.
