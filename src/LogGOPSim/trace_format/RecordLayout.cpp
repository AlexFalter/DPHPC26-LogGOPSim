#include "trace_format/RecordLayout.hpp"
#include "trace_format/BinaryIO.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace traceschema {
using schema_helpers::get_field_label;
using schema_helpers::get_standard_field_name;
using schema_helpers::get_value_type;
using schema_helpers::is_supported_field_type;
using schema_helpers::validate_field_identity;

FieldValueInfo FieldValueInfo::create_standard(
    FieldId id, std::optional<std::uint32_t> requested_width,
    std::optional<FieldValueRange> value_range) {
    return {.field = id, .type = FieldType::UnsignedInteger,
            .value_range = std::move(value_range), .width = requested_width};
}

FieldValueInfo FieldValueInfo::create_custom(
    std::string_view name, FieldType type, std::optional<std::uint32_t> requested_width,
    std::optional<FieldValueRange> value_range) {
    return {.field = std::string(name), .type = type,
            .value_range = std::move(value_range), .width = requested_width};
}

static void validate_field(const FieldStorageInfo& field) {
    validate_field_identity(field.field);
    if (field.bit_width == 0 || field.bit_width > 64)
        throw std::invalid_argument("Field '" + get_field_label(field.field) + "' must have a width of 1..64 bits");
    const auto& encoding = field.offset_encoding;
    if (get_value_type(encoding.value_base) != field.type)
        throw std::invalid_argument("Offset base type does not match field type");
    const auto maximum_code = encoding.get_wildcard_code().value_or(encoding.maximum_offset);
    if (field.bit_width < 64 && maximum_code >= (std::uint64_t{1} << field.bit_width))
        throw std::invalid_argument("Offset range exceeds field width");
    (void)encoding.decode(encoding.maximum_offset);
}

const FieldStorageInfo* RecordLayout::find_field(FieldId id) const {
    get_standard_field_name(id); // Reject unknown IDs.
    for (const auto& field : fields)
        if (const auto* standard = std::get_if<FieldId>(&field.field); standard && *standard == id)
            return &field;
    return nullptr;
}

const FieldStorageInfo* RecordLayout::find_field(std::string_view custom_name) const {
    for (const auto& field : fields)
        if (const auto* name = std::get_if<std::string>(&field.field); name && *name == custom_name)
            return &field;
    return nullptr;
}

void RecordLayout::validate_layout() const {
    if (fields.empty())
        throw std::invalid_argument("Schema must contain at least one field");
    if (record_size_bits == 0 || record_size_bits % bits_per_byte != 0)
        throw std::invalid_argument("Record size must be a positive multiple of 8 bits");

    std::unordered_set<FieldIdentity> identities;
    std::vector<const FieldStorageInfo*> by_offset;
    for (const auto& field : fields) {
        validate_field(field);
        const auto [entry, inserted] = identities.insert(field.field);
        if (!inserted)
            throw std::invalid_argument("Duplicate field '" + get_field_label(*entry) + "'");
        // Subtract before adding, so malicious offsets cannot wrap around.
        if (field.bit_offset > record_size_bits ||
            field.bit_width > record_size_bits - field.bit_offset)
            throw std::invalid_argument("Field '" + get_field_label(field.field) + "' exceeds the record size");
        by_offset.push_back(&field);
    }
    std::ranges::sort(by_offset, {}, &FieldStorageInfo::bit_offset);
    std::uint64_t end = 0;
    for (const auto* field : by_offset) {
        if (field->bit_offset < end)
            throw std::invalid_argument("Overlapping field '" + get_field_label(field->field) + "'");
        end = field->bit_offset + field->bit_width;
    }

}

static bool decoded_values_fit_destination(const FieldStorageInfo& field, std::uint32_t capacity_bits) {
    const auto& encoding = field.offset_encoding;
    return integer_range_fits_storage(field.type, encoding.value_base,
                                      encoding.decode(encoding.maximum_offset), capacity_bits);
}

void RecordLayout::check_requirements(const FieldRequirements& requirements) const {
    validate_layout();
    for (const auto& requirement : requirements) {
        validate_field_identity(requirement.field);
        const auto it = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
            return field.field == requirement.field;
        });
        if (it == fields.end())
            throw std::invalid_argument("Trace lacks required field '" + get_field_label(requirement.field) + "'");
        const auto& field = *it;
        if (field.type != requirement.type)
            throw std::invalid_argument("Incompatible type for required field '" + get_field_label(field.field) + "'");
        if (!decoded_values_fit_destination(field, requirement.capacity_bits))
            throw std::invalid_argument("Incompatible type or width for required field '" + get_field_label(requirement.field) + "'");
    }
}

