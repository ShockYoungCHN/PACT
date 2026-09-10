#!/bin/bash

# Run a single workload with PACT memory tiering.

set -euo pipefail

# --- Argument Parsing ---
WORKLOAD="${1:-}"
skip_setup="${2:-}"

if [ -z "$WORKLOAD" ]; then
    echo "Usage: $0 <workload_name> [skip_setup]"
    exit 1
fi

# --- Source Workload Definitions ---
source ./workloads.sh || exit 1

# --- Load Workload Variables ---
pname_var="${WORKLOAD}_pname"
workload_cmd_var="${WORKLOAD}_workload_cmd"
vmtouch_file_var="${WORKLOAD}_vmtouch_file"
omp_threads_var="${WORKLOAD}_omp_threads"

pname="${!pname_var:-}"
workload_cmd="${!workload_cmd_var:-}"
vmtouch_file="${!vmtouch_file_var:-}"
omp_threads="${!omp_threads_var:-}"

if [ -z "$pname" ] || [ -z "$workload_cmd" ] || [ -z "$omp_threads" ]; then
    echo "Error: Workload '$WORKLOAD' not defined completely in workloads.sh"
    exit 1
fi

# --- CPU Core Allocation ---
if [ "$omp_threads" -gt 8 ]; then
    echo "Error: Workload needs $omp_threads threads but only 8 cores available (2-9)"
    exit 1
fi

AVAILABLE_CORES=(2 3 4 5 6 7 8 9)
cores=()
for ((i = 0; i < omp_threads; i++)); do
    cores+=("${AVAILABLE_CORES[i]}")
done
cpus=$(IFS=,; echo "${cores[*]}")

# --- PACT Configuration (override via env, defaults match config.h) ---
VMTOUCH="${VMTOUCH:-vmtouch}"
PACT="${PACT:-../src/pact}"
pebs_period="${pebs_period:-400}"
migration_limit="${migration_limit:-4096}"
bin_count="${bin_count:-20}"
bin_width="${bin_width:-1000.0}"
# Optional PC-class scaling (both required). Map must be built from the same
# workload binary that will run (PIE offsets). Example:
#   pc_class_weights=/users/samuraiy/pc_driven/results/class_weights.json \
#   pc_class_map=/users/samuraiy/pc_driven/results/gapbs/bc-urand/pc_class.map \
#   ./run-pact.sh bc_urand_8t
pc_class_weights="${pc_class_weights:-}"
pc_class_map="${pc_class_map:-}"
# Scoring policy for a fair A/B: pac (baseline) | freq (sampled remote-miss
# frequency) | pc (pure PC-class) | pac+pc.
# Empty = PACT default (pac+pc if PC files given, else pac).
score_mode="${score_mode:-}"
# kernel (default) | userspace (census + top-K) | off
demotion_policy="${demotion_policy:-}"
# Algorithm 2 m; only with demotion_policy=kernel (userspace rejects this flag)
demotion_margin="${demotion_margin:-}"
fast_tier_frac="${fast_tier_frac:-}"
pac_pool_max="${pac_pool_max:-}"
score_sample="${score_sample:-}"
score_regions="${score_regions:-}"
score_sample_frac="${score_sample_frac:-}"
score_sample_n="${score_sample_n:-}"

# --- Tier placement (paper methodology: first-touch, PACT is the placer) ---
# The workload's memory is allocated FIRST-TOUCH (default NUMA policy): we
# CPU-pin it but do NOT bind its memory to any node. The paper's allocation
# policy is first-touch for application transparency (PACT decides placement
# online, the application does nothing). PACT samples accesses and promotes
# critical pages to the fast tier.
#
# Demotion depends on demotion_policy → --demotion-policy:
#   kernel     — Algorithm 2 toggles demotion_enabled (--demotion-margin)
#   userspace  — census top-K (2MB / 500ms / 1GB/epoch; tune K via fast_tier_frac)
#   off        — no demotion (except optional PEBS θ if PACT_PC_TARGET_FRAC>0)
#
# The fast/slow SPLIT comes from a physically small fast tier, NOT from
# binding the workload's memory. Boot the machine with node 0's usable DRAM
# reduced to the desired fast-tier size via a node-0 memmap= kernel parameter,
# so first-touch fills the fast tier and the kernel then places the overflow
# on the slow tier.
#
# Do NOT use a cgroup memory.high/memory.max cap to emulate a small fast
# tier: a memcg limit caps TOTAL usage (not fast-tier residency), and with
# demotion disabled the kernel has no way to shrink an anonymous working
# set, so the workload throttles in unreclaimable D-state during its
# allocation phase (reported as a hang on graph loading, issue #4).

