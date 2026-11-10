#include "TestHelpers.hpp"
#include "goal_parsing/TraceAnalysis.hpp"
#include "trace_format/BinaryIO.hpp"
#include "trace_format/RecordLayout.hpp"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <string>
#include <string_view>

using namespace traceschema;
using test_helpers::RejectsWith;

namespace {

constexpr auto u = FieldType::UnsignedInteger;
constexpr auto uint64_max = std::numeric_limits<std::uint64_t>::max();
constexpr auto int64_min = std::numeric_limits<std::int64_t>::min();
constexpr auto int64_max = std::numeric_limits<std::int64_t>::max();
constexpr std::uint64_t rank_count = 8;

// A field using all codes of its width, from zero or from the most negative value.
FieldStorageInfo layout(FieldIdentity identity, FieldType type, std::uint32_t bits, std::uint64_t offset) {
    const auto maximum = low_bit_mask(bits);
    const auto base = type == FieldType::SignedInteger ? FieldValue{-static_cast<std::int64_t>(maximum >> 1) - 1}
                                                       : FieldValue{std::uint64_t{0}};
    return {std::move(identity), type, bits, offset, {base, maximum}};
}

FieldValueInfo tag_with_range() {
    return FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{0, 42});
}

FieldValueInfo with_wildcard(FieldValueInfo field) {
    field.has_wildcard = true;
    return field;
}

RecordLayout inferred_layout() {
    return build_record_layout({
        FieldValueInfo::create_standard(FieldId::Type), FieldValueInfo::create_standard(FieldId::Peer),
        tag_with_range(), FieldValueInfo::create_standard(FieldId::Size, std::nullopt, UnsignedRange{0, 1024}),
        FieldValueInfo::create_custom("gpu_id", u, std::nullopt, UnsignedRange{0, 7}),
        FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt, SignedRange{-8, 7})},
        rank_count);
}

RecordLayout offset_tags() {
    return build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 1, UnsignedRange{1000, 1001})});
}

RecordLayout high_unsigned() {
    return build_record_layout({FieldValueInfo::create_standard(
        FieldId::Size, std::nullopt, UnsignedRange{uint64_max - 1, uint64_max})});
}

RecordLayout constant_size() {
    return build_record_layout({FieldValueInfo::create_standard(FieldId::Size, std::nullopt, UnsignedRange{1024, 1024})});
}

FieldValueInfo full_signed_range() {
    return FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt,
                                         SignedRange{int64_min, int64_max});
}

} // namespace

TEST(RecordLayout, RoundsBitsUpToBytes) {
    EXPECT_EQ(round_up_to_byte(0), 0u);
    EXPECT_EQ(round_up_to_byte(1), 8u);
    EXPECT_EQ(round_up_to_byte(8), 8u);
    EXPECT_EQ(round_up_to_byte(9), 16u);
    EXPECT_EQ(round_up_to_byte(uint64_max - 7), uint64_max - 7);
    EXPECT_EQ(round_up_to_byte(uint64_max - 8), uint64_max - 7);
    EXPECT_THAT([] { (void)round_up_to_byte(uint64_max); }, RejectsWith("overflows"));
}

TEST(RecordLayout, FormatsValuesAndTypes) {
    using schema_helpers::format_field_value;
    using schema_helpers::get_value_type;
    EXPECT_EQ(format_field_value(Wildcard{}), "ANY");
    EXPECT_THAT([] { (void)get_value_type(Wildcard{}); }, RejectsWith("numeric value type"));
    EXPECT_EQ(get_value_type(FieldValue{std::uint64_t{1}}), FieldType::UnsignedInteger);
    EXPECT_EQ(get_value_type(FieldValue{std::int64_t{-1}}), FieldType::SignedInteger);
    EXPECT_EQ(format_field_value(FieldValue{uint64_max}), "18446744073709551615");
    EXPECT_EQ(format_field_value(FieldValue{int64_min}), "-9223372036854775808");
    EXPECT_THAT([] { (void)schema_helpers::get_field_type_name(static_cast<FieldType>(99)); },
                RejectsWith("Unsupported"));
}

