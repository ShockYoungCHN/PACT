"""Install compile-time frequency attribution into a fresh PACT main copy."""
from pathlib import Path
import shutil
HERE=Path(__file__).resolve().parent
def install(src):
 src=Path(src);p=src/'pebs-aggregator.c';s=p.read_text();begin=s.index('#define PAC_VALUE_MAX 262143u');end=s.index('/* Decode one PEBS sample',begin)
 s=s[:begin]+'#include "sample_score.h"\n\n'+s[end:]
 begin=s.index('static void compute_attributed_stalls(');end=s.index('\n/*\n * Drain, attribute',begin);s=s[:begin]+'#if !PACT_FREQ_BASED\n'+s[begin:end]+'\n#endif\n'+s[end:]
 old='    compute_attributed_stalls(ctx, agg, &fast_stalls, &slow_stalls);';assert s.count(old)==1;s=s.replace(old,'#if !PACT_FREQ_BASED\n'+old+'\n#endif')
 old='    int debug_counter = 0;';assert s.count(old)==1;s=s.replace(old,'''#if PACT_FREQ_BASED
    log_info("score_mode", "Frequency: fixed per-sample increment=%u;no per-window MLP attribution",
             (unsigned)PACT_FREQ_SAMPLE_SCORE);
#else
    log_info("score_mode", "Original PAC: per-window stall attribution");
#endif

'''+old);p.write_text(s);shutil.copy2(HERE/'sample_score.h',src/'sample_score.h')
 p=src/'Makefile';s=p.read_text();at=s.index('# Optional build with file logging enabled.');s=s[:at]+'''# Frequency score changes only attribution;sampling/PMU/policy remain unchanged.
FREQ_SAMPLE_SCORE ?= 262143
.PHONY: freq
freq:
\t$(MAKE) CFLAGS="$(CFLAGS) -DPACT_FREQ_BASED=1 -DPACT_FREQ_SAMPLE_SCORE=$(FREQ_SAMPLE_SCORE)" all

'''+s[at:];p.write_text(s)
