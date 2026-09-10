/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <asm/unistd.h>
#include <sys/mman.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <assert.h>
#include <dirent.h>
#include <numa.h>
#include <numaif.h>
#include <time.h>

#include "constants.h"
#include "pact.h"
#include "pmu.h"
#include "perf.h"
#include "error.h"

/* Event configuration table for core/thread counting events.
 * Populated at runtime by pmu_platform_init(). */
event_config_t core_event_configs[CORE_EVENT_COUNT];

int validate_hardware_access(void)
{
    /* Initialize NUMA */
    if (numa_available() < 0) {
        log_warning("validate_hardware_access", "NUMA not available, migrations will be simulated");
        return 0;
    }

    /* Check if we have the necessary permissions for hardware counters */
    if (geteuid() != 0) {
        log_warning("validate_hardware_access",
                    "Running without root privileges - hardware counters may not be accessible");
        return 0; /* Not fatal, but degraded functionality */
    }

    /* Check if /proc/sys/kernel/perf_event_paranoid allows access */
    FILE *paranoid_file = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    if (paranoid_file) {
        int paranoid_level;
        if (fscanf(paranoid_file, "%d", &paranoid_level) == 1) {
            if (paranoid_level > 1) {
                log_warning("validate_hardware_access",
                            "perf_event_paranoid level is high - some counters may be restricted");
            }
        }
        fclose(paranoid_file);
    }

    return 1; /* Success */
}

long perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd,
                     unsigned long flags)
{
    return syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
}

/* Function to discover CHA PMUs for target CPUs only */
typedef struct {
    int cha_id;
    int pmu_type;
    char device_name[64];
} cha_discovery_t;

/* Zero-init every cha_pmu_info_t slot to "unused" sentinels (-1 IDs, -1 fds). */
static void cha_pmus_clear_slots(cha_pmu_info_t *cha_pmus)
{
    for (int i = 0; i < MAX_CHAS; i++) {
        cha_pmus[i].cha_id = -1;
        cha_pmus[i].pmu_type = -1;
        cha_pmus[i].core_id = -1;
        cha_pmus[i].device_name[0] = '\0';
        for (int j = 0; j < 4; j++) {
            cha_pmus[i].group_fast.fds[j] = -1;
            cha_pmus[i].group_slow.fds[j] = -1;
            cha_pmus[i].group_fast.ids[j] = -1;
            cha_pmus[i].group_slow.ids[j] = -1;
        }
    }
}

/* Read PMU type for a single uncore_cha_* device and record it into the
 * discovered[] buffer. Returns true on success. */
static bool record_cha_device(const char *dev_name, int cha_id, cha_discovery_t *out)
{
    char path[256];
    int ret = snprintf(path, sizeof(path), "/sys/devices/%s/type", dev_name);
    if (ret >= (int)sizeof(path)) {
        log_warning("detect_cha_pmus", "Path too long for device %s, skipping", dev_name);
        return false;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        exit(EXIT_FAILURE); /* preserved: legacy behavior on /sys read failure */
    }
    int pmu_type;
    bool ok = (fscanf(fp, "%d", &pmu_type) == 1);
    fclose(fp);
    if (!ok) {
        return false;
    }
    out->cha_id = cha_id;
    out->pmu_type = pmu_type;
    strncpy(out->device_name, dev_name, sizeof(out->device_name) - 1);
    out->device_name[sizeof(out->device_name) - 1] = '\0';
    return true;
}

/* Discover all uncore_cha_* devices under /sys/devices. Fills the discovered[]
 * buffer and returns count. */
static int discover_all_cha_devices(cha_discovery_t *discovered)
{
    DIR *dir = opendir("/sys/devices");
    if (!dir) {
        perror("Failed to open /sys/devices");
        return -1;
    }
    int n = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "uncore_cha_", 11) != 0) {
            continue;
        }
        int cha_id = atoi(entry->d_name + 11);
        if (cha_id < 0 || cha_id >= MAX_CHAS) {
            log_warning("detect_cha_pmus", "CHA ID %d is out of bounds (0-%d), skipping", cha_id,
                        MAX_CHAS - 1);
            exit(EXIT_FAILURE);
        }
        if (record_cha_device(entry->d_name, cha_id, &discovered[n])) {
            n++;
        }
    }
    closedir(dir);
    return n;
}

