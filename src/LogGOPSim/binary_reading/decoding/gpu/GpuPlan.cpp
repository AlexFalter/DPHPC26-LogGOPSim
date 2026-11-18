#include "binary_reading/decoding/gpu/GpuPlan.hpp"

#include "binary_reading/DependencyGraph.hpp"
#include "binary_reading/FieldBindings.hpp"
#include "binary_reading/TraceFileView.hpp"
#include "binary_reading/TraceReader.hpp"
#include "binary_reading/decoding/DecodePlan.hpp"
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "simulators/loggopsim/SimulationEvents.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"
#include "trace_format/LegacyFormat.hpp"

#include <algorithm>
#include <bit>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace traceschema::gpu {

// ---------------------------------------------------------------------------------------------
// Plan

// The bindings of LogGOPSimAdapter (create_node_bindings) followed by those
// SelfDescribingTraceReader::create_node_decoder adds for graph nodes, in the same order.
static void add_loggopsim_bindings(FieldRequirements& requirements, std::vector<FieldDestination>& destinations) {
    const auto add = [&]<class Target>(OutputStruct target, std::span<const FieldBinding<Target>> bindings) {
        for (const auto& binding : bindings) {
            requirements.push_back(binding.requirement);
            destinations.push_back({target, binding.destination_offset, binding.destination_size,
                                    binding.wildcard_destination ? integer_to_bits(*binding.wildcard_destination) : 0});
        }
    };
    const std::vector<FieldBinding<LogGOPSimNode>> node{
        bind_field(FieldId::Type, &LogGOPSimNode::Type),
        bind_field(FieldId::Peer, &LogGOPSimNode::Peer, ANY_SOURCE),
        bind_field(FieldId::Size, &LogGOPSimNode::Size),
        bind_field(FieldId::Tag, &LogGOPSimNode::Tag, ANY_TAG),
        bind_field(FieldId::Cpu, &LogGOPSimNode::Proc),
        bind_field(FieldId::Nic, &LogGOPSimNode::Nic),
        DependencyGraph::get_count_binding<LogGOPSimNode>()};
    add(OutputStruct::Node, std::span(node));
    add(OutputStruct::DependencyLists, std::span(DependencyGraph::get_list_bindings()));
}

// Column element sizes the device code writes; they must equal the binding sizes.
static constexpr std::uint32_t column_sizes[RoleCount] = {1, 4, 8, 4, 1, 1, 4, 4, 4, 4, 4};

Plan create_plan(const RecordLayout& layout, std::uint32_t node_index_bits) {
    FieldRequirements requirements;
    std::vector<FieldDestination> destinations;
    add_loggopsim_bindings(requirements, destinations);
    if (requirements.size() != RoleCount) throw std::logic_error("GPU plan: unexpected number of bindings");

    // As Decoder: presence and capacity, then the operation-code range.
    layout.check_requirements(requirements);
    std::vector<FieldStorageInfo> fields;
    for (const auto& requirement : requirements)
        fields.push_back(*std::ranges::find(layout.fields, requirement.field, &FieldStorageInfo::field));
    {
        const auto& encoding = fields[RoleType].offset_encoding;
        if (fields[RoleType].type != FieldType::UnsignedInteger || encoding.has_wildcard ||
            std::get<std::uint64_t>(encoding.value_base) < std::to_underlying(OperationType::Send) ||
            std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset)) >
                std::to_underlying(OperationType::Calculation))
            throw std::invalid_argument("Unknown operation code in trace schema");
    }
    // As DependencyGraph: the counter and the list positions are plain unsigned values.
    for (std::uint32_t role = RoleDependencyCount; role < RoleCount; ++role)
        if (fields[role].type != FieldType::UnsignedInteger || fields[role].offset_encoding.has_wildcard)
            throw std::invalid_argument("Dependency fields must be unsigned without wildcard");

    const auto decode_plan = DecodePlan::create(fields, destinations);
    if (decode_plan.get_type_step() != std::optional<std::size_t>{RoleType} ||
        decode_plan.get_peer_step() != std::optional<std::size_t>{RolePeer})
        throw std::logic_error("GPU plan: type and peer must be the first two steps");

    Plan plan{};
    plan.record_bytes = static_cast<std::uint32_t>(layout.get_record_size_bytes());
    plan.node_index_bits = node_index_bits;
    const auto steps = decode_plan.get_steps();
    for (std::uint32_t role = 0; role < RoleCount; ++role) {
        const auto& [source, destination] = steps[role];
        if (destination.size != column_sizes[role]) throw std::logic_error("GPU plan: unexpected destination size");
        auto& step = plan.steps[role];
        step.mask = source.mask;
        step.base = source.base;
        step.maximum_offset = source.maximum_offset;
        step.wildcard_value = destination.wildcard_value;
        step.destination_mask = low_bit_mask(8u * destination.size);
        step.bit_offset = source.first_byte * 8u + source.shift;
        step.width = static_cast<std::uint32_t>(std::bit_width(source.mask));
        step.wildcard = source.wildcard;
        step.destination_size = destination.size;
    }
    plan.send_check = plan.steps[RolePeer].wildcard;
    return plan;
}

