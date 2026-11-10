#include "binary_reading/LegacyTraceReader.hpp"
#include "binary_reading/TraceReader.hpp"
#include "simulators/atlahs/AtlahsAdapter.hpp"
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace traceschema;

namespace {
// Independent, fixed-width legacy fixture: two calculation nodes, 0 releases 1. Node 1 has
// CPU 2, NIC 1 and peer and tag ANY (~0).
std::vector<std::byte> fixture() {
    std::vector<std::byte> bytes(124);
    const auto put = [&](std::size_t offset, auto value) { write_native(bytes.data() + offset, value); };
    put(0, std::uint64_t{4223});
    put(8, std::uint32_t{1});
    put(12, std::uint8_t{2}); // largest CPU
    put(13, std::uint8_t{1}); // largest NIC
    put(14, std::uint64_t{22}); // Rank-table offsets exclude the eight-byte magic.
    put(22, std::uint64_t{116});
    put(30, std::uint32_t{2});
    put(34, std::uint32_t{1});
    put(46, std::uint8_t{3});
    put(51, std::uint64_t{4096});
    put(65, std::uint32_t{1});
    put(77, std::uint32_t{1});
    put(81, std::uint32_t{1});
    put(85, std::uint8_t{3});
    put(86, std::numeric_limits<std::uint32_t>::max());
    put(98, std::numeric_limits<std::uint32_t>::max());
    put(102, std::uint8_t{2});
    put(103, std::uint8_t{1});
    put(108, std::uint32_t{1});
    put(116, std::uint32_t{1});
    put(120, std::uint32_t{1});
    return bytes;
}

// A consumer's own struct; several members may receive the same legacy field.
struct Node : BaseDeserializedGraphNode {
    std::uint8_t type = 0;
    std::uint64_t size = 0;
    std::uint32_t peer = 0;
    std::uint32_t tag = 0;
    std::uint8_t cpu = 0;
    std::uint32_t stream = 0;
};

std::vector<FieldBinding<Node>> bindings() {
    return {bind_field(FieldId::Type, &Node::type), bind_field(FieldId::Size, &Node::size),
            bind_field(FieldId::Peer, &Node::peer, 7u), bind_field(FieldId::Tag, &Node::tag),
            bind_field(FieldId::Cpu, &Node::cpu), bind_field(FieldId::Cpu, &Node::stream)};
}

TEST(LegacyReader, ReadsAndUpdatesTheOriginalLayout) {
    auto bytes = fixture();
    LegacyTraceReader reader(bytes, bindings());
    EXPECT_EQ(reader.get_rank_count(), 1u);
    EXPECT_EQ(reader.get_runner_name(), "legacy");
    EXPECT_EQ(reader.read_root(0, 0), 0u);
    const auto roots = reader.read_root_nodes<Node>(0);
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0].type, 3);
    EXPECT_EQ(roots[0].size, 4096u);
    EXPECT_TRUE(std::ranges::equal(roots[0].finish_dependents, std::array{std::uint32_t{1}}));
    EXPECT_TRUE(reader.read_dependents(0, 0, DependencyEvent::Start).empty());
    EXPECT_EQ(reader.read_node<Node>(0, 1).dependency_count, 1u);
    EXPECT_EQ(reader.read_dependency_count(0, 1), 1u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 1), 0u);
    EXPECT_EQ(reader.read_dependency_count(0, 1), 0u);
    EXPECT_THROW(reader.decrement_dependency_count(0, 1), std::invalid_argument);
    EXPECT_EQ(reader.read_node<Node>(0, 1).dependency_count, 0u);
}

TEST(LegacyReader, MapsAnyToTheBindingsWildcardAndDuplicatesFields) {
    auto bytes = fixture();
    LegacyTraceReader reader(bytes, bindings());
    const auto node = reader.read_node<Node>(0, 1);
    EXPECT_EQ(node.peer, 7u) << "peer ANY becomes the binding's wildcard value";
    EXPECT_EQ(node.tag, std::numeric_limits<std::uint32_t>::max()) << "without a wildcard, the raw value";
    EXPECT_EQ(node.cpu, 2u);
    EXPECT_EQ(node.stream, 2u) << "one field bound to two members";
    EXPECT_EQ(reader.get_resource_count(FieldId::Cpu), 3u);
    EXPECT_EQ(reader.get_resource_count(FieldId::Nic), 2u);
    EXPECT_THROW((void)reader.get_resource_count(std::string{"gpu_id"}), std::invalid_argument);
}

