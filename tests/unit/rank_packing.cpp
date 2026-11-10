#include "TestHelpers.hpp"
#include "binary_writing/RankPacking.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/Goal.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <vector>

using namespace traceschema;
using test_helpers::decode_record;

namespace {

FieldValue number(std::uint64_t value) { return value; }

// A layout with every standard field and two custom fields, and a three-node rank: calc, then a
// send that waits for it, then a receive that waits for the send and starts with the calc.
class RankPackingTest : public testing::Test {
protected:
    RankPackingTest() {
        std::vector<FieldValueInfo> inputs;
        for (const auto& definition : standard_fields) {
            auto field = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 1024});
            if (definition.id == FieldId::Type) field.value_range = UnsignedRange{1, 3};
            if (definition.id == FieldId::Peer) {
                field.value_range = UnsignedRange{0, 1};
                field.has_wildcard = true;
            }
            if (definition.id == FieldId::Tag) {
                field.value_range = UnsignedRange{42, 42};
                field.has_wildcard = true;
            }
            inputs.push_back(field);
        }
        inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
        layout = build_record_layout(inputs, 2);

        graph.Dependency(send, calc);
        graph.Dependency(receive, send);
        graph.StartDependency(receive, calc);
        calc->custom_values = {number(3), FieldValue{std::int64_t{-2}}};
        send->custom_values = {number(7), FieldValue{std::int64_t{4}}};
        receive->custom_values = {number(0), FieldValue{std::int64_t{0}}};
    }

    const std::vector<FieldValueInfo> custom_fields{
        FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 7}),
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4})};
    RecordLayout layout;
    Goal graph;
    Node* calc = graph.Calc(1000, 0, 0);
    Node* send = graph.Send(1, 1024, 42, 1, 1);
    Node* receive = graph.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
};

std::uint64_t read_index64(const PackedRankData& rank, std::uint64_t offset, std::size_t index) {
    return read_little_endian<std::uint64_t>(std::span(rank.bytes).subspan(offset + index * 8, 8));
}

} // namespace

TEST_F(RankPackingTest, PacksRecordsAppendixAndRoots) {
    const auto packed = RankPacker(layout, 2, custom_fields).pack(graph);
    const auto record_size = layout.get_record_size_bytes();
    EXPECT_EQ(packed.dependency_appendix_offset, 3 * record_size);
    EXPECT_EQ(packed.dependency_count, 3u);
    EXPECT_EQ(packed.root_indices_offset, packed.dependency_appendix_offset + 1);
    EXPECT_EQ(packed.root_count, 1u);
    EXPECT_EQ(packed.bytes.size(), packed.root_indices_offset + 1);
    // Appendix [send=1, receive=2, receive=2], tightly packed into 2-bit indices.
    EXPECT_EQ(packed.bytes[packed.dependency_appendix_offset], std::byte{0x29});
    EXPECT_EQ(packed.bytes.back(), std::byte{0}) << "root index zero or its padding changed";

    // The list positions follow the same finish-then-start appendix order as the old serializer.
    const std::vector<RecordValues> expected{
        {number(0), number(3), number(0), number(1000), number(42), number(0), number(0),
         number(1), number(0), number(1), number(1), number(3), FieldValue{std::int64_t{-2}}},
        {number(1), number(1), number(1), number(1024), number(42), number(1), number(1),
         number(1), number(2), number(0), number(3), number(7), FieldValue{std::int64_t{4}}},
        {number(2), number(2), Wildcard{}, number(1024), Wildcard{}, number(0), number(0),
         number(0), number(3), number(0), number(3), number(0), FieldValue{std::int64_t{0}}}};
    const auto loaded_layout = read_binary_header(write_binary_header(layout)).layout;
    for (std::size_t index = 0; index < expected.size(); ++index)
        EXPECT_EQ(decode_record(loaded_layout, std::span(packed.bytes).subspan(index * record_size, record_size)),
                  expected[index]) << "node " << index;
}

TEST_F(RankPackingTest, LeavesTheGraphAndPackerReusable) {
    RankPacker packer(layout, 2, custom_fields);
    const auto first = packer.pack(graph).bytes;
    EXPECT_EQ(packer.pack(graph).bytes, first) << "the dependency cursor leaked across calls";
    EXPECT_EQ(calc->dependency_count, 0u);
    EXPECT_EQ(send->dependency_count, 1u);
    EXPECT_EQ(receive->dependency_count, 2u);
    EXPECT_EQ(calc->finish_dependents.front(), send);
    EXPECT_EQ(calc->start_dependents.front(), receive);
}

