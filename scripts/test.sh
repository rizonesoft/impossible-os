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
  bash scripts/test.sh UTEST_FILTER=test_harness_*  Glob-filter the user-mode binaries
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
# Glob filter over the user-mode BINARIES (boot.conf `utest_filter`), which
# is a different axis from SUITE= (kernel test categories). It had no runner
# knob either, so the empty-suite artifact contract -- a run selecting zero
# binaries must still publish a parseable envelope rather than none -- could
# not be exercised without hand-editing boot.conf. It is the only way to
# reach that path on demand, so the contract needs it to be testable.
UTEST_FILTER=""

# Per-suite [COUNT] trace, off unless asked for: it costs ~7s of extra serial
# on a ~4s suite, which is why no ordinary run pays it.
COUNT_TRACE=0

# Guest RAM for the test VM. 2G is the value every run used when this was a
# literal, and it stays the default so nothing about an ordinary run changes.
#
# It is a knob because the kernel assertion total has been observed drifting on
# an unchanged tree, and the suspected mechanism is host state rather than code
# -- the same runs carried allocator-exhaustion messages, and the firmware
# memory map itself varies boot to boot (133 vs 135 descriptors on two runs of
# one tree). Waiting for that variance to recur is weak evidence; VARYING the
# guest's memory forces it, so a suite whose assertion count is a function of
# available memory can be found deliberately. scripts/test-count-stability.sh
# --mem is the consumer.
#
# Validated rather than interpolated raw: this value lands in the QEMU argv, so
# an unchecked string is an argument-injection seam on a script that several
# harnesses invoke.
TEST_MEM="${TEST_MEM:-2G}"
case "$TEST_MEM" in
    [1-9]*[MG]) ;;
    *) echo "TEST_MEM must look like 512M or 2G, got '$TEST_MEM'" >&2; exit 2 ;;
esac
case "$TEST_MEM" in
    *[!0-9MG]*) echo "TEST_MEM must look like 512M or 2G, got '$TEST_MEM'" >&2; exit 2 ;;
esac

for arg in "$@"; do
    case "$arg" in
        -h|--help) print_help; exit 0 ;;
        SUITE=*)  SUITE_FILTER="${arg#SUITE=}"
                  SUITE_CATEGORY="$SUITE_FILTER" ;;
        QUIET=1)  QUIET_MODE=1 ;;
        QUIET=0)  QUIET_MODE=0 ;;
        COUNT_TRACE=1) COUNT_TRACE=1 ;;
        COUNT_TRACE=0) COUNT_TRACE=0 ;;
        XML=1)    XML_MODE=1 ;;
        JSON=1)   JSON_MODE=1 ;;
        TAP=1)    TAP_MODE=1 ;;
        UTEST_FILTER=*) UTEST_FILTER="${arg#UTEST_FILTER=}" ;;
    esac
done

# --- Step 0a: Artifact lifecycle, armed before ANY exit ---
#
# Until now both machine artifacts described a run that nothing could place:
# no timestamp, no commit, no environment identity, and one fixed output path
# per artifact that the next configuration's run silently overwrote. The same
# commit is deliberately tested on several configurations, and this repo's
# doctrine treats a failure on ONE of them as the finding rather than noise --
# which is only actionable if two runs' artifacts can be told apart.
#
# This half runs FIRST, ahead of the environment preflight below, because the
# preflight can exit. Stale-artifact removal used to live inside the harvest
# step alone, so every earlier exit -- a failed build, an absent CI-parity
# QEMU, a bad argument -- left the PREVIOUS run's green artifact at the
# canonical path, and CI that uploads artifacts regardless of exit code
# attributes it to this run. Nothing below this line can exit without leaving
# either a document belonging to THIS run or no document at all.

mkdir -p "$PROJECT/build"

# Concurrent same-tree runs are NOT a supported mode, and the refusal is
# EXPLICIT rather than emergent.
#
# This script serializes by construction and always did: it patches the shared
# `boot.conf` (Step 2), boots with a fixed `build/test.log` serial sink
# (Step 3), and copies OVMF vars to one fixed path. Two overlapping runs
# therefore corrupt each other's BOOT state long before their artifacts
# collide, so per-run artifact records below make concurrent runs
# non-colliding without making them meaningful. Supporting them for real means
# per-invocation boot.conf, serial log and OVMF paths -- a much larger change
# than the artifacts, and one nothing has asked for. Until then the honest
# behaviour is to refuse a second run rather than let it silently interleave.
#
# The lock is taken BEFORE the artifact lifecycle is armed below, because a
# refusing run must not touch a single shared name: acquiring after the
# `rm -f` would delete the artifacts of the run it is about to defer to.
# A refusing run therefore publishes NOTHING -- it owes no document, having
# never been the run any consumer was told to expect.
UTEST_LOCK="$PROJECT/build/.test-run.lock"
UTEST_LOCK_FD=9
if command -v flock >/dev/null 2>&1; then
    eval "exec ${UTEST_LOCK_FD}>\"\$UTEST_LOCK\""
    if ! flock -n "$UTEST_LOCK_FD"; then
        echo -e "${RED}[TEST]${RESET} another run of this script holds $UTEST_LOCK."
        echo -e "${YELLOW}       Concurrent same-tree runs are not supported: this script patches the${RESET}"
        echo -e "${YELLOW}       shared boot.conf, writes a fixed build/test.log, and copies OVMF vars${RESET}"
        echo -e "${YELLOW}       to a fixed path, so two runs corrupt each other's boot state. Wait for${RESET}"
        echo -e "${YELLOW}       the other run, or use a separate checkout.${RESET}"
        exit 1
    fi
else
    # Not fatal: flock is util-linux and present on every supported host, but a
    # missing lock tool must not stop a lone run that would have been fine.
    # Say so rather than pretending the refusal is enforced.
    UTEST_LOCK_FD=""
    echo -e "${YELLOW}[TEST]${RESET} flock unavailable -- the concurrent-run refusal is NOT enforced this run."
fi

# --- Orphaned-QEMU recovery ---
#
# The lock above cannot see the hazard it looks like it should. QEMU is launched
# with the lock descriptor closed, so an orphan left by a SIGKILLed wrapper holds
# no lock to contend for -- it just keeps writing to the shared boot state this
# run is about to reuse.
#
# This runs HERE, immediately after the lock and before ANYTHING shared is
# touched, because every later placement is already too late. Below this line the
# script clears the canonical aliases, prunes older run records (which could
# delete the record of the very run that leaked the VM), and REBUILDS
# system-disk.img -- and that image is open in the orphan as a writable AHCI
# drive. A refusal taken at the boot.conf patch would arrive after the disk was
# already rewritten underneath a live VM.
#
# A refusing run touches no shared name at all, exactly like the lock refusal: no
# record directory exists yet, no alias has been cleared, and no artifact trap is
# armed, so it owes no document to anyone.
UTEST_QEMU_PIDFILE="$BUILD_DIR/.test-qemu.pid"

# Ownership is proven by an open descriptor, never by process name: QEMU_BIN is
# caller-supplied and need not contain "qemu". qemu-orphan.py holds that logic
# (and the reap-time identity re-check that keeps a recycled pid from being
# signalled); this function owns the POLICY -- refuse by default, reap only when
# asked, degrade loudly if the check itself is unavailable.
utest_orphan_guard() {
    local rc=0 out="" pid start bid pcomm held stale="" failed=0
    local p args=()
    for p in "$@"; do
        args+=(--path "$p")
    done
    # Detector stderr is KEPT (merged, since stdout is the holder table only on
    # rc 3). Discarding it threw away the one diagnostic that explains a failed
    # scan, at exactly the moment the operator needs it.
    # The recorded pid is passed in so the detector can say INDETERMINATE
    # instead of clean when that one process is alive but hides its
    # descriptors -- the degraded case `pdeathsig.py --check` warns about.
    local rec0="" rec0s="" rec0b=""
    if [ -f "$UTEST_QEMU_PIDFILE" ]; then
        rec0="$(awk '{print $1}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
        rec0s="$(awk '{print $2}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
        rec0b="$(awk '{print $3}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
    fi
    case "$rec0" in ''|*[!0-9]*) rec0=0; rec0s=""; rec0b="" ;; esac
    out="$(python3 "$PROJECT/scripts/qemu-orphan.py" detect "${args[@]}" \
             --exclude-pid "$$" --recorded-pid "$rec0" \
             --recorded-starttime "$rec0s" --recorded-boot-id "$rec0b" 2>&1)" || rc=$?
    if [ "$rc" -eq 0 ]; then
        # Nothing holds this tree's files, so any pidfile left behind names a VM
        # that is already gone. Clearing it now keeps a later reap from aiming at
        # a pid this tree no longer owns.
        rm -f "$UTEST_QEMU_PIDFILE" 2>/dev/null || true
        return 0
    fi
    if [ "$rc" -ne 3 ]; then
        # FAIL CLOSED. This is deliberately NOT the missing-flock stance, and
        # the difference is what the two silences mean. A missing `flock` is a
        # STATIC property of the host -- the tool is absent, we know it, and the
        # hazard it guards (a second run this operator started) is one the
        # operator can see. A detector that RAN and errored is the opposite: it
        # produced no evidence either way, and the hazard it guards is invisible
        # by construction -- an orphan from a run that was SIGKILLed, holding a
        # system-disk.img this run is about to rewrite underneath it. Treating
        # "no evidence" as "no orphan" is precisely the corruption this guard
        # exists to prevent, so an unavailable check refuses the run.
        echo -e "${RED}[TEST]${RESET} orphan detection FAILED (qemu-orphan.py exit $rc) -- refusing the run."
        [ -n "$out" ] && echo -e "${YELLOW}       $out${RESET}"
        echo -e "${YELLOW}       A failed scan is not evidence that no VM holds this tree's boot state,${RESET}"
        echo -e "${YELLOW}       and continuing would rebuild system-disk.img underneath one if it does.${RESET}"
        echo -e "${YELLOW}       Fix the detector, or set UTEST_ORPHAN_UNCHECKED=1 to proceed unguarded.${RESET}"
        if [ "${UTEST_ORPHAN_UNCHECKED:-0}" = "1" ]; then
            echo -e "${YELLOW}[TEST]${RESET} UTEST_ORPHAN_UNCHECKED=1 -- proceeding with the guarantee ABSENT."
            return 0
        fi
        exit 1
    fi
    # Only quote the pidfile when it actually describes one of the processes just
    # detected. A pidfile left by a run whose VM is long gone names a run that
    # has nothing to do with this holder, and attributing the blockage to it
    # sends an operator after the wrong thing.
    if [ -f "$UTEST_QEMU_PIDFILE" ]; then
        stale="$(cat "$UTEST_QEMU_PIDFILE" 2>/dev/null || true)"
        pid="$(printf '%s' "$stale" | awk '{print $1}')"
        if [ -z "$pid" ] || ! printf '%s' "$out" | grep -q "^${pid}$(printf '\t')"; then
            stale=""
        fi
    fi
    if [ "${UTEST_ORPHAN_REAP:-0}" = "1" ]; then
        # AUTHORISED BY PROVENANCE, not merely by association. An open
        # descriptor proves a process is USING one of this tree's files; it says
        # nothing about where that process came from. A `tail -f build/test.log`,
        # a disk-image inspector, or an operator's own QEMU present exactly the
        # evidence a leaked VM does -- and reaping every holder would SIGKILL
        # them, unattended, which is the shape of damage this guard exists to
        # prevent rather than cause. Automatic reaping is therefore limited to
        # the holder THIS TREE RECORDED launching: pid, starttime and boot_id
        # must all match the pidfile. Anything else is named and refused, just
        # as it would be without the opt-in. (The identity re-check inside
        # qemu-orphan.py is a different guarantee -- it stops a RECYCLED pid
        # being signalled; it cannot tell whose process the pid belonged to.)
        local rec_pid="" rec_start="" rec_bid="" reaped_any=0
        if [ -f "$UTEST_QEMU_PIDFILE" ]; then
            rec_pid="$(awk '{print $1}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
            rec_start="$(awk '{print $2}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
            rec_bid="$(awk '{print $3}' "$UTEST_QEMU_PIDFILE" 2>/dev/null)"
        fi
        while IFS="$(printf '\t')" read -r pid start bid pcomm held; do
            [ -n "$pid" ] || continue
            if [ -z "$rec_pid" ] || [ "$pid" != "$rec_pid" ] \
               || [ "$start" != "$rec_start" ] || [ "$bid" != "$rec_bid" ]; then
                echo -e "${YELLOW}[TEST]${RESET} NOT reaping pid $pid ($pcomm) holding $held -- this tree has no record of launching it."
                failed=1
                continue
            fi
            echo -e "${YELLOW}[TEST]${RESET} reaping orphaned VM pid $pid ($pcomm) holding $held"
            if python3 "$PROJECT/scripts/qemu-orphan.py" reap --pid "$pid" \
                    "${args[@]}" --expect-starttime "$start" --expect-boot-id "$bid"; then
                reaped_any=1
                continue
            fi
            failed=1
        done <<< "$out"
        # `reaped_any` matters as much as `failed`: with no recorded holder to
        # reap, nothing was cleared, so falling through to the refusal is the
        # honest outcome rather than reporting a successful recovery.
        if [ "$reaped_any" -eq 1 ]; then
            # RESCAN after EVERY successful reap, including the common case
            # where the recorded orphan was the only holder. Returning straight
            # out on that path skips the only observation that covers the reap
            # WINDOW: a holder can appear, or inherit an owned descriptor, while
            # the reap is in progress, and the run would then go on to truncate
            # the serial log and rebuild system-disk.img underneath it. It also
            # left `$out` stale, so the refusal below would have listed the pid
            # just reaped as a live holder and sent an operator after a process
            # that no longer exists.
            rm -f "$UTEST_QEMU_PIDFILE" 2>/dev/null || true
            rc=0
            out="$(python3 "$PROJECT/scripts/qemu-orphan.py" detect "${args[@]}" \
                     --exclude-pid "$$" 2>&1)" || rc=$?
            if [ "$rc" -eq 0 ]; then
                echo -e "${GREEN}[TEST]${RESET} orphaned VM reaped -- continuing."
                return 0
            fi
            if [ "$rc" -ne 3 ]; then
                echo -e "${RED}[TEST]${RESET} orphan re-detection FAILED after the reap (exit $rc) -- refusing the run."
                [ -n "$out" ] && echo -e "${YELLOW}       $out${RESET}"
                exit 1
            fi
            stale=""
        fi
        echo -e "${RED}[TEST]${RESET} an orphaned VM could not be reaped."
    fi
    echo -e "${RED}[TEST]${RESET} a live process still holds this tree's boot state:"
    while IFS="$(printf '\t')" read -r pid start bid pcomm held; do
        [ -n "$pid" ] || continue
        echo -e "${YELLOW}       pid $pid ($pcomm) holds $held${RESET}"
    done <<< "$out"
    if [ -n "$stale" ]; then
        echo -e "${YELLOW}       recorded by: $stale (pid starttime boot_id run_id)${RESET}"
    fi
    echo -e "${YELLOW}       This run would patch boot.conf, truncate the serial log and rebuild${RESET}"
    echo -e "${YELLOW}       system-disk.img underneath it. Refusing instead of interleaving.${RESET}"
    echo -e "${YELLOW}       Clear it, then re-run -- or set UTEST_ORPHAN_REAP=1 to reap it here.${RESET}"
    exit 1
}
utest_orphan_guard "$OVMF_VARS_CP" "$TEST_LOG" "$DISK"

# --- Per-run artifact record ---
#
# The RECORD is a per-invocation directory; the stable pathnames below are
# ALIASES into it. Until now the leg-suffixed pathname WAS the record, so the
# next run of the same configuration rewrote it, and an invocation that exited
# during the environment preflight -- before its leg was even derivable --
# left an EARLIER run's document in place under a stable name.
#
# RUN_ID is derived here, ahead of every exit path, so even the earliest
# refusal lands in a directory belonging to THIS run. It is created with a
# non-recursive `mkdir`, which FAILS if the directory exists: that is what
# makes the identity collision-proof rather than merely improbable.
RUN_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
# The random suffix is best-effort; the pid plus the exclusive `mkdir` below
# are what make the identity sound, so a host without /dev/urandom degrades to
# a still-unique name rather than failing the run under `set -e`.
RUN_RAND="$(od -An -N4 -tx1 /dev/urandom 2>/dev/null | tr -cd 'a-f0-9' || true)"
[ -n "$RUN_RAND" ] || RUN_RAND="norand"
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)-$$-${RUN_RAND}"
RUN_ID="$(printf '%s' "$RUN_ID" | tr -cd 'A-Za-z0-9._-')"
RUNS_DIR="$PROJECT/build/test-runs"
mkdir -p "$RUNS_DIR"
RECORD_DIR="$RUNS_DIR/$RUN_ID"
if ! mkdir "$RECORD_DIR" 2>/dev/null; then
    echo -e "${RED}[TEST]${RESET} run record $RECORD_DIR already exists -- refusing to overwrite a prior run."
    exit 1
fi
# The reader-lease namespace is created HERE, with the record, and NEVER by a
# reader. An acquirer that `mkdir -p`ed its way in could resurrect an
# already-pruned record as a leases-only skeleton -- and retention counts
# DIRECTORIES (`find ... -type d` in utest_prune_records), so that skeleton
# would take a slot in the newest-KEEP set and push a complete record past the
# cutoff. An acquirer therefore reads an absent `leases/` as "this record is
# gone" and refuses, which is both true and the safe direction to be wrong in.
mkdir -p "$RECORD_DIR/leases" 2>/dev/null || true

# Durable does not mean unbounded: every invocation leaves a directory, so
# without a retention bound `build/` grows for as long as anyone runs tests.
# The newest UTEST_RECORD_KEEP records survive; older ones are pruned at the
# START of a run, so the records a failure investigation wants are never
# removed by the run that is still writing them. RUN_ID leads with a compact
# UTC timestamp, so lexicographic order IS chronological.
#
# The value is normalised in BASE 10 and bounded. `UTEST_RECORD_KEEP=08`
# passes a digits-only check and passes `-ge`, then aborts the whole run in
# `$(( ))`, where bash reads a leading zero as octal and 08 is not octal --
# and this runs under `set -e`. The call site below is deliberately AFTER the
# canonical aliases are cleared and the EXIT trap is armed, so even an abort
# in here cannot leave a previous run's documents sitting under the names that
# claim to describe this one.
# Normalise one numeric retention bound: $1 raw, $2 fallback for anything that
# is not digits-only, $3 ceiling. Echoes the value to use.
#
# The ORDER of the three steps is the whole content of this function.
#
# Leading zeros are stripped FIRST, because both later steps mis-read them.
# `$(( ))` reads `08` as octal and ABORTS the run under `set -e` (8 is not an
# octal digit), and a length-based magnitude test reads `000005` as a six-digit
# number and clamps a request for 5 up to the ceiling -- silently changing a
# documented knob, and turning `0000000` into the ceiling rather than the
# supported zero opt-out.
#
# The LENGTH clamp comes second, before any arithmetic, because bash evaluates
# `$(( 10#9223372036854775808 ))` to a NEGATIVE number and a negative sails
# through a `-le` ceiling test -- so a range check placed after the conversion
# is validating a value that already wrapped. With leading zeros gone, more
# than five digits means above 10000 by construction, so the clamp is exact.
utest_norm_bound() {
    local v="$1"
    case "$v" in
        ''|*[!0-9]*) printf '%s\n' "$2"; return 0 ;;
    esac
    # The RAW length is bounded before the strip loop, because the loop is
    # quadratic: `${v#0}` copies the remaining string every iteration, so a
    # caller-supplied `UTEST_RECORD_KEEP` of ten thousand zeros costs about a
    # second and a half -- spent holding the exclusive run lock, which is the
    # opposite of what a function that exists to bound hostile numeric input
    # should do. No legitimate value needs more than five significant digits,
    # so anything past 32 characters is not a padded number, it is an attack or
    # a typo, and takes the fallback.
    [ "${#v}" -le 32 ] || { printf '%s\n' "$2"; return 0; }
    while :; do
        case "$v" in
            0?*) v="${v#0}" ;;
            *)   break ;;
        esac
    done
    case "$v" in
        ??????*) printf '%s\n' "$3"; return 0 ;;
    esac
    v=$(( v ))
    [ "$v" -le "$3" ] 2>/dev/null || v="$3"
    printf '%s\n' "$v"
    return 0
}

# Extract the run id a per-leg pointer names, or NOTHING if it names no record
# this script could ever have written.
#
# The charset filter alone is not containment. It admits `.` and `..`, and
# `$RUNS_DIR/..` is a directory that exists -- so a hand-written pointer
# carrying `"run_id": ".."` would have been read as LIVE by the sweep below and
# kept forever, while a resolver deriving its path from that value would climb
# straight out of test-runs/. Every id this script generates begins with a UTC
# timestamp digit, so rejecting a leading dot rejects `.`, `..` and every
# hidden name in one test without excluding anything real.
#
# The read is TIME-BOUNDED. The caller checks the entry's type before opening
# it, but a check and an open are two syscalls: something could replace a
# regular file with a FIFO in between and the open would block forever, at
# startup, wedging every later invocation. Bash cannot open with
# `O_NONBLOCK|O_NOFOLLOW`, so it cannot make that pair atomic -- but it can
# refuse to wait, which turns a permanent wedge into a few seconds and a
# pointer treated as unreadable (and therefore swept).
#
# The bound stays load-bearing after section 46: the DESTRUCTIVE half of that
# race is closed (nothing is unlinked that was not itself detached and
# re-verified), but a replacement arriving while this parse is in flight is
# still parsed as its predecessor. The cost of losing that race is seconds and
# a sweep on the next pass, which is what the timeout guarantees.
utest_pointer_run_id() {
    local id
    id="$(timeout 5 sed -n 's/.*"run_id"[[:space:]]*:[[:space:]]*"\([A-Za-z0-9._-]\{1,\}\)".*/\1/p' \
          "$1" 2>/dev/null | head -1)"
    case "$id" in
        ''|.*) return 0 ;;
    esac
    printf '%s\n' "$id"
    return 0
}

# Sweep verdict for one leg pointer: 0 = destroy it, 1 = leave it alone.
#
# $1 = the path to READ, $2 = the basename it was published under, unused. A
# pointer's leg IS part of the contract, but only for the PIN, which is a
# separate and deliberately stricter question (see utest_pointer_pin_id): the
# sweep asks merely "does this name a record that still exists", because
# deleting a pointer for not matching this script's spelling destroys something
# a human may have put there. utest_sweep_verified passes the basename to every
# verdict uniformly, and this one has no use for it.
#
# Same code runs for selection and for the binding recheck on the detached
# object, so the two cannot drift into disagreeing about what is dangling.
utest_ptr_sweepable() {
    local path="$1" id
    if [ -L "$path" ] || [ ! -f "$path" ]; then return 0; fi
    id="$(utest_pointer_run_id "$path")"
    if [ -z "$id" ] || [ ! -d "$RUNS_DIR/$id" ]; then return 0; fi
    return 1
}

