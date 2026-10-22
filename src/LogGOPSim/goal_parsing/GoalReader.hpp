#ifndef LOGGOPSIM_GOALREADER_HPP
#define LOGGOPSIM_GOALREADER_HPP

#include "goal_parsing/Goal.hpp"
#include "trace_format/RecordLayout.hpp"

#include <cstdint>
#include <memory>
#include <string>

class GoalReader {
public:
    explicit GoalReader(const std::string& path,
                        std::vector<traceschema::FieldValueInfo> custom_fields = {});
    ~GoalReader();

    GoalReader(const GoalReader&) = delete;
    GoalReader& operator=(const GoalReader&) = delete;

    // Returns ownership of one rank's graph; nullptr after all declared ranks.
    [[nodiscard]] std::unique_ptr<Goal> read_rank();
    // Available after reading the first rank.
    [[nodiscard]] std::uint32_t get_num_ranks() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
