# Break-even trace size N* = T_jit_cold / (t_ref - t_spec)

T_jit_cold = median jit_generate+jit_compile+jit_load+decoder_lookup of NEW-spec-cold runs; t_* = median decode ns/record (warm cache, simulator fields) on the largest micro trace. 'end-to-end' rows: median(NEW-interp)/median(NEW-spec-cold) per size, crossing 1 at the measured N*.

| host | order | trace | records | T_jit_cold_ms | t_interp_ns | t_spec_ns | t_legacy_ns | t_upstream_ns | t_tnode_ns | N_star_vs_interp | N_star_vs_legacy | N_star_vs_upstream | N_star_bytes_legacy_equiv | N_star_bytes_sd | e2e_ratio_interp_over_specCold |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | seq | stencil3d-p64-n1e8-inferred | 99999744 | 241 | 94.4 | 56.5 | 41.5 | 41.6 | 78.8 | 6,360,375 | never (spec not faster per record) | never (spec not faster per record) | 248,054,630 | 69,964,126 | – |
| DESKTOP-FIJTQDO | topo | stencil3d-p64-n1e8-inferred | 99999744 | 241 | 89.8 | 59.1 | 44.5 | 48.8 | 77 | 7,845,182 | never (spec not faster per record) | never (spec not faster per record) | 305,962,113 | 86,297,006 | – |
| DESKTOP-FIJTQDO | end-to-end | stencil3d-p64 size sweep | – | – | – | – | – | – | – | 7,981,642 | – | – | – | – | 1e+04:0.054; 1e+05:0.198; 1e+06:0.715; 2e+06:0.848; 5e+06:0.965; 1e+07:1.017; 2e+07:1.046; 5e+07:1.061; 1e+08:1.068 |
