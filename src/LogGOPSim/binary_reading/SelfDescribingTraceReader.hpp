#ifndef LOGGOPSIM_SELFDESCRIBINGTRACEREADER_HPP
#define LOGGOPSIM_SELFDESCRIBINGTRACEREADER_HPP

#include "binary_reading/DependencyGraph.hpp"
#include "binary_reading/FieldBindings.hpp"
#include "binary_reading/NodeValidator.hpp"
#include "binary_reading/TraceFileView.hpp"
#include "binary_reading/decoding/Decoder.hpp"
#include "simulators/BaseDeserializedGraphNode.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace traceschema {

// Reads a self-describing trace; TraceReader selects it for files with a binary header.
// Views mapped file storage; the caller keeps that mapping alive and writable.
// Returned graph nodes borrow dependency lists from the mapping too. Their
// immutable appendix is validated once at graph setup; mutate only counters.
// Resolves only the fields requested by this consumer, whether standard or custom.
class SelfDescribingTraceReader {
public:
    // Node access: each binding names a field and the member of Output it is decoded into.
    template<class Output>
    SelfDescribingTraceReader(std::span<std::byte> file, std::vector<FieldBinding<Output>> bindings,
                              std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : file_(file), decoder_(create_node_decoder<Output>(file_.get_layout(), bindings)),
          validator_(std::move(validate_node)) {
        check_peer_range();
        validate_bindings<Output>(bindings);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>)
            set_dependency_counter(FieldId::DependencyCount);
    }

    template<class Output>
    SelfDescribingTraceReader(std::span<std::byte> file, std::initializer_list<FieldBinding<Output>> bindings,
                              std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : SelfDescribingTraceReader(file, std::vector<FieldBinding<Output>>(bindings), std::move(validate_node)) {}

    SelfDescribingTraceReader(const SelfDescribingTraceReader&) = delete;
    SelfDescribingTraceReader& operator=(const SelfDescribingTraceReader&) = delete;

    [[nodiscard]] std::uint32_t get_rank_count() const { return file_.get_rank_count(); }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const { return file_.get_node_count(rank); }
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const { return file_.get_root_count(rank); }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const {
        return file_.read_root(rank, index);
    }
    [[nodiscard]] std::span<const FieldStorageInfo> get_selected_fields() const { return decoder_.get_selected_fields(); }
    [[nodiscard]] std::string_view get_runner_name() const { return decoder_.get_runner_name(); }

    template<class Output>
    [[nodiscard]] Output read_node(std::uint32_t rank, std::uint32_t index) const {
        const auto& validate = validator_.get<Output>();
        const auto record = file_.get_node_record_bytes(rank, index);
        Output output{};
        DependencyGraph::ListPositions lists{};
        std::byte* const targets[] = {reinterpret_cast<std::byte*>(&output), reinterpret_cast<std::byte*>(&lists)};
        decoder_.decode_into(record, file_.get_readable_bytes(record), targets);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
            output.node_index = index;
            graph_->attach_lists(output, lists, rank);
        }
        if (validate) validate(output);
        return output;
    }

    // Preserve the concrete node type while loading and validating roots once.
    template<std::derived_from<BaseDeserializedGraphNode> Node>
    [[nodiscard]] std::vector<Node> read_root_nodes(std::uint32_t rank) const {
        const auto root_count = get_root_count(rank);
        std::vector<Node> nodes;
        nodes.reserve(root_count);
        std::uint32_t previous_root = 0;
        for (std::uint32_t index = 0; index < root_count; ++index) {
            const auto root = read_root(rank, index);
            if (index != 0 && root <= previous_root)
                throw std::invalid_argument("Root indices must be ordered and distinct");
            Node node = read_node<Node>(rank, root);
            if (node.dependency_count != 0)
                throw std::invalid_argument("Root node must have zero dependencies");
            nodes.push_back(std::move(node));
            previous_root = root;
        }
        return nodes;
    }

    // The dependency list released by this event, without decoding the node.
    [[nodiscard]] DependencyListView read_dependents(
        std::uint32_t rank, std::uint32_t index, DependencyEvent event) const {
        if (!graph_) throw std::invalid_argument("No dependency counter configured");
        return graph_->read_dependents(rank, index, event);
    }

    // Configure once before mutation; the unsigned counter must represent zero and have no wildcard.
    void set_dependency_counter(FieldIdentity field);
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index);
    [[nodiscard]] std::uint64_t get_resource_count(FieldIdentity field) const;

private:
    // One requirement and one destination per binding: the node's bindings, followed for graph
    // nodes by the dependency count and the positions of the dependency lists.
    template<class Output>
    [[nodiscard]] static Decoder create_node_decoder(
        const RecordLayout& layout, std::span<const FieldBinding<Output>> bindings) {
        FieldRequirements requirements;
        std::vector<FieldDestination> destinations;
        const auto add = [&]<class Target>(OutputStruct target, std::span<const FieldBinding<Target>> targets) {
            for (const auto& binding : targets) {
                requirements.push_back(binding.requirement);
                destinations.push_back({target, binding.destination_offset, binding.destination_size,
                    binding.wildcard_destination ? integer_to_bits(*binding.wildcard_destination) : 0});
            }
        };
        add(OutputStruct::Node, bindings);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
            const auto dependency_count = DependencyGraph::get_count_binding<Output>();
            add(OutputStruct::Node, std::span(&dependency_count, 1));
            add(OutputStruct::DependencyLists, std::span(DependencyGraph::get_list_bindings()));
        }
        return Decoder(layout, requirements, destinations);
    }

    // Checks wildcard representations against the fields selected for the bindings, in the same
    // order. DependencyGraph checks the dependency count itself.
    template<class Output>
    void validate_bindings(std::span<const FieldBinding<Output>> bindings) const {
        const auto fields = get_selected_fields();
        validate_field_bindings<Output>(bindings, fields);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>)
            validate_field_bindings<DependencyGraph::ListPositions>(DependencyGraph::get_list_bindings(),
                                                                    fields.subspan(bindings.size() + 1));
    }

    void check_peer_range() const;
    [[nodiscard]] const FieldStorageInfo& require_unsigned_field(const FieldIdentity& field) const;

    TraceFileView file_;
    Decoder decoder_;
    NodeValidator validator_;
    std::optional<DependencyGraph> graph_;
};

} // namespace traceschema
#endif
