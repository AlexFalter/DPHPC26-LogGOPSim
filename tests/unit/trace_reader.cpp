#include "binary_reading/TraceFileView.hpp"
#include "binary_reading/SelfDescribingTraceReader.hpp"
#include "binary_reading/TraceReader.hpp"
#include "binary_reading/decoding/Decoder.hpp"
#include "binary_reading/decoding/runners/RunnerRegistry.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "goal_parsing/Goal.hpp"
#ifdef LOGGOPSIM_TEST_ATLAHS
#include "simulators/atlahs/AtlahsAdapter.hpp"
#endif
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "simulators/loggopsim/SimulationSchedule.hpp"
#include "trace_format/BinaryIO.hpp"

#include "TestHelpers.hpp"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <fstream>
#include <iterator>
#include <new>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Count ordinary heap allocations only while exercising the steady-state read path.
static bool count_allocations = false;
static std::size_t allocation_count = 0;
void* operator new(std::size_t size) {
    if (count_allocations) ++allocation_count;
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }

using namespace traceschema;

namespace {

// A consumer's own struct, bound field by field.
struct AnalysisNode {
    std::uint16_t bytes;
    std::int8_t priority;
    std::uint16_t gpu;
    std::uint32_t untouched = 123;
};

struct MatchingNode {
    std::uint8_t type;
    std::uint32_t source;
    std::uint8_t tag;
};

// A consumer's own graph node, which also receives the dependency count and lists.
struct AnalysisGraphNode : BaseDeserializedGraphNode {
    std::uint64_t bytes = 0;
    std::int64_t priority = 0;
    std::uint64_t gpu = 0;
};

std::vector<FieldBinding<AnalysisGraphNode>> graph_bindings() {
    return {bind_field(FieldId::Size, &AnalysisGraphNode::bytes),
            bind_field(std::string{"priority"}, &AnalysisGraphNode::priority),
            bind_field(std::string{"gpu_id"}, &AnalysisGraphNode::gpu)};
}

const std::vector<FieldBinding<AnalysisNode>> priority_and_size{
    bind_field(std::string{"priority"}, &AnalysisNode::priority), bind_field(FieldId::Size, &AnalysisNode::bytes)};

// Two ranks: a calc, a send that waits for it and a wildcard receive that waits for the send and
// starts with the calc; then an empty rank. Every standard field and the ATLAHS custom fields.
class TraceReaderTest : public testing::Test {
protected:
    TraceReaderTest() {
        for (const auto& definition : standard_fields) {
            auto input = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 1024});
            if (definition.id == FieldId::DependencyCount) input.value_range = UnsignedRange{0, 2};
            if (definition.id == FieldId::Type) input.value_range = UnsignedRange{1, 3};
            if (definition.id == FieldId::Peer || definition.id == FieldId::Tag) input.has_wildcard = true;
            if (definition.id == FieldId::Peer) input.value_range = UnsignedRange{0, 1};
            if (definition.id == FieldId::Tag) input.value_range = UnsignedRange{0, 42};
            if (definition.id == FieldId::Cpu || definition.id == FieldId::Nic) input.value_range = UnsignedRange{0, 255};
            inputs.push_back(input);
        }
        custom.push_back(FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4}));
        custom.push_back(FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{1000, 1002}));
        for (const auto* name : {"communicator_id", "channel_id", "sequence_id", "message_kind", "chunk_id"})
            custom.push_back(FieldValueInfo::create_custom(name, FieldType::UnsignedInteger, std::nullopt,
                                                           UnsignedRange{0, std::numeric_limits<std::uint64_t>::max()}));
        custom.push_back(FieldValueInfo::create_custom("stream_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1024}));
        custom.push_back(FieldValueInfo::create_custom("peer_gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1002}));
        inputs.insert(inputs.end(), custom.begin(), custom.end());
        layout = build_record_layout(inputs, 2);

        graph.Dependency(send, calc);
        graph.StartDependency(receive, calc);
        graph.Dependency(receive, send);
        calc->custom_values = {std::int64_t{-2}, std::uint64_t{1000}};
        send->custom_values = {std::int64_t{4}, std::uint64_t{1002}};
        receive->custom_values = {std::int64_t{0}, std::uint64_t{1001}};
        for (auto* node : {calc, send, receive}) {
            for (auto value : {std::uint64_t{1} << 48, std::uint64_t{123}, std::uint64_t{1} << 40, std::uint64_t{5},
                               std::numeric_limits<std::uint64_t>::max()})
                node->custom_values.push_back(value);
            node->custom_values.push_back(std::uint64_t{1024});
            node->custom_values.push_back(std::uint64_t{1002});
        }
        original = make_file(layout);
        header = read_binary_header(original);
        rank0 = read_rank_table_entry(std::span(original).subspan(header.rank_table_offset_bytes, rank_table_entry_size_bytes));
        record_size = layout.get_record_size_bytes();
    }

    std::vector<std::byte> make_file(const RecordLayout& schema, std::uint32_t index_bits = 2) {
        std::ostringstream output;
        TraceWriter writer(output, schema, 2, index_bits, custom);
        writer.write_rank(graph);
        writer.write_rank(Goal{});
        writer.finish();
        const auto text = output.str();
        const auto bytes = std::as_bytes(std::span(text));
        return {bytes.begin(), bytes.end()};
    }

    // ATLAHS matches messages by their identity fields, so its traces have zero tags.
    std::vector<std::byte> make_atlahs_file(const RecordLayout& schema) {
        send->Tag = receive->Tag = 0;
        auto bytes = make_file(schema);
        send->Tag = 42;
        receive->Tag = Node::any_tag;
        return bytes;
    }

    // The record bytes of a rank-0 node in a copy of the file.
    std::span<std::byte> record_of(std::vector<std::byte>& file, std::uint32_t node) const {
        return std::span(file).subspan(rank0.data_offset_bytes + node * record_size, record_size);
    }

    void write_field(std::vector<std::byte>& file, std::uint32_t node, FieldIdentity field, std::uint64_t code) const {
        const auto* info = std::visit([&](const auto& id) { return layout.find_field(id); }, field);
        bit_packing::write_bits(record_of(file, node), info->bit_offset, info->bit_width, code);
    }

    std::vector<FieldValueInfo> inputs;
    std::vector<FieldValueInfo> custom;
    RecordLayout layout;
    Goal graph;
    Node* calc = graph.Calc(1000, 255, 255);
    Node* send = graph.Send(1, 1024, 42, 0, 0);
    Node* receive = graph.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    std::vector<std::byte> original;
    BinaryHeader header;
    RankTableEntry rank0;
    std::uint64_t record_size = 0;
};

#ifdef LOGGOPSIM_TEST_ATLAHS
using AtlahsAdapterTest = TraceReaderTest;
#endif
using LogGOPSimAdapterTest = TraceReaderTest;

} // namespace

static_assert(std::same_as<LogGOPSimAdapter::Node, LogGOPSimNode>);
#ifdef LOGGOPSIM_TEST_ATLAHS
static_assert(std::same_as<AtlahsAdapter::Node, AtlahsNode>);
#endif
static_assert(std::derived_from<LogGOPSimNode, BaseDeserializedGraphNode>);

// TemplatedNodeRunner.cpp lists the adapter's node shape by hand; catch a binding change.
TEST_F(LogGOPSimAdapterTest, HasCompiledInTemplatedNodeRunner) {
    test_helpers::ScopedEnvironment runner("LOGGOPSIM_DECODER", "templated_node");
    EXPECT_EQ(LogGOPSimAdapter(original).get_runner_name(), "templated_node");
}

#ifdef LOGGOPSIM_TEST_ATLAHS
TEST_F(AtlahsAdapterTest, HasCompiledInTemplatedNodeRunner) {
    test_helpers::ScopedEnvironment runner("LOGGOPSIM_DECODER", "templated_node");
    auto bytes = make_atlahs_file(layout);
    EXPECT_EQ(AtlahsAdapter(bytes).get_runner_name(), "templated_node");
}

TEST_F(AtlahsAdapterTest, ReadsGraphFieldsAndMessageIdentity) {
    auto bytes = make_atlahs_file(layout);
    AtlahsAdapter atlahs(bytes);
    const auto roots = atlahs.read_root_nodes(0);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].Size, 1000u);
    EXPECT_EQ(roots[0].start_time, 0u);
    EXPECT_EQ(roots[0].Gpu, 1000u);
    EXPECT_EQ(roots[0].PeerGpu, 1002u);
    EXPECT_EQ(roots[0].Stream, 1024u);
    EXPECT_EQ(roots[0].Nic, 255u);
    EXPECT_TRUE(std::ranges::equal(roots[0].finish_dependents, std::array{std::uint32_t{1}}));

    const auto received = atlahs.read_node(0, 2);
    EXPECT_EQ(received.Peer, Node::any_source);
    EXPECT_EQ(received.Tag, 0u);
    EXPECT_EQ(received.dependency_count, 2u);
    EXPECT_TRUE(received.finish_dependents.empty());
    EXPECT_TRUE(received.start_dependents.empty());
    EXPECT_EQ(received.start_time, 0u);
    EXPECT_EQ(received.Gpu, 1001u);
    EXPECT_EQ(received.Communicator, std::uint64_t{1} << 48);
    EXPECT_EQ(received.Channel, 123u);
    EXPECT_EQ(received.Sequence, std::uint64_t{1} << 40);
    EXPECT_EQ(received.MessageKind, 5u);
    EXPECT_EQ(received.Chunk, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(atlahs.get_resource_count(FieldId::Cpu), 256u);
    EXPECT_EQ(atlahs.get_resource_count(std::string{"gpu_id"}), 1003u);
}

