#include "RecordDecoding.hpp"
#include "goal_parsing/CustomFields.hpp"
#include "binary_writing/RecordPacking.hpp"
#include "goal_parsing/TraceAnalysis.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

using namespace traceschema;
using test_helpers::decode_record;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static auto parse(const char* text) {
    std::istringstream input(text);
    return read_custom_fields(input);
}

int main() try {
    auto fields = parse(R"(
        # Fields retain declaration order.
        [[fields]]
        name = "gpu_id"
        type = "unsigned"
        [[fields]]
        name = "priority"
        type = "signed"
    )");
    check(fields.size() == 2 && !fields[0].value_range && !fields[0].width &&
          !fields[1].value_range && !fields[1].width,
          "Declarations invented a range or width");
    check(fields[0].type == FieldType::UnsignedInteger && fields[1].type == FieldType::SignedInteger,
          "Field types were not preserved");
    bool missing_range = false;
    try { (void)build_record_layout(fields); }
    catch (const std::invalid_argument&) { missing_range = true; }
    check(missing_range, "Resolution accepted fields before collecting values");

    // Schema resolution also accepts producer-provided bounds through the C++ API.
    fields[0].value_range = UnsignedRange{0, 7};
    fields[1].value_range = SignedRange{-2, 2};
    const auto schema = TraceAnalysis(fields).create_schema(2);
    check(schema.fields.size() == 13 &&
          std::get<std::string>(schema.fields[11].field) == "gpu_id" &&
          std::get<std::string>(schema.fields[12].field) == "priority",
          "Custom declaration order was lost");
    const RecordPacker packer(build_record_layout(fields));
    const RecordValues values{FieldValue{std::uint64_t{7}}, FieldValue{std::int64_t{-1}}};
    check(decode_record(packer.get_layout(), packer.pack(values)) == values, "TOML schema packing failed");
    check(parse("").empty(), "Empty configuration was rejected");
    check(parse(" \n\t# No custom fields\n").empty(), "Comment-only configuration was rejected");
    for (const auto* invalid : {
        "[[", "field = []", "fields = 3", "fields = [1]",
        "[[fields]]\ntype = 'unsigned'",
        "[[fields]]\nname = ''\ntype = 'unsigned'",
        "[[fields]]\nname = 3\ntype = 'unsigned'",
        "[[fields]]\nname = 'cpu'\ntype = 'unsigned'",
        "[[fields]]\nname = 'bad-name'\ntype = 'unsigned'",
        "[[fields]]\nname = 'x'\ntype = 'float'",
        "[[fields]]\nname = 'x'\ntype = 'unsigned'\nrange = {min = 0, max = 7}",
        "[[fields]]\nname = 'x'\ntype = 'unsigned'\nbits = 3",
        "[[fields]]\nname = 'x'\ntype = 'unsigned'\n[[fields]]\nname = 'x'\ntype = 'signed'",
        "[[fields]]\nname = 'x'\nname = 'y'\ntype = 'unsigned'"
    }) {
        bool rejected = false;
        try { (void)parse(invalid); }
        catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid TOML definition was accepted");
    }
    std::cout << "Custom field checks passed\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
