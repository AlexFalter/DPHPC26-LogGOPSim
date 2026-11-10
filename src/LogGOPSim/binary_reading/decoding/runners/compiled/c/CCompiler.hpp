#ifndef LOGGOPSIM_CCOMPILER_HPP
#define LOGGOPSIM_CCOMPILER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"

namespace traceschema {

// Writes the plan as C source, compiles it with the system compiler and loads the library.
// LOGGOPSIM_JIT_CC selects the compiler (default cc), LOGGOPSIM_JIT_CACHE the cache directory
// (default $XDG_CACHE_HOME or ~/.cache, subdirectory loggopsim-jit). Unloadable cache entries are
// rebuilt. Returns an empty result if compilation fails.
[[nodiscard]] CompiledCode compile_with_c(const DecodePlan& plan);

// Whether the compiler (LOGGOPSIM_JIT_CC or cc) is on the PATH.
[[nodiscard]] bool has_c_compiler();

} // namespace traceschema
#endif
