#!/usr/bin/env bash
# ============================================================================
# test-smoke.sh -- Automated QEMU smoke test for CI/CD
#
# Builds the OS, boots in QEMU headless mode, captures serial output,
# and checks for expected boot messages / absence of panics.
#
# Usage:  bash scripts/test-smoke.sh
#
# Exit codes:
#   0 = PASS (boot completed successfully)
#   1 = FAIL (panic, timeout, or build failure)
# ============================================================================

set -euo pipefail

case "${1:-}" in
    -h|--help)
        cat <<'EOF'
test-smoke.sh -- end-to-end boot smoke test

Usage:
  bash scripts/test-smoke.sh            Build + boot + pattern check (default)

Behavior:
  Builds the OS, boots build/system-disk.img headless in QEMU (KVM if
  /dev/kvm is writable, TCG fallback), and asserts that "Boot complete in"
  and "C:\>" appear on serial within TIMEOUT_SEC seconds.

Environment:
  TIMEOUT_SEC    QEMU wait budget in seconds (default: 30).

Exit codes:
  0 = PASS (boot completed successfully)
  1 = FAIL (panic, timeout, or build failure)

Artifacts:
  build/smoke-test.log            raw serial capture
  build/smoke-test.stripped.log   ANSI-stripped serial capture
EOF
        exit 0
        ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="$REPO_ROOT/build"

DISK="$BUILD/system-disk.img"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.fd"
SERIAL_LOG="$BUILD/smoke-test.log"
STRIPPED_LOG="$BUILD/smoke-test.stripped.log"
POST16_MANIFEST="$BUILD/post16-manifest.env"
TIMEOUT_SEC="${TIMEOUT_SEC:-30}"

# Legacy smoke test: superseded by `bash scripts/test.sh`. Retained as a light
# boot-only sanity check (no unit-test suite). Boots the canonical GPT disk
# image via AHCI, matching Makefile's run: target. Do not re-introduce the
# grub-mkrescue ISO path -- that is retired.

# Strip ANSI color and control sequences from the serial log into a derived
# file before pattern matching. The kernel emits `\x1b[31m[FAIL]...` etc., and
# OVMF emits screen-clear sequences that collide with any bracketed substring.
# Matching the stripped copy keeps patterns robust against future color edits.
strip_ansi() {
    sed -E 's/\x1b\[[0-9;]*[A-Za-z]//g; s/\x1b[=>]//g' "$SERIAL_LOG" > "$STRIPPED_LOG" 2>/dev/null || :
}

# ---- Colors ----
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Pass/Fail criteria ----
# PASS requires BOTH signals: the kernel's "Boot complete in" sentinel AND the
# shell's `C:\>` prompt. Either alone is insufficient (a kernel can print the
# sentinel and then panic in userland). All matches run against STRIPPED_LOG,
# so bracketed patterns are literal and ANSI colors never confuse grep.
PASS_PATTERNS_ALL=(
    "Boot complete in"
    'C:\>'
)
FAIL_PATTERNS=(
    "KERNEL PANIC"
    "ASSERT FAILED"
    "triple fault"
    "General Protection Fault"
    "Page Fault"
    "Double Fault"
    "[CRIT] ExitBootServices failed"
    "[FAIL] Kernel ELF corrupt"
    "[BOOT HALT]"
    # GENERIC fatal signals (added 2026-07-28). Every entry above names one
    # specific failure, which meant a NEW fatal shape shipped green: a WHPX
    # boot of main@238c49f6 halted at `cmd.exe` with
    #   [CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0
    #   [**] FATAL -- system halted
    # and this test passed, because neither line matched any specific pattern.
    #
    # `[CRIT]` is safe to blanket-fail on: it is the klog tag for LOG_FATAL,
    # documented in include/kernel/klog.h as "serial + framebuffer, then halt".
    # It is fatal BY DEFINITION, not a severity judgement -- and it appears
    # zero times in a healthy captured log (verified against the current
    # build/smoke-test.stripped.log).
    "[CRIT]"
    "FATAL -- system halted"
    "system halted"
)

