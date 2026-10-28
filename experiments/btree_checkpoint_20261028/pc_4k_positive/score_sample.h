/* SPDX-License-Identifier: MIT */
/* score_sample.h — uniform score-pool samples for debug CDFs. */

#ifndef PACT_SCORE_SAMPLE_H
#define PACT_SCORE_SAMPLE_H

#include "pact.h"

/* Open CSV + optional VA→region map. No-op if path empty.
 * frac: fraction of table to keep (e.g. 0.01 = 1%). sample_n_cap: hard
 * ceiling (0 = none). */
int score_sample_init(pact_context_t *pact, const char *csv_path, const char *regions_path,
                      double frac, uint32_t sample_n_cap);

void score_sample_destroy(pact_context_t *pact);

/* Systematic 1/step sample with random phase; append CSV + summary log. */
void score_sample_dump(pact_context_t *pact, double elapsed_sec);

#endif /* PACT_SCORE_SAMPLE_H */