TEST_F(AtlahsAdapterTest, RequiresEveryMessageIdentityField) {
    for (const auto* name : {"gpu_id", "peer_gpu_id", "stream_id", "communicator_id", "channel_id", "sequence_id",
                             "message_kind", "chunk_id"}) {
        auto missing_layout = layout;
        std::erase_if(missing_layout.fields, [&](const auto& field) { return field.field == FieldIdentity{std::string{name}}; });
        auto bytes = make_atlahs_file(missing_layout);
        EXPECT_THROW(AtlahsAdapter{bytes}, std::invalid_argument) << name;
    }
}

TEST_F(AtlahsAdapterTest, RejectsTagsUnknownKindsAndOversizedMessages) {
    AtlahsAdapter with_tags(original);
    EXPECT_THROW((void)with_tags.read_node(0, 1), std::invalid_argument) << "a message with a tag";

    send->custom_values[5] = std::uint64_t{6}; // message_kind
    auto bad_kind = make_atlahs_file(layout);
    AtlahsAdapter bad_kind_reader(bad_kind);
    EXPECT_THROW((void)bad_kind_reader.read_node(0, 1), std::invalid_argument) << "an unknown message kind";
    send->custom_values[5] = std::uint64_t{5};

    auto wide_inputs = inputs;
    const auto large_size = std::uint64_t{std::numeric_limits<int>::max()} + 1;
    std::ranges::find(wide_inputs, FieldIdentity{FieldId::Size}, &FieldValueInfo::field)->value_range = UnsignedRange{0, large_size};
    const auto wide_layout = build_record_layout(wide_inputs, 2);
    send->Size = large_size;
    auto oversized = make_atlahs_file(wide_layout);
    AtlahsAdapter oversized_reader(oversized);
    EXPECT_THROW((void)oversized_reader.read_node(0, 1), std::invalid_argument) << "a message beyond int";

    // The message-size limit does not apply to calculations.
    send->Size = 1024;
    calc->Size = large_size;
    auto long_calculation = make_atlahs_file(wide_layout);
    EXPECT_EQ(AtlahsAdapter(long_calculation).read_node(0, 0).Size, large_size);
}

#endif // LOGGOPSIM_TEST_ATLAHS

TEST_F(LogGOPSimAdapterTest, RejectsTracesWithMessageIdentities) {
    auto bytes = make_atlahs_file(layout);
    LogGOPSimAdapter reader(bytes);
    EXPECT_THROW((void)reader.read_node(0, 1), std::invalid_argument);
}

TEST_F(TraceReaderTest, BindsMembersOfAnyStruct) {
    TraceReader reader(original, {bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu),
                                  bind_field(FieldId::Size, &AnalysisNode::bytes),
                                  bind_field(std::string{"priority"}, &AnalysisNode::priority)});
    const auto first = reader.read_node<AnalysisNode>(0, 0);
    EXPECT_EQ(first.bytes, 1000u);
    EXPECT_EQ(first.priority, -2);
    EXPECT_EQ(first.gpu, 1000u);
    EXPECT_EQ(first.untouched, 123u);
    const auto second = reader.read_node<AnalysisNode>(0, 1);
    EXPECT_EQ(second.bytes, 1024u);
    EXPECT_EQ(second.priority, 4);
    EXPECT_EQ(second.gpu, 1002u);
    EXPECT_EQ(second.untouched, 123u);
    EXPECT_EQ(first.priority, -2) << "the first snapshot changed";
    EXPECT_THROW((void)reader.read_node<AnalysisNode>(1, 0), std::invalid_argument) << "a node of the empty rank";
    EXPECT_THROW((void)reader.read_node<MatchingNode>(0, 0), std::invalid_argument) << "a struct without bindings";
}

TEST_F(TraceReaderTest, UsesEachBindingsWildcardValue) {
    TraceReader reader(original, {bind_field(FieldId::Tag, &MatchingNode::tag, std::uint8_t{255}),
                                  bind_field(FieldId::Peer, &MatchingNode::source, Node::any_source)});
    EXPECT_EQ(reader.read_node<MatchingNode>(0, 1).source, 1u);
    EXPECT_EQ(reader.read_node<MatchingNode>(0, 1).tag, 42u);
    EXPECT_EQ(reader.read_node<MatchingNode>(0, 2).source, Node::any_source);
    EXPECT_EQ(reader.read_node<MatchingNode>(0, 2).tag, 255u);
    // A wildcard value outside the field's range is fine.
    TraceReader outside_range(original, {bind_field(std::string{"priority"}, &AnalysisNode::priority, std::int8_t{-3})});
    EXPECT_EQ(outside_range.read_node<AnalysisNode>(0, 0).priority, -2);
}

TEST_F(TraceReaderTest, RejectsInvalidBindings) {
    const auto rejects = [&](std::initializer_list<FieldBinding<AnalysisNode>> bindings, const char* reason) {
        EXPECT_THROW((TraceReader{original, bindings}), std::invalid_argument) << reason;
    };
    EXPECT_THROW((TraceReader{original, {bind_field(FieldId::Peer, &MatchingNode::source)}}), std::invalid_argument)
        << "a wildcard field without a wildcard value";
    EXPECT_THROW((TraceReader{original, {bind_field(FieldId::Size, &MatchingNode::tag)}}), std::invalid_argument)
        << "a member too narrow for the field";
    rejects({bind_field(std::string{"missing"}, &AnalysisNode::bytes)}, "a missing field");
    rejects({bind_field(std::string{"priority"}, &AnalysisNode::bytes)}, "a signed field into an unsigned member");
    rejects({bind_field(FieldId::Size, &AnalysisNode::bytes), bind_field(FieldId::Size, &AnalysisNode::gpu)}, "a field bound twice");
    rejects({bind_field(std::string{"priority"}, &AnalysisNode::priority, std::int8_t{0})}, "a wildcard value inside the range");
    rejects({bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu, std::uint16_t{1001})}, "a wildcard value inside the range");
    EXPECT_THROW((void)bind_field(FieldId::Size, static_cast<std::uint16_t AnalysisNode::*>(nullptr)), std::invalid_argument);
}

