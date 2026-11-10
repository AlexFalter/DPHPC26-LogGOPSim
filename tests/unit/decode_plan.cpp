// Differential tests: every plan runner must accept exactly the records the exact decoder
// accepts and write the same bytes, on random layouts and records.
#include "binary_reading/decoding/Decoder.hpp"
#include "binary_reading/decoding/runners/RunnerRegistry.hpp"
#include "binary_reading/decoding/runners/interpreted/InterpretedRunner.hpp"
#include "binary_reading/decoding/runners/templated_schema/TemplatedSchemaRunner.hpp"
#include "binary_reading/decoding/runners/templated_node/TemplatedNodeRunner.hpp"
#include "trace_format/BinaryIO.hpp"

#include "TestHelpers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace traceschema;

namespace {

constexpr std::size_t records_per_layout = 2000;

// Random layouts and records. Each test seeds its own generator, so tests are independent.
class RandomTraces {
public:
    explicit RandomTraces(std::uint64_t seed) : random_(seed) {}

    // Uniform in [0, maximum].
    std::uint64_t pick(std::uint64_t maximum) {
        return maximum == std::numeric_limits<std::uint64_t>::max() ? random_() : random_() % (maximum + 1);
    }

    // Widths 0..64, gaps, signed and unsigned bases, wildcards and fields spilling into a ninth
    // byte. Type and Peer, which enable the send-to-ANY check, come first when present.
    RecordLayout layout(std::optional<bool> with_type_and_peer = {}, std::optional<std::size_t> field_count = {}) {
        RecordLayout layout;
        std::uint64_t offset = pick(8);
        if (with_type_and_peer.value_or(pick(1) == 0)) {
            layout.fields.push_back({FieldId::Type, FieldType::UnsignedInteger, 2, offset,
                                     OffsetEncoding{FieldValue{std::uint64_t{1}}, 2}});
            offset += 2;
            const auto width = static_cast<std::uint32_t>(2 + pick(19));
            layout.fields.push_back({FieldId::Peer, FieldType::UnsignedInteger, width, offset,
                                     OffsetEncoding{FieldValue{std::uint64_t{0}}, low_bit_mask(width) - 1, true}});
            offset += width;
        }
        for (auto count = field_count.value_or(1 + pick(11)); count > 0; --count)
            layout.fields.push_back(field("field_" + std::to_string(layout.fields.size()), offset));
        layout.record_size_bits = (offset + pick(16) + 7) / 8 * 8;
        layout.validate_layout();
        return layout;
    }

    // A record of the layout's size plus 16 bytes, so runners can load beyond its end. Codes are
    // valid, the wildcard code or arbitrary bits.
    std::vector<std::byte> record(const RecordLayout& layout) {
        std::vector<std::byte> bytes(layout.get_record_size_bytes() + 16);
        for (auto& byte : bytes) byte = std::byte(random_());
        for (const auto& field : layout.fields)
            bit_packing::write_bits(bytes, field.bit_offset, field.bit_width, code(field));
        return bytes;
    }

    std::uint64_t code(const FieldStorageInfo& field) {
        const auto& encoding = field.offset_encoding;
        switch (pick(3)) {
        case 0: return encoding.has_wildcard ? encoding.maximum_offset + 1 : encoding.maximum_offset;
        case 1: return random_() & low_bit_mask(field.bit_width);
        default: return pick(encoding.maximum_offset);
        }
    }

    std::uint64_t operator()() { return random_(); }

private:
    FieldStorageInfo field(FieldIdentity identity, std::uint64_t& offset) {
        const auto width = static_cast<std::uint32_t>(pick(64));
        const bool wildcard = width != 0 && width < 64 && pick(2) == 0;
        const auto limit = low_bit_mask(width) - (wildcard ? 1 : 0);
        const auto maximum = pick(3) == 0 ? limit : pick(limit);
        offset += pick(8);
        const auto bit_offset = offset;
        offset += width;
        const auto base = pick(std::numeric_limits<std::uint64_t>::max() - maximum);
        if (pick(1) == 0)
            return {std::move(identity), FieldType::UnsignedInteger, width, bit_offset,
                    OffsetEncoding{FieldValue{base}, maximum, wildcard}};
        const auto signed_base = static_cast<std::int64_t>(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::min()) + base);
        return {std::move(identity), FieldType::SignedInteger, width, bit_offset,
                OffsetEncoding{FieldValue{signed_base}, maximum, wildcard}};
    }

