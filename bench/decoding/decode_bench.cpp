// Decoding benchmarks for every registered plan runner on self-describing traces. For each trace
// and runner:
//   Setup/<trace>/<runner>        creating the adapter: header, plan and runner, with a warm JIT cache
//   ColdStart/<trace>/<runner>    a one-shot run: creating the adapter with an empty JIT cache and
//                                 reading every node once
//   ReadNodes/<trace>/<runner>    reading every node once, in order
//   RandomReads/<trace>/<runner>  reading every node once, in a fixed random order
//   Replay/<trace>/<runner>       the simulator's accesses without the network model: a full node
//                                 read when ready, one list per event and a counter update per edge;
//                                 also for the legacy format (runner "legacy"). LogGOPSim traces only
// Traces with ATLAHS message identities (a gpu_id field) are read with the ATLAHS adapter, all
// others with the LogGOPSim one. Traces are <name>.sd.bin files, with an optional
// <name>.legacy.bin next to them; by default those of the bench_traces target, or
// --traces=a.sd.bin,b.sd.bin.

#include "binary_reading/decoding/runners/RunnerRegistry.hpp"
#include "simulators/atlahs/AtlahsAdapter.hpp"
#include "simulators/loggopsim/LogGOPSimAdapter.hpp"
#include "simulators/loggopsim/SimulationSchedule.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <benchmark/benchmark.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using traceschema::AtlahsAdapter;
using traceschema::LogGOPSimAdapter;
using traceschema::RunnerInfo;

struct Trace {
    std::string name;             // file name without .sd.bin
    fs::path packed;              // <name>.sd.bin
    fs::path legacy;              // <name>.legacy.bin, if it exists
    std::vector<std::byte> bytes; // the packed file, read once
    bool atlahs = false;          // has ATLAHS message identities
};

std::vector<std::byte> read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> text{std::istreambuf_iterator<char>(input), {}};
    const auto bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}

// Sets an environment variable for the rest of the scope.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        setenv(name, value.c_str(), 1);
    }
    ~ScopedEnvironment() {
        if (previous_) setenv(name_, previous_->c_str(), 1);
        else unsetenv(name_);
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    const char* name_;
    std::optional<std::string> previous_;
};

// A runner without an instantiation for the trace, or whose compilation failed, makes the
// adapter fall back to plan; such a benchmark is skipped rather than measuring plan.
bool uses_runner(benchmark::State& state, std::string_view actual, std::string_view runner) {
    if (actual == runner) return true;
    state.SkipWithError(std::string(runner) + " is unavailable for this trace");
    return false;
}

template<class Adapter>
std::uint64_t count_nodes(const Adapter& adapter) {
    std::uint64_t nodes = 0;
    for (std::uint32_t rank = 0; rank < adapter.get_rank_count(); ++rank) nodes += adapter.get_node_count(rank);
    return nodes;
}

template<class Adapter>
void read_all_nodes(const Adapter& adapter) {
    for (std::uint32_t rank = 0; rank < adapter.get_rank_count(); ++rank)
        for (std::uint32_t index = 0; index < adapter.get_node_count(rank); ++index) {
            auto node = adapter.read_node(rank, index);
            benchmark::DoNotOptimize(node);
        }
}

void set_time_per_node(benchmark::State& state, std::uint64_t nodes_per_iteration) {
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * nodes_per_iteration));
    state.counters["time_per_node"] = benchmark::Counter(static_cast<double>(state.iterations() * nodes_per_iteration),
                                                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
}

template<class Adapter>
void setup(benchmark::State& state, Trace& trace, const RunnerInfo& runner) {
    const ScopedEnvironment selected("LOGGOPSIM_DECODER", std::string(runner.name));
    { const Adapter warm_up(trace.bytes); } // fills the JIT cache, which ColdStart measures without
    for (auto _ : state) {
        Adapter adapter(trace.bytes);
        benchmark::DoNotOptimize(adapter);
        if (!uses_runner(state, adapter.get_runner_name(), runner.name)) return;
    }
}

