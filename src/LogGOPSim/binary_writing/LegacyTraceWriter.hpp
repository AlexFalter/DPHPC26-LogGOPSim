#pragma once

#include <cstdint>

class Goal;

namespace traceschema {

// Writes the original native-endian, fixed-record format, one rank at a time.
class LegacyTraceWriter {
  public:
    LegacyTraceWriter(const char *filename, std::uint32_t rank_count);
    ~LegacyTraceWriter();
    LegacyTraceWriter(const LegacyTraceWriter &) = delete;
    LegacyTraceWriter &operator=(const LegacyTraceWriter &) = delete;

    void write_rank(const Goal &graph);
    void finish();

  private:
    int fd_;
    std::uint32_t rank_count_;
    std::uint32_t ranks_written_ = 0;
    std::uint64_t file_size_;
    std::uint8_t max_cpu_ = 0;
    std::uint8_t max_nic_ = 0;
};

} // namespace traceschema
