# TASK: Benchmark the self-describing GOAL trace format against legacy LogGOPSim

You are working on the DPHPC 2026 project **"A Self-Describing, Extensible Trace Format for LogGOPSim"**. The format, its load-time validation and its runtime-specialized decoder are being (or have been) implemented elsewhere in this repo. **Your job is the benchmarking part only**: build a rigorous, reproducible harness that compares the new implementation against the unmodified original LogGOPSim, run it, and produce the results tables and plots for the report.

Benchmarking methodology follows Hoefler & Belli, *Scientific Benchmarking of Parallel Computing Systems*, SC'15. A speedup without a fair baseline and reported uncertainty means nothing here.

---

## 0. Before you start

1. Explore the repo. Find out:
   - where the new format lives (writer in `txt2bin`, reader/decoder in the simulator, `Parser.hpp` or its replacement),
   - how the new format is selected (magic number detection, CLI flag, …),
   - which decoder variants exist (generic interpreter, optimized interpreter, template-instantiated, generated C → `cc -O3 -shared` → `dlopen`, in-process JIT) and how each one is selected,
   - where the JIT decoder cache lives and how to clear it,
   - whether `depcnt` still lives in the records or in a separate array.
2. Write what you found into `bench/README.md` (section "System under test") before writing any harness code.
3. If something in this file contradicts the code, the code wins. Note the discrepancy in `bench/README.md`. Don't change it on your own.
4. **Do not change the format, validation or decoder logic.** The only change you may make to the new implementation is to add phase-timing instrumentation (§4). **Never change the baseline.**

Upstream code: `github.com/spcl/LogGOPSim`, `github.com/spcl/atlahs`.

---

## 1. Background you need

### Legacy binary format (the baseline)
- Produced by `txt2bin` from GOAL text (`*.goal` → `*.bin`), one rank block at a time.
- File: `u64 magic = 4223`, `u32 num_ranks`, `u8 max_cpu`, `u8 max_nic`, then a jump table `num_ranks × {u64 start, u64 end}` (offsets relative to the byte after the magic), then the rank blocks.
- Rank block: `u32 num_nodes`, `u32 num_roots`, `u32 root_idx[num_roots]`, `node[0..num_nodes-1]` (39 bytes each), then an appendix `u32 node_idx[num_edges]`.
- 39-byte node record, packed, native-endian, unaligned:
  `depcnt u32 | type u8 | peer u32 | size u64 | tag u32 | proc u8 | nic u8 | nDep u32 | depIx u32 | nSDep u32 | sDepIx u32`.
- The simulator `mmap`s the file and decodes nodes in place (`get_node_by_offset`). Only the frontier is decoded. `depcnt` is decremented **inside the mapping**:
  - default `MAP_PRIVATE` (copy-on-write: touched pages become dirty anonymous memory and count towards RSS),
  - `--save-mem` uses `MAP_SHARED`, **destroys the trace** and sets the magic to `2342`.
- Every run prints `Speed: … ev/s` and every host's finish time.

### New format (system under test)
- New magic (≠ 4223, ≠ 2342), version, byte order, field descriptors, record layout, schema hash, metadata.
- Load-time pipeline: read header → validate (required ⊆ provided, incl. widths) → look up decoder by schema hash → on miss: generate, compile, load → hot loop calls `decode()`.
- Legacy files must still load via the 39-byte path. That path must be benchmarked too (§3, config `NEW-legacyfile`).

### LogGOPS parameters
Defaults: `L=2500 o=1500 g=1000 G=6 O=0 S=65535`. Use the same parameters for all compared runs. Record them in the run metadata.

---

## 2. What to measure

| Metric | How | Why |
|---|---|---|
| **Parsing / decode throughput** (records/s, GB/s) | Standalone microbenchmark (§5) | Isolates the decoder: legacy vs generic vs specialized |
| **End-to-end simulation time** | Wall-clock of the full simulator process, plus the simulator's own `ev/s` | The number users care about |
| **Phase times** | Instrumentation (§4) | Separates header parse and validation, JIT compile, decode, simulation |
| **Trace file size** | `stat` on the `.bin` files | Do narrow or custom fields help or hurt? |
| **Peak memory (RSS)** | `getrusage(RUSAGE_CHILDREN).ru_maxrss` or `/usr/bin/time -v` | Traces are large; memory is often the limit |
| **JIT overhead** | Generate + compile + `dlopen`, cold vs warm cache | Needed for the break-even trace size |
| *(optional)* `txt2bin` time | Wall-clock legacy vs new writer | Cost of write-time range checks |

