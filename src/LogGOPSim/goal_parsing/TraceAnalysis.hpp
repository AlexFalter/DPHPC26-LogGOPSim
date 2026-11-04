#ifndef LOGGOPSIM_TRACE_ANALYSIS_HPP
#define LOGGOPSIM_TRACE_ANALYSIS_HPP

#include "trace_format/RecordLayout.hpp"

#include <array>
#include <cstddef>
#include <ostream>
#include <span>

namespace traceschema {

enum class ReportedLayoutUsage { ProposedOnly, UsedInOutput };

class TraceAnalysis {
public:
    explicit TraceAnalysis(std::vector<FieldValueInfo> custom_fields = {});
    void observe_custom_values(std::span<const FieldValue> values);
    void observe(FieldId field, std::uint64_t value);
    void observe_wildcard(FieldId field);
    void finish_rank(std::uint64_t nodes, std::uint64_t roots, std::uint64_t edges);
    [[nodiscard]] std::uint32_t get_node_index_bits() const;
    [[nodiscard]] RecordLayout create_schema(
        std::uint64_t num_ranks) const;
    void print_report(std::ostream& output, const RecordLayout& schema,
                      ReportedLayoutUsage layout_usage = ReportedLayoutUsage::ProposedOnly) const;

private:
    std::vector<FieldValueInfo> custom_fields_;
    struct FieldStatistics {
        std::optional<UnsignedRange> range;
        bool has_wildcard = false;
    };
    // FieldId values are contiguous and one-based; array indices are ID - 1.
    static constexpr std::size_t field_count = standard_fields.size();
    std::array<FieldStatistics, field_count> fields_{};
    std::uint64_t ranks_ = 0, nodes_ = 0, edges_ = 0;
    std::uint64_t max_nodes_ = 0, max_roots_ = 0, max_edges_ = 0;
};

} // namespace traceschema
#endif
