# Correctness report

Created 2026-11-17T08:10:45Z on DESKTOP-FIJTQDO; overall: **pass**.

## Simulator results vs BASE

| trace | config | status | detail |
|---|---|---|---|
| allreduce_rd-p64-n1e6-inferred | BASE | pass | reference |
| allreduce_rd-p64-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| allreduce_rd-p64-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| allreduce_rd-p64-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| allreduce_rd-p64-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| allreduce_ring-p256-n1e7-inferred | BASE | pass | reference |
| allreduce_ring-p256-n1e7-inferred | LAZY-interp | pass | identical to BASE |
| allreduce_ring-p256-n1e7-inferred | NEW-interp | pass | identical to BASE |
| allreduce_ring-p256-n1e7-inferred | NEW-spec-warm | pass | identical to BASE |
| allreduce_ring-p64-n1e6-inferred | BASE | pass | reference |
| allreduce_ring-p64-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| allreduce_ring-p64-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| allreduce_ring-p64-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| allreduce_ring-p64-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| alltoall-p64-n1e6-inferred | BASE | pass | reference |
| alltoall-p64-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| alltoall-p64-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| alltoall-p64-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| alltoall-p64-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| dp_nccl-p16-n1e6-inferred | BASE | pass | reference |
| dp_nccl-p16-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| dp_nccl-p16-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| dp_nccl-p16-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| dp_nccl-p16-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| dp_nccl-p64-n1e7-inferred | BASE | pass | reference |
| dp_nccl-p64-n1e7-inferred | LAZY-interp | pass | identical to BASE |
| dp_nccl-p64-n1e7-inferred | NEW-interp | pass | identical to BASE |
| dp_nccl-p64-n1e7-inferred | NEW-spec-warm | pass | identical to BASE |
| moe_alltoall-p32-n1e6-inferred | BASE | pass | reference |
| moe_alltoall-p32-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| moe_alltoall-p32-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| moe_alltoall-p32-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| moe_alltoall-p32-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| pipeline_1f1b-p16-n1e6-inferred | BASE | pass | reference |
| pipeline_1f1b-p16-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| pipeline_1f1b-p16-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| pipeline_1f1b-p16-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| pipeline_1f1b-p16-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| pipeline_1f1b-p64-n1e7-inferred | BASE | pass | reference |
| pipeline_1f1b-p64-n1e7-inferred | LAZY-interp | pass | identical to BASE |
| pipeline_1f1b-p64-n1e7-inferred | NEW-interp | pass | identical to BASE |
| pipeline_1f1b-p64-n1e7-inferred | NEW-spec-warm | pass | identical to BASE |
| stencil3d-p64-n1e6-inferred | BASE | pass | reference |
| stencil3d-p64-n1e6-inferred | LAZY-interp | pass | identical to BASE incl. -V timeline |
| stencil3d-p64-n1e6-inferred | NEW-interp | pass | identical to BASE incl. -V timeline |
| stencil3d-p64-n1e6-inferred | NEW-legacyfile | pass | identical to BASE incl. -V timeline |
| stencil3d-p64-n1e6-inferred | NEW-spec-warm | pass | identical to BASE incl. -V timeline |
| stencil3d-p64-n1e8-inferred | BASE | pass | reference |
| stencil3d-p64-n1e8-inferred | LAZY-interp | pass | identical to BASE |
| stencil3d-p64-n1e8-inferred | LAZY-legacyfile | pass | identical to BASE |
| stencil3d-p64-n1e8-inferred | NEW-interp | pass | identical to BASE |
| stencil3d-p64-n1e8-inferred | NEW-legacyfile | pass | identical to BASE |

## Byte identity of legacy/self-describing files written by NEW/omp vs BASE/master

