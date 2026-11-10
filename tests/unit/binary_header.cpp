#include "TestHelpers.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"
#include "trace_format/RecordLayout.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace traceschema;
using test_helpers::decode_record;
using test_helpers::pack_record;

namespace {

std::vector<std::byte> from_hex(std::string_view text) {
    std::vector<std::byte> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2)
        bytes.push_back(static_cast<std::byte>(std::stoul(std::string(text.substr(index, 2)), nullptr, 16)));
    return bytes;
}

// Same fields in the same order, with the same metadata.
void expect_same_layout(const RecordLayout& expected, const RecordLayout& actual) {
    EXPECT_EQ(actual.record_size_bits, expected.record_size_bits);
    ASSERT_EQ(actual.fields.size(), expected.fields.size());
    for (std::size_t index = 0; index < expected.fields.size(); ++index) {
        SCOPED_TRACE("field " + std::to_string(index));
        const auto& a = expected.fields[index];
        const auto& b = actual.fields[index];
        EXPECT_EQ(b.field, a.field);
        EXPECT_EQ(b.type, a.type);
        EXPECT_EQ(b.bit_width, a.bit_width);
        EXPECT_EQ(b.bit_offset, a.bit_offset);
        EXPECT_EQ(b.offset_encoding.value_base, a.offset_encoding.value_base);
        EXPECT_EQ(b.offset_encoding.maximum_offset, a.offset_encoding.maximum_offset);
        EXPECT_EQ(b.offset_encoding.has_wildcard, a.offset_encoding.has_wildcard);
    }
}

FieldValueInfo tag_field() { return FieldValueInfo::create_standard(FieldId::Tag, 3, UnsignedRange{1000, 1007}); }

const RecordLayout simple_layout = build_record_layout({tag_field()});

// The header of a file with one 3-bit tag field and no ranks.
const std::vector<std::byte> golden_header = from_hex(
    "474f414c53443031" "0200" "4b00000000000000" "0800000000000000" "01000000"
    "00000000" "01" "4b00000000000000"
    "00010300" "0000000000000000" "e803000000000000" "0700000000000000" "04000000");

RecordLayout mixed_layout() {
    auto tag = tag_field();
    tag.has_wildcard = true;
    tag.width = std::nullopt;
    return build_record_layout({tag,
        FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt, SignedRange{-1001, -1000}),
        FieldValueInfo::create_custom("tag", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{7, 7}),
        FieldValueInfo::create_standard(FieldId::Cpu, std::nullopt, UnsignedRange{0, 255})});
}

} // namespace

TEST(BinaryHeader, WritesKnownBytes) {
    EXPECT_EQ(write_binary_header(simple_layout), golden_header);
    expect_same_layout(simple_layout, read_binary_header(golden_header).layout);
}

TEST(BinaryHeader, StoresRankCountAndIndexWidth) {
    for (const auto index_bits : {1u, 5u, 64u}) {
        const auto header = read_binary_header(write_binary_header(simple_layout, 8, index_bits));
        EXPECT_EQ(header.rank_count, 8u);
        EXPECT_EQ(header.node_index_bits, index_bits);
        EXPECT_EQ(header.rank_table_offset_bytes, header.header_size_bytes);
    }
    const auto empty = read_binary_header(golden_header);
    EXPECT_EQ(empty.rank_count, 0u);
    EXPECT_EQ(empty.node_index_bits, 1u);
    EXPECT_EQ(empty.rank_table_offset_bytes, golden_header.size());
    for (const auto index_bits : {0u, 65u, 256u})
        EXPECT_THROW((void)write_binary_header(simple_layout, 8, index_bits), std::invalid_argument) << index_bits;
}

TEST(BinaryHeader, KeepsAnExplicitRankTableOffset) {
    auto bytes = write_binary_header(simple_layout, 8, 5);
    write_little_endian(std::uint64_t{1024}, std::span(bytes).subspan(35, 8));
    EXPECT_EQ(read_binary_header(bytes).rank_table_offset_bytes, 1024u);
    write_little_endian(std::numeric_limits<std::uint64_t>::max(), std::span(bytes).subspan(35, 8));
    EXPECT_THROW((void)read_binary_header(bytes), std::invalid_argument);
}

