#!/usr/bin/env bash
# Thin wrapper (TASK.md names gen_traces.sh|.py): bench/gen_traces.sh [CONFIG] [gen_traces.py options]
# CONFIG defaults to bench/configs/full.json. See bench/gen_traces.py --help.
set -euo pipefail
BENCH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="$BENCH/configs/full.json"
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then CONFIG="$1"; shift; fi
exec python3 "$BENCH/gen_traces.py" --config "$CONFIG" "$@"
