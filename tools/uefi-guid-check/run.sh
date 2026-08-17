#!/usr/bin/env bash
# Fetch the EDK2 reference headers (if absent) and validate our UEFI GUIDs.
#
# EDK2 is NOT vendored: its headers are coupled to the EDK2 build system
# (Base.h, ProcessorBind.h, autogen) and adopting them would drag that whole
# world into a bootloader that deliberately stays freestanding. It is used
# here purely as an external oracle, fetched on demand and cached.
#
#   bash tools/uefi-guid-check/run.sh            # fetch if needed, then check
#   bash tools/uefi-guid-check/run.sh --offline  # skip when the cache is absent
#
# Exit: 0 pass (or skipped offline), 1 GUID mismatch, 2 setup failure.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CACHE="${EDK2_CACHE_DIR:-${TMPDIR:-/tmp}/impossible-os-edk2}"
OFFLINE=0
[[ "${1:-}" == "--offline" ]] && OFFLINE=1

if [[ ! -d "$CACHE/MdePkg/Include" ]]; then
    if [[ $OFFLINE -eq 1 ]]; then
        echo "uefi-guid-check: SKIPPED (no EDK2 cache at $CACHE, --offline)"
        exit 0
    fi
    echo "uefi-guid-check: fetching EDK2 MdePkg headers into $CACHE ..."
    rm -rf "$CACHE"
    # Headers only: blob filter + sparse checkout keeps this ~11 MiB rather
    # than the multi-GiB full EDK2 tree.
    if ! git clone --depth 1 --filter=blob:none --sparse --quiet \
            https://github.com/tianocore/edk2.git "$CACHE" 2>/dev/null; then
        echo "uefi-guid-check: SKIPPED (clone failed -- offline or network blocked)"
        exit 0
    fi
    git -C "$CACHE" sparse-checkout set MdePkg/Include >/dev/null 2>&1 || {
        echo "uefi-guid-check: SKIPPED (sparse-checkout failed)"; exit 0; }
fi

exec python3 "$REPO_ROOT/tools/uefi-guid-check/check.py" \
     --edk2 "$CACHE" --efi-header "$REPO_ROOT/src/boot/uefi/efi.h" "${@:2}"
