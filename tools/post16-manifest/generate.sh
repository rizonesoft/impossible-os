#!/usr/bin/env bash
# ============================================================================
# generate.sh -- Emit build/post16-manifest.env from POST16 #define sources
#
# Extracts `#define POST16_<NAME> 0xHHHH` lines from the two authoritative
# source files and emits a shell-sourceable manifest plus a canonical
# required-set array. The smoke test sources this manifest instead of
# asserting raw log strings, so renaming a printf cannot silently break
# the boot-pattern check.
#
# Sources parsed:
#   include/kernel/boot_init.h   -- all POST16_* constants (kernel + bootloader
#                                   namespaces both live here)
#   src/boot/uefi/bootx64.c      -- bootloader-local POST16_BL_* constants
#
# Only codes in the 0xBxxx range are serial-observable today. The bootloader's
# post_code16() emits "[BOOT] POST 0xNNNN\n" on serial for every call; the
# kernel's boot_post_write16() writes to I/O port 0x80 + framebuffer only,
# not serial. The manifest records every POST16 constant for reference, but
# the REQUIRED set is filtered to bootloader codes that DO appear on serial.
#
# Usage: generate.sh
#   No flags. The required-set array (POST16_REQUIRED_NAMES) is curated
#   inside this script and validated against the extracted defines; adding
#   a new bootloader milestone requires editing this script explicitly.
#
# Output: build/post16-manifest.env
#
# Exit codes:
#   0 = manifest emitted OK
#   1 = collision detected OR required name not found in #define sources
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"

BOOT_INIT_H="$REPO_ROOT/include/kernel/boot_init.h"
BOOTX64_C="$REPO_ROOT/src/boot/uefi/bootx64.c"
OUT="$BUILD_DIR/post16-manifest.env"

if [ ! -f "$BOOT_INIT_H" ]; then
    echo "error: $BOOT_INIT_H not found" >&2
    exit 1
fi
if [ ! -f "$BOOTX64_C" ]; then
    echo "error: $BOOTX64_C not found" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"

# Extract "#define POST16_<NAME> 0xHHHH" from both files. AWK is the right
# tool: grep-heads cannot trim trailing comments reliably, and sed pipelines
# are harder to debug. Emits one VAR=0xHHHH per line, sorted by code value.
#
# Filter rules:
#   - Must match ^\s*#define\s+POST16_[A-Z0-9_]+\s+0x[0-9A-F]{4}
#   - Trailing /* ... */ comment allowed, stripped
#   - Duplicate NAMEs across files are a hard error (collision)
#   - Duplicate VALUES across different NAMEs are a hard error (unique
#     constraint; same as the boot-time uniqueness scan the kernel does
#     at startup)
declare -A NAME_TO_CODE
declare -A CODE_TO_NAME
ERRORS=0

extract_defines() {
    local file="$1"
    awk '
        /^[[:space:]]*#define[[:space:]]+POST16_[A-Z0-9_]+[[:space:]]+0x[0-9A-Fa-f]{4}/ {
            # $1=#define, $2=NAME, $3=0xHHHH (maybe with trailing comment)
            name = $2
            code = $3
            # Strip anything after the hex value (trailing comments)
            sub(/[[:space:]]*\/\*.*/, "", code)
            # Uppercase the hex digits for consistency
            gsub(/[a-f]/, "", code)  # kill lowercase letters
            # Re-read original code; simpler to uppercase via a case fold
            # Accept the value as-is; normalize to uppercase hex elsewhere.
            printf "%s %s\n", name, $3
        }
    ' "$file"
}

# Collect from both sources. Normalize hex to uppercase for the comparison
# map but preserve the original code format in the output.
while read -r name code; do
    # Strip trailing comment or whitespace from code
    code_clean="${code%%/\**}"
    code_clean="${code_clean// /}"
    code_upper="$(echo "$code_clean" | tr 'a-f' 'A-F')"

    if [ -n "${NAME_TO_CODE[$name]:-}" ]; then
        if [ "${NAME_TO_CODE[$name]}" != "$code_upper" ]; then
            echo "error: POST16 name collision: $name defined as ${NAME_TO_CODE[$name]} and $code_upper" >&2
            ERRORS=$((ERRORS + 1))
        fi
        # Same name+value in both files: intentional mirror (kernel + bootloader
        # may share the 0xB0xx namespace); accept silently.
        continue
    fi
    if [ -n "${CODE_TO_NAME[$code_upper]:-}" ]; then
        echo "error: POST16 value collision: $code_upper used by both ${CODE_TO_NAME[$code_upper]} and $name" >&2
        ERRORS=$((ERRORS + 1))
        continue
    fi
    NAME_TO_CODE[$name]="$code_upper"
    CODE_TO_NAME[$code_upper]="$name"
