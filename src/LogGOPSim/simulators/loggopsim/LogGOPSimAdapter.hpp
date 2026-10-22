#ifndef LOGGOPSIM_LOGGOPSIMADAPTER_HPP
#define LOGGOPSIM_LOGGOPSIMADAPTER_HPP

#include "simulators/SimulatorAdapter.hpp"

#include <cstdint>

struct LogGOPSimNode : traceschema::BaseDeserializedGraphNode {
    std::uint8_t Type;
    std::uint32_t Peer;
    std::uint64_t Size;
    std::uint32_t Tag;
    std::uint8_t Proc;
    std::uint8_t Nic;
};

namespace traceschema {

// Connects shared trace access to LogGOPSim's node structure and operation rules.
class LogGOPSimAdapter final : public SimulatorAdapter<LogGOPSimNode> {
public:
    explicit LogGOPSimAdapter(std::span<std::byte> file);
};

} // namespace traceschema
#endif