# Decide whether one pointer may PIN, and echo the run id if it may.
#
# Stricter than utest_pointer_run_id on purpose, because the two questions are
# different. The SWEEP asks "does this name a record that still exists" -- a
# weak test, and deliberately so: deleting a pointer merely because this script
# would not have written it that way destroys something a human may have put
# there. The PIN asks "should this hold a record against retention", and a
# budget spent on a pointer that pins nothing is a VALID pointer denied its
# pin, whose record then ages out and whose pointer the sweep afterwards
# removes as dangling. So the pin requires the WHOLE contract: every field at
# its exact value, the leg matching the filename it was found under, and a
# record that is present AND marker-committed as complete.
#
# The documents are read ONCE into variables and matched in-shell. A
# field-by-field `grep` would spawn a dozen processes per pointer on the
# startup path for no added strictness.
utest_pointer_pin_id() {
    local ptr="$1" body mbody id leg sz rest
    # BOUNDED, and bounded by SIZE FIRST. `timeout` bounds elapsed time, not
    # bytes: a huge regular file would be pulled wholly into a shell variable
    # before the clock ran out. And a byte-capped read alone is not enough
    # either -- `head -c` caps what is read but proves nothing about what
    # follows, while command substitution strips trailing newlines, so a file
    # holding the canonical document, newline padding out to the cap, and then
    # arbitrary contradictory bytes captures IDENTICALLY to the real thing.
    # Establishing the size first means the snapshot below is the whole file.
    # `-- "$ptr"`, not `< "$ptr"`: a redirection is performed by THIS shell
    # before `timeout` is exec'd, so the open it was meant to bound happens
    # outside it and a FIFO swapped in after the caller's type test blocks
    # forever. Letting `wc` open the pathname puts the open under the timeout.
    # (Section 45 made this load-bearing: utest_lease_acquire calls this while
    # holding the retention mutex, so an unbounded block here wedges retention
    # for every later run rather than costing one run five seconds.)
    sz="$(timeout 5 wc -c -- "$ptr" 2>/dev/null | awk '{print $1; exit}')" || return 0
    case "$sz" in ''|*[!0-9]*) return 0 ;; esac
    [ "$sz" -le 4096 ] || return 0
    body="$(timeout 5 head -c 4096 -- "$ptr" 2>/dev/null)" || return 0
    [ -n "$body" ] || return 0
    # From the SNAPSHOT, never by re-opening the file -- re-reading would
    # validate one version and extract from another.
    case "$body" in *'"run_id": "'*) ;; *) return 0 ;; esac
    id="${body#*\"run_id\": \"}"
    id="${id%%\"*}"
    case "$id" in ''|.*|*[!A-Za-z0-9._-]*) return 0 ;; esac
    # The leg IS the filename, so a pointer copied under another leg's name --
    # the shape that would let one leg pin another's record -- fails here.
    leg="${ptr##*/test-results-}"
    leg="${leg%.run}"
    # WHOLE-DOCUMENT comparison against what this script would have written for
    # that leg and run id. Field-by-field substring tests were the wrong tool:
    # they accept a document carrying every expected snippet PLUS a duplicate
    # or contradictory extra field, and reject semantically identical JSON that
    # merely spaces differently -- and no JSON parser is available here, since
    # `scripts/test.sh` has to work on a host with no python3 (see the
    # `.no-python3.json` refusal path). Byte-identity is the right test for a
    # PIN specifically: a pointer this script did not write does not get to
    # hold a record against retention. It is NOT the right test for the sweep,
    # which is why the sweep keeps the weaker utest_pointer_run_id and leaves
    # such a pointer alone rather than deleting it.
    [ "$body" = "$(utest_leg_pointer_doc_for "$leg" "$id")" ] || return 0
    [ -d "$RUNS_DIR/$id" ] || return 0
    # The marker cannot be canonicalised the same way -- its qemu fields and
    # document names legitimately vary -- so it is field-tested, with the
    # negative check that a substring test needs: a document carrying BOTH
    # statuses must not read as complete.
    sz="$(timeout 5 wc -c -- "$RUNS_DIR/$id/record-complete.json" 2>/dev/null | awk '{print $1; exit}')" || return 0
    case "$sz" in ''|*[!0-9]*) return 0 ;; esac
    [ "$sz" -le 4096 ] || return 0
    mbody="$(timeout 5 head -c 4096 -- "$RUNS_DIR/$id/record-complete.json" 2>/dev/null)" || return 0
    case "$mbody" in *"\"schema\": \"utest-run-record-v1\""*) ;;  *) return 0 ;; esac
    case "$mbody" in *"\"run_id\": \"$id\""*) ;;                  *) return 0 ;; esac
    case "$mbody" in *"\"status\": \"complete\""*) ;;             *) return 0 ;; esac
    # EXACTLY ONE of each load-bearing key. A positive substring test alone
    # cannot see a contradictory duplicate: a marker carrying the required
    # `"status": "complete"` AND a later `"status":"incomplete"` satisfies
    # every check above, while a real JSON parser -- which is what the
    # documented resolver uses -- takes the LAST occurrence and rejects the
    # generation. Pinning something no consumer can resolve is exactly the
    # wasted-budget failure this validator exists to prevent. Counting
    # occurrences is whitespace-independent, where matching a second spelling
    # of the negative case would not be.
    for rest in schema run_id status; do
        case "${mbody#*\"$rest\"}" in *"\"$rest\""*) return 0 ;; esac
    done
    printf '%s\n' "$id"
    return 0
}

# --- Reader leases ---------------------------------------------------------
#
# The pin above protects the record a CURRENT pointer names. It cannot protect
# one a reader has ALREADY resolved: the instant that leg publishes again the
# previous target is unpinned, and a later prune may remove it while the reader
# is still walking up to its first `open`. A reader can be told to restart --
# that is what the bounded ENOENT loop in docs/testing/usermode-output-formats.md
# is for -- but a one-shot consumer (a CI step, a pipeline reading over a mount)
# has no restart to give. A lease is the grant that closes it.
#
# ORDERING is the entire mechanism, and the obvious construction does not work.
# Writing a lease and then re-checking the record only NARROWS the window:
# prune can classify a record as unleased, the reader can publish its lease and
# verify successfully, and prune can then remove the record it had already
# decided to remove. Nothing in that sequence establishes a happens-before, so
# it buys probability rather than correctness. One mutex does: prune holds it
# for its whole body, an acquirer holds it from resolving the pointer to having
# PUBLISHED and verified its lease, and the two therefore cannot interleave.
# Once the lease is visible under the lock, every later prune must take the same
# lock to run at all, so the acquirer can drop it before it opens anything --
# the critical section is microseconds and a prune never waits on a whole read.
#
# It is deliberately NOT the run lock at the top of this script. That one is
# `flock -n` and REFUSES the entire run on contention, because two runs corrupt
# each other's shared boot state; a consumer must never be able to fail somebody
# else's test run merely by reading its output.
UTEST_RETENTION_FD=8

# Take the retention mutex. 0 = held, 1 = timed out, 2 = no flock on this host.
#
# The three outcomes are distinguished because they mean different things. A
# TIMEOUT is a live reader holding a grant, so retention defers to the next run
# rather than deleting what it cannot see. A MISSING flock is a static property
# of the host, already announced where the run lock is taken; leases then still
# work cooperatively but the guarantee is downgraded to best-effort, and the
# documented ENOENT fallback stays load-bearing for consumers on such a host.
utest_retention_lock() {
    local lock="$PROJECT/build/.test-retention.lock" wait_s="${1:-10}"
    command -v flock >/dev/null 2>&1 || return 2
    mkdir -p "$PROJECT/build" 2>/dev/null || true
    # `>>`, never `>`: opening for truncation would zero the file out from under
    # a concurrent holder. Harmless while the file stays empty, and precisely
    # the kind of thing that stops being harmless the day it does not.
    eval "exec ${UTEST_RETENTION_FD:-8}>>\"\$lock\"" 2>/dev/null || return 1
    if ! flock -w "$wait_s" -x "${UTEST_RETENTION_FD:-8}" 2>/dev/null; then
        utest_retention_unlock
        return 1
    fi
    return 0
}

utest_retention_unlock() {
    eval "exec ${UTEST_RETENTION_FD:-8}>&-" 2>/dev/null || true
    return 0
}

# Ticks-since-boot at which pid $1 started, or NOTHING when that cannot be read.
#
# Parsed after the LAST ')' because field 2 of /proc/<pid>/stat is `comm`, which
# may itself contain spaces and parentheses -- splitting the whole line is the
# classic way to read the wrong field for a process named `(a b)`. This mirrors
# scripts/qemu-orphan.py:62-81, which learned it first; the two must agree,
# because they are answering the same question about the same triple.
utest_proc_starttime() {
    local raw
    # `$UTEST_PROC` rather than a hardcoded /proc, for the same reason
    # scripts/qemu-orphan.py carries a PROC constant: the liveness rules below
    # decide whether a record is deleted, and they are only testable against a
    # fixture procfs. Production never sets it.
    raw="$(timeout 5 cat "${UTEST_PROC:-/proc}/$1/stat" 2>/dev/null)" || return 0
    [ -n "$raw" ] || return 0
    case "$raw" in *')'*) ;; *) return 0 ;; esac
    # After `comm`, state is the first remaining field (field 3), so starttime
    # (field 22) is the 20th of the remainder.
    printf '%s' "${raw##*)}" | awk '{ if (NF >= 20 && $20 ~ /^[0-9]+$/) print $20 }'
}

# MEMOISED, because both of these are invariant for the life of the process and
# were being re-read once per lease examined. Retention runs at the start of
# every invocation, and each uncached read costs 4-5 forks: at the default
# UTEST_RECORD_KEEP=20 that is a measurable startup tax paid entirely inside the
# mutex that blocks the run. `$UTEST_PROC` is part of the cache key in effect,
# since a test that changes proc roots runs in its own process.
# Populate the two identity caches IN THE CALLING SHELL.
#
# This exists because caching inside utest_boot_id / utest_pid_ns_id does
# NOTHING on its own: every caller reads them as `$(utest_boot_id)`, and a
# command substitution is a subshell, so the assignment is made in a child and
# discarded -- both functions then re-read procfs on every single call, which is
# exactly the per-lease fork cost the cache was added to remove. A value can
# only be cached where it is READ from, so it is primed here from a plain
# command call and the subshells below inherit it.
#
# Keyed by proc root, so a test pointing UTEST_PROC at a fixture is never served
# a value primed from the real /proc.
utest_prime_ident() {
    if [ "${UTEST_IDENT_CACHE_ROOT:-}" = "${UTEST_PROC:-/proc}" ] \
       && [ -n "${UTEST_BOOT_ID_CACHE:-}" ] && [ -n "${UTEST_PID_NS_CACHE:-}" ]; then
        return 0
    fi
    UTEST_IDENT_CACHE_ROOT="${UTEST_PROC:-/proc}"
    UTEST_BOOT_ID_CACHE="$(timeout 5 cat "${UTEST_PROC:-/proc}/sys/kernel/random/boot_id" 2>/dev/null | tr -cd 'a-f0-9-' | head -c 64 || true)"
    UTEST_BOOT_ID_CACHE="${UTEST_BOOT_ID_CACHE:-none}"
    UTEST_PID_NS_CACHE="$(readlink "${UTEST_PROC:-/proc}/self/ns/pid" 2>/dev/null || true)"
    UTEST_PID_NS_CACHE="${UTEST_PID_NS_CACHE##*[}"
    UTEST_PID_NS_CACHE="${UTEST_PID_NS_CACHE%]}"
    case "$UTEST_PID_NS_CACHE" in ''|*[!0-9]*) UTEST_PID_NS_CACHE="none" ;; esac
    return 0
}

utest_boot_id() {
    if [ "${UTEST_IDENT_CACHE_ROOT:-}" != "${UTEST_PROC:-/proc}" ] || [ -z "${UTEST_BOOT_ID_CACHE:-}" ]; then
        utest_prime_ident
    fi
    [ "$UTEST_BOOT_ID_CACHE" = "none" ] || printf '%s\n' "$UTEST_BOOT_ID_CACHE"
}

# The inode number behind /proc/self/ns/pid ("pid:[4026531836]"), which is the
# only thing that makes a recorded pid comparable at all. Empty when it cannot
# be read, which the caller must treat as UNKNOWN rather than as a mismatch.
utest_pid_ns_id() {
    if [ "${UTEST_IDENT_CACHE_ROOT:-}" != "${UTEST_PROC:-/proc}" ] || [ -z "${UTEST_PID_NS_CACHE:-}" ]; then
        utest_prime_ident
    fi
    [ "$UTEST_PID_NS_CACHE" = "none" ] || printf '%s\n' "$UTEST_PID_NS_CACHE"
}

# LIVE, DEAD or UNKNOWN for a `<pid>.<starttime>.<boot_id>` holder triple.
#
# THREE states, and the asymmetry is the whole reason. A wrong LIVE verdict
# costs a delayed reclaim, bounded by the TTL. A wrong DEAD verdict DELETES a
# record a live reader is reading. So every failure to OBSERVE -- no /proc, an
# unreadable stat, a permission denial, a malformed field -- is UNKNOWN and the
# lease is retained until it expires. Only readable evidence reclaims early:
# the pid is absent from a /proc we could read, its starttime differs (so the
# pid was recycled), or the boot id differs (no process survives a reboot).
#
# The triple, rather than the pid alone, is what makes any of those provable:
# a starttime is only comparable WITHIN one boot, and `build/` outlives reboots.
utest_lease_holder_state() {
    local holder="$1" pid rest start ns bid now_bid now_ns cur
    pid="${holder%%.*}";  rest="${holder#*.}"
    start="${rest%%.*}";  rest="${rest#*.}"
    ns="${rest%%.*}";     bid="${rest#*.}"
    case "$pid"   in ''|*[!0-9]*) printf 'UNKNOWN\n'; return 0 ;; esac
    case "$start" in ''|*[!0-9]*) printf 'UNKNOWN\n'; return 0 ;; esac
    case "$ns"    in ''|*[!0-9]*) printf 'UNKNOWN\n'; return 0 ;; esac
    case "$bid"   in ''|*[!a-f0-9-]*) printf 'UNKNOWN\n'; return 0 ;; esac
    # The DEGRADED SENTINELS the acquirer writes when it could not observe its
    # own identity (`start=0`, `ns=0`, `bid=nobootid`). They are absences, not
    # values, and comparing them is how "I could not see" silently becomes "it
    # is dead": a live reader whose one `/proc/self/stat` read failed records
    # `start=0`, and a later comparison against its REAL start time then reads
    # as a recycled pid and revokes the grant mid-read.
    [ "$start" != "0" ] || { printf 'UNKNOWN\n'; return 0; }
    [ "$ns" != "0" ]    || { printf 'UNKNOWN\n'; return 0; }
    [ -d "${UTEST_PROC:-/proc}" ] || { printf 'UNKNOWN\n'; return 0; }
    # A pid is only meaningful inside its PID NAMESPACE, and `build/` is exactly
    # the kind of thing a container shares with its host (this repo ships a
    # devcontainer profile, and the lease's whole reason for existing is a
    # consumer reading over a mount). A reader in its own namespace records pid
    # 1; interpreting that against the host's /proc/1 finds a different start
    # time, concludes DEAD, and deletes the record out from under a live reader.
    # So a lease from another namespace is UNKNOWN -- retained until its TTL --
    # rather than something we pretend to be able to evaluate.
    now_ns="$(utest_pid_ns_id)"
    [ -n "$now_ns" ] || { printf 'UNKNOWN\n'; return 0; }
    if [ "$now_ns" != "$ns" ]; then
        printf 'UNKNOWN\n'
        return 0
    fi
    now_bid="$(utest_boot_id)"
    [ -n "$now_bid" ] || { printf 'UNKNOWN\n'; return 0; }
    if [ "$now_bid" != "$bid" ]; then
        printf 'DEAD\n'
        return 0
    fi
    if [ ! -e "${UTEST_PROC:-/proc}/$pid" ]; then
        # ABSENCE IS ONLY EVIDENCE IF PROCFS IS NOT HIDING THINGS FROM US.
        # `-e` cannot tell ESRCH from a process the kernel refuses to show:
        # under `hidepid=2`, another user's LIVE process is simply invisible,
        # and concluding DEAD there deletes a record that reader is mid-way
        # through opening. `/proc/1` is init and root-owned -- present under a
        # normal procfs, hidden under hidepid=2 for any non-root reader, and
        # visible to root, who could have seen the holder too. So it answers
        # exactly the question being asked: "would I have been shown it?"
        if [ -e "${UTEST_PROC:-/proc}/1" ]; then
            printf 'DEAD\n'
        else
            printf 'UNKNOWN\n'
        fi
        return 0
    fi
    cur="$(utest_proc_starttime "$pid")"
    [ -n "$cur" ] || { printf 'UNKNOWN\n'; return 0; }
    if [ "$cur" = "$start" ]; then printf 'LIVE\n'; else printf 'DEAD\n'; fi
    return 0
}

# The canonical lease document, and the ONLY spelling of it.
#
# A lease is validated by regenerating this from the fields read out of the file
# and comparing byte for byte, exactly as utest_pointer_pin_id validates a
# pointer -- and with exactly the same honest limit: byte-identity proves the
# GRAMMAR, never the provenance. Anything running as this user can write a
# well-formed lease. What it cannot do is exceed the admission cap or outlive
# the TTL, which is where the actual bound lives.
# A JSON integer, not merely a digit string.
#
# `08` passes every digits-only test ever written and then ABORTS the shell in
# `$(( ))`, where bash reads a leading zero as octal and 8 is not octal -- under
# `set -e`, mid-prune, holding the retention mutex. It is the identical trap
# utest_norm_bound was written for after `UTEST_RECORD_KEEP=08`, and a lease
# document is far more hostile input than an environment variable: the abort
# empties the live-lease count, an empty count is not "0", and the record then
# reads as permanently leased. The length cap is part of the grammar for the
# same reason -- `$(( ))` wraps a 40-digit literal into a plausible 64-bit value.
utest_is_json_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0)           return 0 ;;
        0*)          return 1 ;;
    esac
    [ "${#1}" -le 12 ]
}

utest_lease_doc_for() {
    # $1 run_id  $2 holder  $3 lease_id  $4 acquired_at  $5 expires_at
    printf '{ "schema": "utest-reader-lease-v1", "run_id": "%s", "holder": "%s", "lease_id": "%s", "acquired_at": %s, "expires_at": %s }\n' \
        "$1" "$2" "$3" "$4" "$5"
}

# Echo "<expires_at> <acquired_at> <holder>" when $1 is a well-formed lease for
# record $2; nothing otherwise. Size- and time-bounded for the same reasons the
# pointer validator is, and numeric fields are LENGTH-capped as well as
# digits-only: `$(( ))` reads a 40-digit literal as a wrapped 64-bit value, so a
# document claiming an astronomically distant expiry must be rejected as
# malformed rather than arithmetic'd into something plausible.
utest_lease_fields() {
    local path="$1" want_run="$2" want_base="${3:-}" body sz base id holder lid acq exp
    # `wc -c -- "$path"`, NEVER `wc -c < "$path"`. The redirection is performed
    # by THIS shell before `timeout` is ever exec'd, so a FIFO swapped in after
    # the caller's type test blocks on open() forever, outside the supervision
    # that was supposed to bound it -- while the retention mutex is held, which
    # wedges retention for every later run rather than costing five seconds.
    # Letting `wc` open the pathname itself puts the open inside the timeout.
    sz="$(timeout 5 wc -c -- "$path" 2>/dev/null | awk '{print $1; exit}')" || return 0
    case "$sz" in ''|*[!0-9]*) return 0 ;; esac
    [ "$sz" -le 4096 ] || return 0
    body="$(timeout 5 head -c 4096 -- "$path" 2>/dev/null)" || return 0
    [ -n "$body" ] || return 0
    case "$body" in
        *'"run_id": "'*'"holder": "'*'"lease_id": "'*'"acquired_at": '*'"expires_at": '*) ;;
        *) return 0 ;;
    esac
    id="${body#*\"run_id\": \"}";      id="${id%%\"*}"
    holder="${body#*\"holder\": \"}";  holder="${holder%%\"*}"
    lid="${body#*\"lease_id\": \"}";   lid="${lid%%\"*}"
    acq="${body#*\"acquired_at\": }";  acq="${acq%%[!0-9]*}"
    exp="${body#*\"expires_at\": }";   exp="${exp%%[!0-9]*}"
    case "$id"     in ''|.*|*[!A-Za-z0-9._-]*) return 0 ;; esac
    case "$holder" in ''|*[!A-Za-z0-9.-]*)     return 0 ;; esac
    case "$lid"    in ''|*[!A-Za-z0-9]*)       return 0 ;; esac
    utest_is_json_uint "$acq" || return 0
    utest_is_json_uint "$exp" || return 0
    [ "$id" = "$want_run" ] || return 0
    # The FILENAME is part of the contract, exactly as a pointer's leg is: a
    # lease copied under another acquisition's name would otherwise let one
    # reader's release unlink a different reader's live grant.
    #
    # WHICH filename is a separate question from which BYTES to read, which is
    # why $3 exists. Section 46's sweep reads a detached candidate under a
    # private staging name while its contract is still the name it was
    # published under; deriving the expectation from the path there would
    # classify every lease as malformed -- including the legitimate replacement
    # the detach exists to protect. Callers holding the published path pass
    # nothing and get the old behaviour.
    base="${want_base:-${path##*/}}"
    [ "$base" = "$holder.$lid.lease" ] || return 0
    [ "$body" = "$(utest_lease_doc_for "$id" "$holder" "$lid" "$acq" "$exp")" ] || return 0
    printf '%s %s %s\n' "$exp" "$acq" "$holder"
    return 0
}