TEST_F(TraceReaderTest, ReadsSelectedFieldsInBindingOrder) {
    SelfDescribingTraceReader reader(original, priority_and_size);
    ASSERT_EQ(reader.get_selected_fields().size(), 2u);
    EXPECT_EQ(reader.get_selected_fields()[0].field, FieldIdentity{std::string{"priority"}});
    EXPECT_EQ(reader.get_selected_fields()[1].field, FieldIdentity{FieldId::Size});
    EXPECT_EQ(reader.read_node<AnalysisNode>(0, 0).priority, -2);
    EXPECT_EQ(reader.read_node<AnalysisNode>(0, 0).bytes, 1000u);
    EXPECT_EQ(reader.read_node<AnalysisNode>(0, 1).priority, 4);
    EXPECT_EQ(reader.read_node<AnalysisNode>(0, 1).bytes, 1024u);
    EXPECT_THROW((void)reader.read_node<AnalysisNode>(1, 0), std::invalid_argument);
}

TEST_F(TraceReaderTest, NeedsOnlyTheFieldsItReads) {
    auto thin_layout = layout;
    std::erase_if(thin_layout.fields, [](const auto& field) {
        return field.field != FieldIdentity{FieldId::Size} && field.field != FieldIdentity{std::string{"priority"}};
    });
    auto thin = make_file(thin_layout);
    TraceReader bound(thin, {bind_field(FieldId::Size, &AnalysisNode::bytes),
                             bind_field(std::string{"priority"}, &AnalysisNode::priority)});
    EXPECT_EQ(bound.read_node<AnalysisNode>(0, 0).bytes, 1000u);
    EXPECT_EQ(bound.read_node<AnalysisNode>(0, 0).priority, -2);
    const auto full = TraceReader(original, priority_and_size).read_node<AnalysisNode>(0, 0);
    EXPECT_EQ(bound.read_node<AnalysisNode>(0, 0).bytes, full.bytes);
    EXPECT_EQ(bound.read_node<AnalysisNode>(0, 0).priority, full.priority);
    EXPECT_THROW(LogGOPSimAdapter{thin}, std::invalid_argument) << "LogGOPSim needs its own fields";
    EXPECT_THROW((TraceReader{thin, {bind_field(FieldId::Size, &AnalysisGraphNode::bytes)}}), std::invalid_argument)
        << "graph nodes need the dependency fields";
}

TEST_F(TraceReaderTest, AllowsMetadataWithoutSelectedFields) {
    SelfDescribingTraceReader reader(original, std::vector<FieldBinding<AnalysisNode>>{});
    EXPECT_TRUE(reader.get_selected_fields().empty());
    EXPECT_EQ(reader.get_node_count(0), 3u);
    EXPECT_EQ(reader.read_node<AnalysisNode>(0, 0).untouched, 123u);
    EXPECT_THROW((void)reader.read_node<MatchingNode>(0, 0), std::invalid_argument);
}

TEST_F(TraceReaderTest, ReadsGraphNodesAndRunsTheValidator) {
    std::size_t validated = 0;
    TraceReader reader(original, graph_bindings(), [&](const AnalysisGraphNode& node) {
        ++validated;
        if (node.priority > 0) throw std::invalid_argument("Consumer rejects positive priority");
    });
    const auto roots = reader.read_root_nodes<AnalysisGraphNode>(0);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].node_index, 0u);
    EXPECT_EQ(roots[0].dependency_count, 0u);
    EXPECT_EQ(roots[0].bytes, 1000u);
    EXPECT_EQ(roots[0].priority, -2);
    EXPECT_EQ(roots[0].gpu, 1000u);
    EXPECT_TRUE(std::ranges::equal(roots[0].finish_dependents, std::array{std::uint32_t{1}}));
    EXPECT_TRUE(std::ranges::equal(roots[0].start_dependents, std::array{std::uint32_t{2}}));
    EXPECT_EQ(validated, 1u);
    EXPECT_TRUE(reader.read_root_nodes<AnalysisGraphNode>(1).empty());
    EXPECT_EQ(validated, 1u) << "the empty rank called the validator";

    const auto received = reader.read_node<AnalysisGraphNode>(0, 2);
    EXPECT_EQ(received.node_index, 2u);
    EXPECT_EQ(received.dependency_count, 2u);
    EXPECT_TRUE(received.finish_dependents.empty());
    EXPECT_TRUE(received.start_dependents.empty());
    EXPECT_EQ(received.priority, 0);
    EXPECT_EQ(received.gpu, 1001u);
    EXPECT_THROW((void)reader.read_node<AnalysisGraphNode>(0, 1), std::invalid_argument) << "the validator's rejection";
}

TEST_F(TraceReaderTest, DecrementsAConfiguredCounter) {
    auto bytes = original;
    struct CounterNode { std::uint32_t count; };
    SelfDescribingTraceReader reader(bytes, {bind_field(FieldId::DependencyCount, &CounterNode::count)});
    EXPECT_THROW((void)reader.decrement_dependency_count(0, 2), std::invalid_argument) << "no counter configured";
    EXPECT_THROW(reader.set_dependency_counter(std::string{"missing"}), std::invalid_argument);
    EXPECT_THROW(reader.set_dependency_counter(std::string{"priority"}), std::invalid_argument) << "signed";
    EXPECT_THROW(reader.set_dependency_counter(std::string{"gpu_id"}), std::invalid_argument) << "nonzero base";
    EXPECT_THROW(reader.set_dependency_counter(FieldId::Peer), std::invalid_argument) << "wildcard";
    reader.set_dependency_counter(FieldId::DependencyCount);
    EXPECT_EQ(reader.read_node<CounterNode>(0, 2).count, 2u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 2), 1u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 2), 0u);
    EXPECT_EQ(reader.read_node<CounterNode>(0, 2).count, 0u);
    EXPECT_THROW((void)reader.decrement_dependency_count(0, 2), std::invalid_argument) << "already zero";
    TraceReader custom_values(bytes, graph_bindings());
    EXPECT_EQ(custom_values.read_node<AnalysisGraphNode>(0, 2).priority, 0) << "the update changed another field";
    EXPECT_EQ(custom_values.read_node<AnalysisGraphNode>(0, 2).gpu, 1001u);
}

TEST_F(TraceReaderTest, CountsResources) {
    TraceReader reader(original, std::vector<FieldBinding<AnalysisNode>>{});
    EXPECT_EQ(reader.get_resource_count(FieldId::Cpu), 256u) << "the count wrapped at 255";
    EXPECT_EQ(reader.get_resource_count(FieldId::Nic), 256u);
    EXPECT_THROW((void)reader.get_resource_count(std::string{"missing"}), std::invalid_argument);
    EXPECT_THROW((void)reader.get_resource_count(std::string{"priority"}), std::invalid_argument);

    auto large_layout = layout;
    for (auto& field : large_layout.fields)
        if (field.field == FieldIdentity{FieldId::Cpu}) {
            field.bit_width = 64;
            field.offset_encoding = OffsetEncoding{std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()};
        }
    auto large = make_file(create_packed_schema(std::move(large_layout.fields)));
    EXPECT_THROW((void)TraceReader(large, std::vector<FieldBinding<AnalysisNode>>{}).get_resource_count(FieldId::Cpu), std::invalid_argument)
        << "a count beyond 64 bits";
}