# ---- Bootloader + kernel presence checks (verified after boot) ----
# Two layers, both must pass:
#
#   Layer 1 (CORE): POST16 code assertions from the source-of-truth manifest
#                   at build/post16-manifest.env. Emitted by the bootloader's
#                   post_code16() as "[BOOT] POST 0xNNNN" on serial. These
#                   survive printf/klog rename drift because the #define
#                   values in bootx64.c are the contract.
#
#   Layer 2 (RESIDUAL): a few string assertions that capture user-visible
#                       boot signals (ExitBootServices OK, Phase 0 BOOT_INFO
#                       banner, "Boot complete in", "C:\>"). These still
#                       drift if someone renames a printf, but they catch
#                       regressions the POST16 layer cannot (userland never
#                       emits POST16 codes).
#
# The previous BOOT_REQUIRED_PATTERNS string list is kept below as the
# fallback layer until the POST16 path has been proven on KVM + TCG +
# VirtualBox + bare metal (per the developer tooling roadmap smoke-test
# assertion section).

# Layer 1 core assertions: manifest is sourced AFTER the build step (see
# Step 1 below), because the build generates the manifest. Declare the
# arrays up-front so 'set -u' doesn't trip later; they get populated by
# sourcing build/post16-manifest.env post-build.
POST16_REQUIRED=()
POST16_REQUIRED_CODES=()

# Layer 2 residual string assertions: user-visible end-to-end signals.
BOOT_REQUIRED_STRING_SIGNALS=(
    "[BOOT] ExitBootServices OK"
    "[PHASE0] BOOT_INFO"
    "Boot complete in"
    'C:\>'
    # Bare-metal hardening signals (TODO-10 Unit Tests). Each is the serial
    # evidence for a gate whose absence is silent: an unparsed FADT probes
    # legacy ports that may not exist, an unallocated IST turns a #DF into a
    # triple fault, and a failed NX enable runs with no no-execute enforcement
    # after the page tables were already marked NX. None of the three is
    # visible from a unit test -- the values are platform state, not API
    # behavior -- so the boot log is where they are asserted.
    "IAPC_BOOT_ARCH:"
    "IST stacks:"
    "Verify: NX enabled"
)

# Fallback layer: legacy string-pattern list kept in place until POST16 path
# is proven across all four validation platforms. Logs residual diagnostics
# (one line per miss) but does NOT fail the smoke test; the core assertion
# is code-based. Remove after KVM+TCG+VBox+bare-metal all confirm stable.
BOOT_FALLBACK_STRINGS=(
    "[BOOT] ELF segment"
    "[BOOT] Kernel found at"
    "[BOOT] Watchdog: armed"
    "[BOOT] Watchdog: disarmed"
)

