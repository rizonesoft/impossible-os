#!/usr/bin/env bash
# build.sh -- Build wrapper with progress bar, timing, and error extraction.
#
# Usage:
#   bash scripts/build.sh              Incremental build (only changed files)
#   bash scripts/build.sh clean        Full clean build (rm build/ + rebuild)
#   bash scripts/build.sh run          Incremental build + launch QEMU
#   bash scripts/build.sh run-usb      Build + launch QEMU with xHCI USB disk
#   bash scripts/build.sh clean run    Full clean build + launch QEMU
#   bash scripts/build.sh --jobs=4     Build with 4 parallel jobs (default: nproc)
#
# Output is tee'd to build/build.log.  The last line is always one of:
#   === BUILD OK ===
#   === BUILD FAILED ===
#
# Verify completion: tail -1 build/build.log

set -uo pipefail

# ── Config ──────────────────────────────────────────────────────────────────
LOG="build/build.log"
mkdir -p build

# ── Parse arguments ─────────────────────────────────────────────────────────
DO_CLEAN=false
DO_RUN=false
DO_RUN_USB=false
DO_RUN_USB_CI=false
DO_RUN_NVME=false
DO_RUN_NVME_CI=false
JOBS=$(nproc 2>/dev/null || echo 4)

print_help() {
    cat <<'EOF'
Impossible OS -- build wrapper

Usage:
  bash scripts/build.sh                 Incremental build (only changed files)
  bash scripts/build.sh clean           Full clean build (rm build/ + rebuild)
  bash scripts/build.sh run             Incremental build + launch QEMU
  bash scripts/build.sh clean run       Clean build + launch QEMU
  bash scripts/build.sh run-usb         Build + launch QEMU with xHCI USB disk
  bash scripts/build.sh run-usb-ci      Headless USB test (20s timeout)
  bash scripts/build.sh run-nvme        Build + launch QEMU with NVMe storage
  bash scripts/build.sh run-nvme-ci     Headless NVMe test (20s timeout)
  bash scripts/build.sh --jobs=N        Override parallel jobs (default: nproc)
  bash scripts/build.sh --help          Show this help

Environment variables (passed through to make):
  BUILD_ALT_BOOT={off,diagnostic,compatible}
                                        Alternate boot protocol policy.
                                        Developer default is `diagnostic`
                                        (parser compiled, never reached on
                                        UEFI boot). Release builds SHOULD
                                        set `off`; scripts/release/build-image.sh
                                        does NOT yet enforce this -- caller
                                        responsibility until the deprecation
                                        gate ships symbol-presence assertion.
                                        See docs/boot/alt-boot.md.
  KERNEL_EXTRA_CFLAGS=...               Append CFLAGS to kernel compile
                                        (e.g. fixture-mode ABI overrides).
                                        Cannot re-enable KERNEL_TESTS under
                                        KERNEL_TESTS=off: the flavor flag is
                                        appended last and wins.
  KERNEL_TESTS={on,off}                 Kernel test-surface flavor. Default
                                        `on`: the KERNEL_TESTS seams and the
                                        src/kernel/test/ suite are compiled in
                                        (scripts/test.sh needs this). `off` is
                                        the RELEASE flavor: seams compile out
                                        and the test sources are pruned from
                                        the build. Flipping the flavor rebuilds
                                        every TU (build/.kernel-tests.stamp).
                                        See todo/02-kernel-core/TODO-10 sec 26.

Output:
  Tee'd to build/build.log. Last line is always one of:
    === BUILD OK ===
    === BUILD FAILED ===

  Primary artifacts:
    build/system-disk.img   canonical GPT disk (EFI + BlackBox + IXFS)
    build/kernel.exe        kernel ELF
    build/kernel.map        symbol map (nm -n output)
    build/kernel.sym        KSYM binary symbol table (for BSOD resolver)
    compile_commands.json   clangd database (clean builds only)

Exit codes:
  0 = build OK
  1 = build failed (errors extracted to tail of build.log)
EOF
}