/* Find the smallest pmu_type — used as the origin for CHA-offset → core
 * mapping in g_pmu_platform.cha_to_core_map. */
static int find_base_pmu_type(const cha_discovery_t *discovered, int n)
{
    int base = INT_MAX;
    for (int i = 0; i < n; i++) {
        if (discovered[i].pmu_type < base) {
            base = discovered[i].pmu_type;
        }
    }
    return base;
}

/* Filter the discovered CHAs by cpu_mask and populate cha_pmus[] for the
 * active set. Returns count of active CHAs and writes count of skipped. */
static int filter_active_chas(const cha_discovery_t *discovered, int n_discovered,
                              int base_pmu_type, uint64_t cpu_mask, cha_pmu_info_t *cha_pmus,
                              int *out_skipped)
{
    int active = 0, skipped = 0;
    for (int i = 0; i < n_discovered; i++) {
        int cha_id = discovered[i].cha_id;
        int pmu_type = discovered[i].pmu_type;
        int cha_offset = pmu_type - base_pmu_type;

        if (cha_offset < 0 || cha_offset >= g_pmu_platform.nr_cha_mapping) {
            log_debug("detect_cha_pmus",
                      "CHA %d: offset %d out of range (only %d mapped), skipping", cha_id,
                      cha_offset, g_pmu_platform.nr_cha_mapping);
            skipped++;
            continue;
        }
        int core_id = g_pmu_platform.cha_to_core_map[cha_offset];
        if (core_id >= 0 && cpu_mask != 0 && !(cpu_mask & (1ULL << core_id))) {
            log_debug("detect_cha_pmus", "Skipping CHA %d (core %d not in target CPU mask 0x%lx)",
                      cha_id, core_id, cpu_mask);
            skipped++;
            continue;
        }

        cha_pmus[active].cha_id = cha_id;
        cha_pmus[active].pmu_type = pmu_type;
        cha_pmus[active].core_id = core_id;
        strncpy(cha_pmus[active].device_name, discovered[i].device_name,
                sizeof(cha_pmus[active].device_name) - 1);
        cha_pmus[active].device_name[sizeof(cha_pmus[active].device_name) - 1] = '\0';
        log_info("detect_cha_pmus", "Added CHA %d: %s (PMU type %d, core %d) at index %d", cha_id,
                 discovered[i].device_name, pmu_type, core_id, active);
        active++;
    }
    *out_skipped = skipped;
    return active;
}

int discover_cha_pmus(cha_pmu_info_t *cha_pmus, int *nr_cha, uint64_t cpu_mask)
{
    cha_pmus_clear_slots(cha_pmus);

    cha_discovery_t discovered[MAX_CHAS];
    int n_discovered = discover_all_cha_devices(discovered);
    if (n_discovered < 0) {
        return -1;
    }
    if (n_discovered == 0) {
        log_info("detect_cha_pmus", "No CHA PMUs discovered");
        *nr_cha = 0;
        return 0;
    }
    log_info("detect_cha_pmus", "Total CHA PMUs discovered: %d", n_discovered);

    int base = find_base_pmu_type(discovered, n_discovered);
    int skipped = 0;
    int active = filter_active_chas(discovered, n_discovered, base, cpu_mask, cha_pmus, &skipped);
    *nr_cha = active;
    if (skipped > 0) {
        log_info("detect_cha_pmus", "Active CHA PMUs: %d (skipped %d CHAs not in target CPU mask)",
                 active, skipped);
    } else {
        log_info("detect_cha_pmus", "Active CHA PMUs: %d", active);
    }
    return active;
}

