#ifndef LOGGOPSIM_FIELDIDENTITY_HPP
#define LOGGOPSIM_FIELDIDENTITY_HPP

#include <cstdint>
#include <array>
#include <string_view>
#include <string>
#include <variant>

namespace traceschema {

// These values are binary format IDs. Never renumber existing fields.
enum class FieldId : std::uint32_t {
    Type = 1, Peer = 2, Size = 3, Tag = 4, Cpu = 5, Nic = 6,
    DependencyCount = 7,
    // Nodes released when this node finishes (GOAL "requires") and when it starts ("irequires"):
    // how many there are and where their list begins in the rank's dependency array.
    FinishDependentsCount = 8, FinishDependentsIndex = 9,
    StartDependentsCount = 10, StartDependentsIndex = 11
};

// Operation codes shared by GOAL producers and consumers.
enum class OperationType : std::uint64_t { Send = 1, Receive = 2, Calculation = 3 };

struct StandardFieldDefinition {
    FieldId id;
    std::string_view name;
};

// Default record order and names; FieldId defines the stable binary IDs.
inline constexpr std::array standard_fields{
    StandardFieldDefinition{FieldId::DependencyCount, "depcnt"},
    StandardFieldDefinition{FieldId::Type, "type"},
    StandardFieldDefinition{FieldId::Peer, "peer"},
    StandardFieldDefinition{FieldId::Size, "size"},
    StandardFieldDefinition{FieldId::Tag, "tag"},
    StandardFieldDefinition{FieldId::Cpu, "cpu"},
    StandardFieldDefinition{FieldId::Nic, "nic"},
    StandardFieldDefinition{FieldId::FinishDependentsCount, "nDep"},
    StandardFieldDefinition{FieldId::FinishDependentsIndex, "depIx"},
    StandardFieldDefinition{FieldId::StartDependentsCount, "nSDep"},
    StandardFieldDefinition{FieldId::StartDependentsIndex, "sDepIx"}};

using FieldIdentity = std::variant<FieldId, std::string>;
namespace schema_helpers {
const char* get_standard_field_name(FieldId id);
void validate_field_identity(const FieldIdentity& field);
std::string get_field_label(const FieldIdentity& field);
} // namespace schema_helpers

} // namespace traceschema

#endif
