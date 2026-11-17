#!/bin/bash
# Benchmark campaign as one Slurm job on the ETH D-INFK student cluster (bench/README.md, "Cluster").
# Submit with bench/cluster/submit.sh (sets account, node type, cores, memory, config); directly:
#   sbatch -A <course-tag> --gpus gb10:1 --cpus-per-task 20 --mem 110G --time 23:50:00 \
#          --export=ALL,CONFIG=cluster-gb10 bench/cluster/job.sbatch
#
# Everything heavy happens on the node's local SSD ($TMPDIR, deleted when the job ends): the
# toolchain-independent build (natively for the node's architecture), the traces and the scratch
# files. Only the campaign results (bench/results/raw/<campaign>, a few MB) and logs are written to
# the repository checkout. If Slurm requeues the job, it rebuilds, regenerates the (deterministic)
# traces and resumes the same campaign.
#SBATCH --job-name=lgs-bench
#SBATCH --output=lgs-bench-%j.out
#SBATCH --requeue
set -euo pipefail
. /etc/profile.d/modules.sh 2>/dev/null || true

REPO="${REPO:-${SLURM_SUBMIT_DIR:-$(pwd)}}"
cd "$REPO"
CONFIG="${CONFIG:-cluster-gb10}"
BUDGET_HOURS="${BUDGET_HOURS:-}"
LOCAL="${TMPDIR:-/tmp}/lgs-bench-${SLURM_JOB_ID:-$$}"
mkdir -p "$LOCAL"
export BENCH_WORK="$LOCAL/work" BENCH_SCRATCH="$LOCAL/scratch"
export BENCH_RESULTS="${BENCH_RESULTS:-$REPO/bench/results}"
export JOBS="${SLURM_CPUS_PER_TASK:-${SLURM_CPUS_ON_NODE:-$(nproc)}}"
[ -d "${BENCH_ENV:-$HOME/.lgs-bench-env}/$(uname -m)" ] && source bench/env/activate.sh

echo "== $(date -Is) job ${SLURM_JOB_ID:-local} on $(hostname) ($(uname -m)), config $CONFIG, $JOBS cpus, restart ${SLURM_RESTART_COUNT:-0}"
echo "   local disk: $(df -h "$LOCAL" | tail -1)"
echo "   cgroup memory limit: $(cat /sys/fs/cgroup$(awk -F: '$1=="0"{print $3}' /proc/self/cgroup)/memory.max 2>/dev/null || echo unknown)"
nproc; free -g | head -2; lscpu | grep -E 'Model name|^CPU\(s\)|NUMA node\(s\)' || true

# Same campaign after a requeue: remembered per job id in the results directory.
STATE="$BENCH_RESULTS/.campaign-${SLURM_JOB_ID:-local}"
RESUME=()
if [ -f "$STATE" ] && [ -d "$(cat "$STATE")" ]; then RESUME=(--resume "$(cat "$STATE")"); echo "resuming $(cat "$STATE")"; fi

BUDGET_ARGS=()
[ -n "$BUDGET_HOURS" ] && BUDGET_ARGS=(--budget-hours "$BUDGET_HOURS")
bash bench/run_all.sh "$CONFIG" ${BUDGET_ARGS[@]+"${BUDGET_ARGS[@]}"} ${RESUME[@]+"${RESUME[@]}"} 2>&1 | tee "$LOCAL/run_all.log" &
PID=$!
# Record the campaign directory as soon as run.py has created it (for --resume after a requeue).
for _ in $(seq 1 7200); do
  CAMP="$(sed -n 's/^campaign //p' "$LOCAL/run_all.log" 2>/dev/null | head -1)"
  if [ -n "$CAMP" ]; then echo "$CAMP" > "$STATE"; break; fi
  kill -0 "$PID" 2>/dev/null || break
  sleep 5
done
STATUS=0
wait "$PID" || STATUS=$?
CAMP="$(sed -n 's/^campaign //p' "$LOCAL/run_all.log" | head -1)"
if [ -n "$CAMP" ] && [ -d "$CAMP" ]; then
  cp "$LOCAL/run_all.log" "$CAMP/run_all.log"
  cp "$BENCH_WORK/traces/manifest.json" "$CAMP/trace-manifest.json" 2>/dev/null || true
  cp "$BENCH_WORK"/build-*.log "$CAMP/" 2>/dev/null || true
  cp "$BENCH_WORK/build/flags-check.json" "$CAMP/" 2>/dev/null || true
fi
rm -f "$STATE"
echo "== $(date -Is) finished with status $STATUS; results in ${CAMP:-?}"
exit $STATUS
