#!/bin/bash
# =============================================================================
# test.sh -- Build, boot QEMU headless, run kernel unit tests, report results
#
# Usage:
#   bash scripts/test.sh              # run all test suites
#   bash scripts/test.sh SUITE=mm     # run only Memory Management suites
#   bash scripts/test.sh SUITE=ob     # run only Object Manager suites
#   bash scripts/test.sh QUIET=1      # summary only (suppress PASS lines)
#   bash scripts/test.sh SUITE=fs QUIET=1  # combine both
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed or timeout
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT/build"
DISK="$BUILD_DIR/system-disk.img"
TEST_LOG="$BUILD_DIR/test.log"
OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS="/usr/share/OVMF/OVMF_VARS_4M.fd"
OVMF_VARS_CP="$BUILD_DIR/OVMF_VARS_4M.fd"
TIMEOUT="${TIMEOUT:-60}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

print_help() {
    cat <<'EOF'
Impossible OS -- kernel unit test wrapper

Usage:
  bash scripts/test.sh              Build, boot QEMU headless, run all test suites
  bash scripts/test.sh SUITE=mm     Filter to Memory Management suites only
  bash scripts/test.sh SUITE=fs     Filter to Filesystem suites
  bash scripts/test.sh SUITE=ob     Filter to Object Manager suites
  bash scripts/test.sh QUIET=1      Summary only (suppress per-test PASS lines)
  bash scripts/test.sh QUIET=0      Force verbose (overrides the overnight default)
  bash scripts/test.sh SUITE=fs QUIET=1   Combine filters
  (QUIET defaults ON when OVERNIGHT_SEQUENCER_RUN=1; FAIL lines always show)
  TIMEOUT=120 bash scripts/test.sh  Extend QEMU timeout (default: 60s)
  bash scripts/test.sh XML=1        Emit JUnit XML to build/test-results.xml
  bash scripts/test.sh JSON=1       Emit the launcher's JSON record stream
  bash scripts/test.sh TAP=1        Emit the launcher's TAP producer stream
  bash scripts/test.sh --help       Show this help

Categories (SUITE=...):
  mm, fs, sched, ob, security, ipc, boot, abi, storage, exec, x86, desktop, ex, nls,
  knf, except, quota

Outputs:
  build/test.log           full QEMU serial output
  stdout                   filtered suite results + final PASS/FAIL summary
  Coverage updated to docs/test-coverage/ on success.

Exit codes:
  0 = all tests passed
  1 = one or more tests failed, or timeout (no summary line found)

Boot.conf is patched with test=1 (plus test_suite/test_quiet) for the QEMU run
and restored on completion. See CLAUDE.md "Testing" for the category registry.
EOF
}

# Parse optional arguments
SUITE_FILTER=""
SUITE_CATEGORY=""
# QUIET defaults ON under the overnight sequencer run: the per-test PASS
# flood is never useful there (it bloats the report log AND the driver's
# context), and QUIET keeps FAIL lines + the summary + format lines. An
# explicit QUIET=0 forces verbose (debugging a specific overnight run);
# QUIET=1 stays accepted everywhere else.
QUIET_MODE=0
[ "${OVERNIGHT_SEQUENCER_RUN:-}" = "1" ] && QUIET_MODE=1
XML_MODE=0
JSON_MODE=0
# TAP is the launcher's third machine artifact and had no runner knob: it
# could only be turned on by hand-editing boot.conf, so the one artifact
# with a documented producer contract was the one this script could never
# exercise. Same shape as XML=1 / JSON=1.
TAP_MODE=0
for arg in "$@"; do
    case "$arg" in
        -h|--help) print_help; exit 0 ;;
        SUITE=*)  SUITE_FILTER="${arg#SUITE=}"
                  SUITE_CATEGORY="$SUITE_FILTER" ;;
        QUIET=1)  QUIET_MODE=1 ;;
        QUIET=0)  QUIET_MODE=0 ;;
        XML=1)    XML_MODE=1 ;;
        JSON=1)   JSON_MODE=1 ;;
        TAP=1)    TAP_MODE=1 ;;
    esac
done

# --- Step 1: Build ---
echo -e "${CYAN}${BOLD}[TEST]${RESET} Building kernel..."
if ! bash "$PROJECT/scripts/build.sh" > /dev/null 2>&1; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} Build failed. Check build/build.log"
    exit 1
fi
echo -e "${GREEN}[TEST]${RESET} Build OK"

# --- Step 2: Patch boot.conf for test mode ---
PATCH_ARGS="test 1"
if [ -n "$SUITE_CATEGORY" ]; then
    PATCH_ARGS="$PATCH_ARGS test_suite $SUITE_CATEGORY"
