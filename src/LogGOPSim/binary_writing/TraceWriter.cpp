#include "binary_writing/TraceWriter.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "binary_writing/RankPacking.hpp"
#include "goal_parsing/Goal.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <stdexcept>
#include <utility>

namespace traceschema {

static void write_bytes(std::ostream& output, std::span<const std::byte> bytes) {
    if (!std::in_range<std::streamsize>(bytes.size()))
        throw std::invalid_argument("Trace section is too large for stream output");
    if (!bytes.empty())
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Failed to write trace bytes");
}

static void seek_to(std::ostream& output, std::uint64_t offset) {
    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!output) throw std::runtime_error("Failed to seek in trace output");
}

static void reserve_rank_table(std::ostream& output, std::uint32_t rank_count) {
    const std::array<std::byte, 4096> zeros{};
    auto remaining = calculate_rank_table_size_bytes(rank_count);
    while (remaining != 0) {
        const auto chunk_size = static_cast<std::size_t>(std::min(remaining, std::uint64_t{zeros.size()}));
        write_bytes(output, std::span(zeros).first(chunk_size));
        remaining -= chunk_size;
    }
}

TraceWriter::TraceWriter(std::ostream& output, RecordLayout layout,
                         std::uint32_t rank_count, std::uint32_t node_index_bits,
                         std::vector<FieldValueInfo> custom_fields)
    : output_(output), rank_packer_(std::move(layout), node_index_bits, custom_fields),
      rank_count_(rank_count) {
    if (output_.tellp() != std::streampos{0})
        throw std::invalid_argument("Trace output must be seekable and positioned at byte zero");
    const auto header = write_binary_header(rank_packer_.get_layout(), rank_count_, node_index_bits);
    rank_table_offset_ = header.size();
    next_rank_offset_ = rank_table_offset_ + calculate_rank_table_size_bytes(rank_count_);
    if (!std::in_range<std::streamoff>(next_rank_offset_))
        throw std::invalid_argument("Rank table exceeds stream capacity");
    write_bytes(output_, header);
    reserve_rank_table(output_, rank_count_);
}

void TraceWriter::write_rank(const Goal& graph) {
    if (finished_ || ranks_written_ == rank_count_)
        throw std::logic_error("No more ranks expected by trace writer");

    const auto packed = rank_packer_.pack(graph);
    const auto end_offset = next_rank_offset_ + packed.bytes.size();
    if (!std::in_range<std::streamoff>(end_offset))
        throw std::invalid_argument("Trace file exceeds stream capacity");
    const RankTableEntry entry{
        next_rank_offset_, static_cast<std::uint32_t>(graph.GetNodes().size()),
        packed.dependency_count, packed.root_count};

    write_bytes(output_, packed.bytes);
    const auto entry_offset = rank_table_offset_ + calculate_rank_table_size_bytes(ranks_written_);
    seek_to(output_, entry_offset);
    write_bytes(output_, write_rank_table_entry(entry));
    seek_to(output_, end_offset);

    next_rank_offset_ = end_offset;
    ++ranks_written_;
}

void TraceWriter::finish() {
    if (finished_) return;
    if (ranks_written_ != rank_count_)
        throw std::logic_error("Trace writer has not received every rank");
    output_.flush();
    if (!output_) throw std::runtime_error("Failed to flush trace output");
    finished_ = true;
}

} // namespace traceschema
