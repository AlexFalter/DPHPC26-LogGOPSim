#include "RecordDecoding.hpp"
#include "goal_parsing/GoalReader.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "goal_parsing/TraceAnalysis.hpp"
#include "binary_writing/RecordPacking.hpp"
#include <sstream>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

using test_helpers::decode_record;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct InputFile {
    std::string path;
    InputFile(std::string path, const std::string& text) : path(std::move(path)) {
        std::ofstream output(this->path, std::ios::binary);
        output << text;
        if (!output) throw std::runtime_error("Cannot write test input");
    }
    ~InputFile() { std::remove(path.c_str()); }
};

static void check_first_rank(const Goal& graph) {
    const auto& nodes = graph.GetNodes();
    check(nodes.size() == 2, "Wrong first-rank node count");
    check(nodes[0]->Type == std::to_underlying(traceschema::OperationType::Calculation) && nodes[0]->Size == 1000 &&
          nodes[0]->Proc == 2 && nodes[0]->DependenciesCnt == 0, "Wrong calculation");
    check(nodes[1]->Type == std::to_underlying(traceschema::OperationType::Send) && nodes[1]->Peer == 1 &&
          nodes[1]->Tag == 42 && nodes[1]->Size == 8 && nodes[1]->Nic == 1 &&
          nodes[1]->Proc == 2 && nodes[1]->DependenciesCnt == 1, "Wrong send");
    check(nodes[0]->DependOnMe.size() == 1 && nodes[0]->DependOnMe[0] == nodes[1],
          "Completion dependency not resolved");
}

