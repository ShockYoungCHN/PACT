#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
test_bin=$(mktemp /tmp/pact-rerank-budget-XXXXXX)
trap 'rm -f "$test_bin"' EXIT
"${CC:-gcc}" -D_GNU_SOURCE -O2 -Wall -Wextra -ffunction-sections -fdata-sections \
  -I"$repo/src" "$repo/tests/rerank_budget.c" -Wl,--gc-sections -lm -lnuma -o "$test_bin"
"$test_bin"
