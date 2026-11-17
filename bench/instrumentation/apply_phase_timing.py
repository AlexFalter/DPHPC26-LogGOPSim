#!/usr/bin/env python3
"""Adds phase-timing hooks to a NEW worktree (never to the baseline).

usage: apply_phase_timing.py WORKTREE [--check]

Copies PhaseTiming.hpp into WORKTREE/src/LogGOPSim/trace_format/ and inserts hooks at fixed
anchors. Every anchor must match exactly once, otherwise nothing is written and the script fails,
so a branch whose code moved is noticed instead of silently losing a phase. The hooks only time;
no decoding, validation or simulation statement is changed. --check verifies the anchors without
writing. A stamp file WORKTREE/.bench-phase-timing.json records what was applied.

Phases (all CLOCK_MONOTONIC, accumulated per process, ns):
  total          main() from start to return                         LogGOPSim.cpp
  load           SimulationSchedule construction (all of the below)  LogGOPSim.cpp
  header_parse   read_binary_header (self-describing)                TraceFileView.cpp
  rank_table     rank table + extent validation (self-describing)    TraceFileView.cpp
  legacy_open    legacy header, rank, root and appendix validation   LegacyTraceReader.hpp
  validate       required ⊆ provided, widths, op codes, peer range,  Decoder.cpp,
                 wildcard bindings                                   SelfDescribingTraceReader.hpp
  plan_build     DecodePlan::create                                  Decoder.cpp
  decoder_select runner creation incl. everything JIT-related below  Decoder.cpp
  decoder_lookup JIT cache lookup (hash + stat)                      CCompiler.cpp
  jit_generate   C source generation                                 CCompiler.cpp
  jit_compile    cc -O2 -shared (jit) / ORC compile incl. IR (orc)   CCompiler.cpp, OrcLoader.cpp
  jit_load       dlopen + dlsym of the library / the ORC plugin      CCompiler.cpp, OrcLoader.cpp
  validate_deps  O(edges) scan of the dependency appendix            TraceFileView.cpp
  load_roots     per-rank schedule setup: decoding the root nodes    SimulationSchedule.hpp
  simulate       main event loop incl. on-demand decoding            LogGOPSim.cpp
Notes: runner (actual plan runner), jit_cache (hit|miss).
"""
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HEADER = HERE / "PhaseTiming.hpp"
INCLUDE = '#include "trace_format/PhaseTiming.hpp"'

# (file, old, new) exact replacements; "re:" prefixed old strings are regular expressions.
EDITS = [
    ("src/LogGOPSim/binary_reading/TraceFileView.cpp",
     ": header_(read_binary_header(file)), record_size_bytes_",
     ': header_(phase_timing::timed("header_parse", [&] { return read_binary_header(file); })), record_size_bytes_'),
    ("src/LogGOPSim/binary_reading/TraceFileView.cpp",
     "      file_end_(file.data() + file.size()) {\n    read_rank_table(file);\n}",
     '      file_end_(file.data() + file.size()) {\n    phase_timing::timed("rank_table", [&] { read_rank_table(file); });\n}'),
    ("src/LogGOPSim/binary_reading/TraceFileView.cpp",
     "    if (dependency_indices_validated_) return;\n",
     '    if (dependency_indices_validated_) return;\n    const phase_timing::Scope phase_scope("validate_deps");\n'),
    ("src/LogGOPSim/binary_reading/decoding/Decoder.cpp",
     "    : fields_(resolve_required_fields(layout, requirements)),",
     '    : fields_(phase_timing::timed("validate", [&] { return resolve_required_fields(layout, requirements); })),'),
    ("src/LogGOPSim/binary_reading/decoding/Decoder.cpp",
     "    auto plan = DecodePlan::create(fields_, destinations);\n    runner_ = create_runner(plan);\n",
     '    auto plan = phase_timing::timed("plan_build", [&] { return DecodePlan::create(fields_, destinations); });\n'
     '    runner_ = phase_timing::timed("decoder_select", [&] { return create_runner(plan); });\n'
     '    phase_timing::note("runner", std::string(runner_->get_name()));\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/c/CCompiler.cpp",
     "    const auto source = generate_decoder_source(plan);\n",
     '    const auto source = phase_timing::timed("jit_generate", [&] { return generate_decoder_source(plan); });\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/c/CCompiler.cpp",
     "    const auto load = [&]() -> CompiledCode {\n",
     '    const auto load = [&]() -> CompiledCode {\n        const phase_timing::Scope phase_scope("jit_load");\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/c/CCompiler.cpp",
     "    if (std::filesystem::exists(library, error))\n        if (const auto cached = load()) return cached;\n",
     '    if (phase_timing::timed("decoder_lookup", [&] { return std::filesystem::exists(library, error); }))\n'
     '        if (const auto cached = load()) { phase_timing::note("jit_cache", "hit"); return cached; }\n'
     '    phase_timing::note("jit_cache", "miss");\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/c/CCompiler.cpp",
     "    const bool compiled = run_compiler(compiler, source_path.string(), temporary.string());\n",
     '    const bool compiled = phase_timing::timed("jit_compile", [&] {\n'
     '        return run_compiler(compiler, source_path.string(), temporary.string()); });\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/orc/OrcLoader.cpp",
     "            void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);\n",
     '            void* handle = phase_timing::timed("jit_load", [&] { return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL); });\n'),
    ("src/LogGOPSim/binary_reading/decoding/runners/compiled/orc/OrcLoader.cpp",
     "    if (!compile(&plan, &code)) return {};\n",
     '    if (!phase_timing::timed("jit_compile", [&] { return compile(&plan, &code); })) return {};\n'),
    ("src/LogGOPSim/binary_reading/SelfDescribingTraceReader.hpp",
     "        check_peer_range();\n        validate_bindings<Output>(bindings);\n",
     '        {\n            const phase_timing::Scope phase_scope("validate");\n'
     "            check_peer_range();\n            validate_bindings<Output>(bindings);\n        }\n"),
    ("src/LogGOPSim/binary_reading/LegacyTraceReader.hpp",
     "        read_header();\n",
     '        phase_timing::timed("legacy_open", [&] { read_header(); });\n'),
    ("src/LogGOPSim/simulators/loggopsim/SimulationSchedule.hpp",
     "        schedules.reserve(trace_adapter_->get_rank_count());\n",
     '        const phase_timing::Scope phase_scope("load_roots");\n'
     "        schedules.reserve(trace_adapter_->get_rank_count());\n"),
    ("src/LogGOPSim/simulators/loggopsim/LogGOPSim.cpp",
     '#include "simulators/loggopsim/SimulationSchedule.hpp"\n',
     '#include "simulators/loggopsim/SimulationSchedule.hpp"\n' + INCLUDE + "\n"),
    ("src/LogGOPSim/simulators/loggopsim/LogGOPSim.cpp",
     "int main(int argc, char **argv) try {\n",
     'int main(int argc, char **argv) try {\n  const phase_timing::Scope phase_total("total");\n'),
    ("src/LogGOPSim/simulators/loggopsim/LogGOPSim.cpp",
     r"re:(\n  SimulationSchedule schedule\([^;]*\);)",
     '\n  const auto phase_load_start = phase_timing::start();\\1\n  phase_timing::add("load", phase_load_start);'),
    ("src/LogGOPSim/simulators/loggopsim/LogGOPSim.cpp",
     "gettimeofday(&tstart, NULL);\n",
     "gettimeofday(&tstart, NULL);\n  const auto phase_simulate_start = phase_timing::start();\n"),
    ("src/LogGOPSim/simulators/loggopsim/LogGOPSim.cpp",
     "gettimeofday(&tend, NULL);\n",
     'phase_timing::add("simulate", phase_simulate_start);\n  gettimeofday(&tend, NULL);\n'),
]


