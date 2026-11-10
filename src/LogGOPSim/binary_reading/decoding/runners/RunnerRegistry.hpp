#ifndef LOGGOPSIM_RUNNERREGISTRY_HPP
#define LOGGOPSIM_RUNNERREGISTRY_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace traceschema {

// One plan runner. RunnerRegistry.cpp lists them all; the Decoder, the benchmarks and the tests
// take their runners from that list, so adding or removing a runner changes only its folder,
// runners/CMakeLists.txt and the list (see runners/README.md).
struct RunnerInfo {
    std::string_view name;        // the LOGGOPSIM_DECODER value, and the runner's get_name()
    std::string_view description; // one line, for messages and documentation
    // Whether create works for every plan when the runner is available, or only for the plans it
    // was instantiated for (templated runners).
    bool handles_any_plan;
    // Whether this build and system have what create needs, such as a compiler.
    bool (*is_available)();
    // The runner for the plan; nothing if it is unavailable or has no instantiation for the plan.
    std::unique_ptr<const PlanRunner> (*create)(const DecodePlan& plan);
};

// Every registered runner; list order does not determine the default.
[[nodiscard]] std::span<const RunnerInfo> get_runners();
// The interpreted runner used by default and when another runner is unavailable.
[[nodiscard]] const RunnerInfo& get_default_runner();
// The runner with this name; nothing for an unknown name.
[[nodiscard]] const RunnerInfo* find_runner(std::string_view name);

} // namespace traceschema
#endif
