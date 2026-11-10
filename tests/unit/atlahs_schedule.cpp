#include "lgs/Parser.hpp"
#include "TestHelpers.hpp"
#include "binary_writing/LegacyTraceWriter.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "goal_parsing/Goal.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Two roots join at node 2; starting that join releases node 3.
class AtlahsSchedule : public testing::TestWithParam<bool> {
protected:
    std::string make_input() const {
        Goal graph;
        graph.SetNumRanks(1);
        auto* first = graph.Calc(10, 0, 0);
        auto* second = graph.Calc(20, 0, 0);
        auto* join = graph.Calc(30, 0, 0);
        auto* tail = graph.Calc(40, 0, 0);
        graph.Dependency(join, first);
        graph.Dependency(join, second);
        graph.StartDependency(tail, join);
        if (GetParam()) {
            const test_helpers::TemporaryFile output({}, false);
            traceschema::LegacyTraceWriter writer(output.path().c_str(), 1);
            writer.write_rank(graph);
            writer.finish();
            std::ifstream input(output.path(), std::ios::binary);
            return {std::istreambuf_iterator<char>(input), {}};
        }
        std::vector<traceschema::FieldValueInfo> fields;
        for (const auto* name : {"gpu_id", "peer_gpu_id", "stream_id", "communicator_id",
                                 "channel_id", "sequence_id", "message_kind", "chunk_id"})
            fields.push_back(traceschema::FieldValueInfo::create_custom(
                name, traceschema::FieldType::UnsignedInteger));
        for (auto* node : graph.GetNodes())
            node->custom_values.assign(fields.size(), std::uint64_t{0});
        traceschema::TraceAnalysis analysis(fields);
        analysis.observe_rank(graph);
        std::ostringstream bytes;
        traceschema::TraceWriter writer(bytes, analysis.create_schema(1), 1,
                                        analysis.get_node_index_bits(), fields);
        writer.write_rank(graph);
        writer.finish();
        return bytes.str();
    }
    static RankSchedule::nodelist_t take_ready(RankSchedule& rank) {
        RankSchedule::nodelist_t ready;
        rank.GetExecutableNodes(&ready);
        return ready;
    }
};

TEST_P(AtlahsSchedule, JoinKeepsTheLatestCompletionTimeInEitherOrder) {
    const auto bytes = make_input();
    for (const bool reverse : {false, true}) {
        const test_helpers::TemporaryFile input(bytes);
        Parser parser(input.path(), false);
        auto& rank = parser.schedules.at(0);
        ASSERT_EQ(take_ready(rank).size(), 2u);
        rank.MarkNodeAsDone(reverse ? 1 : 0, reverse ? 50 : 100);
        EXPECT_TRUE(take_ready(rank).empty());
        rank.MarkNodeAsDone(reverse ? 0 : 1, reverse ? 100 : 50);
        auto ready = take_ready(rank);
        ASSERT_EQ(ready.size(), 1u);
        EXPECT_EQ(ready[0].offset, 2u);
        EXPECT_EQ(ready[0].starttime, 100u);
        rank.MarkNodeAsStarted(2);
        ready = take_ready(rank);
        ASSERT_EQ(ready.size(), 1u);
        EXPECT_EQ(ready[0].offset, 3u);
        EXPECT_EQ(ready[0].starttime, 0u);
    }
}

TEST_P(AtlahsSchedule, UntimedCompletionUsesOnlyFinishDependents) {
    const test_helpers::TemporaryFile input(make_input());
    Parser parser(input.path(), false);
    auto& rank = parser.schedules.at(0);
    ASSERT_EQ(take_ready(rank).size(), 2u);
    rank.MarkNodeAsStarted(0);
    EXPECT_TRUE(take_ready(rank).empty());
    rank.MarkNodeAsDone(0);
    EXPECT_TRUE(take_ready(rank).empty());
    rank.MarkNodeAsDone(1);
    auto ready = take_ready(rank);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].offset, 2u);
    EXPECT_EQ(ready[0].starttime, 0u);
    rank.MarkNodeAsDone(2);
    EXPECT_TRUE(take_ready(rank).empty()) << "Finishing must not release start dependents";
    rank.MarkNodeAsStarted(2);
    ready = take_ready(rank);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].offset, 3u);
}

TEST_P(AtlahsSchedule, SharedInputIsConsumedOnlyForSupportedLegacyFiles) {
    const auto bytes = make_input();
    const test_helpers::TemporaryFile input(bytes);
    {
        traceschema::AtlahsAdapter private_input(input.path().c_str());
        EXPECT_THROW(private_input.invalidate_shared_input(), std::logic_error);
        auto buffer = bytes;
        traceschema::AtlahsAdapter borrowed(std::as_writable_bytes(std::span(buffer)));
        EXPECT_THROW(borrowed.invalidate_shared_input(), std::logic_error);
    }
    if (GetParam()) {
        { Parser parser(input.path(), true); }
        EXPECT_THROW(Parser(input.path(), false), std::invalid_argument);
    } else {
        EXPECT_THROW(Parser(input.path(), true), std::invalid_argument);
        EXPECT_NO_THROW(Parser(input.path(), false));
        std::ifstream file(input.path(), std::ios::binary);
        EXPECT_EQ(std::string(std::istreambuf_iterator<char>(file), {}), bytes);
    }
}

INSTANTIATE_TEST_SUITE_P(Formats, AtlahsSchedule, testing::Bool(),
                        [](const auto& info) { return info.param ? "legacy" : "self_describing"; });
} // namespace
