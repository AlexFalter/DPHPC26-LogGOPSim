#include "binary_reading/MappedTraceFile.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace traceschema {

MappedTraceFile::MappedTraceFile(const char* filename, bool shared) : shared_(shared) {
    const int fd = open(filename, shared ? O_RDWR : O_RDONLY);
    if (fd < 0) throw std::runtime_error("Cannot open simulation schedule");
    struct stat info{};
    if (fstat(fd, &info) != 0 || info.st_size <= 0 || !std::in_range<std::size_t>(info.st_size)) {
        close(fd);
        throw std::invalid_argument("Invalid schedule file size");
    }
    size_ = static_cast<std::size_t>(info.st_size);
    data_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
    close(fd);
    if (data_ == MAP_FAILED) throw std::runtime_error("Cannot mmap simulation schedule");
}

MappedTraceFile::~MappedTraceFile() {
    munmap(data_, size_);
}

} // namespace traceschema
