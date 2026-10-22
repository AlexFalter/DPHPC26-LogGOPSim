#ifndef LOGGOPSIM_NODEDECODER_HPP
#define LOGGOPSIM_NODEDECODER_HPP

#include "trace_format/RecordLayout.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <optional>
#include <stdexcept>
#include <vector>
#include <utility>

namespace traceschema {

// Resolve consumer requirements once, then decode only those fields.
class NodeDecoder {
public:
    NodeDecoder(const RecordLayout& layout, const FieldRequirements& requirements);
    [[nodiscard]] std::span<const FieldStorageInfo> get_selected_fields() const { return fields_; }
    void decode_fields(std::span<const std::byte> record, std::span<FieldValue> output) const;
    // A consumer can assign each decoded value directly, without an intermediate vector.
    template<class Consumer>
    requires std::invocable<Consumer&, std::size_t, const FieldStorageInfo&, FieldValue>
    void decode_fields(std::span<const std::byte> record, Consumer&& consume,
                       std::size_t field_count = std::dynamic_extent) const {
        check_record_size(record.size());
        const auto fields = get_selected_fields();
        if (field_count == std::dynamic_extent) field_count = fields.size();
        if (field_count > fields.size()) throw std::invalid_argument("Selected field count out of bounds");
        std::optional<std::uint64_t> operation;
        bool wildcard_peer = false;
        for (std::size_t index = 0; index < field_count; ++index) {
            auto value = decode_field(record, fields[index]);
            if (index == operation_index_) operation = std::get<std::uint64_t>(value);
            if (index == peer_index_) wildcard_peer = std::holds_alternative<Wildcard>(value);
            consume(index, fields[index], std::move(value));
        }
        if (operation == std::to_underlying(OperationType::Send) && wildcard_peer)
            throw std::invalid_argument("Send cannot use a wildcard destination");
    }
    // Shared by selected-value reads and simulator-specific assignments.
    [[nodiscard]] static FieldValue decode_field(std::span<const std::byte> record, const FieldStorageInfo& field);

private:
    std::vector<FieldStorageInfo> fields_;
    std::optional<std::size_t> operation_index_;
    std::optional<std::size_t> peer_index_;
    std::uint64_t record_size_bytes_; // Total packed bytes per node, including unselected fields and padding.
    void check_record_size(std::size_t size) const;
};

} // namespace traceschema
#endif
