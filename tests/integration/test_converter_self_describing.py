"""The converter's self-describing output, decoded independently and compared with legacy output."""

import struct

import pytest

from helpers import append_to_operations

STANDARD_FIELDS = {
    1: "type", 2: "peer", 3: "size", 4: "tag", 5: "cpu", 6: "nic",
    7: "depcnt", 8: "nDep", 9: "depIx", 10: "nSDep", 11: "sDepIx",
}
LEGACY_FIELDS = ["depcnt", "type", "peer", "size", "tag", "cpu", "nic", "nDep", "depIx", "nSDep", "sDepIx"]

SCHEDULE = """num_ranks 3
rank 0 {
l1: calc 1000 cpu 2
l2: send 8b to 1 tag 42 cpu 2 nic 1
l3: recv 8b from -1 tag -1 cpu 1 nic 1
l2 requires l1
l3 requires l2
l3 irequires l1
}
rank 1 {
l1: recv 8b from 0 tag 42
l2: calc 500
l2 requires l1
}
rank 2 {
}
"""


def bits(data, offset, width):
    return (int.from_bytes(data, "little") >> offset) & ((1 << width) - 1)


def decode_self_describing(data):
    """(ranks, fields, index bits); each rank is (nodes, dependents, roots), a node a dict of
    field values with None for ANY. Checks padding, offsets and roots along the way."""
    magic, version, header_size, record_bits, field_count, rank_count, index_bits, table = \
        struct.unpack_from("<8sHQQIIBQ", data)
    assert magic == b"GOALSD01" and version == 2
    assert table == header_size and 1 <= index_bits <= 64
    offset = struct.calcsize("<8sHQQIIBQ")
    fields = []
    for _ in range(field_count):
        kind, value_type, width, wildcard, bit_offset, base, maximum, identity = \
            struct.unpack_from("<BBBBQQQI", data, offset)
        offset += struct.calcsize("<BBBBQQQI")
        if kind == 1:
            name = data[offset:offset + identity].decode()
            offset += identity
        else:
            assert kind == 0, "unknown field identity kind"
            name = STANDARD_FIELDS[identity]
        if value_type == 2 and base >= (1 << 63):
            base -= 1 << 64
        fields.append((name, width, bit_offset, base, maximum, wildcard))
    assert offset == header_size, "wrong header extent"
    record_size = record_bits // 8
    next_offset = table + rank_count * 20
    ranks = []
    for rank in range(rank_count):
        start, node_count, edge_count, root_count = struct.unpack_from("<QIII", data, table + rank * 20)
        assert start == next_offset, "rank payloads are not consecutive"
        nodes = []
        for node_index in range(node_count):
            record = data[start + node_index * record_size:start + (node_index + 1) * record_size]
            assert len(record) == record_size, "truncated node record"
            node = {}
            for name, width, bit_offset, base, maximum, wildcard in fields:
                code = bits(record, bit_offset, width)
                if wildcard and code == maximum + 1:
                    node[name] = None
                else:
                    assert code <= maximum, f"invalid code for {name}"
                    node[name] = base + code
            assert int.from_bytes(record, "little") >> sum(f[1] for f in fields) == 0, "record padding is not zero"
            nodes.append(node)
        appendix_start = start + node_count * record_size
        roots_start = appendix_start + (edge_count * index_bits + 7) // 8
        roots_size = (root_count * index_bits + 7) // 8
        appendix = data[appendix_start:roots_start]
        roots = data[roots_start:roots_start + roots_size]
        assert len(appendix) == roots_start - appendix_start and len(roots) == roots_size, "truncated index array"
        edges = [bits(appendix, i * index_bits, index_bits) for i in range(edge_count)]
        root_indices = [bits(roots, i * index_bits, index_bits) for i in range(root_count)]
        assert int.from_bytes(appendix, "little") >> (edge_count * index_bits) == 0, "appendix padding is not zero"
        assert int.from_bytes(roots, "little") >> (root_count * index_bits) == 0, "root padding is not zero"
        assert all(index < node_count for index in edges + root_indices), "index outside the rank"
        assert root_indices == [i for i, node in enumerate(nodes) if node["depcnt"] == 0], "wrong roots"
        next_offset = roots_start + roots_size
        ranks.append((nodes, edges, root_indices))
    assert next_offset == len(data), "trailing or missing rank bytes"
    return ranks, fields, index_bits


def decode_legacy(data):
    magic, rank_count = struct.unpack_from("=QI", data)
    assert magic == 4223, "wrong legacy signature"
    ranks = []
    for rank in range(rank_count):
        begin, end = struct.unpack_from("=QQ", data, 14 + rank * 16)
        start, stop = begin + 8, end + 8  # Legacy offsets are relative to the byte after the magic.
        node_count, root_count = struct.unpack_from("=II", data, start)
        roots = list(struct.unpack_from(f"={root_count}I", data, start + 8))
        records_start = start + 8 + root_count * 4
        nodes = []
        for index in range(node_count):
            node = dict(zip(LEGACY_FIELDS, struct.unpack_from("=IBIQIBBIIII", data, records_start + index * 39)))
            if node["type"] != 3:
                for name in ["peer", "tag"]:
                    if node[name] == (1 << 32) - 1:
                        node[name] = None
            nodes.append(node)
        appendix_start = records_start + node_count * 39
        edges = list(struct.unpack_from(f"={(stop - appendix_start) // 4}I", data, appendix_start))
        ranks.append((nodes, edges, roots))
    return ranks


