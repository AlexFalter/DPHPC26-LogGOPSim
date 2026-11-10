#pragma once

#include "binary_reading/DependencyGraph.hpp"
#include "binary_reading/DependencyListView.hpp"
#include "binary_reading/FieldBindings.hpp"
#include "binary_reading/NodeValidator.hpp"
#include "binary_reading/decoding/DecodePlan.hpp"
#include "simulators/BaseDeserializedGraphNode.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "trace_format/LegacyFormat.hpp"

#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace traceschema {

// One validated legacy rank. Borrows storage; only counters are mutable.
class LegacyRankView {
public:
    explicit LegacyRankView(std::span<std::byte> bytes) {
        if (bytes.size() < legacy_format::rank_prefix_bytes)
            throw std::invalid_argument("Truncated legacy rank prefix");
        node_count_ = read_native<std::uint32_t>(bytes.data());
        root_count_ = read_native<std::uint32_t>(bytes.data() + sizeof(std::uint32_t));
        if (root_count_ > node_count_) throw std::invalid_argument("Rank has more roots than nodes");
        bytes = bytes.subspan(legacy_format::rank_prefix_bytes);
        roots_ = take_next_bytes(bytes, std::uint64_t{root_count_} * sizeof(std::uint32_t));
        records_ = take_next_bytes(bytes, std::uint64_t{node_count_} * legacy_format::record_bytes);
        dependency_bytes_ = bytes;
        DependencyListView::from_legacy_u32(bytes).validate(node_count_);
        std::uint32_t previous = 0;
        for (std::uint32_t index = 0; index < root_count_; ++index) {
            const auto root = read_root(index);
            if ((index != 0 && root <= previous) ||
                read_native<std::uint32_t>(record(root).data()) != 0)
                throw std::invalid_argument("Legacy roots must be ordered, distinct and ready");
            previous = root;
        }
    }
    [[nodiscard]] std::uint32_t get_node_count() const { return node_count_; }
    [[nodiscard]] std::uint32_t get_root_count() const { return root_count_; }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t index) const {
        if (index >= root_count_) throw std::invalid_argument("Root position outside legacy rank");
        const auto root = read_native<std::uint32_t>(roots_.data() + std::size_t{index} * sizeof(std::uint32_t));
        if (root >= node_count_) throw std::invalid_argument("Root points outside legacy rank");
        return root;
    }
    // The node's 39-byte record.
    [[nodiscard]] std::span<std::byte> record(std::uint32_t index) const {
        if (index >= node_count_) throw std::invalid_argument("Node index outside legacy rank");
        return records_.subspan(std::size_t{index} * legacy_format::record_bytes, legacy_format::record_bytes);
    }
    [[nodiscard]] DependencyListView read_dependents(std::uint32_t index, DependencyEvent event) const {
        const auto bytes = record(index);
        const auto position = event == DependencyEvent::Finish
            ? legacy_format::finish_count_offset : legacy_format::start_count_offset;
        const auto count = read_native<std::uint32_t>(bytes.data() + position);
        const auto begin = read_native<std::uint32_t>(bytes.data() + position + sizeof(std::uint32_t));
        const auto entry_count = dependency_bytes_.size() / sizeof(std::uint32_t);
        if (begin > entry_count || count > entry_count - begin)
            throw std::invalid_argument("Legacy dependency list exceeds appendix");
        return DependencyListView::from_legacy_u32(dependency_bytes_.subspan(std::size_t{begin} * sizeof(std::uint32_t),
                                                 std::size_t{count} * sizeof(std::uint32_t)));
    }
    std::uint32_t decrement_dependency_count(std::uint32_t index) const {
        const auto bytes = record(index);
        const auto count = read_native<std::uint32_t>(bytes.data());
        if (count == 0) throw std::invalid_argument("Dependency counter is already zero");
        write_native(bytes.data(), count - 1);
        return count - 1;
    }
    [[nodiscard]] std::uint32_t read_dependency_count(std::uint32_t index) const {
        return read_native<std::uint32_t>(record(index).data());
    }

private:
    std::uint32_t node_count_ = 0, root_count_ = 0;
    std::span<std::byte> roots_, records_;
    std::span<const std::byte> dependency_bytes_;
};

