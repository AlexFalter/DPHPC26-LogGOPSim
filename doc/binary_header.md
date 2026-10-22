# Binary header, version 2

The header serializes a `RecordLayout`, rank count, file-wide node-index width and absolute rank-table offset. `txt2bin --format self-describing` uses `TraceWriter` to join the header, table and rank data after a statistics pass. Legacy output remains the default; the simulator detects both formats and uses `LogGOPSimAdapter` over the generic `TraceReader` for self-describing files. Metadata, schema hashes, operation applicability and defaults are not encoded yet. Version 2 replaces the experimental version-1 layout-only header; the current reader rejects version 1. The legacy format's separate compatibility path is unchanged.

All multibyte integers are little-endian. Fields below are written consecutively, without native structure padding. Node records use the bit order implemented by `trace_format/BinaryIO.hpp`: bit zero is the least significant bit of byte zero.

## Fixed prefix: 43 bytes

| Byte offset | Bytes | Value |
|---|---:|---|
| 0 | 8 | ASCII `GOALSD01`; distinct from legacy magic 4223 and invalidated magic 2342 |
| 8 | 2 | Format version: 2 |
| 10 | 8 | Total header length in bytes, including prefix and descriptors |
| 18 | 8 | Node record size in bits, including byte padding |
| 26 | 4 | Number of field descriptors |
| 30 | 4 | Rank count |
| 34 | 1 | File-wide node-index width: 1–64 bits |
| 35 | 8 | Absolute file offset of the rank table |

The magic is a fixed signature; the separate version identifies layout revisions. Version 2 fixes little-endian byte order, so it has no byte-order flag. The header length identifies the first byte after the header. The writer places the rank table at that offset; the reader also accepts a later table location. Table entries occupy `rank_count × 20` bytes.

## Each field descriptor: 32 bytes plus any custom name

Descriptors retain the layout's field order. Their bit offsets may specify gaps or a different physical order.

| Relative byte offset | Bytes | Value |
|---|---:|---|
| 0 | 1 | Identity kind: 0 standard ID, 1 custom name |
| 1 | 1 | Logical type: 1 unsigned integer, 2 signed integer |
| 2 | 1 | Field bit width: 1–64 |
| 3 | 1 | Wildcard flag: 0 or 1 |
| 4 | 8 | Bit offset within the node record |
| 12 | 8 | Numeric base; signed bases use two's-complement bits |
| 20 | 8 | Maximum numeric offset from that base |
| 28 | 4 | Standard field ID, or custom-name length in bytes |
| 32 | variable | Custom name bytes, only for identity kind 1; no terminator |

Version 2 supports offset encoding only: numeric code = value − base. If enabled, ANY uses maximum numeric offset + 1. It is checked before reconstructing the numeric value. The full reconstructed range must fit the logical integer type. No width or encoding is inferred from record data by the reader.

Logical type IDs are the explicit numeric values of `FieldType` in `trace_format/FieldTypes.hpp` and must remain stable. Unknown type codes are rejected on read.

Standard IDs are explicitly assigned to `FieldId` in `FieldIdentity.hpp`. These enum values are part of the binary format and must not be renumbered. The shared `standard_fields` table supplies names and default record order independently:

| ID | Field | ID | Field |
|---:|---|---:|---|
| 1 | type | 7 | depcnt |
| 2 | peer | 8 | nDep |
| 3 | size | 9 | depIx |
| 4 | tag | 10 | nSDep |
| 5 | cpu | 11 | sDepIx |
| 6 | nic | | |

## API

```cpp
auto bytes = traceschema::write_binary_header(layout, rank_count, node_index_bits);
auto header = traceschema::read_binary_header(bytes);
// header.layout is validated; header.rank_table_offset_bytes locates the table.
// header.rank_count and header.node_index_bits describe the rank sections.
```

The reader accepts a header alone or a span containing further file data. It rejects unknown versions, identity/type codes and flags, truncation, inconsistent lengths/counts, extra bytes inside the version-2 header, and invalid record layouts. It bounds names and descriptor counts by the advertised header extent before allocating them. It also rejects index widths outside 1–64, table/header overlap and overflowing table extents. Because the API accepts a header alone, it does not require the actual table or rank payload to be present; `TraceFileView` validates those against file size. `RecordLayout::validate_layout()` handles duplicate identities, overlaps, invalid widths, encoding ranges and record boundaries.

## Rank table entry (written separately from the header)

