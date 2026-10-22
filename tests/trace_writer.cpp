#include "RecordDecoding.hpp"
#include "binary_writing/TraceWriter.hpp"
#include "binary_writing/LegacyTraceWriter.hpp"
#include "simulators/loggopsim/SimulationSchedule.hpp"
#include "trace_format/BinaryFormat.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/Goal.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

using namespace traceschema;
using test_helpers::decode_record;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Exception, class Function>
static void rejects(Function function) {
    try { function(); }
    catch (const Exception&) { return; }
    throw std::runtime_error("Invalid writer operation was accepted");
}

class FailingBuffer : public std::stringbuf {
public:
    bool fail_write = false;
    bool fail_seek = false;
    bool fail_flush = false;

protected:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        return fail_write ? 0 : std::stringbuf::xsputn(bytes, count);
    }
    pos_type seekoff(off_type offset, std::ios_base::seekdir direction,
                     std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekoff(offset, direction, mode);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode mode) override {
        return fail_seek ? pos_type(off_type(-1)) : std::stringbuf::seekpos(position, mode);
    }
    int sync() override { return fail_flush ? -1 : std::stringbuf::sync(); }
};

int main(int argc, char** argv) try {
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
    const std::vector custom_fields{
        FieldValueInfo::create_custom("priority", FieldType::SignedInteger,
                                        std::nullopt, SignedRange{-2, 4})};
    inputs.insert(inputs.end(), custom_fields.begin(), custom_fields.end());
    const auto layout = build_record_layout(inputs, 3);

    Goal first;
    auto* calc = first.Calc(1000, 0, 0);
    auto* send = first.Send(1, 1024, 42, 0, 0);
    auto* recv = first.Recv(Node::any_source, 1024, Node::any_tag, 0, 0);
    first.Dependency(send, calc);
    first.StartDependency(recv, calc);
    first.Dependency(recv, send);
    calc->custom_values = {std::int64_t{-2}};
    send->custom_values = {std::int64_t{4}};
    recv->custom_values = {std::int64_t{0}};

    Goal second;
    auto* receive = second.Recv(0, 1024, 42, 0, 0);
    auto* compute = second.Calc(500, 0, 0);
    second.Dependency(compute, receive);
    receive->custom_values = {std::int64_t{1}};
    compute->custom_values = {std::int64_t{2}};
    Goal empty;

    std::ostringstream output(std::ios::out | std::ios::binary);
    TraceWriter writer(output, layout, 3, 2, custom_fields);
    writer.write_rank(first);
    rejects<std::logic_error>([&] { writer.finish(); });
    writer.write_rank(second);
    writer.write_rank(empty);
    rejects<std::logic_error>([&] { writer.write_rank(empty); });
    writer.finish();
    writer.finish();
    rejects<std::logic_error>([&] { writer.write_rank(empty); });

    const auto file = output.str();
    const auto bytes = std::as_bytes(std::span(file));
    const auto header = read_binary_header(bytes);
    check(header.rank_count == 3 && header.node_index_bits == 2 &&
          header.rank_table_offset_bytes == header.header_size_bytes, "Wrong trace header metadata");
    const auto table_entry = [&](std::size_t rank) {
        return read_rank_table_entry(bytes.subspan(
            header.rank_table_offset_bytes + rank * rank_table_entry_size_bytes, rank_table_entry_size_bytes));
    };
    const auto rank0 = table_entry(0);
    const auto rank1 = table_entry(1);
    const auto rank2 = table_entry(2);
    const auto record_size = header.layout.get_record_size_bytes();
    const auto data_begin = header.header_size_bytes + 3 * rank_table_entry_size_bytes;
    check(rank0 == RankTableEntry{data_begin, 3, 3, 1}, "Wrong first rank entry");
    check(rank1 == RankTableEntry{data_begin + 3 * record_size + 2, 2, 1, 1}, "Wrong second rank entry");
    check(rank2 == RankTableEntry{rank1.data_offset_bytes + 2 * record_size + 2, 0, 0, 0} &&
          rank2.data_offset_bytes == file.size(), "Empty rank offset or total file size changed");

    const auto read_record = [&](const RankTableEntry& rank, std::size_t node) {
        return decode_record(header.layout, bytes.subspan(rank.data_offset_bytes + node * record_size, record_size));
    };
    const RecordValues expected_calc{
        std::uint64_t{0}, std::uint64_t{3}, std::uint64_t{0}, std::uint64_t{1000},
        std::uint64_t{42}, std::uint64_t{0}, std::uint64_t{0}, std::uint64_t{1},
        std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{1}, std::int64_t{-2}};
    check(read_record(rank0, 0) == expected_calc, "Node record or signed custom value changed");
    const auto wildcard_record = read_record(rank0, 2);
    check(std::holds_alternative<Wildcard>(wildcard_record[2]) &&
          std::holds_alternative<Wildcard>(wildcard_record[4]), "Wildcard record changed");
    check(read_record(rank1, 0).back() == FieldValue{std::int64_t{1}} &&
          read_record(rank1, 1).back() == FieldValue{std::int64_t{2}}, "Second rank records changed");
    check(bytes[rank0.data_offset_bytes + 3 * record_size] == std::byte{0x29} &&
          bytes[rank1.data_offset_bytes - 1] == std::byte{0} &&
          bytes[rank1.data_offset_bytes + 2 * record_size] == std::byte{1} &&
          bytes[rank2.data_offset_bytes - 1] == std::byte{0}, "Dependency/root sections changed");

    if (argc > 1) {
        {
            std::ofstream disk_output(argv[1], std::ios::binary | std::ios::trunc);
            TraceWriter disk_writer(disk_output, layout, 3, 2, custom_fields);
            disk_writer.write_rank(first);
            disk_writer.write_rank(second);
            disk_writer.write_rank(empty);
            disk_writer.finish();
        }
        std::ifstream disk_input(argv[1], std::ios::binary);
        const std::string disk_bytes{std::istreambuf_iterator<char>(disk_input), {}};
        check(disk_bytes == file, "File-backed writer differs from memory-backed writer");

        // Interleaving outputs catches the former static file-size/resource state.
        const auto legacy_path = std::string(argv[1]) + ".legacy";
        const auto independent_path = std::string(argv[1]) + ".independent";
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
            LegacyTraceWriter legacy(legacy_path.c_str(), 3);
            LegacyTraceWriter other(independent_path.c_str(), 1);
            legacy.write_rank(first);
            other.write_rank(independent);
            other.finish();
            legacy.write_rank(second);
            legacy.write_rank(empty);
            legacy.finish();
        }
        {
            SimulationSchedule legacy(legacy_path.c_str(), false);
            SimulationSchedule other(independent_path.c_str(), false);
            check(legacy.schedules.size() == 3 && legacy.GetNumCPU() == 8 &&
                  legacy.GetNumNIC() == 6 && legacy.schedules[2].GetNumNodes() == 0,
                  "Extracted legacy writer lost ranks or resource maxima");
            check(other.schedules.size() == 1 && other.GetNumCPU() == 2 && other.GetNumNIC() == 1,
                  "Legacy writer instances shared resource or file state");
            const auto ready = legacy.schedules[0].GetExecutableNodes_DSN();
            check(ready.size() == 1 && ready[0].Size == 1000 && ready[0].Proc == 7,
                  "Legacy root record changed");
            legacy.schedules[0].MarkNodeAsStarted(ready[0].offset);
            check(legacy.schedules[0].GetExecutableNodes_DSN().empty(),
                  "Start dependency released a node too early");
            legacy.schedules[0].MarkNodeAsDone(ready[0].offset);
            const auto next = legacy.schedules[0].GetExecutableNodes_DSN();
            check(next.size() == 1 && next[0].offset == 1 && next[0].Nic == 5,
                  "Legacy completion dependency changed");
            legacy.schedules[0].MarkNodeAsDone(next[0].offset);
            const auto wildcard = legacy.schedules[0].GetExecutableNodes_DSN();
            check(wildcard.size() == 1 && wildcard[0].offset == 2 &&
                  wildcard[0].Peer == Node::any_source && wildcard[0].Tag == Node::any_tag,
                  "Legacy mixed dependencies or wildcard values changed");
        }
        std::remove(legacy_path.c_str());
        std::remove(independent_path.c_str());
    }

    std::ostringstream zero_output;
    TraceWriter zero_writer(zero_output, layout, 0, 2, custom_fields);
    zero_writer.finish();
    const auto zero_file = zero_output.str();
    const auto zero_header = read_binary_header(std::as_bytes(std::span(zero_file)));
    check(zero_header.rank_count == 0 && zero_file.size() == zero_header.header_size_bytes,
          "Zero-rank file must contain only its header");

    std::ostringstream many_output;
    TraceWriter many_writer(many_output, layout, 300, 2, custom_fields);
    for (int rank = 0; rank < 300; ++rank) many_writer.write_rank(empty);
    many_writer.finish();
    const auto many_file = many_output.str();
    const auto many_bytes = std::as_bytes(std::span(many_file));
    const auto many_header = read_binary_header(many_bytes);
    check(many_file.size() == many_header.header_size_bytes + 300 * rank_table_entry_size_bytes,
          "Chunked rank table reservation changed file size");
    for (std::size_t rank = 0; rank < 300; ++rank)
        check(read_rank_table_entry(many_bytes.subspan(
                  many_header.rank_table_offset_bytes + rank * rank_table_entry_size_bytes,
                  rank_table_entry_size_bytes)) == RankTableEntry{many_file.size(), 0, 0, 0},
              "Consecutive empty ranks must share an offset");

    std::ostringstream nonzero_output;
    nonzero_output << "prefix";
    rejects<std::invalid_argument>([&] { TraceWriter invalid(nonzero_output, layout, 0, 2, custom_fields); });

    FailingBuffer unseekable_buffer;
    unseekable_buffer.fail_seek = true;
    std::ostream unseekable_output(&unseekable_buffer);
    rejects<std::invalid_argument>([&] { TraceWriter invalid(unseekable_output, layout, 0, 2, custom_fields); });

    FailingBuffer header_failure;
    header_failure.fail_write = true;
    std::ostream bad_header_output(&header_failure);
    rejects<std::runtime_error>([&] { TraceWriter invalid(bad_header_output, layout, 0, 2, custom_fields); });

    for (const bool fail_seek : {false, true}) {
        FailingBuffer rank_failure;
        std::ostream bad_rank_output(&rank_failure);
        TraceWriter bad_rank_writer(bad_rank_output, layout, 1, 2, custom_fields);
        rank_failure.fail_seek = fail_seek;
        rank_failure.fail_write = !fail_seek;
        rejects<std::runtime_error>([&] { bad_rank_writer.write_rank(first); });
    }

    FailingBuffer flush_failure;
    std::ostream bad_flush_output(&flush_failure);
    TraceWriter bad_flush_writer(bad_flush_output, layout, 0, 2, custom_fields);
    flush_failure.fail_flush = true;
    rejects<std::runtime_error>([&] { bad_flush_writer.finish(); });
    std::cout << "Trace writer checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
