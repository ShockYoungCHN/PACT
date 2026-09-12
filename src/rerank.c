/* SPDX-License-Identifier: MIT */
/* rerank.c — userspace budget enforcement: rerank + top-K + demote-first.
 *
 * PEBS only scores pages (pac_value already reflects --score-mode). This walk
 * ranks Σ page pac_value per 2MB granule, places by move_pages majority,
 * demote-first then promote. Granule score is raw sum (no log1p) so sparse
 * hits cannot dominate a fully sampled neighbor.
 */

#include <errno.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "balance.h"
#include "rerank.h"
#include "constants.h"
#include "error.h"
#include "khashl.h"
#include "minicoro.h"
#include "pact.h"
#include "pmu.h"
#include "utils.h"

#define RERANK_HYST_EPOCHS 2U
#define RERANK_NODE_SAMPLES 8U
/* Keep this much free on node0 before enqueueing promotes (pages). */
#define RERANK_PROMOTE_HEADROOM_4K ((512ULL * 1024 * 1024) / 4096ULL)
#define PM_PRESENT (1ULL << 63)

typedef struct {
    uint64_t granule;
    double score;
    int8_t node; /* 0 fast, 1 slow, -1 unknown */
    uint8_t miss_epochs;
    uint8_t in_flight;
} rerank_granule_t;

KHASHL_MAP_INIT(KH_LOCAL, rerank_table_t, rerank_table, uint64_t, rerank_granule_t *,
                kh_hash_uint64, kh_eq_generic)

struct rerank_state {
    rerank_table_t *table;
    uint64_t granule_bytes;
    uint64_t granule_mask;
    uint64_t node0_pages_4k;
    uint64_t k_granules;
    uint32_t seed_countdown;
};

typedef struct rerank_state rerank_state_t;

/* Granule score is Σ page pac_value (pc: Σ w_c·samples; pac: Σ stalls).
 * Do not take max or log1p here: one PEBS hit must not outrank a fully
 * sampled 2MB neighbor, and log1p(max) inverted C1 vs C2 under sparse PEBS. */
static double page_score(const pact_context_t *pact, const pac_metadata_t *meta)
{
    (void)pact;
    if (!meta) {
        return 0.0;
    }
    return (double)meta->pac_value;
}

static uint64_t read_node0_pages_4k(void)
{
    FILE *fp = fopen("/sys/devices/system/node/node0/meminfo", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    uint64_t kb = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "Node 0 MemTotal: %lu kB", &kb) == 1) {
            break;
        }
    }
    fclose(fp);
    return (kb * 1024ULL) / PAGE_SIZE;
}

static uint64_t read_node0_free_4k(void)
{
    FILE *fp = fopen("/sys/devices/system/node/node0/meminfo", "r");
    if (!fp) {
        return 0;
    }
    char line[256];
    uint64_t kb = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "Node 0 MemFree: %lu kB", &kb) == 1) {
            break;
        }
    }
    fclose(fp);
    return (kb * 1024ULL) / PAGE_SIZE;
}

static rerank_granule_t *rerank_get_or_add(rerank_state_t *st, uint64_t granule)
{
    int ret;
    khint_t k = rerank_table_put(st->table, granule, &ret);
    if (ret < 0 || k == kh_end(st->table)) {
        return NULL;
    }
    if (ret == 0) {
        return kh_val(st->table, k);
    }
    rerank_granule_t *g = calloc(1, sizeof(*g));
    if (!g) {
        return NULL;
    }
    g->granule = granule;
    g->node = -1;
    kh_val(st->table, k) = g;
    return g;
}

static int query_node(pid_t pid, uint64_t page)
{
    void *addr = (void *)page;
    int status = -1;
    if (numa_move_pages(pid, 1, &addr, NULL, &status, 0) < 0) {
        return -1;
    }
    return status;
}

static bool pagemap_present(int pfd, uint64_t page)
{
    uint64_t entry = 0;
    off_t off = (off_t)((page / PAGE_SIZE) * sizeof(uint64_t));
    if (pread(pfd, &entry, sizeof(entry), off) != (ssize_t)sizeof(entry)) {
        return false;
    }
    return (entry & PM_PRESENT) != 0;
}

/* Seed / refresh resident anonymous granules so first-touch node0 pages
 * that PEBS never sees still enter the ranking (score 0 → demote first). */
