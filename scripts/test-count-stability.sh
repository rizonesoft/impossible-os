#!/usr/bin/env bash
# test-count-stability.sh -- prove the kernel assertion total is a fact about
# the code, and name the suite responsible when it is not.
#
# WHY THIS EXISTS. "PASS: N kernel tests passed" is what every reader of this
# repo treats as the regression signal, and it has been caught MOVING on an
# unchanged tree: one run reported 27971 and five reported 27965, zero failures
# either way, every edit stashed. A number that drifts on its own cannot be
# compared, and comparing it is the only thing anyone does with it.
#
# Until now a drift was also unattributable. The runner computed each suite's
# assertion delta and discarded it on the next line, so the sole artifact of a
# moving total was the total; there was no way to ask WHICH suite moved. The
# runner now emits one [COUNT] record per suite plus a [COUNT-END] trailer
# (verbose runs only -- see below), and this script is their consumer.
#
# WHAT IT REFUSES TO DO. It does not compare two totals and call them equal.
# Before any comparison it validates that each run's trace is COMPLETE:
# ordinals contiguous from 1, no duplicates, no malformed records, none marked
# `trunc=1`, and a trailer whose `records=` matches what was actually counted.
# A truncated trace compares its surviving subset perfectly while the record
# that moved is the one missing -- that failure mode reports "stable" and is
# worse than reporting nothing, so an incomplete trace is a hard error here.
#
# WHY VERBOSE. The records cost real serial I/O: emitting them unconditionally
# was measured at 10.7s against a 3.8-4.6s baseline for the same quiet suite.
# The quiet path is what every automated gate runs, so the trace is suppressed
# there and this harness passes QUIET=0. Because that makes the compared runs
# verbose while the drift was originally seen on quiet ones, --cross-mode
# additionally reconciles a quiet total against a verbose one, so a count that
# depends on the MODE is caught rather than assumed away.
#
# WHY --mem. Ten identical runs are weak evidence, not proof: at the originally
# observed rate of one deviation in six runs, ten clean runs still miss it about
# 16% of the time, and any number of clean runs only ever bounds the rate. The
# suspect mechanism is host state -- the same runs whose totals moved also
# carried allocator-exhaustion messages, and the firmware memory map itself
# varies boot to boot (133 vs 135 descriptors, 519259 vs 519263 free frames on
# two runs of one tree). So rather than wait for the variance, --mem FORCES the
# host state to differ: the same suite is run at several guest RAM sizes and
# every per-suite count must still agree. A suite whose assertion count is a
# function of available memory shows up on the first pass instead of on the
# unlucky night.
#
# Usage:
#   bash scripts/test-count-stability.sh                 # 10 runs, same config
#   bash scripts/test-count-stability.sh --runs 4
#   bash scripts/test-count-stability.sh --mem 1G,2G,3G  # designed experiment
#   bash scripts/test-count-stability.sh --cross-mode    # quiet vs verbose total
#   bash scripts/test-count-stability.sh --parse-only <log>...   # offline check
# Exit: 0 only if every run's trace is complete AND all runs agree.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

GREEN='\033[0;32m'; RED='\033[0;31m'; DIM='\033[0;90m'; CYAN='\033[0;36m'; NC='\033[0m'

RUNS=10
MEMS=""
CROSS_MODE=0
PARSE_ONLY=0
PARSE_LOGS=()
OUT_DIR="build/count-stability"

while [ $# -gt 0 ]; do
    case "$1" in
        --runs)       RUNS="${2:-}"; shift 2 ;;
        --mem)        MEMS="${2:-}"; shift 2 ;;
        --cross-mode) CROSS_MODE=1; shift ;;
        --parse-only) PARSE_ONLY=1; shift; PARSE_LOGS=("$@"); break ;;
        --out)        OUT_DIR="${2:-}"; shift 2 ;;
        -h|--help)    sed -n '2,52p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$RUNS" in
    ''|*[!0-9]*) echo "--runs needs a non-negative integer, got '$RUNS'" >&2; exit 2 ;;
