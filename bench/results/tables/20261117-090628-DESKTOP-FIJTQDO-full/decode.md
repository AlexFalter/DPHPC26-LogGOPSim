# Decode microbenchmark (bench/micro/decode_bench.cpp)

ns_per_record: median over repetitions and processes; GB/s counts node-record bytes touched. Checksums agree across variants per trace: {}

| host | experiment | trace | records | variant | runner | order | fields | cache | jit_cold | core_class | n | processes | ns_per_record | ci_lo | ci_hi | mrecords_per_s | gb_per_s | record_bytes | setup_ms | resident_before | checksum |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
