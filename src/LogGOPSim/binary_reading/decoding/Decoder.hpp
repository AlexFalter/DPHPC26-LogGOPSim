#ifndef LOGGOPSIM_DECODER_HPP
#define LOGGOPSIM_DECODER_HPP

#include "binary_reading/decoding/DecodePlan.hpp"
#include "binary_reading/decoding/runners/PlanRunner.hpp"
#include "trace_format/RecordLayout.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace traceschema {

// Resolves field bindings once and decodes records into the caller's output structs.
// The selected runner handles decoding and mapping boundaries.
class Decoder final {
public:
    // Each requirement has one destination in an output struct.
    Decoder(const RecordLayout& layout, const FieldRequirements& requirements,
            std::span<const FieldDestination> destinations);
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    [[nodiscard]] std::span<const FieldStorageInfo> get_selected_fields() const { return fields_; }
    // The runner in use, as its LOGGOPSIM_DECODER value.
    [[nodiscard]] std::string_view get_runner_name() const { return runner_->get_name(); }

    // Writes the selected fields of one record to their destinations. readable_bytes counts
    // the mapped bytes from the record start, which the plan runner may read beyond the record.
    // Defined here so that the per-record path inlines into the reader.
    void decode_into(std::span<const std::byte> record, std::size_t readable_bytes,
                     std::byte* const* targets) const {
        if (record.size() != record_size_bytes_)
            throw std::invalid_argument("Byte buffer size does not match selected node record size");
        runner_->decode(record, readable_bytes, targets);
    }

private:
    std::vector<FieldStorageInfo> fields_;
    std::uint64_t record_size_bytes_; // Complete record size, including unselected fields and padding.
    std::unique_ptr<const PlanRunner> runner_;
};

} // namespace traceschema
#endif
