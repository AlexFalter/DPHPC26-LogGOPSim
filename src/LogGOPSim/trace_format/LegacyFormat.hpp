#pragma once

#include <cstddef>
#include <cstdint>

namespace traceschema::legacy_format {
inline constexpr std::size_t magic_bytes = sizeof(std::uint64_t);
inline constexpr std::size_t rank_count_offset = magic_bytes;
inline constexpr std::size_t cpu_offset = rank_count_offset + sizeof(std::uint32_t);
inline constexpr std::size_t nic_offset = cpu_offset + sizeof(std::uint8_t);
inline constexpr std::size_t prefix_bytes = nic_offset + sizeof(std::uint8_t);
inline constexpr std::size_t rank_table_entry_bytes = 2 * sizeof(std::uint64_t);
inline constexpr std::size_t rank_prefix_bytes = 2 * sizeof(std::uint32_t);

inline constexpr std::size_t dependency_count_offset = 0;
inline constexpr std::size_t type_offset = dependency_count_offset + sizeof(std::uint32_t);
inline constexpr std::size_t peer_offset = type_offset + sizeof(std::uint8_t);
inline constexpr std::size_t size_offset = peer_offset + sizeof(std::uint32_t);
inline constexpr std::size_t tag_offset = size_offset + sizeof(std::uint64_t);
inline constexpr std::size_t cpu_field_offset = tag_offset + sizeof(std::uint32_t);
inline constexpr std::size_t nic_field_offset = cpu_field_offset + sizeof(std::uint8_t);
inline constexpr std::size_t finish_count_offset = nic_field_offset + sizeof(std::uint8_t);
inline constexpr std::size_t finish_begin_offset = finish_count_offset + sizeof(std::uint32_t);
inline constexpr std::size_t start_count_offset = finish_begin_offset + sizeof(std::uint32_t);
inline constexpr std::size_t start_begin_offset = start_count_offset + sizeof(std::uint32_t);
inline constexpr std::size_t record_bytes = start_begin_offset + sizeof(std::uint32_t);
static_assert(record_bytes == 39);
} // namespace traceschema::legacy_format