static void check_second_rank(const Goal& graph) {
    const auto& nodes = graph.GetNodes();
    check(nodes.size() == 2, "Wrong second-rank node count");
    check(nodes[0]->Type == std::to_underlying(traceschema::OperationType::Receive) && nodes[0]->Peer == Node::any_source &&
          nodes[0]->Tag == Node::any_tag && nodes[0]->Proc == 1, "Wrong wildcard receive");
    check(nodes[1]->Type == std::to_underlying(traceschema::OperationType::Calculation) && nodes[1]->Size == 2000 &&
          nodes[1]->Proc == 0 && nodes[1]->Nic == 0 && nodes[1]->DependenciesCnt == 1,
          "Wrong second-rank calculation");
    check(nodes[0]->StartDependOnMe.size() == 1 && nodes[0]->StartDependOnMe[0] == nodes[1],
          "Start dependency not resolved");
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected test input path");
        const std::string path = argv[1];
        InputFile input(path,
            "num_ranks 2\nrank 0 {\n"
            "l1: calc 1000 cpu 2\n"
            "l2: send 8b to 1 tag 42 cpu 2 nic 1\n"
            "l2 requires l1\n}\nrank 1 {\n"
            "l1: recv 8b from -1 tag -1 cpu 1 nic 1\n"
            "l2: calc 2000\n"
            "l2 irequires l1\n}\n");
        // Interleaving detects shared cursors, buffers, or label tables.
        GoalReader first(path), second(path);
        check(first.get_num_ranks() == 0, "Rank count available before parsing");
        auto first_graph = first.read_rank();
        auto second_graph = second.read_rank();
        check_first_rank(*first_graph);
        check_first_rank(*second_graph);
        check(first.get_num_ranks() == 2 && second.get_num_ranks() == 2, "Wrong rank count");
        first_graph = first.read_rank();
        check_second_rank(*first_graph);
        second_graph = second.read_rank();
        check_second_rank(*second_graph);
        check(!first.read_rank() && !first.read_rank() && !second.read_rank(), "Reader did not finish");
        // A new pass starts at the beginning without resetting global state.
        GoalReader reopened(path);
        check_first_rank(*reopened.read_rank());
        check_second_rank(*reopened.read_rank());
        check(!reopened.read_rank(), "Reopened reader did not finish");

        const std::string label(20000, 'a');
        std::string large = "num_ranks 1\n/*" + std::string(30000, 'x') + "*/\nrank 0 {\n";
        large += label + ": calc 1\n";
        for (int index = 1; index <= 500; ++index)
            large += "l" + std::to_string(index) + ": calc " + std::to_string(index) + "\n";
        large += "l500 requires " + label + "\n}"; // No trailing newline.
        InputFile large_input(path + ".large", large);
        GoalReader buffered(large_input.path);
        auto graph = buffered.read_rank();
        check(graph->GetNodes().size() == 501, "Buffer refill lost nodes");
        check(graph->GetNodes().front()->DependOnMe[0] == graph->GetNodes().back(),
              "Long label was not resolved across buffer refills");
        check(!buffered.read_rank(), "Large input did not finish");

        InputFile empty_input(path + ".empty", "num_ranks 1\nrank 0 {\n}\n");
        GoalReader empty(empty_input.path);
        check(empty.read_rank()->GetNodes().empty() && !empty.read_rank(), "Empty rank failed");

        InputFile indented_input(path + ".indented",
            "  num_ranks 1\n  rank 0 {\n    l1: calc 1\n    l2: calc 2\n"
            "    l2 requires l1\n  }\n");
        GoalReader indented(indented_input.path);
        auto indented_graph = indented.read_rank();
        check(indented_graph->GetNodes().size() == 2 &&
              indented_graph->GetNodes()[1]->DependenciesCnt == 1,
              "Indented GOAL input failed");

        for (const auto& text : {
            "num_ranks 1\nrank 0 {\nl1: calc 1\n", // Missing closing brace.
            "num_ranks 1\nrank 0 {\nl1: send\n}\n", // Syntax error.
            "num_ranks 1\nrank 0 {\nl1 requires missing\n}\n",
            "num_ranks 2\nrank 1 {\n}\n", // Rank order.
            "num_ranks 1\nrank 0 {\nl1: calc 1\nl1: calc 2\n}\n", // Duplicate label.
            "num_ranks 1\nrank 0 {\nl1: calc 18446744073709551616\n}\n"}) {
            InputFile invalid(path + ".invalid", text);
            bool rejected = false;
            try {
                GoalReader reader(invalid.path);
                while (reader.read_rank()) {}
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            check(rejected, "Malformed input accepted");
        }
        std::istringstream declarations(
            "[[fields]]\nname = 'gpu_id'\ntype = 'unsigned'\n"
            "[[fields]]\nname = 'priority'\ntype = 'signed'\n");
        const auto custom_fields = traceschema::read_custom_fields(declarations);
        InputFile custom_input(path + ".custom",
            "num_ranks 2\nrank 0 {\n"
            "a: calc 1 priority -2 cpu 1 gpu_id 3\n"
            "b: send 8b to 1 gpu_id 7 nic 2 priority 4 tag 42\n"
            "b requires a\n}\nrank 1 {\n"
            "a: recv 8b from 0 priority -1 tag 42 gpu_id 0\n}\n");
        GoalReader custom_reader(custom_input.path, custom_fields);
        traceschema::TraceAnalysis custom_statistics(custom_fields);
        std::vector<std::vector<traceschema::FieldValue>> custom_values;
        while (auto graph = custom_reader.read_rank()) {
            for (const auto* node : graph->GetNodes()) {
                custom_statistics.observe_custom_values(node->custom_values);
                custom_values.push_back(node->custom_values);
            }
            custom_statistics.finish_rank(graph->GetNodes().size(), 0, 0);
        }
        check(std::get<std::uint64_t>(custom_values[0][0]) == 3 &&
              std::get<std::int64_t>(custom_values[0][1]) == -2,
              "Node custom values are not in declaration order");
        const auto custom_schema = custom_statistics.create_schema(2);
        check(custom_schema.find_field("gpu_id")->bit_width == 3 &&
              custom_schema.find_field("priority")->bit_width == 3,
              "Custom field ranges were not inferred across ranks");
        const traceschema::RecordPacker custom_packer(traceschema::create_packed_schema({
            *custom_schema.find_field("gpu_id"), *custom_schema.find_field("priority")}));
        for (const auto& row : custom_values) {
            traceschema::RecordValues values;
            for (const auto& value : row) values.push_back(value);
            check(decode_record(custom_packer.get_layout(), custom_packer.pack(values)) == values,
                  "Parsed custom values failed packing round trip");
        }
        for (const auto& suffix : {
            "gpu_id 1", "gpu_id 1 priority 0 gpu_id 2", "gpu_id -1 priority 0",
            "gpu_id 1 priority 9223372036854775808", "gpu_id 18446744073709551616 priority 0",
            "gpu_id 1 priority 0 unknown 3"}) {
            InputFile invalid(path + ".badcustom", "num_ranks 1\nrank 0 {\na: calc 1 " +
                              std::string(suffix) + "\n}\n");
            bool rejected = false;
            try { GoalReader reader(invalid.path, custom_fields); (void)reader.read_rank(); }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "Invalid custom attribute accepted");
        }
        InputFile extremes(path + ".extremes", "num_ranks 1\nrank 0 {\n"
            "a: calc 1 priority -9223372036854775808 gpu_id 18446744073709551615\n"
            "b: calc 1 gpu_id 0 priority 9223372036854775807\n}\n");
        GoalReader extremes_reader(extremes.path, custom_fields);
        traceschema::TraceAnalysis extreme_statistics(custom_fields);
        auto extremes_graph = extremes_reader.read_rank();
        for (const auto* node : extremes_graph->GetNodes())
            extreme_statistics.observe_custom_values(node->custom_values);
        check(extreme_statistics.create_schema(1).find_field("priority")->bit_width == 64 &&
              extreme_statistics.create_schema(1).find_field("gpu_id")->bit_width == 64,
              "Custom 64-bit endpoints failed");
        const auto long_name = "attribute_" + std::string(20000, 'x');
        const auto long_field = traceschema::FieldValueInfo::create_custom(
            long_name, traceschema::FieldType::UnsignedInteger);
        InputFile long_custom(path + ".longcustom", "num_ranks 1\nrank 0 {\na: calc 1 " +
                              long_name + " 18446744073709551615\n}\n");
        GoalReader long_custom_reader(long_custom.path, {long_field});
        check(std::get<std::uint64_t>(long_custom_reader.read_rank()->GetNodes()[0]->custom_values[0]) ==
              std::numeric_limits<std::uint64_t>::max(), "Custom token refill failed");
        GoalReader after_errors(path);
        check_first_rank(*after_errors.read_rank());
        std::cout << "GOAL reader checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