// ---------------------------------------------------------------------------------------------
// Image

Image create_image(std::span<std::byte> file) {
    if (detect_trace_format(file) != TraceFormat::SelfDescribing)
        throw std::invalid_argument("The GPU decoder reads self-describing traces only");
    const TraceFileView view(file); // validates the header, the rank table and every section's extent
    const auto header = read_binary_header(file);
    Image image;
    image.record_bytes = static_cast<std::uint32_t>(header.layout.get_record_size_bytes());
    image.node_index_bits = header.node_index_bits;
    const auto table = file.subspan(header.rank_table_offset_bytes, calculate_rank_table_size_bytes(header.rank_count));
    image.ranks.reserve(header.rank_count);
    for (std::uint32_t index = 0; index < header.rank_count; ++index) {
        const auto entry = read_rank_table_entry(table.subspan(std::size_t{index} * rank_table_entry_size_bytes,
                                                               rank_table_entry_size_bytes));
        const auto records = std::uint64_t{entry.node_count} * image.record_bytes;
        image.ranks.push_back({entry.data_offset_bytes, entry.data_offset_bytes + records, image.node_count,
                               image.dependency_count, entry.node_count, entry.dependency_count});
        image.node_count += entry.node_count;
        image.dependency_count += entry.dependency_count;
        image.node_record_bytes += records;
        image.dependency_bytes +=
            bit_packing::calculate_index_array_size_bytes(entry.dependency_count, header.node_index_bits);
        if (entry.node_count != 0 && view.get_node_record_bytes(index, 0).data() != file.data() + entry.data_offset_bytes)
            throw std::logic_error("GPU image: rank section differs from TraceFileView");
    }
    return image;
}

// ---------------------------------------------------------------------------------------------
// Legacy traces

Plan create_legacy_plan() {
    namespace legacy = legacy_format;
    // A field of `bytes` bytes at a fixed offset; with any, ~0 (offset maximum + 1) means ANY.
    const auto field = [](std::size_t offset, std::uint32_t bytes, std::uint32_t destination_size, bool any,
                          std::uint64_t wildcard_value = 0) {
        const auto mask = low_bit_mask(8 * bytes);
        return Step{mask, 0, any ? mask - 1 : mask, wildcard_value, low_bit_mask(8 * destination_size),
                    static_cast<std::uint32_t>(offset * 8), 8 * bytes, any, destination_size};
    };
    Plan plan{};
    plan.record_bytes = legacy::record_bytes;
    plan.node_index_bits = 32;
    plan.send_check = 0; // LegacyTraceReader does not reject sends to ANY
    plan.steps[RoleType] = field(legacy::type_offset, 1, 1, false);
    plan.steps[RolePeer] = field(legacy::peer_offset, 4, 4, true, ANY_SOURCE);
    plan.steps[RoleSize] = field(legacy::size_offset, 8, 8, false);
    plan.steps[RoleTag] = field(legacy::tag_offset, 4, 4, true, ANY_TAG);
    plan.steps[RoleCpu] = field(legacy::cpu_field_offset, 1, 1, false);
    plan.steps[RoleNic] = field(legacy::nic_field_offset, 1, 1, false);
    plan.steps[RoleDependencyCount] = field(legacy::dependency_count_offset, 4, 4, false);
    plan.steps[RoleFinishBegin] = field(legacy::finish_begin_offset, 4, 4, false);
    plan.steps[RoleFinishCount] = field(legacy::finish_count_offset, 4, 4, false);
    plan.steps[RoleStartBegin] = field(legacy::start_begin_offset, 4, 4, false);
    plan.steps[RoleStartCount] = field(legacy::start_count_offset, 4, 4, false);
    return plan;
}

