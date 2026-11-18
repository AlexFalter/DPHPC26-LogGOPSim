// GPU bulk-decode benchmark (bench/gpu/README.md).
//
// Maps one self-describing trace, moves it to the GPU and decodes every record of every rank with
// one thread per record, through either
//   generic      one kernel for every schema; the plan is read from constant memory
//                (the GPU counterpart of LOGGOPSIM_DECODER=plan)
//   specialized  a kernel generated from the trace's header with every offset, shift and mask as
//                a constant, compiled with NVRTC at load time and cached on disk
//                (the GPU counterpart of LOGGOPSIM_DECODER=jit)
// The work is kept alive by bench/micro/decode_bench.cpp's order-independent checksum, so the
// result must equal decode_bench_micro's for the same trace and --fields; --cpu-check computes it
// in-process through LogGOPSimAdapter as well. Prints one JSON object on stdout.
//
// usage: gpu_decode_bench --trace FILE [--kernel generic|specialized] [--fields sim|scalar]
//          [--transfer copy|pinned|mapped|direct] [--output checksum|columns] [--columns-to-host]
//          [--validate] [--reps N] [--warmup W] [--block B] [--device D]
//          [--gpu-cache DIR] [--gpu-cold] [--cpu-check] [--dump-source FILE] [--dump-columns PREFIX]
//
// --dump-columns writes the decoded fields as one array per field (PREFIX.<field>.u<bits>, global
// node order: rank 0's nodes, then rank 1's, ...), e.g. for numpy.fromfile; implies columns output.
//
// Transfers (each repetition moves the trace again unless noted):
//   copy    cuMemcpyHtoD from the pageable mapping (the driver stages it)
//   pinned  the mapping is page-locked once (cuMemHostRegister, reported as setup), then DMA
//   mapped  page-locked and mapped: the kernel reads host memory over the interconnect, no copy
//   direct  the kernel reads the pageable mapping itself (devices with pageable memory access,
//           e.g. GB10's coherent unified memory), no copy

#include "binary_reading/TraceFileView.hpp"
#include "binary_reading/TraceReader.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "binary_reading/decoding/gpu/GpuPlan.hpp"
#include "binary_reading/decoding/gpu/GpuRuntime.hpp"
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gpu = traceschema::gpu;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

[[noreturn]] void usage(const std::string& message) {
    std::cerr << "gpu_decode_bench: " << message
              << "\nusage: gpu_decode_bench --trace FILE [--kernel generic|specialized] [--fields sim|scalar]"
                 " [--transfer copy|pinned|mapped|direct] [--output checksum|columns] [--columns-to-host]"
                 " [--validate] [--reps N] [--warmup W] [--block B] [--device D] [--gpu-cache DIR] [--gpu-cold]"
                 " [--cpu-check] [--dump-source FILE]\n";
    std::exit(2);
}

struct Options {
    std::string trace, kernel = "specialized", fields = "sim", transfer = "copy", output = "checksum";
    std::string gpu_cache, dump_source, dump_columns;
    bool columns_to_host = false, validate = false, gpu_cold = false, cpu_check = false;
    int reps = 10, warmup = 1, block = 256, device = 0;
};

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage("missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--trace") options.trace = value();
        else if (arg == "--kernel") options.kernel = value();
        else if (arg == "--fields") options.fields = value();
        else if (arg == "--transfer") options.transfer = value();
        else if (arg == "--output") options.output = value();
        else if (arg == "--columns-to-host") options.columns_to_host = true;
        else if (arg == "--validate") options.validate = true;
        else if (arg == "--reps") options.reps = std::stoi(value());
        else if (arg == "--warmup") options.warmup = std::stoi(value());
        else if (arg == "--block") options.block = std::stoi(value());
        else if (arg == "--device") options.device = std::stoi(value());
        else if (arg == "--gpu-cache") options.gpu_cache = value();
        else if (arg == "--gpu-cold") options.gpu_cold = true;
        else if (arg == "--cpu-check") options.cpu_check = true;
        else if (arg == "--dump-source") options.dump_source = value();
        else if (arg == "--dump-columns") options.dump_columns = value();
        else usage("unknown argument " + arg);
    }
    if (options.trace.empty()) usage("--trace is required");
    if (options.kernel != "generic" && options.kernel != "specialized") usage("bad --kernel");
    if (options.fields != "sim" && options.fields != "scalar") usage("bad --fields");
    if (options.transfer != "copy" && options.transfer != "pinned" && options.transfer != "mapped" &&
        options.transfer != "direct")
        usage("bad --transfer");
    if (options.output != "checksum" && options.output != "columns") usage("bad --output");
    if (!options.dump_columns.empty()) options.output = "columns", options.columns_to_host = true;
    if (options.columns_to_host && options.output != "columns") usage("--columns-to-host needs --output columns");
    if (options.reps < 1 || options.warmup < 0) usage("bad --reps/--warmup");
    if (options.block < 32 || options.block > 1024 || options.block % 32 != 0) usage("--block must be 32..1024, a multiple of 32");
    return options;
}

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