TEST(RecordLayout, PacksFieldsAndFindsThem) {
    const auto packed = create_packed_schema({layout(FieldId::Type, u, 2, 0), layout(FieldId::Peer, u, 3, 0),
                                              layout(FieldId::Tag, u, 6, 0), layout(std::string("gpu_id"), u, 8, 0)});
    EXPECT_EQ(packed.fields[2].bit_offset, 5u);
    EXPECT_EQ(packed.record_size_bits, 24u);
    EXPECT_EQ(packed.get_record_size_bytes(), 3u);
    EXPECT_NE(packed.find_field("gpu_id"), nullptr);
    EXPECT_EQ(packed.find_field("absent"), nullptr);
    EXPECT_EQ(packed.find_field(FieldId::Tag), &packed.fields[2]);
    EXPECT_EQ(packed.find_field(FieldId::Size), nullptr);

    std::string source_name = "prefix_gpu_id_suffix";
    const std::string_view slice(source_name.data() + 7, 6);
    EXPECT_EQ(packed.find_field(slice), &packed.fields[3]);
    const auto owned_spec = FieldValueInfo::create_custom(slice, u);
    source_name.assign(source_name.size(), 'x');
    EXPECT_EQ(std::get<std::string>(owned_spec.field), "gpu_id") << "the spec did not own its name";
}

TEST(RecordLayout, ChecksRequirementsAgainstFields) {
    const auto packed = create_packed_schema({layout(FieldId::Type, u, 2, 0), layout(FieldId::Peer, u, 3, 0),
                                              layout(FieldId::Tag, u, 6, 0), layout(std::string("gpu_id"), u, 8, 0)});
    // A custom field named tag cannot impersonate the standard Tag ID.
    const auto custom_tag = create_packed_schema({layout(std::string("tag"), u, 6, 0)});
    EXPECT_THAT([&] { custom_tag.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}); }, RejectsWith("tag"));
    const auto unnamed = create_packed_schema({layout(FieldId::Tag, u, 6, 0)});
    EXPECT_NO_THROW(unnamed.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}));
    EXPECT_NO_THROW(packed.check_requirements({require_field<std::uint8_t>(std::string("gpu_id"))}));
    // Extra and custom fields are allowed.
    EXPECT_NO_THROW(packed.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}));
    EXPECT_THAT([&] { packed.check_requirements({require_field<std::uint32_t>(std::string("comm_id"))}); },
                RejectsWith("comm_id"));
    EXPECT_THAT([&] { packed.check_requirements({require_field<std::int32_t>(FieldId::Tag)}); }, RejectsWith("tag"));

    const auto wide_tag = create_packed_schema({layout(FieldId::Tag, u, 40, 0)});
    EXPECT_THAT([&] { wide_tag.check_requirements({require_field<std::uint32_t>(FieldId::Tag)}); }, RejectsWith("tag"));
    EXPECT_NO_THROW(wide_tag.check_requirements({require_field<std::uint64_t>(FieldId::Tag)}));
    const auto signed_field = create_packed_schema({layout(std::string("delta"), FieldType::SignedInteger, 8, 0)});
    EXPECT_NO_THROW(signed_field.check_requirements({require_field<std::int8_t>(std::string("delta"))}));
    EXPECT_THAT([&] { signed_field.check_requirements({require_field<std::uint8_t>(std::string("delta"))}); },
                RejectsWith("delta"));
}

TEST(RecordLayout, RejectsInvalidPackedSchemas) {
    EXPECT_THAT([] { (void)create_packed_schema({layout(FieldId::Tag, u, 6, 0), layout(FieldId::Tag, u, 6, 0)}); },
                RejectsWith("Duplicate"));
    EXPECT_THAT([] { (void)create_packed_schema({layout(static_cast<FieldId>(99), u, 8, 0)}); }, RejectsWith("ID"));
    EXPECT_THAT([] { (void)create_packed_schema({layout(std::string(""), u, 8, 0)}); }, RejectsWith("name"));
    EXPECT_THAT([] { (void)create_packed_schema({layout(std::string("x"), u, 65, 0)}); }, RejectsWith("0..64"));
    EXPECT_THAT([] { (void)create_packed_schema({layout(std::string("x"), static_cast<FieldType>(99), 8, 0)}); },
                RejectsWith("type"));
    EXPECT_THAT([] { (void)create_packed_schema({layout(std::string("x"), u, 8, 0), layout(std::string("x"), u, 8, 0)}); },
                RejectsWith("Duplicate"));
}