static void rerank_seed_resident(pact_context_t *pact, rerank_state_t *st)
{
    pid_t pid = pact->workload->target_pid;
    char maps_path[64], pm_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    snprintf(pm_path, sizeof(pm_path), "/proc/%d/pagemap", pid);

    FILE *maps = fopen(maps_path, "r");
    if (!maps) {
        return;
    }
    int pfd = open(pm_path, O_RDONLY);
    if (pfd < 0) {
        fclose(maps);
        return;
    }

    uint64_t added = 0;
    char line[1024];
    while (fgets(line, sizeof(line), maps)) {
        uint64_t start = 0, end = 0;
        char perm[8] = {0};
        char path[512] = {0};
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &start, &end, perm, path) < 3) {
            continue;
        }
        if (perm[0] != 'r' || perm[1] != 'w') {
            continue;
        }
        char *pp = path;
        while (*pp == ' ') {
            pp++;
        }
        if (pp[0] == '/') {
            continue;
        }
        if (strncmp(pp, "[vvar]", 6) == 0 || strncmp(pp, "[vdso]", 6) == 0 ||
            strncmp(pp, "[vsyscall]", 10) == 0) {
            continue;
        }

        uint64_t g0 = start & st->granule_mask;
        for (uint64_t granule = g0; granule < end; granule += st->granule_bytes) {
            uint64_t lo = granule < start ? start : granule;
            if (!pagemap_present(pfd, lo & PAGE_MASK)) {
                continue;
            }
            rerank_granule_t *g = rerank_get_or_add(st, granule);
            if (!g) {
                continue;
            }
            if (g->node < 0) {
                int node = query_node(pid, lo & PAGE_MASK);
                if (node == 0 || node == 1) {
                    g->node = (int8_t)node;
                }
                added++;
            }
        }
    }
    close(pfd);
    fclose(maps);
    if (added > 0) {
        log_info("rerank_seed", "added/refreshed %lu granules, tracked=%u", added,
                 kh_size(st->table));
    }
}

static void rerank_merge_scores(pact_context_t *pact, rerank_state_t *st)
{
    pac_table_t *table = pact->workload->pac_table;
    khint_t k;
    kh_foreach(table, k)
    {
        pac_metadata_t *meta = kh_val(table, k);
        if (!meta) {
            continue;
        }
        uint64_t granule = meta->page_addr & st->granule_mask;
        rerank_granule_t *g = rerank_get_or_add(st, granule);
        if (!g) {
            continue;
        }
        g->score += page_score(pact, meta);
        /* Placement comes from move_pages majority below, not meta->tier.
         * Under userspace, meta->tier is placement-only (updated on migrate);
         * PEBS local/remote must not overwrite it (see apply_sample_to_meta). */
    }
}

static int cmp_rank(const void *a, const void *b)
{
    const rerank_granule_t *x = *(rerank_granule_t *const *)a;
    const rerank_granule_t *y = *(rerank_granule_t *const *)b;
    if (x->score != y->score) {
        return (x->score < y->score) - (x->score > y->score); /* desc */
    }
    int xfast = (x->node == 0);
    int yfast = (y->node == 0);
    if (xfast != yfast) {
        return yfast - xfast; /* already-on-fast first */
    }
    if (x->granule < y->granule) {
        return -1;
    }
    if (x->granule > y->granule) {
        return 1;
    }
    return 0;
}

static pac_metadata_t *lookup_meta(pact_context_t *pact, uint64_t page)
{
    khint_t k = pac_table_get(pact->workload->pac_table, page);
    if (k == kh_end(pact->workload->pac_table)) {
        return NULL;
    }
    return kh_val(pact->workload->pac_table, k);
}

static uint32_t enqueue_granule(pact_context_t *pact, rerank_granule_t *g, int target,
                                uint32_t left)
{
    if (left == 0) {
        return 0;
    }
    uint64_t n4k = pact->granule_bytes / PAGE_SIZE;
    if (n4k < 1) {
        n4k = 1;
    }
    uint32_t want = (uint32_t)n4k;
    if (want > left) {
        want = left;
    }
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < want; i++) {
        uint64_t page = g->granule + (uint64_t)i * PAGE_SIZE;
        pac_metadata_t *meta = lookup_meta(pact, page);
        if (!pact_enqueue_move(pact, meta, page, target)) {
            break;
        }
        pushed++;
    }
    if (pushed > 0) {
        g->in_flight = 1;
        if (target == 0) {
            pact->workload->stats.rerank_enqueued_promote += pushed;
        } else {
            pact->workload->stats.rerank_enqueued_demote += pushed;
        }
    }
    return pushed;
}