int setup_tor_events(event_group_t *event_group, int pmu_type, int tier, int core_id)
{
    const pmu_platform_t *plat = &g_pmu_platform;
    static const char *event_names[CHA_EVENT_COUNT] = {"TOR_OCCUPANCY", "TOR_CYCLES"};
    int leader_fd = -1;

    for (int i = 0; i < CHA_EVENT_COUNT; i++) {
        struct perf_event_attr pe;
        memset(&pe, 0, sizeof(pe));
        pe.size = sizeof(pe);
        pe.type = pmu_type;
        pe.sample_type = PERF_SAMPLE_IDENTIFIER;
        pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                         PERF_FORMAT_GROUP | PERF_FORMAT_ID;

        /* All config composition delegated to the platform */
        tor_pe_config_t cfg;
        plat->fill_tor_config(plat, &cfg, i, tier, core_id);
        pe.config = cfg.config;
        pe.config1 = cfg.config1;
        pe.config2 = cfg.config2;

        pe.disabled = (i == CHA_TOR_OCCUPANCY) ? 1 : 0;
        pe.exclude_kernel = 0;
        pe.exclude_hv = 0;
        pe.exclude_idle = 0;
        pe.inherit = 1;

        int group_fd = (i == CHA_TOR_OCCUPANCY) ? -1 : leader_fd;
        int fd = perf_event_open(&pe, -1, 0, group_fd, 0);

        if (fd == -1) {
            log_warning("setup_tor_events", "Error opening %s event (tier %d): %s", event_names[i],
                        tier, strerror(errno));
            return -1;
        }

        ioctl(fd, PERF_EVENT_IOC_ID, &event_group->ids[i]);
        event_group->fds[i] = fd;
        event_group->counters_used++;

        if (i == CHA_TOR_OCCUPANCY) {
            leader_fd = fd;
        }
    }

    return 0;
}

/* Setup perf events for active CHAs (already filtered by discover_cha_pmus) */
static void close_event_group_fds(event_group_t *g)
{
    for (int j = 0; j < g->counters_used; j++) {
        if (g->fds[j] != -1) {
            close(g->fds[j]);
            g->fds[j] = -1;
        }
    }
}

/* Cleanup partially-opened TOR event fds for one CHA after setup failure.
 * Fixes a prior bug where both loops indexed group_slow.fds, leaking the
 * group_fast fds when slow-group setup failed after fast-group success. */
static void cleanup_partial_cha_setup(cha_pmu_info_t *cha)
{
    close_event_group_fds(&cha->group_fast);
    close_event_group_fds(&cha->group_slow);
}

/* Open the fast (tier 0) and slow (tier 1) TOR event groups for one CHA.
 * On any group failure, both groups' fds are closed via cleanup helper.
 * Returns the number of events successfully opened (0 on failure). */
static int setup_one_cha_tor_groups(cha_pmu_info_t *cha)
{
    int fast_ok = setup_tor_events(&cha->group_fast, cha->pmu_type, 0, cha->core_id) == 0;
    int slow_ok =
        fast_ok && setup_tor_events(&cha->group_slow, cha->pmu_type, 1, cha->core_id) == 0;
    if (slow_ok) {
        return cha->group_fast.counters_used + cha->group_slow.counters_used;
    }
    fprintf(stderr, "  Failed to setup TOR events for CHA %d, skipping\n", cha->cha_id);
    cleanup_partial_cha_setup(cha);
    return 0;
}

int setup_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int *nr_cha)
{
    int total_events = 0;
    int active_cha_count = 0;

    for (int i = 0; i < *nr_cha; i++) {
        log_info("setup_uncore_events", "Setting up CHA %d (%s, PMU type %d, core %d)",
                 cha_pmus[i].cha_id, cha_pmus[i].device_name, cha_pmus[i].pmu_type,
                 cha_pmus[i].core_id);
        int n = setup_one_cha_tor_groups(&cha_pmus[i]);
        if (n > 0) {
            active_cha_count++;
            total_events += n;
        }
    }
    log_info("setup_uncore_events", "Successfully opened %d individual events across %d CHAs",
             total_events, active_cha_count);
    return total_events;
}

/* manipulate CHA PMU counters */
void ioctl_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha, int request)
{
    for (int i = 0; i < nr_cha; i++) {
        ioctl(cha_pmus[i].group_fast.fds[0], request, PERF_IOC_FLAG_GROUP);
        ioctl(cha_pmus[i].group_slow.fds[0], request, PERF_IOC_FLAG_GROUP);
    }
}

