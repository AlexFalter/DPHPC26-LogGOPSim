// Times txt2bin's first pass (parse every rank, collect field statistics) without output.
// usage: first_pass_bench GOAL_FILE THREADS REPETITIONS [CUSTOM_FIELDS_TOML]
// THREADS = 0 uses the streamed sequential GoalReader (txt2bin before parallelisation);
// THREADS >= 1 uses ParallelGoalReader with that many OpenMP threads.
// Prints CSV rows: repetition,threads,milliseconds,report_hash. Equal hashes mean equal
// statistics; the first repetition warms the page cache.
#include "goal_parsing/CustomFields.hpp"
#include "goal_parsing/GoalReader.hpp"
#include "goal_parsing/ParallelGoalReader.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using traceschema::TraceAnalysis;

static std::string run_first_pass(const std::string& path, int threads,
                                  const std::vector<traceschema::FieldValueInfo>& fields) {
    TraceAnalysis statistics(fields);
    std::uint32_t num_ranks = 0;
    if (threads == 0) {
        GoalReader reader(path, fields);
        while (auto graph = reader.read_rank()) statistics.observe_rank(*graph);
        num_ranks = reader.get_num_ranks();
    } else {
        ParallelGoalReader reader(path, fields);
        num_ranks = reader.get_num_ranks();
        std::vector<TraceAnalysis> partial(ParallelGoalReader::get_thread_count(threads), statistics);
        reader.read_all(threads, {.analyse = [&](int thread, const Goal& graph) {
            partial[thread].observe_rank(graph);
        }});
        for (const auto& part : partial) statistics.merge(part);
    }
    std::ostringstream report;
    statistics.print_report(report, statistics.create_schema(num_ranks));
    return report.str();
}

int main(int argc, char** argv) try {
    if (argc != 4 && argc != 5) {
        std::fprintf(stderr, "usage: %s GOAL_FILE THREADS REPETITIONS [CUSTOM_FIELDS_TOML]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const std::string path = argv[1];
    const int threads = std::atoi(argv[2]);
    const int repetitions = std::atoi(argv[3]);
    std::vector<traceschema::FieldValueInfo> fields;
    if (argc == 5) {
        std::ifstream declarations(argv[4]);
        if (!declarations) throw std::invalid_argument(std::string("Cannot open ") + argv[4]);
        fields = traceschema::read_custom_fields(declarations);
    }
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        const auto start = std::chrono::steady_clock::now();
        const auto report = run_first_pass(path, threads, fields);
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
        std::printf("%d,%d,%.1f,%016zx\n", repetition, threads, elapsed.count(),
                    std::hash<std::string>{}(report));
        std::fflush(stdout);
    }
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::fprintf(stderr, "first_pass_bench: %s\n", error.what());
    return EXIT_FAILURE;
}
