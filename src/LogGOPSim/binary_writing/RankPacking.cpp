#include "binary_writing/RankPacking.hpp"
#include "trace_format/BinaryIO.hpp"
#include "goal_parsing/Goal.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace traceschema {

FieldValue get_node_field_value(
    const Node& node, FieldId field, std::uint64_t dependency_appendix_index) {
    switch (field) {
    case FieldId::DependencyCount: return std::uint64_t{node.DependenciesCnt};
    case FieldId::Type: return static_cast<std::uint64_t>(node.Type);
    case FieldId::Peer:
        if (node.Peer == Node::any_source) return Wildcard{};
        return std::uint64_t{node.Peer};
    case FieldId::Size: return node.Size;
    case FieldId::Tag:
        if (node.Tag == Node::any_tag) return Wildcard{};
        return std::uint64_t{node.Tag};
    case FieldId::Cpu: return std::uint64_t{node.Proc};
    case FieldId::Nic: return std::uint64_t{node.Nic};
    case FieldId::CompletionCount: return std::uint64_t{node.DependOnMe.size()};
    case FieldId::CompletionIndex: return dependency_appendix_index;
    case FieldId::StartCount: return std::uint64_t{node.StartDependOnMe.size()};
    case FieldId::StartIndex: return dependency_appendix_index + node.DependOnMe.size();
    }
    throw std::invalid_argument("Unknown standard node field");
}

RankPacker::RankPacker(RecordLayout layout, std::uint32_t node_index_bits,
                       std::span<const FieldValueInfo> custom_fields)
    : record_packer_(std::move(layout)), node_index_bits_(node_index_bits),
      custom_field_count_(custom_fields.size()) {
    if (node_index_bits < 1 || node_index_bits > 64)
        throw std::invalid_argument("Node index width must be 1..64 bits");
    for (const auto& field : get_layout().fields) {
        if (const auto* id = std::get_if<FieldId>(&field.field)) field_sources_.push_back(*id);
        else {
            const auto definition = std::ranges::find(custom_fields, field.field, &FieldValueInfo::field);
            if (definition == custom_fields.end())
                throw std::invalid_argument("Undeclared node field '" + std::get<std::string>(field.field) + "'");
            field_sources_.push_back(static_cast<std::size_t>(definition - custom_fields.begin()));
        }
    }
    field_values_.reserve(field_sources_.size());
}

void RankPacker::collect_field_values(const Node& node, std::uint64_t dependency_appendix_index) {
    if (node.custom_values.size() != custom_field_count_)
        throw std::invalid_argument("Node custom value count does not match declarations");
    field_values_.clear();
    for (std::size_t index = 0; index < field_sources_.size(); ++index) {
        const auto& source = field_sources_[index];
        if (const auto* id = std::get_if<FieldId>(&source)) {
            if (node.Type == std::to_underlying(traceschema::OperationType::Calculation) && (*id == FieldId::Peer || *id == FieldId::Tag))
                field_values_.push_back(get_layout().fields[index].offset_encoding.value_base);
            else field_values_.push_back(get_node_field_value(node, *id, dependency_appendix_index));
        } else field_values_.push_back(node.custom_values[std::get<std::size_t>(source)]);
    }
}

static void validate_node_index_width(std::size_t node_count, std::uint32_t node_index_bits) {
    const auto maximum_node_index = node_count == 0 ? 0 : node_count - 1;
    const auto required_index_bits = calculate_required_bits(maximum_node_index);
    if (node_index_bits < required_index_bits)
        throw std::invalid_argument("Node index width cannot represent this rank");
}

static std::vector<std::byte> allocate_rank_bytes(
    std::size_t node_count, std::size_t record_size_bytes, std::uint64_t index_arrays_size_bytes) {
    std::vector<std::byte> bytes;
    if (index_arrays_size_bytes > bytes.max_size())
        throw std::invalid_argument("Packed rank is too large for a byte buffer");
    const auto available_node_bytes = bytes.max_size() - index_arrays_size_bytes;
    if (node_count > available_node_bytes / record_size_bytes)
        throw std::invalid_argument("Packed rank is too large for a byte buffer");

    const auto total_size_bytes = node_count * record_size_bytes
        + static_cast<std::size_t>(index_arrays_size_bytes);
    bytes.resize(total_size_bytes, std::byte{0});
    return bytes;
}