# --- Environment Setup Flag ---
# When "true", run full machine preparation (uncore frequency pinning + CXL
# config via prepare_environment.sh) before the run. Default is "false" so a
# plain run does NOT touch machine-wide config (you are expected to have run
# setup/env/prepare_environment.sh yourself).
# Override via env:  run_setup_config=true ./run-pact.sh <workload>
run_setup_config="${run_setup_config:-false}"

# --- Transparent Huge Pages toggle ---
# Default "false" leaves the current THP policy
# untouched.  Override via env:  enable_thp=true ./run-pact.sh <workload>
enable_thp="${enable_thp:-false}"

# Save the pre-run THP policy so clean_up can restore it (the /sys file reports
# the choices with the active one in [brackets], e.g. "always [madvise] never").
# When enable_thp=true we switch it to "always" for the run and put the prior
# policy back on exit, rather than leaving THP forced off.
prev_thp=""
prev_thp_defrag=""
if [ "$enable_thp" = "true" ]; then
    thp_line=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true)
    prev_thp=$(printf '%s\n' "$thp_line" | grep -oE '\[[a-z]+\]' | tr -d '[]' || true)
    thp_defrag_line=$(cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true)
    prev_thp_defrag=$(printf '%s\n' "$thp_defrag_line" | grep -oE '\[[a-z]+\]' | tr -d '[]' || true)
fi

# --- Enable PACT runtime (set enable_pact=false for NoTier baseline) ---
enable_pact="${enable_pact:-true}"

# --- Output Directory ---
# Fixed per-workload path. The monitor logs below (vmstat.txt, numastat.log)
# are appended to during the run, so truncate them up front - otherwise a
# re-run interleaves its samples with the previous run's.
# Optional results_tag overrides the leaf dir name (e.g. results_tag=pact_pc).
results_tag="${results_tag:-}"
if [ -n "$results_tag" ]; then
    OUTDIR="./results/${WORKLOAD}/${results_tag}"
elif [ "$enable_pact" = "true" ]; then
    OUTDIR="./results/${WORKLOAD}/pact"
else
    OUTDIR="./results/${WORKLOAD}/notier"
fi
mkdir -p "$OUTDIR"
OUTDIR=$(realpath "$OUTDIR")
: >"$OUTDIR/vmstat.txt"
: >"$OUTDIR/numastat.log"

if [ "$enable_pact" = "true" ]; then
    echo "=== PACT Run: $WORKLOAD ==="
else
    echo "=== NoTier Run: $WORKLOAD ==="
fi
echo "  CPUs: $cpus ($omp_threads threads)"
if [ "$enable_pact" = "true" ]; then
    echo "  PEBS period: $pebs_period"
    echo "  Migration limit: $migration_limit"
    echo "  Bin count: $bin_count, Bin width: $bin_width"
    if [ -n "$pc_class_weights" ] || [ -n "$pc_class_map" ]; then
        echo "  PC-class weights: ${pc_class_weights:-<unset>}"
        echo "  PC-class map: ${pc_class_map:-<unset>}"
    fi
fi
echo "  Output: $OUTDIR"
echo ""