# These patterns must NOT appear on a clean firmware boot.
BOOT_ABSENT_PATTERNS=(
    "[BOOT] mmap: overlap resolved"
    "[WARN] mmap: carve at cap"
    "[WARN] Memory map entry"
    "[FAIL] Kernel ELF corrupt"
    "[CRIT] ExitBootServices failed"
)

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Smoke Test${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo ""

# ---- Step 1: Build ----
echo -e "${CYAN}[1/3]${NC} Building OS..."
cd "$REPO_ROOT"
bash scripts/build.sh clean >/dev/null 2>&1

BUILD_RESULT=$(tail -1 build/build.log 2>/dev/null || echo "UNKNOWN")
if [ "$BUILD_RESULT" != "=== BUILD OK ===" ]; then
    echo -e "${RED}SMOKE TEST FAILED: Build failed${NC}"
    echo -e "${DIM}  Check build/build.log for details${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} Build succeeded"

# Source the freshly-generated POST16 manifest. Hard-fail on missing,
# empty, malformed, or length-mismatched arrays -- the manifest is the
# core assertion for this smoke test; skipping it silently would defeat
# the regression check.
if [ ! -s "$POST16_MANIFEST" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $POST16_MANIFEST missing or empty${NC}"
    echo -e "${DIM}  The build should have regenerated it; see build/build.log${NC}"
    exit 1
fi
# shellcheck disable=SC1090
source "$POST16_MANIFEST"
if [ "${#POST16_REQUIRED[@]}" -eq 0 ]; then
    echo -e "${RED}SMOKE TEST FAILED: POST16_REQUIRED empty after sourcing $POST16_MANIFEST${NC}"
    echo -e "${DIM}  Manifest is malformed or corrupted. Run 'bash scripts/build.sh' to regenerate.${NC}"
    exit 1
fi
if [ "${#POST16_REQUIRED[@]}" -ne "${#POST16_REQUIRED_CODES[@]}" ]; then
    echo -e "${RED}SMOKE TEST FAILED: POST16_REQUIRED / POST16_REQUIRED_CODES length mismatch${NC}"
    echo -e "${DIM}  Names=${#POST16_REQUIRED[@]} Codes=${#POST16_REQUIRED_CODES[@]}. Manifest corrupted.${NC}"
    exit 1
fi
echo -e "  ${GREEN}✓${NC} POST16 manifest loaded (${#POST16_REQUIRED[@]} required codes)"

# ---- Step 2: Boot in QEMU ----
echo -e "${CYAN}[2/3]${NC} Booting in QEMU (headless, ${TIMEOUT_SEC}s timeout)..."

# Preflight
if [ ! -f "$DISK" ]; then
    echo -e "${RED}SMOKE TEST FAILED: $DISK not found${NC}"
    echo -e "${DIM}  Run 'bash scripts/build.sh' first.${NC}"
    exit 1
fi
if [ ! -f "$OVMF_CODE" ]; then
    echo -e "${RED}SMOKE TEST FAILED: OVMF not found at $OVMF_CODE${NC}"
    exit 1
fi

cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Boot the canonical GPT image via AHCI (matches Makefile run: target).
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=raw,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -m 2G
    # SMP (2026-07-28). There was NO -smp flag here, so QEMU defaulted to ONE
    # cpu and this gate had never exercised SMP -- in a project whose stated
    # doctrine is "SMP-safe by default ... SMP from day one" (CLAUDE.md).
    #
    # That is not a theoretical gap. The exec commit-point work changed the
    # TEB <-> kernel_gs_base handoff across context switches, shipped with
    # this test green, and then halted on the operator's 2-CPU WHPX boot:
    #   [CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0
    # The single-CPU run reaches the IDENTICAL point (PID 4, same PEB/TEB) and
    # survives, because the race needs a timer tick to land in the window
    # between publishing the TEB and assigning kernel_gs_base -- which a second
    # CPU supplies and a single CPU does not.
    #
    # 2 matches the operator's WHPX configuration, which is the setup that
    # actually caught this. Override with SMOKE_SMP for a wider sweep.
    -smp "${SMOKE_SMP:-2}"
    -serial file:"$SERIAL_LOG"
    -no-reboot
    -no-shutdown
    -display none
)

# Use KVM if available
if [ -c /dev/kvm ] && [ -w /dev/kvm ] && [ -z "${SMOKE_FORCE_TCG:-}" ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    echo -e "  ${DIM}KVM acceleration enabled${NC}"
elif [ -n "${SMOKE_FORCE_TCG:-}" ]; then
    # SMOKE_FORCE_TCG (2026-07-28). CLAUDE.md is explicit that TCG is not just
    # a device-emulation net: it "catches timing-sensitive races that KVM's
    # speed hides", and the 2026-07-27 SYS_EXEC frame-handoff bug survived 297
    # commits and a green local suite precisely because KVM always landed the
    # tick that TCG did not. There was no way to ask this gate for that engine,
    # so the cheaper-and-blinder one was always used when /dev/kvm existed.
    echo -e "  ${DIM}TCG forced (SMOKE_FORCE_TCG) -- slower, catches timing races KVM hides${NC}"
fi

# Clear previous log
> "$SERIAL_LOG"

# Launch QEMU in background
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

# Cleanup trap: kill QEMU on normal exit, SIGINT, SIGTERM. Idempotent so the
# existing post-polling kill (if it still runs) is harmless.
cleanup_qemu() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    exit "$ec"
}
trap cleanup_qemu EXIT INT TERM

# Wait for boot with timeout, checking serial log periodically
BOOT_PASSED=false
BOOT_FAILED=false
FAIL_REASON=""

for i in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    strip_ansi

    # Check if QEMU crashed
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        BOOT_FAILED=true
        FAIL_REASON="QEMU exited unexpectedly"
        break
    fi

    # Check for fail patterns (match against stripped log)
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            BOOT_FAILED=true
            FAIL_REASON="Detected: $pattern"
            break 2
        fi
    done

    # PASS requires ALL patterns in PASS_PATTERNS_ALL (AND-set, not OR)
    all_found=true
    for pattern in "${PASS_PATTERNS_ALL[@]}"; do
        if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            all_found=false
            break
        fi
    done
    if [ "$all_found" = true ]; then
        BOOT_PASSED=true
        break
    fi

    # Progress indicator
    printf "\r  ${DIM}Waiting... %d/${TIMEOUT_SEC}s${NC}  " "$i"
done
printf "\r"

# Grace period after PASS markers fire so post-Phase-3 disk flushes (FAT32
# sector cache writeback for postcode.log / hwdump.txt / firmware-advisor.json /
# firmware-tables.json) actually persist to the disk image.  Without this the
# kernel logs the writes but the FAT32 cache never flushes before QEMU dies,
# leaving the disk image's bytes unchanged so consumers like `mtype` see all
# zeros.  3 seconds matches the longest observed post-"Boot complete" write
# (hwdump.txt at +3.4s in the test=1 boot log).  Skip on FAIL since waiting
# longer for a broken boot wastes operator time.
if [ "$BOOT_PASSED" = true ] && [ -z "${SMOKE_NO_GRACE:-}" ]; then
    GRACE_SEC="${SMOKE_GRACE_SEC:-3}"
    printf "  ${DIM}PASS markers detected; waiting %ds for disk flushes...${NC}  " "$GRACE_SEC"
    sleep "$GRACE_SEC"
    strip_ansi
    printf "\r"
fi

# ---- SURVIVE-PAST-THE-PROMPT re-check (2026-07-28) -------------------------
# The poll loop BREAKS the instant PASS_PATTERNS_ALL match, so anything fatal
# after that instant was never examined. That is not a hypothetical: on the
# WHPX boot of main@238c49f6 the markers landed at 22.6s and 24.6s and the
# kernel halted at 25.2s -- INSIDE the grace window above -- and this test
# reported PASS. `cmd.exe` is the first ring-3 process, so a crash there is
# precisely the "does it reach userspace" failure this test exists to catch.
#
# Re-checking the fail patterns after the grace period costs nothing (the wait
# already happened for disk flushes) and converts the grace window from a blind
# spot into the survival assertion. SMOKE_NO_GRACE skips the wait, so re-check
# regardless of whether the wait ran.
if [ "$BOOT_PASSED" = true ]; then
    strip_ansi
    for pattern in "${FAIL_PATTERNS[@]}"; do
        if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
            BOOT_PASSED=false
            BOOT_FAILED=true
            FAIL_REASON="Detected AFTER pass markers (crash past the prompt): $pattern"
            break
        fi
    done
fi

# ---- LOG MUST END CLEAN (2026-07-28) ---------------------------------------
# FAIL_PATTERNS is a denylist and therefore only ever catches fatal shapes
# somebody already enumerated -- which is exactly how the kernel_gs_base halt
# got through. This check is shape-based instead: whatever the kernel was
# saying as it stopped, it must not have been stopping. It costs one tail read
# and catches fatal shapes nobody has named yet.
if [ "$BOOT_PASSED" = true ]; then
    LAST_LINES="$(tail -5 "$STRIPPED_LOG" 2>/dev/null || true)"
    if printf '%s' "$LAST_LINES" | grep -qiE 'halt|panic|fatal|unrecoverable|triple fault|\[\*\*\]'; then
        BOOT_PASSED=false
        BOOT_FAILED=true
        FAIL_REASON="Log ends on a fatal shape (last 5 lines): $(printf '%s' "$LAST_LINES" | grep -iE 'halt|panic|fatal|unrecoverable|triple fault|\[\*\*\]' | head -1 | cut -c1-100)"
    fi
fi

# Kill QEMU (trap also does this; harmless repeat)
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

# Final strip in case the loop exited before the last pass
strip_ansi

# ---- Step 3: Results ----
echo -e "${CYAN}[3/3]${NC} Analyzing results..."
echo ""

LOG_LINES=$(wc -l < "$SERIAL_LOG" 2>/dev/null || echo 0)
echo -e "  ${DIM}Serial log: $SERIAL_LOG ($LOG_LINES lines; ANSI-stripped: $STRIPPED_LOG)${NC}"

# ---- Layer 1 (CORE): POST16 code assertions from manifest ----
# Manifest was already validated non-empty and length-consistent in Step 1;
# this loop just checks serial output.
PATTERN_FAIL=false
POST16_MISSING_COUNT=0
for i in "${!POST16_REQUIRED[@]}"; do
    name="${POST16_REQUIRED[$i]}"
    code="${POST16_REQUIRED_CODES[$i]}"
    # Match the bootloader's exact serial format: "[BOOT] POST 0xNNNN"
    # with uppercase hex. Fixed-string match -- the brackets are literal.
    if ! grep -qF -- "[BOOT] POST $code" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} POST16 $code ($name)"
        PATTERN_FAIL=true
        POST16_MISSING_COUNT=$((POST16_MISSING_COUNT + 1))
    fi
