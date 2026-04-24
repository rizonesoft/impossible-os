#!/usr/bin/env bash
# ============================================================================
# run-fixtures.sh -- drive the stale-ABI QEMU fixture harness.
#
# For each fixture (stale-bootloader, stale-kernel):
#   1. Env-probe: KVM + OVMF + mtools. If any missing, print [SKIP] and
#      move on without failing the script.
#   2. Build the stale binary via build-stale-{bootloader,kernel}.sh.
#   3. Swap the stale binary into a disposable copy of system-disk.img
#      via assemble-esp.sh.
#   4. Boot the stale disk headless under QEMU, capture serial to a
#      per-fixture log file.
#   5. Grep the stripped serial log for `boot_version: protocol mismatch`
#      (the kernel fatal banner) plus a fault-class line. Accept either
#      BAD_VERSION or BAD_SIZE (version bumps typically co-change
#      struct_size, so the classifier may pick either).
#   6. [PASS] on match; [FAIL] with the last 40 lines of serial on
#      mismatch.
#
# Exit 0 when every fixture PASSed or SKIPped. Exit 1 on any FAIL so
# the enclosing `make stale-abi-fixtures` target (and CI step that
# wraps it) can gate on an actual regression rather than environment
# noise.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
FIXTURES_DIR="$REPO_ROOT/build/fixtures"
BUILD_DIR="$REPO_ROOT/build"

OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
# KVM boots in ~2s; TCG in ~10-15s. The per-fixture deadline accounts
# for the stale build + disk assemble + boot + fatal-fire; TCG tail is
# longer so bump the timeout when falling back.
TIMEOUT_KVM="${STALE_ABI_TIMEOUT_KVM:-45}"
TIMEOUT_TCG="${STALE_ABI_TIMEOUT_TCG:-120}"

# Auto-detect accelerator. KVM preferred; TCG fallback so CI runners
# without nested virt still exercise the fail-closed path instead of
# SKIPing the whole harness.
USE_KVM=0
if [ -w /dev/kvm ]; then
    USE_KVM=1
fi

