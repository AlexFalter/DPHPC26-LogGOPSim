#!/usr/bin/env bash
# Submits a benchmark campaign to the ETH D-INFK student cluster (bench/README.md, "Cluster").
#
# usage: bench/cluster/submit.sh ACCOUNT NODE [CONFIG] [extra sbatch arguments]
#   ACCOUNT  your course/project tag (log in and run `courses` to see it)
#   NODE     gb10    GB10 (aarch64, 20 cores, 112 GiB, 850 GiB /tmp, 24 h)   default config cluster-gb10
#            2080ti  RTX 2080 Ti node (Xeon Gold 6240, 4 cores, 36 GiB, 40 GiB /tmp) config cluster-x86
#            default whatever the scheduler picks (2 cores, 24 GiB, 40 GiB /tmp)  config cluster-x86
#   CONFIG   quick | standard | full | cluster-gb10 | cluster-x86 | path (default per NODE above)
# Only one job runs at a time per user (two may queue): submit gb10 and 2080ti, they run one after
# the other. Run from the repository root on a login node (do not run benchmarks there).
set -euo pipefail
[ $# -ge 2 ] || { sed -n '2,15p' "$0"; exit 2; }
ACCOUNT="$1"; NODE="$2"; shift 2
CONFIG=""
if [ $# -gt 0 ] && [ "${1#-}" = "$1" ]; then CONFIG="$1"; shift; fi
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
case "$NODE" in
  gb10)    ARGS=(--gpus gb10:1 --cpus-per-task 20 --mem 110G --time 23:55:00); CONFIG="${CONFIG:-cluster-gb10}"; BUDGET=21 ;;
  2080ti)  ARGS=(--gpus 2080ti:1 --cpus-per-task 4 --mem 34G --time 24:00:00);  CONFIG="${CONFIG:-cluster-x86}"; BUDGET=21 ;;
  default) ARGS=(--cpus-per-task 2 --mem 22G --time 24:00:00);                  CONFIG="${CONFIG:-cluster-x86}"; BUDGET=21 ;;
  *) echo "NODE must be gb10, 2080ti or default" >&2; exit 2 ;;
esac
[ "$CONFIG" = quick ] && BUDGET=1
cd "$REPO"
mkdir -p bench/results/slurm
sbatch -A "$ACCOUNT" "${ARGS[@]}" --chdir "$REPO" --output "bench/results/slurm/lgs-bench-$NODE-%j.out" \
       --export=ALL,CONFIG="$CONFIG",BUDGET_HOURS="${BUDGET_HOURS:-$BUDGET}",REPO="$REPO" "$@" bench/cluster/job.sbatch
echo "monitor with: squeue -u \$USER; tail -f bench/results/slurm/lgs-bench-$NODE-<jobid>.out"
