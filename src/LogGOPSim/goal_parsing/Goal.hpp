#pragma once

#include "trace_format/FieldIdentity.hpp"
#include "trace_format/FieldTypes.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

// In-memory rank graph constructed by GoalReader; no simulator or binary I/O.
struct Node {
    static constexpr std::uint32_t any_source = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t any_tag = std::numeric_limits<std::uint32_t>::max();

    std::vector<traceschema::FieldValue> custom_values; // TOML declaration order.

    uint64_t Size;
    std::vector<Node *> finish_dependents;
    std::vector<Node *> start_dependents;
    uint32_t dependency_count;
    uint32_t node_index;
    uint32_t Peer;
    uint32_t Tag;
    uint8_t Proc;
    uint8_t Nic;
    char Type;

    [[nodiscard]] bool is_root() const { return dependency_count == 0; }
};

class Graph {

  private:
    std::vector<std::unique_ptr<Node>> nodes_;

    uint32_t num_edges;

    void addDependent(Node *node, std::vector<Node *> &dependents) {
        if (num_edges == std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Legacy dependency count exceeds uint32_t");
        dependents.push_back(node);
        ++node->dependency_count;
        ++num_edges;
    }

  public:
    Graph() : num_edges(0) {}
    Graph(const Graph&) = delete;
    Graph& operator=(const Graph&) = delete;
    Graph(Graph&& other) noexcept
        : nodes_(std::move(other.nodes_)), num_edges(std::exchange(other.num_edges, 0)) {}
    Graph& operator=(Graph&& other) noexcept {
        if (this != &other) {
            nodes_ = std::move(other.nodes_);
            num_edges = std::exchange(other.num_edges, 0);
        }
        return *this;
    }

    // Borrowed traversal pointers; the graph retains ownership.
    auto GetNodes() const {
        return std::span(nodes_) | std::views::transform([](const auto& node) { return node.get(); });
    }
    std::uint32_t GetDependencyCount() const { return num_edges; }
    std::uint32_t GetRootCount() const {
        return static_cast<std::uint32_t>(
            std::ranges::count_if(GetNodes(), [](const Node *node) { return node && node->is_root(); }));
    }

    Node* addNode() {
        if (nodes_.size() >= std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Rank node count exceeds uint32_t");
        auto node = std::make_unique<Node>();
        node->node_index = static_cast<std::uint32_t>(nodes_.size());
        auto* result = node.get();
        nodes_.push_back(std::move(node));
        return result;
    }

    // a waits for b to finish.
    void addDependency(Node *a, Node *b) { addDependent(a, b->finish_dependents); }

    // a waits for b to start.
    void addStartDependency(Node *a, Node *b) { addDependent(a, b->start_dependents); }
};

typedef Node *goalop_t;

class Goal {

  private:
    Graph graph;
    uint32_t rank = 0;
    uint32_t num_ranks = 0;

  public:
    std::uint32_t GetRank() const { return rank; }
    std::uint32_t GetNumRanks() const { return num_ranks; }
    auto GetNodes() const { return graph.GetNodes(); }
    std::uint32_t GetDependencyCount() const { return graph.GetDependencyCount(); }
    std::uint32_t GetRootCount() const { return graph.GetRootCount(); }

    goalop_t Send(uint32_t dest, uint64_t size, uint32_t tag, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Send);
        n->Peer = dest;
        n->Tag = tag;
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    goalop_t Recv(uint32_t src, uint64_t size, uint32_t tag, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Receive);
        n->Peer = src;
        n->Tag = tag;
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    goalop_t Calc(uint64_t size, uint8_t cpu, uint8_t nic) {

        Node *n = graph.addNode();

        n->Type = std::to_underlying(traceschema::OperationType::Calculation);
        n->Peer = 0; // this optype has not real peer, i just set it so it is clearly defined
        n->Tag = 0;  // this optype has not real tag, i just set it so it is clearly defined
        n->Proc = cpu;
        n->Nic = nic;
        n->Size = size;

        return n;
    }

    void StartDependency(goalop_t src, goalop_t dest) {
        // a can not be executed before b is started
        graph.addStartDependency(src, dest);
    }

    void Dependency(goalop_t src, goalop_t dest) {
        // a can not be executed before b is finished
        graph.addDependency(src, dest);
    }

    void SetRank(uint32_t r) { rank = r; }

    void SetNumRanks(uint32_t nr) { num_ranks = nr; }
};

namespace traceschema {

// Reads standard values from a parsed node; appendix positions count indices, not bytes.
inline FieldValue get_node_field_value(
    const Node& node, FieldId field, std::uint64_t dependency_appendix_index) {
    switch (field) {
    case FieldId::DependencyCount: return std::uint64_t{node.dependency_count};
    case FieldId::Type: return static_cast<std::uint64_t>(node.Type);
    case FieldId::Peer:
        if (node.Peer == Node::any_source) return Wildcard{};
        return std::uint64_t{node.Peer};
    case FieldId::Size: return node.Size;
    case FieldId::Tag:
        if (node.Tag == Node::any_tag) return Wildcard{};
        return std::uint64_t{node.Tag};
    case FieldId::Cpu: return std::uint64_t{node.Proc};
    case FieldId::Nic: return std::uint64_t{node.Nic};
    case FieldId::FinishDependentsCount: return std::uint64_t{node.finish_dependents.size()};
    case FieldId::FinishDependentsIndex: return dependency_appendix_index;
    case FieldId::StartDependentsCount: return std::uint64_t{node.start_dependents.size()};
    case FieldId::StartDependentsIndex: return dependency_appendix_index + node.finish_dependents.size();
    }
    throw std::invalid_argument("Unknown standard node field");
}

} // namespace traceschema
