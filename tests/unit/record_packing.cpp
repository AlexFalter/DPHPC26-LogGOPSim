#include "TestHelpers.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryIO.hpp"
#include "trace_format/RecordLayout.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

using namespace traceschema;
using test_helpers::decode_record;
using test_helpers::pack_record;

namespace {

constexpr auto u = FieldType::UnsignedInteger;
constexpr auto s = FieldType::SignedInteger;

unsigned get_bit(std::span<const std::byte> bytes, std::uint64_t position) {
    return (std::to_integer<unsigned>(bytes[position / 8]) >> (position % 8)) & 1u;
}

// Four fields over three bytes, with known bytes.
const RecordLayout golden_layout = create_packed_schema({
    {std::string("a"), u, 3, 0, OffsetEncoding{std::uint64_t{0}, 7}},
    {std::string("b"), u, 5, 0, OffsetEncoding{std::uint64_t{0}, 31}},
    {std::string("c"), u, 9, 0, OffsetEncoding{std::uint64_t{0}, 511}},
    {std::string("d"), s, 7, 0, OffsetEncoding{std::int64_t{-64}, 127}}});
const RecordValues golden_values{FieldValue{std::uint64_t{5}}, FieldValue{std::uint64_t{17}},
                                 FieldValue{std::uint64_t{0x1ab}}, FieldValue{std::int64_t{-2}}};
const std::vector<std::byte> golden_bytes{std::byte{0x8d}, std::byte{0xab}, std::byte{0x7d}};

const RecordLayout small_layout = create_packed_schema({{std::string("small"), u, 3, 0, OffsetEncoding{std::uint64_t{0}, 7}}});

} // namespace

// Compares read_bits and write_bits with a bit-by-bit reference. Exact-sized buffers catch
// accidental wide loads past the final field byte.
TEST(BitPacking, MatchesBitwiseReference) {
    std::mt19937_64 random(42);
    for (std::uint32_t offset = 0; offset < 24; ++offset)
        for (std::uint32_t width = 1; width <= 64; ++width)
            for (int sample = 0; sample < 4; ++sample) {
                std::vector<std::byte> bytes((offset + width + 7) / 8);
                for (auto& byte : bytes) byte = static_cast<std::byte>(random());
                std::uint64_t expected_read = 0;
                for (std::uint32_t bit = 0; bit < width; ++bit)
                    expected_read |= std::uint64_t{get_bit(bytes, offset + bit)} << bit;
                ASSERT_EQ(bit_packing::read_bits(bytes, offset, width), expected_read)
                    << "offset " << offset << ", width " << width;

                const auto value = random();
                auto expected_bytes = bytes;
                for (std::uint32_t bit = 0; bit < width; ++bit) {
                    const auto position = offset + bit;
                    const auto mask = static_cast<std::byte>(1u << (position % 8));
                    auto& destination = expected_bytes[position / 8];
                    destination = (destination & ~mask) | static_cast<std::byte>(((value >> bit) & 1) << (position % 8));
                }
                bit_packing::write_bits(bytes, offset, width, value);
                ASSERT_EQ(bytes, expected_bytes) << "offset " << offset << ", width " << width;
            }
}

TEST(BitPacking, WritesLeaveAdjacentBitsUnchanged) {
    for (std::uint32_t width = 1; width <= 64; ++width)
        for (std::uint32_t offset = 0; offset < 8; ++offset) {
            std::array<std::byte, 10> bytes;
            bytes.fill(std::byte{0xff});
            bit_packing::write_bits(bytes, offset, width, 0);
            ASSERT_EQ(bit_packing::read_bits(bytes, offset, width), 0u) << "offset " << offset << ", width " << width;
            const auto value = std::uint64_t{0xa123456789abcdef} & low_bit_mask(width);
            bit_packing::write_bits(bytes, offset, width, value);
            for (std::uint32_t bit = 0; bit < bytes.size() * 8; ++bit) {
                const auto expected = bit < offset || bit >= offset + width
                    ? 1u : static_cast<unsigned>((value >> (bit - offset)) & 1);
                ASSERT_EQ(get_bit(bytes, bit), expected) << "offset " << offset << ", width " << width << ", bit " << bit;
            }
        }
}

TEST(RecordPacker, PacksAndDecodesKnownBytes) {
    const RecordPacker packer(golden_layout);
    EXPECT_EQ(pack_record(packer, golden_values), golden_bytes);
    EXPECT_EQ(decode_record(packer.get_layout(), golden_bytes), golden_values);
    const std::array wrapped{std::byte{0xff}, std::byte{0x8d}, std::byte{0xab}, std::byte{0x7d}, std::byte{0xff}};
    EXPECT_EQ(decode_record(packer.get_layout(), std::span(wrapped).subspan(1, 3)), golden_values)
        << "decoding a view into a larger buffer";
}

