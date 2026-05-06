#!/usr/bin/env bash
# build-iso.sh -- produce a UEFI-only hybrid ISO from the release disk image.
#
# Owner: Hybrid ISO / El Torito UEFI Boot feature in
# todo/01-boot-platform/TODO-06-boot-media-image-installer-handoff.md.
# Scope boundary: this script ships the release-side ISO producer; the more
# general installer-iso pipeline (`scripts/make-iso.sh`) is owned by the
# installer-iso TODO under todo/10-platform-services/ (does not exist yet);
# release-pipeline Joliet+RockRidge+versioned-filename wrapping is owned by
# todo/15-installer-release/TODO-01-release-artifacts.md.
#
# Output: build/release/disk.iso -- ISO9660 image with:
#   - ESP image (FAT32, 64 MiB) registered as the El Torito UEFI boot entry
#     (no BIOS boot record; UEFI firmware handles the legacy-boot rejection)
#   - /IPOS/manifest.json    artifact manifest from build-manifest.sh
#   - /IPOS/installer/       placeholder dir for installer payloads
#   - /IPOS/recovery/        placeholder dir for recovery payloads
#
# Determinism contract:
#   - SOURCE_DATE_EPOCH=0 anchors xorriso modification timestamps
#   - --modification-date=19700101000000 fixes the volume timestamp
#   - --joliet-charset and -volid pinned so two consecutive runs from a
#     clean tree produce a byte-identical disk.iso
#
# Exit codes:
#   0 -- ISO produced
#   1 -- xorriso / ESP extract failure
#   2 -- usage / missing tool

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

err()  { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[build-iso] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--in PATH] [--out PATH]

Options:
  --in PATH    Input raw release image (default: build/release/disk.img)
  --out PATH   Output ISO (default: build/release/disk.iso)
  -h, --help   Show this message
EOF
}

IN_IMG="build/release/disk.img"
OUT_ISO="build/release/disk.iso"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --in)      IN_IMG="$2"; shift 2 ;;
        --out)     OUT_ISO="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

[ -f "$IN_IMG" ] || { err "missing input: $IN_IMG (run scripts/release/build-image.sh)"; exit 1; }
for tool in xorriso dd python3; do
    command -v "$tool" >/dev/null || { err "missing tool: $tool"; exit 2; }
done

# Reject --in == --out: the producer would `rm -f` the verified raw input
# before xorriso wrote the ISO over it -- irreversible release-artifact
# data loss. Compare canonical paths so symlinks/relative-vs-absolute
# don't sneak past the check.
IN_CANON="$(readlink -f -- "$IN_IMG")"
mkdir -p "$(dirname "$OUT_ISO")"
OUT_DIR_CANON="$(readlink -f -- "$(dirname "$OUT_ISO")")"
OUT_CANON="$OUT_DIR_CANON/$(basename "$OUT_ISO")"
if [ "$IN_CANON" = "$OUT_CANON" ]; then
    err "--in and --out resolve to the same canonical path ($IN_CANON); refusing to overwrite the source"
    exit 2
fi

export SOURCE_DATE_EPOCH=0
export TZ=UTC
export LC_ALL=C

mkdir -p "$(dirname "$OUT_ISO")"
WORK_DIR="$(mktemp -d "$(dirname "$OUT_ISO")/build-iso-work.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

# Pin the source image's inode into WORK_DIR via hardlink so a concurrent
# build-image.sh / writer cannot mutate $IN_IMG between extraction and
# verification (TOCTOU). Hardlink shares the inode atomically; if the
# source path is later replaced via rename(2), our link still points at
# the original inode. Cross-filesystem fallback to cp captures content at
# copy time -- strictly weaker but unavoidable when WORK_DIR sits on a
# different FS than IN_IMG. Same pattern as scripts/deploy/write-usb.sh.
PINNED_IMG="$WORK_DIR/source.img"
# `-L` dereferences symlink inputs so the hardlink points at the referent
# inode, not the symlink. GNU ln defaults to `-P` which would let a
# concurrent rename of the symlink target re-introduce the TOCTOU.
if ! ln -L -- "$IN_IMG" "$PINNED_IMG" 2>/dev/null; then
    note "hardlink pin failed (cross-filesystem?); falling back to cp"
    if ! cp -- "$IN_IMG" "$PINNED_IMG"; then
        err "failed to pin source image $IN_IMG"
        exit 1
    fi
fi
IN_IMG="$PINNED_IMG"

# ---- Extract ESP partition from disk.img --------------------------------------
# ESP layout from build-image.sh: LBA 2048..133119, 64 MiB.
ESP_LBA_FIRST=2048
ESP_SECTORS=131072
ESP_BYTES=$(( ESP_SECTORS * 512 ))
ESP_IMG="$WORK_DIR/esp.img"

# Pre-flight: source image must be at least ESP_LBA_FIRST*512 + ESP_BYTES
# bytes. A truncated input would let dd produce a short ESP that still
# carries the required FAT files (verify-esp passes) but lacks the partition
# tail; xorriso would embed the malformed ESP and the boot would fail at
# firmware time, not at construction time.
SRC_SIZE="$(stat -c '%s' "$IN_IMG")"
MIN_SRC_SIZE=$(( ESP_LBA_FIRST * 512 + ESP_BYTES ))
if [ "$SRC_SIZE" -lt "$MIN_SRC_SIZE" ]; then
    err "input $IN_IMG is $SRC_SIZE bytes; need at least $MIN_SRC_SIZE bytes for ESP extraction"
    exit 1
