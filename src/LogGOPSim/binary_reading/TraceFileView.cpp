#include "binary_reading/TraceFileView.hpp"
#include "trace_format/BinaryIO.hpp"

#include <stdexcept>

namespace traceschema {

TraceFileView::TraceFileView(std::span<std::byte> file) : header_(read_binary_header(file)) {
    initialize_ranks(file);
}

static std::span<const std::byte> get_rank_table_bytes(std::span<const std::byte> file,
                                                   const BinaryHeader& header) {
    const auto table_offset = header.rank_table_offset_bytes;
    const auto table_size = calculate_rank_table_size_bytes(header.rank_count);
    if (table_offset > file.size() || table_size > file.size() - table_offset)
        throw std::invalid_argument("Truncated rank table");
    return file.subspan(table_offset, table_size);
}

void TraceFileView::validate_rank_entry(const RankTableEntry& entry, std::uint64_t minimum_offset,
                                     std::size_t file_size) const {
    if (entry.data_offset_bytes < minimum_offset || entry.data_offset_bytes > file_size)
        throw std::invalid_argument("Overlapping or out-of-file rank section");
    if (entry.root_count > entry.node_count)
        throw std::invalid_argument("Rank has more roots than nodes");
    const auto largest_node_index = entry.node_count == 0 ? 0 : entry.node_count - 1;
    if (header_.node_index_bits < calculate_required_bits(largest_node_index))
        throw std::invalid_argument("Node index width cannot represent this rank");
}

TraceFileView::RankSections TraceFileView::get_next_rank_sections(
    std::span<std::byte>& remaining, const RankTableEntry& entry) const {
    const auto record_bytes = header_.layout.get_record_size_bytes();
    if (entry.node_count > remaining.size() / record_bytes)
        throw std::invalid_argument("Truncated node records");

    const auto nodes = take_next_bytes(remaining, std::uint64_t{entry.node_count} * record_bytes);
    const auto dependencies = take_next_bytes(remaining,
        bit_packing::calculate_index_array_size_bytes(entry.dependency_count, header_.node_index_bits));
    const auto roots = take_next_bytes(remaining,
        bit_packing::calculate_index_array_size_bytes(entry.root_count, header_.node_index_bits));
    return {entry, nodes, dependencies, roots};
}

void TraceFileView::initialize_ranks(std::span<std::byte> file) {
    auto table = get_rank_table_bytes(file, header_);
    auto minimum_rank_offset = header_.rank_table_offset_bytes + table.size();
    ranks_.reserve(header_.rank_count);
    for (std::uint32_t index = 0; index < header_.rank_count; ++index) {
        const auto entry = read_rank_table_entry(take_next_bytes(table, rank_table_entry_size_bytes));
        validate_rank_entry(entry, minimum_rank_offset, file.size());

        auto remaining_file = file.subspan(entry.data_offset_bytes);
        ranks_.push_back(get_next_rank_sections(remaining_file, entry));
        minimum_rank_offset = file.size() - remaining_file.size();
    }
}

std::span<std::byte> TraceFileView::get_node_record_bytes(std::uint32_t rank, std::uint32_t index) const {
    const auto& sections = ranks_.at(rank);
    if (index >= sections.entry.node_count) throw std::invalid_argument("Node index outside rank");
    const auto size = header_.layout.get_record_size_bytes();
    return sections.nodes.subspan(std::uint64_t{index} * size, size);
}

std::uint32_t TraceFileView::read_index(std::span<const std::byte> bytes, std::uint32_t index,
                                     std::uint32_t count, std::uint32_t node_count) const {
    if (index >= count) throw std::invalid_argument("Index array position out of bounds");
    const auto value = bit_packing::read_bits(bytes, std::uint64_t{index} * header_.node_index_bits,
                                             header_.node_index_bits);
    if (value >= node_count) throw std::invalid_argument("Dependency/root points outside rank");
    return static_cast<std::uint32_t>(value);
}

NodeIndexView TraceFileView::read_dependents(
    std::uint32_t rank_index, std::uint64_t begin, std::uint64_t count) const {
    const auto& rank = ranks_.at(rank_index);
    if (begin > rank.entry.dependency_count || count > rank.entry.dependency_count - begin)
        throw std::invalid_argument("Dependency list exceeds rank appendix");
    return NodeIndexView::packed(rank.dependencies, static_cast<std::uint32_t>(begin), static_cast<std::uint32_t>(count),
                                 header_.node_index_bits, rank.entry.node_count);
}

std::uint32_t TraceFileView::get_node_count(std::uint32_t rank) const { return ranks_.at(rank).entry.node_count; }
std::uint32_t TraceFileView::get_root_count(std::uint32_t rank) const { return ranks_.at(rank).entry.root_count; }
std::uint32_t TraceFileView::read_root(std::uint32_t rank, std::uint32_t index) const {
    const auto& sections = ranks_.at(rank);
    return read_index(sections.roots, index, sections.entry.root_count, sections.entry.node_count);
}

} // namespace traceschema
