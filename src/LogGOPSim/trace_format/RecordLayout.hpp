#ifndef LOGGOPSIM_RECORDLAYOUT_HPP
#define LOGGOPSIM_RECORDLAYOUT_HPP

#include "trace_format/FieldIdentity.hpp"
#include "trace_format/OffsetEncoding.hpp"

#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace traceschema {

// Information about one field across the trace, not a single node value.
// Used to choose its width, offset, and encoding.
struct FieldValueInfo {
    FieldIdentity field;
    FieldType type;
    std::optional<FieldValueRange> value_range;
    std::optional<std::uint32_t> width; // nullopt = infer; explicit widths: 1..64 bits.
    // Reserve maximum_offset + 1 for ANY; requires a known numeric range.
    bool has_wildcard = false;

    [[nodiscard]] static FieldValueInfo create_standard(
        FieldId id, std::optional<std::uint32_t> requested_width = std::nullopt,
        std::optional<FieldValueRange> value_range = std::nullopt);

    [[nodiscard]] static FieldValueInfo create_custom(
        std::string_view name, FieldType type, std::optional<std::uint32_t> requested_width = std::nullopt,
        std::optional<FieldValueRange> value_range = std::nullopt);
};

// Consumer destination capacity; this does not set the field's on-disk width.
struct FieldRequirement {
    FieldIdentity field;
    FieldType type;
    std::uint32_t capacity_bits;
};

// Built by the consumer and passed to NodeDecoder at setup; the decoder checks
// these requirements and retains the matching field layouts, not this list.
using FieldRequirements = std::vector<FieldRequirement>;

template<std::integral Integer>
requires (!std::same_as<std::remove_cv_t<Integer>, bool>)
[[nodiscard]] FieldRequirement require_field(FieldIdentity field) {
    constexpr bool is_signed = std::numeric_limits<Integer>::is_signed;
    constexpr auto bit_capacity = std::numeric_limits<Integer>::digits + (is_signed ? 1 : 0);
    static_assert(bit_capacity <= 64, "Destination must fit within 64 bits");

    using Value = std::conditional_t<is_signed, std::int64_t, std::uint64_t>;
    return {std::move(field), FieldTypeTraits<Value>::type, bit_capacity};
}

struct FieldStorageInfo {
    FieldIdentity field;
    FieldType type;
    std::uint32_t bit_width; // Bits occupied by the value in the trace record.
    std::uint64_t bit_offset; // Relative to the beginning of each node record.
    OffsetEncoding offset_encoding; // Store value - value_base as unsigned.
};

struct RecordLayout {
    std::vector<FieldStorageInfo> fields;
    std::uint64_t record_size_bits; // Includes padding; must be byte-sized.

    // nullptr if absent; returned pointers follow vector element lifetime.
    [[nodiscard]] const FieldStorageInfo* find_field(FieldId id) const;
    [[nodiscard]] const FieldStorageInfo* find_field(std::string_view custom_name) const;

    // Throws std::invalid_argument for malformed field layouts.
    void validate_layout() const;
    // Reader setup: check required fields and decoded ranges against consumer storage types.
    // Also validates the layout; do not call per node.
    void check_requirements(const FieldRequirements& requirements) const;
    // Accessor for a validated layout.
    [[nodiscard]] std::uint64_t get_record_size_bytes() const;
};

// Reassign offsets consecutively and pad the record to a whole byte.
[[nodiscard]] RecordLayout create_packed_schema(std::vector<FieldStorageInfo> fields);

// Resolve supplied ranges or built-in Type/Peer bounds; a range is required even
// with an explicit width, so every resulting field has a known offset encoding.
// Checks explicit widths too. num_ranks = 0 means unknown.
// Does not scan trace data or validate actual values.
[[nodiscard]] RecordLayout build_record_layout(const std::vector<FieldValueInfo>& fields,
                           std::uint64_t num_ranks = 0);

} // namespace traceschema

#endif
