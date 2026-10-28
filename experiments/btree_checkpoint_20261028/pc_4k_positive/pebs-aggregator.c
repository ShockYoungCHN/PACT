/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* PEBS aggregator - aggregates per-CPU PEBS samples into per-page PAC. */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h> /* For PAGE_SIZE */
#include <signal.h> /* kill(pid, 0) workload-exit probe */
#include <sys/types.h>
#include <time.h>
#include "constants.h"
#include "pact.h"
#include "pmu.h"
#include "error.h"
#include "pebs-aggregator.h"
#include "logging.h"
#include "utils.h"
#include "pc_class.h"

/* One drained PEBS sample before same-window attribution. */
typedef struct {
    uint64_t addr_enc; /* PEBS_ENCODE_ADDR_TIER */
    uint64_t ip;
    pid_t pid;
    pid_t tid;
    uint32_t cpu;
    uint64_t sample_time_ns; /* From PERF_SAMPLE_TIME, default perf clock. */
    uint64_t event_id;
    uint64_t period;
    uint64_t drain_time_ns; /* CLOCK_MONOTONIC at mmap drain, before attribution */
} pebs_staged_sample_t;
static inline void heat_dump_sample(const pebs_staged_sample_t *sample);

#ifndef PAGE_SIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#endif

extern uint64_t rdtsc(void);

/* PEBS Aggregator Context */
typedef struct pebs_aggregator {
    /* Per-CPU PEBS contexts from pact.c */
    per_cpu_state_t **cpu_states;
    int num_cpus;
    uint64_t cpu_mask;

    /* Statistics */
    uint64_t events_per_cpu[64]; /* Support up to 64 CPUs */
    uint64_t events_per_tier[2]; /* Per-tier PEBS events (all target pids) */
    uint64_t events_per_tier_wl[PACT_MAX_WORKLOADS][2];
    uint64_t read_events_from_perf;
    uint64_t pushed_events_to_update;
    uint64_t dropped_events_update_full;
    uint64_t dropped_events_workload;
    uint64_t lost_samples;
    uint64_t lost_events;

    /* Round-robin state for fairness */
    int last_cpu_checked;

    /* Back-reference to pact context — used to resolve TGID → workload index
     * at sample-decode time (replaces the old pidmap khash). */
    pact_context_t *pact_ctx;

    /* Always-on diagnostic counters (cumulative across run) */
    uint64_t aggregation_cycles_total;      /* Total sampling thread cycles */
    uint64_t aggregation_max_samples_cycle; /* High-water mark: max samples in one cycle */
    uint64_t tier_from_dsrc_fast;
    uint64_t tier_from_dsrc_slow;
    uint64_t tier_from_pac_fallback;
    uint64_t lost_records_total;
    uint64_t lost_samples_total;
    uint64_t malformed_records_total;
    uint64_t staging_drops_total;

    /* Per-cycle sample staging: every CPU's perf buffer is drained here
     * FIRST so the window's total sample count A_t is known before the
     * per-sample stall scalar is computed (Algorithm 1 attributes with
     * same-window counts). Sized to the PAC update ring capacity. */
    pebs_staged_sample_t *cycle_buf;
    int cycle_buf_cap;
} pebs_aggregator_t;

/* Initialize PEBS aggregator */
pebs_aggregator_t *pebs_aggregator_create(per_cpu_state_t *cpu_states, int num_cpus,
                                          uint64_t cpu_mask)
{
    pebs_aggregator_t *agg = calloc(1, sizeof(pebs_aggregator_t));
    if (!agg) {
        return NULL;
    }

    agg->cpu_states = malloc(num_cpus * sizeof(per_cpu_state_t *));
    if (!agg->cpu_states) {
        free(agg);
        return NULL;
    }

    /* Store pointers to CPU states */
    for (int i = 0; i < num_cpus; i++) {
        agg->cpu_states[i] = &cpu_states[i];
    }

    agg->num_cpus = num_cpus;
    agg->cpu_mask = cpu_mask;
    agg->last_cpu_checked = 0;

    agg->cycle_buf_cap = 131072; /* == PAC update ring capacity */
    agg->cycle_buf = malloc((size_t)agg->cycle_buf_cap * sizeof(pebs_staged_sample_t));
    if (!agg->cycle_buf) {
        free(agg->cpu_states);
        free(agg);
        return NULL;
    }

    log_info("pebs_aggregator_create", "Created PEBS aggregator: %d CPUs, mask=0x%lx", num_cpus,
             cpu_mask);

    return agg;
}

