#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "simulators/atlahs/AtlahsAdapter.hpp"
#include "binary_reading/NodeDecoder.hpp"
#include "binary_reading/TraceReader.hpp"
#include "binary_reading/TraceFileView.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "trace_format/BinaryIO.hpp"
#include "simulators/loggopsim/SimulationSchedule.hpp"
#include "goal_parsing/Goal.hpp"

#include <array>
#include <cstdlib>
#include <new>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <memory>
#include <limits>
#include <utility>

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

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function>
static void rejects(Function function) {
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid mapped trace accepted");
}

int main() try {
    std::vector<FieldValueInfo> inputs;
    for (const auto& definition : standard_fields) {
        auto input = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 1024});
        if (definition.id == FieldId::DependencyCount) input.value_range = UnsignedRange{0, 2};
        if (definition.id == FieldId::Type) input.value_range = UnsignedRange{1, 3};
        if (definition.id == FieldId::Peer || definition.id == FieldId::Tag) input.has_wildcard = true;
        if (definition.id == FieldId::Peer) input.value_range = UnsignedRange{0, 1};
        if (definition.id == FieldId::Tag) input.value_range = UnsignedRange{0, 42};
        if (definition.id == FieldId::Cpu || definition.id == FieldId::Nic)
            input.value_range = UnsignedRange{0, 255};
        inputs.push_back(input);
    }
    std::vector custom{
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4}),
        FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{1000, 1002})};
    for (const auto* name : {"communicator_id", "channel_id", "sequence_id", "message_kind", "chunk_id"})
        custom.push_back(FieldValueInfo::create_custom(name, FieldType::UnsignedInteger, std::nullopt,
            UnsignedRange{0, std::numeric_limits<std::uint64_t>::max()}));
    custom.push_back(FieldValueInfo::create_custom("stream_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1024}));
    custom.push_back(FieldValueInfo::create_custom("peer_gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 1002}));
    inputs.insert(inputs.end(), custom.begin(), custom.end());
    const auto layout = build_record_layout(inputs, 2);
    Goal graph;
    auto* calc = graph.Calc(1000, 255, 255);
    auto* send = graph.Send(1, 1024, 42, 0, 0);
    auto* receive = graph.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    graph.Dependency(send, calc);
    graph.StartDependency(receive, calc);
    graph.Dependency(receive, send);
    calc->custom_values = {std::int64_t{-2}, std::uint64_t{1000}};
    send->custom_values = {std::int64_t{4}, std::uint64_t{1002}};
    receive->custom_values = {std::int64_t{0}, std::uint64_t{1001}};
    for (auto* node : {calc, send, receive}) {
        for (auto value : {std::uint64_t{1} << 48, std::uint64_t{123},
                           std::uint64_t{1} << 40, std::uint64_t{5}, UINT64_MAX})
            node->custom_values.push_back(value);
        node->custom_values.push_back(std::uint64_t{1024});
        node->custom_values.push_back(std::uint64_t{1002});
    }
    Goal empty;
    const auto make_file = [&](const RecordLayout& schema, std::uint32_t index_bits = 2) {
        std::ostringstream output;
        TraceWriter writer(output, schema, 2, index_bits, custom);
        writer.write_rank(graph);
        writer.write_rank(empty);
        writer.finish();
        const auto text = output.str();
        const auto bytes = std::as_bytes(std::span(text));
        return std::vector<std::byte>(bytes.begin(), bytes.end());
    };
    auto original = make_file(layout);
    const auto make_atlahs_file = [&](const RecordLayout& schema) {
        send->Tag = receive->Tag = 0;
        auto bytes = make_file(schema);
        send->Tag = 42;
        receive->Tag = Node::any_tag;
        return bytes;
    };
    auto atlahs_bytes = make_atlahs_file(layout);
    AtlahsAdapter atlahs(atlahs_bytes);
    const auto atlahs_roots = atlahs.read_root_nodes(0);
    check(atlahs_roots.size() == 1 && atlahs_roots[0].Size == 1000 &&
          atlahs_roots[0].start_time == 0 && atlahs_roots[0].Gpu == 1000 &&
          atlahs_roots[0].PeerGpu == 1002 && atlahs_roots[0].Stream == 1024 && atlahs_roots[0].Nic == 255 && std::ranges::equal(atlahs_roots[0].DependOnMe, std::array{std::uint32_t{1}}),
          "ATLAHS adapter lost graph metadata or initialized runtime state incorrectly");
    const auto atlahs_receive = atlahs.read_node(0, 2);
    check(atlahs_receive.Peer == Node::any_source && atlahs_receive.Tag == 0 &&
          atlahs_receive.DependenciesCnt == 2 && atlahs.get_resource_count(FieldId::Cpu) == 256 &&
          atlahs_receive.Gpu == 1001 && atlahs.get_resource_count(std::string{"gpu_id"}) == 1003,
          "ATLAHS adapter lost wildcard, dependency or resource values");
    check(atlahs_receive.Communicator == (std::uint64_t{1} << 48) && atlahs_receive.Channel == 123 &&
          atlahs_receive.Sequence == (std::uint64_t{1} << 40) && atlahs_receive.MessageKind == 5 &&
          atlahs_receive.Chunk == UINT64_MAX, "ATLAHS message identity was narrowed or lost");
    LogGOPSimAdapter tag_only_consumer(atlahs_bytes);
    rejects([&] { (void)tag_only_consumer.read_node(0, 1); });
    static_assert(std::same_as<LogGOPSimAdapter::Node, LogGOPSimNode>);
    static_assert(std::same_as<AtlahsAdapter::Node, AtlahsNode>);
    auto reused_atlahs = atlahs_roots.front();
    reused_atlahs.start_time = 77;
    atlahs.read_node_into(0, 2, reused_atlahs);
    check(reused_atlahs.Gpu == 1001 && reused_atlahs.DependenciesCnt == 2 &&
          reused_atlahs.DependOnMe.empty() && reused_atlahs.StartDependOnMe.empty() &&
          reused_atlahs.start_time == 77 && atlahs_receive.start_time == 0,
          "Typed adapter reuse lost graph fields or overwrote simulator runtime state");
    for (const auto* name : {"gpu_id", "peer_gpu_id", "stream_id", "communicator_id", "channel_id", "sequence_id", "message_kind", "chunk_id"}) {
        auto missing_layout = layout;
        std::erase_if(missing_layout.fields, [&](const auto& field) {
            return field.field == FieldIdentity{std::string{name}};
        });
        auto missing_field = make_atlahs_file(missing_layout);
        rejects([&] { AtlahsAdapter invalid(missing_field); });
    }
    AtlahsAdapter mixed_tag_reader(original);
    rejects([&] { (void)mixed_tag_reader.read_node(0, 1); });
    send->custom_values[5] = std::uint64_t{6}; // message_kind
    auto bad_kind = make_atlahs_file(layout);
    AtlahsAdapter bad_kind_reader(bad_kind);
    rejects([&] { (void)bad_kind_reader.read_node(0, 1); });
    send->custom_values[5] = std::uint64_t{5};
    auto atlahs_wide_inputs = inputs;
    auto& size_input = *std::ranges::find(atlahs_wide_inputs, FieldIdentity{FieldId::Size}, &FieldValueInfo::field);
    const auto large_size = std::uint64_t{std::numeric_limits<int>::max()} + 1;
    size_input.value_range = UnsignedRange{0, large_size};
    const auto wide_layout = build_record_layout(atlahs_wide_inputs, 2);
    send->Size = large_size;
    auto oversized = make_atlahs_file(wide_layout);
    AtlahsAdapter oversized_reader(oversized);
    rejects([&] { (void)oversized_reader.read_node(0, 1); });
    send->Size = 1024;
    calc->Size = large_size;
    auto long_calculation = make_atlahs_file(wide_layout);
    AtlahsAdapter calculation_reader(long_calculation);
    check(calculation_reader.read_node(0, 0).Size == large_size,
          "ATLAHS message-size restriction incorrectly rejected a long calculation");
    calc->Size = 1000;
    struct AnalysisNode {
        std::uint16_t bytes;
        std::int8_t priority;
        std::uint16_t gpu;
        std::uint32_t untouched = 123;
    };
    TraceReader bound_reader(original, {
        bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu),
        bind_field(FieldId::Size, &AnalysisNode::bytes),
        bind_field(std::string{"priority"}, &AnalysisNode::priority)});
    const auto bound_node = bound_reader.read_node<AnalysisNode>(0, 0);
    check(bound_node.bytes == 1000 && bound_node.priority == -2 && bound_node.gpu == 1000 &&
          bound_node.untouched == 123, "Generic member bindings assigned incorrect fields");
    auto reused_node = bound_node;
    bound_reader.read_node_into(0, 1, reused_node);
    check(reused_node.bytes == 1024 && reused_node.priority == 4 && reused_node.gpu == 1002 &&
          bound_node.priority == -2 && reused_node.untouched == 123,
          "Reused output or owned snapshots failed with generic member bindings");
    rejects([&] { (void)bound_reader.read_node<AnalysisNode>(1, 0); });
    struct MatchingNode { std::uint32_t source; std::uint8_t tag; };
    rejects([&] { (void)bound_reader.read_node<MatchingNode>(0, 0); });
    TraceReader bound_matching(original, {
        bind_field(FieldId::Tag, &MatchingNode::tag, std::uint8_t{255}),
        bind_field(FieldId::Peer, &MatchingNode::source, Node::any_source)});
    check(bound_matching.read_node<MatchingNode>(0, 1).source == 1 && bound_matching.read_node<MatchingNode>(0, 1).tag == 42 &&
          bound_matching.read_node<MatchingNode>(0, 2).source == Node::any_source && bound_matching.read_node<MatchingNode>(0, 2).tag == 255,
          "Generic wildcard assignment did not use destination-specific sentinels");
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(FieldId::Peer, &MatchingNode::source)});
    });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(std::string{"missing"}, &AnalysisNode::bytes)});
    });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(std::string{"priority"}, &AnalysisNode::bytes)});
    });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(FieldId::Size, &MatchingNode::tag)});
    });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(FieldId::Size, &AnalysisNode::bytes),
            bind_field(FieldId::Size, &AnalysisNode::gpu)});
    });
    rejects([&] { (void)bind_field(FieldId::Size, static_cast<std::uint16_t AnalysisNode::*>(nullptr)); });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(std::string{"priority"}, &AnalysisNode::priority, std::int8_t{0})});
    });
    rejects([&] {
        TraceReader invalid(original, {
            bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu, std::uint16_t{1001})});
    });
    TraceReader separate_sentinel(original, {
        bind_field(std::string{"priority"}, &AnalysisNode::priority, std::int8_t{-3})});
    check(separate_sentinel.read_node<AnalysisNode>(0, 0).priority == -2,
          "A wildcard destination outside the numeric range was rejected");
    const FieldRequirements analysis_requirements{
        require_field<std::int8_t>(std::string{"priority"}),
        require_field<std::uint64_t>(FieldId::Size)};
    TraceReader analysis(original, analysis_requirements);
    check(analysis.get_selected_fields().size() == 2 &&
          analysis.get_selected_fields()[0].field == FieldIdentity{std::string{"priority"}} &&
          analysis.get_selected_fields()[1].field == FieldIdentity{FieldId::Size},
          "Consumer selection did not preserve requirement order");
    check(analysis.read_fields(0, 0) == std::vector<FieldValue>{std::int64_t{-2}, std::uint64_t{1000}},
          "Selected signed/custom and standard values were not decoded");
    std::array<FieldValue, 2> reused_values;
    analysis.read_fields_into(0, 1, reused_values);
    check(reused_values[0] == FieldValue{std::int64_t{4}} && reused_values[1] == FieldValue{std::uint64_t{1024}},
          "Caller-owned selected output did not decode the next record");
    rejects([&] { analysis.read_fields_into(0, 0, std::span(reused_values).first(1)); });
    rejects([&] { (void)analysis.read_fields(1, 0); });
    auto thin_layout = layout;
    std::erase_if(thin_layout.fields, [](const auto& field) {
        return field.field != FieldIdentity{FieldId::Size} &&
               field.field != FieldIdentity{std::string{"priority"}};
    });
    auto thin_bytes = make_file(thin_layout);
    TraceReader bound_thin(thin_bytes, {
        bind_field(FieldId::Size, &AnalysisNode::bytes),
        bind_field(std::string{"priority"}, &AnalysisNode::priority)});
    check(bound_thin.read_node<AnalysisNode>(0, 0).bytes == 1000 && bound_thin.read_node<AnalysisNode>(0, 0).priority == -2,
          "Generic bindings required fields belonging only to LogGOPSim");
    TraceReader thin_reader(thin_bytes, analysis_requirements);
    check(thin_reader.read_fields(0, 0) == analysis.read_fields(0, 0),
          "A consumer unnecessarily required the simulator's other fields");
    rejects([&] { LogGOPSimAdapter invalid(thin_bytes); });
    rejects([&] {
        TraceReader invalid(original, {require_field<std::uint64_t>(std::string{"missing"})});
    });
    rejects([&] {
        TraceReader invalid(original, {require_field<std::uint64_t>(std::string{"priority"})});
    });
    rejects([&] {
        TraceReader invalid(original, {require_field<std::uint8_t>(FieldId::Size)});
    });
    rejects([&] {
        TraceReader invalid(original, {analysis_requirements.front(), analysis_requirements.front()});
    });
    TraceReader metadata_only(original, {});
    check(metadata_only.read_fields(0, 0).empty() && metadata_only.get_node_count(0) == 3,
          "An empty selection decoded node values");
    rejects([&] { (void)metadata_only.read_node<AnalysisNode>(0, 0); });
    TraceReader matching_fields(original, {
        require_field<std::uint32_t>(FieldId::Peer),
        require_field<std::uint32_t>(FieldId::Tag)});
    check(matching_fields.read_fields(0, 2) == std::vector<FieldValue>{Wildcard{}, Wildcard{}},
          "Generic field selection replaced wildcards with simulator sentinels");

    struct AnalysisGraphNode : BaseDeserializedGraphNode {
        std::uint64_t bytes = 0;
        std::int64_t priority = 0;
        std::uint64_t gpu = 0;
    };
    static_assert(std::derived_from<LogGOPSimNode, BaseDeserializedGraphNode>);
    std::size_t root_decode_count = 0;
    const std::vector graph_bindings{
        bind_field(FieldId::Size, &AnalysisGraphNode::bytes),
        bind_field(std::string{"priority"}, &AnalysisGraphNode::priority),
        bind_field(std::string{"gpu_id"}, &AnalysisGraphNode::gpu)};
    TraceReader graph_reader(original, graph_bindings,
        [&](const AnalysisGraphNode& node) {
            ++root_decode_count;
            if (node.priority > 0) throw std::invalid_argument("Consumer rejects positive priority");
        });
    const auto analysis_roots = graph_reader.read_root_nodes<AnalysisGraphNode>(0);
    check(analysis_roots.size() == 1 && analysis_roots[0].offset == 0 &&
          analysis_roots[0].DependenciesCnt == 0 && analysis_roots[0].bytes == 1000 &&
          analysis_roots[0].priority == -2 && root_decode_count == 1 &&
          std::ranges::equal(analysis_roots[0].DependOnMe, std::array{std::uint32_t{1}}) &&
          std::ranges::equal(analysis_roots[0].StartDependOnMe, std::array{std::uint32_t{2}}) &&
          analysis_roots[0].gpu == 1000,
          "Shared graph reading lost derived fields, graph lists or selected custom values");
    check(graph_reader.read_root_nodes<AnalysisGraphNode>(1).empty() &&
          root_decode_count == 1, "Empty-rank loading called the node validator");
    auto reused_graph_node = analysis_roots.front();
    graph_reader.read_node_into(0, 2, reused_graph_node);
    check(reused_graph_node.offset == 2 && reused_graph_node.DependenciesCnt == 2 &&
          reused_graph_node.DependOnMe.empty() && reused_graph_node.StartDependOnMe.empty() &&
          reused_graph_node.priority == 0 && reused_graph_node.gpu == 1001,
          "Reusing a graph output retained old dependency lists or bound values");
    rejects([&] { (void)graph_reader.read_node<AnalysisGraphNode>(0, 1); });
    rejects([&] {
        TraceReader missing_graph(thin_bytes, {
            bind_field(FieldId::Size, &AnalysisGraphNode::bytes)});
    });


    auto shared_bytes = original;
    TraceReader shared_operations(shared_bytes, {require_field<std::uint32_t>(FieldId::DependencyCount)});
    rejects([&] { (void)shared_operations.decrement_dependency_count(0, 2); });
    rejects([&] { shared_operations.set_dependency_counter(std::string{"missing"}); });
    rejects([&] { shared_operations.set_dependency_counter(std::string{"priority"}); });
    rejects([&] { shared_operations.set_dependency_counter(std::string{"gpu_id"}); });
    rejects([&] { shared_operations.set_dependency_counter(FieldId::Peer); });
    shared_operations.set_dependency_counter(FieldId::DependencyCount);
    check(shared_operations.read_fields(0, 2).front() == FieldValue{std::uint64_t{2}} &&
          shared_operations.decrement_dependency_count(0, 2) == 1 &&
          shared_operations.decrement_dependency_count(0, 2) == 0 &&
          shared_operations.read_fields(0, 2).front() == FieldValue{std::uint64_t{0}},
          "Shared reader failed to update its configured counter");
    rejects([&] { (void)shared_operations.decrement_dependency_count(0, 2); });
    check(shared_operations.get_resource_count(FieldId::Cpu) == 256 &&
          shared_operations.get_resource_count(FieldId::Nic) == 256,
          "Shared resource counts wrapped at 255");
    rejects([&] { (void)shared_operations.get_resource_count(std::string{"missing"}); });
    rejects([&] { (void)shared_operations.get_resource_count(std::string{"priority"}); });
    TraceReader shared_custom(shared_bytes, graph_bindings);
    check(shared_custom.read_node<AnalysisGraphNode>(0, 2).priority == 0 &&
          shared_custom.read_node<AnalysisGraphNode>(0, 2).gpu == 1001,
          "Counter updates changed bound custom values");
    auto large_resource_layout = layout;
    for (auto& field : large_resource_layout.fields)
        if (field.field == FieldIdentity{FieldId::Cpu}) {
            field.bit_width = 64;
            field.offset_encoding = OffsetEncoding{std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()};
        }
    large_resource_layout = create_packed_schema(std::move(large_resource_layout.fields));
    auto large_resource_bytes = make_file(large_resource_layout);
    TraceReader large_resources(large_resource_bytes, {});
    rejects([&] { (void)large_resources.get_resource_count(FieldId::Cpu); });

    auto bytes = original;
    LogGOPSimAdapter reader(bytes);
    SimulatorAdapter<AnalysisGraphNode> custom_reader(bytes, graph_bindings);
    check(custom_reader.read_node(0, 0).priority == -2 &&
          custom_reader.read_node(0, 1).priority == 4 &&
          custom_reader.read_node(0, 0).gpu == 1000 &&
          custom_reader.read_node(0, 1).gpu == 1002,
          "Custom bindings did not preserve signedness or value bases");
    check(reader.get_rank_count() == 2 && reader.get_node_count(0) == 3 && reader.get_node_count(1) == 0 &&
          reader.get_root_count(0) == 1 && reader.read_root(0, 0) == 0, "Wrong rank/root metadata");
    check(reader.read_root_nodes(1).empty(), "Empty rank returned initial nodes");
    check(reader.get_resource_count(FieldId::Cpu) == 256 && reader.get_resource_count(FieldId::Nic) == 256, "Resource counts wrapped at 255");
    allocation_count = 0;
    count_allocations = true;
    std::uint64_t dependency_sum = 0;
    for (int i = 0; i < 100; ++i) {
        const auto node = reader.read_node(0, 0);
        for (auto target : node.DependOnMe) dependency_sum += target;
        for (auto target : node.StartDependOnMe) dependency_sum += target;
    }
    count_allocations = false;
    check(allocation_count == 0 && dependency_sum == 300,
          "Steady-state node reads allocated or lost dependencies");
    const auto decoded = reader.read_node(0, 0);
    check(decoded.Size == 1000 && decoded.Proc == 255 && decoded.Nic == 255 &&
          std::ranges::equal(decoded.DependOnMe, std::array{std::uint32_t{1}}) &&
          std::ranges::equal(decoded.StartDependOnMe, std::array{std::uint32_t{2}}), "Wrong decoded node/dependencies");
    auto copied_dependencies = [&] {
        auto temporary_bytes = original;
        TraceReader temporary_reader(temporary_bytes, graph_bindings);
        auto node = temporary_reader.read_node<AnalysisGraphNode>(0, 1);
        return std::vector<std::uint32_t>(node.DependOnMe.begin(), node.DependOnMe.end());
    }();
    check(copied_dependencies == std::vector<std::uint32_t>{2},
          "Explicit dependency copy did not outlive the mapping");
    const auto node_after_reader = [&] {
        TraceReader temporary_reader(bytes, graph_bindings);
        return temporary_reader.read_node<AnalysisGraphNode>(0, 1);
    }();
    check(node_after_reader.bytes == 1024 && node_after_reader.DependOnMe[0] == 2,
          "Dependency view depended on the reader instead of the mapped bytes");
    check(reader.read_node(0, 2).Peer == Node::any_source && reader.read_node(0, 2).Tag == Node::any_tag,
          "Wildcard decoding changed");
    auto reordered = layout;
    std::ranges::reverse(reordered.fields);
    auto reordered_bytes = make_file(reordered);
    LogGOPSimAdapter reordered_reader(reordered_bytes);
    TraceReader reordered_custom(reordered_bytes, graph_bindings);
    check(reordered_custom.read_node<AnalysisGraphNode>(0, 0).gpu == 1000 &&
          reordered_custom.read_node<AnalysisGraphNode>(0, 1).priority == 4,
          "Custom bindings assumed descriptor order");
    rejects([&] { (void)reader.read_node(1, 0); });
    check(reordered_reader.read_node(0, 0).Size == 1000 &&
          reordered_reader.decrement_dependency_count(0, 2) == 1,
          "Reader incorrectly assumed descriptor order");
    auto wide_index_bytes = make_file(layout, 64);
    LogGOPSimAdapter wide_index_reader(wide_index_bytes);
    check(wide_index_reader.read_root(0, 0) == 0 &&
          std::ranges::equal(wide_index_reader.read_node(0, 0).DependOnMe, std::array{std::uint32_t{1}}),
          "Reader incorrectly assumed 32-bit index arrays");
    auto standard_layout = layout;
    std::erase_if(standard_layout.fields, [](const auto& field) {
        return std::holds_alternative<std::string>(field.field);
    });
    auto standard_bytes = make_file(standard_layout);
    LogGOPSimAdapter standard_reader(standard_bytes);
    check(standard_reader.read_node(0, 0).Size == decoded.Size,
          "Standard-only schema changed simulator node decoding");
    rejects([&] { TraceReader missing_custom(standard_bytes, graph_bindings); });
    const auto receive_snapshot = reader.read_node(0, 2);
    check(receive_snapshot.DependenciesCnt == 2 && reader.decrement_dependency_count(0, 2) == 1 &&
          reader.read_node(0, 2).DependenciesCnt == 1 && reader.decrement_dependency_count(0, 2) == 0 &&
          reader.read_node(0, 2).DependenciesCnt == 0 && receive_snapshot.DependenciesCnt == 2,
          "Reads or owned snapshots mishandled counter updates");
    check(custom_reader.read_node(0, 2).priority == 0 &&
          custom_reader.read_node(0, 2).gpu == 1001,
          "Runtime counter updates changed bound custom values");
    const auto header = read_binary_header(original);
    const auto entry = read_rank_table_entry(std::span(original).subspan(header.rank_table_offset_bytes,
                                                                       rank_table_entry_size_bytes));
    const auto record_size = layout.get_record_size_bytes();
    for (std::uint32_t bits = 1; bits <= 64; ++bits) {
        std::vector<std::byte> indices(bit_packing::calculate_index_array_size_bytes(7, bits));
        for (std::uint32_t i = 0; i < 7; ++i)
            bit_packing::write_bits(indices, std::uint64_t{i} * bits, bits, i % 2);
        const auto indices_view = NodeIndexView::packed(indices, 1, 5, bits, 2);
        check(std::ranges::equal(indices_view, std::array{1u, 0u, 1u, 0u, 1u}),
              "Dependency view lost bit offsets or wide indices");
        check(NodeIndexView::packed(indices, 7, 0, bits, 2).empty(), "Empty tail is invalid");
        rejects([&] { (void)NodeIndexView::packed(indices, 1, 5, bits, 1); });
        rejects([&] { (void)NodeIndexView::packed(indices, UINT32_MAX, 1, bits, 2); });
    }
    std::array<std::byte, 9> unaligned_indices{};
    const std::array<std::uint32_t, 2> native_indices{1, 2};
    std::memcpy(unaligned_indices.data() + 1, native_indices.data(), 8);
    const auto native_view = NodeIndexView::native(std::span(unaligned_indices).subspan(1), 3);
    check(std::ranges::equal(native_view, native_indices), "Unaligned native view changed indices");
    rejects([&] { (void)NodeIndexView::native(std::span(unaligned_indices).subspan(1), 2); });
    rejects([&] { (void)NodeIndexView::native(unaligned_indices, 3); });
    std::array<std::byte, 8> overflowing_index{};
    bit_packing::write_bits(overflowing_index, 0, 64, std::uint64_t{1} << 32);
    rejects([&] { (void)NodeIndexView::packed(overflowing_index, 0, 1, 64, 3); });
    TraceFileView file_view(original);
    check(file_view.get_rank_count() == 2 && file_view.get_node_count(0) == 3 &&
          file_view.get_node_count(1) == 0 && file_view.get_root_count(0) == 1 &&
          file_view.read_root(0, 0) == 0, "File access lost rank/root metadata");
    const auto first_record = file_view.get_node_record_bytes(0, 0);
    check(first_record.size() == record_size && first_record.data() == original.data() + entry.data_offset_bytes,
          "File access copied records instead of viewing the mapped buffer");
    check(std::ranges::equal(file_view.read_dependents(0, 0, 1), std::array{std::uint32_t{1}}) &&
          std::ranges::equal(file_view.read_dependents(0, 1, 1), std::array{std::uint32_t{2}}) &&
          file_view.read_dependents(0, entry.dependency_count, 0).empty(),
          "File access decoded the wrong dependency slices");
    rejects([&] { (void)file_view.get_node_record_bytes(0, 3); });
    rejects([&] { (void)file_view.read_root(0, 1); });
    rejects([&] { (void)file_view.read_dependents(0, entry.dependency_count, 1); });
    const auto changed_byte = entry.data_offset_bytes + 2 * record_size;
    // Exercise the generic decoder independently of the simulator's field mapping.
    const FieldRequirements decoder_requirements{
        require_field<std::uint8_t>(FieldId::Type),
        require_field<std::uint32_t>(FieldId::Peer),
        require_field<std::uint32_t>(FieldId::Tag),
        require_field<std::uint32_t>(FieldId::DependencyCount)};
    NodeDecoder decoder(layout, decoder_requirements);
    const auto receive_record = std::span(original).subspan(changed_byte, record_size);
    std::vector<FieldValue> direct(decoder.get_selected_fields().size());
    decoder.decode_fields(receive_record, direct);
    check(direct.size() == decoder_requirements.size() &&
          direct[0] == FieldValue{std::uint64_t{std::to_underlying(traceschema::OperationType::Receive)}} && direct[1] == FieldValue{Wildcard{}} &&
          direct[2] == FieldValue{Wildcard{}} && direct[3] == FieldValue{std::uint64_t{2}},
          "Shared node decoder lost selected fields or wildcards");
    std::vector<FieldValue> callback_values;
    decoder.decode_fields(receive_record, [&](std::size_t index, const FieldStorageInfo& field, FieldValue value) {
        check(field.field == decoder.get_selected_fields()[index].field, "Callback field order changed");
        callback_values.push_back(std::move(value));
    });
    check(callback_values == direct, "Callback and buffer overloads decoded differently");
    callback_values.clear();
    decoder.decode_fields(receive_record, [&](std::size_t, const FieldStorageInfo&, FieldValue value) {
        callback_values.push_back(std::move(value));
    }, 1);
    check(callback_values == std::vector<FieldValue>{direct.front()}, "Prefix decoding read extra fields");
    rejects([&] { decoder.decode_fields(receive_record,
        [](std::size_t, const FieldStorageInfo&, FieldValue) {}, direct.size() + 1); });
    const auto truncated_record = receive_record.first(record_size - 1);
    rejects([&] { decoder.decode_fields(truncated_record, direct); });
    rejects([&] { decoder.decode_fields(truncated_record,
        [](std::size_t, const FieldStorageInfo&, FieldValue) {}); });
    auto roots_bytes = original;
    LogGOPSimAdapter roots_reader(roots_bytes);
    const auto initial_nodes = roots_reader.read_root_nodes(0);
    check(initial_nodes.size() == 1 && initial_nodes.front().Size == 1000,
          "Root loading changed during decoder simplification");
    for (std::size_t index = 0; index < bytes.size(); ++index)
        if (index != changed_byte) check(bytes[index] == original[index], "Counter update changed another byte");
    check((bytes[changed_byte] & std::byte{0xfc}) == (original[changed_byte] & std::byte{0xfc}),
          "Counter update changed neighboring fields");
    rejects([&] { reader.decrement_dependency_count(0, 2); });
    rejects([&] { (void)reader.read_node(0, 3); });
    rejects([&] { (void)reader.read_root(0, 1); });

    // Both formats use the same dependency-release and readiness methods.
    auto ready_bytes = original;
    LogGOPSimAdapter ready_reader(ready_bytes);
    SerializedGraph schedule(ready_reader, 0);
    check(schedule.GetExecutableNodes_DSN().front().offset == 0, "Initial root changed");
    schedule.MarkNodeAsStarted(0);
    check(schedule.GetExecutableNodes_DSN().empty(), "Start dependency completed the receive early");
    schedule.MarkNodeAsDone(0);
    check(schedule.GetExecutableNodes_DSN().front().offset == 1, "Completion dependency failed to unlock send");
    schedule.MarkNodeAsDone(1);
    check(schedule.GetExecutableNodes_DSN().front().offset == 2, "Mixed dependencies failed to unlock receive");

    const auto appendix_offset = entry.data_offset_bytes + entry.node_count * record_size;
    const auto roots_offset = appendix_offset + 1;
    const auto invalid_file = [&](auto change, bool read_first_node = false) {
        auto invalid = original;
        change(invalid);
        rejects([&] {
            LogGOPSimAdapter invalid_reader(invalid);
            (void)invalid_reader.read_root_nodes(0);
            if (read_first_node) (void)invalid_reader.read_node(0, 0);
        });
    };
    invalid_file([&](auto& file) { file.resize(header.header_size_bytes + 1); });
    invalid_file([&](auto& file) { file.pop_back(); });
    invalid_file([&](auto& file) {
        write_little_endian(std::uint64_t{0}, std::span(file).subspan(header.rank_table_offset_bytes, 8));
    });
    invalid_file([&](auto& file) { file[roots_offset] = std::byte{3}; });
    invalid_file([&](auto& file) { file[roots_offset] = std::byte{1}; });
    invalid_file([&](auto& file) {
        write_little_endian(std::uint32_t{2}, std::span(file).subspan(header.rank_table_offset_bytes + 16, 4));
    });
    invalid_file([&](auto& file) { file[appendix_offset] |= std::byte{3}; }, true);
    invalid_file([&](auto& file) {
        const auto* field = layout.find_field(FieldId::CompletionCount);
        bit_packing::write_bits(std::span(file).subspan(entry.data_offset_bytes, record_size),
                                field->bit_offset, field->bit_width, 4);
    }, true);
    invalid_file([&](auto& file) {
        write_little_endian(std::uint64_t{0}, std::span(file).subspan(
            header.rank_table_offset_bytes + rank_table_entry_size_bytes, 8));
    });

    auto missing_field = layout;
    std::erase_if(missing_field.fields, [](const auto& field) { return field.field == FieldIdentity{FieldId::Tag}; });
    auto missing_bytes = make_file(missing_field);
    rejects([&] { LogGOPSimAdapter invalid(missing_bytes); });
    auto wide_inputs = inputs;
    for (auto& input : wide_inputs)
        if (input.field == FieldIdentity{FieldId::Cpu}) input.value_range = UnsignedRange{0, 256};
    auto wide_cpu_bytes = make_file(build_record_layout(wide_inputs, 2));
    rejects([&] { LogGOPSimAdapter invalid(wide_cpu_bytes); });
    auto invalid_count_bytes = original;
    bit_packing::write_bits(std::span(invalid_count_bytes).subspan(changed_byte, record_size), 0, 2, 3);
    LogGOPSimAdapter invalid_count_reader(invalid_count_bytes);
    rejects([&] { (void)invalid_count_reader.read_node(0, 2); });
    auto invalid_counter = layout;
    invalid_counter.fields.front().offset_encoding = OffsetEncoding{std::uint64_t{1}, 1};
    auto invalid_counter_header = write_binary_header(invalid_counter);
    rejects([&] { LogGOPSimAdapter invalid(invalid_counter_header); });
    auto invalid_custom_bytes = original;
    const auto* priority_layout = layout.find_field("priority");
    bit_packing::write_bits(std::span(invalid_custom_bytes).subspan(entry.data_offset_bytes, record_size),
                            priority_layout->bit_offset, priority_layout->bit_width, 7);
    LogGOPSimAdapter invalid_custom_reader(invalid_custom_bytes);
    TraceReader unused_priority(invalid_custom_bytes, {
        bind_field(std::string{"gpu_id"}, &AnalysisNode::gpu)});
    check(unused_priority.read_node<AnalysisNode>(0, 0).gpu == 1000,
          "Custom binding decoded a field it did not require");
    TraceReader required_priority(invalid_custom_bytes, graph_bindings);
    rejects([&] { (void)required_priority.read_node<AnalysisGraphNode>(0, 0); });
    TraceReader size_only(invalid_custom_bytes, {
        require_field<std::uint64_t>(FieldId::Size)});
    check(size_only.read_fields(0, 0) == std::vector<FieldValue>{std::uint64_t{1000}},
          "Selected read decoded an unused custom value");
    TraceReader priority_only(invalid_custom_bytes, {analysis_requirements.front()});
    rejects([&] { (void)priority_only.read_fields(0, 0); });
    auto invalid_send_bytes = original;
    const auto* peer_layout = layout.find_field(FieldId::Peer);
    bit_packing::write_bits(std::span(invalid_send_bytes).subspan(entry.data_offset_bytes + record_size, record_size),
                           peer_layout->bit_offset, peer_layout->bit_width,
                           *peer_layout->offset_encoding.get_wildcard_code());
    LogGOPSimAdapter invalid_send(invalid_send_bytes);
    rejects([&] { (void)invalid_send.read_node(0, 1); });
    const FieldRequirements message_requirements{
        require_field<std::uint32_t>(FieldId::Peer),
        require_field<std::uint8_t>(FieldId::Type)};
    TraceReader generic_message_reader(invalid_send_bytes, message_requirements);
    rejects([&] { (void)generic_message_reader.read_fields(0, 1); });
    check(generic_message_reader.read_fields(0, 2).front() == FieldValue{Wildcard{}},
          "Shared validation rejected a receive wildcard");
    // An unselected operation type is not decoded just to validate a selected peer.
    TraceReader peer_only(invalid_send_bytes, {require_field<std::uint32_t>(FieldId::Peer)});
    check(peer_only.read_fields(0, 1).front() == FieldValue{Wildcard{}},
          "Shared validation decoded an unrequested operation type");
    auto invalid_peer_layout = layout;
    for (auto& field : invalid_peer_layout.fields)
        if (field.field == FieldIdentity{FieldId::Peer})
            field.offset_encoding.maximum_offset = 2;
    auto invalid_peer_bytes = make_file(invalid_peer_layout);
    rejects([&] { TraceReader invalid(invalid_peer_bytes, message_requirements); });
    auto invalid_operation_layout = layout;
    for (auto& field : invalid_operation_layout.fields)
        if (field.field == FieldIdentity{FieldId::Type})
            field.offset_encoding = OffsetEncoding{std::uint64_t{0}, 3};
    auto invalid_operation_bytes = make_file(invalid_operation_layout);
    rejects([&] { TraceReader invalid(invalid_operation_bytes, message_requirements); });

    auto invalid_unused_standard = original;
    const auto* type_layout = layout.find_field(FieldId::Type);
    bit_packing::write_bits(std::span(invalid_unused_standard).subspan(entry.data_offset_bytes, record_size),
                            type_layout->bit_offset, type_layout->bit_width, 3);
    TraceReader custom_only(invalid_unused_standard, {analysis_requirements.front()});
    check(custom_only.read_fields(0, 0) == std::vector<FieldValue>{std::int64_t{-2}},
          "Selected custom read decoded an unused standard value");
    LogGOPSimAdapter invalid_type_reader(invalid_unused_standard);
    rejects([&] { (void)invalid_type_reader.read_node(0, 0); });

    check(invalid_custom_reader.read_node(0, 0).Size == 1000, "Unused custom values were decoded eagerly");
    std::cout << "Mapped trace reader checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
