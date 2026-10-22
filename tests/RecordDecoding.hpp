#pragma once

#include "binary_reading/NodeDecoder.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace test_helpers {

// Exercise the production decoder with every field, in layout order.
inline std::vector<traceschema::FieldValue> decode_record(
    const traceschema::RecordLayout& layout, std::span<const std::byte> bytes) {
    traceschema::FieldRequirements requirements;
    for (const auto& field : layout.fields)
        requirements.push_back({field.field, field.type, 64});
    const traceschema::NodeDecoder decoder(layout, requirements);
    std::vector<traceschema::FieldValue> values(layout.fields.size());
    decoder.decode_fields(bytes, values);
    return values;
}

} // namespace test_helpers
