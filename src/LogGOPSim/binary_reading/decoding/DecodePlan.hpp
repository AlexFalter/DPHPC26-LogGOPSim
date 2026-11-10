#ifndef LOGGOPSIM_DECODEPLAN_HPP
#define LOGGOPSIM_DECODEPLAN_HPP

#include "trace_format/BinaryIO.hpp"
#include "trace_format/RecordLayout.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace traceschema {

// Where a field's offset lies in a record, prepared so that it can be read with one
// unaligned 64-bit load starting at the field's first byte.
struct FieldSource {
    std::uint32_t first_byte;  // first byte of the field in the record
    std::uint8_t shift;        // position of the field's lowest bit within that byte
    bool spill;                // the field does not fit in the loaded word and continues in the next byte
    bool wildcard;             // offset maximum_offset + 1 stands for ANY
    std::uint64_t mask;        // the field's width as a mask of low bits
    std::uint64_t base;        // value of offset zero, as 64 bits (two's complement if signed)
    std::uint64_t maximum_offset;

    // Rejects fields whose decoded values do not fit in 64 bits.
    [[nodiscard]] static FieldSource create(const FieldStorageInfo& field);

    // The stored offset; the caller guarantees that load_end() bytes are mapped. Defined here
    // so that it inlines into the plan interpreter's loop, which calls it for every field.
    [[nodiscard]] std::uint64_t load(const std::byte* record) const {
        auto offset = read_little_endian<std::uint64_t>({record + first_byte, sizeof(std::uint64_t)}) >> shift;
        // The loaded word lacks the field's top `shift` bits; they are the low bits of the next byte.
        if (spill) offset |= std::to_integer<std::uint64_t>(record[first_byte + sizeof(std::uint64_t)]) << (64 - shift);
        return offset & mask;
    }
    // Read within one record when the mapping does not permit a word-sized load.
    [[nodiscard]] std::uint64_t load_bounded(std::span<const std::byte> record) const {
        return bit_packing::read_bits(record, std::uint64_t{first_byte} * bits_per_byte + shift,
                                      std::bit_width(mask));
    }
    [[nodiscard]] std::uint64_t load(std::span<const std::byte> record, std::size_t readable_bytes) const {
        if (mask == 0) return 0;
        return readable_bytes >= load_end() ? load(record.data()) : load_bounded(record);
    }
    // Bytes read from the record start, including the spill byte.
    [[nodiscard]] std::size_t load_end() const { return first_byte + sizeof(std::uint64_t) + (spill ? 1 : 0); }

    friend bool operator==(const FieldSource&, const FieldSource&) = default;
};

// Writes the low Size bytes of value to member. A fixed size compiles to a single store; one
// memcpy of a runtime size is a library call and made the plan interpreter about 50% slower.
template<std::uint8_t Size>
requires (Size == 1 || Size == 2 || Size == 4 || Size == 8)
void store_bytes(std::byte* member, std::uint64_t value) {
    static_assert(std::endian::native == std::endian::little, "the low bytes of value come first");
    std::memcpy(member, &value, Size);
}

// Output a decoded field is stored into; the value indexes the decoder's targets array.
enum class OutputStruct : std::uint8_t { Node, DependencyLists };

// Where a decoded value goes: a member of the node struct or of the dependency-list positions.
struct FieldDestination {
    OutputStruct target;
    std::uint32_t offset;
    std::uint8_t size; // 1, 2, 4 or 8, as bind_field allows
    std::uint64_t wildcard_value;

    // Writes value to the member in targets. Defined here so that it inlines into the
    // interpreter's loop, which calls it for every field.
    void store(std::byte* const* targets, std::uint64_t value) const {
        std::byte* const member = targets[std::to_underlying(target)] + offset;
        switch (size) {
        case 1: return store_bytes<1>(member, value);
        case 2: return store_bytes<2>(member, value);
        case 4: return store_bytes<4>(member, value);
        case 8: return store_bytes<8>(member, value);
        default: std::unreachable();
        }
    }

    friend bool operator==(const FieldDestination&, const FieldDestination&) = default;
};

// The selected fields of one file, resolved once into a flat list of steps: where each
// field's bits are and where its value goes. Plan runners interpret or compile it.
class DecodePlan {
public:
    // Steps are structural types, so a runner can also take them as template arguments.
    struct Step {
        FieldSource source;
        FieldDestination destination;

        // The value stored for a loaded offset: base + offset, or the destination's ANY value.
        // Returns nothing for an invalid stored offset.
        [[nodiscard]] std::optional<std::uint64_t> get_value(std::uint64_t offset) const {
            if (offset <= source.maximum_offset) [[likely]] return source.base + offset;
            if (source.wildcard && offset == source.maximum_offset + 1) return destination.wildcard_value;
            return std::nullopt;
        }

        friend bool operator==(const Step&, const Step&) = default;
    };

    // Whether the loaded type and peer offsets describe an invalid send to ANY.
    [[nodiscard]] static bool is_send_to_any(const FieldSource& type, std::uint64_t type_offset,
                                             const FieldSource& peer, std::uint64_t peer_offset) {
        return type.base + type_offset == std::uint64_t(OperationType::Send) && peer.wildcard &&
               peer_offset == peer.maximum_offset + 1;
    }

    // One destination per field, in the same order.
    [[nodiscard]] static DecodePlan create(std::span<const FieldStorageInfo> fields,
                                           std::span<const FieldDestination> destinations);

    [[nodiscard]] std::span<const Step> get_steps() const { return steps_; }
    // Positions of the operation type and peer, used to reject sends to ANY.
    [[nodiscard]] std::optional<std::size_t> get_type_step() const { return type_step_; }
    [[nodiscard]] std::optional<std::size_t> get_peer_step() const { return peer_step_; }
    // Bytes read from the record start by any step.
    [[nodiscard]] std::size_t get_load_extent() const { return load_extent_; }

private:
    std::vector<Step> steps_;
    std::size_t load_extent_ = 0;
    std::optional<std::size_t> type_step_;
    std::optional<std::size_t> peer_step_;
};

} // namespace traceschema
#endif