// Explicit layouts need not be in offset order and may contain padding.
TEST(RecordLayout, ValidatesExplicitLayouts) {
    const RecordLayout unordered_with_padding{{layout(FieldId::Tag, u, 16, 16), layout(FieldId::Peer, u, 8, 0)}, 40};
    const RecordLayout no_fields{{}, 8};
    const RecordLayout partial_byte{{layout(std::string("x"), u, 1, 0)}, 7};
    const RecordLayout overlapping{{layout(std::string("x"), u, 8, 0), layout(std::string("y"), u, 8, 4)}, 16};
    const RecordLayout past_the_end{{layout(std::string("x"), u, 16, 1)}, 16};
    const RecordLayout offset_overflow{{layout(std::string("x"), u, 8, uint64_max)}, 16};
    EXPECT_NO_THROW(unordered_with_padding.validate_layout());
    EXPECT_THAT([&] { no_fields.validate_layout(); }, RejectsWith("at least one"));
    EXPECT_THAT([&] { partial_byte.validate_layout(); }, RejectsWith("multiple of 8"));
    EXPECT_THAT([&] { overlapping.validate_layout(); }, RejectsWith("Overlapping"));
    EXPECT_THAT([&] { past_the_end.validate_layout(); }, RejectsWith("exceeds"));
    EXPECT_THAT([&] { offset_overflow.validate_layout(); }, RejectsWith("exceeds"));
    EXPECT_NO_THROW(create_packed_schema({layout(std::string("signed"), FieldType::SignedInteger, 64, 0)}).validate_layout());
}

// Constant fields take no bits; a record still occupies at least one byte.
TEST(RecordLayout, StoresConstantFieldsInZeroBits) {
    EXPECT_EQ(create_packed_schema({layout(std::string("x"), u, 0, 0)}).record_size_bits, 8u);
    const auto with_constant = create_packed_schema(
        {layout(FieldId::Type, u, 2, 0), layout(std::string("x"), u, 0, 0), layout(std::string("y"), u, 3, 0)});
    EXPECT_EQ(with_constant.find_field("x")->bit_offset, 2u);
    EXPECT_EQ(with_constant.find_field("y")->bit_offset, 2u);
    EXPECT_EQ(with_constant.record_size_bits, 8u);
    EXPECT_EQ(constant_size().fields[0].bit_width, 0u);
}

TEST(BuildRecordLayout, InfersWidthsFromRanges) {
    const auto inferred = inferred_layout();
    EXPECT_EQ(inferred.find_field(FieldId::Type)->bit_width, 2u);
    EXPECT_EQ(inferred.find_field(FieldId::Peer)->bit_width, 3u);
    EXPECT_EQ(inferred.find_field(FieldId::Tag)->bit_width, 6u);
    EXPECT_EQ(inferred.find_field(FieldId::Size)->bit_width, 11u);
    EXPECT_EQ(inferred.find_field("gpu_id")->bit_width, 3u);
    EXPECT_EQ(inferred.find_field("delta")->bit_width, 4u);
    EXPECT_EQ(inferred.record_size_bits, 32u);
    EXPECT_EQ(build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 24, UnsignedRange{0, 42})})
                  .fields[0].bit_width, 24u) << "explicit width was not preserved";
    EXPECT_EQ(build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{1000, 1001})})
                  .fields[0].bit_width, 1u) << "base-offset inference failed";
    EXPECT_EQ(build_record_layout({FieldValueInfo::create_custom("delta", FieldType::SignedInteger, std::nullopt,
                                                                 SignedRange{-1001, -1000})}).fields[0].bit_width, 1u)
        << "negative range did not shrink";
    EXPECT_EQ(build_record_layout({full_signed_range()}).fields[0].bit_width, 64u);
    for (auto id : {FieldId::Cpu, FieldId::Nic, FieldId::DependencyCount, FieldId::FinishDependentsCount,
                    FieldId::FinishDependentsIndex, FieldId::StartDependentsCount, FieldId::StartDependentsIndex})
        EXPECT_EQ(build_record_layout({FieldValueInfo::create_standard(id, std::nullopt, UnsignedRange{0, 255})})
                      .fields[0].bit_width, 8u) << "field " << static_cast<int>(id);
}

