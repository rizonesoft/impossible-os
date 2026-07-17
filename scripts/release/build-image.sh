#!/usr/bin/env bash
# build-image.sh -- produce a deterministic raw GPT disk image for release.
#
# Output: build/release/disk.img -- 512 MiB GPT image with three partitions
#   ESP (FAT32, 64 MiB)  : BOOTX64.EFI + kernel.exe + boot.conf
#   BlackBox (FAT32, 128 MiB) : Logs/Boot/Crash/Perf/Diag/Tools skeleton
#   IXFS-System (4 GiB-ish, fills rest) : zero-filled placeholder
#
# Determinism contract for the boot-media reproducibility feature:
#   -- timestamps zeroed (SOURCE_DATE_EPOCH=0; FAT volume creation, file
#      mtimes, GPT modify time all derive from this single epoch)
#   -- partition + disk GUIDs derived from a manifest-pinned seed
#      (UUID v5 against the boot-artifact-manifest namespace UUID with
#      seed = "<source_sha>|<format>") so a clean checkout reproduces the
#      same GUIDs without an out-of-band registry of random values
#   -- partition ordering fixed (ESP first, BlackBox second, IXFS third)
#   -- FAT volume label fixed (IPOS-ESP / BLACKBOX)
#   -- FAT volume serial derived from the partition GUID's first 8 hex chars
#      (so the FAT serial moves in lockstep with the GPT partition GUID)
#
# Two consecutive runs from a clean tree produce a byte-identical disk.img
# (verified by scripts/release/test-build-image.sh).
#
# Scope boundary: this script owns reproducibility properties only. The full
# release pipeline (compression, signing, multi-format wrap) lives in
# todo/15-installer-release/TODO-01-release-artifacts.md (raw image release +
# USB writer wrapper).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

# ---- Determinism epoch ------------------------------------------------------
# Anchor every timestamp-bearing tool to a fixed epoch. dosfstools >= 4.2
# honors SOURCE_DATE_EPOCH for FAT volume creation timestamps; sgdisk's
# disk modify time is overwritten via --set-alignment / --transform-bsd
# being absent (mod time only updates on writes, which we control).
export SOURCE_DATE_EPOCH=0
export TZ=UTC
export LC_ALL=C

# ---- Inputs -----------------------------------------------------------------
# Approach A (build idempotency): build/tools/BOOTX64.EFI is the UNSIGNED stub;
# signatures live at distinct .signed.efi paths. Ship the MOK-signed loader when
# it exists (keys/MOK.key present); a key-present build missing it is a hard
# error (never ship an unsigned loader from a signed build). Keyless dev builds
# ship the unsigned stub. NOTE: signing uses the self-signed MOK DEV cert today;
# the production cert is a future (~2yr) rotation, so "signed" == MOK-signed.
BL_PATH="build/tools/BOOTX64.EFI"
BL_SIGNED="build/tools/BOOTX64.signed.efi"
if [ -f "keys/MOK.key" ]; then
    if [ -f "$BL_SIGNED" ]; then
        BL_PATH="$BL_SIGNED"
    else
        err "MOK key present but signed loader $BL_SIGNED missing -- run scripts/build.sh"; exit 1
    fi
fi
KR_PATH="build/kernel.exe"
BOOT_CONF="resources/boot/boot.conf"
OUT_DIR="build/release"
OUT_IMG="$OUT_DIR/disk.img"
WORK_DIR=""  # per-invocation mktemp dir; populated below after $OUT_DIR exists

err() { printf '[ERROR] %s\n' "$*" >&2; }
note() { printf '[build-image] %s\n' "$*" >&2; }

usage() {
    cat <<EOF
Usage: $0 [--out PATH] [--format FMT] [--keep-work]

Options:
  --out PATH       Output image path (default: $OUT_IMG)
  --format FMT     Artifact format identifier seeded into UUID derivation
                   (default: raw). Must be a schema-legal format from
                   docs/release/boot-artifact-manifest.md.
  --role NAME      Media role written to /IPOS/role.txt on ESP and
                   BlackBox (default: normal). One of: normal, installer,
                   live, recovery, manufacturing, diagnostics.
  --manifest PATH  Stage the boot artifact manifest at /IPOS/manifest.json
                   on the ESP. Lets tools/bootimg/bootimg.py inspect the
                   image without a sidecar. The manifest must already
                   exist (produce via scripts/release/build-manifest.sh
                   build).
  --keep-work      Keep $WORK_DIR after success (debugging)
  -h, --help       Show this message
EOF
}

