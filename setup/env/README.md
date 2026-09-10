# Environment preparation

Machine-wide configuration for controlled PACT measurements. Run on the host
after booting into the PACT kernel (see [`../kernel/`](../kernel/)).

| Script | Purpose |
|--------|---------|
| `prepare_environment.sh` | One-shot prep: pins uncore frequency, configures the CXL/NUMA layout, and flushes caches. Calls the other two. |
| `modify-uncore-freq.sh` | Pin per-node min/max uncore frequency (fast tier high, slow tier low). |
| `cxl-global.sh` | Helper functions (governor, turbo, THP, KSM, NUMA-balancing, cache flush) sourced by the above. |

## Usage

```bash
# Full one-shot prep (requires root):
sudo ./prepare_environment.sh
```

Override the default uncore targets via `UNCORE_ARGS` (use `sudo env` so the
variable survives into the root shell):

```bash
sudo env UNCORE_ARGS="2000000 2000000 500000 500000" ./prepare_environment.sh
```

> **Do not run `modify-uncore-freq.sh` standalone after CXL setup.** It brings
> all CPUs back online (to enter a known base state before re-pinning), which
> re-populates node 1 and destroys the CPU-less slow tier. Only
> `prepare_environment.sh` runs it in the correct order (frequency pin, then
> offline node 1). If you must re-pin frequencies, re-run the full
> `prepare_environment.sh`.

The scripts locate each other by their own directory, so they work regardless
of the current working directory (the runner invokes
`../setup/env/prepare_environment.sh`).
