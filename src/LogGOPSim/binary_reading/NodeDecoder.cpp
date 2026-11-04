#include "binary_reading/NodeDecoder.hpp"
#include "trace_format/BinaryIO.hpp"

#include <algorithm>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace traceschema {

static std::vector<FieldStorageInfo> resolve_required_fields(
    const RecordLayout& layout, const FieldRequirements& requirements) {
    layout.check_requirements(requirements);
    std::unordered_set<FieldIdentity> selected;
    std::vector<FieldStorageInfo> fields;
    fields.reserve(requirements.size());
    for (const auto& requirement : requirements) {
        const auto& identity = requirement.field;
        if (!selected.insert(identity).second)
            throw std::invalid_argument("Duplicate required field '" + schema_helpers::get_field_label(identity) + "'");
        const auto field = std::ranges::find(layout.fields, identity, &FieldStorageInfo::field);
        fields.push_back(*field); // check_requirements established presence and destination capacity.
    }
    return fields;
}

NodeDecoder::NodeDecoder(const RecordLayout& layout, const FieldRequirements& requirements)
    : fields_(resolve_required_fields(layout, requirements)), record_size_bytes_(layout.get_record_size_bytes()) {
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        const auto& field = fields_[index];
        if (field.field == FieldIdentity{FieldId::Type}) {
            const auto& encoding = field.offset_encoding;
            if (field.type != FieldType::UnsignedInteger || encoding.has_wildcard ||
                std::get<std::uint64_t>(encoding.value_base) < std::to_underlying(OperationType::Send) ||
                std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset)) >
                    std::to_underlying(OperationType::Calculation))
                throw std::invalid_argument("Unknown operation code in trace schema");
            operation_index_ = index;
        }
        if (field.field == FieldIdentity{FieldId::Peer}) peer_index_ = index;
    }
}

FieldValue NodeDecoder::decode_field(std::span<const std::byte> record, const FieldStorageInfo& field) {
    const auto code = bit_packing::read_bits(record, field.bit_offset, field.bit_width);
    return field.offset_encoding.decode(code);
}

void NodeDecoder::check_record_size(std::size_t size) const {
    if (size != record_size_bytes_)
        throw std::invalid_argument("Byte buffer size does not match selected node record size");
}

void NodeDecoder::decode_fields(std::span<const std::byte> record, std::span<FieldValue> output) const {
    if (output.size() != fields_.size())
        throw std::invalid_argument("Output size does not match selected node fields");
    decode_fields(record, [&](std::size_t index, const FieldStorageInfo&, FieldValue value) {
        output[index] = std::move(value);
    });
}

} // namespace traceschema
