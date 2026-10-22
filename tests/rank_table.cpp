#include "trace_format/BinaryFormat.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace traceschema;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void rejects(std::span<const std::byte> bytes) {
    try { (void)read_rank_table_entry(bytes); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid rank table entry length accepted");
}

int main() try {
    const RankTableEntry entry{0x0102030405060708, 0x11223344, 0x55667788, 0x00112233};
    const std::array expected{
        std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
        std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
        std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},
        std::byte{0x88}, std::byte{0x77}, std::byte{0x66}, std::byte{0x55},
        std::byte{0x33}, std::byte{0x22}, std::byte{0x11}, std::byte{0x00}};
    check(write_rank_table_entry(entry) == expected, "Rank entry byte order or field order changed");
    check(read_rank_table_entry(expected) == entry, "Rank entry decoding changed");

    for (std::size_t length = 0; length < expected.size(); ++length)
        rejects(std::span(expected).first(length));
    rejects(std::vector<std::byte>(rank_table_entry_size_bytes + 1));

    const auto maximum_count = std::numeric_limits<std::uint32_t>::max();
    const RankTableEntry maximum{std::numeric_limits<std::uint64_t>::max(),
                                 maximum_count, maximum_count, maximum_count};
    const RankTableEntry empty{128, 0, 0, 0};
    check(read_rank_table_entry(write_rank_table_entry(maximum)) == maximum,
          "Rank entry maximum values changed");
    check(read_rank_table_entry(write_rank_table_entry(empty)) == empty,
          "Empty rank entry changed");

    std::vector<std::byte> table(1 + 2 * rank_table_entry_size_bytes, std::byte{0xff});
    const auto first = write_rank_table_entry(entry);
    const auto second = write_rank_table_entry(empty);
    std::ranges::copy(first, table.begin() + 1);
    std::ranges::copy(second, table.begin() + 1 + rank_table_entry_size_bytes);
    check(table.front() == std::byte{0xff} &&
          read_rank_table_entry(std::span(table).subspan(1, rank_table_entry_size_bytes)) == entry &&
          read_rank_table_entry(std::span(table).subspan(1 + rank_table_entry_size_bytes)) == empty,
          "Consecutive or unaligned rank entries changed");
    std::cout << "Rank table checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
