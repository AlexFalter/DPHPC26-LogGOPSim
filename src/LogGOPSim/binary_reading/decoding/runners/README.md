# Plan runners

A plan runner executes a `DecodePlan` on one record. The shared `PlanRunner::decode` checks
whether enough mapped bytes are available for wide loads, then calls the runner's fast `run`
implementation or the shared bounded decoder. Invalid records throw without retrying.
`Decoder` resolves fields, selects a runner and delegates reads to it. `LOGGOPSIM_DECODER`
selects the runner by name.

| Folder | Runner | How it runs the plan |
|---|---|---|
| `interpreted/` | `plan` | Loops over the steps; the default and the fallback |
| `compiled/c/` | `jit` | Compiles the plan with the system C compiler; caches the library |
| `compiled/orc/` | `orc` | Compiles the plan in-process with LLVM ORC (optional, needs LLVM) |
| `templated_node/` | `templated_node` | A template per simulator node type, each member's size fixed |
| `templated_schema/` | `templated_schema` | A template per known trace schema, every step constant |

`compiled/CompiledRunner` runs any compiled function; each compiler subfolder only produces one.

`RunnerRegistry.cpp` lists the runners and explicitly selects `plan` through `get_default_runner()`,
independently of list order. The `Decoder`, `bench/decoding/decode_bench` and the unit
tests take them from there: the differential test in `tests/unit/decode_plan.cpp` checks every
runner whose entry says it handles any plan, and `tests/unit/trace_reader.cpp` runs every runner
on a trace that ends right before a protected page.

## Adding a runner

1. Add a folder with the runner, a class derived from `PlanRunner` whose `get_name()` returns
   its registry name, initializes `PlanRunner(plan)`, and a `CMakeLists.txt` that adds its sources:
   `target_sources(trace_schema PRIVATE MyRunner.cpp)`. A new compiler goes into a subfolder of
   `compiled/` and returns `CompiledCode` (entry point plus an owner for the executable memory).
2. Add `add_subdirectory(my_runner)` to this folder's `CMakeLists.txt`.
3. Add its entry to `RunnerRegistry.cpp`: name, description, whether it handles any plan, whether
   it is available (for example, a compiler is installed) and the function that creates it.
   `create` returns nothing when the runner cannot handle the plan; the decoder then falls back to
   `plan`.

Tests that are specific to the new runner go into `tests/unit/` as usual.

## Removing a runner

Delete its folder, its `add_subdirectory` line and its registry entry, and any tests that
name it.
