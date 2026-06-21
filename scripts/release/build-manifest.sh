#!/usr/bin/env bash
# build-manifest.sh -- produce or verify the boot artifact manifest.
#
# Schema spec: docs/release/boot-artifact-manifest.md
# Schema owner: TODO-06 boot-media-image-installer-handoff (artifact matrix + manifest format)
# Production owner: TODO-01 release-artifacts in domain 15 (release pipeline)
#
# Subcommands:
#   build   Emit build/artifacts/manifest.json from current build outputs.
#   check   Verify that the manifest at the given path has all required fields populated.
#
# Design adoptions (Codex pre-code review):
# H1: partition_map shape extended in schema doc; check-mode validates structural fields
#     only. The bootloader-side artifact-signing-and-manifest-verification feature
#     cross-checks against the on-disk GPT.
# H2: boot_info_version sourced from build/boot-info-abi.kernel.json (binary-derived)
#     rather than include/kernel/boot_info.h (source) so stale-build / compile-time-
#     override drift cannot lie.
# M1: artifact_uuid is UUID v5 derived from artifact_format + bootloader_sha256 +
#     kernel_sha256 + boot_info_version, so byte-identical builds produce byte-identical
#     manifests (preserves the reproducible-image-build feature contract).
# M2: secure_boot_status derives from the sign-stamp produced by scripts/sign-efi.sh
#     (path via SIGN_FINGERPRINT_FILE env; default build/.signed-artifacts.fingerprint
#     matches the Makefile SIGN_FINGERPRINT variable); a missing or empty stamp implies
#     unsigned. The per-file ".signed" sentinel is NOT trusted.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO_ROOT"

err() { printf '[ERROR] %s\n' "$*" >&2; }

usage() {
    cat >&2 <<'USAGE'
usage:
  build-manifest.sh build [--out PATH] [--format FORMAT]
  build-manifest.sh check PATH

  build mode:
    --out PATH      Output path (default: build/artifacts/manifest.json).
    --format FORMAT One of raw|usb|vhd|vhdx|vdi|iso|qcow2|ova|recovery|installer
                    (default: raw).

  check mode:
    PATH            Path to a manifest.json to verify. Validates required top-level
                    fields, partition_map non-empty, entries[] has bootloader+kernel,
                    sha256 fields are 64-hex, and denormalized sha256 matches entries[].
                    GPT cross-check (disk_guid / first_lba / last_lba) is the
                    bootloader-side artifact-verification job.
USAGE
}

# ---- helpers -----------------------------------------------------------------

sha256_of() {
    local f="$1"
    if [ ! -f "$f" ]; then
        err "missing input file: $f"
        return 1
    fi
    sha256sum "$f" | awk '{print $1}'
}

size_of() {
    local f="$1"
    if [ ! -f "$f" ]; then
        echo 0
        return
    fi
    stat -c '%s' "$f"
}

toolchain_fingerprint() {
    # One-line concatenation of the three toolchain version strings used by
    # scripts/build.sh: clang (compiler), lld (linker), nasm (assembler).
    # Each command is invoked with --version and the first non-empty line is
    # taken; missing tools collapse to "missing-<tool>" so a partial dev
    # host produces a self-describing rather than empty fingerprint.
    local clang_v lld_v nasm_v
    clang_v="$(clang-19 --version 2>/dev/null | head -1)"; clang_v="${clang_v:-missing-clang}"
    lld_v="$(ld.lld-19 --version 2>/dev/null | head -1)"; lld_v="${lld_v:-missing-lld}"
    nasm_v="$(nasm -v 2>/dev/null | head -1)"; nasm_v="${nasm_v:-missing-nasm}"
    printf '%s | %s | %s' "$clang_v" "$lld_v" "$nasm_v"
}

read_boot_info_version_from_binary() {
    # Stale-binding guard: refuse to publish a manifest if the ABI JSON is
    # older than either artifact we are about to hash. Otherwise a release
    # step that rebuilt kernel.exe / BOOTX64.EFI without regenerating
    # boot-info-abi.kernel.json would bind current artifact hashes to a
    # stale boot_info_version field, defeating the binary-derived contract.
    local abi="${BOOT_INFO_ABI_FILE:-build/boot-info-abi.kernel.json}"
    if [ ! -f "$abi" ]; then
        err "missing $abi -- run scripts/build.sh first to generate the binary-derived ABI manifest"
        return 1
    fi
    local bl="build/tools/BOOTX64.EFI"
    local kr="build/kernel.exe"
    local abi_mt bl_mt kr_mt
    abi_mt="$(stat -c '%Y' "$abi" 2>/dev/null || echo 0)"
    bl_mt="$(stat -c '%Y' "$bl" 2>/dev/null || echo 0)"
    kr_mt="$(stat -c '%Y' "$kr" 2>/dev/null || echo 0)"
    if [ "$bl_mt" -gt "$abi_mt" ] || [ "$kr_mt" -gt "$abi_mt" ]; then
        err "$abi is older than build artifacts; rebuild via scripts/build.sh to regenerate the binary-derived ABI manifest"
        return 1
    fi
    BOOT_INFO_ABI_FILE="$abi" python3 -c '
import json, os
with open(os.environ["BOOT_INFO_ABI_FILE"]) as f:
    print(json.load(f)["version"])
'
}

