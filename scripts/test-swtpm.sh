#!/usr/bin/env bash
# ============================================================================
# test-swtpm.sh -- QEMU + swtpm (emulated TPM 2.0) boot validation path
#
# Boots the OS in QEMU with a software TPM 2.0 (swtpm) attached via a TPM-CRB (or
# TPM-TIS) device and runs the kernel SUITE=security test runner against it. Two
# things must hold for a pass: the security suite reports all tests green, AND
# the kernel's REAL TPM transport (CRB/TIS) initialized against swtpm during boot
# -- the live-path coverage the fake-transport unit tests cannot give.
#
# This is the live-validation entry point referenced by TODO-13 "TPM Tests and
# Event-Log Fixtures". The fake-TIS / fake-CRB / event-log / replay unit suites
# also run under `bash scripts/test.sh SUITE=security` without a TPM backend;
# this script additionally exercises the real transport against an actual TPM 2.0.
#
# Usage:  bash scripts/test-swtpm.sh
#
# Exit codes:
#   0 = PASS (boot completed; TPM transport came up; tests green) OR swtpm absent
#       (cleanly skipped -- swtpm is an optional host dependency)
#   1 = FAIL (panic, timeout, or the TPM path did not come up)
# ============================================================================

set -euo pipefail

case "${1:-}" in
    -h|--help)
        cat <<'EOF'
test-swtpm.sh -- QEMU + swtpm (emulated TPM 2.0) boot validation

Usage:
  bash scripts/test-swtpm.sh            Build + boot with swtpm-backed TPM-CRB

Behavior:
  Builds the OS, patches boot.conf for the kernel SUITE=security test runner,
  starts a swtpm 2.0 instance on a unix socket, boots build/system-disk.img
  headless in QEMU with a tpm-crb device bound to it (KVM if /dev/kvm is
  writable, TCG fallback), and asserts BOTH that the security suite reports all
  tests passed AND that the real TPM transport initialized. boot.conf is reset
  on exit. When swtpm is not installed the script prints an install hint and
  exits 0 (skipped) -- it is an optional host dependency.

Environment:
  TIMEOUT_SEC    QEMU wait budget in seconds (default: 45).
  TPM_IFACE      crb (default) or tis -- which TPM device QEMU presents.

Exit codes:
  0 = PASS or skipped (swtpm absent)
  1 = FAIL (panic, timeout, or TPM transport did not come up)

Artifacts:
  build/swtpm-test.log            raw serial capture
  build/swtpm-test.stripped.log   ANSI-stripped serial capture
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
OVMF_VARS_CP="$BUILD/OVMF_VARS_4M.swtpm.fd"
SERIAL_LOG="$BUILD/swtpm-test.log"
STRIPPED_LOG="$BUILD/swtpm-test.stripped.log"
TPM_STATE_DIR="$BUILD/swtpm-state"
TPM_SOCK="$BUILD/swtpm-sock"
TIMEOUT_SEC="${TIMEOUT_SEC:-45}"
TPM_IFACE="${TPM_IFACE:-crb}"

RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; CYAN=$'\033[0;36m'; DIM=$'\033[2m'; NC=$'\033[0m'

# ---- swtpm presence gate (optional host dependency) ----
if ! command -v swtpm >/dev/null 2>&1; then
    echo -e "${CYAN}[swtpm]${NC} swtpm not installed -- skipping the live TPM path."
    echo -e "${DIM}  Install: apt-get install swtpm swtpm-tools  (Debian/Ubuntu)${NC}"
    echo -e "${DIM}  The kernel TPM unit suites still run via: bash scripts/test.sh SUITE=security${NC}"
    exit 0
fi
if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo -e "${RED}swtpm test: qemu-system-x86_64 not found${NC}"
    exit 1
fi

# ---- Step 1: Build + patch boot.conf for the security suite ----
echo -e "${CYAN}[1/3]${NC} Building + arming the security suite..."
bash "$SCRIPT_DIR/build.sh" >/dev/null 2>&1 || { echo -e "${RED}build failed${NC}"; exit 1; }
[ -f "$DISK" ] || { echo -e "${RED}$DISK not found${NC}"; exit 1; }
[ -f "$OVMF_CODE" ] || { echo -e "${RED}OVMF not found at $OVMF_CODE${NC}"; exit 1; }
cp "$OVMF_VARS_SRC" "$OVMF_VARS_CP"

# Arm the kernel test runner (SUITE=security) the same way scripts/test.sh does,
# so this boot ACTUALLY runs + verifies the TPM suites against a real TPM 2.0
# rather than only checking that the transport logged once. The cleanup trap
# (installed below) resets boot.conf even on early failure.
SWTPM_PID=""
QEMU_PID=""
cleanup() {
    local ec=$?
    [ -n "${QEMU_PID:-}" ] && kill "$QEMU_PID" 2>/dev/null && wait "$QEMU_PID" 2>/dev/null || true
    [ -n "${SWTPM_PID:-}" ] && kill "$SWTPM_PID" 2>/dev/null && wait "$SWTPM_PID" 2>/dev/null || true
    rm -f "$TPM_SOCK" "$TPM_SOCK.ctrl"
    bash "$SCRIPT_DIR/patch-boot-conf.sh" reset >/dev/null 2>&1 || true
    exit "$ec"
}
trap cleanup EXIT INT TERM
bash "$SCRIPT_DIR/patch-boot-conf.sh" test 1 test_suite security >/dev/null

