#ifndef LOGGOPSIM_BASEDESERIALIZEDGRAPHNODE_HPP
#define LOGGOPSIM_BASEDESERIALIZEDGRAPHNODE_HPP

#include <cstdint>
#include "trace_format/NodeIndexView.hpp"

namespace traceschema {

// Shared graph state; simulator node types add their operation-specific fields.
// Dependency lists borrow the mapped file, which must outlive these nodes.
struct BaseDeserializedGraphNode {
    std::uint32_t offset = 0; // Node index within its rank.
    std::uint32_t DependenciesCnt = 0;
    NodeIndexView DependOnMe;
    NodeIndexView StartDependOnMe;
};

} // namespace traceschema
#endif