done
echo -e "  ${DIM}POST16 core: ${#POST16_REQUIRED[@]} required, $POST16_MISSING_COUNT missing${NC}"

# ---- Layer 2 (RESIDUAL): user-visible string signals ----
for pattern in "${BOOT_REQUIRED_STRING_SIGNALS[@]}"; do
    if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}MISSING:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done

# ---- Fallback layer: advisory only, does NOT fail the smoke test ----
# Kept until POST16 core is proven on KVM + TCG + VBox + bare metal.
FALLBACK_MISS_COUNT=0
for pattern in "${BOOT_FALLBACK_STRINGS[@]}"; do
    if ! grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${YELLOW}fallback-miss:${NC} $pattern (advisory, not failing)"
        FALLBACK_MISS_COUNT=$((FALLBACK_MISS_COUNT + 1))
    fi
done

# ---- Absent patterns: must NOT appear ----
for pattern in "${BOOT_ABSENT_PATTERNS[@]}"; do
    if grep -qF -- "$pattern" "$STRIPPED_LOG" 2>/dev/null; then
        echo -e "  ${RED}UNEXPECTED:${NC} $pattern"
        PATTERN_FAIL=true
    fi
done

if [ "$PATTERN_FAIL" = true ]; then
    BOOT_FAILED=true
    FAIL_REASON="Boot pattern check failed (see MISSING/UNEXPECTED above)"