/* manipulate per-cpu PMU counters, includes fast/slow tier pebs, and counting events if no pid is specified */
void ioctl_pmu_core_perf_events(per_cpu_state_t *cpu_states, int nr_target_cpus, int request)
{
    for (int i = 0; i < nr_target_cpus; i++) {
        ioctl(cpu_states[i].leader.fd, request, PERF_IOC_FLAG_GROUP);
    }
}

void start_pmu_perf_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_RESET);
    ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_RESET);
    if (wl->counting_leader.fd >= 0) {
        ioctl(wl->counting_leader.fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    }
    ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_ENABLE);
    if (wl->counting_leader.fd >= 0) {
        ioctl(wl->counting_leader.fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_ENABLE);
}

void stop_pmu_perf_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;
    ioctl_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha, PERF_EVENT_IOC_DISABLE);
    if (wl->counting_leader.fd >= 0) {
        ioctl(wl->counting_leader.fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    }
    ioctl_pmu_core_perf_events(ctx->cpu_states, ctx->nr_all_cpus, PERF_EVENT_IOC_DISABLE);
}

void read_pmu_cha_perf_events(cha_pmu_info_t *cha_pmus, int nr_cha)
{
    for (int i = 0; i < nr_cha; i++) {
        read_pmu_event_group(&cha_pmus[i].group_fast);
        read_pmu_event_group(&cha_pmus[i].group_slow);
        /* scale readings with time_enabled / time_running */
        scale_multiplexed_events(&cha_pmus[i].group_fast);
        scale_multiplexed_events(&cha_pmus[i].group_slow);
    }
}

/* Read a perf event group via leader_fd. Matches the N values returned by
 * the kernel to the caller's expected event IDs and writes them into
 * values[]; unmatched slots are left zeroed. Returns bytes read (>0 on
 * success, 0 on empty, <0 on error). */
static int read_perf_event_group_raw(int leader_fd, const uint64_t *ids, uint64_t *values, int n,
                                     uint64_t *out_time_enabled, uint64_t *out_time_running)
{
    char buf[4096];
    read_format_t *rf = (read_format_t *)buf;
    int bytes_read = read(leader_fd, buf, sizeof(buf));
    if (bytes_read <= 0) {
        return bytes_read;
    }
    for (int j = 0; j < n; j++) {
        values[j] = 0;
    }
    for (uint64_t i = 0; i < rf->nr; i++) {
        for (int j = 0; j < n; j++) {
            if (rf->values[i].id == ids[j]) {
                values[j] = rf->values[i].value;
                break;
            }
        }
    }
    if (out_time_enabled) {
        *out_time_enabled = rf->time_enabled;
    }
    if (out_time_running) {
        *out_time_running = rf->time_running;
    }
    return bytes_read;
}

/* Read a perf_event_t[N] group (per_cpu_state_t / pact_workload_t shape):
 * gather ids, call the raw helper, scatter values back. */
static int read_perf_event_array(perf_event_t *leader, perf_event_t *events, int n)
{
    if (leader->fd < 0) {
        return -1;
    }
    uint64_t ids[CORE_EVENT_COUNT];
    uint64_t values[CORE_EVENT_COUNT];
    for (int j = 0; j < n; j++) {
        ids[j] = events[j].id;
    }
    int bytes_read = read_perf_event_group_raw(leader->fd, ids, values, n, &leader->time_enabled,
                                               &leader->time_running);
    if (bytes_read <= 0) {
        return bytes_read;
    }
    for (int j = 0; j < n; j++) {
        events[j].value = (events[j].fd >= 0) ? values[j] : 0;
    }
    return bytes_read;
}

/* Read the workload's per-PID counting group (single fd, all events in
 * one read). Counts are kernel-aggregated across all threads of the
 * workload via inherit=1. */
static void read_workload_counting_events(pact_workload_t *wl)
{
    int bytes_read =
        read_perf_event_array(&wl->counting_leader, wl->counting_events, CORE_EVENT_COUNT);
    if (bytes_read <= 0 && wl->counting_leader.fd >= 0) {
        log_warning("read_workload_counting_events",
                    "Failed to read workload PID %d: bytes_read=%d, errno=%s", wl->target_pid,
                    bytes_read, strerror(errno));
    }
}

