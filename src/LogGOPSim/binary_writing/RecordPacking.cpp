#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryIO.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace traceschema {

RecordPacker::RecordPacker(RecordLayout schema) : schema_(std::move(schema)) {
    schema_.validate_layout();
    if (schema_.get_record_size_bytes() > std::vector<std::byte>{}.max_size())
        throw std::invalid_argument("Record is too large for a byte buffer");
    std::uint64_t payload_bits = 0, last_field_end = 0;
    for (const auto& field : schema_.fields) {
        payload_bits += field.bit_width;
        last_field_end = std::max(last_field_end, field.bit_offset + field.bit_width);
    }
    // Dense fields overwrite the payload; clear only its padding. Gapped layouts
    // retain full clearing so unused bits cannot contain old buffer contents.
    if (payload_bits == last_field_end)
        clear_from_byte_ = static_cast<std::size_t>(payload_bits / bits_per_byte);
}

std::vector<std::byte> RecordPacker::pack(const RecordValues& values) const {
    std::vector<std::byte> bytes(static_cast<std::size_t>(schema_.get_record_size_bytes()));
    pack_into(values, bytes);
    return bytes;
}

void RecordPacker::pack_into(std::span<const FieldValue> values, std::span<std::byte> bytes) const {
    if (values.size() != schema_.fields.size())
        throw std::invalid_argument("Record value count does not match schema");
    if (bytes.size() != schema_.get_record_size_bytes())
        throw std::invalid_argument("Byte buffer size does not match record size");
    std::ranges::fill(bytes.subspan(clear_from_byte_), std::byte{0});
    for (std::size_t index = 0; index < schema_.fields.size(); ++index) {
        const auto& field = schema_.fields[index];
        bit_packing::write_bits(bytes, field.bit_offset, field.bit_width,
                                field.offset_encoding.encode(values[index]));
    }
}

} // namespace traceschema
