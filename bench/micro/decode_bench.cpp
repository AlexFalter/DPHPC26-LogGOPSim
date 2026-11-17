// Standalone decode microbenchmark (bench/README.md, "Decode microbenchmark").
//
// Maps one trace and decodes every record of every rank, in a chosen order, through one of:
//   upstream          the legacy reader copied verbatim from upstream LogGOPSim
//                     (Parser.hpp, SerializedGraph::get_node_by_offset at 76451de), logic unchanged
//   legacy            NEW's LegacyTraceReader (the backward-compatible 39-byte path)
//   plan|jit|orc|templated_node|templated_schema
//                     NEW's SelfDescribingTraceReader with LOGGOPSIM_DECODER set to that runner
// Both NEW paths go through LogGOPSimAdapter::read_node, the call the simulator makes.
//
// The work is kept alive by an order-independent checksum over every decoded field (including
// the dependency lists unless --fields scalar), so equivalent legacy and self-describing traces
// give the same checksum for every variant and order. Prints one JSON object on stdout.
//
// usage: decode_bench_micro --trace FILE --variant V [--order seq|random|topo]
//          [--fields sim|scalar|all] [--cache warm|cold] [--reps N] [--warmup W] [--seed S]
//          [--jit-cache DIR] [--jit-cold]
//        decode_bench_micro --trace FILE --require FIELD     (validation test, exit 3 on error)

#include "binary_reading/MappedTraceFile.hpp"
#include "binary_reading/TraceReader.hpp"
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "simulators/loggopsim/SimulationEvents.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------------------------
// Upstream legacy reader. Copied from upstream LogGOPSim src/LogGOPSim/Parser.hpp (commit
// 76451de16546c7ea39651c183cbc532f4a3457c0): DeserializedNode, the SerializedGraph constructor's
// rank lookup and get_node_by_offset. The function body is unchanged apart from removing
// commented-out debug printfs; only the surrounding class is reduced to what decoding needs.
// ----------------------------------------------------------------------------------------------
namespace upstream {

struct DeserializedNode {

	uint32_t           DependenciesCnt;
	char               Type;
	uint32_t           Peer;
	uint64_t           Size;
	uint32_t           Tag;
	uint8_t            Proc;
	uint8_t            Nic;
	uint32_t           offset;
	std::vector<uint32_t> DependOnMe;
	std::vector<uint32_t> StartDependOnMe;
};

class SerializedGraph {
	char* mapping_start;
	uint32_t num_root_nodes;
	uint32_t num_nodes;
	uint32_t num_ranks_in_schedule;
	uint32_t my_rank;

public:
	// As upstream's constructor, without add_root_nodes(): map_start is the byte after the magic.
	SerializedGraph(char* map_start, size_t, uint32_t rank) {
		mapping_start = map_start;
		my_rank = rank;
		uint64_t ssched;
		num_ranks_in_schedule = *( ((uint32_t*) mapping_start) );
		char* tmp = mapping_start + sizeof(uint32_t) + sizeof(uint8_t)*2 + sizeof(uint64_t)*2*rank;
		ssched = *( (uint64_t*) tmp); // jumping over num_schedules + max_cpu/max_nic + jumptable
		mapping_start += ssched;
		num_nodes = *((uint32_t*) mapping_start);
		num_root_nodes = *((uint32_t*) (mapping_start+sizeof(uint32_t)));
	}

	uint32_t GetNumNodes() const { return num_nodes; }