int setup_dummy_leader_event(perf_event_t *perf_event, pid_t pid, int cpu)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_SOFTWARE;
    pe.config = PERF_COUNT_SW_DUMMY;
    pe.size = sizeof(pe);
    pe.disabled = 1; /* ONLY group leader start disabled*/
    pe.inherit = 1;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    perf_event->fd = perf_event_open(&pe, pid, cpu, -1, 0);
    if (perf_event->fd == -1) {
        log_error("setup_dummy_leader_event", "perf_event_open(pid=%d, cpu=%d) failed: %s", pid,
                  cpu, strerror(errno));
        return -1;
    }

    return 0;
}

/* Open one PEBS sampling event (LOCAL=0 or REMOTE=1) and mmap its ring. */
static int setup_one_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu, int tier)
{
    struct perf_event_attr pe;
    memset(&pe, 0, sizeof(pe));

    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.sample_period = cpu_state->pebs_sampling_period;
    pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID | PERF_SAMPLE_ADDR;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.exclude_idle = 1;
    pe.mmap = 1;
    pe.precise_ip = 2;
    pe.inherit = 1;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    pe.config = (tier == 0) ? g_pmu_platform.event_llc_miss_local
                            : g_pmu_platform.event_llc_miss_remote;

    cpu_state->fd_pebs[tier] = perf_event_open(&pe, pid, cpu, cpu_state->leader.fd, 0);
    if (cpu_state->fd_pebs[tier] < 0) {
        if (errno == EACCES) {
            log_error("setup_pebs_sampling",
                      "Permission denied for PEBS - run as root or adjust perf_event_paranoid");
        } else if (errno == ENODEV) {
            log_error("setup_pebs_sampling", "PEBS not supported on this hardware");
        } else {
            log_error("setup_pebs_sampling", "Failed to open PEBS %s (0x%llx): %s",
                      tier == 0 ? "LOCAL_DRAM" : "REMOTE_DRAM",
                      (unsigned long long)pe.config, strerror(errno));
        }
        return -1;
    }

    size_t mmap_size = (1 + PERF_BUFFER_PAGES) * PAGE_SIZE;
    cpu_state->pebs_mmap[tier] =
        mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, cpu_state->fd_pebs[tier], 0);
    if (cpu_state->pebs_mmap[tier] == MAP_FAILED) {
        log_error("setup_pebs_sampling", "Failed to mmap PEBS %s buffer",
                  tier == 0 ? "LOCAL" : "REMOTE");
        safe_close(cpu_state->fd_pebs[tier], "setup_pebs_sampling");
        cpu_state->fd_pebs[tier] = -1;
        cpu_state->pebs_mmap[tier] = NULL;
        return -1;
    }
    return 0;
}

/* Setup PEBS: separate LOCAL and REMOTE L3-miss streams. Sample tier is
 * which fd overflowed — DATA_SRC on the OR'd 0x03d3 event is NA on SKX. */
int setup_pebs_event(per_cpu_state_t *cpu_state, pid_t pid, int cpu)
{
    if (setup_one_pebs_event(cpu_state, pid, cpu, 0) < 0) {
        return -1;
    }
    if (setup_one_pebs_event(cpu_state, pid, cpu, 1) < 0) {
        if (cpu_state->pebs_mmap[0] && cpu_state->pebs_mmap[0] != MAP_FAILED) {
            munmap(cpu_state->pebs_mmap[0], (1 + PERF_BUFFER_PAGES) * PAGE_SIZE);
        }
        safe_close(cpu_state->fd_pebs[0], "setup_pebs_sampling");
        cpu_state->fd_pebs[0] = -1;
        cpu_state->pebs_mmap[0] = NULL;
        return -1;
    }
    log_info("setup_pebs_sampling",
             "CPU [%d] PEBS local fd:%d (0x01d3) remote fd:%d (0x02d3)", cpu,
             cpu_state->fd_pebs[0], cpu_state->fd_pebs[1]);
    return 0;
}

