#ifndef LOGGOPSIM_GPUPLAN_HPP
#define LOGGOPSIM_GPUPLAN_HPP

#include "trace_format/RecordLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Bulk decoding of a trace on an NVIDIA GPU, host side: the decode plan and rank table in the
// form the device reads, and the CUDA sources compiled with NVRTC (see bench/gpu/README.md).
// Plan, Step and Rank are copied to the device byte for byte; the device code in GpuPlan.cpp
// declares the same layouts.
namespace traceschema::gpu {

// LogGOPSim's node fields in the order SelfDescribingTraceReader binds them: the adapter's
// fields, the dependency count, then the positions of the two dependency lists.
enum Role : std::uint32_t {
    RoleType,
    RolePeer,
    RoleSize,
    RoleTag,
    RoleCpu,
    RoleNic,
    RoleDependencyCount,
    RoleFinishBegin,
    RoleFinishCount,
    RoleStartBegin,
    RoleStartCount,
    RoleCount
};

// One plan step (FieldSource + FieldDestination), with the bit offset counted from the record start.
struct Step {
    std::uint64_t mask;             // the field's width as low bits; 0 for a constant field
    std::uint64_t base;             // value of offset zero
    std::uint64_t maximum_offset;
    std::uint64_t wildcard_value;   // stored for offset maximum_offset + 1 when wildcard is set
    std::uint64_t destination_mask; // low bits kept by the destination member
    std::uint32_t bit_offset;
    std::uint32_t width;
    std::uint32_t wildcard;
    std::uint32_t destination_size; // bytes: 1, 2, 4 or 8
};

struct Plan {
    std::uint32_t record_bytes;
    std::uint32_t node_index_bits;
    std::uint32_t send_check; // the peer has a wildcard, so a send to ANY is an invalid record
    std::uint32_t reserved;
    Step steps[RoleCount];
};

// One rank's sections as absolute file offsets, and the global positions of its first node and
// first dependency entry, by which a thread finds its rank.
struct Rank {
    std::uint64_t node_byte;
    std::uint64_t dependency_byte;
    std::uint64_t first_node;
    std::uint64_t first_dependency;
    std::uint32_t node_count;
    std::uint32_t dependency_count;
};

static_assert(sizeof(Step) == 56 && sizeof(Plan) == 16 + RoleCount * 56 && sizeof(Rank) == 40,
              "the device code declares the same layouts");

// What the kernels need to know about a mapped file besides its bytes.
struct Image {
    std::vector<Rank> ranks;
    std::uint64_t node_count = 0;
    std::uint64_t dependency_count = 0;
    std::uint64_t node_record_bytes = 0; // bytes of node records, the bytes a decoder touches
    std::uint64_t dependency_bytes = 0;
    std::uint32_t record_bytes = 0;
    std::uint32_t node_index_bits = 0;
};

// Bytes the kernels may read past the end of the file: every load is two aligned 64-bit words.
inline constexpr std::size_t tail_padding_bytes = 16;

// The plan LogGOPSimAdapter's reader builds for this layout, with the same checks
// (required fields present and wide enough, valid operation codes).
[[nodiscard]] Plan create_plan(const RecordLayout& layout, std::uint32_t node_index_bits);

// Validates the header and rank table the way TraceFileView does and collects the rank sections.
// Self-describing traces only.
[[nodiscard]] Image create_image(std::span<std::byte> file);

// Legacy (39-byte record) traces expressed as the same plan and rank table, so that both kernels
// read them as well: fixed byte offsets, ANY stored as ~0 in peer and tag, 32-bit dependency
// indices. Mirrors LegacyTraceReader; dependency indices are left to the validation kernel.
[[nodiscard]] Plan create_legacy_plan();
[[nodiscard]] Image create_legacy_image(std::span<std::byte> file);

// The schema-independent kernel, which reads the plan from constant memory (`lgs_plan`): the GPU
// counterpart of the plan interpreter.
[[nodiscard]] std::string get_generic_source();

// A kernel with every offset, shift, mask and base of plan as a constant: the GPU counterpart of
// the generated C decoder. It has the same entry points as the generic kernel.
[[nodiscard]] std::string generate_specialized_source(const Plan& plan);

} // namespace traceschema::gpu
#endif
