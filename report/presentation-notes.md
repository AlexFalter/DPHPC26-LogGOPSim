# Presenter notes: `presentation.tex`

One block per slide; slide numbers match the PDF. Aim for about 23 minutes: about 1 minute per content slide, 10 seconds per step title slide. If time runs short, skip slide 11 (htsim bugs) first; slide 10 already mentions them.

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

**9. One reader for every simulator.**
- This answers the third problem from slide 4 (hard-wired offsets).
- Each simulator only declares its node struct and which fields go into which members. LogGOPSim binds six; ATLAHS binds more of the same file.
- The check uses only the header: the file says 1000–1003, the member is a `uint8_t`, so the run stops before any node is read. Copying would have silently given 234.
- Legacy files go through the same bindings; the simulators contain no format code.

**10. Custom fields: the ATLAHS case.**
- Before and after the same send: the tag no longer encodes anything.
- The TOML file is all a user writes to add a field: a name and a type. Every operation then supplies a value, and the converter infers the range and encoding exactly as for standard fields. No code changes in the converter.
- Each simulator binds only what it uses, and load-time checks reject missing or too-small fields.
- The trace is still 54% smaller. The two htsim bugs come on the next slide.

**11. Explicit fields exposed two htsim bugs.**
- Tag collision: walk the figure. Message A has sequence 12345 (five digits), message B has chunk 10; glued together, both give the same tag. htsim matched on that one number, so it could pair a receive with the wrong send without any error. Now the five fields are compared separately, and a message that never matches stops the run.
- Wrong GPU: upstream used the sender's GPU index on both ends, so the wrong network endpoints were simulated. The new `peer_gpu_id` field had no place in the old trace.
- Point: these bugs were invisible while the information was hidden in other fields.

## Step 2: JIT decoding

**12. Title slide.** Reading: a flexible format must not be slower.

**13. Flexibility has a cost at read time.**
- With the first generic decoder, decoding was 40–47% of the simulation time on broadcast and dissemination.
- alltoall shows 2% because message matching dominates there.
- The generic decoder was slow because every field went through a `std::variant` and its own assignment function call.

**14. Build the decoder once, run it for every node.**
- Sequence diagram: setup once (map the file, check bindings), then repeated `read_node` calls.
- The decode plan is the bridge: one step per field the simulator needs.
- A differential test checks every runner against the exact decoder on random layouts and records.

**15. Compiled runners hard-code the layout.**
- In the generated code, every number is a literal, so the compiler emits a few shifts and masks.
- `jit` costs about 340 ms the first time and is cached after that. On macOS most of this is loading the new library, not compiling it (64 ms compile and 255 ms first load in an earlier measurement).
- `orc` compiles in memory in about 2.5 ms, but loading LLVM makes every run start about 9 ms later than `plan`. None of our benchmark traces is large enough for it to pay off in a complete run.
- The interpreter is the fallback.
- When only decoding, `jit` is about 2× faster than the interpreter and within 6% of code compiled ahead of time, so decoding is no longer the bottleneck.

## Step 3: Parallel conversion

**16. Title slide.** Step 1 made conversion two passes; this step makes them use all cores.

**17. Why conversion reads the input twice.**
- Tag example: you can't widen records you've already written.
- Pass 1 keeps only statistics, and pass 2 packs. Memory is bounded by one rank.
- Pass 1 was a single sequential scan: 22–38 s on the large traces.

**18. Pass 1 in parallel.**
- Ranks are independent text blocks. Cut after each closing brace (comments are skipped) and give whole ranks to OpenMP threads.
- The scanner is unchanged; it now reads from memory instead of a `FILE*`.
- Min, max and counts merge in any order, so the result is the same for any thread count.

**(No slide) Say before leaving "Pass 1 in parallel": same result, and what it took.**
- Identical bytes and identical errors (the lowest invalid rank, with its line number in the whole file), checked by differential tests.
- Two bottlenecks that only appeared with threads. Per-rank remapping in the legacy writer (45 s → 4 s, now `pwrite`), and `omp ordered` spinning that starved the writer (15 s → under 1 s, now a condition variable).

## Step 4: Lazy decoding

**20. Title slide.** Simulating: decode a node only when it runs.

**21. Where the simulator spends memory.**
- Top row: counters are decremented inside the mapped file. The mapping is private (so the file stays reusable), so each written page is copied (4 KiB). Ready nodes are kept as 80-byte decoded copies.
- Bottom row: counters go into an array (4 B/node), ready nodes are 4-byte indices, and each node is decoded once, when it runs.

