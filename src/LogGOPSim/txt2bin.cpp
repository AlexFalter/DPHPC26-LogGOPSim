#include "goal_parsing/GoalReader.hpp"
#include "goal_parsing/ParallelGoalReader.hpp"
#include "goal_parsing/TraceAnalysis.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "binary_writing/LegacyTraceWriter.hpp"
#include "txt2bin_cmdline.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

static void report_progress(std::uint32_t completed, std::uint32_t total,
                            std::string_view phase, int& last_progress) {
    const auto progress = static_cast<int>(std::round(static_cast<double>(completed) / total * 100));
    if (progress > last_progress) {
        last_progress = progress;
        std::cout << "Progress " << progress << "% - " << phase << " rank "
                  << completed << '/' << total << '\n';
    }
}

template<class ProcessRank>
static void process_ranks(GoalReader& reader, bool show_progress, std::string_view phase,
                          ProcessRank process_rank) {
    int last_progress = 0;
    std::uint32_t rank_count = 0;
    while (auto graph = reader.read_rank()) {
        process_rank(*graph);
        ++rank_count;
        if (show_progress) report_progress(rank_count, reader.get_num_ranks(), phase, last_progress);
    }
}

// First pass: field statistics for the schema; legacy output is written in the same pass.
// Regular files are parsed in parallel (one rank per task, per-thread statistics merged
// at the end); other inputs such as pipes use the streamed reader. Returns num_ranks.
static std::uint32_t run_first_pass(const gengetopt_args_info& args, bool self_describing,
                                    const std::vector<traceschema::FieldValueInfo>& custom_fields,
                                    traceschema::TraceAnalysis& statistics) {
    const std::string_view phase = self_describing ? "analysed" : "converted";
    std::unique_ptr<traceschema::LegacyTraceWriter> legacy_writer;
    const auto write_legacy = [&](const Goal& graph, std::uint32_t num_ranks) {
        if (self_describing) return;
        if (!legacy_writer)
            legacy_writer = std::make_unique<traceschema::LegacyTraceWriter>(args.output_arg, num_ranks);
        legacy_writer->write_rank(graph);
    };
    std::uint32_t num_ranks = 0;
    if (ParallelGoalReader::supports(args.input_arg)) {
        ParallelGoalReader reader(args.input_arg, custom_fields);
        num_ranks = reader.get_num_ranks();
        std::vector<traceschema::TraceAnalysis> partial(
            ParallelGoalReader::get_thread_count(args.threads_arg), statistics);
        int last_progress = 0;
        std::uint32_t finished = 0;
        reader.read_all(args.threads_arg, {
            .analyse = [&](int thread, const Goal& graph) { partial[thread].observe_rank(graph); },
            .finish = [&](Goal& graph) {
                write_legacy(graph, num_ranks);
                if (args.progress_given) report_progress(++finished, num_ranks, phase, last_progress);
            },
            // Legacy ranks are written sequentially, so they must arrive in order.
            .finish_in_rank_order = !self_describing,
        });
        for (const auto& part : partial) statistics.merge(part);
    } else {
        GoalReader reader(args.input_arg, custom_fields);
        process_ranks(reader, args.progress_given, phase, [&](Goal& graph) {
            statistics.observe_rank(graph);
            write_legacy(graph, reader.get_num_ranks());
        });
        num_ranks = reader.get_num_ranks();
    }
    if (legacy_writer) legacy_writer->finish();
    return num_ranks;
}

// An explicitly requested file must exist; the default file is optional, so an installed
// converter without the source tree simply has no custom fields.
static std::vector<traceschema::FieldValueInfo> load_custom_fields(const char* requested_path) {
    const auto* path = requested_path ? requested_path : DEFAULT_CUSTOM_FIELDS_CONFIG_PATH;
    std::ifstream definitions(path);
    if (!definitions) {
        if (!requested_path && !std::filesystem::exists(path)) return {};
        throw std::invalid_argument(std::string("Cannot open custom fields TOML: ") + path);
    }
    return traceschema::read_custom_fields(definitions);
}

int main(int argc, char** argv) try {
    gengetopt_args_info args{};
    if (cmdline_parser(argc, argv, &args) != 0) return EXIT_FAILURE;
    std::error_code path_error;
    if (std::filesystem::equivalent(args.input_arg, args.output_arg, path_error))
        throw std::invalid_argument("Input and output must be different files");
    const bool self_describing = std::string_view(args.format_arg) == "self-describing";
    const auto custom_fields = load_custom_fields(args.custom_fields_given ? args.custom_fields_arg : nullptr);
    if (!self_describing && !custom_fields.empty())
        std::fprintf(stderr, "Custom values are parsed and reported; legacy output does not store them.\n");
    if (args.threads_arg < 0) throw std::invalid_argument("--threads must not be negative");
    traceschema::TraceAnalysis statistics(custom_fields);
    const auto num_ranks = run_first_pass(args, self_describing, custom_fields, statistics);
    const auto schema = statistics.create_schema(num_ranks);
    if (self_describing) {
        GoalReader writing_reader(args.input_arg, custom_fields);
        std::ofstream output(args.output_arg, std::ios::binary | std::ios::trunc);
        if (!output) throw std::invalid_argument("Cannot open binary output file");
        traceschema::TraceWriter writer(output, schema, num_ranks,
                                        statistics.get_node_index_bits(), custom_fields);
        process_ranks(writing_reader, args.progress_given, "written", [&](const Goal& graph) {
            if (writing_reader.get_num_ranks() != num_ranks)
                throw std::invalid_argument("Rank count changed between input passes");
            writer.write_rank(graph);
        });
        writer.finish();
        output.close();
        if (!output) throw std::runtime_error("Failed to close binary output file");
    }
    if (args.report_schema_flag)
        statistics.print_report(std::cout, schema, self_describing
            ? traceschema::ReportedLayoutUsage::UsedInOutput
            : traceschema::ReportedLayoutUsage::ProposedOnly);
    cmdline_parser_free(&args);
    return EXIT_SUCCESS;
} catch (const std::exception& error) {
    std::fprintf(stderr, "txt2bin: %s\n", error.what());
    return EXIT_FAILURE;
}
