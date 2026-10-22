#ifndef LOGGOPSIM_NODEINDEXVIEW_HPP
#define LOGGOPSIM_NODEINDEXVIEW_HPP

#include "trace_format/BinaryIO.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <span>
#include <stdexcept>

namespace traceschema {

// A read-only list of node indices in mapped storage. The storage must outlive
// the view and remain unchanged. No indices are copied or allocated.
class NodeIndexView {
public:
    NodeIndexView() = default;

    static NodeIndexView packed(std::span<const std::byte> bytes, std::uint32_t begin,
                                std::uint32_t count, std::uint32_t bits, std::uint32_t node_count) {
        if (bits == 0 || bits > 64) throw std::invalid_argument("Invalid node index width");
        const auto first_bit = std::uint64_t{begin} * bits;
        const auto end_bit = first_bit + std::uint64_t{count} * bits;
        if ((end_bit + 7) / 8 > bytes.size())
            throw std::invalid_argument("Truncated node index list");
        NodeIndexView result;
        result.bytes_ = bytes.subspan(first_bit / 8);
        result.first_bit_ = static_cast<std::uint8_t>(first_bit % 8);
        result.bits_ = static_cast<std::uint8_t>(bits);
        result.count_ = count;
        result.validate(node_count);
        return result;
    }

    // Legacy indices use host byte order and may be unaligned.
    static NodeIndexView native(std::span<const std::byte> bytes, std::uint32_t node_count) {
        if (bytes.size() % sizeof(std::uint32_t) != 0 ||
            bytes.size() / sizeof(std::uint32_t) > UINT32_MAX)
            throw std::invalid_argument("Invalid legacy node index list");
        NodeIndexView result;
        result.bytes_ = bytes;
        result.count_ = static_cast<std::uint32_t>(bytes.size() / sizeof(std::uint32_t));
        result.native_ = true;
        result.validate(node_count);
        return result;
    }

    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] std::uint32_t operator[](std::size_t index) const {
        if (index >= count_) throw std::out_of_range("Node index list position out of bounds");
        if (native_) {
            std::uint32_t value;
            std::memcpy(&value, bytes_.data() + index * sizeof(value), sizeof(value));
            return value;
        }
        return static_cast<std::uint32_t>(bit_packing::read_bits(
            bytes_, first_bit_ + std::uint64_t{index} * bits_, bits_));
    }

    class Iterator {
    public:
        using value_type = std::uint32_t;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;
        using reference = value_type;
        using pointer = void;
        Iterator() = default;
        Iterator(const NodeIndexView* view, std::size_t position) : view_(view), position_(position) {}
        value_type operator*() const { return (*view_)[position_]; }
        Iterator& operator++() { ++position_; return *this; }
        Iterator operator++(int) { auto previous = *this; ++*this; return previous; }
        bool operator==(const Iterator&) const = default;
    private:
        const NodeIndexView* view_ = nullptr;
        std::size_t position_ = 0;
    };
    [[nodiscard]] Iterator begin() const { return {this, 0}; }
    [[nodiscard]] Iterator end() const { return {this, count_}; }

private:
    void validate(std::uint32_t node_count) const {
        for (std::uint32_t index = 0; index < count_; ++index) {
            // Check the full encoded value before narrowing a 64-bit index.
            const auto value = native_ ? std::uint64_t{(*this)[index]} :
                bit_packing::read_bits(bytes_, first_bit_ + std::uint64_t{index} * bits_, bits_);
            if (value >= node_count) throw std::invalid_argument("Dependency points outside rank");
        }
    }
    std::span<const std::byte> bytes_;
    std::uint32_t count_ = 0;
    std::uint8_t bits_ = 0;
    std::uint8_t first_bit_ = 0;
    bool native_ = false;
};

} // namespace traceschema
#endif