**Break-even trace size.** Report the number of records N* above which specialization pays off:
`N* = T_jit_cold / (t_interp − t_spec)`, where `t_*` is per-record decode time from the microbenchmark. Report N* against the generic interpreter and against the legacy hand-specialized reader (if the specialized decoder is not faster per record than legacy, say so). Also give N* in bytes for the legacy-equivalent schema, and check it against end-to-end runs at sizes around N*.

---

## 3. Configurations and sweeps

### Configurations (all built with the same compiler, flags and machine)
| Name | What |
|---|---|
| `BASE` | Unmodified upstream LogGOPSim at a pinned commit, legacy `.bin` |
| `NEW-legacyfile` | New simulator reading the **legacy** `.bin` (backward-compat path) |
| `NEW-interp` | New simulator, new format, generic / optimized interpreter decoder |
| `NEW-spec-cold` | New simulator, new format, specialized decoder, **empty** decoder cache |
| `NEW-spec-warm` | Same, decoder already cached |
| *(if implemented)* other decoder variants | template instantiation, in-process JIT, packed vs aligned layouts |

Build `BASE` from a separate `git worktree` (or clone) of upstream so it really is unmodified. Put the compiler version and flags of both builds in the metadata and assert they are identical.

### Sweep axes
- **Trace size**: small to very large, at least 4 sizes spanning ≥ 3 orders of magnitude in record count. Go as large as the machine's RAM and disk allow, so that at least one size does not fit in the last-level cache and one is large relative to RAM. For scale: in legacy format, 10⁸ records ≈ 3.9 GB of node records plus the appendix.
- **Number of ranks**: at least 3 values at a fixed total size, to separate "many small rank blocks" from "few large ones".
- **Schemas**:
  1. legacy-equivalent (same fields, same widths),
  2. legacy fields + custom fields (e.g. `comm_id`, `gpu_id`, `stream_id`, `channel`),
  3. widened fields (e.g. wider `tag`, `proc`/`nic` > 8 bit),
  4. narrowed fields (e.g. `size` 40 bit, `peer` sized to the rank count): smaller files.
- **Workloads**:
  - MPI collectives / synthetic patterns from Schedgen (e.g. allreduce, alltoall, a stencil/halo exchange),
  - ATLAHS AI traces (NCCL, `goal_gen/ai/`).
- *(Optional)* equivalent traces in Avro, Arrow or OTF2, read by the best reader available, as decode-throughput reference points only.

You don't need the full cross product. Hold the other axes at a documented default while sweeping one axis. Write the chosen grid down in `bench/configs/`.

---

## 4. Phase-timing instrumentation (new implementation only)

Add minimal timing at phase boundaries, using `clock_gettime(CLOCK_MONOTONIC)`:
`header_parse`, `validate`, `decoder_lookup`, `jit_generate`, `jit_compile`, `jit_load`, `simulate` (main event loop, including on-demand decoding), `total`.

- Guard it behind an env var or compile flag (e.g. `GOAL_PHASE_TIMING=1`) and write it to stderr as one machine-readable line (e.g. `PHASES {json}`).
- With timing off, the binary must behave identically. With timing on, check that end-to-end time does not change measurably (compare medians; report the result).
- Decoding is interleaved with simulation (frontier-only). Do not try to time each `decode()` call inside the hot loop. Decode cost in isolation comes from the microbenchmark (§5).
- For `BASE`, only end-to-end time, `ev/s` and RSS are available. That's expected; say so in the report.

---

## 5. Decode microbenchmark

`bench/micro/decode_bench.cpp`: a standalone program that `mmap`s a trace and decodes **every** record of every rank, sequentially and (separately) in a randomized node order that mimics frontier access.

- Variants: legacy reader (copied verbatim from upstream `get_node_by_offset`, logic unchanged), and each new decoder variant called through the same interface the simulator uses.
- Accumulate a checksum over all decoded fields and print it, so the compiler can't eliminate the work. All variants must produce the same checksum on equivalent traces.
- Report records/s and GB/s (bytes of node records actually touched), warm and cold page cache.
- Variants: decode all fields vs only the fields the simulation uses (the specialized decoder can skip unused fields; the interpreter may not).

---

## 6. Benchmarking rules (non-negotiable)

