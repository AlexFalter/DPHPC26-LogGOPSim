#ifndef LOGGOPSIM_GOALBLOCKPARSER_HPP
#define LOGGOPSIM_GOALBLOCKPARSER_HPP

#include "goal_parsing/Goal.hpp"
#include "trace_format/RecordLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Building blocks shared by GoalReader (sequential, streamed) and ParallelGoalReader.
// Both use the same re2c scanner from GoalReader.re.
namespace goal_parsing {

// Declared custom fields and their attribute-name lookup; read-only while parsing,
// so one table can serve several threads.
class CustomFieldTable {
public:
    explicit CustomFieldTable(std::vector<traceschema::FieldValueInfo> fields);

    [[nodiscard]] const std::vector<traceschema::FieldValueInfo>& get_fields() const { return fields_; }
    // Declaration index of the named field, or nullptr if it is not declared.
    [[nodiscard]] const std::size_t* find(const std::string& name) const;

private:
    std::vector<traceschema::FieldValueInfo> fields_;
    std::unordered_map<std::string, std::size_t> indices_;
};

// A GOAL syntax error. diagnostic() is the source excerpt with a caret that the
// sequential reader prints to stderr; the parallel reader prints only the one it reports.
class GoalSyntaxError : public std::invalid_argument {
public:
    GoalSyntaxError(const std::string& message, std::string diagnostic)
        : std::invalid_argument(message), diagnostic_(std::move(diagnostic)) {}
    [[nodiscard]] const std::string& diagnostic() const { return diagnostic_; }

private:
    std::string diagnostic_;
};

struct ParsedRankBlock {
    std::unique_ptr<Goal> graph;
    std::int64_t rank;       // From the "rank N {" header.
    std::uint32_t num_ranks; // In effect when the block's closing brace was read.
};

// Parses the text up to and including one rank's closing brace. `text` must either end
// right after that brace or be the end of the input followed by a '\n' sentinel and
// readable padding. `num_ranks` is the value declared before this block (0 if none yet).
// `preceding_text` is the input before `text`; it is only used for line numbers in errors.
[[nodiscard]] ParsedRankBlock parse_rank_block(const CustomFieldTable& fields, std::string_view text,
                                               std::uint32_t num_ranks, std::string_view preceding_text);

} // namespace goal_parsing

#endif
