#!/usr/bin/env bash
# One command from a clean checkout: build -> traces -> correctness gate + timed runs -> analysis.
#
# usage: bench/run_all.sh [CONFIG] [--budget-hours H] [--experiments a,b] [--skip-build] [--skip-traces]
#   CONFIG: quick | standard | full | cluster-gb10 | cluster-x86 | path/to/config.json (default: quick)
#
# Environment: BENCH_WORK (builds, worktrees, traces; default bench/.work — keep it on a Linux file
# system, e.g. BENCH_WORK=~/lgs-bench under WSL), BENCH_RESULTS (default bench/results), CC/CXX/FC,
# BENCH_CFLAGS/BENCH_CXXFLAGS, JOBS (parallel build/generation jobs; default nproc).
set -euo pipefail
BENCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$BENCH/.." && pwd)"
CONFIG="quick"
RUN_ARGS=()
SKIP_BUILD=0
SKIP_TRACES=0
EXPERIMENTS=""
while [ $# -gt 0 ]; do
  case "$1" in
    --budget-hours) RUN_ARGS+=(--budget-hours "$2"); shift 2 ;;
    --experiments) EXPERIMENTS="$2"; RUN_ARGS+=(--experiments "$2"); shift 2 ;;
    --resume) RUN_ARGS+=(--resume "$2"); shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-traces) SKIP_TRACES=1; shift ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) CONFIG="$1"; shift ;;
  esac
done
case "$CONFIG" in */*|*.json|*.yaml) CFG="$CONFIG" ;; *) CFG="$BENCH/configs/$CONFIG.json" ;; esac
[ -f "$CFG" ] || { echo "no config $CFG" >&2; exit 2; }
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
[ -f "$BENCH/env/activate.sh" ] && [ -d "${BENCH_ENV:-$HOME/.lgs-bench-env}/$(uname -m)" ] && source "$BENCH/env/activate.sh"

BUILDS="$(cd "$BENCH/lib" && python3 -c "
import benchlib, grid, sys
cfg = benchlib.load_config(sys.argv[1])
only = sys.argv[2].split(',') if sys.argv[2] else None
print(','.join(grid.builds_needed(cfg, only)))" "$CFG" "$EXPERIMENTS")"
echo "config $CFG; builds: $BUILDS; BENCH_WORK=${BENCH_WORK:-$BENCH/.work}"

if [ "$SKIP_BUILD" = 0 ]; then bash "$BENCH/build.sh" --builds "$BUILDS" --jobs "$JOBS"; fi
if [ "$SKIP_TRACES" = 0 ]; then
  python3 "$BENCH/gen_traces.py" --config "$CFG" --jobs "$JOBS" ${EXPERIMENTS:+--experiments "$EXPERIMENTS"} || \
    echo "WARNING: some traces failed or were skipped (see the manifest); their cells are skipped" >&2
fi
LOG="$(mktemp)"
python3 "$BENCH/run.py" --config "$CFG" ${RUN_ARGS[@]+"${RUN_ARGS[@]}"} 2>&1 | tee "$LOG"
CAMPAIGN="$(sed -n 's/^campaign //p' "$LOG" | head -1)"
rm -f "$LOG"
[ -n "$CAMPAIGN" ] && python3 "$BENCH/analyze.py" "$CAMPAIGN"
