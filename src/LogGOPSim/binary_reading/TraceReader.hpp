#ifndef LOGGOPSIM_TRACEREADER_HPP
#define LOGGOPSIM_TRACEREADER_HPP

#include "binary_reading/LegacyTraceReader.hpp"
#include "binary_reading/SelfDescribingTraceReader.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace traceschema {

enum class TraceFormat { Legacy, SelfDescribing };

// The format named by the file's magic number. Throws for files invalidated by a simulation run
// with --save-mem and for unknown formats.
[[nodiscard]] TraceFormat detect_trace_format(std::span<const std::byte> file);

// Reads a trace in either format through the same field bindings: the format is detected from
// the magic number, and the reader for it is created once. Legacy files hold only the standard
// fields; is_legacy() lets a consumer bind just those. Views mapped storage; the caller keeps it
// alive and writable.
class TraceReader {
    // Defined first: the forwarding functions below deduce their return types from it. The
    // constructor always emplaces a reader, so the monostate is never visited.
    template<class Function>
    decltype(auto) visit(Function&& function) const {
        if (const auto* legacy = std::get_if<LegacyTraceReader>(&reader_)) return function(*legacy);
        return function(std::get<SelfDescribingTraceReader>(reader_));
    }
    template<class Function>
    decltype(auto) visit(Function&& function) {
        if (auto* legacy = std::get_if<LegacyTraceReader>(&reader_)) return function(*legacy);
        return function(std::get<SelfDescribingTraceReader>(reader_));
    }

public:
    template<class Output>
    TraceReader(std::span<std::byte> file, std::vector<FieldBinding<Output>> bindings,
                std::type_identity_t<std::function<void(const Output&)>> validate_node = {}) {
        if (detect_trace_format(file) == TraceFormat::Legacy)
            reader_.emplace<LegacyTraceReader>(file, std::move(bindings), std::move(validate_node));
        else
            reader_.emplace<SelfDescribingTraceReader>(file, std::move(bindings), std::move(validate_node));
    }

    template<class Output>
    TraceReader(std::span<std::byte> file, std::initializer_list<FieldBinding<Output>> bindings,
                std::type_identity_t<std::function<void(const Output&)>> validate_node = {})
        : TraceReader(file, std::vector<FieldBinding<Output>>(bindings), std::move(validate_node)) {}

    TraceReader(const TraceReader&) = delete;
    TraceReader& operator=(const TraceReader&) = delete;

    [[nodiscard]] bool is_legacy() const { return std::holds_alternative<LegacyTraceReader>(reader_); }

    [[nodiscard]] std::uint32_t get_rank_count() const {
        return visit([](const auto& reader) { return reader.get_rank_count(); });
    }
    [[nodiscard]] std::uint32_t get_node_count(std::uint32_t rank) const {
        return visit([&](const auto& reader) { return reader.get_node_count(rank); });
    }
    [[nodiscard]] std::uint32_t get_root_count(std::uint32_t rank) const {
        return visit([&](const auto& reader) { return reader.get_root_count(rank); });
    }
    [[nodiscard]] std::uint32_t read_root(std::uint32_t rank, std::uint32_t index) const {
        return visit([&](const auto& reader) { return reader.read_root(rank, index); });
    }
    // The plan runner decoding a self-describing file, or "legacy".
    [[nodiscard]] std::string_view get_runner_name() const {
        return visit([](const auto& reader) { return reader.get_runner_name(); });
    }

    template<class Output>
    [[nodiscard]] Output read_node(std::uint32_t rank, std::uint32_t index) const {
        return visit([&](const auto& reader) { return reader.template read_node<Output>(rank, index); });
    }
    template<std::derived_from<BaseDeserializedGraphNode> Node>
    [[nodiscard]] std::vector<Node> read_root_nodes(std::uint32_t rank) const {
        return visit([&](const auto& reader) { return reader.template read_root_nodes<Node>(rank); });
    }
    // The dependency list released by this event, without decoding the node.
    [[nodiscard]] DependencyListView read_dependents(
        std::uint32_t rank, std::uint32_t index, DependencyEvent event) const {
        return visit([&](const auto& reader) { return reader.read_dependents(rank, index, event); });
    }
    std::uint64_t decrement_dependency_count(std::uint32_t rank, std::uint32_t index) {
        return visit([&](auto& reader) { return reader.decrement_dependency_count(rank, index); });
    }
    // The stored dependency count, without changing the file.
    [[nodiscard]] std::uint64_t read_dependency_count(std::uint32_t rank, std::uint32_t index) const {
        return visit([&](const auto& reader) { return reader.read_dependency_count(rank, index); });
    }
    [[nodiscard]] std::uint64_t get_resource_count(const FieldIdentity& field) const {
        return visit([&](const auto& reader) { return reader.get_resource_count(field); });
    }

private:
    // Neither reader is movable, so both are built in place.
    std::variant<std::monostate, SelfDescribingTraceReader, LegacyTraceReader> reader_;
};

} // namespace traceschema
#endif