TEST(BinaryHeader, IsFollowedByPackedRecords) {
    const auto layout = mixed_layout();
    auto bytes = write_binary_header(layout);
    const auto header_size = bytes.size();
    const RecordValues values{Wildcard{}, FieldValue{std::int64_t{-1000}}, FieldValue{std::uint64_t{7}},
                              FieldValue{std::uint64_t{255}}};
    const auto packed = pack_record(RecordPacker(layout), values);
    bytes.insert(bytes.end(), packed.begin(), packed.end());
    const auto header = read_binary_header(bytes);
    expect_same_layout(layout, header.layout);
    EXPECT_EQ(header.header_size_bytes, header_size);
    EXPECT_EQ(decode_record(header.layout, std::span(bytes).subspan(header.header_size_bytes)), values);
}

TEST(BinaryHeader, RoundTripsExplicitAndExtremeLayouts) {
    auto explicit_layout = mixed_layout();
    explicit_layout.fields[0].bit_offset = 32;
    explicit_layout.fields[1].bit_offset = 16;
    explicit_layout.fields[2].bit_offset = 20;
    explicit_layout.fields[3].bit_offset = 0;
    explicit_layout.record_size_bits = 40;
    expect_same_layout(explicit_layout, read_binary_header(write_binary_header(explicit_layout)).layout);

    const auto extremes = build_record_layout({
        FieldValueInfo::create_custom("u", FieldType::UnsignedInteger, std::nullopt,
                                      UnsignedRange{0, std::numeric_limits<std::uint64_t>::max()}),
        FieldValueInfo::create_custom("s", FieldType::SignedInteger, std::nullopt,
                                      SignedRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})});
    expect_same_layout(extremes, read_binary_header(write_binary_header(extremes)).layout);
}

TEST(BinaryHeader, RejectsTruncatedHeaders) {
    for (std::size_t length = 0; length < golden_header.size(); ++length)
        EXPECT_THROW((void)read_binary_header(std::span(golden_header).first(length)), std::invalid_argument)
            << "length " << length;
}

TEST(BinaryHeader, RejectsCorruptedBytes) {
    for (const auto [offset, value] : {std::pair{0, 0}, {8, 1}, {10, 29}, {10, 255}, {26, 255}, {34, 0}, {34, 65},
                                       {35, 0}, {43, 2}, {44, 0}, {44, 3}, {45, 0}, {45, 65}, {46, 2}, {47, 255},
                                       {71, 0}, {71, 255}}) {
        auto corrupted = golden_header;
        corrupted[offset] = static_cast<std::byte>(value);
        EXPECT_THROW((void)read_binary_header(corrupted), std::invalid_argument)
            << "byte " << offset << " = " << value;
    }
}

TEST(BinaryHeader, RejectsInconsistentSizes) {
    auto extra = golden_header;
    extra.push_back(std::byte{0});
    write_little_endian(std::uint64_t(extra.size()), std::span(extra).subspan(10, 8));
    write_little_endian(std::uint64_t(extra.size()), std::span(extra).subspan(35, 8));
    EXPECT_THROW((void)read_binary_header(extra), std::invalid_argument) << "trailing byte inside the header";

    auto bad_name = write_binary_header(build_record_layout(
        {FieldValueInfo::create_custom("x", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1})}));
    write_little_endian(std::uint32_t{1000}, std::span(bad_name).subspan(71, 4));
    EXPECT_THROW((void)read_binary_header(bad_name), std::invalid_argument) << "name length past the header";

    auto invalid_layout = simple_layout;
    invalid_layout.fields[0].bit_width = 65;
    EXPECT_THROW((void)write_binary_header(invalid_layout), std::invalid_argument);
}
