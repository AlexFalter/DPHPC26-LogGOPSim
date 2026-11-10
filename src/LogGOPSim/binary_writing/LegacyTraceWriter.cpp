#include "binary_writing/LegacyTraceWriter.hpp"
#include "goal_parsing/Goal.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/LegacyFormat.hpp"
#include "trace_format/BinaryIO.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace traceschema {
namespace {

// Legacy files store consecutive native-endian integers without struct padding.
template <class Integer> void append_native(std::byte *&position, Integer value) {
    write_native(position, value);
    position += sizeof(value);
}

} // namespace

LegacyTraceWriter::LegacyTraceWriter(const char *filename, std::uint32_t rank_count)
    : fd_(open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)), rank_count_(rank_count),
      file_size_(legacy_format::prefix_bytes + std::uint64_t{rank_count} * legacy_format::rank_table_entry_bytes) {
    if (fd_ < 0)
        throw std::runtime_error("Cannot open legacy output file");
    rank_table_.resize(std::size_t{rank_count} * legacy_format::rank_table_entry_bytes);
}

LegacyTraceWriter::~LegacyTraceWriter() {
    if (fd_ >= 0)
        close(fd_);
}

void LegacyTraceWriter::write_at(const std::byte *data, std::size_t size, std::uint64_t offset) {
    while (size != 0) {
        const auto written = pwrite(fd_, data, size, static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            throw std::runtime_error("Cannot write legacy output file");
        data += written;
        size -= static_cast<std::size_t>(written);
        offset += static_cast<std::uint64_t>(written);
    }
}

void LegacyTraceWriter::write_rank(const Goal &graph) {
    if (fd_ < 0)
        throw std::invalid_argument("Legacy output is already finished");
    if (ranks_written_ >= rank_count_ || graph.GetRank() != ranks_written_ ||
        graph.GetNumRanks() != rank_count_)
        throw std::invalid_argument("Legacy output requires ranks in order");

    const auto &nodes = graph.GetNodes();
    const auto root_count = graph.GetRootCount();
    const auto begin = file_size_;
    const auto rank_size = legacy_format::rank_prefix_bytes + std::uint64_t{root_count} * sizeof(std::uint32_t) +
                           nodes.size() * legacy_format::record_bytes +
                           std::uint64_t{graph.GetDependencyCount()} * sizeof(std::uint32_t);
    rank_bytes_.assign(rank_size, std::byte{0});

    auto *position = rank_bytes_.data();
    append_native(position, static_cast<std::uint32_t>(nodes.size()));
    append_native(position, root_count);
    for (const auto *node : nodes)
        if (node->is_root())
            append_native(position, node->node_index);

    std::uint32_t dependency_index = 0;
    for (const auto *node : nodes) {
        const auto store = [&](std::size_t offset, auto value) { write_native(position + offset, value); };
        store(legacy_format::dependency_count_offset, node->dependency_count);
        store(legacy_format::type_offset, node->Type);
        store(legacy_format::peer_offset, node->Peer);
        store(legacy_format::size_offset, node->Size);
        store(legacy_format::tag_offset, node->Tag);
        store(legacy_format::cpu_field_offset, node->Proc);
        store(legacy_format::nic_field_offset, node->Nic);
        const auto finish_count = static_cast<std::uint32_t>(node->finish_dependents.size());
        const auto start_count = static_cast<std::uint32_t>(node->start_dependents.size());
        store(legacy_format::finish_count_offset, finish_count);
        store(legacy_format::finish_begin_offset, dependency_index);
        dependency_index += finish_count;
        store(legacy_format::start_count_offset, start_count);
        store(legacy_format::start_begin_offset, dependency_index);
        dependency_index += start_count;
        position += legacy_format::record_bytes;
        max_cpu_ = std::max(max_cpu_, node->Proc);
        max_nic_ = std::max(max_nic_, node->Nic);
    }
    for (const auto *node : nodes) {
        for (const auto *dependent : node->finish_dependents)
            append_native(position, dependent->node_index);
        for (const auto *dependent : node->start_dependents)
            append_native(position, dependent->node_index);
    }
    write_at(rank_bytes_.data(), rank_bytes_.size(), begin);
    file_size_ += rank_size;

    // Legacy rank-table offsets are relative to the bytes after the magic.
    position = rank_table_.data() + std::size_t{ranks_written_} * legacy_format::rank_table_entry_bytes;
    append_native(position, begin - sizeof(std::uint64_t));
    append_native(position, file_size_ - sizeof(std::uint64_t));
    ++ranks_written_;
}

void LegacyTraceWriter::finish() {
    if (fd_ < 0)
        throw std::invalid_argument("Legacy output is already finished");
    if (ranks_written_ != rank_count_)
        throw std::invalid_argument("Not all legacy ranks were written");
    std::byte header[legacy_format::prefix_bytes];
    auto *position = header;
    append_native(position, legacy_format_magic);
    append_native(position, rank_count_);
    append_native(position, max_cpu_);
    append_native(position, max_nic_);
    write_at(header, sizeof(header), 0);
    write_at(rank_table_.data(), rank_table_.size(), sizeof(header));
    const auto result = close(fd_);
    fd_ = -1;
    if (result != 0)
        throw std::runtime_error("Cannot close legacy output file");
    sync();
}

} // namespace traceschema
