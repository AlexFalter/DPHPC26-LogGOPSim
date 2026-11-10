#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class Goal;

namespace traceschema {

// Writes the original native-endian, fixed-record format, one rank at a time. Each rank is
// built in memory and appended with one write; the header and rank table follow in finish().
class LegacyTraceWriter {
  public:
    LegacyTraceWriter(const char *filename, std::uint32_t rank_count);
    ~LegacyTraceWriter();
    LegacyTraceWriter(const LegacyTraceWriter &) = delete;
    LegacyTraceWriter &operator=(const LegacyTraceWriter &) = delete;

    void write_rank(const Goal &graph);
    void finish();

  private:
    void write_at(const std::byte *data, std::size_t size, std::uint64_t offset);

    int fd_;
    std::uint32_t rank_count_;
    std::uint32_t ranks_written_ = 0;
    std::uint64_t file_size_;
    std::uint8_t max_cpu_ = 0;
    std::uint8_t max_nic_ = 0;
    std::vector<std::byte> rank_bytes_;  // reused between ranks
    std::vector<std::byte> rank_table_;  // all entries, written in finish()
};

} // namespace traceschema
