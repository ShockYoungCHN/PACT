/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* stats-coro.c — periodic stats coroutine.
 *
 * Yields every `stats_interval_ms` to log rates + PAC_DIST / rerank lines.
 * Full print_stats() runs at exit (stats.c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "minicoro.h"
#include "pact.h"
#include "score_sample.h"
#include "stats.h"
#include "stats-coro.h"
#include "tsc.h"
#include "pebs-aggregator.h"

static uint32_t total_hash_entries(pact_context_t *ctx)
{
    return kh_size(ctx->workload->pac_table);
}

static void log_workload_summary(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    log_info("stats_coroutine", "  WL (%s): hash=%u, Q1=%.1f, Q3=%.1f, bw=%.1f, pebs_samples=%lu",
             wl->name, kh_size(wl->pac_table), wl->binning->q1, wl->binning->q3,
             wl->binning->bin_width, wl->total_pebs_samples);
}

/* PAC histogram for the workload: min/avg/max + per-tier averages + count
 * over threshold. Samples up to ~10K entries from large tables. */
static void log_one_workload_pac_dist(pact_workload_t *wl)
{
    pac_table_t *table = wl->pac_table;
    uint32_t table_size = kh_size(table);
    if (table_size == 0) {
        return;
    }

    uint64_t threshold = (uint64_t)(wl->binning->bin_width * (wl->binning->bin_count - 1));
    uint64_t pac_min = UINT64_MAX, pac_max = 0, pac_sum = 0;
    uint64_t pac_fast_sum = 0, pac_slow_sum = 0;
    uint32_t n_total = 0, n_fast = 0, n_slow = 0, n_above_threshold = 0;
    uint32_t step = (table_size > 10000) ? table_size / 10000 : 1;
    uint32_t count = 0;

    for (khint_t k = 0; k != kh_end(table); k++) {
        if (!kh_exist(table, k)) {
            continue;
        }
        count++;
        if (step > 1 && (count % step) != 0) {
            continue;
        }
        pac_metadata_t *meta = kh_val(table, k);
        if (!meta) {
            continue;
        }
        uint64_t pv = meta->pac_value;
        n_total++;
        if (pv < pac_min) {
            pac_min = pv;
        }
        if (pv > pac_max) {
            pac_max = pv;
        }
        pac_sum += pv;
        if (meta->tier == 0) {
            n_fast++;
            pac_fast_sum += pv;
        } else {
            n_slow++;
            pac_slow_sum += pv;
        }
        if (pv >= threshold) {
            n_above_threshold++;
        }
    }

    if (n_total == 0) {
        return;
    }
    log_info("stats_coroutine",
             "  WL PAC_DIST: n=%u min=%lu avg=%lu max=%lu threshold=%lu above_thresh=%u "
             "fast(n=%u avg=%lu) slow(n=%u avg=%lu)",
             n_total, pac_min, pac_sum / n_total, pac_max, threshold, n_above_threshold, n_fast,
             n_fast > 0 ? pac_fast_sum / n_fast : 0, n_slow,
             n_slow > 0 ? pac_slow_sum / n_slow : 0);
}

static void log_pac_telemetry(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    log_info("stats_coroutine",
             "  WL PAC_INPUTS: llc_fast=%lu llc_slow=%lu mlp_fast=%.2f mlp_slow=%.2f "
             "k_dram=%lu k_cxl=%lu pebs_period=%u",
             wl->stats.llc_misses_fast, wl->stats.llc_misses_slow, wl->workload_mlp_fast,
             wl->workload_mlp_slow, ctx->k_constant_dram, ctx->k_constant_cxl,
             ctx->pebs_sampling_period);
    log_one_workload_pac_dist(wl);
}

/* pc PEBS capacity-aware θ: set threshold to the (1 - pc_target_frac)
 * percentile of the reservoir (top-frac by score). Independent of rerank
 * --fast-tier-frac. Written here each stats interval; aggregator reads it. */
static int pc_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void pc_threshold_feedback(pact_context_t *ctx)
{
    binning_state_t *bin = ctx->workload->binning;
    if (ctx->score_mode != SCORE_MODE_PC || !bin || bin->pc_target_frac <= 0.0) {
        return;
    }
    reservoir_t *r = ctx->workload->reservoir;
    if (!r || r->count < 8) {
        return;
    }
    /* θ = (1 - target_frac) percentile of the score distribution = the top-frac-by-score
     * cutoff. Stable: recomputed directly from the distribution each interval (no feedback
     * oscillation). Chase scores highest, so top-frac ≈ chase → chase-first fill. */
    double frac = bin->pc_target_frac > 0.99 ? 0.99 : bin->pc_target_frac;
    double *work = malloc(r->count * sizeof(double));
    if (!work) {
        return;
    }
    memcpy(work, r->samples, r->count * sizeof(double));
    qsort(work, r->count, sizeof(double), pc_cmp_double);
    size_t idx = (size_t)((1.0 - frac) * (double)(r->count - 1));
    bin->pc_threshold = work[idx];
    free(work);
    log_info("pc_threshold_feedback", "target_frac=%.3f theta=%.3f (p%.0f of %zu samples)", frac,
             bin->pc_threshold, (1.0 - frac) * 100.0, r->count);
}

void stats_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    pact_stats_t last_stats = ctx->workload->stats;
    uint64_t last_tsc = ctx->start_tsc;

    while (ctx->running) {
        uint64_t now = rdtsc();
        double elapsed_sec = (double)(now - last_tsc) / ctx->tsc_freq_hz;
        pact_stats_t *st = &ctx->workload->stats;

        uint64_t events_delta = st->pebs_events_processed - last_stats.pebs_events_processed;
        uint64_t promotions_delta = st->promotion_successes - last_stats.promotion_successes;
        uint64_t demotions_delta;
        if (ctx->demotion_policy == DEMOTION_USERSPACE) {
            demotions_delta =
                (uint64_t)st->pact_demotions - (uint64_t)last_stats.pact_demotions;
        } else {
            demotions_delta = st->demotion_successes - last_stats.demotion_successes;
        }

        log_info("stats_coroutine",
                 "Time passed %.2fs, Events: %lu/sec, Pages promoted: %lu/sec, "
                 "Pages demoted: %lu/sec, Hash entries: %u",
                 elapsed_sec, (uint64_t)(events_delta / elapsed_sec),
                 (uint64_t)(promotions_delta / elapsed_sec),
                 (uint64_t)(demotions_delta / elapsed_sec), total_hash_entries(ctx));

        log_info("stats_coroutine", "  PAC: pool_alloc_skip=%lu", st->pool_alloc_skipped);
        if (ctx->demotion_policy == DEMOTION_USERSPACE) {
            log_info("stats_coroutine",
                     "  Rerank: epochs=%lu tracked=%lu K=%lu enq_demote=%lu enq_promote=%lu "
                     "pact_demotions=%lu",
                     st->rerank_epochs, st->rerank_tracked, st->rerank_k,
                     st->rerank_enqueued_demote, st->rerank_enqueued_promote,
                     (uint64_t)st->pact_demotions);
        }
        log_workload_summary(ctx);
        log_pac_telemetry(ctx);
        pc_threshold_feedback(ctx); /* pc PEBS capacity θ from PACT_PC_TARGET_FRAC */
        score_sample_dump(ctx, (double)(now - ctx->start_tsc) / ctx->tsc_freq_hz);

        last_stats = *st;
        last_tsc = now;
        mco_yield(co);
    }
}
