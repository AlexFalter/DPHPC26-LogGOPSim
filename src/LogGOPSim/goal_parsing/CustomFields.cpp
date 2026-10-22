#include "goal_parsing/CustomFields.hpp"

#include <toml++/toml.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <unordered_set>

namespace traceschema {

static constexpr std::array<std::string_view, 1> allowed_document_properties{"fields"};
static constexpr std::array<std::string_view, 2> allowed_field_properties{"name", "type"};
static constexpr std::array<std::string_view, 9> reserved_goal_keywords{
    "send", "recv", "calc", "rank", "num_ranks", "to", "from", "requires", "irequires"};

static void check_keys(const toml::table& table,
                       std::span<const std::string_view> allowed) {
    for (const auto& [key, value] : table) {
        if (std::ranges::find(allowed, key.str()) == allowed.end())
            throw std::invalid_argument("Unknown custom fields property '" + std::string(key.str()) + "'");
    }
}

static std::string read_string(const toml::table& table, std::string_view key) {
    const auto value = table[key].value<std::string>();
    if (!value) throw std::invalid_argument("Expected a string for '" + std::string(key) + "'");
    return *value;
}

void validate_custom_fields(std::span<const FieldValueInfo> fields) {
    std::unordered_set<std::string> names;
    for (const auto& field : fields) {
        const auto* name = std::get_if<std::string>(&field.field);
        const auto is_ascii_letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        if (!name || name->empty() || !is_ascii_letter(name->front()) ||
            !std::ranges::all_of(*name, [&](char c) { return is_ascii_letter(c) || (c >= '0' && c <= '9') || c == '_'; }))
            throw std::invalid_argument("Custom field names must be GOAL identifiers");
        const auto standard_name = std::ranges::find(standard_fields, *name, &StandardFieldDefinition::name);
        if (standard_name != standard_fields.end() ||
            std::ranges::find(reserved_goal_keywords, *name) != reserved_goal_keywords.end() ||
            !names.insert(*name).second)
            throw std::invalid_argument("Reserved or duplicate custom field name '" + *name + "'");
        if (!schema_helpers::is_supported_field_type(field.type))
            throw std::invalid_argument("Unsupported custom field type");
    }
}

std::vector<FieldValueInfo> read_custom_fields(std::istream& input) {
    try {
        const auto document = toml::parse(input);
        check_keys(document, allowed_document_properties);
        if (document.empty()) return {};
        const auto* fields = document["fields"].as_array();
        if (!fields) throw std::invalid_argument("Expected [[fields]] declarations");
        std::vector<FieldValueInfo> result;

        for (const auto& entry : *fields) {
            const auto* field = entry.as_table();
            if (!field) throw std::invalid_argument("Each custom field must be a table");
            check_keys(*field, allowed_field_properties);
            const auto name = read_string(*field, "name");
            result.push_back(FieldValueInfo::create_custom(name, schema_helpers::parse_field_type(read_string(*field, "type"))));
        }
        validate_custom_fields(result);
        return result;
    } catch (const toml::parse_error& error) {
        throw std::invalid_argument("Invalid custom fields TOML: " + std::string(error.description()));
    }
}

} // namespace traceschema