def add_include(text):
    """Adds the include after the file's first #include line (LogGOPSim.cpp gets it via EDITS)."""
    if INCLUDE in text:
        return text
    match = re.search(r"^#include [^\n]*\n", text, flags=re.M)
    if not match:
        raise ValueError("no #include line")
    return text[:match.end()] + INCLUDE + "\n" + text[match.end():]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    check = "--check" in sys.argv
    if len(args) != 1:
        sys.exit(__doc__)
    tree = Path(args[0]).resolve()
    target_header = tree / "src/LogGOPSim/trace_format/PhaseTiming.hpp"
    if target_header.exists():
        sys.exit(f"{tree} is already instrumented (reset the worktree first)")
    texts, errors, crlf = {}, [], set()  # line endings are preserved per file
    for file, old, new in EDITS:
        path = tree / file
        if file not in texts:
            if not path.exists():
                errors.append(f"{file}: missing")
                continue
            raw = path.read_bytes().decode("utf-8")
            if "\r\n" in raw:
                crlf.add(file)
            texts[file] = raw.replace("\r\n", "\n")
        text = texts[file]
        if old.startswith("re:"):
            matches = len(re.findall(old[3:], text))
            if matches == 1:
                texts[file] = re.sub(old[3:], new, text, count=1)
        else:
            matches = text.count(old)
            if matches == 1:
                texts[file] = text.replace(old, new, 1)
        if matches != 1:
            errors.append(f"{file}: anchor {old[:60]!r} matched {matches} times")
    if errors:
        sys.exit("phase timing NOT applied:\n  " + "\n  ".join(errors))
    for file in texts:
        if not file.endswith("LogGOPSim.cpp"):
            texts[file] = add_include(texts[file])
    if check:
        print(f"phase timing anchors OK in {tree} ({len(EDITS)} edits, {len(texts)} files)")
        return
    for file, text in texts.items():
        if file in crlf:
            text = text.replace("\n", "\r\n")
        (tree / file).write_bytes(text.encode("utf-8"))
    shutil.copyfile(HEADER, target_header)
    stamp = {"applied": True, "header_sha256": digest(HEADER), "script_sha256": digest(__file__),
             "files": sorted(texts), "edits": len(EDITS)}
    (tree / ".bench-phase-timing.json").write_text(json.dumps(stamp, indent=2) + "\n")
    print(f"phase timing applied to {tree} ({len(EDITS)} edits in {len(texts)} files)")


if __name__ == "__main__":
    main()