/*
 * Copy `len` bytes out of the perf ring buffer starting at byte offset `off`
 * (modulo data_size), reassembling a record that wraps the buffer end. `base`
 * points at the first data page; `data_size` is the data-region size.
 */
static inline void ring_copy(void *dst, const char *base, uint64_t off, size_t len,
                             uint64_t data_size)
{
    uint64_t start = off % data_size;
    size_t first = (start + len <= data_size) ? len : (size_t)(data_size - start);
    memcpy(dst, base + start, first);
    if (first < len) {
        memcpy((char *)dst + first, base, len - first);
    }
}

/* Drain one PEBS ring. hw_tier is 0 for LOCAL_DRAM (0x01d3), 1 for REMOTE. */
static int read_one_pebs_mmap(pebs_aggregator_t *agg, void *pebs_mmap,
                              pebs_staged_sample_t *events, int max_events, uint8_t hw_tier)
{
    if (!pebs_mmap || pebs_mmap == MAP_FAILED) {
        return 0;
    }

    struct perf_event_mmap_page *perf_page = (struct perf_event_mmap_page *)pebs_mmap;
    const uint64_t data_size = (uint64_t)PERF_BUFFER_PAGES * PAGE_SIZE;
    char *data = (char *)pebs_mmap + PAGE_SIZE;

    uint64_t head = __atomic_load_n(&perf_page->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = perf_page->data_tail;
    if (head == tail) {
        return 0;
    }

    int count = 0;
    while (tail < head && count < max_events) {
        uint64_t off = tail % data_size;

        struct perf_event_header hdr;
        if (head - tail < sizeof(hdr)) {
            break;
        }
        ring_copy(&hdr, data, off, sizeof(hdr), data_size);
        if (hdr.size < sizeof(hdr) || hdr.size > data_size) {
            agg->malformed_records_total++;
            tail = head; /* Corrupt size cannot be used to find the next record. */
            break;
        }
        if (hdr.size > head - tail) {
            break;
        }

        if (hdr.type == PERF_RECORD_SAMPLE) {
            pact_pebs_record_t rec;
            if (hdr.size == sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                if (is_target_pid(agg->pact_ctx, (pid_t)rec.pid)) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    events[count].drain_time_ns =
                        (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
                    events[count].addr_enc = PEBS_ENCODE_ADDR_TIER(rec.addr, hw_tier);
                    events[count].ip = rec.ip;
                    events[count].pid = (pid_t)rec.pid;
                    events[count].tid = (pid_t)rec.tid;
                    events[count].cpu = rec.cpu;
                    events[count].sample_time_ns = rec.sample_time_ns;
                    events[count].event_id = rec.event_id;
                    events[count].period = rec.period;
                    heat_dump_sample(&events[count]);
                    count++;
                    agg->events_per_tier[hw_tier]++;
                    int wi = pact_wl_index(agg->pact_ctx, (pid_t)rec.pid);
                    if (wi >= 0) {
                        agg->events_per_tier_wl[wi][hw_tier]++;
                    }
                    if (hw_tier == 0) {
                        agg->tier_from_dsrc_fast++;
                    } else {
                        agg->tier_from_dsrc_slow++;
                    }
                }
            } else {
                agg->malformed_records_total++;
            }
        } else if (hdr.type == PERF_RECORD_LOST) {
            struct {
                struct perf_event_header header;
                uint64_t id;
                uint64_t lost;
            } rec;
            if (hdr.size >= sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                agg->lost_events++;
                agg->lost_samples += rec.lost;
                agg->lost_records_total++;
                agg->lost_samples_total += rec.lost;
            } else {
                agg->malformed_records_total++;
            }
        } else if (hdr.type == PERF_RECORD_LOST_SAMPLES) {
            struct {
                struct perf_event_header header;
                uint64_t lost;
            } rec;
            if (hdr.size >= sizeof(rec)) {
                ring_copy(&rec, data, off, sizeof(rec), data_size);
                agg->lost_events++;
                agg->lost_samples += rec.lost;
                agg->lost_records_total++;
                agg->lost_samples_total += rec.lost;
            } else {
                agg->malformed_records_total++;
            }
        }
        tail += hdr.size;
    }

    __atomic_store_n(&perf_page->data_tail, tail, __ATOMIC_RELEASE);
    return count;
}

/* Read PEBS events from one CPU (local + remote L3-miss-to-DRAM streams). */
static int read_cpu_pebs_events(pebs_aggregator_t *agg, per_cpu_state_t *cpu_state,
                                pebs_staged_sample_t *events, int max_events)
{
    if (!cpu_state || max_events <= 0) {
        return 0;
    }
    /* Split the batch so a busy local ring cannot starve remote (or vice versa). */
    int half = max_events / 2;
    if (half < 1) {
        half = max_events;
    }
    int n = 0;
    n += read_one_pebs_mmap(agg, cpu_state->pebs_mmap[0], events + n, half, 0);
    n += read_one_pebs_mmap(agg, cpu_state->pebs_mmap[1], events + n, max_events - n, 1);
    if (n < max_events) {
        n += read_one_pebs_mmap(agg, cpu_state->pebs_mmap[0], events + n, max_events - n, 0);
    }
    return n;
}

/* Aggregate PEBS events from all CPUs and push directly to PAC update ring */
/* Per-sample stall attribution = the cycle's per-tier scalar.
 * PEBS_ENCODE_PAC packs the PAC value into 18 bits, so clamp to 262143 here:
 * a larger value would be silently truncated by the encoder and corrupt the
 * PAC score. */
#define PAC_VALUE_MAX 262143u
static inline uint32_t compute_sample_stalls(uint8_t tier, double fast_stalls, double slow_stalls)
{
    double base = (tier == 0) ? fast_stalls : slow_stalls;
    if (base < 0.0) {
        return 0;
    }
    if (base > (double)PAC_VALUE_MAX) {
        return PAC_VALUE_MAX;
    }
    return (uint32_t)base;
}

/* Decode one staged PEBS sample into address/tier/ip. */
static inline void decode_pebs_sample(const pebs_staged_sample_t *events, int i, uint64_t *out_addr,
                                      uint8_t *out_tier, uint64_t *out_ip)
{
    uint64_t encoded = events[i].addr_enc;
    *out_addr = PEBS_DECODE_ADDR(encoded);
    *out_tier = PEBS_DECODE_TIER(encoded);
    *out_ip = events[i].ip;
}

/* Per-workload attributed stalls = k_constant * llc_misses / (mlp * events).
 * CHA MLP is socket-wide (copied onto every wl); LLC-miss counts are per-PID. */
static void compute_attributed_stalls_wl(pact_context_t *ctx, pebs_aggregator_t *agg, int wi,
                                         double *out_fast_stalls, double *out_slow_stalls)
{
    *out_fast_stalls = 0.0;
    *out_slow_stalls = 0.0;
    if (wi < 0 || wi >= ctx->n_workloads) {
        return;
    }
    pact_workload_t *wl = &ctx->workloads[wi];
    double fast_coeff = wl->workload_mlp_fast * (double)agg->events_per_tier_wl[wi][0];
    double slow_coeff = wl->workload_mlp_slow * (double)agg->events_per_tier_wl[wi][1];
    if (fast_coeff > 0) {
        *out_fast_stalls = (ctx->k_constant_dram * wl->stats.llc_misses_fast) / fast_coeff;
    }
    if (slow_coeff > 0) {
        *out_slow_stalls = (ctx->k_constant_cxl * wl->stats.llc_misses_slow) / slow_coeff;
    }
}

/*
 * Drain, attribute, and publish one window's PEBS samples.
 *
 * Phase A drains every CPU's perf buffer into cycle_buf so the window's
 * per-tier sample population is known; phase B then computes the per-tier
 * stall scalar from THIS window's counters and counts (Algorithm 1: S_p =
 * S * A_p / A_t with all terms from the same window) and pushes each
 * attributed sample to the PAC update ring.
 */
/* Opt-in schema v2 export via PACT_PEBS_SAMPLES_CSV (space separated).
 * Explicit sample_time_ns replaces the old ambiguous time_ns column.
 * page_id is a 2 MiB virtual-region index, not proof of huge-page backing.
 * PACT_PEBS_SAMPLES_STRIDE defaults to 1. */
static FILE *g_heat_fp = NULL;
static bool g_heat_done = false;
static unsigned long g_heat_stride = 1;
static unsigned long g_heat_ctr = 0;
static unsigned long g_heat_written = 0;
typedef struct {
    uint64_t monotonic_before_ns;
    uint64_t tsc;
    uint64_t monotonic_after_ns;
} pebs_clock_anchor_t;
static pebs_clock_anchor_t g_heat_clock_start;

static pebs_clock_anchor_t heat_clock_anchor(void)
{
    pebs_clock_anchor_t anchor;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    anchor.monotonic_before_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    /* LFENCE orders the TSC read with respect to the surrounding clock reads. */
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) : : "memory");
    anchor.tsc = ((uint64_t)hi << 32) | lo;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    anchor.monotonic_after_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    return anchor;
}

