// Differential test: DecodePlan and the JIT decoder must accept exactly the records the
// generic NodeDecoder accepts and produce the same values; counter decrements must match.
#include "binary_reading/DecodePlan.hpp"
#include "binary_reading/JitDecoder.hpp"
#include "binary_reading/NodeDecoder.hpp"
#include "binary_reading/OrcDecoder.hpp"
#include "trace_format/BinaryIO.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace traceschema;

static void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

static std::mt19937_64 rng(20261027);

// Uniform in [0, maximum].
static std::uint64_t pick(std::uint64_t maximum) {
    return maximum == std::numeric_limits<std::uint64_t>::max() ? rng() : rng() % (maximum + 1);
}

static std::uint64_t mask_of(std::uint32_t width) {
    return width == 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << width) - 1;
}

static FieldStorageInfo random_field(FieldIdentity identity, std::uint64_t& offset) {
    const auto width = static_cast<std::uint32_t>(pick(64));
    const bool wildcard = width != 0 && width < 64 && pick(2) == 0;
    const auto limit = mask_of(width) - (wildcard ? 1 : 0);
    const auto maximum = pick(3) == 0 ? limit : pick(limit);
    offset += pick(8);
    const auto bit_offset = offset;
    offset += width;
    const auto base = pick(std::numeric_limits<std::uint64_t>::max() - maximum);
    if (pick(1) == 0)
        return {std::move(identity), FieldType::UnsignedInteger, width, bit_offset,
                OffsetEncoding{FieldValue{base}, maximum, wildcard}};
    const auto signed_base = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min()) + base);
    return {std::move(identity), FieldType::SignedInteger, width, bit_offset,
            OffsetEncoding{FieldValue{signed_base}, maximum, wildcard}};
}

static RecordLayout random_layout() {
    RecordLayout layout;
    std::uint64_t offset = pick(8);
    if (pick(1) == 0) {
        layout.fields.push_back({FieldId::Type, FieldType::UnsignedInteger, 2, offset,
                                 OffsetEncoding{FieldValue{std::uint64_t{1}}, 2}});
        offset += 2;
        const auto width = static_cast<std::uint32_t>(2 + pick(19));
        layout.fields.push_back({FieldId::Peer, FieldType::UnsignedInteger, width, offset,
                                 OffsetEncoding{FieldValue{std::uint64_t{0}}, mask_of(width) - 1, true}});
        offset += width;
    }
    for (auto count = 1 + pick(11); count > 0; --count)
        layout.fields.push_back(random_field("field_" + std::to_string(layout.fields.size()), offset));
    layout.record_size_bits = (offset + pick(16) + 7) / 8 * 8;
    layout.validate_layout();
    return layout;
}

static std::uint64_t random_code(const FieldStorageInfo& field) {
    const auto& encoding = field.offset_encoding;
    switch (pick(3)) {
    case 0: return encoding.has_wildcard ? encoding.maximum_offset + 1 : encoding.maximum_offset;
    case 1: return rng() & mask_of(field.bit_width);
    default: return pick(encoding.maximum_offset);
    }
}

int main() try {
    const auto cache = std::filesystem::temp_directory_path() / ("decode_plan_test_" + std::to_string(rng()));
    setenv("LOGGOPSIM_JIT_CACHE", cache.c_str(), 1);
    std::size_t accepted = 0, rejected = 0, compiled = 0, orc_compiled = 0;
    for (int schema = 0; schema < 300; ++schema) {
        const auto layout = random_layout();
        FieldRequirements requirements;
        for (const auto& field : layout.fields)
            requirements.push_back(field.type == FieldType::SignedInteger
                ? require_field<std::int64_t>(field.field) : require_field<std::uint64_t>(field.field));
        const NodeDecoder decoder(layout, requirements);
        const auto fields = decoder.get_selected_fields();
        std::vector<FieldDestination> destinations;
        for (std::size_t index = 0; index < fields.size(); ++index)
            destinations.push_back({0, static_cast<std::uint32_t>(index * 8), 8, rng()});
        const auto plan = DecodePlan::create(fields, destinations);
        check(plan.has_value(), "plan rejected a valid layout");
        const auto jit = schema < 12 ? load_jit_decoder(*plan) : nullptr;
        check(schema >= 12 || jit, "JIT compilation failed");
        compiled += jit != nullptr;
        const auto orc = load_orc_decoder(*plan);
#ifdef LOGGOPSIM_HAVE_LLVM
        check(orc != nullptr, "ORC compilation failed");
#endif
        orc_compiled += orc != nullptr;

        const auto size = layout.get_record_size_bytes();
        std::vector<std::byte> record(size + 16);
        for (int sample = 0; sample < 2000; ++sample) {
            for (auto& byte : record) byte = std::byte(rng());
            for (const auto& field : layout.fields)
                bit_packing::write_bits(record, field.bit_offset, field.bit_width, random_code(field));

            std::vector<std::uint64_t> expected(fields.size());
            bool generic_ok = true;
            try {
                std::vector<FieldValue> values(fields.size());
                decoder.decode_fields(std::span(record).first(size), values);
                for (std::size_t index = 0; index < values.size(); ++index)
                    expected[index] = std::holds_alternative<Wildcard>(values[index])
                        ? destinations[index].wildcard_bits : integer_to_bits(values[index]);
            } catch (const std::invalid_argument&) {
                generic_ok = false;
            }
            (generic_ok ? accepted : rejected)++;

            std::vector<std::uint64_t> output(fields.size());
            std::byte* const targets[] = {reinterpret_cast<std::byte*>(output.data())};
            check(plan->decode(record.data(), targets) == generic_ok, "plan acceptance differs");
            check(!generic_ok || output == expected, "plan values differ");
            if (jit) {
                std::fill(output.begin(), output.end(), 0);
                check((jit(record.data(), targets) != 0) == generic_ok, "JIT acceptance differs");
                check(!generic_ok || output == expected, "JIT values differ");
            }
            if (orc) {
                std::fill(output.begin(), output.end(), 0);
                check((orc(record.data(), targets) != 0) == generic_ok, "ORC acceptance differs");
                check(!generic_ok || output == expected, "ORC values differ");
            }
        }

        const auto width = static_cast<std::uint32_t>(pick(64));
        const auto counter_offset = pick((size + 8) * 8 - 1);
        FieldStorageInfo counter{"counter", FieldType::UnsignedInteger, width, counter_offset,
                                 OffsetEncoding{FieldValue{std::uint64_t{0}}, pick(mask_of(width))}};
        const auto step = DecodePlan::make_step(counter);
        check(step.has_value(), "counter step rejected");
        for (int sample = 0; sample < 200; ++sample) {
            for (auto& byte : record) byte = std::byte(rng());
            const auto code = pick(3) == 0 ? 0 : random_code(counter);
            bit_packing::write_bits(record, counter_offset, width, code);
            auto reference = record;
            const bool valid = code != 0 && code <= counter.offset_encoding.maximum_offset;
            if (valid) bit_packing::write_bits(reference, counter_offset, width, code - 1);
            const auto result = DecodePlan::decrement(record.data(), *step);
            if (result) {
                check(valid && *result == code - 1, "decrement value differs");
                check(record == reference, "decrement changed neighbouring bits");
            } else {
                check(!valid || step->spill, "decrement fell back unexpectedly");
            }
        }
    }
    std::filesystem::remove_all(cache);
    check(accepted > 0 && rejected > 0, "test did not cover both accepted and rejected records");
    std::cout << "decode plan: " << accepted << " accepted, " << rejected << " rejected, "
              << compiled << " JIT schemas, " << orc_compiled << " ORC schemas\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