FORMAT="raw"
KEEP_WORK=0
ROLE="normal"
MANIFEST_PATH=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --out)        OUT_IMG="$2"; shift 2 ;;
        --format)     FORMAT="$2"; shift 2 ;;
        --role)       ROLE="$2"; shift 2 ;;
        --manifest)   MANIFEST_PATH="$2"; shift 2 ;;
        --keep-work)  KEEP_WORK=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) err "unknown arg: $1"; usage >&2; exit 2 ;;
    esac
done

# Validate --role against the boot-media role-detection feature's enum.
# The bootloader parses role.txt case-insensitively but we keep the
# producer's on-disk content lower-case for byte-identical determinism.
case "$ROLE" in
    normal|installer|live|recovery|manufacturing|diagnostics) ;;
    *) err "invalid --role: $ROLE (must be one of: normal, installer, live, recovery, manufacturing, diagnostics)"; exit 2 ;;
esac

# Validate --format against the same enum that build-manifest.sh and the
# schema doc enforce. Without this, a typo (e.g. --format raaw) seeds the
# UUID derivation with a value that no manifest can describe with the same
# seed, producing a deterministic-but-orphan disk image.
case "$FORMAT" in
    raw|usb|vhd|vhdx|vdi|iso|qcow2|ova|recovery|installer) ;;
    *) err "invalid --format: $FORMAT (must be one of: raw, usb, vhd, vhdx, vdi, iso, qcow2, ova, recovery, installer)"; exit 2 ;;
esac

for f in "$BL_PATH" "$KR_PATH" "$BOOT_CONF"; do
    [ -f "$f" ] || { err "missing input: $f (run scripts/build.sh first)"; exit 1; }
done

for tool in sgdisk mkfs.fat mcopy mmd mtype mdir python3 sha256sum truncate; do
    command -v "$tool" >/dev/null || { err "missing tool: $tool"; exit 1; }
done

# ---- Seed derivation --------------------------------------------------------
# source_sha is the git HEAD; in a shallow clone we still see the current
# commit because release builds always run on a checked-out tree.
SOURCE_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
if [ "$SOURCE_SHA" = "unknown" ]; then
    err "git rev-parse HEAD failed -- this script requires a checked-out tree"
    exit 1
fi
SEED="${SOURCE_SHA}|${FORMAT}"
note "source_sha=$SOURCE_SHA format=$FORMAT"

# UUID v5 namespace shared with build-manifest.sh (boot-artifact-manifest).
NAMESPACE="6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d"

derive_uuid() {
    # $1 = label seed; produces a deterministic UUID v5.
    SEED="$SEED" LABEL="$1" NAMESPACE="$NAMESPACE" python3 - <<'PY'
import os, uuid
ns = uuid.UUID(os.environ["NAMESPACE"])
print(uuid.uuid5(ns, os.environ["SEED"] + "|" + os.environ["LABEL"]))
PY
}

DISK_GUID="$(derive_uuid disk)"
ESP_GUID="$(derive_uuid esp)"
BB_GUID="$(derive_uuid blackbox)"
IXFS_GUID="$(derive_uuid ixfs)"

# FAT volume serial: 8 hex chars from the partition GUID. mkfs.fat -i wants
# 8 hex digits as a single argument; we slice the first 8 from the GUID
# (stripping the dashes first).
fat_serial() { printf '%s' "$1" | tr -d '-' | head -c 8; }
ESP_VOLID="$(fat_serial "$ESP_GUID")"
BB_VOLID="$(fat_serial "$BB_GUID")"

note "DISK_GUID=$DISK_GUID"
note "ESP_GUID=$ESP_GUID  ESP_VOLID=$ESP_VOLID"
note "BB_GUID=$BB_GUID    BB_VOLID=$BB_VOLID"
note "IXFS_GUID=$IXFS_GUID"

# ---- Layout (must match Makefile system-disk for boot-path compatibility) ---
SECTOR=512
DISK_SIZE_BYTES=$((512 * 1024 * 1024))            # 512 MiB
ESP_LBA_FIRST=2048                                # 1 MiB align
ESP_SIZE_MIB=64
ESP_SECTORS=$((ESP_SIZE_MIB * 1024 * 2))           # 64 MiB / 512 B/sector
ESP_LBA_LAST=$((ESP_LBA_FIRST + ESP_SECTORS - 1))