esac
if [ "$RUNS" -lt 2 ] && [ -z "$MEMS" ] && [ "$PARSE_ONLY" -eq 0 ]; then
    echo "--runs must be >= 2: a single run has nothing to compare against" >&2
    exit 2
fi

# --- The parser/comparator -------------------------------------------------
#
# Kept in one python block rather than shelled out to awk/sort, because the
# completeness rules (contiguity, duplicate ordinals, trailer reconciliation)
# are exactly the part that must not be approximated. It reads each log, emits
# a verdict, and names the differing suites.
run_compare() {
    python3 - "$@" <<'PY'
import re, sys

# The record contract, as emitted by src/kernel/test/test_runner.c:
#   [COUNT] #<ordinal> <category> <suite name> p=<n> f=<n> s=<n> P=<n>
#   [COUNT-END] records=<n> p=<n> f=<n> s=<n> P=<n>
# The suite name may contain spaces, and -- as this repo found the hard way --
# may itself contain the literal text "[COUNT-END]" (a suite is named after the
# trailer it tests). So the record is anchored at "[COUNT] #" and the trailing
# four fields are matched from the END of the line, leaving whatever is between
# as the name. Matching the marker anywhere in the line would let a suite NAME
# masquerade as a trailer.
# Both patterns are anchored at the klog TAG boundary ("TEST: ") rather than
# matched anywhere in the line. Without that anchor an ordinary verbose PASS
# line for a suite NAMED after this format -- "TEST: Harness: count-trace
# record format :: ..." -- reads as a malformed record, which is exactly what
# the first run of this script reported. The tag is the only place a real
# record can start.
ANSI = re.compile(r'\x1b\[[0-9;]*m')
REC  = re.compile(r'TEST: \[COUNT\] #(\d+) (\S+) (.*?) p=(\d+) f=(\d+) s=(\d+) P=(\d+)\s*$')
END  = re.compile(r'TEST: \[COUNT-END\] records=(\d+) p=(\d+) f=(\d+) s=(\d+) P=(\d+)\s*$')
CAND = 'TEST: [COUNT'
TRUNC = ' trunc=1'

def parse(path):
    """Return (records, trailer, problems). records maps ordinal -> tuple."""
    records, problems, trailer = {}, [], None
    try:
        with open(path, 'r', errors='replace') as fh:
            lines = fh.readlines()
    except OSError as e:
        return {}, None, ['cannot read %s: %s' % (path, e)]

    for n, raw in enumerate(lines, 1):
        line = ANSI.sub('', raw).rstrip('\n')
        if TRUNC in line and 'TEST: [COUNT] #' in line:
            problems.append('line %d: record marked trunc=1 -- its suite name '
                            'is unusable as a comparison key' % n)
            continue
        m = END.search(line)
        if m and 'TEST: [COUNT] #' not in line:
            if trailer is not None:
                problems.append('line %d: a second [COUNT-END] trailer' % n)
            trailer = tuple(int(x) for x in m.groups())
            continue
        m = REC.search(line)
        if not m:
            if CAND in line:
                problems.append('line %d: malformed [COUNT] record: %s'
                                % (n, line.strip()[:120]))
            continue
        ordinal = int(m.group(1))
        if ordinal in records:
            problems.append('line %d: duplicate ordinal #%d' % (n, ordinal))
            continue
        records[ordinal] = (m.group(2), m.group(3),
                            int(m.group(4)), int(m.group(5)),
                            int(m.group(6)), int(m.group(7)))

    # Everything below is FAIL-CLOSED on purpose. The whole value of this
    # script is that it refuses to bless a trace it cannot vouch for, and a
    # check that only looks at `passed` blesses a trace whose failed/skipped/
    # pending columns are nonsense -- which is exactly how a suite that
    # silently traded 6 passes for 4 skips hid for as long as it did.
    if trailer is None:
        problems.append('no [COUNT-END] trailer: the run did not finish, or '
                        'the trace was never enabled (needs COUNT_TRACE=1)')
    else:
        # The ordinal set must be EXACTLY 1..records. Checking only for gaps
        # below max() accepts an out-of-contract ordinal 0, and checking only
        # the total accepts a set that is the right size and the wrong members.
        expected = set(range(1, trailer[0] + 1))
        got = set(records)
        if got != expected:
            extra   = sorted(got - expected)
            missing = sorted(expected - got)
            if extra:
                problems.append('%d out-of-contract ordinal(s): %s%s -- '
                                'ordinals run 1..records'
                                % (len(extra),
                                   ', '.join('#%d' % i for i in extra[:8]),
                                   ' ...' if len(extra) > 8 else ''))
            if missing:
                problems.append('%d missing ordinal(s): %s%s -- the trace is '
                                'TRUNCATED, not stable'
                                % (len(missing),
                                   ', '.join('#%d' % i for i in missing[:8]),
                                   ' ...' if len(missing) > 8 else ''))
        if trailer[0] != len(records):
            problems.append('trailer announces records=%d but %d were counted '
                            '-- the trace is TRUNCATED, not stable'
                            % (trailer[0], len(records)))

        # Reconcile ALL FOUR columns, not just passed. A skip is how the AVX2
        # suites expressed their drift, so a checker blind to the skip column
        # is blind to the exact bug this script was written for.
        for col, name in ((2, 'passed'), (3, 'failed'),
                          (4, 'skipped'), (5, 'pending')):
            summed = sum(r[col] for r in records.values())
            if summed != trailer[col - 1]:
                problems.append('per-suite %s sums to %d but the trailer '
                                'reports %d for the run'
                                % (name, summed, trailer[col - 1]))

    # (category, suite) is the comparison KEY across runs. A duplicate makes
    # the cross-run diff silently compare one of them and ignore the other.
    seen = {}
    for ordinal, r in records.items():
        seen.setdefault((r[0], r[1]), []).append(ordinal)
    dupes = {k: v for k, v in seen.items() if len(v) > 1}
    if dupes:
        shown = list(dupes.items())[:5]
        problems.append('%d duplicate (category, suite) key(s), which cannot '
                        'be compared across runs: %s'
                        % (len(dupes),
                           '; '.join('%s / %s at %s' % (k[0], k[1],
                                     ', '.join('#%d' % o for o in v))
                                     for k, v in shown)))
    return records, trailer, problems

logs = sys.argv[1:]
parsed, failed = [], False
for path in logs:
    records, trailer, problems = parse(path)
    label = path.rsplit('/', 1)[-1]
    if problems:
        failed = True
        print('  INCOMPLETE %s' % label)
        for p in problems:
            print('      %s' % p)
    else:
        print('  ok         %s -- %d suites, %d assertions'
              % (label, len(records), trailer[1]))
    parsed.append((label, records, trailer))

if failed:
    print('')
    print('REFUSING to compare: at least one trace is incomplete. A truncated '
          'trace compares its surviving subset perfectly while the record that '
          'moved is the one missing.')
    sys.exit(1)

if len(parsed) < 2:
    sys.exit(0)

# Compare every run against the first, keyed by (category, suite name) rather
# than by ordinal: a suite that stops registering shifts every later ordinal,
# and an ordinal-keyed diff would then report thousands of differences instead
# of the one that happened.
base_label, base_records, base_trailer = parsed[0]
base = {(r[0], r[1]): r[2:] for r in base_records.values()}
diffs = []
for label, records, trailer in parsed[1:]:
    cur = {(r[0], r[1]): r[2:] for r in records.values()}
    for key in sorted(set(base) | set(cur)):
        b, c = base.get(key), cur.get(key)
        if b == c:
            continue
        name = '%s / %s' % key
        if b is None:
            diffs.append('%s: suite present only in %s' % (name, label))
        elif c is None:
            diffs.append('%s: suite present only in %s' % (name, base_label))
        else:
            diffs.append('%s: %s p=%d f=%d s=%d P=%d  vs  %s p=%d f=%d s=%d P=%d'
                         % (name, base_label, b[0], b[1], b[2], b[3],
                            label, c[0], c[1], c[2], c[3]))

# Trailers are compared as whole tuples. Per-suite rows can all agree while a
# run's totals differ -- an assertion made outside any suite body would land in
# the trailer and in no record -- and a comparator that never looks would call
# that stable.
base_trailer_all = parsed[0][2]
for label, _, trailer in parsed[1:]:
    if trailer != base_trailer_all:
        diffs.append('run TRAILER: %s records=%d p=%d f=%d s=%d P=%d  vs  '
                     '%s records=%d p=%d f=%d s=%d P=%d'
                     % ((base_label,) + base_trailer_all + (label,) + trailer))

print('')
if diffs:
    print('UNSTABLE -- %d suite(s) reported different counts across %d runs:'
          % (len(diffs), len(parsed)))
    for d in diffs[:40]:
        print('  %s' % d)
    if len(diffs) > 40:
        print('  ... and %d more' % (len(diffs) - 40))
    print('')
    print('This is the finding, not noise. Bound the loop so the count is '
          'fixed, or report the variable assertions in their own bucket so the '
          'headline total stays comparable. Do not widen a comparison to '
          'accept two totals.')
    sys.exit(1)

print('STABLE -- %d runs, %d suites each, %d assertions every time'
      % (len(parsed), len(base_records), base_trailer[1]))
sys.exit(0)
PY
}

