/* SPDX-License-Identifier: MIT */

#include "pc_class.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "error.h"
#include "pact.h"

#define PC_CLASS_MAP_MIN_CAP 1024u

struct pc_class_entry {
    uint64_t offset;
    uint8_t class_id; /* enum pc_class_id */
    uint8_t used;
};

struct pc_class_state {
    bool enabled;
    bool weights_loaded;
    bool map_loaded;

    double weights[PC_CLASS_COUNT];

    uint64_t text_base;
    uint64_t text_end;
    char exe_path[512];

    struct pc_class_entry *table;
    uint32_t capacity; /* power of two */
    uint32_t size;

    /* Diagnostics */
    uint64_t lookups;
    uint64_t hits;
    uint64_t miss_oob;   /* IP outside text mapping */
    uint64_t miss_table; /* in text but not in map → C4 */
    uint64_t saturations;
    uint64_t last_logged_lookups; /* rate-limit pc_class_log_stats */
};

static const char *const CLASS_NAMES[PC_CLASS_COUNT] = {
    "C1_latency",
    "C2_stream",
    "C3_hot_l1",
    "C4_other",
};

static int class_id_from_name(const char *name)
{
    for (int i = 0; i < PC_CLASS_COUNT; i++) {
        if (strcmp(name, CLASS_NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static uint32_t next_pow2(uint32_t n)
{
    uint32_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p < PC_CLASS_MAP_MIN_CAP ? PC_CLASS_MAP_MIN_CAP : p;
}

/* SplitMix64-ish mix for 64-bit offsets. */
static uint32_t hash_offset(uint64_t offset, uint32_t mask)
{
    uint64_t x = offset + 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    x = x ^ (x >> 31);
    return (uint32_t)x & mask;
}

pc_class_state_t *pc_class_create(void)
{
    pc_class_state_t *st = calloc(1, sizeof(*st));
    if (!st) {
        return NULL;
    }
    /* Neutral defaults matching pc_driven until a weights file is loaded. */
    st->weights[PC_CLASS_C1_LATENCY] = 1.0;
    st->weights[PC_CLASS_C2_STREAM] = 1.0;
    st->weights[PC_CLASS_C3_HOT_L1] = 1.0;
    st->weights[PC_CLASS_C4_OTHER] = 1.0;
    return st;
}

void pc_class_destroy(pc_class_state_t *st)
{
    if (!st) {
        return;
    }
    free(st->table);
    free(st);
}

bool pc_class_enabled(const pc_class_state_t *st)
{
    return st && st->enabled;
}

void pc_class_set_text_base(pc_class_state_t *st, uint64_t base, uint64_t end)
{
    if (!st) {
        return;
    }
    st->text_base = base;
    st->text_end = end;
}

static int ensure_table(pc_class_state_t *st, uint32_t min_cap)
{
    uint32_t cap = next_pow2(min_cap);
    if (st->table && st->capacity >= cap) {
        return 0;
    }
    struct pc_class_entry *nt = calloc(cap, sizeof(*nt));
    if (!nt) {
        return -1;
    }
    if (st->table) {
        uint32_t old_mask = st->capacity - 1;
        for (uint32_t i = 0; i < st->capacity; i++) {
            if (!st->table[i].used) {
                continue;
            }
            uint32_t j = hash_offset(st->table[i].offset, cap - 1);
            while (nt[j].used) {
                j = (j + 1) & (cap - 1);
            }
            nt[j] = st->table[i];
            (void)old_mask;
        }
        free(st->table);
    }
    st->table = nt;
    st->capacity = cap;
    return 0;
}

static int map_insert(pc_class_state_t *st, uint64_t offset, uint8_t class_id)
{
    if (!st->table || st->size * 10 >= st->capacity * 7) {
        if (ensure_table(st, st->capacity ? st->capacity * 2 : PC_CLASS_MAP_MIN_CAP) != 0) {
            return -1;
        }
    }
    uint32_t mask = st->capacity - 1;
    uint32_t i = hash_offset(offset, mask);
    for (;;) {
        if (!st->table[i].used) {
            st->table[i].used = 1;
            st->table[i].offset = offset;
            st->table[i].class_id = class_id;
            st->size++;
            return 0;
        }
        if (st->table[i].offset == offset) {
            st->table[i].class_id = class_id; /* last write wins */
            return 0;
        }
        i = (i + 1) & mask;
    }
}

static int map_lookup(const pc_class_state_t *st, uint64_t offset, uint8_t *out_class)
{
    if (!st->table || st->capacity == 0) {
        return -1;
    }
    uint32_t mask = st->capacity - 1;
    uint32_t i = hash_offset(offset, mask);
    for (uint32_t n = 0; n < st->capacity; n++) {
        if (!st->table[i].used) {
            return -1;
        }
        if (st->table[i].offset == offset) {
            *out_class = st->table[i].class_id;
            return 0;
        }
        i = (i + 1) & mask;
    }
    return -1;
}

/* Extract a JSON number immediately after `"key"` (simple, no nesting walk). */
static int json_find_number_after_key(const char *buf, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    while (*p && (isspace((unsigned char)*p) || *p == ':' || *p == ',')) {
        p++;
    }
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) {
        return -1;
    }
    *out = v;
    return 0;
}

static int load_weights_from_json(pc_class_state_t *st, const char *buf)
{
    /* Prefer the nested "weights" object if present. */
    const char *section = strstr(buf, "\"weights\"");
    const char *scan = section ? section : buf;
    int found = 0;
    for (int i = 0; i < PC_CLASS_COUNT; i++) {
        double v;
        if (json_find_number_after_key(scan, CLASS_NAMES[i], &v) == 0) {
            st->weights[i] = v;
            found++;
        }
    }
    return found > 0 ? 0 : -1;
}

static int load_weights_from_lines(pc_class_state_t *st, FILE *fp)
{
    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#' || *p == '{') {
            continue;
        }
        char name[64];
        double v;
        if (sscanf(p, "%63s %lf", name, &v) == 2) {
            int id = class_id_from_name(name);
            if (id >= 0) {
                st->weights[id] = v;
                found++;
            }
        }
    }
    return found > 0 ? 0 : -1;
}

int pc_class_load_weights(pc_class_state_t *st, const char *path)
{
    if (!st || !path || !path[0]) {
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_error("pc_class_load_weights", "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz < 0 || sz > 8 * 1024 * 1024) {
        fclose(fp);
        log_error("pc_class_load_weights", "Weights file too large or unreadable: %s", path);
        return -1;
    }
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    buf[n] = '\0';
    fclose(fp);

    int rc;
    if (strchr(buf, '{')) {
        rc = load_weights_from_json(st, buf);
    } else {
        FILE *mem = fmemopen(buf, n, "r");
        if (!mem) {
            free(buf);
            return -1;
        }
        rc = load_weights_from_lines(st, mem);
        fclose(mem);
    }
    free(buf);

    if (rc != 0) {
        log_error("pc_class_load_weights", "Failed to parse weights from %s", path);
        return -1;
    }
    st->weights_loaded = true;
    log_info("pc_class_load_weights",
             "Loaded class weights: C1=%.3f C2=%.3f C3=%.3f C4=%.3f from %s",
             st->weights[0], st->weights[1], st->weights[2], st->weights[3], path);
    return 0;
}

int pc_class_load_map(pc_class_state_t *st, const char *path)
{
    if (!st || !path || !path[0]) {
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_error("pc_class_load_map", "Cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (ensure_table(st, PC_CLASS_MAP_MIN_CAP) != 0) {
        fclose(fp);
        return -1;
    }

    char line[256];
    uint32_t nlines = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0' || *p == '#') {
            continue;
        }
        char off_s[64], cls_s[64];
        if (sscanf(p, "%63s %63s", off_s, cls_s) != 2) {
            continue;
        }
        char *end = NULL;
        uint64_t offset = strtoull(off_s, &end, 0);
        if (end == off_s) {
            continue;
        }
        int id = class_id_from_name(cls_s);
        if (id < 0) {
            /* Allow numeric class ids 0..3 */
            if (cls_s[0] >= '0' && cls_s[0] <= '3' && cls_s[1] == '\0') {
                id = cls_s[0] - '0';
            } else {
                log_warning("pc_class_load_map", "Unknown class '%s' (offset %s); skipping",
                            cls_s, off_s);
                continue;
            }
        }
        if (map_insert(st, offset, (uint8_t)id) != 0) {
            fclose(fp);
            return -1;
        }
        nlines++;
    }
    fclose(fp);

    if (nlines == 0) {
        log_error("pc_class_load_map", "No entries loaded from %s", path);
        return -1;
    }
    st->map_loaded = true;
    log_info("pc_class_load_map", "Loaded %u PC→class entries from %s", nlines, path);
    return 0;
}

static int read_exe_path(pid_t pid, char *out, size_t out_sz)
{
    char link[64];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(link, out, out_sz - 1);
    if (n < 0) {
        return -1;
    }
    out[n] = '\0';
    return 0;
}

/*
 * Find the first r-xp mapping whose pathname matches the workload exe
 * (exact or basename). Sets text_base/text_end.
 */
static int resolve_text_mapping(pc_class_state_t *st, pid_t pid)
{
    if (read_exe_path(pid, st->exe_path, sizeof(st->exe_path)) != 0) {
        log_error("pc_class_enable_for_pid", "readlink /proc/%d/exe failed: %s", (int)pid,
                  strerror(errno));
        return -1;
    }

    const char *base_name = strrchr(st->exe_path, '/');
    base_name = base_name ? base_name + 1 : st->exe_path;

    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    FILE *fp = fopen(maps_path, "r");
    if (!fp) {
        log_error("pc_class_enable_for_pid", "Cannot open %s: %s", maps_path, strerror(errno));
        return -1;
    }

    char line[768];
    uint64_t best_base = 0, best_end = 0;
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        uint64_t start = 0, end = 0;
        char perms[8] = {0};
        char path[512] = {0};
        /* address perms offset dev inode pathname */
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %511[^\n]", &start, &end, perms, path);
        if (n < 3) {
            continue;
        }
        if (strncmp(perms, "r-xp", 4) != 0 && strncmp(perms, "r-x", 3) != 0) {
            continue;
        }
        if (n < 4 || path[0] == '\0' || path[0] == '[') {
            continue;
        }
        /* trim leading spaces from path (sscanf may keep them) */
        char *pp = path;
        while (*pp == ' ') {
            pp++;
        }
        bool match = (strcmp(pp, st->exe_path) == 0);
        if (!match) {
            const char *bn = strrchr(pp, '/');
            bn = bn ? bn + 1 : pp;
            match = (strcmp(bn, base_name) == 0);
        }
        if (!match) {
            continue;
        }
        /* Prefer the lowest-address RX mapping (typical ELF text). */
        if (!found || start < best_base) {
            best_base = start;
            best_end = end;
            found = true;
        }
    }
    fclose(fp);

    if (!found) {
        log_error("pc_class_enable_for_pid",
                  "No r-xp mapping for exe '%s' in /proc/%d/maps", st->exe_path, (int)pid);
        return -1;
    }

    pc_class_set_text_base(st, best_base, best_end);
    log_info("pc_class_enable_for_pid",
             "text_base=0x%lx text_end=0x%lx exe=%s", (unsigned long)st->text_base,
             (unsigned long)st->text_end, st->exe_path);
    return 0;
}

