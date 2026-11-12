# Presenter notes: `presentation.tex`

One block per slide; slide numbers match the PDF. Aim for about 20 minutes: about 1 minute per content slide, 10 seconds per step title slide.

The thread through the talk: **Step 1 fixes the format; steps 2–4 make sure it costs nothing.** On each step title slide, point at the highlighted box in the pipeline before going into detail.

## Old design and problem

**1. Title.** Who we are, supervisor. One sentence: "We made LogGOPSim's trace format describe itself, then made every stage that touches it faster."

**2. LogGOPSim today.**
- LogGOPSim predicts how long an MPI program runs.
- Two tools: `txt2bin` converts the GOAL text once, and LogGOPSim reads the binary in every run.
- Point at `SerializedGraph`: the reader lives inside the simulator and hard-codes the byte offsets.

**3. A schedule is a dependency graph.**
- Walk the example: the rank 0 calc is a root (counter 0), and the final calc waits for 2 nodes.
- Explain the counters carefully, because step 4 is about where they are stored.
- Output: the finishing time of each rank, and the maximum is the predicted runtime.

**4. The problem.**
- 39 bytes per node, whatever the values; most traces need 2–4.
- Closed: ATLAHS hides the GPU in `nic` and glues five numbers into the tag. Nothing in the file says so, so a wrong guess gives wrong results silently.
- Hard-wired: every simulator needs its own copy of the offsets.

**5. Our design in four steps.**
- Walk the pipeline left to right, then give the order of the talk: format (1), reading (2), converting (3), simulating (4).

## Step 1: Self-describing format

**6. Title slide.** The file itself is the change.

**7. The file describes itself.**
- The schema header holds each field's name, type, bit position, width and base.
- Records have a fixed size within a file, so indexing is direct: no scanning, which is the same property the legacy format had.
- A new magic number distinguishes the formats, and legacy files go through the same reader.

**8. Each field gets only the bits it needs.**
- Walk the figure: base 1000 in the header, offset 2 in the record, value 1002.
- If all values are equal, the offset is always 0, so the field takes 0 bits.
- The grand-picture box connects to the next steps: the layout depends on all values, so conversion needs two passes (step 3), and decoding is no longer a fixed struct copy (step 2).

**9. Custom fields: the ATLAHS case.**
- Before and after the same send: the tag no longer encodes anything.
- Each simulator binds only what it uses, and load-time checks reject missing or too-small fields.
- The two htsim bugs were invisible in the old encoding. The trace is still 54% smaller.

## Step 2: JIT decoding

**10. Title slide.** Reading: a flexible format must not be slower.

**11. Flexibility has a cost at read time.**
- With the first generic decoder, decoding was 40–47% of the simulation time on broadcast and dissemination.
- alltoall shows 2% because message matching dominates there.

**12. Build the decoder once, run it for every node.**
- Sequence diagram: setup once (map the file, check bindings), then repeated `read_node` calls.
- The decode plan is the bridge: one step per field the simulator needs.

**13. Compiled runners hard-code the layout.**
- In the generated code, every number is a literal, so the compiler emits a few shifts and masks.
- `jit` costs about 340 ms the first time and is cached after that. `orc` compiles in memory in 2.5 ms but loads LLVM. The interpreter is the fallback.
- Result: 2× faster than the interpreter and within 6% of code compiled ahead of time, so decoding is no longer the bottleneck.

## Step 3: Parallel conversion

**14. Title slide.** Step 1 made conversion two passes; this step makes them use all cores.

**15. Why conversion reads the input twice.**
- Tag example: you can't widen records you've already written.
- Pass 1 keeps only statistics, and pass 2 packs. Memory is bounded by one rank.
- Pass 1 was a single sequential scan: 22–38 s on the large traces.

**16. Pass 1 in parallel.**
- Ranks are independent text blocks. Cut after each closing brace (comments are skipped) and give whole ranks to OpenMP threads.
- The scanner is unchanged; it now reads from memory instead of a `FILE*`.
- Min, max and counts merge in any order, so the result is the same for any thread count.

**17. Same result, and what it took.**
- Left: identical bytes and identical errors (the lowest invalid rank, with its line number in the whole file), checked by differential tests.
- Right: two bottlenecks that only appeared with threads. Per-rank remapping in the legacy writer (45 s → 4 s, now `pwrite`), and `omp ordered` spinning that starved the writer (15 s → under 1 s, now a condition variable).

## Step 4: Lazy decoding

**18. Title slide.** Simulating: decode a node only when it runs.

**19. Where the simulator spends memory.**
- Top row: counters are decremented inside the mapped file. The mapping is private (so the file stays reusable), so each written page is copied (4 KiB). Ready nodes are kept as 80-byte decoded copies.
- Bottom row: counters go into an array (4 B/node), ready nodes are 4-byte indices, and each node is decoded once, when it runs.

**20. Implementation.**
- The whole change is this loop. The +1 trick: 0 means "not read from the file yet", so no startup pass over the file is needed.
- Same reader, so it works for both formats and every decoder. Opt-in flag.

**21. Same simulation, fewer copies.**
- The full event log is identical, not just the final time.
- Trade-off: 4 B/node up front (about 4 GB at a billion nodes), still much less than the copied pages. It can't be combined with `--save-mem`.

## Benchmarks

**22. Files are smaller.** 53–93%. Constant and narrow fields shrink most, and even random 64-bit values save half.

**23. Simulations are faster than the original.**
- Complete runs, including startup, against the last upstream commit: 10–24% faster, identical results.
- alltoall is dominated by matching. The original crashes on wide_64k.

**24. Parallel conversion and lazy decoding.**
- Pass 1 is 2.6–4.6× faster with 8 threads and flattens beyond 4 (cause not measured yet). Pass 2 is still sequential, so a full conversion gains 1.3–1.5×.
- Lazy decoding: about a third less peak memory and 2–11% faster.
- Different machine (WSL2) from slide 23 (M3 laptop).

## Outlook

**25. Outlook.**
- Next: parallelise pass 2, dictionary encoding, automatic decoder choice, Joshua's full study on Euler.
- GPU: conversion is embarrassingly parallel (independent ranks and nodes, min/max reductions, per-node packing). A CUDA kernel could use thousands of threads and the GPU's memory bandwidth. Exploratory, no numbers yet.

**26. Who did what.** Read it briefly.

**27. Questions.** Prepare for:
- Why isn't lazy decoding the default? (It could be. It's opt-in so that default behaviour doesn't change.)
- Which decoder should be the default? (Short runs: interpreter or templated; long runs: `jit` or `orc`.)
- What if the counter array doesn't fit? (4 B/node. A trace that large is already tens of GB on disk. It fails at startup, not mid-run: `bad_alloc`, or the OOM killer on Linux.)
- Why not keep all ranks in memory instead of parsing twice? (Large schedules don't fit. A temporary binary file is a prototyped alternative.)
