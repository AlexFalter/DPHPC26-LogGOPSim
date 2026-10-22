#ifndef LOGGOPSIM_FIELDBINDINGS_HPP
#define LOGGOPSIM_FIELDBINDINGS_HPP

#include "trace_format/RecordLayout.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace traceschema {

// Declares both the requested field and where its decoded value belongs.
template<class Output>
struct FieldBinding {
    FieldRequirement requirement;
    std::function<void(Output&, const FieldValue&)> assign;
    std::optional<FieldValue> wildcard_destination;
};

template<class Output, std::integral Integer>
requires (!std::same_as<std::remove_cv_t<Integer>, bool> && !std::is_const_v<Integer>)
[[nodiscard]] FieldBinding<Output> bind_field(
    FieldIdentity field, Integer Output::* member,
    std::optional<std::type_identity_t<Integer>> wildcard_value = std::nullopt) {
    if (member == nullptr) throw std::invalid_argument("Field binding has no destination member");
    using Value = std::conditional_t<std::numeric_limits<Integer>::is_signed, std::int64_t, std::uint64_t>;
    return {require_field<Integer>(std::move(field)),
        [member, wildcard_value](Output& output, const FieldValue& value) {
            if (std::holds_alternative<Wildcard>(value)) {
                if (!wildcard_value) throw std::invalid_argument("Field binding has no wildcard value");
                output.*member = *wildcard_value;
            } else {
                output.*member = static_cast<Integer>(std::get<Value>(value));
            }
        }, wildcard_value ? std::optional<FieldValue>{FieldValue{static_cast<Value>(*wildcard_value)}}
                          : std::nullopt};
}

template<class Output>
[[nodiscard]] FieldRequirements get_field_requirements(
    std::span<const FieldBinding<Output>> bindings) {
    FieldRequirements requirements;
    requirements.reserve(bindings.size());
    for (const auto& binding : bindings) requirements.push_back(binding.requirement);
    return requirements;
}

template<class Output>
void validate_field_bindings(std::span<const FieldBinding<Output>> bindings,
                             std::span<const FieldStorageInfo> fields) {
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (!binding.assign) throw std::invalid_argument("Field binding has no assignment");
        if (fields[index].offset_encoding.has_wildcard && !binding.wildcard_destination)
            throw std::invalid_argument("Missing wildcard destination for '" +
                schema_helpers::get_field_label(binding.requirement.field) + "'");
        if (binding.wildcard_destination) {
            const auto& encoding = fields[index].offset_encoding;
            const auto maximum = encoding.decode(encoding.maximum_offset);
            const auto& sentinel = *binding.wildcard_destination;
            const bool collision = schema_helpers::visit_field_type(fields[index].type, [&]<class Value> {
                const auto number = std::get<Value>(sentinel);
                return number >= std::get<Value>(encoding.value_base) && number <= std::get<Value>(maximum);
            });
            if (collision)
                throw std::invalid_argument("Numeric range collides with wildcard destination for '" +
                    schema_helpers::get_field_label(binding.requirement.field) + "'");
        }
    }
}

} // namespace traceschema
#endif
