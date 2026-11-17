# Runs under a cgroup memory limit (swap off)

| host | trace | config | limit_gb | runs | completed | oom_killed | median_ms | majflt |
|---|---|---|---|---|---|---|---|---|
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | BASE | 2 | 3 | 3 | 0 | 54,215 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | BASE | 4 | 3 | 3 | 0 | 54,789 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | BASE | 8 | 3 | 3 | 0 | 54,377 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-interp | 2 | 3 | 3 | 0 | 50,585 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-interp | 4 | 3 | 3 | 0 | 50,569 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-interp | 8 | 3 | 3 | 0 | 50,642 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-legacyfile | 2 | 3 | 3 | 0 | 40,090 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-legacyfile | 4 | 3 | 3 | 0 | 40,389 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | LAZY-legacyfile | 8 | 3 | 3 | 0 | 40,353 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-interp | 2 | 3 | 3 | 0 | 52,694 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-interp | 4 | 3 | 3 | 0 | 52,979 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-interp | 8 | 3 | 3 | 0 | 52,620 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-legacyfile | 2 | 3 | 3 | 0 | 44,394 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-legacyfile | 4 | 3 | 3 | 0 | 45,012 | 0 |
| DESKTOP-FIJTQDO | stencil3d-p64-n1e8-inferred | NEW-legacyfile | 8 | 3 | 3 | 0 | 44,469 | 0 |