// Each iteration compiles again: jit into a new, empty cache directory, orc without reusing the
// functions it compiled before.
template<class Adapter>
void cold_start(benchmark::State& state, Trace& trace, const RunnerInfo& runner) {
    const ScopedEnvironment selected("LOGGOPSIM_DECODER", std::string(runner.name));
    const ScopedEnvironment no_reuse("LOGGOPSIM_ORC_REUSE", "0");
    std::uint64_t nodes = 0;
    std::uint64_t run = 0;
    for (auto _ : state) {
        state.PauseTiming();
        const auto cache = fs::temp_directory_path() / ("decode_bench_cache_" + std::to_string(getpid()) + "_" + std::to_string(run++));
        const ScopedEnvironment empty_cache("LOGGOPSIM_JIT_CACHE", cache.string());
        state.ResumeTiming();
        {
            Adapter adapter(trace.bytes);
            if (!uses_runner(state, adapter.get_runner_name(), runner.name)) return;
            read_all_nodes(adapter);
            nodes = count_nodes(adapter);
        }
        state.PauseTiming();
        fs::remove_all(cache);
        state.ResumeTiming();
    }
    set_time_per_node(state, nodes);
}

template<class Adapter>
void read_nodes(benchmark::State& state, Trace& trace, const RunnerInfo& runner) {
    const ScopedEnvironment selected("LOGGOPSIM_DECODER", std::string(runner.name));
    Adapter adapter(trace.bytes);
    if (!uses_runner(state, adapter.get_runner_name(), runner.name)) return;
    for (auto _ : state) read_all_nodes(adapter);
    set_time_per_node(state, count_nodes(adapter));
}

template<class Adapter>
void random_reads(benchmark::State& state, Trace& trace, const RunnerInfo& runner) {
    const ScopedEnvironment selected("LOGGOPSIM_DECODER", std::string(runner.name));
    Adapter adapter(trace.bytes);
    if (!uses_runner(state, adapter.get_runner_name(), runner.name)) return;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
    for (std::uint32_t rank = 0; rank < adapter.get_rank_count(); ++rank)
        for (std::uint32_t index = 0; index < adapter.get_node_count(rank); ++index) order.emplace_back(rank, index);
    std::shuffle(order.begin(), order.end(), std::mt19937_64(20261106));
    for (auto _ : state)
        for (const auto [rank, index] : order) {
            auto node = adapter.read_node(rank, index);
            benchmark::DoNotOptimize(node);
        }
    set_time_per_node(state, order.size());
}

// Each iteration maps the file privately again, since the replay consumes the dependency counts;
// only the replay itself is timed. runner is "legacy" for the legacy file.
void replay(benchmark::State& state, const fs::path& file, std::string_view runner) {
    const ScopedEnvironment selected("LOGGOPSIM_DECODER", runner == "legacy" ? "plan" : std::string(runner));
    std::uint64_t nodes = 0;
    double setup_seconds = 0;
    for (auto _ : state) {
        const auto load = std::chrono::steady_clock::now();
        SimulationSchedule schedule(file.c_str(), false);
        if (!uses_runner(state, schedule.get_runner_name(), runner)) return;
        const auto start = std::chrono::steady_clock::now();
        nodes = 0;
        std::vector<LogGOPSimNode> ready;
        for (auto& graph : schedule.schedules) {
            graph.GetExecutableNodes_DSN(ready);
            while (!ready.empty()) {
                for (const auto& node : ready) {
                    graph.MarkNodeAsStarted(node.node_index);
                    graph.MarkNodeAsDone(node.node_index);
                    ++nodes;
                }
                graph.GetExecutableNodes_DSN(ready);
            }
        }
        const auto end = std::chrono::steady_clock::now();
        state.SetIterationTime(std::chrono::duration<double>(end - start).count());
        setup_seconds += std::chrono::duration<double>(start - load).count();
    }
    set_time_per_node(state, nodes);
    state.counters["setup"] = benchmark::Counter(setup_seconds, benchmark::Counter::kAvgIterations);
    state.counters["nodes"] = static_cast<double>(nodes);
}

