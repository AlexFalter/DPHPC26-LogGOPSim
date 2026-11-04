#include "binary_writing/LegacyTraceWriter.hpp"
#include "goal_parsing/Goal.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace traceschema {
namespace {

// Legacy files store consecutive native-endian integers without struct padding.
template <class Integer> void append_native(char *&position, Integer value) {
    std::memcpy(position, &value, sizeof(value));
    position += sizeof(value);
}

constexpr std::uint64_t legacy_record_bytes = 39;
constexpr std::uint64_t legacy_prefix_bytes =
    sizeof(std::uint64_t) + sizeof(std::uint32_t) + 2 * sizeof(std::uint8_t);

} // namespace

LegacyTraceWriter::LegacyTraceWriter(const char *filename, std::uint32_t rank_count)
    : fd_(open(filename, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR)), rank_count_(rank_count),
      file_size_(legacy_prefix_bytes + std::uint64_t{rank_count} * 2 * sizeof(std::uint64_t)) {
    if (fd_ < 0)
        throw std::runtime_error("Cannot open legacy output file");
}

LegacyTraceWriter::~LegacyTraceWriter() {
    if (fd_ >= 0)
        close(fd_);
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
    file_size_ += 2 * sizeof(std::uint32_t) + std::uint64_t{root_count} * sizeof(std::uint32_t) +
                  nodes.size() * legacy_record_bytes +
                  std::uint64_t{graph.GetDependencyCount()} * sizeof(std::uint32_t);
    if (ftruncate(fd_, file_size_) != 0)
        throw std::runtime_error("Cannot resize legacy output file");
    auto *mapping =
        static_cast<char *>(mmap(nullptr, file_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
    if (mapping == MAP_FAILED)
        throw std::runtime_error("Cannot mmap legacy output file");

    auto *position = mapping + begin;
    append_native(position, static_cast<std::uint32_t>(nodes.size()));
    append_native(position, root_count);
    for (const auto *node : nodes)
        if (node->is_root())
            append_native(position, node->offset);

    std::uint32_t dependency_index = 0;
    for (const auto *node : nodes) {
        append_native(position, node->DependenciesCnt);
        append_native(position, node->Type);
        append_native(position, node->Peer);
        append_native(position, node->Size);
        append_native(position, node->Tag);
        append_native(position, node->Proc);
        append_native(position, node->Nic);
        const auto completion_count = static_cast<std::uint32_t>(node->DependOnMe.size());
        const auto start_count = static_cast<std::uint32_t>(node->StartDependOnMe.size());
        append_native(position, completion_count);
        append_native(position, dependency_index);
        dependency_index += completion_count;
        append_native(position, start_count);
        append_native(position, dependency_index);
        dependency_index += start_count;
        max_cpu_ = std::max(max_cpu_, node->Proc);
        max_nic_ = std::max(max_nic_, node->Nic);
    }
    for (const auto *node : nodes) {
        for (const auto *dependent : node->DependOnMe)
            append_native(position, dependent->offset);
        for (const auto *dependent : node->StartDependOnMe)
            append_native(position, dependent->offset);
    }

    position = mapping;
    append_native(position, legacy_format_magic);
    append_native(position, rank_count_);
    append_native(position, max_cpu_);
    append_native(position, max_nic_);
    position += std::uint64_t{ranks_written_} * 2 * sizeof(std::uint64_t);
    // Legacy rank-table offsets are relative to the bytes after the magic.
    append_native(position, begin - sizeof(std::uint64_t));
    append_native(position, file_size_ - sizeof(std::uint64_t));
    if (munmap(mapping, file_size_) != 0)
        throw std::runtime_error("Cannot unmap legacy output file");
    ++ranks_written_;
}

void LegacyTraceWriter::finish() {
    if (fd_ < 0)
        throw std::invalid_argument("Legacy output is already finished");
    if (ranks_written_ != rank_count_)
        throw std::invalid_argument("Not all legacy ranks were written");
    const auto result = close(fd_);
    fd_ = -1;
    if (result != 0)
        throw std::runtime_error("Cannot close legacy output file");
    sync();
}

} // namespace traceschema
