#!/bin/bash
# =============================================================================
# run-with-boot-conf.sh -- Boot the built image once with one boot.conf key set
#
# Patches KEY=VALUE into the system disk's boot.conf, launches QEMU through
# `make run-only` (which has no prerequisites, so nothing rebuilds the disk
# and overwrites the patch), and restores boot.conf when QEMU exits, fails or
# is interrupted. Used by `make run-unit-tests` and `make run-boot-tests`.
#
# It is a script rather than a recipe line because GNU make runs any recipe
# line that names $(MAKE) even under `make -n`, and the patch must not.
#
# Usage: bash scripts/run-with-boot-conf.sh <key> <value>
# =============================================================================

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <key> <value>" >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"

bash "$SCRIPT_DIR/patch-boot-conf.sh" "$1" "$2"
trap 'bash "$SCRIPT_DIR/patch-boot-conf.sh" reset' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

make -C "$PROJECT" --no-print-directory run-only
