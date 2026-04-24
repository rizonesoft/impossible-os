#!/usr/bin/env bash
# ============================================================================
# build-stale-bootloader.sh -- rebuild BOOTX64.EFI with
#                              BOOT_INFO_VERSION = (current - 1) so the
#                              stale-ABI fixture harness can exercise
#                              the pre-jump fatal path.
#
# Reads the current BOOT_INFO_VERSION from include/kernel/boot_info.h,
# rebuilds ONLY the bootloader with the decremented value via the
# EXTRA_CFLAGS pass-through hook in src/boot/uefi/Makefile, copies the
# resulting BOOTX64.EFI to build/fixtures/bootx64-stale-v${STALE}.efi,
# and restores the real bootloader on exit (even on SIGINT/SIGTERM).
#
# Exit 0 on successful fixture build + restore.
# Exit non-zero on any build failure; the trap still restores the real
# bootloader so the tree is left in a clean state.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURES_DIR="$REPO_ROOT/build/fixtures"

cd "$REPO_ROOT"

# Everything except the final fixture path goes to stderr so callers
# can `path=$(build-stale-bootloader.sh)` without filtering. The
# cleanup trap fires AFTER the path is echoed, and its "restoring..."
# messages otherwise polluted stdout.
exec 3>&1   # keep fd 3 as the "real" stdout for the path emit
exec 1>&2   # redirect stdout to stderr for all subsequent prints

# Parse current BOOT_INFO_VERSION from BOTH headers (kernel +
# bootloader mirror) and reject if they have drifted. If the
# fixture build trusted only one header, a pre-existing kernel/
# bootloader ABI drift would silently pass the harness (stale
# binary built from the "other" version, matching whichever side
# we chose to baseline). The harness is a consistency gate; its
# preflight must therefore enforce consistency before building.
K_VER=$(grep -oP '#define\s+BOOT_INFO_VERSION\s+\K\d+' \
    include/kernel/boot_info.h | head -1)
B_VER=$(grep -oP '#define\s+BOOT_INFO_VERSION\s+\K\d+' \
    src/boot/uefi/boot_info_mirror.h | head -1)
if [ -z "$K_VER" ] || [ -z "$B_VER" ]; then
    echo "build-stale-bootloader: could not parse BOOT_INFO_VERSION (kernel=$K_VER mirror=$B_VER)" >&2
    exit 2
fi
if [ "$K_VER" != "$B_VER" ]; then
    echo "build-stale-bootloader: FATAL: BOOT_INFO_VERSION drift -- kernel header=$K_VER, bootloader mirror=$B_VER. Fix the headers and re-run 'make boot-info-abi'." >&2
    exit 2
fi
# Also sanity-check BOOT_INFO_MAGIC parity.
K_MAGIC=$(grep -oP '#define\s+BOOT_INFO_MAGIC\s+\K0x[0-9A-Fa-f]+' \
    include/kernel/boot_info.h | head -1)
B_MAGIC=$(grep -oP '#define\s+BOOT_INFO_MAGIC\s+\K0x[0-9A-Fa-f]+' \
    src/boot/uefi/boot_info_mirror.h | head -1)
if [ "$K_MAGIC" != "$B_MAGIC" ]; then
    echo "build-stale-bootloader: FATAL: BOOT_INFO_MAGIC drift -- kernel=$K_MAGIC mirror=$B_MAGIC" >&2
    exit 2
fi
CURRENT="$K_VER"

STALE=$((CURRENT - 1))
if [ "$STALE" -lt 0 ]; then
    echo "build-stale-bootloader: BOOT_INFO_VERSION=$CURRENT cannot decrement to $STALE" >&2
    exit 2
fi

echo "[stale-bootloader] current=v$CURRENT stale=v$STALE"

# Trap cleanup: ALWAYS rebuild the real bootloader, even if the stale
# build fails OR we are interrupted. Otherwise the tree would be left
# with a bootloader that the system-disk.img still packages.
SCRATCH=""
cleanup() {
    local orig_rc=$?
    # Reap the scratch dir BEFORE any exit so it never leaks. Earlier
    # revision put the `rm -rf "$SCRATCH"` after `cleanup` in the trap
    # expression, but cleanup() exits in its body, making the rm
    # unreachable. Move it inside.
    if [ -n "$SCRATCH" ] && [ -d "$SCRATCH" ]; then
        rm -rf "$SCRATCH"
    fi
    echo "[stale-bootloader] restoring real BOOTX64.EFI..."
    make -C src/boot/uefi clean >/dev/null 2>&1 || true
    local restore_rc=0
    # Narrow restore: `make uefi-boot system-disk` rebuilds ONLY the
    # bootloader + repacks the ESP. Kernel + userland are untouched
    # because only the bootloader changed. Saves ~10s per-run on CI
    # vs the previous `bash scripts/build.sh` which rebuilt the whole
    # tree. If make fails for any reason, fall back to a full
    # `scripts/build.sh` rebuild as a safety net (e.g. a kernel .o
    # went missing from an earlier interrupted run).
    if ! make uefi-boot system-disk >/dev/null 2>&1; then
        echo "[stale-bootloader] uefi-boot+system-disk restore failed; falling back to full build" >&2
        if ! bash scripts/build.sh >/dev/null 2>&1; then
            echo "[stale-bootloader] FATAL: full restore build failed; tree IS dirty" >&2
            restore_rc=1
        fi
    fi
    # A restore failure propagates non-zero so callers (make
    # stale-abi-fixtures, CI) treat it as failure rather than silently
    # shipping a tree with a stale bootloader + build/system-disk.img.
    if [ "$restore_rc" -ne 0 ]; then
        exit 1
    fi
    exit "$orig_rc"
}
trap cleanup EXIT INT TERM

# Build the stale bootloader. Direct the output to a scratch dir so
# it does not overwrite build/tools/BOOTX64.EFI before we copy it out.
# cleanup() reaps SCRATCH on any exit path.
SCRATCH="$(mktemp -d)"

make -C src/boot/uefi clean >/dev/null 2>&1 || true
if ! make -C src/boot/uefi \
    EXTRA_CFLAGS="-DBOOT_INFO_VERSION=$STALE" \
    OUTDIR="$SCRATCH"; then
    echo "[stale-bootloader] build FAILED" >&2
    exit 1
fi

mkdir -p "$FIXTURES_DIR"
OUT="$FIXTURES_DIR/bootx64-stale-v${STALE}.efi"
cp "$SCRATCH/BOOTX64.EFI" "$OUT"
echo "[stale-bootloader] fixture at $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"

# Emit the fixture path on fd 3 (the original stdout) so callers can
# capture it with a clean `$(build-stale-bootloader.sh)`. Other output
# already went to stderr via the redirect at the top of this script.
echo "$OUT" >&3
