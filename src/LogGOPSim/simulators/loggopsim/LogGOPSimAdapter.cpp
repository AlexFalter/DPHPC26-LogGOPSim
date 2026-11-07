#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "SimulationEvents.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <algorithm>

namespace traceschema {

// Graph fields are supplied by TraceReader; declare LogGOPSim's extra fields here. Legacy
// traces hold exactly these.
static std::vector<FieldBinding<LogGOPSimNode>> create_node_bindings() {
    return {
        bind_field(FieldId::Type, &LogGOPSimNode::Type),
        bind_field(FieldId::Peer, &LogGOPSimNode::Peer, ANY_SOURCE),
        bind_field(FieldId::Size, &LogGOPSimNode::Size),
        bind_field(FieldId::Tag, &LogGOPSimNode::Tag, ANY_TAG),
        bind_field(FieldId::Cpu, &LogGOPSimNode::Proc),
        bind_field(FieldId::Nic, &LogGOPSimNode::Nic)};
}

// Legacy traces have no custom fields, so only self-describing ones can carry ATLAHS identities.
static std::function<void(const LogGOPSimNode&)> check_message_matching(std::span<std::byte> file) {
    if (detect_trace_format(file) == TraceFormat::Legacy) return {};
    const auto header = read_binary_header(file);
    const auto names = {"gpu_id", "communicator_id", "channel_id", "sequence_id", "message_kind", "chunk_id"};
    if (!std::ranges::all_of(names, [&](const char* name) { return header.layout.find_field(name); }))
        return {};
    return [](const LogGOPSimNode& node) {
        if (node.Type != OP_LOCOP && node.Tag == 0)
            throw std::invalid_argument(
                "LogGOPSim's tag matcher cannot consume explicit ATLAHS message identities; use ATLAHS");
    };
}

LogGOPSimAdapter::LogGOPSimAdapter(std::span<std::byte> file)
    : SimulatorAdapter(file, create_node_bindings(), check_message_matching(file)) {}

LogGOPSimAdapter::LogGOPSimAdapter(std::shared_ptr<MappedTraceFile> file)
    : SimulatorAdapter(file, create_node_bindings(),
                       check_message_matching(file ? file->bytes() : std::span<std::byte>{})) {}

LogGOPSimAdapter::LogGOPSimAdapter(const char* filename, bool shared)
    : LogGOPSimAdapter(std::make_shared<MappedTraceFile>(filename, shared)) {}

} // namespace traceschema