# --- Colors (no-op when stdout is not a TTY) ------------------------
if [ -t 1 ]; then
    RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[1;33m'
    CYAN=$'\033[0;36m'; DIM=$'\033[0;90m'; NC=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; DIM=''; NC=''
fi

say_pass() { printf "%s[PASS]%s %s\n" "$GREEN" "$NC" "$1"; }
say_fail() { printf "%s[FAIL]%s %s\n" "$RED"   "$NC" "$1"; }
say_skip() { printf "%s[SKIP]%s %s\n" "$YELLOW" "$NC" "$1"; }
say_info() { printf "%s%s%s\n"        "$DIM"   "$1"  "$NC"; }

# --- Env probe: enforce fail-SKIP semantics -------------------------
# Missing env is a developer-machine gap, not a bug. CI needs this to
# turn CI flakes (no KVM in certain runners, OVMF missing) into SKIP.
probe_env() {
    local missing=()
    # KVM is preferred but not required -- TCG fallback below. Skip the
    # KVM probe here on purpose so a KVM-less host still runs the
    # harness under TCG (slower but catches the same fail-closed path).
    [ -f "$OVMF_CODE" ] || missing+=("OVMF not at $OVMF_CODE")
    [ -f "$OVMF_VARS_SRC" ] || missing+=("OVMF_VARS not at $OVMF_VARS_SRC")
    command -v mcopy >/dev/null || missing+=("mtools (mcopy) not installed")
    command -v qemu-system-x86_64 >/dev/null || \
        missing+=("qemu-system-x86_64 not installed")
    if [ ${#missing[@]} -gt 0 ]; then
        for m in "${missing[@]}"; do
            say_info "  env gap: $m"
        done
        return 1
    fi
    if [ "$USE_KVM" -eq 1 ]; then
        say_info "  accelerator: KVM (enabled)"
    else
        say_info "  accelerator: TCG (KVM unavailable; using software emulation)"
    fi
    return 0
}

# --- Boot a fixture disk headless, capture serial ------------------
# $1 = path to fixture disk image
# $2 = serial log path
# Returns 0 on QEMU exit (any cause); the caller greps the log.
boot_fixture() {
    local disk="$1"
    local serial="$2"

    local ovmf_vars="$FIXTURES_DIR/$(basename "$disk" .img).OVMF_VARS.fd"
    cp "$OVMF_VARS_SRC" "$ovmf_vars"

    local args=(
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
        -drive "if=pflash,format=raw,file=$ovmf_vars"
        -drive "id=disk0,file=$disk,format=raw,if=none"
        -device "ich9-ahci,id=ahci0"
        -device "ide-hd,drive=disk0,bus=ahci0.0"
        -m 2G
        -serial "file:$serial"
        -no-reboot
        -no-shutdown
        -display none
    )
    local timeout
    if [ "$USE_KVM" -eq 1 ]; then
        args+=(-enable-kvm -cpu host)
        timeout="$TIMEOUT_KVM"
    else
        # TCG fallback -- slower but same fail-closed path. qemu64 is
        # a portable x86_64 baseline that avoids KVM's real-host CPU
        # passthrough. `-machine q35` keeps the PCIe/SATA topology
        # identical to the KVM path so BOOTX64 / kernel see the same
        # devices.
        args+=(-machine q35 -cpu qemu64)
        timeout="$TIMEOUT_TCG"
    fi

    : > "$serial"

    qemu-system-x86_64 "${args[@]}" &
    local qpid=$!

    local deadline=$(( $(date +%s) + timeout ))
    # The stale-ABI path is expected to halt (bootloader fatal on the
    # stale-BOOTX64 fixture; kernel fatal + boot_halt on the stale-
    # kernel fixture). Wait for the marker OR timeout.
    while kill -0 "$qpid" 2>/dev/null; do
        if [ "$(date +%s)" -ge "$deadline" ]; then
            kill "$qpid" 2>/dev/null || true
            wait "$qpid" 2>/dev/null || true
            break
        fi
        # If we see either fatal signature, stop waiting early.
        if grep -qiE "boot_version:|ANTI-ROLLBACK|ABI MISMATCH|\[BOOT HALT\]|KERNEL PANIC|ImpossibleBootProtoFault" \
               "$serial" 2>/dev/null; then
            # Give QEMU a short tail to flush remaining serial then kill.
            sleep 1
            kill "$qpid" 2>/dev/null || true
            wait "$qpid" 2>/dev/null || true
            break
        fi
        sleep 0.5
    done

    return 0
}

# --- Assert the serial log carries the expected fault pattern -------
# $1 = fixture label
# $2 = serial log path
# $3 = named fault-class regex, e.g. "BAD_VERSION|BAD_SIZE"
# $4 = numeric fault-class digits for the bootloader decimal form,
#      e.g. "2|3|4" -- MUST correspond to $3. Per-fixture tight
#      matching prevents a regression that swaps the wrong fault
#      class from false-passing.
#
# enum boot_version_fault_class (include/kernel/boot_version.h):
#   0=OK  1=NULL_HDR  2=BAD_MAGIC  3=BAD_VERSION  4=BAD_SIZE
#   5=SEC_ROLLBACK  6=BAD_SHA  7=BAD_PARSE
#
# Returns 0 on match (emits [PASS]); non-zero on no-match (emits
# [FAIL] + last 40 lines of serial to stderr).
assert_fault_pattern() {
    local label="$1"
    local serial="$2"
    local classes="$3"
    local numeric_digits="$4"

    local stripped="${serial%.log}.stripped.log"
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$serial" \
        > "$stripped" 2>/dev/null || cp "$serial" "$stripped"

    # Match both the overall fatal banner AND a specific fault class.
    # The bootloader pre-jump screen uses "ABI MISMATCH" / "SEC_ROLLBACK"
    # / "Boot Protocol Mismatch"; the kernel fatal uses
    # "boot_version: protocol mismatch" + named class.
    # Use -iE (case-insensitive) because the bootloader serial path
    # emits uppercase and the kernel path emits lowercase.
    local saw_banner=0
    local saw_class=0
    if grep -qiE "boot_version: protocol mismatch|boot_version_classify|ABI MISMATCH|Boot Protocol Mismatch|BootVersionFault|ImpossibleBootProtoFault|ANTI-ROLLBACK|Anti-Rollback Refusal" \
           "$stripped"; then
        saw_banner=1
    fi
    # Class match: accept the named class (kernel fatal path prints
    # BAD_VERSION / BAD_SHA / BAD_PARSE / ...) OR the bootloader's
    # decimal form (`fault_class: N`). Numeric digits are now per-
    # fixture; a regression that emits fault_class: 2 (BAD_MAGIC) on
    # a test that should fire BAD_VERSION no longer false-passes.
    local numeric_regex="fault_class: ($numeric_digits)"
    if grep -qE "$classes" "$stripped" || \
       grep -qE "$numeric_regex" "$stripped"; then
        saw_class=1
    fi

    if [ "$saw_banner" -eq 1 ] && [ "$saw_class" -eq 1 ]; then
        say_pass "$label (fault banner + class $classes)"
        return 0
    fi

    say_fail "$label"
    if [ "$saw_banner" -eq 0 ]; then
        say_info "  MISSING: boot_version fatal banner"
    fi
    if [ "$saw_class" -eq 0 ]; then
        say_info "  MISSING: fault-class match for $classes"
    fi
    say_info "  --- last 40 lines of $stripped ---"
    tail -n 40 "$stripped" >&2 || true
    say_info "  --- end serial tail ---"
    return 1
}

# --- Main fixture runners -------------------------------------------
FAILED=0

run_stale_bootloader() {
    local label="stale-bootloader"
    EXECUTED=$((EXECUTED + 1))
    echo
    printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"

    local stale_bin
    stale_bin="$(bash "$SCRIPT_DIR/build-stale-bootloader.sh")"
    if [ ! -f "$stale_bin" ]; then
        say_fail "$label: stale build did not produce artifact"
        FAILED=$((FAILED + 1))
        return
    fi

    local stale_disk
    stale_disk="$(bash "$SCRIPT_DIR/assemble-esp.sh" \
        --stale-bootloader "$stale_bin")"

    local serial="$FIXTURES_DIR/$label.serial.log"
    boot_fixture "$stale_disk" "$serial"

    # Stale bootloader writes an older BOOT_INFO_VERSION into the
    # boot_info header. The kernel's boot_version_classify then sees
    # BAD_VERSION (3) or BAD_SIZE (4). A decrement cannot produce
    # BAD_MAGIC (2) unless the magic override is used, so exclude 2
    # here -- tightening prevents a regression in which the fault
    # path accidentally reports the wrong class but still passes the
    # harness.
    if ! assert_fault_pattern "$label" "$serial" \
        "BAD_VERSION|BAD_SIZE" "3|4"; then
        FAILED=$((FAILED + 1))
    fi
}

run_stale_kernel() {
    local label="stale-kernel"
    EXECUTED=$((EXECUTED + 1))
    echo
    printf "%s=== %s ===%s\n" "$CYAN" "$label" "$NC"

    local stale_bin
    stale_bin="$(bash "$SCRIPT_DIR/build-stale-kernel.sh")"
    if [ ! -f "$stale_bin" ]; then
        say_fail "$label: stale build did not produce artifact"
        FAILED=$((FAILED + 1))
        return
    fi

    local stale_disk
    stale_disk="$(bash "$SCRIPT_DIR/assemble-esp.sh" \
        --stale-kernel "$stale_bin")"

    local serial="$FIXTURES_DIR/$label.serial.log"
    boot_fixture "$stale_disk" "$serial"

    # Stale kernel: bootloader writes current BOOT_INFO_VERSION, the
    # kernel's compile-time expected value is one older. This
    # triggers EITHER (a) the bootloader pre-jump `.bootproto`
    # mismatch (SHA + version differ; fault_class 3 BAD_VERSION or 6
    # BAD_SHA), OR (b) the kernel-side boot_version_classify path
    # (fault_class 3 BAD_VERSION / 4 BAD_SIZE). BAD_PARSE (7) is
    # possible if the ELF parse fails; keep as accepted. BAD_MAGIC
    # (2) is NOT reachable from a version decrement and is excluded
    # to prevent a regression that swaps the wrong class from false-
    # passing. "ABI MISMATCH" text banner alone (without a decimal
    # class) satisfies the banner check, not the class check.
    if ! assert_fault_pattern "$label" "$serial" \
        "BAD_VERSION|BAD_SIZE|BAD_SHA|BAD_PARSE" "3|4|6|7"; then
        FAILED=$((FAILED + 1))
    fi
}

# --- Orchestrate ----------------------------------------------------
mkdir -p "$FIXTURES_DIR"

echo "========================================"
echo "  Stale-ABI QEMU Fixture Harness"
echo "========================================"

if ! probe_env; then
    say_skip "all fixtures (env gaps listed above)"
    echo
    echo "========================================"
    echo "  SKIPPED (environment not ready)"
    echo "========================================"
    # On GitHub Actions, treat full-SKIP as FAIL. The rollout
    # plan promotes this step from continue-on-error to mandatory
    # once three green runs land; those green runs must reflect
    # actual fixture execution, not silent SKIP-passes. The
    # $GITHUB_ACTIONS env var is set to "true" on every Actions
    # runner; locally it is unset.
    if [ "${GITHUB_ACTIONS:-}" = "true" ]; then
        printf "%s  CI SKIP-ALL UPGRADED TO FAIL (env gaps on Actions runner)%s\n" \
            "$RED" "$NC" >&2
        exit 1
    fi
    exit 0
fi

# Track how many fixtures we actually executed. Used below to
# distinguish "all PASSed" (executed > 0 AND failed == 0) from
# "harness ran but produced no signal" (executed == 0).
EXECUTED=0

# Preflight: ensure the current system-disk.img exists. If not, rebuild.
if [ ! -f "$BUILD_DIR/system-disk.img" ]; then
    say_info "  system-disk.img missing; running bash scripts/build.sh..."
    bash "$REPO_ROOT/scripts/build.sh" >/dev/null 2>&1 || {
        say_fail "preflight: scripts/build.sh failed"
        exit 1
    }
fi

run_stale_bootloader
run_stale_kernel

echo
echo "========================================"
printf "  executed=%d failed=%d\n" "$EXECUTED" "$FAILED"
if [ "$FAILED" -eq 0 ] && [ "$EXECUTED" -gt 0 ]; then
    printf "%s  STALE-ABI HARNESS PASSED%s\n" "$GREEN" "$NC"
    echo "========================================"
    exit 0
elif [ "$EXECUTED" -eq 0 ]; then
    # env probe passed but no fixture actually ran -- shouldn't
    # happen on a healthy tree. Treat as FAIL so the "three green
    # runs" promotion criterion cannot be met by a silent no-op.
    printf "%s  STALE-ABI HARNESS NO-OP (zero fixtures executed)%s\n" \
        "$RED" "$NC"
    echo "========================================"
    exit 1
else
    printf "%s  STALE-ABI HARNESS FAILED (%d fixture(s))%s\n" \
        "$RED" "$FAILED" "$NC"
    echo "========================================"
    exit 1
fi