done < <(extract_defines "$BOOT_INIT_H"; extract_defines "$BOOTX64_C")

if [ "$ERRORS" -gt 0 ]; then
    echo "error: $ERRORS POST16 collision(s); refusing to emit manifest" >&2
    exit 1
fi

# Required set: POST16 codes observed on a clean boot (KVM, current tree).
# Update when new bootloader milestones land OR when a code is removed.
# Every name here MUST be defined in the extracted set above; missing name is
# a hard error (drift: someone renamed/removed a define without updating this
# list).
POST16_REQUIRED_NAMES=(
    POST16_BL_ENTRY
    POST16_BL_GOP
    POST16_BL_KERNEL_OPEN
    POST16_BL_KERNEL_LOAD
    POST16_BL_RSDP
    POST16_BL_ENTROPY
    POST16_BL_ENTROPY_OK
    POST16_BL_MEMMAP
    POST16_BL_EXIT_BS
    POST16_BL_PAGE_TABLES
    POST16_BL_HHDM_PLAN
    POST16_BL_HHDM_ARENA_OK
    POST16_BL_HHDM_NXE
    POST16_BL_HHDM_INSTALL
    POST16_BL_HHDM_OK
    POST16_BL_KERNEL_JUMP
    POST16_BL_USB_DISC
    POST16_BL_USB_DISC_OK
    POST16_BL_XHCI_DMA
    POST16_BL_XHCI_DMA_OK
    POST16_BL_XHCI_TAKEOVER
    POST16_BL_XHCI_TAKEOVER_OK
    POST16_BL_BOOT_DEV
    POST16_BL_BOOT_DEV_OK
    POST16_BL_BOOT_FS
    POST16_BL_BOOT_FS_OK
    POST16_BL_BOOT_VAR_EXT
    POST16_BL_BOOT_VAR_EXT_OK
    POST16_BL_SELF_MEASURE
    POST16_BL_SELF_MEASURE_OK
    POST16_BL_BOOT_POLICY
    POST16_BL_BOOT_POLICY_DECIDE
    POST16_BL_BOOT_POLICY_OK
    POST16_BL_COUNTER_SCAN
    POST16_BL_KIND_VALIDATE
    POST16_BL_KIND_VALIDATE_OK
    POST16_BL_AB_SELECT
    POST16_BL_AB_SELECT_OK
)

# Optional set: POST16 codes that ARE emitted by the bootloader but are
# scenario-dependent (error paths, fallback chains, rare hardware). These
# are classified so the inverse check below doesn't flag them as "unknown
# emission" -- but they're NOT asserted by the smoke test because they
# don't appear on a clean happy-path boot.
#
# Every entry needs a short reason so future maintainers know WHY it's
# optional rather than required. Format: NAME=reason
POST16_OPTIONAL_REASONS=(
    "POST16_BL_FALLBACK=scenario-dependent: boot-device fallback chain"
    "POST16_BL_FALLBACK_OK=scenario-dependent: boot-device fallback chain"
    "POST16_BL_ROLLBACK_REFUSE=scenario-dependent: anti-rollback halts only on downgrade attack"
    "POST16_BL_ROLLBACK_PASS=scenario-dependent: emitted only on boots that evaluate the counter"
    "POST16_BL_UKI_DETECT=scenario-dependent: UKI section probe always runs but only emits when LoadedImage is available"
    "POST16_BL_UKI_DETECT_OK=scenario-dependent: only emitted when invoked through a UKI artifact carrying a .linux section"
    "POST16_BL_ESP_INTEGRITY=scenario-dependent: ESP integrity gate runs only when boot device handle is present (skipped in PXE/RAM boot)"
    "POST16_BL_ESP_INTEGRITY_OK=scenario-dependent: ESP integrity gate exits via this code on UKI fast-skip OR after all sub-checks pass"
    "POST16_BL_ESP_GPT=scenario-dependent: GPT type-GUID check skipped on non-GPT boot media (MBR test images, network boot)"
    "POST16_BL_ESP_BPB=scenario-dependent: FAT BPB check requires partition BlockIO with usable BlockSize"
    "POST16_BL_ESP_FILES=scenario-dependent: required-files batch requires SimpleFS on the boot device handle"
    "POST16_BL_BOOT_POLICY_PARSE=scenario-dependent: emitted only when bootentries.json is present and readable on the ESP"
    "POST16_BL_COUNTER_DECR=scenario-dependent: emitted only when the policy ladder selects a real store entry (not FALLBACK_STORE_INVALID / NO_VIABLE)"
    "POST16_BL_MENU=scenario-dependent: emitted only when the policy ladder picks a soft entry AND >=2 viable candidates remain after filter"
)

