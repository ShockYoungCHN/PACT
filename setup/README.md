# Host setup

Bring a fresh machine to a state where PACT can run. Do this **before**
building `../src/` or using `../run/`.

| Subdir | Purpose |
|--------|---------|
| [`kernel/`](kernel/) | Build, install, and boot vanilla **Linux 6.3**, then build and load the two modules the tiering subsystem requires - `tierinit` (registers the slow tier + demotion targets) and `kswapdrst` (keeps `kswapd`/demotion from stalling). |
| [`env/`](env/) | Prepare the machine for controlled runs: uncore-frequency pinning, CXL/NUMA layout, performance governor, and disabling turbo / THP / KSM / NUMA-balancing. |

## Order

1. **Kernel** - [`kernel/`](kernel/): `setup_kernel.sh` → reboot into 6.3.
2. **Fast-tier size** - shrink node 0 with a `memmap=` boot parameter → reboot
   again (see [Set the local fast-tier DRAM size](#set-the-local-fast-tier-dram-size)
   below). **This is what creates the fast/slow capacity split; without it the
   whole workload fits in local DRAM and PACT has nothing to migrate.**
3. **Modules** - [`kernel/`](kernel/): `build_modules.sh`, then load
   `tierinit.ko` + `kswapdrst.ko` (or let `run-pact.sh` load them).
4. **Environment** - [`env/`](env/): `sudo ./prepare_environment.sh` (or let
   the runner do it via `run_setup_config=true ./run-pact.sh ...`).

> **Re-run `prepare_environment.sh` after EVERY reboot.** Its effects (node-1
> CPUs offline, uncore frequency, THP/KSM/NUMA-balancing off) are runtime-only
> and do NOT persist across a reboot. In particular, after the `memmap` reboot
> in step 2 you must run it again before measuring, or the slow tier still has
> online CPUs and the run is invalid. The modules (step 3) also need reloading
> after a reboot; `run-pact.sh` reloads them automatically.

The env scripts source each other by their own location, so they can be
invoked from anywhere (e.g. the runner calls
`../setup/env/prepare_environment.sh`).

## Validate the CXL emulation (important)

The slow tier is emulated by a remote NUMA node with its CPUs taken offline
and its uncore frequency pinned low. `check_cxl_conf` now **aborts** if Node 1
still has online CPUs (an unvalidated layout silently produces invalid
results). Two things are *not* auto-validated and you should confirm them:

1. **Topology** - after `prepare_environment.sh`, the slow-tier node must be
   CPU-less:

   ```bash
   numactl --hardware        # 'node 1 cpus:' should be empty
   ```

2. **Latency** - the emulated tiers should land near the paper's targets
   (~90 ns local DRAM, ~140 ns remote NUMA, ~190 ns CXL-emulated; see
   [`../modeling/README.md`](../modeling/README.md)). Measure with a
   pointer-chase latency tool (e.g. Intel MLC `--latency_matrix`, or the
   `ptr_chase` microbenchmark) and adjust the uncore-frequency targets in
   [`env/modify-uncore-freq.sh`](env/modify-uncore-freq.sh) /
   `UNCORE_ARGS` until the slow tier matches. Pinning the frequency without
   checking the resulting latency is the most common source of invalid
   tiering results.

## Set the local (fast-tier) DRAM size

The fast/slow split is created by making the fast tier (node 0) **physically
small** with a `memmap=` boot parameter, so first-touch fills node 0 and the
overflow spills to the slow tier. Do NOT use a cgroup memory limit for this: a
memcg cap limits total usage (not fast-tier residency) and deadlocks the
workload in D-state when demotion is disabled.

### Size node 0 from the workload's RSS

For a target fast:slow ratio `R:1`, set node-0 usable DRAM to
`RSS x R/(R+1)`:

| Ratio (fast:slow) | node-0 usable DRAM |
|-------------------|--------------------|
| 1:1 (paper default) | `RSS / 2` |
| 1:2 | `RSS / 3` |
| 8:1 | `RSS x 8/9` |

Measure the workload's RSS first, on an unconstrained boot:

```bash
/usr/bin/time -v numactl -C 2-9 --membind 0 \
    env OMP_NUM_THREADS=8 ./bc -f kron.sg -i1 -n1   # "Maximum resident set size"
```

### Apply memmap

`memmap=<R>G!<off>G` reserves `<R>` GB of DRAM starting at `<off>` GB, removing
it from node 0. Prefer
[`samuraiy_soaralto/run/set_memmap.sh`](../../samuraiy_soaralto/run/set_memmap.sh)
(E820-checked) over hand-editing grub; it updates the last
`GRUB_CMDLINE_LINUX=` override CloudLab actually uses. Then `sudo update-grub`
and reboot.

On c220g5 the PCI hole covers ~2G–4G, so **offset must be `4G`** (usable DRAM
resumes at `0x100000000`). Older `!2G` recipes fail `set_memmap.sh` validation.

```bash
# c220g5: node 0 is ~95 GB. To leave ~10 GB usable (a 1:1 split for a ~19.5 GB
# RSS bc-kron), reserve 84 GB starting at 4G:
sudo /path/to/samuraiy_soaralto/run/set_memmap.sh --memmap 84G!4G
sudo update-grub && sudo reboot
# After reboot, confirm node 0 free ~= RSS/2:
numactl -H | grep -E 'node 0 (size|free)'   # e.g. size ~10600 MB, free ~10 GB
```

> The exact node-0 size for a given `memmap` varies by ~hundreds of MB each
> boot and by machine - tune the reserved GB up/down by 1-2 and re-check
> `numactl -H` until `node 0 free` is close to your target. On a fresh boot the
> OS holds only ~0.5 GB of node 0, so `node 0 free ~= node 0 size`.
> **Verified values (c220g5, bc-kron 8t, RSS ~19.5 GB):** `memmap=84G!4G` gives
> node 0 ~10.6 GB total / ~9–10 GB free = 1:1. `memmap=74G!4G` gives ~20 GB =
> the FULL RSS (no split - do not use for a 1:1 experiment).
> (`86G!2G` / `76G!2G` were old equivalents that overlapped the PCI hole; do not use.)

## Notes

- The current scripts assume a **2-NUMA-node** server (one fast tier, one
  CXL-like slow tier).
