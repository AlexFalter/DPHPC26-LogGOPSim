#ifndef LOGGOPSIM_ORCDECODER_HPP
#define LOGGOPSIM_ORCDECODER_HPP

#include "binary_reading/JitDecoder.hpp"

namespace traceschema {

// Emits the plan as LLVM IR and compiles it in-process with ORC. The LLVM code lives in the
// loggopsim_orc plugin (LOGGOPSIM_ORC_LIBRARY overrides its path), loaded on first use.
// Returns nullptr on any failure, or when the build has no LLVM (LOGGOPSIM_HAVE_LLVM undefined).
[[nodiscard]] JitDecodeFunction load_orc_decoder(const DecodePlan& plan);

} // namespace traceschema
#endif