TEST(BuildRecordLayout, RejectsInvalidDefinitions) {
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 24)}); },
                RejectsWith("Missing value range"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag)}); },
                RejectsWith("Missing value range"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, 5, UnsignedRange{0, 42})}); },
                RejectsWith("too small"));
    for (int width : {0, -1, 65})
        EXPECT_THAT([&] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Tag, width)}); },
                    RejectsWith("1..64")) << "width " << width;
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Type, 1)}); },
                RejectsWith("too small"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_custom("unknown", static_cast<FieldType>(99), 8,
                                                                              UnsignedRange{0, 7})}); },
                RejectsWith("type"));
    auto signed_tag = tag_with_range();
    signed_tag.type = FieldType::SignedInteger;
    EXPECT_THAT([&] { (void)build_record_layout({signed_tag}); }, RejectsWith("type"));
    EXPECT_THAT([] { (void)build_record_layout({tag_with_range(), tag_with_range()}); }, RejectsWith("Duplicate"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(FieldId::Peer, std::nullopt,
                                                                                UnsignedRange{0, 8})}, rank_count); },
                RejectsWith("num_ranks"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_custom("delta", FieldType::SignedInteger,
                                                                              std::nullopt, SignedRange{8, 7})}); },
                RejectsWith("value range"));
    auto wrong_range = tag_with_range();
    wrong_range.value_range = SignedRange{0, 42};
    EXPECT_THAT([&] { (void)build_record_layout({wrong_range}); }, RejectsWith("unsigned value range"));
    wrong_range.value_range = UnsignedRange{43, 42};
    EXPECT_THAT([&] { (void)build_record_layout({wrong_range}); }, RejectsWith("unsigned value range"));
    auto unsigned_range_for_signed = full_signed_range();
    unsigned_range_for_signed.value_range = UnsignedRange{0, 42};
    EXPECT_THAT([&] { (void)build_record_layout({unsigned_range_for_signed}); }, RejectsWith("signed value range"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_custom("", u, 8)}); }, RejectsWith("name"));
    EXPECT_THAT([] { (void)build_record_layout({FieldValueInfo::create_standard(static_cast<FieldId>(99), 8)}); },
                RejectsWith("ID"));
}

TEST(BuildRecordLayout, ReservesWildcardCodes) {
    EXPECT_EQ(build_record_layout({with_wildcard(FieldValueInfo::create_standard(FieldId::Peer))}, 8).fields[0].bit_width, 4u);
    auto custom = with_wildcard(FieldValueInfo::create_custom("channel", u, std::nullopt, UnsignedRange{0, 7}));
    EXPECT_EQ(build_record_layout({custom}).fields[0].bit_width, 4u);
    custom.type = FieldType::SignedInteger;
    custom.value_range = SignedRange{-8, 7};
    EXPECT_EQ(build_record_layout({custom}).fields[0].bit_width, 5u) << "signed wildcard capacity";
    custom.value_range = SignedRange{-7, 7};
    EXPECT_EQ(build_record_layout({custom}).fields[0].bit_width, 4u) << "signed spare code ignored";
    custom.value_range = SignedRange{int64_min, int64_max};
    EXPECT_THAT([&] { (void)build_record_layout({custom}); }, RejectsWith("No room for wildcard"));

    auto tag = with_wildcard(FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{0, 63}));
    EXPECT_EQ(build_record_layout({tag}).fields[0].bit_width, 7u) << "tag wildcard collision";
    std::get<UnsignedRange>(*tag.value_range).maximum = uint64_max;
    EXPECT_THAT([&] { (void)build_record_layout({tag}); }, RejectsWith("wildcard"));
    tag.has_wildcard = false;
    EXPECT_EQ(build_record_layout({tag}).fields[0].bit_width, 64u);

    auto explicit_width = with_wildcard(FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{42, 42}));
    explicit_width.value_range.reset();
    explicit_width.width = 8;
    EXPECT_THAT([&] { (void)build_record_layout({explicit_width}); }, RejectsWith("Missing value range"));
}

TEST(OffsetEncoding, EncodesRelativeToTheBase) {
    const auto tags = offset_tags();
    const auto& encoding = tags.fields[0].offset_encoding;
    EXPECT_EQ(std::get<std::uint64_t>(encoding.value_base), 1000u);
    EXPECT_EQ(encoding.encode(std::uint64_t{1000}), 0u);
    EXPECT_EQ(encoding.encode(std::uint64_t{1001}), 1u);
    EXPECT_EQ(std::get<std::uint64_t>(encoding.decode(1)), 1001u);
    EXPECT_THAT([&] { (void)encoding.encode(std::uint64_t{999}); }, RejectsWith("below"));
    EXPECT_THAT([&] { (void)encoding.encode(std::uint64_t{1002}); }, RejectsWith("exceeds"));
    EXPECT_THAT([&] { (void)encoding.decode(2); }, RejectsWith("exceeds"));
    EXPECT_THAT([&] { (void)encoding.encode(std::int64_t{1000}); }, RejectsWith("type"));
    EXPECT_THAT([&] { (void)encoding.encode(Wildcard{}); }, RejectsWith("wildcard"));
}

