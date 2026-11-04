#include "goal_parsing/TraceAnalysis.hpp"
#include "trace_format/BinaryIO.hpp"
#include "goal_parsing/CustomFields.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace traceschema {

TraceAnalysis::TraceAnalysis(std::vector<FieldValueInfo> custom_fields)
    : custom_fields_(std::move(custom_fields)) {
    validate_custom_fields(custom_fields_);
}

void TraceAnalysis::observe_custom_values(std::span<const FieldValue> values) {
    if (values.size() != custom_fields_.size())
        throw std::invalid_argument("Custom value count does not match declarations");
    for (std::size_t index = 0; index < values.size(); ++index) {
        auto& field = custom_fields_[index];
        const auto& value = values[index];
        schema_helpers::observe_field_value(field.value_range, field.type, value,
                                            std::get<std::string>(field.field));
    }
}

static void add_checked(std::uint64_t& total, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - total)
        throw std::invalid_argument("Trace statistics total overflows");
    total += value;
}

void TraceAnalysis::observe(FieldId field, std::uint64_t value) {
    auto& range = fields_.at(static_cast<std::size_t>(field) - 1).range;
    if (!range) range = UnsignedRange{value, value};
    else {
        range->minimum = std::min(range->minimum, value);
        range->maximum = std::max(range->maximum, value);
    }
}

void TraceAnalysis::observe_wildcard(FieldId field) {
    fields_.at(static_cast<std::size_t>(field) - 1).has_wildcard = true;
}

void TraceAnalysis::finish_rank(std::uint64_t nodes, std::uint64_t roots, std::uint64_t edges) {
    add_checked(ranks_, 1);
    add_checked(nodes_, nodes);
    add_checked(edges_, edges);
    max_nodes_ = std::max(max_nodes_, nodes);
    max_roots_ = std::max(max_roots_, roots);
    max_edges_ = std::max(max_edges_, edges);
}

std::uint32_t TraceAnalysis::get_node_index_bits() const {
    const auto maximum_index = max_nodes_ == 0 ? 0 : max_nodes_ - 1;
    return calculate_required_bits(maximum_index);
}

RecordLayout TraceAnalysis::create_schema(
    std::uint64_t num_ranks) const {
    std::vector<FieldValueInfo> inputs;
    for (const auto& definition : standard_fields) {
        const auto id = definition.id;
        auto input = FieldValueInfo::create_standard(id);
        const auto& statistics = fields_.at(static_cast<std::size_t>(id) - 1);
        if (statistics.range) input.value_range = *statistics.range;
        input.has_wildcard = statistics.has_wildcard;
        // No numeric observations (empty trace, unused field, or wildcard-only).
        if (!input.value_range && id != FieldId::Type && id != FieldId::Peer)
            input.value_range = UnsignedRange{0, 0};
        inputs.push_back(std::move(input));
    }
    for (const auto& field : custom_fields_) {
        auto input = field;
        // No operations means no numeric observations; use zero for an empty field.
        if (!input.value_range) input.value_range = schema_helpers::make_zero_range(input.type);
        inputs.push_back(std::move(input));
    }
    return build_record_layout(inputs, num_ranks);
}

void TraceAnalysis::print_report(std::ostream& output, const RecordLayout& schema,
                                  ReportedLayoutUsage layout_usage) const {
    const bool layout_used = layout_usage == ReportedLayoutUsage::UsedInOutput;
    output << (layout_used ? "Schema report (written to self-describing output)\n"
                       : "Schema report (proposal only; output remains legacy format)\n")
           << "Ranks: " << ranks_ << "; nodes: " << nodes_ << "; dependency edges: " << edges_ << '\n';
    output << std::left << std::setw(12) << "Field" << std::setw(26) << "Observed numeric range"
           << std::setw(10) << "Wildcard" << std::setw(22) << "Value base" << std::setw(8) << "Bits" << "Bit offset\n";
    for (const auto& field : schema.fields) {
        const auto* id = std::get_if<FieldId>(&field.field);
        const auto* statistics = id ? &fields_.at(static_cast<std::size_t>(*id) - 1) : nullptr;
        std::string range = "none";
        if (statistics && statistics->range)
            range = std::to_string(statistics->range->minimum) + ".." + std::to_string(statistics->range->maximum);
        if (!id) {
            const auto observed = std::ranges::find(custom_fields_, field.field, &FieldValueInfo::field);
            if (observed != custom_fields_.end() && observed->value_range)
                range = schema_helpers::format_value_range(*observed->value_range);
        }
        const auto name = id ? std::string(schema_helpers::get_standard_field_name(*id))
                             : std::get<std::string>(field.field);
        output << std::setw(12) << name
               << std::setw(26) << range << std::setw(10)
               << (field.offset_encoding.has_wildcard ? "yes" : "no")
               << std::setw(22) << schema_helpers::format_field_value(field.offset_encoding.value_base)
               << std::setw(8) << field.bit_width << field.bit_offset << '\n';
    }
    output << (layout_used ? "Node record: " : "Proposed node record: ") << schema.record_size_bits << " bits / "
           << schema.get_record_size_bytes() << " bytes including byte padding (legacy: 39 bytes)\n"
           << "Largest rank: nodes=" << max_nodes_ << "; roots=" << max_roots_
           << "; edges=" << max_edges_ << '\n'
           << (layout_used ? "Root/dependency node indices: " : "Proposed root/dependency node indices: ")
           << get_node_index_bits()
           << " bits each, shared file-wide; base 0 (legacy: 32 bits).\n"
           << "Header and root/dependency array sizes are not included in the node record size.\n"
           << "Numeric storage: value - value base (unsigned); constant fields use 0 bits.\n"
           << "Type uses codes 1..3; Peer uses the declared rank count. "
           << (layout_used ? "Two input passes: statistics, then writing.\n" : "No extra input pass.\n");
}

} // namespace traceschema
