#include "binary_reading/TraceReader.hpp"

#include "trace_format/BinaryFormat.hpp"

#include <stdexcept>

namespace traceschema {

TraceFormat detect_trace_format(std::span<const std::byte> file) {
    if (file.size() < sizeof(std::uint64_t)) throw std::invalid_argument("Truncated schedule magic");
    const auto magic = file.first(sizeof(std::uint64_t));
    // --save-mem overwrites the magic in little-endian; older runs wrote it natively.
    if (read_native<std::uint64_t>(magic.data()) == invalid_schedule_magic ||
        read_little_endian<std::uint64_t>(magic) == invalid_schedule_magic)
        throw std::invalid_argument("Schedule was invalidated by a prior simulation run");
    if (has_binary_header_magic(file)) return TraceFormat::SelfDescribing;
    if (read_native<std::uint64_t>(magic.data()) == legacy_format_magic) return TraceFormat::Legacy;
    throw std::invalid_argument("Unrecognized schedule format");
}

} // namespace traceschema
