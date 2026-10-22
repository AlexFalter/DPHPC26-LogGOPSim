#include "trace_format/FieldIdentity.hpp"

#include <algorithm>
#include <stdexcept>

namespace traceschema::schema_helpers {

const char* get_standard_field_name(FieldId id) {
    const auto field = std::ranges::find(standard_fields, id, &StandardFieldDefinition::id);
    if (field == standard_fields.end()) throw std::invalid_argument("Unsupported field ID");
    return field->name.data();
}

void validate_field_identity(const FieldIdentity& field) {
    if (const auto* id = std::get_if<FieldId>(&field)) {
        get_standard_field_name(*id);
    } else if (std::get<std::string>(field).empty()) {
        throw std::invalid_argument("Custom field name must not be empty");
    }
}

std::string get_field_label(const FieldIdentity& field) {
    if (const auto* id = std::get_if<FieldId>(&field))
        return std::string("standard:") + get_standard_field_name(*id);
    return "custom:" + std::get<std::string>(field);
}

} // namespace traceschema::schema_helpers
