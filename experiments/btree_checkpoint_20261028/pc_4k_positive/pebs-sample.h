/* SPDX-License-Identifier: MIT */
/* Shared perf sample ABI: producer attributes and consumer layout must agree. */
#ifndef PACT_PEBS_SAMPLE_H
#define PACT_PEBS_SAMPLE_H

#include <stdint.h>
#include <linux/perf_event.h>

#define PACT_PEBS_SAMPLE_TYPE (PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_TIME | \
                              PERF_SAMPLE_ADDR | PERF_SAMPLE_ID | PERF_SAMPLE_CPU | \
                              PERF_SAMPLE_PERIOD)

/* PERF_RECORD_SAMPLE order is specified by perf_event_open, not by the
 * order in which the PERF_SAMPLE_* flags are ORed together. */
typedef struct {
    struct perf_event_header header;
    uint64_t ip;
    uint32_t pid;
    uint32_t tid;
    uint64_t sample_time_ns;
    uint64_t addr;
    uint64_t event_id;
    uint32_t cpu;
    uint32_t reserved;
    uint64_t period;
} pact_pebs_record_t;

_Static_assert(sizeof(pact_pebs_record_t) == 64, "Unexpected PEBS sample ABI layout");

#endif