double median(std::vector<double> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2;
}

std::string hex64(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016" PRIx64, value);
    return buffer;
}

std::string json_string(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    return out + "\"";
}

// A private writable mapping of the file followed by at least one zero page, so that the kernels'
// word loads past the end of the file stay inside mapped memory in the mapped and direct modes.
class HostMapping {
public:
    explicit HostMapping(const std::string& path) {
        const int fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) throw std::runtime_error("cannot open " + path);
        struct stat info{};
        if (fstat(fd, &info) != 0 || info.st_size <= 0) {
            close(fd);
            throw std::runtime_error("cannot stat " + path);
        }
        size_ = static_cast<std::size_t>(info.st_size);
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        reserved_ = (size_ + page - 1) / page * page + page;
        base_ = mmap(nullptr, reserved_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base_ == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("cannot reserve address space");
        }
        if (mmap(base_, size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED) {
            close(fd);
            munmap(base_, reserved_);
            throw std::runtime_error("cannot map " + path);
        }
        close(fd);
    }
    ~HostMapping() { munmap(base_, reserved_); }
    HostMapping(const HostMapping&) = delete;
    HostMapping& operator=(const HostMapping&) = delete;

    [[nodiscard]] std::span<std::byte> bytes() const { return {static_cast<std::byte*>(base_), size_}; }
    [[nodiscard]] void* base() const { return base_; }
    [[nodiscard]] std::size_t reserved() const { return reserved_; }

    // Reads every page so that the first timed copy does not include page faults.
    void touch() const {
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        volatile std::uint8_t sink = 0;
        for (std::size_t offset = 0; offset < size_; offset += page) sink = sink + static_cast<std::uint8_t*>(base_)[offset];
    }

private:
    void* base_ = nullptr;
    std::size_t size_ = 0, reserved_ = 0;
};

// ---------------- CPU reference: decode_bench_micro's AdapterReader in file order ----------------

constexpr std::uint64_t mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ull;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
    return x ^ (x >> 31);
}

template<class List>
std::uint64_t node_hash(std::uint32_t rank, std::uint32_t index, const LogGOPSimNode& n, const List& finish,
                        const List& start, bool lists) {
    std::uint64_t acc = (std::uint64_t{rank} << 32 | index) * 0x9e3779b97f4a7c15ull;
    acc += std::uint64_t{n.Type} * 0xc2b2ae3d27d4eb4full + std::uint64_t{n.Peer} * 0x165667b19e3779f9ull +
           n.Size * 0xd6e8feb86659fd93ull + std::uint64_t{n.Tag} * 0xff51afd7ed558ccdull +
           std::uint64_t{n.Proc} * 0xc4ceb9fe1a85ec53ull + std::uint64_t{n.Nic} * 0x87c37b91114253d5ull +
           std::uint64_t{n.dependency_count} * 0x4cf5ad432745937full + finish.size() * 0x52dce729da3ed4c5ull +
           start.size() * 0x38495ab5c1b5e5a7ull;
    if (lists) {
        for (const auto d : finish) acc += (d + 1ull) * 0x2127599bf4325c37ull;
        for (const auto d : start) acc += (d + 1ull) * 0x880355f21e6d1965ull;
    }
    return mix(acc);
}

struct CpuReference {
    std::string runner;
    std::uint64_t checksum = 0;
    double setup_ms = 0, decode_ms = 0, validate_ms = 0;
};