**22. Implementation.**
- The whole change is this loop. The +1 trick: 0 means "not read from the file yet", so no startup pass over the file is needed.
- Same reader, so it works for both formats and every decoder. Opt-in flag.

**22b. One counter, three meanings.**
- Walk the table: 0 = not read from the file yet, k+1 = k dependencies still open, 1 = ready.
- Walk the example left to right: the first release reads 3 from the file and stores 4. That release is itself one of the 3 dependencies finishing, so it decrements at once to 3 (2 open). The brace marks that both steps happen in one release. Two more releases reach 1, and the index goes to the ready list.
- Why: the array starts zero-filled without touching the file, and each count is read exactly once. Storing +1 frees the 0, so there is no second flag array.
- A release at 1 means more releases than dependencies: corrupt trace, error.

**(No slide) Say before leaving the counter slide:**
- The full `-v` event log is identical in both modes, not just the final time. Checked on dissemination, ResNet and ring traces (14.7M–72.8M log lines), both formats.
- The file is never written, so concurrent runs can share it.
- Cost: 4 B/node up front (about 4 GB at a billion nodes), still much less than the copied pages.
- `--save-mem` is rejected together with `--lazy-decoding`. That's a fact, not a trade-off: `--save-mem` exists only to write counters back into the file, which lazy mode never does.

## Correctness

**(No slide) Say before the bug slide: same results as the original LogGOPSim.**
- The per-step slides each said "identical"; this is the overall evidence.
- One-off audit against the original: 1,480 schedules (60 Schedgen collectives, 1,420 random graphs), four network parameter sets each. Byte-identical legacy files, identical times in all 5,920 simulations.
- Decoders: a differential test with 300 random layouts and 2,000 records each; every runner must accept exactly what the exact decoder accepts.
- Robustness: 2,000 randomly corrupted files never crash; clean under AddressSanitizer and UBSan.
- "Identical" holds everywhere except one case where the original is wrong, which is this slide.

**A bug in the original LogGOPSim** (slide 29).
- Walk the trace: rank 0 sends 8 B (eager, below S = 64 KiB), so its send completes at once. Rank 1 computes for 100,000 and only then posts a 100,000 B receive, so the message is already waiting as unexpected.
- The original (upstream `76451de`, `src/LogGOPSim/LogGOPSim.cpp:370`) checks `elem.size > S`, and `elem` is the receive there. So it takes the rendezvous branch, completes rank 0's send a second time and moves rank 0's clock to the receive time. Result: 100,000 instead of 1,700. Its counter decrement (`Parser.hpp:685`, `(*dep_cnt)--`) has no check, so the extra decrement underflows silently.
- The original contradicts itself: the sender (`:328`) and the receive-posted-first path (`:447`) both decide by the message size.
- Fix: `matched_elem.size > S`, the message. Regression test: `tests/integration/test_simulation_equivalence.py:147`.
- Does it happen in practice? Not in generated collectives, where send and receive sizes match. But when Schedgen converts a recorded MPI run (`src/Schedgen/process_trace.cpp:618`), a receive gets the posted buffer size (`count × type size`), not the size that arrived. MPI allows a larger buffer, so a recorded trace can hit it. We have not seen it in a real trace ourselves.
- The 1,480-schedule audit excluded this case.

## Benchmarking study (Joshua)

Slide numbers in this section and below match the PDF (slide 30 is the "Benchmarking study" title). About 6–7 minutes in total. If time is short, skip 35 (workloads) and 36 (conversion) and only mention the summary cards on 37.

**30. Title slide.** Every branch measured on one harness, against the unmodified original.