if [[ $# -eq 0 ]]; then
    : # default: incremental build only
else
    for arg in "$@"; do
        case "$arg" in
            -h|--help) print_help; exit 0 ;;
            clean) DO_CLEAN=true ;;
            run)     DO_RUN=true ;;
            run-usb) DO_RUN_USB=true ;;
            run-usb-ci) DO_RUN_USB_CI=true ;;
            run-nvme) DO_RUN_NVME=true ;;
            run-nvme-ci) DO_RUN_NVME_CI=true ;;
            --jobs=*) JOBS="${arg#--jobs=}" ;;
            *)
                echo "Unknown argument: $arg"
                echo ""
                print_help
                exit 1
                ;;
        esac
    done
fi

# Make flags for parallel compilation
MAKE_FLAGS="-j${JOBS}"

# Bear wraps make on clean builds to generate compile_commands.json for clangd
BEAR_PREFIX=""
if command -v bear &>/dev/null && [ "$DO_CLEAN" = true ]; then
    BEAR_PREFIX="bear --append --"
    rm -f compile_commands.json
    echo "[BEAR] Will generate compile_commands.json" | tee -a "$LOG"
fi

# ── Helpers ─────────────────────────────────────────────────────────────────
BOLD='\033[1m'
DIM='\033[2m'
GREEN='\033[32m'
RED='\033[31m'
YELLOW='\033[33m'
CYAN='\033[36m'
WHITE='\033[37m'
RESET='\033[0m'
CLEAR_LINE='\033[2K'

divider()  { printf '%b──────────────────────────────────────────────────%b\n' "$DIM" "$RESET"; }
header()   { printf '%b══════════════════════════════════════════════════%b\n' "$BOLD" "$RESET"; }

# Elapsed time since $1 (epoch seconds with nanoseconds)
elapsed() {
    local start=$1
    local now
    now=$(date +%s.%N)
    awk "BEGIN { t = $now - $start; if (t < 0) t = 0; printf \"%.1f\", t }"
}

# Count source files that make will compile (for progress bar)
count_sources() {
    find src/ -name '*.c' -o -name '*.asm' 2>/dev/null | wc -l
}

# ── Progress bar renderer ──────────────────────────────────────────────────
# Draws: ██████████░░░░░░░░░░  42/76  55%  [CC] gfx_core.c
# Only shown on terminal (stderr), not in log file.
draw_progress() {
    local current=$1 total=$2 filename=$3
    local bar_width=24

    if [[ $total -eq 0 ]]; then return; fi

    local pct=$(( current * 100 / total ))
    local filled=$(( current * bar_width / total ))
    local empty=$(( bar_width - filled ))

    # Build the bar
    local bar=""
    for ((i=0; i<filled; i++)); do bar+="█"; done
    for ((i=0; i<empty; i++));  do bar+="░"; done

    # Color transitions: cyan → green as we approach 100%
    local bar_color
    if   [[ $pct -ge 90 ]]; then bar_color="$GREEN"
    elif [[ $pct -ge 50 ]]; then bar_color="$CYAN"
    else                         bar_color="$WHITE"
    fi

    # Render on stderr (terminal only) with carriage return
    printf '\r%b%b %s %b%3d/%d  %3d%%%b  %s%b' \
        "$CLEAR_LINE" "$bar_color" "$bar" \
        "$BOLD" "$current" "$total" "$pct" "$RESET" \
        "$filename" "$RESET" >&2
}

clear_progress() {
    printf '\r%b' "$CLEAR_LINE" >&2
}

# ── Step runners ───────────────────────────────────────────────────────────
STEP_TIMES=()
STEP_NAMES=()

# Run a make target with progress banner (no progress bar)
run_step() {
    local num=$1 total=$2 label=$3
    shift 3
    local targets=("$@")
    local step_start
    step_start=$(date +%s.%N)

    divider | tee -a "$LOG"
    printf ' %b[%d/%d]%b %b%s%b\n' "$CYAN" "$num" "$total" "$RESET" "$BOLD" "$label" "$RESET" | tee -a "$LOG"
    divider | tee -a "$LOG"

    make $MAKE_FLAGS "${targets[@]}" 2>&1 | tee -a "$LOG"
    local rc=${PIPESTATUS[0]}

    local secs
    secs=$(elapsed "$step_start")

    if [[ $rc -ne 0 ]]; then
        printf ' %b✗ %s FAILED%b (%ss)\n' "$RED" "$label" "$RESET" "$secs" | tee -a "$LOG"
        return $rc
    fi

    printf ' %b✓ %s%b (%ss)\n' "$GREEN" "$label" "$RESET" "$secs" | tee -a "$LOG"
    STEP_TIMES+=("$secs")
    STEP_NAMES+=("$label")
    return 0
}

