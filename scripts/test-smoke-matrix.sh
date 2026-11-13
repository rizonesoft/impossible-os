#!/usr/bin/env bash
# test-smoke-matrix.sh -- boot the image across the engine x CPU-count matrix.
#
# WHY THIS EXISTS. `test-smoke.sh` proves the image boots to userspace on ONE
# configuration. Until 2026-07-28 that configuration was KVM with no `-smp` flag
# at all, i.e. a single CPU -- in a project whose stated doctrine is "SMP-safe by
# default ... SMP from day one". A section that changed the TEB <-> kernel_gs_base
# handoff across context switches shipped green through it and then halted on the
# operator's 2-CPU WHPX boot with
#     [CRIT] sched: ring-3 task N thread 0 has TEB but kernel_gs_base=0
# The single-CPU run reached the identical point and survived.
#
# The two engines are NOT redundant, and CLAUDE.md says so: KVM catches real-CPU
# MSR traps and SMP timing; TCG catches "timing-sensitive races that KVM's speed
# hides" -- a SYS_EXEC frame-handoff bug survived 297 commits and a green local
# suite because KVM always landed a tick that TCG did not. Crossing both engines
# with both CPU counts is four boots and roughly half a minute, against a class
# of bug that has twice reached a shipped section.
#
# OUTPUT IS DELIBERATELY BOUNDED. Each leg's full log stays on disk; this prints
# one line per leg plus a verdict. Cache-read is ~77% of an overnight run's bill
# and per-turn context is the driver, so a matrix that dumped four boot logs into
# the conversation would cost more than the regressions it catches. Run it under
# scripts/overnight/run-artifact.sh for the JSON envelope.
#
# WHPX is NOT in this matrix: it is a Windows accelerator reached through
# scripts/machines/run-qemu.ps1, costs ~141s per boot against ~3-10s here, and
# belongs at the section boundary rather than the per-edit loop.
#
# Usage:
#   bash scripts/test-smoke-matrix.sh              # all four legs
#   SMOKE_MATRIX_LEGS="kvm:2 tcg:2" bash ...       # a subset
# Exit: 0 only if every leg passed.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

# ONE MATRIX PER WORKTREE (v19 capture, 2026-09-05). Two concurrent runs share
# build/smoke-matrix/ and the built image, and both reported "4/4 legs failed"
# with legs ending in 0-5 s -- indistinguishable from a real boot regression --
# while the same tree passed 4/4 alone. Serialise like scripts/test-tooling.sh:
# wait for the other run, refuse loudly only if it is still running afterwards.
_SM_LOCK="${TMPDIR:-/tmp}/impossible-os-smoke-matrix.$(printf '%s' "$REPO_ROOT" | cksum | cut -d' ' -f1).lock"
_SM_WAIT="${SMOKE_MATRIX_LOCK_TIMEOUT:-1200}"
if command -v flock >/dev/null 2>&1 && { exec 8>"$_SM_LOCK"; } 2>/dev/null; then
    if ! flock -n 8 2>/dev/null; then
        echo "test-smoke-matrix: another matrix is running in this worktree; waiting up to ${_SM_WAIT}s (lock: $_SM_LOCK)" >&2
        if ! flock -w "$_SM_WAIT" 8 2>/dev/null; then
            echo "test-smoke-matrix: REFUSING to start -- another matrix still holds the lock after ${_SM_WAIT}s; two runs in one worktree fail each other's legs." >&2
            exit 3
        fi
    fi
fi

GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[0;90m'; CYAN='\033[0;36m'; NC='\033[0m'

# engine:cpus. KVM first: it is the fastest, so a break that both engines see is
# reported in seconds rather than after the slow leg.
LEGS="${SMOKE_MATRIX_LEGS:-kvm:1 kvm:2 tcg:1 tcg:2}"

LOG_DIR="$REPO_ROOT/build/smoke-matrix"
mkdir -p "$LOG_DIR"

PASS=0; FAIL=0; FAILED_LEGS=""
CAPTURES=""
START_ALL=$(date +%s)

echo -e "${CYAN}=== smoke matrix: $LEGS ===${NC}"

# BUILD ONCE. Every leg used to run its own `build.sh clean` inside
# test-smoke.sh: 20 identical clean builds, 2720 s on 2026-11-10 for about
# 150 s of booting. The matrix now clean-builds here, keeps a copy of the image
# before any boot has written to it, and hands that copy to each leg
# (SMOKE_PRISTINE_IMAGE), which restores it instead of rebuilding. The copy
# lives outside build/ and is removed on exit. SMOKE_MATRIX_BUILD_EACH=1 brings
# the per-leg builds back.
_SM_PRISTINE=""
_sm_cleanup() { [ -n "$_SM_PRISTINE" ] && rm -f "$_SM_PRISTINE" "$_SM_PRISTINE.receipt"; }
trap _sm_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
if [ "${SMOKE_MATRIX_BUILD_EACH:-0}" != "1" ]; then
    t0=$(date +%s)
    bash scripts/build.sh clean >/dev/null 2>&1
    if [ "$(tail -1 build/build.log 2>/dev/null)" != "=== BUILD OK ===" ]; then
        echo -e "${RED}SMOKE MATRIX FAILED${NC}  (the build failed before any leg ran; see build/build.log)"
        exit 1
    fi
    _SM_PRISTINE="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-pristine.XXXXXX")"
    if ! cp --sparse=always build/system-disk.img "$_SM_PRISTINE" \
        || ! cp build/build-receipt.json "$_SM_PRISTINE.receipt"; then
        echo -e "${RED}SMOKE MATRIX FAILED${NC}  (could not keep a pristine copy of build/system-disk.img and its build receipt)"
        exit 1
    fi
    export SMOKE_PRISTINE_IMAGE="$_SM_PRISTINE"
    printf "  ${DIM}built once in %ds; every leg boots a restored copy of that image${NC}\n" "$(( $(date +%s) - t0 ))"
fi

for leg in $LEGS; do
    engine="${leg%%:*}"; cpus="${leg##*:}"
    log="$LOG_DIR/${engine}-${cpus}cpu.log"
    t0=$(date +%s)

    # Capture OUTSIDE build/, then move the log in afterwards.
    #
    # An earlier fix recreated $LOG_DIR before each leg, on the theory that the
    # build only deleted logs written by EARLIER legs. It does worse than that:
    # the redirect below opens the log before test-smoke.sh runs, test-smoke.sh
    # builds, and the build removes subdirectories under build/ -- unlinking the
    # file while the shell still holds the fd. The leg then writes to an inode
    # with no name, so the log is gone even for the leg that produced it, the
    # FAIL branch's `grep ... "$log"` reads nothing, and a failing configuration
    # reports no reason at all. Measured 2026-07-28: a full green matrix left
    # build/smoke-matrix/ EMPTY while printing four paths into it.
    #
    # A capture path outside build/ cannot be unlinked by the build, so the log
    # survives to be read by the FAIL branch below and to be moved into its
    # documented location for the operator.
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-${engine}-${cpus}cpu.XXXXXX")"

    # Each leg is a full test-smoke.sh run on the image built above: it
    # re-boots and re-asserts, which is the point -- the assertions are what
    # differ per configuration.
    # A 2-CPU leg is also the first boot of that engine's two-boot pair (below):
    # it records its boot-trend entry and ends with the power button (S5).
    trend=""; sd=0; [ "$cpus" = "2" ] && { trend=1; sd=1; }
    if [ "$engine" = "tcg" ]; then
        SMOKE_TREND_BOOTS="$trend" SMOKE_SHUTDOWN="$sd" SMOKE_FORCE_TCG=1 SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$cap" 2>&1
    else
        SMOKE_TREND_BOOTS="$trend" SMOKE_SHUTDOWN="$sd" SMOKE_SMP="$cpus" bash scripts/test-smoke.sh > "$cap" 2>&1
    fi
    rc=$?
    # Publish at the END of the run, not here: a LATER leg's build wipes
    # build/ subdirectories again, so a log moved into place now would be
    # deleted by the next leg. Only the final leg's log ever survived that
    # way (measured 2026-07-28: a green four-leg matrix left exactly one
    # file behind). Keep every capture on its unlinkable-by-build path and
    # move them all in once the last leg is done.
    CAPTURES="$CAPTURES $cap:$log"
    t1=$(date +%s); dur=$((t1 - t0))

    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS ${engine}:${cpus}cpu"
        printf "  ${RED}FAIL${NC}  %-8s %s cpu   %2ds   ${DIM}%s${NC}\n" "$engine" "$cpus" "$dur" "$log"
        # One reason line, not the log: the operator (or the artifact envelope)
        # has the path when they want the rest. Read the live CAPTURE, not the
        # published path -- publication happens after the last leg, so $log
        # does not exist yet while this leg is being reported.
        reason=$(grep -aoE 'Detected[^"]{0,90}|Log ends on a fatal shape[^"]{0,70}|SMOKE TEST FAILED[^"]{0,90}' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi

    # Second boot of the same machine, after the first powered off cleanly, run
    # now, before another leg replaces the disk and variables. Every other leg boots a fresh disk, so state a boot
    # leaves on disk for the next was never booted: this one keeps the first
    # boot's disk and variables and must read back its boot-trend record.
    if [ "$cpus" = "2" ] && [ "$rc" = "0" ]; then
        name="$engine-2"; blog="$LOG_DIR/boots-$name.log"
        bcap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-boots-$name.XXXXXX")"
        settings="SMOKE_TREND_BOOTS=2 SMOKE_KEEP_DISK=1 SMOKE_SMP=2"
        [ "$engine" = "tcg" ] && settings="$settings SMOKE_FORCE_TCG=1"
        t0=$(date +%s)
        # shellcheck disable=SC2086  # word-split on purpose: one assignment per word
        env $settings bash scripts/test-smoke.sh > "$bcap" 2>&1
        brc=$?
        CAPTURES="$CAPTURES $bcap:$blog"
        dur=$(( $(date +%s) - t0 ))
        if [ "$brc" = "0" ]; then
            PASS=$((PASS + 1))
            printf "  ${GREEN}PASS${NC}  boots    %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$blog"
        else
            FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS boots:$name"
            printf "  ${RED}FAIL${NC}  boots    %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$blog"
            reason=$(grep -a 'SMOKE TEST FAILED' "$bcap" 2>/dev/null | head -1)
            [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
        fi
    fi
done

# Boot entry store fixtures, one boot each on the default engine. The plain legs
# boot an image with no store, so they never run the loader's parser. That is how
# a valid store being discarded on every boot stayed invisible until 2026-11-07.
# default and saved are one machine across two boots: the menu's d key saves the
# second entry, and the next boot picks it on its own.
# The last three are ONE machine across three boots, sharing a variable store: the
# rejection is recorded, a repeat writes nothing, and repairing the store clears it.
for leg in VALID:valid:SMOKE_VALID_STORE=1 \
           MENU:menu:SMOKE_MENU=1 \
           MENU:keys:SMOKE_MENU=keys \
           MENU:default:SMOKE_MENU=default \
           MENU:saved:SMOKE_MENU=saved \
           CORRUPT:recovered:SMOKE_CORRUPT_STORE=1:SMOKE_SHADOW_STORE=1 \
           CORRUPT:recovered-tmp:SMOKE_CORRUPT_STORE=1:SMOKE_SHADOW_STORE=tmp \
           VALID:empty-tmp:SMOKE_VALID_STORE=1:SMOKE_SHADOW_STORE=empty \
           CORRUPT:corrupt:SMOKE_CORRUPT_STORE=1 \
           CORRUPT:repeat:SMOKE_CORRUPT_STORE=1:SMOKE_STORE_RECORD=unchanged \
           VALID:repaired:SMOKE_VALID_STORE=1:SMOKE_STORE_RECORD=cleared; do
    name="${leg#*:}"; name="${name%%:*}"
    settings="${leg#*:*:}"
    log="$LOG_DIR/store-$name.log"
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-store-$name.XXXXXX")"
    t0=$(date +%s)
    # shellcheck disable=SC2086  # word-split on purpose: one assignment per colon
    env ${settings//:/ } bash scripts/test-smoke.sh > "$cap" 2>&1
    rc=$?
    CAPTURES="$CAPTURES $cap:$log"
    dur=$(( $(date +%s) - t0 ))
    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  store    %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS store:$name"
        printf "  ${RED}FAIL${NC}  store    %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
        reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

# Stale crash-region pointer fixtures, one boot each on the default engine.
# Every other leg boots a fresh variable store, so none of them sees the
# ImpossibleCrashLog pointer an update inherits from an older build; on
# 2026-11-13 one named the new heap guard page and Phase 0 faulted. `guard`
# plants THIS build's heap guard address, read from a leg above, and must be
# claimed with the heap placed elsewhere; `mmio` plants the IOAPIC window, in
# range for the old check, and must be skipped.
heap_guard="$(grep -ahoE 'guard at 0x[0-9a-f]+' "$LOG_DIR"/*.log 2>/dev/null | head -1 | awk '{print $3}')"
for leg in "guard|${heap_guard}|claimed" "mmio|0xFEC00000|skipped"; do
    name="${leg%%|*}"; rest="${leg#*|}"
    addr="${rest%%|*}"; expect="${rest#*|}"
    log="$LOG_DIR/crashptr-$name.log"
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-crashptr-$name.XXXXXX")"
    t0=$(date +%s)
    if [ -z "$addr" ]; then
        echo "SMOKE TEST FAILED: no 'guard at' line in the earlier legs' logs" > "$cap"; rc=1
    else
        SMOKE_CRASH_NVRAM="$addr" SMOKE_CRASH_NVRAM_EXPECT="$expect" bash scripts/test-smoke.sh > "$cap" 2>&1
        rc=$?
    fi
    CAPTURES="$CAPTURES $cap:$log"
    dur=$(( $(date +%s) - t0 ))
    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  crashptr %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS crashptr:$name"
        printf "  ${RED}FAIL${NC}  crashptr %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
        reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

# Forced-degradation fixture (resilient-boot roadmap), one boot on the default engine.
# Every other leg boots a healthy machine, so a non-critical init that FAILS was
# never booted: this one makes rtc_init() fail for real and requires the boot to
# reach the prompt anyway with the failure named in the degraded summary.
for name in rtc fb; do
    log="$LOG_DIR/degrade-$name.log"
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-degrade-$name.XXXXXX")"
    t0=$(date +%s)
    SMOKE_DEGRADE="$name" bash scripts/test-smoke.sh > "$cap" 2>&1
    rc=$?
    CAPTURES="$CAPTURES $cap:$log"
    dur=$(( $(date +%s) - t0 ))
    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  degrade  %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS degrade:$name"
        printf "  ${RED}FAIL${NC}  degrade  %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
        reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

# Bad-strings crash fixture, one boot on the default engine: crash_test=3 panics
# with a description and file pointer that fault when read, and the leg passes
# only when the stop screen renders both placeholders and the crash dump lands.
# It is also the only boot that reaches the stop screen at all: every other leg
# is healthy, which is how a panic that hung under the boot splash shipped.
log="$LOG_DIR/crash-bad-strings.log"
cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-crash.XXXXXX")"
t0=$(date +%s)
SMOKE_CRASH_TEST=bad-strings bash scripts/test-smoke.sh > "$cap" 2>&1
rc=$?
CAPTURES="$CAPTURES $cap:$log"
dur=$(( $(date +%s) - t0 ))
if [ "$rc" = "0" ]; then
    PASS=$((PASS + 1))
    printf "  ${GREEN}PASS${NC}  crash    %-8s %2ds   ${DIM}%s${NC}\n" "strings" "$dur" "$log"
else
    FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS crash:strings"
    printf "  ${RED}FAIL${NC}  crash    %-8s %2ds   ${DIM}%s${NC}\n" "strings" "$dur" "$log"
    reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
    [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
fi

# Double-panic fixtures, one 2-CPU boot each on the default engine: two CPUs
# panic in one boot in a forced ordering, the guest is warm-reset, and the leg
# passes only when both CPUs' reports, the one park line, the winner's stop
# code on screen and the next boot's evidence record all name the same winner
# (scripts/smoke-double-panic.py). These are the only legs that panic on more
# than one CPU and the only ones that read a crash record across a reset.
for mode in race ap split; do
    log="$LOG_DIR/crash-double-$mode.log"
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-double.XXXXXX")"
    t0=$(date +%s)
    SMOKE_CRASH_TEST="double-$mode" SMOKE_SMP=2 bash scripts/test-smoke.sh > "$cap" 2>&1
    rc=$?
    CAPTURES="$CAPTURES $cap:$log"
    dur=$(( $(date +%s) - t0 ))
    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  crash    %-8s %2ds   ${DIM}%s${NC}\n" "dbl-$mode" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS crash:double-$mode"
        printf "  ${RED}FAIL${NC}  crash    %-8s %2ds   ${DIM}%s${NC}\n" "dbl-$mode" "$dur" "$log"
        reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

# UKI-direct fixtures (UKI signed-payload sections), one boot each on the default engine.
# Every leg above boots the split-path loader, so none of them exercises the
# signed-payload copy, its kernel pin, or the two layers that refuse a disk
# override in a UKI cmdline. `token` is the literal form the pre-scan refuses;
# `key` only normalises to `module=`, so only the parser-level gate catches it.
for leg in "signed|signed|" "token|override|initrd=disk.img" "key|override|mod ule = disk.eif"; do
    name="${leg%%|*}"; rest="${leg#*|}"
    mode="${rest%%|*}"; token="${rest#*|}"
    log="$LOG_DIR/uki-$name.log"
    cap="$(mktemp "${TMPDIR:-/tmp}/smoke-matrix-uki-$name.XXXXXX")"
    t0=$(date +%s)
    if [ "$mode" = "override" ]; then
        SMOKE_UKI=override SMOKE_UKI_OVERRIDE="$token" bash scripts/test-smoke.sh > "$cap" 2>&1
    else
        SMOKE_UKI=signed bash scripts/test-smoke.sh > "$cap" 2>&1
    fi
    rc=$?
    CAPTURES="$CAPTURES $cap:$log"
    dur=$(( $(date +%s) - t0 ))
    if [ "$rc" = "0" ]; then
        PASS=$((PASS + 1))
        printf "  ${GREEN}PASS${NC}  uki      %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
    else
        FAIL=$((FAIL + 1)); FAILED_LEGS="$FAILED_LEGS uki:$name"
        printf "  ${RED}FAIL${NC}  uki      %-8s %2ds   ${DIM}%s${NC}\n" "$name" "$dur" "$log"
        reason=$(grep -a 'SMOKE TEST FAILED' "$cap" 2>/dev/null | head -1)
        [ -n "$reason" ] && printf "        ${DIM}%s${NC}\n" "$reason"
    fi
done

TOTAL=$(( $(date +%s) - START_ALL ))

# Publish every capture now that no further build can wipe the directory.
# Recreated here because the last leg's build removed it again.
mkdir -p "$LOG_DIR"
for pair in $CAPTURES; do
    mv -f "${pair%%:*}" "${pair##*:}" 2>/dev/null || true
done

echo ""
if [ "$FAIL" = "0" ]; then
    echo -e "${GREEN}SMOKE MATRIX PASSED${NC}  ($PASS/$((PASS+FAIL)) legs, ${TOTAL}s)"
    exit 0
fi
echo -e "${RED}SMOKE MATRIX FAILED${NC}  ($FAIL/$((PASS+FAIL)) legs failed:${FAILED_LEGS} )"
echo -e "  ${DIM}A leg that fails on ONE configuration is the finding, not noise --${NC}"
echo -e "  ${DIM}the engine/CPU difference IS the evidence (CLAUDE.md).${NC}"
exit 1
