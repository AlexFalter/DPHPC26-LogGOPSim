#include "TestHelpers.hpp"
#include "binary_writing/LegacyTraceWriter.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "goal_parsing/Goal.hpp"
#include "simulators/loggopsim/SimulationSchedule.hpp"
#include "trace_format/BinaryFormat.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace traceschema;
using test_helpers::decode_record;
using test_helpers::TemporaryFile;

namespace {

// A stream buffer whose writes, seeks or flushes can be made to fail.
class FailingBuffer : public std::stringbuf {
public:
    bool fail_write = false;
    bool fail_seek = false;
    bool fail_flush = false;

protected:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        return fail_write ? 0 : std::stringbuf::xsputn(bytes, count);
    }
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekoff(offset, direction, mode);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekpos(position, mode);
    }
    int sync() override { return fail_flush ? -1 : std::stringbuf::sync(); }
};

// Three ranks: calc, send and wildcard receive with both dependency kinds; a receive followed
// by a calc; and an empty rank. One signed custom field.
class TraceWriterTest : public testing::Test {
protected:
    TraceWriterTest() {
        std::vector<FieldValueInfo> inputs;
        for (const auto& definition : standard_fields) {
            auto field = FieldValueInfo::create_standard(definition.id, std::nullopt, UnsignedRange{0, 1024});
            if (definition.id == FieldId::Type) field.value_range = UnsignedRange{1, 3};
            if (definition.id == FieldId::Peer) {
                field.value_range = UnsignedRange{0, 2};
                field.has_wildcard = true;
            }
            if (definition.id == FieldId::Tag) {
                field.value_range = UnsignedRange{42, 42};
                field.has_wildcard = true;
            }
            inputs.push_back(field);
        }
        inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
        layout = build_record_layout(inputs, 3);

        first.Dependency(send, calc);
        first.StartDependency(receive, calc);
        first.Dependency(receive, send);
        calc->custom_values = {std::int64_t{-2}};
        send->custom_values = {std::int64_t{4}};
        receive->custom_values = {std::int64_t{0}};

        auto* second_receive = second.Recv(0, 1024, 42, 0, 0);
        auto* compute = second.Calc(500, 0, 0);
        second.Dependency(compute, second_receive);
        second_receive->custom_values = {std::int64_t{1}};
        compute->custom_values = {std::int64_t{2}};
    }

    void write_ranks(std::ostream& output) {
        TraceWriter writer(output, layout, 3, 2, custom_fields);
        writer.write_rank(first);
        writer.write_rank(second);
        writer.write_rank(empty);
        writer.finish();
    }

    std::string write_ranks() {
        std::ostringstream output(std::ios::out | std::ios::binary);
        write_ranks(output);
        return output.str();
    }

    const std::vector<FieldValueInfo> custom_fields{
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger, std::nullopt, SignedRange{-2, 4})};
    RecordLayout layout;
    Goal first;
    Node* calc = first.Calc(1000, 0, 0);
    Node* send = first.Send(1, 1024, 42, 0, 0);
    Node* receive = first.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    Goal second;
    Goal empty;
};

RankTableEntry read_table_entry(std::span<const std::byte> file, const BinaryHeader& header, std::size_t rank) {
    return read_rank_table_entry(file.subspan(header.rank_table_offset_bytes + rank * rank_table_entry_size_bytes,
                                              rank_table_entry_size_bytes));
}

} // namespace

TEST_F(TraceWriterTest, EnforcesRankCountAndFinishOrder) {
    std::ostringstream output(std::ios::out | std::ios::binary);
    TraceWriter writer(output, layout, 3, 2, custom_fields);
    writer.write_rank(first);
    EXPECT_THROW(writer.finish(), std::logic_error) << "finished before every rank was written";
    writer.write_rank(second);
    writer.write_rank(empty);
    EXPECT_THROW(writer.write_rank(empty), std::logic_error) << "more ranks than declared";
    writer.finish();
    EXPECT_NO_THROW(writer.finish());
    EXPECT_THROW(writer.write_rank(empty), std::logic_error) << "rank written after finish";
}

