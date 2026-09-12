/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* usage.c — CLI usage/help printer. */

#include <stdio.h>

#include "usage.h"
#include "build-info.h"

void pact_print_version(void)
{
    printf("pact %s (built %s)\n", PACT_GIT_VERSION, PACT_BUILD_DATE);
}

void pact_print_usage(const char *prog_name)
{
    printf("PACT: A Criticality-First Design for Tiered Memory\n");
    printf("A userspace runtime that places pages by *performance criticality*, "
           "not just access\nfrequency, across a fast (local DRAM) and a slow "
           "(CXL/remote) memory tier.\n");

    printf("\n");
    printf("How it works (per ~20 ms cycle, all in user space):\n");
    printf("  1. SAMPLE   PEBS records which pages the workload loads from the "
           "slow tier, and\n");
    printf("              CHA/uncore counters measure memory-level parallelism "
           "(MLP).\n");
    printf("  2. SCORE    Each page gets a PAC (Per-page Access Criticality) value, "
           "~ stalled cycles\n");
    printf("              attributable to it (LLC misses weighted by tier latency / "
           "MLP). High\n");
    printf("              PAC = stalling the CPU, regardless of how often it is "
           "touched.\n");
    printf("  3. BIN      Pages are grouped into PAC bins (Freedman-Diaconis width, "
           "self-tuning).\n");
    printf("  4. MIGRATE  The highest-criticality bins are promoted to the fast "
           "tier; cold pages\n");
    printf("              fall back to the slow tier. Repeat.\n");
    printf("\nSteps 1-3 run as coroutines on a single thread, plus a migration "
           "thread, all on\none dedicated core — PACT manages the whole workload "
           "at a cost of one core.\n");

    printf("\nUsage: %s --workload PID [options]\n", prog_name);
    printf("PACT attaches to an already-running, externally CPU-pinned workload "
           "and must run\nas root (PMU/PEBS access). See run/run-pact.sh for the "
           "full experiment harness.\n");

    printf("\nRequired:\n");
    printf("  --workload PID                    Target process to manage "
           "(pin it externally, e.g. taskset).\n");

    printf("\nPAC + migration tuning:\n");
    printf("  --pebs-period N                   PEBS sample period: 1 sample per N "
           "events (default 400).\n");
    printf("  --max-migrations-per-cycle N      Max 4K pages per numa_move_pages batch "
           "(promote or demote; default 4096).\n");
    printf("  --bin-count N                     Number of PAC bins; the top bin is "
           "promoted\n");
    printf("                                    (default 20; unused for placement under "
           "userspace demotion).\n");
    printf("  --bin-width W                     Initial PAC bin width; self-tunes at "
           "runtime (default 1000.0).\n");
    printf("  --cooling-alpha A                 PAC EWMA decay in [0,1]; 1.0 "
           "disables cooling (default 1.0).\n");
    printf("  --cooling-trigger-samples N       Samples before cooling kicks in "
           "(default 200000).\n");
    printf("  --pac-pool-max N                  Max tracked pages "
           "(default 6291456 ≈20GB of 4K; 0 = that default).\n");
    printf("  --demotion-margin M               Kernel only (Algorithm 2): keep demotion "
           "on while\n");
    printf("                                    demoted < promoted + M pages; larger M = "
           "more headroom\n");
    printf("                                    (default 0). Incompatible with "
           "--demotion-policy userspace.\n");
    printf("  --demotion-policy P               kernel = LRU + Algorithm 2 (default);\n");
    printf("                                    userspace = rerank top-K by score "
           "(2MB granules, 500ms,\n");
    printf("                                    1GB/epoch migrate cap); "
           "off = no demotion.\n");
    printf("  --fast-tier-frac F                Userspace only: keep top F of node0 "
           "by score (default 0.90).\n");
    printf("                                    Not env PACT_PC_TARGET_FRAC (PEBS θ).\n");
    printf("  --class-weights PATH              Calibrated class weights JSON "
           "(with --pc-class-map).\n");
    printf("  --pc-class-map PATH               Offline PC offset→class map "
           "(with --class-weights).\n");
    printf("                                    Both required to enable PC-class "
           "PAC scaling; map keys\n");
    printf("                                    are main-binary file offsets "
           "(PIE-safe).\n");
    printf("  --score-mode MODE                 Page-criticality scoring policy "
           "(fair A/B):\n");
    printf("                                    MODE=pac|freq|pc|pac+pc (see docs).\n");
    printf("                                    pac    = PACT PAC only (baseline;\n");
    printf("                                             default if no PC files /\n");
    printf("                                             explicit --score-mode)\n");
    printf("                                    freq   = sampled remote-miss "
           "frequency (score = 1 per sample)\n");
    printf("                                    pc     = pure PC-class "
           "(score = w_c; needs both files)\n");
    printf("                                    pac+pc = PAC x w_c "
           "(default when both PC files given\n");
    printf("                                             and --score-mode omitted)\n");
    printf("  --score-sample PATH               Debug: append uniform page-score sample CSV "
           "each stats tick.\n");
    printf("  --score-regions PATH              Debug: name lo_hex hi_hex VA bands for "
           "CSV region column.\n");
    printf("  --score-sample-frac F             Debug: keep fraction F of table "
           "(default 0.01 = 1%%, systematic).\n");
    printf("  --score-sample-n N                Debug: optional hard cap on rows/dump "
           "(0 = none).\n");

    printf("\nTiming (milliseconds):\n");
    printf("  --sampling-interval MS            Sampling cadence (default 20).\n");
    printf("  --adaptive-interval MS            Bin re-tuning cadence (default 20).\n");
    printf("  --stats-interval MS               Stats dump cadence (default 5000).\n");

    printf("\nCPU affinity:\n");
    printf("  --monitor-cpu CPU                 Pin the PACT event loop "
           "(-1 = none, default -1).\n");
    printf("  --migration-cpu CPU               Pin the migration thread "
           "(-1 = none, default -1).\n");

    printf("\nDiagnostics:\n");
    printf("  --crash-marker PATH               Write an ok/crash marker file "
           "on exit.\n");
    printf("  -l, --logging [FORMAT]            Enable logging (csv | json); "
           "requires a `make logging` build.\n");
    printf("  --log-file PATH                   Log file path.\n");
    printf("  --log-level LEVEL                 0=error 1=warn 2=info 3=trace "
           "(default 2).\n");
    printf("  -h, --help                        Show this help and exit.\n");
    printf("  -V, --version                     Print version and exit.\n");

    printf("\nExample:\n");
    printf("  sudo numactl -C 1 %s --workload $(pgrep bc) "
           "--max-migrations-per-cycle 4096\n",
           prog_name);
}
