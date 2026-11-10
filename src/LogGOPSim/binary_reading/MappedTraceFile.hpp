#pragma once

#include <cstddef>
#include <span>

namespace traceschema {
// The same mmap owner serves legacy and self-describing files.
class MappedTraceFile {
public:
    MappedTraceFile(const char* filename, bool shared);
    ~MappedTraceFile();
    MappedTraceFile(const MappedTraceFile&) = delete;
    MappedTraceFile& operator=(const MappedTraceFile&) = delete;
    [[nodiscard]] bool is_shared() const { return shared_; }
    std::span<std::byte> bytes() const { return {static_cast<std::byte*>(data_), size_}; }

private:
    void* data_;
    std::size_t size_;
    bool shared_;
};

} // namespace traceschema
