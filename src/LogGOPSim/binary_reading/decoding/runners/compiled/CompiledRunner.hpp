#ifndef LOGGOPSIM_COMPILEDRUNNER_HPP
#define LOGGOPSIM_COMPILEDRUNNER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

namespace traceschema {

// The entry point and the object keeping its executable memory alive travel together.
struct CompiledCode {
    using Function = int (*)(const std::byte* record, std::byte* const* targets);
    Function function = nullptr;
    std::shared_ptr<void> owner;
    [[nodiscard]] explicit operator bool() const { return function != nullptr && owner != nullptr; }
};

// Runs a decode plan compiled to machine code: one function with every offset, shift and mask as
// a constant, without a loop over the steps. The compilers in the subfolders produce it (c/ with
// the system C compiler, orc/ with LLVM ORC); each is a runner of its own in RunnerRegistry.cpp.
class CompiledRunner final : public PlanRunner {
public:
    // Same contract as run: zero rejects an invalid record. The compiled code stays
    // loaded while its owner is alive.
    using Function = CompiledCode::Function;

    // A runner named name for the compiled function; nothing if compilation failed (no function).
    [[nodiscard]] static std::unique_ptr<CompiledRunner> create(const DecodePlan& plan, CompiledCode code, std::string_view name);

    [[nodiscard]] bool run(const std::byte* record, std::byte* const* targets) const override {
        return code_.function(record, targets) != 0;
    }
    [[nodiscard]] std::string_view get_name() const override { return name_; }

private:
    CompiledRunner(const DecodePlan& plan, CompiledCode code, std::string_view name)
        : PlanRunner(plan), code_(std::move(code)), name_(name) {}

    CompiledCode code_;
    std::string_view name_; // a string literal from the registry
};

} // namespace traceschema
#endif
