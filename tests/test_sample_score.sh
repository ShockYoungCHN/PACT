#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
test_dir=$(mktemp -d /tmp/pact-sample-score-XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
for variant in pac freq freq_one; do
    flags=()
    if [[ "$variant" != pac ]]; then flags+=(-DPACT_FREQ_BASED=1); fi
    if [[ "$variant" == freq_one ]]; then flags+=(-DPACT_FREQ_SAMPLE_SCORE=1); fi
    "${CC:-gcc}" -O2 -Wall -Wextra -I"$repo/src" "${flags[@]}" \
        "$repo/tests/sample_score.c" -o "$test_dir/$variant"
    "$test_dir/$variant"
done
for invalid in 0 -1 262144; do
    if "${CC:-gcc}" -c -I"$repo/src" -DPACT_FREQ_BASED=1 \
        "-DPACT_FREQ_SAMPLE_SCORE=$invalid" "$repo/tests/sample_score.c" \
        -o "$test_dir/invalid.o" 2>"$test_dir/compiler.log"; then
        echo "Invalid score unexpectedly accepted: $invalid" >&2
        exit 1
    fi
done