TEST(RecordPacker, PacksIntoAViewWithoutTouchingNeighbours) {
    const RecordPacker packer(golden_layout);
    std::array bytes{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    const auto destination = std::span(bytes).subspan(1, 3);
    packer.pack(golden_values, destination);
    EXPECT_TRUE(std::ranges::equal(destination, golden_bytes));
    EXPECT_EQ(bytes.front(), std::byte{0xff});
    EXPECT_EQ(bytes.back(), std::byte{0xff});
    const RecordValues zero_codes{FieldValue{std::uint64_t{0}}, FieldValue{std::uint64_t{0}},
                                  FieldValue{std::uint64_t{0}}, FieldValue{std::int64_t{-64}}};
    packer.pack(zero_codes, destination);
    EXPECT_TRUE(std::ranges::all_of(destination, [](auto byte) { return byte == std::byte{0}; }))
        << "packing did not clear old bits";
    EXPECT_THROW(packer.pack(golden_values, destination.first(2)), std::invalid_argument);
    EXPECT_THROW(packer.pack({}, destination), std::invalid_argument);
}

// Every field width at every bit alignment, including unaligned 64-bit values.
TEST(RecordPacker, RoundTripsEveryWidthAndAlignment) {
    std::mt19937_64 random(42);
    for (std::uint32_t bits = 1; bits <= 64; ++bits) {
        const auto mask = low_bit_mask(bits);
        const auto signed_max = static_cast<std::int64_t>(mask >> 1);
        for (std::uint64_t offset = 0; offset < 8; ++offset) {
            const RecordLayout unsigned_layout{{{std::string("x"), u, bits, offset, OffsetEncoding{std::uint64_t{0}, mask}}},
                                               ((offset + bits + 7) / 8) * 8};
            const RecordPacker unsigned_packer(unsigned_layout);
            for (auto number : {std::uint64_t{0}, mask, random() & mask}) {
                const RecordValues values{FieldValue{number}};
                const auto bytes = pack_record(unsigned_packer, values);
                ASSERT_EQ(decode_record(unsigned_packer.get_layout(), bytes), values) << bits << " bits at " << offset;
                for (std::uint64_t position = 0; position < unsigned_layout.record_size_bits; ++position) {
                    const auto expected = position >= offset && position < offset + bits ? (number >> (position - offset)) & 1 : 0;
                    ASSERT_EQ(get_bit(bytes, position), expected) << bits << " bits at " << offset << ", bit " << position;
                }
            }
            auto signed_layout = unsigned_layout;
            signed_layout.fields[0].type = s;
            signed_layout.fields[0].offset_encoding = OffsetEncoding{-signed_max - 1, mask};
            const RecordPacker signed_packer(signed_layout);
            for (auto number : {-signed_max - 1, std::int64_t{-1}, std::int64_t{0}, signed_max}) {
                const RecordValues values{FieldValue{number}};
                ASSERT_EQ(decode_record(signed_packer.get_layout(), pack_record(signed_packer, values)), values)
                    << bits << " signed bits at " << offset;
            }
        }
    }
}

TEST(RecordPacker, SharesBoundaryBytesBetweenFields) {
    const RecordPacker packer(create_packed_schema({
        {std::string("before"), u, 7, 0, {std::uint64_t{0}, 127}},
        {std::string("wide"), u, 64, 0, {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}},
        {std::string("after"), u, 1, 0, {std::uint64_t{0}, 1}}}));
    const RecordValues values{FieldValue{std::uint64_t{0x55}}, FieldValue{std::uint64_t{0x0123456789abcdef}},
                              FieldValue{std::uint64_t{1}}};
    const std::vector<std::byte> bytes{std::byte{0xd5}, std::byte{0xf7}, std::byte{0xe6}, std::byte{0xd5}, std::byte{0xc4},
                                       std::byte{0xb3}, std::byte{0xa2}, std::byte{0x91}, std::byte{0x80}};
    EXPECT_EQ(pack_record(packer, values), bytes);
    EXPECT_EQ(decode_record(packer.get_layout(), bytes), values);
}

TEST(RecordPacker, EncodesOffsetsFromBaseAndWildcards) {
    auto tag = FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{1000, 1001});
    tag.has_wildcard = true;
    const RecordPacker packer(build_record_layout(
        {tag, FieldValueInfo::create_custom("delta", s, std::nullopt, SignedRange{-1001, -1000})}));
    EXPECT_EQ(pack_record(packer, {FieldValue{std::uint64_t{1001}}, FieldValue{std::int64_t{-1000}}}),
              std::vector<std::byte>{std::byte{5}});
    const RecordValues wildcard_values{Wildcard{}, FieldValue{std::int64_t{-1001}}};
    EXPECT_EQ(pack_record(packer, wildcard_values), std::vector<std::byte>{std::byte{2}});
    EXPECT_EQ(decode_record(packer.get_layout(), std::array{std::byte{2}}), wildcard_values);
    EXPECT_EQ(decode_record(packer.get_layout(), std::array{std::byte{0xfa}}), wildcard_values) << "padding changed values";
    // Tag code 3 is unassigned.
    EXPECT_THROW((void)decode_record(packer.get_layout(), std::array{std::byte{3}}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(packer, {FieldValue{std::uint64_t{1002}}, FieldValue{std::int64_t{-1001}}}),
                 std::invalid_argument);
}

TEST(RecordPacker, RoundTripsTheFullSignedRange) {
    const RecordPacker packer(build_record_layout({FieldValueInfo::create_custom(
        "full", s, std::nullopt, SignedRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})}));
    for (auto number : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
        const RecordValues values{FieldValue{number}};
        EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), values);
    }
}