# Run kernel build with per-file progress bar
run_kernel_step() {
    local num=$1 total=$2
    local step_start
    step_start=$(date +%s.%N)

    local src_total
    src_total=$(count_sources)

    divider | tee -a "$LOG"
    printf ' %b[%d/%d]%b %bKernel%b  (%d source files)\n' \
        "$CYAN" "$num" "$total" "$RESET" "$BOLD" "$RESET" "$src_total" | tee -a "$LOG"
    divider | tee -a "$LOG"

    local compiled=0
    $BEAR_PREFIX make $MAKE_FLAGS _increment_build kernel 2>&1 | while IFS= read -r line; do
        # Log every line
        echo "$line" >> "$LOG"

        # Check for compilation markers
        case "$line" in
            "[CC]"*|"[AS]"*|"[CC/SSE2]"*)
                compiled=$((compiled + 1))
                # Extract just the filename from e.g. "[CC] src/kernel/gfx/gfx_core.c"
                local fname
                fname=$(echo "$line" | sed 's/^\[.*\] //' | xargs basename 2>/dev/null || echo "$line")
                draw_progress "$compiled" "$src_total" "$fname"
                ;;
            "[LD]"*|"[KERNEL]"*)
                # Linker/kernel steps -- show at 100% without incrementing count
                draw_progress "$src_total" "$src_total" "Linking..."
                ;;
            *)
                # Non-compilation lines: print normally
                echo "$line"
                ;;
        esac
    done
    local rc=${PIPESTATUS[0]}

    clear_progress

    local secs
    secs=$(elapsed "$step_start")

    if [[ $rc -ne 0 ]]; then
        printf ' %b✗ Kernel FAILED%b (%ss)\n' "$RED" "$RESET" "$secs" | tee -a "$LOG"
        return $rc
    fi

    printf ' %b✓ Kernel%b (%ss)\n' "$GREEN" "$RESET" "$secs" | tee -a "$LOG"
    STEP_TIMES+=("$secs")
    STEP_NAMES+=("Kernel")
    return 0
}

# ── Error and summary display ──────────────────────────────────────────────

print_errors() {
    header | tee -a "$LOG"
    printf ' %b BUILD FAILED%b\n' "${RED}${BOLD}" "$RESET" | tee -a "$LOG"
    header | tee -a "$LOG"

    local errors
    errors=$(grep -iE 'error:|undefined reference|fatal error' "$LOG" | grep -v '=== BUILD' | head -15)
    if [[ -n "$errors" ]]; then
        printf ' %bErrors:%b\n' "$YELLOW" "$RESET" | tee -a "$LOG"
        echo "$errors" | sed 's/^/   /' | tee -a "$LOG"
        divider | tee -a "$LOG"
    fi
}

print_summary() {
    local total_secs=$1
    header | tee -a "$LOG"
    printf ' %b BUILD OK%b (%ss total)\n' "${GREEN}${BOLD}" "$RESET" "$total_secs" | tee -a "$LOG"
    header | tee -a "$LOG"

    for i in "${!STEP_NAMES[@]}"; do
        printf ' %-16s %6ss\n' "${STEP_NAMES[$i]}" "${STEP_TIMES[$i]}" | tee -a "$LOG"
    done
    divider | tee -a "$LOG"
}

# ── Main ────────────────────────────────────────────────────────────────────
BUILD_START=$(date +%s.%N)

# Header
echo "" > "$LOG"
printf '\n%b Impossible OS -- Build System%b\n' "${BOLD}${CYAN}" "$RESET" | tee -a "$LOG"
printf ' %b%s  (%d parallel jobs)%b\n\n' "$DIM" "$(date '+%Y-%m-%d %H:%M:%S')" "$JOBS" "$RESET" | tee -a "$LOG"