// Small encoded offsets must not hide values too large for the reader.
TEST(OffsetEncoding, RequirementsSeeDecodedValues) {
    const auto tags = offset_tags();
    EXPECT_THAT([&] { tags.check_requirements({require_field<std::uint8_t>(FieldId::Tag)}); }, RejectsWith("tag"));
    EXPECT_NO_THROW(tags.check_requirements({require_field<std::uint16_t>(FieldId::Tag)}));
    const auto narrow_signed = build_record_layout({FieldValueInfo::create_custom(
        "delta", FieldType::SignedInteger, std::nullopt, SignedRange{-1001, -1000})});
    EXPECT_NO_THROW(narrow_signed.check_requirements({require_field<std::int16_t>(std::string("delta"))}));
    EXPECT_THAT([&] { narrow_signed.check_requirements({require_field<std::int8_t>(std::string("delta"))}); },
                RejectsWith("delta"));
}

TEST(OffsetEncoding, RoundTripsBoundaryValues) {
    const auto signed_schema = build_record_layout({full_signed_range()});
    const auto& signed_encoding = signed_schema.fields[0].offset_encoding;
    for (auto value : {int64_min, std::int64_t{-1}, std::int64_t{0}, int64_max})
        EXPECT_EQ(std::get<std::int64_t>(signed_encoding.decode(signed_encoding.encode(value))), value);
    const auto high = high_unsigned();
    EXPECT_EQ(std::get<std::uint64_t>(high.fields[0].offset_encoding.decode(1)), uint64_max);
    const OffsetEncoding signed_upper{int64_max, 1};
    const OffsetEncoding signed_negative{std::int64_t{-1}, uint64_max};
    EXPECT_THAT([&] { (void)signed_upper.decode(1); }, RejectsWith("overflows signed"));
    EXPECT_THAT([&] { (void)signed_negative.decode(uint64_max); }, RejectsWith("overflows signed"));
}

TEST(OffsetEncoding, EncodesWildcardsAfterTheRange) {
    const auto constant_schema = build_record_layout(
        {with_wildcard(FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{42, 42}))});
    EXPECT_EQ(constant_schema.fields[0].bit_width, 1u) << "a constant plus ANY needs only two codes";
    const auto& constant = constant_schema.fields[0].offset_encoding;
    EXPECT_EQ(constant.get_wildcard_code(), 1u);
    EXPECT_EQ(constant.encode(Wildcard{}), 1u);
    EXPECT_TRUE(std::holds_alternative<Wildcard>(constant.decode(1)));
    EXPECT_EQ(std::get<std::uint64_t>(constant.decode(0)), 42u);
    EXPECT_THAT([&] { (void)constant.decode(2); }, RejectsWith("exceeds"));

    const auto pair_schema = build_record_layout(
        {with_wildcard(FieldValueInfo::create_standard(FieldId::Tag, std::nullopt, UnsignedRange{1000, 1001}))});
    const auto& pair = pair_schema.fields[0].offset_encoding;
    EXPECT_EQ(pair_schema.fields[0].bit_width, 2u);
    EXPECT_EQ(pair.encode(Wildcard{}), 2u);
    EXPECT_TRUE(std::holds_alternative<Wildcard>(pair.decode(2)));
    EXPECT_EQ(std::get<std::uint64_t>(pair.decode(1)), 1001u);

    const auto signed_schema = build_record_layout({with_wildcard(
        FieldValueInfo::create_custom("signed_any", FieldType::SignedInteger, std::nullopt, SignedRange{-1, 1}))});
    EXPECT_TRUE(std::holds_alternative<Wildcard>(signed_schema.fields[0].offset_encoding.decode(3)));
    EXPECT_EQ(signed_schema.fields[0].offset_encoding.encode(Wildcard{}), 3u);
}