1. **Correctness first.** For every (trace, config) pair, the per-host finish times printed by LogGOPSim must be **identical** to `BASE` on the equivalent legacy trace. Write `bench/check_correctness.py` and run it before any timing. A config that fails correctness gets no performance numbers. Report the failure instead. Also add a validation-failure test: a trace missing a required field (e.g. `gpu_id`) must give a clear error, and a value too wide for its declared field must be rejected by `txt2bin`.
2. **Proper baseline.** `BASE` = unmodified upstream, same compiler, flags, machine, parameters.
3. **Report uncertainty.** Discard ≥ 1 warm-up run. Repeat until the nonparametric 95% CI of the median is within ±5% of the median, with at least 10 and at most 50 repetitions (lower the cap for the largest traces and document it). Report median and 95% CI (order-statistic or bootstrap). Show distributions (box or violin plots), not just bars.
4. **Control the environment.**
   - Pin to cores (`taskset`/`numactl`; also pin memory on NUMA machines).
   - Set the CPU governor to `performance` and disable turbo if you're allowed; otherwise record the frequency settings and report them.
   - Measure warm and cold page cache separately. Cold: `echo 3 > /proc/sys/vm/drop_caches` if root, else `posix_fadvise(POSIX_FADV_DONTNEED)` / `vmtouch -e` on the trace file. Record which method was used.
   - Idle machine. Log the load average before each run and flag noisy runs.
   - Interleave configurations (randomize the run order) so drift doesn't favor one config.
5. **Separate the phases** (§4). Report JIT compile time separately, never hidden inside an average.
6. **Reproducible.** For every run, record: hostname, `lscpu`, RAM, kernel/OS, compiler versions, build flags, commit hashes of `BASE` and `NEW`, LogGOPS parameters, trace file name + size + hash, schema hash, cache state, pinning, governor, timestamp.
7. **Say what "faster" means.** Every speedup comes with both absolute times and states exactly which two configs and which trace were compared.
8. **Never use `--save-mem`** on a shared trace file, because it destroys the trace. If you benchmark it, copy the trace first for every single run and keep the copy out of the timed region.

---

## 7. Deliverables

```
bench/
  README.md              # system under test, how to run, machine notes, discrepancies
  configs/               # sweep grid(s), YAML or JSON
  gen_traces.sh|.py      # generates all .goal → legacy .bin + new-format .bin per schema
  build.sh               # builds BASE (pinned upstream worktree) and NEW with identical flags
  run.py                 # driver: runs the grid, randomized order, writes raw results
  check_correctness.py   # diffs per-host finish times vs BASE; gate for run.py
  micro/decode_bench.cpp
  analyze.py             # raw results → tables (CSV + Markdown) and plots (PDF/PNG)
  results/
    raw/*.jsonl          # one line per run, incl. all metadata
    tables/
    plots/
```

- One command reproduces everything from a clean checkout, e.g. `bench/build.sh && bench/gen_traces.sh && python bench/run.py --config bench/configs/full.yaml && python bench/analyze.py`.
- Also provide a `--quick` config that finishes in a few minutes on a laptop, for CI and for smoke-testing the harness.
- Raw data is never overwritten. Each campaign goes into its own timestamped directory.

### Plots and tables to produce
1. End-to-end time vs trace size, one line per config, with CI bands (log-log).
2. Phase breakdown (stacked bars) per config at a representative size.
3. Decode throughput (records/s) per decoder variant, warm vs cold cache.
4. Break-even analysis: cumulative time vs records for `NEW-interp`, `NEW-spec-cold`, `NEW-spec-warm`, `BASE`, with N* marked.
5. File size per schema relative to legacy.
6. Peak RSS vs trace size per config.
7. Summary table: for each (workload, size, schema) the median ± CI of every config, the speedup vs `BASE` with absolute times, and the correctness status.

---

## 8. Acceptance criteria

- [ ] `bench/README.md` describes the system under test as found in the code.
- [ ] `BASE` is built from unmodified upstream at a recorded commit, with flags identical to `NEW`.
- [ ] The correctness check passes (or failures are reported) for every trace/config before timing; the validation-failure tests behave as specified.
- [ ] All metrics in §2 are measured over the sweeps in §3.
- [ ] Every reported number has a median and 95% CI; distributions are shown.
- [ ] JIT cost is reported separately, and the break-even size N* is computed and checked against end-to-end runs.
- [ ] The full campaign reproduces from a single documented command; `--quick` runs in minutes.
- [ ] The plots and summary table in §7 exist and are generated by `analyze.py` from raw data only.

---

## 9. Open questions: ask before you assume

- Which machine is the reference machine for the final numbers (and do we have root for governor and cache control)?
- Which ATLAHS AI traces should be used, and are they available locally or must they be generated?
- Upper bound on trace size (disk and RAM limits of the reference machine)?
- Which decoder variants are final and which are experimental (and should be benchmarked anyway)?