# Stale-ABI fixture sentinel: build-stale-kernel.sh writes
# build/.stale-abi-dirty before invoking `make kernel
# KERNEL_EXTRA_CFLAGS=-DBOOT_INFO_VERSION=N` and removes it after
# the cleanup() trap restores the real kernel. If the sentinel
# survives (script killed by SIGKILL, host shutdown, second signal
# during cleanup), the build tree may carry .o files compiled with
# a stale BOOT_INFO_VERSION that an incremental build would treat
# as up-to-date. Force a clean rebuild rather than ship a silently
# stale kernel.
if [ -f build/.stale-abi-dirty ] && ! $DO_CLEAN; then
    echo "[build] stale-ABI sentinel present (build/.stale-abi-dirty);" >&2
    echo "[build]   the build tree may contain objects compiled with" >&2
    echo "[build]   BOOT_INFO_VERSION-1 from an interrupted fixture run." >&2
    echo "[build]   Forcing a clean rebuild to ensure a consistent kernel." >&2
    DO_CLEAN=true
fi

# Step counter
STEP=0
if $DO_CLEAN; then
    TOTAL=8  # clean + kernel + userland + efi + sign + abi + post16 + disk
else
    TOTAL=7  # kernel + userland + efi + sign + abi + post16 + disk
fi

# Clean (optional)
if $DO_CLEAN; then
    STEP=$((STEP + 1))
    run_step $STEP $TOTAL "Clean" "clean" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }
    # make clean removes build/ -- re-create it for the log file
    mkdir -p build
    echo "" > "$LOG"
fi

