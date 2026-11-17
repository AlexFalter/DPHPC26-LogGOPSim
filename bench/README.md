# Benchmarking the self-describing GOAL trace format

This directory benchmarks every feature branch of the project against the unmodified upstream
LogGOPSim, following Hoefler & Belli, *Scientific Benchmarking of Parallel Computing Systems*
(SC'15). The task is specified in [`TASK.md`](../TASK.md). Where TASK.md and the code disagree,
the code wins; see [Discrepancies](#discrepancies-between-taskmd-and-the-code).

```sh
# local (WSL/Linux), smoke test of the whole harness in a few minutes:
BENCH_WORK=~/lgs-bench bash bench/run_all.sh quick
# reportable local campaign (~2-3 h) / full campaign incl. 1e8 records (~8-9 h):
BENCH_WORK=~/lgs-bench bash bench/run_all.sh standard
BENCH_WORK=~/lgs-bench bash bench/run_all.sh full --budget-hours 10
# ETH student cluster (from the repository root on a login node):
bash bench/cluster/submit.sh <course-tag> gb10   # and/or: ... 2080ti
```

`run_all.sh` is the single command from a clean checkout:
`build.sh` → `gen_traces.py` → `run.py` (correctness gate, then timed runs) → `analyze.py`.

---

## System under test

As found in the code on 2026-11-12. Commits are those of the branch heads at that time; every
campaign records the exact commits it built.

### Branches and builds

| Build name | Git ref | Commit | What it adds | Benchmarked as |
|---|---|---|---|---|
| `base` | upstream `76451de` | `76451de16546` | Unmodified upstream LogGOPSim (spcl/LogGOPSim, merge of PR #18), the last commit before this project | `BASE` |
| `master` | `master` | `d7b342d7517c` | Self-describing format v2, unified `TraceReader` (both formats), five plan runners | `NEW-*` |
| `master-plain` | `master` | same | `master` without the phase-timing hooks | `PLAIN-interp` (instrumentation check) |
| `omp` | `origin/feature/omp-first-pass` | `34cd5fc6492a` | txt2bin first pass parallelised with OpenMP (`--threads`); legacy writer uses `pwrite`; `first_pass_bench` | conversion experiment |
| `lazy` | `origin/feature/lazy-decoding` | `81436e9f1e78` | `LogGOPSim --lazy-decoding` (includes `omp`) | `LAZY-*` |

The other branches need no build of their own: `feature/jit-decoder`,
`feature/plan-runners-and-unified-reader` and `feature/self-describing-traces` are fully merged
into `master`. `feature/jit-orc` (one commit on an older base) is superseded by master's
`runners/compiled/orc`. `omp` and `lazy` branch off `1bbb9e1`, whose tree is identical to `master`.

### Where the new format lives

* **Writer:** `src/LogGOPSim/txt2bin.cpp` (`--format legacy|self-describing`, default `legacy`).
  The self-describing path reads the GOAL file twice. The first pass, `goal_parsing/TraceAnalysis`,
  infers each field's range. The second pass, `binary_writing/TraceWriter` with `RankPacking` and
  `RecordPacking`, writes the header, rank table, bit-packed records, dependency appendix and roots.
  Custom fields are declared in a TOML file (`--custom-fields`, default `config/custom_fields.toml`).
* **Reader:** upstream's `Parser.hpp` is replaced by `simulators/loggopsim/SimulationSchedule.hpp`.
  That class uses `LogGOPSimAdapter` → `SimulatorAdapter` → `binary_reading/TraceReader`, which
  holds either a `SelfDescribingTraceReader` (`TraceFileView` + `Decoder` + `DependencyGraph`) or a
  `LegacyTraceReader`. The file is mapped by `MappedTraceFile`: `MAP_PRIVATE`, or `MAP_SHARED`
  with `--save-mem`.
* **Format selection:** by magic number (`TraceReader::detect_trace_format`). ASCII `GOALSD01`
  means self-describing, u64 `4223` means legacy, and `2342` (left behind by `--save-mem`) is
  rejected. There is no CLI flag.
* **Format (doc/binary_header.md):** a 43-byte prefix (magic, version 2, header length, record
  size in bits, descriptor count, rank count, file-wide node-index width, rank-table offset), then
  one 32-byte descriptor per field. A descriptor holds the identity (standard ID or custom name),
  the type, bit width, wildcard flag, bit offset, base and maximum offset. Values are stored as
  `value − base` in the narrowest width; a field that is constant on every node takes 0 bits.
  Everything is little-endian.

### Decoder variants and how each is selected

All variants decode through `Decoder` → `DecodePlan` (one step per bound field) → a `PlanRunner`.
The runner is chosen by `LOGGOPSIM_DECODER` (`RunnerRegistry.cpp`). An unavailable runner falls
back to `plan` and prints one message. `LOGGOPSIM_TRACE_DECODER=1` prints the runner actually in
use; the harness sets it on every NEW run and rejects runs where the actual runner differs from
the requested one.

| `LOGGOPSIM_DECODER` | Implementation | TASK.md term | Config |
|---|---|---|---|
| `plan` (default) | `InterpretedRunner`: loops over the plan steps | generic interpreter | `NEW-interp` |
| `jit` | `CCompiler`: generates C, runs `$LOGGOPSIM_JIT_CC` (default `cc`) with **`-O2`** `-shared -fPIC`, `dlopen`s it, caches the library on disk | generated C → cc → dlopen | `NEW-spec-cold`, `NEW-spec-warm` |
| `orc` | LLVM ORC in-process JIT (`-O2` pipeline) in the plugin `loggopsim_orc`, opened on first use; in-memory reuse only (`LOGGOPSIM_ORC_REUSE=0` disables it) | in-process JIT | `NEW-orc` (only if built with LLVM) |
| `templated_node` | template per simulator node type: unrolled steps, fixed store sizes | template instantiation | `NEW-tnode` |
| `templated_schema` | template per compiled-in schema (`KnownSchemas.hpp`: only `bench_traces` schemas) | fully specialised template | `NEW-tschema`: unavailable for generated traces |
| (legacy file) | `LegacyTraceReader`: fixed 39-byte offsets through the same bindings | backward-compatible path | `NEW-legacyfile` |

**JIT decoder cache:** `$LOGGOPSIM_JIT_CACHE`, else `$XDG_CACHE_HOME/loggopsim-jit`, else
`~/.cache/loggopsim-jit`. The key is FNV-1a of (generated source + compiler + flags) and the file
is `<key>.so`. Clear the cache by deleting the directory. The harness always sets
`LOGGOPSIM_JIT_CACHE` explicitly: a fresh empty directory per run for `cold`, one per campaign for
`warm`, which the warm-up run fills.

**`depcnt`:** in both formats it lives *in the records*, and the simulator decrements it in place
in the private mapping, so touched pages become dirty anonymous memory (`DependencyGraph::decrement`
via `bit_packing::try_decrement_bits`; the legacy reader writes the u32 in place). Only the `lazy`
branch with `--lazy-decoding` keeps counters in a separate `std::vector<uint32_t>` (count + 1,
read lazily from the file), never writes the file and cannot be combined with `--save-mem`.

**Load-time pipeline:** mmap → detect format → `read_binary_header` (validates the layout) →
`TraceFileView` (rank table, extents) → `Decoder` (`resolve_required_fields`: required ⊆ provided,
type, destination capacity; op-code range) → `DecodePlan` → runner creation (JIT: generate →
cache lookup → compile → `dlopen`) → peer range and wildcard bindings → `DependencyGraph` (an
**O(edges) scan** of all dependency indices) → decode the root nodes. The legacy path validates
every rank's roots and appendix when the file is opened, also O(edges). Upstream validates nothing.
Decoding then happens on demand in the event loop: only frontier nodes are decoded, and start and
finish read only the one dependency list they need.

**Simulator output:** `PERFORMANCE: … Events: E … Time: T s … Speed: X ev/s`. `Time` is whole
seconds (`gettimeofday` seconds difference), so `Speed` is `inf` or coarse for short runs. The
harness therefore uses its own wall clock and the phase timer, and computes events/s itself. With
16 or fewer ranks, every host's finish time is printed; otherwise only `Maximum finishing time at
host h: t`.

### Phase timing (the only change to NEW)

`instrumentation/apply_phase_timing.py` copies `PhaseTiming.hpp` into each NEW worktree at build
time and inserts hooks at fixed anchors. Branches are never edited; the baseline is never
touched. If any anchor does not match exactly once, the build fails. With `GOAL_PHASE_TIMING=1`
the simulator prints one line `PHASES {json}` to stderr at exit (CLOCK_MONOTONIC, ns). Without it,
each hook is one branch on a cached flag.

| Phase | Covers |
|---|---|
| `total` | `main()` from start to return (wall − total = exec, dynamic loading, exit) |
| `load` | construction of `SimulationSchedule` (everything below) |
| `header_parse`, `rank_table` | self-describing header and rank table |
| `legacy_open` | legacy header + per-rank validation (roots, appendix) |
| `validate` | required ⊆ provided, widths, op codes, peer range, wildcard bindings |
| `plan_build`, `decoder_select` | `DecodePlan::create`; runner creation (includes the JIT phases) |
| `decoder_lookup`, `jit_generate`, `jit_compile`, `jit_load` | JIT cache lookup; C generation; `cc` (ORC: IR + compile); `dlopen`/`dlsym`. Notes: `jit_cache: hit|miss`, `runner` |
| `validate_deps` | O(edges) dependency-index scan |
| `load_roots` | decoding the root nodes |
| `simulate` | the main event loop, including on-demand decoding |

The `instrumentation_overhead` experiment compares timing on, timing off (same binary) and an
uninstrumented build (`master-plain`). The correctness gate checks that all three print identical
results. BASE has only wall time, ev/s and RSS; that is expected.

## Discrepancies between TASK.md and the code

1. **No schema hash, metadata or byte-order field** in the header (version 2 is fixed
   little-endian; doc/binary_header.md says "schema hashes … are not encoded yet"). The harness
   uses the SHA-256 of the header bytes as the schema hash (`header_sha256` in the manifest and in
   every run record). The JIT's own cache key is a hash of the generated source.
2. **Field widths cannot be chosen.** txt2bin always infers the narrowest offset encoding from the
   data, so the default output *is* the "narrowed" schema. A legacy-equivalent schema (same widths
   as legacy) cannot be written. The harness produces schemas **through the data**:
   * `inferred`: realistic values; the writer narrows every field.
   * `custom`: inferred plus `comm_id` (a 64-bit NCCL-like communicator hash), `gpu_id`,
     `stream_id` and `channel`.
   * `wide`: compute sizes over 40 bits, message sizes over 24 bits, tags over 31 bits, and cpu/nic
     over 8 bits, so the standard fields approach their legacy widths.

   N* in bytes for the legacy-equivalent schema is therefore computed as N* × 39 B.
3. **Fields wider than legacy are impossible for standard fields**: the GOAL parser stores tag as
   u32 and cpu/nic as u8 for both formats ("Legacy cpu exceeds 255" etc.). Only custom fields can
   be up to 64 bits, which is exactly what `comm_id` is.
4. **The JIT compiles with `-O2`, not `-O3`**, using `$LOGGOPSIM_JIT_CC`. The harness sets that to
   the same compiler as the builds.
5. **Decoder variants:** besides the generic interpreter and generated C there is an in-process
   ORC JIT and two template variants. `templated_schema` only has instantiations for the
   `bench_traces` schemas, so on generated traces it falls back to `plan` and is reported as
   *unavailable*, not timed.
6. **"Missing required field (e.g. `gpu_id`)":** LogGOPSim requires no custom field, so the test
   binds `gpu_id` through the same `TraceReader`/`Decoder` path the simulator uses
   (`decode_bench_micro --require gpu_id`). That path rejects the trace with
   `Trace lacks required field 'custom:gpu_id'` (verified). **"Value too wide for its declared
   field":** custom fields are declared only by type (u64/i64), so the tests check values outside
   the type (2^64, negative for unsigned), the standard-field limits (cpu 256, tag 2^32), peers
   outside `num_ranks`, and a declared field missing on an operation.
7. **C++ standard differs between BASE and NEW.** Upstream's CMakeLists sets `CMAKE_CXX_STANDARD 11`;
   NEW requires 23. This cannot be changed without modifying the baseline. Everything else is
   identical (same compiler, `RelWithDebInfo` = `-O2 -g -DNDEBUG`, same extra flags). `build.sh`
   asserts this and writes `flags-check.json`.
8. **NEW fixes a bug:** an eager message matched by a larger receive. NEW's results can differ
   from BASE when that happens. The generated workloads always use equal send/receive sizes, so
   BASE and NEW must agree, and the gate reports any difference.
9. **For more than 16 ranks LogGOPSim prints only the maximum finishing time.** To still compare
   per-host results, the gate also compares the `-V` timeline (start/end of every operation; the
   code is unchanged from upstream) byte for byte for traces up to `check.viz_max_records`.
10. **Upstream crashes when a trace uses CPU or NIC 255:** `Parser::GetNumCPU/GetNumNIC` return
    `max+1` as `uint8_t`, which overflows to 0 and segfaults (found by the `wide` schema; NEW handles
    it). The `wide` schema therefore draws cpu/nic from 0–254 so that BASE stays the reference.
11. **ATLAHS AI traces are not available locally.** The recorded traces are Nsight reports
    (≈250 MB) that need `nsys` and the ATLAHS checkout to convert. The harness substitutes
    NCCL-like generators (`dp_nccl`, `pipeline_1f1b`, `moe_alltoall`) and accepts real GOAL files
    as workload `goal:<path>`.

## What is measured and why

Each experiment answers one question. Experiments marked ★ go beyond TASK.md's minimum; the
column on the right says where the idea comes from.

| Experiment (`kind`) | Question | Metrics | Basis |
|---|---|---|---|
| `size_sweep` (e2e) | How does end-to-end time scale from 1e4 to 1e8 (1e9 on GB10) records, per config? | wall, ev/s, RSS, phases | TASK §2/§3. The sizes span L2 → LLC (16 MB) → beyond LLC → a sizeable fraction of RAM |
| `breakeven` (e2e) + `jit_cold_micro` (micro) | Above which trace size does specialisation pay off? N* = T_jit_cold / (t_interp − t_spec), also vs the legacy and upstream readers, checked against end-to-end runs around N* | N* in records and bytes | Adaptive execution of compiled code, which interprets first and compiles once it pays off: Kohn, Leis, Neumann, *Adaptive Execution of Compiled Queries*, ICDE 2018; Neumann, VLDB 2011 |
| `rank_sweep` (e2e) | Many small vs few large rank blocks at a fixed size | wall, `load_roots`, `validate_deps` | TASK §3 |
| `schema_sweep` (e2e) | Do custom fields or near-legacy widths cost decode time or file size? | wall, file size, record bits | TASK §3 schemas, realised through the data (above) |
| `workloads` (e2e) | Does the result hold across communication patterns: HPC halo exchange, collectives, AI training (data, pipeline, expert parallel)? | speedup vs BASE | LogGOPSim (Hoefler, Schneider, Lumsdaine, HPDC'10) and ATLAHS (Shen et al., SC'25, arXiv:2505.08936) workload classes |
| `cold_cache` (e2e) | First run on a trace not in the page cache: does a smaller file win back the decode cost? | wall, major faults, resident fraction (mincore) | TASK §6.4; eviction is verified per run, not assumed |
| `instrumentation_overhead` (e2e) | Does phase timing perturb the result? | median ratio + CI overlap | TASK §4 |
| `save_mem` (e2e) | MAP_SHARED with destructive counters vs MAP_PRIVATE copy-on-write vs lazy in-memory counters | wall, peak **anonymous** RSS | ★ Crotty, Leis, Pavlo, *Are You Sure You Want to Use MMAP in Your DBMS?*, CIDR 2022: page-cache vs anonymous memory is what decides whether a larger-than-RAM input survives |
| `memory_budget` (memcap) | Which configs still finish under a cgroup memory limit smaller than their dirty-page demand? (local, needs `systemd-run --user`) | completed / OOM, time | ★ same; turns "RSS" into the operational question "how large a trace fits in M GB" |
| `concurrency` (concurrent) | k simultaneous simulations of one trace (a parameter sweep over L/o/g/G): throughput and total memory | simulations/s, Σ anon RSS | ★ MAP_PRIVATE duplicates the dirtied pages per process; lazy shares the clean page cache |
| `decode_micro`, `decode_micro_cold` (micro) | Decoder cost in isolation: sequential, random, and **frontier (topological, ranks round-robin)** order; warm/cold cache | ns/record, Mrecords/s, GB/s of node records | TASK §5; ★ frontier order replays the simulator's access pattern instead of only the two extremes |
| `decode_fields` (micro) | Does decoding only the simulator's fields (specialised plan) help when custom fields exist? all vs simulator fields vs scalars only | ns/record | TASK §5; table-driven vs generated parsers: Haberman, *Parsing Protobuf at 2+GB/s* (upb, 2021) |
| `little_cores` (micro, GB10) | Does specialisation matter more on a narrow efficiency core (Cortex-A725) than on a wide one (X925)? | ns/record | ★ heterogeneous big.LITTLE node available on the cluster; second ISA (aarch64) overall |
| `convert` (convert) | txt2bin cost: upstream vs NEW legacy writer vs two-pass self-describing writer vs OpenMP first pass at 1–20 threads | wall, RSS, MB/s, speedup | TASK §2 optional; `omp` branch |

Built in as methodology rather than as separate experiments:

* **Correctness first.** Results, the `-V` timeline, the actual decoder and phase output are
  checked per (trace, config). Byte identity of legacy files written by BASE/master/omp and of
  self-describing files written by master/omp is checked per trace.
* **Setup randomisation:** random environment padding per run (`BENCH_ENV_PAD`, 0–4 KiB).
  Environment size shifts the stack and can change results by several percent (Mytkowicz et al.,
  *Producing Wrong Data Without Doing Anything Obviously Wrong!*, ASPLOS'09). Runs are interleaved
  in a new random order every round.
* **Verified cache state:** mincore resident fraction before every run.
* **The checksum** in the micro benchmark is order-independent, so every variant and order must
  produce the same value on equivalent traces.

Literature context for the format itself: self-describing trace formats with load-time metadata,
such as CTF/Babeltrace and OTF2 (Eschweiler et al., 2012); bit-packed integer decoding (Lemire &
Boytsov, SPE 2015; FastLanes, VLDB 2023); cheap JIT alternatives (copy-and-patch, Xu & Kjolstad,
OOPSLA 2021).

### Workloads

`workloads/goalgen.py` generates GOAL with a target record count and rank count, so size and rank
count are independent axes. Schedgen has no repetition count. Each pattern repeats per iteration;
every send has exactly one matching receive of the same size and tag; output is deterministic and
byte-identical regardless of `--jobs`. The generator is checked against upstream's GOAL grammar:
`calc` takes only `cpu`; send/recv take tag, cpu, nic in that order.

| Workload | Pattern | Graph shape |
|---|---|---|
| `stencil3d` | 7-point halo exchange on a periodic 3D grid; z faces > S (rendezvous), x/y eager | wide frontier, 13 ops/iteration |
| `allreduce_ring` | ring reduce-scatter + allgather (MPI/NCCL, large messages) | long chains, O(P) ops per rank |
| `allreduce_rd` | recursive doubling (latency-optimal; P a power of two) | log P stages |
| `alltoall` | linear all-to-all, all posted at once | widest frontier, long match queues |
| `dp_nccl` | data-parallel step: L layers forward/backward; gradient buckets ring-allreduced on C channels, overlapping backward | AI training (ATLAHS-style) |
| `pipeline_1f1b` | pipeline-parallel 1F1B over P stages, M micro-batches | strictly sequential per rank |
| `moe_alltoall` | MoE layer: attention, skewed all-to-all dispatch, experts, combine | variable message sizes |
| `schedgen:<ptrn>` | upstream Schedgen pattern (`--commsize` = ranks) | as Schedgen |
| `goal:<path>` | existing GOAL file (e.g. ATLAHS traces converted without `--explicit-fields`) | — |

## Methodology (TASK.md §6)

| Rule | Implementation |
|---|---|
| Correctness first | `check_correctness.py` runs before any timing (`run.py` calls it). A (trace, config) that is not `pass` gets no numbers and is listed with its status. Each timed run also records `matches_reference`. Validation-failure tests: see [Discrepancies](#discrepancies-between-taskmd-and-the-code) 6. |
| Proper baseline | BASE = upstream `76451de` from its own worktree, built with upstream's CMake files, the same compiler and flags; legacy traces are written by BASE's own txt2bin. |
| Uncertainty | ≥ 1 warm-up run per cell. Repeat until the nonparametric 95% CI of the median (exact binomial order statistics) is within ±5% of the median, 10–50 repetitions. Caps for large traces: 10–20 above 5e6 records, 5–8 above 5e7, 3 for 1e9 on GB10. Speedups: ratio of medians, percentile-bootstrap CI. Distributions: box plots of all repetitions. Micro: 2–3 processes × 5–10 repetitions. |
| Pinning | One core per run, by default the fastest by `cpu_capacity`/`cpuinfo_max_freq`, avoiding CPU 0 and SMT siblings (`--cpus` overrides). Memory is bound to that core's NUMA node with `numactl` on multi-socket nodes. |
| Governor / turbo | Not settable without root (neither WSL nor the cluster allows it). Governor, current frequency, `no_turbo`/`boost` and THP are recorded per campaign and per run. |
| Cold / warm cache | Cold: `drop_caches` if root, else `posix_fadvise(DONTNEED)` on the trace. Warm: the trace is read if less than 99% resident. Method and resident fraction (mincore) are recorded per run. |
| Idle machine | The load average is logged before every run; runs above `noisy_loadavg_per_cpu` are flagged `noisy`; `analyze.py --exclude-noisy`. |
| Interleaving | All cells of an experiment run in a new random order every round. |
| Phases | See [Phase timing](#phase-timing-the-only-change-to-new). JIT cost is reported separately, never averaged in. |
| Reproducibility | `metadata.json` per campaign: hostname, lscpu, RAM, kernel/OS, cgroup limit, Slurm variables, compiler versions, compile command, commits, LogGOPS parameters (L=2500 o=1500 g=1000 G=6 O=0 S=65535 for every config), trace names, sizes, SHA-256, header SHA-256. Every run line repeats the trace file, hash, cache state, pinning, governor, build commit and timestamp. |
| Meaning of "faster" | Every speedup row names the two configs and the trace and gives both absolute medians. |
| `--save-mem` | Only on a per-run copy (outside the timed region); the gate also tests that a consumed copy is rejected afterwards. |

## Running it

### Local (WSL2 Ubuntu 24.04 on the i9-9900K)

1. **Toolchain.** The machine has gcc 13.3, cmake 3.28, ninja and LLVM 18 but is missing gengetopt,
   re2c, gfortran and the Graphviz headers. Either
   `sudo apt-get install gengetopt re2c gfortran libgraphviz-dev`, or without root
   `bash bench/env/setup_env.sh && source bench/env/activate.sh`. For the plots,
   `pip install matplotlib` (or use a venv that has it); everything else needs only the
   standard library.
2. **Keep builds and traces on the Linux file system.** `/mnt/c` (DrvFs) is slow and distorts
   page-cache effects. Either clone the repository inside WSL, or keep it where it is and set
   `BENCH_WORK=~/lgs-bench`.
3. `BENCH_WORK=~/lgs-bench bash bench/run_all.sh quick` (minutes), then `standard` or
   `full --budget-hours 8`.
4. Before a reportable run: close other applications and keep Windows awake. WSL cannot pin
   frequency or disable turbo; this is recorded in `environment.md`. WSL2 gives the VM 15 GB here,
   so the 1e8-record traces are about a third of "RAM".

Steps can also be run one at a time:

```sh
export BENCH_WORK=~/lgs-bench
bash bench/build.sh                              # all builds in configs/builds.json
python3 bench/gen_traces.py --config bench/configs/standard.json --jobs 8
python3 bench/check_correctness.py --config bench/configs/standard.json   # optional: run.py runs it too
python3 bench/run.py --config bench/configs/standard.json [--experiments size_sweep,decode_micro]
python3 bench/analyze.py                         # newest campaign; or pass campaign directories
```

`run.py --dry-run` lists the cells; `--budget-hours H` stops cleanly; `--resume DIR` continues an
interrupted campaign (cells with enough repetitions are skipped).

### Cluster (ETH D-INFK student cluster)

Constraints from the cluster documentation: Slurm; account `-A <course-tag>` (shown by `courses`);
one running and two queued jobs per user; default jobs get 2 cores, 24 GiB and a 40 GiB `/tmp`;
GB10 jobs get 20 cores, 112 GiB and an 850 GiB `/tmp` for at most 24 h; 2080 Ti jobs get 4 cores
and 36 GiB. There is no root (no governor, no `drop_caches`). Login nodes are only for building
and enforce CPU limits.

1. Toolchain: check whether `gcc --version` (≥ 13), `cmake`, `gengetopt`, `re2c`, `gfortran` and
   the Graphviz headers exist (`module avail`). If not, create the environment once per
   architecture inside a job:
   `srun -A <tag> -t 60 bash bench/env/setup_env.sh` (x86) and
   `srun -A <tag> --gpus gb10:1 -t 60 bash bench/env/setup_env.sh` (aarch64). It installs into
   `~/.lgs-bench-env/<arch>`, about 1.5 GB of the 20 GiB home quota; use `--prefix` for elsewhere.
2. `bash bench/cluster/submit.sh <tag> gb10` (config `cluster-gb10`, 21 h budget) and/or
   `bash bench/cluster/submit.sh <tag> 2080ti` (config `cluster-x86`). Try
   `bash bench/cluster/submit.sh <tag> default quick` first.
3. The job builds natively on the node, generates traces on the local SSD (`$TMPDIR`), runs and
   analyses. Only `bench/results/` is written to the checkout. A requeued job resumes its
   campaign. Output: `bench/results/slurm/lgs-bench-<node>-<jobid>.out`.

Cluster nodes are shared with other jobs, so expect more `noisy` flags and wider CIs than locally.
The CI rule keeps sampling until it converges or hits the cap; both outcomes are recorded in
`status.json`.

### Configurations

| Config | Use | Largest trace |
|---|---|---|
| `configs/quick.json` | smoke test of the whole harness (minutes) | 1e5 |
| `configs/standard.json` | reportable local campaign (~2-3 h) | 2e7 |
| `configs/full.json` | full local campaign (TASK.md's `full`) incl. memory budget, ~8-9 h | 1e8 |
| `configs/cluster-gb10.json` | GB10 node (aarch64, big.LITTLE) | 1e9 |
| `configs/cluster-x86.json` | 2080 Ti / default x86 node | 1e8 |
| `configs/builds.json` | build catalogue (refs, instrumentation) | |
| `configs/configs.json` | runtime configs (BASE, NEW-*, LAZY-*, save-mem) and txt2bin tools | |

A campaign config lists experiments; traces are given as a cross product
(`{"records": [1e4, 1e5]}`, others from `trace_defaults`) or as a list of combinations. Other axes
are held at the documented defaults (stencil3d, 64 ranks, 1e6 records, inferred schema) while one
is swept.

## Outputs

```
bench/
  README.md  build.sh  gen_traces.py|.sh  run.py  check_correctness.py  analyze.py  run_all.sh
  configs/          builds, runtime configs, campaign grids
  micro/            decode_bench.cpp (decode microbenchmark) + inject.cmake (adds it to NEW builds)
  instrumentation/  PhaseTiming.hpp + apply_phase_timing.py
  workloads/        goalgen.py + custom-field declarations
  lib/              benchlib (stats, metadata, cache control, measured runs), grid, runner
  env/              no-root toolchain (setup_env.sh, activate.sh)
  cluster/          submit.sh, job.sbatch
  results/
    raw/<date>-<host>-<config>/   metadata.json, runs.jsonl, status.json, correctness/  (never overwritten)
    tables/<campaign>/            summary, phases, decode, breakeven, file_sizes, memory, memory_budget,
                                  convert, concurrency, overhead, environment (.csv + .md), correctness
    plots/<campaign>/             see below (.pdf + .png)
  .work/            (BENCH_WORK) worktrees, builds, traces, scratch; not in git
```

TASK.md §7 deliverables → files:

1. time vs size with CI bands: `e2e_vs_size`
2. phase breakdown: `phases`
3. decode throughput, warm vs cold: `decode_throughput_<host>`
4. break-even with N*: `breakeven` and `breakeven.md` (model + end-to-end crossing)
5. file size per schema: `file_size`, `file_sizes.md`
6. RSS vs size: `rss_vs_size`, `anon_vs_size`
7. summary table: `summary.md`

Also: `distributions_<host>` (box plots), `rank_sweep`, `workloads`, `convert_scaling` and
`concurrency`. All of them are computed from `runs.jsonl` and `metadata.json` only. `analyze.py`
accepts several campaigns at once (local + cluster) and keeps hosts apart.

## Open questions from TASK.md §9: assumptions made

* **Reference machine:** both. Headline numbers from GB10 (most memory, room for 1e9 records)
  plus the local x86 desktop as the second architecture. No root on either, so governor and turbo
  are recorded, not controlled, and cold cache uses `posix_fadvise`.
* **ATLAHS AI traces:** not available locally; substituted (see Discrepancies 10). If you have
  converted GOAL files, add `{"workload": "goal:/path/x.goal", "ranks": P}` to an experiment.
* **Upper bound on size:** local 1e8 (~5 GB legacy; 15 GB WSL RAM), GB10 1e9 (~46 GB legacy;
  112 GiB), x86 cluster nodes 1e8 (40 GiB `/tmp`). `gen_traces.py` checks free disk space first.
* **Decoder variants:** every registry runner is benchmarked. `templated_schema` only applies to
  compiled-in schemas and is reported as unavailable on generated traces; `orc` needs LLVM at
  build time.

## Verified while building the harness (2026-11-12, WSL2, i9-9900K)

* The phase-timing anchors apply to `master`, `lazy` and `omp`. The instrumented reader sources
  compile and print `PHASES` with JIT hit/miss.
* `decode_bench_micro` compiled with g++ 13 against master's reader sources. On a 1.15 M-record
  trace written by the real writers, all seven variants and three orders give the same checksum.
  Cold-cache eviction is verified (0% resident). A cold JIT setup takes about 245 ms. The
  `--require gpu_id` test fails as intended. Preliminary frontier-order numbers: legacy 39, upstream 44,
  jit 51, templated_node 67, plan 78 ns/record.
* `goalgen.py`: every workload and schema satisfies upstream's grammar, every send is matched, and
  output is identical for any `--jobs`.
* gen_traces → check_correctness → run → analyze ran end to end against stub binaries. The real
  LogGOPSim, txt2bin and BASE builds could not be built on this machine yet (gengetopt, re2c,
  gfortran and the Graphviz headers are missing), so run `quick` first in the morning.

---

## Component benchmarks (pre-existing)

The Google-Benchmark suite below predates the harness above and remains useful for quick,
in-process comparisons of the runners. It is built with `bench/build.sh --gbench` or a normal
project build.

```
bench/
  writing/       conversion_bench.py: GOAL-to-binary conversion times
  decoding/      Google Benchmark programs for reading traces (decode_bench)
  profiling/     profile.py: which functions a command spends its time in
  end_to_end/    decode_share.py: decoding's share of full LogGOPSim simulations
  CMakeLists.txt Google Benchmark, the benchmark traces and the programs
```

A benchmark for another part, for example writing traces, gets its own folder next to
`decoding/` and its own `add_executable` in `CMakeLists.txt`.

The benchmarks are built with the project (`-DLOGGOPSIM_BUILD_BENCHMARKS=OFF` disables them;
CMake fetches Google Benchmark 1.9.1 unless it is installed). The project always builds with
`RelWithDebInfo` (`-O2 -g`): optimized, and with the symbols the profiles need.

### Traces

```sh
cmake --build build --target bench_traces decode_bench
```

`bench_traces` generates the traces with `schedgen`, rewrites the field values of two of them
with `traces/vary_fields.py` (seeded, so every build generates the same files) and converts each
to both formats, in `build/bench/traces/<name>.{legacy,sd}.bin`. Each one varies a property
that favours a different runner:

| Trace | Source | Nodes | Shows |
|---|---|---|---|
| `bcast_8` | `binomialtreebcast`, 8 ranks | 14 | a tiny trace: setup and compilation dominate |
| `bcast_1k` | `binomialtreebcast`, 1,024 ranks | 2,046 | a small trace |
| `bcast_64k` | `binomialtreebcast`, 65,536 ranks | 131,070 | a large trace with mostly constant fields |
| `dissem_16k` | `dissemination`, 16,384 ranks | 458,752 | many dependencies per node |
| `ring_256` | `allreduce_ring`, 256 ranks | 261,120 | long dependency chains |
| `alltoall_256` | `linear_alltoall`, 256 ranks | 130,560 | no dependencies, many peers |
| `wide_64k` | `bcast_64k`, `vary_fields.py wide` | 131,070 | no constant fields; random 64-bit sizes and tags, CPUs and NICs up to 255 |
| `atlahs_64k` | `bcast_64k`, `vary_fields.py atlahs` | 131,070 | ATLAHS message identities: 19 fields per node instead of 11, read with the ATLAHS adapter; no legacy file |

A new scenario is one `add_bench_trace` line in `CMakeLists.txt`. Their plans are compiled into
the `templated_schema` runner
(`src/LogGOPSim/binary_reading/decoding/runners/templated_schema/KnownSchemas.hpp`); new traces without a compiled-in schema are skipped by that runner. Other traces can be passed with `--traces=a.sd.bin,b.sd.bin`;
a `.legacy.bin` file next to one adds the legacy replay.

### decode_bench

For every trace and every runner of the registry
(`src/LogGOPSim/binary_reading/decoding/runners/RunnerRegistry.cpp`):

| Benchmark | Measures |
|---|---|
| `Setup/<trace>/<runner>` | Creating the adapter: header, decode plan and runner, with a warm JIT cache and ORC's functions reused |
| `ColdStart/<trace>/<runner>` | A one-shot run: creating the adapter with an empty JIT cache (and `LOGGOPSIM_ORC_REUSE=0`) and reading every node once |
| `ReadNodes/<trace>/<runner>` | Reading every node once, in order; decoding only |
| `RandomReads/<trace>/<runner>` | Reading every node once, in a fixed random order |
| `Replay/<trace>/<runner>` | The simulator's accesses without the network model: one full read per node (roots are loaded during setup), one event-specific list read at start and finish, and one counter decrement per dependency edge; also `legacy` for the legacy format. LogGOPSim traces only. Reports `time_per_node` and the mean `setup` time |

A runner that is unavailable in the build or for a trace, such as `orc` without LLVM or
`templated_schema` for a trace without an instantiation, is reported as skipped instead of
silently measuring `plan`. Earlier result files call the two templated runners `shape` and
`layout`.

```sh
build/bench/decode_bench --benchmark_filter='^ReadNodes/bcast_64k/'
build/bench/decode_bench --benchmark_filter='^Replay/.*/(plan|jit)/' --benchmark_repetitions=10
```

### Which runner wins where

`decoding/summarize.py` turns a JSON result into one table per benchmark kind (traces × runners,
fastest marked) and a final table of the fastest runner per kind and trace. Runners within 3% of
the fastest count as tied. `--exclude templated_schema` leaves that runner out of the ranking,
since it only exists for the benchmark traces themselves.

```sh
build/bench/decode_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
  --benchmark_min_time=0.1s --benchmark_out=results.json --benchmark_out_format=json
python3 bench/decoding/summarize.py --exclude templated_schema results.json
```

All scenarios take about three minutes this way.

### Comparing before and after a change

Save the results of both versions as JSON and compare them with Google Benchmark's
`compare.py`, which tests each difference with a Mann–Whitney U test (it needs NumPy and
SciPy; use at least 9 repetitions):

```sh
build/bench/decode_bench --benchmark_filter=ReadNodes --benchmark_repetitions=10 \
  --benchmark_out=before.json --benchmark_out_format=json
# change the code, rebuild, then the same with --benchmark_out=after.json
python3 build/_deps/googlebenchmark-src/tools/compare.py benchmarks before.json after.json
```

The laptop timings vary by up to 30% between sessions, so compare runs from one session.

### Profiling

`profiling/profile.py` starts a command, samples it with the system profiler (`sample` on macOS,
which needs no Xcode; `perf` on Linux) and lists the functions with the most samples of their
own time. The raw profile, with the full call tree, is kept in `build/bench/profiles/`.

```sh
python3 bench/profiling/profile.py --seconds 5 -- \
  build/bench/decode_bench --benchmark_filter='^Replay/bcast_64k/plan/' --benchmark_min_time=30s
```

Let the benchmark run longer than the sampling, and filter it to one benchmark so that the
profile shows only that one. Any other program, such as `LogGOPSim -f trace.bin`, can be
profiled the same way.

### Decode share of full simulations

```sh
python3 bench/end_to_end/decode_share.py build 21 bcast_64k dissem_16k
```

Runs full LogGOPSim simulations and the `Replay` benchmark for each format and runner and
prints medians with 95% confidence intervals and replay time relative to complete-simulation time. Replay includes dependency and ready-node handling; the ratio does not isolate decoding cost. Raw
samples go to `build/bench/decode_share.csv`.

### Baseline: the original LogGOPSim

```sh
cmake --build build --target loggopsim_original
```

builds the last upstream LogGOPSim before this project (commit `76451de`, set by
`LOGGOPSIM_ORIGINAL_COMMIT`) from the git history, with its own CMake files and the same build
type, into `build/bench/original/build/LogGOPSim`. It is not part of the default build. Once it
exists, `decode_share.py` also times it on each trace's legacy file, as the row `original`
(simulation only; it has no replay). On the LogGOPSim benchmark traces it produces the same
output as our simulator in both formats; it crashes on `wide_64k`, whose random tags leave
messages unmatched, so leave that trace out.

### Shared memory-work changes

The reader validates immutable dependency indices once during graph setup, while
retaining per-read slice bounds checks. The scheduler reuses ready-node buffers,
and the node validator's type-erased address is resolved once at setup. Field-only
readers do not scan dependency arrays. Tests cover malformed indices and bounds,
output-type mismatches, and allocation-free ready batches after setup.

The measured before/after JSON and end-to-end samples for this change are kept
locally in `build/bench/results/memory-work/`. Replay improved by 24–47% across
the comparable self-describing cases on this machine; this excludes setup, which
now includes the dependency-index scan. The report contains the measured table.
Use an explicit writable `LOGGOPSIM_JIT_CACHE` for reproducible JIT measurements;
the broadcast JIT baseline in this session was skipped because the default cache
was not writable. The standalone `jit` measurements use a warm cache.

### Dependency access improvements

`read_dependents(rank, index, event)` selects the start or completion list before reading
its position and count. It reuses `FieldSource` to prepare bit access once; zero-bit
fields do not load record bytes. The legacy scheduler also selects one list. No validation
is removed and no dependency arrays are copied.

The incremental baseline and each optimization's measurements are in
`build/bench/results/dependency-optimization/`, alongside the final eight-trace run
and complete-simulation samples. Setup and compilation are excluded from Replay;
root nodes are already loaded before its timer starts.

### Writing / conversion

```sh
python3 bench/writing/conversion_bench.py --build-dir build --repetitions 5
```

Generates broadcast and dissemination GOAL inputs with schedgen, warms both conversion
paths, then interleaves whole-process conversion runs. Saves raw samples, medians and
output sizes in `build/bench/results/writing/conversion.json`. Generation is excluded.
The current legacy writer remaps per rank and calls system-wide `sync()` at finish;
the new writer closes its output stream. These existing I/O policies are retained,
so the numbers compare implementations and do not isolate format or disk throughput.
