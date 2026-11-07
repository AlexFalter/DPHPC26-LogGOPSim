#include "TestHelpers.hpp"
#include "binary_writing/RankPacking.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/Goal.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace traceschema;
using test_helpers::decode_record;
using test_helpers::pack_record;

namespace {

// One send node with every standard field, two custom fields and both kinds of dependents.
class NodeFieldValuesTest : public testing::Test {
protected:
    NodeFieldValuesTest() {
        std::vector<FieldValueInfo> inputs;
        for (const auto& definition : standard_fields) {
            auto field = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 100});
            if (definition.id == FieldId::Type) field.value_range = UnsignedRange{1, 3};
            if (definition.id == FieldId::Peer) {
                field.value_range = UnsignedRange{0, 3};
                field.has_wildcard = true;
            }
            if (definition.id == FieldId::Size) field.value_range = UnsignedRange{1024, 1024};
            if (definition.id == FieldId::Tag) {
                field.value_range = UnsignedRange{42, 43};
                field.has_wildcard = true;
            }
            inputs.push_back(field);
        }
        inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
        layout = build_record_layout(inputs, 4);

        node.dependency_count = 2;
        auto* dependent = graph.Calc(1024, 0, 0);
        graph.Dependency(dependent, &node);
        graph.Dependency(dependent, &node);
        graph.StartDependency(dependent, &node);
        node.custom_values = {FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-2}}};
        dependent->custom_values = node.custom_values;
    }

    // The values the writer stores for the node, in layout order.
    RecordValues pack_node(const RecordLayout& record_layout, std::span<const FieldValueInfo> declarations) {
        const auto packed = RankPacker(record_layout, 1, declarations).pack(graph);
        return decode_record(record_layout, std::span(packed.bytes).first(record_layout.get_record_size_bytes()));
    }

    const std::vector<FieldValueInfo> custom_fields{
        FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 7}),
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4})};
    RecordLayout layout;
    Goal graph;
    Node& node = *graph.Send(3, 1024, 42, 2, 1);
    const RecordValues expected{
        FieldValue{std::uint64_t{2}}, FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{3}},
        FieldValue{std::uint64_t{1024}}, FieldValue{std::uint64_t{42}}, FieldValue{std::uint64_t{2}},
        FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{2}}, FieldValue{std::uint64_t{0}},
        FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{2}},
        FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-2}}};
};

} // namespace

TEST_F(NodeFieldValuesTest, StoresNodeFieldsAndListPositions) {
    const auto values = pack_node(layout, custom_fields);
    EXPECT_EQ(values, expected);
    const RecordPacker packer(read_binary_header(write_binary_header(layout)).layout);
    EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), expected);
}

TEST_F(NodeFieldValuesTest, FollowsTheLayoutOrder) {
    auto reordered = layout;
    std::ranges::reverse(reordered.fields);
    auto reversed = expected;
    std::ranges::reverse(reversed);
    EXPECT_EQ(pack_node(reordered, custom_fields), reversed);
}

TEST_F(NodeFieldValuesTest, ConvertsLegacySentinelsToAny) {
    node.Type = std::to_underlying(OperationType::Receive);
    node.Peer = Node::any_source;
    node.Tag = Node::any_tag;
    const auto values = pack_node(layout, custom_fields);
    EXPECT_TRUE(std::holds_alternative<Wildcard>(values[2]));
    EXPECT_TRUE(std::holds_alternative<Wildcard>(values[4]));
    const RecordPacker packer(layout);
    EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), values);
}

// Calculations have no peer or tag; the writer stores a valid numeric placeholder.
TEST_F(NodeFieldValuesTest, StoresPlaceholdersForUnusedCalculationFields) {
    node.Type = std::to_underlying(OperationType::Calculation);
    node.Peer = node.Tag = 0;
    const auto values = pack_node(layout, custom_fields);
    EXPECT_EQ(values[4], FieldValue{std::uint64_t{42}});
    const RecordPacker packer(layout);
    EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), values);
}

TEST_F(NodeFieldValuesTest, RejectsMissingOrUnknownCustomFields) {
    EXPECT_THROW((void)pack_node(layout, {}), std::invalid_argument);
    auto unknown_field = layout;
    unknown_field.fields.back().field = std::string("unknown");
    EXPECT_THROW((void)pack_node(unknown_field, custom_fields), std::invalid_argument);
}