Image create_legacy_image(std::span<std::byte> file) {
    namespace legacy = legacy_format;
    if (detect_trace_format(file) != TraceFormat::Legacy) throw std::invalid_argument("Not a legacy trace");
    // The bounds checks of LegacyTraceReader::read_header and LegacyRankView, without the O(edges)
    // index scan, which the validation kernel performs.
    if (file.size() < legacy::prefix_bytes) throw std::invalid_argument("Invalid legacy schedule header");
    const auto rank_count = read_native<std::uint32_t>(file.data() + legacy::rank_count_offset);
    const auto table_end = legacy::prefix_bytes + std::uint64_t{rank_count} * legacy::rank_table_entry_bytes;
    if (table_end > file.size()) throw std::invalid_argument("Truncated legacy rank table");
    const auto size = file.size() - legacy::magic_bytes;
    Image image;
    image.record_bytes = legacy::record_bytes;
    image.node_index_bits = 32;
    image.ranks.reserve(rank_count);
    for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
        const auto* entry = file.data() + legacy::prefix_bytes + std::size_t{rank} * legacy::rank_table_entry_bytes;
        const auto begin = read_native<std::uint64_t>(entry);
        const auto end = read_native<std::uint64_t>(entry + sizeof(std::uint64_t));
        if (begin < table_end - legacy::magic_bytes || begin > end || end > size)
            throw std::invalid_argument("Invalid legacy rank section bounds");
        const auto section = begin + legacy::magic_bytes, section_end = end + legacy::magic_bytes;
        if (section_end - section < legacy::rank_prefix_bytes) throw std::invalid_argument("Truncated legacy rank prefix");
        const auto node_count = read_native<std::uint32_t>(file.data() + section);
        const auto root_count = read_native<std::uint32_t>(file.data() + section + sizeof(std::uint32_t));
        if (root_count > node_count) throw std::invalid_argument("Rank has more roots than nodes");
        const auto node_byte = section + legacy::rank_prefix_bytes + std::uint64_t{root_count} * sizeof(std::uint32_t);
        const auto records = std::uint64_t{node_count} * legacy::record_bytes;
        if (node_byte > section_end || records > section_end - node_byte)
            throw std::invalid_argument("Truncated legacy rank section");
        const auto dependency_byte = node_byte + records;
        const auto dependency_bytes = section_end - dependency_byte;
        if (dependency_bytes % sizeof(std::uint32_t) != 0 || dependency_bytes / sizeof(std::uint32_t) > UINT32_MAX)
            throw std::invalid_argument("Invalid legacy node index list");
        const auto dependency_count = static_cast<std::uint32_t>(dependency_bytes / sizeof(std::uint32_t));
        image.ranks.push_back({node_byte, dependency_byte, image.node_count, image.dependency_count, node_count,
                               dependency_count});
        image.node_count += node_count;
        image.dependency_count += dependency_count;
        image.node_record_bytes += records;
        image.dependency_bytes += dependency_bytes;
    }
    return image;
}

// ---------------------------------------------------------------------------------------------
// Device sources

// Shared by both kernels. The hash is bench/micro/decode_bench.cpp's node_hash, so the GPU checksum
// equals decode_bench_micro's for the same trace and --fields.
static const char* const common_source = R"cuda(
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