fi
if [ "$QUIET_MODE" -eq 1 ]; then
    PATCH_ARGS="$PATCH_ARGS test_quiet 1"
fi
if [ "$XML_MODE" -eq 1 ]; then
    PATCH_ARGS="$PATCH_ARGS xml 1"
fi
if [ "$JSON_MODE" -eq 1 ]; then
    PATCH_ARGS="$PATCH_ARGS json 1"
fi
if [ "$TAP_MODE" -eq 1 ]; then
    PATCH_ARGS="$PATCH_ARGS tap 1"
fi
bash "$PROJECT/scripts/patch-boot-conf.sh" $PATCH_ARGS > /dev/null

# Install cleanup trap now that boot.conf is patched. Runs on normal exit,
# SIGINT (Ctrl-C), or SIGTERM, restoring boot.conf and killing any backgrounded
# QEMU process. Preserves the caller's exit code.
QEMU_PID=""
cleanup() {
    local ec=$?
    if [ -n "${QEMU_PID:-}" ] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    bash "$PROJECT/scripts/patch-boot-conf.sh" reset > /dev/null 2>&1 || true
    exit "$ec"
}
trap cleanup EXIT INT TERM

# --- Step 3: Boot QEMU headless ---
cp "$OVMF_VARS" "$OVMF_VARS_CP"
rm -f "$TEST_LOG"

# CI_PARITY=1 -- reproduce what CI runs, exactly, before code leaves the machine.
#
# This is a TIER, not a replacement for the KVM inner loop. KVM stays the default
# (~2s boot); losing it would wreck iteration speed for no benefit, because the
# two engines catch different things:
#
#   inner loop      KVM (auto-selected)        every edit
#   CI-parity gate  distro QEMU + forced TCG   before push
#   authoritative   WHPX / VBox / bare metal   before shipping
#
# Why this tier has to exist: a fork+exec frame-handoff bug survived 297 commits
# and a green local suite because it only reproduced on the QEMU CI installs.
# The dev host runs a custom /usr/local build; CI runs `apt-get install
# qemu-system-x86` on ubuntu-latest with no reliable /dev/kvm. Different engine,
# different timing, different bug -- and the local runs never saw it.
#
# The selection is DERIVED, not pinned to a version string: CI installs the
# distro package, so this uses the distro package (/usr/bin), which tracks apt
# the same way CI's does. A hardcoded "8.2.2" would silently stop matching the
# day GitHub moves the runner image. The recorded version below is an assertion,
# not the selector -- a mismatch WARNS loudly (drift detected) instead of quietly
# testing something CI does not run.
CI_PARITY_QEMU="/usr/bin/qemu-system-x86_64"
QEMU_BIN="${QEMU_BIN:-qemu-system-x86_64}"
if [ "${CI_PARITY:-0}" = "1" ]; then
    if [ ! -x "$CI_PARITY_QEMU" ]; then
        echo -e "${RED}[TEST]${RESET} CI_PARITY=1 but $CI_PARITY_QEMU is absent."
        echo -e "${YELLOW}       CI runs 'apt-get install qemu-system-x86'; install it locally to match.${RESET}"
        exit 1
    fi
    QEMU_BIN="$CI_PARITY_QEMU"
    FORCE_TCG=1          # CI has no reliable /dev/kvm (see .github/workflows/build.yml)
    CI_PARITY_VER="$("$CI_PARITY_QEMU" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    CI_PARITY_EXPECT_FILE="$PROJECT/scripts/ci-qemu-version.txt"
    if [ -f "$CI_PARITY_EXPECT_FILE" ]; then
        CI_PARITY_EXPECT="$(grep -vE '^\s*#|^\s*$' "$CI_PARITY_EXPECT_FILE" | head -1 | tr -d '[:space:]')"
        if [ -n "$CI_PARITY_EXPECT" ] && [ "$CI_PARITY_VER" != "$CI_PARITY_EXPECT" ]; then
            echo -e "${YELLOW}[TEST]${RESET} CI-parity DRIFT: local QEMU $CI_PARITY_VER, recorded CI $CI_PARITY_EXPECT."
            echo -e "${YELLOW}       The runner image may have moved. Confirm against a CI log and refresh${RESET}"
            echo -e "${YELLOW}       $CI_PARITY_EXPECT_FILE -- do not assume the gate still matches CI.${RESET}"
        fi
    fi
    echo -e "${CYAN}[TEST]${RESET} CI-parity mode: $CI_PARITY_QEMU ($CI_PARITY_VER), forced TCG"
fi

# Detect acceleration: KVM > TCG. `FORCE_TCG=1` overrides for runs that
# must exercise the software-emulation path (device emulation bugs,
# leak-detector platform-coverage sweeps, etc.).
if [ "${FORCE_TCG:-0}" = "1" ]; then
    ACCEL_ARGS="-accel tcg -cpu Haswell"
    ACCEL_NAME="TCG (FORCE_TCG=1)"
