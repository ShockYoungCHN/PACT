/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */
/* constants.h — Project-wide tunable constants. Keeps the magic numbers
 * scattered across the codebase pinned to a single source-of-truth.
 *
 * Convention: only put constants here that are TUNABLES (could plausibly
 * be changed by an operator or a future CLI flag). Pure protocol/ABI
 * constants (PEBS_DECODE_* masks, struct field widths) stay in their
 * subsystem headers. */

#ifndef PACT_CONSTANTS_H
#define PACT_CONSTANTS_H

/* Default migration ring size. Power of 2. */
#define MIGRATION_RING_DEFAULT_SIZE 65536U

/* PAC update ring drain batch — number of encoded samples popped per
 * ring_buffer_uint64_pop_batch() call inside the adaptive coroutine. */
#define PAC_DRAIN_BATCH 1024

#endif /* PACT_CONSTANTS_H */