# Destroy one swept entry through ONE verified object.
#
# $1 = the pathname to remove, $2 = a verdict function, $3.. = its extra args.
# The verdict is invoked as `$2 <staged-path> <original-basename> $3..` and
# returns 0 when the DETACHED object must be destroyed, 1 when it must be kept.
#
# WHY detach at all. A classification and the `rm` that follows it are two
# syscalls against a NAME, so the thing removed need not be the thing checked:
# a writer that drops a legitimate new pointer onto that name in between has it
# deleted by a verdict passed on its predecessor. `rename(2)` is the one
# primitive bash does have that acts atomically on the directory entry without
# ever opening the object, so detaching first turns "remove whatever is at this
# name" into "remove exactly this inode", and whatever arrives afterwards lands
# on a free name this pass no longer touches.
#
# WHY only destroy-candidates are detached. Detaching every entry would bind
# the SURVIVOR verdict to one inode too, but it also makes every published
# pointer briefly ENOENT on every startup for every leg, to close a window that
# is unreachable while scripts/test.sh holds its exclusive retention flock. The
# invariant bought instead is the one that matters: nothing is ever destroyed
# that was not itself verified. A replacement arriving after the candidate
# parse survives to the NEXT sweep, which classifies it normally -- bounded,
# non-wedging and self-healing, none of which a wrongful delete is.
#
# `mv -fT` / `ln -PT`, never the bare forms: without `-T` a destination that is
# unexpectedly a DIRECTORY turns the rename into a move INSIDE it (the same
# reason utest_lease_acquire and utest_publish_leg_pointer need it), and `-P`
# stops `ln` dereferencing a symlink source.
# RETURNS 0 when the object was DESTROYED, 1 when it was RETAINED (put back,
# left untouched, or preserved because it could not be put back). The caller
# MUST honour that: a lease the recheck votes to keep is restored, and counting
# it reclaimed anyway drops the record from the leased set and deletes it out
# from under a live reader.
utest_sweep_verified() {
    local path="$1" verdict="$2" dir base sdir staged n
    shift 2
    dir="${path%/*}"; [ "$dir" != "$path" ] || dir="."
    base="${path##*/}"
    # `mkdir` is the atomic no-clobber claim bash actually has -- it FAILS when
    # the name exists -- so the staging destination INSIDE it cannot be
    # occupied by anyone between the check and the rename. An `rm -f` plus an
    # `[ -e ]` test ahead of a `mv -fT` cannot promise that: a creator landing
    # in the gap has its object overwritten and unlinked without either verdict
    # ever seeing it, which is this helper's own wrongful delete, merely moved
    # into the staging namespace. `$$` is no ownership token either (bash keeps
    # the parent's pid inside a subshell), which is precisely why the claim has
    # to be atomic rather than merely unlikely.
    #
    # Self-initialising counter rather than an assignment above this function:
    # the tooling suite extracts these helpers one
    # `sed -n '/^name() {/,/^}/p'` at a time, so a global declared OUTSIDE a
    # body never reaches the fixture and `set -u` would abort there alone.
    sdir=""
    for n in 1 2 3 4 5; do
        # Sanitise then force base 10, the same belt-and-braces this file
        # applies to every value that could arrive from outside: this is an
        # ENVIRONMENT-INHERITABLE name, and `$(( 08 + 1 ))` is not a wrong
        # answer but a shell ABORT ("value too great for base") under
        # `set -e`, mid-sweep, with the retention mutex held.
        case "${UTEST_SWEEP_SEQ:-}" in ''|*[!0-9]*) UTEST_SWEEP_SEQ=0 ;; esac
        UTEST_SWEEP_SEQ=$(( 10#$UTEST_SWEEP_SEQ + 1 ))
        if mkdir -- "$dir/.utest-sweep.$$.$UTEST_SWEEP_SEQ.d" 2>/dev/null; then
            sdir="$dir/.utest-sweep.$$.$UTEST_SWEEP_SEQ.d"
            break
        fi
    done
    # No claim means nothing was detached and the entry is untouched.
    [ -n "$sdir" ] || return 1
    staged="$sdir/obj"
    if ! mv -fT -- "$path" "$staged" 2>/dev/null; then
        rmdir -- "$sdir" 2>/dev/null || true
        # Gone under us: another remover got there first, which IS the outcome
        # asked for. Still present means the detach simply failed.
        if [ -e "$path" ] || [ -L "$path" ]; then return 1; fi
        return 0
    fi
    if "$verdict" "$staged" "$base" "$@"; then
        rm -f -- "$staged" 2>/dev/null || true
        if [ ! -e "$staged" ] && [ ! -L "$staged" ]; then
            rmdir -- "$sdir" 2>/dev/null || true
            return 0
        fi
        # `rm` did not actually remove it -- a DIRECTORY is the reachable case.
        # Fall through and put it back, rather than report a destruction that
        # did not happen and leave the thing hidden under a staging name.
    fi
    # The object changed between selection and detach, or could not be
    # destroyed. Put it back: `ln` fails atomically when the name is occupied,
    # so a newcomer is never clobbered.
    if ln -PT -- "$staged" "$path" 2>/dev/null; then
        rm -f -- "$staged" 2>/dev/null || true
        rmdir -- "$sdir" 2>/dev/null || true
        return 1
    fi
    # A failed relink is NOT self-evidently EEXIST -- ENOSPC, EPERM, a
    # directory source, and a filesystem without hard links all land here. A
    # NON-DIRECTORY now at the name is the ordinary newcomer: it wins, and this
    # copy is dropped.
    if { [ -e "$path" ] || [ -L "$path" ]; } && [ ! -d "$path" ]; then
        rm -f -- "$staged" 2>/dev/null || true
        rmdir -- "$sdir" 2>/dev/null || true
        return 0
    fi
    # Anything else is unexplained, so the object STAYS in its staging claim
    # and is reported. Nothing ever deletes it: the reap is `rmdir`, which
    # collects empty claims only. A glob-and-`rm` reap would turn every
    # preserved object into a delayed wrongful delete on the next sweep --
    # exactly the failure this helper exists to prevent, deferred by one run.
    printf 'test.sh: could not restore %s -- object left at %s\n' "$path" "$staged" >&2
    return 1
}

# Collect the staging claims an earlier sweep finished with, and REPORT the
# ones it did not. `rmdir` is the entire policy: it removes an empty claim and
# refuses a non-empty one, so a claim still holding an object -- a crash
# between the detach and the verdict, or a restore that could not be made --
# survives and gets named instead of being destroyed by a pattern match.
# REPORTS the number of claims it could NOT resolve, because reporting alone is
# not protection. A preserved object lives inside its claim and matches no
# `*.lease` glob, so a later pass that merely printed a warning would count
# zero live leases and let retention delete the record -- and the preserved
# object with it, one pass after the preservation. The count is what makes the
# unresolved case FAIL CLOSED: its record is held until a human resolves it.
utest_sweep_reap() {
    local dir="$1" d
    # Sets UTEST_SWEEP_STUCK rather than echoing it: the only consumer runs
    # once per RETAINED RECORD inside the retention mutex, and a command
    # substitution there would fork a subshell per record purely to discover
    # that no claim exists -- which is the common case on every startup.
    UTEST_SWEEP_STUCK=0
    for d in "$dir"/.utest-sweep.*.d; do
        [ -d "$d" ] || continue
        rmdir -- "$d" 2>/dev/null && continue
        UTEST_SWEEP_STUCK=$(( UTEST_SWEEP_STUCK + 1 ))
        printf 'test.sh: unresolved swept object under %s (needs manual review)\n' "$d" >&2
    done
    return 0
}

# Reclaim verdict for one lease: 0 = destroy it, 1 = count it live.
#
# $1 = the path to READ, $2 = the basename its grammar must validate against,
# $3 = record id, $4 = now, $5 = the TTL ceiling. The two names are separate
# arguments for the reason utest_lease_fields documents: a detached candidate
# is read under a staging name while its contract is still its published one.
#
# Extracted from the loop below so that the selection pass and the binding
# recheck inside utest_sweep_verified are the SAME code. That is also what
# makes the recheck safe: for an unchanged object every input here is either a
# loop constant (`now`, `ttl`, the record id) or monotone toward reclaim (a
# holder only ever goes LIVE -> DEAD), so a KEEP verdict after the detach is
# proof the object at that name changed, never a lease resurrected behind a
# concurrent lock-free utest_lease_release.
utest_lease_reclaimable() {
    local path="$1" base="$2" rec="$3" now="$4" ttl="$5" want="${6:-}" fields exp acq holder eff state
    # What this call READ, published for the caller to hand back as $6 on the
    # binding recheck. Cleared first so no path can leave a previous call's
    # snapshot standing.
    UTEST_LEASE_SEEN=""
    # -L first: `-f` follows a symlink, so a link to a valid lease elsewhere
    # would otherwise be parsed instead of removed.
    if [ -L "$path" ] || [ ! -f "$path" ]; then return 0; fi
    fields="$(utest_lease_fields "$path" "$rec" "$base")"
    UTEST_LEASE_SEEN="$fields"
    [ -n "$fields" ] || return 0
    # THE BINDING RECHECK ASKS A DIFFERENT QUESTION. Given what the selection
    # read, the question is no longer "is this reclaimable" but "is this still
    # the same object"; anything else is a replacement and must be put back,
    # not judged afresh.
    #
    # Re-deriving the verdict on a match would NOT be monotone, which is the
    # subtle way this loses a live reader's record: utest_lease_holder_state
    # answers UNKNOWN on a transient procfs failure (an unreadable
    # /proc/<pid>/stat, a momentarily missing /proc/1) and LIVE on the retry,
    # and the future-dated branch below maps UNKNOWN to reclaim and LIVE to
    # keep. So a future-dated lease could be SELECTED as reclaimable and then
    # flip to keep on an inode that never changed -- and the same flip is what
    # would let a relink undo a lock-free utest_lease_release that landed
    # inside the detach window. Comparing the snapshot removes both, without
    # making release take the retention mutex (it is deliberately lock-free;
    # see its own comment).
    if [ -n "$want" ]; then
        # DIFFERENT object: a replacement, to be put back rather than judged.
        if [ "$fields" != "$want" ]; then return 1; fi
        # SAME object, and the caller only reaches the recheck because the
        # selection already ruled it reclaimable -- so identity IS the answer
        # and re-deriving here is not merely wasted work, it REOPENS the flip
        # this binding exists to close: utest_lease_holder_state below would be
        # observed a second time, and an UNKNOWN -> LIVE transition on an
        # unchanged inode would relink a lease a lock-free release had removed.
        return 0
    fi
    exp="${fields%% *}"; acq="${fields#* }"; holder="${acq#* }"; acq="${acq%% *}"
    # `10#` on every value that reached here from a FILE, belt-and-braces
    # behind utest_is_json_uint: the grammar already rejects a leading zero,
    # and forcing base 10 means a future relaxation of it cannot resurrect
    # the octal abort as a silent retention bypass.
    acq="$(( 10#$acq ))"; exp="$(( 10#$exp ))"
    state="$(utest_lease_holder_state "$holder")"
    if [ "$state" = "DEAD" ]; then return 0; fi
    # A FUTURE acquired_at, and why neither obvious answer is right.
    # Clamping it up to `now` and computing `acq + ttl` recomputes the
    # deadline from a MOVING origin, so a document claiming
    # acquired_at=999999999999 is renewed by every pass and never expires.
    # Deleting it unconditionally is the mirror hazard: an NTP correction, a
    # VM restore, or an operator moving the clock backwards makes a
    # perfectly valid lease look future-dated, and revoking it mid-read is
    # precisely what the lease promises will not happen.
    #
    # So the LIVENESS evidence decides, since that is the thing a clock
    # cannot corrupt. A live holder is honoured whatever its timestamps say
    # (it is bounded by the admission cap regardless); an unobservable
    # holder carrying a timestamp it could not honestly have written buys
    # nothing. `now` is sampled AFTER the mutex is held, so an acquirer that
    # published while this prune waited is not mistaken for either case.
    if [ "$acq" -gt "$now" ]; then
        if [ "$state" = "LIVE" ]; then return 1; fi
        return 0
    fi
    eff="$exp"
    if [ "$(( acq + ttl ))" -lt "$eff" ]; then eff="$(( acq + ttl ))"; fi
    if [ "$now" -ge "$eff" ]; then return 0; fi
    return 1
}

# How many LIVE leases record $1 holds, RECLAIMING the rest as it goes.
# $2 = now (epoch seconds), $3 = the TTL ceiling.
#
# Reclaim is self-healing, matching the dangling-pointer sweep below: a lease
# that is malformed, expired, or whose holder is provably gone is UNLINKED
# rather than merely ignored, or `leases/` becomes a directory that only grows.
# It is safe to unlink here precisely BECAUSE the caller holds the retention
# lock -- an unsynchronised check-then-unlink could delete a grant made between
# the two syscalls, which is the same class of race the lock exists to remove.
#
# This WRITES inside a record, so `leases/` is an explicitly MUTABLE control
# namespace. The payload documents and the marker stay immutable, and that -- not
# the directory as a whole -- is the property the documented resolver depends on.
#
# The TTL is applied HERE and not merely trusted from the document, because
# `expires_at` is written by the ACQUIRER: a bound that only the writer enforces
# is not a bound. A future `acquired_at` is resolved by the holder's LIVENESS
# rather than by arithmetic -- see the branch below for why neither clamping it
# nor deleting it outright is safe.
utest_record_live_leases() {
    local rec="$1" now="$2" ttl="$3" dir f seen live=0
    dir="$RUNS_DIR/$rec/leases"
    [ -d "$dir" ] || { printf '0\n'; return 0; }
    # ABANDONED STAGING first. An acquisition killed between writing
    # `.<lease_id>.tmp` and renaming it leaves a file that matches neither
    # `*.lease` nor `*`, so nothing below would ever see it -- and on a record
    # that stays current and pinned, repeated interrupted acquisitions
    # accumulate hidden files forever, past both the TTL and the lease cap. The
    # mutex is what makes removing them safe: a live acquirer's staging file
    # cannot exist while this runs.
    # `-L` beside `-e`, or a DANGLING symlink named `.<id>.tmp` fails the
    # existence test while still occupying the namespace, and staging that
    # nothing can reap is the leak this loop exists to prevent. The type test
    # the old spelling carried is dropped for the same reason: everything
    # matching this private pattern is ours by contract, and `rm` never opens
    # what it unlinks, so there is nothing to be cautious about. It also reaps
    # any `.utest-sweep.*.tmp` a detach below could not put back.
    for f in "$dir"/.*.tmp; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        rm -f -- "$f" 2>/dev/null || true
    done
    for f in "$dir"/*.lease; do
        [ -e "$f" ] || [ -L "$f" ] || continue
        # A DIRECTORY under a lease name is not a lease, and not something `rm`
        # can remove. The pre-46 loop's `rm -f` failed silently and left it
        # published and uncounted; detaching it instead would merely hide it
        # under a staging name, which is strictly worse. Same outcome as
        # before, now on purpose.
        if [ -d "$f" ] && [ ! -L "$f" ]; then continue; fi
        # SELECTION by name, then DESTRUCTION through the detached object: the
        # same verdict runs twice, and only the second one -- taken on an inode
        # nothing else can reach, and BOUND to what the first one read -- is
        # allowed to unlink.
        if utest_lease_reclaimable "$f" "${f##*/}" "$rec" "$now" "$ttl"; then
            seen="${UTEST_LEASE_SEEN:-}"
            # RETAINED counts as live. The recheck can vote keep (the object
            # was replaced between selection and detach), in which case the
            # lease is back at its name and a record dropped from the leased
            # set here would be deleted under a reader still holding it.
            if ! utest_sweep_verified "$f" utest_lease_reclaimable "$rec" "$now" "$ttl" "$seen"; then
                live=$(( live + 1 ))
            elif [ -e "$f" ] || [ -L "$f" ]; then
                # DESTROYED the detached predecessor, and a replacement took
                # the published name while it was gone. The glob above was
                # expanded before the loop began, so this entry is never
                # revisited -- reporting the record unleased while a fresh
                # grant sits on disk is how it gets deleted under its reader.
                # Counted conservatively; the next pass classifies it properly.
                live=$(( live + 1 ))
            fi
        else
            live=$(( live + 1 ))
        fi
    done
    # ADMITTED LEASES ONLY, one number, because utest_lease_acquire reads this
    # to enforce the admission cap. Unresolved sweep claims also hold a record,
    # but they are NOT leases and are counted by the pruner separately -- see
    # the tagged `L`/`U` emission there for why conflating them evicts a real
    # lease from a saturated cap.
    printf '%s\n' "$live"
    return 0
}

# Acquire a reader lease on whatever leg $1 currently resolves to. Echoes
# "<record_dir> <lease_path>" and returns 0 on success; echoes nothing and
# returns 1 on refusal, which is the caller's signal to fall back to the bounded
# ENOENT re-resolve rather than to assume protection it was never granted.
#
# This script is the PRODUCER and never reads a record, so nothing here calls
# it. It ships anyway because it is the reference implementation of the reader
# half, and shipping one spelling of the grammar is what keeps a consumer and
# the pruner from drifting apart -- the same argument that makes
# utest_leg_pointer_doc_for the only emitter of a pointer document.
#
# ADMISSION CONTROL, not revocation. The cap is enforced HERE, at acquisition,
# so an over-capacity reader is refused explicitly and knows it is unprotected.
# Ranking leases at prune time instead would hand out a grant, report success,
# and then silently delete the record anyway -- a lease that can be revoked
# without telling the holder is not a lease.
utest_lease_acquire() {
    local leg="$1" ptr rec run_id now ttl lease_max leased holder lid path tmp acq exp lockrc bid start ns n
    ptr="$PROJECT/build/test-results-${leg}.run"
    ttl="$(utest_norm_bound "${UTEST_LEASE_TTL:-300}" 300 86400)"
    lease_max="$(utest_norm_bound "${UTEST_LEASE_MAX:-8}" 8 10000)"
    [ "$lease_max" -gt 0 ] || return 1
    # Same one-second floor the pruner applies, so the two cannot disagree about
    # what a lease's lifetime is.
    [ "$ttl" -ge 1 ] 2>/dev/null || ttl=300

    # `|| lockrc=$?`, never `cmd; lockrc=$?`: this script runs under `set -e`,
    # where a bare non-zero command aborts before the assignment is reached.
    lockrc=0; utest_retention_lock 10 || lockrc=$?
    if [ "$lockrc" -eq 1 ]; then return 1; fi
    # AFTER the lock, for the same reason the pruner does it: the admission scan
    # below classifies leases by age, and a `now` sampled before a ten-second
    # wait would read a lease published during that wait as future-dated.
    now="$(date -u +%s 2>/dev/null)"
    case "$now" in ''|*[!0-9]*) utest_retention_unlock; return 1 ;; esac
    utest_prime_ident

    [ -f "$ptr" ] || { utest_retention_unlock; return 1; }
    run_id="$(utest_pointer_pin_id "$ptr")"
    if [ -z "$run_id" ]; then utest_retention_unlock; return 1; fi
    rec="$RUNS_DIR/$run_id"
    # Never `mkdir -p`: see the record-creation comment. An absent leases/ means
    # the record is gone or was never completed, and both are refusals.
    if [ ! -d "$rec/leases" ]; then utest_retention_unlock; return 1; fi

    # Admit against the number of DISTINCT leased records: a second lease on a
    # record already held costs no additional retention, so it must not consume
    # a slot the cap exists to ration.
    # The count is VALIDATED, never used raw. An empty or non-numeric answer
    # means the counter itself failed, and "not the string 0" would otherwise
    # read as "already leased" -- which skips the cap entirely and hands out a
    # grant above capacity. Admission refuses on anything it cannot read.
    n="$(utest_record_live_leases "$run_id" "$now" "$ttl")" || n=""
    case "$n" in ''|*[!0-9]*) utest_retention_unlock; return 1 ;; esac
    if [ "$n" = "0" ]; then
        leased=0
        for path in "$RUNS_DIR"/*/; do
            [ -d "$path" ] || continue
            path="${path%/}"
            # The target was counted above and is known to hold none; counting
            # it a second time here re-walks its whole leases directory inside
            # the mutex for an answer already in hand.
            [ "${path##*/}" != "$run_id" ] || continue
            n="$(utest_record_live_leases "${path##*/}" "$now" "$ttl")" || n=""
            case "$n" in ''|*[!0-9]*) utest_retention_unlock; return 1 ;; esac
            if [ "$n" != "0" ]; then
                leased=$(( leased + 1 ))
                # The cap is a threshold, not a census: once it is reached the
                # answer cannot change, and every further record scanned is
                # pure latency on the lock that blocks retention.
                [ "$leased" -lt "$lease_max" ] || break
            fi
        done
        if [ "$leased" -ge "$lease_max" ]; then utest_retention_unlock; return 1; fi
    fi

    start="$(utest_proc_starttime $$)"; [ -n "$start" ] || start=0
    ns="$(utest_pid_ns_id)"; [ -n "$ns" ] || ns=0
    bid="$(utest_boot_id)"; [ -n "$bid" ] || bid=nobootid
    holder="$$.$start.$ns.$bid"
    # A NONCE per acquisition, not per process. The pathname has to identify the
    # ACQUISITION: two overlapping reads in one process would otherwise share
    # one filename, and the first release would unlink the second's live grant.
    lid="$(od -An -N8 -tx1 /dev/urandom 2>/dev/null | tr -cd 'a-f0-9' || true)"
    [ -n "$lid" ] || lid="$(date -u +%s%N 2>/dev/null | tr -cd '0-9')"
    [ -n "$lid" ] || lid="0"
    # RE-SAMPLE the clock here, not at entry. `now` was taken before a lock
    # wait bounded at 10s and before an admission scan over every record, so
    # publishing `entry_now + ttl` can hand back a lease that is ALREADY past
    # its deadline -- with UTEST_LEASE_TTL=1 and two seconds of contention, the
    # grant is expired before the caller sees it, and the next prune reclaims a
    # record the reader was told it had.
    acq="$(date -u +%s 2>/dev/null)"
    case "$acq" in ''|*[!0-9]*) utest_retention_unlock; return 1 ;; esac
    exp="$(( acq + ttl ))"
    # A grant with no remaining lifetime is not a grant. Refusing sends the
    # caller to the unleased fallback, which is honest; returning it would
    # claim a protection that expires on the same tick.
    if [ "$exp" -le "$acq" ]; then utest_retention_unlock; return 1; fi
    path="$rec/leases/$holder.$lid.lease"
    tmp="$rec/leases/.$lid.tmp"
    if ! utest_lease_doc_for "$run_id" "$holder" "$lid" "$acq" "$exp" > "$tmp" 2>/dev/null; then
        rm -f -- "$tmp" 2>/dev/null || true
        utest_retention_unlock
        return 1
    fi
    # `-T` for the same reason utest_publish_leg_pointer needs it: without it a
    # target that is unexpectedly a directory turns the rename into a move
    # INSIDE it, and the lease lands somewhere nothing will ever look.
    if ! mv -fT -- "$tmp" "$path" 2>/dev/null; then
        rm -f -- "$tmp" 2>/dev/null || true
        utest_retention_unlock
        return 1
    fi
    # VERIFY the record is still whole after publishing. Under the lock a prune
    # cannot have run inside this sequence, so the only way it is gone is that
    # one COMPLETED before the lock was taken -- in which case the pointer named
    # a corpse and the honest answer is a refusal, not a grant over nothing.
    if [ ! -d "$rec" ] || [ ! -f "$rec/record-complete.json" ]; then
        rm -f -- "$path" 2>/dev/null || true
        utest_retention_unlock
        return 1
    fi
    utest_retention_unlock
    printf '%s %s\n' "$rec" "$path"
    return 0
}

