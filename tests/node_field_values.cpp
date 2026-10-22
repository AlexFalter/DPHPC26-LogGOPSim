#include "RecordDecoding.hpp"
#include "binary_writing/RankPacking.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "goal_parsing/Goal.hpp"

#include <algorithm>
#include <iostream>
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
    throw std::runtime_error("Invalid node values accepted");
}

int main() try {
    std::vector<FieldValueInfo> inputs;
    for (const auto& definition : standard_fields) {
        auto field = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 100});
        if (definition.id == FieldId::Type) field.value_range = UnsignedRange{1, 3};
        if (definition.id == FieldId::Peer) field.value_range = UnsignedRange{0, 3};
        if (definition.id == FieldId::Size) field.value_range = UnsignedRange{1024, 1024};
        if (definition.id == FieldId::Tag) {
            field.value_range = UnsignedRange{42, 43};
            field.has_wildcard = true;
        }
        if (definition.id == FieldId::Peer) field.has_wildcard = true;
        inputs.push_back(field);
    }
    const std::vector custom_fields{
        FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 7}),
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4})};
    inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
    const auto layout = build_record_layout(inputs, 4);
    Goal graph;
    auto& node = *graph.Send(3, 1024, 42, 2, 1);
    node.Type = std::to_underlying(traceschema::OperationType::Send);
    node.Peer = 3;
    node.Tag = 42;
    node.Size = 1024;
    node.Proc = 2;
    node.Nic = 1;
    node.DependenciesCnt = 2;
    auto* dependent = graph.Calc(1024, 0, 0);
    graph.Dependency(dependent, &node);
    graph.Dependency(dependent, &node);
    graph.StartDependency(dependent, &node);
    node.custom_values = {FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-2}}};
    dependent->custom_values = node.custom_values;
    const auto read_node_values = [&](const RecordLayout& record_layout,
                                      std::span<const FieldValueInfo> declarations) {
        const auto packed = RankPacker(record_layout, 1, declarations).pack(graph);
        const auto record_bytes = std::span(packed.bytes).first(record_layout.get_record_size_bytes());
        return decode_record(record_layout, record_bytes);
    };
    const RecordValues expected{
        FieldValue{std::uint64_t{2}}, FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{3}},
        FieldValue{std::uint64_t{1024}}, FieldValue{std::uint64_t{42}}, FieldValue{std::uint64_t{2}},
        FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{2}}, FieldValue{std::uint64_t{0}},
        FieldValue{std::uint64_t{1}}, FieldValue{std::uint64_t{2}},
        FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-2}}};
    const auto values = read_node_values(layout, custom_fields);
    check(values == expected, "Node fields or dependency-list indices changed");
    const auto loaded_layout = read_binary_header(write_binary_header(layout)).layout;
    const RecordPacker packer(loaded_layout);
    check(decode_record(packer.get_layout(), packer.pack(values)) == expected, "Node-to-packed-record round trip failed");

    auto reordered = layout;
    std::ranges::reverse(reordered.fields);
    auto reversed_values = expected;
    std::ranges::reverse(reversed_values);
    check(read_node_values(reordered, custom_fields) == reversed_values,
          "Node values did not follow layout order");

    node.Type = std::to_underlying(traceschema::OperationType::Receive);
    node.Peer = Node::any_source;
    node.Tag = Node::any_tag;
    const auto wildcard_values = read_node_values(layout, custom_fields);
    check(std::holds_alternative<Wildcard>(wildcard_values[2]) &&
          std::holds_alternative<Wildcard>(wildcard_values[4]) &&
          decode_record(packer.get_layout(), packer.pack(wildcard_values)) == wildcard_values,
          "Legacy wildcard sentinels were not converted to ANY");

    node.Type = std::to_underlying(traceschema::OperationType::Calculation);
    node.Peer = node.Tag = 0;
    const auto calc_values = read_node_values(layout, custom_fields);
    check(calc_values[4] == FieldValue{std::uint64_t{42}} &&
          decode_record(packer.get_layout(), packer.pack(calc_values)) == calc_values,
          "Unused calc fields did not use valid numeric placeholders");

    rejects([&] { (void)read_node_values(layout, {}); });
    auto unknown_field = layout;
    unknown_field.fields.back().field = std::string("unknown");
    rejects([&] { (void)read_node_values(unknown_field, custom_fields); });
    std::cout << "Node record values checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
