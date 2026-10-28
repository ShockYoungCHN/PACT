/* SPDX-License-Identifier: MIT */
/* rerank.c — userspace budget enforcement: rerank + top-K + demote-first.
 *
 * PEBS only scores pages (pac_value already reflects --score-mode). This walk
 * ranks page scores at the configured granularity (4KiB by default), then moves only pages that are
 * on the wrong node. A granule may be split (4KB interleave); it is not
 * treated as entirely local just because half its samples sit on node0.
 * Demote-first, then promote. Granule score is raw sum (no log1p) so sparse
 * hits cannot dominate a fully sampled neighbor.
 *
 * Node queries run inline on the control thread. A page-lock wait stalls all
 * control coroutines; the sampling and migration threads keep running.
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
#include <time.h>

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
#define RERANK_NODE_FAST 0
#define RERANK_NODE_SLOW 1
#define RERANK_NODE_MIXED 2
/* Keep this much free on node0 before enqueueing promotes (pages). */
#define RERANK_PROMOTE_HEADROOM_4K 0ULL
#define PM_PRESENT (1ULL << 63)

typedef struct {
    uint64_t granule;
    pid_t pid;
    double score;
    uint32_t n_pages; /* pages with pac_value > 0 merged this epoch */
    uint16_t present; /* estimated resident 4K pages in this granule */
    int8_t node; /* 0 fast, 1 slow, -1 unknown */
    uint8_t miss_epochs;
    uint8_t in_flight;
} rerank_granule_t;

/* Keep identity separate from hashing: a 4KiB page number uses more than
 * 32 bits, so OR-ing it with pid<<32 aliases distinct (pid, VA) pairs. */
typedef struct { uint64_t granule; pid_t pid; } rerank_key_t;
static rerank_key_t rerank_key(pid_t pid, uint64_t granule, uint64_t granule_bytes)
{
    (void)granule_bytes;
    return (rerank_key_t){.granule = granule, .pid = pid};
}
static khint_t rerank_key_hash(rerank_key_t key)
{
    return kh_hash_uint64(key.granule) ^ kh_hash_uint64((uint32_t)key.pid);
}
static bool rerank_key_equal(rerank_key_t a, rerank_key_t b)
{
    return a.pid == b.pid && a.granule == b.granule;
}
KHASHL_MAP_INIT(KH_LOCAL, rerank_table_t, rerank_table, rerank_key_t, rerank_granule_t *,
                rerank_key_hash, rerank_key_equal)

typedef struct { uint64_t first, end; int node; } rerank_zone_t;

struct rerank_state {
    rerank_table_t *table;
    uint64_t granule_bytes;
    uint64_t granule_mask;
    uint64_t node0_pages_4k;
    uint64_t k_granules;
    uint32_t seed_countdown;
    rerank_zone_t zones[64];
    size_t n_zones;
    bool scan_active;
};

typedef struct rerank_state rerank_state_t;

/* Granule score is the sum of page pac_value.
 * pc: each LLC-miss sample already added w_c*1024 (pc_class_score), so this
 * sum is weight*miss. freq: pac_value is the miss count. pac: sum of stalls.
 * Do not divide by access_count and do not replace the sum with one
 * per-process scalar: that gives every granule of a pid the same rank. */
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

static rerank_granule_t *rerank_get_or_add(rerank_state_t *st, pid_t pid, uint64_t granule)
{
    int ret;
    rerank_key_t key = rerank_key(pid, granule, st->granule_bytes);
    khint_t k = rerank_table_put(st->table, key, &ret);
    if (ret < 0 || k == kh_end(st->table)) {
        return NULL;
    }
    if (ret == 0) {
        return kh_val(st->table, k);
    }
    rerank_granule_t *g = calloc(1, sizeof(*g));
    if (!g) {
        rerank_table_del(st->table, k);
        return NULL;
    }
    g->granule = granule;
    g->pid = pid;
    g->node = -1;
    kh_val(st->table, k) = g;
    return g;
}