# Release a lease taken by utest_lease_acquire.
#
# Deliberately NEEDS NO LOCK. Unlinking a lease a prune is concurrently reading
# is safe in both orderings: prune either counted it (and keeps the record for
# one more pass, which is merely conservative) or finds it gone and treats the
# record as unleased -- which is exactly what the release means. The reader's
# descriptors keep the bytes alive regardless, so the record may be removed the
# moment it stops being wanted.
utest_lease_release() {
    case "${1:-}" in
        ''|*[!A-Za-z0-9./_-]*) return 0 ;;
        */leases/*.lease) rm -f -- "$1" 2>/dev/null || true ;;
    esac
    return 0
}

utest_prune_records() {
    local keep="${UTEST_RECORD_KEEP:-20}" pin_max="${UTEST_POINTER_PIN_MAX:-64}" old pinned ptr
    local lease_max="${UTEST_LEASE_MAX:-8}" ttl="${UTEST_LEASE_TTL:-300}"
    local now leased leased_all leased_l stuck_u n_stuck nu lockrc n_all n_kept n all_records
    keep="$(utest_norm_bound "$keep" 20 10000)"
    # Zero records kept is nonsense, so the floor is applied here and not in
    # the shared normaliser -- the pin cap deliberately does NOT have one.
    [ "$keep" -ge 1 ] 2>/dev/null || keep=20
    pin_max="$(utest_norm_bound "$pin_max" 64 10000)"
    lease_max="$(utest_norm_bound "$lease_max" 8 10000)"
    ttl="$(utest_norm_bound "$ttl" 300 86400)"
    # A ONE-SECOND FLOOR, matching what the doc promises. The shared normaliser
    # deliberately has no floor (UTEST_POINTER_PIN_MAX=0 is a real opt-out), but
    # a zero TTL here does not disable leases -- it expires every lease the
    # instant it is written and makes acquisition refuse outright, so an
    # operator reading "normalised into 1..86400" would silently switch the
    # protection off. UTEST_LEASE_MAX=0 is the documented disable switch.
    [ "$ttl" -ge 1 ] 2>/dev/null || ttl=300

    # EVERYTHING below runs under the retention mutex, which is what turns a
    # reader's lease from a hint into a guarantee: an acquirer publishes and
    # verifies its lease while holding this same lock, so no prune can be part
    # way through classifying the record it is about to lease.
    #
    # A TIMEOUT defers retention to the next run rather than deleting records it
    # cannot safely classify. That is the correct direction to fail -- retention
    # has always been best-effort-per-run (it only happens when this script
    # runs), whereas a record deleted out from under a reader is unrecoverable.
    # A lock held forever is bounded by the holder's own lifetime, because
    # `flock` releases on descriptor close and therefore on process death.
    # `|| lockrc=$?`, never `cmd; lockrc=$?`: under `set -e` a bare non-zero
    # command aborts the run before the assignment is ever reached.
    lockrc=0; utest_retention_lock 10 || lockrc=$?
    if [ "$lockrc" -eq 1 ]; then
        echo -e "${YELLOW}[TEST]${RESET} retention SKIPPED this run -- a reader holds build/.test-retention.lock."
        return 0
    fi
    # SAMPLE THE CLOCK AFTER THE LOCK, never before. The wait above is bounded
    # at ten seconds, and an acquirer holding the lock publishes leases during
    # it: a pre-lock `now` would see those perfectly valid, just-written leases
    # as future-dated and unlink them, then prune the record a live reader had
    # just been granted. Priming the identity cache here likewise happens once
    # per prune, in THIS shell, so the per-lease scans below inherit it.
    now="$(date -u +%s 2>/dev/null)"
    case "$now" in ''|*[!0-9]*) now=0 ;; esac
    utest_prime_ident

    # PIN the records the live per-leg pointers still resolve to. Age alone
    # would delete the record a `.run` pointer names, and that pointer is the
    # documented resolution path: pruning its target turns a published contract
    # into a dangling one after twenty unrelated runs of any other leg.
    #
    # BOUNDED, because a pin must not defeat the disk bound it sits inside.
    # `UTEST_LEG` appends any `[a-z0-9-]{1,32}` label to the derived leg name,
    # so the number of DISTINCT stable leg names -- and therefore of pointer
    # files -- is unbounded, and an unconditional exemption would pin an
    # unbounded number of record directories. Only the most recently PUBLISHED
    # pin_max pointers win a pin. Mtime is the right ordering because a pointer
    # is rewritten by every completed run of its leg, so it ranks legs by how
    # recently they actually ran; the losers become ordinary age candidates and
    # are swept below.
    # The cap counts VALIDATED pins, not raw filenames. Truncating the
    # candidate list first spends the budget on files that turn out to pin
    # nothing: with `pin_max=1`, one newer CORRUPT pointer displaced a valid
    # older one out of the list, the valid pointer's record then aged out and
    # was pruned, and the sweep afterwards removed that pointer as dangling --
    # so self-healing one junk file destroyed an unrelated published
    # generation. Validating first and taking the cap afterwards means a
    # malformed pointer costs only its own pin.
    pinned=""
    if [ "$pin_max" -gt 0 ]; then
        pinned="$(
            {
                find "$PROJECT/build" -maxdepth 1 -type f -name 'test-results-*.run' \
                    -printf '%T@\t%p\n' 2>/dev/null | sort -rn |
                    while IFS="$(printf '\t')" read -r _mtime ptr; do
                        [ -n "$ptr" ] || continue
                        # The WHOLE contract, not just an extractable run id --
                        # see utest_pointer_pin_id. Nothing that fails
                        # validation pins anything or consumes budget, so a
                        # malformed pointer can only ever cost its own record
                        # its pin, never protect something outside RUNS_DIR and
                        # never deny a valid pointer the slot it needed.
                        old="$(utest_pointer_pin_id "$ptr")"
                        [ -n "$old" ] || continue
                        printf '%s\n' "$old"
                    done | head -n "$pin_max"
            } || true
        )"
    fi

    # HOLD what a reader has resolved, and reclaim what no reader can still be
    # using. This sweeps EVERY record, not just the age candidates: a lease on a
    # record young enough to survive on age would otherwise never be revisited,
    # and `leases/` would only ever grow.
    #
    # The cap here is a BACKSTOP, not the mechanism. A conforming reader is
    # rationed at acquisition (utest_lease_acquire refuses over capacity and
    # says so), so it is never revoked after being told it was safe. This cap
    # exists for the non-conforming case -- anything running as this user can
    # write a lease document -- and it ANNOUNCES itself when it bites, because a
    # disk bound that silently discards grants is how the pointer pin was nearly
    # got wrong too.
    # ONE enumeration, reused by both passes below. It was walked and sorted
    # twice -- once for lease classification and again for the age cut -- on the
    # startup path, inside the mutex.
    all_records="$(find "$RUNS_DIR" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -r || true)"

    leased=""
    if [ "$lease_max" -gt 0 ]; then
        leased_all="$(
            {
                while IFS= read -r old; do
                    [ -n "$old" ] || continue
                    # Validated, not used raw. "Anything but the string 0"
                    # would let a counter failure read as LEASED and hold a
                    # record against retention forever; an unreadable count
                    # is treated as unleased, which is recoverable.
                    # Two fields: admitted leases, then unresolved sweep
                    # claims. TAGGED on the way out because they are held on
                    # different terms -- `L` competes for UTEST_LEASE_MAX, `U`
                    # is a fail-closed hold that must never consume a capped
                    # slot (an unresolved claim taking the last one evicts a
                    # record with a real lease, which is a reader losing its
                    # record to a crash artefact).
                    n="$(utest_record_live_leases "$old" "$now" "$ttl")" || n=""
                    case "$n"  in ''|*[!0-9]*) n=0 ;; esac
                    # The claim count is taken HERE, not folded into the count
                    # above, because utest_lease_acquire reads that one for the
                    # admission cap and a claim is not an admitted lease. The
                    # reap reports through a variable, so this costs no fork.
                    utest_sweep_reap "$RUNS_DIR/$old/leases"
                    nu="${UTEST_SWEEP_STUCK:-0}"
                    case "$nu" in ''|*[!0-9]*) nu=0 ;; esac
                    if [ "$n" != "0" ]; then
                        printf 'L %s\n' "$old"
                    fi
                    if [ "$nu" != "0" ]; then
                        printf 'U %s\n' "$old"
                    fi
                done <<< "$all_records"
            } || true
        )"
        # The CAP applies to admitted leases only.
        leased_l="$(printf '%s' "$leased_all" | sed -n 's/^L //p')"
        stuck_u="$(printf '%s' "$leased_all" | sed -n 's/^U //p')"
        leased="$(printf '%s' "$leased_l" | head -n "$lease_max")"
        n_all="$(printf '%s' "$leased_l" | grep -c . || true)"
        n_kept="$(printf '%s' "$leased" | grep -c . || true)"
        if [ "${n_all:-0}" -gt "${n_kept:-0}" ] 2>/dev/null; then
            echo -e "${YELLOW}[TEST]${RESET} $(( n_all - n_kept )) leased record(s) over UTEST_LEASE_MAX=$lease_max are NOT held."
        fi
        # Unresolved claims are unioned in AFTER the truncation, so they are
        # held without ever costing an admitted lease its slot.
        if [ -n "$stuck_u" ]; then
            n_stuck="$(printf '%s' "$stuck_u" | grep -c . || true)"
            echo -e "${YELLOW:-}[TEST]${RESET:-} ${n_stuck:-0} record(s) held by an unresolved sweep claim (see the paths reported above)."
            if [ -n "$leased" ]; then leased="$leased"$'\n'"$stuck_u"; else leased="$stuck_u"; fi
        fi
    fi

    # Membership as SETS, not as a `grep` per candidate. The exemption lists are
    # bounded by pin_max (64) and lease_max, and the candidate list by however
    # many records have accumulated, so the old form spawned one grep per
    # candidate on the startup path to answer a question bash can answer in
    # process.
    local -A pinned_set=() leased_set=()
    if [ -n "$pinned" ]; then
        while IFS= read -r old; do
            [ -n "$old" ] || continue
            pinned_set["$old"]=1
        done <<< "$pinned"
    fi
    if [ -n "$leased" ]; then
        while IFS= read -r old; do
            [ -n "$old" ] || continue
            leased_set["$old"]=1
        done <<< "$leased"
    fi

    {
        printf '%s\n' "$all_records" | tail -n +$(( keep + 1 )) |
            while IFS= read -r old; do
                # %f yields a bare basename, so this can never escape RUNS_DIR,
                # and the run now in progress is never a candidate.
                [ -n "$old" ] || continue
                [ "$old" = "$RUN_ID" ] && continue
                if [ -n "${pinned_set["$old"]+x}" ]; then
                    continue
                fi
                # A resolved generation a reader still holds. Unlike the pin,
                # this survives the leg publishing again -- that is the entire
                # point: the pin protects what the CURRENT pointer names, the
                # lease protects what a reader ALREADY resolved.
                if [ -n "${leased_set["$old"]+x}" ]; then
                    continue
                fi
                rm -rf -- "$RUNS_DIR/$old"
            done
    } || true

    # SELF-HEALING second half. A pointer whose record is gone -- pruned past
    # the pin cap, or removed by hand -- would otherwise resolve to an absent
    # directory forever. A consumer must fail closed on that either way, but
    # "the pointer is absent" is a far clearer answer than "the pointer names
    # nothing", so the dangling pointer is removed rather than left to be
    # diagnosed. This half is deliberately UNBOUNDED where the pin is bounded:
    # it only ever removes, so it cannot grow anything.
    #
    # `! -type d` rather than `-type f`, because a SYMLINK named
    # test-results-<leg>.run is neither pinned by the scan above nor swept by a
    # regular-file-only scan here -- it would sit there resolving through an
    # arbitrary target that no part of this script controls. Nothing in
    # production ever creates one (the pointer arrives by `mv` of a regular
    # file), so its presence is hand-made and it is removed on sight.
    #
    # CLASSIFY BEFORE OPENING. Widening the scan past regular files also admits
    # FIFOs, sockets and devices, and reading a FIFO with no writer BLOCKS --
    # forever, at startup, wedging every later invocation of this script rather
    # than sweeping the thing that wedged it. So the type test decides the fate
    # of an entry entirely on its own; only a plain regular file is ever opened.
    #
    # And the type test alone decides only what is WORTH removing, never what
    # is removed: every unlink goes through utest_sweep_verified, which detaches
    # the name and re-takes the verdict on the detached inode, so a legitimate
    # pointer published onto that name mid-sweep cannot be deleted in place of
    # the thing that was classified.
    #
    # Reap the previous pass's staging claims first, so a claim this pass makes
    # can never be collected by its own reap. The reap only ever `rmdir`s, so
    # an unresolved object inside a claim is reported and kept, never deleted.
    {
        # No count is consulted here: `build/` holds no record to protect,
        # and an unresolved claim is reported by the reap itself.
        utest_sweep_reap "$PROJECT/build"
        find "$PROJECT/build" -maxdepth 1 ! -type d -name 'test-results-*.run' 2>/dev/null |
            while IFS= read -r ptr; do
                [ -n "$ptr" ] || continue
                # ONE verdict function, called twice: here to select a
                # candidate by name, and again inside utest_sweep_verified on
                # the detached inode, which is the only call allowed to unlink.
                # Spelling the selection out inline instead would be a second
                # definition of "dangling", free to drift from the one that
                # actually destroys things.
                #
                # It defines what is DESTROYABLE, not what is a candidate.
                # DIRECTORIES are excluded upstream by `! -type d` and never
                # reach it -- deliberately, and symmetrically with the lease
                # loop, because `rm` cannot remove one and detaching it would
                # only hide it. A directory left at a pointer name is inert to
                # this sweep and blocks that leg's publication; that gap is
                # older than this mechanism and is owned by section 53.
                if utest_ptr_sweepable "$ptr" "${ptr##*/}"; then
                    utest_sweep_verified "$ptr" utest_ptr_sweepable
                fi
            done
    } || true
    utest_retention_unlock
    return 0
}

# Every staging path lives inside the record, so no two invocations share one.
# They were fixed paths under build/ (`.test-results-refusal.*`, the `.staged`
# siblings, `$canonical.tmp`, and the run slice), which made them shared state
# between any two runs that did overlap -- the lock above now refuses that
# overlap, and private staging means a stale file from a killed run cannot be
# mistaken for this run's either.
XML_RECORD="$RECORD_DIR/test-results.xml"
JSON_RECORD="$RECORD_DIR/test-results.json"
IDENTITY_RECORD="$RECORD_DIR/test-run-identity.json"
RECORD_MARKER="$RECORD_DIR/record-complete.json"

# ALIASES. The canonical pair names THIS invocation and is cleared right here,
# so no exit can leave a stale document under it. The leg-suffixed pair means
# "the latest COMPLETED run of that configuration" -- it is never cleared on
# behalf of another leg, because runs of different configurations deliberately
# accumulate one artifact each, and it is only ever rewritten from a committed
# record.
XML_OUT="$PROJECT/build/test-results.xml"
JSON_OUT="$PROJECT/build/test-results.json"
IDENTITY_OUT="$PROJECT/build/test-run-identity.json"
# The leg-suffixed destinations cannot be named until the accelerator is
# chosen; until then the guard publishes to the canonical paths alone. The
# per-leg generation POINTER shares that fate -- it names a leg, so it cannot
# exist before the leg does.
XML_LEG_OUT=""
JSON_LEG_OUT=""
RUN_POINTER_OUT=""

rm -f "$XML_OUT" "$JSON_OUT" "$IDENTITY_OUT"

# Identity fields, filled by Step 0b. Read at call time, so a refusal
# published before Step 0b carries "unknown" rather than a stale value.
# RUN_TS and RUN_ID are the exceptions: both are properties of the INVOCATION
# rather than of the environment it went on to detect, they are known the
# moment the run starts, and a refusal is far easier to place with a real
# start time than with "unknown".
RUN_COMMIT="unknown"
RUN_LEG="unknown"
RUN_LEG_SOURCE="underived"
RUN_ACCEL="unknown"
RUN_HOST="unknown"
RUN_HOSTNAME="unknown"
RUN_QEMU="unknown"
SMP_CPUS_SAFE="0"

# Every identity value is generated or charset-filtered before it reaches
# these emitters (ISO timestamp, hex SHA, [a-z0-9-] leg, filtered hostname and
# QEMU basename, integer CPU count), so neither XML nor JSON escaping has
# anything to escape. The filters are what make that true -- do not widen one
# without adding the escaping it removes the need for.
utest_xml_identity_attrs() {
    printf ' timestamp="%s" hostname="%s"' "$RUN_TS" "$RUN_HOSTNAME"
}

utest_json_identity() {
    printf '{\n'
    printf '  "schema": "utest-run-identity-v1",\n'
    printf '  "run_id": "%s",\n' "$RUN_ID"
    printf '  "timestamp": "%s",\n' "$RUN_TS"
    printf '  "commit": "%s",\n' "$RUN_COMMIT"
    printf '  "leg": "%s",\n' "$RUN_LEG"
    printf '  "leg_source": "%s",\n' "$RUN_LEG_SOURCE"
    printf '  "host": "%s",\n' "$RUN_HOST"
    printf '  "hostname": "%s",\n' "$RUN_HOSTNAME"
    printf '  "accel": "%s",\n' "$RUN_ACCEL"
    printf '  "cpus": %s,\n' "$SMP_CPUS_SAFE"
    printf '  "qemu": "%s",\n' "$RUN_QEMU"
    printf '  "ci_parity": %s\n' "$([ "${CI_PARITY:-0}" = "1" ] && echo true || echo false)"
    printf '}'
}

# The XML projection carries the SAME field set as the JSON object. A field in
# one and not the other gives two consumers two different identity contracts
# for one run.
utest_xml_identity_props() {
    # `schema` rides here too. Without it the XML projection carried no
    # version marker at all, so identity-schema drift was untestable on the
    # side that most consumers actually read.
    printf '    <property name="schema" value="utest-run-identity-v1"/>\n'
    printf '    <property name="run_id" value="%s"/>\n' "$RUN_ID"
    printf '    <property name="commit" value="%s"/>\n' "$RUN_COMMIT"
    printf '    <property name="leg" value="%s"/>\n' "$RUN_LEG"
    printf '    <property name="leg_source" value="%s"/>\n' "$RUN_LEG_SOURCE"
    printf '    <property name="accel" value="%s"/>\n' "$RUN_ACCEL"
    printf '    <property name="cpus" value="%s"/>\n' "$SMP_CPUS_SAFE"
    printf '    <property name="qemu" value="%s"/>\n' "$RUN_QEMU"
    printf '    <property name="host" value="%s"/>\n' "$RUN_HOST"
    printf '    <property name="ci_parity" value="%s"/>\n' \
        "$([ "${CI_PARITY:-0}" = "1" ] && echo true || echo false)"
}

# Land a staged document as this run's RECORD. That is ALL this does: the
# aliases are published later, by utest_finalize_record(), and only after the
# commit marker names what the record holds.
#
# The split is load-bearing. An alias updated the moment one format landed is
# visible while the run is still assembling the other, so a death in between
# would leave a stable pathname pointing into a record with no commit marker
# and possibly only one of two requested documents -- an alias enumerator
# would consume an uncommitted run. Aliases must be updated only after the
# record is COMPLETE, which is exactly what the section requires.
#
# The `mv` is within the record directory, so the record itself commits
# atomically. Returns non-zero only if the record could not land; the caller
# marks the format published on that basis, because the guard's refusal path
# would otherwise move a run_incomplete document OVER a valid, fully
# assembled artifact -- destroying the evidence it exists to preserve.
utest_publish() {
    local staged="$1" record="$2"
    mv -f "$staged" "$record" || return 1
    return 0
}

# Copy one landed record out to its CANONICAL alias. That pair names THIS
# invocation, so each format stands alone there and per-file atomicity is the
# whole contract. The LEG pair is different -- its two files must agree with
# each other -- and goes through utest_publish_leg_set below. A failure is
# reported and FAILS the run rather than leaving a promised pathname silently
# absent.
utest_alias_record() {
    local record="$1" canonical="$2" tag="$3"
    [ -f "$record" ] || return 0
    if ! { cp -f "$record" "$RECORD_DIR/.${tag}canon.tmp" &&
           mv -f "$RECORD_DIR/.${tag}canon.tmp" "$canonical"; }; then
        rm -f "$RECORD_DIR/.${tag}canon.tmp"
        echo -e "  ${RED}[UTEST]${RESET} run record published at $record but the canonical alias $canonical could not be written"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
    fi
    return 0
}

# The per-leg GENERATION POINTER document.
#
# Deliberately MINIMAL. `record-complete.json` is already the authoritative
# statement of what a record holds, and a pointer that restated the document
# inventory would be a second source of truth that can disagree with the first
# -- so this file answers exactly one question ("which record does this leg
# resolve to right now") and defers every other question to the marker it
# names. `record` is derived from `run_id` rather than carried independently,
# so the two fields cannot drift apart either.
#
# Both interpolated values are generated under a charset filter (RUN_ID is
# `tr -cd 'A-Za-z0-9._-'`, RUN_LEG is a derived accelerator/CPU name optionally
# suffixed with a `^[a-z0-9][a-z0-9-]{0,31}$` label), so there is nothing here
# for JSON escaping to escape. That is a property of the filters, not luck --
# do not widen one without adding the escaping it removes the need for.
# Parameterised so the RETENTION path can reconstruct exactly what publication
# would have written for a given leg and run id, and compare a candidate
# pointer against it byte for byte. One emitter, so the two can never drift
# into disagreeing about what a valid pointer looks like.
utest_leg_pointer_doc_for() {
    printf '{\n'
    printf '  "schema": "utest-leg-pointer-v1",\n'
    printf '  "leg": "%s",\n' "$1"
    printf '  "run_id": "%s",\n' "$2"
    printf '  "record": "test-runs/%s",\n' "$2"
    printf '  "marker": "record-complete.json"\n'
    printf '}\n'
}

utest_leg_pointer_doc() {
    utest_leg_pointer_doc_for "$RUN_LEG" "$RUN_ID"
}

# Replace the pointer in ONE rename, and never as a member of the alias set.
#
# The set above is cleared and rewritten, which means every member is briefly
# absent by construction. That is the right trade for a compatibility surface
# -- absence is fail-closed and sends a consumer to the record -- but it is the
# wrong one for the resolution path itself: a consumer that finds no pointer
# has nowhere to resolve TO, and a death between the clear and the rewrite
# would leave it that way permanently.
#
# Outside the clear, the pointer is only ever replaced by a rename over a file
# that already names a COMPLETE, immutable, marker-committed record. So the
# only two states a reader can observe are "the previous generation" and "this
# generation", both internally coherent, and there is no window in which the
# leg resolves to nothing. A staged temp inside the record keeps the rename
# same-filesystem and keeps a partial write out of the published name.
utest_publish_leg_pointer() {
    local tmp="$RECORD_DIR/.legptr.tmp"
    [ -n "$RUN_POINTER_OUT" ] || return 0
    # `-T` is load-bearing, not tidiness. Plain `mv -f file dir` MOVES the file
    # INTO the directory and returns SUCCESS, so a directory sitting at the
    # pointer pathname would swallow the staged document, report a published
    # pointer, and count no failure -- while the pathname a consumer resolves
    # stays an unreadable directory, run after green run. The sweep cannot
    # clean that up either: it excludes directories by design, precisely so it
    # can never `rm -rf` something in build/. `-T` refuses the overwrite and
    # sends the whole thing down the counted-failure path below.
    if { utest_leg_pointer_doc > "$tmp"; } 2>/dev/null && mv -fT -- "$tmp" "$RUN_POINTER_OUT"; then
        return 0
    fi
    rm -f "$tmp"
    # A pointer that cannot be advanced is a counted failure, not a silent
    # one: the leg's documents now describe a run the resolution path does not
    # name, and a consumer resolving through it would read the PREVIOUS run and
    # be correct to. Saying so is what lets the operator tell that apart from
    # "this leg has not run yet".
    echo -e "  ${RED}[UTEST]${RESET} leg pointer $RUN_POINTER_OUT could not be advanced -- it still names the previous run"
    UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
    return 1
}

# Publish the per-leg alias SET as ONE generation, in two phases.
#
# The leg pair means "the latest COMPLETED run of this leg", so its two files
# must never come from different runs. Replacing them one at a time cannot
# hold that: with both formats present, whichever is written first sits beside
# the other's previous-generation file for the whole gap between the two
# operations, and a reader needs no pre-held descriptor to observe it. An
# earlier draft only dropped a format the record did NOT carry, which fixed
# the one-format case and left the two-format case exposed.
#
# Phase 1 stages every document this record carries, touching no stable name.
# If staging fails the previous COMPLETE set is left exactly as it was -- an
# intact older generation beats a torn new one. Phase 2 removes the whole old
# set, then moves the staged files into place. A reader therefore sees the
# previous generation, or missing files, or the new generation, never a pair
# drawn from two runs; missing is fail-closed and sends it to the record.
#
# A reader that already holds an open descriptor keeps reading its bytes
# regardless, and no scheme here can revoke that. That is why the record
# directory plus its marker -- not these aliases -- is the generation-coherent
# artifact, and why the per-leg POINTER published below resolves a consumer to
# that record rather than to these two paths.
utest_publish_leg_set() {
    local staged_x="" staged_j="" failed=0
    [ -n "$XML_LEG_OUT" ] || return 0

    if [ -f "$XML_RECORD" ]; then
        if cp -f "$XML_RECORD" "$RECORD_DIR/.xleg.tmp"; then
            staged_x="$RECORD_DIR/.xleg.tmp"
        else
            rm -f "$RECORD_DIR/.xleg.tmp"
            echo -e "  ${RED}[UTEST]${RESET} leg alias set NOT published: $XML_RECORD could not be staged (previous set left intact)"
            UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
            return 1
        fi
    fi
    if [ -f "$JSON_RECORD" ]; then
        if cp -f "$JSON_RECORD" "$RECORD_DIR/.jleg.tmp"; then
            staged_j="$RECORD_DIR/.jleg.tmp"
        else
            rm -f "$RECORD_DIR/.xleg.tmp" "$RECORD_DIR/.jleg.tmp"
            echo -e "  ${RED}[UTEST]${RESET} leg alias set NOT published: $JSON_RECORD could not be staged (previous set left intact)"
            UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
            return 1
        fi
    fi

    # Phase 2: the old set goes as a SET, so no new document is ever visible
    # beside an old one, and a format this record does not carry cannot
    # linger from an earlier run.
    #
    # The clear is CHECKED, not assumed. This function is invoked through
    # `|| true`, which suppresses errexit for everything inside it, so an
    # unchecked `rm -f` of two operands could unlink one and fail on the
    # other and publication would carry on regardless -- publishing the new
    # XML beside a surviving previous JSON, precisely the mixed pair the two
    # phases exist to prevent. If the old set cannot be made wholly absent,
    # nothing is published: the staged files are dropped and the run fails.
    # What remains is then all-previous or part-previous, never a pair drawn
    # from two runs, which is the property that actually matters here.
    rm -f "$XML_LEG_OUT" "$JSON_LEG_OUT" 2>/dev/null || true
    if [ -e "$XML_LEG_OUT" ] || [ -e "$JSON_LEG_OUT" ]; then
        rm -f "$staged_x" "$staged_j" 2>/dev/null || true
        echo -e "  ${RED}[UTEST]${RESET} leg alias set NOT published: the previous set could not be cleared"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
        return 1
    fi
    if [ -n "$staged_x" ] && ! mv -f "$staged_x" "$XML_LEG_OUT"; then
        rm -f "$staged_x" "$XML_LEG_OUT"
        echo -e "  ${RED}[UTEST]${RESET} leg alias $XML_LEG_OUT could not be written"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
        failed=1
    fi
    if [ -n "$staged_j" ] && ! mv -f "$staged_j" "$JSON_LEG_OUT"; then
        rm -f "$staged_j" "$JSON_LEG_OUT"
        echo -e "  ${RED}[UTEST]${RESET} leg alias $JSON_LEG_OUT could not be written"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
        failed=1
    fi

    # The pointer moves to this generation only if the generation actually
    # landed. A torn alias set is already a counted failure; advancing the
    # resolution path over it would additionally point every consumer at a
    # generation whose compatibility surface disagrees with it. Left alone,
    # the pointer keeps naming the PREVIOUS run's record -- which is complete,
    # immutable and marker-committed, so it is a coherent answer rather than a
    # stale one.
    # Written as a full `if` with an explicit `|| true`: the regressions extract
    # this function and run it under `set -euo`, where a bare trailing call that
    # returns non-zero would abort the caller rather than report a counted
    # failure.
    if [ "$failed" -eq 0 ]; then
        utest_publish_leg_pointer || true
    fi
    return 0
}

