# Binary header, version 2

The header serializes a `RecordLayout`, rank count, file-wide node-index width and absolute rank-table offset. `txt2bin --format self-describing` uses `TraceWriter` to join the header, table and rank data after a statistics pass. Legacy output remains the default. `TraceReader` detects either format from the magic number and holds a `SelfDescribingTraceReader` or a `LegacyTraceReader`; both take the same field bindings, so `LogGOPSimAdapter` and `AtlahsAdapter` read both formats without branches of their own. Metadata, schema hashes, operation applicability and defaults are not encoded yet. Version 2 replaces the experimental version-1 layout-only header; the current reader rejects version 1. The legacy format itself is unchanged.

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
| 2 | 1 | Field bit width: 0–64. A width of 0 means the field has the same value on every node, which decodes to the value base. |
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

The entry reader requires an exact 20-byte span. It does not validate offsets against a file. The header supplies the rank count, index width and table location. `TraceWriter` writes the complete file; `TraceFileView` validates file/rank extents at setup, then `SelfDescribingTraceReader::read_root_nodes<Node>(rank)` validates roots while the shared node reader decodes the initial ready nodes. This combines validation and queue loading in one traversal. Graph setup validates every dependency-appendix index once; node reads still check each requested list's bounds. The immutable appendix is then reused without rescanning its indices on each node read. Field-only consumers do not scan dependency arrays. Other node values are checked when accessed; unreachable payload/custom values are not eagerly scanned. The existing simulator uses this reader through `RankSchedule`. Legacy converter output is unchanged.

## Runtime counter updates

The simulator requires the dependency-count encoding to include zero (unsigned base zero). It updates only that field's bits in the mapped record, preserving adjacent fields. Other required fields must fit the existing simulator destinations. Only Peer/Tag may decode as wildcard sentinels; numeric tags cannot collide with the legacy ANY value. CPU/NIC counts are obtained from their declared range maxima plus one. `SimulatorAdapter::invalidate_shared_input()` rejects borrowed buffers and private mappings. Both simulators call it before using destructive shared input; it replaces the first eight bytes with little-endian invalidation marker 2342, which subsequent loads reject. Default private mappings preserve files.

## Consumer field bindings

Consumers bind standard field IDs or custom names directly to their node members with `bind_field(identity, &Node::member)`. The destination type supplies the requirement. The reader resolves names and checks presence, type and decoded range once at setup, then decodes only the bound fields. There is no per-node custom-value list or separate custom-reading mode. A simulator gives these attributes their behavior.

`BaseDeserializedGraphNode` stays in `simulators/` and provides the common `node_index`, `dependency_count` and the lists of nodes released when the node finishes and when it starts. Graph-node children automatically require those fields; their bindings declare the additional standard/custom members. `adapter.read_node(rank, index)` returns an owned concrete node; the type is fixed by `SimulatorAdapter<Node>`. `adapter.read_dependents(rank, index, event)` returns the one list released by `DependencyEvent::Start` or `DependencyEvent::Finish`. LogGOPSim's scheduler requests only the relevant list in both formats. Its position and count use the same prepared `FieldSource` as full decoding, with shifts and masks resolved once at setup. Zero-bit fields return their constant base without loading record bytes. Node and slice bounds are still checked, including empty lists. Shared root loading preserves the derived type. Analysis consumers bind just the fields they need into a small struct without inheriting the graph base.

`binary_reading/DependencyListView.hpp` provides a borrowed view of one dependency list. `DependencyGraph` locates these lists for each node and manages counters; the view only reads and iterates over the stored indices. `from_bit_packed` reads variable-width little-endian indices in the new format; `from_legacy_u32` reads the old host-order 32-bit indices, including unaligned storage. Both expose indexing and iteration without copying the list. A plain `span<uint32_t>` cannot represent the packed form.

Decoded scalar values can outlive the reader and mapping; dependency-list views still borrow the mapped storage. Subsequent reads observe mapped counter updates; already decoded results remain snapshots. `binary_reading/TraceFileView.hpp/.cpp` handles header/table setup, record views and bounded root/dependency-index reads. `binary_reading/SelfDescribingTraceReader.hpp/.cpp` turns bindings into decoder input and reads nodes and roots. `binary_reading/TraceReader.hpp/.cpp` detects the format (`detect_trace_format`, which also rejects files invalidated by `--save-mem`) and forwards to the matching reader; `is_legacy()` tells an adapter which fields exist. `binary_reading/DependencyGraph.hpp/.cpp` holds the graph side: the dependency count and list bindings, attaching each graph node's dependency lists, and counter updates. The adapter supplies the node type and its bindings. Filename constructors own a `MappedTraceFile` in the shared `SimulatorAdapter`, declared before the reader so it is destroyed afterward. Borrowed-span constructors remain available for tests and tools; their caller owns the storage. Both LogGOPSim and ATLAHS use adapter-owned mappings for file input. `LegacyTraceReader.hpp` reads the original format with the same bindings: each standard field (type, peer, size, tag, CPU, NIC; the dependency count internally) has a fixed position given by `trace_format/LegacyFormat.hpp`, a field may be bound to several members, ANY (~0) becomes the binding's wildcard value, and bindings to custom fields are rejected. It validates every rank, its roots and its dependency appendix when opened; `MappedTraceFile.hpp` owns the mapping. `RankSchedule` keeps readiness and event handling in the simulator, separate from binary decoding.

## Decoder boundary