# Build the optional-name set from the reasons array for fast lookup.
declare -A POST16_OPTIONAL_SET
for entry in "${POST16_OPTIONAL_REASONS[@]}"; do
    name="${entry%%=*}"
    POST16_OPTIONAL_SET[$name]=1
done

for name in "${POST16_REQUIRED_NAMES[@]}"; do
    if [ -z "${NAME_TO_CODE[$name]:-}" ]; then
        echo "error: required POST16 name '$name' not found in #define sources" >&2
        echo "       -- someone renamed/removed a define; update POST16_REQUIRED_NAMES in $0" >&2
        exit 1
    fi
    if [ -n "${POST16_OPTIONAL_SET[$name]:-}" ]; then
        echo "error: POST16 name '$name' appears in BOTH required and optional sets" >&2
        echo "       -- pick one; required wins for serial assertion semantics" >&2
        exit 1
    fi
done

for name in "${!POST16_OPTIONAL_SET[@]}"; do
    if [ -z "${NAME_TO_CODE[$name]:-}" ]; then
        echo "error: optional POST16 name '$name' not found in #define sources" >&2
        echo "       -- someone renamed/removed a define; update POST16_OPTIONAL_REASONS in $0" >&2
        exit 1
    fi
done

# Inverse validation: every post_code16(POST16_BL_*) call site in the
# bootloader MUST be classified as either required or optional. Catches
# the failure mode where someone adds a new boot milestone via a
# post_code16() call without adding it to the assertion set, which would
# leave the new phase unchecked by the smoke test. This is the drift the
# manifest exists to prevent.
#
# Whitespace-tolerant extractor. Capture EVERY post_code16(...) call as a
# whole unit, then require each captured argument to match the canonical
# literal form `POST16_[A-Z0-9_]+` exactly. This rejects uppercase aliases
# (CODE), macro-expanded names, and arithmetic expressions (POST16_BL_X+1)
# that a first-character exclusion would miss. Strip comments first so
# commented-out calls do not count as live emissions.
unclassified=()
non_literal=()
stripped=$(sed -E 's|//.*$||' "$BOOTX64_C" \
    | tr '\n' ' ' \
    | tr -s '[:space:]' ' ' \
    | sed -E 's|/\*[^*]*\*+([^/*][^*]*\*+)*/||g')

# Step 1: capture every post_code16(...); invocation, whitespace-tolerant.
# Requiring the trailing semicolon (with optional whitespace) excludes the
# function declaration + definition site (`static inline void post_code16(
# UINT16 code) { ... }`) which would otherwise match. Also disallows nested
# parens because our codebase does not use them and a nested paren would
# indicate an expression argument we reject anyway.
all_calls=$(echo "$stripped" \
    | grep -oE 'post_code16[[:space:]]*\([[:space:]]*[^)]*[[:space:]]*\)[[:space:]]*;' \
    | sed -E 's|[[:space:]]*;$||' \
    || true)

# Step 2: for each capture, extract the argument and classify. Canonical
# form: argument is a literal POST16_[A-Z0-9_]+ identifier with optional
# surrounding whitespace. Anything else (alias, expression, variable) is
# a hard error. The classifier cannot prove coverage for indirection.
literal_names=()
while read -r call; do
    [ -z "$call" ] && continue
    arg=$(echo "$call" \
        | sed -E 's|^post_code16[[:space:]]*\([[:space:]]*||; s|[[:space:]]*\)[[:space:]]*$||')
    # Forward declaration or prototype? Skip: arg is `<TYPE> <param-name>`.
    # Common UEFI types our bootloader uses as the post_code16 parameter.
    if [[ "$arg" =~ ^(UINT8|UINT16|UINT32|UINT64|UINTN|BOOLEAN|VOID|CHAR16|EFI_STATUS)[[:space:]]+[a-zA-Z_][a-zA-Z0-9_]*$ ]]; then
        continue
    fi
    # Canonical call form: arg is exactly a POST16_* identifier.
    if [[ "$arg" =~ ^POST16_[A-Z0-9_]+$ ]]; then
        literal_names+=("$arg")
    else
        non_literal+=("$call")
    fi
done <<< "$all_calls"

