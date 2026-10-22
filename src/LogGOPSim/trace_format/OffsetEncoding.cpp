#include "trace_format/OffsetEncoding.hpp"

#include <bit>
#include <limits>
#include <type_traits>

namespace traceschema {
using schema_helpers::get_value_type;
using schema_helpers::visit_field_type;

std::optional<std::uint64_t> OffsetEncoding::get_wildcard_code() const {
    if (!has_wildcard) return std::nullopt;
    if (maximum_offset == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("No room for wildcard");
    return maximum_offset + 1;
}

template<std::integral Integer>
static std::uint64_t calculate_integer_offset(const FieldValue& value, Integer base) {
    const auto* number = std::get_if<Integer>(&value);
    if (!number) throw std::invalid_argument("Offset value type does not match its base");
    if (*number < base) throw std::invalid_argument("Value is below its base");
    return static_cast<std::uint64_t>(*number) - static_cast<std::uint64_t>(base);
}

std::uint64_t OffsetEncoding::encode(const FieldValue& value) const {
    if (std::holds_alternative<Wildcard>(value)) {
        const auto wildcard = get_wildcard_code();
        if (!wildcard) throw std::invalid_argument("Field does not support a wildcard");
        return *wildcard;
    }
    const auto offset = std::visit([&](const auto& base) -> std::uint64_t {
        if constexpr (std::integral<std::decay_t<decltype(base)>>)
            return calculate_integer_offset(value, base);
        else throw std::invalid_argument("Offset encoding requires an integer field type");
    }, value_base);
    if (offset > maximum_offset)
        throw std::invalid_argument("Value exceeds its offset range");
    return offset;
}

static std::uint64_t decode_unsigned_offset(std::uint64_t base, std::uint64_t offset) {
    if (offset > std::numeric_limits<std::uint64_t>::max() - base)
        throw std::invalid_argument("Offset decoding overflows unsigned value");
    return base + offset;
}

static std::int64_t decode_signed_offset(std::int64_t base, std::uint64_t offset) {
    const auto maximum_signed = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    // Unsigned arithmetic handles negative bases and ranges spanning the full signed domain.
    const auto distance_to_maximum = maximum_signed - static_cast<std::uint64_t>(base);
    if (offset > distance_to_maximum)
        throw std::invalid_argument("Offset decoding overflows signed value");
    return std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(base) + offset);
}

FieldValue OffsetEncoding::decode(std::uint64_t offset) const {
    if (get_wildcard_code() == offset) return Wildcard{};
    if (offset > maximum_offset)
        throw std::invalid_argument("Encoded offset exceeds its numeric range");
    switch (get_value_type(value_base)) {
    case FieldType::UnsignedInteger:
        return decode_unsigned_offset(std::get<std::uint64_t>(value_base), offset);
    case FieldType::SignedInteger:
        return decode_signed_offset(std::get<std::int64_t>(value_base), offset);
    default:
        throw std::invalid_argument("Offset encoding requires an integer field type");
    }
}

OffsetEncoding create_integer_encoding(FieldType type, const FieldValueRange& range, bool has_wildcard) {
    return visit_field_type(type, [&]<class Value>() -> OffsetEncoding {
        if constexpr (std::integral<Value>) {
            const auto& bounds = std::get<typename FieldTypeTraits<Value>::Range>(range);
            // Unsigned subtraction also handles INT64_MIN..INT64_MAX.
            const auto span = static_cast<std::uint64_t>(bounds.maximum) -
                              static_cast<std::uint64_t>(bounds.minimum);
            return {FieldValue{bounds.minimum}, span, has_wildcard};
        } else throw std::invalid_argument("No integer encoding available for this field type");
    });
}

std::uint64_t integer_to_bits(const FieldValue& value) {
    return std::visit([](const auto& number) -> std::uint64_t {
        if constexpr (std::integral<std::decay_t<decltype(number)>>)
            return static_cast<std::uint64_t>(number);
        else throw std::invalid_argument("Integer bits require a numeric integer value");
    }, value);
}

FieldValue integer_from_bits(FieldType type, std::uint64_t bits) {
    return visit_field_type(type, [&]<class Value>() -> FieldValue {
        if constexpr (std::same_as<Value, std::uint64_t>) return bits;
        else if constexpr (std::same_as<Value, std::int64_t>) return std::bit_cast<std::int64_t>(bits);
        else throw std::invalid_argument("Integer bits require an integer field type");
    });
}

bool integer_range_fits_storage(FieldType type, const FieldValue& minimum,
                               const FieldValue& maximum, std::uint32_t capacity_bits) {
    if (capacity_bits == 0 || capacity_bits > 64)
        throw std::invalid_argument("Integer storage capacity must be 1..64 bits");
    return visit_field_type(type, [&]<class Value>() -> bool {
        if constexpr (std::same_as<Value, std::uint64_t>) {
            const auto limit = capacity_bits == 64 ? std::numeric_limits<Value>::max()
                                                  : (std::uint64_t{1} << capacity_bits) - 1;
            return std::get<Value>(maximum) <= limit;
        } else if constexpr (std::same_as<Value, std::int64_t>) {
            const auto limit = capacity_bits == 64 ? std::numeric_limits<Value>::max()
                                                  : (std::int64_t{1} << (capacity_bits - 1)) - 1;
            return std::get<Value>(minimum) >= -limit - 1 && std::get<Value>(maximum) <= limit;
        } else throw std::invalid_argument("Integer storage requires an integer field type");
    });
}

} // namespace traceschema
