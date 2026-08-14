#!/bin/bash
# Build userspace perf from Linux v6.3 (same tag as ../kernel/setup_kernel.sh).
#
# Do NOT clone torvalds/master: newer perf needs a newer libcapstone than Noble
# ships, and mismatches the PACT 6.3 PMU/PEBS ABI.
#
# Output: $(dirname $0)/linux-perf/tools/perf/perf
# Override tag with PERF_LINUX_TAG=v6.3 ./install-perf.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}/linux-perf"
PERF_TAG="${PERF_LINUX_TAG:-v6.3}"

sudo apt-get update -y
sudo apt-get install -y build-essential flex bison pkg-config \
  libelf-dev libdw-dev libnuma-dev zlib1g-dev libunwind-dev \
  libtraceevent-dev libdebuginfod-dev libslang2-dev \
  clang libbabeltrace-dev libcapstone-dev libssl-dev

if [ -d "${SRC_DIR}/.git" ]; then
  cur="$(git -C "${SRC_DIR}" describe --tags --exact-match 2>/dev/null || true)"
  if [ "${cur}" != "${PERF_TAG}" ]; then
    echo "ERROR: ${SRC_DIR} exists but is not ${PERF_TAG} (got '${cur:-unknown}')." >&2
    echo "       rm -rf ${SRC_DIR} and re-run, or set PERF_LINUX_TAG." >&2
    exit 1
  fi
  echo "==> Reusing existing ${SRC_DIR} at ${PERF_TAG}"
else
  echo "==> Cloning Linux ${PERF_TAG} (shallow)..."
  git clone --depth=1 --branch "${PERF_TAG}" \
    https://github.com/torvalds/linux.git "${SRC_DIR}"
fi

echo "==> Building tools/perf..."
# GCC 14 on Noble treats epoll_pwait(NULL) in tests/bpf.c as -Werror=nonnull.
make -C "${SRC_DIR}/tools/perf" -j"$(nproc)" WERROR=0
echo "==> perf: ${SRC_DIR}/tools/perf/perf"
"${SRC_DIR}/tools/perf/perf" --version
