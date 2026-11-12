# Presenter notes: `presentation.tex`

One block per slide. Aim for 12–15 minutes in total: about 1 minute per content slide, a few seconds per divider.

## 1. Title
- Who we are, supervisor. One sentence: "We made LogGOPSim's trace format describe itself, then made it fast."

## 2. LogGOPSim today
- LogGOPSim predicts the runtime of an MPI program from a trace: per rank, a dependency graph of send, receive and calc operations.
- Two tools: `txt2bin` converts the GOAL text once, and LogGOPSim reads the binary in every run.
- Point at `SerializedGraph`: the reader is built into the simulator and hard-codes the byte offsets.

## 3. The problem
- Every node is 39 bytes, whatever its values. Most of our traces need 2–4 bytes.
- No room for new information. ATLAHS (GPU workloads) puts the GPU into `nic` and glues five numbers into the tag. Nothing in the file says so, so a wrong guess gives wrong results without an error.
- Every simulator that wants to read the file needs its own copy of the offsets.

## 4. Our design in four steps
- Walk the pipeline left to right, then give the order of the talk: first the format (1), then making it fast to read (2), then the converter (3), then the simulator (4).

## 5–7. Step 1: Self-describing format
- The header lists every field: name, type, bit width, base. A reader needs no built-in knowledge.
- Each field uses only the bits it needs. Offset encoding: the base is in the header, so a constant field costs 0 bits.
- Custom fields come from a TOML file. A simulator binds only the fields it uses, and a missing or too-wide field fails at load time.
- ATLAHS: eight explicit fields replace the guesses. This exposed and fixed two htsim bugs (wrong destination GPU, tag collisions), and the trace is still 54% smaller.

## 8–9. Step 2: JIT decoding
- Catch: positions are only known when the file is opened. The first generic decoder interpreted the schema for every field of every node, which took up to 47% of the simulation time.
- Once per file we build a decode plan and generate C in which every shift and mask is a constant. `jit` compiles it with `cc` and caches the result; `orc` does the same in memory with LLVM.
- If no compiler is available, the plan interpreter is used. Only numbers from the header go into the code, so a trace can't inject code.

## 10–11. Step 3: Parallel conversion
- Why two passes: the layout must be fixed before the first record is written, so pass 1 collects every field's min/max.
- Ranks are independent blocks in GOAL. We map the file, cut it after each rank's closing brace and parse the blocks on OpenMP threads.
- Statistics are min/max/counts, so the merge order doesn't matter. The output bytes and error messages are identical to the sequential version (differential tests at 1–8 threads).
- If asked, two fixes came out of this: the legacy writer remapped the whole file per rank (now `pwrite`), and `omp ordered` spinning starved the writer (now a condition variable).

## 12–13. Step 4: Lazy decoding
- Profiling showed two kinds of waste: a full decoded copy of every ready node, and dependency counters decremented in the mapped file, which copies every page that is written.
- With `--lazy-decoding`, ready nodes are 4-byte indices decoded when they run, and counters live in a 4-byte-per-node array filled on first use.
- Same simulation: the full `-v` event log is identical. If asked: it can't be combined with `--save-mem`, because it never writes the file.

## 14. Files are smaller
- 53–93% smaller. Constant and narrow fields shrink most, and even random 64-bit values save half.

## 15. Simulations are faster
- Complete runs, including startup, against the last upstream commit: 10–24% faster on the larger collectives, with identical results.
- alltoall is dominated by message matching, so decoding hardly matters there. The original crashes on wide_64k.
- `jit` reaches the speed of code compiled ahead of time.

## 16. Parallel conversion and lazy decoding
- Pass 1 is 2.6–4.6× faster with 8 threads, but flattens beyond 4 threads (cause not measured yet). Pass 2 is still sequential, so a complete conversion gains 1.3–1.5×.
- Lazy decoding: about a third less peak memory and 2–11% faster.
- Note the different machine (WSL2) from slide 15 (M3 laptop).

## 17. Outlook
- Next: parallelise pass 2, dictionary encoding, automatic choice of decoder, Joshua's full study on Euler.
- GPU: conversion is embarrassingly parallel (independent ranks and nodes, min/max reductions, per-node bit packing). A CUDA kernel could use thousands of threads and the GPU's memory bandwidth. Exploratory, no numbers yet.

## 18. Who did what
- Read it briefly; don't walk through every line.

## 19. Questions
Possible questions to prepare for:
- Why not make lazy decoding the default? (We could. It's opt-in so that the default behaviour is unchanged.)
- Which decoder should be the default? (Cold start favours the interpreter or templates; long runs favour `jit` or `orc`.)
- Memory of the lazy counter array: 4 B per node, about 4 GB at a billion nodes, still far less than the copied pages it replaces.