elif [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL_ARGS="-accel kvm -cpu host"
    ACCEL_NAME="KVM"
else
    ACCEL_ARGS="-accel tcg -cpu Haswell"
    ACCEL_NAME="TCG (slow)"
    echo -e "${YELLOW}[TEST]${RESET} KVM not available -- using TCG (add user to kvm group for 10x speedup)"
fi

echo -e "${CYAN}[TEST]${RESET} Booting QEMU headless (${ACCEL_NAME}, ${TIMEOUT}s timeout)..."

# Launch QEMU in background -- kernel continues to desktop after tests,
# so we poll for the summary line and kill QEMU once we have results.
"$QEMU_BIN" \
    $ACCEL_ARGS \
    -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$OVMF_VARS_CP" \
    -drive id=disk0,file="$DISK",format=raw,if=none \
    -device ich9-ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -m 2G \
    -serial file:"$TEST_LOG" \
    -display none \
    -device rtl8139,netdev=net0 \
    -netdev user,id=net0 \
    -device virtio-tablet-pci \
    -rtc base=localtime \
    -no-reboot 2>/dev/null &
QEMU_PID=$!

# Poll for test summary line or timeout.
#
# Two completion markers:
#   1. Kernel TEST runner: `=== N tests passed ...` -- always required.
#   2. User-mode UTEST launcher: `UTEST: === N passed, N failed, N skipped
#      of N total` -- ALSO required when (a) XML=1/JSON=1 (machine-readable
#      streams need a clean envelope) OR (b) any `UTEST:` line has appeared
#      on serial. Case (b) means the launcher started; killing QEMU before
#      it finishes would silently drop user-mode regressions because Step 5b
#      would see HAS_UTEST=0 and the gate would stay kernel-only. Pure
#      kernel-only runs (test_kernel_skip=0 + no usermode binaries / boot
#      where launcher never starts) emit zero `UTEST:` lines, so this gate
#      is a no-op for them. Codex adversarial 2026-04-22.
ELAPSED=0
FOUND=0
while [ "$ELAPSED" -lt "$TIMEOUT" ]; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ -f "$TEST_LOG" ] && grep -q '=== .* tests\? passed' "$TEST_LOG" 2>/dev/null; then
        FOUND=1
        UMODE_NEEDED=0
        [ "$XML_MODE" -eq 1 ] && UMODE_NEEDED=1
        [ "$JSON_MODE" -eq 1 ] && UMODE_NEEDED=1
        # TAP is a machine-readable stream on the same footing: its plan
        # line is emitted at the END of the run, so a launcher cut short
        # would leave a plan-less (malformed) TAP stream.
        [ "$TAP_MODE" -eq 1 ] && UMODE_NEEDED=1
        # If any UTEST: line has appeared, the launcher has started;
        # require its summary too so a mid-run panic cannot exit green.
        if [ "$UMODE_NEEDED" -eq 0 ] && grep -q 'UTEST:' "$TEST_LOG" 2>/dev/null; then
            UMODE_NEEDED=1
        fi
        if [ "$UMODE_NEEDED" -eq 1 ]; then
            if grep -q 'UTEST: === .* passed, .* failed, .* skipped of' \
                    "$TEST_LOG" 2>/dev/null; then
                sleep 1
                break
            fi
            # User-mode summary not yet present -- keep polling.
            FOUND=0
            continue
        fi
        sleep 1
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
done

# --- Step 4: Cleanup trap (installed above) handles QEMU kill + boot.conf reset ---

# --- Step 5: Parse results ---
if [ ! -f "$TEST_LOG" ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} No test output (QEMU did not produce serial log)"
    exit 1
fi

if [ "$FOUND" -eq 0 ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} Test summary not found within ${TIMEOUT}s"
    echo ""
    echo -e "${YELLOW}Last 10 lines of serial output:${RESET}"
    tail -10 "$TEST_LOG" 2>/dev/null || true
    exit 1
fi

# Look for the summary line
SUMMARY=$(grep -E '=== [0-9]+ tests? passed' "$TEST_LOG" 2>/dev/null | tail -1 || true)

if [ -z "$SUMMARY" ]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} No test summary found in output"
    echo ""
    echo -e "${YELLOW}Last 20 lines of serial output:${RESET}"
    tail -20 "$TEST_LOG" 2>/dev/null || true
    exit 1
fi