TEST_F(LogGOPSimAdapterTest, ReadsMetadataRootsAndNodes) {
    auto bytes = original;
    LogGOPSimAdapter reader(bytes);
    EXPECT_EQ(reader.get_rank_count(), 2u);
    EXPECT_EQ(reader.get_node_count(0), 3u);
    EXPECT_EQ(reader.get_node_count(1), 0u);
    EXPECT_EQ(reader.get_root_count(0), 1u);
    EXPECT_EQ(reader.read_root(0, 0), 0u);
    EXPECT_TRUE(reader.read_root_nodes(1).empty());
    const auto roots = reader.read_root_nodes(0);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots.front().Size, 1000u);
    EXPECT_EQ(reader.get_resource_count(FieldId::Cpu), 256u);
    EXPECT_EQ(reader.get_resource_count(FieldId::Nic), 256u);

    const auto node = reader.read_node(0, 0);
    EXPECT_EQ(node.Size, 1000u);
    EXPECT_EQ(node.Proc, 255u);
    EXPECT_EQ(node.Nic, 255u);
    EXPECT_TRUE(std::ranges::equal(node.finish_dependents, std::array{std::uint32_t{1}}));
    EXPECT_TRUE(std::ranges::equal(node.start_dependents, std::array{std::uint32_t{2}}));
    EXPECT_EQ(reader.read_node(0, 2).Peer, Node::any_source);
    EXPECT_EQ(reader.read_node(0, 2).Tag, Node::any_tag);
    EXPECT_THROW((void)reader.read_node(1, 0), std::invalid_argument);
    EXPECT_THROW((void)reader.read_node(0, 3), std::invalid_argument);
    EXPECT_THROW((void)reader.read_root(0, 1), std::invalid_argument);

    SimulatorAdapter<AnalysisGraphNode> custom_reader(bytes, graph_bindings());
    EXPECT_EQ(custom_reader.read_node(0, 0).priority, -2) << "signedness";
    EXPECT_EQ(custom_reader.read_node(0, 1).priority, 4);
    EXPECT_EQ(custom_reader.read_node(0, 0).gpu, 1000u) << "value base";
    EXPECT_EQ(custom_reader.read_node(0, 1).gpu, 1002u);
}

TEST_F(LogGOPSimAdapterTest, ReadsNodesWithoutAllocating) {
    LogGOPSimAdapter reader(original);
    allocation_count = 0;
    count_allocations = true;
    std::uint64_t dependency_sum = 0;
    for (int i = 0; i < 100; ++i) {
        const auto node = reader.read_node(0, 0);
        for (auto target : node.finish_dependents) dependency_sum += target;
        for (auto target : node.start_dependents) dependency_sum += target;
    }
    count_allocations = false;
    EXPECT_EQ(allocation_count, 0u);
    EXPECT_EQ(dependency_sum, 300u);
}

// Dependency lists are views over the mapped bytes, not over the reader.
TEST_F(TraceReaderTest, DependencyListsOutliveTheReader) {
    const auto copied = [&] {
        auto bytes = original;
        TraceReader reader(bytes, graph_bindings());
        const auto node = reader.read_node<AnalysisGraphNode>(0, 1);
        return std::vector<std::uint32_t>(node.finish_dependents.begin(), node.finish_dependents.end());
    }();
    EXPECT_EQ(copied, std::vector<std::uint32_t>{2});
    const auto node = [&] {
        TraceReader reader(original, graph_bindings());
        return reader.read_node<AnalysisGraphNode>(0, 1);
    }();
    EXPECT_EQ(node.bytes, 1024u);
    ASSERT_EQ(node.finish_dependents.size(), 1u);
    EXPECT_EQ(node.finish_dependents[0], 2u);
}

TEST_F(LogGOPSimAdapterTest, FollowsTheLayoutsFieldOrder) {
    auto reordered = layout;
    std::ranges::reverse(reordered.fields);
    auto bytes = make_file(reordered);
    TraceReader custom_reader(bytes, graph_bindings());
    EXPECT_EQ(custom_reader.read_node<AnalysisGraphNode>(0, 0).gpu, 1000u);
    EXPECT_EQ(custom_reader.read_node<AnalysisGraphNode>(0, 1).priority, 4);
    LogGOPSimAdapter reader(bytes);
    EXPECT_EQ(reader.read_node(0, 0).Size, 1000u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 2), 1u);
}

// Starting and finishing a node read only its lists; they must equal those of the full read.
TEST_F(LogGOPSimAdapterTest, ReadsDependencyListsWithoutTheNode) {
    auto bytes = original;
    LogGOPSimAdapter reader(bytes);
    for (std::uint32_t index = 0; index < reader.get_node_count(0); ++index) {
        const auto node = reader.read_node(0, index);
        const auto finish = reader.read_dependents(0, index, DependencyEvent::Finish);
        const auto start = reader.read_dependents(0, index, DependencyEvent::Start);
        EXPECT_TRUE(std::ranges::equal(finish, node.finish_dependents)) << "node " << index;
        EXPECT_TRUE(std::ranges::equal(start, node.start_dependents)) << "node " << index;
    }
    EXPECT_THROW((void)reader.read_dependents(0, 3, DependencyEvent::Start), std::invalid_argument) << "outside the rank";
    EXPECT_THROW((void)reader.read_dependents(1, 0, DependencyEvent::Finish), std::invalid_argument) << "a node of the empty rank";
}

TEST_F(LogGOPSimAdapterTest, ChecksOnlyTheRequestedDependencyList) {
    auto bytes = original;
    write_field(bytes, 0, FieldId::StartDependentsIndex, rank0.dependency_count + 1);
    LogGOPSimAdapter reader(bytes);
    EXPECT_TRUE(std::ranges::equal(reader.read_dependents(0, 0, DependencyEvent::Finish),
                                   std::array{std::uint32_t{1}}));
    EXPECT_THROW((void)reader.read_dependents(0, 0, DependencyEvent::Start), std::invalid_argument);
    EXPECT_THROW((void)reader.read_node(0, 0), std::invalid_argument);
    write_field(bytes, 0, FieldId::StartDependentsCount, 0);
    EXPECT_THROW((void)reader.read_dependents(0, 0, DependencyEvent::Start), std::invalid_argument)
        << "an empty slice must still have a valid position";
}

TEST_F(LogGOPSimAdapterTest, ValidatesConstantEmptyDependencySlices) {
    auto bytes = original;
    auto constant_layout = layout;
    for (auto& field : constant_layout.fields) {
        if (field.field == FieldIdentity{FieldId::StartDependentsCount} ||
            field.field == FieldIdentity{FieldId::StartDependentsIndex}) {
            field.bit_width = 0;
            const auto base = field.field == FieldIdentity{FieldId::StartDependentsIndex}
                ? std::uint64_t{rank0.dependency_count} + 1 : 0;
            field.offset_encoding = OffsetEncoding{base, 0};
        }
    }
    const auto updated_header = write_binary_header(constant_layout, header.rank_count, header.node_index_bits);
    ASSERT_EQ(updated_header.size(), header.header_size_bytes);
    std::ranges::copy(updated_header, bytes.begin());
    LogGOPSimAdapter reader(bytes);
    EXPECT_THROW((void)reader.read_dependents(0, 0, DependencyEvent::Start), std::invalid_argument);
    EXPECT_THROW((void)reader.read_dependents(0, 3, DependencyEvent::Finish), std::invalid_argument);
    EXPECT_TRUE(std::ranges::equal(reader.read_dependents(0, 0, DependencyEvent::Finish),
                                   std::array{std::uint32_t{1}}));
}

