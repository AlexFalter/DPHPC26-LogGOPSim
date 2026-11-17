# Trace file size per schema

| trace | workload | ranks | schema | records | edges | legacy_bytes | sd_bytes | sd_over_legacy | legacy_bytes_per_record | sd_bytes_per_record | sd_record_bits | goal_bytes | header_sha256 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| stencil3d-p64-n1e4-custom | stencil3d | 64 | custom | 9984 | 17664 | 461838 | 179304 | 0.388 | 46.3 | 18 | 128 | 1194020 | 29ab14f0450708cc |
| stencil3d-p64-n1e6-custom | stencil3d | 64 | custom | 1000064 | 1845504 | 46386318 | 21232744 | 0.458 | 46.4 | 21.2 | 144 | 130205268 | bf845b055b587817 |
| stencil3d-p64-n1e7-custom | stencil3d | 64 | custom | 9999808 | 18460416 | 463835982 | 231534312 | 0.499 | 46.4 | 23.2 | 152 | 1348993628 | bfb9a01abc367c09 |
| allreduce_rd-p64-n1e6-inferred | allreduce_rd | 64 | inferred | 999552 | 1315136 | 44244878 | 10299723 | 0.233 | 44.3 | 10.3 | 64 | 54504628 | 2f1504162bf1f323 |
| allreduce_ring-p64-n1e6-inferred | allreduce_ring | 64 | inferred | 990976 | 1188480 | 43403790 | 11000459 | 0.253 | 43.8 | 11.1 | 72 | 56960556 | e1de87788a437e3f |
| allreduce_ring-p256-n1e7-inferred | allreduce_ring | 256 | inferred | 10126336 | 12149760 | 443533326 | 125568907 | 0.283 | 43.8 | 12.4 | 80 | 600592105 | 7c62187094079d12 |
| alltoall-p64-n1e6-inferred | alltoall | 64 | inferred | 999744 | 1975680 | 46894542 | 13456715 | 0.287 | 46.9 | 13.5 | 80 | 74155680 | c787305e8e29eba4 |
| dp_nccl-p16-n1e6-inferred | dp_nccl | 16 | inferred | 996864 | 1193456 | 43651982 | 12356299 | 0.283 | 43.8 | 12.4 | 80 | 58169044 | f77596c85599d221 |
| dp_nccl-p64-n1e7-inferred | dp_nccl | 64 | inferred | 10029376 | 12028160 | 439260110 | 137388363 | 0.313 | 43.8 | 13.7 | 88 | 607228804 | b0127c183a3381c1 |
| moe_alltoall-p32-n1e6-inferred | moe_alltoall | 32 | inferred | 1007872 | 1976032 | 47212046 | 13784907 | 0.292 | 46.8 | 13.7 | 80 | 77994928 | aac70290a6552e93 |
| pipeline_1f1b-p16-n1e6-inferred | pipeline_1f1b | 16 | inferred | 958272 | 958256 | 41206094 | 10541707 | 0.256 | 43 | 11 | 72 | 50080768 | c9b47ff6991449d9 |
| pipeline_1f1b-p64-n1e7-inferred | pipeline_1f1b | 64 | inferred | 9898240 | 9898176 | 425625870 | 121255179 | 0.285 | 43 | 12.3 | 80 | 535970868 | a0d3f38d1363f08d |
| stencil3d-p64-n1e4-inferred | stencil3d | 64 | inferred | 9984 | 17664 | 461838 | 89291 | 0.193 | 46.3 | 8.94 | 56 | 609572 | 30239963545c3e70 |
| stencil3d-p64-n1e5-inferred | stencil3d | 64 | inferred | 99840 | 183552 | 4629774 | 1052939 | 0.227 | 46.4 | 10.5 | 64 | 6673860 | 4965be3ec0435411 |
| stencil3d-p64-n1e6-inferred | stencil3d | 64 | inferred | 1000064 | 1845504 | 46386318 | 12232011 | 0.264 | 46.4 | 12.2 | 72 | 71663060 | 4cada367eb6fd4ed |
| stencil3d-p64-n2e6-inferred | stencil3d | 64 | inferred | 2000128 | 3691776 | 92773902 | 24925067 | 0.269 | 46.4 | 12.5 | 72 | 146677476 | f6a8356b4b0814b1 |
| stencil3d-p64-n5e6-inferred | stencil3d | 64 | inferred | 5000320 | 9230592 | 231936654 | 69620107 | 0.3 | 46.4 | 13.9 | 80 | 371720724 | cd66d6d3b9d6651a |
| stencil3d-p32768-n1e7-inferred | stencil3d | 32768 | inferred | 9797632 | 17694720 | 453804046 | 108822923 | 0.24 | 46.3 | 11.1 | 72 | 645171891 | f6622a36bbf4a654 |
| stencil3d-p512-n1e7-inferred | stencil3d | 512 | inferred | 9997312 | 18450432 | 463711246 | 134579595 | 0.29 | 46.4 | 13.5 | 80 | 731771633 | 601cd81948183431 |
| stencil3d-p64-n1e7-inferred | stencil3d | 64 | inferred | 9999808 | 18460416 | 463835982 | 141535883 | 0.305 | 46.4 | 14.2 | 80 | None | 938bffcde964adba |
| stencil3d-p8-n1e7-inferred | stencil3d | 8 | inferred | 10000016 | 18461472 | 463846750 | 148462107 | 0.32 | 46.4 | 14.8 | 80 | 794443709 | f7b85f685487fb14 |
| stencil3d-p4096-n1e7-inferred | stencil3d | 4096 | inferred | 10010624 | 18432000 | 464257038 | 117834123 | 0.254 | 46.4 | 11.8 | 72 | 699164122 | c982b4eb0def3d08 |
| stencil3d-p64-n2e7-inferred | stencil3d | 64 | inferred | 19999616 | 36921600 | 927673230 | 287686859 | 0.31 | 46.4 | 14.4 | 80 | 1560624180 | d8346bb3c88482d4 |
| stencil3d-p64-n5e7-inferred | stencil3d | 64 | inferred | 49999872 | 92306688 | 2319223566 | 730767307 | 0.315 | 46.4 | 14.6 | 80 | 3951702276 | d446a3d2befa7019 |
| stencil3d-p64-n1e8-inferred | stencil3d | 64 | inferred | 99999744 | 184614144 | 4638448398 | 1584611211 | 0.342 | 46.4 | 15.8 | 88 | 8105709636 | 5be50b829a29d3ec |
| stencil3d-p64-n1e4-wide | stencil3d | 64 | wide | 9984 | 17664 | 461838 | 179147 | 0.388 | 46.3 | 17.9 | 128 | 853202 | 4b14f6bb7c9ae406 |
| stencil3d-p64-n1e6-wide | stencil3d | 64 | wide | 1000064 | 1845504 | 46386318 | 20232523 | 0.436 | 46.4 | 20.2 | 136 | 96414950 | cd895a0d4455454a |
| stencil3d-p64-n1e7-wide | stencil3d | 64 | wide | 9999808 | 18460416 | 463835982 | 221534347 | 0.478 | 46.4 | 22.2 | 144 | 1012706894 | aaaaffb494c8c8b8 |