// Explicit layouts may have gaps and list fields in a different order from their offsets.
TEST(RecordPacker, PacksExplicitLayoutsWithGaps) {
    const RecordPacker packer(RecordLayout{{{std::string("late"), u, 3, 10, OffsetEncoding{std::uint64_t{0}, 7}},
                                            {std::string("early"), u, 2, 0, OffsetEncoding{std::uint64_t{0}, 3}}}, 24});
    const RecordValues values{FieldValue{std::uint64_t{7}}, FieldValue{std::uint64_t{2}}};
    const std::array bytes{std::byte{2}, std::byte{0x1c}, std::byte{0}};
    EXPECT_TRUE(std::ranges::equal(pack_record(packer, values), bytes));
    EXPECT_EQ(decode_record(packer.get_layout(), bytes), values);
    std::array reused{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    packer.pack(values, reused);
    EXPECT_EQ(reused, bytes) << "packing into reused storage must clear gaps and padding";
}

TEST(RecordPacker, ClearsPaddingInReusedStorage) {
    const RecordPacker packer(RecordLayout{{{std::string("value"), u, 3, 0, OffsetEncoding{std::uint64_t{0}, 7}}}, 16});
    std::array bytes{std::byte{0xff}, std::byte{0xff}};
    packer.pack(std::array{FieldValue{std::uint64_t{5}}}, bytes);
    EXPECT_EQ(bytes, (std::array{std::byte{5}, std::byte{0}}));
    packer.pack(std::array{FieldValue{std::uint64_t{0}}}, bytes);
    EXPECT_EQ(bytes, (std::array{std::byte{0}, std::byte{0}})) << "old nonzero field bits remained";
}

TEST(RecordPacker, RejectsInvalidValuesAndBuffers) {
    const RecordPacker packer(small_layout);
    EXPECT_THROW((void)pack_record(packer, {}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(packer, {Wildcard{}}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(packer, {FieldValue{std::uint64_t{8}}}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(packer, {FieldValue{std::int64_t{1}}}), std::invalid_argument);
    EXPECT_THROW((void)decode_record(packer.get_layout(), {}), std::invalid_argument);
    EXPECT_THROW((void)decode_record(packer.get_layout(), std::array{std::byte{0}, std::byte{0}}), std::invalid_argument);

    const RecordPacker signed_packer(create_packed_schema({{std::string("small"), s, 3, 0, OffsetEncoding{std::int64_t{-4}, 7}}}));
    EXPECT_THROW((void)pack_record(signed_packer, {FieldValue{std::int64_t{-5}}}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(signed_packer, {FieldValue{std::int64_t{4}}}), std::invalid_argument);
    EXPECT_THROW((void)pack_record(signed_packer, {FieldValue{std::uint64_t{1}}}), std::invalid_argument);

    auto too_wide = small_layout;
    too_wide.fields[0].bit_width = 65;
    EXPECT_THROW((void)RecordPacker(too_wide), std::invalid_argument);
}

TEST(RecordPacker, CopiesItsLayout) {
    auto layout = small_layout;
    const RecordPacker packer(layout);
    layout.fields.clear();
    layout.record_size_bits = 0;
    const RecordValues values{FieldValue{std::uint64_t{5}}};
    EXPECT_EQ(pack_record(packer, values), std::vector{std::byte{5}});
    EXPECT_EQ(decode_record(packer.get_layout(), std::array{std::byte{5}}), values);
}
