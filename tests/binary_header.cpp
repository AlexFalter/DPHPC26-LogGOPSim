#include "RecordDecoding.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/RecordLayout.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

using namespace traceschema;
using test_helpers::decode_record;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
static void rejects(Function function) {
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid binary header was accepted");
}

static std::vector<std::byte> from_hex(std::string_view text) {
    std::vector<std::byte> bytes;
    for (std::size_t index = 0; index < text.size(); index += 2)
        bytes.push_back(static_cast<std::byte>(std::stoul(std::string(text.substr(index, 2)), nullptr, 16)));
    return bytes;
}

static void compare_layouts(const RecordLayout& expected, const RecordLayout& actual) {
    check(expected.record_size_bits == actual.record_size_bits && expected.fields.size() == actual.fields.size(),
          "Header changed record size or field count");
    for (std::size_t index = 0; index < expected.fields.size(); ++index) {
        const auto& a = expected.fields[index];
        const auto& b = actual.fields[index];
        check(a.field == b.field && a.type == b.type && a.bit_width == b.bit_width && a.bit_offset == b.bit_offset &&
              a.offset_encoding.value_base == b.offset_encoding.value_base &&
              a.offset_encoding.maximum_offset == b.offset_encoding.maximum_offset &&
              a.offset_encoding.has_wildcard == b.offset_encoding.has_wildcard,
              "Header changed field metadata or declaration order");
    }
}

int main() try {
    auto tag = FieldValueInfo::create_standard(FieldId::Tag, 3, UnsignedRange{1000, 1007});
    const auto simple = build_record_layout({tag});
    const auto golden = from_hex(
        "474f414c53443031" "0200" "4b00000000000000" "0800000000000000" "01000000"
        "00000000" "01" "4b00000000000000"
        "00010300" "0000000000000000" "e803000000000000" "0700000000000000" "04000000");
    check(write_binary_header(simple) == golden, "Wrong header bytes or byte order");
    compare_layouts(simple, read_binary_header(golden).layout);

    for (const auto index_bits : {1u, 5u, 64u}) {
        const auto file_header = read_binary_header(write_binary_header(simple, 8, index_bits));
        check(file_header.rank_count == 8 && file_header.node_index_bits == index_bits &&
              file_header.rank_table_offset_bytes == file_header.header_size_bytes,
              "Rank count, node-index width or table offset changed");
    }
    const auto empty_header = read_binary_header(golden);
    check(empty_header.rank_count == 0 && empty_header.node_index_bits == 1 &&
          empty_header.rank_table_offset_bytes == golden.size(), "Empty-file metadata changed");
    for (const auto index_bits : {0u, 65u, 256u})
        rejects([&] { (void)write_binary_header(simple, 8, index_bits); });

    auto relocated_table = write_binary_header(simple, 8, 5);
    write_little_endian(std::uint64_t{1024}, std::span(relocated_table).subspan(35, 8));
    check(read_binary_header(relocated_table).rank_table_offset_bytes == 1024,
          "Explicit table location was not preserved");
    write_little_endian(std::numeric_limits<std::uint64_t>::max(),
                        std::span(relocated_table).subspan(35, 8));
    rejects([&] { (void)read_binary_header(relocated_table); });

    tag.has_wildcard = true;
    tag.width = std::nullopt;
    const auto mixed = build_record_layout({tag,
        FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt, SignedRange{-1001, -1000}),
        FieldValueInfo::create_custom("tag", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{7, 7}),
        FieldValueInfo::create_standard(FieldId::Cpu, std::nullopt, UnsignedRange{0, 255})});
    auto bytes = write_binary_header(mixed);
    const auto header_size = bytes.size();
    const RecordValues values{Wildcard{}, FieldValue{std::int64_t{-1000}},
                             FieldValue{std::uint64_t{7}}, FieldValue{std::uint64_t{255}}};
    const auto packed = RecordPacker(mixed).pack(values);
    bytes.insert(bytes.end(), packed.begin(), packed.end());
    const auto header = read_binary_header(bytes);
    compare_layouts(mixed, header.layout);
    check(header.header_size_bytes == header_size &&
          decode_record(header.layout, std::span(bytes).subspan(header.header_size_bytes)) == values,
          "Header did not preserve the following packed record");

    auto explicit_layout = mixed;
    explicit_layout.fields[0].bit_offset = 32;
    explicit_layout.fields[1].bit_offset = 16;
    explicit_layout.fields[2].bit_offset = 20;
    explicit_layout.fields[3].bit_offset = 0;
    explicit_layout.record_size_bits = 40;
    compare_layouts(explicit_layout, read_binary_header(write_binary_header(explicit_layout)).layout);

    const auto extremes = build_record_layout({
        FieldValueInfo::create_custom("u", FieldType::UnsignedInteger, std::nullopt,
            UnsignedRange{0, std::numeric_limits<std::uint64_t>::max()}),
        FieldValueInfo::create_custom("s", FieldType::SignedInteger, std::nullopt,
            SignedRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()})});
    compare_layouts(extremes, read_binary_header(write_binary_header(extremes)).layout);

    for (std::size_t length = 0; length < golden.size(); ++length)
        rejects([&] { (void)read_binary_header(std::span(golden).first(length)); });
    for (const auto [offset, value] : {std::pair{0, 0}, {8, 1}, {10, 29}, {10, 255},
                                      {26, 255}, {34, 0}, {34, 65}, {35, 0},
                                      {43, 2}, {44, 0}, {44, 3}, {45, 0}, {45, 65},
                                      {46, 2}, {47, 255}, {71, 0}, {71, 255}}) {
        auto invalid = golden;
        invalid[offset] = static_cast<std::byte>(value);
        rejects([&] { (void)read_binary_header(invalid); });
    }
    auto extra = golden;
    extra.push_back(std::byte{0});
    write_little_endian(std::uint64_t(extra.size()), std::span(extra).subspan(10, 8));
    write_little_endian(std::uint64_t(extra.size()), std::span(extra).subspan(35, 8));
    rejects([&] { (void)read_binary_header(extra); });
    auto bad_name = write_binary_header(build_record_layout({FieldValueInfo::create_custom(
        "x", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1})}));
    write_little_endian(std::uint32_t{1000}, std::span(bad_name).subspan(71, 4));
    rejects([&] { (void)read_binary_header(bad_name); });
    auto invalid_layout = simple;
    invalid_layout.fields[0].bit_width = 65;
    rejects([&] { (void)write_binary_header(invalid_layout); });
    std::cout << "Binary header checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
