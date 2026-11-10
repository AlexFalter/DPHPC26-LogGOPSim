#ifndef LOGGOPSIM_TEMPLATEDSCHEMARUNNER_HPP
#define LOGGOPSIM_TEMPLATEDSCHEMARUNNER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace traceschema {

inline constexpr std::size_t no_step = std::numeric_limits<std::size_t>::max();

// Runs one exact plan compiled into the program: every step is a template argument, so every
// position, mask, range and member offset is a constant, as in the JIT's generated code. It
// matches only traces with exactly this layout and these destinations; KnownSchemas.hpp lists
// the instantiations.
template<std::size_t TypeStep, std::size_t PeerStep, DecodePlan::Step... Steps>
class TemplatedSchemaRunner final : public PlanRunner {
public:
    [[nodiscard]] static bool matches(const DecodePlan& plan) {
        constexpr std::array<DecodePlan::Step, sizeof...(Steps)> steps{Steps...};
        return std::ranges::equal(plan.get_steps(), steps) && plan.get_type_step().value_or(no_step) == TypeStep &&
               plan.get_peer_step().value_or(no_step) == PeerStep;
    }

    // The plan must match; everything it holds is already in the type.
    explicit TemplatedSchemaRunner(const DecodePlan& plan) : PlanRunner(plan) {}

    [[nodiscard]] bool run(const std::byte* record, std::byte* const* targets) const override {
        return run_steps(record, targets, std::make_index_sequence<sizeof...(Steps)>{});
    }
    [[nodiscard]] std::string_view get_name() const override { return "templated_schema"; }

private:
    static constexpr std::array<DecodePlan::Step, sizeof...(Steps)> steps_{Steps...};

    template<std::size_t... Indices>
    static bool run_steps(const std::byte* record, std::byte* const* targets, std::index_sequence<Indices...>) {
        std::array<std::uint64_t, sizeof...(Steps)> offsets;
        if (!(run_step<Steps>(record, targets, offsets[Indices]) && ...)) return false;
        if constexpr (TypeStep != no_step && PeerStep != no_step)
            return !DecodePlan::is_send_to_any(steps_[TypeStep].source, offsets[TypeStep],
                                               steps_[PeerStep].source, offsets[PeerStep]);
        return true;
    }

    template<DecodePlan::Step Step>
    static bool run_step(const std::byte* record, std::byte* const* targets, std::uint64_t& offset) {
        offset = Step.source.load(record);
        const auto value = Step.get_value(offset);
        if (!value) return false;
        store_bytes<Step.destination.size>(targets[std::to_underlying(Step.destination.target)] + Step.destination.offset,
                                           *value);
        return true;
    }
};

// A runner for the plan if KnownSchemas.hpp lists its exact schema.
[[nodiscard]] std::unique_ptr<const PlanRunner> create_templated_schema_runner(const DecodePlan& plan);


} // namespace traceschema
#endif