// Reads the original fixed-layout format through the same field bindings as
// SelfDescribingTraceReader; TraceReader selects it for files with the legacy magic number. A
// legacy record holds only the standard fields, at fixed positions, so bindings to custom fields
// are rejected. Several bindings may read the same field. Views mapped storage; the caller keeps
// it alive and writable.
class LegacyTraceReader {
public:
    template<class Output>
    LegacyTraceReader(std::span<std::byte> file, std::vector<FieldBinding<Output>> bindings,
                      std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : file_(file), validator_(std::move(validate_node)) {
        read_header();
        for (const auto& binding : bindings) copies_.push_back(resolve(binding));
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>)
            copies_.push_back(resolve(DependencyGraph::get_count_binding<Output>()));
    }

    template<class Output>
    LegacyTraceReader(std::span<std::byte> file, std::initializer_list<FieldBinding<Output>> bindings,
                      std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : LegacyTraceReader(file, std::vector<FieldBinding<Output>>(bindings), std::move(validate_node)) {}

    LegacyTraceReader(const LegacyTraceReader&) = delete;
    LegacyTraceReader& operator=(const LegacyTraceReader&) = delete;

    [[nodiscard]] std::uint32_t get_rank_count() const { return static_cast<std::uint32_t>(ranks_.size()); }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const { return get_rank(rank).get_node_count(); }
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const { return get_rank(rank).get_root_count(); }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const {
        return get_rank(rank).read_root(index);
    }
    // Legacy files are read at fixed offsets, without a plan runner.
    [[nodiscard]] std::string_view get_runner_name() const { return "legacy"; }

    template<class Output>
    [[nodiscard]] Output read_node(std::uint32_t rank, std::uint32_t index) const {
        const auto& validate = validator_.get<Output>();
        const auto& view = get_rank(rank);
        const auto record = view.record(index);
        Output output{};
        std::byte* const targets[] = {reinterpret_cast<std::byte*>(&output)};
        for (const auto& copy : copies_) copy.store(record, targets);
        if constexpr (std::derived_from<Output, BaseDeserializedGraphNode>) {
            output.node_index = index;
            output.finish_dependents = view.read_dependents(index, DependencyEvent::Finish);
            output.start_dependents = view.read_dependents(index, DependencyEvent::Start);
        }
        if (validate) validate(output);
        return output;
    }

    // Roots were checked when the file was opened: ordered, distinct and without dependencies.
    template<std::derived_from<BaseDeserializedGraphNode> Node>
    [[nodiscard]] std::vector<Node> read_root_nodes(std::uint32_t rank) const {
        std::vector<Node> nodes;
        nodes.reserve(get_root_count(rank));
        for (std::uint32_t index = 0; index < get_root_count(rank); ++index)
            nodes.push_back(read_node<Node>(rank, read_root(rank, index)));
        return nodes;
    }

    [[nodiscard]] DependencyListView read_dependents(
        std::uint32_t rank, std::uint32_t index, DependencyEvent event) const {
        return get_rank(rank).read_dependents(index, event);
    }
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
        return get_rank(rank).decrement_dependency_count(index);
    }
    [[nodiscard]] std::uint64_t read_dependency_count(std::uint32_t rank, std::uint32_t index) const {
        return get_rank(rank).read_dependency_count(index);
    }
    // The header stores the largest CPU and NIC; no other resource is described.
    [[nodiscard]] std::uint64_t get_resource_count(FieldIdentity field) const {
        if (field == FieldIdentity{FieldId::Cpu}) return cpu_count_;
        if (field == FieldIdentity{FieldId::Nic}) return nic_count_;
        throw std::invalid_argument("Legacy traces have no field '" + schema_helpers::get_field_label(field) + "'");
    }

private:
    // One bound field: where its bytes lie in a record and the member that receives them.
    struct FieldCopy {
        std::size_t source_offset;
        std::uint8_t source_bytes;
        bool maps_any; // the value ~0 means ANY and becomes the binding's wildcard value
        FieldDestination destination;

        void store(std::span<const std::byte> record, std::byte* const* targets) const {
            const auto* source = record.data() + source_offset;
            std::uint64_t value = 0;
            switch (source_bytes) {
            case 1: value = read_native<std::uint8_t>(source); break;
            case 4: value = read_native<std::uint32_t>(source); break;
            case 8: value = read_native<std::uint64_t>(source); break;
            default: std::unreachable();
            }
            if (maps_any && value == std::numeric_limits<std::uint32_t>::max()) value = destination.wildcard_value;
            destination.store(targets, value);
        }
    };

