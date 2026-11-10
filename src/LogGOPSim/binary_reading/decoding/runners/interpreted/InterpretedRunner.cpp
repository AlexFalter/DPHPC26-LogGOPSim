#include "binary_reading/decoding/runners/interpreted/InterpretedRunner.hpp"

namespace traceschema {

bool InterpretedRunner::run(const std::byte* record, std::byte* const* targets) const {
    const auto steps = plan_.get_steps();
    std::uint64_t type_offset = 0;
    std::uint64_t peer_offset = 0;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        const auto& step = steps[index];
        const auto offset = step.source.load(record);
        if (index == plan_.get_type_step()) type_offset = offset;
        if (index == plan_.get_peer_step()) peer_offset = offset;
        const auto value = step.get_value(offset);
        if (!value) return false;
        step.destination.store(targets, *value);
    }
    return !is_send_to_any(type_offset, peer_offset);
}

bool InterpretedRunner::is_send_to_any(std::uint64_t type_offset, std::uint64_t peer_offset) const {
    const auto type_step = plan_.get_type_step();
    const auto peer_step = plan_.get_peer_step();
    return type_step && peer_step &&
           DecodePlan::is_send_to_any(plan_.get_steps()[*type_step].source, type_offset,
                                      plan_.get_steps()[*peer_step].source, peer_offset);
}

} // namespace traceschema