if [ "$PARSE_ONLY" -eq 1 ]; then
    if [ "${#PARSE_LOGS[@]}" -eq 0 ]; then
        echo "--parse-only needs at least one log path" >&2
        exit 2
    fi
    run_compare "${PARSE_LOGS[@]}"
    exit $?
fi

mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/run-*.log

# Build ONCE up front. Every run must exercise the same image: rebuilding
# between runs would let a rebuild difference masquerade as count instability,
# which is the opposite of what this measures.
echo -e "${CYAN}[count-stability]${NC} building once, and pinning the source tree every run"
if ! bash scripts/build.sh >/dev/null 2>&1; then
    echo -e "${RED}[count-stability] build failed${NC} -- see build/build.log"
    tail -20 build/build.log 2>/dev/null
    exit 1
fi
if ! tail -1 build/build.log 2>/dev/null | grep -q '=== BUILD OK ==='; then
    echo -e "${RED}[count-stability] build did not report BUILD OK${NC}"
    exit 1
fi

# CONTENT-BIND what the sweep is characterising. "Built once up front" is not
# an invariant this script can assert on its own: every iteration runs the test
# runner, which runs its own incremental build, so a source edit landing
# mid-sweep would rebuild the kernel and the later runs would compare DIFFERENT
# code while reporting as repeated executions of one. That reads as instability
# in the counts -- the exact signal this script exists to measure -- so it has
# to be excluded by hash rather than by intent.
#
# The hash is over the SOURCE TREE, not over build/kernel.exe. Hashing the
# binary was the obvious first answer and it is wrong: the build stamps a
# monotonic build number and a timestamp into the image, so two builds of one
# unchanged tree are never byte-identical and the check failed run-01 every
# time. Nor is the system disk usable -- the runner patches boot.conf into it
# per run (test=1, test_quiet=2, the category filter), so it is SUPPOSED to
# differ. What must not move is the input: committed state, every tracked
# modification, and the untracked set.
source_digest() {
    {
        git rev-parse HEAD 2>/dev/null || echo "no-head"
        git diff HEAD 2>/dev/null
        git status --porcelain 2>/dev/null
    } | sha256sum 2>/dev/null | cut -d' ' -f1
}
BASE_DIGEST="$(source_digest)"
if [ -z "$BASE_DIGEST" ]; then
    echo -e "${RED}[count-stability]${NC} cannot hash the source tree -- refusing"
    echo "        Without it a source edit landing mid-sweep would be reported"
    echo "        as count instability, which is precisely the wrong answer."
    exit 1