int setup_counting_event(perf_event_t *perf_event, pid_t pid, int cpu, perf_event_t *leader,
                         uint64_t config, const char *name)
{
    struct perf_event_attr pe;

    memset(&pe, 0, sizeof(pe));
    pe.type = PERF_TYPE_RAW;
    pe.size = sizeof(pe);
    pe.config = config;                      /* event config */
    pe.sample_type = PERF_SAMPLE_IDENTIFIER; /* key for counting mode */
    pe.sample_period = 0;
    pe.exclude_kernel = 1;
    pe.exclude_hv = 1;
    pe.inherit = 1;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING |
                     PERF_FORMAT_GROUP | PERF_FORMAT_ID;

    /* CRITICAL: Leader must start disabled, members inherit this */
    if (leader == NULL) {
        pe.disabled = 1; /* This is the leader - start disabled */
    } else {
        pe.disabled = 0; /* Group member - will follow leader's state */
    }

    perf_event->fd = perf_event_open(&pe, pid, cpu, (leader == NULL ? -1 : leader->fd), 0x8);
    if (perf_event->fd < 0) {
        log_error("setup_counting_event", "Failed to open counting event config=0x%llx", config);
        perf_event->fd = -1;
        exit(EXIT_FAILURE);
    } else {
        log_info("setup_counting_event", "thread [%d] counting event %s (config=0x%llx) fd:%d", pid,
                 name, config, perf_event->fd);
    }
    ioctl(perf_event->fd, PERF_EVENT_IOC_ID, &perf_event->id);

    return 0;
}


/* Read an event group */
int read_pmu_event_group(event_group_t *event_group)
{
    if (event_group->fds[0] < 0) {
        return -1; /* Group not available */
    }
    uint64_t te = event_group->time_enabled, tr = event_group->time_running;
    int bytes_read =
        read_perf_event_group_raw(event_group->fds[0], event_group->ids, event_group->values,
                                  event_group->counters_used, &te, &tr);
    if (bytes_read > 0) {
        event_group->last_time_enabled = event_group->time_enabled;
        event_group->last_time_running = event_group->time_running;
        event_group->time_enabled = te;
        event_group->time_running = tr;
    }
    return 0;
}

void scale_multiplexed_events(event_group_t *event_group)
{
    double scale_factor = 1.0;
    if (event_group->time_running - event_group->last_time_running == 0) {
        return; /* No change in time_running, no scaling needed */
    }
    scale_factor = (double)(event_group->time_enabled - event_group->last_time_enabled) /
                   (double)(event_group->time_running - event_group->last_time_running);
    for (int i = 0; i < event_group->counters_used; i++) {
        event_group->values[i] = (uint64_t)((double)event_group->values[i] * scale_factor);
    }
}

/*
 * Per-tier MLP from CHA TOR counters (Algorithm 1): MLP = ΔT1/ΔT2, where
 * T1 accumulates TOR occupancy and T2 counts cycles with at least one
 * outstanding TOR entry. Occupancy and cycle deltas are summed across all
 * of the workload's CHAs before dividing, so each tier yields one ratio
 * per sampling window. Counters are IOC_RESET at window start, so group
 * values are true window deltas; each group's values were already
 * multiplex-corrected by scale_multiplexed_events(), so summing across
 * groups with different schedule fractions is sound.
 */
static double calculate_tier_mlp(pact_workload_t *wl, int tier)
{
    uint64_t sum_occupancy = 0;
    uint64_t sum_cycles = 0;
    int valid_groups = 0;

    for (int cha = 0; cha < wl->nr_cha; cha++) {
        event_group_t *group =
            (tier == 0) ? &wl->cha_pmus[cha].group_fast : &wl->cha_pmus[cha].group_slow;

        /* Skip groups scheduled for under 1 ms of THIS window; their
         * scaled-up readings are noise. time_running is cumulative since
         * open (IOC_RESET clears only counter values), so the window's
         * share is the delta against the previous read. */
        if (group->time_running - group->last_time_running < 1000000) {
            continue;
        }
        valid_groups++;
        sum_occupancy += group->values[CHA_TOR_OCCUPANCY];
        sum_cycles += group->values[CHA_TOR_CYCLES];
    }

    if (valid_groups == 0) {
        return -1.0; /* no valid measurement this window (all multiplexed out) */
    }
    if (sum_occupancy == 0 || sum_cycles == 0) {
        return g_pmu_platform.mlp_min; /* measured, but no traffic to this tier */
    }

    double mlp = (double)sum_occupancy / (double)sum_cycles;
    if (mlp < g_pmu_platform.mlp_min) {
        mlp = g_pmu_platform.mlp_min;
    }
    if (mlp > g_pmu_platform.mlp_max) {
        mlp = g_pmu_platform.mlp_max;
    }
    return mlp;
}