`BinaryFormat.hpp/.cpp` provides `write_rank_table_entry` and `read_rank_table_entry`. Each entry is exactly 20 little-endian bytes; its position in the table identifies its rank.

| Byte offset | Type | Meaning |
| --- | --- | --- |
| 0 | u64 | Absolute file offset to the rank's first node record |
| 8 | u32 | Node count |
| 12 | u32 | Dependency appendix entry count |
| 16 | u32 | Root index count |

Rank data is `[node records][dependency appendix][root indices]`. Node bytes equal node count times record size. Each index array occupies `ceil(count × file-wide index bits / 8)` bytes, including its own final-byte padding. Counts use the current graph's 32-bit limits; file offsets are 64-bit. No native struct padding is serialized.

The entry reader requires an exact 20-byte span. It does not validate offsets against a file. The header supplies the rank count, index width and table location. `TraceWriter` writes the complete file; `TraceFileView` validates file/rank extents at setup, then shared `TraceReader::read_root_nodes<Node>(rank)` validates roots while the shared node reader decodes the initial ready nodes. This combines validation and queue loading in one traversal. Other node values and dependency indices are checked when accessed. Unreachable payload/custom values are not eagerly scanned. The existing simulator uses this reader through `SerializedGraph`. Legacy converter output is unchanged.

## Runtime counter updates

The simulator requires the dependency-count encoding to include zero (unsigned base zero). It updates only that field's bits in the mapped record, preserving adjacent fields. Other required fields must fit the existing simulator destinations. Only Peer/Tag may decode as wildcard sentinels; numeric tags cannot collide with the legacy ANY value. CPU/NIC counts are obtained from their declared range maxima plus one. Private mappings preserve files; destructive shared mappings replace the first eight bytes with little-endian invalidation marker 2342, which subsequent loads reject.

## Consumer field bindings

Consumers bind standard field IDs or custom names directly to their node members with `bind_field(identity, &Node::member)`. The destination type supplies the requirement. The reader resolves names and checks presence, type and decoded range once at setup, then decodes only the bound fields. There is no per-node custom-value list or separate custom-reading mode. A simulator gives these attributes their behavior.

`BaseDeserializedGraphNode` provides the common node index, dependency count and completion/start dependent lists. Graph-node children automatically require those fields; their bindings declare the additional standard/custom members. `adapter.read_node(rank, index)` returns an owned concrete node; the type is fixed by `SimulatorAdapter<Node>`. `read_node_into` reuses an output object. Shared root loading preserves the derived type. Field-only consumers need not use the base and can also request raw values through `read_fields` or `read_fields_into` in requirement order.

Decoded nodes can outlive the reader and mapping. Subsequent reads observe mapped counter updates; already decoded results remain snapshots. `binary_reading/TraceFileView.hpp/.cpp` handles header/table setup, record views and bounded root/dependency-index reads. `binary_reading/TraceReader.hpp/.cpp` handles selected-field assignment, graph assembly and root loading. The adapter supplies the node type and its bindings.

## Decoder boundary

After reading the header, `NodeDecoder(layout, requirements)` runs `resolve_required_fields` once. This setup helper validates requirements and returns descriptors in requirement order; the decoder stores them together with the complete record size from the layout. There is no separate selection class. The decoder provides two `decode_fields` overloads: one fills a caller-owned buffer, and the other invokes a callback for each field. `TraceReader::read_fields` provides the allocating vector convenience operation. `TraceReader` turns `bind_field(identity, &Output::member)` declarations into requirements and uses the shared callback to assign decoded values. It checks wildcard support and numeric collisions with per-binding wildcard sentinels at setup. Shared code checks selected operation-code and peer ranges; selected Type/Peer values reject sends to ANY without decoding unselected fields. `LogGOPSimAdapter` derives from `SimulatorAdapter<LogGOPSimNode>`, which holds a `TraceReader` and exposes typed node/root reads, counts and dependency updates. The simulator-specific adapter supplies the node structure, bindings and wildcard conversions. Graph-node children automatically add the shared dependency requirements and receive their graph state through the common base. An optional typed validator runs after decoding. `TraceReader` performs packed counter updates and resource-count calculation. Graph-node setup automatically configures `FieldId::DependencyCount`; it requires an unsigned zero-based field without a wildcard. Field-only readers may explicitly configure a counter with `set_dependency_counter`. `decrement_dependency_count` rejects zero and preserves neighboring bits. `get_resource_count` derives maximum + 1 with overflow checking. The former simulator-only decoder/factory is removed; a future JIT backend must specialize the shared decoding path. Runtime compilation is not implemented.