static void heat_dump_init_once(void)
{
    g_heat_done = true;
    const char *path = getenv("PACT_PEBS_SAMPLES_CSV");
    if (!path || !path[0]) {
        return;
    }
    g_heat_fp = fopen(path, "w");
    if (!g_heat_fp) {
        log_error("heat_dump_init_once", "Cannot open PEBS export %s: %s", path,
                  strerror(errno));
        return;
    }
    const char *s = getenv("PACT_PEBS_SAMPLES_STRIDE");
    if (s && s[0]) {
        unsigned long v = strtoul(s, NULL, 10);
        if (v > 0) {
            g_heat_stride = v;
        }
    }
    fprintf(g_heat_fp, "sample_time_ns pid ip addr page_id tid cpu event_id period tier drain_time_ns\n");
    g_heat_clock_start = heat_clock_anchor();
    log_info("heat_dump_init_once", "PEBS export schema v2: default perf sample time; "
             "drain_time_ns uses CLOCK_MONOTONIC; stride=%lu", g_heat_stride);
}

static inline void heat_dump_sample(const pebs_staged_sample_t *sample)
{
    if (!g_heat_done) {
        heat_dump_init_once();
    }
    if (!g_heat_fp) {
        return;
    }
    if ((g_heat_ctr++ % g_heat_stride) != 0) {
        return;
    }
    uint64_t addr = PEBS_DECODE_ADDR(sample->addr_enc);
    fprintf(g_heat_fp, "%llu %d 0x%lx 0x%lx %llu %d %u %llu %llu %u %llu\n",
            (unsigned long long)sample->sample_time_ns, (int)sample->pid,
            (unsigned long)sample->ip, (unsigned long)addr,
            (unsigned long long)(addr >> 21), (int)sample->tid, sample->cpu,
            (unsigned long long)sample->event_id, (unsigned long long)sample->period,
            (unsigned)PEBS_DECODE_TIER(sample->addr_enc),
            (unsigned long long)sample->drain_time_ns);
    g_heat_written++;
    if ((g_heat_ctr & 0x1FFF) == 0) {
        fflush(g_heat_fp);
    }
}