TEST_F(LogGOPSimAdapterTest, ReadsSpillingDependencyFieldsAndRejectsInvalidCodes) {
    auto wide_inputs = inputs;
    for (auto& field : wide_inputs)
        if (field.field == FieldIdentity{FieldId::FinishDependentsIndex} ||
            field.field == FieldIdentity{FieldId::StartDependentsIndex}) field.width = 64;
    const auto wide = build_record_layout(wide_inputs, 2);
    const auto& position = *wide.find_field(FieldId::FinishDependentsIndex);
    ASSERT_NE(position.bit_offset % 8, 0u);
    auto bytes = make_file(wide);
    LogGOPSimAdapter reader(bytes);
    for (std::uint32_t index = 0; index < reader.get_node_count(0); ++index) {
        const auto node = reader.read_node(0, index);
        EXPECT_TRUE(std::ranges::equal(reader.read_dependents(0, index, DependencyEvent::Finish), node.finish_dependents));
        EXPECT_TRUE(std::ranges::equal(reader.read_dependents(0, index, DependencyEvent::Start), node.start_dependents));
    }
    const auto wide_header = read_binary_header(bytes);
    const auto rank = read_rank_table_entry(std::span(bytes).subspan(wide_header.rank_table_offset_bytes, rank_table_entry_size_bytes));
    bit_packing::write_bits(std::span(bytes).subspan(rank.data_offset_bytes, wide.get_record_size_bytes()),
                            position.bit_offset, position.bit_width, UINT64_MAX);
    EXPECT_THROW((void)reader.read_dependents(0, 0, DependencyEvent::Finish), std::invalid_argument);
}

TEST_F(LogGOPSimAdapterTest, ReadsWideIndexArrays) {
    auto bytes = make_file(layout, 64);
    LogGOPSimAdapter reader(bytes);
    EXPECT_EQ(reader.read_root(0, 0), 0u);
    EXPECT_TRUE(std::ranges::equal(reader.read_node(0, 0).finish_dependents, std::array{std::uint32_t{1}}));
}

TEST_F(LogGOPSimAdapterTest, ReadsStandardOnlyLayouts) {
    auto standard_layout = layout;
    std::erase_if(standard_layout.fields, [](const auto& field) { return std::holds_alternative<std::string>(field.field); });
    auto bytes = make_file(standard_layout);
    EXPECT_EQ(LogGOPSimAdapter(bytes).read_node(0, 0).Size, 1000u);
    EXPECT_THROW((TraceReader{bytes, graph_bindings()}), std::invalid_argument) << "the custom fields are missing";
}

// Decrements change the counter in the mapped file and nothing else; earlier snapshots keep
// their value.
TEST_F(LogGOPSimAdapterTest, CounterUpdatesTouchOnlyTheCounter) {
    auto bytes = original;
    LogGOPSimAdapter reader(bytes);
    SimulatorAdapter<AnalysisGraphNode> custom_reader(bytes, graph_bindings());
    const auto snapshot = reader.read_node(0, 2);
    EXPECT_EQ(snapshot.dependency_count, 2u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 2), 1u);
    EXPECT_EQ(reader.read_node(0, 2).dependency_count, 1u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 2), 0u);
    EXPECT_EQ(reader.read_node(0, 2).dependency_count, 0u);
    EXPECT_EQ(snapshot.dependency_count, 2u);
    EXPECT_EQ(custom_reader.read_node(0, 2).priority, 0);
    EXPECT_EQ(custom_reader.read_node(0, 2).gpu, 1001u);
    EXPECT_THROW(reader.decrement_dependency_count(0, 2), std::invalid_argument);

    // The counter is the receive record's lowest two bits.
    const auto changed_byte = rank0.data_offset_bytes + 2 * record_size;
    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (index != changed_byte) ASSERT_EQ(bytes[index], original[index]) << "byte " << index;
    EXPECT_EQ(bytes[changed_byte] & std::byte{0xfc}, original[changed_byte] & std::byte{0xfc});
}

static_assert(std::forward_iterator<DependencyListView::Iterator>);
static_assert(std::ranges::forward_range<const DependencyListView>);

TEST(DependencyListView, SupportsIndependentIteratorCopiesAndStandardAlgorithms) {
    std::array<std::byte, 1> bytes{};
    for (std::uint32_t index = 0; index < 3; ++index)
        bit_packing::write_bits(bytes, index * 2, 2, index + 1);
    const auto view = DependencyListView::from_bit_packed(bytes, 0, 3, 2);
    auto iterator = view.begin();
    const auto copy = iterator;
    EXPECT_EQ(*iterator++, 1u);
    EXPECT_EQ(*iterator, 2u);
    EXPECT_EQ(*copy, 1u) << "advancing one iterator must not advance its copy";
    EXPECT_EQ(std::distance(view.begin(), view.end()), 3);
    std::array<std::uint32_t, 3> decoded{};
    std::copy(view.begin(), view.end(), decoded.begin());
    EXPECT_EQ(decoded, (std::array<std::uint32_t, 3>{1, 2, 3}));
    EXPECT_THROW((void)view[3], std::out_of_range);
    const DependencyListView empty;
    EXPECT_EQ(empty.begin(), empty.end());
}

TEST(DependencyListView, ReadsPackedIndicesOfEveryWidth) {
    for (std::uint32_t bits = 1; bits <= 64; ++bits) {
        std::vector<std::byte> indices(bit_packing::calculate_index_array_size_bytes(7, bits));
        for (std::uint32_t i = 0; i < 7; ++i) bit_packing::write_bits(indices, std::uint64_t{i} * bits, bits, i % 2);
        const auto slice = DependencyListView::from_bit_packed(indices, 1, 5, bits);
        EXPECT_TRUE(std::ranges::equal(slice, std::array{1u, 0u, 1u, 0u, 1u})) << bits << " bits";
        EXPECT_NO_THROW(slice.validate(2)) << bits << " bits";
        EXPECT_THROW(slice.validate(1), std::invalid_argument) << "an index beyond the nodes";
        EXPECT_TRUE(DependencyListView::from_bit_packed(indices, 7, 0, bits).empty()) << "an empty tail, " << bits << " bits";
        EXPECT_THROW((void)DependencyListView::from_bit_packed(indices, UINT32_MAX, 1, bits), std::invalid_argument) << "a slice beyond the array";
    }
    std::array<std::byte, 8> overflowing{};
    bit_packing::write_bits(overflowing, 0, 64, std::uint64_t{1} << 32);
    EXPECT_THROW(DependencyListView::from_bit_packed(overflowing, 0, 1, 64).validate(3), std::invalid_argument)
        << "a 64-bit index must not wrap into a valid node";
}

TEST(DependencyListView, ReadsUnalignedNativeIndices) {
    std::array<std::byte, 9> bytes{};
    const std::array<std::uint32_t, 2> indices{1, 2};
    std::memcpy(bytes.data() + 1, indices.data(), 8);
    const auto view = DependencyListView::from_legacy_u32(std::span(bytes).subspan(1));
    EXPECT_TRUE(std::ranges::equal(view, indices));
    EXPECT_NO_THROW(view.validate(3));
    EXPECT_THROW(view.validate(2), std::invalid_argument) << "an index beyond the nodes";
    EXPECT_THROW((void)DependencyListView::from_legacy_u32(bytes), std::invalid_argument) << "not a multiple of four bytes";
}

