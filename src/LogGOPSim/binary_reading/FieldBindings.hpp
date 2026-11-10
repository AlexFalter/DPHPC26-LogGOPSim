#ifndef LOGGOPSIM_FIELDBINDINGS_HPP
#define LOGGOPSIM_FIELDBINDINGS_HPP

#include "trace_format/RecordLayout.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace traceschema {

// Declares both the requested field and where its decoded value belongs: a member of Output,
// given by its byte offset and size, into which the decoder writes the value.
template<class Output>
struct FieldBinding {
    FieldRequirement requirement;
    std::optional<FieldValue> wildcard_destination;
    std::uint32_t destination_offset = 0;
    std::uint8_t destination_size = 0;
};

template<class Output, std::integral Integer>
// Members are 1, 2, 4 or 8 bytes, the sizes the decoders can store.
requires (!std::same_as<std::remove_cv_t<Integer>, bool> && !std::is_const_v<Integer> &&
          (sizeof(Integer) == 1 || sizeof(Integer) == 2 || sizeof(Integer) == 4 || sizeof(Integer) == 8))
[[nodiscard]] FieldBinding<Output> bind_field(
    FieldIdentity field, Integer Output::* member,
    std::optional<std::type_identity_t<Integer>> wildcard_value = std::nullopt) {
    if (member == nullptr) throw std::invalid_argument("Field binding has no destination member");
    using Value = std::conditional_t<std::numeric_limits<Integer>::is_signed, std::int64_t, std::uint64_t>;
    const Output probe{};
    const auto offset = reinterpret_cast<const std::byte*>(&(probe.*member)) -
                        reinterpret_cast<const std::byte*>(&probe);
    return {require_field<Integer>(std::move(field)),
        wildcard_value ? std::optional<FieldValue>{FieldValue{static_cast<Value>(*wildcard_value)}}
                       : std::nullopt,
        static_cast<std::uint32_t>(offset), static_cast<std::uint8_t>(sizeof(Integer))};
}

template<class Output>
void validate_field_bindings(std::span<const FieldBinding<Output>> bindings,
                             std::span<const FieldStorageInfo> fields) {
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
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