TEST_F(TraceWriterTest, WritesHeaderRankTableAndRecords) {
    const auto file = write_ranks();
    const auto bytes = std::as_bytes(std::span(file));
    const auto header = read_binary_header(bytes);
    EXPECT_EQ(header.rank_count, 3u);
    EXPECT_EQ(header.node_index_bits, 2u);
    EXPECT_EQ(header.rank_table_offset_bytes, header.header_size_bytes);

    const auto record_size = header.layout.get_record_size_bytes();
    const auto rank0 = read_table_entry(bytes, header, 0);
    const auto rank1 = read_table_entry(bytes, header, 1);
    const auto rank2 = read_table_entry(bytes, header, 2);
    const auto data_begin = header.header_size_bytes + 3 * rank_table_entry_size_bytes;
    EXPECT_EQ(rank0, (RankTableEntry{data_begin, 3, 3, 1}));
    EXPECT_EQ(rank1, (RankTableEntry{data_begin + 3 * record_size + 2, 2, 1, 1}));
    EXPECT_EQ(rank2, (RankTableEntry{rank1.data_offset_bytes + 2 * record_size + 2, 0, 0, 0}));
    EXPECT_EQ(rank2.data_offset_bytes, file.size());

    const auto read_record = [&](const RankTableEntry& rank, std::size_t node) {
        return decode_record(header.layout, bytes.subspan(rank.data_offset_bytes + node * record_size, record_size));
    };
    const RecordValues expected_calc{std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{0}, std::uint64_t{1000},
                                     std::uint64_t{42}, std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{1},
                                     std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1}, std::int64_t{-2}};
    EXPECT_EQ(read_record(rank0, 0), expected_calc);
    const auto wildcard_receive = read_record(rank0, 2);
    EXPECT_TRUE(std::holds_alternative<Wildcard>(wildcard_receive[2]));
    EXPECT_TRUE(std::holds_alternative<Wildcard>(wildcard_receive[4]));
    EXPECT_EQ(read_record(rank1, 0).back(), FieldValue{std::int64_t{1}});
    EXPECT_EQ(read_record(rank1, 1).back(), FieldValue{std::int64_t{2}});

    // Dependency appendices and root indices.
    EXPECT_EQ(bytes[rank0.data_offset_bytes + 3 * record_size], std::byte{0x29});
    EXPECT_EQ(bytes[rank1.data_offset_bytes - 1], std::byte{0});
    EXPECT_EQ(bytes[rank1.data_offset_bytes + 2 * record_size], std::byte{1});
    EXPECT_EQ(bytes[rank2.data_offset_bytes - 1], std::byte{0});
}

TEST_F(TraceWriterTest, FileOutputMatchesMemoryOutput) {
    const TemporaryFile file({}, false);
    {
        std::ofstream output(file.path(), std::ios::binary | std::ios::trunc);
        write_ranks(output);
    }
    std::ifstream input(file.path(), std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(written, write_ranks());
}

// Interleaving two writers catches shared file-size or resource state.
TEST_F(TraceWriterTest, LegacyWritersAreIndependent) {
    const TemporaryFile legacy_file({}, false);
    const TemporaryFile other_file({}, false);
    first.SetRank(0);
    second.SetRank(1);
    empty.SetRank(2);
    for (auto* graph : {&first, &second, &empty}) graph->SetNumRanks(3);
    calc->Proc = 7;
    send->Nic = 5;
    Goal independent;
    independent.SetNumRanks(1);
    independent.Calc(17, 1, 0);
    {
        LegacyTraceWriter legacy(legacy_file.path().c_str(), 3);
        LegacyTraceWriter other(other_file.path().c_str(), 1);
        legacy.write_rank(first);
        other.write_rank(independent);
        other.finish();
        legacy.write_rank(second);
        legacy.write_rank(empty);
        legacy.finish();
    }
    SimulationSchedule legacy(legacy_file.path().c_str(), false);
    SimulationSchedule other(other_file.path().c_str(), false);
    ASSERT_EQ(legacy.schedules.size(), 3u);
    EXPECT_EQ(legacy.GetNumCPU(), 8u);
    EXPECT_EQ(legacy.GetNumNIC(), 6u);
    EXPECT_EQ(legacy.schedules[2].GetNumNodes(), 0u);
    EXPECT_EQ(other.schedules.size(), 1u);
    EXPECT_EQ(other.GetNumCPU(), 2u);
    EXPECT_EQ(other.GetNumNIC(), 1u);

    // Walk rank 0: the calc, then the send it releases on finishing, then the wildcard receive.
    auto& rank = legacy.schedules[0];
    std::vector<LogGOPSimNode> ready;
    rank.GetExecutableNodes_DSN(ready);
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0].Size, 1000u);
    EXPECT_EQ(ready[0].Proc, 7u);
    rank.MarkNodeAsStarted(ready[0].node_index);
    std::vector<LogGOPSimNode> next;
    rank.GetExecutableNodes_DSN(next);
    EXPECT_TRUE(next.empty()) << "the start dependency released the receive too early";
    rank.MarkNodeAsDone(ready[0].node_index);
    rank.GetExecutableNodes_DSN(next);
    ASSERT_EQ(next.size(), 1u);
    EXPECT_EQ(next[0].node_index, 1u);
    EXPECT_EQ(next[0].Nic, 5u);
    rank.MarkNodeAsDone(next[0].node_index);
    std::vector<LogGOPSimNode> wildcard;
    rank.GetExecutableNodes_DSN(wildcard);
    ASSERT_EQ(wildcard.size(), 1u);
    EXPECT_EQ(wildcard[0].node_index, 2u);
    EXPECT_EQ(wildcard[0].Peer, Node::any_source);
    EXPECT_EQ(wildcard[0].Tag, Node::any_tag);
}