# --- Cleanup Function ---
clean_up() {
    echo ""
    echo "Cleaning up..."

    if [ -n "${WORKLOAD_PID:-}" ]; then
        echo "  Killing workload PID $WORKLOAD_PID"
        sudo pkill -TERM -P "$WORKLOAD_PID" 2>/dev/null || true
        kill -TERM "$WORKLOAD_PID" 2>/dev/null || true
        sleep 1
        kill -KILL "$WORKLOAD_PID" 2>/dev/null || true
    fi

    if [ -n "${PACT_PID:-}" ]; then
        echo "  Killing PACT PID $PACT_PID"
        # PACT runs under sudo. Kill only the exact background PID; avoid
        # SIGINT and process-group signaling because the background command
        # shares the harness's process group.
        sudo kill -KILL "$PACT_PID" 2>/dev/null || kill -KILL "$PACT_PID" 2>/dev/null || true
    fi

    # Reap the vmstat/numastat monitor loops. Kill each subshell AND its
    # current child (the in-flight numastat/grep), otherwise that child keeps
    # running and holds the script's stdout open, so a piped reader never sees
    # EOF and the run appears to hang after the workload has finished.
    for mon in "${pid_vmstat:-}" "${pid_numastat:-}"; do
        [ -n "$mon" ] || continue
        pkill -P "$mon" 2>/dev/null || true
        kill "$mon" 2>/dev/null || true
    done

    echo "  Disabling demotion_enabled..."
    echo 0 | sudo tee /sys/kernel/mm/numa/demotion_enabled >/dev/null 2>&1 || true

    if [ "$enable_thp" = "true" ]; then
        # Restore the THP policy captured before the run (fall back to "never"
        # only if it could not be read).
        echo "  Restoring transparent huge pages policy (enabled=${prev_thp:-never}, defrag=${prev_thp_defrag:-never})"
        echo "${prev_thp:-never}" | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null 2>&1 || true
        echo "${prev_thp_defrag:-never}" | sudo tee /sys/kernel/mm/transparent_hugepage/defrag >/dev/null 2>&1 || true
    fi

    echo "Cleanup complete"
}
trap clean_up EXIT INT TERM

# --- PACT kernel modules ---
# tierinit sets up NUMA tiering; kswapdrst keeps kswapd from backing off under
# demotion pressure.  Both are out-of-tree modules under setup/kernel/.
# If a module is already loaded we keep it; if its .ko is built we load it;
# otherwise we abort and tell the user to build it.
ensure_kernel_module() {
    local modname=$1 moddir=$2 ko=$3
    # No `grep -q` here: under `set -o pipefail` an early -q exit can kill
    # lsmod with SIGPIPE and turn "module already loaded" into rc=141.
    if lsmod | grep -E "^${modname}[[:space:]]" >/dev/null; then
        echo "  module '$modname' already loaded"
        return 0
    fi
    if [ -f "$ko" ]; then
        echo "  loading module '$modname' ($ko)"
        sudo insmod "$ko" || { echo "  ERROR: failed to load $ko"; exit 1; }
    else
        echo "  ERROR: kernel module '$modname' is not loaded and not built ($ko missing)."
        echo "         Build the PACT kernel modules first:"
        echo "             make -C $moddir"
        echo "         See setup/kernel/README.md for details."
        exit 1
    fi
}

# --- System Setup ---
if [ "$run_setup_config" = "true" ]; then
    # Full machine prep: uncore freq + check_cxl_conf + flush (cold start).
    echo "=== Running full environment setup (prepare_environment.sh) ==="
    ../setup/env/prepare_environment.sh
elif [ "$skip_setup" != "skip_setup" ]; then
    echo "=== Flushing caches ==="
    echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
    free
fi

echo "=== Loading PACT kernel modules ==="
ensure_kernel_module tierinit  ../setup/kernel/tierinit  ../setup/kernel/tierinit/tierinit.ko
ensure_kernel_module kswapdrst ../setup/kernel/kswapdrst ../setup/kernel/kswapdrst/kswapdrst.ko

echo "=== Kernel setup ==="
echo 0 | sudo tee /sys/kernel/mm/numa/demotion_enabled >/dev/null
echo 0 | sudo tee /proc/sys/kernel/numa_balancing >/dev/null
echo "  demotion_enabled=0, numa_balancing=0"

# --- Phase 1: vmtouch (preload data to slow tier) ---
if [ "$skip_setup" != "skip_setup" ] && [ -n "$vmtouch_file" ]; then
    echo "=== Phase 1: Loading $vmtouch_file into slow tier ==="
    numactl --membind 1 "$VMTOUCH" -f -t "$vmtouch_file" -m 64G
    sleep 20
fi

# --- Phase 2: Start Monitoring ---
echo "=== Phase 2: Starting monitoring ==="

cat /proc/vmstat >"$OUTDIR/before_vmstat.log"

touch "$OUTDIR/vmstat.txt"
(
trap - EXIT INT TERM
while true; do
    ts=$(date +%s)
    echo "$ts" >>"$OUTDIR/vmstat.txt"
    grep -E "pgdemote|pgpromote|pgmigrate|thp_migration|numa_pte_updates" /proc/vmstat >>"$OUTDIR/vmstat.txt"
    sleep 1
done) &
pid_vmstat=$!

