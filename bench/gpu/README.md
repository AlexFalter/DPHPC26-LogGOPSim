# GPU bulk decoding (experimental, branch `cuda-experiments`)

Decodes **every** record of a trace on an NVIDIA GPU, one thread per record, in both formats.
The GPU reads the self-describing header and builds its own decoder kernel at load time,
compiled with NVRTC. This is the GPU counterpart of the C JIT (`LOGGOPSIM_DECODER=jit`).

## Scope: what this is and is not

* **Not a GPU LogGOPSim.** LogGOPSim's event loop is a sequential discrete-event simulation that
  decodes only the frontier. `simulate` is ~95% of wall time and decoding ~10–20% of that, so
  even infinitely fast decoding would change end-to-end simulation time by at most that much.
* **Whole-trace passes**: decoding into columns for analytics, validation (the O(edges)
  `validate_dependency_indices` scan) and statistics. Here the question is how fast a
  self-describing trace can be consumed by a second, very different reader, and what limits it.

## Build and run

The CUDA toolkit must be installed (driver API + NVRTC only; no nvcc and no CMake CUDA language).
The kernels are compiled at run time.

```sh
cmake -S src -B build -DLOGGOPSIM_BUILD_CUDA=ON -DCUDAToolkit_ROOT=/usr/local/cuda   # + usual options
cmake --build build --target gpu_decode_bench
build/gpu/gpu_decode_bench --trace x.sd.bin --kernel specialized --cpu-check --validate
```

| Option | Meaning |
|---|---|
| `--kernel specialized` (default) | kernel generated from the trace's header: every offset, shift, mask and base a constant (≈ `jit`) |
| `--kernel generic` | one kernel for every schema; the plan is read from `__constant__` memory (≈ `plan`) |
| `--fields sim` / `scalar` | with or without reading the dependency lists (as `decode_bench_micro --fields`) |
| `--transfer copy` (default) | `cuMemcpyHtoD` from the pageable `mmap` |
| `--transfer pinned` | page-lock the mapping once (`cuMemHostRegister`, reported as setup), then DMA |
| `--transfer mapped` | page-locked and mapped: the kernel reads host memory over PCIe, no copy |
| `--transfer direct` | the kernel reads the pageable mapping itself; needs pageable memory access (GB10) |
| `--output columns [--columns-to-host]` | also write the decoded fields as 11 column arrays (and copy them back) |
| `--dump-columns PREFIX` | write the decoded columns to `PREFIX.<field>.u<bits>` (`numpy.fromfile`-ready) |
| `--validate` | also run the dependency-index check on the GPU |
| `--cpu-check` | decode the same file on the CPU through `LogGOPSimAdapter` and compare checksums |
| `--gpu-cache DIR`, `--gpu-cold` | NVRTC cubin cache (default `$LOGGOPSIM_GPU_CACHE`, else `~/.cache/loggopsim-gpu`); `--gpu-cold` uses a fresh empty one |

Output is one JSON object: setup phases (`context`, `generate`, `compile`, `module_load`,
`alloc`, `register`), median `h2d` / `kernel` / `validate` / `d2h` / `total` per repetition,
ns/record, GB/s, the checksum and, with `--cpu-check`, the CPU result.

## Design

| File | Role |
|---|---|
| `src/LogGOPSim/binary_reading/decoding/gpu/GpuPlan.{hpp,cpp}` | Builds the decode plan LogGOPSim's reader would build (same bindings, same checks) and flattens it into POD structs. Builds the rank table. Holds the CUDA sources (generic kernel, specialised-kernel generator, validation kernel). Legacy files map onto the same plan: fixed byte offsets, ANY = `~0`, 32-bit indices. |
| `src/LogGOPSim/binary_reading/decoding/gpu/GpuRuntime.{hpp,cpp}` | Driver-API context, NVRTC compile with an on-disk cubin cache (key: FNV-1a of source + target + options + NVRTC version; atomic rename, as in `CCompiler.cpp`), module loading. Falls back to PTX when NVRTC does not know the device. |
| `bench/gpu/gpu_decode_bench.cpp` | The driver. |

* The whole file is copied (padded with 16 zero bytes). A field is read as two aligned 64-bit
  words and a funnel shift, so any record size and alignment works (9-byte SD records, 39-byte
  legacy records). The specialised kernel loads each record word once and extracts fields with
  constant shifts.
* Thread → (rank, index): thread 0 of each block binary-searches the rank of the block's first
  node; the other threads walk forward from there. Empty ranks are handled.
* Same validity rules as the CPU: out-of-range offsets, wildcard codes, sends to ANY and list
  slices outside the rank's appendix. Invalid records are counted, and a non-zero count fails
  the run.
* **Checksum = `bench/micro/decode_bench.cpp`'s `node_hash`**, summed with a block reduction. The
  GPU result is therefore directly comparable with `decode_bench_micro` and the campaign tables.

## Correctness

All 33 `*.sd.bin` traces of the local campaign up to 250 MB were checked: every workload, the
`inferred`/`custom`/`wide` schemas, and 8–32768 ranks. Each ran with both kernels, with and
without lists, and with GPU validation. **All 132 cases equal the CPU checksum, with 0 invalid
records.** The legacy files checked (6 traces × 2 kernels) also equal their CPU checksum.

## Results so far (local, WSL2, i9-9900K + RTX 2080 on PCIe 3.0 x16; not yet a reportable campaign)