# Generated headers: extract MS UEFI CA generation from the pinned
# shim binary so uefi_runtime.c can publish HKLM\SYSTEM\SecureBoot\ShimCA
# at boot. Idempotent; runs every build (cheap; ~10 ms).
bash scripts/extract-shim-ca.sh >> "$LOG" 2>&1 || { \
    echo "[shim-ca] FATAL: extract-shim-ca.sh failed; refusing to ship stale generated header" >> "$LOG"; \
    print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# ── Generated-ABI drift gate ────────────────────────────────────────────────
# `make all` USED TO list check-abi as a prerequisite, but THIS script -- the
# canonical build path per CLAUDE.md, which forbids raw `make` -- drove `kernel`
# and `userland` directly and never ran it. The drift guard was therefore off on
# the only path anyone actually uses: editing a constant in one of the generator's
# source headers compiled the NEW value into the kernel while the generated ABI
# artifact kept the OLD one, and the crt0 SYS_ABI_HANDSHAKE passed because the
# generated side agreed with itself -- ring 3 compared against a stale number
# with nothing complaining. Runs BEFORE compilation so the failure lands on the
# edit, not on a later boot.
#
# --check also verifies the two STATIC shims (user/include/abi_numbers.h,
# include/kernel/abi_hash.h) still route to abi/generated/abi_contract.h with
# the right visibility and hand-define no ABI constant of their own. The shims
# are ordinary committed source, so nothing else would catch a one-line edit
# that detaches a side of the build from the generator.
#
# The Makefile ALSO gates every artifact target through its ABI validation stamp
# (build/.abi-check.stamp), so this call is no longer the only net. It is kept
# because it is the FAIL-FAST one: it reports "ABI DRIFT" with the regeneration
# command before any build step starts, where make would instead report a failed
# stamp target.
if ! python3 scripts/gen-user-abi.py --check >> "$LOG" 2>&1; then
    printf '\n %b✗ ABI DRIFT:%b generated ABI contract is stale vs kernel source\n' \
        "$RED" "$RESET" | tee -a "$LOG"
    printf '   Regenerate with: python3 scripts/gen-user-abi.py\n' | tee -a "$LOG"
    print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1
fi

# Kernel (with progress bar)
STEP=$((STEP + 1))
run_kernel_step $STEP $TOTAL || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# ── BSS / user-mode address collision check ─────────────────────────────────
# Kernel BSS must not overlap the user-mode ELF base address (user/user.ld).
# If BSS grows past USER_BASE, user programs overwrite kernel data at runtime.
# Derived from user/user.ld rather than hardcoded: this check previously
# carried its own copy of 0x800000, a fourth definition that user_range.h's
# dependency list did not even mention, so moving the user base left the gate
# testing the OLD address and failing a build that was actually correct.
USER_BASE=$(grep -oE '^\s*\.\s*=\s*0x[0-9A-Fa-f]+' user/user.ld | grep -oE '0x[0-9A-Fa-f]+' | head -1)
if [[ -z "$USER_BASE" ]]; then
    printf '\n %b! BSS check skipped:%b could not parse the load address from user/user.ld\n' \
        "$YELLOW" "$RESET" | tee -a "$LOG"
fi
if [[ -f build/kernel.map && -n "$USER_BASE" ]]; then
    BSS_END_HEX=$(grep ' [bB] ' build/kernel.map | awk '{print $1}' | sort | tail -1)
    if [[ -n "$BSS_END_HEX" ]]; then
        BSS_END=$((16#${BSS_END_HEX}))
        if [[ $BSS_END -ge $USER_BASE ]]; then
            printf '\n %b✗ BSS COLLISION:%b Kernel BSS end (0x%s) >= user base (0x%X)\n' \
                "$RED" "$RESET" "$BSS_END_HEX" "$USER_BASE" | tee -a "$LOG"
            printf '   Increase USER_BASE in user/user.ld or reduce kernel static allocations.\n' | tee -a "$LOG"
            echo "=== BUILD FAILED ===" >> "$LOG"
            exit 1
        fi
        printf ' %b✓ BSS check:%b kernel BSS end 0x%s < user base 0x%X\n' \
            "$GREEN" "$RESET" "$BSS_END_HEX" "$USER_BASE" | tee -a "$LOG"

        # Firmware-reserved floor. Separate from, and stricter than, the user
        # base above: the bootloader refuses any PT_LOAD whose destination is
        # not EfiConventionalMemory, and on this platform ACPIMemoryNVS begins
        # at 0x800000 (measured 2026-08-17 from the loader's own rejection,
        # "type=ACPIMemoryNVS at=0x00800000"). Without this the overflow is
        # invisible until boot, where it shows up as "Kernel ELF could not be
        # loaded" with no size in the message.
        FW_FLOOR=0x800000
        if [[ $BSS_END -ge $FW_FLOOR ]]; then
            printf '\n %b✗ FIRMWARE OVERLAP:%b kernel image ends 0x%s, past the reserved floor 0x%X\n' \
                "$RED" "$RESET" "$BSS_END_HEX" "$FW_FLOOR" | tee -a "$LOG"
            printf '   The bootloader will refuse this image (PT_LOAD into ACPIMemoryNVS).\n' | tee -a "$LOG"
            printf '   Over by %d KiB. Reduce kernel .text/.bss or relocate the kernel.\n' \
                "$(( (BSS_END - FW_FLOOR) / 1024 ))" | tee -a "$LOG"
            echo "=== BUILD FAILED ===" >> "$LOG"
            exit 1
        fi
        printf ' %b✓ Firmware floor:%b kernel end 0x%s < 0x%X (%d KiB free)\n' \
            "$GREEN" "$RESET" "$BSS_END_HEX" "$FW_FLOOR" "$(( (FW_FLOOR - BSS_END) / 1024 ))" | tee -a "$LOG"
    fi
fi

# Userland
STEP=$((STEP + 1))
run_step $STEP $TOTAL "Userland" "userland" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# EFI
STEP=$((STEP + 1))
run_step $STEP $TOTAL "EFI Boot" "uefi-boot" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# Pack BOOTX64.UKI.efi -- Unified Kernel Image per UAPI Group spec
# (https://uapi-group.org/specifications/specs/unified_kernel_image/).
# Bundles the BOOTX64.EFI stub + kernel.exe (.linux section) + boot.conf
# (.cmdline section) + os-release info (.osrel section) into a single
# signable PE. UEFI firmware can invoke the UKI directly via direct-
# firmware-invoke, getting whole-chain Secure Boot signature coverage
# (kernel + cmdline are inside the firmware-verified PE blob, so per-
# file split-path signing is no longer required).
#
# Section virtual addresses are spec-defined (0x20000 .osrel,
# 0x30000 .cmdline, 0x2000000 .linux) to avoid overlap with the stub
# PE's existing sections. The bootloader's detect_uki_sections() walks
# the PE section table at boot time looking for these.
{
    set -e
    UKI_OUT="build/tools/BOOTX64.UKI.efi"
    UKI_STUB="build/tools/BOOTX64.EFI"
    UKI_KERNEL="build/kernel.exe"
    UKI_CMDLINE="build/uki-cmdline.txt"
    UKI_OSREL="build/uki-osrel.txt"
    if [ -f "$UKI_STUB" ] && [ -f "$UKI_KERNEL" ]; then
        # Source boot.conf into .cmdline so the bootloader's UKI path
        # parses the same shape it would have read from disk under the
        # split path.
        if [ -f "resources/boot/boot.conf" ]; then
            cp "resources/boot/boot.conf" "$UKI_CMDLINE"
        else
            : > "$UKI_CMDLINE"
        fi
        # Minimal os-release info; matches the UAPI Group spec layout.
        cat > "$UKI_OSREL" <<'OSEOF'
NAME="Impossible OS"
ID=impossible-os
VERSION_ID="0.1"
PRETTY_NAME="Impossible OS (UKI)"
OSEOF
        # llvm-objcopy preserves the PE/COFF format and supports
        # --add-section for section injection. Spec virtual addresses
        # via --change-section-vma; raw size auto-aligned by objcopy.
        OBJCOPY=""
        for cand in llvm-objcopy-19 llvm-objcopy objcopy; do
            if command -v "$cand" >/dev/null 2>&1; then
                OBJCOPY="$cand"
                break
            fi
        done
        if [ -z "$OBJCOPY" ]; then
            echo "[uki] WARN: no objcopy found (tried llvm-objcopy-19, llvm-objcopy, objcopy); skipping UKI pack" >> "$LOG"
        else
            # Section virtual addresses are auto-placed by objcopy
            # immediately past the stub's existing sections. The
            # bootloader's detect_uki_sections() walks the table by
            # name (VirtualAddress + VirtualSize from each entry), so
            # specific VAs don't need to match the UAPI Group spec
            # constants. llvm-objcopy lacks a --change-section-vma flag
            # for PE; section placement is left to the tool's default.
            # Section ordering is pinned by argument order
            # (.osrel -> .cmdline -> .linux -> .initrd -> .recovery ->
            # .modules per docs/guides/secure-boot-keys.md UKI
            # subsection -- load-bearing for PCR measurement
            # reproducibility in the measured-boot path). Optional
            # payloads at build/uki-payloads/{initrd.img,recovery.img,
            # modules.cpio} are appended only when present so existing
            # builds without signed payloads ship unchanged (whole-chain
            # signed-payload feature back-compat contract).
            UKI_PAYLOAD_DIR="build/uki-payloads"
            UKI_INITRD="$UKI_PAYLOAD_DIR/initrd.img"
            UKI_RECOVERY="$UKI_PAYLOAD_DIR/recovery.img"
            UKI_MODULES="$UKI_PAYLOAD_DIR/modules.cpio"
            OBJCOPY_OPTIONAL_ARGS=""
            if [ -f "$UKI_INITRD" ]; then
                OBJCOPY_OPTIONAL_ARGS="$OBJCOPY_OPTIONAL_ARGS --add-section .initrd=$UKI_INITRD --set-section-flags .initrd=alloc,readonly,data"
                echo "[uki] embedding .initrd from $UKI_INITRD ($(wc -c < "$UKI_INITRD" | tr -d ' ') bytes)" >> "$LOG"
            fi
            if [ -f "$UKI_RECOVERY" ]; then
                OBJCOPY_OPTIONAL_ARGS="$OBJCOPY_OPTIONAL_ARGS --add-section .recovery=$UKI_RECOVERY --set-section-flags .recovery=alloc,readonly,data"
                echo "[uki] embedding .recovery from $UKI_RECOVERY ($(wc -c < "$UKI_RECOVERY" | tr -d ' ') bytes)" >> "$LOG"
            fi
            if [ -f "$UKI_MODULES" ]; then
                OBJCOPY_OPTIONAL_ARGS="$OBJCOPY_OPTIONAL_ARGS --add-section .modules=$UKI_MODULES --set-section-flags .modules=alloc,readonly,data"
                echo "[uki] embedding .modules from $UKI_MODULES ($(wc -c < "$UKI_MODULES" | tr -d ' ') bytes)" >> "$LOG"
            fi
            "$OBJCOPY" \
                --add-section .osrel="$UKI_OSREL" \
                --set-section-flags .osrel=alloc,readonly,data \
                --add-section .cmdline="$UKI_CMDLINE" \
                --set-section-flags .cmdline=alloc,readonly,data \
                --add-section .linux="$UKI_KERNEL" \
                --set-section-flags .linux=alloc,readonly,data \
                $OBJCOPY_OPTIONAL_ARGS \
                "$UKI_STUB" "$UKI_OUT" 2>>"$LOG" \
                && echo "[uki] generated $UKI_OUT ($(wc -c < "$UKI_OUT" | tr -d ' ') bytes)" >> "$LOG" \
                || { echo "[uki] FATAL: $OBJCOPY --add-section failed" >> "$LOG"; print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }
        fi
    else
        echo "[uki] skip: missing stub or kernel ($UKI_STUB / $UKI_KERNEL)" >> "$LOG"
    fi
} || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# EFI Signing (skipped silently if keys/MOK.key is absent)
STEP=$((STEP + 1))
run_step $STEP $TOTAL "EFI Signing" "sign-efi" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# SBAT revocation-metadata gate: the boot artifacts must stay SBAT-revocable
# (a vulnerable build must be revocable by SBAT generation, not just dbx).
# Stage 1 validates the source CSV structurally; stage 2
# verifies each built artifact carries a .sbat section whose bytes equal the
# source. A name-only presence check would pass an empty/stale/wrong section,
# so this compares exact content. Runs after signing -- sbsign appends a
# signature and does not touch the .sbat section.
{
    SBAT_SRC="src/boot/uefi/sbat.csv"
    SBAT_HDR="sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md"
    sbat_fail() { echo "[sbat] FATAL: $1" >> "$LOG"; print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }
    [ -f "$SBAT_SRC" ] || sbat_fail "$SBAT_SRC missing"
    # Stage 1: structural + semantic validation of the source CSV.
    [ "$(sed -n '1p' "$SBAT_SRC")" = "$SBAT_HDR" ] || sbat_fail "sbat.csv line 1 is not the shim SBAT header"
    # Whole-file shape: exactly the header + impossibleos rows (2 non-empty
    # lines), and EVERY non-empty line has 6 comma fields. Without this a
    # third malformed row would slip past the impossibleos-only check and
    # then pass Stage 2 (artifacts byte-match the same bad source).
    sbat_nonempty=$(grep -c . "$SBAT_SRC")
    [ "$sbat_nonempty" -eq 2 ] || sbat_fail "sbat.csv must have exactly 2 non-empty rows (header + impossibleos; found $sbat_nonempty)"
    sbat_badfields=$(awk -F, 'NF>0 && NF!=6 {c++} END{print c+0}' "$SBAT_SRC")
    [ "$sbat_badfields" -eq 0 ] || sbat_fail "sbat.csv has $sbat_badfields row(s) without exactly 6 fields"
    sbat_rows=$(grep -c '^impossibleos,' "$SBAT_SRC")
    [ "$sbat_rows" -eq 1 ] || sbat_fail "sbat.csv needs exactly one impossibleos row (found $sbat_rows)"
    sbat_line=$(grep '^impossibleos,' "$SBAT_SRC")
    sbat_fields=$(printf '%s' "$sbat_line" | awk -F, '{print NF}')
    [ "$sbat_fields" -eq 6 ] || sbat_fail "impossibleos row needs 6 fields (found $sbat_fields)"
    sbat_gen=$(printf '%s' "$sbat_line" | cut -d, -f2)
    case "$sbat_gen" in ''|*[!0-9]*) sbat_fail "impossibleos generation '$sbat_gen' is not a positive integer";; esac
    [ "$sbat_gen" -ge 1 ] || sbat_fail "impossibleos generation must be >= 1 (got $sbat_gen)"
    # Stage 2: per-artifact byte-compare of the embedded .sbat against the source.
    for sbat_art in build/tools/BOOTX64.EFI build/tools/BOOTX64.UKI.efi; do
        [ -f "$sbat_art" ] || continue   # UKI is skipped when stub/kernel are absent
        sbat_tmp=$(mktemp)
        objcopy -O binary --only-section=.sbat "$sbat_art" "$sbat_tmp" 2>>"$LOG" || { rm -f "$sbat_tmp"; sbat_fail "cannot dump .sbat from $sbat_art"; }
        cmp -s "$sbat_tmp" "$SBAT_SRC" || { rm -f "$sbat_tmp"; sbat_fail "$sbat_art .sbat does not match $SBAT_SRC (missing/empty/stale)"; }
        rm -f "$sbat_tmp"
    done
    echo "[sbat] OK: BOOTX64.EFI + UKI carry .sbat == $SBAT_SRC (impossibleos gen $sbat_gen)" >> "$LOG"
} || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# boot_info ABI manifest: compile kernel-view + mirror-view dumpers, emit
# JSON for both, diff them. Fails the build on any field/offset/size drift.
# Static asserts in the headers remain the first line of defense; this
# catches same-size reorders the asserts miss.
STEP=$((STEP + 1))
run_step $STEP $TOTAL "boot_info ABI" "boot-info-abi" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# POST16 manifest: extract POST16_* #defines from include/kernel/boot_init.h
# and src/boot/uefi/bootx64.c; emit build/post16-manifest.env for smoke-test
# consumption. Fails the build on name/value collisions or required-set
# drift (someone removed a define listed in POST16_REQUIRED_NAMES).
STEP=$((STEP + 1))
run_step $STEP $TOTAL "POST16 manifest" "post16-manifest" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# System Disk
STEP=$((STEP + 1))
run_step $STEP $TOTAL "System Disk" "system-disk" || { print_errors; echo "=== BUILD FAILED ===" >> "$LOG"; exit 1; }

# Summary
TOTAL_SECS=$(elapsed "$BUILD_START")
print_summary "$TOTAL_SECS"
echo "=== BUILD OK ===" >> "$LOG"

# Content-addressed build receipt: binds this green build to the exact build
# inputs + toolchain so evidence gates validate on content, not wall-clock age.
python3 scripts/overnight/receipts.py record-build . >/dev/null 2>&1 || true

# Update test coverage report (static source scan, no QEMU needed)
bash scripts/test-coverage.sh --save --quiet 2>/dev/null || true

# Run-mode banners print to terminal only -- do NOT tee to $LOG, otherwise
# `tail -1 build/build.log` would no longer be `=== BUILD OK ===` and CI
# sentinel checks break. The build sentinel written above is the contract.
# Run-mode failures DO propagate to the script's exit code so CI / hooks can
# detect QEMU-launch failures separately from build failures. The build sentinel
# remains authoritative for the build portion.
RUN_STATUS=0
run_make() {
    local target=$1
    if ! make $MAKE_FLAGS "$target" 2>&1; then
        RUN_STATUS=1
        printf ' %b✗ make %s failed%b\n' "$RED" "$target" "$RESET" >&2
    fi
}

if $DO_RUN; then
    divider
    printf ' %b▶ Launching QEMU%b\n' "${CYAN}${BOLD}" "$RESET"
    divider
    run_make run
fi

if $DO_RUN_USB; then
    divider
    printf ' %b▶ Launching QEMU with xHCI + USB storage%b\n' "${CYAN}${BOLD}" "$RESET"
    divider
    run_make run-usb
fi

if $DO_RUN_USB_CI; then
    divider
    printf ' %b▶ Launching headless QEMU (xHCI + USB, 20s timeout)%b\n' "${CYAN}${BOLD}" "$RESET"
    divider
    run_make run-usb-ci
fi

if $DO_RUN_NVME; then
    divider
    printf ' %b▶ Launching QEMU with NVMe storage%b\n' "${CYAN}${BOLD}" "$RESET"
    divider
    run_make run-nvme
fi

if $DO_RUN_NVME_CI; then
    divider
    printf ' %b▶ Launching headless QEMU (NVMe, 20s timeout)%b\n' "${CYAN}${BOLD}" "$RESET"
    divider
    run_make run-nvme-ci
fi

exit "$RUN_STATUS"