struct Rank { u64 node_byte, dependency_byte, first_node, first_dependency; u32 node_count, dependency_count; };
struct Step { u64 mask, base, maximum_offset, wildcard_value, destination_mask; u32 bit_offset, width, wildcard, destination_size; };
struct Plan { u32 record_bytes, node_index_bits, send_check, reserved; Step steps[11]; };

#define LGS_LISTS 1u
#define LGS_COLUMNS 2u

// Bits [bit, bit + 64) of the file as a little-endian bit stream, from two aligned word loads.
// The device buffer is padded, so the second word always exists.
__device__ __forceinline__ u64 lgs_window(const u64* __restrict__ words, u64 bit) {
    const u64* p = words + (bit >> 6);
    const u32 s = (u32)(bit & 63);
    return (__ldg(p) >> s) | ((__ldg(p + 1) << 1) << (63 - s));
}

__device__ __forceinline__ u64 lgs_mix(u64 x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// node_hash with the list terms summed first: sum((d + 1) * K) = K * sum(d + 1) modulo 2^64.
__device__ __forceinline__ u64 lgs_node_hash(u32 rank, u32 index, const u64* v, u64 finish_sum, u64 start_sum) {
    u64 acc = (((u64)rank << 32) | index) * 0x9e3779b97f4a7c15ull;
    acc += v[0] * 0xc2b2ae3d27d4eb4full + v[1] * 0x165667b19e3779f9ull + v[2] * 0xd6e8feb86659fd93ull +
           v[3] * 0xff51afd7ed558ccdull + v[4] * 0xc4ceb9fe1a85ec53ull + v[5] * 0x87c37b91114253d5ull +
           v[6] * 0x4cf5ad432745937full + v[8] * 0x52dce729da3ed4c5ull + v[10] * 0x38495ab5c1b5e5a7ull;
    acc += finish_sum * 0x2127599bf4325c37ull + start_sum * 0x880355f21e6d1965ull;
    return lgs_mix(acc);
}

// The largest rank whose first node (or dependency entry) is at most g; empty ranks never win.
template<bool Dependencies>
__device__ u32 lgs_find_rank(const Rank* __restrict__ ranks, u32 rank_count, u64 g) {
    u32 low = 0, high = rank_count;
    while (high - low > 1) {
        const u32 middle = (low + high) / 2;
        const u64 first = Dependencies ? ranks[middle].first_dependency : ranks[middle].first_node;
        if (first <= g) low = middle; else high = middle;
    }
    return low;
}

// Thread 0 searches for the block's first element; the others walk forward from there.
template<bool Dependencies>
__device__ __forceinline__ u32 lgs_walk_rank(const Rank* __restrict__ ranks, u32 rank_count, u32 r, u64 g) {
    while (r + 1 < rank_count && (Dependencies ? ranks[r + 1].first_dependency : ranks[r + 1].first_node) <= g) ++r;
    return r;
}

// Adds every thread's value to *target with one atomic per block; every thread must call it.
__device__ __forceinline__ void lgs_block_add(u64 value, u64* target) {
    __shared__ u64 warp_sums[32];
    for (int offset = 16; offset > 0; offset >>= 1) value += __shfl_down_sync(0xffffffffu, value, offset);
    const u32 lane = threadIdx.x & 31, warp = threadIdx.x >> 5;
    if (lane == 0) warp_sums[warp] = value;
    __syncthreads();
    if (warp == 0) {
        value = lane < ((blockDim.x + 31) >> 5) ? warp_sums[lane] : 0;
        for (int offset = 16; offset > 0; offset >>= 1) value += __shfl_down_sync(0xffffffffu, value, offset);
        if (lane == 0 && value != 0) atomicAdd(target, value);
    }
}

__device__ __forceinline__ u64 lgs_list_sum(const u64* __restrict__ words, u64 first_bit, u32 count, u32 bits) {
    const u64 mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    u64 sum = 0;
    for (u32 j = 0; j < count; ++j) sum += (lgs_window(words, first_bit + (u64)j * bits) & mask) + 1;
    return sum;
}

__device__ __forceinline__ void lgs_store_columns(void* const* columns, u64 g, const u64* v) {
    ((u8*)columns[0])[g] = (u8)v[0];
    ((u32*)columns[1])[g] = (u32)v[1];
    ((u64*)columns[2])[g] = v[2];
    ((u32*)columns[3])[g] = (u32)v[3];
    ((u8*)columns[4])[g] = (u8)v[4];
    ((u8*)columns[5])[g] = (u8)v[5];
#pragma unroll
    for (int s = 6; s < 11; ++s) ((u32*)columns[s])[g] = (u32)v[s];
}

// After the fields: the list slices must lie in the rank's appendix (DependencyGraph::attach_lists),
// then the lists are read, the columns written and the node hashed. Invalid records count in result[1].
__device__ __forceinline__ u64 lgs_complete(const u64* __restrict__ words, const Rank& rank, u32 r, u32 index,
                                            u64 g, const u64* v, bool valid, u32 flags, u32 bits, u64* result,
                                            void* const* columns) {
    const u64 n = rank.dependency_count;
    if (v[7] > n || v[8] > n - v[7] || v[9] > n || v[10] > n - v[9]) valid = false;
    if (!valid) {
        atomicAdd(result + 1, 1ull);
        return 0;
    }
    u64 finish = 0, start = 0;
    if (flags & LGS_LISTS) {
        const u64 base = rank.dependency_byte * 8;
        finish = lgs_list_sum(words, base + v[7] * bits, (u32)v[8], bits);
        start = lgs_list_sum(words, base + v[9] * bits, (u32)v[10], bits);
    }
    if (flags & LGS_COLUMNS) lgs_store_columns(columns, g, v);
    return lgs_node_hash(r, index, v, finish, start);
}

// The O(edges) check of TraceFileView::validate_dependency_indices, one thread per entry:
// result[2] counts indices outside their rank.
extern "C" __global__ void lgs_validate(const u64* __restrict__ words, const Rank* __restrict__ ranks, u32 rank_count,
                                        u64 total, u32 bits, u64* result) {
    __shared__ u32 block_rank;
    const u64 first = (u64)blockIdx.x * blockDim.x;
    if (threadIdx.x == 0) block_rank = lgs_find_rank<true>(ranks, rank_count, first);
    __syncthreads();
    const u64 d = first + threadIdx.x;
    u64 bad = 0;
    if (d < total) {
        const u32 r = lgs_walk_rank<true>(ranks, rank_count, block_rank, d);
        const Rank rank = ranks[r];
        const u64 mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
        const u64 index = lgs_window(words, rank.dependency_byte * 8 + (d - rank.first_dependency) * bits) & mask;
        bad = index >= rank.node_count;
    }
    lgs_block_add(bad, result + 2);
}
)cuda";

