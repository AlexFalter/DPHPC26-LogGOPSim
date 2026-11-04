#include "trace_format/BinaryIO.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

using namespace traceschema;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
static void rejects(Function function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Incorrect buffer size accepted");
}

template<class Integer>
static void check_limits() {
    std::array<std::byte, sizeof(Integer)> bytes;
    for (const auto value : {Integer{0}, std::numeric_limits<Integer>::max()}) {
        write_little_endian(value, bytes);
        check(read_little_endian<Integer>(bytes) == value, "Integer boundary round trip failed");
    }
}

int main() {
    try {
        std::array<std::byte, 4> bytes;
        write_little_endian(std::uint32_t{0x12345678}, bytes);
        const std::array expected{std::byte{0x78}, std::byte{0x56}, std::byte{0x34}, std::byte{0x12}};
        check(bytes == expected, "Incorrect little-endian bytes");
        check(read_little_endian<std::uint32_t>(expected) == 0x12345678, "Incorrect little-endian read");

        std::array<std::byte, 10> buffer;
        buffer.fill(std::byte{0xff});
        auto view = std::span(buffer).subspan(1, 8);
        write_little_endian(std::uint64_t{0x0123456789abcdef}, view);
        const std::array expected_buffer{std::byte{0xff}, std::byte{0xef}, std::byte{0xcd},
            std::byte{0xab}, std::byte{0x89}, std::byte{0x67}, std::byte{0x45},
            std::byte{0x23}, std::byte{0x01}, std::byte{0xff}};
        check(buffer == expected_buffer, "Unaligned view overwrote neighboring bytes");
        check(read_little_endian<std::uint64_t>(view) == 0x0123456789abcdef,
              "Unaligned view read failed");
        rejects([&] { write_little_endian(std::uint64_t{0}, std::span(buffer).first(7)); });
        rejects([&] { write_little_endian(std::uint64_t{0}, std::span(buffer).first(9)); });
        rejects([&] { (void)read_little_endian<std::uint64_t>(std::span(buffer).first(7)); });
        rejects([&] { (void)read_little_endian<std::uint64_t>(std::span(buffer).first(9)); });
        check(buffer == expected_buffer, "Rejected write changed the buffer");
        check_limits<std::uint8_t>();
        check_limits<std::uint16_t>();
        check_limits<std::uint32_t>();
        check_limits<std::uint64_t>();
        std::cout << "Byte order checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