    std::mt19937_64 random_;
};

FieldRequirements require_all(const RecordLayout& layout) {
    FieldRequirements requirements;
    for (const auto& field : layout.fields)
        requirements.push_back(field.type == FieldType::SignedInteger ? require_field<std::int64_t>(field.field)
                                                                      : require_field<std::uint64_t>(field.field));
    return requirements;
}

// One 8-byte member per field, with a random value for ANY.
std::vector<FieldDestination> word_destinations(std::size_t count, RandomTraces& random) {
    std::vector<FieldDestination> destinations;
    for (std::size_t index = 0; index < count; ++index)
        destinations.push_back({OutputStruct::Node, static_cast<std::uint32_t>(index * 8), 8, random()});
    return destinations;
}

using Output = std::array<std::uint64_t, 16>;

// The exact decoder's result for a record: nothing if it rejects the record. No byte is
// readable beyond the record, so the decoder cannot hand it to its runner.
std::optional<Output> decode_exactly(const Decoder& decoder, std::span<const std::byte> record) {
    Output output{};
    std::byte* const targets[] = {reinterpret_cast<std::byte*>(output.data())};
    try {
        decoder.decode_into(record, 0, targets);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    return output;
}

// The runner must accept exactly the records the exact decoder accepts and write the same bytes.
void expect_same_as_exact(const PlanRunner& runner, const Decoder& decoder, const RecordLayout& layout,
                          const std::vector<std::byte>& record) {
    const auto expected = decode_exactly(decoder, std::span(record).first(layout.get_record_size_bytes()));
    Output output{};
    std::byte* const targets[] = {reinterpret_cast<std::byte*>(output.data())};
    ASSERT_EQ(runner.run(record.data(), targets), expected.has_value()) << "acceptance differs";
    if (expected) ASSERT_EQ(output, *expected) << "values differ";
}

std::vector<const RunnerInfo*> runners_for_any_plan() {
    std::vector<const RunnerInfo*> runners;
    for (const auto& runner : get_runners())
        if (runner.handles_any_plan) runners.push_back(&runner);
    return runners;
}

// Every registered runner for any plan, compared on many random layouts.
class AnyPlanRunner : public testing::TestWithParam<const RunnerInfo*> {
protected:
    test_helpers::TemporaryDirectory cache_;
    test_helpers::ScopedEnvironment cache_environment_{"LOGGOPSIM_JIT_CACHE", cache_.path().string()};

    // The C compiler starts a process per layout, so it gets fewer of them.
    int layout_count() const { return GetParam()->name == "jit" ? 12 : 300; }
};

} // namespace

TEST_P(AnyPlanRunner, MatchesExactDecoderOnRandomLayouts) {
    const auto& info = *GetParam();
    if (!info.is_available()) GTEST_SKIP() << info.name << " is unavailable in this build";
    RandomTraces random(20261027);
    for (int index = 0; index < layout_count(); ++index) {
        SCOPED_TRACE("layout " + std::to_string(index));
        const auto layout = random.layout();
        const auto destinations = word_destinations(layout.fields.size(), random);
        const Decoder decoder(layout, require_all(layout), destinations);
        const auto runner = info.create(DecodePlan::create(decoder.get_selected_fields(), destinations));
        ASSERT_NE(runner, nullptr) << "compilation failed";
        EXPECT_EQ(runner->get_name(), info.name);
        for (std::size_t sample = 0; sample < records_per_layout; ++sample)
            ASSERT_NO_FATAL_FAILURE(expect_same_as_exact(*runner, decoder, layout, random.record(layout)))
                << "record " << sample;
    }
}

INSTANTIATE_TEST_SUITE_P(DecodePlan, AnyPlanRunner, testing::ValuesIn(runners_for_any_plan()),
                         [](const testing::TestParamInfo<const RunnerInfo*>& info) { return std::string(info.param->name); });