// The prologue and epilogue of lgs_decode: one thread per node, nodes numbered across ranks.
static const char* const decode_prologue = R"cuda(
extern "C" __global__ void lgs_decode(const u64* __restrict__ words, const Rank* __restrict__ ranks, u32 rank_count,
                                      u64 total, u32 flags, u64* result, void* const* columns) {
    __shared__ u32 block_rank;
    const u64 first = (u64)blockIdx.x * blockDim.x;
    if (threadIdx.x == 0) block_rank = lgs_find_rank<false>(ranks, rank_count, first);
    __syncthreads();
    const u64 g = first + threadIdx.x;
    u64 hash = 0;
    if (g < total) {
        const u32 r = lgs_walk_rank<false>(ranks, rank_count, block_rank, g);
        const Rank rank = ranks[r];
        const u32 index = (u32)(g - rank.first_node);
        bool valid = true, peer_any = false;
        u64 type_value = 0;
        u64 v[11];
        (void)peer_any;
        (void)type_value;
)cuda";

static const char* const decode_epilogue = R"cuda(
    }
    lgs_block_add(hash, result);
}
)cuda";

std::string get_generic_source() {
    std::string source = common_source;
    source += "\n__constant__ Plan lgs_plan;\n";
    source += decode_prologue;
    source += R"cuda(
        const u64 record_bit = (rank.node_byte + (u64)index * lgs_plan.record_bytes) * 8;
#pragma unroll
        for (int s = 0; s < 11; ++s) {
            const Step& step = lgs_plan.steps[s];
            u64 c = step.width ? lgs_window(words, record_bit + step.bit_offset) & step.mask : 0;
            if (c <= step.maximum_offset) c += step.base;
            else if (step.wildcard && c == step.maximum_offset + 1) { c = step.wildcard_value; if (s == 1) peer_any = true; }
            else valid = false;
            if (s == 0) type_value = c;
            v[s] = c & step.destination_mask;
        }
        if (lgs_plan.send_check && peer_any && type_value == 1) valid = false;
        hash = lgs_complete(words, rank, r, index, g, v, valid, flags, lgs_plan.node_index_bits, result, columns);
)cuda";
    source += decode_epilogue;
    return source;
}