# The single observable commit point for the record directory.
#
# A directory that merely EXISTS is not a completed record: it is created
# before the run does anything, and a SIGKILL can leave it holding only an
# identity file, one of two requested formats, or a half-written staging file.
# This marker is written LAST, names exactly which documents the record
# actually contains, and is what a consumer must check before trusting any of
# them. Its own write goes through a temp plus `mv`, so the marker can never
# be observed partially written either.
utest_commit_record() {
    local status="$1" tmp="$RECORD_DIR/.marker.tmp" qpid="null"
    # A JSON number or `null`, never a bare empty field: an exit before the
    # launch has no pid to report, and every path from the environment preflight
    # onward can reach this function.
    case "${QEMU_PID:-}" in
        ''|*[!0-9]*) qpid="null" ;;
        *) qpid="${QEMU_PID}" ;;
    esac
    if {
        printf '{\n'
        printf '  "schema": "utest-run-record-v1",\n'
        printf '  "run_id": "%s",\n' "$RUN_ID"
        printf '  "status": "%s",\n' "$status"
        printf '  "qemu_pid": %s,\n' "$qpid"
        printf '  "qemu_state": "%s",\n' "${QEMU_STATE:-none}"
        printf '  "xml": %s,\n' "$([ -f "$XML_RECORD" ] && echo '"test-results.xml"' || echo 'null')"
        printf '  "json": %s,\n' "$([ -f "$JSON_RECORD" ] && echo '"test-results.json"' || echo 'null')"
        printf '  "identity": %s\n' "$([ -f "$IDENTITY_RECORD" ] && echo '"test-run-identity.json"' || echo 'null')"
        printf '}\n'
    } > "$tmp" 2>/dev/null && mv -f "$tmp" "$RECORD_MARKER" 2>/dev/null; then
        return 0
    fi
    # A marker that cannot land is NOT a cosmetic loss. It is the only thing
    # that tells a consumer the record is finished, so a run whose marker
    # failed must not report success: a green exit over an uncommitted record
    # is precisely the false-green this lifecycle exists to close.
    rm -f "$tmp"
    echo -e "  ${RED}[UTEST]${RESET} run record $RECORD_DIR could not be committed -- $RECORD_MARKER was not written"
    UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
    return 1
}

# The one place a record becomes consumable. Three things happen in a fixed
# order, and each gates the next:
#
#   1. Every format this run OWED but never published gets its refusal
#      document, so the record's contents are settled before anything
#      describes them. Skipping this let a marker say `"json": null` while the
#      EXIT guard afterwards wrote a run_incomplete document into the record
#      -- an immutable record disagreeing with its own commit marker.
#   2. The marker is written. If it cannot land, NO alias is published: a
#      stable pathname pointing into a record with no marker is exactly the
#      uncommitted-record exposure the marker exists to prevent.
#   3. The aliases are copied out, identity LAST. Per-file `mv` makes each
#      alias atomic but does not make the SET atomic, so identity is the
#      generation pointer: it carries `run_id`, it is published after the
#      documents, and a consumer that resolves through it and cross-checks
#      `run_id` against the document it read either sees one coherent
#      generation or detects the skew. The record plus marker remains the
#      authoritative pair; the aliases are a compatibility surface.
#
# Idempotent. The normal path calls it before the final verdict, so an alias
# or marker failure can still fail the run; the EXIT guard calls it again for
# every path that never got there.
# The one place QEMU is reaped, and it is called from TWO places on purpose.
#
# The cleanup trap owns the exit path. utest_finalize_record calls it as well,
# BEFORE the commit marker is written, because the normal path finalizes ahead of
# the verdict -- long before any trap runs. A marker that said `status:
# complete` while the VM it describes was still running would be the same class
# of false-green the rest of this lifecycle exists to close, and `qemu_pid`
# alone cannot distinguish "exited cleanly" from "never reaped".
#
# SIGTERM first (QEMU exits cleanly and flushes its serial sink), then SIGKILL,
# because "reaped" has to be a fact rather than a request. Only a confirmed-gone
# pid clears the pidfile: that file is what the NEXT run's detection reads to
# name the run that leaked a VM, so clearing it optimistically would erase the
# one clue an investigation has.
#
# Initialised here, ahead of the first artifact trap. QEMU_PID used to come into
# existence only at the launch, so every exit path before it -- the environment
# preflight, the build, the boot.conf patch -- reached a reap or a marker with
# the variable unset under `set -u`.
QEMU_PID=""
QEMU_STATE=none
utest_reap_qemu() {
    [ -n "${QEMU_PID:-}" ] || return 0
    # Already settled: the pid is a RECORD from here on, not a live handle.
    # This function is called twice on the normal path -- once from
    # utest_finalize_record, ahead of the marker, and again from the EXIT trap
    # -- and QEMU_PID deliberately survives the first call because the marker
    # reports it. Once the child has been `wait`ed, though, the kernel is free
    # to hand that number to something else, and a second entry that re-derives
    # liveness from `kill -0` alone would SIGTERM (then SIGKILL, on the timer)
    # whatever inherited it. `QEMU_STATE` is the fact the second call must read.
    # Written as an `if` rather than `[ ... ] && return 0`: the latter yields a
    # non-zero list status on the common path, which is a needless `set -e`
    # hazard in a function the regressions extract and run under `set -euo`.
    if [ "${QEMU_STATE:-none}" = "reaped" ]; then
        return 0
    fi
    # Defaulted HERE, not at file scope: under `set -u` an unset grace makes the
    # watchdog subshell die on its own `sleep`, which disarms the escalation
    # SILENTLY -- the failure mode is indistinguishable from a VM that exited on
    # SIGTERM. Keeping it inside also keeps the function self-contained, which
    # is what lets the regressions extract and exercise it directly.
    local grace="${UTEST_REAP_GRACE:-5}" watchdog=""
    # VALIDATED, because an unusable grace is worse than a wrong one: `sleep`
    # rejects it, the watchdog subshell (which inherits this script's `set -e`)
    # dies before reaching its escalation, and the parent then blocks in the
    # unbounded `wait` forever -- the exact hang the timer exists to prevent,
    # reachable from a single typo'd environment variable. Anything that is not
    # a positive number falls back to the default rather than disarming the
    # deadline.
    # Shape first (digits and at most one dot), then VALUE: `0`, `00`, `.0` and
    # `0.0` all pass a shape check while meaning "no grace at all", which turns
    # the graceful SIGTERM into an immediate SIGKILL and costs the VM its
    # chance to flush the serial sink. The contract is a strictly positive
    # number, so enforce that and not merely its spelling.
    case "$grace" in
        ''|*[!0-9.]*|.|*.*.*)   grace=5 ;;
        *[1-9]*)                : ;;     # contains a non-zero digit -> positive
        *)                      grace=5 ;;
    esac
    # BOUNDED as well as positive. A deadline is only a deadline if it arrives:
    # `UTEST_REAP_GRACE=99999999` passes every shape and value check above and
    # still leaves the run parked in its `wait` for a year, which is the same
    # hang the timer exists to prevent, reached by a different typo. 120s is far
    # beyond any legitimate QEMU shutdown and still terminates a run.
    if [ "${grace%%.*}" -gt 120 ] 2>/dev/null; then
        echo -e "${YELLOW}[TEST]${RESET} UTEST_REAP_GRACE=$grace exceeds the 120s bound -- using 120."
        grace=120
    fi
    # PUBLISH the resolved grace. It is a `local`, so the only way a caller
    # could previously tell that a bad value had been substituted was to TIME
    # the reap -- and the regressions that did (`elapsed >= 4` against the 5s
    # default) are load-sensitive: measured 2026-07-31, one failed on a
    # saturated host and passed on an immediate quiet rerun with no change, so
    # the suite reported red on correct code. Two independent integer-second
    # truncations ($SECONDS in the harness, the deadline arithmetic here) can
    # each shed most of a second. Exporting the decision makes the assertion
    # exact and load-independent: the test reads what the function DECIDED
    # instead of inferring it from a stopwatch.
    UTEST_REAP_GRACE_RESOLVED="$grace"
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        # Identity captured SYNCHRONOUSLY, in the parent, BEFORE the SIGTERM.
        # Read inside the timer instead, it is read after the signal and after
        # a scheduling gap: a VM that exits promptly can be reaped and its
        # number reused before that read happens, at which point the recorded
        # identity describes the SUCCESSOR and the later comparison "matches"
        # the wrong process. Capturing here means the value provably belongs to
        # the VM this call was asked to end. An empty read means the process is
        # already gone, and the timer stands down rather than guessing.
        local st0
        st0="$(awk '{sub(/^.*\) /, ""); print $20}' \
                "/proc/$QEMU_PID/stat" 2>/dev/null)"
        kill "$QEMU_PID" 2>/dev/null || true
        # The escalation is ARMED BEFORE the wait, not sequenced after it.
        # `wait` on a child is UNBOUNDED, and the whole point of the SIGKILL is
        # the case where SIGTERM is never processed -- a QEMU wedged in device
        # emulation. Written as "SIGTERM; wait; then SIGKILL" the escalation is
        # unreachable in exactly that case: the wait never returns, so the run
        # hangs forever holding the run lock with boot.conf still patched and
        # the record unfinalised. A background timer makes the SIGKILL a
        # deadline rather than a follow-up.
        # `9>&-` for the SAME reason the QEMU launch carries it: this subshell
        # would otherwise inherit the run-lock descriptor, and killing it
        # orphans its `sleep`, which keeps that descriptor -- and therefore the
        # lock -- open for the rest of the grace. Measured: the next run was
        # refused with "another run of this script holds build/.test-run.lock"
        # for seconds after the previous one had exited. That is exactly the
        # inherited-descriptor hazard section 30 closed at the launch, walked
        # back in through the timer.
        # `set +e` and `|| true` make the escalation FAIL-SAFE independently of
        # the validation above: whatever goes wrong with the delay, the SIGKILL
        # still runs. The subshell inherits `set -e` from the script, so a
        # non-zero `sleep` would otherwise abandon the kill and strand the
        # parent in its wait.
        #
        # But "a failed delay still escalates" cuts both ways, and cancellation
        # works by INTERRUPTING that very delay -- so a blanket `sleep || true`
        # turns every cancellation into an immediate escalation, aimed at a pid
        # the parent has just `wait`ed and released.
        #
        # The delay's exit status is the discriminator, and it is trustworthy
        # only because `grace` is validated above: a positive number cannot make
        # `sleep` fail on its argument, so a non-zero status means it was
        # SIGNALLED, which is precisely cancellation. The deadline therefore
        # escalates when the delay COMPLETES and stands down when it is cut
        # short. No cancellation token, no file, no shared path -- there is
        # nothing for a concurrent run to collide with or for a symlink to
        # redirect.
        #
        # `starttime` is then re-read as the identity proof: ticks-since-boot at
        # process start is unique to a process, so a recycled number never
        # matches. It is the same proof qemu-orphan.py requires before it
        # signals, applied to the one signal path still holding a raw pid. An
        # unreadable /proc entry means the process is already gone, which is
        # also a reason not to signal.
        (
            set +e
            [ -n "$st0" ] || exit 0
            sleep "$grace" || exit 0
            st1="$(awk '{sub(/^.*\) /, ""); print $20}' \
                    "/proc/$QEMU_PID/stat" 2>/dev/null)"
            [ -n "$st1" ] && [ "$st0" = "$st1" ] || exit 0
            kill -9 "$QEMU_PID" 2>/dev/null
        ) 9>&- &
        watchdog=$!
        # Safe against the normal path too: QEMU is our child and the poll loop
        # never reaps it, so an exited VM is a ZOMBIE here -- `wait` returns at
        # once and the timer is cancelled before it ever fires.
        wait "$QEMU_PID" 2>/dev/null || true
        # End the `sleep` as well as the subshell around it: ending the parent
        # alone leaves the sleep running to term as an orphan. Ending the sleep
        # is ALSO what cancels the deadline -- see the exit-status rule above.
        for _wdkid in $(cat "/proc/$watchdog/task/$watchdog/children" 2>/dev/null); do
            kill "$_wdkid" 2>/dev/null || true
        done
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
        # `wait` RETURNING for our own child is itself the proof it is gone --
        # that is what `wait` means. Re-probing with `kill -0` afterwards asks
        # about a NUMBER the kernel has already released, so a pid reused in
        # the interval reports "alive" and this run records `unreaped` about an
        # unrelated process (and, worse, tells its marker the VM outlived it).
        QEMU_STATE=reaped
        if [ -n "${UTEST_QEMU_PIDFILE:-}" ]; then
            rm -f "$UTEST_QEMU_PIDFILE" 2>/dev/null || true
        fi
        return 0
    fi
    # Not alive at entry and not previously reaped: it exited on its own and
    # something else already collected it. Nothing to signal, nothing to probe.
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        QEMU_STATE=unreaped
        return 0
    fi
    QEMU_STATE=reaped
    if [ -n "${UTEST_QEMU_PIDFILE:-}" ]; then
        rm -f "$UTEST_QEMU_PIDFILE" 2>/dev/null || true
    fi
    return 0
}

UTEST_FINALIZED=0
utest_finalize_record() {
    local status="$1"
    [ "${UTEST_FINALIZED:-0}" -eq 0 ] || return 0
    UTEST_FINALIZED=1
    # Settle the VM before the record describes the run -- see utest_reap_qemu.
    utest_reap_qemu
    # An UNREAPED VM is a run-level failure, not a field in a document nobody
    # reads. The whole point of settling before the marker is that `complete`
    # must not describe a run whose VM outlived it -- so if the VM survived even
    # the deadline escalation, say so where it will be seen, and downgrade the
    # status the record claims.
    if [ "${QEMU_STATE:-none}" = "unreaped" ]; then
        echo -e "  ${RED}[UTEST]${RESET} QEMU pid ${QEMU_PID:-?} survived SIGTERM and the escalation -- this run is NOT clean."
        echo -e "  ${YELLOW}       The next run will detect it as an orphan and refuse (or reap with UTEST_ORPHAN_REAP=1).${RESET}"
        status="incomplete"
    fi
    utest_publish_missing_refusals "${UTEST_FINALIZE_WHY:-the run ended before the artifact was assembled}"
    if ! utest_commit_record "$status"; then
        echo -e "  ${RED}[UTEST]${RESET} record not committed -- publishing no aliases; read $RECORD_DIR directly"
        return 1
    fi
    # Leg aliases mean "the latest COMPLETED run of this leg", so only a
    # complete record may replace one. An incomplete run publishes to the
    # canonical pair alone -- that names THIS invocation and is supposed to
    # say the run did not finish -- and leaves the previous leg's completed
    # artifact where a matrix consumer can still find it.
    # Only a COMPLETE record may touch the leg set, and it replaces the whole
    # set at once. An incomplete run publishes to the canonical pair alone --
    # that names THIS invocation and is supposed to say the run did not
    # finish -- leaving the previous leg's completed artifacts where a matrix
    # consumer can still find them.
    if [ "$status" = "complete" ]; then
        utest_publish_leg_set || true
    fi
    utest_alias_record "$XML_RECORD" "$XML_OUT" x
    utest_alias_record "$JSON_RECORD" "$JSON_OUT" j
    utest_alias_record "$IDENTITY_RECORD" "$IDENTITY_OUT" i
    return 0
}

# Every XML refusal on this path emits the SAME document shape: a one-testcase
# <testsuite> carrying errors="1" and aborted="true", differing only in the
# testcase name and the error message. It was written out five times inline,
# which is five places for the shape to drift and -- more to the point -- five
# places no test could reach. As a function it is emitted once and can be
# driven directly by scripts/test-tooling.sh.
#
# Writes to stdout; the caller redirects. `errors="1"` rather than a failure
# count is what makes a JUnit consumer that never reads our exit code show red.
utest_xml_refusal_doc() {
    local case_name="$1" message="$2"
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    printf '<testsuite name="impossible-os-usermode" tests="1" failures="0" skipped="0" errors="1" time="0"%s>\n' \
        "$(utest_xml_identity_attrs)"
    echo '  <properties>'
    echo '    <property name="aborted" value="true"/>'
    echo '    <property name="not_run" value="0"/>'
    utest_xml_identity_props
    echo '  </properties>'
    printf '  <testcase name="%s" classname="infrastructure">\n' "$case_name"
    printf '    <error message="%s"/>\n' "$message"
    echo '  </testcase>'
    echo '</testsuite>'
}

# Stage a refusal document privately, publish it to the record and both
# aliases, and mark XML published so a later guard cannot overwrite it.
# Returns 0 regardless: a refusal that cannot be written must not, under
# `set -e`, abort the very cleanup path that was reporting the problem.
utest_publish_xml_refusal() {
    local case_name="$1" message="$2" staged="$RECORD_DIR/.refusal.xml"
    if utest_xml_refusal_doc "$case_name" "$message" > "$staged" 2>/dev/null; then
        utest_publish "$staged" "$XML_RECORD" && XML_PUBLISHED=1
    fi
    rm -f "$staged"
    return 0
}

# Add each binary's captured stdout to the assembled document as <system-out>,
# and its launcher diagnostic as <system-err> (section 36). STRUCTURAL, not
# textual: utest-capture.py parses and re-serializes the document, because the
# assembler above keeps only physical lines beginning with `<testcase` -- a
# hand-spliced multi-line child would be silently dropped by the very next run,
# and hand-escaping a payload that may contain `&` or `<` is a second way to
# emit an invalid artifact.
#
# Returns 0 in every degraded case so the `&&` publish chain still runs: by the
# time this is called the payload has ALREADY been reconciled (a corrupt one
# took the refusal branch and never reached assembly), so a tool fault here
# costs the artifact its captured-output field, not its validity.
utest_splice_capture() {
    local doc="$1" staged="$RECORD_DIR/.spliced.xml" rc=0
    [ "${CAPTURE_ATTACH:-0}" -eq 1 ] || return 0
    [ -s "$CAPTURE_MODEL" ] || return 0
    python3 "$PROJECT/scripts/utest-capture.py" splice-xml \
        "$doc" "$CAPTURE_MODEL" "$staged" >/dev/null 2>&1 || rc=$?
    if [ "$rc" -eq 0 ] && [ -s "$staged" ]; then
        mv -f "$staged" "$doc"
        return 0
    fi
    rm -f "$staged"
    if [ "$rc" -eq 2 ]; then
        # exit 2 means the splicer could not PARSE the document this script just
        # assembled, or could not write its output. Publishing the input anyway
        # would ship an artifact already proven to be invalid JUnit -- the
        # failure was detected and then discarded. Refuse instead.
        echo -e "  ${RED}[UTEST]${RESET} assembled XML is not parseable -- refusing to publish it"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
        utest_publish_xml_refusal "artifact-pipeline" \
            "assembled JUnit document failed to parse during captured-output splice"
        return 1
    fi
    # exit 1 is a population disagreement: the capture model names a binary the
    # testcase set does not contain, so a VERDICT went missing between two views
    # of one run. That makes the document untrustworthy, not merely
    # capture-less -- publishing it "without <system-out>" would leave JUnit
    # green while the JSON side refuses the same run, which is exactly the
    # divergence the shared model exists to prevent. Refuse both.
    echo -e "  ${RED}[UTEST]${RESET} captured-output population drift -- refusing to publish the XML artifact"
    UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
    utest_publish_xml_refusal "capture-population" \
        "capture model names binaries with no testcase -- a verdict is missing from this run"
    return 1
}

# Tracked PER FORMAT. One flag for both would let a signal landing between the
# XML publication and the JSON assembly suppress the JSON refusal, leaving a
# requested artifact simply absent with nothing saying why.
XML_PUBLISHED=0
JSON_PUBLISHED=0

# Set by the INT/TERM handlers so a refusal can say what actually happened. A
# signalled run reaches the trap with $? still 0, and a document reading
# "exited with status 0" while carrying errors="1" is exactly the kind of
# self-contradicting artifact the rest of this pipeline refuses to publish.
UTEST_SIGNALLED=0

# Give every format this run OWED but never published its refusal document.
# Called from utest_finalize_record BEFORE the marker, so the record's
# contents are settled before the marker describes them.
#
# Only a format this run would have produced owes a document. Publishing a
# refusal for an artifact nobody asked for is noise, not honesty.
UTEST_FINALIZE_WHY="the run ended before the artifact was assembled"
utest_publish_missing_refusals() {
    local why="$1" staged
    if [ "${XML_MODE:-0}" -eq 1 ] || [ "${HAS_XML:-0}" -eq 1 ]; then
        if [ "${XML_PUBLISHED:-0}" -eq 0 ]; then
            utest_publish_xml_refusal "run-incomplete" "$why"
        fi
    fi

    if [ "${JSON_MODE:-0}" -eq 1 ] || [ "${HAS_JSON:-0}" -eq 1 ]; then
        if [ "${JSON_PUBLISHED:-0}" -eq 0 ]; then
            staged="$RECORD_DIR/.refusal.json"
            if {
                printf '{"schema": "utest-json-v2", "testcases": [], "skip_blocks": [],\n'
                printf ' "summary": null, "summary_error": "run_incomplete",\n'
                printf ' "detail": "%s",\n' "$why"
                printf ' "run_identity": '
                utest_json_identity
                printf '}\n'
            } > "$staged" 2>/dev/null; then
                utest_publish "$staged" "$JSON_RECORD" && JSON_PUBLISHED=1
            fi
            rm -f "$staged"
        fi
    fi
    return 0
}

utest_artifact_guard() {
    local ec="${1:-0}"
    if [ "${UTEST_SIGNALLED:-0}" -eq 1 ]; then
        UTEST_FINALIZE_WHY="interrupted by a signal before the artifact was assembled"
    else
        UTEST_FINALIZE_WHY="the run exited with status $ec before the artifact was assembled"
    fi
    # Settle the record, name it, alias it. A no-op when the normal path
    # already finalized; the whole sequence for every path that never did.
    utest_finalize_record "$([ "$ec" -eq 0 ] && echo complete || echo incomplete)" || true
    return 0
}

