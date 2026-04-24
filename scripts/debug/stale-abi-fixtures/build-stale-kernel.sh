#!/usr/bin/env bash
# ============================================================================
# build-stale-kernel.sh -- rebuild kernel.exe with
#                         BOOT_INFO_VERSION = (current - 1).
#
# Mirror of build-stale-bootloader.sh for the kernel side. Uses the
# top-level Makefile's KERNEL_EXTRA_CFLAGS hook instead of the UEFI
# sub-Makefile's EXTRA_CFLAGS.
#
# Trap guarantees the real kernel is rebuilt on exit so the tree is
# left clean even on failure or interrupt.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURES_DIR="$REPO_ROOT/build/fixtures"

cd "$REPO_ROOT"

# Reserve fd 3 for the fixture path; redirect stdout to stderr for
# everything else so callers can capture the path cleanly.
exec 3>&1
exec 1>&2

K_VER=$(grep -oP '#define\s+BOOT_INFO_VERSION\s+\K\d+' \
    include/kernel/boot_info.h | head -1)
B_VER=$(grep -oP '#define\s+BOOT_INFO_VERSION\s+\K\d+' \
    src/boot/uefi/boot_info_mirror.h | head -1)
if [ -z "$K_VER" ] || [ -z "$B_VER" ]; then
    echo "build-stale-kernel: could not parse BOOT_INFO_VERSION (kernel=$K_VER mirror=$B_VER)" >&2
    exit 2
fi
if [ "$K_VER" != "$B_VER" ]; then
    echo "build-stale-kernel: FATAL: BOOT_INFO_VERSION drift -- kernel=$K_VER mirror=$B_VER. Fix the headers and re-run 'make boot-info-abi'." >&2
    exit 2
fi
K_MAGIC=$(grep -oP '#define\s+BOOT_INFO_MAGIC\s+\K0x[0-9A-Fa-f]+' \
    include/kernel/boot_info.h | head -1)
B_MAGIC=$(grep -oP '#define\s+BOOT_INFO_MAGIC\s+\K0x[0-9A-Fa-f]+' \
    src/boot/uefi/boot_info_mirror.h | head -1)
if [ "$K_MAGIC" != "$B_MAGIC" ]; then
    echo "build-stale-kernel: FATAL: BOOT_INFO_MAGIC drift -- kernel=$K_MAGIC mirror=$B_MAGIC" >&2
    exit 2
fi
CURRENT="$K_VER"

STALE=$((CURRENT - 1))
if [ "$STALE" -lt 0 ]; then
    echo "build-stale-kernel: BOOT_INFO_VERSION=$CURRENT cannot decrement to $STALE" >&2
    exit 2
fi

echo "[stale-kernel] current=v$CURRENT stale=v$STALE"

cleanup() {
    local orig_rc=$?
    echo "[stale-kernel] restoring real kernel.exe..."
    # Scoped invalidation for the restore too: only re-build the TUs
    # that actually saw the KERNEL_EXTRA_CFLAGS override (the
    # boot_info.h closure). Same list as the forward path above.
    local restore_count=0
    if [ -d build ]; then
        while IFS= read -r dfile; do
            local ofile="${dfile%.d}.o"
            [ -f "$ofile" ] || continue
            if grep -q 'boot_info\.h\|boot_proto_sha\.h\|boot_proto_descriptor\.h' "$dfile" 2>/dev/null; then
                rm -f "$ofile" "$dfile"
                restore_count=$((restore_count + 1))
            fi
        done < <(find build -name '*.d' -path '*/kernel/*' 2>/dev/null)
    fi
    echo "[stale-kernel] restore invalidated $restore_count .o files"
    rm -f build/kernel.exe build/kernel.map build/kernel.sym \
          build/boot_proto_sha.h
    local restore_rc=0
    # Narrow restore: `make kernel system-disk` rebuilds the scoped
    # kernel TUs + repacks the disk image. Userland + bootloader are
    # untouched (neither changed). Saves ~10s on CI vs the previous
    # full `bash scripts/build.sh`.
    if ! make kernel system-disk >/dev/null 2>&1; then
        echo "[stale-kernel] narrow restore failed; falling back to full build" >&2
        if ! bash scripts/build.sh >/dev/null 2>&1; then
            echo "[stale-kernel] FATAL: full restore build failed; tree IS dirty" >&2
            restore_rc=1
        fi
    fi
    # If restore itself failed, propagate non-zero so callers (make
    # stale-abi-fixtures, CI) treat the run as failed even if the
    # fixture itself had no issue. A restore failure is a worse state
    # than a fixture failure: it leaves stale artifacts on disk that
    # can corrupt the next build.
    if [ "$restore_rc" -ne 0 ]; then
        exit 1
    fi
    # Propagate the pre-cleanup script exit code (preserves the
    # stale-build-failure signal when that was what triggered us).
    exit "$orig_rc"
}
trap cleanup EXIT INT TERM

# Nuke ONLY the kernel objects whose .d files (make -MMD dep tracking)
# mention boot_info.h. Deleting every *.o under build/kernel/ would
# force ~200 recompiles; scoping to the ~20 TUs that actually consume
# BOOT_INFO_VERSION cuts the stale-kernel build time by ~90%.
# The existing .d files were written by the last real build, so they
# accurately reflect the transitive include closure.
invalidated_count=0
if [ -d build ]; then
    while IFS= read -r dfile; do
        ofile="${dfile%.d}.o"
        [ -f "$ofile" ] || continue
        if grep -q 'boot_info\.h\|boot_proto_sha\.h\|boot_proto_descriptor\.h' "$dfile" 2>/dev/null; then
            rm -f "$ofile" "$dfile"
            invalidated_count=$((invalidated_count + 1))
        fi
    done < <(find build -name '*.d' -path '*/kernel/*' 2>/dev/null)
fi
echo "[stale-kernel] invalidated $invalidated_count .o files (scope: boot_info.h closure)"
# Always nuke the linked kernel binary + the generated SHA header so
# both sides of the build rebuild consistently.
rm -f build/kernel.exe build/kernel.map build/kernel.sym \
      build/boot_proto_sha.h

if ! make kernel KERNEL_EXTRA_CFLAGS="-DBOOT_INFO_VERSION=$STALE"; then
    echo "[stale-kernel] build FAILED" >&2
    exit 1
fi

mkdir -p "$FIXTURES_DIR"
OUT="$FIXTURES_DIR/kernel-stale-v${STALE}.exe"
cp build/kernel.exe "$OUT"
echo "[stale-kernel] fixture at $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"

echo "$OUT" >&3