TEST(LegacyReader, RejectsBindingsTheFormatCannotFill) {
    auto bytes = fixture();
    struct Custom { std::uint32_t gpu; };
    EXPECT_THROW((LegacyTraceReader{bytes, {bind_field(std::string{"gpu_id"}, &Custom::gpu)}}), std::invalid_argument)
        << "legacy traces have no custom fields";
    struct Narrow { std::uint8_t size; std::int32_t peer; };
    EXPECT_THROW((LegacyTraceReader{bytes, {bind_field(FieldId::Size, &Narrow::size)}}), std::invalid_argument)
        << "a member too small for the 64-bit size";
    EXPECT_THROW((LegacyTraceReader{bytes, {bind_field(FieldId::Peer, &Narrow::peer)}}), std::invalid_argument)
        << "a signed member cannot hold every unsigned 32-bit peer";
    LegacyTraceReader reader(bytes, bindings());
    EXPECT_THROW((void)reader.read_node<Narrow>(0, 0), std::invalid_argument) << "another output type";
}

TEST(LegacyReader, RejectsTruncatedOrOutOfRangeSectionsAndLists) {
    auto bytes = fixture();
    bytes.pop_back();
    EXPECT_THROW((LegacyTraceReader{bytes, bindings()}), std::invalid_argument);
    bytes = fixture();
    write_native(bytes.data() + 38, std::uint32_t{2});
    EXPECT_THROW((LegacyTraceReader{bytes, bindings()}), std::invalid_argument) << "root outside the rank";
    bytes = fixture();
    write_native(bytes.data() + 69, std::uint32_t{2});
    LegacyTraceReader reader(bytes, bindings());
    EXPECT_THROW((void)reader.read_dependents(0, 0, DependencyEvent::Finish), std::invalid_argument);
    EXPECT_THROW((void)reader.read_node<Node>(1, 0), std::invalid_argument) << "rank outside the schedule";
}

TEST(TraceReaderDispatch, DetectsTheFormatFromTheMagicNumber) {
    auto bytes = fixture();
    EXPECT_EQ(detect_trace_format(bytes), TraceFormat::Legacy);
    TraceReader reader(bytes, bindings());
    EXPECT_TRUE(reader.is_legacy());
    EXPECT_EQ(reader.get_runner_name(), "legacy");
    EXPECT_EQ(reader.read_node<Node>(0, 1).peer, 7u);
    EXPECT_EQ(reader.decrement_dependency_count(0, 1), 0u);
    EXPECT_EQ(reader.read_root_nodes<Node>(0).size(), 1u);

    EXPECT_THROW((void)detect_trace_format(std::span(bytes).first(7)), std::invalid_argument) << "truncated";
    write_native(bytes.data(), std::uint64_t{2342});
    EXPECT_THROW((TraceReader{bytes, bindings()}), std::invalid_argument) << "invalidated by --save-mem";
    write_native(bytes.data(), std::uint64_t{1234});
    EXPECT_THROW((TraceReader{bytes, bindings()}), std::invalid_argument) << "unknown magic";
}

TEST(TraceReaderDispatch, AdaptersReadLegacyFiles) {
    auto bytes = fixture();
    LogGOPSimAdapter loggopsim(bytes);
    EXPECT_TRUE(loggopsim.is_legacy());
    const auto node = loggopsim.read_node(0, 1);
    EXPECT_EQ(node.Peer, std::numeric_limits<std::uint32_t>::max()) << "ANY_SOURCE";
    EXPECT_EQ(node.Tag, std::numeric_limits<std::uint32_t>::max()) << "ANY_TAG";
    EXPECT_EQ(node.Proc, 2);
    EXPECT_EQ(node.Nic, 1);
    auto atlahs_bytes = fixture();
    AtlahsAdapter atlahs(atlahs_bytes);
    EXPECT_TRUE(atlahs.is_legacy());
    const auto atlahs_node = atlahs.read_node(0, 1);
    EXPECT_EQ(atlahs_node.Stream, 2u) << "the CPU selects the stream";
    EXPECT_EQ(atlahs_node.Gpu, 1u) << "the NIC selects the local GPU";
    EXPECT_EQ(atlahs_node.PeerGpu, 1u) << "and the peer GPU";
    EXPECT_EQ(atlahs_node.Communicator, 0u) << "no message identity";
    EXPECT_EQ(atlahs.get_stream_count(), 3u);
    EXPECT_EQ(atlahs.get_gpu_count(), 2u);
}
} // namespace