TEST(RecordLayout, ValidateRejectsInconsistentEncodings) {
    auto mismatched_base = inferred_layout();
    mismatched_base.fields[0].offset_encoding.value_base = std::int64_t{1};
    EXPECT_THAT([&] { mismatched_base.validate_layout(); }, RejectsWith("base type"));
    auto wildcard_base = offset_tags();
    wildcard_base.fields[0].offset_encoding.value_base = Wildcard{};
    EXPECT_THAT([&] { wildcard_base.validate_layout(); }, RejectsWith("numeric value type"));

    auto narrow_wildcard = build_record_layout({with_wildcard(FieldValueInfo::create_standard(FieldId::Peer))}, 8);
    narrow_wildcard.fields[0].bit_width = 3;
    EXPECT_THAT([&] { narrow_wildcard.validate_layout(); }, RejectsWith("width"));
    auto no_wildcard_code = high_unsigned();
    no_wildcard_code.fields[0].offset_encoding.maximum_offset = uint64_max;
    no_wildcard_code.fields[0].offset_encoding.has_wildcard = true;
    EXPECT_THAT([&] { no_wildcard_code.validate_layout(); }, RejectsWith("wildcard"));

    auto offset_beyond_width = constant_size();
    offset_beyond_width.fields[0].offset_encoding.maximum_offset = 2;
    EXPECT_THAT([&] { offset_beyond_width.validate_layout(); }, RejectsWith("width"));
    auto overflowing = high_unsigned();
    overflowing.fields[0].bit_width = 2;
    overflowing.record_size_bits = 8;
    overflowing.fields[0].offset_encoding.maximum_offset = 2;
    EXPECT_THAT([&] { overflowing.validate_layout(); }, RejectsWith("overflows"));
}

TEST(TraceAnalysis, SizesNodeIndicesForTheLargestRank) {
    TraceAnalysis statistics;
    EXPECT_EQ(statistics.get_node_index_bits(), 1u);
    statistics.finish_rank(1, 1, 0);
    EXPECT_EQ(statistics.get_node_index_bits(), 1u);
    statistics.finish_rank(256, 1, 255);
    EXPECT_EQ(statistics.get_node_index_bits(), 8u);
    statistics.finish_rank(257, 1, 256);
    EXPECT_EQ(statistics.get_node_index_bits(), 9u);
    statistics.finish_rank(1000, 1, 999);
    statistics.finish_rank(2, 1, 1);
    EXPECT_EQ(statistics.get_node_index_bits(), 10u);
}

TEST(TraceAnalysis, InfersSchemaFromObservedValues) {
    TraceAnalysis observed;
    observed.observe(FieldId::Type, 1);
    observed.observe(FieldId::Size, 1000);
    observed.finish_rank(1, 1, 0);
    observed.observe(FieldId::Type, 2);
    observed.observe(FieldId::Size, 1001);
    observed.observe(FieldId::Tag, 42);
    observed.observe_wildcard(FieldId::Tag);
    observed.finish_rank(3, 1, 2);
    const auto schema = observed.create_schema(2);

    const auto* size = schema.find_field(FieldId::Size);
    ASSERT_NE(size, nullptr);
    EXPECT_EQ(size->bit_width, 1u);
    EXPECT_EQ(size->offset_encoding.encode(FieldValue{std::uint64_t{1000}}), 0u);
    EXPECT_EQ(size->offset_encoding.encode(FieldValue{std::uint64_t{1001}}), 1u);
    const auto* tag = schema.find_field(FieldId::Tag);
    ASSERT_NE(tag, nullptr);
    EXPECT_EQ(tag->offset_encoding.get_wildcard_code(), 1u);
    EXPECT_EQ(tag->offset_encoding.decode(0), FieldValue{std::uint64_t{42}});
    EXPECT_EQ(schema.find_field(FieldId::Type)->bit_width, 2u);
    EXPECT_EQ(schema.find_field(FieldId::Peer)->offset_encoding.maximum_offset, 1u);

    std::ostringstream report;
    observed.print_report(report, schema);
    EXPECT_THAT(report.str(), testing::HasSubstr("Proposed node record: " + std::to_string(schema.record_size_bits)));
}

TEST(TraceAnalysis, EmptyTraceStillYieldsASchema) {
    const auto schema = TraceAnalysis{}.create_schema(2);
    EXPECT_EQ(schema.fields.size(), 11u);
    EXPECT_EQ(schema.find_field(FieldId::Size)->bit_width, 0u);
    EXPECT_EQ(schema.record_size_bits, 8u);
}
