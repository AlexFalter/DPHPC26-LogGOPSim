# Simulator adapters

Each simulator has its own subdirectory. `binary_reading/TraceFileView` provides mapped record/index access, `NodeDecoder` decodes selected values, and `TraceReader` assigns them and assembles graph nodes; file layouts and byte helpers live in `trace_format/`.

Each simulator is a separate program with its own entry point, command-line options and event loop. Choose which simulator to run by invoking its executable. Its adapter declares a node type and field bindings. `SimulatorAdapter<Node>` supplies the shared typed interface and holds a `TraceReader` internally; the reader validates and decodes the bindings inside that program.

`loggopsim/LogGOPSim.cpp` contains LogGOPSim's `main`. Startup creates a `SimulationSchedule`, which maps the binary and constructs a `LogGOPSimAdapter` for self-describing input. This adapter inherits `SimulatorAdapter<LogGOPSimNode>`: `read_node(rank, index)` returns a `LogGOPSimNode` without a template argument at each call.

`atlahs/AtlahsAdapter.hpp/.cpp` supplies a second trace adapter for ATLAHS's htsim backend. Its existing executable and GOAL scheduler call the shared reader through ordinary source edits in the pinned `atlahs/upstream/` fork. It is built separately as `atlahs_htsim`, keeping htsim's packet model and event loop intact. See [ATLAHS build and usage](atlahs/README.md).

To adapt another simulator, keep its node type and adapter together and declare the fields and destinations:

```cpp
#include "simulators/SimulatorAdapter.hpp"

struct MyNode : traceschema::BaseDeserializedGraphNode {
    std::uint64_t bytes;
    std::int32_t priority;
};

class MyAdapter final : public traceschema::SimulatorAdapter<MyNode> {
public:
    explicit MyAdapter(std::span<std::byte> file)
        : SimulatorAdapter(file, {
            traceschema::bind_field(traceschema::FieldId::Size, &MyNode::bytes),
            traceschema::bind_field(std::string{"priority"}, &MyNode::priority)}) {}
};

MyAdapter adapter(mapped_bytes);
auto node = adapter.read_node(rank, node_index);
auto roots = adapter.read_root_nodes(rank);
if (adapter.decrement_dependency_count(rank, next_index) == 0)
    ready_nodes.push_back(adapter.read_node(rank, next_index));
```

The shared reader derives destination types, checks presence and decoded ranges once, and assigns only the requested fields. Both standard IDs and custom names work. Supply a typed numeric wildcard sentinel as the third `bind_field` argument when the field supports wildcards. Use `read_node_into` to reuse an output object. The caller must keep the mapped file alive while using the reader.

`simulators/BaseDeserializedGraphNode.hpp` defines the shared output contract for graph simulators. Graph-node children automatically require the dependency counter and completion/start list counts and indices. The shared reader decodes these fields, fills the inherited index/counter/lists and configures counter updates. Declare only the child's additional fields. Standard and custom fields both bind to additional node members. Unbound file fields are skipped; there is no per-node custom-value list. A field-only consumer can omit the graph base and read just its bindings.

Node operation fields and counters are decoded values. `DependOnMe` and `StartDependOnMe` are read-only `NodeIndexView` lists backed by the mapped file, supporting iteration, `size()`, `empty()` and indexing. Keep the mapping alive and its dependency bytes unchanged for as long as any node uses them. Copy a list explicitly when it must outlive that mapping:

```cpp
std::vector<std::uint32_t> saved(node.DependOnMe.begin(), node.DependOnMe.end());
```

Node reads allocate no dependency vectors. The view validates indices when created, then decodes them as they are visited; root-node collections and simulator event queues can still allocate.

`SimulatorAdapter.hpp` exposes node reads, root loading, rank/node/root counts, resource counts and dependency-counter updates. Its node type is fixed when the adapter is constructed; `read_node_into` accepts only that type. Decoding, validation and graph assembly remain in `TraceReader`; the adapter delegates to it. Field-only analysis consumers can still use `TraceReader` directly.

`loggopsim/LogGOPSimAdapter.hpp/.cpp` declares the node, extra field bindings and wildcard conversions. `SimulationSchedule.hpp` manages mapped input and per-rank ready nodes; `SimulationEvents.hpp` defines runtime queue entries. The network model, noise injection, timeline output and command-line definition also live in `loggopsim/`.

The simulator's input-loading function constructs its adapter and calls `read_node(rank, index)` when it needs an operation. The simulator keeps ownership of its startup and scheduling loop. GOAL text parsing and binary conversion remain a separate `txt2bin` command.

Shared validation checks operation-code ranges, selected peer ranges against the rank count, and wildcard-destination collisions against numeric ranges. When both Type and Peer are selected, the decoder rejects sends to ANY while accepting receive wildcards. It uses values already being decoded; unselected fields remain unread. LogGOPSim declares no validation callbacks. Consumers can still supply an optional callback for rules specific to their model.
