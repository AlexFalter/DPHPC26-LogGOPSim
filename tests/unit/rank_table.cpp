#include "trace_format/BinaryFormat.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

using namespace traceschema;

namespace {

const RankTableEntry entry{0x0102030405060708, 0x11223344, 0x55667788, 0x00112233};
const std::array entry_bytes{
    std::byte{0x08}, std::byte{0x07}, std::byte{0x06}, std::byte{0x05},
    std::byte{0x04}, std::byte{0x03}, std::byte{0x02}, std::byte{0x01},
    std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},
    std::byte{0x88}, std::byte{0x77}, std::byte{0x66}, std::byte{0x55},
    std::byte{0x33}, std::byte{0x22}, std::byte{0x11}, std::byte{0x00}};
const RankTableEntry empty_entry{128, 0, 0, 0};

} // namespace

TEST(RankTable, KeepsByteAndFieldOrder) {
    EXPECT_EQ(write_rank_table_entry(entry), entry_bytes);
    EXPECT_EQ(read_rank_table_entry(entry_bytes), entry);
}

TEST(RankTable, RejectsWrongEntryLengths) {
    for (std::size_t length = 0; length < entry_bytes.size(); ++length)
        EXPECT_THROW((void)read_rank_table_entry(std::span(entry_bytes).first(length)), std::invalid_argument)
            << "length " << length;
    EXPECT_THROW((void)read_rank_table_entry(std::vector<std::byte>(rank_table_entry_size_bytes + 1)),
                 std::invalid_argument);
}

TEST(RankTable, RoundTripsMaximumAndEmptyEntries) {
    const auto maximum_count = std::numeric_limits<std::uint32_t>::max();
    const RankTableEntry maximum{std::numeric_limits<std::uint64_t>::max(), maximum_count, maximum_count,
                                 maximum_count};
    EXPECT_EQ(read_rank_table_entry(write_rank_table_entry(maximum)), maximum);
    EXPECT_EQ(read_rank_table_entry(write_rank_table_entry(empty_entry)), empty_entry);
}

TEST(RankTable, ReadsConsecutiveUnalignedEntries) {
    std::vector<std::byte> table(1 + 2 * rank_table_entry_size_bytes, std::byte{0xff});
    std::ranges::copy(write_rank_table_entry(entry), table.begin() + 1);
    std::ranges::copy(write_rank_table_entry(empty_entry), table.begin() + 1 + rank_table_entry_size_bytes);
    EXPECT_EQ(table.front(), std::byte{0xff});
    EXPECT_EQ(read_rank_table_entry(std::span(table).subspan(1, rank_table_entry_size_bytes)), entry);
    EXPECT_EQ(read_rank_table_entry(std::span(table).subspan(1 + rank_table_entry_size_bytes)), empty_entry);
}
