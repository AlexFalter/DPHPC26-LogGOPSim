#ifndef LOGGOPSIM_BASEDESERIALIZEDGRAPHNODE_HPP
#define LOGGOPSIM_BASEDESERIALIZEDGRAPHNODE_HPP

#include <cstdint>
#include "binary_reading/DependencyListView.hpp"

namespace traceschema {

// Shared graph state; simulator node types add their operation-specific fields.
// Dependency lists borrow the mapped file, which must outlive these nodes.
struct BaseDeserializedGraphNode {
    std::uint32_t node_index = 0; // Node index within its rank.
    std::uint32_t dependency_count = 0; // Nodes this node still waits for.
    DependencyListView finish_dependents;          // Nodes released when this node finishes (GOAL "requires").
    DependencyListView start_dependents;     // Nodes released when this node starts (GOAL "irequires").
};

} // namespace traceschema
#endif
