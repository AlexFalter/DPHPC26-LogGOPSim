#ifndef LOGGOPSIM_TRACEFILEVIEW_HPP
#define LOGGOPSIM_TRACEFILEVIEW_HPP

#include "trace_format/BinaryFormat.hpp"
#include "binary_reading/DependencyListView.hpp"

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
    // Mapped bytes from the start of a record to the end of the file; fast decoders may read
    // past the record itself.
    [[nodiscard]] std::size_t get_readable_bytes(std::span<const std::byte> record) const {
        return static_cast<std::size_t>(file_end_ - record.data());
    }
    // Graph setup checks every appendix index once. Dependency storage must
    // remain unchanged afterwards; counter updates touch only node records.
    void validate_dependency_indices();
    // A slice of the rank's dependency list; entries are decoded when iterated. The whole list
    // is checked once by validate_dependency_indices, which must be called first.
    [[nodiscard]] DependencyListView get_dependents(
        std::uint32_t rank, std::uint64_t begin, std::uint64_t count) const;

private:
    struct RankSections {
        RankTableEntry entry;
        std::span<std::byte> nodes;
        std::span<std::byte> dependencies;
        std::span<std::byte> roots;
    };

    BinaryHeader header_;
    std::size_t record_size_bytes_; // fixed by the layout; computed once
    std::vector<RankSections> ranks_;
    const std::byte* file_end_;
    bool dependency_indices_validated_ = false;

    void read_rank_table(std::span<std::byte> file);
    void validate_rank_entry(const RankTableEntry& entry, std::uint64_t minimum_offset,
                             std::size_t file_size) const;
    // Cuts the rank's node records, dependency list and roots off the front of remaining.
    [[nodiscard]] RankSections take_rank_sections(std::span<std::byte>& remaining,
                                                  const RankTableEntry& entry) const;
};

} // namespace traceschema
#endif