/* Persist clock/schema and loss information beside the data, including an
 * empty capture. PERF_SAMPLE_TIME itself does not expose whether the kernel
 * fell back from hardware TSC to its software clock for a given record. */
static void heat_dump_finish(const pebs_aggregator_t *agg)
{
    if (!g_heat_done) {
        heat_dump_init_once();
    }
    if (!g_heat_fp) {
        return;
    }
    bool write_error = ferror(g_heat_fp) != 0;
    if (fclose(g_heat_fp) != 0) {
        write_error = true;
    }
    g_heat_fp = NULL;

    const char *path = getenv("PACT_PEBS_SAMPLES_CSV");
    size_t len = strlen(path) + sizeof(".meta.json");
    char *meta_path = malloc(len);
    if (!meta_path) {
        log_error("heat_dump_finish", "Cannot allocate PEBS metadata path");
        return;
    }
    snprintf(meta_path, len, "%s.meta.json", path);
    FILE *fp = fopen(meta_path, "w");
    if (fp) {
        pebs_clock_anchor_t end = heat_clock_anchor();
        fprintf(fp, "{\n  \"schema_version\": 2,\n"
                "  \"sample_clock\": \"perf_default\",\n"
                "  \"drain_clock\": \"CLOCK_MONOTONIC\",\n"
                "  \"timestamp_source\": \"PERF_SAMPLE_TIME; hardware TSC when kernel supports conversion\",\n"
                "  \"sample_type\": %llu,\n  \"region_shift\": 21,\n"
                "  \"tier_source\": \"sampling_event_fd; 0=local, 1=remote\",\n"
                "  \"local_event_config\": %llu,\n  \"remote_event_config\": %llu,\n"
                "  \"export_stride\": %lu,\n  \"written\": %lu,\n"
                "  \"considered\": %lu,\n  \"lost_records\": %llu,\n"
                "  \"lost_samples\": %llu,\n  \"malformed_records\": %llu,\n"
                "  \"staging_drops\": %llu,\n  \"write_error\": %s,\n",
                (unsigned long long)PACT_PEBS_SAMPLE_TYPE,
                (unsigned long long)g_pmu_platform.event_llc_miss_local,
                (unsigned long long)g_pmu_platform.event_llc_miss_remote,
                g_heat_stride, g_heat_written, g_heat_ctr,
                (unsigned long long)agg->lost_records_total,
                (unsigned long long)agg->lost_samples_total,
                (unsigned long long)agg->malformed_records_total,
                (unsigned long long)agg->staging_drops_total, write_error ? "true" : "false");
        fprintf(fp, "  \"clock_anchors\": [\n"
                "    {\"monotonic_before_ns\": %llu, \"tsc\": %llu, \"monotonic_after_ns\": %llu},\n"
                "    {\"monotonic_before_ns\": %llu, \"tsc\": %llu, \"monotonic_after_ns\": %llu}\n"
                "  ],\n  \"streams\": [\n",
                (unsigned long long)g_heat_clock_start.monotonic_before_ns,
                (unsigned long long)g_heat_clock_start.tsc,
                (unsigned long long)g_heat_clock_start.monotonic_after_ns,
                (unsigned long long)end.monotonic_before_ns, (unsigned long long)end.tsc,
                (unsigned long long)end.monotonic_after_ns);
        bool first = true;
        for (int ci = 0; ci < agg->num_cpus; ci++) {
            per_cpu_state_t *state = agg->cpu_states[ci];
            for (int tier = 0; tier < 2; tier++) {
                struct perf_event_mmap_page *page = state->pebs_mmap[tier];
                if (!page || page == MAP_FAILED) {
                    continue;
                }
                uint32_t seq, mult, shift;
                uint64_t zero, cycles, mask, capabilities;
                /* perf mmap time-conversion fields use the metadata seqlock. */
                do {
                    seq = __atomic_load_n(&page->lock, __ATOMIC_ACQUIRE);
                    if (seq & 1) {
                        continue;
                    }
                    mult = page->time_mult;
                    shift = page->time_shift;
                    zero = page->time_zero;
                    cycles = page->time_cycles;
                    mask = page->time_mask;
                    capabilities = page->capabilities;
                    __atomic_thread_fence(__ATOMIC_ACQUIRE);
                } while ((seq & 1) || __atomic_load_n(&page->lock, __ATOMIC_RELAXED) != seq);
                uint64_t id = 0;
                if (ioctl(state->fd_pebs[tier], PERF_EVENT_IOC_ID, &id) < 0) {
                    log_warning("heat_dump_finish", "Cannot read PEBS event ID for CPU %d tier %d",
                                state->cpu_id, tier);
                }
                fprintf(fp, "%s    {\"cpu\": %d, \"tier\": %d, \"event_id\": %llu, "
                        "\"time_mult\": %u, \"time_shift\": %u, \"time_zero\": %llu, "
                        "\"time_cycles\": %llu, \"time_mask\": %llu, \"capabilities\": %llu}",
                        first ? "" : ",\n", state->cpu_id, tier, (unsigned long long)id,
                        mult, shift, (unsigned long long)zero, (unsigned long long)cycles,
                        (unsigned long long)mask, (unsigned long long)capabilities);
                first = false;
            }
        }
        fprintf(fp, "\n  ]\n}\n");
        if (fclose(fp) != 0) {
            log_error("heat_dump_finish", "Cannot finish PEBS metadata %s", meta_path);
        }
    } else {
        log_error("heat_dump_finish", "Cannot open PEBS metadata %s: %s", meta_path,
                  strerror(errno));
    }
    free(meta_path);
    if (write_error) {
        log_error("heat_dump_finish", "PEBS export encountered a write error");
    }
}

