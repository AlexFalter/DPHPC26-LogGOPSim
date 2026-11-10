#include "TestHelpers.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "goal_parsing/GoalReader.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <type_traits>
#include <vector>

using test_helpers::decode_record;
using test_helpers::pack_record;
using test_helpers::TemporaryFile;
using traceschema::OperationType;

static_assert(!std::is_copy_constructible_v<Graph>);
static_assert(!std::is_copy_constructible_v<Goal>);
static_assert(std::is_nothrow_move_constructible_v<Graph>);

TEST(GoalOwnership, NodePointersAndEdgesSurviveGrowthAndMoving) {
    Graph graph;
    auto* root = graph.addNode();
    auto* dependent = graph.addNode();
    graph.addDependency(dependent, root);
    for (int index = 0; index < 1024; ++index) graph.addNode();
    Graph moved(std::move(graph));
    EXPECT_EQ(moved.GetNodes().front(), root);
    EXPECT_EQ(root->finish_dependents.front(), dependent);
    EXPECT_EQ(dependent->node_index, 1u);
    EXPECT_EQ(moved.GetDependencyCount(), 1u);
    Graph assigned;
    assigned.addNode();
    assigned = std::move(moved);
    EXPECT_EQ(assigned.GetNodes()[1], dependent);
    EXPECT_EQ(assigned.GetDependencyCount(), 1u);
    EXPECT_EQ(graph.GetDependencyCount(), 0u);
    EXPECT_EQ(graph.addNode()->node_index, 0u);
}

namespace {

const char* const two_ranks =
    "num_ranks 2\nrank 0 {\n"
    "l1: calc 1000 cpu 2\n"
    "l2: send 8b to 1 tag 42 cpu 2 nic 1\n"
    "l2 requires l1\n}\nrank 1 {\n"
    "l1: recv 8b from -1 tag -1 cpu 1 nic 1\n"
    "l2: calc 2000\n"
    "l2 irequires l1\n}\n";

void expect_first_rank(const Goal& graph) {
    const auto& nodes = graph.GetNodes();
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0]->Type, std::to_underlying(OperationType::Calculation));
    EXPECT_EQ(nodes[0]->Size, 1000u);
    EXPECT_EQ(nodes[0]->Proc, 2u);
    EXPECT_EQ(nodes[0]->dependency_count, 0u);
    EXPECT_EQ(nodes[1]->Type, std::to_underlying(OperationType::Send));
    EXPECT_EQ(nodes[1]->Peer, 1u);
    EXPECT_EQ(nodes[1]->Tag, 42u);
    EXPECT_EQ(nodes[1]->Size, 8u);
    EXPECT_EQ(nodes[1]->Nic, 1u);
    EXPECT_EQ(nodes[1]->Proc, 2u);
    EXPECT_EQ(nodes[1]->dependency_count, 1u);
    ASSERT_EQ(nodes[0]->finish_dependents.size(), 1u);
    EXPECT_EQ(nodes[0]->finish_dependents[0], nodes[1]) << "requires was not resolved";
}

void expect_second_rank(const Goal& graph) {
    const auto& nodes = graph.GetNodes();
    ASSERT_EQ(nodes.size(), 2u);
    EXPECT_EQ(nodes[0]->Type, std::to_underlying(OperationType::Receive));
    EXPECT_EQ(nodes[0]->Peer, Node::any_source);
    EXPECT_EQ(nodes[0]->Tag, Node::any_tag);
    EXPECT_EQ(nodes[0]->Proc, 1u);
    EXPECT_EQ(nodes[1]->Type, std::to_underlying(OperationType::Calculation));
    EXPECT_EQ(nodes[1]->Size, 2000u);
    EXPECT_EQ(nodes[1]->Proc, 0u);
    EXPECT_EQ(nodes[1]->Nic, 0u);
    EXPECT_EQ(nodes[1]->dependency_count, 1u);
    ASSERT_EQ(nodes[0]->start_dependents.size(), 1u);
    EXPECT_EQ(nodes[0]->start_dependents[0], nodes[1]) << "irequires was not resolved";
}

void read_all(const std::string& path, const std::vector<traceschema::FieldValueInfo>& custom_fields = {}) {
    GoalReader reader(path, custom_fields);
    while (reader.read_rank()) {}
}