TEST_F(TraceReaderTest, FileViewViewsTheMappedBytes) {
    TraceFileView view(original);
    view.validate_dependency_indices();
    EXPECT_EQ(view.get_rank_count(), 2u);
    EXPECT_EQ(view.get_node_count(0), 3u);
    EXPECT_EQ(view.get_node_count(1), 0u);
    EXPECT_EQ(view.get_root_count(0), 1u);
    EXPECT_EQ(view.read_root(0, 0), 0u);
    const auto record = view.get_node_record_bytes(0, 0);
    EXPECT_EQ(record.size(), record_size);
    EXPECT_EQ(record.data(), original.data() + rank0.data_offset_bytes) << "the record was copied";
    EXPECT_TRUE(std::ranges::equal(view.get_dependents(0, 0, 1), std::array{std::uint32_t{1}}));
    EXPECT_TRUE(std::ranges::equal(view.get_dependents(0, 1, 1), std::array{std::uint32_t{2}}));
    EXPECT_TRUE(view.get_dependents(0, rank0.dependency_count, 0).empty());
    EXPECT_THROW((void)view.get_node_record_bytes(0, 3), std::invalid_argument);
    EXPECT_THROW((void)view.read_root(0, 1), std::invalid_argument);
    EXPECT_THROW((void)view.get_dependents(0, rank0.dependency_count, 1), std::invalid_argument);
}

TEST_F(TraceReaderTest, DecoderWritesSelectedFieldsAndWildcards) {
    std::array<std::uint64_t, 4> values{};
    std::vector<FieldDestination> destinations;
    for (std::uint32_t index = 0; index < values.size(); ++index)
        destinations.push_back({OutputStruct::Node, index * 8, 8, UINT32_MAX});
    const Decoder decoder(layout, {require_field<std::uint8_t>(FieldId::Type), require_field<std::uint32_t>(FieldId::Peer),
                                   require_field<std::uint32_t>(FieldId::Tag),
                                   require_field<std::uint32_t>(FieldId::DependencyCount)}, destinations);
    const auto record = std::span<const std::byte>(original).subspan(rank0.data_offset_bytes + 2 * record_size, record_size);
    std::byte* targets[]{reinterpret_cast<std::byte*>(values.data())};
    decoder.decode_into(record, 0, targets);
    EXPECT_EQ(values, (std::array<std::uint64_t, 4>{std::to_underlying(OperationType::Receive), UINT32_MAX, UINT32_MAX, 2}));
    EXPECT_THROW(decoder.decode_into(record.first(record_size - 1), 0, targets), std::invalid_argument);
}

// Both trace formats and both schedule modes use the same dependency-release and readiness methods.
TEST_F(TraceReaderTest, ScheduleReleasesDependents) {
    for (const auto mode : {ScheduleMode::Eager, ScheduleMode::Lazy}) {
        SCOPED_TRACE(mode == ScheduleMode::Lazy ? "lazy" : "eager");
        auto bytes = original;
        LogGOPSimAdapter reader(bytes);
        RankSchedule schedule(reader, 0, mode);
        std::vector<LogGOPSimNode> ready;
        schedule.GetExecutableNodes_DSN(ready);
        ASSERT_EQ(ready.size(), 1u);
        EXPECT_EQ(ready.front().node_index, 0u);
        schedule.MarkNodeAsStarted(0);
        schedule.GetExecutableNodes_DSN(ready);
        EXPECT_TRUE(ready.empty()) << "the start dependency released the receive early";
        schedule.MarkNodeAsDone(0);
        schedule.GetExecutableNodes_DSN(ready);
        ASSERT_EQ(ready.size(), 1u);
        EXPECT_EQ(ready.front().node_index, 1u) << "finishing the calc releases the send";
        schedule.MarkNodeAsDone(1);
        schedule.GetExecutableNodes_DSN(ready);
        ASSERT_EQ(ready.size(), 1u);
        EXPECT_EQ(ready.front().node_index, 2u) << "both dependencies release the receive";
        EXPECT_EQ(bytes == original, mode == ScheduleMode::Lazy) << "only eager mode writes counters";
    }
}

// Lazy mode keeps counts in memory: the file is read-only and counts start from the stored value.
TEST_F(TraceReaderTest, LazyScheduleKeepsCountsInMemory) {
    auto bytes = original;
    LogGOPSimAdapter reader(bytes);
    EXPECT_EQ(reader.read_dependency_count(0, 2), 2u);
    RankSchedule schedule(reader, 0, ScheduleMode::Lazy);
    RankSchedule::nodelist_t events;
    schedule.GetExecutableNodes(&events);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events.front().type, OP_LOCOP);
    schedule.MarkNodeAsStarted(0);
    schedule.MarkNodeAsDone(0);
    schedule.MarkNodeAsDone(1);
    EXPECT_EQ(reader.read_dependency_count(0, 2), 2u) << "the stored count was changed";
    EXPECT_THROW(schedule.MarkNodeAsDone(1), std::invalid_argument) << "a released node was released again";
    EXPECT_EQ(bytes, original);
}

// Both scheduler interfaces can drain ready nodes without allocating each batch.
TEST_F(LogGOPSimAdapterTest, ReusesReadyBuffersWithoutAllocating) {
    for (const bool decoded_batch : {false, true}) {
        auto bytes = original;
        LogGOPSimAdapter reader(bytes);
        RankSchedule schedule(reader, 0);
        std::vector<LogGOPSimNode> decoded;
        RankSchedule::nodelist_t events;
        decoded.reserve(1);
        events.reserve(1);
        std::uint32_t processed = 0;
        allocation_count = 0;
        count_allocations = true;
        for (;;) {
            if (decoded_batch) {
                schedule.GetExecutableNodes_DSN(decoded);
                if (decoded.empty()) break;
                for (const auto& node : decoded) {
                    schedule.MarkNodeAsStarted(node.node_index);
                    schedule.MarkNodeAsDone(node.node_index);
                    ++processed;
                }
            } else {
                events.clear();
                schedule.GetExecutableNodes(&events);
                if (events.empty()) break;
                for (const auto& node : events) {
                    schedule.MarkNodeAsStarted(node.node_index);
                    schedule.MarkNodeAsDone(node.node_index);
                    ++processed;
                }
            }
        }
        count_allocations = false;
        EXPECT_EQ(processed, 3u);
        EXPECT_EQ(allocation_count, 0u) << (decoded_batch ? "decoded batches" : "scheduler event batches");
    }
}

// Graph setup validates the whole immutable appendix, including unused entries.
// Field-only consumers still avoid inspecting data they never use.
TEST_F(LogGOPSimAdapterTest, ChecksDependencyIndicesOnceWhenOpeningAGraph) {
    auto bytes = original;
    const auto appendix = rank0.data_offset_bytes + rank0.node_count * record_size;
    ASSERT_EQ(rank0.dependency_count, 3u);
    write_little_endian(std::uint32_t{4}, std::span(bytes).subspan(header.rank_table_offset_bytes + 12, 4));
    bytes[appendix] |= std::byte{0xc0}; // fourth 2-bit index = 3, outside this three-node rank
    TraceReader values(bytes, {bind_field(FieldId::Size, &AnalysisNode::bytes)});
    EXPECT_EQ(values.read_node<AnalysisNode>(0, 0).bytes, 1000u);
    TraceFileView raw(bytes);
    EXPECT_THROW((void)raw.get_dependents(0, 3, 1), std::logic_error) << "lists before the check";
    EXPECT_THROW(raw.validate_dependency_indices(), std::invalid_argument);
    EXPECT_THROW((void)LogGOPSimAdapter{bytes}, std::invalid_argument);
}

