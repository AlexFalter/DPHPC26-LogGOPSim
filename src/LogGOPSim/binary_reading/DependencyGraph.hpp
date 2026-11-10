#ifndef LOGGOPSIM_DEPENDENCYGRAPH_HPP
#define LOGGOPSIM_DEPENDENCYGRAPH_HPP

#include "binary_reading/FieldBindings.hpp"
#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/TraceFileView.hpp"
#include "simulators/BaseDeserializedGraphNode.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace traceschema {

// The dependency information of a trace's graph nodes: where each node's lists of dependents lie,
// turned into views over the mapped file, and the dependency counts, updated in place.
class DependencyGraph {
public:
    // Where a node's two lists of dependents lie in its rank's dependency array: the nodes released
    // when it finishes and when it starts. The decoder writes them here; attach_lists turns them
    // into the node's list views.
    struct ListPositions {
        std::uint32_t on_finish_begin = 0;
        std::uint32_t on_finish_count = 0;
        std::uint32_t on_start_begin = 0;
        std::uint32_t on_start_count = 0;
    };

    // The dependency count is decoded directly into the node.
    template<class Node>
    [[nodiscard]] static FieldBinding<Node> get_count_binding() {
        return bind_field<Node>(FieldId::DependencyCount,
                                static_cast<std::uint32_t Node::*>(&BaseDeserializedGraphNode::dependency_count));
    }
    [[nodiscard]] static const std::vector<FieldBinding<ListPositions>>& get_list_bindings();

    // The file must outlive the graph. The count field must be unsigned, include zero as its base
    // and have no wildcard.
    DependencyGraph(TraceFileView& file, const FieldStorageInfo& count_field);

    void attach_lists(BaseDeserializedGraphNode& node, const ListPositions& lists, std::uint32_t rank) const;

    // Reads only the list released by this event, without decoding the node's other fields.
    [[nodiscard]] DependencyListView read_dependents(
        std::uint32_t rank, std::uint32_t index, DependencyEvent event) const;

    // Decrements the node's dependency count in place and returns the new value; rejects a count
    // that is already zero or outside the field's range.
    std::uint64_t decrement(std::uint32_t rank, std::uint32_t index) const;

private:
    const TraceFileView& file_;
    FieldStorageInfo count_field_;
    // Prepared bit access, shared with the full-node decoder, in ListPositions order.
    // Empty for layouts without list fields, as used by some analysis consumers.
    std::optional<std::array<FieldSource, 4>> list_fields_;
};

} // namespace traceschema
#endif