# Armed before the environment preflight. The boot.conf `cleanup` handler
# below REPLACES these (bash keeps one handler per signal), and calls the same
# guard. INT and TERM carry their conventional 128+signo status: a cancelled
# run that exited 0 would be reported as a SUCCESS by every caller, including
# CI, no matter what its artifact said.
trap 'utest_artifact_guard $?' EXIT
trap 'UTEST_SIGNALLED=1; utest_artifact_guard 130; exit 130' INT
trap 'UTEST_SIGNALLED=1; utest_artifact_guard 143; exit 143' TERM

# Retention runs HERE, not at record creation: it is fallible (a bad
# UTEST_RECORD_KEEP, an unreadable directory) and everything above this line
# is what makes a failure safe -- the canonical aliases are already cleared
# and the guard is already armed, so an abort in retention leaves a document
# belonging to this run rather than the previous run's success.
utest_prune_records

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

# --- Step 0b: Run identity ---
#
# The accelerator is settled, so the leg can be named. Everything here is
# derived from what the run OBSERVED; nothing is taken on an operator's word.

# RUN_TS was taken in Step 0a, when the run actually started, so that a
# refusal published before this point can still be placed in time. It is
# xs:dateTime (the type the JUnit `timestamp` attribute carries) with the
# trailing Z: a local naive timestamp cannot be ordered against another leg
# that ran in a different zone, which is most of the point of carrying one.

RUN_COMMIT="$(git -C "$PROJECT" rev-parse HEAD 2>/dev/null | tr -cd 'a-f0-9')"
if [ -z "$RUN_COMMIT" ]; then
    RUN_COMMIT="unknown"
elif [ -n "$(git -C "$PROJECT" status --porcelain 2>/dev/null)" ]; then
    # UNTRACKED files count. The Makefile discovers sources with `find`
    # (ASM_SRCS/C_SRCS), so an untracked .c or .asm is compiled INTO the image
    # under test -- attributing that run to the bare SHA would make an
    # unreproducible result look commit-reproducible. `git diff --quiet HEAD`
    # misses exactly that case, which is why this uses status --porcelain.
    RUN_COMMIT="${RUN_COMMIT}-dirty"
fi

# Read back from the arguments QEMU will actually receive, not from ACCEL_NAME
# (human prose) -- the artifact must report what ran.
case "$ACCEL_ARGS" in
    *"-accel kvm"*) RUN_ACCEL="kvm" ;;
    *)              RUN_ACCEL="tcg" ;;
esac

# One variable behind both the -smp flag and the leg name. While the flag was
# a bare literal, any identifier naming a CPU count was an assertion nothing
# kept true. It is validated because it reaches a filename, an XML attribute
# and an UNQUOTED JSON number: `SMP_CPUS=2x` would publish invalid JSON, and a
# value carrying a slash would invent path components.
SMP_CPUS="${SMP_CPUS:-2}"
case "$SMP_CPUS" in
    ''|*[!0-9]*) SMP_CPUS_VALID=0 ;;
    *)           SMP_CPUS_VALID=1 ;;
esac
if [ "$SMP_CPUS_VALID" -eq 1 ] && [ "$SMP_CPUS" -ge 1 ] && [ "$SMP_CPUS" -le 256 ]; then
    SMP_CPUS_SAFE="$SMP_CPUS"
else
    echo -e "${RED}[TEST]${RESET} SMP_CPUS='$SMP_CPUS' is not a CPU count (expected 1-256)."
    exit 1
fi

# QEMU_BIN is caller-supplied and its basename reaches both artifact formats.
# Filtering to the same charset the other identity fields use is what lets
# every emitter below skip escaping.
RUN_QEMU="$(basename "$QEMU_BIN" | tr -cd 'A-Za-z0-9._-')"
[ -n "$RUN_QEMU" ] || RUN_QEMU="unknown"

if [ -n "${WSL_DISTRO_NAME:-}" ] || grep -qi microsoft /proc/version 2>/dev/null; then
    RUN_HOST="wsl2"
else
    RUN_HOST="$(uname -s 2>/dev/null | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9')"
    [ -n "$RUN_HOST" ] || RUN_HOST="unknown"
fi

RUN_HOSTNAME="$(uname -n 2>/dev/null | tr -cd 'A-Za-z0-9._-')"
[ -n "$RUN_HOSTNAME" ] || RUN_HOSTNAME="unknown"

RUN_LEG="${RUN_HOST}-${RUN_ACCEL}-${SMP_CPUS_SAFE}cpu"
[ "${CI_PARITY:-0}" = "1" ] && RUN_LEG="${RUN_LEG}-ciparity"
RUN_LEG_SOURCE="derived"

# UTEST_LEG may ADD a label; it may not redefine what ran. This script can only
# ever select KVM or TCG, so an override that renamed a leg to whpx, vbox or
# baremetal would publish coverage nothing executed. The derived accelerator
# and CPU count stay in the name and in their own fields either way, and the
# provenance is recorded so a consumer can see a human had a hand in it.
if [ -n "${UTEST_LEG:-}" ]; then
    if printf '%s' "$UTEST_LEG" | grep -qE '^[a-z0-9][a-z0-9-]{0,31}$'; then
        RUN_LEG="${RUN_LEG}-${UTEST_LEG}"
        RUN_LEG_SOURCE="derived+override"
    else
        # Refuse rather than ignore: the value lands in a filename, and a
        # silently dropped override publishes under a name the caller does
        # not expect -- which is the collision this whole step exists to stop.
        echo -e "${RED}[TEST]${RESET} UTEST_LEG='$UTEST_LEG' is not a valid leg label."
        echo -e "${YELLOW}       Allowed: ^[a-z0-9][a-z0-9-]{0,31}$ -- 1-32 chars, first alphanumeric.${RESET}"
        exit 1
    fi
fi

# Leg-suffixed aliases accumulate, one per configuration, and mean "the latest
# COMPLETED run of this leg". Only THIS leg's alias is cleared, never another
# configuration's: a matrix that runs several legs in sequence must still find
# every leg readable at the end, which is the contract these aliases were
# introduced to provide. They are NOT cleared here either.
#
# An earlier draft cleared this leg's own alias up front, reasoning that an
# absent alias is more honest than a stale one. It is not, once the alias
# means "latest COMPLETED": a run that then died left NO leg artifact even
# though a perfectly good completed run existed a minute earlier, and an
# aborted run's refusal document replaced it outright. Both are impossible
# now -- utest_finalize_record replaces a leg alias only from a record whose
# marker says `complete` -- and every run stays retrievable under its own run
# id in build/test-runs/ regardless.
#
# The canonical unsuffixed pair means something different: THIS invocation. It
# was already cleared in Step 0a, ahead of every exit path, so no preflight
# exit can leave a stale document under the name a consumer reads as current,
# and an incomplete run publishes its refusal there and nowhere else.
XML_LEG_OUT="$PROJECT/build/test-results-${RUN_LEG}.xml"
JSON_LEG_OUT="$PROJECT/build/test-results-${RUN_LEG}.json"
# The per-leg GENERATION POINTER. The two aliases above are a compatibility
# surface a consumer must enumerate; this one file is the resolution path,
# replaced by a single rename per completed run and naming the immutable
# record that run committed. See utest_publish_leg_pointer.
RUN_POINTER_OUT="$PROJECT/build/test-results-${RUN_LEG}.run"

# The identity file lands in the RECORD now; its alias is published by
# utest_finalize_record() along with the documents, after the commit marker.
# The staging path is inside the record, so it is not shared with any other
# invocation.
{ utest_json_identity; printf '\n'; } > "$RECORD_DIR/.identity.tmp" &&
    mv -f "$RECORD_DIR/.identity.tmp" "$IDENTITY_RECORD"

# --- Step 1: Build ---
echo -e "${CYAN}${BOLD}[TEST]${RESET} Building kernel..."
if ! bash "$PROJECT/scripts/build.sh" > /dev/null 2>&1; then
    echo -e "${RED}${BOLD}[FAIL]${RESET} Build failed. Check build/build.log"
    exit 1
fi
echo -e "${GREEN}[TEST]${RESET} Build OK"

# --- Step 2: Patch boot.conf for test mode ---
#
# An ARRAY, not a space-joined string. The values here are user-supplied and
# one of them is a GLOB: a scalar expanded unquoted as `$PATCH_ARGS` is
# pathname-expanded against the repository first, so `UTEST_FILTER=*` would
# reach the patcher as a list of repo filenames -- silently writing some
# arbitrary filename as the filter and shifting every later key/value pair.
# The array keeps each element exactly as typed.
PATCH_ARGS=(test 1)
if [ -n "$SUITE_CATEGORY" ]; then
    PATCH_ARGS+=(test_suite "$SUITE_CATEGORY")
fi
# test_quiet is a small enum (include/kernel/test/test.h TEST_QUIET_*), not a
# flag: 1 suppresses PASS lines, 2 additionally emits the per-suite [COUNT]
# trace that scripts/test-count-stability.sh consumes. The trace implies quiet
# because a verbose run does not finish inside the timeout below.
if [ "$COUNT_TRACE" -eq 1 ]; then
    PATCH_ARGS+=(test_quiet 2)
elif [ "$QUIET_MODE" -eq 1 ]; then
    PATCH_ARGS+=(test_quiet 1)
fi
if [ "$XML_MODE" -eq 1 ]; then
    PATCH_ARGS+=(xml 1)
fi
if [ "$JSON_MODE" -eq 1 ]; then
    PATCH_ARGS+=(json 1)
fi
if [ "$TAP_MODE" -eq 1 ]; then
    PATCH_ARGS+=(tap 1)
fi
if [ -n "$UTEST_FILTER" ]; then
    PATCH_ARGS+=(utest_filter "$UTEST_FILTER")
fi
bash "$PROJECT/scripts/patch-boot-conf.sh" "${PATCH_ARGS[@]}" > /dev/null

# Install cleanup trap now that boot.conf is patched. Runs on normal exit,
# SIGINT (Ctrl-C), or SIGTERM, restoring boot.conf and killing any backgrounded
# QEMU process. Preserves the caller's exit code.
cleanup() {
    local ec="${1:-$?}"
    # Run EXACTLY once. `exit` below re-enters the EXIT trap, and a signal
    # arriving mid-publication would otherwise re-enter from the top -- either
    # way the guard could overwrite a document it had just published.
    trap - EXIT INT TERM
    # One reap implementation, shared with utest_finalize_record: this handler
    # and the marker must never disagree about whether the VM is gone.
    utest_reap_qemu
    bash "$PROJECT/scripts/patch-boot-conf.sh" reset > /dev/null 2>&1 || true
    # This handler replaced the Step 0 EXIT trap, so it owns the artifact
    # obligation too: an interrupt or an early exit past this point must still
    # leave a document belonging to THIS run.
    utest_artifact_guard "$ec"
    exit "$ec"
}
# These REPLACE the Step 0a handlers and carry the same contract: the status
# is passed explicitly, because a signal handler reaches cleanup with $? still
# 0 and a cancelled run reported as exit 0 is a green CI step over an
# artifact that says the run never finished.
trap 'cleanup $?' EXIT
trap 'UTEST_SIGNALLED=1; cleanup 130' INT
trap 'UTEST_SIGNALLED=1; cleanup 143' TERM

# --- Step 3: Boot QEMU headless ---
cp "$OVMF_VARS" "$OVMF_VARS_CP"
rm -f "$TEST_LOG"


echo -e "${CYAN}[TEST]${RESET} Booting QEMU headless (${ACCEL_NAME}, ${TIMEOUT}s timeout)..."

# Record the pid the moment there is one, in two places for two readers.
#
# The run record's copy is durable and pruned with the record; it is also the ONLY
# copy that survives the SIGKILL which prevents any commit marker from ever being
# written, which is exactly the case a later investigation needs. The shared
# pidfile is what the NEXT run reads to name the run that leaked a VM.
#
# Neither is authority to send a signal. That is decided by live descriptor
# ownership in qemu-orphan.py, re-checked immediately before the kill, so a stale
# copy of either file is inert rather than dangerous -- a pid is a reused name,
# not an identity.
utest_record_qemu_pid() {
    local ident
    ident="$(python3 "$PROJECT/scripts/qemu-orphan.py" identity --pid "$QEMU_PID" 2>/dev/null || true)"
    [ -n "$ident" ] || ident="$QEMU_PID - -"
    printf '%s %s\n' "$ident" "$RUN_ID" > "$RECORD_DIR/qemu.pid" 2>/dev/null || true
    printf '%s %s\n' "$ident" "$RUN_ID" > "$UTEST_QEMU_PIDFILE" 2>/dev/null || true
    return 0
}

# Parent-death reaping is armed in the KERNEL rather than in a trap, because a
# SIGKILLed wrapper runs no handler at all -- and the VM it leaves behind holds
# this tree's boot state until the next run notices.
#
# The capability is probed HERE, before the launch, so its warning reaches the
# operator's terminal instead of disappearing into the launch's own 2>/dev/null.
UTEST_PDEATHSIG=1
if ! python3 "$PROJECT/scripts/pdeathsig.py" --check --exe "$QEMU_BIN" 2>/dev/null; then
    UTEST_PDEATHSIG=0
    echo -e "${YELLOW}[TEST]${RESET} parent-death reaping NOT enforced this run (prctl unavailable, or $QEMU_BIN is set-id / carries file capabilities)."
    echo -e "${YELLOW}       A SIGKILLed wrapper can leave a live VM; the next run detects and names it.${RESET}"
fi

# Launch QEMU in background -- kernel continues to desktop after tests,
# so we poll for the summary line and kill QEMU once we have results.
#
# An ARRAY for the same reason PATCH_ARGS is one: the argv is built once and
# launched through one of two shapes, and a space-joined string would re-split
# and glob every element at each use site. ACCEL_ARGS stays unquoted (it is an
# internally derived word list, e.g. `-accel kvm -cpu host`), exactly as before.
QEMU_ARGV=("$QEMU_BIN"
    $ACCEL_ARGS
    -smp "$SMP_CPUS"
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE"
    -drive if=pflash,format=raw,file="$OVMF_VARS_CP"
    -drive id=disk0,file="$DISK",format=raw,if=none
    -device ich9-ahci,id=ahci0
    -device ide-hd,drive=disk0,bus=ahci0.0
    -m "$TEST_MEM"
    -serial file:"$TEST_LOG"
    -display none
    -device rtl8139,netdev=net0
    -netdev user,id=net0
    -device virtio-tablet-pci
    -rtc base=localtime
    -no-reboot)

# pdeathsig.py EXECS the real QEMU, so the pid `$!` reports is QEMU's own and the
# reap path, the poll's `kill -0`, and the closed lock descriptor all behave
# exactly as they did without the wrapper. `--parent $$` is what closes the
# fork-then-prctl race: the helper refuses to exec at all if the shell that
# launched it is already gone, which is the one case where exec'ing WOULD create
# the orphan this is meant to prevent.
#
# `--provenance` makes the helper publish the shared pidfile ITSELF, before it
# execs QEMU. The parent cannot do that in time: it only learns the pid after
# the fork, so its own write (still made below, as the no-pdeathsig fallback)
# lands with the VM already running. A wrapper `SIGKILL`ed in that window used
# to leave a VM this tree genuinely launched but could no longer PROVE it had,
# which the provenance-gated recovery path must then refuse to reap.
if [ "$UTEST_PDEATHSIG" -eq 1 ]; then
    python3 "$PROJECT/scripts/pdeathsig.py" --parent "$$" \
        --provenance "$UTEST_QEMU_PIDFILE" --run-id "$RUN_ID" \
        -- "${QEMU_ARGV[@]}" 2>/dev/null 9>&- &
else
    "${QEMU_ARGV[@]}" 2>/dev/null 9>&- &
fi
QEMU_PID=$!
QEMU_STATE=running
utest_record_qemu_pid

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
# Learn this boot's launcher frame from the announcement, echoing the
# subsystem prefix every launcher-owned record carries (or nothing).
#
# Shared by the boot-completion poll and the Step 5b gates so both agree on
# what "framed" means. The nonce lives in the TAG and nowhere else -- the
# announcement body deliberately does not restate it, because a body copy
# would survive the kernel's disk-sink alias and put the value back into the
# one log ring 3 can read. The FIRST announcement wins: the launcher
# announces before it creates any ring-3 task, so nothing of that run can
# have printed ahead of it.
#
# It has to be learned, not pattern-matched. A poll that accepted any
# `UTEST-[0-9a-f]{8}:` prefix would accept a GUESSED one -- which is not
# hypothetical: test_forge.exe prints `UTEST-deadbeef: [UTEST-FRAME-END]`,
# and against a generic pattern that line ended the run early, reopening
# the exact hole the framing closes.
utest_frame_scan() {
    local log="$1"
    [ -f "$log" ] || { echo '{}'; return 0; }
    python3 "$PROJECT/scripts/utest-frame.py" "$log" 2>/dev/null || true
}

# The same question, asked once per second against a GROWING capture.
#
# The plain scan above re-reads from byte zero every time, so the poll's cost
# scaled with the whole serial log rather than with what arrived since the
# last answer -- on a 25 MB capture that is the dominant host-side cost of the
# completion gate. `--state` resumes from the previous byte offset and
# `--poll` drops the selected run's TEXT (the poll only needs to know whether
# a run closed), which keeps the once-per-second state file small.
#
# It is not a weaker check. The parser persists its COMPLETE state machine and
# binds it to the log's device/inode and committed offset: a rotated,
# recreated or truncated log fails that binding and restarts from byte zero,
# and every sticky failure (a foreign nonce, an unreconciled close) survives
# the resume. scripts/utest-frame-difftest.py asserts equivalence with a full
# reparse on every append prefix, byte by byte.
UTEST_POLL_STATE="${TEST_LOG}.framestate"
utest_frame_poll() {
    local log="$1" budget="${2:-1}" out rc
    [ -f "$log" ] || { echo '{}'; return 0; }
    # Bounded by the CALLER's remaining deadline budget, not left to run to
    # completion regardless of size: a poll loop that only checks the
    # deadline between calls still lets one call itself run unboundedly long
    # on a large capture, which is exactly the "counts sleep calls, not wall
    # time" bug this section's whole poll-deadline rework exists to close.
    # `if out=$(...); then rc=0; else rc=$?; fi`, NOT a plain assignment
    # statement followed by `rc=$?` on the next line: this file runs under
    # `set -euo pipefail` (line 17), and a bare `out="$(cmd)"` statement IS
    # subject to errexit when `cmd`'s exit status is nonzero -- confirmed:
    # `set -e; out="$(bash -c 'exit 2')"; echo unreached` never reaches the
    # echo, with no `inherit_errexit` shopt involved. utest-frame.py's exit
    # 1 (framed, unreconciled) and 2 (no frame) are the ROUTINE answer for
    # most of a poll's lifetime, so a plain assignment here would abort the
    # entire test harness on the very first ordinary "not complete yet"
    # poll. The `if`/`else` form is one of the contexts `set -e` exempts
    # (an if-condition), which is what the original pre-review function
    # relied on via `cmd || true` before this poll gained a captured rc.
    if out="$(timeout "$budget" python3 "$PROJECT/scripts/utest-frame.py" \
        --state "$UTEST_POLL_STATE" --poll "$log" 2>/dev/null)"; then
        rc=0
    else
        rc=$?
    fi
    # utest-frame.py's module docstring documents exactly three CLEAN exit
    # codes: 0 (complete, reconciled run), 1 (framed but unreconciled), 2 (no
    # frame learned). Any other code -- `timeout`'s 124 when it had to KILL
    # the child, a shell-level 125/126/127, a signal death (128+N, e.g. 137
    # for SIGKILL), or a crash this list never anticipated -- means the
    # process never reached its own documented exit path, so its stdout
    # proves NOTHING about frame presence. Checking the exit code allowlist
    # alone is not enough either: a kill can land after a PARTIAL write even
    # under an accidentally-clean-looking status, so the output is also
    # required to look like a complete JSON object. Both failure modes map
    # to the SAME inconclusive marker, because the caller's correct response
    # is identical either way: this poll answered nothing, so treat it as
    # "not yet", never silently as "no frame" -- the false-green a single
    # `-eq 124` check closed only for ONE of the ways this can happen.
    case "$rc" in
    0 | 1 | 2) ;;
    *) out='' ;;
    esac
    # A shell glob on the outer braces is NOT proof of complete, well-formed
    # JSON: a response truncated right after a NESTED object closes (e.g.
    # `{"runs":[{"run":1}`) still starts with `{` and ends with `}`, and
    # non-JSON garbage can too. Validate with the SAME parser the caller's
    # own field extraction (utest_frame_field) trusts, so there is exactly
    # ONE JSON-parsing implementation in this contract, not a bash
    # approximation that can be fooled by where the truncation happened to
    # land.
    if printf '%s' "$out" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    sys.exit(1)
sys.exit(0 if isinstance(d, dict) else 1)
' 2>/dev/null; then
        printf '%s' "$out"
    else
        echo '{"__poll_inconclusive__": true}'
    fi
}

utest_frame_field() {
    python3 -c '
import json, sys
try:
    d = json.loads(sys.argv[1] or "{}")
except Exception:
    d = {}
cur = d
for key in sys.argv[2].split("."):
    if not isinstance(cur, dict):
        cur = None
        break
    cur = cur.get(key)
print("" if cur is None or cur is False else ("1" if cur is True else cur))
' "$1" "$2"
}

# A MONOTONIC deadline, not an iteration count and not the wall clock.
#
# The loop used to advance ELAPSED by one per pass and compare it against
# TIMEOUT, so what it actually bounded was the number of `sleep 1` calls --
# every second of parse time, QEMU startup and grep fell OUTSIDE the budget it
# advertised. On a large capture the poll therefore ran well past `TIMEOUT`
# seconds while still reporting it had waited `TIMEOUT`.
#
# The clock source is /proc/uptime, which is MONOTONIC. `date +%s` is wall
# time: an NTP correction or a host resume steps it, and a backward step would
# extend the run past TIMEOUT while a forward one would cut it short -- the
# exact guarantee this loop exists to make. The fallback keeps the loop
# bounded on a host without /proc/uptime rather than looping forever.
mono_now() {
    if [ -r /proc/uptime ]; then
        read -r _mono _ < /proc/uptime
        echo "${_mono%%.*}"
    else
        date +%s
    fi
}
rm -f "$UTEST_POLL_STATE"
POLL_DEADLINE=$(( $(mono_now) + TIMEOUT ))
FOUND=0
while [ "$(mono_now)" -lt "$POLL_DEADLINE" ]; do
    sleep 1
    # Recheck the deadline immediately after the sleep, BEFORE the grep and
    # frame-parse below run: checking only at the top of the loop bounded
    # when an iteration was allowed to START, not how long its own work could
    # then run, so an iteration starting a moment before POLL_DEADLINE could
    # still finish well after it on a large capture -- the same "counts sleep
    # calls, not wall time" failure mode this deadline exists to close, just
    # moved one level down. REMAIN is also handed to grep/utest_frame_poll as
    # a hard ceiling on THEIR execution, not merely a re-check between them:
    # a single slow parse can no longer run past the deadline either.
    REMAIN=$(( POLL_DEADLINE - $(mono_now) ))
    [ "$REMAIN" -le 0 ] && break
    if [ -f "$TEST_LOG" ] && timeout "$REMAIN" grep -q '=== .* tests\? passed' "$TEST_LOG" 2>/dev/null; then
        FOUND=1
        UMODE_NEEDED=0
        [ "$XML_MODE" -eq 1 ] && UMODE_NEEDED=1
        [ "$JSON_MODE" -eq 1 ] && UMODE_NEEDED=1
        # TAP is a machine-readable stream on the same footing: its plan
        # line is emitted at the END of the run, so a launcher cut short
        # would leave a plan-less (malformed) TAP stream.
        [ "$TAP_MODE" -eq 1 ] && UMODE_NEEDED=1
        # If a FRAMED launcher line has appeared, the launcher has started;
        # require its terminator too so a mid-run panic cannot exit green.
        # Framed, not bare `UTEST:`: ring-3 stdout shares this serial stream
        # (sys_write copies caller bytes straight to serial_putchar), so an
        # unframed match proves nothing about who wrote it.
        REMAIN=$(( POLL_DEADLINE - $(mono_now) ))
        if [ "$REMAIN" -le 0 ]; then
            # FOUND was set the instant the kernel summary line matched,
            # before the frame check below ran at all -- breaking here
            # without resetting it would report success on a run whose
            # user-mode completion was never actually verified, exactly
            # the false-green class this whole poll exists to refuse.
            FOUND=0
            break
        fi
        POLL_SCAN=$(utest_frame_poll "$TEST_LOG" "$REMAIN")
        case "$POLL_SCAN" in
        *'"__poll_inconclusive__"'*)
            # The frame-presence check did not reach a clean, complete
            # answer (timed out, was killed, or crashed) -- we cannot tell
            # whether a launcher frame is present, so this iteration must
            # NOT fall through to the kernel-summary-alone completion path
            # below (that is exactly the false-green this deadline rework
            # exists to refuse). Poll again; the outer loop's own deadline
            # check is what ends the run if the budget is truly exhausted.
            FOUND=0
            continue
            ;;
        esac
        POLL_UF=$(utest_frame_field "$POLL_SCAN" prefix)
        if [ "$UMODE_NEEDED" -eq 0 ] && [ -n "$POLL_UF" ]; then
            UMODE_NEEDED=1
        fi
        if [ "$UMODE_NEEDED" -eq 1 ]; then
            # Wait for the launcher's TERMINATOR, not its summary. A binary
            # that prints a plausible summary line and then hangs used to end
            # the run here; the terminator is framed with THIS boot's learned
            # nonce and states the record count the whole stream must
            # reconcile against, so only the launcher can produce it and only
            # a complete run carries it.
            if [ -n "$(utest_frame_field "$POLL_SCAN" last_complete.run)" ]; then
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
#
# EVERY pattern below keys on the FRAMED subsystem tag the kernel derives
# once per boot (`UTEST-<8 hex>:`), never on the bare `UTEST:` literal.
# Ring-3 stdout and the launcher share one serial stream -- `sys_write(fd=1)`
# copies caller-controlled bytes straight to `serial_putchar` -- so before
# framing a test binary could print a well-formed summary and this gate
# would accept it as authoritative. A record that fails the framing check is
# REFUSED, never accepted as a fallback.

