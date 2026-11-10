#ifndef LOGGOPSIM_RANKPACKING_HPP
#define LOGGOPSIM_RANKPACKING_HPP

#include "trace_format/RecordLayout.hpp"
#include "binary_writing/RecordPacking.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <variant>

class Goal;
struct Node;

namespace traceschema {

struct PackedRankData {
    std::vector<std::byte> bytes; // Node records, dependency appendix, then root indices.
    std::size_t dependency_appendix_offset; // Byte offset within bytes.
    std::uint32_t dependency_count;
    std::size_t root_indices_offset; // Byte offset within bytes.
    std::uint32_t root_count;
};

// Resolves where each field value comes from and validates the layout once; reuses the value
// buffer across nodes and ranks.
class RankPacker {
public:
    RankPacker(RecordLayout layout, std::uint32_t node_index_bits,
               std::span<const FieldValueInfo> custom_fields = {});
    [[nodiscard]] PackedRankData pack(const Goal& graph);
    [[nodiscard]] const RecordLayout& get_layout() const { return record_packer_.get_layout(); }

private:
    using ValueOrigin = std::variant<FieldId, std::size_t>; // a standard field, or an index into the custom values
    RecordPacker record_packer_;
    std::uint32_t node_index_bits_;
    std::size_t custom_field_count_;
    std::vector<ValueOrigin> value_origins_;
    RecordValues field_values_;
    void collect_field_values(const Node& node, std::uint64_t dependency_appendix_index);
};

} // namespace traceschema
#endif