std::uint64_t RecordLayout::get_record_size_bytes() const {
    return record_size_bits / bits_per_byte;
}

RecordLayout create_packed_schema(std::vector<FieldStorageInfo> fields) {
    std::uint64_t end = 0;
    for (auto& field : fields) {
        if (end > std::numeric_limits<std::uint64_t>::max() - field.bit_width)
            throw std::invalid_argument("Packed schema size overflows");
        field.bit_offset = end;
        end += field.bit_width;
    }
    // Whole-byte records allow direct addressing: base + node_index * record_bytes.
    RecordLayout schema{
        .fields = std::move(fields),
        .record_size_bits = round_up_to_byte(end)
    };
    schema.validate_layout();

    return schema;
}

static void validate_standard_field_range(const FieldValueInfo& input, std::uint64_t num_ranks) {
    if (!std::holds_alternative<FieldId>(input.field) || !input.value_range) return;
    // General type/range validation runs first; standard fields are unsigned.
    const auto& range = std::get<UnsignedRange>(*input.value_range);
    if (input.field == FieldIdentity{FieldId::Type} && (range.minimum < std::to_underlying(OperationType::Send) ||
        range.maximum > std::to_underlying(OperationType::Calculation)))
        throw std::invalid_argument("Unknown operation code in value range for '" + get_field_label(input.field) + "'");
    if (input.field == FieldIdentity{FieldId::Peer} && num_ranks != 0 && range.maximum >= num_ranks)
        throw std::invalid_argument("Peer value range exceeds num_ranks");
}

static void validate_layout_input(const FieldValueInfo& input, std::uint64_t num_ranks) {
    validate_field_identity(input.field);
    if (!is_supported_field_type(input.type) ||
        (std::holds_alternative<FieldId>(input.field) && input.type != FieldType::UnsignedInteger))
        throw std::invalid_argument("Invalid type for '" + get_field_label(input.field) + "'");
    if (input.width && (*input.width < 1 || *input.width > 64))
        throw std::invalid_argument("Explicit width must be 1..64 bits for '" + get_field_label(input.field) + "'");
    if (input.value_range)
        schema_helpers::validate_value_range(input.type, *input.value_range, get_field_label(input.field));
    validate_standard_field_range(input, num_ranks);
}

static FieldValueRange resolve_value_range(
    const FieldValueInfo& input, std::uint64_t num_ranks) {
    if (input.field == FieldIdentity{FieldId::Type}) return UnsignedRange{std::to_underlying(OperationType::Send), std::to_underlying(OperationType::Calculation)};
    if (input.field == FieldIdentity{FieldId::Peer} && num_ranks != 0)
        return UnsignedRange{0, num_ranks - 1};
    if (!input.value_range)
        throw std::invalid_argument("Missing value range for '" + get_field_label(input.field) +
            "': provide producer bounds or collect statistics first");
    return *input.value_range;
}

static std::uint32_t resolve_field_width(
    const FieldValueInfo& input, const OffsetEncoding& encoding) {
    const auto maximum_code = encoding.get_wildcard_code().value_or(encoding.maximum_offset);
    const auto required_bits = calculate_required_bits(maximum_code);
    if (input.width && *input.width < required_bits)
        throw std::invalid_argument("Explicit width is too small for '" + get_field_label(input.field) + "'");
    return input.width.value_or(required_bits);
}

RecordLayout build_record_layout(const std::vector<FieldValueInfo>& fields,
                            std::uint64_t num_ranks) {
    std::vector<FieldStorageInfo> resolved;
    resolved.reserve(fields.size());
    for (const auto& input : fields) {
        validate_layout_input(input, num_ranks);
        const auto value_range = resolve_value_range(input, num_ranks);
        const auto encoding = create_integer_encoding(input.type, value_range, input.has_wildcard);
        const auto width = resolve_field_width(input, encoding);
        resolved.push_back({input.field, input.type, width, 0, encoding});
    }
    return create_packed_schema(std::move(resolved));
}

} // namespace traceschema