	DeserializedNode get_node_by_offset(uint32_t offset) {

		uint32_t num_nodes = (uint32_t) *((uint32_t*) mapping_start);

		if (offset > num_nodes) {
			fprintf(stderr, "[rank %i] got offset %i, have %i nodes\n", my_rank, offset, num_nodes);
			exit(EXIT_FAILURE);
		}
		int SIZEOF_NODE_INFO = sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*7 + sizeof(uint8_t)*2;
		char* start_of_node = mapping_start + sizeof(uint32_t)*2 + sizeof(uint32_t)*num_root_nodes + SIZEOF_NODE_INFO*offset;
		DeserializedNode N;

		N.DependenciesCnt = (uint32_t) *( (uint32_t*) start_of_node);
		N.Type = (char) *(start_of_node + sizeof(uint32_t)); // after depcnt
		N.Peer = (uint32_t) *( (uint32_t*) (start_of_node + sizeof(uint32_t) + sizeof(char)) ); // after depcnt + type
		N.Size = (uint64_t) *( (uint64_t*) (start_of_node + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t)));
		N.Tag = (uint32_t) *( (uint32_t*) (start_of_node + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t) + sizeof(uint64_t)));
		N.Proc = (uint8_t) *( (uint8_t*) (start_of_node + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t)));
		N.Nic = (uint8_t) *( (uint8_t*) (start_of_node + sizeof(uint32_t) + sizeof(char) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint8_t)));
		N.offset = (uint32_t) offset;
		uint32_t num_deps =                      (uint32_t) *( (uint32_t*) (start_of_node + sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*3 + sizeof(uint8_t)*2));
		uint32_t deps_startoffset_in_apdx =      (uint32_t) *( (uint32_t*) (start_of_node + sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*4 + sizeof(uint8_t)*2));
		uint32_t num_startdeps =                 (uint32_t) *( (uint32_t*) (start_of_node + sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*5 + sizeof(uint8_t)*2));
		uint32_t startdeps_startoffset_in_apdx = (uint32_t) *( (uint32_t*) (start_of_node + sizeof(char) + sizeof(uint64_t) + sizeof(uint32_t)*6 + sizeof(uint8_t)*2));
		char* start_of_apdx = mapping_start + sizeof(uint32_t)*2 + sizeof(uint32_t)*num_root_nodes + SIZEOF_NODE_INFO*num_nodes;
		for (uint32_t cnt=0; cnt<num_deps; cnt++) {
			uint32_t depnode = (uint32_t) *( (uint32_t*) (start_of_apdx + (deps_startoffset_in_apdx + cnt)*sizeof(uint32_t)));

			assert(depnode < num_nodes);

			N.DependOnMe.push_back(depnode);
		}
		for (uint32_t cnt=0; cnt<num_startdeps; cnt++) {
			N.StartDependOnMe.push_back((uint32_t) *( (uint32_t*) (start_of_apdx + (startdeps_startoffset_in_apdx + cnt)*sizeof(uint32_t))));
		}

		return N;
	}
};

} // namespace upstream

// ----------------------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

[[noreturn]] void usage(const char* message) {
    std::fprintf(stderr, "decode_bench_micro: %s\n"
                 "usage: decode_bench_micro --trace FILE --variant upstream|legacy|plan|jit|orc|templated_node|"
                 "templated_schema [--order seq|random|topo] [--fields sim|scalar|all] [--cache warm|cold] "
                 "[--reps N] [--warmup W] [--seed S] [--jit-cache DIR] [--jit-cold]\n"
                 "       decode_bench_micro --trace FILE --require FIELD\n", message);
    std::exit(2);
}

struct Options {
    std::string trace, variant = "plan", order = "seq", fields = "sim", cache = "warm", require, jit_cache;
    int reps = 10, warmup = 1;
    std::uint64_t seed = 20261112;
    bool jit_cold = false;
};

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage("missing value");
            return argv[++i];
        };
        if (arg == "--trace") options.trace = value();
        else if (arg == "--variant") options.variant = value();
        else if (arg == "--order") options.order = value();
        else if (arg == "--fields") options.fields = value();
        else if (arg == "--cache") options.cache = value();
        else if (arg == "--reps") options.reps = std::stoi(value());
        else if (arg == "--warmup") options.warmup = std::stoi(value());
        else if (arg == "--seed") options.seed = std::stoull(value());
        else if (arg == "--require") options.require = value();
        else if (arg == "--jit-cache") options.jit_cache = value();
        else if (arg == "--jit-cold") options.jit_cold = true;
        else usage(("unknown argument " + std::string(arg)).c_str());
    }
    if (options.trace.empty()) usage("--trace is required");
    if (options.order != "seq" && options.order != "random" && options.order != "topo") usage("bad --order");
    if (options.fields != "sim" && options.fields != "scalar" && options.fields != "all") usage("bad --fields");
    if (options.cache != "warm" && options.cache != "cold") usage("bad --cache");
    if (options.reps < 1 || options.warmup < 0) usage("bad --reps/--warmup");
    return options;
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += c;
    }
    return out;
}

