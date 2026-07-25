#!/bin/bash
#
# Run this once before run-pact.sh, or let run-pact.sh call it automatically
# via:  run_setup_config=true ./run-pact.sh <workload>

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR" || exit 1
source ./cxl-global.sh || { echo "Error: cannot source ./cxl-global.sh" >&2; exit 1; }

# Uncore frequency args (kHz): node0-min node0-max node1-min node1-max
# Default: fast tier (node 0) pinned high, slow tier (node 1) pinned low.
# Paper/artifact: 2000/2000 + 500/500. On this Silver 4114, MLC --latency_matrix
# under that lock measures ~91/~192 ns (paper targets ~90/~190).
UNCORE_ARGS="${UNCORE_ARGS:-2000000 2000000 500000 500000}"

echo "=== [1/4] Setting uncore frequency: $UNCORE_ARGS ==="
./modify-uncore-freq.sh $UNCORE_ARGS

echo "=== [2/4] Configuring CXL experiment environment (check_cxl_conf) ==="
check_cxl_conf

echo "=== [3/4] Flushing filesystem caches ==="
flush_fs_caches

# PACT opens PEBS + CHA/uncore PMU counters and PEBS-samples the workload's
# (another uid's) accesses. perf_event_paranoid must be permissive or PACT
# collects zero PEBS samples (no per-page criticality, no migration) even when
# running as root. -1 allows full access; use 0 if your policy forbids -1.
echo "=== [4/4] Lowering perf_event_paranoid for PEBS/PMU access ==="
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid >/dev/null
echo "    perf_event_paranoid = $(cat /proc/sys/kernel/perf_event_paranoid)"

echo "=== prepare_environment.sh DONE ==="