int pc_class_enable_for_pid(pc_class_state_t *st, pid_t pid)
{
    if (!st) {
        return -1;
    }
    if (!st->weights_loaded || !st->map_loaded) {
        st->enabled = false;
        return 0;
    }
    if (pid <= 0) {
        log_error("pc_class_enable_for_pid", "Invalid pid %d", (int)pid);
        return -1;
    }
    if (resolve_text_mapping(st, pid) != 0) {
        st->enabled = false;
        return -1;
    }
    st->enabled = true;
    log_info("pc_class_enable_for_pid",
             "PC-class scaling ENABLED (%u map entries, %d classes)", st->size,
             PC_CLASS_COUNT);
    return 0;
}

void pc_class_log_stats(const pc_class_state_t *st)
{
    if (!st || !st->enabled) {
        return;
    }
    /* Hot path calls this every PEBS window; print at most every 50k lookups. */
    pc_class_state_t *mut = (pc_class_state_t *)st;
    if (st->last_logged_lookups != 0 &&
        st->lookups - st->last_logged_lookups < 50000) {
        return;
    }
    mut->last_logged_lookups = st->lookups ? st->lookups : 1;
    double hit_rate = st->lookups ? (100.0 * (double)st->hits / (double)st->lookups) : 0.0;
    log_info("pc_class_stats",
             "lookups=%lu hits=%lu (%.1f%%) miss_oob=%lu miss_table=%lu saturations=%lu "
             "text_base=0x%lx",
             (unsigned long)st->lookups, (unsigned long)st->hits, hit_rate,
             (unsigned long)st->miss_oob, (unsigned long)st->miss_table,
             (unsigned long)st->saturations, (unsigned long)st->text_base);
}