constexpr std::uint64_t mix(std::uint64_t x) { // splitmix64 finalizer
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

// Everything the checksum covers, independent of the reader that produced it.
struct Fields {
    std::uint64_t type, peer, size, tag, proc, nic, depcnt;
};

// One multiply-add per value and one mix per node: cheap enough not to hide decoder differences,
// and every field (and list entry) influences the sum, so the work cannot be removed.
template<class List>
std::uint64_t node_hash(std::uint32_t rank, std::uint32_t index, const Fields& f, const List& finish,
                        const List& start, bool lists) {
    std::uint64_t acc = (std::uint64_t{rank} << 32 | index) * 0x9e3779b97f4a7c15ull;
    acc += f.type * 0xc2b2ae3d27d4eb4full + f.peer * 0x165667b19e3779f9ull + f.size * 0xd6e8feb86659fd93ull +
           f.tag * 0xff51afd7ed558ccdull + f.proc * 0xc4ceb9fe1a85ec53ull + f.nic * 0x87c37b91114253d5ull +
           f.depcnt * 0x4cf5ad432745937full + finish.size() * 0x52dce729da3ed4c5ull +
           start.size() * 0x38495ab5c1b5e5a7ull;
    if (lists) {
        for (const auto d : finish) acc += (d + 1ull) * 0x2127599bf4325c37ull;
        for (const auto d : start) acc += (d + 1ull) * 0x880355f21e6d1965ull;
    }
    return mix(acc);
}

// ---------------- file / page-cache helpers ----------------

std::size_t file_size(const std::string& path) {
    struct stat info{};
    if (stat(path.c_str(), &info) != 0) throw std::runtime_error("cannot stat " + path);
    return static_cast<std::size_t>(info.st_size);
}

// Fraction of the file's pages resident in the page cache (mincore on a fresh read-only mapping).
double resident_fraction(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return -1;
    const auto size = file_size(path);
    void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return -1;
    const long page = sysconf(_SC_PAGESIZE);
    const std::size_t pages = (size + page - 1) / page;
    std::vector<unsigned char> vec(pages);
    double result = -1;
    if (mincore(map, size, vec.data()) == 0) {
        std::size_t resident = 0;
        for (const auto v : vec) resident += v & 1;
        result = pages ? static_cast<double>(resident) / pages : 0;
    }
    munmap(map, size);
    return result;
}

// Evicts the file's clean pages (no root needed). Mappings of the file must be gone first.
void evict(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    fdatasync(fd);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    close(fd);
}

void pretouch(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return;
    std::vector<char> buffer(1 << 20);
    while (read(fd, buffer.data(), buffer.size()) > 0) {}
    close(fd);
}

// ---------------- readers behind one interface ----------------

// One decoded node, as seen by the checksum: scalar fields and the two dependency lists.
// Each reader decodes into its own native node type; visit() hands that to a callback.
class Reader {
public:
    virtual ~Reader() = default;
    virtual std::uint32_t ranks() const = 0;
    virtual std::uint32_t nodes(std::uint32_t rank) const = 0;
    virtual std::string runner() const = 0;
    virtual std::uint64_t record_bytes() const = 0;
    // Decodes node (rank, index) and returns its checksum contribution.
    virtual std::uint64_t decode(std::uint32_t rank, std::uint32_t index, bool lists) = 0;
    // For building the topological order (not timed): count and both dependent lists.
    virtual void graph(std::uint32_t rank, std::uint32_t index, std::uint32_t& count,
                       std::vector<std::uint32_t>& out) = 0;
    virtual std::uint64_t custom_checksum() const { return 0; }
};

class UpstreamReader final : public Reader {
public:
    explicit UpstreamReader(const std::string& path) : path_(path) {
        // Upstream opens the file "r+" and maps it MAP_PRIVATE; a private writable mapping only needs
        // read access, so read-only trace files work too.
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path);
        size_ = file_size(path);
        void* map = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        close(fd);
        if (map == MAP_FAILED) throw std::runtime_error("mmap failed");
        mapping_ = static_cast<char*>(map);
        if (*reinterpret_cast<std::uint64_t*>(mapping_) != 4223) throw std::runtime_error("not a legacy trace");
        std::uint32_t rank_count;
        std::memcpy(&rank_count, mapping_ + 8, 4);
        for (std::uint32_t rank = 0; rank < rank_count; ++rank)
            graphs_.emplace_back(mapping_ + sizeof(std::uint64_t), size_ - sizeof(std::uint64_t), rank);
    }
    ~UpstreamReader() override { munmap(mapping_, size_); }
    std::uint32_t ranks() const override { return static_cast<std::uint32_t>(graphs_.size()); }
    std::uint32_t nodes(std::uint32_t rank) const override { return graphs_[rank].GetNumNodes(); }
    std::string runner() const override { return "upstream"; }
    std::uint64_t record_bytes() const override { return 39; }
    std::uint64_t decode(std::uint32_t rank, std::uint32_t index, bool lists) override {
        const auto n = graphs_[rank].get_node_by_offset(index);
        const Fields f{static_cast<std::uint8_t>(n.Type), n.Peer, n.Size, n.Tag, n.Proc, n.Nic, n.DependenciesCnt};
        return node_hash(rank, index, f, n.DependOnMe, n.StartDependOnMe, lists);
    }
    void graph(std::uint32_t rank, std::uint32_t index, std::uint32_t& count, std::vector<std::uint32_t>& out) override {
        const auto n = graphs_[rank].get_node_by_offset(index);
        count = n.DependenciesCnt;
        out.assign(n.DependOnMe.begin(), n.DependOnMe.end());
        out.insert(out.end(), n.StartDependOnMe.begin(), n.StartDependOnMe.end());
    }

private:
    std::string path_;
    char* mapping_ = nullptr;
    std::size_t size_ = 0;
    std::vector<upstream::SerializedGraph> graphs_;
};