detect_secure_boot_status() {
    # Trust the sign-stamp pair produced by scripts/sign-efi.sh + Makefile:
    #   SIGN_FINGERPRINT (cert identity) AND SIGN_STAMP (rebuild-dependency
    #   stamp tied to the EFI/UKI prerequisites). Both must exist AND the
    #   stamp must be newer than the signed artifacts, otherwise the
    #   artifacts were rebuilt after the last successful signing run and
    #   the manifest must report unsigned.
    local stamp_fp="${SIGN_FINGERPRINT_FILE:-build/.signed-artifacts.fingerprint}"
    local stamp_mk="${SIGN_STAMP_FILE:-build/.signed-artifacts.stamp}"
    local bl="build/tools/BOOTX64.EFI"
    local kr="build/kernel.exe"

    if [ ! -s "$stamp_fp" ] || [ ! -e "$stamp_mk" ]; then
        echo "unsigned"
        return
    fi
    if [ ! -e "$bl" ] || [ ! -e "$kr" ]; then
        echo "unsigned"
        return
    fi
    if [ "$stamp_mk" -ot "$bl" ] || [ "$stamp_mk" -ot "$kr" ]; then
        echo "unsigned"  # stale: artifacts rebuilt after last signing run
        return
    fi
    echo "signed"
}

# ---- build mode --------------------------------------------------------------

cmd_build() {
    local out="build/artifacts/manifest.json"
    local format="raw"
    local vm_image_path=""

    while [ $# -gt 0 ]; do
        case "$1" in
            --out) out="$2"; shift 2 ;;
            --format) format="$2"; shift 2 ;;
            --vm-image) vm_image_path="$2"; shift 2 ;;
            -h|--help) usage; exit 0 ;;
            *) err "unknown build flag: $1"; usage; exit 2 ;;
        esac
    done

    case "$format" in
        raw|usb|vhd|vhdx|vdi|iso|qcow2|ova|recovery|installer) ;;
        *) err "invalid --format: $format"; exit 2 ;;
    esac

    local bootloader_path="build/tools/BOOTX64.EFI"
    local kernel_path="build/kernel.exe"

    # Approach A: when the build is signed, the shipped + attested bootloader is
    # the SIGNED artifact at a distinct path (build/tools/BOOTX64.signed.efi).
    # Bind the manifest's bootloader hash to THAT, not the unsigned canonical, so
    # manifest-based ESP verification certifies the file that actually ships.
    if [ "$(detect_secure_boot_status)" = "signed" ]; then
        bootloader_path="build/tools/BOOTX64.signed.efi"
        if [ ! -f "$bootloader_path" ]; then
            err "secure_boot_status=signed but $bootloader_path missing -- run scripts/build.sh"
            exit 1
        fi
    fi

    for f in "$bootloader_path" "$kernel_path"; do
        if [ ! -f "$f" ]; then
            err "missing required build artifact: $f (run scripts/build.sh first)"
            exit 1
        fi
    done

    local bl_sha kernel_sha bl_size kernel_size
    bl_sha="$(sha256_of "$bootloader_path")"
    kernel_sha="$(sha256_of "$kernel_path")"
    bl_size="$(size_of "$bootloader_path")"
    kernel_size="$(size_of "$kernel_path")"

    local biv
    biv="$(read_boot_info_version_from_binary)"
    if [ -z "$biv" ]; then
        err "could not derive boot_info_version from build/boot-info-abi.kernel.json"
        exit 1
    fi

    local secure_boot_status
    secure_boot_status="$(detect_secure_boot_status)"

    mkdir -p "$(dirname "$out")"

    # Cross-field consistency: role-bearing formats imply matching media_role.
    # Generic disk formats default to "normal".
    local media_role
    case "$format" in
        installer) media_role="installer" ;;
        recovery)  media_role="recovery"  ;;
        *)         media_role="normal"    ;;
    esac

    # Provenance fields (additive, optional in v1 schema): record the
    # toolchain identity, source commit, and the seed used by deterministic
    # partition GUID derivation in build-image.sh, so a release manifest
    # carries everything needed to reproduce the artifact byte-for-byte.
    local toolchain_version source_sha manifest_seed
    toolchain_version="$(toolchain_fingerprint)"
    source_sha="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    manifest_seed="${source_sha}|${format}"

    # boot.conf is required ESP content per the deterministic image producer
    # and both ESP/USB verifiers, so it has to land in entries[] for
    # manifest-based bootloader verification to cover it. The source file
    # MUST exist; emitting a manifest without boot_config would let it pass
    # `check` and then fail `verify-esp --manifest` -- internally inconsistent.
    local boot_conf_path="resources/boot/boot.conf"
    if [ ! -f "$boot_conf_path" ]; then
        err "missing required ESP source file: $boot_conf_path -- cannot emit boot_config entry"
        exit 1
    fi
    local boot_conf_sha boot_conf_size
    boot_conf_sha="$(sha256_of "$boot_conf_path")"
    boot_conf_size="$(size_of "$boot_conf_path")"

    # vm_image_metadata: derive from --vm-image PATH when supplied OR from
    # the conventional build/release/disk.<ext> location when --format names
    # a VM-container format (vhd / vhdx / vdi / qcow2 / ova). The field is
    # OPTIONAL in v1 (additive top-level field per schema policy) -- a
    # raw/usb/iso manifest skips it entirely.
    local vm_format="" vm_subformat="" vm_block_size_bytes="" vm_virtual_size_bytes=""
    if [ -z "$vm_image_path" ]; then
        case "$format" in
            vhd)   vm_image_path="build/release/disk.vhd"   ;;
            vhdx)  vm_image_path="build/release/disk.vhdx"  ;;
            vdi)   vm_image_path="build/release/disk.vdi"   ;;
            qcow2) vm_image_path="build/release/disk.qcow2" ;;
        esac
    fi
    if [ -n "$vm_image_path" ] && [ -f "$vm_image_path" ]; then
        if ! command -v qemu-img >/dev/null; then
            err "vm_image_metadata derivation needs qemu-img (install qemu-utils)"
            exit 1
        fi
        # Single python invocation parses the qemu-img JSON once and emits
        # all four fields tab-separated (was four serialized python startups
        # at ~50ms each = 200ms wasted on identical JSON parses).
        local vm_info_json vm_fields
        vm_info_json="$(qemu-img info --output=json "$vm_image_path")"
        vm_fields="$(printf '%s' "$vm_info_json" | python3 -c '
