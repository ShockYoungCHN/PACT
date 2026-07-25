#!/bin/bash

# Workload definitions for run-pact.sh.
# Source this file — do not execute directly.
#
# run-pact.sh reads four variables per workload "<name>" by indirection:
#   <name>_pname         process name run-pact.sh waits on / pins
#   <name>_vmtouch_file  file to pre-fault into DRAM before the run (or "")
#   <name>_omp_threads   OMP_NUM_THREADS for the run
#   <name>_workload_cmd  the command line; "$numactl_args" is substituted by
#                         run-pact.sh with CPU pinning and default first-touch
#                         memory allocation (numactl -C <cpus> --).
#
# Dataset/benchmark locations come from environment variables so no
# machine-specific path is baked in. Set these to match your machine:
#   GAPBS_DIR   built GAP Benchmark Suite (provides ./bc and graphs)
#   SPEC_DIR    licensed SPEC CPU 2017 install
#   SILO_DIR    built Silo (out-perf.masstree/benchmarks/dbtest)
#   UBENCH_DIR  ptr_chase / seq_array microbenchmark binaries

GAPBS_DIR="${GAPBS_DIR:-/path/to/gapbs}"
GAPBS_GRAPH_DIR="${GAPBS_GRAPH_DIR:-${GAPBS_DIR}/benchmark/graphs}"
SPEC_DIR="${SPEC_DIR:-/path/to/cpu2017}"
SILO_DIR="${SILO_DIR:-/path/to/silo}"
UBENCH_DIR="${UBENCH_DIR:-/path/to/microbenchmarks}"

# <name>_rss records the workload's peak resident set size in MB. It is
# REFERENCE DATA for sizing the fast tier by hand (set memmap = _rss/2 for a
# 1:1 split; see run/README.md) - run-pact.sh does not read it.
#
# GRAPH GENERATION: the paper's bc-kron uses a Kronecker graph of scale 27,
# degree 16 (134.2M vertices, 2.11B edges, ~18 GB .sg file, ~19.5 GB RSS).
# Generate it once with the GAPBS converter:
#     ./converter -g27 -k16 -b benchmark/graphs/kron.sg
# The _rss values below are for THAT graph. It is GRAPH-DEPENDENT: if you use a
# different scale (e.g. -g25 for a smaller ~4.5 GB / ~5 GB-RSS graph), re-measure
# with:
#     /usr/bin/time -v ./bc -f kron.sg -i1 -n1   # "Maximum resident set size"
# and update _rss (kbytes/1024 -> MB), then re-size memmap = _rss/2 for 1:1.

# --- bc_kron_8t : GAP betweenness-centrality on a Kronecker graph, 8 threads
# Paper/artifact: converter -g27 -k16, -i4 -n4, CPUs 2-9.
bc_kron_8t_pname="bc"
bc_kron_8t_rss=20000
bc_kron_8t_vmtouch_file="${GAPBS_GRAPH_DIR}/kron.sg"
bc_kron_8t_omp_threads=8
bc_kron_8t_workload_cmd="\$numactl_args ${GAPBS_DIR}/bc -f ${GAPBS_GRAPH_DIR}/kron.sg -i4 -n4"

# --- bc_kron_4t : same graph/RSS, 4 threads on CPUs 2-5 (half the cores).
bc_kron_4t_pname="bc"
bc_kron_4t_rss=20000
bc_kron_4t_vmtouch_file="${GAPBS_GRAPH_DIR}/kron.sg"
bc_kron_4t_omp_threads=4
bc_kron_4t_workload_cmd="\$numactl_args ${GAPBS_DIR}/bc -f ${GAPBS_GRAPH_DIR}/kron.sg -i4 -n4"

# --- bc_urand_8t : GAP bc on uniform-random graph (-u27 -k16), 8 threads
# Same scale/RSS class as bc_kron (~20 GB); memmap=86G!2G → ~1:1. -i4 -n4.
bc_urand_8t_pname="bc"
bc_urand_8t_rss=20000
bc_urand_8t_vmtouch_file="${GAPBS_GRAPH_DIR}/urand.sg"
bc_urand_8t_omp_threads=8
bc_urand_8t_workload_cmd="\$numactl_args ${GAPBS_DIR}/bc -f ${GAPBS_GRAPH_DIR}/urand.sg -i4 -n4"

# --- bc_urand_log : identical to bc_urand_8t but -l (per-source phase timing:
#     'b'=forward PBFS [atomic/barrier-bound], 'p'=backward accum [gather-bound])
bc_urand_log_pname="bc"
bc_urand_log_rss=20000
bc_urand_log_vmtouch_file="${GAPBS_GRAPH_DIR}/urand.sg"
bc_urand_log_omp_threads=8
bc_urand_log_workload_cmd="\$numactl_args ${GAPBS_DIR}/bc -f ${GAPBS_GRAPH_DIR}/urand.sg -i4 -n4 -l"

# --- bwaves_8t : SPEC CPU 2017 603.bwaves_s, 8 threads
bwaves_8t_pname="speed_bwaves_base.mytest-m64"
bwaves_8t_vmtouch_file=""
bwaves_8t_omp_threads=8
bwaves_8t_workload_cmd="cd ${SPEC_DIR}/603.bwaves_s && \$numactl_args ./speed_bwaves_base.mytest-m64 bwaves_1 < bwaves_1.in > bwaves_1.out 2>> bwaves_1.err"

# --- w_649_8t : SPEC CPU 2017 649.fotonik3d_s, 8 threads
w_649_8t_pname="fotonik3d_s_base.mytest-m64"
w_649_8t_vmtouch_file=""
w_649_8t_omp_threads=8
w_649_8t_workload_cmd="cd ${SPEC_DIR}/649.fotonik3d_s && \$numactl_args ./cmd.sh"

# --- silo_5t : Silo in-memory DB, YCSB bench, 5 threads
silo_5t_pname="dbtest"
silo_5t_vmtouch_file=""
silo_5t_omp_threads=5
silo_5t_workload_cmd="\$numactl_args ${SILO_DIR}/out-perf.masstree/benchmarks/dbtest --verbose --bench ycsb --num-threads 5 --scale-factor 240000 --parallel-loading --runtime 300"

# --- ptr_chase : pointer-chasing microbenchmark (latency-bound)
ptr_chase_pname="ptr_chase"
ptr_chase_vmtouch_file=""
ptr_chase_omp_threads=1
ptr_chase_workload_cmd="cd ${UBENCH_DIR} && \$numactl_args timeout 200 ./ptr_chase 5120 10"

# --- seq_array : sequential-array microbenchmark (bandwidth-bound)
seq_array_pname="seq_array"
seq_array_vmtouch_file=""
seq_array_omp_threads=1
seq_array_workload_cmd="cd ${UBENCH_DIR} && \$numactl_args timeout 200 ./seq_array 5120 10"