// NEW: either format, through the simulator's adapter (the call LogGOPSim makes).
class AdapterReader final : public Reader {
public:
    explicit AdapterReader(const std::string& path)
        : file_(std::make_shared<traceschema::MappedTraceFile>(path.c_str(), false)), adapter_(file_) {
        if (adapter_.is_legacy()) record_bytes_ = 39;
        else record_bytes_ = traceschema::read_binary_header(file_->bytes()).layout.get_record_size_bytes();
    }
    std::uint32_t ranks() const override { return adapter_.get_rank_count(); }
    std::uint32_t nodes(std::uint32_t rank) const override { return adapter_.get_node_count(rank); }
    std::string runner() const override { return std::string(adapter_.get_runner_name()); }
    std::uint64_t record_bytes() const override { return record_bytes_; }
    std::uint64_t decode(std::uint32_t rank, std::uint32_t index, bool lists) override {
        const auto n = adapter_.read_node(rank, index);
        const Fields f{n.Type, n.Peer, n.Size, n.Tag, n.Proc, n.Nic, n.dependency_count};
        return node_hash(rank, index, f, n.finish_dependents, n.start_dependents, lists);
    }
    void graph(std::uint32_t rank, std::uint32_t index, std::uint32_t& count, std::vector<std::uint32_t>& out) override {
        const auto n = adapter_.read_node(rank, index);
        count = n.dependency_count;
        out.clear();
        for (const auto d : n.finish_dependents) out.push_back(d);
        for (const auto d : n.start_dependents) out.push_back(d);
    }

private:
    std::shared_ptr<traceschema::MappedTraceFile> file_;
    traceschema::LogGOPSimAdapter adapter_;
    std::uint64_t record_bytes_ = 0;
};

// NEW, self-describing only: the simulator's fields plus every custom field of the layout.
struct AllNode : LogGOPSimNode {
    std::uint64_t custom[32];
};

