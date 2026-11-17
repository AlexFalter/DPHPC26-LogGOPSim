# Concurrent simulations of one trace

| host | trace | config | k | n | makespan_ms | ci_lo_ms | ci_hi_ms | simulations_per_s | parallel_efficiency | sum_peak_anon_mb | sum_maxrss_mb |
|---|---|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | BASE | 1 | 10 | 5,340 | 5,304 | 5,384 | 0.187 | 1 | 372 | 446 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | BASE | 2 | 10 | 5,696 | 5,660 | 5,735 | 0.351 | 0.938 | 743 | 893 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | BASE | 4 | 10 | 6,116 | 5,977 | 6,180 | 0.654 | 0.873 | 1,487 | 1,785 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | BASE | 8 | 10 | 7,128 | 7,038 | 7,172 | 1.12 | 0.749 | 2,975 | 3,571 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | LAZY-interp | 1 | 10 | 5,047 | 5,032 | 5,081 | 0.198 | 1 | 38.6 | 177 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | LAZY-interp | 2 | 10 | 5,385 | 5,243 | 5,551 | 0.371 | 0.937 | 77.3 | 355 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | LAZY-interp | 4 | 10 | 5,735 | 5,643 | 5,802 | 0.698 | 0.88 | 155 | 710 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | LAZY-interp | 8 | 10 | 6,552 | 6,511 | 6,621 | 1.22 | 0.77 | 309 | 1,420 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | 1 | 10 | 5,225 | 5,162 | 5,269 | 0.191 | 1 | 95.8 | 139 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | 2 | 10 | 5,506 | 5,429 | 5,591 | 0.363 | 0.949 | 191 | 278 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | 4 | 10 | 5,991 | 5,901 | 6,052 | 0.668 | 0.872 | 383 | 557 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | 8 | 10 | 6,839 | 6,778 | 7,016 | 1.17 | 0.764 | 766 | 1,113 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-spec-warm | 1 | 10 | 4,852 | 4,803 | 4,940 | 0.206 | 1 | 95.7 | 139 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-spec-warm | 2 | 10 | 5,223 | 5,098 | 5,285 | 0.383 | 0.929 | 191 | 279 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-spec-warm | 4 | 10 | 5,591 | 5,516 | 5,685 | 0.715 | 0.868 | 383 | 558 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-spec-warm | 8 | 10 | 6,366 | 6,298 | 6,478 | 1.26 | 0.762 | 766 | 1,116 |