import json, sys
m = json.load(sys.stdin)
# qemu-img reports legacy Microsoft VHD (Connectix Virtual PC) under the
# name "vpc"; manifest schema standardizes on "vhd". Normalize at
# extraction so producer and check always round-trip on the same name.
fmt = {"vpc": "vhd"}.get(m["format"], m["format"])
vsz = m.get("virtual-size", 0)
fsi = m.get("format-specific", {}).get("data", {})
sub = fsi.get("subformat", "dynamic" if fmt in ("vhdx", "vdi", "qcow2") else "")
# VHDX + qcow2 expose container block size as top-level cluster-size; VDI
# does not surface it, so we default to the 1 MiB block VDI ships with.
bs = m.get("cluster-size") or fsi.get("block-size") or fsi.get("cluster_size")
if not bs and fmt == "vdi":
    bs = 1048576
print("\t".join((fmt, sub, str(bs or 0), str(vsz))))
')"
        IFS=$'\t' read -r vm_format vm_subformat vm_block_size_bytes vm_virtual_size_bytes <<< "$vm_fields"
    elif [ -n "$vm_image_path" ]; then
        err "missing --vm-image / conventional VM image: $vm_image_path"
        exit 1
    fi

    # Fail closed when vm_image_metadata extraction could not produce real
    # sizes: emitting zeros would silently mask qemu-img schema drift, and
    # check mode rejects zero anyway -- failing here is the same answer
    # but with a clearer error pointing at the producer's parser.
    if [ -n "$vm_format" ]; then
        if [ -z "$vm_virtual_size_bytes" ] || [ "$vm_virtual_size_bytes" = "0" ]; then
            err "qemu-img info did not report a positive virtual-size for $vm_image_path"
            exit 1
        fi
        if [ -z "$vm_block_size_bytes" ] || [ "$vm_block_size_bytes" = "0" ]; then
            err "qemu-img info did not report a positive container block size for $vm_image_path (format=$vm_format)"
            exit 1
        fi
    fi

    BOOTLOADER_SHA="$bl_sha" \
    BOOTLOADER_SIZE="$bl_size" \
    KERNEL_SHA="$kernel_sha" \
    KERNEL_SIZE="$kernel_size" \
    BOOT_CONF_SHA="$boot_conf_sha" \
    BOOT_CONF_SIZE="$boot_conf_size" \
    BIV="$biv" \
    ARTIFACT_FORMAT="$format" \
    MEDIA_ROLE="$media_role" \
    SECURE_BOOT_STATUS="$secure_boot_status" \
    TOOLCHAIN_VERSION="$toolchain_version" \
    SOURCE_SHA="$source_sha" \
    MANIFEST_SEED="$manifest_seed" \
    VM_IMAGE_FORMAT="$vm_format" \
    VM_IMAGE_SUBFORMAT="$vm_subformat" \
    VM_IMAGE_BLOCK_SIZE_BYTES="$vm_block_size_bytes" \
    VM_IMAGE_VIRTUAL_SIZE_BYTES="$vm_virtual_size_bytes" \
    OUT_PATH="$out" \
    python3 - <<'PY'