/* Seed placement on the control thread, before the full epoch refresh. */
static int query_node(pid_t pid, uint64_t page)
{
    void *addr = (void *)page;
    int status = -1;
    if (numa_move_pages(pid, 1, &addr, NULL, &status, 0) != 0) {
        return -1;
    }
    return status >= 0 ? status : -1;
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
static uint64_t rerank_seed_one(pact_context_t *pact, rerank_state_t *st, pid_t pid)
{
    char maps_path[64], pm_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    snprintf(pm_path, sizeof(pm_path), "/proc/%d/pagemap", pid);

    FILE *maps = fopen(maps_path, "r");
    if (!maps) {
        return 0;
    }
    int pfd = open(pm_path, O_RDONLY);
    if (pfd < 0) {
        fclose(maps);
        return 0;
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
            rerank_granule_t *g = rerank_get_or_add(st, pid, granule);
            if (!g) {
                continue;
            }
            if (g->present == 0) {
                g->present = 1; /* underestimate until refresh; keeps K large */
            }
            if (g->node < 0 && !g->in_flight && !pact_wl_exited(pact, pid)) {
                g->node = query_node(pid, lo & PAGE_MASK);
                added++;
            }
        }
    }
    close(pfd);
    fclose(maps);
    return added;
}

static void rerank_seed_resident(pact_context_t *pact, rerank_state_t *st)
{
    uint64_t added = 0;
    for (int i = 0; i < pact->n_workloads; i++) {
        if (pact->workloads[i].exited) {
            continue;
        }
        pid_t pid = pact->workloads[i].target_pid;
        if (pid > 0) {
            added += rerank_seed_one(pact, st, pid);
        }
    }
    if (added > 0) {
        log_info("rerank_seed", "added/refreshed %lu granules, tracked=%u", added,
                 kh_size(st->table));
    }
}

/* Read physical node ranges once. Root pagemap PFNs let the 4KiB path
 * refresh millions of pages with contiguous reads rather than one syscall
 * per page. If PFNs are hidden or outside these ranges, query in batches. */