int pebs_aggregate_events(pebs_aggregator_t *agg, pact_context_t *ctx)
{
    if (!agg || !ctx) {
        return -1;
    }

    /* Phase A: drain all CPUs into the staging buffer. Round-robin start
     * keeps CPU 0 from monopolizing the buffer when it fills. */
    int n = 0;
    for (int ci = 0; ci < agg->num_cpus; ci++) {
        int cpu = (agg->last_cpu_checked + ci) % agg->num_cpus;
        per_cpu_state_t *cpu_state = agg->cpu_states[cpu];

        int count;
        do {
            int room = agg->cycle_buf_cap - n;
            if (room == 0) {
                break;
            }
            count =
                read_cpu_pebs_events(agg, cpu_state, agg->cycle_buf + n, room < 2048 ? room : 2048);
            agg->events_per_cpu[cpu] += count;
            agg->read_events_from_perf += count;
            n += count;
        } while (count > 0);

        /* Staging full: exhaust this CPU's perf buffer into drop counters
         * so the producer ring cannot wedge. */
        if (n == agg->cycle_buf_cap) {
            pebs_staged_sample_t scratch[512];
            int dropped;
            while ((dropped = read_cpu_pebs_events(agg, cpu_state, scratch, 512)) > 0) {
                agg->read_events_from_perf += dropped;
                agg->dropped_events_update_full += dropped;
                agg->dropped_events_workload += dropped;
                agg->staging_drops_total += dropped;
            }
        }
    }
    agg->last_cpu_checked = (agg->last_cpu_checked + 1) % agg->num_cpus;

    /* Phase B: same-window attribution, per owning pid. */
    double fast_stalls_wl[PACT_MAX_WORKLOADS];
    double slow_stalls_wl[PACT_MAX_WORKLOADS];
    for (int wi = 0; wi < ctx->n_workloads; wi++) {
        compute_attributed_stalls_wl(ctx, agg, wi, &fast_stalls_wl[wi], &slow_stalls_wl[wi]);
    }

    int pac_count = 0;
    for (int i = 0; i < n; i++) {
        uint64_t addr;
        uint64_t ip;
        uint8_t tier;
        decode_pebs_sample(agg->cycle_buf, i, &addr, &tier, &ip);
        pid_t pid = agg->cycle_buf[i].pid;
        int wi = pact_wl_index(ctx, pid);
        uint64_t page = addr & PAGE_MASK;
        uint32_t attributed;
        if (ctx->score_mode == SCORE_MODE_FREQ) {
            attributed = 1;
        } else if (ctx->score_mode == SCORE_MODE_PC) {
            attributed = pc_class_score(ctx, pid, ip, PAC_VALUE_MAX);
        } else {
            double fs = (wi >= 0) ? fast_stalls_wl[wi] : 0.0;
            double ss = (wi >= 0) ? slow_stalls_wl[wi] : 0.0;
            attributed = compute_sample_stalls(tier, fs, ss);
            if (ctx->score_mode == SCORE_MODE_PAC_PC) {
                attributed = scale_by_pc_class(ctx, pid, ip, attributed, PAC_VALUE_MAX);
            }
        }
        log_pebs_sample(ctx, "pebs_aggregate_events", wi, addr, tier, attributed);


        pac_update_t upd = {.encoded = PEBS_ENCODE_PAC(attributed, page, tier), .pid = pid};
        if (ring_buffer_pac_update_push(ctx->pac_update_ring, upd) == 0) {
            agg->dropped_events_update_full++;
            continue;
        }
        pac_count++;
    }

    agg->pushed_events_to_update += pac_count;

    agg->aggregation_cycles_total++;
    if ((uint64_t)pac_count > agg->aggregation_max_samples_cycle) {
        agg->aggregation_max_samples_cycle = pac_count;
    }

    return pac_count;
}