/* Fresh per-tier MLP ratios for this window; no cross-window smoothing.
 * A window with no valid measurement (every CHA group multiplexed out)
 * keeps the previous window's value instead of fabricating the minimum,
 * which would over-attribute stalls for that window. */
static void calculate_workload_mlp(pact_workload_t *wl)
{
    double fast = calculate_tier_mlp(wl, 0);
    double slow = calculate_tier_mlp(wl, 1);

    if (fast >= 0.0) {
        wl->workload_mlp_fast = fast;
    }
    if (slow >= 0.0) {
        wl->workload_mlp_slow = slow;
    }
}

/* Read the per-PID counting group (single fd with inherit=1, aggregating
 * across all child threads of the workload) and roll up into workload
 * stats. MLP is computed from CHA TOR ratios (Algorithm 1). */
void read_pmu_counting_events(pact_context_t *ctx)
{
    pact_workload_t *wl = ctx->workload;

    read_workload_counting_events(wl);
    uint64_t wl_fast = wl->counting_events[CORE_EVENT_LLC_MISS_FAST].value;
    uint64_t wl_slow = wl->counting_events[CORE_EVENT_LLC_MISS_SLOW].value;
    log_trace("read_pmu_counting_events", "Workload LLC misses fast=%lu, slow=%lu", wl_fast,
              wl_slow);

    wl->stats.llc_misses_fast = wl_fast;
    wl->stats.llc_misses_slow = wl_slow;
    if (wl->counting_leader.fd >= 0) {
        wl->stats.time_running = wl->counting_leader.time_running - wl->stats.last_time_running;
        wl->stats.last_time_running = wl->counting_leader.time_running;
    }

    read_pmu_cha_perf_events(wl->cha_pmus, wl->nr_cha);
    calculate_workload_mlp(wl);
    log_debug("read_pmu_counting_events", "Workload (PID %d) MLP: fast=%.2f, slow=%.2f",
              wl->target_pid, wl->workload_mlp_fast, wl->workload_mlp_slow);
}

/* Open one counting group per workload: dummy SW leader + LLC-miss events,
 * all on (pid=target_pid, cpu=-1, inherit=1, mmap=0). Kernel auto-attributes
 * counts across every thread of the workload via inherit. Replaces the
 * per-TID setup that needed /proc/<pid>/task polling. */
void setup_workload_counting_events(pact_workload_t *wl)
{
    init_perf_event(&wl->counting_leader);
    for (int j = 0; j < CORE_EVENT_COUNT; j++) {
        init_perf_event(&wl->counting_events[j]);
    }

    if (setup_dummy_leader_event(&wl->counting_leader, wl->target_pid, -1) < 0) {
        log_error("setup_workload_counting_events",
                  "Failed to create counting leader for workload PID %d", wl->target_pid);
        return;
    }
    for (int j = 0; j < CORE_EVENT_COUNT; j++) {
        if (setup_counting_event(&wl->counting_events[j], wl->target_pid, -1, &wl->counting_leader,
                                 core_event_configs[j].config, core_event_configs[j].name) < 0) {
            log_error("setup_workload_counting_events",
                      "Failed to setup %s event for workload PID %d", core_event_configs[j].name,
                      wl->target_pid);
            return;
        }
    }
    log_info("setup_workload_counting_events",
             "Workload PID %d: per-workload counting events setup (1 fd per event, inherit=1)",
             wl->target_pid);
}
