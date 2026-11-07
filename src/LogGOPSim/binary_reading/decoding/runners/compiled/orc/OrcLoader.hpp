#ifndef LOGGOPSIM_ORCLOADER_HPP
#define LOGGOPSIM_ORCLOADER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"

namespace traceschema {

// Builds the plan as LLVM IR and compiles it in-process with LLVM ORC, without a compiler process,
// file or disk cache. The LLVM code lives in the loggopsim_orc plugin (OrcCompiler.cpp), opened on first
// use so that other runs never load LLVM; LOGGOPSIM_ORC_LIBRARY overrides its path. Each plan is
// compiled once per process (LOGGOPSIM_ORC_REUSE=0 compiles every time). Returns an empty result if
// compilation fails or the build has no LLVM.
[[nodiscard]] CompiledCode compile_with_orc(const DecodePlan& plan);

// Whether this build has the plugin.
[[nodiscard]] bool has_orc_compiler();

} // namespace traceschema
#endif