void pc_class_log_stats_final(pc_class_state_t *st)
{
    if (!st) {
        return;
    }
    st->last_logged_lookups = 0;
    pc_class_log_stats(st);
}

/* Per-sample score unit for pure-PC mode: score = w_c * this. The absolute
 * value only sets binning resolution (ranking is scale-invariant); 1024 keeps
 * C3 (w=0.05 -> ~51) nonzero and C1 (w~16 -> ~16k) well inside 18-bit PAC. */
#define PC_CLASS_SCORE_UNIT 1024.0

/* Resolve ip -> class weight, updating lookup/hit/miss diagnostics. */
static double pc_class_weight_for_ip(pc_class_state_t *st, uint64_t ip)
{
    st->lookups++;
    uint8_t class_id = PC_CLASS_C4_OTHER;
    if (ip < st->text_base || ip >= st->text_end) {
        st->miss_oob++;
    } else {
        uint64_t offset = ip - st->text_base;
        uint8_t found;
        if (map_lookup(st, offset, &found) == 0) {
            class_id = found;
            st->hits++;
        } else {
            st->miss_table++;
        }
    }
    return st->weights[class_id];
}

uint32_t scale_by_pc_class(struct pact_context *ctx, uint64_t ip, uint32_t attributed,
                           uint32_t pac_value_max)
{
    if (!ctx || !ctx->pc_class || !ctx->pc_class->enabled || attributed == 0) {
        return attributed;
    }
    pc_class_state_t *st = ctx->pc_class;
    double w = pc_class_weight_for_ip(st, ip);
    if (w <= 0.0) {
        return 0;
    }
    if (w == 1.0) {
        return attributed > pac_value_max ? pac_value_max : attributed;
    }

    double scaled = (double)attributed * w;
    if (scaled > (double)pac_value_max) {
        st->saturations++;
        return pac_value_max;
    }
    if (scaled < 0.0) {
        return 0;
    }
    return (uint32_t)scaled;
}

uint32_t pc_class_score(struct pact_context *ctx, uint64_t ip, uint32_t pac_value_max)
{
    if (!ctx || !ctx->pc_class || !ctx->pc_class->enabled) {
        return 0;
    }
    pc_class_state_t *st = ctx->pc_class;
    double s = pc_class_weight_for_ip(st, ip) * PC_CLASS_SCORE_UNIT;
    if (s <= 0.0) {
        return 0;
    }
    if (s > (double)pac_value_max) {
        st->saturations++;
        return pac_value_max;
    }
    return (uint32_t)s;
}