TEST_F(TraceWriterTest, ZeroRankFileHasOnlyTheHeader) {
    std::ostringstream output;
    TraceWriter writer(output, layout, 0, 2, custom_fields);
    writer.finish();
    const auto file = output.str();
    const auto header = read_binary_header(std::as_bytes(std::span(file)));
    EXPECT_EQ(header.rank_count, 0u);
    EXPECT_EQ(file.size(), header.header_size_bytes);
}

// The rank table is reserved in chunks; consecutive empty ranks share an offset.
TEST_F(TraceWriterTest, WritesManyEmptyRanks) {
    std::ostringstream output;
    TraceWriter writer(output, layout, 300, 2, custom_fields);
    for (int rank = 0; rank < 300; ++rank) writer.write_rank(empty);
    writer.finish();
    const auto file = output.str();
    const auto bytes = std::as_bytes(std::span(file));
    const auto header = read_binary_header(bytes);
    EXPECT_EQ(file.size(), header.header_size_bytes + 300 * rank_table_entry_size_bytes);
    for (std::size_t rank = 0; rank < 300; ++rank)
        ASSERT_EQ(read_table_entry(bytes, header, rank), (RankTableEntry{file.size(), 0, 0, 0})) << "rank " << rank;
}

TEST_F(TraceWriterTest, RejectsUnusableStreams) {
    std::ostringstream nonempty;
    nonempty << "prefix";
    EXPECT_THROW(TraceWriter(nonempty, layout, 0, 2, custom_fields), std::invalid_argument);

    FailingBuffer unseekable;
    unseekable.fail_seek = true;
    std::ostream unseekable_output(&unseekable);
    EXPECT_THROW(TraceWriter(unseekable_output, layout, 0, 2, custom_fields), std::invalid_argument);

    FailingBuffer unwritable;
    unwritable.fail_write = true;
    std::ostream unwritable_output(&unwritable);
    EXPECT_THROW(TraceWriter(unwritable_output, layout, 0, 2, custom_fields), std::runtime_error);
}

TEST_F(TraceWriterTest, ReportsFailedRankWritesAndFlushes) {
    for (const bool fail_seek : {false, true}) {
        FailingBuffer buffer;
        std::ostream output(&buffer);
        TraceWriter writer(output, layout, 1, 2, custom_fields);
        buffer.fail_seek = fail_seek;
        buffer.fail_write = !fail_seek;
        EXPECT_THROW(writer.write_rank(first), std::runtime_error) << (fail_seek ? "failed seek" : "failed write");
    }
    FailingBuffer buffer;
    std::ostream output(&buffer);
    TraceWriter writer(output, layout, 0, 2, custom_fields);
    buffer.fail_flush = true;
    EXPECT_THROW(writer.finish(), std::runtime_error);
}