# Extract numbers
PASSED=$(echo "$SUMMARY" | grep -oP '\d+(?= tests? passed)' || echo "0")
FAILED=$(echo "$SUMMARY" | grep -oP '\d+(?= FAILED)' || echo "0")
# -9 CI-gating: leaks were advisory while the retrofit was in flight;
# now that every existing suite reaches L=0, promote unannotated leaks
# into the failure count so a new regression cannot silently pollute
# the summary. TEST_EXPECT_LEAK / TEST_LEAK_IGNORE annotations
# suppress the counter entirely at the kernel level, so a legitimate
# deliberate leak never reaches this gate.
#
# Fail-CLOSED on parse drift: if the summary line exists but does not
# carry a ", N leaked" field, the kernel produced an unexpected summary
# format (or `grep -P` is missing). Treat that as a gate failure, not
# as "0 leaks" -- silently defaulting to zero defeats the point of the
# gate. Codex adversarial review of the leak-retrofit pass (2026-04-22).
if ! echo "$SUMMARY" | grep -qE ' leaked'; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} test summary missing 'leaked' counter (format drift or grep -P unavailable)"
    echo "  summary: $SUMMARY"
    exit 1
fi
LEAKED=$(echo "$SUMMARY" | grep -oP '\d+(?= leaked)')
if ! [[ "$LEAKED" =~ ^[0-9]+$ ]]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} could not parse leak count from summary"
    echo "  summary: $SUMMARY"
    exit 1
fi
if [ "$LEAKED" -gt 0 ]; then
    FAILED=$((${FAILED:-0} + LEAKED))
fi

# Quota leak gate (kernel resource accounting). Counted per test CATEGORY, not per
# suite, and folded into FAILED exactly like the heap gate above -- a category
# that ends with more quota charged than it started with left an obligation
# nobody returned, which is a real resource leak even when the heap balances
# (the charge and the allocation are separate ledgers).
#
# Deliberately a SEPARATE counter from 'leaked': that one is defined in bytes
# of heap per suite, so overloading it would corrupt its meaning and make a
# legitimately retained canonical USER block read as a heap leak.
#
# Fail-CLOSED on parse drift, same rationale as the heap gate: a summary
# without the field means the kernel emitted an unexpected format, and
# defaulting to zero would silently disarm the gate.
if ! echo "$SUMMARY" | grep -qE ' quota-leaked'; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} test summary missing 'quota-leaked' counter (format drift)"
    echo "  summary: $SUMMARY"
    exit 1
fi
QUOTA_LEAKED=$(echo "$SUMMARY" | grep -oP '\d+(?= quota-leaked)')
if ! [[ "$QUOTA_LEAKED" =~ ^[0-9]+$ ]]; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} could not parse quota-leak count from summary"
    echo "  summary: $SUMMARY"
    exit 1
fi
if [ "$QUOTA_LEAKED" -gt 0 ]; then
    FAILED=$((${FAILED:-0} + QUOTA_LEAKED))
fi

# Show individual test results (filtered by suite if requested)
echo ""
if [ -n "$SUITE_FILTER" ]; then
    echo -e "${CYAN}[TEST]${RESET} Filter: SUITE=${SUITE_FILTER}"
    echo ""
fi

# Show suite results. QUIET_MODE=1 suppresses per-test [ OK ] lines at the
# host layer (belt-and-suspenders with the kernel's g_quiet gate in
# test_runner.c -- if boot.conf patching silently fails and the kernel
# emits TEST: lines anyway, the host still honors the --help contract
# "summary only"). FAIL lines are ALWAYS shown regardless of QUIET so
# real regressions cannot hide behind the suppress.
{ grep -E 'TEST:.*::' "$TEST_LOG" 2>/dev/null || true; } | while IFS= read -r line; do
    if [ -n "$SUITE_FILTER" ]; then
        echo "$line" | grep -qi "$SUITE_FILTER" || continue
    fi
    if echo "$line" | grep -q '\[FAIL\]'; then
        echo -e "  ${RED}FAIL${RESET}  $(echo "$line" | sed 's/.*TEST: //')"
    elif [ "$QUIET_MODE" -eq 0 ]; then
        echo -e "  ${GREEN} OK ${RESET}  $(echo "$line" | sed 's/.*TEST: //')"
    fi
done

# Show any FAIL lines that aren't suite results (boot test failures)
{ grep -E '\[FAIL\]' "$TEST_LOG" 2>/dev/null | grep -v '::' || true; } | while IFS= read -r line; do
    echo -e "  ${RED}FAIL${RESET}  $(echo "$line" | sed 's/.*TEST: //')"
done