TEST(Decoder, SeparatesBoundedReadsFromRunnerRejections) {
    test_helpers::ScopedEnvironment engine("LOGGOPSIM_DECODER", "plan");
    const auto layout = create_packed_schema({
        {std::string{"value"}, FieldType::UnsignedInteger, 2, 0, OffsetEncoding{std::uint64_t{0}, 2}}});
    const std::array<FieldDestination, 1> destinations{{{OutputStruct::Node, 0, 8, 0}}};
    const Decoder decoder(layout, {require_field<std::uint64_t>(std::string{"value"})}, destinations);
    std::array<std::byte, 8> storage{};
    const auto record = std::span<const std::byte>(storage).first(1);
    std::uint64_t value = 0;
    std::byte* targets[]{reinterpret_cast<std::byte*>(&value)};
    storage[0] = std::byte{2};
    decoder.decode_into(record, storage.size(), targets);
    EXPECT_EQ(value, 2u);
    value = 0;
    decoder.decode_into(record, record.size(), targets);
    EXPECT_EQ(value, 2u);

    storage[0] = std::byte{3}; // Unused code: both paths must reject it.
    EXPECT_THAT([&] { decoder.decode_into(record, storage.size(), targets); },
                testing::ThrowsMessage<std::invalid_argument>(testing::Eq("Invalid node record")));
    EXPECT_THROW(decoder.decode_into(record, record.size(), targets), std::invalid_argument);
}

// Compare production decoding against a separate bit-by-bit test reference.
TEST(Decoder, MatchesIndependentReferenceValues) {
    RandomTraces random(20261105);
    for (int index = 0; index < 100; ++index) {
        SCOPED_TRACE("layout " + std::to_string(index));
        const auto layout = random.layout();
        const auto destinations = word_destinations(layout.fields.size(), random);
        const Decoder decoder(layout, require_all(layout), destinations);
        for (std::size_t sample = 0; sample < 200; ++sample) {
            const auto record = random.record(layout);
            const auto exact = std::span(record).first(layout.get_record_size_bytes());
            std::vector<FieldValue> values(layout.fields.size());
            bool valid = true;
            try {
                values = test_helpers::decode_record(layout, exact);
            } catch (const std::invalid_argument&) {
                valid = false;
            }
            const auto decoded = decode_exactly(decoder, exact);
            ASSERT_EQ(decoded.has_value(), valid);
            if (!valid) continue;
            for (std::size_t field = 0; field < values.size(); ++field)
                ASSERT_EQ((*decoded)[field], std::holds_alternative<Wildcard>(values[field])
                                                 ? destinations[field].wildcard_value : integer_to_bits(values[field]))
                    << "field " << field;
        }
    }
}

namespace {

constexpr StepShape word{OutputStruct::Node, 8};
using EightWordRunner = TemplatedNodeRunner<word, word, word, word, word, word, word, word>;

} // namespace

// Type and Peer plus six fields, each into an 8-byte member: positions and ranges vary.
TEST(TemplatedNodeRunner, MatchesExactDecoderOnRandomLayoutsOfItsShape) {
    RandomTraces random(20261106);
    for (int index = 0; index < 300; ++index) {
        SCOPED_TRACE("layout " + std::to_string(index));
        const auto layout = random.layout(true, 6);
        const auto destinations = word_destinations(layout.fields.size(), random);
        const Decoder decoder(layout, require_all(layout), destinations);
        const auto plan = DecodePlan::create(decoder.get_selected_fields(), destinations);
        ASSERT_TRUE(EightWordRunner::matches(plan));
        const EightWordRunner runner(plan);
        for (std::size_t sample = 0; sample < records_per_layout; ++sample)
            ASSERT_NO_FATAL_FAILURE(expect_same_as_exact(runner, decoder, layout, random.record(layout)))
                << "record " << sample;
    }
}

TEST(TemplatedNodeRunner, RejectsPlansOfOtherShapes) {
    RandomTraces random(20261107);
    const auto layout = random.layout(true, 5);
    auto destinations = word_destinations(layout.fields.size(), random);
    const Decoder decoder(layout, require_all(layout), destinations);
    EXPECT_FALSE(EightWordRunner::matches(DecodePlan::create(decoder.get_selected_fields(), destinations)))
        << "one step too few";
}