std::string generate_specialized_source(const Plan& plan) {
    std::ostringstream out;
    out << common_source << decode_prologue << std::hex;
    out << "        const u64 record_bit = (rank.node_byte + (u64)index * 0x" << plan.record_bytes << "ull) * 8;\n"
        << "        const u64* p = words + (record_bit >> 6);\n"
        << "        const u32 s0 = (u32)(record_bit & 63);\n";
    // The record's 64-bit words, aligned to the record start; only those holding selected bits.
    std::set<std::uint32_t> needed;
    for (const auto& step : plan.steps)
        if (step.width != 0)
            for (auto word = step.bit_offset / 64; word <= (step.bit_offset + step.width - 1) / 64; ++word)
                needed.insert(word);
    for (const auto word : needed)
        out << "        const u64 w" << word << " = (__ldg(p + 0x" << word << ") >> s0) | ((__ldg(p + 0x" << word + 1
            << ") << 1) << (63 - s0));\n";
    for (std::uint32_t role = 0; role < RoleCount; ++role) {
        const auto& step = plan.steps[role];
        const bool full_destination = step.destination_mask == ~std::uint64_t{0};
        if (step.width == 0) { // a constant field: offset zero, so the value is the base
            out << "        v[" << std::dec << role << std::hex << "] = 0x" << (step.base & step.destination_mask) << "ull;\n";
            if (role == RoleType) out << "        type_value = 0x" << step.base << "ull;\n";
            continue;
        }
        const auto word = step.bit_offset / 64;
        const auto shift = step.bit_offset % 64;
        out << "        { u64 c = ";
        if (shift + step.width > 64) out << "((w" << word << " >> 0x" << shift << ") | (w" << word + 1 << " << 0x" << 64 - shift << "))";
        else if (shift != 0) out << "(w" << word << " >> 0x" << shift << ")";
        else out << "w" << word;
        if (step.width < 64) out << " & 0x" << step.mask << "ull";
        out << ";\n";
        if (step.maximum_offset != step.mask) {
            if (step.wildcard) {
                out << "          if (c > 0x" << step.maximum_offset << "ull) { if (c != 0x" << step.maximum_offset + 1
                    << "ull) valid = false; c = 0x" << step.wildcard_value << "ull;";
                if (role == RolePeer) out << " peer_any = true;";
                out << " } else c += 0x" << step.base << "ull;\n";
            } else {
                out << "          if (c > 0x" << step.maximum_offset << "ull) valid = false;\n";
                if (step.base != 0) out << "          c += 0x" << step.base << "ull;\n";
            }
        } else if (step.base != 0) out << "          c += 0x" << step.base << "ull;\n";
        if (role == RoleType) out << "          type_value = c;\n";
        out << "          v[" << std::dec << role << std::hex << "] = c";
        if (!full_destination) out << " & 0x" << step.destination_mask << "ull";
        out << "; }\n";
    }
    if (plan.send_check) out << "        if (peer_any && type_value == 1) valid = false;\n";
    out << "        hash = lgs_complete(words, rank, r, index, g, v, valid, flags, 0x" << plan.node_index_bits
        << "u, result, columns);\n";
    out << decode_epilogue;
    return out.str();
}

} // namespace traceschema::gpu
