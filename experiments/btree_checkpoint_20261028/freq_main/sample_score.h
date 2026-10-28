/* Compile-time PAC signal selection. There is no runtime mode branch. */
#ifndef PACT_MAIN_SAMPLE_SCORE_H
#define PACT_MAIN_SAMPLE_SCORE_H
#include <stdint.h>
#ifndef PACT_FREQ_BASED
#define PACT_FREQ_BASED 0
#endif
#define PAC_VALUE_MAX 262143u /*18-bit PEBS_ENCODE_PAC payload*/
#if PACT_FREQ_BASED
#ifndef PACT_FREQ_SAMPLE_SCORE
#define PACT_FREQ_SAMPLE_SCORE PAC_VALUE_MAX
#endif
_Static_assert(PACT_FREQ_SAMPLE_SCORE > 0 && PACT_FREQ_SAMPLE_SCORE <= PAC_VALUE_MAX,
               "PACT_FREQ_SAMPLE_SCORE must fit18bits and be positive");
#endif
static inline uint32_t compute_sample_stalls(uint8_t tier, double fast_stalls, double slow_stalls)
{
#if PACT_FREQ_BASED
    (void)tier;
    (void)fast_stalls;
    (void)slow_stalls;
    return PACT_FREQ_SAMPLE_SCORE;
#else
    double base = (tier == 0) ? fast_stalls : slow_stalls;
    if (base < 0.0) {
        return 0;
    }
    if (base > (double)PAC_VALUE_MAX) {
        return PAC_VALUE_MAX;
    }
    return (uint32_t)base;
#endif
}
#endif