std::vector<fs::path> find_traces(int& argc, char** argv) {
    std::vector<fs::path> paths;
    constexpr std::string_view flag = "--traces=";
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (!argument.starts_with(flag)) continue;
        for (auto rest = argument.substr(flag.size()); !rest.empty();) {
            const auto comma = rest.find(',');
            paths.emplace_back(rest.substr(0, comma));
            rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        }
        std::copy(argv + index + 1, argv + argc, argv + index);
        --argc;
        --index;
    }
    if (paths.empty() && fs::is_directory(BENCH_TRACE_DIR))
        for (const auto& entry : fs::directory_iterator(BENCH_TRACE_DIR))
            if (entry.path().string().ends_with(".sd.bin")) paths.push_back(entry.path());
    std::ranges::sort(paths);
    return paths;
}

Trace load_trace(const fs::path& path) {
    auto name = path.filename().string();
    name = name.substr(0, name.size() - std::string_view(".sd.bin").size());
    const auto legacy = path.parent_path() / (name + ".legacy.bin");
    Trace trace{name, path, fs::exists(legacy) ? legacy : fs::path{}, read_file(path)};
    trace.atlahs = traceschema::read_binary_header(trace.bytes).layout.find_field("gpu_id") != nullptr;
    return trace;
}

// Skips the benchmark if the runner is unavailable in this build, for example orc without LLVM.
template<class Function>
auto if_available(const RunnerInfo& runner, Function function) {
    return [&runner, function](benchmark::State& state) {
        if (!runner.is_available()) state.SkipWithError(std::string(runner.name) + " is unavailable in this build");
        else function(state);
    };
}

template<class Adapter>
void register_trace(Trace& trace) {
    using Benchmark = void (*)(benchmark::State&, Trace&, const RunnerInfo&);
    const std::pair<std::string_view, Benchmark> kinds[] = {
        {"Setup", setup<Adapter>}, {"ColdStart", cold_start<Adapter>},
        {"ReadNodes", read_nodes<Adapter>}, {"RandomReads", random_reads<Adapter>}};
    for (const auto& [kind, function] : kinds)
        for (const auto& runner : traceschema::get_runners()) {
            auto* registered = benchmark::RegisterBenchmark(
                std::string(kind) + "/" + trace.name + "/" + std::string(runner.name),
                if_available(runner, [&trace, &runner, function = function](benchmark::State& state) {
                    function(state, trace, runner);
                }));
            registered->Unit(benchmark::kMillisecond);
            // A cold start may run a compiler process; a few iterations suffice.
            if (kind == "ColdStart") registered->Iterations(5);
        }
}

void register_replays(Trace& trace) {
    if (!trace.legacy.empty())
        benchmark::RegisterBenchmark("Replay/" + trace.name + "/legacy",
                                     [&trace](benchmark::State& state) { replay(state, trace.legacy, "legacy"); })
            ->UseManualTime()->Unit(benchmark::kMillisecond);
    for (const auto& runner : traceschema::get_runners())
        benchmark::RegisterBenchmark("Replay/" + trace.name + "/" + std::string(runner.name),
                                     if_available(runner, [&trace, &runner](benchmark::State& state) {
                                         replay(state, trace.packed, runner.name);
                                     }))
            ->UseManualTime()->Unit(benchmark::kMillisecond);
}

} // namespace

int main(int argc, char** argv) {
    const auto paths = find_traces(argc, argv);
    if (paths.empty()) {
        std::cerr << "No traces: build the bench_traces target or pass --traces=a.sd.bin,b.sd.bin\n";
        return 1;
    }
    std::vector<Trace> traces;
    traces.reserve(paths.size());
    for (const auto& path : paths) traces.push_back(load_trace(path));
    for (auto& trace : traces) {
        if (trace.atlahs) {
            register_trace<AtlahsAdapter>(trace);
        } else {
            register_trace<LogGOPSimAdapter>(trace);
            register_replays(trace);
        }
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
