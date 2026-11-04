#ifndef LOGGOPSIM_BINARYFORMAT_HPP
#define LOGGOPSIM_BINARYFORMAT_HPP

#include "trace_format/RecordLayout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace traceschema {

inline constexpr std::uint64_t legacy_format_magic = 4223;
inline constexpr std::uint64_t invalid_schedule_magic = 2342;

[[nodiscard]] bool has_binary_header_magic(std::span<const std::byte> bytes);

struct BinaryHeader {
    RecordLayout layout;
    std::uint64_t header_size_bytes; // Start of the data following this header.
    std::uint32_t rank_count;
    std::uint32_t node_index_bits;
    std::uint64_t rank_table_offset_bytes; // Absolute file offset.
};

// Version 2 locates the rank table immediately after the header; table/data are written separately.
[[nodiscard]] std::vector<std::byte> write_binary_header(
    const RecordLayout& layout, std::uint32_t rank_count = 0, std::uint32_t node_index_bits = 1);
// Accepts a header or a whole file; validates the header and leaves following data alone.
[[nodiscard]] BinaryHeader read_binary_header(std::span<const std::byte> bytes);

struct RankTableEntry {
    std::uint64_t data_offset_bytes; // Absolute file offset of this rank's first node record.
    std::uint32_t node_count;
    std::uint32_t dependency_count;
    std::uint32_t root_count;

    bool operator==(const RankTableEntry&) const = default;
};

// Entries follow rank order. Counts match the current graph's 32-bit limits.
inline constexpr std::size_t rank_table_entry_size_bytes =
    sizeof(RankTableEntry::data_offset_bytes)
    + sizeof(RankTableEntry::node_count)
    + sizeof(RankTableEntry::dependency_count)
    + sizeof(RankTableEntry::root_count);

[[nodiscard]] constexpr std::uint64_t calculate_rank_table_size_bytes(std::uint32_t rank_count) {
    return std::uint64_t{rank_count} * rank_table_entry_size_bytes;
}

[[nodiscard]] std::array<std::byte, rank_table_entry_size_bytes> write_rank_table_entry(
    const RankTableEntry& entry);
[[nodiscard]] RankTableEntry read_rank_table_entry(std::span<const std::byte> entry_bytes);

} // namespace traceschema
#endif
