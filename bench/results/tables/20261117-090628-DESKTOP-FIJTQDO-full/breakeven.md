# Break-even trace size N* = T_jit_cold / (t_ref - t_spec)

T_jit_cold = median jit_generate+jit_compile+jit_load+decoder_lookup of NEW-spec-cold runs; t_* = median decode ns/record (warm cache, simulator fields) on the largest micro trace. 'end-to-end' rows: median(NEW-interp)/median(NEW-spec-cold) per size, crossing 1 at the measured N*.

| host | order | trace | records | T_jit_cold_ms | t_interp_ns | t_spec_ns | t_legacy_ns | t_upstream_ns | t_tnode_ns | N_star_vs_interp | N_star_vs_legacy | N_star_vs_upstream | N_star_bytes_legacy_equiv | N_star_bytes_sd | e2e_ratio_interp_over_specCold |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | end-to-end | stencil3d-p64 size sweep | – | – | – | – | – | – | – | None | – | – | – | – |  |
