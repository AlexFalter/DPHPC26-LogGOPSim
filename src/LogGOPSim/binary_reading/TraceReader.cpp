#include "binary_reading/TraceReader.hpp"
#include "trace_format/BinaryIO.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace traceschema {

TraceReader::TraceReader(std::span<std::byte> file, const FieldRequirements& requirements)
    : file_(file), decoder_(file_.get_layout(), requirements) {
    for (const auto& field : get_selected_fields()) {
        if (field.field != FieldIdentity{FieldId::Peer}) continue;
        const auto& encoding = field.offset_encoding;
        if (field.type != FieldType::UnsignedInteger || (file_.get_rank_count() != 0 &&
            std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset)) >= file_.get_rank_count()))
            throw std::invalid_argument("Peer range exceeds rank count");
    }
}

std::vector<FieldValue> TraceReader::read_fields(std::uint32_t rank, std::uint32_t index) const {
    std::vector<FieldValue> values(get_selected_fields().size());
    read_fields_into(rank, index, values);
    return values;
}

void TraceReader::read_fields_into(std::uint32_t rank, std::uint32_t index, std::span<FieldValue> output) const {
    decoder_.decode_fields(file_.get_node_record_bytes(rank, index), output);
}

const FieldStorageInfo& TraceReader::require_unsigned_field(const FieldIdentity& identity) const {
    const auto* field = std::visit([&](const auto& id) { return file_.get_layout().find_field(id); }, identity);
    if (!field || field->type != FieldType::UnsignedInteger)
        throw std::invalid_argument("Expected unsigned field '" + schema_helpers::get_field_label(identity) + "'");
    return *field;
}

void TraceReader::set_dependency_counter(FieldIdentity identity) {
    const auto& field = require_unsigned_field(identity);
    if (std::get<std::uint64_t>(field.offset_encoding.value_base) != 0 || field.offset_encoding.has_wildcard)
        throw std::invalid_argument("Dependency counter must include zero and have no wildcard");
    dependency_counter_ = field;
}

std::uint64_t TraceReader::decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
    if (!dependency_counter_) throw std::invalid_argument("No dependency counter configured");
    const auto& field = *dependency_counter_;
    const auto bytes = file_.get_node_record_bytes(rank, index);
    const auto count = std::get<std::uint64_t>(NodeDecoder::decode_field(bytes, field));
    if (count == 0) throw std::invalid_argument("Dependency counter is already zero");
    const auto code = field.offset_encoding.encode(FieldValue{count - 1});
    bit_packing::write_bits(bytes, field.bit_offset, field.bit_width, code);
    return count - 1;
}

std::uint64_t TraceReader::get_resource_count(FieldIdentity identity) const {
    const auto& encoding = require_unsigned_field(identity).offset_encoding;
    const auto maximum = std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset));
    if (maximum == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("Resource count exceeds uint64 capacity");
    return maximum + 1;
}

const std::vector<FieldBinding<TraceReader::DecodedDependencyFields>>& TraceReader::get_dependency_bindings() {
    static const std::vector fields{
        bind_field(FieldId::DependencyCount, &DecodedDependencyFields::dependency_count),
        bind_field(FieldId::CompletionCount, &DecodedDependencyFields::completion_count),
        bind_field(FieldId::CompletionIndex, &DecodedDependencyFields::completion_begin),
        bind_field(FieldId::StartCount, &DecodedDependencyFields::start_count),
        bind_field(FieldId::StartIndex, &DecodedDependencyFields::start_begin)};
    return fields;
}

void TraceReader::populate_graph_node(BaseDeserializedGraphNode& node, const DecodedDependencyFields& fields,
                                      std::uint32_t rank, std::uint32_t index) const {
    node.offset = index;
    node.DependenciesCnt = fields.dependency_count;
    node.DependOnMe = file_.read_dependents(rank, fields.completion_begin, fields.completion_count);
    node.StartDependOnMe = file_.read_dependents(rank, fields.start_begin, fields.start_count);
}

} // namespace traceschema
