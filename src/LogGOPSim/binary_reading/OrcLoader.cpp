#include "binary_reading/OrcDecoder.hpp"

#include <cstdlib>
#include <dlfcn.h>

namespace traceschema {

JitDecodeFunction load_orc_decoder(const DecodePlan& plan) {
#ifdef LOGGOPSIM_ORC_LIBRARY
    const char* path = std::getenv("LOGGOPSIM_ORC_LIBRARY");
    void* library = dlopen(path ? path : LOGGOPSIM_ORC_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    if (!library) return nullptr;
    using Compile = JitDecodeFunction (*)(const DecodePlan&);
    const auto compile = reinterpret_cast<Compile>(dlsym(library, "loggopsim_compile_orc_decoder"));
    return compile ? compile(plan) : nullptr;
#else
    (void)plan;
    return nullptr;
#endif
}

} // namespace traceschema
