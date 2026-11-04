#ifndef LOGGOPSIM_TRACEFILEVIEW_HPP
#define LOGGOPSIM_TRACEFILEVIEW_HPP

#include "trace_format/BinaryFormat.hpp"
#include "trace_format/NodeIndexView.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace traceschema {

// Borrows a writable mapped file. Record/array views refer directly to that mapping.
class TraceFileView {
public:
    explicit TraceFileView(std::span<std::byte> file);

    [[nodiscard]] const RecordLayout& get_layout() const { return header_.layout; }
    [[nodiscard]] std::uint32_t get_rank_count() const { return header_.rank_count; }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const;
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const;
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const;
    [[nodiscard]] std::span<std::byte> get_node_record_bytes(std::uint32_t rank, std::uint32_t index) const;
    [[nodiscard]] NodeIndexView read_dependents(
        std::uint32_t rank, std::uint64_t begin, std::uint64_t count) const;

private:
    struct RankSections {
        RankTableEntry entry;
        std::span<std::byte> nodes;
        std::span<std::byte> dependencies;
        std::span<std::byte> roots;
    };

    BinaryHeader header_;
    std::vector<RankSections> ranks_;

    void initialize_ranks(std::span<std::byte> file);
    void validate_rank_entry(const RankTableEntry& entry, std::uint64_t minimum_offset,
                             std::size_t file_size) const;
    [[nodiscard]] RankSections get_next_rank_sections(std::span<std::byte>& remaining,
                                                      const RankTableEntry& entry) const;
    [[nodiscard]] std::uint32_t read_index(std::span<const std::byte> bytes, std::uint32_t index,
                                         std::uint32_t count, std::uint32_t node_count) const;
};

} // namespace traceschema
#endif