fi
if [ ! -f build/kernel.exe ]; then
    echo -e "${RED}[count-stability]${NC} build/kernel.exe missing after a green build"
    exit 1
fi

# The run plan: either N runs at the default memory size, or one run per size.
PLAN=()
if [ -n "$MEMS" ]; then
    IFS=',' read -r -a MEM_LIST <<< "$MEMS"
    for m in "${MEM_LIST[@]}"; do
        [ -n "$m" ] && PLAN+=("$m")
    done
    if [ "${#PLAN[@]}" -lt 2 ]; then
        echo "--mem needs at least two sizes to compare, got '$MEMS'" >&2
        exit 2
    fi
else
    for _ in $(seq 1 "$RUNS"); do PLAN+=(""); done
fi

FAILED_RUNS=0
idx=0
for mem in "${PLAN[@]}"; do
    idx=$((idx + 1))
    label="run-$(printf '%02d' "$idx")"
    [ -n "$mem" ] && label="$label-mem$mem"

    # COUNT_TRACE=1 is what enables the trace at all; TEST_MEM is honoured by
    # scripts/test.sh. test.sh runs its own incremental build, which is a no-op
    # against the image built above -- and the digest check below is what
    # actually PROVES it was a no-op, rather than trusting that it was.
    if [ -n "$mem" ]; then
        TEST_MEM="$mem" bash scripts/test.sh COUNT_TRACE=1 >/dev/null 2>&1
    else
        bash scripts/test.sh COUNT_TRACE=1 >/dev/null 2>&1
    fi
    rc=$?

    if [ "$(source_digest)" != "$BASE_DIGEST" ]; then
        echo -e "  ${RED}FAIL${NC} $label -- the SOURCE TREE changed mid-sweep"
        echo "        Every run must exercise one build of one tree or the"
        echo "        comparison is meaningless: the next iteration rebuilds and"
        echo "        the counts that follow describe different code. Re-run on a"
        echo "        quiescent tree."
        FAILED_RUNS=$((FAILED_RUNS + 1))
        break
    fi

    if [ ! -f build/test.log ]; then
        echo -e "  ${RED}FAIL${NC} $label -- no serial log produced"
        FAILED_RUNS=$((FAILED_RUNS + 1))
        continue
    fi
    cp build/test.log "$OUT_DIR/$label.log"
    if [ "$rc" -ne 0 ]; then
        # A failing suite is reported, not swallowed -- but its trace is still
        # kept and compared: a run whose assertion COUNT moved is exactly the
        # thing being hunted, and it may well be why the suite went red.
        echo -e "  ${DIM}note${NC} $label -- scripts/test.sh exited $rc (trace kept)"
        FAILED_RUNS=$((FAILED_RUNS + 1))
    else
        summary=$(sed -E 's/\x1b\[[0-9;]*m//g' "$OUT_DIR/$label.log" \
                  | grep -oE '=== [0-9]+ tests passed[^=]*' | tail -1)
        echo -e "  ${GREEN}ok${NC}   $label -- ${summary:-no summary line}"
    fi
