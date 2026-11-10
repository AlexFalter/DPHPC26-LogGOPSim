#ifndef LOGGOPSIM_DEPENDENCYLISTVIEW_HPP
#define LOGGOPSIM_DEPENDENCYLISTVIEW_HPP

#include "trace_format/BinaryIO.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <stdexcept>

namespace traceschema {

// Which event releases a node's list of dependents.
enum class DependencyEvent { Start, Finish };

// A borrowed list of node numbers, used for a node's start and completion dependents.
// Each index is read on demand from the mapped trace: bit-packed in the new format,
// or an unaligned 32-bit integer in the legacy format. The scheduler can iterate
// either representation in the same way, without copying indices into a vector.
// The mapped storage must outlive this view and remain unchanged.
class DependencyListView {
public:
    DependencyListView() = default;

    // A slice of a bit-packed index array. Checks only that the slice fits the bytes; the
    // indices themselves are checked by validate, once per array rather than once per slice.
    static DependencyListView from_bit_packed(
        std::span<const std::byte> bytes, std::uint32_t first_index,
        std::uint32_t count, std::uint32_t index_bits) {
        if (index_bits == 0 || index_bits > 64) throw std::invalid_argument("Invalid node index width");
        const auto first_bit = std::uint64_t{first_index} * index_bits;
        const auto end_bit = first_bit + std::uint64_t{count} * index_bits;
        if (round_up_to_byte(end_bit) / bits_per_byte > bytes.size())
            throw std::invalid_argument("Truncated node index list");
        DependencyListView result;
        result.bytes_ = bytes.subspan(first_bit / bits_per_byte);
        result.first_bit_in_byte_ = static_cast<std::uint8_t>(first_bit % bits_per_byte);
        result.index_bits_ = static_cast<std::uint8_t>(index_bits);
        result.count_ = count;
        return result;
    }

    // Legacy indices use host byte order and may be unaligned. As for from_bit_packed, the
    // indices are checked by validate.
    static DependencyListView from_legacy_u32(std::span<const std::byte> bytes) {
        if (bytes.size() % sizeof(std::uint32_t) != 0 ||
            bytes.size() / sizeof(std::uint32_t) > UINT32_MAX)
            throw std::invalid_argument("Invalid legacy node index list");
        DependencyListView result;
        result.bytes_ = bytes;
        result.count_ = static_cast<std::uint32_t>(bytes.size() / sizeof(std::uint32_t));
        result.legacy_u32_ = true;
        return result;
    }

    // Throws unless every index is a node of a rank with node_count nodes. Readers call it once
    // for a rank's whole dependency array when they open a file; views of slices of a checked
    // array need no further check.
    void validate(std::uint32_t node_count) const {
        for (std::uint32_t index = 0; index < count_; ++index)
            if (read_index(index) >= node_count)
                throw std::invalid_argument("Dependency points outside rank");
    }

    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] std::uint32_t operator[](std::size_t index) const {
        if (index >= count_) throw std::out_of_range("Node index list position out of bounds");
        return static_cast<std::uint32_t>(read_index(index));
    }

    // Supports range-based for loops and standard algorithms. Defined below so the
    // list's storage and decoding remain separate from the iterator mechanics.
    class Iterator;
    [[nodiscard]] Iterator begin() const;
    [[nodiscard]] Iterator end() const;

private:

    // Keep the full value until validation has checked it: a corrupt 64-bit index
    // must not wrap into a valid 32-bit node number.
    [[nodiscard]] std::uint64_t read_index(std::size_t index) const {
        if (legacy_u32_) {
            // Legacy storage uses native byte order and can be unaligned.
            std::uint32_t value;
            std::memcpy(&value, bytes_.data() + index * sizeof(value), sizeof(value));
            return value;
        }
        const auto bit_offset = first_bit_in_byte_ + std::uint64_t{index} * index_bits_;
        return bit_packing::read_bits(bytes_, bit_offset, index_bits_);
    }

    std::span<const std::byte> bytes_;
    std::uint32_t count_ = 0;
    std::uint8_t index_bits_ = 0;
    std::uint8_t first_bit_in_byte_ = 0;
    bool legacy_u32_ = false;
};

// The iterator stores only a view and a position. Dereferencing reads one index;
// advancing moves to the next position without decoding or allocating anything.
class DependencyListView::Iterator {
public:
    // Standard iterator traits let algorithms recognize the values and traversal type.
    using value_type = std::uint32_t; // Dereferencing returns a node index.
    using difference_type = std::ptrdiff_t; // The signed type used for distances.
    using iterator_concept = std::forward_iterator_tag; // Copies advance independently.

    Iterator() = default;
    Iterator(const DependencyListView* view, std::size_t position) : view_(view), position_(position) {}

    [[nodiscard]] value_type operator*() const { return (*view_)[position_]; }
    Iterator& operator++() {
        ++position_;
        return *this;
    }
    Iterator operator++(int) {
        const auto previous = *this;
        ++*this;
        return previous;
    }
    bool operator==(const Iterator&) const = default;

private:
    const DependencyListView* view_ = nullptr;
    std::size_t position_ = 0;
};

inline DependencyListView::Iterator DependencyListView::begin() const { return {this, 0}; }
inline DependencyListView::Iterator DependencyListView::end() const { return {this, count_}; }

} // namespace traceschema
#endif
