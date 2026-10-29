// Replays the simulator's schedule accesses (3 node reads per node, one counter
// decrement per dependency edge) without the network model.
// Output CSV: repetition,elapsed_ns,nodes,ns_per_node

#include "SimulationSchedule.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

static std::uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return std::uint64_t(t.tv_sec) * 1'000'000'000u + std::uint64_t(t.tv_nsec);
}

int main(int argc, char** argv) try {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s schedule repetitions\n", argv[0]);
        return 1;
    }
    const int repetitions = std::atoi(argv[2]);
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        SimulationSchedule schedule(argv[1], false);
        std::uint64_t nodes = 0;
        const auto start = now_ns();
        for (auto& graph : schedule.schedules)
            for (auto ready = graph.GetExecutableNodes_DSN(); !ready.empty(); ready = graph.GetExecutableNodes_DSN())
                for (const auto& node : ready) {
                    graph.MarkNodeAsStarted(node.offset);
                    graph.MarkNodeAsDone(node.offset);
                    ++nodes;
                }
        const auto elapsed = now_ns() - start;
        std::printf("%d,%llu,%llu,%.2f\n", repetition, (unsigned long long)elapsed,
                    (unsigned long long)nodes, double(elapsed) / double(nodes));
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
