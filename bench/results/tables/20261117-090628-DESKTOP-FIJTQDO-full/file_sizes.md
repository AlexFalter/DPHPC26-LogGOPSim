# Trace file size per schema

| trace | workload | ranks | schema | records | edges | legacy_bytes | sd_bytes | sd_over_legacy | legacy_bytes_per_record | sd_bytes_per_record | sd_record_bits | goal_bytes | header_sha256 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| allreduce_rd-p64-n1e6-inferred | allreduce_rd | 64 | inferred | 999552 | 1315136 | 44244878 | 10299723 | 0.233 | 44.3 | 10.3 | 64 | 54504628 | 2f1504162bf1f323 |
| allreduce_ring-p64-n1e6-inferred | allreduce_ring | 64 | inferred | 990976 | 1188480 | 43403790 | 11000459 | 0.253 | 43.8 | 11.1 | 72 | 56960556 | e1de87788a437e3f |
| allreduce_ring-p256-n1e7-inferred | allreduce_ring | 256 | inferred | 10126336 | 12149760 | 443533326 | 125568907 | 0.283 | 43.8 | 12.4 | 80 | 600592105 | 7c62187094079d12 |
| alltoall-p64-n1e6-inferred | alltoall | 64 | inferred | 999744 | 1975680 | 46894542 | 13456715 | 0.287 | 46.9 | 13.5 | 80 | 74155680 | c787305e8e29eba4 |
| dp_nccl-p16-n1e6-inferred | dp_nccl | 16 | inferred | 996864 | 1193456 | 43651982 | 12356299 | 0.283 | 43.8 | 12.4 | 80 | 58169044 | f77596c85599d221 |
| dp_nccl-p64-n1e7-inferred | dp_nccl | 64 | inferred | 10029376 | 12028160 | 439260110 | 137388363 | 0.313 | 43.8 | 13.7 | 88 | 607228804 | b0127c183a3381c1 |
| moe_alltoall-p32-n1e6-inferred | moe_alltoall | 32 | inferred | 1007872 | 1976032 | 47212046 | 13784907 | 0.292 | 46.8 | 13.7 | 80 | 77994928 | aac70290a6552e93 |
| pipeline_1f1b-p16-n1e6-inferred | pipeline_1f1b | 16 | inferred | 958272 | 958256 | 41206094 | 10541707 | 0.256 | 43 | 11 | 72 | 50080763 | 01ecea4b2e99849b |
| pipeline_1f1b-p64-n1e7-inferred | pipeline_1f1b | 64 | inferred | 9898240 | 9898176 | 425625870 | 121255179 | 0.285 | 43 | 12.3 | 80 | 535970861 | 34b59909cd32eb02 |
| stencil3d-p64-n1e6-inferred | stencil3d | 64 | inferred | 1000064 | 1845504 | 46386318 | 12232011 | 0.264 | 46.4 | 12.2 | 72 | 71663060 | 4cada367eb6fd4ed |
| stencil3d-p64-n1e8-inferred | stencil3d | 64 | inferred | 99999744 | 184614144 | 4638448398 | 1584611211 | 0.342 | 46.4 | 15.8 | 88 | 8105709636 | 5be50b829a29d3ec |