static void rerank_refresh_nodes(pact_context_t *pact, rerank_state_t *st, mco_coro *co)
{
    pid_t pid = pact->workload->target_pid;
    uint64_t n4k = pact->granule_bytes / PAGE_SIZE;
    if (n4k < 1) {
        n4k = 1;
    }
    uint64_t step = n4k / RERANK_NODE_SAMPLES;
    if (step < 1) {
        step = 1;
    }

    uint64_t n = 0;
    khint_t k;
    kh_foreach(st->table, k)
    {
        rerank_granule_t *g = kh_val(st->table, k);
        if (!g || g->in_flight) {
            continue;
        }

        void *pages[RERANK_NODE_SAMPLES];
        int status[RERANK_NODE_SAMPLES];
        int nq = 0;
        for (uint64_t i = 0; i < n4k && nq < (int)RERANK_NODE_SAMPLES; i += step) {
            pages[nq++] = (void *)(g->granule + i * PAGE_SIZE);
        }
        memset(status, 0xff, sizeof(status));
        if (numa_move_pages(pid, nq, pages, NULL, status, 0) == 0) {
            int n0 = 0, n1 = 0;
            for (int j = 0; j < nq; j++) {
                if (status[j] == 0) {
                    n0++;
                } else if (status[j] == 1) {
                    n1++;
                }
            }
            if (n0 + n1 > 0) {
                g->node = (int8_t)(n0 >= n1 ? 0 : 1);
            }
        }

        if (co && (++n % 2048) == 0) {
            mco_yield(co);
        }
    }
}

static void rerank_enforce(pact_context_t *pact, mco_coro *co)
{
    rerank_state_t *st = pact->rerank;
    if (!st || !st->table || !pact->workload) {
        return;
    }

    ring_buffer_migration_entry_t *ring = pact->workload->migration_ring;
    if (ring && ring_buffer_migration_entry_size(ring) > (MIGRATION_RING_DEFAULT_SIZE / 4)) {
        log_debug("rerank_enforce", "ring still draining (%u); skip epoch",
                  ring_buffer_migration_entry_size(ring));
        return;
    }

    /* Zero scores then re-merge so a page that cooled / stopped sampling
     * does not keep a stale high rank. Seeded-only granules stay at 0. */
    khint_t k;
    kh_foreach(st->table, k)
    {
        rerank_granule_t *g = kh_val(st->table, k);
        if (g) {
            g->score = 0.0;
            /* One-epoch sticky: a granule enqueued last tick is eligible again. */
            g->in_flight = 0;
        }
    }

    if (st->seed_countdown == 0) {
        rerank_seed_resident(pact, st);
        st->seed_countdown = 4;
    } else {
        st->seed_countdown--;
    }

    rerank_merge_scores(pact, st);
    rerank_refresh_nodes(pact, st, co);

    uint32_t n = kh_size(st->table);
    if (n == 0) {
        return;
    }

    rerank_granule_t **rank = malloc((size_t)n * sizeof(*rank));
    if (!rank) {
        return;
    }
    uint32_t i = 0;
    kh_foreach(st->table, k)
    {
        rerank_granule_t *g = kh_val(st->table, k);
        if (g) {
            rank[i++] = g;
        }
    }
    n = i;
    qsort(rank, n, sizeof(*rank), cmp_rank);

    uint64_t budget = (uint64_t)(pact->fast_tier_frac * (double)st->node0_pages_4k);
    uint64_t pages_per = pact->granule_bytes / PAGE_SIZE;
    if (pages_per < 1) {
        pages_per = 1;
    }
    st->k_granules = budget / pages_per;
    if (st->k_granules < 1) {
        st->k_granules = 1;
    }
    if (st->k_granules > n) {
        st->k_granules = n;
    }

    for (i = 0; i < n; i++) {
        if (i < st->k_granules) {
            rank[i]->miss_epochs = 0;
        } else if (rank[i]->node == 0) {
            if (rank[i]->miss_epochs < 255) {
                rank[i]->miss_epochs++;
            }
        } else {
            rank[i]->miss_epochs = 0;
        }
    }

    uint32_t left = pact->rerank_migrate_limit;
    uint32_t ndemo = 0, npromo = 0;

    /* Demote first: free node0 before promotions try to allocate there. */
    for (i = (uint32_t)st->k_granules; i < n && left > 0; i++) {
        rerank_granule_t *g = rank[i];
        if (g->node != 0 || g->miss_epochs < RERANK_HYST_EPOCHS) {
            continue;
        }
        uint32_t got = enqueue_granule(pact, g, 1, left);
        left -= got;
        if (got) {
            ndemo++;
        }
    }
    /* Promote into MemFree headroom only (avoids stuffing a full node0).
     *
     * Do NOT require a prior demote this run: coloc2 / preferred=1 starts with
     * everything on node1, so demote never fires and the old
     * rerank_enqueued_demote>0 gate deadlocked promotions (promo_g=0 forever).
     * Within an epoch we still demote before promote (loop order above). */
    {
        uint64_t free4k = read_node0_free_4k();
        uint32_t promo_cap = left;
        if (free4k <= RERANK_PROMOTE_HEADROOM_4K) {
            promo_cap = 0;
        } else {
            uint64_t room = free4k - RERANK_PROMOTE_HEADROOM_4K;
            if ((uint64_t)promo_cap > room) {
                promo_cap = (uint32_t)room;
            }
        }
        for (i = 0; i < st->k_granules && promo_cap > 0; i++) {
            rerank_granule_t *g = rank[i];
            if (g->node != 1) {
                continue;
            }
            uint32_t got = enqueue_granule(pact, g, 0, promo_cap);
            promo_cap -= got;
            left -= got;
            if (got) {
                npromo++;
            }
        }
    }

    pact->workload->stats.rerank_epochs++;
    pact->workload->stats.rerank_tracked = n;
    pact->workload->stats.rerank_k = st->k_granules;

    log_info("rerank_enforce",
             "tracked=%u K=%lu demo_g=%u promo_g=%u left=%u node0_4k=%lu frac=%.2f granule=%lu",
             n, st->k_granules, ndemo, npromo, left, st->node0_pages_4k, pact->fast_tier_frac,
             st->granule_bytes);

    free(rank);
    (void)co;
}

