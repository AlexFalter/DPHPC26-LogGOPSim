#include "binary_reading/decoding/DecodePlan.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace traceschema {

// Reuse the encoding's range checks instead of repeating signed/unsigned overflow arithmetic.
static std::uint64_t get_validated_base_bits(const FieldStorageInfo& field) {
    const auto& encoding = field.offset_encoding;
    try {
        // If the largest offset decodes, every smaller numeric offset fits as well.
        // decode() also checks that ANY has a distinct code when wildcards are enabled.
        (void)encoding.decode(encoding.maximum_offset);
        return integer_to_bits(encoding.value_base);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument("Field '" + schema_helpers::get_field_label(field.field) + "': " + error.what());
    }
}

FieldSource FieldSource::create(const FieldStorageInfo& field) {
    const auto width = field.bit_width;
    if (width > 64)
        throw std::invalid_argument("Field '" + schema_helpers::get_field_label(field.field) + "' is wider than 64 bits");
    const auto base_bits = get_validated_base_bits(field);

    // Constant (0-bit) fields read a zero offset at byte 0 and decode to the base.
    const auto bit_offset = width == 0 ? 0 : field.bit_offset;
    const auto shift = static_cast<std::uint8_t>(bit_offset % bits_per_byte);
    return FieldSource{.first_byte = static_cast<std::uint32_t>(bit_offset / bits_per_byte),
                       .shift = shift,
                       .spill = shift + width > 64,
                       .wildcard = field.offset_encoding.has_wildcard,
                       .mask = low_bit_mask(width),
                       .base = base_bits,
                       .maximum_offset = field.offset_encoding.maximum_offset};
}

DecodePlan DecodePlan::create(std::span<const FieldStorageInfo> fields,
                              std::span<const FieldDestination> destinations) {
    if (fields.size() != destinations.size())
        throw std::invalid_argument("Expected one output destination per selected field");
    DecodePlan plan;
    plan.steps_.reserve(fields.size());
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        const auto& destination = destinations[index];
        if (destination.size != 1 && destination.size != 2 && destination.size != 4 && destination.size != 8)
            throw std::invalid_argument("Output destinations must be 1, 2, 4 or 8 bytes");

        const auto source = FieldSource::create(field);
        plan.steps_.push_back({source, destination});

        // Track the widest read and the two fields needed to reject sends to ANY.
        plan.load_extent_ = std::max(plan.load_extent_, source.load_end());
        if (field.field == FieldIdentity{FieldId::Type}) plan.type_step_ = index;
        if (field.field == FieldIdentity{FieldId::Peer}) plan.peer_step_ = index;
    }
    return plan;
}

} // namespace traceschema
