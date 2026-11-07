#include "trace_format/BinaryIO.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

using namespace traceschema;

TEST(BinaryIO, WritesAndReadsLittleEndian) {
    std::array<std::byte, 4> bytes;
    write_little_endian(std::uint32_t{0x12345678}, bytes);
    const std::array expected{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
    EXPECT_EQ(bytes, expected);
    EXPECT_EQ(read_little_endian<std::uint32_t>(expected), 0x12345678u);
}

TEST(BinaryIO, UnalignedViewLeavesNeighboursUnchanged) {
    std::array<std::byte, 10> buffer;
    buffer.fill(std::byte{0xff});
    const auto view = std::span(buffer).subspan(1, 8);
    write_little_endian(std::uint64_t{0x0123456789abcdef}, view);
    const std::array expected{std::byte{0xff}, std::byte{0xef}, std::byte{0xcd}, std::byte{0xab},
                              std::byte{0x89}, std::byte{0x67}, std::byte{0x45}, std::byte{0x23},
                              std::byte{0x01}, std::byte{0xff}};
    EXPECT_EQ(buffer, expected);
    EXPECT_EQ(read_little_endian<std::uint64_t>(view), 0x0123456789abcdefu);
}

TEST(BinaryIO, RejectsWrongBufferSizesWithoutWriting) {
    std::array<std::byte, 10> buffer;
    buffer.fill(std::byte{0xff});
    const auto original = buffer;
    EXPECT_THROW(write_little_endian(std::uint64_t{0}, std::span(buffer).first(7)), std::invalid_argument);
    EXPECT_THROW(write_little_endian(std::uint64_t{0}, std::span(buffer).first(9)), std::invalid_argument);
    EXPECT_THROW((void)read_little_endian<std::uint64_t>(std::span(buffer).first(7)), std::invalid_argument);
    EXPECT_THROW((void)read_little_endian<std::uint64_t>(std::span(buffer).first(9)), std::invalid_argument);
    EXPECT_EQ(buffer, original);
}

template<class Integer>
class BinaryIOLimits : public testing::Test {};
using UnsignedIntegers = testing::Types<std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t>;
TYPED_TEST_SUITE(BinaryIOLimits, UnsignedIntegers);

TYPED_TEST(BinaryIOLimits, RoundTripsZeroAndMaximum) {
    std::array<std::byte, sizeof(TypeParam)> bytes;
    for (const auto value : {TypeParam{0}, std::numeric_limits<TypeParam>::max()}) {
        write_little_endian(value, bytes);
        EXPECT_EQ(read_little_endian<TypeParam>(bytes), value);
    }
}
