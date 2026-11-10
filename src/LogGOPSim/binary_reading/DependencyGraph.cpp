#include "binary_reading/DependencyGraph.hpp"
#include "trace_format/BinaryIO.hpp"

#include <stdexcept>
#include <variant>

namespace traceschema {

const std::vector<FieldBinding<DependencyGraph::ListPositions>>& DependencyGraph::get_list_bindings() {
    static const std::vector bindings{
        bind_field(FieldId::FinishDependentsIndex, &ListPositions::on_finish_begin),
        bind_field(FieldId::FinishDependentsCount, &ListPositions::on_finish_count),
        bind_field(FieldId::StartDependentsIndex, &ListPositions::on_start_begin),
        bind_field(FieldId::StartDependentsCount, &ListPositions::on_start_count)};
    return bindings;
}

DependencyGraph::DependencyGraph(TraceFileView& file, const FieldStorageInfo& count_field)
    : file_(file), count_field_(count_field) {
    // With base zero and no wildcard, the stored code is the count itself.
    if (count_field.type != FieldType::UnsignedInteger ||
        std::get<std::uint64_t>(count_field.offset_encoding.value_base) != 0 ||
        count_field.offset_encoding.has_wildcard)
        throw std::invalid_argument("Dependency counter must include zero and have no wildcard");
    file.validate_dependency_indices();
    std::array<FieldSource, 4> fields{};
    const auto& bindings = get_list_bindings();
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto* field = std::visit([&](const auto& id) { return file.get_layout().find_field(id); },
                                       bindings[index].requirement.field);
        if (!field) return; // no lists to read; read_dependents rejects the call
        if (field->type != FieldType::UnsignedInteger || field->offset_encoding.has_wildcard)
            throw std::invalid_argument("Dependency list fields must be unsigned without wildcard");
        fields[index] = FieldSource::create(*field);
    }
    list_fields_ = fields;
}

// FieldSource prepares shifts, masks and load extents once at setup. Constant fields need
// no record access; mapping tails use bounded reads, as in the full-node decoder.
static std::uint64_t read_list_field(const FieldSource& source, std::span<const std::byte> record,
                                     std::size_t readable_bytes) {
    if (source.mask == 0) return source.base;
    const auto code = source.load(record, readable_bytes);
    if (code > source.maximum_offset) throw std::invalid_argument("Invalid node record");
    return source.base + code;
}

DependencyListView DependencyGraph::read_dependents(
    std::uint32_t rank, std::uint32_t index, DependencyEvent event) const {
    if (!list_fields_) throw std::invalid_argument("Trace has no dependency list fields");
    const auto record = file_.get_node_record_bytes(rank, index);
    const auto readable = file_.get_readable_bytes(record);
    const auto first = event == DependencyEvent::Finish ? 0u : 2u;
    const auto& begin = (*list_fields_)[first];
    const auto& count = (*list_fields_)[first + 1];
    // Constant positions need no record loads; get_node_record_bytes above still validates
    // the node, and get_dependents still checks the slice against this rank's appendix.
    if (begin.mask == 0 && count.mask == 0)
        return file_.get_dependents(rank, begin.base, count.base);
    return file_.get_dependents(
        rank, read_list_field(begin, record, readable), read_list_field(count, record, readable));
}

void DependencyGraph::attach_lists(BaseDeserializedGraphNode& node, const ListPositions& lists,
                                   std::uint32_t rank) const {
    node.finish_dependents = file_.get_dependents(rank, lists.on_finish_begin, lists.on_finish_count);
    node.start_dependents = file_.get_dependents(rank, lists.on_start_begin, lists.on_start_count);
}

std::uint64_t DependencyGraph::decrement(std::uint32_t rank, std::uint32_t index) const {
    const auto record = file_.get_node_record_bytes(rank, index);
    const auto count = bit_packing::try_decrement_bits(
        std::span(record.data(), file_.get_readable_bytes(record)), count_field_.bit_offset,
        count_field_.bit_width, count_field_.offset_encoding.maximum_offset);
    if (!count) throw std::invalid_argument("Dependency counter is already zero or invalid");
    return *count;
}

std::uint64_t DependencyGraph::read_count(std::uint32_t rank, std::uint32_t index) const {
    const auto record = file_.get_node_record_bytes(rank, index);
    const auto count = bit_packing::read_bits(std::span(record.data(), file_.get_readable_bytes(record)),
                                              count_field_.bit_offset, count_field_.bit_width);
    if (count > count_field_.offset_encoding.maximum_offset)
        throw std::invalid_argument("Dependency counter is invalid");
    return count;
}

} // namespace traceschema
