#!/usr/bin/env bash
# host-gates.sh [ROOT] -- the fast host gates that compile kernel and loader
# sources (about 5 s together). A signature change in src/boot/ broke
# tools/boot-entries-parser-tests/test_crc_poison.c, and because the pre-push
# tooling gate fires only on scripts and hooks, three `Build` runs went red before
# anything local noticed (v22, 2026-11-07). A gate that cannot run is a failure,
# never a skip: a missing script here means the list below has drifted.
set -u
ROOT="${1:-$(git rev-parse --show-toplevel)}"
fail=0
for gate in tools/boot-header-tests/run.sh \
            tools/boot-entries-parser-tests/run.sh \
            tools/boot-trend-scan-tests/run.sh \
            tools/memmap-check/check.sh \
            tools/atomic-claim-check/check.sh \
            tools/uefi-guid-check/run.sh; do
  if [ ! -f "$ROOT/$gate" ]; then
    echo "host-gates: FAIL $gate is missing" >&2
    fail=1
    continue
  fi
  if ! out="$(timeout 120 bash "$ROOT/$gate" 2>&1)"; then
    echo "host-gates: FAIL $gate" >&2
    printf '%s\n' "$out" | tail -15 >&2
    fail=1
  fi
done
exit "$fail"
