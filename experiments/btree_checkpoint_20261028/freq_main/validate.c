/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* validate.c — CLI configuration sanity checks. */

#include <stdio.h>

#include "validate.h"

int pact_validate_configuration(const pact_config_t *config)
{
    if (config->target_pid <= 0) {
        fprintf(stderr, "Error: --workload <pid> is required.\n");
        return -1;
    }

    if (config->sampling_interval_ms < 5) {
        fprintf(
            stderr,
            "Warning: --sampling-interval %u ms is very aggressive and may cause high overhead\n",
            config->sampling_interval_ms);
    }

    return 0;
}
