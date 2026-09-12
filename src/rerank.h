/* SPDX-License-Identifier: MIT */
/* rerank.h — userspace top-K placement (DEMOTION_USERSPACE). */

#ifndef PACT_RERANK_H
#define PACT_RERANK_H

#include "minicoro.h"
#include "pact.h"

/* Snapshot node0 capacity, turn kernel demotion off, allocate granule table. */
int rerank_init(pact_context_t *pact);

void rerank_destroy(pact_context_t *pact);

/* Coroutine: every 500ms (fixed), rank known 2MB granules by the sum of
 * page scores and enqueue demote-first then promote so the fast tier holds
 * the top-K (K from --fast-tier-frac × node0). */
void rerank_coroutine(mco_coro *co);

#endif /* PACT_RERANK_H */