| trace | writer | identical to |
|---|---|---|
| allreduce_rd-p64-n1e6-inferred | master-legacy | allreduce_rd-p64-n1e6-inferred.legacy.bin: yes |
| allreduce_rd-p64-n1e6-inferred | omp-legacy-t4 | allreduce_rd-p64-n1e6-inferred.legacy.bin: yes |
| allreduce_rd-p64-n1e6-inferred | omp-sd-t4 | allreduce_rd-p64-n1e6-inferred.sd.bin: yes |
| allreduce_ring-p64-n1e6-inferred | master-legacy | allreduce_ring-p64-n1e6-inferred.legacy.bin: yes |
| allreduce_ring-p64-n1e6-inferred | omp-legacy-t4 | allreduce_ring-p64-n1e6-inferred.legacy.bin: yes |
| allreduce_ring-p64-n1e6-inferred | omp-sd-t4 | allreduce_ring-p64-n1e6-inferred.sd.bin: yes |
| alltoall-p64-n1e6-inferred | master-legacy | alltoall-p64-n1e6-inferred.legacy.bin: yes |
| alltoall-p64-n1e6-inferred | omp-legacy-t4 | alltoall-p64-n1e6-inferred.legacy.bin: yes |
| alltoall-p64-n1e6-inferred | omp-sd-t4 | alltoall-p64-n1e6-inferred.sd.bin: yes |
| dp_nccl-p16-n1e6-inferred | master-legacy | dp_nccl-p16-n1e6-inferred.legacy.bin: yes |
| dp_nccl-p16-n1e6-inferred | omp-legacy-t4 | dp_nccl-p16-n1e6-inferred.legacy.bin: yes |
| dp_nccl-p16-n1e6-inferred | omp-sd-t4 | dp_nccl-p16-n1e6-inferred.sd.bin: yes |
| moe_alltoall-p32-n1e6-inferred | master-legacy | moe_alltoall-p32-n1e6-inferred.legacy.bin: yes |
| moe_alltoall-p32-n1e6-inferred | omp-legacy-t4 | moe_alltoall-p32-n1e6-inferred.legacy.bin: yes |
| moe_alltoall-p32-n1e6-inferred | omp-sd-t4 | moe_alltoall-p32-n1e6-inferred.sd.bin: yes |
| pipeline_1f1b-p16-n1e6-inferred | master-legacy | pipeline_1f1b-p16-n1e6-inferred.legacy.bin: yes |
| pipeline_1f1b-p16-n1e6-inferred | omp-legacy-t4 | pipeline_1f1b-p16-n1e6-inferred.legacy.bin: yes |
| pipeline_1f1b-p16-n1e6-inferred | omp-sd-t4 | pipeline_1f1b-p16-n1e6-inferred.sd.bin: yes |
| stencil3d-p64-n1e6-inferred | master-legacy | stencil3d-p64-n1e6-inferred.legacy.bin: yes |
| stencil3d-p64-n1e6-inferred | omp-legacy-t4 | stencil3d-p64-n1e6-inferred.legacy.bin: yes |
| stencil3d-p64-n1e6-inferred | omp-sd-t4 | stencil3d-p64-n1e6-inferred.sd.bin: yes |

## Validation-failure tests

| test | expected | exit | result | output |
|---|---|---|---|---|
| missing required custom field gpu_id is rejected at load time | failure | 3 | pass | `{"tool":"decode_bench_micro","mode":"require","trace":"/home/joshua/lgs-bench/traces/allreduce_rd-p64-n1e6-inferred.sd.bin","field":"gpu_id","accepted":false,"e` |
| legacy trace has no custom fields: gpu_id rejected | failure | 3 | pass | `{"tool":"decode_bench_micro","mode":"require","trace":"/home/joshua/lgs-bench/traces/pipeline_1f1b-p16-n1e6-inferred.legacy.bin","field":"gpu_id","accepted":fal` |
| txt2bin --format legacy rejects: cpu 256 exceeds the 8-bit cpu field | failure | 1 | pass | `txt2bin: Legacy cpu exceeds 255` |
| txt2bin --format self-describing rejects: cpu 256 exceeds the 8-bit cpu field | failure | 1 | pass | `txt2bin: Legacy cpu exceeds 255` |
| (upstream txt2bin, informational) cpu 256 exceeds the 8-bit cpu field | success | 0 | info | `` |
| txt2bin --format legacy rejects: tag 2^32 exceeds the 32-bit tag field | failure | 1 | pass | `txt2bin: Legacy tag exceeds uint32_t` |
| txt2bin --format self-describing rejects: tag 2^32 exceeds the 32-bit tag field | failure | 1 | pass | `txt2bin: Legacy tag exceeds uint32_t` |
| (upstream txt2bin, informational) tag 2^32 exceeds the 32-bit tag field | success | 0 | info | `` |
| txt2bin --format legacy rejects: peer outside num_ranks | failure | 1 | pass | `txt2bin: Peer is outside num_ranks` |
| txt2bin --format self-describing rejects: peer outside num_ranks | failure | 1 | pass | `txt2bin: Peer is outside num_ranks` |
| (upstream txt2bin, informational) peer outside num_ranks | success | 0 | info | `` |
| txt2bin --format self-describing rejects: custom unsigned value 2^64 exceeds uint64 | failure | 1 | pass | `txt2bin: Invalid unsigned value for 'gpu_id'` |
| txt2bin --format self-describing rejects: negative value for an unsigned custom field | failure | 1 | pass | `txt2bin: Invalid unsigned value for 'gpu_id'` |
| txt2bin --format self-describing rejects: declared custom field missing on an operation | failure | 1 | pass | `txt2bin: Missing custom field 'gpu_id' on operation 'l1'` |
| NEW rejects a truncated self-describing trace | failure | 1 | pass | `LogGOPSim: Truncated byte buffer` |
| NEW rejects an unknown format | failure | 1 | pass | `LogGOPSim: Unrecognized schedule format` |
| BASE --save-mem run on a copy succeeds | success | 0 | pass | `The schedule will be invalid after this simulation! LogGP network backend; size: 16 (1 CPUs, 1 NICs); L=2500, o=1500 g=1000, G=6, O=0, P=16, S=65535 PERFORMANCE` |
| BASE: a trace consumed by --save-mem is rejected afterwards | failure | 1 | pass | `This is serialized goal schedule was invalidated by a prior simulation run!` |
| NEW --save-mem run on a copy succeeds | success | 0 | pass | `The schedule will be invalid after this simulation! LogGP network backend; size: 16 (1 CPUs, 1 NICs); L=2500, o=1500 g=1000, G=6, O=0, P=16, S=65535 PERFORMANCE` |
| NEW: a trace consumed by --save-mem is rejected afterwards | failure | 1 | pass | `LogGOPSim: Schedule was invalidated by a prior simulation run` |