Medians of 5–10 repetitions after one warm-up, warm page cache, `--fields sim`,
stencil3d-p64-*-inferred (9–10 B/record):

| records | file | H2D copy (pageable) | specialised kernel | generic kernel | GPU validate | total per scan |
|---|---|---|---|---|---|---|
| 1e6 | 12 MB | 1.4 ms (8.7 GB/s) | 0.14 ms | 0.20 ms | 0.08 ms | 1.9 ms |
| 1e7 | 142 MB | 16.4 ms | 1.16 ms | 1.31 ms | 0.66 ms | 20 ms |
| 5e7 | 731 MB | 82 ms | 4.7 ms | 6.0 ms | 3.0 ms | 97 ms |
| 1e8 | 1.58 GB | 171 ms (9.2 GB/s) | 7.7 ms (0.077 ns/rec, 143 GB/s) | 12.0 ms (0.12 ns/rec) | 5.1 ms | 200 ms |

For comparison, the CPU (one core, same process) decodes at 56–90 ns/record, i.e. 5.6–9 s for
1e8 records. `validate_dependency_indices` takes 15 ms at 1e6 and 1.9 s at 1e8 in the campaign.

Fixed costs: **CUDA context ≈ 0.44 s per process**. NVRTC cold compile ≈ 22 ms
(21–23 ms over 5 runs; ≈ 250 ms only on the very first run, while NVRTC's own libraries are
loaded). A cache hit costs ≈ 2 ms (`cuModuleLoadData`). For comparison, the cc-based C JIT
takes 240 ms.

### Format comparison on the GPU (1e7 records, specialised kernel, copy)

| trace | legacy file → GPU total | self-describing file → GPU total | CPU ns/record, legacy vs SD `plan` |
|---|---|---|---|
| stencil3d-p32768 | 454 MB → 51.4 ms | 109 MB → 14.5 ms | 31 vs 76 |
| dp_nccl-p64 | 439 MB → 50.9 ms | 137 MB → 16.8 ms | 28 vs 77 |
| stencil3d-p64-wide | 464 MB → 50.5 ms | 222 MB → 24.7 ms | 29 vs 75 |

### What this shows

1. **On the GPU, bytes are the bottleneck, not instructions.** About 85% of a scan is the host-to-device copy at
   1e8 records. The compact self-describing records (≈ 10 B vs 39 B) therefore make the GPU scan
   **2–3.5× faster than legacy**. On the CPU the legacy reader is ≈ 2.6× faster per record. The
   format's narrow fields pay off once decoding is no longer instruction-bound.
2. **Specialisation still helps, but barely matters on the GPU.** The specialised kernel is
   1.4–1.6× faster than the generic one (as `jit` vs `plan` on the CPU), but that saves ~4 ms
   out of 200 ms at 1e8. Break-even of specialised vs generic, counting transfer, is about
   22 ms / 0.043 ns ≈ 5e8 records. Below that, pick the generic kernel. Above it, or when the
   trace stays resident on the device, specialise.
3. **GPU vs one CPU core for a one-shot full scan:** ≈ 0.65 s (context + copy + kernel) vs
   5.6–7.8 s at 1e8. Break-even ≈ 0.46 s / (78 − 2) ns ≈ 6e6 records, similar to the C JIT's
   N* ≈ 6.4e6. Below a few million records the CUDA context alone loses.
4. **Pinning and zero-copy do not pay off on a discrete GPU for a single scan.**
   `cuMemHostRegister` costs ≈ 7.5 GB/s worth of time, more than the faster DMA saves, and WSL
   refuses to pin 1.58 GB. `mapped` reads over PCIe at ≈ 7 GB/s, slower than copy + kernel. GB10
   (coherent unified memory, `direct`) is expected to differ and is the main cluster experiment.
5. **Materialising decoded columns costs more than decoding them.** The 11 columns are
   39 B/record (≈ 4× the packed record), and copying them back took 34 ms at 1e7. The compact
   form should stay compact as long as possible.

## Caveats

* Single machine, WSL2 (no frequency control, WSL pinned-memory cap), warm page cache, no CIs
  yet. Run the harness methodology (repetitions until CI ±5%) before reporting numbers.
* The kernels decode LogGOPSim's eleven fields, not custom fields (`--fields all` of
  `decode_bench_micro` is not implemented).
* `check_peer_range` (peer < rank count, an O(1) header check) and NodeValidator's ATLAHS
  checks are not repeated on the GPU.

## Side finding (pre-existing, not caused by this code)

Legacy and self-describing files of the same GOAL input decode to different `tag` values on
**calc** operations (`--dump-columns` + diff). Legacy stores 0. The self-describing writer stores
offset 0, which decodes to the field's base, i.e. the trace's smallest tag (7 in alltoall, 12345 in
`wide`). LogGOPSim ignores tags on calc, so simulation results agree, but the decoded node
differs. That is why the micro-benchmark checksums of the two formats agree for most traces but
not these.

## Next steps

* Cluster: GB10 (`--transfer direct` on unified memory, aarch64) and the 2080 Ti nodes (PCIe).
* Harness integration (a `gpu_micro` experiment kind in `run.py`, a table in `analyze.py`) with
  the usual repetition/CI rules; cold page cache (disk → GPU).
* Optional: chunked, pipelined copy + decode (overlapping the PCIe transfer with the kernel);
  decoding custom fields.