std::vector<traceschema::FieldBinding<AllNode>> all_bindings(const traceschema::RecordLayout& layout,
                                                             std::size_t& custom_count) {
    using namespace traceschema;
    std::vector<FieldBinding<AllNode>> bindings{
        bind_field<AllNode>(FieldId::Type, static_cast<std::uint8_t AllNode::*>(&AllNode::Type)),
        bind_field<AllNode>(FieldId::Peer, static_cast<std::uint32_t AllNode::*>(&AllNode::Peer), ANY_SOURCE),
        bind_field<AllNode>(FieldId::Size, static_cast<std::uint64_t AllNode::*>(&AllNode::Size)),
        bind_field<AllNode>(FieldId::Tag, static_cast<std::uint32_t AllNode::*>(&AllNode::Tag), ANY_TAG),
        bind_field<AllNode>(FieldId::Cpu, static_cast<std::uint8_t AllNode::*>(&AllNode::Proc)),
        bind_field<AllNode>(FieldId::Nic, static_cast<std::uint8_t AllNode::*>(&AllNode::Nic))};
    const AllNode probe{};
    custom_count = 0;
    for (const auto& field : layout.fields) {
        const auto* name = std::get_if<std::string>(&field.field);
        if (!name) continue;
        if (custom_count == std::size(probe.custom)) throw std::runtime_error("too many custom fields");
        const auto offset = reinterpret_cast<const std::byte*>(&probe.custom[custom_count]) -
                            reinterpret_cast<const std::byte*>(&probe);
        const auto requirement = field.type == FieldType::SignedInteger ? require_field<std::int64_t>(*name)
                                                                        : require_field<std::uint64_t>(*name);
        bindings.push_back({requirement, std::nullopt, static_cast<std::uint32_t>(offset), 8});
        ++custom_count;
    }
    return bindings;
}

class AllFieldsReader final : public Reader {
public:
    explicit AllFieldsReader(const std::string& path)
        : file_(std::make_shared<traceschema::MappedTraceFile>(path.c_str(), false)),
          header_(traceschema::read_binary_header(file_->bytes())),
          reader_(file_->bytes(), all_bindings(header_.layout, custom_count_)) {}
    std::uint32_t ranks() const override { return reader_.get_rank_count(); }
    std::uint32_t nodes(std::uint32_t rank) const override { return reader_.get_node_count(rank); }
    std::string runner() const override { return std::string(reader_.get_runner_name()); }
    std::uint64_t record_bytes() const override { return header_.layout.get_record_size_bytes(); }
    std::uint64_t decode(std::uint32_t rank, std::uint32_t index, bool lists) override {
        const auto n = reader_.read_node<AllNode>(rank, index);
        const Fields f{n.Type, n.Peer, n.Size, n.Tag, n.Proc, n.Nic, n.dependency_count};
        std::uint64_t custom = 0;
        for (std::size_t i = 0; i < custom_count_; ++i) custom = mix(custom ^ n.custom[i]);
        custom_sum_ += custom;
        return node_hash(rank, index, f, n.finish_dependents, n.start_dependents, lists);
    }
    void graph(std::uint32_t rank, std::uint32_t index, std::uint32_t& count, std::vector<std::uint32_t>& out) override {
        const auto n = reader_.read_node<AllNode>(rank, index);
        count = n.dependency_count;
        out.clear();
        for (const auto d : n.finish_dependents) out.push_back(d);
        for (const auto d : n.start_dependents) out.push_back(d);
    }
    std::uint64_t custom_checksum() const override { return custom_sum_; }
    std::size_t custom_count() const { return custom_count_; }

private:
    std::shared_ptr<traceschema::MappedTraceFile> file_;
    traceschema::BinaryHeader header_;
    std::size_t custom_count_ = 0;
    traceschema::TraceReader reader_;
    std::uint64_t custom_sum_ = 0;
};

bool is_legacy_file(const std::string& path) {
    traceschema::MappedTraceFile file(path.c_str(), false);
    return traceschema::detect_trace_format(file.bytes()) == traceschema::TraceFormat::Legacy;
}

std::unique_ptr<Reader> open_reader(const Options& options) {
    if (options.variant == "upstream") return std::make_unique<UpstreamReader>(options.trace);
    const bool legacy = is_legacy_file(options.trace);
    if (options.variant == "legacy") {
        if (!legacy) throw std::invalid_argument("variant legacy needs a legacy trace");
        return std::make_unique<AdapterReader>(options.trace);
    }
    if (legacy) throw std::invalid_argument("runner variants need a self-describing trace");
    setenv("LOGGOPSIM_DECODER", options.variant.c_str(), 1);
    if (options.fields == "all") return std::make_unique<AllFieldsReader>(options.trace);
    return std::make_unique<AdapterReader>(options.trace);
}

using Order = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

