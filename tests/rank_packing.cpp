#include "RecordDecoding.hpp"
#include "binary_writing/RankPacking.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"
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
    throw std::runtime_error("Invalid rank node records accepted");
}

static FieldValue number(std::uint64_t value) { return value; }

int main() try {
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
    const std::vector custom_fields{
        FieldValueInfo::create_custom("gpu_id", FieldType::UnsignedInteger, std::nullopt, UnsignedRange{0, 7}),
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4})};
    inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
    const auto layout = build_record_layout(inputs, 2);
    const auto loaded_layout = read_binary_header(write_binary_header(layout)).layout;

    Goal graph;
    auto* calc = graph.Calc(1000, 0, 0);
    auto* send = graph.Send(1, 1024, 42, 1, 1);
    auto* recv = graph.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    graph.Dependency(send, calc);
    graph.Dependency(recv, send);
    graph.StartDependency(recv, calc);
    calc->custom_values = {number(3), FieldValue{std::int64_t{-2}}};
    send->custom_values = {number(7), FieldValue{std::int64_t{4}}};
    recv->custom_values = {number(0), FieldValue{std::int64_t{0}}};

    // depIx/sDepIx follow the same completion-then-start appendix order as the old serializer.
    const std::vector<RecordValues> expected{
        {number(0), number(3), number(0), number(1000), number(42), number(0), number(0),
         number(1), number(0), number(1), number(1), number(3), FieldValue{std::int64_t{-2}}},
        {number(1), number(1), number(1), number(1024), number(42), number(1), number(1),
         number(1), number(2), number(0), number(3), number(7), FieldValue{std::int64_t{4}}},
        {number(2), number(2), Wildcard{}, number(1024), Wildcard{}, number(0), number(0),
         number(0), number(3), number(0), number(3), number(0), FieldValue{std::int64_t{0}}}};
    RankPacker packer(layout, 2, custom_fields);
    const auto packed_rank = packer.pack(graph);
    const auto& bytes = packed_rank.bytes;
    const auto record_size = layout.get_record_size_bytes();
    check(packed_rank.dependency_appendix_offset == expected.size() * record_size &&
          packed_rank.dependency_count == 3 && packed_rank.root_indices_offset == packed_rank.dependency_appendix_offset + 1 &&
          packed_rank.root_count == 1 && bytes.size() == packed_rank.root_indices_offset + 1,
          "Wrong rank section size or appendix metadata");
    // Appendix [send=1, recv=2, recv=2], tightly packed into 2-bit indices.
    check(bytes[packed_rank.dependency_appendix_offset] == std::byte{0x29}, "Wrong dependency appendix bytes or padding");
    check(bytes.back() == std::byte{0}, "Root index zero or its padding changed");
    for (std::size_t index = 0; index < expected.size(); ++index)
        check(decode_record(loaded_layout, std::span(bytes).subspan(index * record_size, record_size)) == expected[index],
              "Packed rank field values or dependency indices differ");
    check(packer.pack(graph).bytes == bytes,
          "Dependency cursor leaked across rank calls");
    check(calc->DependenciesCnt == 0 && send->DependenciesCnt == 1 && recv->DependenciesCnt == 2 &&
          calc->DependOnMe.front() == send && calc->StartDependOnMe.front() == recv,
          "Packing modified the graph");
    const auto wider = RankPacker(layout, 5, custom_fields).pack(graph);
    check(wider.root_indices_offset == wider.dependency_appendix_offset + 2 &&
          wider.bytes.size() == wider.root_indices_offset + 1 &&
          wider.bytes[wider.dependency_appendix_offset] == std::byte{0x41} &&
          wider.bytes[wider.dependency_appendix_offset + 1] == std::byte{0x08} &&
          wider.bytes.back() == std::byte{0}, "File-wide index width or cross-byte packing changed");
    const auto widest = RankPacker(layout, 64, custom_fields).pack(graph);
    for (std::size_t index = 0; index < 3; ++index)
        check(read_little_endian<std::uint64_t>(std::span(widest.bytes).subspan(
                  widest.dependency_appendix_offset + index * 8, 8)) == (index == 0 ? 1 : 2),
              "64-bit dependency indices changed");
    check(widest.bytes.size() == widest.root_indices_offset + 8 &&
          read_little_endian<std::uint64_t>(std::span(widest.bytes).subspan(
              widest.root_indices_offset, 8)) == 0, "64-bit root index changed");

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
    const auto roots_rank = RankPacker(layout, 4, custom_fields).pack(multiple_roots);
    const std::vector<std::byte> expected_roots{std::byte{0x20}, std::byte{0x64}, std::byte{0x08}};
    const auto root_bytes = std::span(roots_rank.bytes).subspan(roots_rank.root_indices_offset);
    check(roots_rank.root_count == 5 && std::ranges::equal(root_bytes, expected_roots),
          "Roots must include only zero-counter nodes, in node order, with zero padding");
    const auto wide_roots = RankPacker(layout, 64, custom_fields).pack(multiple_roots);
    for (std::size_t index = 0; index < wide_roots.root_count; ++index)
        check(read_little_endian<std::uint64_t>(std::span(wide_roots.bytes).subspan(
                  wide_roots.root_indices_offset + index * 8, 8)) == index * 2,
              "Wide roots must contain rank-local node indices");

    Goal independent_nodes;
    auto* independent_calc = independent_nodes.Calc(1000, 0, 0);
    auto* independent_recv = independent_nodes.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    independent_calc->custom_values = calc->custom_values;
    independent_recv->custom_values = recv->custom_values;
    const auto independent_rank = RankPacker(layout, 1, custom_fields).pack(independent_nodes);
    check(independent_rank.dependency_count == 0 && independent_rank.root_count == 2 &&
          independent_rank.dependency_appendix_offset == independent_rank.root_indices_offset &&
          independent_rank.bytes.back() == std::byte{0x02},
          "All independent nodes, including an unmatched receive, must be roots");

    Goal empty;
    const auto empty_rank = RankPacker(layout, 1, custom_fields).pack(empty);
    check(empty_rank.bytes.empty() && empty_rank.dependency_count == 0 &&
          empty_rank.dependency_appendix_offset == 0 && empty_rank.root_count == 0 &&
          empty_rank.root_indices_offset == 0, "Empty rank should have no data");
    rejects([&] { (void)RankPacker(layout, 2, {}).pack(graph); });
    for (const auto width : {0u, 1u, 65u})
        rejects([&] { (void)RankPacker(layout, width, custom_fields).pack(graph); });
    Node foreign{};
    calc->DependOnMe[0] = &foreign;
    rejects([&] { (void)packer.pack(graph); });
    calc->DependOnMe[0] = send;
    calc->StartDependOnMe.clear();
    rejects([&] { (void)packer.pack(graph); });
    calc->StartDependOnMe = {recv};
    send->DependOnMe.push_back(recv);
    rejects([&] { (void)packer.pack(graph); });
    send->DependOnMe.pop_back();
    recv->custom_values[0] = number(8);
    rejects([&] { (void)packer.pack(graph); });
    std::cout << "Rank node packing checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
