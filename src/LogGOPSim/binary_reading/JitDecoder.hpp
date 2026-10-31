#ifndef LOGGOPSIM_JITDECODER_HPP
#define LOGGOPSIM_JITDECODER_HPP

#include "binary_reading/DecodePlan.hpp"

#include <cstddef>
#include <string>

namespace traceschema {

// Same contract as DecodePlan::decode: zero leaves the record to the generic decoder.
using JitDecodeFunction = int (*)(const std::byte* record, std::byte* const* targets);

// C source specialized to one plan; it contains only numeric constants from the schema.
[[nodiscard]] std::string generate_decoder_source(const DecodePlan& plan);

// Compiles (or loads from the cache) the specialized decoder. Returns nullptr on any failure.
// LOGGOPSIM_JIT_CC selects the compiler (default cc), LOGGOPSIM_JIT_CACHE the cache directory
// (default $XDG_CACHE_HOME or ~/.cache, subdirectory loggopsim-jit).
[[nodiscard]] JitDecodeFunction load_jit_decoder(const DecodePlan& plan);

} // namespace traceschema
#endif