# --- Step 5b: User-mode UTEST results ---
#
# The scenario launcher emits one verdict line per binary as
# `UTEST: <name>: PASS (exit=0)` / `FAIL (exit=N)` / `SKIP (exit=77)` /
# `TIMEOUT` / `ISOLATION` / `LEAK`, plus a final
# `UTEST: === N passed, N failed, N skipped of N total ===` summary.
# When boot.conf `tap=1` is set, the launcher ADDITIONALLY emits TAP
# producer lines (`UTEST: 1..N`, `UTEST: ok N - name`, `UTEST: not ok N - name # ...`).
# We display a per-binary verdict block (kernel TEST style: green OK,
# red FAIL) and fold any UTEST FAIL count into the final exit code so
# `make test` exits non-zero when ANY user-mode binary regressed --
# the kernel TEST summary alone does not catch user-mode failures.
HAS_UTEST=0
grep -qE 'UTEST: === [0-9]+ passed' "$TEST_LOG" 2>/dev/null && HAS_UTEST=1

UTEST_PASS=0
UTEST_FAIL=0
UTEST_SKIP=0

if [ "$HAS_UTEST" -eq 1 ]; then
    echo ""
    echo -e "${CYAN}[UTEST]${RESET} User-mode test binaries:"

    # Per-binary verdict lines (`UTEST: <name>: <STATUS> ...`). Skip the
    # informational `format=` plumb-through and the framing summary lines
    # that the loop below would otherwise echo as ambiguous "OK" entries.
    { grep -E 'UTEST: [^ ]+\.exe: (PASS|FAIL|SKIP|TIMEOUT|ISOLATION|LEAK|format=)' \
          "$TEST_LOG" 2>/dev/null || true; } | while IFS= read -r line; do
        verdict=$(echo "$line" | sed -E 's/.*UTEST: //')
        if echo "$verdict" | grep -qE ': (FAIL|TIMEOUT|ISOLATION|LEAK)'; then
            echo -e "  ${RED}FAIL${RESET}  $verdict"
        elif echo "$verdict" | grep -q ': SKIP'; then
            # `|| true` so the while loop body's last command is always
            # success-exit. Under `set -e`, a quiet-mode branch where
            # the short-circuit `&&` evaluates to false trips the loop
            # on the final iteration (QUIET_MODE=1 + a UTEST: PASS line
            # at EOF would end the stream with exit 1, firing the
            # cleanup trap with ec=1). Codex adversarial caught this
            # when forcing a TCG run via FORCE_TCG=1 QUIET=1.
            { [ "$QUIET_MODE" -eq 0 ] && echo -e "  ${YELLOW}SKIP${RESET}  $verdict"; } || true
        elif echo "$verdict" | grep -q ': PASS'; then
            { [ "$QUIET_MODE" -eq 0 ] && echo -e "  ${GREEN} OK ${RESET}  $verdict"; } || true
        elif echo "$verdict" | grep -q 'format='; then
            # Show `<name>: format=<FMT>` lines in QUIET too so a loader-
            # routing regression surfaces even when per-binary OK lines
            # are suppressed. Cheap (one line per loader-coverage binary).
            echo -e "  ${CYAN}FMT ${RESET}  $verdict"
        fi
    done

    # TAP producer lines (`UTEST: ok N - name` / `not ok N - name # ...` /
    # `1..N` plan). When `tap=1` was set in boot.conf, both the verdict
    # lines above AND these TAP lines are emitted; show TAP separately so
    # downstream TAP consumers (CI, tap-junit) can also lift them straight
    # from the host log without the launcher's wrapper formatting.
    # `Bail out!` is part of the producer stream and is the STRONGEST line
    # in it -- it means the run aborted. Excluding it (as this extraction
    # did until 2026-07-28) hid the abort from every downstream consumer
    # reading the lifted stream, which would then see a short, plan-less
    # run and no reason for it. The plan is now trailing and is suppressed
    # after a bail-out, per the TAP contract that nothing follows it.
    HAS_TAP=0
    grep -qE 'UTEST: (ok|not ok|Bail out!|1\.\.[0-9]+)' "$TEST_LOG" 2>/dev/null && HAS_TAP=1
    if [ "$HAS_TAP" -eq 1 ] && [ "$QUIET_MODE" -eq 0 ]; then
        echo -e "  ${CYAN}TAP${RESET} producer stream:"
        grep -E 'UTEST: (ok|not ok|Bail out!|1\.\.[0-9]+)' "$TEST_LOG" 2>/dev/null |
            sed -E 's/.*UTEST: /    /'
    fi

    # Pull the launcher's authoritative summary numbers.
    UTEST_SUM=$(grep -E 'UTEST: === [0-9]+ passed, [0-9]+ failed, [0-9]+ skipped of [0-9]+ total' \
                "$TEST_LOG" 2>/dev/null | tail -1 || true)
    if [ -n "$UTEST_SUM" ]; then
        UTEST_PASS=$(echo "$UTEST_SUM" | sed -E 's/.*=== ([0-9]+) passed.*/\1/')
        UTEST_FAIL=$(echo "$UTEST_SUM" | sed -E 's/.*passed, ([0-9]+) failed.*/\1/')
        UTEST_SKIP=$(echo "$UTEST_SUM" | sed -E 's/.*failed, ([0-9]+) skipped.*/\1/')
        echo -e "  ${CYAN}[UTEST]${RESET} ${UTEST_PASS} passed, ${UTEST_FAIL} failed, ${UTEST_SKIP} skipped"
    fi

    # Cross-check the launcher summary against the per-binary verdict
    # stream. If a binary printed FAIL/TIMEOUT/ISOLATION/LEAK but the
    # summary says 0 failed, trust the per-binary count (fail-closed).
    # Covers a launcher counter bug or a malformed summary line -- the
    # data to fail correctly is already on serial; dropping it would
    # re-introduce the false-green the Step 5b gate was meant to close.
    # `grep -c` exits 1 when zero matches; set -euo pipefail would abort
    # the run, so swallow the exit + guarantee an integer with `|| true`
    # and a `:-0` default.
    UTEST_FAIL_OBSERVED=$({ grep -cE 'UTEST: [^ ]+\.exe: (FAIL|TIMEOUT|ISOLATION|LEAK)' \
                            "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_FAIL_OBSERVED=${UTEST_FAIL_OBSERVED:-0}
    if [ "$UTEST_FAIL_OBSERVED" -gt "${UTEST_FAIL:-0}" ]; then
        echo -e "  ${RED}[UTEST]${RESET} summary says ${UTEST_FAIL:-0} failed but ${UTEST_FAIL_OBSERVED} FAIL/TIMEOUT/ISOLATION/LEAK lines on serial -- trusting per-binary count"
        UTEST_FAIL=$UTEST_FAIL_OBSERVED
    fi

    # --- Report dimension (ring-3 self-reported assertions + skip blocks) ---
    #
    # Parsed from its OWN tagged lines, never from the summary above: that
    # line is position-parsed by the sed expressions at the top of this
    # block AND is the string the boot-completion poll waits for, so
    # widening it would let the greedy patterns capture assertion counts as
    # binary counts. The launcher emits one `[UTEST-REPORT] <name> ...`
    # line per reporting binary plus a single `[UTEST-REPORT-SUMMARY]`.
    #
    # Fail-closed, the same stance as the failure recount above: if a
    # binary's report was rejected as self-contradicting, or the summary
    # under-reports what the per-binary lines show, the run FAILS. A
    # partially-skipped binary that reports honestly is not a failure --
    # an inconsistent reporting path is.
    UTEST_REPORT_SUM=$(grep -E 'UTEST: \[UTEST-REPORT-SUMMARY\] ' \
                       "$TEST_LOG" 2>/dev/null | tail -1 || true)
    if [ -n "$UTEST_REPORT_SUM" ]; then
        RPT_BLOCKS=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* skip_blocks=([0-9]+).*/\1/')
        RPT_INVALID=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* invalid=([0-9]+).*/\1/')
        RPT_REPORTED=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* reported=([0-9]+).*/\1/')
        echo -e "  ${CYAN}[UTEST]${RESET} reported: ${RPT_REPORTED} binaries, ${RPT_BLOCKS} skip block(s), ${RPT_INVALID} invalid"

        # Cross-check: recount the per-binary report lines from serial. A
        # summary claiming fewer reporting binaries than the stream shows
        # means a launcher counter bug -- trust the stream.
        RPT_LINES=$({ grep -cE 'UTEST: \[UTEST-REPORT\] ' "$TEST_LOG" 2>/dev/null || true; } | head -1)
        RPT_LINES=${RPT_LINES:-0}
        RPT_INVALID_OBSERVED=$({ grep -cE 'UTEST: \[UTEST-REPORT\] .* state=INVALID' \
                                 "$TEST_LOG" 2>/dev/null || true; } | head -1)
        RPT_INVALID_OBSERVED=${RPT_INVALID_OBSERVED:-0}
        RPT_TOTAL_EXPECTED=$(( ${RPT_REPORTED:-0} + ${RPT_INVALID:-0} ))
        if [ "$RPT_LINES" -gt "$RPT_TOTAL_EXPECTED" ]; then
            echo -e "  ${RED}[UTEST]${RESET} report summary accounts for ${RPT_TOTAL_EXPECTED} binaries but ${RPT_LINES} [UTEST-REPORT] lines are on serial -- counting the difference as failures"
            UTEST_FAIL=$(( UTEST_FAIL + RPT_LINES - RPT_TOTAL_EXPECTED ))
        fi
        if [ "$RPT_INVALID_OBSERVED" -gt 0 ]; then
            echo -e "  ${RED}[UTEST]${RESET} ${RPT_INVALID_OBSERVED} binary/binaries submitted a self-contradicting report (the launcher already failed them)"
        fi
    fi

    # A summary line that could not be formatted is never published
    # truncated -- the launcher emits an explicit overflow marker instead.
    # Its presence means an artifact is missing its summary, which is an
    # observability failure on the reporting path itself.
    UTEST_OVERFLOW=$({ grep -cE 'UTEST: \[UTEST-(XML|JSON|REPORT)-SUMMARY-OVERFLOW\]' \
                       "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_OVERFLOW=${UTEST_OVERFLOW:-0}
    if [ "$UTEST_OVERFLOW" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_OVERFLOW} summary line(s) exceeded their buffer and were not emitted -- artifacts are incomplete"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_OVERFLOW ))
    fi

    # A suite ABORT must never exit green. The launcher aborts the run when
    # a smoke binary returns any non-PASS verdict -- including SKIP, which
    # increments the skipped counter and leaves the summary reporting ZERO
    # failures. Every non-smoke binary then never runs, and before
    # 2026-07-28 the host still exited 0 on that: a run where most of the
    # suite silently did not execute reported success. The abort itself is
    # the failure, whatever verdict triggered it.
    UTEST_ABORT=$({ grep -cE 'UTEST: (Bail out!|suite ABORT)' "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_ABORT=${UTEST_ABORT:-0}
    if [ "$UTEST_ABORT" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} suite ABORTED (smoke gate) -- the remaining binaries never ran; failing the run"
        # RAISE to one, never ADD: the smoke binary that triggered the
        # abort is usually already in the parsed failure count, and adding
        # an abort event on top would report two failed binaries when one
        # failed. The case this must still catch is the smoke binary that
        # SKIPPED -- zero failures counted, whole suite abandoned.
        [ "${UTEST_FAIL:-0}" -eq 0 ] && UTEST_FAIL=1
    fi

    # The launcher refuses to emit skip records past its run-wide budget
    # and says so explicitly. Records it knows about but did not emit mean
    # the artifacts under-report skips, which is the same false-coverage
    # class the reporting path exists to close.
    UTEST_BUDGET=$({ grep -cE 'UTEST: \[UTEST-SKIP-RECORD-BUDGET\]' "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_BUDGET=${UTEST_BUDGET:-0}
    if [ "$UTEST_BUDGET" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_BUDGET} binary/binaries exceeded the run-wide skip-record budget -- artifacts under-report skips"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_BUDGET ))
    fi

    # A per-record emit that did not fit its buffer is published as a
    # verdict-preserving fallback with the name dropped, so the artifact
    # stays well-formed -- but a record whose identity was lost is still
    # an artifact that cannot be traced back to its binary. Host-fatal.
    UTEST_RECORD_OVERFLOW=$({ grep -cE 'UTEST: \[UTEST-RECORD-OVERFLOW\]' "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_RECORD_OVERFLOW=${UTEST_RECORD_OVERFLOW:-0}
    if [ "$UTEST_RECORD_OVERFLOW" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_RECORD_OVERFLOW} artifact record(s) lost their name to a buffer overflow -- untraceable results"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_RECORD_OVERFLOW ))
    fi
fi

# --- Step 6: User-mode test JUnit XML post-processor ---
#
# The kernel-side launcher emits `[UTEST-XML] <testcase ...>` lines on
# serial when boot.conf `xml=1` is set. Those lines don't form a valid
# XML document on their own because the launcher doesn't know the final
# tests=/failures=/skipped= counts until the suite ends, so we filled
# the opening <testsuite> header with placeholder zeros and emitted a
# separate `[UTEST-XML-SUMMARY] tests=N failures=N ...` line at the
# end. This step grep-extracts both streams and assembles a valid JUnit
# XML file at build/test-results.xml for CI tools (GitLab
# artifacts.reports.junit, GitHub Actions actions/upload-artifact,
# Jenkins junit plugin).
#
# No-op when no `[UTEST-XML]` lines are present (xml= was not enabled).

XML_OUT="$PROJECT/build/test-results.xml"
HAS_XML=0
grep -q '\[UTEST-XML\]' "$TEST_LOG" 2>/dev/null && HAS_XML=1

# Belt-and-suspenders: XML=1 was requested but the kernel produced no
# [UTEST-XML] stream (test_usermode_run bailed before emitting, e.g.
# C:\\ not mounted). CI tools fail on missing artifacts, so generate
# an explicit zero-test <testsuite/> envelope. Normal empty-suite case
# (total_planned=0 with xml=1) is handled kernel-side and goes through
# the HAS_XML=1 branch below.
if [ "$XML_MODE" -eq 1 ] && [ "$HAS_XML" -eq 0 ]; then
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo '<testsuite name="impossible-os-usermode" tests="0" failures="0" skipped="0" errors="0" time="0"/>'
    } > "$XML_OUT"
    echo -e "${CYAN}[TEST]${RESET} JUnit XML written: $XML_OUT (empty -- no [UTEST-XML] stream on serial)"
fi

if [ "$HAS_XML" -eq 1 ]; then
    # Strip ANSI + klog wrapper prefix (timestamp, [cpu:N], level badge,
    # UTEST subsystem tag) in one sed pass. Everything after the first
    # `[UTEST-XML] ` or `[UTEST-XML-SUMMARY] ` marker is the payload.
    STRIPPED=$(sed -E 's/\x1b\[[0-9;]*m//g' "$TEST_LOG")

    # Extract the summary numbers (tests=N failures=N skipped=N time=S.MMM).
    SUM_LINE=$(echo "$STRIPPED" | sed -n 's/.*\[UTEST-XML-SUMMARY\] //p' |
               grep -E '^tests=[0-9]+ failures=[0-9]+ skipped=[0-9]+ time=' |
               tail -1 || true)

    if [ -n "$SUM_LINE" ]; then
        XML_TESTS=$(echo "$SUM_LINE" | sed -E 's/.*tests=([0-9]+).*/\1/')
        XML_FAIL=$(echo "$SUM_LINE" | sed -E 's/.*failures=([0-9]+).*/\1/')
        XML_SKIP=$(echo "$SUM_LINE" | sed -E 's/.*skipped=([0-9]+).*/\1/')
        XML_TIME=$(echo "$SUM_LINE" | sed -E 's/.*time=([0-9.]+).*/\1/')
    else
        XML_TESTS=0; XML_FAIL=0; XML_SKIP=0; XML_TIME=0
    fi

    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        echo "<testsuite name=\"impossible-os-usermode\" tests=\"${XML_TESTS}\" failures=\"${XML_FAIL}\" skipped=\"${XML_SKIP}\" errors=\"0\" time=\"${XML_TIME}\">"
        # Strip the klog prefix + `[UTEST-XML] ` marker. Keep only the
        # <testcase ...> / <testcase ...>...</testcase> payloads. Skip
        # the placeholder <testsuite ...> opener and the </testsuite>
        # closer emitted by the launcher: we already wrote fresh ones
        # with the real counts above.
        #
        # `|| true` keeps the pipeline exit code 0 when zero testcases
        # are present -- an empty suite is a valid state (filter
        # matched nothing, test=0, etc.) and the script must continue
        # to the final verdict + cleanup trap. Without this, set -e
        # would abort the entire test.sh run before the normal path.
        echo "$STRIPPED" | sed -n 's/.*\[UTEST-XML\] //p' |
            grep -E '^<testcase' || true
        echo '</testsuite>'
    } > "$XML_OUT"
    echo -e "${CYAN}[TEST]${RESET} JUnit XML written: $XML_OUT (tests=${XML_TESTS} failures=${XML_FAIL} skipped=${XML_SKIP})"
fi

# Final verdict -- kernel TEST and user-mode UTEST failures both gate exit.
# A user-mode binary regression (e.g. PE loader segfaulting at _start)
# would otherwise hide behind a green kernel summary; folding UTEST_FAIL
# into the gate makes `make test` and CI fail loudly when ANY tier breaks.
echo ""
TOTAL_FAIL=$((${FAILED:-0} + ${UTEST_FAIL:-0}))
TOTAL_PASS=$((${PASSED:-0} + ${UTEST_PASS:-0}))
if [ "$TOTAL_FAIL" = "0" ]; then
    if [ "${UTEST_PASS:-0}" -gt 0 ]; then
        echo -e "${GREEN}${BOLD}PASS: ${PASSED} kernel + ${UTEST_PASS} user-mode tests passed${RESET}"
    else
        echo -e "${GREEN}${BOLD}PASS: ${PASSED} tests passed${RESET}"
    fi
    # Update coverage report on success
    bash "$PROJECT/scripts/test-coverage.sh" --save --quiet
    # Content-bound suite receipt so a re-run over unchanged inputs is free
    # (rollover gate + targeted fix-loop verification read this). Records the
    # suite that ran (a filtered SUITE=, else "all"). Best-effort.
    python3 "$PROJECT/scripts/overnight/receipts.py" record-suite . "${SUITE:-all}" >/dev/null 2>&1 || true
    exit 0
else
    TOTAL=$((TOTAL_PASS + TOTAL_FAIL))
    if [ "${UTEST_FAIL:-0}" -gt 0 ]; then
        echo -e "${RED}${BOLD}FAIL: ${FAILED} kernel + ${UTEST_FAIL} user-mode of ${TOTAL} failed${RESET}"
    else
        echo -e "${RED}${BOLD}FAIL: ${FAILED} of ${TOTAL} failed${RESET}"
    fi
    exit 1
fi