# --- Phase 3: Launch Workload ---
echo "=== Phase 3: Launching workload ==="

if [ "$enable_thp" = "true" ]; then
    echo "  Enabling transparent huge pages (THP policy = always)"
    echo always | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null
    echo always | sudo tee /sys/kernel/mm/transparent_hugepage/defrag >/dev/null
fi

# CPU-pin the workload but deliberately do NOT bind its memory: PACT is the
# page placer, and the paper's allocation policy is first-touch. Memory
# lands on whichever tier first-touch fills (the fast tier, sized physically;
# overflow to slow), and PACT promotes the critical pages. Binding memory
# here would defeat the experiment. workload_cmd references $numactl_args
# (see workloads.sh), so the CPU pinning applies to the workload binary even
# when the command cd's first.
numactl_args="numactl -C ${cpus} --"
echo "  Executing: ${workload_cmd}"
echo "    (numactl_args = ${numactl_args})"

OMP_NUM_THREADS=${omp_threads} eval "${workload_cmd}" >"$OUTDIR/workload.output" 2>&1 &
WORKLOAD_PID=$!
sleep 2

if ! ps -p "$WORKLOAD_PID" >/dev/null 2>&1; then
    echo "Error: Workload (PID $WORKLOAD_PID) exited immediately"
    cat "$OUTDIR/workload.output"
    exit 1
fi

# When workload_cmd uses "cd ... && numactl -- binary", $! is the launching
# shell, not the workload binary. Resolve the real target by its process name
# ($pname, defined in workloads.sh) within this launch's process tree, so we
# don't latch onto the wrong child (a plain "first child" guess is racy for
# multi-process or wrapper-launched workloads).
shell_pid="$WORKLOAD_PID"
target_pid=""
# Match on the kernel comm name, which is truncated to 15 chars (TASK_COMM_LEN),
# so compare against the first 15 chars of $pname.
pname_comm="${pname:0:15}"
# Prefer a pname match anywhere under the launching shell's subtree.
for cand in $(pgrep -P "$shell_pid" 2>/dev/null) $shell_pid; do
    for p in $(pgrep -P "$cand" 2>/dev/null) "$cand"; do
        if [ "$(ps -p "$p" -o comm= 2>/dev/null)" = "$pname_comm" ]; then
            target_pid="$p"
            break 2
        fi
    done
done
# Fall back to the first child if no pname match (e.g. comm name differs).
# (Plain '[ -z x ] && y=...' would return nonzero when target_pid is already
# set and trip 'set -e', so use an explicit if.)
if [ -z "$target_pid" ]; then
    target_pid=$(pgrep -P "$shell_pid" 2>/dev/null | head -1 || true)
fi
if [ -n "$target_pid" ] && [ "$target_pid" != "$shell_pid" ]; then
    echo "  Shell PID: $shell_pid, workload PID: $target_pid ($(ps -p "$target_pid" -o comm= 2>/dev/null))"
    WORKLOAD_PID="$target_pid"
fi

echo "  Workload PID: $WORKLOAD_PID"

# Start numastat monitoring now that we have the PID
touch "$OUTDIR/numastat.log"
(
trap - EXIT INT TERM
while true; do
    ts=$(date +%s)
    numastat="$(numastat -p "$WORKLOAD_PID" 2>/dev/null | tail -n1 | awk '{print $2 "," $3}')"
    echo "$ts, $numastat" >>"$OUTDIR/numastat.log"
    sleep 1
done) &
pid_numastat=$!

# --- Phase 4: Start PACT (skipped for NoTier) ---
start_time=$(date +%s)