Order build_order(Reader& reader, const std::string& kind, std::uint64_t seed) {
    Order order;
    std::uint64_t total = 0;
    for (std::uint32_t r = 0; r < reader.ranks(); ++r) total += reader.nodes(r);
    order.reserve(total);
    if (kind != "topo") {
        for (std::uint32_t r = 0; r < reader.ranks(); ++r)
            for (std::uint32_t i = 0; i < reader.nodes(r); ++i) order.emplace_back(r, i);
        if (kind == "random") std::shuffle(order.begin(), order.end(), std::mt19937_64(seed));
        return order;
    }
    // Frontier order: Kahn's algorithm per rank, ranks interleaved round-robin, the way the event
    // loop alternates between hosts. Cross-rank matching is not modelled (GOAL edges are local).
    std::vector<std::vector<std::uint32_t>> remaining(reader.ranks());
    std::vector<std::deque<std::uint32_t>> ready(reader.ranks());
    std::vector<std::uint32_t> dependents;
    for (std::uint32_t r = 0; r < reader.ranks(); ++r) {
        remaining[r].resize(reader.nodes(r));
        for (std::uint32_t i = 0; i < reader.nodes(r); ++i) {
            std::uint32_t count;
            reader.graph(r, i, count, dependents);
            remaining[r][i] = count;
            if (count == 0) ready[r].push_back(i);
        }
    }
    bool progress = true;
    while (progress) {
        progress = false;
        for (std::uint32_t r = 0; r < reader.ranks(); ++r) {
            if (ready[r].empty()) continue;
            progress = true;
            const auto i = ready[r].front();
            ready[r].pop_front();
            order.emplace_back(r, i);
            std::uint32_t count;
            reader.graph(r, i, count, dependents);
            for (const auto d : dependents)
                if (d < remaining[r].size() && remaining[r][d] > 0 && --remaining[r][d] == 0) ready[r].push_back(d);
        }
    }
    if (order.size() != total) { // unreachable nodes (should not happen): append them in file order
        std::vector<std::vector<bool>> seen(reader.ranks());
        for (std::uint32_t r = 0; r < reader.ranks(); ++r) seen[r].assign(reader.nodes(r), false);
        for (const auto& [r, i] : order) seen[r][i] = true;
        for (std::uint32_t r = 0; r < reader.ranks(); ++r)
            for (std::uint32_t i = 0; i < reader.nodes(r); ++i)
                if (!seen[r][i]) order.emplace_back(r, i);
    }
    return order;
}

