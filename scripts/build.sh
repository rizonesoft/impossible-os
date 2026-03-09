#!/usr/bin/env bash
# build.sh — Build wrapper with log capture and sentinel markers.
#
# Usage:
#   ./scripts/build.sh              # make clean && make all
#   ./scripts/build.sh all          # make all (skip clean)
#   ./scripts/build.sh run          # make run
#   ./scripts/build.sh clean all    # explicit targets
#
# Output is tee'd to build/build.log.  The last line is always one of:
#   === BUILD OK ===
#   === BUILD FAILED (exit N) ===
#
# The agent should verify completion with: tail -1 build/build.log

set -uo pipefail

LOG="build/build.log"
mkdir -p build

# Default targets: clean + all (no ISO)
TARGETS=("${@}")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(clean all)
fi

echo "=== BUILD START $(date '+%Y-%m-%d %H:%M:%S') ===" | tee "$LOG"
echo "Targets: ${TARGETS[*]}" | tee -a "$LOG"
echo "---" | tee -a "$LOG"

for target in "${TARGETS[@]}"; do
    echo "[*] make $target" | tee -a "$LOG"
    make "$target" 2>&1 | tee -a "$LOG"
    RC=${PIPESTATUS[0]}
    if [[ $RC -ne 0 ]]; then
        echo "=== BUILD FAILED (exit $RC) ===" | tee -a "$LOG"
        exit "$RC"
    fi
done

echo "=== BUILD OK ===" | tee -a "$LOG"
