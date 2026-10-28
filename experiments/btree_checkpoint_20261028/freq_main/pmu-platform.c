/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

#include <stdio.h>
#include <cpuid.h>

#include "pmu-platform.h"
#include "pmu.h"
#include "error.h"

pmu_platform_t g_pmu_platform;

/* Skylake Xeon CHA-to-core mapping (CloudLab c220g5) */
static const int skx_cha_to_core_map[10] = {2, 3, 4, 5, 6, 7, 8, 9, 0, 1};
static const int skx_core_to_tid[10] = {0x00, 0x10, 0x20, 0x30, 0x40, 0x08, 0x18, 0x28, 0x38, 0x48};

/* TID goes into config1 (ORed with config1_base). */
static void generic_fill_tor_config(const pmu_platform_t *plat, tor_pe_config_t *out, int event_idx,
                                    int tier, int core_id)
{
    out->config = plat->tor_base[event_idx][tier];
    out->config1 = plat->config1_base[tier];
    out->config2 = 0;

    if (core_id >= 0 && core_id < plat->nr_cha_mapping) {
        out->config |= plat->tid_en_bit;
        out->config1 |= plat->core_to_tid[core_id];
    }
}

static pmu_platform_id_t detect_platform(void)
{
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return PMU_PLATFORM_UNKNOWN;
    }

    unsigned int family = (eax >> 8) & 0xF;
    unsigned int model = (eax >> 4) & 0xF;

    if (family == 0xF) {
        family += (eax >> 20) & 0xFF;
    }
    if (family == 6 || family == 0xF) {
        model += ((eax >> 16) & 0xF) << 4;
    }

    /* Family 6: Intel Core microarchitectures */
    if (family == 6) {
        switch (model) {
        case 0x55: /* Skylake-X / Cascade Lake */
            return PMU_PLATFORM_SKX;
        }
    }

    return PMU_PLATFORM_UNKNOWN;
}

/* Per-platform descriptors. Built once at file scope; pmu_platform_init copies
 * the matching descriptor into the global g_pmu_platform. */
static const pmu_platform_t platform_skx = {
    .id = PMU_PLATFORM_SKX,
    .name = "Skylake-X",
    .event_llc_miss_local = 0x01d3,
    .event_llc_miss_remote = 0x02d3,
    .nr_cha_mapping = 10,
    .cha_to_core_map = skx_cha_to_core_map,
    .core_to_tid = skx_core_to_tid,
    .tor_base =
        {
            {0x2136, 0x2136},               /* [TOR_OCC][local], [TOR_OCC][remote] */
            {0x0100211fULL, 0x0100211fULL}, /* [TOR_CYC][local], [TOR_CYC][remote] */
        },
    .config1_base =
        {
            0x4043200000000ULL, /* tier 0 (local): DRD_LOC opcode << 32. */
            0x4043100000000ULL, /* tier 1 (remote): DRD_REM opcode << 32. */
        },
    .tid_en_bit = (1ULL << 19),
    .fill_tor_config = generic_fill_tor_config,
    .mlp_min = 1.0,
    .mlp_max = 16.0,
    .k_constant_dram = 238,
    .k_constant_cxl = 771,
};

int pmu_platform_init(void)
{
    pmu_platform_id_t id = detect_platform();
    const pmu_platform_t *src = &platform_skx;
    if (id == PMU_PLATFORM_UNKNOWN) {
        log_warning("pmu_platform_init", "Unknown CPU (falling back to SKX defaults)");
    }
    g_pmu_platform = *src;
    if (id == PMU_PLATFORM_UNKNOWN) {
        g_pmu_platform.id = PMU_PLATFORM_UNKNOWN;
        g_pmu_platform.name = "Unknown (SKX fallback)";
    }

    printf("PMU platform detected: %s\n", g_pmu_platform.name);

    extern event_config_t core_event_configs[CORE_EVENT_COUNT];
    core_event_configs[CORE_EVENT_LLC_MISS_FAST] =
        (event_config_t){g_pmu_platform.event_llc_miss_local, "LLC_MISS_FAST"};
    core_event_configs[CORE_EVENT_LLC_MISS_SLOW] =
        (event_config_t){g_pmu_platform.event_llc_miss_remote, "LLC_MISS_SLOW"};

    return (id == PMU_PLATFORM_UNKNOWN) ? -1 : 0;
}