static void rerank_load_zones(rerank_state_t *st)
{
    FILE *fp = fopen("/proc/zoneinfo", "r");
    if (!fp) return;
    char line[512]; int node = -1; uint64_t first = 0, span = 0;
    while (fgets(line, sizeof(line), fp)) {
        int next;
        if (sscanf(line, "Node %d, zone", &next) == 1) {
            if (span && st->n_zones < 64)
                st->zones[st->n_zones++] = (rerank_zone_t){first, first + span, node};
            node = next; first = span = 0;
        } else {
            unsigned long long value;
            if (sscanf(line, " start_pfn: %llu", &value) == 1) first = value;
            if (sscanf(line, " spanned %llu", &value) == 1) span = value;
        }
    }
    if (span && st->n_zones < 64)
        st->zones[st->n_zones++] = (rerank_zone_t){first, first + span, node};
    fclose(fp);
}
static int rerank_pfn_node(const rerank_state_t *st, uint64_t pfn)
{
    if (!pfn) return -1; /* CAP_SYS_ADMIN may be absent. */
    for (size_t i = 0; i < st->n_zones; i++)
        if (pfn >= st->zones[i].first && pfn < st->zones[i].end)
            return st->zones[i].node;
    return -1;
}
#define RERANK_QUERY_BATCH 1024
static void rerank_query_batch(pid_t pid, rerank_granule_t **items, size_t count)
{
    void *pages[RERANK_QUERY_BATCH]; int status[RERANK_QUERY_BATCH];
    for (size_t i = 0; i < count; i++) { pages[i] = (void *)items[i]->granule; status[i] = -1; }
    if (numa_move_pages(pid, (int)count, pages, NULL, status, 0) != 0) return;
    for (size_t i = 0; i < count; i++) {
        items[i]->present = status[i] >= 0 ? 1 : 0;
        items[i]->node = (status[i] == 0 || status[i] == 1) ? status[i] : -1;
    }
}
static uint64_t rerank_now_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}
static void rerank_refresh_pages(pact_context_t *pact, rerank_state_t *st, mco_coro *co)
{
    uint64_t began = rerank_now_ns(), resident = 0, reads = 0, queries = 0;
    st->scan_active = true;
    khint_t k;
    kh_foreach(st->table, k) {
        rerank_granule_t *g = kh_val(st->table, k);
        if (g) { g->present = 0; g->node = -1; }
    }
    uint64_t *entries = malloc(65536 * sizeof(*entries));
    if (!entries) { log_error("rerank_refresh_pages", "pagemap buffer allocation failed"); st->scan_active = false; return; }
    for (int wi = 0; wi < pact->n_workloads && pact->running; wi++) {
        pact_workload_t *wl = &pact->workloads[wi];
        if (wl->exited || wl->target_pid <= 0) continue;
        pid_t pid = wl->target_pid;
        char path[64], line[1024];
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);
        FILE *maps = fopen(path, "r");
        snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
        int pfd = open(path, O_RDONLY);
        if (!maps || pfd < 0) { if (maps) fclose(maps); if (pfd >= 0) close(pfd); continue; }
        rerank_granule_t *pending[RERANK_QUERY_BATCH]; size_t count = 0;
        while (pact->running && fgets(line, sizeof(line), maps)) {
            unsigned long lo, hi; char perm[8], name[512] = {0};
            if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &lo, &hi, perm, name) < 3) continue;
            char *n = name; while (*n == ' ') n++;
            if (perm[0] != 'r' || perm[1] != 'w' || *n == '/') continue;
            for (uint64_t at = lo; at < hi && pact->running;) {
                size_t pages = (hi - at) / PAGE_SIZE;
                if (pages > 65536) pages = 65536;
                ssize_t bytes = pread(pfd, entries, pages * sizeof(*entries),
                                      (off_t)((at / PAGE_SIZE) * sizeof(*entries)));
                reads++;
                if (bytes != (ssize_t)(pages * sizeof(*entries))) break;
                for (size_t i = 0; i < pages; i++) {
                    if (!(entries[i] & PM_PRESENT)) continue;
                    rerank_granule_t *g = rerank_get_or_add(st, pid, at + i * PAGE_SIZE);
                    if (!g) continue;
                    g->present = 1; resident++;
                    g->node = rerank_pfn_node(st, entries[i] & ((1ULL << 55) - 1));
                    if (g->node < 0) {
                        pending[count++] = g;
                        if (count == RERANK_QUERY_BATCH) {
                            rerank_query_batch(pid, pending, count); queries++; count = 0;
                        }
                    }
                }
                at += pages * PAGE_SIZE;
                mco_yield(co);
            }
        }
        if (count) { rerank_query_batch(pid, pending, count); queries++; }
        fclose(maps); close(pfd);
        /* PEBS may also score file-backed pages; refresh those in batches.
         * Anonymous pages absent from pagemap are checked too if scored. */
        count = 0;
        kh_foreach(st->table, k) {
            rerank_granule_t *g = kh_val(st->table, k);
            if (g && g->pid == pid && !g->present && g->score > 0) {
                pending[count++] = g;
                if (count == RERANK_QUERY_BATCH) {
                    rerank_query_batch(pid, pending, count); queries++; count = 0;
                    mco_yield(co);
                }
            }
            if (!pact->running) break;
        }
        if (count) { rerank_query_batch(pid, pending, count); queries++; }
    }
    free(entries);
    st->scan_active = false;
    log_info("rerank_refresh_pages", "resident=%lu pagemap_reads=%lu node_query_batches=%lu elapsed_ms=%.1f",
             resident, reads, queries, (rerank_now_ns() - began) / 1e6);
}

