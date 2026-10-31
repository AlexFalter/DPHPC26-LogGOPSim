#ifndef LOGGOPSIM_DECODEPLAN_HPP
#define LOGGOPSIM_DECODEPLAN_HPP

#include "trace_format/RecordLayout.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace traceschema {

struct FieldDestination {
    std::uint8_t target;
    std::uint32_t offset;
    std::uint8_t size;
    std::uint64_t wildcard_bits;
};

// Flat per-field decode steps, each one unaligned 64-bit load. Records the plan cannot
// accept (codes outside the encoded range, sends to ANY) are left to the generic decoder,
// which reports the error.
class DecodePlan {
public:
    struct Step {
        std::uint32_t byte;
        std::uint8_t shift;
        std::uint8_t bytes;
        bool spill;
        bool wildcard;
        std::uint64_t mask;
        std::uint64_t base;
        std::uint64_t maximum;
        std::uint8_t target = 0;
        std::uint8_t size = 0;
        std::uint32_t offset = 0;
        std::uint64_t wildcard_bits = 0;
    };

    [[nodiscard]] static std::optional<Step> make_step(const FieldStorageInfo& field);
    [[nodiscard]] static std::optional<DecodePlan> create(std::span<const FieldStorageInfo> fields,
                                                          std::span<const FieldDestination> destinations);

    [[nodiscard]] std::span<const Step> get_steps() const { return steps_; }
    [[nodiscard]] int get_type_step() const { return type_step_; }
    [[nodiscard]] int get_peer_step() const { return peer_step_; }
    // Bytes read from the record start; the caller guarantees they are mapped.
    [[nodiscard]] std::size_t get_load_extent() const { return load_extent_; }

    static std::uint64_t load_code(const std::byte* record, const Step& step) {
        std::uint64_t word;
        std::memcpy(&word, record + step.byte, sizeof(word));
        if constexpr (std::endian::native == std::endian::big) word = std::byteswap(word);
        word >>= step.shift;
        if (step.spill) word |= std::uint64_t(record[step.byte + 8]) << (64 - step.shift);
        return word & step.mask;
    }

    bool decode(const std::byte* record, std::byte* const* targets) const {
        for (const auto& step : steps_) {
            const auto code = load_code(record, step);
            std::uint64_t value;
            if (code <= step.maximum) [[likely]] value = step.base + code;
            else if (step.wildcard && code == step.maximum + 1) value = step.wildcard_bits;
            else return false;
            store(targets[step.target] + step.offset, step.size, value);
        }
        if (type_step_ >= 0 && peer_step_ >= 0) {
            const auto& type = steps_[type_step_];
            const auto& peer = steps_[peer_step_];
            if (type.base + load_code(record, type) == std::uint64_t(OperationType::Send) &&
                peer.wildcard && load_code(record, peer) == peer.maximum + 1)
                return false;
        }
        return true;
    }

    // Counter fields have base zero; returns nothing when the generic path must handle it.
    static std::optional<std::uint64_t> decrement(std::byte* record, const Step& step) {
        if (step.spill) return std::nullopt;
        std::uint64_t word;
        std::memcpy(&word, record + step.byte, sizeof(word));
        if constexpr (std::endian::native == std::endian::big) word = std::byteswap(word);
        const auto code = (word >> step.shift) & step.mask;
        if (code == 0 || code > step.maximum) return std::nullopt;
        word -= std::uint64_t{1} << step.shift;
        if constexpr (std::endian::native == std::endian::big) word = std::byteswap(word);
        std::memcpy(record + step.byte, &word, step.bytes);
        return code - 1;
    }

private:
    static void store(std::byte* target, std::uint8_t size, std::uint64_t value) {
        switch (size) {
        case 1: { const auto narrow = std::uint8_t(value); std::memcpy(target, &narrow, 1); break; }
        case 2: { const auto narrow = std::uint16_t(value); std::memcpy(target, &narrow, 2); break; }
        case 4: { const auto narrow = std::uint32_t(value); std::memcpy(target, &narrow, 4); break; }
        default: std::memcpy(target, &value, 8);
        }
    }

    std::vector<Step> steps_;
    std::size_t load_extent_ = 0;
    int type_step_ = -1;
    int peer_step_ = -1;
};

} // namespace traceschema
#endif