fi

# ---- Loader self-measurement oracle ---------------------------------------
#
# The loader hashes the ESP file it was launched from and prints the digest.
# Comparing legs against each other would prove only DETERMINISM: four equal
# outputs are equally consistent with hashing the wrong file, a prefix, or a
# constant. So the check here is against an INDEPENDENTLY computed hash of the
# exact loader binary this build staged, plus its byte count -- which a
# prefix-only read or a wrong-file open cannot satisfy.
#
# Which file: the Makefile stages $(UEFI_EFI_SIGNED) when a MOK key exists and
# build/tools/BOOTX64.EFI otherwise, and under the shim chain-load layout that
# same binary is staged as grubx64.efi. In every layout it is the file the
# running loader was launched from, so one variable covers all three.
SELF_MEASURE_LINE="$(grep -F -- '[BOOT] self-measure:' "$STRIPPED_LOG" 2>/dev/null | head -1)"
if [ -z "$SELF_MEASURE_LINE" ]; then
    echo -e "  ${RED}MISSING:${NC} [BOOT] self-measure: line absent from serial"
    BOOT_FAILED=true
    FAIL_REASON="${FAIL_REASON:-Loader self-measurement did not report}"
else
    # MOK_KEY mirrors the Makefile default and honours the same override, so an
    # externally keyed build does not hash the unsigned loader while the disk
    # staged the signed one.
    SMOKE_MOK_KEY="${MOK_KEY:-keys/MOK.key}"
    LOADER_FILE="$BUILD/tools/BOOTX64.EFI"
    if [ -f "$SMOKE_MOK_KEY" ] && [ -f "$BUILD/tools/BOOTX64.signed.efi" ]; then
        LOADER_FILE="$BUILD/tools/BOOTX64.signed.efi"
    fi
    # Validate the WHOLE line against the emitted schema before extracting
    # anything: field order, decimal byte count, 16 uppercase TSC digits and 64
    # uppercase digest digits, in that exact order. Matching fields "anywhere on
    # the line" would let the producer/consumer format drift silently.
    # The digest token is esp-file-sha256, not a bare sha256: the loader hashes
    # the ESP FILE, never the bytes firmware executed (that measurement is the
    # firmware's own Authenticode PE hash in PCR 4). The anchored schema is what
    # makes the rename real -- a producer left on the old token fails here on
    # every leg rather than quietly reverting the claim.
    SELF_MEASURE_SCHEMA='^\[BOOT\] self-measure: status=ok bytes=[0-9]{1,20} tsc=[0-9A-F]{16} esp-file-sha256=[0-9A-F]{64}$'
    SELF_MEASURE_BODY="$(printf '%s' "$SELF_MEASURE_LINE" | sed 's/\r$//')"
    if [ ! -f "$LOADER_FILE" ]; then
        echo -e "  ${YELLOW}self-measure:${NC} $LOADER_FILE absent -- oracle skipped (advisory)"
    elif ! printf '%s' "$SELF_MEASURE_BODY" | grep -qE -- "$SELF_MEASURE_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} self-measure line is not a schema-valid success: $SELF_MEASURE_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader self-measurement reported ABSENT or off-schema}"
    else
        REPORTED_SHA="$(printf '%s' "$SELF_MEASURE_BODY" | sed -n 's/.* esp-file-sha256=\([0-9A-F]\{64\}\)$/\1/p' | tr 'A-F' 'a-f')"
        REPORTED_BYTES="$(printf '%s' "$SELF_MEASURE_BODY" | sed -n 's/.* bytes=\([0-9]\{1,20\}\) .*/\1/p')"
        ORACLE_SHA="$(sha256sum "$LOADER_FILE" | cut -d' ' -f1)"
        ORACLE_BYTES="$(wc -c < "$LOADER_FILE" | tr -d ' ')"
        if [ "$REPORTED_SHA" = "$ORACLE_SHA" ] && [ "$REPORTED_BYTES" = "$ORACLE_BYTES" ]; then
            echo -e "  ${GREEN}✓${NC} self-measure matches $LOADER_FILE ($ORACLE_BYTES bytes)"
        else
            echo -e "  ${RED}MISMATCH:${NC} self-measure ${REPORTED_SHA:0:16}.../${REPORTED_BYTES} B"
            echo -e "  ${DIM}  oracle  ${ORACLE_SHA:0:16}.../${ORACLE_BYTES} B ($LOADER_FILE)${NC}"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Loader self-measurement does not match the staged binary}"
        fi
    fi

    # ---- PCR 4 correlation verdict -----------------------------------------
    # The correlation is the section's actual deliverable and was the one line
    # of the three with no schema behind it -- omission, not intent. Anchor it,
    # because the failure this guards is not a wrong digest but a SILENT one:
    # if correlate_pcr4_measurement stopped running, or started reporting an
    # outcome nobody enumerated, every other assertion here would still pass.
    #
    # The verdict itself is deliberately NOT constrained to a single value. A
    # dev-host boot has no TPM and correctly reports no-event-log; a machine
    # with one reports a match or a named absence. What IS constrained is that
    # the line exists, names an enumerated outcome, and carries its cost. A
    # bare AGREE is rejected on purpose: nothing on this path reads or replays
    # a PCR, so that word must not appear.
    PCR4_LINE="$(grep -F -- '[BOOT] pcr4-correlate:' "$STRIPPED_LOG" 2>/dev/null | head -1)"
    PCR4_BODY="$(printf '%s' "$PCR4_LINE" | sed 's/\r$//')"
    PCR4_SCHEMA='^\[BOOT\] pcr4-correlate: result=(UNAUTHENTICATED-LOG-MATCH|DISAGREE|no-local-digest|no-image-device-path|no-devpath-utilities|degenerate-device-path|no-event-log|log-unusable|no-comparison)([ ][a-z0-9-]+=[0-9A-Za-z-]+)* tsc=[0-9A-F]{16}$'
    if [ -z "$PCR4_LINE" ]; then
        echo -e "  ${RED}MISSING:${NC} [BOOT] pcr4-correlate: line absent from serial"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-PCR 4 correlation did not report}"
    elif ! printf '%s' "$PCR4_BODY" | grep -qE -- "$PCR4_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} pcr4-correlate line is not a schema-valid outcome: $PCR4_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-PCR 4 correlation reported an unenumerated outcome}"
    else
        echo -e "  ${GREEN}OK${NC} pcr4-correlate: $(printf '%s' "$PCR4_BODY" | sed -n 's/.*result=\([A-Za-z-]*\).*/\1/p')"
    fi

    # ---- Authenticode PE digest oracle -------------------------------------
    # The flat digest above says which FILE was hashed. This one says whether
    # that file could be what firmware executed: it is the Authenticode PE hash,
    # the only digest a PCR 4 EV_EFI_BOOT_SERVICES_APPLICATION measurement can
    # ever equal. Cross-checked against an INDEPENDENT transcription of the
    # spec's numbered steps, so agreement here is the loader agreeing with the
    # spec as read by someone else -- not with itself. A flat hash cannot
    # satisfy this, which is the point.
    SELF_MEASURE_PE_LINE="$(grep -F -- '[BOOT] self-measure-pe:' "$STRIPPED_LOG" 2>/dev/null | head -1)"
    SELF_MEASURE_PE_BODY="$(printf '%s' "$SELF_MEASURE_PE_LINE" | sed 's/\r$//')"
    SELF_MEASURE_PE_SCHEMA='^\[BOOT\] self-measure-pe: status=ok pe-authenticode-sha256=[0-9A-F]{64}$'
    if [ ! -f "$LOADER_FILE" ]; then
        : # already reported advisory-skipped above
    elif [ -z "$SELF_MEASURE_PE_LINE" ]; then
        echo -e "  ${RED}MISSING:${NC} [BOOT] self-measure-pe: line absent from serial"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader Authenticode measurement did not report}"
    elif ! printf '%s' "$SELF_MEASURE_PE_BODY" | grep -qE -- "$SELF_MEASURE_PE_SCHEMA"; then
        echo -e "  ${RED}MISSING:${NC} self-measure-pe line is not a schema-valid success: $SELF_MEASURE_PE_BODY"
        BOOT_FAILED=true
        FAIL_REASON="${FAIL_REASON:-Loader Authenticode measurement reported ABSENT or off-schema}"
    else
        REPORTED_PE_SHA="$(printf '%s' "$SELF_MEASURE_PE_BODY" \
            | sed -n 's/.* pe-authenticode-sha256=\([0-9A-F]\{64\}\)$/\1/p' | tr 'A-F' 'a-f')"
        ORACLE_PE_SHA="$(python3 "$REPO_ROOT/tools/boot-header-tests/authenticode_oracle.py" \
            "$LOADER_FILE" 2>/dev/null || true)"
        if [ -z "$ORACLE_PE_SHA" ]; then
            echo -e "  ${YELLOW}self-measure-pe:${NC} oracle unavailable -- comparison skipped (advisory)"
        elif [ "$REPORTED_PE_SHA" = "$ORACLE_PE_SHA" ]; then
            echo -e "  ${GREEN}✓${NC} self-measure-pe matches the independent Authenticode oracle"
        else
            echo -e "  ${RED}MISMATCH:${NC} self-measure-pe ${REPORTED_PE_SHA:0:16}..."
            echo -e "  ${DIM}  oracle      ${ORACLE_PE_SHA:0:16}... ($LOADER_FILE)${NC}"
            BOOT_FAILED=true
            FAIL_REASON="${FAIL_REASON:-Loader Authenticode digest does not match the independent oracle}"
        fi
    fi