static void rerank_merge_scores(pact_context_t *pact, rerank_state_t *st)
{
    /* One shared ranking. Each 2MB granule sums the page scores of its pid.
     * A zero-sample granule stays at 0 and only fills leftover budget. */
    for (int wi = 0; wi < pact->n_workloads; wi++) {
        pac_table_t *table = pact->workloads[wi].pac_table;
        pid_t pid = pact->workloads[wi].target_pid;
        if (!table || pid <= 0 || pact->workloads[wi].exited) {
            continue;
        }
        khint_t k;
        kh_foreach(table, k)
        {
            pac_metadata_t *meta = kh_val(table, k);
            if (!meta || meta->pac_value == 0) {
                continue;
            }
            uint64_t granule = meta->page_addr & st->granule_mask;
            rerank_granule_t *g = rerank_get_or_add(st, pid, granule);
            if (!g) {
                continue;
            }
            g->score += page_score(pact, meta);
            if (g->n_pages < UINT32_MAX) {
                g->n_pages++;
            }
        }
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
    if (x->pid != y->pid) {
        return (x->pid > y->pid) - (x->pid < y->pid);
    }
    return 0;
}

static pac_metadata_t *lookup_meta(pact_context_t *pact, pid_t pid, uint64_t page)
{
    pact_workload_t *wl = pact_wl_find(pact, pid);
    if (!wl || !wl->pac_table) {
        return NULL;
    }
    khint_t k = pac_table_get(wl->pac_table, page);
    if (k == kh_end(wl->pac_table)) {
        return NULL;
    }
    return kh_val(wl->pac_table, k);
}

static uint32_t enqueue_granule(pact_context_t *pact, rerank_granule_t *g, int target,
                                uint32_t left)
{
    if (left == 0 || pact_wl_exited(pact, g->pid)) {
        return 0;
    }
    /* A capacity slot alone is not evidence that an unsampled page is hot.
     * Leave it on slow until its score becomes positive; demotion is unchanged. */
    if (target == 0 && g->score <= 0.0) {
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
    char pm[64];
    snprintf(pm, sizeof(pm), "/proc/%d/pagemap", (int)g->pid);
    int pfd = open(pm, O_RDONLY);
    uint32_t pushed = 0;
    /* Skip pages already on `target`, so a half-remote granule only spends
     * quota on the half that actually has to move. */
    enum { CHUNK = 64 };
    void *pages[CHUNK];
    int status[CHUNK];
    uint64_t addrs[CHUNK];
    uint32_t filled = 0;
    int stop = 0;

    for (uint32_t i = 0; i <= n4k && pushed < want && !stop; i++) {
        int flush = (i == n4k) || (filled == CHUNK);
        if (!flush && i < n4k) {
            uint64_t page = g->granule + (uint64_t)i * PAGE_SIZE;
            if (pfd >= 0 && !pagemap_present(pfd, page)) {
                continue;
            }
            pages[filled] = (void *)page;
            addrs[filled] = page;
            filled++;
            flush = (filled == CHUNK);
        }
        if (!flush || filled == 0) {
            continue;
        }
        int qok = (g->pid > 0 &&
                   numa_move_pages(g->pid, (int)filled, pages, NULL, status, 0) == 0);
        for (uint32_t j = 0; j < filled && pushed < want; j++) {
            if (qok && (status[j] < 0 || status[j] == target)) {
                continue;
            }
            pac_metadata_t *meta = lookup_meta(pact, g->pid, addrs[j]);
            if (!pact_enqueue_move(pact, meta, addrs[j], target, g->pid)) {
                stop = 1;
                break;
            }
            pushed++;
        }
        filled = 0;
    }
    if (pfd >= 0) {
        close(pfd);
    }
    if (pushed > 0) {
        g->in_flight = 1;
        pact_workload_t *owner = pact_wl_find(pact, g->pid);
        if (!owner || owner->exited) {
            return pushed;
        }
        if (target == 0) {
            owner->stats.rerank_enqueued_promote += pushed;
        } else {
            owner->stats.rerank_enqueued_demote += pushed;
        }
    }
    return pushed;
}

/* Eight 4KB samples, one syscall. Blocks if a sampled page is mid-migration. */
static void rerank_refresh_granule(rerank_granule_t *g, uint64_t granule_bytes, int pfd)
{
    uint64_t n4k = granule_bytes / PAGE_SIZE;
    if (n4k < 1) {
        n4k = 1;
    }
    uint16_t exact_present = 0;
    bool exact_ok = false;
    if (pfd >= 0) {
        uint64_t entries[512];
        uint64_t first = g->granule / PAGE_SIZE;
        uint64_t done = 0;
        exact_ok = true;
        while (done < n4k) {
            size_t count = n4k - done > 512 ? 512 : (size_t)(n4k - done);
            ssize_t bytes = pread(pfd, entries, count * sizeof(uint64_t),
                                  (off_t)((first + done) * sizeof(uint64_t)));
            if (bytes != (ssize_t)(count * sizeof(uint64_t))) { exact_ok = false; break; }
            for (size_t j = 0; j < count; j++) if (entries[j] & PM_PRESENT) exact_present++;
            done += count;
        }
        if (exact_ok) g->present = exact_present;
    }
    uint64_t step = n4k / RERANK_NODE_SAMPLES;
    if (step < 1) {
        step = 1;
    }
    /* 512/8 = 64, an even stride. With 4KB interleave every even page is the
     * same node, so all 8 samples agree and the remote half is invisible. */
    if ((step & 1ULL) == 0 && step > 1) {
        step--;
    }

    void *pages[RERANK_NODE_SAMPLES];
    int status[RERANK_NODE_SAMPLES];
    int nq = 0;
    for (uint64_t i = 0; i < n4k && nq < (int)RERANK_NODE_SAMPLES; i += step) {
        pages[nq++] = (void *)(g->granule + i * PAGE_SIZE);
    }
    memset(status, 0xff, sizeof(status));
    if (g->pid <= 0 || nq == 0) {
        return;
    }
    if (numa_move_pages(g->pid, nq, pages, NULL, status, 0) != 0) {
        return;
    }
    int n0 = 0, n1 = 0;
    for (int j = 0; j < nq; j++) {
        if (status[j] == 0) {
            n0++;
        } else if (status[j] == 1) {
            n1++;
        }
    }
    if (n0 + n1 == 0) {
        return;
    }
    if (n0 > 0 && n1 > 0) {
        g->node = RERANK_NODE_MIXED;
    } else {
        g->node = (int8_t)(n0 > 0 ? RERANK_NODE_FAST : RERANK_NODE_SLOW);
    }
    uint64_t est = ((uint64_t)(n0 + n1) * n4k) / (uint64_t)nq;
    if (est < 1) {
        est = 1;
    }
    if (est > n4k) {
        est = n4k;
    }
    if (!exact_ok) g->present = (uint16_t)est;
}

/* Query on the rerank coroutine: a blocking syscall intentionally stalls
 * control, while sampling continues on its own thread. */
static void rerank_refresh_nodes(pact_context_t *pact, rerank_state_t *st, mco_coro *co)
{
    int pfds[PACT_MAX_WORKLOADS];
    for (int wi = 0; wi < pact->n_workloads; wi++) {
        char path[64]; snprintf(path, sizeof(path), "/proc/%d/pagemap", pact->workloads[wi].target_pid);
        pfds[wi] = pact->workloads[wi].exited ? -1 : open(path, O_RDONLY);
    }
    uint32_t visited = 0;
    khint_t k;
    kh_foreach(st->table, k)
    {
        if (!pact->running) {
            break;
        }
        rerank_granule_t *g = kh_val(st->table, k);
        if (g && !g->in_flight && g->pid > 0 && !pact_wl_exited(pact, g->pid)) {
            int wi = pact_wl_index(pact, g->pid);
            rerank_refresh_granule(g, st->granule_bytes, wi >= 0 ? pfds[wi] : -1);
        }
        if (++visited % 2048 == 0) {
            mco_yield(co);
        }
    }
    for (int wi = 0; wi < pact->n_workloads; wi++) if (pfds[wi] >= 0) close(pfds[wi]);
}

static void rerank_log_rank(pact_context_t *pact, rerank_granule_t **rank, uint32_t n,
                            uint64_t k)
{
    for (int wi = 0; wi < pact->n_workloads; wi++) {
        pid_t pid = pact->workloads[wi].target_pid;
        if (pid <= 0) {
            continue;
        }
        uint32_t scored = 0, kept = 0;
        uint64_t kept_present = 0;
        double mn = 0.0, mx = 0.0, sum = 0.0;
        for (uint32_t i = 0; i < n; i++) {
            if (rank[i]->pid != pid || rank[i]->score <= 0.0) {
                continue;
            }
            if (scored == 0 || rank[i]->score < mn) {
                mn = rank[i]->score;
            }
            if (rank[i]->score > mx) {
                mx = rank[i]->score;
            }
            sum += rank[i]->score;
            scored++;
            if ((uint64_t)i < k) {
                kept++;
                kept_present += rank[i]->present;
            }
        }
        log_info("rerank_score",
                 "pid=%d scored_g=%u kept_g=%u min=%.0f max=%.0f sum=%.0f kept_present_4k=%lu",
                 (int)pid, scored, kept, mn, mx, sum, kept_present);
    }
    if (n == 0 || k == 0) {
        return;
    }
    uint32_t cut = (uint32_t)((k < n) ? k - 1 : n - 1);
    rerank_granule_t *g = rank[cut];
    log_info("rerank_cut",
             "rank=%u pid=%d granule=0x%lx score=%.0f node=%d pages=%u K=%lu",
             cut, (int)g->pid, (unsigned long)g->granule, g->score, (int)g->node,
             g->n_pages, (unsigned long)k);
}

static void rerank_enforce(pact_context_t *pact, mco_coro *co)
{
    rerank_state_t *st = pact->rerank;
    if (!st || !st->table || !pact->workload) {
        return;
    }

    for (int i = 0; i < pact->n_workloads; i++) {
        if (pact->workloads[i].exited) {
            continue;
        }
        ring_buffer_migration_entry_t *ring = pact->workloads[i].migration_ring;
        if (ring && ring_buffer_migration_entry_size(ring) > (MIGRATION_RING_DEFAULT_SIZE / 4)) {
            log_debug("rerank_enforce", "ring still draining (%u); skip epoch",
                      ring_buffer_migration_entry_size(ring));
            return;
        }
    }

    /* Zero scores then re-merge so a page that cooled / stopped sampling
     * does not keep a stale high rank. Unsampled granules stay at 0. */
    khint_t k;
    kh_foreach(st->table, k)
    {
        rerank_granule_t *g = kh_val(st->table, k);
        if (g) {
            g->score = 0.0;
            g->n_pages = 0;
            /* One-epoch sticky: a granule enqueued last tick is eligible again. */
            g->in_flight = 0;
        }
    }

    if (st->granule_bytes == PAGE_SIZE) {
        rerank_merge_scores(pact, st);
        rerank_refresh_pages(pact, st, co);
    } else {
        if (st->seed_countdown == 0) {
            rerank_seed_resident(pact, st);
            st->seed_countdown = 4;
        } else {
            st->seed_countdown--;
        }
        rerank_merge_scores(pact, st);
        rerank_refresh_nodes(pact, st, co);
    }
    if (!pact->running) {
        return;
    }

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
        if (g && !pact_wl_exited(pact, g->pid) && g->present > 0) {
            rank[i++] = g;
        }
    }
    n = i;
    if (n == 0) { free(rank); return; }
    qsort(rank, n, sizeof(*rank), cmp_rank);

    uint64_t budget = (uint64_t)(pact->fast_tier_frac * (double)st->node0_pages_4k);
    uint64_t target_local = 0;
    for (int wi = 0; wi < pact->n_workloads; wi++) {
        if (pact->workloads[wi].exited) continue;
        char path[64], line[4096];
        snprintf(path, sizeof(path), "/proc/%d/numa_maps", pact->workloads[wi].target_pid);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, " file=")) continue;
            char *n0 = strstr(line, " N0=");
            if (n0) {
                char *ks = strstr(line, " kernelpagesize_kB=");
                uint64_t kb = ks ? strtoull(ks + strlen(" kernelpagesize_kB="), NULL, 10) : 4;
                target_local += strtoull(n0 + 4, NULL, 10) * (kb / 4);
            }
        }
        fclose(fp);
    }
    uint64_t free_pages = read_node0_free_4k();
    uint64_t available = free_pages + target_local;
    available = available > RERANK_PROMOTE_HEADROOM_4K ? available - RERANK_PROMOTE_HEADROOM_4K : 0;
    if (budget > available) budget = available;
    log_info("rerank_budget", "configured_4k=%lu effective_4k=%lu local_4k=%lu free_4k=%lu",
             (unsigned long)(pact->fast_tier_frac * st->node0_pages_4k), budget, target_local, free_pages);
    uint64_t pages_per = pact->granule_bytes / PAGE_SIZE;
    if (pages_per < 1) {
        pages_per = 1;
    }
    /* K is "keep adding granules until resident 4K pages reach C".
     * Counting granules as dense 2MB locked PC at 2GB of a 5.3GB sparse
     * btree (20260922f: promo_g=0, k_wl0=2048, NUMA stuck at 2018MB). */
    uint64_t kept = 0;
    st->k_granules = n;
    for (i = 0; i < n; i++) {
        if (kept >= budget) {
            st->k_granules = i;
            break;
        }
        uint64_t pr = rank[i]->present ? (uint64_t)rank[i]->present : 1;
        kept += pr;
    }
    if (st->k_granules < 1 && budget > 0) {
        st->k_granules = 1;
    }

    rerank_log_rank(pact, rank, n, st->k_granules);

    for (i = 0; i < n; i++) {
        if (i < st->k_granules) {
            rank[i]->miss_epochs = 0;
        } else if (rank[i]->node == RERANK_NODE_FAST ||
                   rank[i]->node == RERANK_NODE_MIXED) {
            if (rank[i]->miss_epochs < 255) {
                rank[i]->miss_epochs++;
            }
        } else {
            rank[i]->miss_epochs = 0;
        }
    }

    uint32_t left = pact->rerank_migrate_limit;
    uint32_t ndemo = 0, npromo = 0;
    /* Reserve half the epoch for promote. Otherwise a ranking swap
     * demotes 512 granules, burns migrate_limit, and C drains for an
     * epoch (PC 20260922e: demo_g=512 promo_g=0). */
    uint32_t demo_budget = left / 2;

    /* Demote first: free node0 before promotions try to allocate there. */
    for (i = (uint32_t)st->k_granules; i < n && demo_budget > 0; i++) {
        rerank_granule_t *g = rank[i];
        /* Fast or mixed: some pages still occupy node0 and this granule
         * is outside the keep set. Mixed pages already on node1 are skipped
         * inside enqueue_granule. */
        if (pact_wl_exited(pact, g->pid) || g->miss_epochs < RERANK_HYST_EPOCHS ||
            (g->node != RERANK_NODE_FAST && g->node != RERANK_NODE_MIXED)) {
            continue;
        }
        uint32_t got = enqueue_granule(pact, g, 1, demo_budget);
        demo_budget -= got;
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
            if (pact_wl_exited(pact, g->pid) ||
                (g->node != RERANK_NODE_SLOW && g->node != RERANK_NODE_MIXED)) {
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

    int k_wl[PACT_MAX_WORKLOADS];
    memset(k_wl, 0, sizeof(k_wl));
    for (i = 0; i < st->k_granules && i < n; i++) {
        int wi = pact_wl_index(pact, rank[i]->pid);
        if (wi >= 0) {
            k_wl[wi]++;
        }
    }
    log_info("rerank_enforce",
             "tracked=%u K=%lu kept_4k=%lu demo_g=%u promo_g=%u left=%u frac=%.2f "
             "k_wl0=%d k_wl1=%d",
             n, st->k_granules, kept, ndemo, npromo, left, pact->fast_tier_frac,
             k_wl[0], k_wl[1]);

    free(rank);
    (void)co;
}

bool rerank_scan_in_progress(const pact_context_t *pact)
{
    const rerank_state_t *st = pact ? pact->rerank : NULL;
    return st && st->scan_active;
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
    if (st->granule_bytes == PAGE_SIZE) rerank_load_zones(st);
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
