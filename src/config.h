/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* config.h — pact_config_t (CLI/init time configuration).
 *
 * pact_config_t holds startup-time configuration. It is populated by
 * pact_parse_command_line_args + pact_init_config defaults, then handed to
 * initialize_pact_context which copies relevant fields into pact_context_t.
 *
 * Naming: pact_config_t is the "request"; pact_context_t is the "state".
 */

#ifndef PACT_CONFIG_H
#define PACT_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "pact.h" /* demotion_policy_t */

/* Default config values exposed for validation + init. */
#define PACT_PEBS_SAMPLE_PERIOD 400U
#define PACT_DEFAULT_SAMPLING_INTERVAL_MS 20U
#define PACT_DEFAULT_COOLING_INTERVAL_MS 5000U
#define PACT_DEFAULT_ADAPTIVE_INTERVAL_MS 20U
#define PACT_DEFAULT_STATS_INTERVAL_MS 5000U
#define PACT_DEFAULT_CENSUS_INTERVAL_MS 500U
#define PACT_DEFAULT_FAST_TIER_FRAC 0.90
#define PACT_DEFAULT_GRANULE_BYTES (2UL * 1024 * 1024)
#define PACT_DEFAULT_CENSUS_MIGRATE_LIMIT 262144U
/* Cap on pages promoted per 20 ms cycle. A low cap throttles promotion so
 * PACT converges too slowly to relieve slow-tier pressure; 4096 keeps the
 * critical set moving without overwhelming the migration thread. Override
 * with --max-migrations-per-cycle. */
#define PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE 4096U
#define PACT_INITIAL_BIN_WIDTH 1000.0
#define PACT_INITIAL_BIN_COUNT 20U
/* ~20GB of 4K pages (5.24M) + headroom; ~768MB meta at 128B/entry. */
#define PACT_DEFAULT_PAC_POOL_MAX (6UL * 1024 * 1024)

typedef struct pact_config {
    demotion_policy_t demotion_policy;
    uint32_t sampling_interval_ms;
    uint32_t cooling_interval_ms;
    uint32_t adaptive_interval_ms;
    uint32_t stats_interval_ms;
    uint32_t census_interval_ms;
    uint32_t max_migrations_per_cycle;
    double fast_tier_frac;
    uint64_t granule_bytes;
    uint32_t census_migrate_limit;
    uint64_t demotion_margin; /* Algorithm 2 m (kernel demotion only; default 0) */
    uint64_t pebs_period;
    uint32_t bin_count;
    double bin_width;
    double cooling_alpha;             /* α ∈ [0, 1]; 1.0 = disabled (default) */
    uint64_t cooling_trigger_samples; /* trigger threshold (default 200K) */
    bool enable_logging;
    char log_format[16];
    char log_file[256];

    /* Workload identification. target_pid is required (CLI errors out if
     * absent); sentinel value <= 0 in the config struct only at parse
     * time. */
    pid_t target_pid;
    char workload_name[64];

    /* CPU affinity */
    int monitor_cpu;
    int migration_cpu;

    /* PAC metadata pool cap */
    size_t pac_pool_max;

    /* Optional PC-class scaling (both paths required to enable). */
    char class_weights_path[512];
    char pc_class_map_path[512];

    /* Optional score-pool random sample CSV (debug CDF / tier-by-region). */
    char score_sample_path[512];
    char score_regions_path[512];
    double score_sample_frac;   /* default 0.01 = uniform 1% */
    uint32_t score_sample_n;    /* optional hard cap; 0 = none */

    /* Scoring policy. SCORE_MODE_AUTO (default) resolves to pac+pc when PC
     * files are supplied, else pac. Override with
     * --score-mode {pac|freq|pc|pac+pc}. */
    score_mode_t score_mode;
} pact_config_t;

/* Initialize all fields of `config` to documented defaults. */
void pact_init_config(pact_config_t *config);

#endif /* PACT_CONFIG_H */
