"""Check complete CLI output against legacy graphs and independently decode packed fields."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile


STANDARD_FIELDS = {
    1: "type", 2: "peer", 3: "size", 4: "tag", 5: "cpu", 6: "nic",
    7: "depcnt", 8: "nDep", 9: "depIx", 10: "nSDep", 11: "sDepIx",
}
LEGACY_FIELDS = ["depcnt", "type", "peer", "size", "tag", "cpu", "nic",
                 "nDep", "depIx", "nSDep", "sDepIx"]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def bits(data, offset, width):
    return (int.from_bytes(data, "little") >> offset) & ((1 << width) - 1)


def decode_new(data):
    magic, version, header_size, record_bits, field_count, rank_count, index_bits, table = \
        struct.unpack_from("<8sHQQIIBQ", data)
    require(magic == b"GOALSD01" and version == 2, "Wrong self-describing signature/version")
    require(table == header_size and 1 <= index_bits <= 64, "Wrong table location/index width")
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
            require(kind == 0, "Unknown field identity kind")
            name = STANDARD_FIELDS[identity]
        if value_type == 2 and base >= (1 << 63):
            base -= 1 << 64
        fields.append((name, width, bit_offset, base, maximum, wildcard))
    require(offset == header_size, "Wrong header extent")
    record_size = record_bits // 8
    next_offset = table + rank_count * 20
    ranks = []
    for rank in range(rank_count):
        start, node_count, edge_count, root_count = struct.unpack_from("<QIII", data, table + rank * 20)
        require(start == next_offset, "Rank payloads are not consecutive")
        nodes = []
        for node_index in range(node_count):
            record = data[start + node_index * record_size:start + (node_index + 1) * record_size]
            require(len(record) == record_size, "Truncated node record")
            node = {}
            for name, width, bit_offset, base, maximum, wildcard in fields:
                code = bits(record, bit_offset, width)
                if wildcard and code == maximum + 1:
                    node[name] = None
                else:
                    require(code <= maximum, "Invalid encoded numeric offset")
                    node[name] = base + code
            require(int.from_bytes(record, "little") >> sum(f[1] for f in fields) == 0,
                    "Record padding is not zero")
            nodes.append(node)
        appendix_start = start + node_count * record_size
        appendix_size = (edge_count * index_bits + 7) // 8
        roots_start = appendix_start + appendix_size
        roots_size = (root_count * index_bits + 7) // 8
        appendix = data[appendix_start:roots_start]
        roots = data[roots_start:roots_start + roots_size]
        require(len(appendix) == appendix_size and len(roots) == roots_size, "Truncated index array")
        edges = [bits(appendix, i * index_bits, index_bits) for i in range(edge_count)]
        root_indices = [bits(roots, i * index_bits, index_bits) for i in range(root_count)]
        require(int.from_bytes(appendix, "little") >> (edge_count * index_bits) == 0 and
                int.from_bytes(roots, "little") >> (root_count * index_bits) == 0, "Index padding is not zero")
        require(all(index < node_count for index in edges + root_indices), "Index outside rank")
        require(root_indices == [i for i, node in enumerate(nodes) if node["depcnt"] == 0], "Wrong roots")
        next_offset = roots_start + roots_size
        ranks.append((nodes, edges, root_indices))
    require(next_offset == len(data), "Trailing or missing rank bytes")
    return ranks, fields, index_bits


def decode_legacy(data):
    magic, rank_count = struct.unpack_from("=QI", data)
    require(magic == 4223, "Wrong legacy signature")
    ranks = []
    for rank in range(rank_count):
        begin, end = struct.unpack_from("=QQ", data, 14 + rank * 16)
        start, stop = begin + 8, end + 8  # Legacy offsets are relative to the byte after the magic.
        node_count, root_count = struct.unpack_from("=II", data, start)
        roots = list(struct.unpack_from(f"={root_count}I", data, start + 8))
        records_start = start + 8 + root_count * 4
        nodes = []
        for index in range(node_count):
            values = struct.unpack_from("=IBIQIBBIIII", data, records_start + index * 39)
            node = dict(zip(LEGACY_FIELDS, values))
            if node["type"] != 3:
                for name in ["peer", "tag"]:
                    if node[name] == (1 << 32) - 1:
                        node[name] = None
            nodes.append(node)
        appendix_start = records_start + node_count * 39
        edge_count = (stop - appendix_start) // 4
        edges = list(struct.unpack_from(f"={edge_count}I", data, appendix_start))
        ranks.append((nodes, edges, roots))
    return ranks


def compare_graphs(legacy, packed):
    require(len(legacy) == len(packed), "Rank count changed")
    for (old_nodes, old_edges, old_roots), (nodes, edges, roots) in zip(legacy, packed):
        require(old_edges == edges and old_roots == roots and len(old_nodes) == len(nodes),
                "Graph connections or roots changed")
        for old, node in zip(old_nodes, nodes):
            for name, value in old.items():
                if old["type"] == 3 and name in ["peer", "tag"]:
                    continue  # Calc placeholders have no communication meaning.
                require(node[name] == value, f"Changed {name}: {node[name]} != {value}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", required=True)
    parser.add_argument("--plain-fields", required=True, help="Custom-field TOML for plain traces")
    parser.add_argument("--custom-fields", required=True, help="Custom-field TOML declaring gpu_id and priority")
    parser.add_argument("--reference", required=True)
    args = parser.parse_args()
    schedule = """num_ranks 3
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
    with tempfile.TemporaryDirectory(prefix="self-describing-") as temporary:
        directory = Path(temporary)
        source, target = directory / "input.goal", directory / "output.bin"

        def convert(text, *options, fields=args.plain_fields, success=True):
            source.write_text(text)
            result = subprocess.run([args.converter, "-i", str(source), "-o", str(target), *options,
                                     "--custom-fields", fields],
                                    capture_output=True, text=True, timeout=30)
            require((result.returncode == 0) == success, result.stdout + result.stderr)
            return result

        convert(schedule)
        legacy_bytes = target.read_bytes()
        convert(schedule, "--format", "legacy")
        require(target.read_bytes() == legacy_bytes, "Explicit legacy format changed bytes")
        reported = convert(schedule, "--format", "self-describing", "--report-schema", "--progress")
        packed_bytes = target.read_bytes()
        packed, _, index_bits = decode_new(packed_bytes)
        compare_graphs(decode_legacy(legacy_bytes), packed)
        require(index_bits == 2 and packed[0][1] == [1, 2, 2] and packed[1][1] == [1], "Wrong appendix")
        require("written to self-describing output" in reported.stdout and
                "Two input passes" in reported.stdout and "analysed rank 3/3" in reported.stdout and
                "written rank 3/3" in reported.stdout and "proposal only" not in reported.stdout,
                reported.stdout)
        convert(schedule, "--format", "self-describing")
        require(target.read_bytes() == packed_bytes, "Report/progress changed self-describing bytes")

        reference = Path(args.reference).read_text()
        convert(reference)
        reference_legacy = decode_legacy(target.read_bytes())
        convert(reference, "--format", "self-describing")
        compare_graphs(reference_legacy, decode_new(target.read_bytes())[0])

        priority = iter([-2, 4, 0, 1, 2])
        expected_custom = []
        custom_lines = []
        for line in schedule.splitlines():
            if re.match(r"l\d+:", line):
                value = next(priority)
                gpu = len(expected_custom) % 3
                expected_custom.append((gpu, value))
                line += f" priority {value} gpu_id {gpu}"
            custom_lines.append(line)
        custom_text = "\n".join(custom_lines) + "\n"
        result = convert(custom_text, "--format", "self-describing", "--report-schema",
                         fields=args.custom_fields)
        ranks, fields, _ = decode_new(target.read_bytes())
        actual_custom = [(node["gpu_id"], node["priority"]) for nodes, _, _ in ranks for node in nodes]
        require(actual_custom == expected_custom and [f[0] for f in fields][-2:] == ["gpu_id", "priority"],
                "Custom fields changed values or TOML declaration order")
        require("legacy output does not store" not in result.stderr, result.stderr)
        widths = {field[0]: field[1] for field in fields}
        require(widths["gpu_id"] == 2 and widths["priority"] == 3, "Custom ranges were not inferred")

        extremes = """num_ranks 1
rank 0 {
l1: calc 0 gpu_id 0 priority -9223372036854775808
l2: calc 18446744073709551615 priority 9223372036854775807 gpu_id 18446744073709551615
}
"""
        convert(extremes, "--format", "self-describing", fields=args.custom_fields)
        ranks, fields, _ = decode_new(target.read_bytes())
        require(ranks[0][0][0]["priority"] == -(1 << 63) and
                ranks[0][0][1]["gpu_id"] == (1 << 64) - 1 and
                ranks[0][0][1]["size"] == (1 << 64) - 1, "64-bit boundary values changed")
        require(all(field[1] == 64 for field in fields if field[0] in ["gpu_id", "priority", "size"]),
                "Full-range fields need 64 bits")

        convert("num_ranks 1\nrank 0 {\n}\n", "--format", "self-describing")
        require(decode_new(target.read_bytes())[0] == [([], [], [])], "Empty rank changed")
        larger = "num_ranks 1\nrank 0 {\n" + "".join(f"l{i}: calc 1\n" for i in range(9)) + "}\n"
        convert(larger, "--format", "self-describing")
        ranks, _, index_bits = decode_new(target.read_bytes())
        require(index_bits == 4 and ranks[0][2] == list(range(9)), "Wrong inferred root index width")

        convert(schedule, "--format", "unknown", success=False)
        # A failed first pass must not truncate an existing output.
        target.write_bytes(b"keep existing output")
        convert(schedule.replace("calc 1000", "calc 18446744073709551616"),
                "--format", "self-describing", success=False)
        require(target.read_bytes() == b"keep existing output", "Statistics failure truncated output")
        source.write_text(schedule)
        for output in [source, directory / "input-alias.goal"]:
            if output != source:
                output.symlink_to(source)
            result = subprocess.run([args.converter, "-i", str(source), "-o", str(output),
                                     "--format", "self-describing", "--custom-fields", args.plain_fields],
                                    capture_output=True, text=True, timeout=30)
            require(result.returncode != 0 and source.read_text() == schedule, "Aliased output damaged input")
        print("PASS: two-pass conversion, legacy graphs, custom fields, indices, empty ranks and errors")


if __name__ == "__main__":
    main()