**31. How we measured.**
- Five builds straight from their branches: the original (last upstream commit), master with the new format and all decoders, the lazy-decoding branch and the parallel-conversion branch. Same compiler and flags; the harness checks that the flags are identical.
- Seven generated workloads that look like real HPC and ML traffic. Sizes go up to 10^8 nodes (a 4.6 GB legacy file).
- Method follows Hoefler & Belli (SC'15): report medians with confidence intervals, repeat until the 95% CI is within ±5%, interleave configurations randomly so drift hits all of them equally.
- Every configuration is first checked against the original: same simulated times and the same per-operation timeline (`-V`). No speedup is counted for a wrong result.

**32. 10^8 nodes: up to 1.36× faster.**
- Read the table top to bottom: new format with the interpreter barely differs from the original; the compiled decoders give about 10%.
- The big jumps come from the restructured reader on the legacy file (1.22×) and lazy decoding (1.36×), not from the format.
- Grey numbers are within 5%. Why 5%: the same source built with and without our timing hooks differs by 5% with non-overlapping CIs. That is code layout (Mytkowicz et al., ASPLOS'09), so we don't claim gaps that small. Switching the timing on and off in the same binary changes only 0.2–0.7%.
- The jit with an empty cache is as fast as the cached one here: the 241 ms compile is 0.5% of a 48 s run.

**33. Lazy decoding: memory.**
- "Non-reclaimable" means anonymous memory: the kernel can't drop it, only swap it. Clean file pages can simply be dropped and re-read.
- The original decrements counters inside the private mapping, so every touched page of the 4.6 GB file becomes a private copy: 3.7 GB. The new format copies less because the file is 3× smaller. Lazy never writes the file; what remains is the counter array (4 B per node = 0.4 GB).
- Right table: 8 simulations of the same trace at once. The file pages are shared, so lazy needs 0.31 GB in total against 3 GB for the original, and it has the highest throughput.
- If asked why lazy's total RSS (1.9 GB) is higher than the new format's (1.5 GB): RSS also counts the clean, shared file pages, and lazy keeps the counters separately instead of inside the file pages.
- `--save-mem` gets the original to zero anonymous memory, but it writes into the trace (the file can't be reused) and takes 950k major page faults.

**34. When does compiling the decoder pay off?**
- Right table: decoding alone. The jit is 1.7× faster than the interpreter, but still slower than reading legacy records (57 vs 41 ns). These numbers include both dependency lists and a trace far larger than the cache, so they are higher than the 12–18 ns on the JIT slide (22).
- The compile with an empty cache costs 241 ms. Divide by the per-node saving over the interpreter: the jit pays off after 6.4M nodes (7.8M in dependency order).
- The left plot measures it end to end: the jit with an empty cache overtakes the interpreter at 8.0M nodes. Model and measurement agree.
- Practical rule: small traces use `plan` (or a cached jit); large ones the jit.

**35. Workloads and scale.**
- Left: the gains depend on how much time goes into decoding. Allreduce and pipeline gain 10–22% with the jit; where message matching dominates (alltoall, MoE, data-parallel), everything is within noise.
- Right: with 8 ranks the new format is 11% slower with the interpreter; from 512 ranks on it is 7–17% faster. We haven't profiled the cause of the 8-rank case yet.

**36. Conversion is now the slowest step.**
- Complete `txt2bin` runs (both passes) on a 1.35 GB GOAL file.
- One thread: the new format takes 2.5× as long as the original (two passes plus packing).
- 16 threads: legacy output is 3.3× faster than the original, but the new format only gains 1.65× because pass 2 is still sequential (Amdahl). Hence the outlook item: parallelise pass 2.
- The parallel branch maps the whole input: 0.8–1.3 GB peak memory instead of 45 MB.

**37. What the study shows.** Read the four cards. Next: the same harness on the ETH student cluster (bare Linux, aarch64 GB10 and x86), where we can evict the page cache and enforce memory limits. Under WSL the memory-limit runs completed even with a limit below the original's footprint, so the limit evidently wasn't enforced; we don't report them.

## Outlook

**38. Outlook.**
- Next: parallelise pass 2, dictionary encoding, automatic decoder choice, the benchmark study on the ETH student cluster, and a trace larger than main memory (conversion maps the input, and lazy decoding keeps the trace pages droppable, but neither is tested beyond RAM).
- GPU: conversion is embarrassingly parallel (independent ranks and nodes, min/max reductions, per-node packing). A CUDA kernel could use thousands of threads and the GPU's memory bandwidth. Exploratory, no numbers yet.

**39. Who did what.** Read it briefly.

**40. Questions.** Prepare for:
- Why isn't lazy decoding the default? (It could be. It's opt-in so that default behaviour doesn't change.)
- Which decoder should be the default? (Short runs: interpreter or templated; long runs: `jit` or `orc`.)
- What if the counter array doesn't fit? (4 B/node. A trace that large is already tens of GB on disk. It fails at startup, not mid-run: `bad_alloc`, or the OOM killer on Linux.)
- Why not keep all ranks in memory instead of parsing twice? (Large schedules don't fit. A temporary binary file is a prototyped alternative.)
- Why WSL and not bare metal for the study? (It was the machine available before today. The harness records the environment and flags runs with background load; the cluster runs repeat everything on bare Linux.)
- Are the synthetic workloads representative? (They follow the communication patterns of real HPC and ML jobs, but they are generated. Recorded traces, such as the ATLAHS Llama run, are next.)
- Why does the new format lose at 8 ranks? (Not profiled yet. The legacy file through our reader is faster at every rank count, so the cost is in decoding the packed records, not in the restructured simulator.)
