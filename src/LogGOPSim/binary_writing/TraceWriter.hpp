#ifndef LOGGOPSIM_TRACEWRITER_HPP
#define LOGGOPSIM_TRACEWRITER_HPP

#include "binary_writing/RankPacking.hpp"

#include <cstdint>
#include <ostream>
#include <vector>

class Goal;

namespace traceschema {

// Output must be an empty, seekable binary stream positioned at byte zero.
// The caller owns the stream and calls finish() after writing every rank in order.
class TraceWriter {
public:
    TraceWriter(std::ostream& output, RecordLayout layout,
                std::uint32_t rank_count, std::uint32_t node_index_bits,
                std::vector<FieldValueInfo> custom_fields = {});
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    void write_rank(const Goal& graph);
    void finish();

private:
    std::ostream& output_;
    RankPacker rank_packer_;
    const std::uint32_t rank_count_;
    std::uint32_t ranks_written_ = 0;
    std::uint64_t rank_table_offset_;
    std::uint64_t next_rank_offset_;
    bool finished_ = false;
};

} // namespace traceschema
#endif
