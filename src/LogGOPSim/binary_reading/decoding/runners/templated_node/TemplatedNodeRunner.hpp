#ifndef LOGGOPSIM_TEMPLATEDNODERUNNER_HPP
#define LOGGOPSIM_TEMPLATEDNODERUNNER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace traceschema {

// What a node type fixes about one step at compile time: the output struct and member size.
struct StepShape {
    OutputStruct target;
    std::uint8_t size;
};

// Runs a plan whose steps have the given shapes, one per step: the steps are unrolled and each
// stores a fixed size into a fixed output struct. Where the field's bits are, its range and its
// member offset still come from the plan, so one instantiation serves every trace that a node
// type reads.
template<StepShape... Shapes>
class TemplatedNodeRunner final : public PlanRunner {
public:
    static constexpr std::size_t step_count = sizeof...(Shapes);

    [[nodiscard]] static bool matches(const DecodePlan& plan) {
        constexpr std::array<StepShape, step_count> shapes{Shapes...};
        const auto steps = plan.get_steps();
        return std::ranges::equal(steps, shapes, [](const DecodePlan::Step& step, const StepShape& shape) {
            return step.destination.target == shape.target && step.destination.size == shape.size;
        });
    }

    // The plan must match.
    explicit TemplatedNodeRunner(const DecodePlan& plan) : PlanRunner(plan), type_step_(plan.get_type_step()), peer_step_(plan.get_peer_step()) {
        std::ranges::copy(plan.get_steps(), steps_.begin());
    }

    [[nodiscard]] bool run(const std::byte* record, std::byte* const* targets) const override {
        return run_steps(record, targets, std::make_index_sequence<step_count>{});
    }
    [[nodiscard]] std::string_view get_name() const override { return "templated_node"; }

private:
    template<std::size_t... Indices>
    bool run_steps(const std::byte* record, std::byte* const* targets, std::index_sequence<Indices...>) const {
        std::array<std::uint64_t, step_count> offsets;
        if (!(run_step<Indices, Shapes>(record, targets, offsets[Indices]) && ...)) return false;
        return !(type_step_ && peer_step_ &&
                 DecodePlan::is_send_to_any(steps_[*type_step_].source, offsets[*type_step_],
                                            steps_[*peer_step_].source, offsets[*peer_step_]));
    }

    template<std::size_t Index, StepShape Shape>
    bool run_step(const std::byte* record, std::byte* const* targets, std::uint64_t& offset) const {
        const auto& step = steps_[Index];
        offset = step.source.load(record);
        const auto value = step.get_value(offset);
        if (!value) return false;
        store_bytes<Shape.size>(targets[std::to_underlying(Shape.target)] + step.destination.offset, *value);
        return true;
    }

    std::array<DecodePlan::Step, step_count> steps_;
    std::optional<std::size_t> type_step_;
    std::optional<std::size_t> peer_step_;
};

// A shape runner for the plan if a simulator's node type, compiled in, has its shape.
[[nodiscard]] std::unique_ptr<const PlanRunner> create_templated_node_runner(const DecodePlan& plan);

} // namespace traceschema
#endif
