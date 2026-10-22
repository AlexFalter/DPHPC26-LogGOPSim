#ifndef LOGGOPSIM_TRACEREADER_HPP
#define LOGGOPSIM_TRACEREADER_HPP

#include "binary_reading/TraceFileView.hpp"
#include "binary_reading/NodeDecoder.hpp"
#include "simulators/BaseDeserializedGraphNode.hpp"
#include "binary_reading/FieldBindings.hpp"

#include <any>
#include <concepts>
#include <functional>
#include <type_traits>
#include <initializer_list>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace traceschema {

// Views mapped file storage; the caller keeps that mapping alive and writable.
// Returned graph nodes borrow dependency lists from the mapping too.
// Resolves only the fields requested by this consumer, whether standard or custom.
class TraceReader {
public:
    TraceReader(std::span<std::byte> file, const FieldRequirements& requirements);
    template<class Output>
    TraceReader(std::span<std::byte> file, std::vector<FieldBinding<Output>> bindings,
                std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : TraceReader(file, get_node_requirements<Output>(bindings)) {
        validate_field_bindings<Output>(bindings, get_selected_fields());
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
            const auto& dependencies = get_dependency_bindings();
            validate_field_bindings<DecodedDependencyFields>(dependencies, get_selected_fields().subspan(bindings.size()));
            set_dependency_counter(FieldId::DependencyCount);
        }
        bindings_ = NodeOutputBindings<Output>{std::move(bindings), std::move(validate_node)};
    }

    template<class Output>
    TraceReader(std::span<std::byte> file, std::initializer_list<FieldBinding<Output>> bindings,
                std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : TraceReader(file, std::vector<FieldBinding<Output>>(bindings), std::move(validate_node)) {}

    ~TraceReader() = default;
    TraceReader(const TraceReader&) = delete;
    TraceReader& operator=(const TraceReader&) = delete;

    [[nodiscard]] std::uint32_t get_rank_count() const { return file_.get_rank_count(); }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const { return file_.get_node_count(rank); }
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const { return file_.get_root_count(rank); }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const {
        return file_.read_root(rank, index);
    }
    [[nodiscard]] std::span<const FieldStorageInfo> get_selected_fields() const { return decoder_.get_selected_fields(); }
    // Values follow requirement order; the caller may reuse an output buffer.
    [[nodiscard]] std::vector<FieldValue> read_fields(std::uint32_t rank, std::uint32_t index) const;
    void read_fields_into(std::uint32_t rank, std::uint32_t index, std::span<FieldValue> output) const;
    template<class Output>
    [[nodiscard]] Output read_node(std::uint32_t rank, std::uint32_t index) const {
        Output output{};
        read_node_into(rank, index, output);
        return output;
    }

    template<class Output>
    void read_node_into(std::uint32_t rank, std::uint32_t index, Output& output) const {
        const auto& bindings = get_node_output_bindings<Output>();
        const auto record = file_.get_node_record_bytes(rank, index);
        const auto graph = decode_selected_fields(record, bindings, output);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>)
            populate_graph_node(output, graph, rank, index);
        if (bindings.validate_output) bindings.validate_output(output);
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
            if (node.DependenciesCnt != 0)
                throw std::invalid_argument("Root node must have zero dependencies");
            nodes.push_back(std::move(node));
            previous_root = root;
        }
        return nodes;
    }

    // Configure once before mutation; the unsigned counter must represent zero and have no wildcard.
    void set_dependency_counter(FieldIdentity field);
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index);
    [[nodiscard]] std::uint64_t get_resource_count(FieldIdentity field) const;

private:
    struct DecodedDependencyFields {
        std::uint32_t dependency_count = 0;
        std::uint32_t completion_count = 0;
        std::uint32_t completion_begin = 0;
        std::uint32_t start_count = 0;
        std::uint32_t start_begin = 0;
    };

    template<class Output>
    struct NodeOutputBindings {
        std::vector<FieldBinding<Output>> fields;
        std::function<void(const Output&)> validate_output;
    };

    template<class Output>
    [[nodiscard]] static FieldRequirements get_node_requirements(
        std::span<const FieldBinding<Output>> bindings) {
        auto requirements = get_field_requirements<Output>(bindings);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
            const auto dependencies = get_field_requirements<DecodedDependencyFields>(get_dependency_bindings());
            requirements.insert(requirements.end(), dependencies.begin(), dependencies.end());
        }
        return requirements;
    }

    template<class Output>
    [[nodiscard]] const NodeOutputBindings<Output>& get_node_output_bindings() const {
        const auto* bindings = std::any_cast<NodeOutputBindings<Output>>(&bindings_);
        if (!bindings) throw std::invalid_argument("Reader has no bindings for this output type");
        return *bindings;
    }

    template<class Output>
    [[nodiscard]] DecodedDependencyFields decode_selected_fields(
        std::span<const std::byte> record, const NodeOutputBindings<Output>& bindings,
        Output& output) const {
        DecodedDependencyFields graph{};
        decoder_.decode_fields(record, [&](std::size_t position, const FieldStorageInfo&, FieldValue value) {
            if (position < bindings.fields.size()) {
                bindings.fields[position].assign(output, value);
            } else if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
                get_dependency_bindings()[position - bindings.fields.size()].assign(graph, value);
            }
        });
        return graph;
    }

    [[nodiscard]] static const std::vector<FieldBinding<DecodedDependencyFields>>& get_dependency_bindings();
    void populate_graph_node(BaseDeserializedGraphNode& node, const DecodedDependencyFields& fields,
                             std::uint32_t rank, std::uint32_t index) const;

    // Retains the binding output type so reads cannot assign into an unrelated struct.
    std::any bindings_;
    TraceFileView file_;
    NodeDecoder decoder_;
    std::optional<FieldStorageInfo> dependency_counter_;

    [[nodiscard]] const FieldStorageInfo& require_unsigned_field(const FieldIdentity& field) const;
};

} // namespace traceschema
#endif