BB_LBA_FIRST=$((ESP_LBA_LAST + 1))
BB_SIZE_MIB=128
BB_SECTORS=$((BB_SIZE_MIB * 1024 * 2))
BB_LBA_LAST=$((BB_LBA_FIRST + BB_SECTORS - 1))

# IXFS fills from BB_LBA_LAST+1 to disk_end - 34 (GPT secondary header room).
IXFS_LBA_FIRST=$((BB_LBA_LAST + 1))
IXFS_LBA_LAST=$((DISK_SIZE_BYTES / SECTOR - 34))

# GPT partition type GUIDs (UEFI 2.10 Appendix A).
ESP_TYPE_GUID="C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
# Microsoft Basic Data partition (used here for BlackBox + IXFS until the
# Impossible-OS-specific type GUIDs are pinned in the IXFS-on-disk-format
# work owned by the kernel-fs domain).
MSBASIC_TYPE_GUID="EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

# ---- Build ------------------------------------------------------------------
mkdir -p "$OUT_DIR"
# Per-invocation work directory so parallel build-image.sh runs (different
# --out paths under CI or developer workflows) cannot race on a shared
# stage tree. Old fixed path (build/release/build-image-work) was reentrant-
# unsafe: two invocations would clobber each other's mcopy stages and the
# first to exit would rm -rf the other's working state.
WORK_DIR="$(mktemp -d "$OUT_DIR/build-image-work.XXXXXX")"
trap '[ "$KEEP_WORK" -eq 1 ] || rm -rf "$WORK_DIR"' EXIT

# 1. Allocate the empty image. We rm -f first so a pre-existing $OUT_IMG
#    (e.g. from a prior run with the same path) cannot leak stale bytes
#    into untouched regions like the IXFS zero-fill zone -- truncate on an
#    existing file at the same size is a no-op for content.
rm -f "$OUT_IMG"
truncate -s "$DISK_SIZE_BYTES" "$OUT_IMG"

# 2. GPT header + partition table with deterministic GUIDs.
sgdisk \
    --zap-all \
    --disk-guid="$DISK_GUID" \
    --new=1:$ESP_LBA_FIRST:$ESP_LBA_LAST \
    --typecode=1:"$ESP_TYPE_GUID" \
    --partition-guid=1:"$ESP_GUID" \
    --change-name=1:"ESP" \
    --new=2:$BB_LBA_FIRST:$BB_LBA_LAST \
    --typecode=2:"$MSBASIC_TYPE_GUID" \
    --partition-guid=2:"$BB_GUID" \
    --change-name=2:"BlackBox" \
    --new=3:$IXFS_LBA_FIRST:$IXFS_LBA_LAST \
    --typecode=3:"$MSBASIC_TYPE_GUID" \
    --partition-guid=3:"$IXFS_GUID" \
    --change-name=3:"IXFS-System" \
    "$OUT_IMG" >/dev/null

# 3. Format ESP as FAT32 with the derived volume serial.
# `-s 1` (one 512 B sector per cluster) gives 131072 clusters in the 64 MiB
# ESP, which is above the FAT32 minimum cluster count (65525) per the
# Microsoft FAT specification. With the default mkfs.fat cluster size of 8
# sectors (4 KiB), 64 MiB only yields 16384 clusters -- below the FAT32
# spec minimum. mkfs.fat warns but creates the volume anyway; OVMF's FAT
# driver then rejects it as malformed and falls through to PXE boot. Bug
# discovered while wiring boot-test-vhdx.sh (the raw image was sha-stable
# but not bootable on OVMF until this flag landed). FAT32 is the only
# filesystem the UEFI specification mandates the firmware support on the
# ESP, so specification compliance is the gate, not "what mkfs.fat will
# silently accept".
mkfs.fat --invariant -F 32 -s 1 -i "$ESP_VOLID" -n "IPOS-ESP" \
    --offset "$ESP_LBA_FIRST" \
    "$OUT_IMG" "$((ESP_SECTORS / 2))" >/dev/null   # size in 1 KiB blocks

# 4. Stage ESP files with deterministic mtimes.
ESP_STAGE="$WORK_DIR/esp_stage"
mkdir -p "$ESP_STAGE/EFI/BOOT" "$ESP_STAGE/EFI/ImpossibleOS" "$ESP_STAGE/boot" \
         "$ESP_STAGE/IPOS"