fi

# ---- Log cleanliness: ANY unexpected [FAIL] fails the run -------------------
#
# FAIL_PATTERNS above is a nine-string allowlist of FATAL signatures. It is an
# early abort, not a verdict: a [FAIL] outside those nine passed straight
# through, so this script would print SMOKE TEST PASSED over a log that
# contained one. Demonstrated 2026-07-27 -- a local run reported PASS with
# "[FAIL] BOOT-BUDGET: EXEC -> DESKTOP_READY took 1139ms" and 32 [WARN] lines in
# its own output. That is the specific reason smoke testing after each TODO
# section did not catch what a full native run later surfaced.
#
# So the verdict is inverted here: every [FAIL] counts UNLESS it is baselined.
# The baseline is shared with the kernel-runner cleanliness gate so the two
# cannot drift, and every entry there carries a reason.
#
# WARN is deliberately NOT gated yet. A clean run still emits ~30 of them, and
# gating before the expected-output declaration lands (the test-side half of
# this work) would mean baselining thirty correct lines -- which is how a
# baseline turns into a rubber stamp. FAIL is the tight, high-signal half.
BASELINE_FILE="$REPO_ROOT/scripts/log-baseline.txt"
UNEXPECTED_FAILS=""
if [ -f "$STRIPPED_LOG" ]; then
    while IFS= read -r fail_line; do
        [ -n "$fail_line" ] || continue
        is_baselined=false
        if [ -f "$BASELINE_FILE" ]; then
            while IFS= read -r entry; do
                case "$entry" in ''|'#'*) continue ;; esac
                case "$fail_line" in *"$entry"*) is_baselined=true; break ;; esac
            done < "$BASELINE_FILE"
        fi
        if [ "$is_baselined" = false ]; then
            UNEXPECTED_FAILS="${UNEXPECTED_FAILS}${fail_line}"$'\n'
        fi
    done < <(grep -F '[FAIL]' "$STRIPPED_LOG" 2>/dev/null || true)
