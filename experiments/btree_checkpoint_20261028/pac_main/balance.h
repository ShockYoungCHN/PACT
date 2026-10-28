/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* balance.h — migration balance check. */

#ifndef PACT_BALANCE_H
#define PACT_BALANCE_H

#include <stdint.h>
#include "pact.h"

/* Read a sum of values from /proc/vmstat for any keys appearing in stat_names. */
uint64_t read_migration_stats(const char *stat_names);

/* Algorithm 2 balance check: reads this run's kernel demotion count from
 * /proc/vmstat and toggles /sys/kernel/mm/numa/demotion_enabled so demotion
 * stays on while N_demoted < N_promoted + demotion_margin. Called ~1 Hz from
 * the migration thread. */
void check_migration_balance(pact_context_t *pact);

#endif /* PACT_BALANCE_H */
