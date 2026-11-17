# Phase-timing instrumentation overhead (TASK.md 4)

| host | trace | timing_on | compared_with | on_median_ms | on_ci | other_median_ms | other_ci | ratio_on_over_other | cis_overlap |
|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | stencil3d-p64-n1e6-inferred | NEW-interp | NEW-interp-notiming | 516 | [514, 527] | 517 | [516, 521] | 0.998 | True |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e6-inferred | NEW-interp | PLAIN-interp | 516 | [514, 527] | 543 | [541, 555] | 0.95 | False |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | NEW-interp-notiming | 5,131 | [5,114, 5,165] | 5,167 | [5,125, 5,198] | 0.993 | True |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | NEW-interp | PLAIN-interp | 5,131 | [5,114, 5,165] | 5,422 | [5,394, 5,492] | 0.946 | False |