/* Reset per-cycle drop/lost counters at the top of each aggregation
 * iteration. */
static void reset_per_cycle_counters(pebs_aggregator_t *agg)
{
    agg->read_events_from_perf = 0;
    agg->pushed_events_to_update = 0;
    agg->dropped_events_update_full = 0;
    agg->dropped_events_workload = 0;
    agg->lost_samples = 0;
    agg->lost_events = 0;
}

/* Periodic check for workload exit. With per-PID inherit counting events
 * the kernel auto-tracks child threads, so a liveness probe on the
 * workload's TGID suffices. Polled every ~5s of PEBS cycles. */
static bool poll_and_update_threads(pact_context_t *ctx, int debug_counter)
{
    if ((debug_counter % 250) != 0) {
        return false;
    }
    bool any_alive = false;
    for (int i = 0; i < ctx->n_workloads; i++) {
        pact_workload_t *wl = &ctx->workloads[i];
        if (wl->target_pid <= 0 || wl->exited) {
            continue;
        }
        if (kill(wl->target_pid, 0) == 0) {
            any_alive = true;
            continue;
        }
        if (errno == ESRCH) {
            log_info("poll_and_update_threads", "Workload (PID %d) has exited", wl->target_pid);
            wl->exited = true;
        } else {
            any_alive = true; /* EPERM or other: treat as alive */
        }
    }
    return !any_alive;
}

