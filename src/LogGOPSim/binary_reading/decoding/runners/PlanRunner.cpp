#include "binary_reading/decoding/runners/PlanRunner.hpp"

#include <bit>
#include <stdexcept>

namespace traceschema {

void PlanRunner::decode_bounded(std::span<const std::byte> record, std::byte* const* targets) const {
    std::uint64_t type_offset = 0, peer_offset = 0;
    const auto steps = plan_.get_steps();
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        const auto& source = step.source;
        const auto offset = source.load_bounded(record);
        const auto value = step.get_value(offset);
        if (!value) throw std::invalid_argument("Invalid node record");
        if (index == plan_.get_type_step()) type_offset = offset;
        if (index == plan_.get_peer_step()) peer_offset = offset;
        step.destination.store(targets, *value);
    }
    if (plan_.get_type_step() && plan_.get_peer_step() &&
        DecodePlan::is_send_to_any(steps[*plan_.get_type_step()].source, type_offset,
                                 steps[*plan_.get_peer_step()].source, peer_offset))
        throw std::invalid_argument("Invalid node record");
}

} // namespace traceschema
