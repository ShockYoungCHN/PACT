/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/*
 * pact_logging.c - Optional logging implementation for PACT
 *
 * This file contains the actual logging implementation, only compiled when
 * PACT_ENABLE_LOGGING is defined. When logging is disabled, this file is
 * not needed as pact_logging.h provides inline no-ops.
 */

#include "logging.h"

#ifdef PACT_ENABLE_LOGGING

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "pact.h"

/* rdtsc for timestamp */
static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/*
 * Initialize logging subsystem
 * Opens log file and writes header
 */
void init_logging(pact_context_t *pact, const char *filename, const char *format)
{
    char log_filename[256];

    /* Store format */
    strncpy(pact->log_format, format, sizeof(pact->log_format) - 1);
    pact->log_format[sizeof(pact->log_format) - 1] = '\0';

    /* Generate filename if not provided */
    if (filename && strlen(filename) > 0) {
        strncpy(log_filename, filename, sizeof(log_filename) - 1);
        log_filename[sizeof(log_filename) - 1] = '\0';
    } else {
        /* Generate log file name based on timestamp */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(log_filename, sizeof(log_filename), "pact_log_%04d%02d%02d_%02d%02d%02d.%s",
                 tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday, tm_info->tm_hour,
                 tm_info->tm_min, tm_info->tm_sec, format);
    }

    pact->log_file = fopen(log_filename, "w");
    if (!pact->log_file) {
        fprintf(stderr, "Warning: Failed to open log file %s\n", log_filename);
        pact->enable_logging = false;
        return;
    }

    /* Write header based on format */
    if (strcmp(format, "csv") == 0) {
        fprintf(pact->log_file, "timestamp,event_type,page_addr,pac_value,tier,bin_index\n");
    } else if (strcmp(format, "json") == 0) {
        fprintf(pact->log_file, "[\n");
    }

    fflush(pact->log_file);
}

/*
 * Cleanup logging subsystem
 * Closes log file and writes footer if needed
 */
void cleanup_logging(pact_context_t *pact)
{
    if (!pact->log_file) {
        return;
    }

    if (strcmp(pact->log_format, "json") == 0) {
        fprintf(pact->log_file, "\n]\n");
    }

    fclose(pact->log_file);
    pact->log_file = NULL;
}

/*
 * Unified event logging function
 * All specialized logging functions call this
 */
void log_event(pact_context_t *pact, const char *event_type, const char *function,
               const char *format, ...)
{
    if (!pact->enable_logging || !pact->log_file) {
        return;
    }

    uint64_t timestamp = rdtsc();
    if (strcmp(pact->log_format, "csv") == 0) {
        fprintf(pact->log_file, "%lu,%s,%s,", timestamp, event_type, function);
        va_list args;
        va_start(args, format);
        vfprintf(pact->log_file, format, args);
        va_end(args);
        fprintf(pact->log_file, "\n");
    }
}

/* Specialized logging functions - wrappers around log_event */

void log_pebs_sample(pact_context_t *pact, const char *function, int workload_id, uint64_t addr,
                     uint8_t tier, uint32_t attributed_stalls)
{
    log_event(pact, "pebs_sample", function, "%d,0x%lx,%d,%u", workload_id, addr, tier,
              attributed_stalls);
}

void log_coro_event(pact_context_t *pact, const char *function, const char *coro_name,
                    const char *event, uint64_t elapsed_cycles)
{
    log_event(pact, "coro_event", function, "%s,%s,%lu", coro_name, event, elapsed_cycles);
}

void log_pac_calculation(pact_context_t *pact, const char *function, uint64_t page_addr,
                         uint32_t frequency, uint64_t total_samples, uint64_t stalls,
                         uint64_t attributed_stalls, uint8_t tier, double mlp)
{
    log_event(pact, "pac_calc", function, "0x%lx,%u,%lu,%lu,%lu,%d,%.2f", page_addr, frequency,
              total_samples, stalls, attributed_stalls, tier, mlp);
}

void log_ring_buffer_op(pact_context_t *pact, const char *function, const char *op,
                        const char *buffer_name, size_t count, size_t current_size, size_t capacity)
{
    log_event(pact, "ring_buffer", function, "%s,%s,%zu,%zu,%zu", op, buffer_name, count,
              current_size, capacity);
}

void log_workload_info(pact_context_t *pact, const char *function, int workload_id, pid_t pid,
                       const char *name, const char *cores)
{
    log_event(pact, "workload_info", function, "%d,%d,%s,%s", workload_id, pid, name, cores);
}

void log_migration_stats(pact_context_t *pact, const char *function, int workload_id,
                         uint64_t new_promotions, uint64_t new_demotions,
                         uint64_t avg_promotion_pac, uint64_t avg_demotion_pac,
                         uint64_t repromotions_pact_pact, uint64_t repromotions_pact_other,
                         uint64_t repromotions_other_pact, uint64_t repromotions_other_other,
                         uint64_t redemotions_pact_pact, uint64_t redemotions_pact_other,
                         uint64_t redemotions_other_pact, uint64_t redemotions_other_other)
{
    log_event(pact, "migration_stats", function,
              "%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu", workload_id, new_promotions,
              new_demotions, avg_promotion_pac, avg_demotion_pac, repromotions_pact_pact,
              repromotions_pact_other, repromotions_other_pact, repromotions_other_other,
              redemotions_pact_pact, redemotions_pact_other, redemotions_other_pact,
              redemotions_other_other);
}

void log_pmu(pact_context_t *pact, const char *function, int workload_id, double fast_tier_mlp,
             double slow_tier_mlp, uint64_t llc_misses_fast, uint64_t llc_misses_slow,
             uint64_t fast_tier_events, uint64_t slow_tier_events)
{
    log_event(pact, "pmu_stats", function, "%d,%.2f,%.2f,%lu,%lu,%lu,%lu", workload_id,
              fast_tier_mlp, slow_tier_mlp, llc_misses_fast, llc_misses_slow, fast_tier_events,
              slow_tier_events);
}

void log_pac_update(pact_context_t *pact, const char *function, int workload_id, uint64_t page_addr,
                    uint64_t pac_value, const char *tier_str, int bin_index)
{
    log_event(pact, "pac_update", function, "%d,0x%lx,%lu,%s,%d", workload_id, page_addr, pac_value,
              tier_str, bin_index);
}

void log_migration_event(pact_context_t *pact, const char *function, int workload_id,
                         const char *operation, uint64_t page_addr, int from_tier, int to_tier)
{
    log_event(pact, "migration", function, "%d,%s,0x%lx,%d,%d", workload_id, operation, page_addr,
              from_tier, to_tier);
}

void log_pebs_aggregator(pact_context_t *pact, const char *function, int workload_id,
                         uint64_t theory_samples, uint64_t processed_samples,
                         uint64_t perf_dropped_events, uint64_t perf_dropped_samples,
                         uint64_t agg_dropped_events_agg_full,
                         uint64_t agg_dropped_events_update_full)
{
    log_event(pact, "pebs_aggregator", function, "%d,%lu,%lu,%lu,%lu,%lu,%lu", workload_id,
              theory_samples, processed_samples, perf_dropped_events, perf_dropped_samples,
              agg_dropped_events_agg_full, agg_dropped_events_update_full);
}

#endif /* PACT_ENABLE_LOGGING */
