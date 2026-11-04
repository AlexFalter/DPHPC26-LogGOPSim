# Adding a simulator adapter

Keep the simulator's node type and adapter together under `simulators/<name>/`.
Inherit the graph fields from `BaseDeserializedGraphNode` and bind the additional
fields needed by the simulator:

```cpp
#include "simulators/SimulatorAdapter.hpp"

struct MyNode : traceschema::BaseDeserializedGraphNode {
    std::uint64_t bytes = 0;
    std::int32_t priority = 0;
};

class MyAdapter final : public traceschema::SimulatorAdapter<MyNode> {
public:
    explicit MyAdapter(std::span<std::byte> file)
        : SimulatorAdapter(file, {
            traceschema::bind_field(traceschema::FieldId::Size, &MyNode::bytes),
            traceschema::bind_field(std::string{"priority"}, &MyNode::priority)}) {}
};

MyAdapter adapter(mapped_bytes);
auto roots = adapter.read_root_nodes(rank);
auto node = adapter.read_node(rank, node_index);
if (adapter.decrement_dependency_count(rank, next_index) == 0)
    ready_nodes.push_back(adapter.read_node(rank, next_index));
```

The simulator constructs the adapter over its mapped binary and uses it inside
its own scheduling loop. Setup checks that the requested fields exist and their
decoded ranges fit the destination members. The graph base supplies dependency
fields automatically; unbound fields are skipped. Custom fields such as
`priority` must be declared during conversion.

Keep the mapping alive while the adapter or returned dependency views are in
use; copy dependency indices if they must outlive it. Each simulation needs a
fresh private mapping so counter updates leave the input file unchanged.
For a complete integration, see [the ATLAHS adapter](atlahs/README.md).
