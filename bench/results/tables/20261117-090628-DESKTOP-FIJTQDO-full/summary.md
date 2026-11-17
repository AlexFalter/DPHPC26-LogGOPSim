# End-to-end summary (TASK.md 7, table 7)

Wall time of the whole LogGOPSim process. speedup_vs_base = median(BASE) / median(config) on the same trace (BASE reads the equivalent legacy file), CI by percentile bootstrap. correctness = gate result for this (trace, config).

| host | experiment | workload | records | ranks | schema | cache | config | n | median_ms | ci_lo_ms | ci_hi_ms | ci_halfwidth_pct | base_median_ms | speedup_vs_base | speedup_ci_lo | speedup_ci_hi | events_per_s | loop_events_per_s | peak_rss_mb | peak_anon_mb | majflt | noisy_runs | correctness | matches_reference |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | workloads | allreduce_rd | 1000000 | 64 | inferred | warm | BASE | 10 | 324 | 322 | 329 | 1.14 | – | – | – | – | 4,059,417 | – | 46 | 36.6 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_rd | 1000000 | 64 | inferred | warm | LAZY-interp | 10 | 302 | 300 | 306 | 0.973 | 324 | 1.07 | 1.06 | 1.08 | 4,355,049 | 4,676,046 | 35.1 | 4.09 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_rd | 1000000 | 64 | inferred | warm | NEW-interp | 14 | 310 | 298 | 325 | 4.32 | 324 | 1.05 | 1.02 | 1.08 | 4,247,289 | 4,560,974 | 35.1 | 7.94 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_rd | 1000000 | 64 | inferred | warm | NEW-legacyfile | 15 | 245 | 242 | 250 | 1.72 | 324 | 1.32 | 1.3 | 1.34 | 5,374,630 | 5,652,488 | 46.1 | 35.8 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_rd | 1000000 | 64 | inferred | warm | NEW-spec-warm | 12 | 266 | 263 | 267 | 0.822 | 324 | 1.22 | 1.21 | 1.23 | 4,948,776 | 5,378,438 | 35.1 | 7.61 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_ring | 1000000 | 64 | inferred | warm | BASE | 10 | 346 | 344 | 348 | 0.678 | – | – | – | – | 4,009,034 | – | 45.1 | 36.1 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_ring | 1000000 | 64 | inferred | warm | LAZY-interp | 10 | 326 | 323 | 330 | 1.11 | 346 | 1.06 | 1.05 | 1.07 | 4,254,176 | 4,544,528 | 35.1 | 4.06 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_ring | 1000000 | 64 | inferred | warm | NEW-interp | 15 | 324 | 321 | 336 | 2.28 | 346 | 1.07 | 1.03 | 1.08 | 4,276,939 | 4,554,217 | 35.1 | 8.68 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_ring | 1000000 | 64 | inferred | warm | NEW-legacyfile | 15 | 268 | 266 | 275 | 1.51 | 346 | 1.29 | 1.27 | 1.3 | 5,168,747 | 5,413,641 | 45.4 | 35.4 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | allreduce_ring | 1000000 | 64 | inferred | warm | NEW-spec-warm | 10 | 288 | 286 | 299 | 2.34 | 346 | 1.2 | 1.17 | 1.21 | 4,806,080 | 5,149,076 | 35.1 | 8.48 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | alltoall | 1000000 | 64 | inferred | warm | BASE | 10 | 2,800 | 2,778 | 2,838 | 1.07 | – | – | – | – | 534,134 | – | 49.9 | 38.9 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | alltoall | 1000000 | 64 | inferred | warm | LAZY-interp | 10 | 2,894 | 2,856 | 2,945 | 1.54 | 2,800 | 0.968 | 0.954 | 0.984 | 516,904 | 521,969 | 35.1 | 4.8 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | alltoall | 1000000 | 64 | inferred | warm | NEW-interp | 10 | 2,907 | 2,843 | 2,964 | 2.09 | 2,800 | 0.963 | 0.943 | 0.986 | 514,451 | 519,166 | 35.1 | 11.3 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | alltoall | 1000000 | 64 | inferred | warm | NEW-legacyfile | 10 | 2,806 | 2,785 | 2,879 | 1.68 | 2,800 | 0.998 | 0.983 | 1.01 | 533,087 | 535,565 | 50 | 38.8 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | alltoall | 1000000 | 64 | inferred | warm | NEW-spec-warm | 10 | 2,845 | 2,829 | 2,876 | 0.82 | 2,800 | 0.984 | 0.974 | 0.996 | 525,691 | 530,555 | 35.1 | 11.3 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | dp_nccl | 1000000 | 16 | inferred | warm | BASE | 10 | 633 | 628 | 663 | 2.83 | – | – | – | – | 2,156,875 | – | 45.4 | 37 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | dp_nccl | 1000000 | 16 | inferred | warm | LAZY-interp | 12 | 645 | 640 | 651 | 0.873 | 633 | 0.981 | 0.969 | 1.02 | 2,116,530 | 2,181,518 | 35.1 | 4.09 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | dp_nccl | 1000000 | 16 | inferred | warm | NEW-interp | 10 | 640 | 634 | 659 | 1.89 | 633 | 0.989 | 0.964 | 1.03 | 2,133,143 | 2,192,737 | 35.1 | 9.63 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | dp_nccl | 1000000 | 16 | inferred | warm | NEW-legacyfile | 10 | 574 | 562 | 589 | 2.39 | 633 | 1.1 | 1.07 | 1.15 | 2,380,693 | 2,428,470 | 45.5 | 36.8 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | dp_nccl | 1000000 | 16 | inferred | warm | NEW-spec-warm | 10 | 606 | 601 | 633 | 2.66 | 633 | 1.04 | 1.01 | 1.09 | 2,253,769 | 2,323,540 | 35.1 | 9.55 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | moe_alltoall | 1000000 | 32 | inferred | warm | BASE | 10 | 1,507 | 1,499 | 1,542 | 1.42 | – | – | – | – | 995,345 | – | 49.1 | 37.7 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | moe_alltoall | 1000000 | 32 | inferred | warm | LAZY-interp | 10 | 1,478 | 1,472 | 1,497 | 0.836 | 1,507 | 1.02 | 1.01 | 1.04 | 1,014,755 | 1,034,336 | 35.1 | 4.35 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | moe_alltoall | 1000000 | 32 | inferred | warm | NEW-interp | 10 | 1,520 | 1,495 | 1,543 | 1.59 | 1,507 | 0.991 | 0.978 | 1.01 | 986,562 | 1,004,622 | 35.1 | 10.1 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | moe_alltoall | 1000000 | 32 | inferred | warm | NEW-legacyfile | 10 | 1,435 | 1,422 | 1,455 | 1.14 | 1,507 | 1.05 | 1.04 | 1.07 | 1,045,400 | 1,055,427 | 49.3 | 37.7 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | moe_alltoall | 1000000 | 32 | inferred | warm | NEW-spec-warm | 10 | 1,460 | 1,455 | 1,477 | 0.763 | 1,507 | 1.03 | 1.02 | 1.05 | 1,027,078 | 1,046,148 | 35.1 | 10.2 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | pipeline_1f1b | 1000000 | 16 | inferred | warm | BASE | 10 | 294 | 290 | 302 | 2.01 | – | – | – | – | 4,319,770 | – | 43 | 34.4 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | pipeline_1f1b | 1000000 | 16 | inferred | warm | LAZY-interp | 10 | 299 | 294 | 303 | 1.54 | 294 | 0.983 | 0.968 | 1.01 | 4,248,305 | 4,504,064 | 35.1 | 3.96 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | pipeline_1f1b | 1000000 | 16 | inferred | warm | NEW-interp | 12 | 301 | 295 | 310 | 2.51 | 294 | 0.976 | 0.945 | 1.01 | 4,215,217 | 4,449,768 | 35.1 | 8.19 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | pipeline_1f1b | 1000000 | 16 | inferred | warm | NEW-legacyfile | 10 | 235 | 233 | 241 | 1.75 | 294 | 1.25 | 1.23 | 1.28 | 5,401,725 | 5,684,420 | 43.3 | 35.2 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | pipeline_1f1b | 1000000 | 16 | inferred | warm | NEW-spec-warm | 10 | 267 | 261 | 277 | 2.95 | 294 | 1.1 | 1.07 | 1.14 | 4,763,216 | 5,084,966 | 35.1 | 8.11 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | stencil3d | 1000000 | 64 | inferred | warm | BASE | 10 | 549 | 540 | 564 | 2.25 | – | – | – | – | 2,662,197 | – | 48.2 | 36.9 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | stencil3d | 1000000 | 64 | inferred | warm | LAZY-interp | 10 | 516 | 509 | 527 | 1.73 | 549 | 1.06 | 1.04 | 1.09 | 2,832,589 | 2,984,578 | 35.1 | 4.19 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | stencil3d | 1000000 | 64 | inferred | warm | NEW-interp | 10 | 527 | 525 | 543 | 1.73 | 549 | 1.04 | 1.02 | 1.06 | 2,772,280 | 2,911,429 | 35.1 | 8.87 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | stencil3d | 1000000 | 64 | inferred | warm | NEW-legacyfile | 10 | 448 | 441 | 465 | 2.66 | 549 | 1.23 | 1.18 | 1.26 | 3,264,572 | 3,352,646 | 48.4 | 37.3 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads | stencil3d | 1000000 | 64 | inferred | warm | NEW-spec-warm | 12 | 494 | 491 | 515 | 2.46 | 549 | 1.11 | 1.07 | 1.13 | 2,960,190 | 3,114,353 | 35.1 | 8.79 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | allreduce_ring | 10000000 | 256 | inferred | warm | BASE | 10 | 4,276 | 4,218 | 4,323 | 1.22 | – | – | – | – | 3,314,393 | – | 427 | 376 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | allreduce_ring | 10000000 | 256 | inferred | warm | LAZY-interp | 10 | 3,536 | 3,483 | 3,598 | 1.63 | 4,276 | 1.21 | 1.19 | 1.23 | 4,008,937 | 4,149,807 | 163 | 39.4 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | allreduce_ring | 10000000 | 256 | inferred | warm | NEW-interp | 10 | 3,733 | 3,714 | 3,804 | 1.21 | 4,276 | 1.15 | 1.13 | 1.16 | 3,797,272 | 3,907,859 | 124 | 97.5 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | allreduce_ring | 10000000 | 256 | inferred | warm | NEW-spec-warm | 10 | 3,444 | 3,369 | 3,513 | 2.09 | 4,276 | 1.24 | 1.22 | 1.26 | 4,115,585 | 4,252,611 | 124 | 97.4 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | dp_nccl | 10000000 | 64 | inferred | warm | BASE | 10 | 8,227 | 8,135 | 8,346 | 1.28 | – | – | – | – | 1,697,426 | – | 423 | 373 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | dp_nccl | 10000000 | 64 | inferred | warm | LAZY-interp | 10 | 7,650 | 7,588 | 7,752 | 1.07 | 8,227 | 1.08 | 1.06 | 1.09 | 1,825,364 | 1,858,741 | 174 | 38.9 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | dp_nccl | 10000000 | 64 | inferred | warm | NEW-interp | 10 | 7,693 | 7,637 | 7,719 | 0.53 | 8,227 | 1.07 | 1.06 | 1.08 | 1,815,131 | 1,845,452 | 135 | 106 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | dp_nccl | 10000000 | 64 | inferred | warm | NEW-spec-warm | 10 | 7,260 | 7,199 | 7,334 | 0.929 | 8,227 | 1.13 | 1.12 | 1.15 | 1,923,488 | 1,956,947 | 135 | 106 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | pipeline_1f1b | 10000000 | 64 | inferred | warm | BASE | 10 | 2,954 | 2,894 | 3,010 | 1.96 | – | – | – | – | 4,461,580 | – | 410 | 367 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | pipeline_1f1b | 10000000 | 64 | inferred | warm | LAZY-interp | 10 | 3,053 | 3,010 | 3,097 | 1.43 | 2,954 | 0.968 | 0.95 | 0.986 | 4,317,054 | 4,489,169 | 157 | 38.1 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | pipeline_1f1b | 10000000 | 64 | inferred | warm | NEW-interp | 10 | 3,009 | 2,991 | 3,077 | 1.42 | 2,954 | 0.982 | 0.956 | 0.997 | 4,379,977 | 4,532,130 | 120 | 94.4 | 0 | 0 | pass | True |
| DESKTOP-FIJTQDO | workloads_large | pipeline_1f1b | 10000000 | 64 | inferred | warm | NEW-spec-warm | 12 | 2,610 | 2,597 | 2,630 | 0.625 | 2,954 | 1.13 | 1.11 | 1.15 | 5,050,198 | 5,257,433 | 120 | 94.3 | 0 | 0 | pass | True |