TEST_F(LogGOPSimAdapterTest, RejectsCorruptFiles) {
    const auto appendix_offset = rank0.data_offset_bytes + rank0.node_count * record_size;
    const auto roots_offset = appendix_offset + 1;
    const auto rejects = [&](const char* corruption, auto change, bool read_first_node = false) {
        auto file = original;
        change(file);
        EXPECT_THROW({
            LogGOPSimAdapter reader(file);
            (void)reader.read_root_nodes(0);
            if (read_first_node) (void)reader.read_node(0, 0);
        }, std::invalid_argument) << corruption;
    };
    rejects("truncated after the header", [&](auto& file) { file.resize(header.header_size_bytes + 1); });
    rejects("one byte short", [](auto& file) { file.pop_back(); });
    rejects("rank data inside the header", [&](auto& file) {
        write_little_endian(std::uint64_t{0}, std::span(file).subspan(header.rank_table_offset_bytes, 8));
    });
    rejects("root index beyond the nodes", [&](auto& file) { file[roots_offset] = std::byte{3}; });
    rejects("root with dependencies", [&](auto& file) { file[roots_offset] = std::byte{1}; });
    rejects("more roots than stored", [&](auto& file) {
        write_little_endian(std::uint32_t{2}, std::span(file).subspan(header.rank_table_offset_bytes + 16, 4));
    });
    rejects("dependent index beyond the nodes", [&](auto& file) { file[appendix_offset] |= std::byte{3}; }, true);
    rejects("dependency list beyond the appendix", [&](auto& file) { write_field(file, 0, FieldId::FinishDependentsCount, 4); }, true);
    rejects("second rank inside the first", [&](auto& file) {
        write_little_endian(std::uint64_t{0}, std::span(file).subspan(header.rank_table_offset_bytes + rank_table_entry_size_bytes, 8));
    });
}

TEST_F(LogGOPSimAdapterTest, RejectsUnsupportedLayouts) {
    auto without_tag = layout;
    std::erase_if(without_tag.fields, [](const auto& field) { return field.field == FieldIdentity{FieldId::Tag}; });
    auto missing_tag = make_file(without_tag);
    EXPECT_THROW(LogGOPSimAdapter{missing_tag}, std::invalid_argument) << "no tag";

    auto wide_inputs = inputs;
    for (auto& input : wide_inputs)
        if (input.field == FieldIdentity{FieldId::Cpu}) input.value_range = UnsignedRange{0, 256};
    auto wide_cpu = make_file(build_record_layout(wide_inputs, 2));
    EXPECT_THROW(LogGOPSimAdapter{wide_cpu}, std::invalid_argument) << "CPU IDs beyond 8 bits";

    auto counter_with_base = layout;
    counter_with_base.fields.front().offset_encoding = OffsetEncoding{std::uint64_t{1}, 1};
    auto header_bytes = write_binary_header(counter_with_base);
    EXPECT_THROW(LogGOPSimAdapter{header_bytes}, std::invalid_argument) << "a dependency count without zero";
}

TEST_F(LogGOPSimAdapterTest, RejectsInvalidCounts) {
    auto bytes = original;
    write_field(bytes, 2, FieldId::DependencyCount, 3);
    LogGOPSimAdapter reader(bytes);
    EXPECT_THROW((void)reader.read_node(0, 2), std::invalid_argument);
}

// Invalid values only matter in the fields a consumer reads.
TEST_F(TraceReaderTest, DecodesOnlyTheSelectedFields) {
    auto bad_priority = original;
    write_field(bad_priority, 0, std::string{"priority"}, 7);
    EXPECT_EQ(TraceReader(bad_priority, {bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu)}).read_node<AnalysisNode>(0, 0).gpu, 1000u);
    EXPECT_EQ(LogGOPSimAdapter(bad_priority).read_node(0, 0).Size, 1000u);
    EXPECT_EQ(TraceReader(bad_priority, {bind_field(FieldId::Size, &AnalysisNode::bytes)}).read_node<AnalysisNode>(0, 0).bytes, 1000u);
    TraceReader graph_reader(bad_priority, graph_bindings());
    EXPECT_THROW((void)graph_reader.read_node<AnalysisGraphNode>(0, 0), std::invalid_argument);
    TraceReader priority_reader(bad_priority, {priority_and_size.front()});
    EXPECT_THROW((void)priority_reader.read_node<AnalysisNode>(0, 0), std::invalid_argument);

    auto bad_type = original;
    write_field(bad_type, 0, FieldId::Type, 3);
    EXPECT_EQ(TraceReader(bad_type, {priority_and_size.front()}).read_node<AnalysisNode>(0, 0).priority, -2);
    LogGOPSimAdapter type_reader(bad_type);
    EXPECT_THROW((void)type_reader.read_node(0, 0), std::invalid_argument);
}

TEST_F(TraceReaderTest, RejectsSendsToAny) {
    auto bytes = original;
    write_field(bytes, 1, FieldId::Peer, *layout.find_field(FieldId::Peer)->offset_encoding.get_wildcard_code());
    LogGOPSimAdapter adapter(bytes);
    EXPECT_THROW((void)adapter.read_node(0, 1), std::invalid_argument);
    TraceReader fields(bytes, {bind_field(FieldId::Peer, &MatchingNode::source, UINT32_MAX), bind_field(FieldId::Type, &MatchingNode::type)});
    EXPECT_THROW((void)fields.read_node<MatchingNode>(0, 1), std::invalid_argument);
    EXPECT_EQ(fields.read_node<MatchingNode>(0, 2).source, UINT32_MAX) << "a receive from ANY is fine";
    // Without the operation type, a wildcard peer cannot be checked and is not decoded for it.
    EXPECT_EQ(TraceReader(bytes, {bind_field(FieldId::Peer, &MatchingNode::source, UINT32_MAX)}).read_node<MatchingNode>(0, 1).source, UINT32_MAX);
}

TEST_F(TraceReaderTest, ChecksPeerAndOperationRanges) {
    const std::vector<FieldBinding<MatchingNode>> message{bind_field(FieldId::Peer, &MatchingNode::source, UINT32_MAX), bind_field(FieldId::Type, &MatchingNode::type)};
    auto peers_beyond_ranks = layout;
    for (auto& field : peers_beyond_ranks.fields)
        if (field.field == FieldIdentity{FieldId::Peer}) field.offset_encoding.maximum_offset = 2;
    auto peer_bytes = make_file(peers_beyond_ranks);
    EXPECT_THROW((TraceReader{peer_bytes, message}), std::invalid_argument) << "peers beyond the rank count";

    auto unknown_operations = layout;
    for (auto& field : unknown_operations.fields)
        if (field.field == FieldIdentity{FieldId::Type}) field.offset_encoding = OffsetEncoding{std::uint64_t{0}, 3};
    auto operation_bytes = make_file(unknown_operations);
    EXPECT_THROW((TraceReader{operation_bytes, message}), std::invalid_argument) << "operation code 0";
}

namespace {

class GuardedBytes {
public:
    explicit GuardedBytes(std::span<const std::byte> bytes) {
        const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        const auto accessible = (bytes.size() + page - 1) / page * page;
        size_ = accessible + page;
        mapping_ = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mapping_ == MAP_FAILED) throw std::runtime_error("mmap failed");
        auto* end = static_cast<std::byte*>(mapping_) + accessible;
        if (::mprotect(end, page, PROT_NONE) != 0) {
            ::munmap(mapping_, size_);
            throw std::runtime_error("mprotect failed");
        }
        file_ = {end - bytes.size(), bytes.size()};
        std::ranges::copy(bytes, file_.begin());
    }
    ~GuardedBytes() { ::munmap(mapping_, size_); }
    std::span<std::byte> file() const { return file_; }
private:
    void* mapping_;
    std::size_t size_;
    std::span<std::byte> file_;
};

class TraceReaderBoundary : public testing::TestWithParam<const RunnerInfo*> {};

