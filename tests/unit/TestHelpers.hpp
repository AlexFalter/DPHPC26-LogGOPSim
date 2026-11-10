#pragma once

#include "binary_writing/RecordPacking.hpp"

#include <gmock/gmock.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace test_helpers {

// Matches a callable that throws std::invalid_argument whose message contains text:
// EXPECT_THAT([&] { parse(bad); }, RejectsWith("name"));
inline auto RejectsWith(std::string text) {
    return testing::ThrowsMessage<std::invalid_argument>(testing::HasSubstr(std::move(text)));
}

// A file in GoogleTest's temporary directory, named after the running test and removed when the
// object goes out of scope. Pass contents to create it, or leave it for the code under test.
class TemporaryFile {
public:
    explicit TemporaryFile(const std::string& contents = {}, bool create = true) : path_(make_path()) {
        if (!create) return;
        std::ofstream output(path_, std::ios::binary);
        output << contents;
        if (!output) throw std::runtime_error("Cannot write " + path_);
    }
    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }

private:
    static std::string make_path() {
        static std::atomic<unsigned> sequence{0};
        const auto* test = testing::UnitTest::GetInstance()->current_test_info();
        auto name = test ? std::string(test->test_suite_name()) + "." + test->name() : std::string("global");
        std::ranges::replace(name, '/', '_'); // parameterized tests have names like Suite/Test/0
        return testing::TempDir() + name + "." + std::to_string(sequence++);
    }

    std::string path_;
};

// Private directories avoid sharing JIT caches or generated files across test processes.
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        auto pattern = testing::TempDir() + "loggopsim-test-XXXXXX";
        const auto* created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("Cannot create temporary directory");
        path_ = created;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const std::string& value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        if (::setenv(name, value.c_str(), 1) != 0) throw std::runtime_error("Cannot set test environment");
    }
    ~ScopedEnvironment() {
        if (previous_) ::setenv(name_.c_str(), previous_->c_str(), 1);
        else ::unsetenv(name_.c_str());
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;
private:
    std::string name_;
    std::optional<std::string> previous_;
};

// Independent bit-by-bit oracle: keep test expectations separate from production runners.
inline std::vector<traceschema::FieldValue> decode_record(
    const traceschema::RecordLayout& layout, std::span<const std::byte> bytes) {
    using namespace traceschema;
    if (bytes.size() != layout.get_record_size_bytes())
        throw std::invalid_argument("Incorrect reference record size");
    std::vector<FieldValue> values;
    std::optional<std::uint64_t> operation;
    bool wildcard_peer = false;
    for (const auto& field : layout.fields) {
        if (field.bit_width > 64 || std::uint64_t{field.bit_offset} + field.bit_width > bytes.size() * 8)
            throw std::invalid_argument("Reference field outside record");
        std::uint64_t code = 0;
        for (unsigned bit = 0; bit < field.bit_width; ++bit) {
            const auto position = std::uint64_t{field.bit_offset} + bit;
            if ((std::to_integer<unsigned>(bytes[position / 8]) >> (position % 8)) & 1u)
                code |= std::uint64_t{1} << bit;
        }
        const auto& encoding = field.offset_encoding;
        FieldValue value;
        if (code > encoding.maximum_offset) {
            if (!encoding.has_wildcard || encoding.maximum_offset == UINT64_MAX || code != encoding.maximum_offset + 1)
                throw std::invalid_argument("Invalid reference field code");
            value = Wildcard{};
        } else if (field.type == FieldType::UnsignedInteger) {
            const auto base = std::get<std::uint64_t>(encoding.value_base);
            if (code > UINT64_MAX - base) throw std::invalid_argument("Reference unsigned overflow");
            value = base + code;
        } else {
            const auto base = std::get<std::int64_t>(encoding.value_base);
            if (code > std::uint64_t{INT64_MAX} - static_cast<std::uint64_t>(base))
                throw std::invalid_argument("Reference signed overflow");
            value = std::bit_cast<std::int64_t>(static_cast<std::uint64_t>(base) + code);
        }
        if (field.field == FieldIdentity{FieldId::Type}) operation = std::get<std::uint64_t>(value);
        if (field.field == FieldIdentity{FieldId::Peer}) wildcard_peer = std::holds_alternative<Wildcard>(value);
        values.push_back(value);
    }
    if (operation == std::to_underlying(OperationType::Send) && wildcard_peer)
        throw std::invalid_argument("Send cannot use a wildcard destination");
    return values;
}

// Packs one record into a new buffer of the layout's record size.
inline std::vector<std::byte> pack_record(const traceschema::RecordPacker& packer,
                                          const traceschema::RecordValues& values) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(packer.get_layout().get_record_size_bytes()));
    packer.pack(values, bytes);
    return bytes;
}

} // namespace test_helpers