TEST_F(RankPackingTest, UsesTheFileWideIndexWidth) {
    const auto wider = RankPacker(layout, 5, custom_fields).pack(graph);
    EXPECT_EQ(wider.root_indices_offset, wider.dependency_appendix_offset + 2);
    EXPECT_EQ(wider.bytes.size(), wider.root_indices_offset + 1);
    EXPECT_EQ(wider.bytes[wider.dependency_appendix_offset], std::byte{0x41});
    EXPECT_EQ(wider.bytes[wider.dependency_appendix_offset + 1], std::byte{0x08});
    EXPECT_EQ(wider.bytes.back(), std::byte{0});

    const auto widest = RankPacker(layout, 64, custom_fields).pack(graph);
    for (std::size_t index = 0; index < 3; ++index)
        EXPECT_EQ(read_index64(widest, widest.dependency_appendix_offset, index), index == 0 ? 1u : 2u);
    EXPECT_EQ(widest.bytes.size(), widest.root_indices_offset + 8);
    EXPECT_EQ(read_index64(widest, widest.root_indices_offset, 0), 0u);
}

// Roots are the nodes without dependencies, in node order, padded with zero bits.
TEST_F(RankPackingTest, ListsRootsInNodeOrder) {
    Goal multiple_roots;
    for (std::uint32_t index = 0; index < 10; ++index) {
        auto* node = multiple_roots.Calc(1000, 0, 0);
        node->custom_values = calc->custom_values;
        if (index % 2 != 0) {
            auto* predecessor = multiple_roots.GetNodes()[index - 1];
            if (index == 9) multiple_roots.StartDependency(node, predecessor);
            else multiple_roots.Dependency(node, predecessor);
        }
    }
    const auto roots = RankPacker(layout, 4, custom_fields).pack(multiple_roots);
    EXPECT_EQ(roots.root_count, 5u);
    EXPECT_TRUE(std::ranges::equal(std::span(roots.bytes).subspan(roots.root_indices_offset),
                                   std::vector{std::byte{0x20}, std::byte{0x64}, std::byte{0x08}}));
    const auto wide = RankPacker(layout, 64, custom_fields).pack(multiple_roots);
    for (std::size_t index = 0; index < wide.root_count; ++index)
        EXPECT_EQ(read_index64(wide, wide.root_indices_offset, index), index * 2);
}

// Including an unmatched receive.
TEST_F(RankPackingTest, IndependentNodesAreAllRoots) {
    Goal independent;
    independent.Calc(1000, 0, 0)->custom_values = calc->custom_values;
    independent.Recv(Node::any_source, 1024, Node::any_tag, 0, 0)->custom_values = receive->custom_values;
    const auto packed = RankPacker(layout, 1, custom_fields).pack(independent);
    EXPECT_EQ(packed.dependency_count, 0u);
    EXPECT_EQ(packed.root_count, 2u);
    EXPECT_EQ(packed.dependency_appendix_offset, packed.root_indices_offset);
    EXPECT_EQ(packed.bytes.back(), std::byte{0x02});
}

TEST_F(RankPackingTest, EmptyRankHasNoData) {
    const auto packed = RankPacker(layout, 1, custom_fields).pack(Goal{});
    EXPECT_TRUE(packed.bytes.empty());
    EXPECT_EQ(packed.dependency_count, 0u);
    EXPECT_EQ(packed.dependency_appendix_offset, 0u);
    EXPECT_EQ(packed.root_count, 0u);
    EXPECT_EQ(packed.root_indices_offset, 0u);
}

TEST_F(RankPackingTest, RejectsMissingDeclarationsAndBadIndexWidths) {
    EXPECT_THROW((void)RankPacker(layout, 2, {}).pack(graph), std::invalid_argument);
    for (const auto width : {0u, 1u, 65u})
        EXPECT_THROW((void)RankPacker(layout, width, custom_fields).pack(graph), std::invalid_argument) << "width " << width;
}

TEST_F(RankPackingTest, RejectsInconsistentGraphs) {
    RankPacker packer(layout, 2, custom_fields);
    Node foreign{};
    calc->finish_dependents[0] = &foreign;
    EXPECT_THROW((void)packer.pack(graph), std::invalid_argument) << "dependent outside the rank";
    calc->finish_dependents[0] = send;
    calc->start_dependents.clear();
    EXPECT_THROW((void)packer.pack(graph), std::invalid_argument) << "dependency count without a list entry";
    calc->start_dependents = {receive};
    send->finish_dependents.push_back(receive);
    EXPECT_THROW((void)packer.pack(graph), std::invalid_argument) << "list entry without a dependency count";
    send->finish_dependents.pop_back();
    receive->custom_values[0] = number(8);
    EXPECT_THROW((void)packer.pack(graph), std::invalid_argument) << "custom value out of range";
}
