#!/usr/bin/env bash
# test-raw-make.sh -- raw `make all` must build a tree that has no build/.
#
# Owner: todo/00-infrastructure/TODO-01-developer-tooling-stack.md section 19.
#
# scripts/build.sh is the entry point and drives make target by target, so an
# artifact that only build.sh produced looked fine on that path while the make
# graph itself was incomplete: on 2026-11-17 `make all` on a fresh copy failed
# on `No rule to make target 'build/tools/BOOTX64.UKI.efi'` and on a missing
# build/generated/shim_ca.h. This test copies the TRACKED tree (working-tree
# contents, so uncommitted edits are tested; no build/, no keys, no .git) into
# a throwaway directory and checks the graph there, never in the live build/.
#
#   bash scripts/test-raw-make.sh   real `make all` in the copy, twice: both
#                                   succeed, and the second relinks no loader,
#                                   rewrites no unchanged shim_ca.h and
#                                   recompiles no object that only reads it
#
# A dry run (`make -n all`) cannot check this graph: the loader is a recursive
# make, and under -n the top make never creates the headers that sub-make
# reads, so it reports a missing rule that a real build does not have.
#
# Exit 0 pass, 1 fail, 2 usage. The copy is removed on every exit.
set -euo pipefail

[ "$#" -eq 0 ] || { echo "usage: $0" >&2; exit 2; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/raw-make.XXXXXX")"
BUILD_PGID=""

# The build runs in its own session (setsid), so its process group is exactly
# the build: on any exit, cancellation included, the whole group is stopped
# (TERM, then KILL after a bounded grace) and reaped before the copy is removed.
stop_build() {
    [ -n "$BUILD_PGID" ] || return 0
    kill -TERM -- "-$BUILD_PGID" 2>/dev/null || true
    for _ in $(seq 1 20); do
        kill -0 -- "-$BUILD_PGID" 2>/dev/null || break
        sleep 0.5
    done
    kill -KILL -- "-$BUILD_PGID" 2>/dev/null || true
    wait "$BUILD_PGID" 2>/dev/null || true
    BUILD_PGID=""
}
trap 'stop_build; rm -rf "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
TREE="$WORK/tree"
mkdir -p "$TREE"

fail() { echo "[raw-make] FAIL: $*" >&2; exit 1; }

# Tracked files only: what a fresh clone holds, as the working tree has them.
# A tracked path deleted in the working tree is skipped, as a clone would lack it.
( cd "$REPO" && git ls-files -z | while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done ) |
    ( cd "$REPO" && tar --null -T - -cf - ) | ( cd "$TREE" && tar -xf - )
[ ! -e "$TREE/build" ] || fail "the copy already holds build/"

# The real build runs under one deadline as well; `timeout` without --foreground
# signals the process group it leads, so no compiler outlives the deadline.
DEADLINE="${RAW_MAKE_TIMEOUT:-1800}"
JOBS="$(nproc 2>/dev/null || echo 4)"

build() {
    local rc=0
    ( cd "$TREE" && exec setsid timeout --kill-after=10 "$DEADLINE" make -j"$JOBS" all ) > "$WORK/$1.log" 2>&1 &
    BUILD_PGID=$!
    wait "$BUILD_PGID" || rc=$?
    BUILD_PGID=""
    [ "$rc" -eq 0 ] ||
        { grep -E "\*\*\*|error:|No rule" "$WORK/$1.log" | head -20 >&2; fail "make all ($1) failed"; }
}

export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(date +%s)}"
build first
for f in build/tools/BOOTX64.EFI build/tools/BOOTX64.UKI.efi build/generated/shim_ca.h build/kernel.exe; do
    [ -f "$TREE/$f" ] || fail "make all did not produce $f"
done
loader_before=$(sha256sum "$TREE/build/tools/BOOTX64.EFI" | cut -d' ' -f1)
header_before=$(stat -c %Y "$TREE/build/generated/shim_ca.h")
obj_before=$(stat -c %Y "$TREE/build/kernel/uefi_runtime.o")

build second
loader_after=$(sha256sum "$TREE/build/tools/BOOTX64.EFI" | cut -d' ' -f1)
[ "$loader_before" = "$loader_after" ] || fail "a second make all changed BOOTX64.EFI"
[ "$header_before" = "$(stat -c %Y "$TREE/build/generated/shim_ca.h")" ] ||
    fail "a second make all rewrote shim_ca.h although its content is the same"
[ "$obj_before" = "$(stat -c %Y "$TREE/build/kernel/uefi_runtime.o")" ] ||
    fail "a second make all recompiled uefi_runtime.o"
# The UKI is packed from the canonical, unsigned stub (TODO-02 build
# idempotency): its .sbat must still equal the source after the repack.
sbat="$WORK/uki.sbat"
objcopy -O binary --only-section=.sbat "$TREE/build/tools/BOOTX64.UKI.efi" "$sbat"
cmp -s "$sbat" "$TREE/src/boot/uefi/sbat.csv" || fail "the repacked UKI's .sbat no longer equals src/boot/uefi/sbat.csv"
echo "[raw-make] OK: make all built a tree with no build/, twice; the loader, shim_ca.h and uefi_runtime.o were left alone and the UKI's .sbat is intact"
