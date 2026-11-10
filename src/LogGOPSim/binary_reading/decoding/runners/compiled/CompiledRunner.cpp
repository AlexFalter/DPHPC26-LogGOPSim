#include "binary_reading/decoding/runners/compiled/CompiledRunner.hpp"

namespace traceschema {

std::unique_ptr<CompiledRunner> CompiledRunner::create(const DecodePlan& plan, CompiledCode code, std::string_view name) {
    if (!code) return nullptr;
    return std::unique_ptr<CompiledRunner>(new CompiledRunner(plan, std::move(code), name));
}

} // namespace traceschema
