#include "simulators/atlahs/AtlahsAdapter.hpp"

#include <limits>

namespace traceschema {

static std::vector<FieldBinding<AtlahsNode>> create_node_bindings(bool legacy) {
    constexpr auto wildcard = std::numeric_limits<std::uint32_t>::max();
    std::vector bindings{
        bind_field(FieldId::Type, &AtlahsNode::Type),
        bind_field(FieldId::Peer, &AtlahsNode::Peer, wildcard),
        bind_field(FieldId::Size, &AtlahsNode::Size),
        bind_field(FieldId::Tag, &AtlahsNode::Tag, wildcard),
        bind_field(FieldId::Cpu, &AtlahsNode::Proc),
        bind_field(FieldId::Nic, &AtlahsNode::Nic)};
    if (legacy) {
        bindings.insert(bindings.end(), {
            bind_field(FieldId::Cpu, &AtlahsNode::Stream),
            bind_field(FieldId::Nic, &AtlahsNode::Gpu),
            bind_field(FieldId::Nic, &AtlahsNode::PeerGpu)});
        return bindings;
    }
    bindings.insert(bindings.end(), {
        bind_field("gpu_id", &AtlahsNode::Gpu),
        bind_field("peer_gpu_id", &AtlahsNode::PeerGpu),
        bind_field("stream_id", &AtlahsNode::Stream),
        bind_field("communicator_id", &AtlahsNode::Communicator),
        bind_field("channel_id", &AtlahsNode::Channel),
        bind_field("sequence_id", &AtlahsNode::Sequence),
        bind_field("message_kind", &AtlahsNode::MessageKind),
        bind_field("chunk_id", &AtlahsNode::Chunk)});
    return bindings;
}

static void check_message_size(const AtlahsNode& node) {
    // logsim-interface.cpp passes message sizes to htsim_schedule as int.
    if (node.Type != std::to_underlying(OperationType::Calculation) && node.Size > std::numeric_limits<int>::max())
        throw std::invalid_argument("ATLAHS htsim message size exceeds its int-sized API");
}

static void check_message_identity(const AtlahsNode& node) {
    check_message_size(node);
    if (node.Type == std::to_underlying(OperationType::Calculation)) return;
    if (node.Tag != 0)
        throw std::invalid_argument("ATLAHS explicit message identity requires tag 0");
    if (node.MessageKind > 5)
        throw std::invalid_argument("Unknown ATLAHS message_kind");
}

static std::function<void(const AtlahsNode&)> create_validator(bool legacy) {
    return legacy ? check_message_size : check_message_identity;
}

// Keep packed ATLAHS input reusable: its counter updates must use a private mapping.
static std::shared_ptr<MappedTraceFile> require_private_packed(std::shared_ptr<MappedTraceFile> file, bool legacy) {
    if (!legacy && file->is_shared())
        throw std::invalid_argument("ATLAHS packed traces require a private mapping");
    return file;
}

AtlahsAdapter::AtlahsAdapter(std::span<std::byte> file)
    : AtlahsAdapter(file, detect_trace_format(file) == TraceFormat::Legacy) {}

AtlahsAdapter::AtlahsAdapter(std::span<std::byte> file, bool legacy)
    : SimulatorAdapter(file, create_node_bindings(legacy), create_validator(legacy)) {
    check_gpu_endpoints();
}

AtlahsAdapter::AtlahsAdapter(std::shared_ptr<MappedTraceFile> file)
    : AtlahsAdapter(file, detect_trace_format(file ? file->bytes() : std::span<std::byte>{}) == TraceFormat::Legacy) {}

AtlahsAdapter::AtlahsAdapter(std::shared_ptr<MappedTraceFile> file, bool legacy)
    : SimulatorAdapter(require_private_packed(std::move(file), legacy),
                       create_node_bindings(legacy), create_validator(legacy)) {
    check_gpu_endpoints();
}

AtlahsAdapter::AtlahsAdapter(const char* filename, bool shared)
    : AtlahsAdapter(std::make_shared<MappedTraceFile>(filename, shared)) {}

void AtlahsAdapter::check_gpu_endpoints() const {
    if (!is_legacy() && get_resource_count("peer_gpu_id") > get_resource_count("gpu_id"))
        throw std::invalid_argument("ATLAHS peer_gpu_id exceeds the local GPU endpoint range");
}

std::uint32_t AtlahsAdapter::get_stream_count() const {
    const auto count = get_resource_count(is_legacy() ? FieldIdentity{FieldId::Cpu} : FieldIdentity{"stream_id"});
    if (count > std::numeric_limits<int>::max())
        throw std::invalid_argument("ATLAHS stream count exceeds its int-sized API");
    return static_cast<std::uint32_t>(count);
}

std::uint32_t AtlahsAdapter::get_gpu_count() const {
    const auto count = get_resource_count(is_legacy() ? FieldIdentity{FieldId::Nic} : FieldIdentity{"gpu_id"});
    if (count > std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("ATLAHS GPU count exceeds 32 bits");
    return static_cast<std::uint32_t>(count);
}

} // namespace traceschema
