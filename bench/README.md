# Benchmarks and profiling

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

## Traces

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

## decode_bench

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

## Which runner wins where

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

## Comparing before and after a change

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

## Profiling

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

## Decode share of full simulations

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

## Shared memory-work changes

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

## Dependency access improvements

`read_dependents(rank, index, event)` selects the start or completion list before reading
its position and count. It reuses `FieldSource` to prepare bit access once; zero-bit
fields do not load record bytes. The legacy scheduler also selects one list. No validation
is removed and no dependency arrays are copied.

The incremental baseline and each optimization's measurements are in
`build/bench/results/dependency-optimization/`, alongside the final eight-trace run
and complete-simulation samples. Setup and compilation are excluded from Replay;
root nodes are already loaded before its timer starts.

## Writing / conversion

```sh
python3 bench/writing/conversion_bench.py --build-dir build --repetitions 5
```

Generates broadcast and dissemination GOAL inputs with schedgen, warms both conversion
paths, then interleaves whole-process conversion runs. Saves raw samples, medians and
output sizes in `build/bench/results/writing/conversion.json`. Generation is excluded.
The current legacy writer remaps per rank and calls system-wide `sync()` at finish;
the new writer closes its output stream. These existing I/O policies are retained,
so the numbers compare implementations and do not isolate format or disk throughput.