cp "$BL_PATH" "$ESP_STAGE/EFI/BOOT/BOOTX64.EFI"
# Release-flavor proof: run the FULL A+B+C gate before packaging, not just the
# provenance sub-check. The .ipos.provenance marker alone only proves Make chose
# the off branch; an off-flavor kernel contaminated by a stale or accidentally
# added test object still carries the marker. PART A (seam symbols) + PART B
# (link inputs) + PART C (provenance) together prove the kernel is clean. This
# packaging path is INDEPENDENT of .github/workflows/release.yml (which never
# invokes build-image.sh), so it must enforce the whole proof itself. The gate
# fails closed if compile_commands.json or any proof input is missing.
if ! bash scripts/check-release-symbols.sh; then
    err "$KR_PATH failed the release-flavor proof -- refusing to package a test-flavor or contaminated kernel."
    err "Rebuild the pruned flavor with: KERNEL_TESTS=off bash scripts/build.sh clean"
    exit 1
fi
cp "$KR_PATH" "$ESP_STAGE/boot/kernel.exe"
cp "$BOOT_CONF" "$ESP_STAGE/EFI/ImpossibleOS/boot.conf"

# Seed the boot-entry store with the idempotent default. Running twice
# yields a byte-identical store, so re-runs of build-image do not
# perturb the image hash. The seed entry points at the same kernel
# path the bootloader's in-firmware fallback would synthesize, so a
# seeded store and a missing store boot the same kernel.
python3 "$REPO_ROOT/tools/bootcfg/bootcfg.py" emit-seed \
    "$ESP_STAGE/EFI/ImpossibleOS/bootentries.json" >/dev/null

# /IPOS/role.txt drives the boot-media role-detection feature: the
# bootloader reads this file from both ESP and BlackBox before kernel
# load and feeds the decision into boot_info->boot_media_role.
printf '%s\n' "$ROLE" > "$ESP_STAGE/IPOS/role.txt"

# Optional /IPOS/manifest.json staging: when --manifest is passed the
# offline-artifact-inspector feature (tools/bootimg/bootimg.py) can
# verify the image without a sidecar. Schema produced by
# scripts/release/build-manifest.sh; this script does not regenerate
# it (the caller picks which manifest to embed).
if [ -n "$MANIFEST_PATH" ]; then
    [ -f "$MANIFEST_PATH" ] || { err "missing --manifest input: $MANIFEST_PATH"; exit 1; }
    cp "$MANIFEST_PATH" "$ESP_STAGE/IPOS/manifest.json"
    note "staged manifest -> /IPOS/manifest.json from $MANIFEST_PATH"
fi

find "$ESP_STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

# 5. Populate ESP. mtools' mcopy preserves source mtimes (since we just
#    pinned them). MTOOLS_SKIP_CHECK=1 silences the cluster-size warning
#    that mtools prints when the FAT geometry differs from MS-DOS defaults
#    (cosmetic only; does not affect the image bytes).
export MTOOLS_SKIP_CHECK=1
mcopy -i "$OUT_IMG@@$((ESP_LBA_FIRST * SECTOR))" -s -m "$ESP_STAGE/EFI" "::"
mcopy -i "$OUT_IMG@@$((ESP_LBA_FIRST * SECTOR))" -s -m "$ESP_STAGE/boot" "::"
mcopy -i "$OUT_IMG@@$((ESP_LBA_FIRST * SECTOR))" -s -m "$ESP_STAGE/IPOS" "::"

# 6. Format BlackBox as FAT32.
mkfs.fat --invariant -F 32 -i "$BB_VOLID" -n "BLACKBOX" \
    --offset "$BB_LBA_FIRST" \
    "$OUT_IMG" "$((BB_SECTORS / 2))" >/dev/null

# 7. BlackBox skeleton. mtools mmd creates directories with the current
#    SOURCE_DATE_EPOCH-driven mtime; the marker file is stage-touched
#    before mcopy.
mmd -i "$OUT_IMG@@$((BB_LBA_FIRST * SECTOR))" \
    "::Logs" "::Boot" "::Crash" "::Perf" "::Diag" "::Tools" "::IPOS" \
    "::Crash/WER" "::Logs/Serial"

