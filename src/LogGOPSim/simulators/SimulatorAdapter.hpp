#ifndef LOGGOPSIM_SIMULATORADAPTER_HPP
#define LOGGOPSIM_SIMULATORADAPTER_HPP

#include "binary_reading/TraceReader.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

namespace traceschema {

// Shared simulator-facing operations. Each adapter fixes its node type and bindings.
// The caller keeps the mapped file alive and writable for the adapter's lifetime.
template<std::derived_from<BaseDeserializedGraphNode> NodeType>
class SimulatorAdapter {
public:
    using Node = NodeType;

    SimulatorAdapter(std::span<std::byte> file, std::vector<FieldBinding<Node>> bindings,
                     std::function<void(const Node&)> validate_node = {})
        : reader_(file, std::move(bindings), std::move(validate_node)) {}
    virtual ~SimulatorAdapter() = default;

    [[nodiscard]] Node read_node(std::uint32_t rank, std::uint32_t index) const {
        return reader_.read_node<Node>(rank, index);
    }

    void read_node_into(std::uint32_t rank, std::uint32_t index, Node& output) const {
        reader_.read_node_into(rank, index, output);
    }

    [[nodiscard]] std::vector<Node> read_root_nodes(std::uint32_t rank) const {
        return reader_.read_root_nodes<Node>(rank);
    }

    [[nodiscard]] std::uint32_t get_rank_count() const { return reader_.get_rank_count(); }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const {
        return reader_.get_node_count(rank);
    }
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const {
        return reader_.get_root_count(rank);
    }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const {
        return reader_.read_root(rank, index);
    }
    [[nodiscard]] std::uint64_t get_resource_count(FieldIdentity field) const {
        return reader_.get_resource_count(std::move(field));
    }
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
        return reader_.decrement_dependency_count(rank, index);
    }

private:
    TraceReader reader_;
};

} // namespace traceschema
#endif