int require_mode(const Options& options) {
    // Binds the simulator's fields plus one custom field: a trace without it must be rejected at
    // setup with a clear message (load-time validation, "required ⊆ provided").
    std::string error;
    bool ok = false;
    try {
        traceschema::MappedTraceFile file(options.trace.c_str(), false);
        using namespace traceschema;
        std::vector<FieldBinding<AllNode>> bindings{
            bind_field<AllNode>(FieldId::Type, static_cast<std::uint8_t AllNode::*>(&AllNode::Type)),
            bind_field<AllNode>(FieldId::Peer, static_cast<std::uint32_t AllNode::*>(&AllNode::Peer), ANY_SOURCE)};
        const AllNode probe{};
        const auto offset = reinterpret_cast<const std::byte*>(&probe.custom[0]) - reinterpret_cast<const std::byte*>(&probe);
        bindings.push_back({require_field<std::uint64_t>(options.require), std::nullopt, static_cast<std::uint32_t>(offset), 8});
        TraceReader reader(file.bytes(), std::move(bindings));
        ok = true;
    } catch (const std::exception& e) {
        error = e.what();
    }
    std::printf("{\"tool\":\"decode_bench_micro\",\"mode\":\"require\",\"trace\":\"%s\",\"field\":\"%s\","
                "\"accepted\":%s,\"error\":\"%s\"}\n",
                json_escape(options.trace).c_str(), json_escape(options.require).c_str(), ok ? "true" : "false",
                json_escape(error).c_str());
    return ok ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) try {
    const auto options = parse(argc, argv);
    if (!options.require.empty()) return require_mode(options);

    std::string jit_cache = options.jit_cache;
    if (!jit_cache.empty()) setenv("LOGGOPSIM_JIT_CACHE", jit_cache.c_str(), 1);
    const bool lists = options.fields != "scalar";
    const bool cold = options.cache == "cold";
    const auto total_reps = options.warmup + options.reps;

    struct Rep { bool warmup; double resident; std::uint64_t setup_ns, decode_ns, checksum; };
    std::vector<Rep> reps;
    std::unique_ptr<Reader> reader;
    Order order;
    std::string runner;
    std::uint64_t records = 0, edges = 0, record_bytes = 0, custom_checksum = 0;
    std::uint32_t ranks = 0;
    unsigned jit_dir_counter = 0;

    const auto make_reader = [&]() -> std::uint64_t {
        if (options.jit_cold) { // a fresh, empty decoder cache for every setup
            const auto dir = fs::temp_directory_path() /
                ("decode_bench_jit_" + std::to_string(getpid()) + "_" + std::to_string(jit_dir_counter++));
            fs::remove_all(dir);
            setenv("LOGGOPSIM_JIT_CACHE", dir.c_str(), 1);
            setenv("LOGGOPSIM_ORC_REUSE", "0", 1);
        }
        const auto start = Clock::now();
        reader = open_reader(options);
        const auto end = Clock::now();
        if (options.jit_cold) fs::remove_all(std::getenv("LOGGOPSIM_JIT_CACHE"));
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    };

    std::uint64_t first_setup = make_reader();
    runner = reader->runner();
    ranks = reader->ranks();
    record_bytes = reader->record_bytes();
    for (std::uint32_t r = 0; r < ranks; ++r) records += reader->nodes(r);
    order = build_order(*reader, options.order, options.seed);
    {   // edge count, untimed
        std::vector<std::uint32_t> dependents;
        std::uint32_t count;
        for (std::uint32_t r = 0; r < ranks; ++r)
            for (std::uint32_t i = 0; i < reader->nodes(r); ++i) { reader->graph(r, i, count, dependents); edges += dependents.size(); }
    }
    if (!cold) pretouch(options.trace);

    for (int rep = 0; rep < total_reps; ++rep) {
        std::uint64_t setup_ns = rep == 0 ? first_setup : 0;
        if (cold || options.jit_cold) {
            reader.reset();
            if (cold) evict(options.trace);
        }
        const double resident = resident_fraction(options.trace);
        if (cold || options.jit_cold) setup_ns = make_reader();
        std::uint64_t checksum = 0;
        const auto start = Clock::now();
        for (const auto& [r, i] : order) checksum += reader->decode(r, i, lists);
        const auto end = Clock::now();
        reps.push_back({rep < options.warmup, resident, setup_ns,
                        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()),
                        checksum});
    }
    custom_checksum = reader->custom_checksum();
    for (const auto& rep : reps)
        if (rep.checksum != reps.front().checksum) throw std::runtime_error("checksum differs between repetitions");

    std::ostringstream out;
    out << "{\"tool\":\"decode_bench_micro\",\"version\":1,\"trace\":\"" << json_escape(options.trace)
        << "\",\"variant\":\"" << options.variant << "\",\"runner_actual\":\"" << json_escape(runner)
        << "\",\"order\":\"" << options.order << "\",\"fields\":\"" << options.fields << "\",\"cache\":\""
        << options.cache << "\",\"jit_cold\":" << (options.jit_cold ? "true" : "false")
        << ",\"eviction\":\"" << (cold ? "posix_fadvise_dontneed" : "none") << "\",\"seed\":" << options.seed
        << ",\"ranks\":" << ranks << ",\"records\":" << records << ",\"edges\":" << edges
        << ",\"record_bytes\":" << record_bytes << ",\"node_bytes\":" << records * record_bytes
        << ",\"file_bytes\":" << file_size(options.trace);
    char hex[32];
    std::snprintf(hex, sizeof hex, "%016" PRIx64, reps.front().checksum);
    out << ",\"checksum\":\"" << hex << "\"";
    std::snprintf(hex, sizeof hex, "%016" PRIx64, custom_checksum);
    out << ",\"custom_checksum\":\"" << hex << "\",\"reps\":[";
    for (std::size_t i = 0; i < reps.size(); ++i) {
        const auto& rep = reps[i];
        out << (i ? "," : "") << "{\"rep\":" << i << ",\"warmup\":" << (rep.warmup ? "true" : "false")
            << ",\"setup_ns\":" << rep.setup_ns << ",\"decode_ns\":" << rep.decode_ns
            << ",\"resident_before\":" << rep.resident << "}";
    }
    out << "]}";
    std::puts(out.str().c_str());
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "decode_bench_micro: %s\n", error.what());
    return 1;
}
