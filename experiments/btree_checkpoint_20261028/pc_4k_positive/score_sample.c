/* SPDX-License-Identifier: MIT */
/* score_sample.c — uniform score-pool samples for CDF / tier-by-region debug.
 *
 * Default: keep 1% of hash entries via systematic sampling (step=100) with a
 * fresh random phase each dump — uniform over tracked pages, one table walk.
 * Region tag is VA range only (no IP in the PAC ring).
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "score_sample.h"

#define SCORE_REGION_NAME_MAX 64
#define SCORE_REGION_MAX 16

typedef struct {
    char name[SCORE_REGION_NAME_MAX];
    uint64_t lo;
    uint64_t hi;
} score_region_t;

typedef struct {
    uint64_t page;
    uint64_t score;
    uint32_t access_count;
    uint8_t tier;
} score_row_t;

typedef struct score_sample_state {
    FILE *fp;
    score_region_t regions[SCORE_REGION_MAX];
    int n_regions;
    double frac;          /* e.g. 0.01 */
    uint32_t sample_n_cap; /* 0 = uncapped */
    uint64_t dumps;
    fast_prng_t prng;
} score_sample_state_t;

static uint64_t prng_next(fast_prng_t *p)
{
    uint64_t x = p->state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    p->state = x ? x : 0x9e3779b97f4a7c15ULL;
    return p->state;
}

static const char *region_of(const score_sample_state_t *st, uint64_t page)
{
    for (int i = 0; i < st->n_regions; i++) {
        if (page >= st->regions[i].lo && page < st->regions[i].hi) {
            return st->regions[i].name;
        }
    }
    return "other";
}

static int load_regions(score_sample_state_t *st, const char *path)
{
    if (!path || !path[0]) {
        return 0;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        log_warning("score_sample", "cannot open regions file %s", path);
        return -1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && st->n_regions < SCORE_REGION_MAX) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        char name[SCORE_REGION_NAME_MAX];
        uint64_t lo = 0, hi = 0;
        if (sscanf(line, "%63s %" SCNx64 " %" SCNx64, name, &lo, &hi) != 3) {
            continue;
        }
        if (hi <= lo) {
            continue;
        }
        score_region_t *r = &st->regions[st->n_regions++];
        memcpy(r->name, name, sizeof(r->name) - 1);
        r->name[sizeof(r->name) - 1] = '\0';
        r->lo = lo & ~0xFFFULL;
        r->hi = (hi + 0xFFFULL) & ~0xFFFULL;
        if (r->hi <= r->lo) {
            r->hi = r->lo + 0x1000ULL;
        }
    }
    fclose(f);
    log_info("score_sample", "loaded %d VA regions from %s", st->n_regions, path);
    return 0;
}

int score_sample_init(pact_context_t *pact, const char *csv_path, const char *regions_path,
                      double frac, uint32_t sample_n_cap)
{
    if (!pact || !csv_path || !csv_path[0]) {
        return 0;
    }
    score_sample_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return -1;
    }
    if (frac <= 0.0 || frac > 1.0) {
        frac = 0.01;
    }
    st->frac = frac;
    st->sample_n_cap = sample_n_cap;
    st->prng.state = 0xc0ffeeULL ^ (uint64_t)(uintptr_t)st;
    if (load_regions(st, regions_path) < 0) {
        /* region column stays "other" */
    }
    st->fp = fopen(csv_path, "w");
    if (!st->fp) {
        log_warning("score_sample", "cannot open %s for write", csv_path);
        free(st);
        return -1;
    }
    fprintf(st->fp, "dump,elapsed_sec,page,score,tier,access_count,region\n");
    fflush(st->fp);
    pact->score_sample = st;
    log_info("score_sample", "uniform sample frac=%.4f cap=%u → %s", st->frac, st->sample_n_cap,
             csv_path);
    return 0;
}

