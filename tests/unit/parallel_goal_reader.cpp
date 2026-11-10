// ParallelGoalReader must produce the same graphs, statistics and errors as GoalReader.
#include "TestHelpers.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "goal_parsing/GoalReader.hpp"
#include "goal_parsing/ParallelGoalReader.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using test_helpers::TemporaryFile;
using Fields = std::vector<traceschema::FieldValueInfo>;

namespace {

// Every parsed property of a rank, in a comparable form.
std::string describe(const Goal& graph) {
    std::ostringstream out;
    out << "rank " << graph.GetRank() << '/' << graph.GetNumRanks() << '\n';
    for (const auto* node : graph.GetNodes()) {
        out << int(node->Type) << ' ' << node->Peer << ' ' << node->Tag << ' ' << node->Size << ' '
            << int(node->Proc) << ' ' << int(node->Nic) << ' ' << node->dependency_count << " [";
        for (const auto& value : node->custom_values)
            out << traceschema::schema_helpers::format_field_value(value) << ' ';
        out << "] ->";
        for (const auto* dependent : node->finish_dependents) out << ' ' << dependent->node_index;
        out << " ~>";
        for (const auto* dependent : node->start_dependents) out << ' ' << dependent->node_index;
        out << '\n';
    }
    return out.str();
}

std::string report(const traceschema::TraceAnalysis& statistics, std::uint32_t num_ranks) {
    std::ostringstream out;
    statistics.print_report(out, statistics.create_schema(num_ranks));
    return out.str();
}

struct Result {
    std::vector<std::string> ranks; // In finish order.
    std::string statistics;
    std::string error;              // what() of the reported error, empty on success.
};

Result read_sequential(const std::string& path, const Fields& fields) {
    Result result;
    traceschema::TraceAnalysis statistics(fields);
    try {
        GoalReader reader(path, fields);
        while (auto graph = reader.read_rank()) {
            statistics.observe_rank(*graph);
            result.ranks.push_back(describe(*graph));
        }
        result.statistics = report(statistics, reader.get_num_ranks());
    } catch (const std::invalid_argument& error) {
        result.error = error.what();
    }
    return result;
}

Result read_parallel(const std::string& path, const Fields& fields, int threads, bool ordered) {
    Result result;
    try {
        ParallelGoalReader reader(path, fields);
        std::vector<traceschema::TraceAnalysis> partial(
            ParallelGoalReader::get_thread_count(threads), traceschema::TraceAnalysis(fields));
        reader.read_all(threads, {
            .analyse = [&](int thread, const Goal& graph) { partial.at(thread).observe_rank(graph); },
            .finish = [&](Goal& graph) { result.ranks.push_back(describe(graph)); },
            .finish_in_rank_order = ordered,
        });
        traceschema::TraceAnalysis statistics(fields);
        for (const auto& part : partial) statistics.merge(part);
        result.statistics = report(statistics, reader.get_num_ranks());
    } catch (const std::invalid_argument& error) {
        result.error = error.what();
    }
    return result;
}

void expect_same_as_sequential(const std::string& text, const Fields& fields = {}) {
    TemporaryFile input(text);
    const auto expected = read_sequential(input.path(), fields);
    for (const int threads : {1, 2, 3, 8}) {
        for (const bool ordered : {false, true}) {
            SCOPED_TRACE("threads=" + std::to_string(threads) + (ordered ? ", ordered" : ""));
            auto actual = read_parallel(input.path(), fields, threads, ordered);
            EXPECT_EQ(actual.error, expected.error);
            // Ordered finishing stops at the failing rank, like the sequential pass.
            if (ordered) EXPECT_EQ(actual.ranks, expected.ranks);
            if (!expected.error.empty()) continue;
            EXPECT_EQ(actual.statistics, expected.statistics);
            auto sorted = expected.ranks;
            std::ranges::sort(sorted);
            std::ranges::sort(actual.ranks);
            EXPECT_EQ(actual.ranks, sorted);
        }
    }
}

std::string many_ranks(int ranks) {
    std::string text = "// generated\nnum_ranks " + std::to_string(ranks) + "\n";
    for (int rank = 0; rank < ranks; ++rank) {
        text += "rank " + std::to_string(rank) + " {\n";
        for (int op = 0; op < rank % 7; ++op) {
            const auto peer = std::to_string((rank + op + 1) % ranks);
            text += "s" + std::to_string(op) + ": send " + std::to_string(rank * 100 + op) + "b to " + peer +
                    " tag " + std::to_string(op) + " cpu " + std::to_string(rank % 3) + "\n";
            text += "r" + std::to_string(op) + ": recv 8b from " + (op % 2 ? "-1" : peer) +
                    " tag " + (op % 3 ? "-1" : "5") + " nic " + std::to_string(op % 2) + "\n";
            text += "c" + std::to_string(op) + ": calc " + std::to_string(rank * op) + "\n";
            text += "r" + std::to_string(op) + " requires s" + std::to_string(op) + "\n";
            if (op > 0) text += "c" + std::to_string(op) + " irequires c" + std::to_string(op - 1) + "\n";
        }
        text += "}\n";
    }
    return text;
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

Fields custom_declarations() {
    std::istringstream declarations(
        "[[fields]]\nname = 'gpu_id'\ntype = 'unsigned'\n[[fields]]\nname = 'priority'\ntype = 'signed'\n");
    return traceschema::read_custom_fields(declarations);
}

std::string custom_ranks() {
    std::string text = "num_ranks 16\n";
    for (int rank = 0; rank < 16; ++rank)
        text += "rank " + std::to_string(rank) + " {\na: calc 1 gpu_id " + std::to_string(rank * 3) +
                " priority " + std::to_string(rank % 2 ? -rank : rank * 10) + "\n}\n";
    return text;
}

} // namespace

TEST(ParallelGoalReader, MatchesSequentialReader) {
    expect_same_as_sequential(many_ranks(200));
}

TEST(ParallelGoalReader, HandlesCrlf) {
    expect_same_as_sequential(replace_all(many_ranks(20), "\n", "\r\n"));
}

TEST(ParallelGoalReader, HandlesEdgeLayouts) {
    expect_same_as_sequential("num_ranks 3\nrank 0 {\n}\nrank 1 {\n}\nrank 2 {\n}\n");
    expect_same_as_sequential("num_ranks 2\nrank 0 {\na: calc 1\n}\nrank 1 {\nb: calc 2\n}");
    expect_same_as_sequential("num_ranks 1\nrank 0 {\na: calc 5 cpu 3\n}\n");
    // Content after the last declared rank is ignored.
    expect_same_as_sequential(
        "num_ranks 2\nrank 0 {\n}\nrank 1 {\na: calc 1\n}\nrank 2 {\nnot even valid\n}\n} garbage");
}

TEST(ParallelGoalReader, SkipsBracesInComments) {
    expect_same_as_sequential(
        "/* } header } */\nnum_ranks 3\n// } before rank 0\nrank 0 {\n// }\na: calc 1\n/* } \n } */\n}\n"
        "/*/ } */ rank 1 {\n/**/\nb: calc 7\n}\n//}\nrank 2 {\n/* multi\n line } comment */\nc: calc 3\n}\n");
}

TEST(ParallelGoalReader, MergesCustomFieldStatistics) {
    expect_same_as_sequential(custom_ranks(), custom_declarations());
}

// The parallel reader reports the error of the lowest failing rank, with the same message.
TEST(ParallelGoalReader, ReportsSameErrors) {
    const auto base = many_ranks(40);
    expect_same_as_sequential(replace_all(base, "rank 31 {\n", "rank 31 {\nl1: send\n"));
    expect_same_as_sequential(replace_all(replace_all(base, "rank 33 {\n", "rank 33 {\nx requires y\n"),
        "rank 12 {\n", "rank 12 {\nl1: calc 1 cpu 999\n"));
    expect_same_as_sequential(replace_all(base, "rank 0 {\n", "rank 0 {\nl1: calc 1 bogus 3\n"));
    expect_same_as_sequential(replace_all(base, "rank 5 {\n", "rank 5 {\nq: calc 1\nq: calc 2\n"));
    expect_same_as_sequential(replace_all(base, "rank 20 {\n", "rank 20 {\nz: send 1b to 40\n"));
    expect_same_as_sequential(replace_all(custom_ranks(), "gpu_id 27 ", ""), custom_declarations());
}

TEST(ParallelGoalReader, ReportsSameStructuralErrors) {
    expect_same_as_sequential("num_ranks 3\nrank 0 {\n}\nrank 1 {\nl1: calc 1\n");
    expect_same_as_sequential("num_ranks 3\nrank 0 {\n}\nrank 1 {\n}\n");
    expect_same_as_sequential("num_ranks 3\nrank 0 {\n}\nrank 2 {\n}\nrank 1 {\n}\n");
    expect_same_as_sequential("num_ranks 2\nrank 1 {\n}\n");
    expect_same_as_sequential("rank 0 {\n}\n");
    expect_same_as_sequential("");
    expect_same_as_sequential("num_ranks 2\nrank 0 {\n}\n/* never closed } \nrank 1 {\n}\n");
    expect_same_as_sequential("num_ranks 2\nrank 0 {\na: calc 1 /* } */\n}\nrank 1 {\n}\n");
}

// The sequential reader would apply a second declaration to later ranks; the parallel
// reader parses ranks independently and rejects a changed value.
TEST(ParallelGoalReader, RejectsChangedNumRanks) {
    TemporaryFile changed("num_ranks 2\nrank 0 {\n}\nnum_ranks 3\nrank 1 {\n}\n");
    EXPECT_EQ(read_parallel(changed.path(), {}, 2, false).error,
        "num_ranks is redeclared with a different value inside the schedule");
    TemporaryFile same("num_ranks 2\nrank 0 {\n}\nnum_ranks 2\nrank 1 {\n}\n");
    EXPECT_EQ(read_parallel(same.path(), {}, 2, false).error, "");
}

TEST(ParallelGoalReader, SupportsOnlyRegularFiles) {
    EXPECT_FALSE(ParallelGoalReader::supports("/nonexistent/input.goal"));
    EXPECT_FALSE(ParallelGoalReader::supports("/dev/null"));
}
