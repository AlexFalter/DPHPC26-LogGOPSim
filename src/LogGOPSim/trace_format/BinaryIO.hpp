#ifndef LOGGOPSIM_BINARYIO_HPP
#define LOGGOPSIM_BINARYIO_HPP

#include <algorithm>
#include <bit>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace traceschema {

inline constexpr std::uint32_t bits_per_byte = 8;

[[nodiscard]] inline std::uint32_t calculate_required_bits(std::uint64_t maximum) {
    return std::max(1, std::bit_width(maximum));
}

[[nodiscard]] inline std::uint64_t round_up_to_byte(std::uint64_t bit_count) {
    const auto remainder = bit_count % bits_per_byte;
    if (remainder == 0) return bit_count;
    const auto padding_bits = bits_per_byte - remainder;
    if (bit_count > std::numeric_limits<std::uint64_t>::max() - padding_bits)
        throw std::invalid_argument("Padded schema size overflows");
    return bit_count + padding_bits;
}

// Returns a view of the next bytes and advances remaining; no data is copied.
template<class Byte>
requires std::same_as<std::remove_const_t<Byte>, std::byte>
[[nodiscard]] std::span<Byte> take_next_bytes(std::span<Byte>& remaining, std::uint64_t count) {
    if (count > remaining.size()) throw std::invalid_argument("Truncated byte buffer");
    const auto size = static_cast<std::size_t>(count);
    const auto next = remaining.first(size);
    remaining = remaining.subspan(size);
    return next;
}

static_assert(std::endian::native == std::endian::little ||
              std::endian::native == std::endian::big,
              "Mixed-endian machines are unsupported");

// Whole integers and packed fields both use little-endian, least-significant-bit-first storage.
template<std::unsigned_integral Integer>
requires (!std::same_as<std::remove_cv_t<Integer>, bool>)
void write_little_endian(Integer value, std::span<std::byte> destination) {
    if (destination.size() != sizeof(Integer))
        throw std::invalid_argument("Byte buffer size does not match integer size");
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    std::memcpy(destination.data(), &value, sizeof(value));
}

template<std::unsigned_integral Integer>
requires (!std::same_as<std::remove_cv_t<Integer>, bool>)
[[nodiscard]] Integer read_little_endian(std::span<const std::byte> bytes) {
    if (bytes.size() != sizeof(Integer))
        throw std::invalid_argument("Byte buffer size does not match integer size");
    Integer value;
    std::memcpy(&value, bytes.data(), sizeof(value));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}

} // namespace traceschema

namespace traceschema::bit_packing {

[[nodiscard]] inline std::uint64_t calculate_index_array_size_bytes(
    std::uint32_t entry_count, std::uint32_t index_bits) {
    const auto size_bits = std::uint64_t{entry_count} * index_bits;
    return round_up_to_byte(size_bits) / bits_per_byte;
}

inline void write_partial_byte(std::byte& destination, std::uint32_t bit_offset,
                               std::uint32_t bit_count, std::uint64_t code) {
    const auto mask = (1u << bit_count) - 1;
    const auto field_mask = static_cast<std::byte>(mask << bit_offset);
    destination = (destination & ~field_mask) | static_cast<std::byte>((code & mask) << bit_offset);
}

[[nodiscard]] inline std::uint64_t read_partial_byte(
    std::byte source, std::uint32_t bit_offset, std::uint32_t bit_count) {
    const auto mask = (1u << bit_count) - 1;
    return (std::to_integer<unsigned>(source) >> bit_offset) & mask;
}

// Callers check bounds and widths (at most 64 bits). Only boundary bytes need masking;
// memcpy handles whole bytes without alignment assumptions or reading past the field.
inline void write_bits(std::span<std::byte> bytes, std::uint64_t bit_offset,
                       std::uint32_t bit_count, std::uint64_t code) {
    auto byte_index = bit_offset / bits_per_byte;
    const auto first_bit = static_cast<std::uint32_t>(bit_offset % bits_per_byte);
    if (first_bit != 0 && bit_count != 0) {
        const auto head_bits = std::min(bit_count, bits_per_byte - first_bit);
        write_partial_byte(bytes[byte_index++], first_bit, head_bits, code);
        code >>= head_bits;
        bit_count -= head_bits;
    }

    const auto whole_bytes = bit_count / bits_per_byte;
    if (whole_bytes != 0) {
        auto encoded = code;
        if constexpr (std::endian::native == std::endian::big) encoded = std::byteswap(encoded);
        std::memcpy(bytes.data() + byte_index, &encoded, whole_bytes);
        byte_index += whole_bytes;
        bit_count %= bits_per_byte;
        if (bit_count != 0) code >>= whole_bytes * bits_per_byte;
    }
    if (bit_count != 0) write_partial_byte(bytes[byte_index], 0, bit_count, code);
}

[[nodiscard]] inline std::uint64_t read_bits(
    std::span<const std::byte> bytes, std::uint64_t bit_offset, std::uint32_t bit_count) {
    auto byte_index = bit_offset / bits_per_byte;
    const auto first_bit = static_cast<std::uint32_t>(bit_offset % bits_per_byte);
    std::uint64_t code = 0;
    std::uint32_t copied_bits = 0;
    if (first_bit != 0 && bit_count != 0) {
        copied_bits = std::min(bit_count, bits_per_byte - first_bit);
        code = read_partial_byte(bytes[byte_index++], first_bit, copied_bits);
        bit_count -= copied_bits;
    }

    const auto whole_bytes = bit_count / bits_per_byte;
    if (whole_bytes != 0) {
        std::uint64_t encoded = 0;
        std::memcpy(&encoded, bytes.data() + byte_index, whole_bytes);
        if constexpr (std::endian::native == std::endian::big) encoded = std::byteswap(encoded);
        code |= encoded << copied_bits;
        byte_index += whole_bytes;
        copied_bits += whole_bytes * bits_per_byte;
        bit_count %= bits_per_byte;
    }
    if (bit_count != 0)
        code |= read_partial_byte(bytes[byte_index], 0, bit_count) << copied_bits;
    return code;
}

} // namespace traceschema::bit_packing
#endif
