# CLAUDE.md

Working notes for Claude Code sessions in this repository. Read `README.md` (toolchain, source
layout) and `bench/README.md` (benchmark harness, system under test) first.

## Current work: CUDA exploration (branch `cuda-experiments`)

Started 2026-11-18. If a session ends mid-task, continue from "Next steps" below and keep this
file up to date after every step.

### Decision: does a CUDA path make sense?

Yes, but only as a **bulk decode/scan backend for the self-describing format**, not as a GPU
LogGOPSim. Evidence from the local campaign (`bench/results/tables/*/phases.md`, `decode.md`):

* `simulate` is ~95% of wall time; LogGOPSim's event loop is a sequential discrete-event loop
  (global priority queue, matching queues). Porting it to the GPU is a separate research project
  and has nothing to do with the trace format.
* Per-record decode on the CPU costs 40-95 ns (legacy 39-45, jit 50-59, plan 90-95), against
  roughly 500 ns of simulation per record, so even infinitely fast frontier decoding would buy at
  most 10-20% end to end. Frontier-only decoding also cannot be batched for a GPU.
* What the GPU *can* show for the project's research question: fixed-size bit-packed records are
  exactly what GPUs unpack well (one thread per record, cf. FastLanes / GPU Parquet decoding), and
  the self-describing header lets a second consumer generate its **own specialised kernel at load
  time** (NVRTC), mirroring the C JIT. This gives: GPU interpreter vs GPU NVRTC-specialised
  kernel, NVRTC cold vs warm cache (a GPU break-even N*), transfer cost (PCIe on the RTX 2080 /
  2080 Ti, unified memory on the cluster's GB10), and whole-trace passes such as the O(edges)
  `validate_dependency_indices` scan (~4% of NEW's end-to-end time at 1e8 records).
* Honest framing for the report: the GPU path accelerates bulk decoding, validation and analytics
  over a trace; it does not accelerate LogGOPSim's event loop.

### Design

* `src/LogGOPSim/binary_reading/decoding/gpu/` (optional, CMake `-DLOGGOPSIM_BUILD_CUDA=ON`):
  * `GpuPlanPod.h`: POD step/plan structs shared by host C++23 code and device code (nvcc C++17,
    which cannot include the C++23 project headers).
  * `GpuPlan.{hpp,cpp}` (g++, C++23): `DecodePlan` → POD plan with semantic roles; CUDA source
    generator for the specialised kernel (mirrors `CCompiler::generate_decoder_source`).
  * `GpuKernels.cu` (nvcc): generic interpreter kernel (plan in `__constant__` memory),
    dependency-index validation kernel.
  * `NvrtcCompiler.cpp`: NVRTC compile + on-disk cubin cache (`$LOGGOPSIM_GPU_CACHE`, key = FNV-1a
    of source + arch + options), driver-API module load and launch.
* `bench/gpu/gpu_decode_bench.cpp`: driver. Same checksum (`node_hash` + `mix`) as
  `bench/micro/decode_bench.cpp`, so the GPU checksum must equal the CPU one for the same trace and
  `--fields`. JSON output like `decode_bench_micro`. Measures H2D, kernel, D2H, NVRTC
  (cold/warm), records/s and GB/s, for the generic and specialised kernels, with transfer modes
  `copy` (pageable), `pinned` (cudaHostRegister on the mapping), `chunked` (pipelined streams)
  and `direct` (pageable memory access, GB10 only).

### Environment

* Local: WSL2 Ubuntu 24.04, i9-9900K, RTX 2080 (sm_75, 8 GB), driver 581.57 (CUDA 13.0),
  toolkit **CUDA 12.9 at `/usr/local/cuda`, not on PATH** (`export PATH=/usr/local/cuda/bin:$PATH`).
  Build outside `/mnt/c`, e.g. `~/lgs-bench/cuda-build`.
* Cluster: 2080 Ti nodes (sm_75, PCIe) and GB10 (`--gpus gb10:1`, aarch64, unified memory);
  see the memory note on the student cluster for Slurm limits.

### Progress

- [x] Decision and design (this file).
- [x] `gpu/GpuPlan.{hpp,cpp}`: POD plan/rank table, generic + specialised CUDA sources (as raw
      strings; both kernels go through NVRTC, so there is **no .cu file and no nvcc/CMake CUDA
      language**; the design above originally planned a `GpuKernels.cu`).
- [x] `gpu/GpuRuntime.{hpp,cpp}`: driver-API context, NVRTC compile + cubin cache, module, buffers.
- [x] `bench/gpu/gpu_decode_bench.cpp` + CMake (`-DLOGGOPSIM_BUILD_CUDA=ON`, target
      `gpu_decode_bench`, binary at `<build>/gpu/gpu_decode_bench`).
- [x] Builds in WSL; **checksums equal the CPU's** (in-process `--cpu-check` and
      `decode_bench_micro`) for both kernels, `--fields sim` and `scalar`, on
      stencil3d-p64-n1e6-inferred (712de9eed30a6184 / bba8f24f58baba15).
- [x] Correctness on all 33 `~/lgs-bench/traces/*.sd.bin` ≤ 250 MB (every workload, inferred /
      custom / wide, 8–32768 ranks) × both kernels × sim/scalar: **132/132 match the CPU**.
- [x] Numbers at 1e6–1e8 for copy/pinned/mapped; NVRTC cold vs warm (see below).
- [x] Legacy format on the GPU (same kernels) + `--dump-columns`; format comparison done.
- [x] GPU break-even N* written up in `bench/gpu/README.md` ("What this shows").
- [x] `bench/gpu/README.md` (scope, usage, design, correctness, results, caveats, side finding);
      pointer section in `bench/README.md`.
- [x] `--columns-to-host` pageable fallback verified at 5e7 (1.95 GB columns, D2H 210 ms).
- [ ] Integrate into the harness (`configs`, `run.py` kind `gpu_micro`, `analyze.py` table) with
      the CI/repetition rules; cold page cache.
- [ ] Cluster runs: GB10 (`--transfer direct`), 2080 Ti.
- [ ] Report/slides paragraph (honest framing, see Decision) — only if the user asks.
- Nothing is committed yet (the user did not ask); all changes are in the working tree of
  `cuda-experiments`.

### First numbers (2026-11-18, RTX 2080, stencil3d-p64-n1e6-inferred, 9 B records)

| | kernel ns/record (sim / scalar) | CPU same process (plan) |
|---|---|---|
| specialised (NVRTC) | 0.138 / 0.090 | 93 / 58 ns |
| generic (constant-memory plan) | 0.192 / 0.137 | |

H2D of the 12 MB file ≈ 1.3 ms (~9.5 GB/s, pageable). GPU dependency validation 0.08 ms vs CPU
`validate_dependency_indices` 15 ms. NVRTC cold compile ≈ 250 ms (C JIT: 240 ms), cubin cache hit
≈ 0 ms. **CUDA context creation ≈ 0.5 s** per process: a fixed cost that dominates small traces.

### Scale sweep (2026-11-18, RTX 2080 PCIe 3.0, stencil3d-p64-*-inferred, --fields sim, medians)

| records | file | copy H2D (pageable) | pinned H2D | spec kernel | generic kernel | GPU validate | total (copy) |
|---|---|---|---|---|---|---|---|
| 1e6 | 12 MB | 1.4 ms (8.7 GB/s) | 1.0 ms (12.1) | 0.14 ms | 0.20 ms | 0.08 ms | 1.9 ms |
| 1e7 | 142 MB | 16.4 ms | 11.5 ms | 1.16 ms | 1.31 ms | 0.66 ms | 20 ms |
| 5e7 | 731 MB | 82 ms | 59 ms | 4.7 ms | 6.0 ms | 3.0 ms | 97 ms |
| 1e8 | 1.58 GB | 171 ms (9.2 GB/s) | WSL refuses | 7.7 ms (0.077 ns/rec, 143 GB/s) | 12.0 ms (0.12 ns/rec) | 5.1 ms | 200 ms |

* Transfer dominates (~95% of GPU time); kernel specialisation saves ~4 ms of 200 ms at 1e8.
  GPU spec-vs-generic break-even ≈ 22 ms / 0.043 ns ≈ 5e8 records: specialisation hardly matters
  on the GPU unless the trace stays resident. What matters is bytes moved → the compact format
  (≈10 B/record vs 39 B legacy) directly cuts PCIe time ~4×.
* `pinned`: cuMemHostRegister costs ~7.5 GB/s (724 ms for 731 MB), more than it saves for one
  scan; WSL refuses to pin 1.58 GB. `mapped` (zero-copy over PCIe): kernel reads at ~7 GB/s,
  slower than copy + kernel. Both are expected to differ on GB10 (coherent unified memory).
* Columns output (39 B/record of decoded SoA) back to host: 34 ms at 1e7, i.e. materialising
  decoded data costs more than decoding; `--columns-to-host` now falls back to pageable memory
  when pinned allocation fails (WSL cap) — not yet re-run at 5e7/1e8.
* NVRTC: first compile in a fresh boot/page cache ≈ 250 ms (loading NVRTC itself); a cold
  cache miss afterwards ≈ **22 ms** (5 runs: 21.3–23.1); cache hit: cuModuleLoadData ≈ 2 ms.
  cc-based C JIT: 240 ms. CUDA context creation ≈ 0.44 s per process.
* One-shot full scan of 1e8 records: GPU ≈ 0.44 s context + 0.2 s ≈ 0.65 s vs CPU single thread
  5.6 s (jit, 56 ns/rec) – 7.8 s (plan). GPU vs CPU-plan break-even ≈ 0.46 s / (78 − 2) ns ≈ 6e6
  records (similar to the C JIT's N* ≈ 6.4e6).

### Legacy files on the GPU (same kernels; `create_legacy_plan/create_legacy_image`)

Legacy 39-byte records fit the same Plan (fixed byte offsets, ANY = ~0 as wildcard = max + 1,
32-bit indices), so `gpu_decode_bench` reads both formats. All checked cases match the CPU.
Medians, 1e7 records, copy transfer, specialised kernel:

| trace | legacy file → GPU total | SD file → GPU total | CPU ns/record legacy vs SD plan |
|---|---|---|---|
| stencil3d-p32768-n1e7 | 454 MB → 51.4 ms | 109 MB → 14.5 ms | 31 vs 76 |
| dp_nccl-p64-n1e7 | 439 MB → 50.9 ms | 137 MB → 16.8 ms | 28 vs 77 |
| stencil3d-p64-n1e7-wide | 464 MB → 50.5 ms | 222 MB → 24.7 ms | 29 vs 75 |

**Headline:** on the CPU the legacy reader is ~2.6× faster per record than the self-describing
plan interpreter; on the GPU the self-describing file is 2–3.5× faster end to end, because the
bottleneck is bytes over PCIe, not instructions.

**Side finding (pre-existing, CPU-side, not a GPU bug):** legacy and SD checksums differ for
alltoall-p64-n1e6 and the `wide` traces. Diffing the GPU's decoded columns (`--dump-columns`)
shows the only difference is `tag` on **calc** operations: legacy stores 0, the self-describing
writer stores offset 0, which decodes to the field's base = the trace's smallest tag (7, 12345).
LogGOPSim ignores tags on calc, so simulations agree (the gate passed). Worth telling the team:
a field absent on an operation decodes to the field's base, not 0. Not changed.

### How to build and test (WSL)

Scripts in the session scratchpad are not durable; the equivalent commands:

```sh
R=/mnt/c/Users/Joshua/Desktop/Programing/DPHPC26-LogGOPSim; B=~/lgs-bench/cuda-build
cmake -S $R/src -B $B -G Ninja -DLOGGOPSIM_BUILD_CUDA=ON -DCUDAToolkit_ROOT=/usr/local/cuda \
  -DLOGGOPSIM_BUILD_BENCHMARKS=OFF -DBUILD_TESTING=OFF -DBUILD_ATLAHS=OFF -DLOGGOPSIM_WITH_LLVM=OFF \
  -DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON -DCMAKE_PROJECT_LogGOPSim_INCLUDE=$R/bench/micro/inject.cmake
ninja -C $B gpu_decode_bench bench_decode_micro
LOGGOPSIM_GPU_CACHE=~/lgs-bench/gpu-cache $B/gpu/gpu_decode_bench \
  --trace ~/lgs-bench/traces/stencil3d-p64-n1e6-inferred.sd.bin --kernel specialized --cpu-check --validate
```

From Git Bash, call WSL as `MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash script.sh` (inline
`bash -lc '...'` loses `$VARS`). `CMAKE_DISABLE_FIND_PACKAGE_Clang=ON` is needed because the WSL
LLVM 18 Clang CMake package is broken (missing libclangBasic.a).

### Next steps

1. Harness integration: read `bench/run.py` + `bench/lib/runner.py` for how `decode_micro` cells
   are defined/run, add a `gpu_micro` kind that calls `<build>/gpu/gpu_decode_bench` (build via
   `bench/build.sh` with `-DLOGGOPSIM_BUILD_CUDA=ON` when `nvcc`/CUDA is found), and a
   `gpu_decode` table in `analyze.py`. Keep `--quick` working without a GPU (skip the kind).
2. Cluster job variants for GB10 (`--transfer direct`) and 2080 Ti.
3. Optional: pipelined chunked copy + decode; custom fields.
