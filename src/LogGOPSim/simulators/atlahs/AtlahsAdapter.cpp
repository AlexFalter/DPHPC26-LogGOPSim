#include "simulators/atlahs/AtlahsAdapter.hpp"

#include <limits>

namespace traceschema {

static std::vector<FieldBinding<AtlahsNode>> create_node_bindings() {
    constexpr auto wildcard = std::numeric_limits<std::uint32_t>::max();
    return {
        bind_field(FieldId::Type, &AtlahsNode::Type),
        bind_field(FieldId::Peer, &AtlahsNode::Peer, wildcard),
        bind_field(FieldId::Size, &AtlahsNode::Size),
        bind_field(FieldId::Tag, &AtlahsNode::Tag, wildcard),
        bind_field(FieldId::Cpu, &AtlahsNode::Proc),
        bind_field(FieldId::Nic, &AtlahsNode::Nic),
        bind_field(std::string{"gpu_id"}, &AtlahsNode::Gpu),
        bind_field(std::string{"peer_gpu_id"}, &AtlahsNode::PeerGpu),
        bind_field(std::string{"stream_id"}, &AtlahsNode::Stream),
        bind_field(std::string{"communicator_id"}, &AtlahsNode::Communicator),
        bind_field(std::string{"channel_id"}, &AtlahsNode::Channel),
        bind_field(std::string{"sequence_id"}, &AtlahsNode::Sequence),
        bind_field(std::string{"message_kind"}, &AtlahsNode::MessageKind),
        bind_field(std::string{"chunk_id"}, &AtlahsNode::Chunk)};
}

static void validate_htsim_node(const AtlahsNode& node) {
    if (node.Type != std::to_underlying(OperationType::Calculation)) {
        if (node.Tag != 0)
            throw std::invalid_argument("ATLAHS explicit message identity requires tag 0");
        if (node.MessageKind > 5)
            throw std::invalid_argument("Unknown ATLAHS message_kind");
    }
    // logsim-interface.cpp passes communication sizes to htsim_schedule as int.
    if (node.Type != std::to_underlying(OperationType::Calculation) &&
        node.Size > std::numeric_limits<int>::max())
        throw std::invalid_argument("ATLAHS htsim message size exceeds its int-sized API");
}

AtlahsAdapter::AtlahsAdapter(std::span<std::byte> file)
    : SimulatorAdapter(file, create_node_bindings(), validate_htsim_node) {
    if (get_resource_count(std::string{"peer_gpu_id"}) > get_resource_count(std::string{"gpu_id"}))
        throw std::invalid_argument("ATLAHS peer_gpu_id exceeds the local GPU endpoint range");
}

} // namespace traceschema
