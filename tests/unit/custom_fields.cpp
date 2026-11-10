#include "TestHelpers.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <stdexcept>
#include <string>

using namespace traceschema;
using test_helpers::decode_record;
using test_helpers::pack_record;

namespace {

auto parse(const char* text) {
    std::istringstream input(text);
    return read_custom_fields(input);
}

const char* const declarations = R"(
    # Fields retain declaration order.
    [[fields]]
    name = "gpu_id"
    type = "unsigned"
    [[fields]]
    name = "priority"
    type = "signed"
)";

} // namespace

TEST(CustomFields, ParsesDeclarationsWithoutInventingRanges) {
    const auto fields = parse(declarations);
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_FALSE(fields[0].value_range || fields[0].width || fields[1].value_range || fields[1].width);
    EXPECT_EQ(fields[0].type, FieldType::UnsignedInteger);
    EXPECT_EQ(fields[1].type, FieldType::SignedInteger);
}

TEST(CustomFields, LayoutNeedsCollectedRanges) {
    EXPECT_THROW((void)build_record_layout(parse(declarations)), std::invalid_argument);
}

// Schema resolution also accepts producer-provided bounds through the C++ API.
TEST(CustomFields, KeepsDeclarationOrderAndPacks) {
    auto fields = parse(declarations);
    fields[0].value_range = UnsignedRange{0, 7};
    fields[1].value_range = SignedRange{-2, 2};
    const auto schema = TraceAnalysis(fields).create_schema(2);
    ASSERT_EQ(schema.fields.size(), 13u);
    EXPECT_EQ(std::get<std::string>(schema.fields[11].field), "gpu_id");
    EXPECT_EQ(std::get<std::string>(schema.fields[12].field), "priority");
    const RecordPacker packer(build_record_layout(fields));
    const RecordValues values{FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-1}}};
    EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), values);
}

TEST(CustomFields, AcceptsEmptyAndCommentOnlyFiles) {
    EXPECT_TRUE(parse("").empty());
    EXPECT_TRUE(parse(" \n\t# No custom fields\n").empty());
}

class InvalidCustomFields : public testing::TestWithParam<const char*> {};

TEST_P(InvalidCustomFields, AreRejected) {
    EXPECT_THROW((void)parse(GetParam()), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(CustomFields, InvalidCustomFields, testing::Values(
    "[[", "field = []", "fields = 3", "fields = [1]",
    "[[fields]]\ntype = 'unsigned'",
    "[[fields]]\nname = ''\ntype = 'unsigned'",
    "[[fields]]\nname = 3\ntype = 'unsigned'",
    "[[fields]]\nname = 'cpu'\ntype = 'unsigned'",
    "[[fields]]\nname = 'bad-name'\ntype = 'unsigned'",
    "[[fields]]\nname = 'x'\ntype = 'float'",
    "[[fields]]\nname = 'x'\ntype = 'unsigned'\nrange = {min = 0, max = 7}",
    "[[fields]]\nname = 'x'\ntype = 'unsigned'\nbits = 3",
    "[[fields]]\nname = 'x'\ntype = 'unsigned'\n[[fields]]\nname = 'x'\ntype = 'signed'",
    "[[fields]]\nname = 'x'\nname = 'y'\ntype = 'unsigned'",
    "[[fields]]\nname = 'x'\ntype = 'unsigned'\ndefault = 0"));
