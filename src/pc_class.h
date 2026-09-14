/* SPDX-License-Identifier: MIT */
/* pc_class.h — offline PC → behavior-class weights for PAC scaling.
 *
 * When enabled (both --class-weights and --pc-class-map supplied), each PEBS
 * sample's attributed stalls are multiplied by w_c(class(PC)). Map keys are
 * main-binary file offsets (ip - text_base) so PIE/ASLR is handled after
 * resolving /proc/<pid>/maps. Disabled → scale_by_pc_class is identity.
 */

#ifndef PACT_PC_CLASS_H
#define PACT_PC_CLASS_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct pact_context;

/* Canonical class ids (stable names matching pc_driven/pc_behavior_classes.py).
 * C2 is split: affine scan vs independent gather. Legacy map/weight names
 * "C2_stream" → C2_affine and "C3_hot_l1" → C3_invariant when loading. */
enum pc_class_id {
    PC_CLASS_C1_LATENCY = 0,
    PC_CLASS_C2_AFFINE = 1,
    PC_CLASS_C2_GATHER = 2,
    PC_CLASS_C3_INVARIANT = 3,
    PC_CLASS_C4_OTHER = 4,
    PC_CLASS_COUNT = 5
};

/* Legacy enum name (pre-invariant rename). */
#define PC_CLASS_C3_HOT_L1 PC_CLASS_C3_INVARIANT

typedef struct pc_class_state pc_class_state_t;

/* Allocate empty (disabled) state. Always non-NULL on success. */
pc_class_state_t *pc_class_create(void);

void pc_class_destroy(pc_class_state_t *st);

/* Load class_weights.json (or "C1_latency 16.5" lines). Returns 0 on success. */
int pc_class_load_weights(pc_class_state_t *st, const char *path);

/* Load "offset_hex class_id" map (one entry per line). Returns 0 on success. */
int pc_class_load_map(pc_class_state_t *st, const char *path);

/*
 * Enable only when both weights and map loaded successfully.
 * Resolves main-binary r-xp [base,end) from /proc/<pid>/maps matching exe.
 * Returns 0 if enabled (or intentionally left disabled); -1 on hard failure
 * when paths were requested but could not be applied.
 */
int pc_class_enable_for_pid(pc_class_state_t *st, pid_t pid);

bool pc_class_enabled(const pc_class_state_t *st);

/* Override / refresh text mapping (rarely needed outside enable_for_pid). */
void pc_class_set_text_base(pc_class_state_t *st, uint64_t base, uint64_t end);

/*
 * Scale attributed stalls by w_c(class(ip)). Identity when disabled.
 * Result is clamped to pac_value_max (typically PAC_VALUE_MAX).
 * Used by --score-mode pac+pc.
 */
uint32_t scale_by_pc_class(struct pact_context *ctx, uint64_t ip, uint32_t attributed,
                           uint32_t pac_value_max);

/*
 * Pure PC-class per-sample score = w_c(class(ip)) * unit, ignoring the PAC
 * model. Accumulated per page this is score = Σ_c w_c · samples_c. Returns 0
 * when disabled. Used by --score-mode pc. Clamped to pac_value_max.
 */
uint32_t pc_class_score(struct pact_context *ctx, uint64_t ip, uint32_t pac_value_max);

/* Periodic diagnostics (rate-limited). */
void pc_class_log_stats(const pc_class_state_t *st);

/* Always print a stats line (teardown). */
void pc_class_log_stats_final(pc_class_state_t *st);

#endif /* PACT_PC_CLASS_H */