BB_STAGE="$WORK_DIR/blackbox_stage"
mkdir -p "$BB_STAGE"
printf 'BlackBox-v1' > "$BB_STAGE/blackbox-marker.txt"
# /IPOS/role.txt mirror on BlackBox: the bootloader cross-checks this
# against the ESP marker; both files must agree or the loader emits a
# [WARN] line and falls back to NORMAL.
printf '%s\n' "$ROLE" > "$BB_STAGE/role.txt"
touch -h -d "@$SOURCE_DATE_EPOCH" "$BB_STAGE/blackbox-marker.txt" "$BB_STAGE/role.txt"
mcopy -i "$OUT_IMG@@$((BB_LBA_FIRST * SECTOR))" -m \
    "$BB_STAGE/blackbox-marker.txt" "::Diag/blackbox-marker.txt"
mcopy -i "$OUT_IMG@@$((BB_LBA_FIRST * SECTOR))" -m \
    "$BB_STAGE/role.txt" "::IPOS/role.txt"

# 8. IXFS partition: format + populate with the system sysroot so the
#    kernel mounts C:\ at boot. Mirrors the Makefile system-disk recipe:
#    mkfs-ixfs --populate writes inode mtime fields as zero, so the IXFS
#    bytes are deterministic for byte-identical reproducibility (the
#    test-build-image.sh byte-identity gate stays green).
IXFS_TOOL="build/tools/mkfs-ixfs"
if [ ! -x "$IXFS_TOOL" ]; then
    err "missing tool: $IXFS_TOOL (run scripts/build.sh first)"
    exit 1
fi
SYSROOT_SRC="build/sysroot"
if [ ! -d "$SYSROOT_SRC" ]; then
    err "missing sysroot: $SYSROOT_SRC (run scripts/build.sh first)"
    exit 1
fi
# Minimum bootable-payload contract: cmd.exe MUST exist at the C:\ root or
# the kernel boots to "C:\ not mounted" (or "cmd.exe not found"); this
# preflight catches a stale or partial sysroot before we waste time
# producing an unbootable disk.img. A full sysroot-manifest validation is
# tracked separately by the user-platform SDK packaging work.
if [ ! -f "$SYSROOT_SRC/cmd.exe" ]; then
    err "sysroot missing required boot payload: $SYSROOT_SRC/cmd.exe"
    exit 1
fi

# Stage a kernel.sym mirror at C:\Impossible\System\kernel.sym so the
# kernel's symbol-table loader has a consistent path; the Makefile recipe
# does the same. SOURCE_DATE_EPOCH-pinned mtime keeps the staged tree
# byte-stable across runs.
SYSROOT_STAGE="$WORK_DIR/sysroot_stage"
cp -a "$SYSROOT_SRC" "$SYSROOT_STAGE"
mkdir -p "$SYSROOT_STAGE/Impossible/System/Logs/Serial"
if [ -f build/kernel.sym ]; then
    cp build/kernel.sym "$SYSROOT_STAGE/Impossible/System/kernel.sym"
fi
find "$SYSROOT_STAGE" -exec touch -h -d "@$SOURCE_DATE_EPOCH" {} +

# IXFS_PART_SIZE: bytes from IXFS_LBA_FIRST through IXFS_LBA_LAST inclusive.
IXFS_PART_BYTES=$(( (IXFS_LBA_LAST - IXFS_LBA_FIRST + 1) * SECTOR ))
"$IXFS_TOOL" \
    -o "$OUT_IMG" \
    -s "$IXFS_PART_BYTES" \
    -l "Impossible OS" \
    --offset $(( IXFS_LBA_FIRST * SECTOR )) \
    --populate "$SYSROOT_STAGE" >/dev/null

# 9. Final sha256 + size report.
SHA="$(sha256sum "$OUT_IMG" | awk '{print $1}')"
SZ="$(stat -c '%s' "$OUT_IMG")"
note "OUTPUT $OUT_IMG  size=$SZ  sha256=$SHA"

# 10. Re-emit the image fingerprint key=value pairs that build-manifest.sh
#     wraps into the schema; running build-image.sh standalone produces a
#     consumable summary on stdout for downstream tooling.
cat <<EOF
disk_image=$OUT_IMG
disk_size_bytes=$SZ
disk_sha256=$SHA
disk_guid=$DISK_GUID
esp_guid=$ESP_GUID
blackbox_guid=$BB_GUID
ixfs_guid=$IXFS_GUID
esp_volid=$ESP_VOLID
blackbox_volid=$BB_VOLID
source_sha=$SOURCE_SHA
artifact_format=$FORMAT
seed=$SEED
EOF