int rerank_init(pact_context_t *pact)
{
    if (!pact) {
        return -1;
    }
    rerank_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return -1;
    }
    st->granule_bytes = pact->granule_bytes;
    if (st->granule_bytes < PAGE_SIZE || (st->granule_bytes & (st->granule_bytes - 1)) != 0) {
        log_error("rerank_init", "granule_bytes must be power-of-two >= 4K (got %lu)",
                  st->granule_bytes);
        free(st);
        return -1;
    }
    st->granule_mask = ~(st->granule_bytes - 1);
    st->table = rerank_table_init();
    if (!st->table) {
        free(st);
        return -1;
    }
    st->node0_pages_4k = read_node0_pages_4k();
    if (st->node0_pages_4k == 0) {
        log_warning("rerank_init", "could not read node0 MemTotal; K will be tracked-set sized");
        st->node0_pages_4k = 1;
    }
    st->seed_countdown = 0;
    pact->rerank = st;

    set_kernel_demotion_enabled(0);
    log_info("rerank_init", "userspace demote: node0=%lu 4K pages (%.1f GB) frac=%.2f "
                            "granule=%lu interval=%ums migrate_limit=%u",
             st->node0_pages_4k, (double)st->node0_pages_4k * PAGE_SIZE / (1024.0 * 1024.0 * 1024.0),
             pact->fast_tier_frac, st->granule_bytes, pact->rerank_interval_ms,
             pact->rerank_migrate_limit);
    return 0;
}

void rerank_destroy(pact_context_t *pact)
{
    if (!pact || !pact->rerank) {
        return;
    }
    rerank_state_t *st = pact->rerank;
    if (st->table) {
        khint_t k;
        kh_foreach(st->table, k)
        {
            free(kh_val(st->table, k));
        }
        rerank_table_destroy(st->table);
    }
    free(st);
    pact->rerank = NULL;
}

void rerank_coroutine(mco_coro *co)
{
    pact_context_t *ctx = (pact_context_t *)mco_get_user_data(co);
    while (ctx->running) {
        if (ctx->demotion_policy == DEMOTION_USERSPACE && ctx->rerank) {
            rerank_enforce(ctx, co);
        }
        mco_yield(co);
    }
}
