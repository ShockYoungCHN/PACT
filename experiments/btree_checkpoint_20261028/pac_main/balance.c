/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* balance.c — demotion balance check (Algorithm 2).
 *
 * Controls /sys/kernel/mm/numa/demotion_enabled from the migration thread
 * at ~1Hz: kernel LRU demotion stays enabled while
 * N_demoted < N_promoted + m, where m is the demotion aggressiveness
 * (--demotion-margin, default 0). Demotions are counted from the
 * system-wide /proc/vmstat pgdemote counters relative to a baseline taken
 * on the first check, so both sides of the inequality count this run's
 * pages.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "error.h"
#include "pact.h"
#include "balance.h"

uint64_t read_migration_stats(const char *stat_names)
{
    FILE *fp = fopen("/proc/vmstat", "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    char word[100];
    uint64_t sum = 0;
    uint64_t value = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%99s %ld", word, &value) == 2) {
            if (strstr(stat_names, word)) {
                sum += value;
            }
        }
    }

    fclose(fp);
    return sum;
}

/* Write "1"/"0" to /sys/kernel/mm/numa/demotion_enabled. */
static void write_demotion_enabled(const char *value)
{
    int fd = open("/sys/kernel/mm/numa/demotion_enabled", O_WRONLY);
    if (fd < 0) {
        log_warning("write_demotion_enabled", "Failed to open: %s", strerror(errno));
        return;
    }
    ssize_t n = write(fd, value, strlen(value));
    if (n < 0) {
        log_warning("write_demotion_enabled", "Failed to write '%s': %s", value, strerror(errno));
    }
    close(fd);
}

void check_migration_balance(pact_context_t *pact)
{
    if (pact->demotion_policy != DEMOTION_KERNEL_LRU) {
        return;
    }

    /* pgdemote counters are cumulative since boot; pact_init snapshots the
     * startup value into demotion_baseline so this run's demotions count
     * from time zero (the first balance check runs ~1s in, and demotions
     * during that first second must not be lost). */
    uint64_t demoted = read_migration_stats("pgdemote_kswapd pgdemote_direct");
    uint64_t n_demoted =
        (demoted >= pact->demotion_baseline) ? demoted - pact->demotion_baseline : 0;
    uint64_t new_demotions = demoted - pact->workload->stats.last_demotions_successes;
    pact->workload->stats.last_demotions_successes = demoted;
    pact->workload->stats.new_demotions = new_demotions;
    pact->workload->stats.demotion_successes = n_demoted;

    uint64_t n_promoted = pact->workload->stats.promotion_successes;
    /* Algorithm 2: demote while N_demoted < N_promoted + m. Saturate the
     * threshold so a huge margin cannot wrap it back to a small number. */
    uint64_t threshold = n_promoted + pact->demotion_margin;
    if (threshold < n_promoted) {
        threshold = UINT64_MAX;
    }
    const char *value = (n_demoted < threshold) ? "1" : "0";

    log_debug("check_migration_balance",
              "demoted=%lu, promoted=%lu, margin=%lu, demotion_enabled=%s", n_demoted, n_promoted,
              pact->demotion_margin, value);

    write_demotion_enabled(value);
}
