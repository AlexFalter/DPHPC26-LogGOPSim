#include "RecordDecoding.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryIO.hpp"
#include "trace_format/RecordLayout.hpp"

#include <array>
#include <algorithm>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

using namespace traceschema;
using test_helpers::decode_record;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
static void rejects(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid record was accepted");
}

static void check_bit_encoding() {
    std::mt19937_64 random(42);
    for (std::uint32_t offset = 0; offset < 24; ++offset) {
        for (std::uint32_t width = 1; width <= 64; ++width) {
            // Exact-sized buffers catch accidental wide loads past the final field byte.
            const auto byte_count = (offset + width + 7) / 8;
            for (int sample = 0; sample < 4; ++sample) {
                std::vector<std::byte> bytes(byte_count);
                for (auto& byte : bytes) byte = static_cast<std::byte>(random());
                std::uint64_t expected_read = 0;
                for (std::uint32_t bit = 0; bit < width; ++bit) {
                    const auto position = offset + bit;
                    const auto value = (std::to_integer<unsigned>(bytes[position / 8]) >> (position % 8)) & 1u;
                    expected_read |= std::uint64_t{value} << bit;
                }
                check(bit_packing::read_bits(bytes, offset, width) == expected_read,
                      "Packed read differs from independent bit reference");

                const auto value = random();
                auto expected_bytes = bytes;
                for (std::uint32_t bit = 0; bit < width; ++bit) {
                    const auto position = offset + bit;
                    const auto mask = static_cast<std::byte>(1u << (position % 8));
                    auto& destination = expected_bytes[position / 8];
                    destination = (destination & ~mask) |
                        static_cast<std::byte>(((value >> bit) & 1) << (position % 8));
                }
                bit_packing::write_bits(bytes, offset, width, value);
                check(bytes == expected_bytes, "Packed write differs from independent bit reference");
            }
        }
    }
}

