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

CURRENT=$(grep -oP '#define\s+BOOT_INFO_VERSION\s+\K\d+' \
    include/kernel/boot_info.h | head -1)
if [ -z "$CURRENT" ]; then
    echo "build-stale-kernel: could not parse BOOT_INFO_VERSION" >&2
    exit 2
fi

STALE=$((CURRENT - 1))
if [ "$STALE" -lt 0 ]; then
    echo "build-stale-kernel: BOOT_INFO_VERSION=$CURRENT cannot decrement to $STALE" >&2
    exit 2
fi

echo "[stale-kernel] current=v$CURRENT stale=v$STALE"

cleanup() {
    local orig_rc=$?
    echo "[stale-kernel] restoring real kernel.exe..."
    # Nuke .o files so the restore re-compiles every TU without the
    # override. make's -MMD deps track headers only; if the header
    # content is unchanged between stale + restore builds (it is --
    # the override came from CFLAGS, not the source), make would
    # skip the recompile and leave stale .o files behind, producing
    # a kernel.exe that still has BOOT_INFO_VERSION=stale.
    find build -name '*.o' -path '*/kernel/*' -delete 2>/dev/null || true
    rm -f build/kernel.exe build/kernel.map build/kernel.sym \
          build/boot_proto_sha.h
    local restore_rc=0
    if ! make kernel >/dev/null 2>&1; then
        echo "[stale-kernel] FATAL: kernel restore build failed; tree IS dirty" >&2
        restore_rc=1
    fi
    # Also regenerate disk image so the ESP has the restored kernel.
    if ! bash scripts/build.sh >/dev/null 2>&1; then
        echo "[stale-kernel] FATAL: disk rebuild failed; tree IS dirty" >&2
        restore_rc=1
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

# Nuke existing kernel objects so the new CFLAGS take effect on every
# TU (make's .o files would otherwise be skipped if only the macro
# value changed -- -MMD deps track the HEADER, which is unchanged).
find build -name '*.o' -path '*/kernel/*' -delete 2>/dev/null || true
rm -f build/kernel.exe build/kernel.map build/kernel.sym

if ! make kernel KERNEL_EXTRA_CFLAGS="-DBOOT_INFO_VERSION=$STALE"; then
    echo "[stale-kernel] build FAILED" >&2
    exit 1
fi

mkdir -p "$FIXTURES_DIR"
OUT="$FIXTURES_DIR/kernel-stale-v${STALE}.exe"
cp build/kernel.exe "$OUT"
echo "[stale-kernel] fixture at $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"

echo "$OUT" >&3
