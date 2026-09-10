/* SPDX-License-Identifier: MIT */
/* census.h — userspace top-K placement (DEMOTION_USERSPACE). */

#ifndef PACT_CENSUS_H
#define PACT_CENSUS_H

#include "minicoro.h"
#include "pact.h"

/* Snapshot node0 capacity, turn kernel demotion off, allocate granule table. */
int census_init(pact_context_t *pact);

void census_destroy(pact_context_t *pact);

/* Coroutine: every 500ms (fixed), rank known 2MB granules by the sum of
 * page scores and enqueue demote-first then promote so the fast tier holds
 * the top-K (K from --fast-tier-frac × node0). */
void census_coroutine(mco_coro *co);

#endif /* PACT_CENSUS_H */
