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

// Used by ATLAHS's existing scheduler; packet simulation stays in htsim. Self-describing traces
// carry explicit GPUs, streams and message identities. Legacy traces hold only the standard
// fields: the NIC also selects the local and the peer GPU, the CPU selects the stream, and
// messages are matched by tag.
class AtlahsAdapter final : public SimulatorAdapter<AtlahsNode> {
public:
    explicit AtlahsAdapter(std::span<std::byte> file);
    explicit AtlahsAdapter(std::shared_ptr<MappedTraceFile> file);
    // shared maps the file MAP_SHARED, which htsim's --save-mem allows for legacy traces only.
    explicit AtlahsAdapter(const char* filename, bool shared = false);

    // Explicit endpoints, streams and tuple message identities, rather than legacy NIC/CPU/tag conventions.
    [[nodiscard]] bool uses_explicit_metadata() const { return !is_legacy(); }
    [[nodiscard]] std::uint32_t get_stream_count() const;
    [[nodiscard]] std::uint32_t get_gpu_count() const;

private:
    AtlahsAdapter(std::span<std::byte> file, bool legacy);
    AtlahsAdapter(std::shared_ptr<MappedTraceFile> file, bool legacy);
    void check_gpu_endpoints() const;
};

} // namespace traceschema
#endif
