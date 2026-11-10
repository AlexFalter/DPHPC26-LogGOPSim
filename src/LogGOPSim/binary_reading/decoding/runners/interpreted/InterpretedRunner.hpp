#ifndef LOGGOPSIM_INTERPRETEDRUNNER_HPP
#define LOGGOPSIM_INTERPRETEDRUNNER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <cstdint>
#include <utility>

namespace traceschema {

// Interprets the decode plan: one load, range check and store per step, without the exact
// decoder's per-field variants.
class InterpretedRunner final : public PlanRunner {
public:
    explicit InterpretedRunner(DecodePlan plan) : PlanRunner(std::move(plan)) {}

    [[nodiscard]] bool run(const std::byte* record, std::byte* const* targets) const override;
    [[nodiscard]] std::string_view get_name() const override { return "plan"; }

private:
    [[nodiscard]] bool is_send_to_any(std::uint64_t type_offset, std::uint64_t peer_offset) const;

};

} // namespace traceschema
#endif