After reading the header, `Decoder(layout, requirements, destinations)` runs `resolve_required_fields` once. This setup helper validates requirements and returns descriptors in requirement order; the decoder stores them together with the complete record size from the layout. There is no separate selection class. `SelfDescribingTraceReader` turns `bind_field(identity, &Output::member)` declarations into requirements and destinations (member offset and size), into which the decoder writes the values. It checks wildcard support and numeric collisions with per-binding wildcard sentinels at setup. Shared code checks selected operation-code and peer ranges; selected Type/Peer values reject sends to ANY without decoding unselected fields. `LogGOPSimAdapter` derives from `SimulatorAdapter<LogGOPSimNode>`, which holds a `TraceReader` (either format) and exposes typed node/root reads, counts and dependency updates. The simulator-specific adapter supplies the node structure, bindings and wildcard conversions. Graph-node children automatically add the shared dependency requirements and receive their graph state through the common base. An optional typed validator runs after decoding. `SelfDescribingTraceReader` provides resource counts and, through `DependencyGraph`, packed counter updates. Graph-node setup automatically configures `FieldId::DependencyCount`; it requires an unsigned zero-based field without a wildcard. Field-only readers may explicitly configure a counter with `set_dependency_counter`. `decrement_dependency_count` rejects zero and preserves neighboring bits. `get_resource_count` derives maximum + 1 with overflow checking. The former simulator-only decoder/factory is removed; the decoder classes are described below.

## Decoders

The decoding code lives in `binary_reading/decoding/`. Its `runners/` folder contains
`PlanRunner.hpp`, the common interface, and groups implementations by family:

```text
runners/
  PlanRunner.hpp
  interpreted/      InterpretedRunner
  templated_node/    TemplatedNodeRunner
  templated_schema/  TemplatedSchemaRunner and KnownSchemas.hpp
  compiled/         Shared CompiledRunner interface and dispatch
    c/              CCompiler.cpp: generated C and the disk cache
    orc/            OrcCompiler.cpp and OrcLoader.cpp: LLVM plugin and loading
```

`Decoder` resolves and validates the requested fields, then builds a `DecodePlan` and a
`PlanRunner` that writes decoded values directly into their destinations. `Decoder::decode_into`
checks the complete record size and delegates to `PlanRunner::decode`. This shared runner
interface selects the fast `run` implementation when enough mapped bytes are available for its
wide loads, or `decode_bounded` near the mapping boundary. The bounded implementation uses the
same plan and reads only the record's own bits. Invalid codes and sends to ANY throw
`Invalid node record` without retrying the decode.

The `DecodePlan` (`DecodePlan.hpp`) is only data: one step per selected field, a `FieldSource` (where the field's bits
are: first byte, shift, mask, base, maximum offset) and a `FieldDestination` (output struct, member
offset and size, value written for ANY). `DependencyGraph` decrements counters in place with
`bit_packing::try_decrement_bits`, which updates one 64-bit word when the field fits in it.

`LOGGOPSIM_DECODER` selects the runner; other values are rejected.

- `plan` (default, `InterpretedRunner`) interprets the steps: one unaligned 64-bit load, shift, mask,
  range check and store per field.
- `jit` (`CompiledRunner`, C compiler) turns the steps into a C function, compiles it with `$LOGGOPSIM_JIT_CC`
  (`cc` by default, `-O2 -shared -fPIC`), and loads it via `dlopen`. Compiled libraries are cached
  under `$LOGGOPSIM_JIT_CACHE` (default `~/.cache/loggopsim-jit`) keyed by a hash of the source and
  compiler; concurrent builds write under a temp name and rename.
- `orc` (`CompiledRunner`, LLVM ORC) builds the same function as LLVM IR and compiles it
  in-process with ORC (`-O2` pipeline), without a compiler process or disk cache. Its in-memory plan cache can be disabled with `LOGGOPSIM_ORC_REUSE=0`. It is built
  only when CMake finds LLVM (`-DLOGGOPSIM_WITH_LLVM=OFF` disables it; `-DLLVM_DIR=...` selects
  an installation) and lives in the `loggopsim_orc` plugin, opened on first use so that other
  runs never load LLVM; `$LOGGOPSIM_ORC_LIBRARY` overrides the plugin path.

- `templated_node` (`TemplatedNodeRunner`) is a template instantiated per simulator node type: the step count and
  each member's size and output struct are template arguments, so the steps are unrolled and
  every store has a fixed size. Positions, ranges and member offsets still come from the plan.
  `TemplatedNodeRunner.cpp` lists the shapes of the LogGOPSim and ATLAHS nodes; `tests/unit/trace_reader.cpp`
  checks that both adapters get a templated node runner.
- `templated_schema` (`TemplatedSchemaRunner`) takes every step as a template argument, so the whole plan is
  constant, as in the generated C. `KnownSchemas.hpp` lists the compiled-in layouts; on a trace
  without one, it reports the missing match once and falls back to `plan`.

If compilation fails or no template instantiation matches, the decoder uses `InterpretedRunner`.
`SimulatorAdapter::get_runner_name` reports the runner in use.

`tests/unit/decode_plan.cpp` checks every runner against the exact decoder on random layouts and
records (`TemplatedNodeRunner` on random layouts of one shape, `TemplatedSchemaRunner` on one fixed layout), and
decoding into destinations against an independent bit-by-bit reference decoder in `tests/unit/TestHelpers.hpp`.

Compiled runners receive a `CompiledCode` containing both the entry point and a shared lifetime owner. C libraries are shared between live readers of the same cache entry and unloaded when unused; the on-disk cache remains. ORC returns an owned engine, and its optional per-plan cache retains reusable engines. Disabling ORC reuse lets each runner release its engine. The plugin stays loaded until its executable owners are destroyed.