namespace {

// Type, a wildcard Peer, a field spilling into a ninth byte, a signed field and a constant, into
// members of 1, 4, 8, 2 and 4 bytes.
const RecordLayout fixed_layout{{
    {FieldId::Type, FieldType::UnsignedInteger, 2, 0, OffsetEncoding{FieldValue{std::uint64_t{1}}, 2}},
    {FieldId::Peer, FieldType::UnsignedInteger, 5, 2, OffsetEncoding{FieldValue{std::uint64_t{0}}, 30, true}},
    {std::string("spill"), FieldType::UnsignedInteger, 60, 7,
     OffsetEncoding{FieldValue{std::uint64_t{1000}}, (std::uint64_t{1} << 60) - 5}},
    {std::string("delta"), FieldType::SignedInteger, 7, 67, OffsetEncoding{FieldValue{std::int64_t{-50}}, 100}},
    {std::string("constant"), FieldType::UnsignedInteger, 0, 0, OffsetEncoding{FieldValue{std::uint64_t{42}}, 0}}}, 80};
const std::vector<FieldDestination> fixed_destinations{
    {OutputStruct::Node, 0, 1, 0xffffffff}, {OutputStruct::Node, 8, 4, 0xffffffff}, {OutputStruct::Node, 16, 8, 0xffffffff},
    {OutputStruct::Node, 24, 2, 0xffffffff}, {OutputStruct::Node, 32, 4, 0xffffffff}};

// The specialization for the fixed test layout.
using FixedTemplatedSchemaRunner = TemplatedSchemaRunner<0, 1,
    DecodePlan::Step{FieldSource{0, 0, false, false, 3u, 1u, 2u}, FieldDestination{OutputStruct::Node, 0, 1, 4294967295u}},
    DecodePlan::Step{FieldSource{0, 2, false, true, 31u, 0u, 30u}, FieldDestination{OutputStruct::Node, 8, 4, 4294967295u}},
    DecodePlan::Step{FieldSource{0, 7, true, false, 1152921504606846975u, 1000u, 1152921504606846971u},
                     FieldDestination{OutputStruct::Node, 16, 8, 4294967295u}},
    DecodePlan::Step{FieldSource{8, 3, false, false, 127u, 18446744073709551566u, 100u},
                     FieldDestination{OutputStruct::Node, 24, 2, 4294967295u}},
    DecodePlan::Step{FieldSource{0, 0, false, false, 0u, 42u, 0u}, FieldDestination{OutputStruct::Node, 32, 4, 4294967295u}}>;

} // namespace

namespace {

// Compare both output structs including all untouched bytes, not only decoded members.
void expect_mixed_stores(const PlanRunner& runner, const Decoder& decoder, const RecordLayout& layout,
                         const std::vector<std::byte>& record) {
    using Buffer = std::array<std::byte, 128>;
    std::array<Buffer, 2> expected, actual;
    for (auto& buffer : expected) buffer.fill(std::byte{0xa5});
    actual = expected;
    std::byte* expected_targets[] = {expected[0].data(), expected[1].data()};
    bool valid = true;
    try { decoder.decode_into(std::span(record).first(layout.get_record_size_bytes()), 0, expected_targets); }
    catch (const std::invalid_argument&) { valid = false; }
    std::byte* actual_targets[] = {actual[0].data(), actual[1].data()};
    ASSERT_EQ(runner.run(record.data(), actual_targets), valid);
    if (valid) EXPECT_EQ(actual, expected) << "wrong destination or neighbouring byte overwritten";
}

constexpr StepShape mixed_node1{OutputStruct::Node, 1}, mixed_node4{OutputStruct::Node, 4};
constexpr StepShape mixed_node2{OutputStruct::Node, 2};
constexpr StepShape mixed_list8{OutputStruct::DependencyLists, 8}, mixed_list4{OutputStruct::DependencyLists, 4};
using MixedTemplatedNodeRunner = TemplatedNodeRunner<mixed_node1, mixed_node4, mixed_list8, mixed_node2, mixed_list4>;

std::vector<FieldDestination> mixed_destinations(RandomTraces& random) {
    auto destinations = fixed_destinations;
    destinations[2].target = destinations[4].target = OutputStruct::DependencyLists;
    for (std::size_t index = 0; index < destinations.size(); ++index)
        destinations[index].offset = static_cast<std::uint32_t>(index * 16 + 1 + random.pick(5));
    return destinations;
}

} // namespace

TEST_P(AnyPlanRunner, MixedDestinationSizesPreserveNeighbouringBytes) {
    const auto& info = *GetParam();
    if (!info.is_available()) GTEST_SKIP() << info.name << " is unavailable in this build";
    RandomTraces random(20261110);
    for (int index = 0; index < 12; ++index) {
        SCOPED_TRACE("destinations " + std::to_string(index));
        const auto destinations = mixed_destinations(random);
        const Decoder decoder(fixed_layout, require_all(fixed_layout), destinations);
        const auto runner = info.create(DecodePlan::create(decoder.get_selected_fields(), destinations));
        ASSERT_NE(runner, nullptr);
        for (int sample = 0; sample < 2000; ++sample)
            ASSERT_NO_FATAL_FAILURE(expect_mixed_stores(*runner, decoder, fixed_layout, random.record(fixed_layout)));
    }
}

