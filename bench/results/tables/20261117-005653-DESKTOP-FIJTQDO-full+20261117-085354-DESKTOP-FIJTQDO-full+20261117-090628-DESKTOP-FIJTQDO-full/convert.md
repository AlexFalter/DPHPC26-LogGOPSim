# txt2bin conversion

| host | trace | tool | build | format | threads | n | median_ms | ci_lo_ms | ci_hi_ms | speedup_vs_base_txt2bin | input_mb_per_s | peak_rss_mb | output_bytes |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | base-legacy | base | legacy | 1 | 10 | 12,407 | 12,268 | 12,561 | 1 | 61.5 | 44.7 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | master-legacy | master | legacy | 1 | 10 | 16,653 | 16,382 | 17,091 | 0.745 | 45.9 | 47.4 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | master-sd | master | self-describing | 1 | 10 | 31,764 | 30,763 | 32,224 | 0.391 | 24 | 43 | 141535883 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-legacy-t1 | omp | legacy | 1 | 10 | 16,455 | 16,146 | 17,074 | 0.754 | 46.4 | 776 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-legacy-t16 | omp | legacy | 16 | 10 | 3,729 | 3,708 | 3,783 | 3.33 | 205 | 1,316 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-legacy-t2 | omp | legacy | 2 | 10 | 9,052 | 8,875 | 9,324 | 1.37 | 84.4 | 812 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-legacy-t4 | omp | legacy | 4 | 10 | 5,303 | 5,173 | 5,411 | 2.34 | 144 | 885 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-legacy-t8 | omp | legacy | 8 | 10 | 3,843 | 3,734 | 3,903 | 3.23 | 199 | 1,029 | 463835982 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-sd-t1 | omp | self-describing | 1 | 10 | 30,354 | 29,991 | 30,657 | 0.409 | 25.2 | 769 | 141535883 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-sd-t16 | omp | self-describing | 16 | 12 | 18,426 | 18,048 | 19,318 | 0.673 | 41.4 | 1,307 | 141535883 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-sd-t2 | omp | self-describing | 2 | 10 | 23,531 | 23,266 | 23,719 | 0.527 | 32.5 | 805 | 141535883 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-sd-t4 | omp | self-describing | 4 | 15 | 19,686 | 19,408 | 20,479 | 0.63 | 38.8 | 878 | 141535883 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e7-inferred | omp-sd-t8 | omp | self-describing | 8 | 12 | 18,588 | 18,224 | 19,376 | 0.667 | 41.1 | 1,020 | 141535883 |