# One scan answers every framing question: the nonce, whether any marker
# failed to pair, how many foreign nonces announced a frame, and which run
# closed cleanly. The parser is shared with the poll above, with
# test-swtpm.sh and with the JSON harvester -- three consumers used to carry
# their own copies of these rules and had already drifted apart.
UTEST_FRAME_JSON=$(utest_frame_scan "$TEST_LOG")
UTEST_NONCE=$(utest_frame_field "$UTEST_FRAME_JSON" nonce)
FRAME_UNPAIRED=$(utest_frame_field "$UTEST_FRAME_JSON" unpaired)
FRAME_UNPAIRED=${FRAME_UNPAIRED:-0}
NONCE_CONFLICT=$(utest_frame_field "$UTEST_FRAME_JSON" conflicts)
NONCE_CONFLICT=${NONCE_CONFLICT:-0}
FRAME_COMPLETE_RUN=$(utest_frame_field "$UTEST_FRAME_JSON" last_complete.run)
FRAME_BAD_CLOSE=$(utest_frame_field "$UTEST_FRAME_JSON" bad_close)
FRAME_BAD_CLOSE=${FRAME_BAD_CLOSE:-0}
FRAME_OK=$(utest_frame_field "$UTEST_FRAME_JSON" ok)

# The prefix every framed pattern below is built from. A missing frame is an
# INVALID STATE, not a printable placeholder: ring 3 controls serial, so any
# sentinel string it could print would become an acceptance prefix. When no
# nonce was learned, NO acceptance grep runs at all.
UF=""
HAS_UTEST=0
if [ -n "$UTEST_NONCE" ]; then
    UF="UTEST-${UTEST_NONCE}: "
    grep -qE "${UF}=== [0-9]+ passed" "$TEST_LOG" 2>/dev/null && HAS_UTEST=1
fi

# Format obligations are settled HERE, the moment the frame prefix is known and
# before any fallible host-side parsing. The guard owes a refusal for a format
# this run would have produced; deciding that at assembly time meant an exit
# during parsing could publish one format's refusal and silently skip the
# other's, because the flag it keys on had not been assigned yet.
HAS_XML=0
HAS_JSON=0
if [ -n "$UF" ]; then
    grep -qE "${UF}\[UTEST-XML\]" "$TEST_LOG" 2>/dev/null && HAS_XML=1
    grep -qE "${UF}\[UTEST-JSON\]" "$TEST_LOG" 2>/dev/null && HAS_JSON=1
fi

UTEST_PASS=0
UTEST_FAIL=0
UTEST_SKIP=0