fi

if [ -n "$UNEXPECTED_FAILS" ]; then
    UNEXPECTED_COUNT=$(printf '%s' "$UNEXPECTED_FAILS" | grep -c . || true)
    echo -e "  ${RED}UNEXPECTED [FAIL] lines: $UNEXPECTED_COUNT${NC}"
    printf '%s' "$UNEXPECTED_FAILS" | head -10 | sed 's/^/    /'
    echo -e "  ${DIM}If one is correct, add it to scripts/log-baseline.txt WITH a reason.${NC}"
    BOOT_FAILED=true
    FAIL_REASON="Unexpected [FAIL] in serial log ($UNEXPECTED_COUNT line(s))"
else
    echo -e "  ${DIM}Log cleanliness: no unexpected [FAIL] lines${NC}"
fi

if [ "$BOOT_FAILED" = true ]; then
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: $FAIL_REASON${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    # Show relevant log lines
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
elif [ "$BOOT_PASSED" = true ]; then
    # Extract boot time
    BOOT_TIME=$(grep -o "Boot complete in [0-9.]*s" "$SERIAL_LOG" 2>/dev/null | head -1 || echo "")
    echo ""
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}  SMOKE TEST PASSED${NC}"
    if [ -n "$BOOT_TIME" ]; then
        echo -e "  ${DIM}$BOOT_TIME${NC}"
    fi
    echo -e "${GREEN}══════════════════════════════════════════════════${NC}"
    echo ""
    # Content-bound smoke receipt (image + build inputs + toolchain + markers).
    # A rollover / verification over an unchanged image + inputs is then free;
    # any change to the image or a build input invalidates it. Best-effort.
    python3 "$(dirname "$0")/overnight/receipts.py" record-smoke . >/dev/null 2>&1 || true
    exit 0
else
    echo ""
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo -e "${RED}  SMOKE TEST FAILED: Timeout (${TIMEOUT_SEC}s)${NC}"
    echo -e "${RED}  Boot did not complete within the time limit.${NC}"
    echo -e "${RED}══════════════════════════════════════════════════${NC}"
    echo ""
    echo -e "${DIM}Last 10 lines of serial output:${NC}"
    tail -10 "$STRIPPED_LOG" 2>/dev/null | sed 's/^/  /'
    echo ""
    exit 1
fi
