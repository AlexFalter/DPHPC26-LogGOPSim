#include "trace_format/RecordLayout.hpp"
#include "trace_format/BinaryIO.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>

using traceschema::FieldStorageInfo;
using traceschema::FieldId;
using traceschema::FieldIdentity;
using traceschema::FieldType;
using traceschema::require_field;
using traceschema::RecordLayout;
using traceschema::Wildcard;

static FieldStorageInfo layout(FieldIdentity identity, FieldType type,
                          std::uint32_t bits, std::uint64_t offset) {
    const auto maximum = bits >= 64 ? std::numeric_limits<std::uint64_t>::max()
                                   : (std::uint64_t{1} << bits) - 1;
    const auto base = type == FieldType::SignedInteger
        ? traceschema::FieldValue{-static_cast<std::int64_t>(maximum >> 1) - 1}
        : traceschema::FieldValue{std::uint64_t{0}};
    return {std::move(identity), type, bits, offset, {base, maximum}};
}

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function function, const std::string& diagnostic) {
    try {
        function();
    } catch (const std::invalid_argument& error) {
        check(std::string(error.what()).find(diagnostic) != std::string::npos,
              "Missing diagnostic context");
        return;
    }
    throw std::runtime_error("Invalid schema was accepted");
}

int main() {
    try {
        using traceschema::round_up_to_byte;
        check(round_up_to_byte(0) == 0 && round_up_to_byte(1) == 8 &&
              round_up_to_byte(8) == 8 && round_up_to_byte(9) == 16,
              "Byte rounding failed");
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        check(round_up_to_byte(maximum - 7) == maximum - 7 &&
              round_up_to_byte(maximum - 8) == maximum - 7, "Byte rounding boundary failed");
        rejects([&] { (void)round_up_to_byte(maximum); }, "overflows");
        using traceschema::FieldValue;
        using traceschema::schema_helpers::format_field_value;
        using traceschema::schema_helpers::get_value_type;
        check(format_field_value(Wildcard{}) == "ANY", "Wildcard formatting failed");
        rejects([&] { (void)get_value_type(Wildcard{}); }, "numeric value type");
        check(get_value_type(FieldValue{std::uint64_t{1}}) == FieldType::UnsignedInteger &&
              get_value_type(FieldValue{std::int64_t{-1}}) == FieldType::SignedInteger,
              "Wrong logical value type");
        check(format_field_value(FieldValue{std::numeric_limits<std::uint64_t>::max()}) ==
              "18446744073709551615", "Unsigned value formatting failed");
        check(format_field_value(FieldValue{std::numeric_limits<std::int64_t>::min()}) ==
              "-9223372036854775808", "Signed value formatting failed");
        rejects([&] { (void)traceschema::schema_helpers::get_field_type_name(
            static_cast<FieldType>(99)); }, "Unsupported");
        const auto u = FieldType::UnsignedInteger;
        auto packed = traceschema::create_packed_schema({
            layout(FieldId::Type, u, 2, 0), layout(FieldId::Peer, u, 3, 0), layout(FieldId::Tag, u, 6, 0),
            layout(std::string("gpu_id"), u, 8, 0)});
        check(packed.fields[2].bit_offset == 5, "Wrong packed offset");
        check(packed.record_size_bits == 24 && packed.get_record_size_bytes() == 3,
              "Wrong byte padding");
        check(packed.find_field("gpu_id") != nullptr, "Custom field missing");
        check(packed.find_field("absent") == nullptr, "Unexpected field");
        check(packed.find_field(FieldId::Tag) == &packed.fields[2], "Standard ID lookup failed");
        check(packed.find_field(FieldId::Size) == nullptr, "Unexpected standard field");
        std::string source_name = "prefix_gpu_id_suffix";
        std::string_view slice(source_name.data() + 7, 6);
        check(packed.find_field(slice) == &packed.fields[3], "Substring view lookup failed");
        auto owned_spec = traceschema::FieldValueInfo::create_custom(slice, u);
        source_name.assign(source_name.size(), 'x');
        check(std::get<std::string>(owned_spec.field) == "gpu_id", "Spec did not own its name");
        // A custom field named tag cannot impersonate the standard Tag ID.
        auto custom_tag = traceschema::create_packed_schema({layout(std::string("tag"), u, 6, 0)});
        rejects([&] { custom_tag.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}); }, "tag");
        rejects([&] { (void)traceschema::create_packed_schema({layout(FieldId::Tag, u, 6, 0),
                                                layout(FieldId::Tag, u, 6, 0)}); }, "Duplicate");
        rejects([&] { (void)traceschema::create_packed_schema({layout(static_cast<FieldId>(99), u, 8, 0)}); }, "ID");
        auto unnamed = traceschema::create_packed_schema({layout(FieldId::Tag, u, 6, 0)});
        unnamed.check_requirements({require_field<std::uint32_t>(FieldId::Tag)});
        packed.check_requirements({require_field<std::uint8_t>(std::string("gpu_id"))});
        packed.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}); // Extra/custom fields are allowed.
        rejects([&] { packed.check_requirements({require_field<std::uint32_t>(std::string("comm_id"))}); }, "comm_id");
        rejects([&] { packed.check_requirements({require_field<std::int32_t>(FieldId::Tag)}); }, "tag");

        auto wide_tag = traceschema::create_packed_schema({layout(FieldId::Tag, u, 40, 0)});
        rejects([&] { wide_tag.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}); }, "tag");
        wide_tag.check_requirements({require_field<std::uint64_t>(FieldId::Tag)});
        auto signed_field = traceschema::create_packed_schema({layout(std::string("delta"), FieldType::SignedInteger, 8, 0)});
        signed_field.check_requirements({require_field<std::int8_t>(std::string("delta"))});
        rejects([&] { signed_field.check_requirements({require_field<std::uint8_t>(std::string("delta"))}); }, "delta");

        // Explicit layouts need not be in offset order and may contain padding.
        RecordLayout explicit_layout{{layout(FieldId::Tag, u, 16, 16), layout(FieldId::Peer, u, 8, 0)}, 40};
        explicit_layout.validate_layout();
        rejects([&] { RecordLayout{{}, 8}.validate_layout(); }, "at least one");
        rejects([&] { RecordLayout{{layout(std::string("x"), u, 1, 0)}, 7}.validate_layout(); }, "multiple of 8");
        rejects([&] { (void)traceschema::create_packed_schema({layout(std::string(""), u, 8, 0)}); }, "name");
        rejects([&] { (void)traceschema::create_packed_schema({layout(std::string("x"), u, 0, 0)}); }, "positive multiple of 8");
        rejects([&] { (void)traceschema::create_packed_schema({layout(FieldId::Type, u, 2, 0), layout(std::string("x"), u, 0, 0)}); }, "1..64");
        rejects([&] { (void)traceschema::create_packed_schema({layout(std::string("x"), u, 65, 0)}); }, "1..64");
        rejects([&] { (void)traceschema::create_packed_schema({layout(std::string("x"), static_cast<FieldType>(99), 8, 0)}); }, "type");
        rejects([&] { (void)traceschema::create_packed_schema({layout(std::string("x"), u, 8, 0), layout(std::string("x"), u, 8, 0)}); }, "Duplicate");
        rejects([&] { RecordLayout{{layout(std::string("x"), u, 8, 0), layout(std::string("y"), u, 8, 4)}, 16}.validate_layout(); }, "Overlapping");
        rejects([&] { RecordLayout{{layout(std::string("x"), u, 16, 1)}, 16}.validate_layout(); }, "exceeds");
        rejects([&] { RecordLayout{{layout(std::string("x"), u, 8, std::numeric_limits<std::uint64_t>::max())}, 16}.validate_layout(); }, "exceeds");
        traceschema::create_packed_schema({layout(std::string("signed"), FieldType::SignedInteger, 64, 0)}).validate_layout();

        using traceschema::FieldValueInfo;
        using traceschema::FieldValueRange;
        using traceschema::UnsignedRange;
        using traceschema::SignedRange;
        using traceschema::build_record_layout;
        const std::uint64_t num_ranks = 8;
        const FieldValueRange tags = UnsignedRange{0, 42};
        const auto tag = FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, tags);
        auto inferred = build_record_layout({
            FieldValueInfo::create_standard(FieldId::Type), FieldValueInfo::create_standard(FieldId::Peer), tag,
            FieldValueInfo::create_standard(FieldId::Size, std::nullopt, UnsignedRange{0, 1024}),
            FieldValueInfo::create_custom("gpu_id", u, std::nullopt, UnsignedRange{0, 7}),
            FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt,
                SignedRange{-8, 7})}, num_ranks);
        check(inferred.find_field(FieldId::Type)->bit_width == 2, "Type bound failed");
        check(inferred.find_field(FieldId::Peer)->bit_width == 3, "Peer bound failed");
        check(inferred.find_field(FieldId::Tag)->bit_width == 6, "Tag inference failed");
        check(inferred.find_field(FieldId::Size)->bit_width == 11, "Size inference failed");
        check(inferred.find_field("gpu_id")->bit_width == 3, "Custom inference failed");
        check(inferred.find_field("delta")->bit_width == 4, "Signed inference failed");
        check(inferred.record_size_bits == 32, "Resolved layout padding failed");
        check(build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 24, tags)}).fields[0].bit_width == 24,
              "Explicit width was not preserved");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 24)}); }, "Missing value range");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag)}); }, "Missing value range");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 5, tags)}); }, "too small");
        for (int width : {0, -1, 65}) {
            rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, width)}); }, "1..64");
        }
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Type, 1)}); }, "too small");
        auto unsupported_type = FieldValueInfo::create_custom("unknown",
            static_cast<FieldType>(99), 8, UnsignedRange{0, 7});
        rejects([&] { (void)build_record_layout({unsupported_type}); }, "type");
        auto mismatched_base = inferred;
        mismatched_base.fields[0].offset_encoding.value_base = std::int64_t{1};
        rejects([&] { mismatched_base.validate_layout(); }, "base type");
        auto invalid_definition = tag;
        invalid_definition.type = FieldType::SignedInteger;
        rejects([&] { (void)build_record_layout({invalid_definition}); }, "type");
        auto wildcard_peer = FieldValueInfo::create_standard(FieldId::Peer);
        wildcard_peer.has_wildcard = true;
        check(build_record_layout({wildcard_peer}, 8).fields[0].bit_width == 4, "Explicit Peer wildcard failed");
        auto wildcard_custom = FieldValueInfo::create_custom("channel", u, std::nullopt, UnsignedRange{0, 7});
        wildcard_custom.has_wildcard = true;
        check(build_record_layout({wildcard_custom}).fields[0].bit_width == 4, "Custom wildcard failed");
        wildcard_custom.type = FieldType::SignedInteger;
        wildcard_custom.value_range = SignedRange{-8, 7};
        check(build_record_layout({wildcard_custom}).fields[0].bit_width == 5, "Signed wildcard capacity failed");
        wildcard_custom.value_range = SignedRange{-7, 7};
        check(build_record_layout({wildcard_custom}).fields[0].bit_width == 4, "Signed spare code ignored");
        wildcard_custom.value_range = SignedRange{std::numeric_limits<std::int64_t>::min(),
                                                  std::numeric_limits<std::int64_t>::max()};
        rejects([&] { (void)build_record_layout({wildcard_custom}); }, "No room for wildcard");
        auto wildcard_tag = FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{0, 63});
        wildcard_tag.has_wildcard = true;
        check(build_record_layout({wildcard_tag}).fields[0].bit_width == 7, "Tag wildcard collision");
        std::get<UnsignedRange>(*wildcard_tag.value_range).maximum = std::numeric_limits<std::uint64_t>::max();
        rejects([&] { (void)build_record_layout({wildcard_tag}); }, "wildcard");
        wildcard_tag.has_wildcard = false;
        check(build_record_layout({wildcard_tag}).fields[0].bit_width == 64, "Unsigned limit failed");
        rejects([&] { (void)build_record_layout({tag, tag}); }, "Duplicate");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Peer, std::nullopt,
            UnsignedRange{0, 8})}, num_ranks); }, "num_ranks");
        auto invalid_signed = FieldValueInfo::create_custom("delta", FieldType::SignedInteger,
            std::nullopt, SignedRange{8, 7});
        rejects([&] { (void)build_record_layout({invalid_signed}); }, "value range");
        auto signed_limits = FieldValueInfo::create_custom("delta", FieldType::SignedInteger,
            std::nullopt, SignedRange{std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()});
        check(build_record_layout({signed_limits}).fields[0].bit_width == 64, "Signed limit failed");
        auto wrong_range = tag;
        wrong_range.value_range = SignedRange{0, 42};
        rejects([&] { (void)build_record_layout({wrong_range}); }, "unsigned value range");
        wrong_range.value_range = UnsignedRange{43, 42};
        rejects([&] { (void)build_record_layout({wrong_range}); }, "unsigned value range");
        signed_limits.value_range = UnsignedRange{0, 42};
        rejects([&] { (void)build_record_layout({signed_limits}); }, "signed value range");
        check(build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{1000, 1001})})
            .fields[0].bit_width == 1, "Base-offset inference failed");
        const auto offset_tags = build_record_layout({FieldValueInfo::create_standard(
            FieldId::Tag, 1, UnsignedRange{1000, 1001})});
        auto wildcard_base = offset_tags;
        wildcard_base.fields[0].offset_encoding.value_base = Wildcard{};
        rejects([&] { wildcard_base.validate_layout(); }, "numeric value type");
        const auto& encoding = offset_tags.fields[0].offset_encoding;
        check(std::get<std::uint64_t>(encoding.value_base) == 1000, "Wrong value base");
        check(encoding.encode(std::uint64_t{1000}) == 0 &&
              encoding.encode(std::uint64_t{1001}) == 1, "Wrong unsigned encoding");
        check(std::get<std::uint64_t>(encoding.decode(1)) == 1001, "Wrong unsigned decoding");
        rejects([&] { (void)encoding.encode(std::uint64_t{999}); }, "below");
        rejects([&] { (void)encoding.encode(std::uint64_t{1002}); }, "exceeds");
        rejects([&] { (void)encoding.decode(2); }, "exceeds");
        rejects([&] { (void)encoding.encode(std::int64_t{1000}); }, "type");
        // Small encoded offsets must not hide values too large for the reader.
        rejects([&] { offset_tags.check_requirements({
            require_field<std::uint8_t>(FieldId::Tag)}); }, "tag");
        offset_tags.check_requirements({require_field<std::uint16_t>(FieldId::Tag)});
        signed_limits.value_range = SignedRange{std::numeric_limits<std::int64_t>::min(),
                                                std::numeric_limits<std::int64_t>::max()};
        const auto signed_schema = build_record_layout({signed_limits});
        const auto& signed_encoding = signed_schema.fields[0].offset_encoding;
        for (auto value : {std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                           std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
            check(std::get<std::int64_t>(signed_encoding.decode(signed_encoding.encode(value))) == value,
                  "Signed boundary round trip failed");
        }
        auto narrow_signed = build_record_layout({FieldValueInfo::create_custom("delta",
            FieldType::SignedInteger, std::nullopt, SignedRange{-1001, -1000})});
        check(narrow_signed.fields[0].bit_width == 1, "Negative range did not shrink");
        narrow_signed.check_requirements({require_field<std::int16_t>(std::string("delta"))});
        rejects([&] { narrow_signed.check_requirements({
            require_field<std::int8_t>(std::string("delta"))}); }, "delta");
        auto constant = build_record_layout({FieldValueInfo::create_standard(FieldId::Size,
            std::nullopt, UnsignedRange{1024, 1024})});
        check(constant.fields[0].bit_width == 1, "Constant width failed");
        auto high_unsigned = build_record_layout({FieldValueInfo::create_standard(FieldId::Size,
            std::nullopt, UnsignedRange{std::numeric_limits<std::uint64_t>::max() - 1,
                                     std::numeric_limits<std::uint64_t>::max()})});
        const auto& high_encoding = high_unsigned.fields[0].offset_encoding;
        check(std::get<std::uint64_t>(high_encoding.decode(1)) ==
            std::numeric_limits<std::uint64_t>::max(), "Unsigned boundary round trip failed");
        auto wildcard_constant = FieldValueInfo::create_standard(FieldId::Tag,
            std::nullopt, UnsignedRange{42, 42});
        wildcard_constant.has_wildcard = true;
        check(build_record_layout({wildcard_constant}).fields[0].bit_width == 1,
              "Constant plus wildcard needs only two codes");
        const auto wildcard_schema = build_record_layout({wildcard_constant});
        const auto& wildcard_encoding = wildcard_schema.fields[0].offset_encoding;
        check(wildcard_encoding.get_wildcard_code() == 1, "Wrong constant wildcard code");
        check(wildcard_encoding.encode(Wildcard{}) == 1, "Wildcard encoding failed");
        check(std::holds_alternative<Wildcard>(wildcard_encoding.decode(1)), "Wildcard decoding failed");
        check(std::get<std::uint64_t>(wildcard_encoding.decode(0)) == 42,
              "Wildcard collided with numeric value");
        rejects([&] { (void)wildcard_encoding.decode(2); }, "exceeds");
        rejects([&] { (void)encoding.encode(Wildcard{}); }, "wildcard");
        auto explicit_wildcard = wildcard_constant;
        explicit_wildcard.value_range.reset();
        explicit_wildcard.width = 8;
        rejects([&] { (void)build_record_layout({explicit_wildcard}); }, "Missing value range");
        auto wildcard_pair = FieldValueInfo::create_standard(FieldId::Tag,
            std::nullopt, UnsignedRange{1000, 1001});
        wildcard_pair.has_wildcard = true;
        const auto pair_schema = build_record_layout({wildcard_pair});
        const auto& pair_encoding = pair_schema.fields[0].offset_encoding;
        check(pair_schema.fields[0].bit_width == 2 && pair_encoding.encode(Wildcard{}) == 2,
              "Pair wildcard code failed");
        check(std::holds_alternative<Wildcard>(pair_encoding.decode(2)) &&
              std::get<std::uint64_t>(pair_encoding.decode(1)) == 1001,
              "Pair wildcard decoding failed");
        auto signed_wildcard = FieldValueInfo::create_custom("signed_any",
            FieldType::SignedInteger, std::nullopt, SignedRange{-1, 1});
        signed_wildcard.has_wildcard = true;
        const auto signed_any = build_record_layout({signed_wildcard});
        check(std::holds_alternative<Wildcard>(signed_any.fields[0].offset_encoding.decode(3)) &&
              signed_any.fields[0].offset_encoding.encode(Wildcard{}) == 3,
              "Signed wildcard round trip failed");
        auto invalid_wildcard = build_record_layout({wildcard_peer}, 8);
        invalid_wildcard.fields[0].bit_width = 3;
        rejects([&] { invalid_wildcard.validate_layout(); }, "width");
        invalid_wildcard = high_unsigned;
        invalid_wildcard.fields[0].offset_encoding.maximum_offset =
            std::numeric_limits<std::uint64_t>::max();
        invalid_wildcard.fields[0].offset_encoding.has_wildcard = true;
        rejects([&] { invalid_wildcard.validate_layout(); }, "wildcard");
        auto invalid_offset = constant;
        invalid_offset.fields[0].offset_encoding.maximum_offset = 2;
        rejects([&] { invalid_offset.validate_layout(); }, "width");
        invalid_offset = high_unsigned;
        invalid_offset.fields[0].bit_width = 2;
        invalid_offset.record_size_bits = 8;
        invalid_offset.fields[0].offset_encoding.maximum_offset = 2;
        rejects([&] { invalid_offset.validate_layout(); }, "overflows");
        const traceschema::OffsetEncoding signed_upper{
            std::numeric_limits<std::int64_t>::max(), 1};
        rejects([&] { (void)signed_upper.decode(1); }, "overflows signed");
        const traceschema::OffsetEncoding signed_negative{
            std::int64_t{-1}, std::numeric_limits<std::uint64_t>::max()};
        rejects([&] { (void)signed_negative.decode(std::numeric_limits<std::uint64_t>::max()); },
                "overflows signed");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_custom("", u, 8)}); }, "name");
        rejects([&] { (void)build_record_layout({FieldValueInfo::create_standard(static_cast<FieldId>(99), 8)}); }, "ID");
        for (auto id : {FieldId::Cpu, FieldId::Nic, FieldId::DependencyCount,
                        FieldId::CompletionCount, FieldId::CompletionIndex,
                        FieldId::StartCount, FieldId::StartIndex}) {
            check(build_record_layout({FieldValueInfo::create_standard(id, std::nullopt, UnsignedRange{0, 255})})
                .fields[0].bit_width == 8, "Structural/resource inference failed");
        }
        traceschema::TraceAnalysis statistics;
        check(statistics.get_node_index_bits() == 1, "Empty graph index width failed");
        statistics.finish_rank(1, 1, 0);
        check(statistics.get_node_index_bits() == 1, "Single node index width failed");
        statistics.finish_rank(256, 1, 255);
        check(statistics.get_node_index_bits() == 8, "Power-of-two node count failed");
        statistics.finish_rank(257, 1, 256);
        check(statistics.get_node_index_bits() == 9, "Index width boundary failed");
        statistics.finish_rank(1000, 1, 999);
        statistics.finish_rank(2, 1, 1);
        check(statistics.get_node_index_bits() == 10, "File-wide index bound failed");
        traceschema::TraceAnalysis observed;
        observed.observe(FieldId::Type, 1);
        observed.observe(FieldId::Size, 1000);
        observed.finish_rank(1, 1, 0);
        observed.observe(FieldId::Type, 2);
        observed.observe(FieldId::Size, 1001);
        observed.observe(FieldId::Tag, 42);
        observed.observe_wildcard(FieldId::Tag);
        observed.finish_rank(3, 1, 2);
        const auto statistics_schema = observed.create_schema(2);
        const auto* size = statistics_schema.find_field(FieldId::Size);
        check(size && size->bit_width == 1 &&
              size->offset_encoding.encode(FieldValue{std::uint64_t{1000}}) == 0 &&
              size->offset_encoding.encode(FieldValue{std::uint64_t{1001}}) == 1,
              "Statistics did not infer a file-wide size range");
        const auto* tag_field = statistics_schema.find_field(FieldId::Tag);
        check(tag_field && tag_field->offset_encoding.get_wildcard_code() == 1 &&
              tag_field->offset_encoding.decode(0) == FieldValue{std::uint64_t{42}},
              "Statistics lost wildcard or numeric base");
        check(statistics_schema.find_field(FieldId::Type)->bit_width == 2 &&
              statistics_schema.find_field(FieldId::Peer)->offset_encoding.maximum_offset == 1,
              "Statistics did not apply standard field bounds");
        const auto empty_schema = traceschema::TraceAnalysis{}.create_schema(2);
        check(empty_schema.fields.size() == 11 && empty_schema.find_field(FieldId::Size)->bit_width == 1,
              "Empty statistics did not produce a usable schema");
        std::ostringstream report;
        observed.print_report(report, statistics_schema);
        check(report.str().find("Proposed node record: " + std::to_string(statistics_schema.record_size_bits)) !=
              std::string::npos, "Report did not use the supplied schema");
        std::cout << "RecordLayout checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
