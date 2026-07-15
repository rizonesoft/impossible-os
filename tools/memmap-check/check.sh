#!/usr/bin/env bash
# ============================================================================
# check.sh -- host-side behavioral gate for include/kernel/mm/memmap.h
#
# The header's _Static_asserts cover its CONSTANTS. They cannot cover the
# inline translation helpers, because a static-inline call is not an integer
# constant expression and no assert can evaluate one. This gate closes that
# blind spot on the host, at zero cost to the kernel image -- which is what
# lets it run while the kernel BSS ceiling blocks the in-kernel suite.
#
# Usage:
#   bash tools/memmap-check/check.sh
#
# Dependencies: a 64-bit host C compiler ($HOST_CC, default gcc). No kernel
# build, no network.
# ============================================================================

set -euo pipefail

HOST_CC="${HOST_CC:-gcc}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

if ! command -v "$HOST_CC" >/dev/null 2>&1; then
    echo "error: HOST_CC ($HOST_CC) not found" >&2
    exit 2
fi

OUT_DIR="${BUILD_DIR:-$REPO_ROOT/build}/tools"
mkdir -p "$OUT_DIR"
BIN="$OUT_DIR/test-memmap-layout"

"$HOST_CC" -m64 -O2 -Wall -Wextra -Werror \
    -I "$REPO_ROOT/include" \
    -o "$BIN" \
    "$SCRIPT_DIR/test-memmap-layout.c"

exec "$BIN"