static PackedRankData allocate_packed_rank(
    std::size_t node_count, std::uint32_t dependency_count, std::uint32_t root_count,
    std::size_t record_size_bytes, std::uint32_t node_index_bits) {
    validate_node_index_width(node_count, node_index_bits);

    const auto appendix_bytes = bit_packing::calculate_index_array_size_bytes(dependency_count, node_index_bits);
    const auto root_bytes = bit_packing::calculate_index_array_size_bytes(root_count, node_index_bits);
    auto bytes = allocate_rank_bytes(node_count, record_size_bytes, appendix_bytes + root_bytes);

    const auto appendix_offset = node_count * record_size_bytes;
    const auto root_indices_offset = appendix_offset + static_cast<std::size_t>(appendix_bytes);
    return {std::move(bytes), appendix_offset, dependency_count, root_indices_offset, root_count};
}

class DependencyAppendixWriter {
public:
    DependencyAppendixWriter(std::span<std::byte> bytes, std::span<Node* const> nodes,
                            std::uint32_t index_bits, std::uint32_t entry_count)
        : bytes_(bytes), nodes_(nodes), index_bits_(index_bits), entry_count_(entry_count) {}

    std::uint64_t get_entry_index() const { return entry_index_; }

    void append(std::span<Node* const> dependents) {
        if (dependents.size() > entry_count_ - entry_index_)
            throw std::invalid_argument("Dependency count does not match the graph");
        for (const auto* dependent : dependents) {
            if (!dependent || dependent->offset >= nodes_.size() || nodes_[dependent->offset] != dependent)
                throw std::invalid_argument("Dependency points outside its rank");
            bit_packing::write_bits(bytes_, entry_index_ * index_bits_, index_bits_, dependent->offset);
            ++entry_index_;
        }
    }

    void validate_complete() const {
        if (entry_index_ != entry_count_)
            throw std::invalid_argument("Dependency count does not match the graph");
    }

private:
    std::span<std::byte> bytes_;
    std::span<Node* const> nodes_;
    std::uint32_t index_bits_;
    std::uint32_t entry_count_;
    std::uint64_t entry_index_ = 0;
};

PackedRankData RankPacker::pack(const Goal& graph) {
    const auto& layout = get_layout();
    const auto node_index_bits = node_index_bits_;
    const auto record_size_bytes = static_cast<std::size_t>(layout.get_record_size_bytes());
    auto packed_rank = allocate_packed_rank(graph.GetNodes().size(), graph.GetDependencyCount(),
        graph.GetRootCount(), record_size_bytes, node_index_bits);

    const auto rank_bytes = std::span(packed_rank.bytes);
    auto remaining_node_records = rank_bytes.first(packed_rank.dependency_appendix_offset);
    const auto dependency_appendix_bytes = rank_bytes.subspan(
        packed_rank.dependency_appendix_offset,
        packed_rank.root_indices_offset - packed_rank.dependency_appendix_offset);
    const auto root_indices_bytes = rank_bytes.subspan(packed_rank.root_indices_offset);
    const auto& nodes = graph.GetNodes();
    DependencyAppendixWriter appendix(
        dependency_appendix_bytes, nodes, node_index_bits, packed_rank.dependency_count);

    std::uint64_t root_bit_offset = 0;
    std::uint64_t node_index = 0;
    for (const auto* node : nodes) {
        if (!node) throw std::invalid_argument("Rank contains a null node");

        if (node->is_root()) {
            bit_packing::write_bits(root_indices_bytes, root_bit_offset, node_index_bits, node_index);
            root_bit_offset += node_index_bits;
        }
        ++node_index;

        collect_field_values(*node, appendix.get_entry_index());
        const auto record_bytes = take_next_bytes(remaining_node_records, record_size_bytes);
        record_packer_.pack_into(field_values_, record_bytes);

        // Completion dependents precede start dependents, matching the record's list indices.
        appendix.append(node->DependOnMe);
        appendix.append(node->StartDependOnMe);
    }
    appendix.validate_complete();
    return packed_rank;
}

} // namespace traceschema
