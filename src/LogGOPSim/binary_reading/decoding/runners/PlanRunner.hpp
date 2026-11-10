#ifndef LOGGOPSIM_PLANRUNNER_HPP
#define LOGGOPSIM_PLANRUNNER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <utility>

namespace traceschema {

// Executes a decode plan, including safe reads near the mapping boundary.
class PlanRunner {
public:
    explicit PlanRunner(DecodePlan plan) : plan_(std::move(plan)) {}
    virtual ~PlanRunner() = default;

    // Selects the fast implementation or bounded reads according to available mapped bytes.
    // Invalid records throw; either path may have written some output before rejection.
    // Defined here so that the fast path inlines into the decoder; the bounded path is rare.
    void decode(std::span<const std::byte> record, std::size_t readable_bytes,
                std::byte* const* targets) const {
        if (readable_bytes < plan_.get_load_extent()) [[unlikely]] {
            decode_bounded(record, targets);
            return;
        }
        if (!run(record.data(), targets)) [[unlikely]]
            throw std::invalid_argument("Invalid node record");
    }

    // Writes the plan's fields to their destinations in targets. Returns false, possibly after
    // writing some of them, for an invalid code or a send to ANY. Low-level callers must
    // guarantee that the plan's load extent is readable; other callers use decode().
    [[nodiscard]] virtual bool run(const std::byte* record, std::byte* const* targets) const = 0;
    // The LOGGOPSIM_DECODER value that selects this runner.
    [[nodiscard]] virtual std::string_view get_name() const = 0;

protected:
    DecodePlan plan_;

private:
    void decode_bounded(std::span<const std::byte> record, std::byte* const* targets) const;
};

// The first of the compiled-in runner types (a std::tuple of them) whose static matches accepts
// the plan, constructed from it; nothing if none does.
template<class Runners>
[[nodiscard]] std::unique_ptr<PlanRunner> create_first_matching(const DecodePlan& plan) {
    std::unique_ptr<PlanRunner> runner;
    const auto try_type = [&]<class Runner>() {
        if (!runner && Runner::matches(plan)) runner = std::make_unique<Runner>(plan);
    };
    [&]<class... Types>(std::tuple<Types...>*) {
        (try_type.template operator()<Types>(), ...);
    }(static_cast<Runners*>(nullptr));
    return runner;
}

} // namespace traceschema
#endif