CpuReference cpu_reference(std::span<std::byte> file, bool lists) {
    CpuReference result;
    result.validate_ms = -1; // legacy files validate their indices inside the reader's constructor (setup_ms)
    if (traceschema::detect_trace_format(file) == traceschema::TraceFormat::SelfDescribing) {
        traceschema::TraceFileView view(file);
        const auto start = Clock::now();
        view.validate_dependency_indices();
        result.validate_ms = ms_since(start);
    }
    auto start = Clock::now();
    const traceschema::LogGOPSimAdapter adapter(file);
    result.setup_ms = ms_since(start);
    result.runner = std::string(adapter.get_runner_name());
    start = Clock::now();
    for (std::uint32_t rank = 0; rank < adapter.get_rank_count(); ++rank)
        for (std::uint32_t index = 0; index < adapter.get_node_count(rank); ++index) {
            const auto n = adapter.read_node(rank, index);
            result.checksum += node_hash(rank, index, n, n.finish_dependents, n.start_dependents, lists);
        }
    result.decode_ms = ms_since(start);
    return result;
}

// ---------------- GPU ----------------

class Event {
public:
    Event() { gpu::check(cuEventCreate(&event_, CU_EVENT_DEFAULT), "cuEventCreate"); }
    ~Event() { cuEventDestroy(event_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void record() { gpu::check(cuEventRecord(event_, nullptr), "cuEventRecord"); }
    [[nodiscard]] CUevent get() const { return event_; }
    [[nodiscard]] float since(const Event& start) const {
        float ms = 0;
        gpu::check(cuEventElapsedTime(&ms, start.event_, event_), "cuEventElapsedTime");
        return ms;
    }

private:
    CUevent event_ = nullptr;
};

struct Rep {
    bool warmup;
    double h2d_ms, kernel_ms, validate_ms, d2h_ms, total_ms;
    std::uint64_t checksum, invalid_records, invalid_dependencies;
};

constexpr std::uint32_t column_sizes[gpu::RoleCount] = {1, 4, 8, 4, 1, 1, 4, 4, 4, 4, 4};
constexpr const char* role_names[gpu::RoleCount] = {"type", "peer", "size", "tag", "cpu", "nic", "depcnt",
                                                    "finish_begin", "finish_count", "start_begin", "start_count"};

int run(const Options& options) {
    const auto process_start = Clock::now();
    HostMapping mapping(options.trace);
    const auto file = mapping.bytes();

    auto start = Clock::now();
    const bool legacy = traceschema::detect_trace_format(file) == traceschema::TraceFormat::Legacy;
    const auto image = legacy ? gpu::create_legacy_image(file) : gpu::create_image(file);
    const auto plan = legacy ? gpu::create_legacy_plan() : [&] {
        const auto header = traceschema::read_binary_header(file);
        return gpu::create_plan(header.layout, header.node_index_bits);
    }();
    const double plan_ms = ms_since(start);

    start = Clock::now();
    gpu::Context context(options.device);
    const double context_ms = ms_since(start);
    if (options.transfer == "direct" && !context.pageable_memory_access)
        throw std::runtime_error("--transfer direct needs a device with pageable memory access (e.g. GB10)");
    if (options.transfer == "mapped" && !context.can_map_host_memory)
        throw std::runtime_error("--transfer mapped: the device cannot map host memory");

    // Compile (or load from the cache) and load the module.
    fs::path cache = options.gpu_cache.empty() ? gpu::get_cache_directory() : fs::path(options.gpu_cache);
    fs::path cold_cache;
    if (options.gpu_cold) {
        char pattern[] = "/tmp/lgs-gpu-cold-XXXXXX";
        if (!mkdtemp(pattern)) throw std::runtime_error("mkdtemp failed");
        cold_cache = cache = pattern;
    }
    start = Clock::now();
    const std::string source =
        options.kernel == "generic" ? gpu::get_generic_source() : gpu::generate_specialized_source(plan);
    const double generate_ms = ms_since(start);
    if (!options.dump_source.empty()) std::ofstream(options.dump_source) << source;
    const auto compiled = gpu::compile(source, context, cache);
    if (!cold_cache.empty()) fs::remove_all(cold_cache);
    const gpu::Module module(compiled);
    const CUfunction decode = module.get_function("lgs_decode");
    const CUfunction validate = module.get_function("lgs_validate");
    if (options.kernel == "generic") {
        std::size_t bytes = 0;
        const auto global = module.get_global("lgs_plan", &bytes);
        if (bytes != sizeof plan) throw std::logic_error("device Plan has a different size");
        gpu::check(cuMemcpyHtoD(global, &plan, sizeof plan), "copy plan");
    }

    // Device buffers. The trace is padded with zeros for the kernels' word loads.
    start = Clock::now();
    const std::size_t padded = (file.size() + gpu::tail_padding_bytes + 7) / 8 * 8;
    gpu::DeviceBuffer trace_buffer;
    if (options.transfer == "copy" || options.transfer == "pinned") {
        trace_buffer = gpu::DeviceBuffer(padded);
        gpu::check(cuMemsetD8(trace_buffer.get() + file.size(), 0, padded - file.size()), "clear tail");
    }
    gpu::DeviceBuffer ranks(image.ranks.size() * sizeof(gpu::Rank));
    if (!image.ranks.empty()) gpu::check(cuMemcpyHtoD(ranks.get(), image.ranks.data(), ranks.size()), "copy ranks");
    gpu::DeviceBuffer result(3 * sizeof(std::uint64_t));
    std::vector<gpu::DeviceBuffer> columns;
    std::array<CUdeviceptr, gpu::RoleCount> column_pointers{};
    if (options.output == "columns")
        for (std::uint32_t role = 0; role < gpu::RoleCount; ++role) {
            columns.emplace_back(std::max<std::size_t>(1, image.node_count * column_sizes[role]));
            column_pointers[role] = columns.back().get();
        }
    gpu::DeviceBuffer column_table(sizeof column_pointers);
    gpu::check(cuMemcpyHtoD(column_table.get(), column_pointers.data(), sizeof column_pointers), "copy columns");
    std::size_t column_bytes = 0;
    for (const auto& column : columns) column_bytes += column.size();
    // Page-locked if the system allows that much (WSL caps it), else ordinary memory.
    void* host_columns = nullptr;
    std::vector<std::byte> pageable_columns;
    bool pinned_columns = false;
    if (options.columns_to_host) {
        if (cuMemAllocHost(&host_columns, std::max<std::size_t>(1, column_bytes)) == CUDA_SUCCESS) pinned_columns = true;
        else {
            pageable_columns.resize(std::max<std::size_t>(1, column_bytes));
            host_columns = pageable_columns.data();
        }
    }
    const double alloc_ms = ms_since(start);

    // Host side of the transfer.
    mapping.touch();
    double register_ms = 0;
    bool registered = false;
    CUdeviceptr words = trace_buffer.get();
    if (options.transfer == "pinned" || options.transfer == "mapped") {
        int read_only = 0;
        cuDeviceGetAttribute(&read_only, CU_DEVICE_ATTRIBUTE_READ_ONLY_HOST_REGISTER_SUPPORTED, context.device);
        unsigned flags = options.transfer == "mapped" ? CU_MEMHOSTREGISTER_DEVICEMAP : 0;
        if (read_only) flags |= CU_MEMHOSTREGISTER_READ_ONLY; // avoids copy-on-write of the private mapping
        start = Clock::now();
        gpu::check(cuMemHostRegister(mapping.base(), mapping.reserved(), flags), "cuMemHostRegister");
        register_ms = ms_since(start);
        registered = true;
        if (options.transfer == "mapped")
            gpu::check(cuMemHostGetDevicePointer(&words, mapping.base(), 0), "cuMemHostGetDevicePointer");
    } else if (options.transfer == "direct") {
        words = reinterpret_cast<CUdeviceptr>(mapping.base());
    }

    const bool copies = options.transfer == "copy" || options.transfer == "pinned";
    const std::uint32_t rank_count = static_cast<std::uint32_t>(image.ranks.size());
    const std::uint64_t node_total = image.node_count;
    const std::uint64_t dependency_total = image.dependency_count;
    const std::uint32_t flags = (options.fields == "sim" ? 1u : 0u) | (options.output == "columns" ? 2u : 0u);
    const std::uint32_t bits = image.node_index_bits;
    CUdeviceptr ranks_pointer = ranks.get(), result_pointer = result.get(), columns_pointer = column_table.get();
    void* decode_args[] = {&words, &ranks_pointer, const_cast<std::uint32_t*>(&rank_count),
                           const_cast<std::uint64_t*>(&node_total), const_cast<std::uint32_t*>(&flags),
                           &result_pointer, &columns_pointer};
    void* validate_args[] = {&words, &ranks_pointer, const_cast<std::uint32_t*>(&rank_count),
                             const_cast<std::uint64_t*>(&dependency_total), const_cast<std::uint32_t*>(&bits),
                             &result_pointer};
    const auto block = static_cast<unsigned>(options.block);
    const auto blocks = [&](std::uint64_t n) { return static_cast<unsigned>((n + block - 1) / block); };
    int registers = 0;
    cuFuncGetAttribute(&registers, CU_FUNC_ATTRIBUTE_NUM_REGS, decode);

    Event e0, e1, e2, e3, e4;
    std::vector<Rep> reps;
    for (int iteration = 0; iteration < options.warmup + options.reps; ++iteration) {
        const auto wall = Clock::now();
        gpu::check(cuMemsetD8Async(result.get(), 0, result.size(), nullptr), "clear result");
        e0.record();
        if (copies) gpu::check(cuMemcpyHtoDAsync(trace_buffer.get(), file.data(), file.size(), nullptr), "copy trace");
        e1.record();
        if (node_total)
            gpu::check(cuLaunchKernel(decode, blocks(node_total), 1, 1, block, 1, 1, 0, nullptr, decode_args, nullptr),
                       "launch lgs_decode");
        e2.record();
        if (options.validate && dependency_total)
            gpu::check(cuLaunchKernel(validate, blocks(dependency_total), 1, 1, block, 1, 1, 0, nullptr, validate_args,
                                      nullptr),
                       "launch lgs_validate");
        e3.record();
        if (options.columns_to_host) {
            auto* target = static_cast<std::byte*>(host_columns);
            for (const auto& column : columns) {
                gpu::check(cuMemcpyDtoHAsync(target, column.get(), column.size(), nullptr), "copy column");
                target += column.size();
            }
        }
        std::uint64_t values[3] = {};
        gpu::check(cuMemcpyDtoH(values, result.get(), sizeof values), "copy result");
        e4.record();
        gpu::check(cuEventSynchronize(e4.get()), "cuEventSynchronize");
        reps.push_back({iteration < options.warmup, e1.since(e0), e2.since(e1), e3.since(e2), e4.since(e3),
                        ms_since(wall), values[0], values[1], values[2]});
    }
    if (registered) cuMemHostUnregister(mapping.base());
    if (!options.dump_columns.empty()) { // one little-endian array per field: PREFIX.<field>.u<bits>
        const auto* source = static_cast<const char*>(host_columns);
        for (std::uint32_t role = 0; role < gpu::RoleCount; ++role) {
            const auto bytes = static_cast<std::streamsize>(node_total * column_sizes[role]);
            std::ofstream(options.dump_columns + "." + role_names[role] + ".u" + std::to_string(8 * column_sizes[role]),
                          std::ios::binary).write(source, bytes);
            source += columns[role].size();
        }
    }
    if (pinned_columns) cuMemFreeHost(host_columns);

    const auto& first = reps.back();
    for (const auto& rep : reps)
        if (rep.checksum != first.checksum) throw std::runtime_error("checksum differs between repetitions");
    std::vector<double> h2d, kernel, validate_times, d2h, total;
    for (const auto& rep : reps)
        if (!rep.warmup) {
            h2d.push_back(rep.h2d_ms);
            kernel.push_back(rep.kernel_ms);
            validate_times.push_back(rep.validate_ms);
            d2h.push_back(rep.d2h_ms);
            total.push_back(rep.total_ms);
        }

    CpuReference cpu;
    if (options.cpu_check) cpu = cpu_reference(file, options.fields == "sim");

    const double n = static_cast<double>(std::max<std::uint64_t>(1, node_total));
    const double kernel_ms = median(kernel), h2d_ms = median(h2d), total_ms = median(total);
    std::ostringstream out;
    out << "{\"tool\":\"gpu_decode_bench\",\"trace\":" << json_string(options.trace)
        << ",\"format\":\"" << (legacy ? "legacy" : "self-describing") << "\",\"kernel\":\"" << options.kernel << "\",\"fields\":\"" << options.fields << "\",\"transfer\":\""
        << options.transfer << "\",\"output\":\"" << options.output << "\",\"columns_to_host\":"
        << (options.columns_to_host ? "true" : "false") << ",\"columns_host_pinned\":" << (pinned_columns ? "true" : "false")
        << ",\"validate\":" << (options.validate ? "true" : "false")
        << ",\"block\":" << options.block << ",\"registers\":" << registers
        << ",\"records\":" << node_total << ",\"dependencies\":" << dependency_total << ",\"ranks\":" << rank_count
        << ",\"record_bytes\":" << image.record_bytes << ",\"node_index_bits\":" << bits
        << ",\"file_bytes\":" << file.size() << ",\"node_record_bytes\":" << image.node_record_bytes
        << ",\"dependency_bytes\":" << image.dependency_bytes << ",\"column_bytes\":" << column_bytes
        << ",\"device\":{\"name\":" << json_string(context.name) << ",\"cc\":\"" << context.major << "." << context.minor
        << "\",\"memory_bytes\":" << context.total_memory << ",\"integrated\":" << (context.integrated ? "true" : "false")
        << ",\"pageable_memory_access\":" << (context.pageable_memory_access ? "true" : "false")
        << ",\"driver\":" << gpu::get_driver_version() << ",\"nvrtc\":\"" << gpu::get_nvrtc_version() << "\"}"
        << ",\"module\":{\"target\":\"" << compiled.target << "\",\"cubin\":" << (compiled.cubin ? "true" : "false")
        << ",\"key\":\"" << compiled.key << "\",\"cache\":\""
        << (cache.empty() ? "off" : compiled.cache_hit ? "hit" : "miss") << "\",\"cold\":"
        << (options.gpu_cold ? "true" : "false") << ",\"source_bytes\":" << source.size()
        << ",\"image_bytes\":" << compiled.image.size() << "}"
        << ",\"setup_ms\":{\"plan\":" << plan_ms << ",\"context\":" << context_ms << ",\"generate\":" << generate_ms
        << ",\"compile\":" << compiled.compile_ms << ",\"module_load\":" << module.get_load_ms()
        << ",\"alloc\":" << alloc_ms << ",\"register\":" << register_ms << "}"
        << ",\"checksum\":\"" << hex64(first.checksum) << "\",\"invalid_records\":" << first.invalid_records
        << ",\"invalid_dependencies\":" << first.invalid_dependencies
        << ",\"median_ms\":{\"h2d\":" << h2d_ms << ",\"kernel\":" << kernel_ms << ",\"validate\":" << median(validate_times)
        << ",\"d2h\":" << median(d2h) << ",\"total\":" << total_ms << "}"
        << ",\"kernel_ns_per_record\":" << kernel_ms * 1e6 / n << ",\"total_ns_per_record\":" << total_ms * 1e6 / n
        << ",\"kernel_mrecords_per_s\":" << (kernel_ms > 0 ? n / (kernel_ms * 1e3) : 0)
        << ",\"kernel_gb_per_s\":" << (kernel_ms > 0 ? image.node_record_bytes / (kernel_ms * 1e6) : 0)
        << ",\"h2d_gb_per_s\":" << (h2d_ms > 0 ? file.size() / (h2d_ms * 1e6) : 0);
    if (options.cpu_check)
        out << ",\"cpu\":{\"runner\":\"" << cpu.runner << "\",\"checksum\":\"" << hex64(cpu.checksum)
            << "\",\"match\":" << (cpu.checksum == first.checksum ? "true" : "false") << ",\"setup_ms\":" << cpu.setup_ms
            << ",\"decode_ms\":" << cpu.decode_ms << ",\"validate_ms\":" << cpu.validate_ms
            << ",\"decode_ns_per_record\":" << cpu.decode_ms * 1e6 / n << "}";
    out << ",\"columns\":[";
    for (std::uint32_t role = 0; role < gpu::RoleCount; ++role)
        out << (role ? "," : "") << "\"" << role_names[role] << ":" << column_sizes[role] << "\"";
    out << "],\"reps\":[";
    for (std::size_t i = 0; i < reps.size(); ++i) {
        const auto& rep = reps[i];
        out << (i ? "," : "") << "{\"warmup\":" << (rep.warmup ? "true" : "false") << ",\"h2d_ms\":" << rep.h2d_ms
            << ",\"kernel_ms\":" << rep.kernel_ms << ",\"validate_ms\":" << rep.validate_ms << ",\"d2h_ms\":" << rep.d2h_ms
            << ",\"total_ms\":" << rep.total_ms << "}";
    }
    out << "],\"process_ms\":" << ms_since(process_start) << "}";
    std::cout << out.str() << std::endl;

    if (first.invalid_records || first.invalid_dependencies) {
        std::cerr << "gpu_decode_bench: " << first.invalid_records << " invalid records, " << first.invalid_dependencies
                  << " dependency indices outside their rank\n";
        return 3;
    }
    if (options.cpu_check && cpu.checksum != first.checksum) {
        std::cerr << "gpu_decode_bench: GPU checksum " << hex64(first.checksum) << " differs from CPU "
                  << hex64(cpu.checksum) << "\n";
        return 4;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    return run(parse(argc, argv));
} catch (const std::exception& error) {
    std::cerr << "gpu_decode_bench: " << error.what() << "\n";
    return 1;
}
