#include "binary_reading/decoding/runners/templated_node/TemplatedNodeRunner.hpp"

#include <tuple>

namespace traceschema {

namespace {

constexpr StepShape node(std::uint8_t size) { return {OutputStruct::Node, size}; }
constexpr StepShape list_position() { return {OutputStruct::DependencyLists, 4}; }

// The steps TraceReader builds for a graph node: the adapter's bindings in order, then the
// dependency count (a uint32_t in the node) and the four list positions. They mirror the
// adapters' bindings; tests/unit/trace_reader.cpp checks that both adapters get a shape runner.
using KnownTemplatedNodeRunners = std::tuple<
    // LogGOPSimAdapter: Type, Peer, Size, Tag, Proc, Nic.
    TemplatedNodeRunner<node(1), node(4), node(8), node(4), node(1), node(1),
                node(4), list_position(), list_position(), list_position(), list_position()>,
    // AtlahsAdapter: the same, then Gpu, PeerGpu, Stream, Communicator, Channel, Sequence,
    // MessageKind, Chunk.
    TemplatedNodeRunner<node(1), node(4), node(8), node(4), node(1), node(1),
                node(4), node(4), node(4), node(8), node(8), node(8), node(8), node(8),
                node(4), list_position(), list_position(), list_position(), list_position()>>;

} // namespace

std::unique_ptr<const PlanRunner> create_templated_node_runner(const DecodePlan& plan) {
    return create_first_matching<KnownTemplatedNodeRunners>(plan);
}

} // namespace traceschema
