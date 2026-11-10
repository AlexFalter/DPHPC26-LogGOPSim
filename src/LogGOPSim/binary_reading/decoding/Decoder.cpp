#include "binary_reading/decoding/Decoder.hpp"
#include "binary_reading/decoding/runners/RunnerRegistry.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace traceschema {

static std::vector<FieldStorageInfo> resolve_required_fields(
    const RecordLayout& layout, const FieldRequirements& requirements) {
    layout.check_requirements(requirements);
    std::unordered_set<FieldIdentity> selected;
    std::vector<FieldStorageInfo> fields;
    fields.reserve(requirements.size());
    for (const auto& requirement : requirements) {
        const auto& identity = requirement.field;
        if (!selected.insert(identity).second)
            throw std::invalid_argument("Duplicate required field '" + schema_helpers::get_field_label(identity) + "'");
        const auto field = std::ranges::find(layout.fields, identity, &FieldStorageInfo::field);
        fields.push_back(*field); // check_requirements established presence and destination capacity.
    }
    return fields;
}

// LOGGOPSIM_DECODER names a runner of the registry. A runner that is
// unavailable for the plan (no compiler, no instantiation) falls back to the default, with a
// message once per run.
static std::unique_ptr<const PlanRunner> create_runner(const DecodePlan& plan) {
    const auto& fallback = get_default_runner();
    const char* const variable = std::getenv("LOGGOPSIM_DECODER");
    const auto* requested = variable ? find_runner(variable) : &fallback;
    if (!requested) {
        std::string names;
        for (const auto& runner : get_runners()) names += (names.empty() ? "" : ", ") + std::string(runner.name);
        throw std::invalid_argument("LOGGOPSIM_DECODER must be one of: " + names);
    }
    if (auto runner = requested->create(plan)) return runner;
    static std::once_flag message;
    std::call_once(message, [&] {
        std::cerr << "LOGGOPSIM_DECODER=" << requested->name << " is unavailable for this trace; using "
                  << fallback.name << ".\n";
    });
    return fallback.create(plan);
}

Decoder::Decoder(const RecordLayout& layout, const FieldRequirements& requirements,
                 std::span<const FieldDestination> destinations)
    : fields_(resolve_required_fields(layout, requirements)),
      record_size_bytes_(layout.get_record_size_bytes()) {
    for (std::size_t index = 0; index < fields_.size(); ++index) {
        const auto& field = fields_[index];
        if (field.field == FieldIdentity{FieldId::Type}) {
            const auto& encoding = field.offset_encoding;
            if (field.type != FieldType::UnsignedInteger || encoding.has_wildcard ||
                std::get<std::uint64_t>(encoding.value_base) < std::to_underlying(OperationType::Send) ||
                std::get<std::uint64_t>(encoding.decode(encoding.maximum_offset)) >
                    std::to_underlying(OperationType::Calculation))
                throw std::invalid_argument("Unknown operation code in trace schema");
        }
    }
    auto plan = DecodePlan::create(fields_, destinations);
    runner_ = create_runner(plan);
}

} // namespace traceschema
