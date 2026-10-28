/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* config.c — pact_config_t defaults initialization.
 *
 * Companion: pact_config.h holds the type + PACT_DEFAULT_* macros.
 */

#include <stdbool.h>
#include <string.h>

#include "config.h"

void pact_init_config(pact_config_t *config)
{
    config->demotion_policy = DEMOTION_KERNEL_LRU;
    config->sampling_interval_ms = PACT_DEFAULT_SAMPLING_INTERVAL_MS;
    config->cooling_interval_ms = PACT_DEFAULT_COOLING_INTERVAL_MS;
    config->adaptive_interval_ms = PACT_DEFAULT_ADAPTIVE_INTERVAL_MS;
    config->stats_interval_ms = PACT_DEFAULT_STATS_INTERVAL_MS;
    config->max_migrations_per_cycle = PACT_DEFAULT_MAX_MIGRATIONS_PER_CYCLE;
    config->demotion_margin = 0;
    config->enable_logging = false;
    config->log_file[0] = '\0';
    strncpy(config->log_format, "csv", sizeof(config->log_format) - 1);
    config->log_format[sizeof(config->log_format) - 1] = '\0';
    config->pebs_period = PACT_PEBS_SAMPLE_PERIOD;
    config->bin_width = PACT_INITIAL_BIN_WIDTH;
    config->bin_count = PACT_INITIAL_BIN_COUNT;
    config->cooling_alpha = 1.0; /* 1.0 = no cooling (default) */
    config->cooling_trigger_samples = 200000;

    /* target_pid is required; -1 here means "not yet supplied" — CLI
     * parsing or pact_validate_configuration errors out before init. */
    config->target_pid = -1;
    config->workload_name[0] = '\0';

    /* CPU affinity (disabled by default) */
    config->monitor_cpu = -1;
    config->migration_cpu = -1;

    config->pac_pool_max = PACT_DEFAULT_PAC_POOL_MAX;
}