void score_sample_destroy(pact_context_t *pact)
{
    if (!pact || !pact->score_sample) {
        return;
    }
    score_sample_state_t *st = pact->score_sample;
    if (st->fp) {
        fclose(st->fp);
    }
    free(st);
    pact->score_sample = NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

void score_sample_dump(pact_context_t *pact, double elapsed_sec)
{
    if (!pact || !pact->score_sample || !pact->workload || !pact->workload->pac_table) {
        return;
    }
    score_sample_state_t *st = pact->score_sample;
    pac_table_t *table = pact->workload->pac_table;
    uint32_t table_size = kh_size(table);
    if (table_size == 0) {
        return;
    }

    /* Systematic sample: step ≈ 1/frac, phase uniform in [0, step). */
    uint32_t step = (uint32_t)llround(1.0 / st->frac);
    if (step < 1) {
        step = 1;
    }
    uint32_t phase = (uint32_t)(prng_next(&st->prng) % step);
    uint32_t ncap = (table_size + step - 1) / step;
    if (st->sample_n_cap > 0 && ncap > st->sample_n_cap) {
        /* Widen step so we stay under the cap while remaining uniform. */
        step = (table_size + st->sample_n_cap - 1) / st->sample_n_cap;
        if (step < 1) {
            step = 1;
        }
        phase = (uint32_t)(prng_next(&st->prng) % step);
        ncap = (table_size + step - 1) / step;
        if (ncap > st->sample_n_cap) {
            ncap = st->sample_n_cap;
        }
    }

    score_row_t *rows = calloc(ncap, sizeof(*rows));
    if (!rows) {
        return;
    }

    uint32_t idx = 0;
    uint32_t filled = 0;
    for (khint_t k = 0; k != kh_end(table); k++) {
        if (!kh_exist(table, k)) {
            continue;
        }
        pac_metadata_t *meta = kh_val(table, k);
        if (!meta) {
            continue;
        }
        if ((idx % step) == phase && filled < ncap) {
            rows[filled].page = meta->page_addr;
            rows[filled].score = meta->pac_value;
            rows[filled].access_count = meta->access_count;
            rows[filled].tier = meta->tier;
            filled++;
        }
        idx++;
    }
    uint64_t seen = idx;

    if (filled == 0) {
        free(rows);
        return;
    }

    uint64_t *scores = malloc(filled * sizeof(uint64_t));
    if (!scores) {
        free(rows);
        return;
    }
    for (uint32_t i = 0; i < filled; i++) {
        scores[i] = rows[i].score;
    }
    qsort(scores, filled, sizeof(uint64_t), cmp_u64);
    uint64_t median = scores[filled / 2];

    enum { MAX_R = SCORE_REGION_MAX + 1 };
    uint32_t rn[MAX_R] = {0}, rfast[MAX_R] = {0}, r_above[MAX_R] = {0};
    uint64_t rsum[MAX_R] = {0};
    const char *rname[MAX_R];
    int nlab = st->n_regions;
    for (int i = 0; i < nlab; i++) {
        rname[i] = st->regions[i].name;
    }
    rname[nlab] = "other";
    int ntot = nlab + 1;

    st->dumps++;
    for (uint32_t i = 0; i < filled; i++) {
        const char *reg = region_of(st, rows[i].page);
        int ridx = nlab;
        for (int r = 0; r < nlab; r++) {
            if (strcmp(reg, rname[r]) == 0) {
                ridx = r;
                break;
            }
        }
        rn[ridx]++;
        rsum[ridx] += rows[i].score;
        if (rows[i].tier == 0) {
            rfast[ridx]++;
        }
        if (rows[i].score > median) {
            r_above[ridx]++;
        }
        fprintf(st->fp, "%lu,%.2f,0x%lx,%lu,%u,%u,%s\n", st->dumps, elapsed_sec, rows[i].page,
                rows[i].score, (unsigned)rows[i].tier, rows[i].access_count, reg);
    }
    fflush(st->fp);

    char buf[1024];
    size_t off = 0;
    off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                            "SCORE_SAMPLE dump=%lu n=%u/%lu (%.2f%% step=%u) median=%lu |",
                            st->dumps, filled, seen, 100.0 * (double)filled / (double)seen, step,
                            median);
    for (int i = 0; i < ntot && off + 80 < sizeof(buf); i++) {
        if (rn[i] == 0) {
            continue;
        }
        double fast_pct = 100.0 * (double)rfast[i] / (double)rn[i];
        double above_pct = 100.0 * (double)r_above[i] / (double)rn[i];
        double avg = (double)rsum[i] / (double)rn[i];
        off += (size_t)snprintf(buf + off, sizeof(buf) - off,
                                " %s:n=%u fast=%.0f%% >p50=%.0f%% avg=%.0f", rname[i], rn[i],
                                fast_pct, above_pct, avg);
    }
    log_info("score_sample", "%s", buf);

    free(scores);
    free(rows);
}