static void log_workload_pebs_stats(pact_context_t *ctx, pebs_aggregator_t *agg)
{
    pact_workload_t *wl = ctx->workload;
    uint64_t theory =
        (wl->stats.llc_misses_fast + wl->stats.llc_misses_slow) / ctx->pebs_sampling_period;
    uint64_t processed = agg->events_per_tier[0] + agg->events_per_tier[1];
    log_pebs_aggregator(ctx, "pebs_sampling_thread", 0, theory, processed, agg->lost_events,
                        agg->lost_samples, 0, agg->dropped_events_workload);
    for (int i = 0; i < ctx->n_workloads; i++) {
        if (ctx->workloads[i].pc_class) {
            pc_class_log_stats(ctx->workloads[i].pc_class);
        }
    }
    if ((agg->aggregation_cycles_total % 250) == 0) {
        log_info("pebs_aggregator", "tier_pebs local=%lu remote=%lu", agg->tier_from_dsrc_fast,
                 agg->tier_from_dsrc_slow);
    }
}

/* Sole owner of PMU stop/read/start and PEBS mmap draining. */
void *pebs_sampling_thread(void *arg)
{
    pact_context_t *ctx = arg;
    if (ctx->monitor_cpu >= 0) {
        pact_pin_to_cpu(ctx->monitor_cpu);
    }

    /* Get aggregator from context */
    pebs_aggregator_t *agg = ctx->pebs_aggregator;
    if (!agg) {
        log_error("pebs_sampling_thread", "No aggregator context available");
        ctx->running = false;
        return NULL;
    }

    log_info("pebs_sampling_thread", "Starting with %d CPUs, mask=0x%lx", agg->num_cpus,
             agg->cpu_mask);

    int debug_counter = 0;
    heat_dump_init_once();

    while (ctx->running) {
        reset_per_cycle_counters(agg);
        /* Fresh per-tier sample counts for this window; phase A of
         * pebs_aggregate_events repopulates them from the drained samples
         * so attribution divides by this window's population. */
        agg->events_per_tier[0] = 0;
        agg->events_per_tier[1] = 0;
        memset(agg->events_per_tier_wl, 0, sizeof(agg->events_per_tier_wl));

        /* turn off pmu counters */
        stop_pmu_perf_events(ctx);

        /* read counting events, e.g. LLC stalls, CHA stats */
        read_pmu_counting_events(ctx);

        if (poll_and_update_threads(ctx, debug_counter)) {
            log_info("pebs_sampling_thread", "Workload has exited, stopping PACT");
            ctx->running = false;
            break;
        }

        /* reset and turn back on pmu counters before aggregation to minimize dead time */
        start_pmu_perf_events(ctx);

        /* Drain, attribute (same-window), and publish this window's samples */
        int aggregated = pebs_aggregate_events(agg, ctx);

        /* Accumulate per-workload PEBS samples into cumulative counters */
        for (int i = 0; i < ctx->n_workloads; i++) {
            ctx->workloads[i].total_pebs_samples +=
                agg->events_per_tier_wl[i][0] + agg->events_per_tier_wl[i][1];
        }

        /* Log workload PMU stats (after aggregation so event counts are available) */
        log_pmu(ctx, "pebs_sampling_thread", 0, ctx->workload->workload_mlp_fast,
                ctx->workload->workload_mlp_slow, ctx->workload->stats.llc_misses_fast,
                ctx->workload->stats.llc_misses_slow, agg->events_per_tier[0],
                agg->events_per_tier[1]);

        if (aggregated == 0) {
            /* No events available; still sleep with PMU enabled below. */
            /* Debug: Print periodically */
            if (++debug_counter % 1000 == 0) {
                log_debug("pebs_sampling_thread",
                          "PEBS aggregator: No events collected (attempt %d)", debug_counter);
            }
        } else {
            ctx->sample_counts += aggregated;
            ctx->workload->stats.pebs_events_processed += aggregated;
            log_debug("pebs_sampling_thread", "PEBS aggregator: Pushed %d events to PAC ring",
                      aggregated);
            debug_counter = 0;
        }

        log_workload_pebs_stats(ctx, agg);

        /* Leave PMU enabled while samples accumulate for the next window.
         * Resume an interrupted sleep unless shutdown was requested. */
        struct timespec delay = {
            .tv_sec = ctx->sampling_interval_ms / 1000,
            .tv_nsec = (long)(ctx->sampling_interval_ms % 1000) * 1000000L,
        };
        while (ctx->running && nanosleep(&delay, &delay) < 0 && errno == EINTR) {
        }
    }

    stop_pmu_perf_events(ctx);
    log_info("pebs_sampling_thread", "PEBS export summary: written=%lu considered=%lu "
             "stride=%lu lost_records=%lu lost_samples=%lu malformed=%lu staging_drops=%lu",
             g_heat_written, g_heat_ctr, g_heat_stride, agg->lost_records_total,
             agg->lost_samples_total, agg->malformed_records_total, agg->staging_drops_total);
    heat_dump_finish(agg);
    log_info("pebs_sampling_thread", "PEBS sampling thread: Exiting");
    return NULL;
}