TEST(TemplatedNodeRunner, MixedDestinationSizesPreserveNeighbouringBytes) {
    RandomTraces random(20261111);
    for (int index = 0; index < 100; ++index) {
        const auto destinations = mixed_destinations(random);
        const Decoder decoder(fixed_layout, require_all(fixed_layout), destinations);
        const auto plan = DecodePlan::create(decoder.get_selected_fields(), destinations);
        ASSERT_TRUE(MixedTemplatedNodeRunner::matches(plan));
        const MixedTemplatedNodeRunner runner(plan);
        for (int sample = 0; sample < 200; ++sample)
            ASSERT_NO_FATAL_FAILURE(expect_mixed_stores(runner, decoder, fixed_layout, random.record(fixed_layout)));
    }
}

TEST(TemplatedSchemaRunner, MatchesItsLayoutOnly) {
    const Decoder decoder(fixed_layout, require_all(fixed_layout), fixed_destinations);
    const auto plan = DecodePlan::create(decoder.get_selected_fields(), fixed_destinations);
    EXPECT_TRUE(FixedTemplatedSchemaRunner::matches(plan));
    auto shifted = fixed_layout;
    shifted.fields[3].bit_offset += 1;
    const Decoder shifted_decoder(shifted, require_all(shifted), fixed_destinations);
    EXPECT_FALSE(FixedTemplatedSchemaRunner::matches(DecodePlan::create(shifted_decoder.get_selected_fields(), fixed_destinations)));
}

TEST(TemplatedSchemaRunner, MatchesExactDecoderOnRandomRecords) {
    RandomTraces random(20261108);
    const Decoder decoder(fixed_layout, require_all(fixed_layout), fixed_destinations);
    const FixedTemplatedSchemaRunner runner(DecodePlan::create(decoder.get_selected_fields(), fixed_destinations));
    for (std::size_t sample = 0; sample < 20 * records_per_layout; ++sample)
        ASSERT_NO_FATAL_FAILURE(expect_mixed_stores(runner, decoder, fixed_layout, random.record(fixed_layout)))
            << "record " << sample;
}

// try_decrement_bits must change only the field, and only for a code in 1..maximum, also when
// too few bytes are readable for the one-word update.
TEST(TryDecrementBits, MatchesBitwiseReference) {
    RandomTraces random(20261109);
    std::vector<std::byte> record(40);
    for (int layout = 0; layout < 300; ++layout) {
        const auto width = static_cast<std::uint32_t>(random.pick(64));
        const auto bit_offset = random.pick(record.size() * 8 - 1 - width);
        const FieldStorageInfo counter{"counter", FieldType::UnsignedInteger, width, bit_offset,
                                       OffsetEncoding{FieldValue{std::uint64_t{0}}, random.pick(low_bit_mask(width))}};
        const auto maximum = counter.offset_encoding.maximum_offset;
        for (int sample = 0; sample < 200; ++sample) {
            for (auto& byte : record) byte = std::byte(random());
            const auto code = random.pick(3) == 0 ? 0 : random.code(counter);
            bit_packing::write_bits(record, bit_offset, width, code);
            const auto original = record;
            const bool valid = code != 0 && code <= maximum;
            auto expected = record;
            if (valid) bit_packing::write_bits(expected, bit_offset, width, code - 1);
            const auto readable = (bit_offset + width + 7) / 8 + random.pick(record.size() - (bit_offset + width + 7) / 8);
            const auto result = bit_packing::try_decrement_bits(std::span(record).first(readable), bit_offset, width, maximum);
            SCOPED_TRACE("width " + std::to_string(width) + " at bit " + std::to_string(bit_offset) + ", code " +
                         std::to_string(code) + ", " + std::to_string(readable) + " bytes readable");
            ASSERT_EQ(result.has_value(), valid);
            if (valid) ASSERT_EQ(*result, code - 1);
            ASSERT_EQ(record, valid ? expected : original) << "other bits changed";
        }
    }
}
