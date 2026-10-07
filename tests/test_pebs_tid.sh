#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
test_bin=$(mktemp /tmp/pact-pebs-tid-test-XXXXXX)
trap 'rm -f "$test_bin"' EXIT
"${CC:-gcc}" -D_GNU_SOURCE -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
    -I"$repo/src" "$repo/tests/pebs_tid.c" -Wl,--gc-sections -lm -o "$test_bin"
"$test_bin" enabled
"$test_bin" stride
"$test_bin" disabled
