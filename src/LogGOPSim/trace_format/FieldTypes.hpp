#ifndef LOGGOPSIM_FIELDTYPES_HPP
#define LOGGOPSIM_FIELDTYPES_HPP

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace traceschema {

// Stable binary type IDs; bit_width determines value storage size. Never renumber.
enum class FieldType : std::uint8_t { UnsignedInteger = 1, SignedInteger = 2 };

struct Wildcard {
    bool operator==(const Wildcard&) const = default;
};
using FieldValue = std::variant<std::uint64_t, std::int64_t, Wildcard>;

template<std::integral Integer>
struct IntegerRange {
    Integer minimum;
    Integer maximum;
};
using UnsignedRange = IntegerRange<std::uint64_t>;
using SignedRange = IntegerRange<std::int64_t>;
using FieldValueRange = std::variant<UnsignedRange, SignedRange>;

template<class Value> struct FieldTypeTraits;
template<> struct FieldTypeTraits<std::uint64_t> {
    static constexpr FieldType type = FieldType::UnsignedInteger;
    static constexpr const char* name = "unsigned";
    using Range = UnsignedRange;
};
template<> struct FieldTypeTraits<std::int64_t> {
    static constexpr FieldType type = FieldType::SignedInteger;
    static constexpr const char* name = "signed";
    using Range = SignedRange;
};

namespace schema_helpers {

// Dispatch once here; parsing, ranges and formatting use the same C++ value types.
template<class Function>
decltype(auto) visit_field_type(FieldType type, Function&& function) {
    switch (type) {
    case FieldType::UnsignedInteger: return function.template operator()<std::uint64_t>();
    case FieldType::SignedInteger: return function.template operator()<std::int64_t>();
    default: throw std::invalid_argument("Unsupported field type");
    }
}

bool is_supported_field_type(FieldType type);
[[nodiscard]] const char* get_field_type_name(FieldType type);
[[nodiscard]] FieldType parse_field_type(std::string_view name);
[[nodiscard]] FieldType get_value_type(const FieldValue& value);
[[nodiscard]] FieldValue parse_field_value(FieldType type, std::string_view text, std::string_view field_name);
[[nodiscard]] std::string format_field_value(const FieldValue& value);
void validate_value_range(FieldType type, const FieldValueRange& range, std::string_view field_label);
void observe_field_value(std::optional<FieldValueRange>& range, FieldType type,
                         const FieldValue& value, std::string_view field_label);
[[nodiscard]] FieldValueRange make_zero_range(FieldType type);
[[nodiscard]] std::string format_value_range(const FieldValueRange& range);

} // namespace schema_helpers
} // namespace traceschema
#endif