std::vector<traceschema::FieldValueInfo> gpu_and_priority() {
    std::istringstream declarations(
        "[[fields]]\nname = 'gpu_id'\ntype = 'unsigned'\n"
        "[[fields]]\nname = 'priority'\ntype = 'signed'\n");
    return traceschema::read_custom_fields(declarations);
}

} // namespace

// Interleaving detects shared cursors, buffers or label tables.
TEST(GoalReader, ReadersAreIndependent) {
    const TemporaryFile input(two_ranks);
    GoalReader first(input.path()), second(input.path());
    EXPECT_EQ(first.get_num_ranks(), 0u) << "rank count available before parsing";
    expect_first_rank(*first.read_rank());
    expect_first_rank(*second.read_rank());
    EXPECT_EQ(first.get_num_ranks(), 2u);
    EXPECT_EQ(second.get_num_ranks(), 2u);
    expect_second_rank(*first.read_rank());
    expect_second_rank(*second.read_rank());
    EXPECT_FALSE(first.read_rank());
    EXPECT_FALSE(first.read_rank());
    EXPECT_FALSE(second.read_rank());
}

// A new pass starts at the beginning without resetting global state, also after errors.
TEST(GoalReader, ReopeningStartsAtTheBeginning) {
    const TemporaryFile input(two_ranks);
    { GoalReader consumed(input.path()); while (consumed.read_rank()) {} }
    GoalReader reopened(input.path());
    expect_first_rank(*reopened.read_rank());
    expect_second_rank(*reopened.read_rank());
    EXPECT_FALSE(reopened.read_rank());

    const TemporaryFile invalid("num_ranks 1\nrank 0 {\nl1: send\n}\n");
    EXPECT_THROW(read_all(invalid.path()), std::invalid_argument);
    GoalReader after_error(input.path());
    expect_first_rank(*after_error.read_rank());
}

TEST(GoalReader, ResolvesLongLabelsAcrossBufferRefills) {
    const std::string label(20000, 'a');
    std::string text = "num_ranks 1\n/*" + std::string(30000, 'x') + "*/\nrank 0 {\n" + label + ": calc 1\n";
    for (int index = 1; index <= 500; ++index)
        text += "l" + std::to_string(index) + ": calc " + std::to_string(index) + "\n";
    text += "l500 requires " + label + "\n}"; // No trailing newline.
    const TemporaryFile input(text);
    GoalReader reader(input.path());
    const auto graph = reader.read_rank();
    ASSERT_EQ(graph->GetNodes().size(), 501u);
    EXPECT_EQ(graph->GetNodes().front()->finish_dependents[0], graph->GetNodes().back());
    EXPECT_FALSE(reader.read_rank());
}

TEST(GoalReader, ReadsEmptyAndIndentedRanks) {
    const TemporaryFile empty("num_ranks 1\nrank 0 {\n}\n");
    GoalReader empty_reader(empty.path());
    EXPECT_TRUE(empty_reader.read_rank()->GetNodes().empty());
    EXPECT_FALSE(empty_reader.read_rank());

    const TemporaryFile indented("  num_ranks 1\n  rank 0 {\n    l1: calc 1\n    l2: calc 2\n    l2 requires l1\n  }\n");
    GoalReader indented_reader(indented.path());
    const auto graph = indented_reader.read_rank();
    ASSERT_EQ(graph->GetNodes().size(), 2u);
    EXPECT_EQ(graph->GetNodes()[1]->dependency_count, 1u);
}

class MalformedGoal : public testing::TestWithParam<const char*> {};

