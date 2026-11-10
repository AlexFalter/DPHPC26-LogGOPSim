#include "binary_reading/SelfDescribingTraceReader.hpp"

#include <limits>
#include <stdexcept>
#include <string>

namespace traceschema {

void SelfDescribingTraceReader::check_peer_range() const {
    for (const auto& field : get_selected_fields()) {
        if (field.field != FieldIdentity{FieldId::Peer}) continue;
        const auto& encoding = field.offset_encoding;
        if (field.type != FieldType::UnsignedInteger || (file_.get_rank_count() != 0 &&
            std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset)) >= file_.get_rank_count()))
            throw std::invalid_argument("Peer range exceeds rank count");
    }
}

const FieldStorageInfo& SelfDescribingTraceReader::require_unsigned_field(const FieldIdentity& identity) const {
    const auto* field = std::visit([&](const auto& id) { return file_.get_layout().find_field(id); }, identity);
    if (!field || field->type != FieldType::UnsignedInteger)
        throw std::invalid_argument("Expected unsigned field '" + schema_helpers::get_field_label(identity) + "'");
    return *field;
}

void SelfDescribingTraceReader::set_dependency_counter(FieldIdentity identity) {
    graph_.emplace(file_, require_unsigned_field(identity));
}

std::uint64_t SelfDescribingTraceReader::decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
    if (!graph_) throw std::invalid_argument("No dependency counter configured");
    return graph_->decrement(rank, index);
}

std::uint64_t SelfDescribingTraceReader::get_resource_count(FieldIdentity identity) const {
    const auto& encoding = require_unsigned_field(identity).offset_encoding;
    const auto maximum = std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset));
    if (maximum == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("Resource count exceeds uint64 capacity");
    return maximum + 1;
}

} // namespace traceschema
