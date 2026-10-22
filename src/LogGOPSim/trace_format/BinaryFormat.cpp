#include "trace_format/BinaryFormat.hpp"
#include "trace_format/BinaryIO.hpp"

#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace traceschema {

static constexpr std::array<std::byte, 8> format_magic{
    std::byte{'G'}, std::byte{'O'}, std::byte{'A'}, std::byte{'L'},
    std::byte{'S'}, std::byte{'D'}, std::byte{'0'}, std::byte{'1'}};
static constexpr std::uint16_t format_version = 2;
static constexpr std::size_t header_length_offset = format_magic.size() + sizeof(format_version);
static constexpr std::size_t prefix_size = header_length_offset
    + sizeof(BinaryHeader::header_size_bytes)
    + sizeof(RecordLayout::record_size_bits)
    + sizeof(std::uint32_t) // Field count.
    + sizeof(BinaryHeader::rank_count)
    + sizeof(std::uint8_t) // Index width occupies one byte on disk.
    + sizeof(BinaryHeader::rank_table_offset_bytes);
static constexpr std::size_t rank_table_offset_position = prefix_size - sizeof(std::uint64_t);
static constexpr std::size_t field_descriptor_size = 32;

bool has_binary_header_magic(std::span<const std::byte> bytes) {
    return bytes.size() >= format_magic.size() &&
           std::ranges::equal(bytes.first(format_magic.size()), format_magic);
}

enum class FieldIdentityKind : std::uint8_t { Standard = 0, Custom = 1 };

static void validate_node_index_bits(std::uint32_t node_index_bits) {
    if (node_index_bits < 1 || node_index_bits > 64)
        throw std::invalid_argument("Node index width must be 1..64 bits");
}

static void validate_rank_table_location(std::uint64_t header_size, std::uint32_t rank_count,
                                       std::uint64_t table_offset) {
    if (table_offset < header_size)
        throw std::invalid_argument("Rank table overlaps binary header");
    const auto table_size = calculate_rank_table_size_bytes(rank_count);
    if (table_offset > std::numeric_limits<std::uint64_t>::max() - table_size)
        throw std::invalid_argument("Rank table file extent overflows");
}

static FieldId decode_field_id(std::uint32_t code) {
    if (code < std::to_underlying(FieldId::Type) || code > std::to_underlying(FieldId::StartIndex))
        throw std::invalid_argument("Unknown standard field ID in header");
    return static_cast<FieldId>(code);
}

template<std::unsigned_integral Integer>
static void append_integer(std::vector<std::byte>& bytes, Integer value) {
    std::array<std::byte, sizeof(Integer)> encoded;
    write_little_endian(value, encoded);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
}

template<std::unsigned_integral... Integers>
static void append_integers(std::vector<std::byte>& bytes, Integers... values) {
    (append_integer(bytes, values), ...);
}

class HeaderReader {
public:
    explicit HeaderReader(std::span<const std::byte> bytes) : remaining_(bytes) {}

    std::span<const std::byte> take(std::size_t size) {
        return take_next_bytes(remaining_, size);
    }

    template<std::unsigned_integral Integer>
    Integer read_integer() { return read_little_endian<Integer>(take(sizeof(Integer))); }

    template<std::unsigned_integral... Integers>
    auto read_integers() {
        // Braced initialization reads in order, advancing the cursor for each integer.
        return std::tuple{read_integer<Integers>()...};
    }

    std::size_t remaining_size() const { return remaining_.size(); }

private:
    std::span<const std::byte> remaining_;
};

static void write_field(std::vector<std::byte>& bytes, const FieldStorageInfo& field) {
    const auto* name = std::get_if<std::string>(&field.field);
    if (name && !std::in_range<std::uint32_t>(name->size()))
        throw std::invalid_argument("Custom field name is too long for header");
    const auto identity_kind = name ? FieldIdentityKind::Custom : FieldIdentityKind::Standard;
    const auto bit_width = static_cast<std::uint8_t>(field.bit_width);
    const auto wildcard_flag = static_cast<std::uint8_t>(field.offset_encoding.has_wildcard);
    const auto base_bits = integer_to_bits(field.offset_encoding.value_base);
    const auto identity = name ? static_cast<std::uint32_t>(name->size())
                               : std::to_underlying(std::get<FieldId>(field.field));
    append_integers(bytes,
        std::to_underlying(identity_kind),
        std::to_underlying(field.type),
        bit_width,
        wildcard_flag,
        field.bit_offset,
        base_bits,
        field.offset_encoding.maximum_offset,
        identity);
    if (name) {
        const auto characters = std::as_bytes(std::span(name->data(), name->size()));
        bytes.insert(bytes.end(), characters.begin(), characters.end());
    }
}