if [ "$enable_pact" = "true" ]; then
    echo "=== Phase 4: Starting PACT ==="

    # PACT needs root (euid 0) to open the CHA/uncore PMU and PEBS counters
    # (validate_hardware_access() aborts otherwise). Launch it under sudo; the
    # machine is assumed to allow passwordless sudo (CloudLab does).
    #
    # No core pinning by default: PACT's CPU-affinity knobs (--monitor-cpu,
    # --migration-cpu) default to -1 (unpinned), so the OS schedules the
    # coroutine event loop and the migration thread. Pin them explicitly
    # (e.g. --monitor-cpu 1 --migration-cpu 1) to reserve a dedicated core.
    # --log-level 2 = info (0=error 1=warn 2=info 3=debug). Keeps TRACE off.
    # Optional per-page dumps (dump_pages=1). sudo strips env, so pass VAR=val
    # explicitly on the sudo command line (read by pact via getenv).
    pact_dump_env=""
    if [ "${dump_pages:-}" = "1" ]; then
        pact_dump_env="PACT_MIGRATION_EVENTS_CSV=$OUTDIR/migrations.csv \
PACT_PEBS_SAMPLES_CSV=$OUTDIR/pebs_samples.csv PACT_PEBS_SAMPLES_STRIDE=${pebs_stride:-20}"
        echo "  Page dumps ON: $OUTDIR/{migrations,pebs_samples}.csv"
    fi
    PACT_CMD="sudo PACT_PC_TARGET_FRAC=${PACT_PC_TARGET_FRAC:-${PACT_FAST_TIER_FRAC:-0}} $pact_dump_env $PACT \
        --workload $WORKLOAD_PID \
        --pebs-period $pebs_period \
        --max-migrations-per-cycle $migration_limit \
        --bin-width $bin_width \
        --bin-count $bin_count \
        --log-level ${log_level:-2}"
    if [ -n "$pc_class_weights" ] && [ -n "$pc_class_map" ]; then
        PACT_CMD="$PACT_CMD --class-weights $pc_class_weights --pc-class-map $pc_class_map"
    elif [ -n "$pc_class_weights" ] || [ -n "$pc_class_map" ]; then
        echo "  WARNING: both pc_class_weights and pc_class_map are required; ignoring PC-class flags"
    fi
    if [ -n "$score_mode" ]; then
        PACT_CMD="$PACT_CMD --score-mode $score_mode"
    fi
    if [ -n "$demotion_policy" ]; then
        PACT_CMD="$PACT_CMD --demotion-policy $demotion_policy"
    fi
    if [ -n "$demotion_margin" ]; then
        PACT_CMD="$PACT_CMD --demotion-margin $demotion_margin"
    fi
    if [ -n "$fast_tier_frac" ]; then
        PACT_CMD="$PACT_CMD --fast-tier-frac $fast_tier_frac"
    fi
    if [ -n "$pac_pool_max" ]; then
        PACT_CMD="$PACT_CMD --pac-pool-max $pac_pool_max"
    fi
    if [ -n "$score_sample" ]; then
        PACT_CMD="$PACT_CMD --score-sample $score_sample"
    fi
    if [ -n "$score_regions" ]; then
        PACT_CMD="$PACT_CMD --score-regions $score_regions"
    fi
    if [ -n "$score_sample_frac" ]; then
        PACT_CMD="$PACT_CMD --score-sample-frac $score_sample_frac"
    fi
    if [ -n "$score_sample_n" ]; then
        PACT_CMD="$PACT_CMD --score-sample-n $score_sample_n"
    fi

    echo "  Executing: $PACT_CMD"
    $PACT_CMD >"$OUTDIR/pact_debug.log" 2>&1 &
    PACT_PID=$!
    sleep 3

    if ! ps -p "$PACT_PID" >/dev/null 2>&1; then
        echo "  FAILED: PACT didn't start"
        tail -20 "$OUTDIR/pact_debug.log"
        exit 1
    fi
    echo "  PACT started (PID $PACT_PID)"
else
    echo "=== Phase 4: Skipping PACT (NoTier baseline) ==="
fi

local_mem=$(numastat -p "$WORKLOAD_PID" 2>/dev/null | tail -n1 | awk '{print $2}')
echo "  Initial memory on node 0: $local_mem MB"

# --- Phase 5: Wait for Workload ---
echo "=== Waiting for workload to complete ==="
while ps -p "$WORKLOAD_PID" >/dev/null 2>&1; do
    sleep 0.1
done

end_time=$(date +%s)
runtime=$((end_time - start_time))
echo "Workload finished. Runtime: ${runtime}s"
echo "Runtime: ${runtime}s" >>"$OUTDIR/workload.output"

# --- Cleanup ---
# Disarm the EXIT trap first so clean_up runs exactly once (not again on exit).
trap - EXIT INT TERM
clean_up

cat /proc/vmstat >"$OUTDIR/after_vmstat.log"

echo ""
echo "=== Run complete ==="
echo "  Results: $OUTDIR"
echo "  Debug log: $OUTDIR/pact_debug.log"
exit 0
