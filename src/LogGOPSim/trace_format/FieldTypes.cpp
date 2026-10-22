#include "trace_format/FieldTypes.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <type_traits>

namespace traceschema::schema_helpers {

static constexpr std::array supported_types{
    FieldTypeTraits<std::uint64_t>::type, FieldTypeTraits<std::int64_t>::type};

bool is_supported_field_type(FieldType type) {
    return std::ranges::find(supported_types, type) != supported_types.end();
}

const char* get_field_type_name(FieldType type) {
    return visit_field_type(type, []<class Value> { return FieldTypeTraits<Value>::name; });
}

FieldType parse_field_type(std::string_view name) {
    for (auto type : supported_types)
        if (name == get_field_type_name(type)) return type;
    throw std::invalid_argument("Unknown custom field type '" + std::string(name) + "'");
}

FieldType get_value_type(const FieldValue& value) {
    return std::visit([](const auto& number) -> FieldType {
        using Value = std::decay_t<decltype(number)>;
        if constexpr (std::same_as<Value, Wildcard>)
            throw std::invalid_argument("Wildcard has no numeric value type");
        else return FieldTypeTraits<Value>::type;
    }, value);
}

FieldValue parse_field_value(FieldType type, std::string_view text, std::string_view field_name) {
    return visit_field_type(type, [&]<class Value>() -> FieldValue {
        if (text.empty())
            throw std::invalid_argument(std::string("Invalid ") + FieldTypeTraits<Value>::name +
                " value for '" + std::string(field_name) + "'");
        Value number;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || end != text.data() + text.size())
            throw std::invalid_argument(std::string("Invalid ") + FieldTypeTraits<Value>::name +
                " value for '" + std::string(field_name) + "'");
        return number;
    });
}

std::string format_field_value(const FieldValue& value) {
    return std::visit([](const auto& number) -> std::string {
        if constexpr (std::same_as<std::decay_t<decltype(number)>, Wildcard>) return "ANY";
        else return std::to_string(number);
    }, value);
}

void validate_value_range(FieldType type, const FieldValueRange& range, std::string_view field_label) {
    visit_field_type(type, [&]<class Value> {
        const auto* bounds = std::get_if<typename FieldTypeTraits<Value>::Range>(&range);
        if (!bounds || bounds->minimum > bounds->maximum)
            throw std::invalid_argument(std::string("Invalid ") + FieldTypeTraits<Value>::name +
                " value range for '" + std::string(field_label) + "'");
    });
}

void observe_field_value(std::optional<FieldValueRange>& range, FieldType type,
                         const FieldValue& value, std::string_view field_label) {
    visit_field_type(type, [&]<class Value> {
        const auto* number = std::get_if<Value>(&value);
        if (!number)
            throw std::invalid_argument("Wrong custom value type for '" + std::string(field_label) + "'");
        using Range = typename FieldTypeTraits<Value>::Range;
        if (!range) range = Range{*number, *number};
        else {
            auto* bounds = std::get_if<Range>(&*range);
            if (!bounds) throw std::invalid_argument("Observed value range type mismatch");
            bounds->minimum = std::min(bounds->minimum, *number);
            bounds->maximum = std::max(bounds->maximum, *number);
        }
    });
}

FieldValueRange make_zero_range(FieldType type) {
    return visit_field_type(type, []<class Value>() -> FieldValueRange {
        return typename FieldTypeTraits<Value>::Range{0, 0};
    });
}

std::string format_value_range(const FieldValueRange& range) {
    return std::visit([](const auto& bounds) {
        return std::to_string(bounds.minimum) + ".." + std::to_string(bounds.maximum);
    }, range);
}

} // namespace traceschema::schema_helpers
