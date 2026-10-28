# Btree checkpoint,2026-10-28

This branch preserves two explicitly distinguished states:

* Root `src/`,run scripts and environment helpers capture the pre-existing local
  `pc-criticality` working changes. They include older multi-workload/PC changes;
  they are not claimed to be the exact measured Btree runtime.
* `pac_main/`,`freq_main/` and `pc_4k_positive/` are exact C/header/Makefile source
  snapshots of the runtimes used in the completed Btree experiments. Source and
  measured binary hashes are recorded in SOURCE_MANIFEST.json. Build timestamps
  and Git version strings can differ on rebuild.

The default-clock PAC main is unchanged by the frequency ablation. In freq_main,
`make freq` enables PACT_FREQ_BASED=1,with262143 units per processed sample;
`make` selects original PAC. FREQ_SAMPLE_SCORE changes the scale at compile time.
No runtime scoring-mode branch is used. Existing PMU/PEBS acquisition and
promotion/kernel-LRU policies are retained. Fixed score scale affects threshold
eligibility;this is not a scale-independent oracle or all-load frequency.

The PC4KiB snapshot includes positive-score-only promotion. The proposed
migration-acknowledgement/ENOMEM coordination fix has NOT been implemented and
is not part of this checkpoint.

Validation JSON files,completed pooled1-worker summaries,and the research
wrap-up accompany the sources. Raw PEBS,compiled binaries,kernel trees,backups
and machine-local scratch files are not uploaded. Experiment launchers and the
six-pool Btree implementation remain in the local pc_driven experiment branch;
its exact commit is recorded in SOURCE_MANIFEST.json.

This is a reproducible source checkpoint for review,not a merge into upstream
PACT and not a claim that the older root runtime was used for the latest runs.