import json, os, uuid

# Deterministic UUID v5 from build-input fields. Identical inputs produce
# identical UUIDs, preserving the byte-reproducibility contract of the
# reproducible-raw-USB-image-build feature.
NS = uuid.UUID("6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d")  # project-local namespace
seed = "|".join((
    os.environ["ARTIFACT_FORMAT"],
    os.environ["BOOTLOADER_SHA"],
    os.environ["KERNEL_SHA"],
    os.environ["BIV"],
))
artifact_uuid = str(uuid.uuid5(NS, seed))

m = {
    "manifest_version":  1,
    "artifact_format":   os.environ["ARTIFACT_FORMAT"],
    "artifact_uuid":     artifact_uuid,
    "boot_target":       "uefi-x86_64",
    "boot_info_version": int(os.environ["BIV"]),
    "secure_boot_status": os.environ["SECURE_BOOT_STATUS"],
    "media_role":        os.environ["MEDIA_ROLE"],
}

# partition_map: format-aware. Disk artifacts (raw / usb / vhd / vhdx /
# vdi / qcow2 / ova / installer / recovery) carry a full GPT layout
# (ESP + BlackBox + IXFS); ISO artifacts only carry the El Torito ESP
# image inside ISO9660 -- no GPT, no BlackBox, no IXFS. Emitting the
# full disk partition_map for an iso would tell downstream consumers
# (verify-esp, the bootloader's GPT cross-check at load time, the
# offline inspector) to expect a layout that does not exist on the
# medium, leading to false-negative validation failures.
# Layout for disk formats matches scripts/release/build-image.sh:
# ESP 64 MiB at LBA 2048, BlackBox 128 MiB at LBA 133120,
# IXFS-System fills LBA 395264..1048542 (sectors 653279, ~318 MiB)
# of a 512 MiB total image. Exact LBA fields land in v2 once the
# GPT-backed layout becomes mandatory.
ESP_TYPE_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
MSBASIC_TYPE_GUID = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
if os.environ["ARTIFACT_FORMAT"] == "iso":
    m["partition_map"] = [
        {"index": 1, "name": "ESP", "type_guid": ESP_TYPE_GUID, "size_mib": 64, "filesystem": "fat32"},
    ]
else:
    m["partition_map"] = [
        {"index": 1, "name": "ESP",         "type_guid": ESP_TYPE_GUID,    "size_mib": 64,  "filesystem": "fat32"},
        {"index": 2, "name": "BlackBox",    "type_guid": MSBASIC_TYPE_GUID, "size_mib": 128, "filesystem": "fat32"},
        {"index": 3, "name": "IXFS-System", "type_guid": MSBASIC_TYPE_GUID, "size_mib": 318, "filesystem": "ixfs"},
    ]
m.update({
    "entries": [
        {"name": "bootloader",
         "path": "\\EFI\\BOOT\\BOOTX64.EFI",
         "sha256": os.environ["BOOTLOADER_SHA"],
         "size_bytes": int(os.environ["BOOTLOADER_SIZE"]),
         "optional": False},
        {"name": "kernel",
         "path": "\\boot\\kernel.exe",
         "sha256": os.environ["KERNEL_SHA"],
         "size_bytes": int(os.environ["KERNEL_SIZE"]),
         "optional": False},
    ],
    "bootloader_sha256": os.environ["BOOTLOADER_SHA"],
    "kernel_sha256":     os.environ["KERNEL_SHA"],
    "toolchain_version": os.environ["TOOLCHAIN_VERSION"],
    "source_sha":        os.environ["SOURCE_SHA"],
    "manifest_seed":     os.environ["MANIFEST_SEED"],
})

# boot_config is required ESP content (build mode aborted earlier if the
# source file was missing), so unconditionally append it to entries[].
m["entries"].append({
    "name":       "boot_config",
    "path":       "\\EFI\\ImpossibleOS\\boot.conf",
    "sha256":     os.environ["BOOT_CONF_SHA"],
    "size_bytes": int(os.environ["BOOT_CONF_SIZE"]),
    "optional":   False,
})