TEST_P(MalformedGoal, IsRejected) {
    const TemporaryFile input(GetParam());
    EXPECT_THROW(read_all(input.path()), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(GoalReader, MalformedGoal, testing::Values(
    "num_ranks 1\nrank 0 {\nl1: calc 1\n",                        // missing closing brace
    "num_ranks 1\nrank 0 {\nl1: send\n}\n",                       // syntax error
    "num_ranks 1\nrank 0 {\nl1 requires missing\n}\n",            // unknown label
    "num_ranks 2\nrank 1 {\n}\n",                                 // rank order
    "num_ranks 1\nrank 0 {\nl1: calc 1\nl1: calc 2\n}\n",         // duplicate label
    "num_ranks 1\nrank 0 {\nl1: calc 18446744073709551616\n}\n")); // value beyond 64 bits

TEST(GoalReader, ReadsCustomAttributesInDeclarationOrder) {
    const auto custom_fields = gpu_and_priority();
    const TemporaryFile input(
        "num_ranks 2\nrank 0 {\n"
        "a: calc 1 priority -2 cpu 1 gpu_id 3\n"
        "b: send 8b to 1 gpu_id 7 nic 2 priority 4 tag 42\n"
        "b requires a\n}\nrank 1 {\n"
        "a: recv 8b from 0 priority -1 tag 42 gpu_id 0\n}\n");
    GoalReader reader(input.path(), custom_fields);
    traceschema::TraceAnalysis statistics(custom_fields);
    std::vector<std::vector<traceschema::FieldValue>> rows;
    while (auto graph = reader.read_rank()) {
        for (const auto* node : graph->GetNodes()) {
            statistics.observe_custom_values(node->custom_values);
            rows.push_back(node->custom_values);
        }
        statistics.finish_rank(graph->GetNodes().size(), 0, 0);
    }
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(std::get<std::uint64_t>(rows[0][0]), 3u);
    EXPECT_EQ(std::get<std::int64_t>(rows[0][1]), -2);

    // The ranges are inferred across ranks, and the values pack and decode unchanged.
    const auto schema = statistics.create_schema(2);
    EXPECT_EQ(schema.find_field("gpu_id")->bit_width, 3u);
    EXPECT_EQ(schema.find_field("priority")->bit_width, 3u);
    const traceschema::RecordPacker packer(
        traceschema::create_packed_schema({*schema.find_field("gpu_id"), *schema.find_field("priority")}));
    for (const auto& row : rows) {
        const traceschema::RecordValues values(row.begin(), row.end());
        EXPECT_EQ(decode_record(packer.get_layout(), pack_record(packer, values)), values);
    }
}

class InvalidCustomAttribute : public testing::TestWithParam<const char*> {};

TEST_P(InvalidCustomAttribute, IsRejected) {
    const TemporaryFile input("num_ranks 1\nrank 0 {\na: calc 1 " + std::string(GetParam()) + "\n}\n");
    EXPECT_THROW(read_all(input.path(), gpu_and_priority()), std::invalid_argument);
}

INSTANTIATE_TEST_SUITE_P(GoalReader, InvalidCustomAttribute, testing::Values(
    "gpu_id 1",                                // missing attribute
    "gpu_id 1 priority 0 gpu_id 2",            // repeated attribute
    "gpu_id -1 priority 0",                    // negative unsigned value
    "gpu_id 1 priority 9223372036854775808",   // beyond int64
    "gpu_id 18446744073709551616 priority 0",  // beyond uint64
    "gpu_id 1 priority 0 unknown 3"));         // undeclared attribute

TEST(GoalReader, ReadsCustomValuesAt64BitLimits) {
    const auto custom_fields = gpu_and_priority();
    const TemporaryFile input("num_ranks 1\nrank 0 {\n"
                              "a: calc 1 priority -9223372036854775808 gpu_id 18446744073709551615\n"
                              "b: calc 1 gpu_id 0 priority 9223372036854775807\n}\n");
    GoalReader reader(input.path(), custom_fields);
    traceschema::TraceAnalysis statistics(custom_fields);
    for (const auto* node : reader.read_rank()->GetNodes()) statistics.observe_custom_values(node->custom_values);
    const auto schema = statistics.create_schema(1);
    EXPECT_EQ(schema.find_field("priority")->bit_width, 64u);
    EXPECT_EQ(schema.find_field("gpu_id")->bit_width, 64u);
}

TEST(GoalReader, ReadsLongCustomAttributeNames) {
    const auto long_name = "attribute_" + std::string(20000, 'x');
    const TemporaryFile input("num_ranks 1\nrank 0 {\na: calc 1 " + long_name + " 18446744073709551615\n}\n");
    GoalReader reader(input.path(), {traceschema::FieldValueInfo::create_custom(long_name, traceschema::FieldType::UnsignedInteger)});
    EXPECT_EQ(std::get<std::uint64_t>(reader.read_rank()->GetNodes()[0]->custom_values[0]),
              std::numeric_limits<std::uint64_t>::max());
}