    // Where each standard field lies in a legacy record. Peer and tag store ANY as ~0.
    template<class Output>
    [[nodiscard]] static FieldCopy resolve(const FieldBinding<Output>& binding) {
        const auto& field = binding.requirement.field;
        const auto* id = std::get_if<FieldId>(&field);
        struct Source { std::size_t offset; std::uint8_t bytes; bool any; };
        const auto source = [&]() -> Source {
            switch (id ? *id : FieldId{}) {
            case FieldId::DependencyCount: return {legacy_format::dependency_count_offset, 4, false};
            case FieldId::Type: return {legacy_format::type_offset, 1, false};
            case FieldId::Peer: return {legacy_format::peer_offset, 4, true};
            case FieldId::Size: return {legacy_format::size_offset, 8, false};
            case FieldId::Tag: return {legacy_format::tag_offset, 4, true};
            case FieldId::Cpu: return {legacy_format::cpu_field_offset, 1, false};
            case FieldId::Nic: return {legacy_format::nic_field_offset, 1, false};
            default:
                throw std::invalid_argument("Legacy traces have no field '" +
                                            schema_helpers::get_field_label(field) + "'");
            }
        }();
        // Legacy values are unsigned and may use all their bits; the member must hold every one.
        const auto& requirement = binding.requirement;
        const auto needed_bits = 8u * source.bytes + (requirement.type == FieldType::SignedInteger ? 1u : 0u);
        if (requirement.capacity_bits < needed_bits)
            throw std::invalid_argument("Member is too small for legacy field '" +
                                        schema_helpers::get_field_label(field) + "'");
        return {source.offset, source.bytes, source.any && binding.wildcard_destination.has_value(),
                {OutputStruct::Node, binding.destination_offset, binding.destination_size,
                 binding.wildcard_destination ? integer_to_bits(*binding.wildcard_destination) : 0}};
    }

    [[nodiscard]] const LegacyRankView& get_rank(std::uint32_t rank) const {
        if (rank >= ranks_.size()) throw std::invalid_argument("Rank outside legacy schedule");
        return ranks_[rank];
    }

    // Reads the header and opens every rank, checking its bounds, roots and dependency indices.
    void read_header() {
        if (file_.size() < legacy_format::prefix_bytes ||
            read_native<std::uint64_t>(file_.data()) != legacy_format_magic)
            throw std::invalid_argument("Invalid legacy schedule header");
        const auto rank_count = read_native<std::uint32_t>(file_.data() + legacy_format::rank_count_offset);
        cpu_count_ = std::to_integer<std::uint8_t>(file_[legacy_format::cpu_offset]) + 1u;
        nic_count_ = std::to_integer<std::uint8_t>(file_[legacy_format::nic_offset]) + 1u;
        const auto table_end = legacy_format::prefix_bytes +
                               std::uint64_t{rank_count} * legacy_format::rank_table_entry_bytes;
        if (table_end > file_.size()) throw std::invalid_argument("Truncated legacy rank table");
        // Legacy table offsets exclude the magic. Check before addition to avoid overflow.
        const auto size = file_.size() - legacy_format::magic_bytes;
        ranks_.reserve(rank_count);
        for (std::uint32_t rank = 0; rank < rank_count; ++rank) {
            const auto* entry = file_.data() + legacy_format::prefix_bytes
                + std::size_t{rank} * legacy_format::rank_table_entry_bytes;
            const auto begin = read_native<std::uint64_t>(entry);
            const auto end = read_native<std::uint64_t>(entry + sizeof(std::uint64_t));
            if (begin < table_end - legacy_format::magic_bytes || begin > end || end > size)
                throw std::invalid_argument("Invalid legacy rank section bounds");
            ranks_.emplace_back(file_.subspan(begin + legacy_format::magic_bytes, end - begin));
        }
    }

    std::span<std::byte> file_;
    NodeValidator validator_;
    std::vector<FieldCopy> copies_;
    std::vector<LegacyRankView> ranks_;
    std::uint32_t cpu_count_ = 0, nic_count_ = 0;
};

} // namespace traceschema