fi

note "extracting ESP from $IN_IMG (LBA $ESP_LBA_FIRST, ${ESP_SECTORS} sectors)"
dd if="$IN_IMG" of="$ESP_IMG" bs=512 skip="$ESP_LBA_FIRST" count="$ESP_SECTORS" status=none

# Confirm dd produced the full ESP. dd is happy with short reads on
# regular files; an incomplete ESP would silently break El Torito boot.
ESP_OUT_SIZE="$(stat -c '%s' "$ESP_IMG")"
if [ "$ESP_OUT_SIZE" -ne "$ESP_BYTES" ]; then
    err "ESP extract short: got $ESP_OUT_SIZE bytes, expected $ESP_BYTES"
    exit 1
fi

# ---- Stage ISO root contents -------------------------------------------------
ISO_STAGE="$WORK_DIR/iso_stage"
mkdir -p "$ISO_STAGE/IPOS/installer" "$ISO_STAGE/IPOS/recovery"

# Generate the iso-format artifact manifest. ESP-bearing format set in
# build-manifest.sh check excludes iso (boot_config lives inside the ESP
# image, not at ISO root), so a raw iso manifest passes check without
# requiring a top-level boot_config entry.
MANIFEST_PATH="$ISO_STAGE/IPOS/manifest.json"
note "generating artifact manifest at /IPOS/manifest.json (--format iso)"
bash scripts/release/build-manifest.sh build --out "$MANIFEST_PATH" --format iso >/dev/null

# Bind the manifest to the ESP that will actually be embedded: verify-esp.sh
# --manifest reads the source disk image's ESP and asserts the required-name
# entries (bootloader / kernel / boot_config) hash-match the manifest. A
# stale --in or a manifest emitted from a different build-tree state would
# fail closed here, before xorriso burns the mismatched bytes into the ISO.
note "binding /IPOS/manifest.json to ESP from $IN_IMG (verify-esp --manifest)"
bash scripts/release/verify-esp.sh "$IN_IMG" --manifest "$MANIFEST_PATH" >/dev/null

# Placeholder marker so /IPOS/installer and /IPOS/recovery are not empty
# (some ISO toolchains drop empty directories; placing a 0-byte sentinel
# keeps the directory layout forward-compatible with the media-role
# detection feature in a later boot-media section).
: > "$ISO_STAGE/IPOS/installer/.placeholder"
: > "$ISO_STAGE/IPOS/recovery/.placeholder"

# Stage the ESP image inside the ISO tree. xorriso embeds file/dir mtimes
# in ISO9660 directory records, so every staged dir+file mtime MUST match
# the determinism epoch -- otherwise two runs from a clean tree differ in
# the seconds field (incident: build-iso 2026-05-06 -- the EFI/ dir was
# mkdir'd after the global touch sweep and inherited current time).
mkdir -p "$ISO_STAGE/EFI"
cp "$ESP_IMG" "$ISO_STAGE/EFI/esp.img"
find "$ISO_STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

# ---- Build the ISO -----------------------------------------------------------
# xorriso flags:
#   -as mkisofs                      mkisofs-compatible CLI
#   -V IPOS-INSTALLER                volume label
#   -modification-date=...           pinned volume timestamp (19700101000000)
#   -joliet                          Joliet extension for Windows tooling
#   -rational-rock                   Rock Ridge for POSIX paths/perms
#   -no-emul-boot                    El Torito no-emulation mode
#   -e EFI/esp.img                   El Torito boot image path inside ISO
#   -isohybrid-gpt-basdat            mark El Torito entry as GPT basic data
#   No `-b` / `-c` BIOS boot args -- UEFI-only by design.
note "running xorriso -> $OUT_ISO"
rm -f "$OUT_ISO"

xorriso -as mkisofs \
    -iso-level 3 \
    -V "IPOS_INSTALL" \
    -joliet \
    -rational-rock \
    -no-emul-boot \
    -e EFI/esp.img \
    -isohybrid-gpt-basdat \
    -o "$OUT_ISO" \
    "$ISO_STAGE" 2>&1 | sed 's/^/[xorriso] /' >&2

# ---- Final report ------------------------------------------------------------
SHA="$(sha256sum "$OUT_ISO" | awk '{print $1}')"
SZ="$(stat -c '%s' "$OUT_ISO")"
note "OUTPUT $OUT_ISO  size=$SZ  sha256=$SHA"

# El Torito sanity report
note "El Torito report:"
xorriso -indev "$OUT_ISO" -report_el_torito as_mkisofs 2>&1 \
    | grep -E "^-(b|e|partition_offset|isohybrid|hd_pre)" | sed 's/^/  /' >&2 || true

cat <<EOF
iso_path=$OUT_ISO
iso_size_bytes=$SZ
iso_sha256=$SHA
EOF
