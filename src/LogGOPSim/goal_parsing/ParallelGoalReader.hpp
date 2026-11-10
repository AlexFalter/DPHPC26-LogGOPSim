#ifndef LOGGOPSIM_PARALLELGOALREADER_HPP
#define LOGGOPSIM_PARALLELGOALREADER_HPP

#include "goal_parsing/Goal.hpp"
#include "goal_parsing/GoalBlockParser.hpp"
#include "trace_format/RecordLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Parses the rank blocks of a GOAL file on several OpenMP threads.
//
// The file is memory-mapped and split after the closing brace of each rank block
// (braces inside comments are skipped). Each block is then parsed independently by the
// same scanner GoalReader uses. Results and errors match a sequential GoalReader pass:
// the reported error is the one of the lowest failing rank.
class ParallelGoalReader {
public:
    struct RankCallbacks {
        // Called concurrently for different ranks; `thread` is in [0, get_thread_count(threads)).
        std::function<void(int thread, const Goal& graph)> analyse;
        // Called for one rank at a time after analyse; optional.
        std::function<void(Goal& graph)> finish;
        // Call finish in rank order (needed when it writes output sequentially).
        bool finish_in_rank_order = false;
    };

    // False for inputs that cannot be memory-mapped, e.g. pipes; use GoalReader for those.
    [[nodiscard]] static bool supports(const std::string& path);
    // Number of threads read_all uses; `threads` <= 0 selects the OpenMP default.
    [[nodiscard]] static int get_thread_count(int threads);

    // Maps the file and parses rank 0, which declares num_ranks.
    ParallelGoalReader(const std::string& path, std::vector<traceschema::FieldValueInfo> custom_fields);
    ~ParallelGoalReader();
    ParallelGoalReader(const ParallelGoalReader&) = delete;
    ParallelGoalReader& operator=(const ParallelGoalReader&) = delete;

    [[nodiscard]] std::uint32_t get_num_ranks() const { return num_ranks_; }
    // Parses every rank once. Rethrows the error of the lowest failing rank.
    void read_all(int threads, const RankCallbacks& callbacks);

private:
    struct MappedFile;
    goal_parsing::CustomFieldTable fields_;
    std::unique_ptr<MappedFile> file_;
    std::string_view text_;
    std::uint32_t num_ranks_ = 0;
    // block_ends_[r] is the offset just past rank r's closing brace.
    std::vector<std::size_t> block_ends_;
    // Text after the last closing brace with the '\n' sentinel the streamed reader adds at
    // end of input, plus padding the scanner may look ahead into.
    std::string tail_;
    std::unique_ptr<Goal> first_rank_;

    void find_block_ends(std::size_t count);
    // Not thread-safe for rank 0, which hands out the graph parsed by the constructor;
    // read_all requests every rank exactly once.
    [[nodiscard]] std::unique_ptr<Goal> read_rank(std::uint32_t rank);
    [[nodiscard]] goal_parsing::ParsedRankBlock parse_block(std::uint32_t index,
                                                            std::uint32_t num_ranks) const;
};

#endif
