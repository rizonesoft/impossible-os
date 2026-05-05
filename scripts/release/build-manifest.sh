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

    while [ $# -gt 0 ]; do
        case "$1" in
            --out) out="$2"; shift 2 ;;
            --format) format="$2"; shift 2 ;;
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

    BOOTLOADER_SHA="$bl_sha" \
    BOOTLOADER_SIZE="$bl_size" \
    KERNEL_SHA="$kernel_sha" \
    KERNEL_SIZE="$kernel_size" \
    BIV="$biv" \
    ARTIFACT_FORMAT="$format" \
    MEDIA_ROLE="$media_role" \
    SECURE_BOOT_STATUS="$secure_boot_status" \
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
    "partition_map": [
        {"index": 1, "name": "ESP",         "type_guid": "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "size_mib": 64,   "filesystem": "fat32"},
        {"index": 2, "name": "BlackBox",    "type_guid": "00000000-0000-0000-0000-000000000000", "size_mib": 128,  "filesystem": "fat32"},
        {"index": 3, "name": "IXFS-System", "type_guid": "00000000-0000-0000-0000-000000000000", "size_mib": 4096, "filesystem": "ixfs"},
    ],
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
ALLOWED_ENTRY_NAMES = {"bootloader", "kernel", "boot_entries", "blackbox_skeleton", "recovery_payloads"}

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
    # Required rows must each appear exactly once and pass validation.
    for required_entry in ("bootloader", "kernel"):
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
