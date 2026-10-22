#include "goal_parsing/GoalReader.hpp"
#include "binary_writing/RankPacking.hpp"
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

static void collect_graph_statistics(const Goal& goal, traceschema::TraceAnalysis& schema_statistics) {
    using traceschema::FieldId;
    uint64_t roots = 0, edges = 0;
    for (const auto* node : goal.GetNodes()) {
        schema_statistics.observe_custom_values(node->custom_values);
        for (const auto& definition : traceschema::standard_fields) {
            const auto id = definition.id;
            if (node->Type == std::to_underlying(traceschema::OperationType::Calculation) && (id == FieldId::Peer || id == FieldId::Tag)) continue;
            const auto value = traceschema::get_node_field_value(*node, id, edges);
            if (std::holds_alternative<traceschema::Wildcard>(value)) schema_statistics.observe_wildcard(id);
            else schema_statistics.observe(id, std::get<std::uint64_t>(value));
        }
        if (node->is_root()) ++roots;
        edges += node->DependOnMe.size() + node->StartDependOnMe.size();
    }
    schema_statistics.finish_rank(goal.GetNodes().size(), roots, edges);
}

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
    GoalReader reader(args.input_arg, custom_fields);
    traceschema::TraceAnalysis statistics(custom_fields);
    std::unique_ptr<traceschema::LegacyTraceWriter> legacy_writer;
    process_ranks(reader, args.progress_given, self_describing ? "analysed" : "converted",
        [&](Goal& graph) {
            collect_graph_statistics(graph, statistics);
            if (!self_describing) {
                if (!legacy_writer)
                    legacy_writer = std::make_unique<traceschema::LegacyTraceWriter>(
                        args.output_arg, reader.get_num_ranks());
                legacy_writer->write_rank(graph);
            }
        });
    if (legacy_writer) legacy_writer->finish();
    const auto schema = statistics.create_schema(reader.get_num_ranks());
    if (self_describing) {
        GoalReader writing_reader(args.input_arg, custom_fields);
        std::ofstream output(args.output_arg, std::ios::binary | std::ios::trunc);
        if (!output) throw std::invalid_argument("Cannot open binary output file");
        traceschema::TraceWriter writer(output, schema, reader.get_num_ranks(),
                                        statistics.get_node_index_bits(), custom_fields);
        process_ranks(writing_reader, args.progress_given, "written", [&](const Goal& graph) {
            if (writing_reader.get_num_ranks() != reader.get_num_ranks())
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
