#!/usr/bin/env bash
# uki-pack.sh -- pack a Unified Kernel Image (UAPI Group UKI layout).
#
# Usage: scripts/uki-pack.sh <stub.efi> <kernel.exe> <cmdline-file> <payload-dir> <out.efi>
#
# The ONE implementation of the UKI pack. scripts/build.sh calls it for the
# shipped artifact and scripts/test-smoke.sh calls it for the UKI-direct boot
# fixtures, so the image a test boots is packed exactly the way the shipped
# one is: same tool selection, same section order, same section flags.
#
# Section order is load-bearing: .osrel -> .cmdline -> .linux -> .initrd ->
# .recovery -> .modules (docs/guides/secure-boot-keys.md, UKI subsection).
# The measured-boot path consumes the sections in this order, so argument
# order to objcopy decides PCR reproducibility. Optional payloads are read
# from <payload-dir>/{initrd.img,recovery.img,modules.cpio} and appended only
# when present, so a build without signed payloads ships the kernel-only UKI.
#
# Section virtual addresses are auto-placed by objcopy past the stub's own
# sections; the loader's detect_uki_sections() finds them by NAME, so the
# UAPI spec's suggested addresses need not be matched (llvm-objcopy has no
# --change-section-vma for PE).
#
# Exit status: 0 packed; 3 no objcopy on PATH (the caller decides whether a
# missing UKI is fatal); 2 bad arguments or missing input; 1 objcopy failed.
# Progress lines go to stdout prefixed "[uki]".
set -euo pipefail

if [ "$#" -ne 5 ]; then
    echo "[uki] usage: $0 <stub.efi> <kernel.exe> <cmdline-file> <payload-dir> <out.efi>" >&2
    exit 2
fi
STUB="$1"
KERNEL="$2"
CMDLINE="$3"
PAYLOAD_DIR="$4"
OUT="$5"

for f in "$STUB" "$KERNEL" "$CMDLINE"; do
    if [ ! -f "$f" ]; then
        echo "[uki] missing input: $f" >&2
        exit 2
    fi
done

OBJCOPY=""
for cand in llvm-objcopy-19 llvm-objcopy objcopy; do
    if command -v "$cand" >/dev/null 2>&1; then
        OBJCOPY="$cand"
        break
    fi
done
if [ -z "$OBJCOPY" ]; then
    echo "[uki] WARN: no objcopy found (tried llvm-objcopy-19, llvm-objcopy, objcopy); skipping UKI pack"
    exit 3
fi

# The output must not be one of the inputs: objcopy would read a file it is
# truncating. Compared by resolved path, so ./a and a are the same file.
OUT_REAL="$(realpath -m -- "$OUT")"
for f in "$STUB" "$KERNEL" "$CMDLINE" "$PAYLOAD_DIR/initrd.img" \
         "$PAYLOAD_DIR/recovery.img" "$PAYLOAD_DIR/modules.cpio"; do
    if [ "$(realpath -m -- "$f")" = "$OUT_REAL" ]; then
        echo "[uki] output $OUT is also an input ($f)" >&2
        exit 2
    fi
done

# Minimal os-release info; matches the UAPI Group spec layout. A private
# temporary file, never a path derived from an argument: a derived name can
# alias an input (a cmdline file called <out>.osrel.txt) and would be
# truncated before objcopy read it.
OSREL="$(mktemp "${TMPDIR:-/tmp}/uki-osrel.XXXXXX")"
trap 'rm -f "$OSREL"' EXIT
cat > "$OSREL" <<'OSEOF'
NAME="Impossible OS"
ID=impossible-os
VERSION_ID="0.1"
PRETTY_NAME="Impossible OS (UKI)"
OSEOF

ARGS=(
    --add-section ".osrel=$OSREL" --set-section-flags .osrel=alloc,readonly,data
    --add-section ".cmdline=$CMDLINE" --set-section-flags .cmdline=alloc,readonly,data
    --add-section ".linux=$KERNEL" --set-section-flags .linux=alloc,readonly,data
)
for spec in initrd:initrd.img recovery:recovery.img modules:modules.cpio; do
    name="${spec%%:*}"
    file="$PAYLOAD_DIR/${spec#*:}"
    if [ -f "$file" ]; then
        ARGS+=(--add-section ".$name=$file" --set-section-flags ".$name=alloc,readonly,data")
        echo "[uki] embedding .$name from $file ($(wc -c < "$file" | tr -d ' ') bytes)"
    fi
done

if ! "$OBJCOPY" "${ARGS[@]}" "$STUB" "$OUT"; then
    echo "[uki] FATAL: $OBJCOPY --add-section failed"
    exit 1
fi
echo "[uki] generated $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