# vm_image_metadata: top-level optional v1 field; populated when --format
# is a VM-container format AND the converted artifact exists. Absent on
# raw / usb / iso manifests by design (no container layer to describe).
vm_fmt = os.environ.get("VM_IMAGE_FORMAT", "")
if vm_fmt:
    m["vm_image_metadata"] = {
        "format":              vm_fmt,
        "subformat":           os.environ.get("VM_IMAGE_SUBFORMAT", ""),
        "block_size_bytes":    int(os.environ.get("VM_IMAGE_BLOCK_SIZE_BYTES") or 0),
        "virtual_size_bytes":  int(os.environ.get("VM_IMAGE_VIRTUAL_SIZE_BYTES") or 0),
    }

with open(os.environ["OUT_PATH"], "w") as f:
    json.dump(m, f, indent=2, sort_keys=False)
    f.write("\n")
PY

    printf '%s\n' "$out"
}

# ---- check mode --------------------------------------------------------------

cmd_check() {
    if [ $# -ne 1 ]; then
        err "check requires exactly one path argument"
        usage
        exit 2
    fi
    local path="$1"
    if [ ! -f "$path" ]; then
        err "manifest not found: $path"
        exit 1
    fi

    MANIFEST_PATH="$path" python3 - <<'PY'
import json, os, sys, re, uuid

# Same namespace as build mode (keep in sync).
UUID_NS = uuid.UUID("6f1b3c4a-1d4e-5a6b-8c9d-0e1f2a3b4c5d")

path = os.environ["MANIFEST_PATH"]
try:
    with open(path) as f:
        m = json.load(f)
except json.JSONDecodeError as e:
    sys.stderr.write(f"[ERROR] manifest is not valid JSON: {e}\n")
    sys.exit(1)

required_fields = [
    "manifest_version",
    "artifact_format",
    "artifact_uuid",
    "boot_target",
    "boot_info_version",
    "secure_boot_status",
    "media_role",
    "partition_map",
    "entries",
    "bootloader_sha256",
    "kernel_sha256",
]

missing = [k for k in required_fields if k not in m]
if missing:
    for k in missing:
        sys.stderr.write(f"[ERROR] required field missing: {k}\n")
    sys.exit(1)

errors = []

# v1 schema-version invariant: this script ships v1; future bumps invalidate it
# until the script is updated. Bootloader-side handling of manifest_version > 1
# is a separate concern (see schema doc).
if m["manifest_version"] != 1:
    errors.append(f"manifest_version must be 1 (this is the v1 packaging gate; got: {m['manifest_version']!r})")

# Enum: artifact_format.
ALLOWED_FORMATS = {"raw", "usb", "vhd", "vhdx", "vdi", "iso", "qcow2", "ova", "recovery", "installer"}
if m["artifact_format"] not in ALLOWED_FORMATS:
    errors.append(f"artifact_format must be one of {sorted(ALLOWED_FORMATS)} (got: {m['artifact_format']!r})")

# UUID v5 string format: 8-4-4-4-12 lowercase hex with dashes.
uuid_re = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
if not isinstance(m["artifact_uuid"], str) or not uuid_re.match(m["artifact_uuid"]):
    errors.append(f"artifact_uuid must match 8-4-4-4-12 lowercase hex (got: {m['artifact_uuid']!r})")
else:
    # Semantic check: artifact_uuid MUST be UUID v5 of (artifact_format |
    # bootloader_sha256 | kernel_sha256 | boot_info_version) under the
    # project namespace, matching build mode. A syntactically-valid but
    # content-mismatched UUID is rejected so a tampered manifest can't
    # claim a different artifact identity than its hashes describe.
    if (isinstance(m.get("artifact_format"), str)
        and isinstance(m.get("bootloader_sha256"), str)
        and isinstance(m.get("kernel_sha256"), str)
        and isinstance(m.get("boot_info_version"), int)
        and not isinstance(m.get("boot_info_version"), bool)):
        seed = "|".join((
            m["artifact_format"],
            m["bootloader_sha256"],
            m["kernel_sha256"],
            str(m["boot_info_version"]),
        ))
        expected = str(uuid.uuid5(UUID_NS, seed))
        if m["artifact_uuid"] != expected:
            errors.append(
                f"artifact_uuid does not match deterministic UUID v5 of "
                f"artifact_format|bootloader_sha256|kernel_sha256|boot_info_version "
                f"(expected: {expected}, got: {m['artifact_uuid']})"
            )

# Optional top-level GPT fields: validate shape when present (v1-optional).
guid_top_re = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
if "disk_guid" in m and m["disk_guid"] is not None:
    if not isinstance(m["disk_guid"], str) or not guid_top_re.match(m["disk_guid"]):
        errors.append(f"disk_guid must match GUID format when present (got: {m['disk_guid']!r})")
if "sector_size" in m and m["sector_size"] is not None:
    ss = m["sector_size"]
    # Allow common physical sector sizes; reject negative / huge / non-int.
    if not isinstance(ss, int) or isinstance(ss, bool) or ss not in (512, 4096):
        errors.append(f"sector_size must be 512 or 4096 when present (got: {ss!r})")
if "total_sectors" in m and m["total_sectors"] is not None:
    ts = m["total_sectors"]
    if not isinstance(ts, int) or isinstance(ts, bool) or ts < 0 or ts > 0xFFFFFFFFFFFFFFFF:
        errors.append(f"total_sectors must be a non-negative integer <= 2^64-1 when present (got: {ts!r})")

# boot_target: non-empty string.
if not isinstance(m["boot_target"], str) or not m["boot_target"]:
    errors.append(f"boot_target must be a non-empty string (got: {m['boot_target']!r})")

# boot_info_version: non-negative integer; bound to UINT16 since the on-disk
# header field is uint16_t (see boot_info.h header.version).
biv = m["boot_info_version"]
if not isinstance(biv, int) or isinstance(biv, bool) or biv < 0 or biv > 0xFFFF:
    errors.append(f"boot_info_version must be a non-negative integer <= 65535 (got: {biv!r})")

if not isinstance(m["partition_map"], list) or len(m["partition_map"]) == 0:
    errors.append("partition_map must be a non-empty array")
else:
    # Validate each partition_map row's structural shape.
    guid_re = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
    ALLOWED_FS = {"fat32", "ixfs", "ntfs"}
    for i, p in enumerate(m["partition_map"]):
        label = f"partition_map[{i}]"
        if not isinstance(p, dict):
            errors.append(f"{label} must be an object")
            continue
        if not isinstance(p.get("index"), int) or isinstance(p.get("index"), bool) or p.get("index") < 1:
            errors.append(f"{label}.index must be a positive integer (got: {p.get('index')!r})")
        if not isinstance(p.get("name"), str) or not p.get("name"):
            errors.append(f"{label}.name must be a non-empty string")
        if not isinstance(p.get("type_guid"), str) or not guid_re.match(p.get("type_guid", "")):
            errors.append(f"{label}.type_guid must match 8-4-4-4-12 hex GUID (got: {p.get('type_guid')!r})")
        sm = p.get("size_mib")
        if not isinstance(sm, int) or isinstance(sm, bool) or sm < 0 or sm > 0xFFFFFFFF:
            errors.append(f"{label}.size_mib must be a non-negative integer <= 2^32-1 (got: {sm!r})")
        # Optional v1 fields: validate shape when present, do not require.
        if "filesystem" in p and p["filesystem"] is not None:
            if p["filesystem"] not in ALLOWED_FS:
                errors.append(f"{label}.filesystem must be one of {sorted(ALLOWED_FS)} or absent (got: {p['filesystem']!r})")
        for opt_guid in ("unique_partition_guid",):
            if opt_guid in p and p[opt_guid] is not None:
                if not isinstance(p[opt_guid], str) or not guid_re.match(p[opt_guid]):
                    errors.append(f"{label}.{opt_guid} must match GUID format when present (got: {p[opt_guid]!r})")
        for opt_int in ("first_lba", "last_lba", "attributes"):
            if opt_int in p and p[opt_int] is not None:
                v = p[opt_int]
                if not isinstance(v, int) or isinstance(v, bool) or v < 0 or v > 0xFFFFFFFFFFFFFFFF:
                    errors.append(f"{label}.{opt_int} must be a non-negative integer <= 2^64-1 when present (got: {v!r})")

hex64 = re.compile(r"^[0-9a-fA-F]{64}$")
ALLOWED_ENTRY_NAMES = {"bootloader", "kernel", "boot_config", "boot_entries", "blackbox_skeleton", "recovery_payloads"}

def validate_entry_row(e, required):
    """Validate one entries[] row. `required` toggles strict-required-row checks.

    Schema-version policy (manifest_version=1) allows additive optional entry
    names without bumping the version: the verifier skips unknown optional
    entries shape-wise, but still enforces path/sha256/size_bytes/optional
    so a malformed row cannot hide behind an unknown name.
    """
    if not isinstance(e, dict):
        return [f"entries[] item is not an object: {e!r}"]
    row_errs = []
    name = e.get("name")
    if not isinstance(name, str) or not name:
        row_errs.append(f"entries[] item missing string name (got: {name!r})")
        return row_errs
    if name not in ALLOWED_ENTRY_NAMES:
        if required or e.get("optional") is not True:
            row_errs.append(f"entries[].name must be one of {sorted(ALLOWED_ENTRY_NAMES)} when not optional (got: {name!r})")
            return row_errs
        # Additive optional row: validate shape only, name is forward-compatible.
    label = f"entries[name={name}]"
    if "path" not in e or not isinstance(e["path"], str) or not e["path"]:
        row_errs.append(f"{label}.path missing or not a non-empty string")
    if "sha256" not in e or not isinstance(e["sha256"], str) or not hex64.match(e["sha256"]):
        row_errs.append(f"{label}.sha256 missing or not a 64-character hex string")
    sz = e.get("size_bytes")
    # Bound size_bytes to UINT64 max so consumers reading uint64_t cannot overflow.
    if not isinstance(sz, int) or isinstance(sz, bool) or sz < 0 or sz > 0xFFFFFFFFFFFFFFFF:
        row_errs.append(f"{label}.size_bytes must be a non-negative integer <= 2^64-1 (got: {sz!r})")
    if "optional" not in e or not isinstance(e["optional"], bool):
        row_errs.append(f"{label}.optional missing or not a boolean")
    if required and e.get("optional") is True:
        row_errs.append(f"{label} is a required entry but has optional=true")
    return row_errs

if not isinstance(m["entries"], list):
    errors.append("entries must be an array")
else:
    seen_names = []
    for e in m["entries"]:
        seen_names.append(e.get("name") if isinstance(e, dict) else None)
    # ESP-bearing disk formats embed boot.conf; the manifest verifier
    # (verify-esp.sh --manifest) fails closed when boot_config is absent,
    # so check mode must enforce the same required-entry set or a hand-
    # crafted manifest could pass `check` and fail `verify-esp` -- two
    # official gates disagreeing about what a valid manifest looks like.
    ESP_FORMATS = {"raw", "usb", "vhd", "vhdx", "vdi", "qcow2", "ova", "installer", "recovery"}
    required_names = ["bootloader", "kernel"]
    if m.get("artifact_format") in ESP_FORMATS:
        required_names.append("boot_config")
    for required_entry in required_names:
        count = seen_names.count(required_entry)
        if count == 0:
            errors.append(f"entries[] missing required name={required_entry}")
        elif count > 1:
            errors.append(f"entries[] has {count} rows with name={required_entry}; expected exactly 1")
        else:
            row = next(e for e in m["entries"] if isinstance(e, dict) and e.get("name") == required_entry)
            errors.extend(validate_entry_row(row, required=True))
    # Optional rows must still validate as objects with the right shape if present.
    for e in m["entries"]:
        if not isinstance(e, dict):
            continue
        if e.get("name") in ("bootloader", "kernel"):
            continue  # already validated above
        errors.extend(validate_entry_row(e, required=False))

for k in ("bootloader_sha256", "kernel_sha256"):
    v = m.get(k, "")
    if not isinstance(v, str) or not hex64.match(v):
        errors.append(f"{k} must be a 64-character hex sha256 (got: {v!r})")

# Cross-check denormalized sha256 against entries[].
def find_entry_sha(name):
    if not isinstance(m["entries"], list):
        return None
    for e in m["entries"]:
        if isinstance(e, dict) and e.get("name") == name:
            return e.get("sha256")
    return None

bl_entry = find_entry_sha("bootloader")
if bl_entry is not None and bl_entry != m["bootloader_sha256"]:
    errors.append("bootloader_sha256 does not match entries[name=bootloader].sha256")
kr_entry = find_entry_sha("kernel")
if kr_entry is not None and kr_entry != m["kernel_sha256"]:
    errors.append("kernel_sha256 does not match entries[name=kernel].sha256")

if m["secure_boot_status"] not in ("signed", "unsigned", "unknown"):
    errors.append(f"secure_boot_status must be signed|unsigned|unknown (got: {m['secure_boot_status']!r})")

if m["media_role"] not in ("normal", "installer", "live", "recovery", "manufacturing", "diagnostics"):
    errors.append(f"media_role must be one of normal|installer|live|recovery|manufacturing|diagnostics (got: {m['media_role']!r})")

# Cross-field consistency: role-bearing artifact_format MUST agree with media_role.
# An installer-format manifest claiming media_role=normal would silently mislead
# the bootloader into running the installer image as normal boot media.
fmt = m.get("artifact_format")
mr  = m.get("media_role")
if fmt == "installer" and mr != "installer":
    errors.append(f"artifact_format=installer requires media_role=installer (got: {mr!r})")
if fmt == "recovery" and mr != "recovery":
    errors.append(f"artifact_format=recovery requires media_role=recovery (got: {mr!r})")

# Provenance fields: optional in v1, but if present they must be the right
# shape so a downstream consumer can rely on them without extra parsing.
hex40 = re.compile(r"^[0-9a-fA-F]{40}$")
if "source_sha" in m:
    v = m["source_sha"]
    if not isinstance(v, str) or (v != "unknown" and not hex40.match(v)):
        errors.append(f"source_sha must be 40-hex git SHA or 'unknown' (got: {v!r})")
if "toolchain_version" in m:
    v = m["toolchain_version"]
    if not isinstance(v, str) or not v:
        errors.append(f"toolchain_version must be a non-empty string (got: {v!r})")
if "manifest_seed" in m:
    v = m["manifest_seed"]
    if not isinstance(v, str) or "|" not in v:
        errors.append(f"manifest_seed must be '<source_sha>|<artifact_format>' (got: {v!r})")
    elif "source_sha" in m and "artifact_format" in m:
        expected = f"{m['source_sha']}|{m['artifact_format']}"
        if v != expected:
            errors.append(f"manifest_seed must equal '{expected}' (got: {v!r})")

# vm_image_metadata: optional top-level v1 field describing the VM-container
# layer. When present, every subfield is structurally validated; consumers
# unaware of the field MUST skip it (per schema policy "additive optional
# top-level field with documented ignore semantics").
VM_IMAGE_FORMATS = {"vhd", "vhdx", "vdi", "qcow2"}
VM_IMAGE_SUBFORMATS = {"dynamic", "fixed", ""}  # "" allowed for forward compat
if "vm_image_metadata" in m:
    v = m["vm_image_metadata"]
    if not isinstance(v, dict):
        errors.append(f"vm_image_metadata must be an object when present (got: {v!r})")
    else:
        fmt = v.get("format")
        if fmt not in VM_IMAGE_FORMATS:
            errors.append(f"vm_image_metadata.format must be one of {sorted(VM_IMAGE_FORMATS)} (got: {fmt!r})")
        sub = v.get("subformat")
        if not isinstance(sub, str) or sub not in VM_IMAGE_SUBFORMATS:
            errors.append(f"vm_image_metadata.subformat must be one of {sorted(s for s in VM_IMAGE_SUBFORMATS if s)} or '' (got: {sub!r})")
        # block_size_bytes and virtual_size_bytes MUST be positive when
        # vm_image_metadata is present: a manifest claiming "this is a VHDX"
        # with virtual_size_bytes=0 or block_size_bytes=0 describes an
        # impossible artifact and signals broken metadata extraction (e.g.
        # a qemu-img info schema change the parser does not recognize).
        # Failing closed here forces the producer to emit real numbers
        # instead of silently emitting zeros.
        bs = v.get("block_size_bytes")
        if not isinstance(bs, int) or isinstance(bs, bool) or bs <= 0 or bs > 0xFFFFFFFFFFFFFFFF:
            errors.append(f"vm_image_metadata.block_size_bytes must be a positive integer <= 2^64-1 (got: {bs!r})")
        vs = v.get("virtual_size_bytes")
        if not isinstance(vs, int) or isinstance(vs, bool) or vs <= 0 or vs > 0xFFFFFFFFFFFFFFFF:
            errors.append(f"vm_image_metadata.virtual_size_bytes must be a positive integer <= 2^64-1 (got: {vs!r})")
        # Cross-field consistency: vm_image_metadata.format SHOULD match
        # the top-level artifact_format when both are container formats.
        # raw/usb/iso/installer/recovery + vm_image_metadata together is
        # legal (a release pipeline may attach VM metadata to a raw image
        # for downstream wrapping), so we only warn-via-error when they
        # are both container formats and disagree.
        if (fmt in VM_IMAGE_FORMATS
            and m.get("artifact_format") in VM_IMAGE_FORMATS
            and fmt != m.get("artifact_format")):
            errors.append(
                f"vm_image_metadata.format={fmt!r} disagrees with artifact_format={m.get('artifact_format')!r}"
            )

if errors:
    for e in errors:
        sys.stderr.write(f"[ERROR] {e}\n")
    sys.exit(1)

sys.stdout.write(f"manifest OK: {path}\n")
PY
}

# ---- dispatch ---------------------------------------------------------------

if [ $# -lt 1 ]; then
    usage
    exit 2
fi

sub="$1"
shift
case "$sub" in
    build) cmd_build "$@" ;;
    check) cmd_check "$@" ;;
    -h|--help) usage; exit 0 ;;
    *) err "unknown subcommand: $sub"; usage; exit 2 ;;
esac
