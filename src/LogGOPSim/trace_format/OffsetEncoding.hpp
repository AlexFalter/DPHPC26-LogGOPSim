#ifndef LOGGOPSIM_OFFSETENCODING_HPP
#define LOGGOPSIM_OFFSETENCODING_HPP

#include "trace_format/FieldTypes.hpp"

namespace traceschema {

// Integer values use unsigned offsets; other logical types need their own encoding.
struct OffsetEncoding {
    OffsetEncoding(FieldValue base, std::uint64_t maximum, bool wildcard = false)
        : value_base(base), maximum_offset(maximum), has_wildcard(wildcard) {}

    FieldValue value_base;
    std::uint64_t maximum_offset;
    bool has_wildcard;

    [[nodiscard]] std::optional<std::uint64_t> get_wildcard_code() const;
    // Wildcard represents ANY, separately from numeric values.
    [[nodiscard]] std::uint64_t encode(const FieldValue& value) const;
    [[nodiscard]] FieldValue decode(std::uint64_t offset) const;
};

[[nodiscard]] OffsetEncoding create_integer_encoding(
    FieldType type, const FieldValueRange& range, bool has_wildcard);
[[nodiscard]] std::uint64_t integer_to_bits(const FieldValue& value);
[[nodiscard]] FieldValue integer_from_bits(FieldType type, std::uint64_t bits);
[[nodiscard]] bool integer_range_fits_storage(
    FieldType type, const FieldValue& minimum, const FieldValue& maximum, std::uint32_t capacity_bits);

} // namespace traceschema
#endif