if [ ${#non_literal[@]} -gt 0 ]; then
    echo "error: $BOOTX64_C uses non-literal post_code16() argument(s):" >&2
    for call in "${non_literal[@]}"; do
        echo "       $call" >&2
    done
    echo "       Every post_code16() call must take a literal POST16_BL_*" >&2
    echo "       identifier so the manifest can classify it. Indirection" >&2
    echo "       (variables, aliases, expressions) defeats smoke-test" >&2
    echo "       coverage verification." >&2
    exit 1
fi

# Step 3: classify each literal name as required, optional, or unclassified.
declare -A seen_names
for name in "${literal_names[@]}"; do
    seen_names[$name]=1
done

for name in "${!seen_names[@]}"; do
    if [ -z "${NAME_TO_CODE[$name]:-}" ]; then
        echo "error: bootx64.c emits post_code16($name) but no matching #define found" >&2
        exit 1
    fi
    classified=0
    for req in "${POST16_REQUIRED_NAMES[@]}"; do
        if [ "$req" = "$name" ]; then classified=1; break; fi
    done
    if [ "$classified" -eq 0 ] && [ -n "${POST16_OPTIONAL_SET[$name]:-}" ]; then
        classified=1
    fi
    if [ "$classified" -eq 0 ]; then
        unclassified+=("$name")
    fi
done

if [ ${#unclassified[@]} -gt 0 ]; then
    echo "error: ${#unclassified[@]} POST16 emission(s) in $BOOTX64_C are unclassified:" >&2
    for name in "${unclassified[@]}"; do
        echo "       - $name (${NAME_TO_CODE[$name]})" >&2
    done
    echo "       Add to POST16_REQUIRED_NAMES (asserted on clean boot) or" >&2
    echo "       POST16_OPTIONAL_REASONS (scenario-dependent) in $0." >&2
    echo "       Every bootloader POST16 emission must be explicitly classified" >&2
    echo "       so new boot milestones cannot land without smoke-test coverage." >&2
    exit 1
fi

# Emit the manifest. Format:
#   # auto-generated, do not edit
#   POST16_ALL_COUNT=<n>
#   POST16_<NAME>=<0xHHHH>   one line per discovered define, sorted by value
#   POST16_REQUIRED=( NAME1 NAME2 ... )
#   POST16_REQUIRED_CODES=( 0xHHHH 0xHHHH ... )   convenience: codes only
{
    echo "# auto-generated by tools/post16-manifest/generate.sh"
    echo "# Sources: include/kernel/boot_init.h, src/boot/uefi/bootx64.c"
    echo "# Do not edit; run 'make post16-manifest' to regenerate."
    echo ""
    echo "POST16_ALL_COUNT=${#NAME_TO_CODE[@]}"
    echo ""
    # Sort names by the integer value of their code (hex -> decimal for sort)
    for name in "${!NAME_TO_CODE[@]}"; do
        code="${NAME_TO_CODE[$name]}"
        printf '%d %s=%s\n' "$((code))" "$name" "$code"
    done | sort -n | cut -d' ' -f2-
    echo ""
    echo "# Required set -- POST16 codes that MUST appear on serial during a"
    echo "# clean boot. Smoke test (scripts/test-smoke.sh) asserts each of"
    echo "# these as '[BOOT] POST 0xNNNN' on serial output."
    printf "POST16_REQUIRED=("
    for name in "${POST16_REQUIRED_NAMES[@]}"; do
        printf ' %s' "$name"
    done
    printf ' )\n'
    printf "POST16_REQUIRED_CODES=("
    for name in "${POST16_REQUIRED_NAMES[@]}"; do
        printf ' %s' "${NAME_TO_CODE[$name]}"
    done
    printf ' )\n'
    echo ""
    echo "# Optional set -- POST16 codes that ARE emitted by the bootloader"
    echo "# but are scenario-dependent (error paths, fallback, rare HW) and"
    echo "# therefore NOT asserted on a clean-boot smoke test. Reason follows"
    echo "# each name. Emission classification (required vs optional) is"
    echo "# exhaustive: every post_code16(POST16_BL_*) call in bootx64.c is"
    echo "# in exactly one of the two lists."
    printf "POST16_OPTIONAL=("
    for name in "${!POST16_OPTIONAL_SET[@]}"; do
        printf ' %s' "$name"
    done
    printf ' )\n'
    for entry in "${POST16_OPTIONAL_REASONS[@]}"; do
        echo "# $entry"
    done
} > "$OUT"

echo "[POST16] manifest: ${#NAME_TO_CODE[@]} defines, ${#POST16_REQUIRED_NAMES[@]} required -> $OUT"