# ---- Step 2: Start swtpm ----
echo -e "${CYAN}[2/3]${NC} Starting swtpm (TPM 2.0)..."
rm -rf "$TPM_STATE_DIR" "$TPM_SOCK"
mkdir -p "$TPM_STATE_DIR"
swtpm socket \
    --tpmstate dir="$TPM_STATE_DIR" \
    --ctrl type=unixio,path="$TPM_SOCK.ctrl" \
    --server type=unixio,path="$TPM_SOCK" \
    --tpm2 \
    --flags startup-clear &
SWTPM_PID=$!

# Give swtpm a moment to create the socket.
for _ in $(seq 1 50); do [ -S "$TPM_SOCK" ] && break; sleep 0.1; done
[ -S "$TPM_SOCK" ] || { echo -e "${RED}swtpm socket never appeared${NC}"; exit 1; }

# ---- Step 3: Boot QEMU with the TPM device ----
echo -e "${CYAN}[3/3]${NC} Booting QEMU with tpm-${TPM_IFACE} (${TIMEOUT_SEC}s budget)..."
QEMU_FLAGS=(
    -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE"
    -drive "if=pflash,format=raw,file=$OVMF_VARS_CP"
    -drive "id=disk0,file=$DISK,format=raw,if=none"
    -device "ich9-ahci,id=ahci0"
    -device "ide-hd,drive=disk0,bus=ahci0.0"
    -chardev "socket,id=chrtpm,path=$TPM_SOCK"
    -tpmdev "emulator,id=tpm0,chardev=chrtpm"
    -device "tpm-${TPM_IFACE},tpmdev=tpm0"
    -m 2G
    -serial file:"$SERIAL_LOG"
    -no-reboot -no-shutdown -display none
)
if [ -c /dev/kvm ] && [ -w /dev/kvm ]; then
    QEMU_FLAGS+=(-enable-kvm -cpu host)
    echo -e "  ${DIM}KVM acceleration enabled${NC}"
fi

> "$SERIAL_LOG"
qemu-system-x86_64 "${QEMU_FLAGS[@]}" &
QEMU_PID=$!