/* Setup PEBS aggregator for unified context */
int setup_pebs_aggregator(pact_context_t *ctx, per_cpu_state_t *cpu_states, int num_cpus,
                          uint64_t cpu_mask)
{
    /* Create aggregator using per-CPU states */
    pebs_aggregator_t *agg = pebs_aggregator_create(cpu_states, num_cpus, cpu_mask);

    if (!agg) {
        log_error("setup_pebs_aggregator", "Failed to create PEBS aggregator");
        return -1;
    }

    /* Store back-reference to pact context (needed for CPU-to-workload mapping) */
    agg->pact_ctx = ctx;

    /* Store in context */
    ctx->pebs_aggregator = agg;

    /* Mark PEBS as available via aggregator */
    ctx->pebs_available = true;

    log_info("setup_pebs_aggregator", "PEBS filtering enabled for %d PID(s)", ctx->n_workloads);
    log_info("setup_pebs_aggregator", "PEBS aggregator setup complete");
    return 0;
}

/* Cleanup aggregator */
void pebs_aggregator_destroy(pebs_aggregator_t *agg)
{
    if (!agg) {
        return;
    }

    log_info("pebs_aggregator_destroy", "Destroying PEBS aggregator");

    free(agg->cycle_buf);
    free(agg->cpu_states);
    free(agg);
}
