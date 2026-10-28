/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 MoatLab, Virginia Tech. */

/* binning.c — adaptive binning (Freedman-Diaconis), Algorithm 3. */

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "pact.h"
#include "binning.h"
#include "tsc.h" /* rdtsc */

/* TH_SCALE threshold for bin-width doubling/halving. */
#ifndef PACT_BINNING_TH_SCALE
#define PACT_BINNING_TH_SCALE 0.7
#endif

/* Partition for quickselect. Returns pivot final position. */
static size_t partition(double *arr, size_t low, size_t high)
{
    double pivot = arr[high];
    size_t i = low;

    for (size_t j = low; j < high; j++) {
        if (arr[j] <= pivot) {
            double temp = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
            i++;
        }
    }

    double temp = arr[i];
    arr[i] = arr[high];
    arr[high] = temp;
    return i;
}

/* Quickselect — k-th smallest element, O(n) average. */
static double quickselect(double *arr, size_t low, size_t high, size_t k)
{
    if (low == high) {
        return arr[low];
    }

    size_t pivot_idx = partition(arr, low, high);

    if (k == pivot_idx) {
        return arr[k];
    } else if (k < pivot_idx) {
        return quickselect(arr, low, pivot_idx - 1, k);
    } else {
        return quickselect(arr, pivot_idx + 1, high, k);
    }
}

void calculate_quartiles(reservoir_t *r, double *q1, double *q3)
{
    if (r->count < 4) {
        *q1 = *q3 = 0.0;
        return;
    }

    double *work = malloc(r->count * sizeof(double));
    if (!work) {
        /* do not crash mid-bin-width-computation on OOM */
        *q1 = *q3 = 0.0;
        return;
    }
    memcpy(work, r->samples, r->count * sizeof(double));

    size_t q1_idx = r->count / 4;
    size_t q3_idx = (3 * r->count) / 4;

    *q1 = quickselect(work, 0, r->count - 1, q1_idx);
    /* For Q3, we can reuse the partially sorted array */
    *q3 = quickselect(work, q1_idx, r->count - 1, q3_idx);

    free(work);
}

void update_bin_width(pact_context_t *pact)
{
    pact_workload_t *wl = pact->workload;
    binning_state_t *bin = wl->binning;

    double q1, q3;
    calculate_quartiles(wl->reservoir, &q1, &q3);
    if (q3 <= q1) {
        return;
    }

    size_t n = kh_size(wl->pac_table);
    if (n == 0) {
        return;
    }

    double iqr = q3 - q1;
    double n_cbrt = cbrt((double)n);
    double new_width = 2.0 * iqr / n_cbrt;

    bin->q1 = q1;
    bin->q3 = q3;

    /* Pressure adjust (Algorithm 3): scale up if the table is much
     * larger than the promotion-candidate set (most pages cold → widen
     * bins to compress the long tail). N_c is the migration ring fill. */
    size_t n_c = wl->migration_ring ? ring_buffer_migration_entry_size(wl->migration_ring) : 0;
    if (n_c > 0) {
        if ((double)n / (double)n_c > PACT_BINNING_TH_SCALE) {
            new_width = 2 * new_width;
        } else {
            new_width = new_width / 2;
        }
    }
    bin->bin_width = new_width;
    bin->last_update = rdtsc();
}
