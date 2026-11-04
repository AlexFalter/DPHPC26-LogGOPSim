#include "binary_reading/DecodePlan.hpp"

#include <algorithm>
#include <limits>

namespace traceschema {

std::optional<DecodePlan::Step> DecodePlan::make_step(const FieldStorageInfo& field) {
    const auto& encoding = field.offset_encoding;
    const auto width = field.bit_width;
    if (width > 64) return std::nullopt;
    if (encoding.has_wildcard && encoding.maximum_offset == std::numeric_limits<std::uint64_t>::max())
        return std::nullopt;
    std::uint64_t base;
    std::uint64_t headroom;
    if (const auto* value = std::get_if<std::uint64_t>(&encoding.value_base)) {
        base = *value;
        headroom = std::numeric_limits<std::uint64_t>::max() - base;
    } else if (const auto* value = std::get_if<std::int64_t>(&encoding.value_base)) {
        base = static_cast<std::uint64_t>(*value);
        headroom = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) - base;
    } else return std::nullopt;
    if (encoding.maximum_offset > headroom) return std::nullopt;
    // Constant (0-bit) fields read a zero code at byte 0 and decode to the base.
    const auto bit_offset = width == 0 ? 0 : field.bit_offset;
    const auto shift = static_cast<std::uint8_t>(bit_offset % 8);
    return Step{static_cast<std::uint32_t>(bit_offset / 8), shift,
                static_cast<std::uint8_t>(std::min<std::uint32_t>((shift + width + 7) / 8, 8)),
                shift + width > 64, encoding.has_wildcard,
                width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1,
                base, encoding.maximum_offset};
}

std::optional<DecodePlan> DecodePlan::create(std::span<const FieldStorageInfo> fields,
                                             std::span<const FieldDestination> destinations) {
    if (fields.size() != destinations.size()) return std::nullopt;
    DecodePlan plan;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto size = destinations[index].size;
        if (size != 1 && size != 2 && size != 4 && size != 8) return std::nullopt;
        auto step = make_step(fields[index]);
        if (!step) return std::nullopt;
        step->target = destinations[index].target;
        step->size = size;
        step->offset = destinations[index].offset;
        step->wildcard_bits = destinations[index].wildcard_bits;
        plan.steps_.push_back(*step);
        plan.load_extent_ = std::max<std::size_t>(plan.load_extent_, step->byte + (step->spill ? 9 : 8));
        if (fields[index].field == FieldIdentity{FieldId::Type}) plan.type_step_ = static_cast<int>(index);
        if (fields[index].field == FieldIdentity{FieldId::Peer}) plan.peer_step_ = static_cast<int>(index);
    }
    return plan;
}

} // namespace traceschema