if [ "$HAS_UTEST" -eq 1 ]; then
    echo ""
    echo -e "${CYAN}[UTEST]${RESET} User-mode test binaries:"

    # Per-binary verdict lines (`UTEST: <name>: <STATUS> ...`). Skip the
    # informational `format=` plumb-through and the framing summary lines
    # that the loop below would otherwise echo as ambiguous "OK" entries.
    { grep -E "${UF}[^ ]+\.exe: (PASS|FAIL|SKIP|TIMEOUT|ISOLATION|LEAK|format=)" \
          "$TEST_LOG" 2>/dev/null || true; } | while IFS= read -r line; do
        verdict=$(echo "$line" | sed -E "s/.*${UF}//")
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
    grep -qE "${UF}(ok|not ok|Bail out!|1\.\.[0-9]+)" "$TEST_LOG" 2>/dev/null && HAS_TAP=1
    if [ "$HAS_TAP" -eq 1 ] && [ "$QUIET_MODE" -eq 0 ]; then
        echo -e "  ${CYAN}TAP${RESET} producer stream:"
        grep -E "${UF}(ok|not ok|Bail out!|1\.\.[0-9]+)" "$TEST_LOG" 2>/dev/null |
            sed -E "s/.*${UF}/    /"
    fi

    # Pull the launcher's authoritative summary numbers.
    UTEST_SUM=$(grep -E "${UF}=== [0-9]+ passed, [0-9]+ failed, [0-9]+ skipped of [0-9]+ total" \
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
    UTEST_FAIL_OBSERVED=$({ grep -cE "${UF}[^ ]+\.exe: (FAIL|TIMEOUT|ISOLATION|LEAK)" \
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
    UTEST_REPORT_SUM=$(grep -E "${UF}\[UTEST-REPORT-SUMMARY\] " \
                       "$TEST_LOG" 2>/dev/null | tail -1 || true)
    if [ -n "$UTEST_REPORT_SUM" ]; then
        RPT_BLOCKS=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* skip_blocks=([0-9]+).*/\1/')
        RPT_INVALID=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* invalid=([0-9]+).*/\1/')
        RPT_REPORTED=$(echo "$UTEST_REPORT_SUM" | sed -E 's/.* reported=([0-9]+).*/\1/')
        echo -e "  ${CYAN}[UTEST]${RESET} reported: ${RPT_REPORTED} binaries, ${RPT_BLOCKS} skip block(s), ${RPT_INVALID} invalid"

        # Cross-check: recount the per-binary report lines from serial. A
        # summary claiming fewer reporting binaries than the stream shows
        # means a launcher counter bug -- trust the stream.
        RPT_LINES=$({ grep -cE "${UF}\[UTEST-REPORT\] " "$TEST_LOG" 2>/dev/null || true; } | head -1)
        RPT_LINES=${RPT_LINES:-0}
        RPT_INVALID_OBSERVED=$({ grep -cE "${UF}\[UTEST-REPORT\] .* state=INVALID" \
                                 "$TEST_LOG" 2>/dev/null || true; } | head -1)
        RPT_INVALID_OBSERVED=${RPT_INVALID_OBSERVED:-0}
        RPT_TOTAL_EXPECTED=$(( ${RPT_REPORTED:-0} + ${RPT_INVALID:-0} ))
        if [ "$RPT_LINES" -gt "$RPT_TOTAL_EXPECTED" ]; then
            echo -e "  ${RED}[UTEST]${RESET} report summary accounts for ${RPT_TOTAL_EXPECTED} binaries but ${RPT_LINES} [UTEST-REPORT] lines are on serial -- counting the difference as failures"
            UTEST_FAIL=$(( UTEST_FAIL + RPT_LINES - RPT_TOTAL_EXPECTED ))
        fi
        # An INVALID report FAILS the run from this channel, independently
        # of whether the launcher's own escalation counted it. The whole
        # point of parsing the report dimension separately is that it
        # still catches the case where the launcher's escalation is what
        # regressed -- printing a warning and exiting 0 would have made
        # this an observation channel rather than the gate the TODO
        # claims it is.
        RPT_INVALID_TOTAL=$(( ${RPT_INVALID:-0} > RPT_INVALID_OBSERVED ? ${RPT_INVALID:-0} : RPT_INVALID_OBSERVED ))
        if [ "$RPT_INVALID_TOTAL" -gt 0 ]; then
            echo -e "  ${RED}[UTEST]${RESET} ${RPT_INVALID_TOTAL} binary/binaries submitted a self-contradicting report -- failing the run"
            UTEST_FAIL=$(( UTEST_FAIL + RPT_INVALID_TOTAL ))
        fi
    elif grep -qE "${UF}\[UTEST-REPORT\] " "$TEST_LOG" 2>/dev/null; then
        # Per-binary report lines but no summary: the report channel was
        # cut off mid-run. Fail rather than silently skipping the whole
        # cross-check.
        echo -e "  ${RED}[UTEST]${RESET} per-binary report lines present but no [UTEST-REPORT-SUMMARY] -- report channel truncated"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi

    # A summary line that could not be formatted is never published
    # truncated -- the launcher emits an explicit overflow marker instead.
    # Its presence means an artifact is missing its summary, which is an
    # observability failure on the reporting path itself.
    UTEST_OVERFLOW=$({ grep -cE "${UF}\[UTEST-(XML|JSON|REPORT)-SUMMARY-OVERFLOW\]" \
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
    UTEST_ABORT=$({ grep -cE "${UF}(Bail out!|suite ABORT)" "$TEST_LOG" 2>/dev/null || true; } | head -1)
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

    # A COMPLETED run that produced fewer results than it planned. The
    # launcher plans binaries in one walk of C:\ and executes them in a
    # second one taken after live children have run, so a binary that
    # disappears between the two is omitted from the results while the
    # summary still reports success -- and a name REFUSED at ingest, which
    # the planning walk counts precisely so it cannot vanish, would vanish
    # exactly that way. The abort gate above does not cover it: nothing
    # aborted, the walk simply came up short. Distinct from the abort so
    # the diagnosis names the right producer bug.
    UTEST_INCOMPLETE=$({ grep -cE "${UF}\[UTEST-RUN-INCOMPLETE\]" "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_INCOMPLETE=${UTEST_INCOMPLETE:-0}
    if [ "$UTEST_INCOMPLETE" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_INCOMPLETE} launcher run(s) finished with planned binaries that produced no result -- the run is incomplete, not green"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_INCOMPLETE ))
    fi

    # The launcher refuses to emit skip records past its run-wide budget
    # and says so explicitly. Records it knows about but did not emit mean
    # the artifacts under-report skips, which is the same false-coverage
    # class the reporting path exists to close.
    UTEST_BUDGET=$({ grep -cE "${UF}\[UTEST-SKIP-RECORD-BUDGET\]" "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_BUDGET=${UTEST_BUDGET:-0}
    if [ "$UTEST_BUDGET" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_BUDGET} binary/binaries exceeded the run-wide skip-record budget -- artifacts under-report skips"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_BUDGET ))
    fi

    # A per-record emit that did not fit its buffer is published as a
    # verdict-preserving fallback with the name dropped, so the artifact
    # stays well-formed -- but a record whose identity was lost is still
    # an artifact that cannot be traced back to its binary. Host-fatal.
    UTEST_RECORD_OVERFLOW=$({ grep -cE "${UF}\[UTEST-RECORD-OVERFLOW\]" "$TEST_LOG" 2>/dev/null || true; } | head -1)
    UTEST_RECORD_OVERFLOW=${UTEST_RECORD_OVERFLOW:-0}
    if [ "$UTEST_RECORD_OVERFLOW" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${UTEST_RECORD_OVERFLOW} artifact record(s) lost their name to a buffer overflow -- untraceable results"
        UTEST_FAIL=$(( UTEST_FAIL + UTEST_RECORD_OVERFLOW ))
    fi

    # Record-count reconciliation. The launcher's terminator states how many
    # framed records it emitted before it; a host that saw the whole stream
    # counts exactly `records + 1` framed lines for that run. A disagreement
    # means the stream was cut, dropped, or extended -- all of which make
    # every count above unreliable, so it is host-fatal rather than a note.
    # Summed over runs because test_usermode_run() is documented safe to
    # call repeatedly and each invocation frames its own run.
    # Reconciliation is the PARSER's verdict, computed per run in its single
    # pass: a terminator counts only when it closes the open announcement with
    # the SAME ordinal, and a run is complete only when the framed lines
    # between its markers equal `records + 1`. Summing boot-wide (what this
    # did before) let `BEGIN run=1 ... END run=2` reconcile, and let two runs
    # compensate each other's count errors across the boundary.
    if [ "$FRAME_UNPAIRED" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${FRAME_UNPAIRED} framed run marker(s) do not pair: a terminator closed no matching announcement, or a run re-opened before its terminator"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi
    if [ -z "$FRAME_COMPLETE_RUN" ]; then
        echo -e "  ${RED}[UTEST]${RESET} no framed launcher run reconciled -- the run did not finish, or its stream was truncated"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi

    # A forged frame announcement did not corrupt the results above (the
    # launcher's own announcement came first and is what every pattern keyed
    # on), but a binary emitting launcher-shaped framing is a defect in its
    # own right and must not pass silently.
    # ANY foreign announcement is fatal. The threshold used to be `> 1`, from
    # when this counted every distinct nonce including the launcher's own; the
    # parser reports FOREIGN nonces only, so one forged announcement -- the
    # normal shape -- slipped through here while the harvester and the swtpm
    # runner refused the same stream. A consumer that disagrees with the
    # shared parser is the drift the parser was introduced to remove.
    if [ "$NONCE_CONFLICT" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${NONCE_CONFLICT} foreign launcher nonce(s) announced a frame -- something other than the launcher emitted framing (results parsed from the launcher's own, which came first)"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi
    if [ "$FRAME_BAD_CLOSE" -gt 0 ]; then
        echo -e "  ${RED}[UTEST]${RESET} ${FRAME_BAD_CLOSE} framed run(s) closed without reconciling their declared record count -- the newest closed stream is incomplete"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi
    if [ -z "$FRAME_OK" ]; then
        echo -e "  ${RED}[UTEST]${RESET} the launcher frame did not validate -- refusing to read this stream as results"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    fi

    # The ring-3 forgery fixture is only proof if something CHECKS it. When
    # test_forge.exe ran, its forged records reached serial by construction;
    # assert here that none of them reached a verdict or an artifact. Without
    # this the fixture proves only that bytes were written, and an XML/JSON
    # parser regression could accept forged output while the suite stays
    # green. Scoped to runs where the fixture actually executed.
    if grep -qE "${UF}test_forge\.exe: (PASS|FAIL)" "$TEST_LOG" 2>/dev/null; then
        FORGE_LEAK=0
        for artifact in "$PROJECT/build/test-results.xml" "$PROJECT/build/test-results.json"; do
            [ -f "$artifact" ] || continue
            if grep -qE 'forged-(xml|json)|"total": ?9[89]|tests="9[89]"' "$artifact" 2>/dev/null; then
                echo -e "  ${RED}[UTEST]${RESET} FORGED record from test_forge.exe reached $(basename "$artifact") -- the framing check is not being applied by the artifact assembler"
                FORGE_LEAK=$(( FORGE_LEAK + 1 ))
            fi
        done
        # The forged summaries claim 99 and 98 binaries; the run's own count
        # must come from the launcher, not from them.
        if [ "$UTEST_PASS" -ge 98 ]; then
            echo -e "  ${RED}[UTEST]${RESET} run counts (${UTEST_PASS} passed) match test_forge.exe's forged summary -- a forged verdict was read as authoritative"
            FORGE_LEAK=$(( FORGE_LEAK + 1 ))
        fi
        if [ "$FORGE_LEAK" -gt 0 ]; then
            UTEST_FAIL=$(( UTEST_FAIL + FORGE_LEAK ))
        fi
    fi

fi

# Framing is fail-closed in BOTH directions. Above, a framed stream that
# does not reconcile fails. Here, the absence of a usable frame fails
# whenever anything on serial claims to be a launcher record: that is either
# a producer that stopped framing (drift against a host that now requires
# it) or ring-3 output imitating one, and neither may pass as "no user-mode
# tests ran".
if [ "$HAS_UTEST" -eq 0 ]; then
    if [ -n "$UTEST_NONCE" ]; then
        # The launcher framed a run and then never published its summary --
        # it was cut short. Distinct from "nothing ran": the announcement
        # proves the launcher started.
        echo ""
        echo -e "  ${RED}[UTEST]${RESET} launcher announced frame ${UTEST_NONCE} but never published a summary -- the run was cut short"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
    elif grep -qE '(^|[^-])UTEST: (=== [0-9]+ passed|\[UTEST-(XML|JSON|REPORT))' \
              "$TEST_LOG" 2>/dev/null; then
        # Launcher-shaped records with no frame at all: either a producer
        # that stopped framing (drift against a host that now requires it)
        # or ring-3 output imitating one. Neither may pass as "no user-mode
        # tests ran".
        echo ""
        echo -e "  ${RED}[UTEST]${RESET} serial carries UNFRAMED launcher-shaped records and no frame announcement -- refusing to read them as results"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
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

# XML_OUT / XML_LEG_OUT are set in Step 0a and HAS_XML right after the serial
# capture: both the destination and the obligation exist before this point.

# Belt-and-suspenders: XML=1 was requested but the kernel produced no
# [UTEST-XML] stream. The normal empty-suite case (total_planned=0 with
# xml=1) is handled kernel-side and goes through the HAS_XML=1 branch
# below, so reaching here means the producer never ran while the launcher
# itself did -- the boot.conf `xml=` patch did not take, or the stream was
# cut. That is an INFRASTRUCTURE failure, not an empty run.
#
# Until 2026-07-29 this wrote a clean zero-test <testsuite/> and exited
# green, which made "the artifact pipeline is broken" indistinguishable
# from "the filter matched nothing". Now it fails the run and publishes a
# document that says so: a single <error> testcase, so a JUnit consumer
# that never reads our exit code still sees red rather than an empty
# green suite. A stale artifact from a previous run is removed first so a
# death before this point cannot leave one behind masquerading as current.
if [ "$XML_MODE" -eq 1 ] && [ "$HAS_XML" -eq 0 ]; then
    rm -f "$XML_OUT"
    utest_publish_xml_refusal "artifact-pipeline" \
        "XML=1 requested but no [UTEST-XML] stream reached serial"
    echo -e "  ${RED}[UTEST]${RESET} XML=1 requested but no [UTEST-XML] stream on serial -- artifact pipeline broken, failing the run"
    UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
fi

# --- Canonical run slice + captured-output model (shared by BOTH artifacts) ---
#
# Produced ONCE, here, ahead of either artifact step, because the JUnit XML and
# the JSON record must describe the same run. The slice used to be built inside
# the XML step, which meant a JSON-only run (`JSON=1` without `XML=1`) never
# built it at all and silently published testcases with no captured output --
# the artifacts disagreeing about what the run contained, which is exactly the
# divergence a single shared model exists to prevent.
RUNSLICE="$RECORD_DIR/runslice"
SLICE_FAILED=0
CAPTURE_MODEL="$RECORD_DIR/capture.json"
CAPTURE_OK=1
CAPTURE_ATTACH=0
rm -f "$RUNSLICE"
if [ "$HAS_XML" -eq 1 ] || [ "$JSON_MODE" -eq 1 ]; then
    if [ -n "$FRAME_COMPLETE_RUN" ]; then
        if ! python3 "$PROJECT/scripts/utest-frame.py" \
                --emit-run "$RUNSLICE" "$TEST_LOG" >/dev/null 2>&1; then
            SLICE_FAILED=1
        fi
        [ -s "$RUNSLICE" ] || SLICE_FAILED=1
    fi

    # Captured per-binary output (section 36). Runs ONLY for a complete framed
    # run whose slice succeeded: without one there is no canonical population
    # to attribute bytes to, and the JSON harvester already refuses to read
    # results from an unreconciled stream. No complete run therefore means no
    # captured output in either artifact -- a degraded artifact, not a refusal.
    if [ -n "$FRAME_COMPLETE_RUN" ] && [ "$SLICE_FAILED" -eq 0 ] && [ -s "$RUNSLICE" ]; then
        if python3 "$PROJECT/scripts/utest-capture.py" model \
                "$RUNSLICE" "$CAPTURE_MODEL" --prefix "$UF" >/dev/null 2>&1; then
            CAPTURE_ATTACH=1
        else
            rc=$?
            # Only exit 2 is a considered "the tool could not run" verdict. Any
            # OTHER non-zero status -- a crash, or the OOM kill an abusive
            # payload can provoke (rc 137) -- is NOT evidence that the run's
            # bytes are fine; treating it as a degrade would let resource
            # exhaustion silently downgrade into a clean artifact.
            if [ "$rc" -ne 2 ]; then
                # Reconciliation failed. Under source framing the kernel's
                # declared byte count and the host's decoded count cover the
                # identical byte range, so a mismatch is corruption rather than
                # ordinary interleaving: fail the run and publish a refusal,
                # the same stance the failed-slice and unparseable-summary
                # branches take. A plausible-looking artifact built over
                # known-corrupt bytes is worse than no artifact.
                CAPTURE_REASON=$(python3 -c 'import json,sys
try:
    m = json.load(open(sys.argv[1]))
    r = m.get("refusal") or {}
    print("{0}: {1}".format(r.get("reason", "unknown"), r.get("detail", "")))
except Exception:
    print("unknown: capture model unreadable")' "$CAPTURE_MODEL" 2>/dev/null)
                echo -e "  ${RED}[UTEST]${RESET} captured-output reconciliation FAILED -- ${CAPTURE_REASON}"
                UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
                CAPTURE_OK=0
                # A crash leaves no model behind, and the JSON side refuses only
                # on a model it can READ. Without this the XML artifact would
                # refuse while the JSON artifact published a green envelope for
                # the same run -- the divergence the shared model exists to make
                # impossible.
                if ! grep -q '"ok": false' "$CAPTURE_MODEL" 2>/dev/null; then
                    printf '{"schema": "utest-capture-v1", "ok": false, "refusal": {"reason": "capture_tool_failed", "detail": "capture model build exited %s"}, "binaries": []}\n' \
                        "$rc" > "$CAPTURE_MODEL" 2>/dev/null || true
                fi
            else
                # A usage/IO fault in the capture tool is an infrastructure
                # problem, not evidence about the run's bytes. Degrade to no
                # captured output rather than condemning a payload that was
                # never actually examined.
                echo -e "  ${YELLOW}[UTEST]${RESET} capture model unavailable (tool error) -- artifacts omit captured output"
            fi
        fi
    fi
fi

XML_SUMMARY_OK=1
if [ "$HAS_XML" -eq 1 ]; then
    # Any artifact from a previous run goes first: a death partway through
    # assembly must not leave the old document at the canonical path where a
    # consumer would read it as this run's result. The LEG alias is left
    # alone -- it means the latest COMPLETED run of this configuration, and
    # only a complete record of this run may replace it.
    rm -f "$XML_OUT"
    # The XML payload source, as a FILE rather than a shell variable.
    #
    # This used to be `STRIPPED=$(sed ... "$TEST_LOG")` -- the entire
    # ANSI-stripped capture in one shell variable -- and then `RUN_SLICE=$(...)`
    # holding the selected run in a second one, which was already the third
    # materialization after the parser's own line list. Peak host memory
    # therefore scaled with the whole serial log (tens of MB) to extract a run
    # slice measured in kilobytes. The parser writes the slice straight to
    # disk instead, and the sed extractions below read that file.
    #
    # Narrowing to the LAST COMPLETE framed run still uses the parser's
    # matched announcement/terminator pair. Selecting the last announcement
    # and the last terminator INDEPENDENTLY breaks on a complete run followed
    # by an unterminated one: the last terminator then precedes the last
    # announcement, slicing is skipped, and both runs' testcases get published
    # under one run's summary.
    # Inside the run record: it was a fixed `$TEST_LOG.runslice`, which is
    # shared state between any two invocations that overlap.
    # The slice is built once, above, and SHARED with the capture model so both
    # artifacts describe the same run. This step only consumes it -- and never
    # writes to it, because the fallback below would otherwise overwrite the
    # very file the capture model was reconciled against.
    XML_SRC="$RUNSLICE"
    XML_SLICE_FAILED=$SLICE_FAILED
    if [ "$XML_SLICE_FAILED" -eq 1 ]; then
        # A complete run EXISTS but could not be sliced out. Falling back to
        # the whole capture here would publish exactly the artifact the
        # slicing exists to prevent -- the comment above describes it: two
        # runs' testcases assembled under one run's summary, which reads to a
        # JUnit consumer as a single coherent suite. That is a false artifact,
        # so the run FAILS instead, matching the stance every other refusal on
        # this path takes.
        echo -e "  ${RED}[UTEST]${RESET} run slice failed for a COMPLETE framed run -- refusing to assemble from the whole capture"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
        XML_SUMMARY_OK=0
        utest_publish_xml_refusal "artifact-pipeline" \
            "run slice failed for a complete framed run -- artifact would mix runs"
        # The payload greps below still run; give them an EMPTY source rather
        # than a missing file (set -e would abort on the read) and rather than
        # the whole capture (that is the mixing this branch refuses). It is a
        # SEPARATE file: the shared slice must stay exactly what the capture
        # model was reconciled against.
        XML_SRC="$RECORD_DIR/xmlsrc"
        : > "$XML_SRC"
    fi
    # No complete run at all: the whole capture, ANSI-stripped, still as a
    # file. This is not the failure case above -- with no complete run there
    # is no slice to prefer, and the framing gates have already decided
    # whether this run is publishable; this only controls what the payload
    # greps see. Also a separate file, for the same reason.
    if [ ! -s "$XML_SRC" ] && [ "$XML_SLICE_FAILED" -eq 0 ]; then
        XML_SRC="$RECORD_DIR/xmlsrc"
        sed -E 's/\x1b\[[0-9;]*m//g' "$TEST_LOG" > "$XML_SRC" || true
    fi

    # Extract the summary numbers (tests=N failures=N skipped=N time=S.MMM).
    #
    # Skipped entirely after a slice failure. That branch has already failed
    # the run and published a refusal naming its SPECIFIC cause; falling
    # through would find the deliberately-emptied XML_SRC, necessarily take
    # the no-summary branch, count a SECOND failure for the same event, and
    # overwrite the precise diagnosis with a generic "counts unknown"
    # document. XML_SUMMARY_OK alone only stops final assembly -- it does not
    # protect the refusal already published.
    if [ "$XML_SLICE_FAILED" -eq 0 ]; then
    SUM_LINE=$(sed -n "s/.*${UF}\[UTEST-XML-SUMMARY\] //p" "$XML_SRC" |
               grep -E '^tests=[0-9]+ failures=[0-9]+ skipped=[0-9]+ time=' |
               tail -1 || true)

    if [ -n "$SUM_LINE" ]; then
        XML_TESTS=$(echo "$SUM_LINE" | sed -E 's/.*tests=([0-9]+).*/\1/')
        XML_FAIL=$(echo "$SUM_LINE" | sed -E 's/.*failures=([0-9]+).*/\1/')
        XML_SKIP=$(echo "$SUM_LINE" | sed -E 's/.*skipped=([0-9]+).*/\1/')
        XML_TIME=$(echo "$SUM_LINE" | sed -E 's/.*time=([0-9.]+).*/\1/')
        # Every counter is BOUNDED before it is compared or added. The
        # extraction above accepts `[0-9]+` of any length, and an oversized
        # decimal here is not merely wrong -- it FAILS OPEN: `[ "$a" -gt "$b" ]`
        # prints "integer expression expected" and evaluates FALSE, so the
        # reconciliation guards below would silently pass, and the projection
        # `$(( failures - errors ))` then wraps through bash's signed 64-bit
        # arithmetic to a negative. Every field the producer emits is a
        # uint32, so anything wider is drift or forgery and the run refuses.
        #
        # CANONICAL decimal, not merely digits: bash's `[` compares in base
        # 10 but `$(( ))` reads a leading zero as OCTAL, so `failures=08`
        # passes a digits-only guard and then aborts the projection with
        # "value too great for base", while `failures=010` silently becomes
        # 8. The producer formats with %u and cannot emit either shape.
        XML_COUNTS_OK=1
        for _xml_field in "$XML_TESTS" "$XML_FAIL" "$XML_SKIP"; do
            if printf '%s' "$_xml_field" | grep -qE '^(0|[1-9][0-9]{0,9})$'; then
                [ "$_xml_field" -le 4294967295 ] || XML_COUNTS_OK=0
            else
                XML_COUNTS_OK=0
            fi
        done
        if [ "$XML_COUNTS_OK" -ne 1 ]; then
            echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML-SUMMARY] carries a count outside the uint32 range the producer can emit -- refusing to assemble from it"
            UTEST_FAIL=$(( UTEST_FAIL + 1 ))
            utest_publish_xml_refusal "artifact-pipeline" \
                "[UTEST-XML-SUMMARY] carries a count outside the uint32 range"
            XML_SUMMARY_OK=0
        fi
        # Run-completeness fields. Fail-CLOSED on absence, matching the
        # stance taken on an unparseable summary below and the JSON
        # harvester's `missing_completeness` refusal: test.sh boots the
        # kernel it just built, so a summary without these fields is
        # producer/host drift, not an old artifact. Defaulting them to
        # "complete" would let exactly that drift republish an aborted run
        # as a finished one.
        XML_ABORTED=0
        XML_NOT_RUN=0
        XML_NEVER_RAN=0
        if echo "$SUM_LINE" | grep -qE ' aborted=[0-9]+ not_run=[0-9]+ errors=[0-9]+'; then
            XML_ABORTED=$(echo "$SUM_LINE" | sed -E 's/.* aborted=([0-9]+).*/\1/')
            XML_NOT_RUN=$(echo "$SUM_LINE" | sed -E 's/.* not_run=([0-9]+).*/\1/')
            # The never-ran subset of `failures`. Fail-CLOSED on absence for
            # the same reason as aborted=/not_run=: this script boots the
            # kernel it just built, so a summary without the field is
            # producer/host drift, and defaulting it to zero would publish
            # every refusal as an ordinary assertion failure.
            XML_NEVER_RAN=$(echo "$SUM_LINE" | sed -E 's/.* errors=([0-9]+).*/\1/')
            # Same uint32 bound as the three counters above, for the same
            # fail-open reason: this value is BOTH compared and subtracted.
            for _xml_field in "$XML_ABORTED" "$XML_NOT_RUN" "$XML_NEVER_RAN"; do
                if printf '%s' "$_xml_field" | grep -qE '^(0|[1-9][0-9]{0,9})$'; then
                    [ "$_xml_field" -le 4294967295 ] || XML_COUNTS_OK=0
                else
                    XML_COUNTS_OK=0
                fi
            done
            if [ "$XML_COUNTS_OK" -ne 1 ] && [ "$XML_SUMMARY_OK" -eq 1 ]; then
                echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML-SUMMARY] carries an aborted=/not_run=/errors= value outside the uint32 range -- refusing to assemble from it"
                UTEST_FAIL=$(( UTEST_FAIL + 1 ))
                utest_publish_xml_refusal "artifact-pipeline" \
                    "[UTEST-XML-SUMMARY] carries a completeness value outside the uint32 range"
                XML_SUMMARY_OK=0
            fi
        else
            echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML-SUMMARY] carries no aborted=/not_run=/errors= fields -- cannot tell a complete run from an aborted one"
            UTEST_FAIL=$(( UTEST_FAIL + 1 ))
            # Same refusal as an unparseable summary, for the same reason:
            # continuing would publish a normal document asserting
            # aborted="false", which is a CLAIM the stream never made. An
            # artifact-only consumer would read a version-skewed or truncated
            # run as a completed green suite.
            utest_publish_xml_refusal "artifact-pipeline" \
                "[UTEST-XML-SUMMARY] carries no aborted=/not_run=/errors= fields -- run completeness unknown"
            XML_SUMMARY_OK=0
        fi
    else
        # No parseable summary, but [UTEST-XML] records DID reach serial
        # (HAS_XML=1). Substituting zeros here produced the worst possible
        # artifact: a <testsuite tests="0" failures="0" skipped="0">
        # wrapped around real testcases, which reads to a JUnit consumer
        # as a clean empty run while carrying evidence of an incomplete
        # one. The counts are unknown, so the run FAILS rather than
        # publishing an artifact that contradicts itself.
        echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML] records on serial but no parseable [UTEST-XML-SUMMARY] -- XML artifact counts unknown"
        UTEST_FAIL=$(( UTEST_FAIL + 1 ))
        # Publish a document that SAYS the counts are unknown, and stop.
        # Falling through with zeros wrote `<testsuite tests="0" ...>` wrapped
        # around the real testcases still on serial -- the self-contradicting
        # artifact the comment above disclaims, which reads to a JUnit
        # consumer as a clean empty run while carrying evidence of an
        # incomplete one. The intent was always to refuse; only the code
        # continued.
        utest_publish_xml_refusal "artifact-pipeline" \
            "[UTEST-XML] records on serial with no parseable [UTEST-XML-SUMMARY] -- counts unknown"
        XML_SUMMARY_OK=0
    fi
    fi  # XML_SLICE_FAILED guard: the slice-failure refusal stands alone

    # JUnit's two failure columns are DISJOINT -- `failures` counts
    # <failure> elements, `errors` counts <error> ones -- while the producer
    # reports `errors` as an overlapping SUBSET of `failures` so its own
    # summary line, the serial recount and the TAP stream keep describing
    # one failure population. This is where the two models meet: the
    # projection happens at assembly, next to the document it describes.
    #
    # It is reconciled before it is trusted. The producer's count and the
    # elements actually harvested from the canonical run slice are two
    # independent pieces of evidence, and every other count in this
    # assembler is cross-checked the same way. Trusting the field alone
    # would let producer counter drift publish a <testsuite errors="N">
    # whose N contradicts the elements underneath it.
    if [ "$XML_SUMMARY_OK" -eq 1 ]; then
        # Counted over the BINARY testcase population only. A synthetic
        # skip-block record carries classname="skip-block" and has no
        # never-ran dimension, so an <error> inside one is not evidence of
        # a refused binary -- counting it would let a corrupted skip record
        # satisfy errors= while no binary ERROR record exists at all, and
        # the suite attributes would then contradict the children beneath
        # them. Excluded here, so that shape reconciles to a MISMATCH and
        # the run refuses, which is what it is.
        XML_ERROR_ELEMS=$({ sed -n "s/.*${UF}\[UTEST-XML\] //p" "$XML_SRC" |
                            grep -E '^<testcase' |
                            grep -v 'classname="skip-block"' |
                            grep -cE '<error( |/>)' || true; } | head -1)
        XML_ERROR_ELEMS=${XML_ERROR_ELEMS:-0}
        # Excluding the skip-block population from the count is only half a
        # check. Nothing above verifies those records are SKIPPED-only, so a
        # malformed one carrying <error> would be silently dropped from the
        # count while still reaching the document: the artifact would then
        # hold two <error> elements under errors="1". A skip record has no
        # failure or never-ran dimension by construction, so any other
        # element in one is producer drift and the run refuses.
        XML_BAD_SKIP=$({ sed -n "s/.*${UF}\[UTEST-XML\] //p" "$XML_SRC" |
                         grep -E '^<testcase' |
                         grep 'classname="skip-block"' |
                         grep -cE '<(error|failure)( |/>)' || true; } | head -1)
        XML_BAD_SKIP=${XML_BAD_SKIP:-0}
        if [ "$XML_BAD_SKIP" -gt 0 ]; then
            echo -e "  ${RED}[UTEST]${RESET} ${XML_BAD_SKIP} skip-block record(s) carry an <error>/<failure> element -- a skip record has neither dimension, so the counts and the document would disagree"
            UTEST_FAIL=$(( UTEST_FAIL + 1 ))
            utest_publish_xml_refusal "artifact-pipeline" \
                "${XML_BAD_SKIP} skip-block record(s) carry a non-skipped element"
            XML_SUMMARY_OK=0
        fi
        if [ "${XML_NEVER_RAN:-0}" -gt "${XML_FAIL:-0}" ]; then
            echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML-SUMMARY] errors=${XML_NEVER_RAN} exceeds failures=${XML_FAIL} -- the never-ran count is not a subset of the failures it is drawn from"
            UTEST_FAIL=$(( UTEST_FAIL + 1 ))
            utest_publish_xml_refusal "artifact-pipeline" \
                "[UTEST-XML-SUMMARY] errors=${XML_NEVER_RAN} exceeds failures=${XML_FAIL}"
            XML_SUMMARY_OK=0
        elif [ "$XML_ERROR_ELEMS" -ne "${XML_NEVER_RAN:-0}" ]; then
            echo -e "  ${RED}[UTEST]${RESET} [UTEST-XML-SUMMARY] errors=${XML_NEVER_RAN} but ${XML_ERROR_ELEMS} <error> element(s) on serial -- producer counts disagree with the records they describe"
            UTEST_FAIL=$(( UTEST_FAIL + 1 ))
            utest_publish_xml_refusal "artifact-pipeline" \
                "[UTEST-XML-SUMMARY] errors=${XML_NEVER_RAN} but ${XML_ERROR_ELEMS} <error> element(s) harvested"
            XML_SUMMARY_OK=0
        fi
    fi

    # An aborted suite ADDITIONALLY gets a synthetic infrastructure
    # <testcase> carrying an <error>, counted on top of the producer's own.
    #
    # <properties> alone would not be enough. The smoke gate aborts on ANY
    # non-PASS verdict including SKIP, and a skipped smoke leaves failures=0
    # with every remaining count internally consistent -- so a plain JUnit
    # consumer (dorny/test-reporter, the GitLab junit schema, the Jenkins
    # plugin) reports a run where most binaries never executed as green. The
    # properties carry the queryable numbers; the error element is what makes
    # the document itself red. `tests` is incremented to match, because it
    # counts elements in the file.
    #
    # Projected ONLY over values that survived the checks above. A refused
    # summary has already published its diagnosis and stopped assembly, so
    # projecting anyway computes nothing anyone reads -- and does it on the
    # very values just judged untrustworthy, which is how a refused
    # `errors=08` still reached `$(( ))` and printed a raw bash arithmetic
    # error after the refusal it had correctly triggered.
    XML_ERRORS=0
    if [ "$XML_SUMMARY_OK" -eq 1 ]; then
        XML_ERRORS=${XML_NEVER_RAN:-0}
        XML_FAIL=$(( ${XML_FAIL:-0} - ${XML_NEVER_RAN:-0} ))
        if [ "${XML_ABORTED:-0}" -ne 0 ]; then
            XML_ERRORS=$(( XML_ERRORS + 1 ))
            XML_TESTS=$(( XML_TESTS + 1 ))
        fi
    fi
fi

# Assembly runs only when the summary parsed. The unparseable-summary branch
# above has already published its own error document, and re-entering here
# would overwrite it with the zeros-around-real-testcases artifact it exists
# to avoid.
if [ "$HAS_XML" -eq 1 ] && [ "$XML_SUMMARY_OK" -eq 1 ] && [ "${CAPTURE_OK:-1}" -eq 1 ]; then
    {
        echo '<?xml version="1.0" encoding="UTF-8"?>'
        printf '<testsuite name="impossible-os-usermode" tests="%s" failures="%s" skipped="%s" errors="%s" time="%s"%s>\n' \
            "${XML_TESTS}" "${XML_FAIL}" "${XML_SKIP}" "${XML_ERRORS}" "${XML_TIME}" \
            "$(utest_xml_identity_attrs)"
        # <properties> must be the first child of <testsuite> per the JUnit
        # schema. Emitted on every run, not only aborted ones, so that a
        # consumer can distinguish "this run completed" from "this artifact
        # predates the completeness dimension".
        echo '  <properties>'
        if [ "${XML_ABORTED:-0}" -ne 0 ]; then
            echo '    <property name="aborted" value="true"/>'
        else
            echo '    <property name="aborted" value="false"/>'
        fi
        echo "    <property name=\"not_run\" value=\"${XML_NOT_RUN:-0}\"/>"
        utest_xml_identity_props
        echo '  </properties>'
        if [ "${XML_ABORTED:-0}" -ne 0 ]; then
            echo '  <testcase name="suite-abort" classname="infrastructure">'
            echo "    <error message=\"smoke gate aborted the suite; ${XML_NOT_RUN:-0} binaries never ran\"/>"
            echo '  </testcase>'
        fi
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
        sed -n "s/.*${UF}\[UTEST-XML\] //p" "$XML_SRC" |
            grep -E '^<testcase' || true
        echo '</testsuite>'
    } > "$RECORD_DIR/.assembled.xml" &&
        utest_splice_capture "$RECORD_DIR/.assembled.xml" &&
        utest_publish "$RECORD_DIR/.assembled.xml" "$XML_RECORD" && XML_PUBLISHED=1
    if [ "${XML_ABORTED:-0}" -ne 0 ]; then
        echo -e "${CYAN}[TEST]${RESET} JUnit XML written: $XML_RECORD (tests=${XML_TESTS} failures=${XML_FAIL} errors=${XML_ERRORS} skipped=${XML_SKIP} ABORTED, not_run=${XML_NOT_RUN})"
    else
        echo -e "${CYAN}[TEST]${RESET} JUnit XML written: $XML_RECORD (tests=${XML_TESTS} failures=${XML_FAIL} errors=${XML_ERRORS} skipped=${XML_SKIP})"
    fi
fi

# --- Step 6b: User-mode test JSON artifact ---
#
# The JSON sibling of step 6. The launcher has emitted a valid `[UTEST-JSON]`
# record per binary since the typed-stream work, but nothing ever assembled
# them into a file: XML=1 produced build/test-results.xml while JSON=1
# produced only serial output, so every JSON consumer had to re-derive the
# harvest itself.
#
# Assembly and validation live in scripts/utest-json-harvest.py rather than
# inline sed. The artifact is a gate: a truncated, duplicated, or
# self-inconsistent stream must fail the run instead of publishing a smaller
# plausible result, and that check is real JSON parsing, not pattern
# matching. The harvester publishes atomically and writes an explicit
# error envelope (summary: null) on every refusal.

# JSON_OUT / JSON_LEG_OUT are set in Step 0a and HAS_JSON alongside HAS_XML.

if [ "$JSON_MODE" -eq 1 ] || [ "$HAS_JSON" -eq 1 ]; then
    # Remove any artifact from a previous run BEFORE anything can fail --
    # including the dependency check below. A stale successful envelope left
    # at the canonical path outlives the run that failed to replace it, and
    # CI that uploads artifacts regardless of exit code would attribute that
    # old success to this run. The LEG alias is left alone -- it means the
    # latest COMPLETED run of this configuration, and only a complete record
    # of this run may replace it.
    rm -f "$JSON_OUT"
    if ! command -v python3 >/dev/null 2>&1; then
        # Fail rather than skip: JSON=1 asked for an artifact, and silently
        # producing none is the false-green this step exists to close. Leave
        # an error envelope behind so a consumer reading only the file sees
        # the reason instead of a missing path. It carries identity like every
        # other envelope -- an unattributable failure is the one a dashboard
        # most needs to place.
        {
            printf '{"schema": "utest-json-v2", "testcases": [], "skip_blocks": [],\n'
            printf ' "summary": null, "summary_error": "no_python3",\n'
            printf ' "detail": "python3 unavailable -- artifact could not be assembled",\n'
            printf ' "run_identity": '
            cat "$IDENTITY_RECORD" 2>/dev/null || printf 'null'
            printf '}\n'
        } > "$RECORD_DIR/.no-python3.json" &&
            utest_publish "$RECORD_DIR/.no-python3.json" "$JSON_RECORD" && JSON_PUBLISHED=1
        echo -e "  ${RED}[UTEST]${RESET} python3 not available -- cannot assemble $JSON_RECORD"
        UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
    else
        # The harvester writes a PRIVATE staged file, which is then published
        # through the same utest_publish() every other document goes through.
        # An earlier draft let the harvester write the record directly and
        # hand-rolled the alias copies here, which forked the lifecycle in two
        # ways an adversarial review caught: a leg-alias failure was swallowed
        # without failing the run, and a canonical-alias failure left
        # JSON_PUBLISHED at 0, so the EXIT guard treated an already-landed
        # record as unpublished and overwrote it with a run_incomplete
        # refusal. One publication path is what makes the contract testable.
        # --capture is passed whenever a model EXISTS -- reconciled or refused.
        # Gating on CAPTURE_ATTACH alone withheld the refusal model from the
        # harvester, so a run whose reconciliation FAILED still published a
        # normal JSON envelope with a green summary and no captured fields,
        # and JSON_PUBLISHED then stopped finalization from replacing it. The
        # flag is omitted only when capture was never attempted at all.
        JSON_CAPTURE_ARGS=""
        if [ -s "${CAPTURE_MODEL:-}" ]; then
            JSON_CAPTURE_ARGS="--capture $CAPTURE_MODEL"
        fi
        # shellcheck disable=SC2086 # deliberate word-split: empty means no flag
        JSON_ERR=$(python3 "$PROJECT/scripts/utest-json-harvest.py" \
                       "$TEST_LOG" "$RECORD_DIR/.harvest.json" --identity "$IDENTITY_RECORD" \
                       $JSON_CAPTURE_ARGS 2>&1) &&
            JSON_RC=0 || JSON_RC=$?
        if [ -f "$RECORD_DIR/.harvest.json" ]; then
            utest_publish "$RECORD_DIR/.harvest.json" "$JSON_RECORD" && JSON_PUBLISHED=1
        fi
        if [ "$JSON_RC" -eq 0 ]; then
            echo -e "${CYAN}[TEST]${RESET} JSON results written: $JSON_RECORD"
        else
            # rc 3 is "no stream at all", which is only a failure when the
            # run actually asked for JSON. Every other refusal means records
            # DID reach serial and did not agree with their own summary.
            if [ "$JSON_RC" -eq 3 ] && [ "$JSON_MODE" -eq 0 ]; then
                :
            else
                echo -e "  ${RED}[UTEST]${RESET} JSON artifact refused: ${JSON_ERR}"
                UTEST_FAIL=$(( ${UTEST_FAIL:-0} + 1 ))
            fi
        fi
    fi
fi

# Both formats have had their chance to assemble, so the record is finished:
# commit its marker and publish the aliases from it. This happens BEFORE the
# verdict on purpose. The EXIT guard would finalize too, but only after the
# exit status is already decided, so a failed alias write or an uncommittable
# marker discovered there could never fail the run -- and both are exactly the
# kind of silent artifact loss this lifecycle exists to make loud.
# `|| true` because finalization returns non-zero when the marker could not
# land, and a bare non-zero statement under `set -e` would abort the script
# right here -- skipping the verdict summary the operator reads. The failure
# is not lost: utest_commit_record already incremented UTEST_FAIL, so the
# verdict below fails the run and SAYS why.
utest_finalize_record complete || true

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
