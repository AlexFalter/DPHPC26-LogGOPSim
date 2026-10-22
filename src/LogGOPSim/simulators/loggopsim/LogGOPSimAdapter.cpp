#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "SimulationEvents.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <algorithm>

namespace traceschema {

// Graph fields are supplied by TraceReader; declare LogGOPSim's extra fields here.
static std::vector<FieldBinding<LogGOPSimNode>> create_node_bindings() {
    return {
        bind_field(FieldId::Type, &LogGOPSimNode::Type),
        bind_field(FieldId::Peer, &LogGOPSimNode::Peer, ANY_SOURCE),
        bind_field(FieldId::Size, &LogGOPSimNode::Size),
        bind_field(FieldId::Tag, &LogGOPSimNode::Tag, ANY_TAG),
        bind_field(FieldId::Cpu, &LogGOPSimNode::Proc),
        bind_field(FieldId::Nic, &LogGOPSimNode::Nic)};
}

static std::function<void(const LogGOPSimNode&)> check_message_matching(std::span<std::byte> file) {
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

} // namespace traceschema
