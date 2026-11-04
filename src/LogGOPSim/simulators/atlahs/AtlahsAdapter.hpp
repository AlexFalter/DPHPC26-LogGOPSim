#ifndef LOGGOPSIM_ATLAHSADAPTER_HPP
#define LOGGOPSIM_ATLAHSADAPTER_HPP

#include "simulators/SimulatorAdapter.hpp"

namespace traceschema {

// ATLAHS's htsim scheduler uses start_time as runtime state, not a trace field.
struct AtlahsNode : BaseDeserializedGraphNode {
    std::uint8_t Type = 0;
    std::uint32_t Peer = 0;
    std::uint64_t Size = 0;
    std::uint32_t Tag = 0;
    std::uint8_t Proc = 0;
    std::uint8_t Nic = 0;
    std::uint32_t Gpu = 0;
    std::uint32_t PeerGpu = 0;
    std::uint32_t Stream = 0;
    std::uint64_t Communicator = 0;
    std::uint64_t Channel = 0;
    std::uint64_t Sequence = 0;
    std::uint64_t MessageKind = 0;
    std::uint64_t Chunk = 0;
    std::uint64_t start_time = 0;
};

// Used by ATLAHS's existing scheduler; packet simulation stays in htsim.
class AtlahsAdapter final : public SimulatorAdapter<AtlahsNode> {
public:
    explicit AtlahsAdapter(std::span<std::byte> file);
};

} // namespace traceschema
#endif
