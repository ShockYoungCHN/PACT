#include <assert.h>
#include <stdio.h>
#include "sample_score.h"
__attribute__((noinline)) uint32_t scored(uint8_t tier,double fast,double slow)
{
    return compute_sample_stalls(tier,fast,slow);
}
int main(void)
{
#if PACT_FREQ_BASED
    const double values[]={-1.0,0.0,1.99,200000.0,1e9};
    for(unsigned i=0;i<5;i++)for(unsigned j=0;j<5;j++)for(unsigned tier=0;tier<2;tier++)
        assert(scored(tier,values[i],values[j])==PACT_FREQ_SAMPLE_SCORE);
    uint64_t page_a=0,page_b=0;
    for(int i=0;i<7;i++)page_a+=scored(i%2,0,1e9);
    for(int i=0;i<3;i++)page_b+=scored(1,100,0);
    assert(page_a==7ULL*PACT_FREQ_SAMPLE_SCORE&&page_b==3ULL*PACT_FREQ_SAMPLE_SCORE);
    assert((((uint64_t)scored(1,0,0))&((1u<<18)-1))==PACT_FREQ_SAMPLE_SCORE);
    puts("PASS: frequency is constant across tiers/windows/scalars;page totals proportional to samples;18-bit fit");
#else
    assert(scored(0,12.75,99.9)==12&&scored(1,12.75,99.9)==99);
    assert(scored(0,-1,0)==0&&scored(1,0,0)==0);
    assert(scored(1,0,262143)==262143&&scored(1,0,1e9)==262143);
    puts("PASS: original PAC tier choice,truncation,negative clamp and18-bit saturation");
#endif
}