def assert_same_graphs(legacy, packed):
    assert len(legacy) == len(packed), "rank count changed"
    for rank, ((old_nodes, old_edges, old_roots), (nodes, edges, roots)) in enumerate(zip(legacy, packed)):
        assert (old_edges, old_roots, len(old_nodes)) == (edges, roots, len(nodes)), f"rank {rank}"
        for index, (old, node) in enumerate(zip(old_nodes, nodes)):
            for name, value in old.items():
                if old["type"] == 3 and name in ["peer", "tag"]:
                    continue  # Calculation placeholders have no meaning.
                assert node[name] == value, f"rank {rank}, node {index}, field {name}"


@pytest.fixture
def convert(converter, tmp_path):
    """Converts GOAL text to tmp_path/output.bin and returns the converter's result."""
    def convert(text, *options, success=True, **keywords):
        source = tmp_path / "input.goal"
        source.write_text(text)
        result = converter(source, tmp_path / "output.bin", *options, check=False, **keywords)
        assert (result.returncode == 0) == success, result.stdout + result.stderr
        return result
    return convert


@pytest.fixture
def output(tmp_path):
    return tmp_path / "output.bin"


def test_legacy_is_the_default_format(convert, output):
    convert(SCHEDULE)
    default = output.read_bytes()
    convert(SCHEDULE, format="legacy")
    assert output.read_bytes() == default


def test_stores_the_legacy_graph(convert, output):
    convert(SCHEDULE)
    legacy = decode_legacy(output.read_bytes())
    convert(SCHEDULE, format="self-describing")
    packed, _, index_bits = decode_self_describing(output.read_bytes())
    assert_same_graphs(legacy, packed)
    assert index_bits == 2
    assert packed[0][1] == [1, 2, 2]
    assert packed[1][1] == [1]


def test_stores_the_reference_graph(convert, output, reference_case):
    reference = (reference_case / "schedule.goal").read_text()
    convert(reference)
    legacy = decode_legacy(output.read_bytes())
    convert(reference, format="self-describing")
    assert_same_graphs(legacy, decode_self_describing(output.read_bytes())[0])


def test_report_and_progress_leave_the_output_unchanged(convert, output):
    reported = convert(SCHEDULE, "--report-schema", "--progress", format="self-describing")
    with_report = output.read_bytes()
    for text in ["written to self-describing output", "Two input passes", "analysed rank 3/3", "written rank 3/3"]:
        assert text in reported.stdout
    assert "proposal only" not in reported.stdout
    convert(SCHEDULE, format="self-describing")
    assert output.read_bytes() == with_report


def test_stores_custom_fields_in_declaration_order(convert, output, custom_fields):
    priorities = iter([-2, 4, 0, 1, 2])
    expected = []

    def attributes(line):
        gpu, priority = len(expected) % 3, next(priorities)
        expected.append((gpu, priority))
        return f" priority {priority} gpu_id {gpu}"

    result = convert(append_to_operations(SCHEDULE, attributes), "--report-schema",
                     format="self-describing", fields=custom_fields)
    assert "legacy output does not store" not in result.stderr
    ranks, fields, _ = decode_self_describing(output.read_bytes())
    assert [(node["gpu_id"], node["priority"]) for nodes, _, _ in ranks for node in nodes] == expected
    assert [field[0] for field in fields][-2:] == ["gpu_id", "priority"]
    widths = {field[0]: field[1] for field in fields}
    assert (widths["gpu_id"], widths["priority"]) == (2, 3), "ranges were not inferred"


def test_keeps_64_bit_boundary_values(convert, output, custom_fields):
    convert("""num_ranks 1
rank 0 {
l1: calc 0 gpu_id 0 priority -9223372036854775808
l2: calc 18446744073709551615 priority 9223372036854775807 gpu_id 18446744073709551615
}
""", format="self-describing", fields=custom_fields)
    ranks, fields, _ = decode_self_describing(output.read_bytes())
    nodes = ranks[0][0]
    assert nodes[0]["priority"] == -(1 << 63)
    assert nodes[1]["gpu_id"] == (1 << 64) - 1
    assert nodes[1]["size"] == (1 << 64) - 1
    assert all(field[1] == 64 for field in fields if field[0] in ["gpu_id", "priority", "size"])


def test_requires_every_custom_field_on_every_operation(convert, custom_fields):
    result = convert("num_ranks 1\nrank 0 {\nl1: calc 5 gpu_id 2\n}\n", format="self-describing",
                     fields=custom_fields, success=False)
    assert "Missing custom field 'priority'" in result.stderr


def test_writes_empty_ranks(convert, output):
    convert("num_ranks 1\nrank 0 {\n}\n", format="self-describing")
    assert decode_self_describing(output.read_bytes())[0] == [([], [], [])]


def test_sizes_indices_for_the_node_count(convert, output):
    convert("num_ranks 1\nrank 0 {\n" + "".join(f"l{i}: calc 1\n" for i in range(9)) + "}\n", format="self-describing")
    ranks, _, index_bits = decode_self_describing(output.read_bytes())
    assert index_bits == 4
    assert ranks[0][2] == list(range(9))


def test_rejects_unknown_formats(convert):
    convert(SCHEDULE, format="unknown", success=False)


def test_failed_analysis_keeps_an_existing_output(convert, output):
    output.write_bytes(b"keep existing output")
    convert(SCHEDULE.replace("calc 1000", "calc 18446744073709551616"), format="self-describing", success=False)
    assert output.read_bytes() == b"keep existing output"


@pytest.mark.parametrize("alias", [False, True], ids=["same path", "symbolic link"])
def test_refuses_to_overwrite_its_input(converter, tmp_path, alias):
    source = tmp_path / "input.goal"
    source.write_text(SCHEDULE)
    output = source
    if alias:
        output = tmp_path / "input-alias.goal"
        output.symlink_to(source)
    result = converter(source, output, format="self-describing", check=False)
    assert result.returncode != 0
    assert source.read_text() == SCHEDULE
