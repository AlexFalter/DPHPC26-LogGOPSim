#ifndef LOGGOPSIM_RECORDPACKING_HPP
#define LOGGOPSIM_RECORDPACKING_HPP

#include "trace_format/RecordLayout.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace traceschema {

// Values follow schema.fields order; Wildcard means ANY, not a missing field.
using RecordValues = std::vector<FieldValue>;

// Owns a schema validated at construction; later changes to the input cannot affect it.
// Bit zero is the least significant bit of byte zero, independent of host byte order.
class RecordPacker {
public:
    explicit RecordPacker(RecordLayout schema);

    // Packing clears padding and writes exactly one complete record.
    [[nodiscard]] std::vector<std::byte> pack(const RecordValues& values) const;
    void pack_into(std::span<const FieldValue> values, std::span<std::byte> destination) const;
    [[nodiscard]] const RecordLayout& get_layout() const { return schema_; }

private:
    const RecordLayout schema_;
    std::size_t clear_from_byte_ = 0;
};

} // namespace traceschema

#endif