done

echo ""
echo -e "${CYAN}[count-stability]${NC} completeness + comparison"
run_compare "$OUT_DIR"/*.log
compare_rc=$?

if [ "$CROSS_MODE" -eq 1 ] && [ "$compare_rc" -ne 0 ]; then
    echo ""
    echo -e "${CYAN}[count-stability]${NC} cross-mode: SKIPPED"
    echo "        The traced runs did not agree with each other, so a quiet"
    echo "        total that differs from one of them says nothing about the"
    echo "        MODE -- it is the same instability, seen once more. Fix the"
    echo "        instability above, then re-run with --cross-mode."
elif [ "$CROSS_MODE" -eq 1 ]; then
    echo ""
    echo -e "${CYAN}[count-stability]${NC} cross-mode: quiet total vs traced total"
    # The trace itself is verbose-only, so the quiet leg is compared on the
    # headline total alone. That is the whole point of the check: it proves the
    # number this harness measures in verbose mode is the same number the gates
    # read in quiet mode, rather than assuming the two modes count alike.
    # SEVERAL ordinary quiet samples, not one. The traced runs above are
    # checked N times against each other; comparing that to a single untraced
    # run would leave the mode every gate actually uses sampled exactly once,
    # which is the sampling depth this whole script exists to reject.
    traced_total=$(sed -E 's/\x1b\[[0-9;]*m//g' "$OUT_DIR"/run-01*.log 2>/dev/null \
                   | grep -oE '=== [0-9]+ tests passed' | tail -1 \
                   | grep -oE '[0-9]+')
    if [ -z "$traced_total" ]; then
        echo -e "  ${RED}FAIL${NC} could not read the traced total"
        compare_rc=1
    elif [ "$(source_digest)" != "$BASE_DIGEST" ]; then
        echo -e "  ${RED}FAIL${NC} the source tree changed before the cross-mode leg"
        compare_rc=1
    else
        # The traced trailer's four counters, to reconcile each quiet sample
        # against. Comparing only `passed` is fail-OPEN: the kernel keeps the
        # "tests passed" prefix on FAILURE summaries too, so a quiet run with
        # the same passes plus new failures satisfies a passes-only check.
        traced_tuple=$(sed -E 's/\x1b\[[0-9;]*m//g' "$OUT_DIR"/run-01*.log 2>/dev/null \
                       | grep -oE '=== [0-9]+ tests passed, [0-9]+ (failed|FAILED), [0-9]+ skipped, [0-9]+ pending' \
                       | tail -1 | grep -oE '[0-9]+' | tr '\n' ' ')
        quiet_totals=""
        for q in 1 2 3; do
            if ! bash scripts/test.sh QUIET=1 >/dev/null 2>&1; then
                echo -e "  ${RED}FAIL${NC} quiet sample $q: the test runner exited nonzero"
                echo "        A failing run is not a usable cross-mode sample, and"
                echo "        its summary line still carries the 'tests passed'"
                echo "        prefix -- so accepting it would compare a red run's"
                echo "        pass count as though the run were green."
                compare_rc=1
                continue
            fi
            qtuple=$(sed -E 's/\x1b\[[0-9;]*m//g' build/test.log 2>/dev/null \
                     | grep -oE '=== [0-9]+ tests passed, [0-9]+ (failed|FAILED), [0-9]+ skipped, [0-9]+ pending' \
                     | tail -1 | grep -oE '[0-9]+' | tr '\n' ' ')
            if [ -z "$qtuple" ]; then
                echo -e "  ${RED}FAIL${NC} quiet sample $q produced no parseable summary line"
                compare_rc=1
                continue
            fi
            quiet_totals="$quiet_totals [$qtuple]"
            if [ "$qtuple" != "$traced_tuple" ]; then
                echo -e "  ${RED}FAIL${NC} quiet sample $q reports [$qtuple], traced runs report [$traced_tuple]"
                echo "        (passed failed skipped pending). Either the counts depend"
                echo "        on the LOGGING MODE -- so the gates and this harness"
                echo "        measure different things -- or the quiet path is itself"
                echo "        unstable. Both are root causes, not a reason to pick a"
                echo "        mode and move on."
                compare_rc=1
            fi
        done
        if [ "$compare_rc" -eq 0 ]; then
            echo -e "  ${GREEN}ok${NC}   3 green quiet samples and the traced runs all report [$traced_tuple] (passed failed skipped pending)"
        else
            echo "        quiet samples were:$quiet_totals"
        fi
    fi
fi

echo ""
if [ "$compare_rc" -eq 0 ] && [ "$FAILED_RUNS" -eq 0 ]; then
    echo -e "${GREEN}[count-stability] PASS${NC} -- logs in $OUT_DIR/"
    exit 0
fi
echo -e "${RED}[count-stability] FAIL${NC} -- logs in $OUT_DIR/"
exit 1