std::vector<const RunnerInfo*> all_runners() {
    std::vector<const RunnerInfo*> runners;
    for (const auto& runner : get_runners()) runners.push_back(&runner);
    return runners;
}

} // namespace

TEST(DependencyLists, ReadsVariableFieldsBeforeProtectedPage) {
    std::vector<FieldValueInfo> inputs;
    for (const auto& field : standard_fields) {
        auto range = UnsignedRange{0, 0};
        if (field.id == FieldId::DependencyCount || field.id == FieldId::FinishDependentsCount ||
            field.id == FieldId::FinishDependentsIndex || field.id == FieldId::StartDependentsIndex) range = {0, 1};
        if (field.id == FieldId::Type) range = {3, 3};
        if (field.id == FieldId::Size) range = {42, 42};
        inputs.push_back(FieldValueInfo::create_standard(field.id, std::nullopt, range));
    }
    const auto layout = build_record_layout(inputs, 1);
    Goal graph;
    auto* first = graph.Calc(42, 0, 0);
    auto* last = graph.Calc(42, 0, 0);
    graph.Dependency(last, first);
    std::ostringstream output;
    TraceWriter writer(output, layout, 1, 1);
    writer.write_rank(graph);
    writer.finish();
    const auto text = output.str();
    GuardedBytes bytes(std::as_bytes(std::span(text)));
    const TraceFileView view(bytes.file());
    ASSERT_LT(view.get_readable_bytes(view.get_node_record_bytes(0, 1)), 8u);
    LogGOPSimAdapter reader(bytes.file());
    EXPECT_TRUE(std::ranges::equal(reader.read_dependents(0, 0, DependencyEvent::Finish), std::array{std::uint32_t{1}}));
    EXPECT_TRUE(reader.read_dependents(0, 0, DependencyEvent::Start).empty());
    EXPECT_TRUE(reader.read_dependents(0, 1, DependencyEvent::Finish).empty());
    EXPECT_TRUE(reader.read_dependents(0, 1, DependencyEvent::Start).empty());
}

TEST_P(TraceReaderBoundary, ReadsLastRecordBeforeProtectedPage) {
    const auto& info = *GetParam();
    if (!info.is_available()) GTEST_SKIP() << info.name << " is unavailable in this build";
    test_helpers::TemporaryDirectory cache;
    test_helpers::ScopedEnvironment cache_environment("LOGGOPSIM_JIT_CACHE", cache.path().string());
    test_helpers::ScopedEnvironment runner_environment("LOGGOPSIM_DECODER", std::string(info.name));
    std::vector<FieldValueInfo> inputs;
    for (const auto& field : standard_fields) {
        auto range = UnsignedRange{0, 0};
        if (field.id == FieldId::DependencyCount) range = {0, 1};
        if (field.id == FieldId::Type) range = {1, 3};
        if (field.id == FieldId::Size) range = {42, 42};
        inputs.push_back(FieldValueInfo::create_standard(field.id, std::nullopt, range));
    }
    const auto layout = build_record_layout(inputs, 1);
    ASSERT_EQ(layout.get_record_size_bytes(), 1u);
    Goal graph;
    for (int i = 0; i < 10; ++i) graph.Calc(42, 0, 0);
    std::ostringstream output;
    TraceWriter writer(output, layout, 1, 4);
    writer.write_rank(graph);
    writer.finish();
    const auto text = output.str();
    GuardedBytes bytes(std::as_bytes(std::span(text)));
    const TraceFileView view(bytes.file());
    ASSERT_EQ(view.get_readable_bytes(view.get_node_record_bytes(0, 9)), 6u);
    ASSERT_GE(view.get_readable_bytes(view.get_node_record_bytes(0, 0)), 8u);
    LogGOPSimAdapter adapter(bytes.file());
    if (!info.handles_any_plan && adapter.get_runner_name() != info.name)
        GTEST_SKIP() << info.name << " has no instantiation for this trace";
    ASSERT_EQ(adapter.get_runner_name(), info.name) << "the requested runner silently fell back";
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto roots = adapter.read_root_nodes(0);
        ASSERT_EQ(roots.size(), 10u);
        for (std::uint32_t i = 0; i < 10; ++i) {
            const auto node = adapter.read_node(0, i);
            EXPECT_EQ(node.Type, std::to_underlying(OperationType::Calculation));
            EXPECT_EQ(node.Size, 42u);
            EXPECT_EQ(node.node_index, i);
            EXPECT_EQ(node.dependency_count, 0u);
            EXPECT_TRUE(node.finish_dependents.empty());
            EXPECT_TRUE(node.start_dependents.empty());
            EXPECT_TRUE(adapter.read_dependents(0, i, DependencyEvent::Start).empty());
            EXPECT_TRUE(adapter.read_dependents(0, i, DependencyEvent::Finish).empty());
        }
    }
    EXPECT_THROW((void)adapter.read_dependents(0, 10, DependencyEvent::Finish), std::invalid_argument);
    EXPECT_TRUE(std::ranges::equal(bytes.file(), std::as_bytes(std::span(text))));
}

INSTANTIATE_TEST_SUITE_P(Runners, TraceReaderBoundary, testing::ValuesIn(all_runners()),
                         [](const auto& info) { return std::string(info.param->name); });

TEST_F(LogGOPSimAdapterTest, KeepsTheOwnedMappingAliveWithoutTheCallersReference) {
    const test_helpers::TemporaryFile file(std::string(reinterpret_cast<const char*>(original.data()), original.size()));
    std::weak_ptr<MappedTraceFile> lifetime;
    {
        auto mapping = std::make_shared<MappedTraceFile>(file.path().c_str(), false);
        lifetime = mapping;
        LogGOPSimAdapter adapter(mapping);
        mapping.reset();
        EXPECT_FALSE(lifetime.expired());
        EXPECT_EQ(adapter.read_node(0, 0).Size, 1000u);
        EXPECT_EQ(adapter.read_dependents(0, 0, DependencyEvent::Finish).size(), 1u);
    }
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(LogGOPSimAdapterTest, FileConstructionGivesEachRunPrivateCounters) {
    const std::string contents(reinterpret_cast<const char*>(original.data()), original.size());
    const test_helpers::TemporaryFile file(contents);
    {
        LogGOPSimAdapter first(file.path().c_str());
        LogGOPSimAdapter second(file.path().c_str());
        EXPECT_EQ(first.decrement_dependency_count(0, 2), 1u);
        EXPECT_EQ(first.read_node(0, 2).dependency_count, 1u);
        EXPECT_EQ(second.read_node(0, 2).dependency_count, 2u);
    }
    std::ifstream input(file.path(), std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), contents);
}

#ifdef LOGGOPSIM_TEST_ATLAHS
TEST_F(AtlahsAdapterTest, OwnsItsPrivateFileAndLeavesTheInputReusable) {
    const auto bytes = make_atlahs_file(layout);
    const std::string contents(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const test_helpers::TemporaryFile file(contents);
    {
        AtlahsAdapter first(file.path().c_str());
        EXPECT_EQ(first.decrement_dependency_count(0, 2), 1u);
        EXPECT_EQ(first.read_node(0, 1).Gpu, 1002u);
    }
    AtlahsAdapter second(file.path().c_str());
    EXPECT_EQ(second.read_node(0, 2).dependency_count, 2u);
    std::ifstream input(file.path(), std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), contents);
}

TEST_F(AtlahsAdapterTest, RejectsOwnedSharedMappingForPackedInput) {
    const auto bytes = make_atlahs_file(layout);
    const test_helpers::TemporaryFile file(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    auto mapping = std::make_shared<MappedTraceFile>(file.path().c_str(), true);
    EXPECT_THROW(AtlahsAdapter{mapping}, std::invalid_argument);
}
#endif