PASSED=false
for _ in $(seq 1 "$TIMEOUT_SEC"); do
    sleep 1
    if [ -f "$SERIAL_LOG" ]; then
        sed 's/\x1b\[[0-9;]*m//g' "$SERIAL_LOG" > "$STRIPPED_LOG" 2>/dev/null || true
        if grep -qiE "panic|KeBugCheck|#PF|triple fault" "$STRIPPED_LOG"; then
            echo -e "${RED}SWTPM TEST FAILED: panic/fault on serial${NC}"
            tail -20 "$STRIPPED_LOG"; exit 1
        fi
        # A degraded/failed transport must NOT pass: the kernel logs these WARNs
        # when the real TPM never came up. Fail explicitly if any appear.
        if grep -qiE "startup probe (failed|skipped)|transport timed out|transport (not started|degraded)|start method .* unsupported|CRB (buffer map failed|control area)" "$STRIPPED_LOG"; then
            echo -e "${RED}SWTPM TEST FAILED: TPM transport did not come up (degraded marker on serial)${NC}"
            grep -iE "TPM" "$STRIPPED_LOG" | tail -8; exit 1
        fi
        # Require: the kernel test summary, the user-mode (UTEST) summary, AND the
        # kernel's exact transport-up marker. Then apply scripts/test.sh's gates
        # so this expensive live path cannot false-pass where the normal gate
        # fails: zero kernel FAILED, the `leaked` field present AND zero leaks,
        # and zero UTEST failures.
        # Framing is parsed by the CANONICAL parser (scripts/utest-frame.py),
        # the same one scripts/test.sh uses. This driver used to carry a hand
        # copy of the learn-and-reconcile logic, and the copy had already
        # drifted: it bound no terminator to its announcement, summed counts
        # boot-wide, and checked none of the abort/integrity markers the main
        # runner treats as fatal -- so an aborted suite with zero failed
        # binaries reached PASSED=true on the most expensive validation path
        # in the tree.
        UF_TPM=""
        FRAME_TPM_JSON=$(python3 "$PROJECT/scripts/utest-frame.py" "$STRIPPED_LOG" 2>/dev/null || true)
        if [ -n "$FRAME_TPM_JSON" ]; then
            UF_TPM=$(python3 -c '
import json, sys
try:
    d = json.loads(sys.argv[1] or "{}")
except Exception:
    d = {}
print(d.get("prefix") or "" if d.get("ok") else "")
' "$FRAME_TPM_JSON")
        fi
        # No printable sentinel when the frame is unknown or unreconciled:
        # ring 3 controls serial, so any placeholder it can print becomes an
        # acceptance prefix. An unusable frame simply never satisfies the gate
        # and the loop keeps waiting.
        if [ -n "$UF_TPM" ] && \
           grep -qE "=== [0-9]+ tests? passed" "$STRIPPED_LOG" && \
           grep -qE "${UF_TPM}=== [0-9]+ passed, [0-9]+ failed, [0-9]+ skipped of" "$STRIPPED_LOG" && \
           grep -qE "TPM2 transport up \\(" "$STRIPPED_LOG"; then
            # The main runner's integrity markers are fatal here too. An
            # aborted suite leaves most binaries unexecuted while reporting
            # zero failures, which is exactly the false-green this driver
            # existed to catch on real hardware.
            for marker in "suite ABORT" "Bail out!" "\\[UTEST-SKIP-RECORD-BUDGET\\]" \
                          "\\[UTEST-RECORD-OVERFLOW\\]" "-SUMMARY-OVERFLOW\\]"; do
                if grep -qE "${UF_TPM}.*${marker}" "$STRIPPED_LOG"; then
                    echo -e "${RED}SWTPM TEST FAILED: launcher reported an integrity marker (${marker}) -- the run is incomplete${NC}"
                    exit 1
                fi
            done
            SUM=$(grep -E "=== [0-9]+ tests? passed" "$STRIPPED_LOG" | tail -1)
            UT=$(grep -E "${UF_TPM}=== [0-9]+ passed, [0-9]+ failed" "$STRIPPED_LOG" | tail -1)
            if echo "$SUM" | grep -qE "[1-9][0-9]* FAILED"; then
                echo -e "${RED}SWTPM TEST FAILED: kernel suite reported failures${NC}"; echo "$SUM"; exit 1
            fi
            if ! echo "$SUM" | grep -qE "[0-9]+ leaked"; then
                echo -e "${RED}SWTPM TEST FAILED: malformed summary (no leaked counter)${NC}"; echo "$SUM"; exit 1
            fi
            if echo "$SUM" | grep -qE "[1-9][0-9]* leaked"; then
                echo -e "${RED}SWTPM TEST FAILED: resource leak reported${NC}"; echo "$SUM"; exit 1
            fi
            # Quota leak gate, fail-closed exactly like the heap gate above.
            # This driver applies the same gates as the main test driver, so it
            # must reject a nonzero quota-leaked too -- the kernel picks its
            # success-form summary from `failed` alone, so "0 failed, 0 leaked,
            # 2 quota-leaked" would otherwise pass every check here. Note the
            # heap regex above cannot match inside "quota-leaked" (it requires a
            # space before "leaked"), which is why this needs its own check.
            if ! echo "$SUM" | grep -qE "[0-9]+ quota-leaked"; then
                echo -e "${RED}SWTPM TEST FAILED: malformed summary (no quota-leaked counter)${NC}"; echo "$SUM"; exit 1
            fi
            if echo "$SUM" | grep -qE "[1-9][0-9]* quota-leaked"; then
                echo -e "${RED}SWTPM TEST FAILED: quota leak reported${NC}"; echo "$SUM"; exit 1
            fi
            if echo "$UT" | grep -qE ", [1-9][0-9]* failed"; then
                echo -e "${RED}SWTPM TEST FAILED: user-mode tests reported failures${NC}"; echo "$UT"; exit 1
            fi
            # Per-binary cross-check: any explicit UTEST failure verdict (uppercase
            # FAIL/TIMEOUT/ISOLATION/LEAK) must not slip past a clean summary count.
            if grep -E "${UF_TPM}" "$STRIPPED_LOG" | grep -qE "\b(FAIL|TIMEOUT|ISOLATION|LEAK)\b"; then
                echo -e "${RED}SWTPM TEST FAILED: a user-mode binary reported a failure verdict${NC}"
                grep -E "${UF_TPM}" "$STRIPPED_LOG" | grep -E "\b(FAIL|TIMEOUT|ISOLATION|LEAK)\b" | tail -5; exit 1
            fi
            # Record-count reconciliation already passed: the parser only
            # reports `ok` when a run's markers pair by ordinal AND its framed
            # lines equal the terminator's declared count + 1.
            PASSED=true; break
        fi
    fi
    kill -0 "$QEMU_PID" 2>/dev/null || break
done

if $PASSED; then
    echo -e "${GREEN}SWTPM TEST PASSED${NC} -- security suite green + TPM 2.0 transport up against swtpm."
    exit 0
fi
echo -e "${RED}SWTPM TEST FAILED${NC} -- no security-suite-pass + TPM-transport markers within ${TIMEOUT_SEC}s."
[ -f "$STRIPPED_LOG" ] && { echo -e "${DIM}Last 20 serial lines:${NC}"; tail -20 "$STRIPPED_LOG"; }
exit 1