int main() {
    try {
        check_bit_encoding();
        for (std::uint32_t width = 1; width <= 64; ++width) {
            for (std::uint32_t offset = 0; offset < 8; ++offset) {
                std::array<std::byte, 10> bytes;
                bytes.fill(std::byte{0xff});
                bit_packing::write_bits(bytes, offset, width, 0);
                check(bit_packing::read_bits(bytes, offset, width) == 0, "Overwriting ones with zero failed");
                const auto mask = width == 64 ? std::numeric_limits<std::uint64_t>::max()
                                              : (std::uint64_t{1} << width) - 1;
                const auto value = std::uint64_t{0xa123456789abcdef} & mask;
                bit_packing::write_bits(bytes, offset, width, value);
                for (std::uint32_t bit = 0; bit < bytes.size() * 8; ++bit) {
                    const auto actual = (std::to_integer<unsigned>(bytes[bit / 8]) >> (bit % 8)) & 1;
                    const auto expected = bit < offset || bit >= offset + width ? 1u
                        : static_cast<unsigned>((value >> (bit - offset)) & 1);
                    check(actual == expected, "In-place update changed adjacent bits");
                }
            }
        }
        const auto u = FieldType::UnsignedInteger;
        const auto s = FieldType::SignedInteger;
        const auto golden_schema = create_packed_schema({
            {std::string("a"), u, 3, 0, OffsetEncoding{std::uint64_t{0}, 7}},
            {std::string("b"), u, 5, 0, OffsetEncoding{std::uint64_t{0}, 31}},
            {std::string("c"), u, 9, 0, OffsetEncoding{std::uint64_t{0}, 511}},
            {std::string("d"), s, 7, 0, OffsetEncoding{std::int64_t{-64}, 127}}});
        const RecordValues golden_values{FieldValue{std::uint64_t{5}},
            FieldValue{std::uint64_t{17}}, FieldValue{std::uint64_t{0x1ab}},
            FieldValue{std::int64_t{-2}}};
        const std::vector<std::byte> golden_bytes{std::byte{0x8d}, std::byte{0xab}, std::byte{0x7d}};
        const RecordPacker golden_schema_packer(golden_schema);
        check(golden_schema_packer.pack(golden_values) == golden_bytes, "Wrong packed bytes");
        check(decode_record(golden_schema_packer.get_layout(), golden_bytes) == golden_values, "Wrong decoded golden bytes");

        const std::array wrapped_bytes{std::byte{0xff}, std::byte{0x8d}, std::byte{0xab},
                                       std::byte{0x7d}, std::byte{0xff}};
        check(decode_record(golden_schema_packer.get_layout(), std::span(wrapped_bytes).subspan(1, 3)) == golden_values,
              "Decoding a byte-buffer view failed");

        std::array direct_bytes{std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                                std::byte{0xff}, std::byte{0xff}};
        auto destination = std::span(direct_bytes).subspan(1, 3);
        golden_schema_packer.pack_into(golden_values, destination);
        check(std::ranges::equal(destination, golden_bytes) && direct_bytes.front() == std::byte{0xff} &&
              direct_bytes.back() == std::byte{0xff}, "Direct packing changed neighboring bytes");
        const RecordValues zero_codes{FieldValue{std::uint64_t{0}}, FieldValue{std::uint64_t{0}},
                                      FieldValue{std::uint64_t{0}}, FieldValue{std::int64_t{-64}}};
        golden_schema_packer.pack_into(zero_codes, destination);
        check(std::ranges::all_of(destination, [](auto byte) { return byte == std::byte{0}; }),
              "Direct packing did not clear old bits");
        rejects([&] { golden_schema_packer.pack_into(golden_values, destination.first(2)); });
        rejects([&] { golden_schema_packer.pack_into({}, destination); });

        std::mt19937_64 random(42);
        // Every supported field size at every bit alignment, including unaligned 64-bit values.
        for (std::uint32_t bits = 1; bits <= 64; ++bits) {
            const auto mask = std::numeric_limits<std::uint64_t>::max() >> (64 - bits);
            const auto signed_max = static_cast<std::int64_t>(mask >> 1);
            for (std::uint64_t offset = 0; offset < 8; ++offset) {
                RecordLayout unsigned_schema{{{std::string("x"), u, bits, offset, OffsetEncoding{std::uint64_t{0}, mask}}},
                    ((offset + bits + 7) / 8) * 8};
                const RecordPacker unsigned_schema_packer(unsigned_schema);
                for (auto number : {std::uint64_t{0}, mask, random() & mask}) {
                    const RecordValues values{FieldValue{number}};
                    const auto bytes = unsigned_schema_packer.pack(values);
                    check(decode_record(unsigned_schema_packer.get_layout(), bytes) == values, "Unsigned round trip failed");
                    for (std::uint64_t position = 0; position < unsigned_schema.record_size_bits; ++position) {
                        const auto expected_bit = position >= offset && position < offset + bits
                            ? (number >> (position - offset)) & 1 : 0;
                        const auto actual_bit = (std::to_integer<unsigned int>(bytes[position / 8]) >>
                                                 (position % 8)) & 1;
                        check(actual_bit == expected_bit, "Packed bytes differ from bit reference");
                    }
                }
                auto signed_schema = unsigned_schema;
                signed_schema.fields[0].type = s;
                signed_schema.fields[0].offset_encoding = OffsetEncoding{-signed_max - 1, mask};
                const RecordPacker signed_schema_packer(signed_schema);
                for (auto number : {-signed_max - 1, std::int64_t{-1}, std::int64_t{0}, signed_max}) {
                    const RecordValues values{FieldValue{number}};
                    check(decode_record(signed_schema_packer.get_layout(), signed_schema_packer.pack(values)) == values,
                          "Signed round trip failed");
                }
            }
        }

        const auto neighbors = create_packed_schema({
            {std::string("before"), u, 7, 0, {std::uint64_t{0}, 127}},
            {std::string("wide"), u, 64, 0, {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}},
            {std::string("after"), u, 1, 0, {std::uint64_t{0}, 1}}});
        const RecordValues neighbor_values{FieldValue{std::uint64_t{0x55}},
            FieldValue{std::uint64_t{0x0123456789abcdef}}, FieldValue{std::uint64_t{1}}};
        const std::vector<std::byte> neighbor_bytes{
            std::byte{0xd5}, std::byte{0xf7}, std::byte{0xe6},
            std::byte{0xd5}, std::byte{0xc4}, std::byte{0xb3},
            std::byte{0xa2}, std::byte{0x91}, std::byte{0x80}};
        const RecordPacker neighbors_packer(neighbors);
        check(neighbors_packer.pack(neighbor_values) == neighbor_bytes, "Shared boundary bytes changed");
        check(decode_record(neighbors_packer.get_layout(), neighbor_bytes) == neighbor_values, "Shared boundary decode failed");

        auto tag = FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{1000, 1001});
        tag.has_wildcard = true;
        const auto based_schema = build_record_layout({tag, FieldValueInfo::create_custom(
            "delta", s, std::nullopt, SignedRange{-1001, -1000})});
        const RecordValues based_values{FieldValue{std::uint64_t{1001}}, FieldValue{std::int64_t{-1000}}};
        const RecordPacker based_schema_packer(based_schema);
        check(based_schema_packer.pack(based_values) == std::vector<std::byte>{std::byte{5}},
              "Wrong base-offset bytes");
        const RecordValues wildcard_values{Wildcard{}, FieldValue{std::int64_t{-1001}}};
        check(based_schema_packer.pack(wildcard_values) == std::vector<std::byte>{std::byte{2}},
              "Wrong wildcard bytes");
        check(decode_record(based_schema_packer.get_layout(), std::array{std::byte{2}}) == wildcard_values,
              "Wildcard round trip failed");
        check(decode_record(based_schema_packer.get_layout(), std::array{std::byte{0xfa}}) == wildcard_values,
              "Padding changed decoded values");
        // Unassigned tag code.
        rejects([&] { (void)decode_record(based_schema_packer.get_layout(), std::array{std::byte{3}}); });
        rejects([&] { (void)based_schema_packer.pack(
            {FieldValue{std::uint64_t{1002}}, FieldValue{std::int64_t{-1001}}}); });

        const auto full_signed = build_record_layout({FieldValueInfo::create_custom("full", s,
            std::nullopt, SignedRange{std::numeric_limits<std::int64_t>::min(),
                                     std::numeric_limits<std::int64_t>::max()})});
        const RecordPacker full_signed_packer(full_signed);
        for (auto number : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0},
                            std::numeric_limits<std::int64_t>::max()}) {
            const RecordValues values{FieldValue{number}};
            check(decode_record(full_signed_packer.get_layout(), full_signed_packer.pack(values)) == values,
                  "Full-range base-offset round trip failed");
        }

        // Explicit layouts may have gaps and list fields in a different order from their offsets.
        const RecordLayout gaps{{{std::string("late"), u, 3, 10, OffsetEncoding{std::uint64_t{0}, 7}},
                                 {std::string("early"), u, 2, 0, OffsetEncoding{std::uint64_t{0}, 3}}}, 24};
        const RecordValues gap_values{FieldValue{std::uint64_t{7}}, FieldValue{std::uint64_t{2}}};
        const RecordPacker gaps_packer(gaps);
        check(gaps_packer.pack(gap_values) == std::vector<std::byte>({std::byte{2}, std::byte{0x1c}, std::byte{0}}),
              "Wrong explicit-layout bytes");
        check(decode_record(gaps_packer.get_layout(),
                  std::array{std::byte{2}, std::byte{0x1c}, std::byte{0}}) == gap_values,
              "Wrong explicit-layout decode");
        std::array dirty_gap_bytes{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
        gaps_packer.pack_into(gap_values, dirty_gap_bytes);
        check(std::ranges::equal(dirty_gap_bytes, gaps_packer.pack(gap_values)),
              "Packing into reused storage must clear gaps and padding");

        const RecordLayout padded{{{std::string("value"), u, 3, 0,
                                         OffsetEncoding{std::uint64_t{0}, 7}}}, 16};
        const RecordPacker padded_packer(padded);
        std::array dirty_padded_bytes{std::byte{0xff}, std::byte{0xff}};
        const std::array padded_values{FieldValue{std::uint64_t{5}}};
        padded_packer.pack_into(padded_values, dirty_padded_bytes);
        check(dirty_padded_bytes == std::array{std::byte{5}, std::byte{0}},
              "Dense packing must overwrite payload and clear partial/full padding bytes");
        const std::array zero_padded_values{FieldValue{std::uint64_t{0}}};
        padded_packer.pack_into(zero_padded_values, dirty_padded_bytes);
        check(dirty_padded_bytes == std::array{std::byte{0}, std::byte{0}},
              "Dense packing must overwrite old nonzero field bits");

        const auto small = create_packed_schema({{std::string("small"), u, 3, 0, OffsetEncoding{std::uint64_t{0}, 7}}});
        const RecordPacker small_packer(small);
        rejects([&] { (void)small_packer.pack({}); });
        rejects([&] { (void)small_packer.pack({Wildcard{}}); });
        rejects([&] { (void)small_packer.pack({FieldValue{std::uint64_t{8}}}); });
        rejects([&] { (void)small_packer.pack({FieldValue{std::int64_t{1}}}); });
        rejects([&] { (void)decode_record(small_packer.get_layout(), {}); });
        rejects([&] { (void)decode_record(small_packer.get_layout(), std::array{std::byte{0}, std::byte{0}}); });
        const auto signed_small = create_packed_schema({{std::string("small"), s, 3, 0, OffsetEncoding{std::int64_t{-4}, 7}}});
        const RecordPacker signed_small_packer(signed_small);
        rejects([&] { (void)signed_small_packer.pack({FieldValue{std::int64_t{-5}}}); });
        rejects([&] { (void)signed_small_packer.pack({FieldValue{std::int64_t{4}}}); });
        rejects([&] { (void)signed_small_packer.pack({FieldValue{std::uint64_t{1}}}); });
        auto invalid_schema = small;
        invalid_schema.fields[0].bit_width = 65;
        rejects([&] { (void)RecordPacker(invalid_schema); });

        auto mutable_schema = small;
        const RecordPacker independent_packer(mutable_schema);
        mutable_schema.fields.clear();
        mutable_schema.record_size_bits = 0;
        const RecordValues independent_values{FieldValue{std::uint64_t{5}}};
        check(independent_packer.pack(independent_values) == std::vector{std::byte{5}},
              "Input schema mutation changed packing");
        check(decode_record(independent_packer.get_layout(), std::array{std::byte{5}}) == independent_values,
              "Input schema mutation changed the stored decoding layout");
        std::cout << "Node packing checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