static FieldStorageInfo read_field(HeaderReader& reader) {
    const auto [identity_kind, type_code, width, wildcard, offset, base_bits, maximum_offset, identity] =
        reader.read_integers<
            std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t,
            std::uint64_t, std::uint64_t, std::uint64_t, std::uint32_t>();
    const auto type = static_cast<FieldType>(type_code);
    if (wildcard > 1) throw std::invalid_argument("Invalid wildcard flag in header");
    FieldIdentity field;
    switch (static_cast<FieldIdentityKind>(identity_kind)) {
    case FieldIdentityKind::Standard: field = decode_field_id(identity); break;
    case FieldIdentityKind::Custom: {
        const auto name = reader.take(identity);
        field = std::string(reinterpret_cast<const char*>(name.data()), name.size());
        break;
    }
    default: throw std::invalid_argument("Invalid field identity kind in header");
    }
    const auto base = integer_from_bits(type, base_bits);
    return {std::move(field), type, width, offset, {base, maximum_offset, wildcard != 0}};
}

std::vector<std::byte> write_binary_header(
    const RecordLayout& layout, std::uint32_t rank_count, std::uint32_t node_index_bits) {
    layout.validate_layout();
    if (!std::in_range<std::uint32_t>(layout.fields.size()))
        throw std::invalid_argument("Too many fields for binary header");
    validate_node_index_bits(node_index_bits);
    std::vector<std::byte> bytes(format_magic.begin(), format_magic.end());
    const std::uint64_t header_size_placeholder = 0;
    append_integers(bytes,
        format_version,
        header_size_placeholder,
        layout.record_size_bits,
        static_cast<std::uint32_t>(layout.fields.size()),
        rank_count,
        static_cast<std::uint8_t>(node_index_bits),
        std::uint64_t{0}); // Rank-table offset is filled after writing the descriptors.
    for (const auto& field : layout.fields) write_field(bytes, field);
    // The final length is known after writing the variable-length custom names.
    write_little_endian(static_cast<std::uint64_t>(bytes.size()),
                       std::span(bytes).subspan(header_length_offset, sizeof(std::uint64_t)));
    write_little_endian(static_cast<std::uint64_t>(bytes.size()),
                       std::span(bytes).subspan(rank_table_offset_position, sizeof(std::uint64_t)));
    return bytes;
}

BinaryHeader read_binary_header(std::span<const std::byte> bytes) {
    HeaderReader prefix(bytes);
    if (!std::ranges::equal(prefix.take(format_magic.size()), format_magic))
        throw std::invalid_argument("Unrecognized binary header magic");
    const auto [version, header_size, record_size_bits, field_count] =
        prefix.read_integers<std::uint16_t, std::uint64_t, std::uint64_t, std::uint32_t>();
    if (version != format_version)
        throw std::invalid_argument("Unsupported binary header version");
    const auto [rank_count, node_index_bits, rank_table_offset] =
        prefix.read_integers<std::uint32_t, std::uint8_t, std::uint64_t>();
    if (header_size < prefix_size || header_size > bytes.size())
        throw std::invalid_argument("Invalid binary header length");
    validate_node_index_bits(node_index_bits);
    validate_rank_table_location(header_size, rank_count, rank_table_offset);
    HeaderReader reader(bytes.subspan(prefix_size, static_cast<std::size_t>(header_size) - prefix_size));
    if (field_count > reader.remaining_size() / field_descriptor_size)
        throw std::invalid_argument("Field count exceeds binary header length");
    RecordLayout layout{{}, record_size_bits};
    layout.fields.reserve(field_count);
    for (std::uint32_t index = 0; index < field_count; ++index) layout.fields.push_back(read_field(reader));
    if (reader.remaining_size() != 0)
        throw std::invalid_argument("Unexpected data inside binary header");
    layout.validate_layout();
    return {std::move(layout), header_size, rank_count, node_index_bits, rank_table_offset};
}

template<std::unsigned_integral Integer>
static void write_next_integer(std::span<std::byte>& remaining, Integer value) {
    write_little_endian(value, take_next_bytes(remaining, sizeof(value)));
}

template<std::unsigned_integral Integer>
static Integer read_next_integer(std::span<const std::byte>& remaining) {
    return read_little_endian<Integer>(take_next_bytes(remaining, sizeof(Integer)));
}

std::array<std::byte, rank_table_entry_size_bytes> write_rank_table_entry(const RankTableEntry& entry) {
    std::array<std::byte, rank_table_entry_size_bytes> bytes;
    std::span<std::byte> remaining = bytes;
    write_next_integer(remaining, entry.data_offset_bytes);
    write_next_integer(remaining, entry.node_count);
    write_next_integer(remaining, entry.dependency_count);
    write_next_integer(remaining, entry.root_count);
    return bytes;
}

RankTableEntry read_rank_table_entry(std::span<const std::byte> entry_bytes) {
    if (entry_bytes.size() != rank_table_entry_size_bytes)
        throw std::invalid_argument("Byte buffer size does not match rank table entry size");
    return {
        read_next_integer<decltype(RankTableEntry::data_offset_bytes)>(entry_bytes),
        read_next_integer<decltype(RankTableEntry::node_count)>(entry_bytes),
        read_next_integer<decltype(RankTableEntry::dependency_count)>(entry_bytes),
        read_next_integer<decltype(RankTableEntry::root_count)>(entry_bytes)};
}

} // namespace traceschema
