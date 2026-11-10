#ifndef LOGGOPSIM_SIMULATORADAPTER_HPP
#define LOGGOPSIM_SIMULATORADAPTER_HPP

#include "binary_reading/TraceReader.hpp"
#include "binary_reading/MappedTraceFile.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace traceschema {

// Shared simulator-facing operations. Each adapter fixes its node type and bindings.
// An owned mapping outlives the reader. Buffer construction instead borrows bytes
// whose lifetime remains the caller's responsibility.
template<std::derived_from<BaseDeserializedGraphNode> NodeType>
class SimulatorAdapter {
public:
    using Node = NodeType;

    SimulatorAdapter(std::span<std::byte> file, std::vector<FieldBinding<Node>> bindings,
                     std::function<void(const Node&)> validate_node = {})
        : reader_(file, std::move(bindings), std::move(validate_node)) {}
    SimulatorAdapter(std::shared_ptr<MappedTraceFile> file, std::vector<FieldBinding<Node>> bindings,
                     std::function<void(const Node&)> validate_node = {})
        : mapping_(std::move(file)), reader_(mapped_bytes(), std::move(bindings), std::move(validate_node)) {}
    virtual ~SimulatorAdapter() = default;

    // Legacy files hold only the standard fields; see TraceReader.
    [[nodiscard]] bool is_legacy() const { return reader_.is_legacy(); }

    [[nodiscard]] Node read_node(std::uint32_t rank, std::uint32_t index) const {
        return reader_.read_node<Node>(rank, index);
    }

    // Only the dependency list released when the node starts or finishes.
    [[nodiscard]] DependencyListView read_dependents(
        std::uint32_t rank, std::uint32_t index, DependencyEvent event) const {
        return reader_.read_dependents(rank, index, event);
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
    [[nodiscard]] std::uint64_t get_resource_count(const FieldIdentity& field) const {
        return reader_.get_resource_count(field);
    }
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
        return reader_.decrement_dependency_count(rank, index);
    }
    [[nodiscard]] std::uint64_t read_dependency_count(std::uint32_t rank, std::uint32_t index) const {
        return reader_.read_dependency_count(rank, index);
    }
    // Mark destructive shared input as consumed before simulation mutates its counters.
    // Borrowed buffers and private mappings must never be invalidated through this operation.
    void invalidate_shared_input() {
        if (!mapping_ || !mapping_->is_shared())
            throw std::logic_error("Input invalidation requires an owned shared mapping");
        write_little_endian(std::uint64_t{invalid_schedule_magic},
                            mapping_->bytes().first(sizeof(std::uint64_t)));
    }
    // Which plan runner decodes the nodes (LOGGOPSIM_DECODER falls back to plan when its runner
    // fails), or "legacy".
    [[nodiscard]] std::string_view get_runner_name() const { return reader_.get_runner_name(); }

private:
    std::span<std::byte> mapped_bytes() const {
        if (!mapping_) throw std::invalid_argument("Adapter requires a mapped trace file");
        return mapping_->bytes();
    }
    // Declaration order matters: the reader is destroyed before its backing mapping.
    std::shared_ptr<MappedTraceFile> mapping_;
    TraceReader reader_;
};

} // namespace traceschema
#endif
