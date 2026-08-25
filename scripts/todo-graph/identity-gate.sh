#!/usr/bin/env bash
# ============================================================================
# identity-gate.sh -- run corpus_resolution_snapshot.py as a GATE instead of
# as a thing a human remembers to invoke.
#
# WHAT THIS IS. THREE checks over one base..head range, not one (section 18
# widened it; the header said "exactly one question" long after that stopped
# being true):
#   1. RESOLVER-CODE DIFFERENTIAL -- does the resolver still produce the same
#      verdict for every symbol ref it produced a verdict for before?
#   2. PRODUCER DIFFERENTIAL -- do the base and head `build.py` emit the same
#      stamped-ref population, over BOTH corpora? The resolver differential
#      structurally cannot see this: it builds the cache ONCE, so a ref the
#      producer stops emitting is absent from both of its walks.
#   3. PROTOCOL SEPARATION -- is a vocabulary/schema change provably a
#      data-only migration, with every executable closure file byte-identical?
# It is still NOT a replacement for the full manual snapshot contract, and that
# boundary is deliberate rather than an oversight.
#
# THE KEY-STABILITY PROBLEM, DISSOLVED RATHER THAN WORKED AROUND. Occurrence
# keys embed `item_idx`/`ref_i`, so a TRACKED baseline file moves every time a
# TODO gains an item -- section 12 parked the wiring on exactly that, because
# such a baseline either misses real regressions (as a WARN) or fires on
# ordinary TODO edits (as an ERROR). There is therefore NO tracked baseline
# here. The cache is built ONCE and BOTH walks read that same byte-identical
# file, so the key set is identical by construction and a TODO edit cannot move
# a key: only the resolver CODE varies between the two walks, so any difference
# is attributable to it. Nothing to regenerate, nothing to go stale.
#
# ONE CACHE HAS EXACTLY ONE EXCEPTION, and it is not a loophole: when the
# PRODUCER CONTRACT IDENTITY moves base..head, no single artifact is readable
# by both readers at all, so each side reads a cache built by its own producer
# and the producer differential -- which compares precisely the population the
# walks consume -- is what re-establishes the identical key set. See the
# "PRODUCER CONTRACT IDENTITY" block below for why this is the only shape that
# can work.
#
# WHY NOT A PRE-COMMIT HOOK (the mechanism section 12 tried first):
#   1. Git hooks are OPT-IN per developer (scripts/install-hooks.sh), so a hook
#      gate protects only the authors who chose to be protected -- which is the
#      exact property section 16 exists to remove.
#   2. Conditioned on the worktree diff it validates the WRONG artifact under
#      partial staging (`git add -p`, `git commit -- <paths>`), because the
#      resolver reads worktree files while the commit records the index.
#   3. It lives in .githooks/, which is control plane the unattended runner may
#      not edit -- so the runner could not have shipped that mechanism at all.
#
# WHAT THIS GATE HONESTLY IS NOT. This repo pushes DIRECTLY TO MAIN, so a
# push-triggered CI job runs after main has already advanced: it DETECTS, it
# does not PREVENT. Preventing needs a server-side pre-receive hook or a
# protected merge queue, both operator-reserved. What is guaranteed here is
# ADJUDICATION -- see the range rule below -- so no push slips through
# unexamined even though a bad one lands before it is caught.
#
# THE RANGE IS `last successfully gated SHA .. HEAD`, NOT `event.before..HEAD`.
# With `cancel-in-progress`, push P1 carrying a regression is cancelled when P2
# arrives; P2 then compares P1..P2, sees the resolver closure unchanged, early-
# exits, and NOTHING ever adjudicates P1 (Codex design review, section 16 --
# and the cancellation is real here: build.yml runs e49acdc2, ca61219d and
# faacf6eb were all cancelled by rapid pushes on 2026-08-06). Walking back to
# the last SHA this gate actually passed closes that hole: a cancelled run's
# range is absorbed by the next completed one.
#
# Exit codes (every non-zero FAILS the job -- "could not run" is never "passed"):
#   0  no prior verdict changed, and nothing was gained or added unreviewed
#   1  REGRESSION -- a prior verdict was dropped/lost/moved/reclassified, or an
#      unreviewed GAINED/ADDED mapping appeared (see --strict)
#   2  USAGE -- this gate was invoked wrongly (unknown argument, missing option
#      value, malformed IDENTITY_GATE_BUDGET_SECS). A caller mistake, never a
#      statement about the tree under test.
#   3  INFRASTRUCTURE -- the gate could not run: no usable base, a protocol
#      migration bundled with a resolver change, a cache/baseline failure, or a
#      worktree it could not materialize
#
#      An rc 3 raised by the protocol read carries a STABLE LEADING TOKEN so a
#      caller can branch on WHICH of these it is without parsing prose
#      (section 50). They are NOT interchangeable: three of them say nothing is
#      wrong with the tree under test, and two say the REPOSITORY -- not the
#      tree and not the gate -- is what could not answer. The base side is
#      adjudicated first, so a base token wins when both endpoints fail.
#        BASE_PREDATES_PROTOCOL       the base is older than the snapshot
#                                     mechanism; the range is unadjudicatable
#        BASE_PROTOCOL_INCOMPLETE     the base carries part of the protocol and
#                                     can express neither form -- either the
#                                     extraction was still in progress there,
#                                     or the rest was removed (the message says
#                                     which, from that base's own history)
#        BASE_PROTOCOL_REMOVED        the base's own history added the files
#                                     and they are gone: deleted or renamed
#        BASE_PROTOCOL_UNREADABLE     complete at the base, but unparseable
#        BASE_TREE_UNREADABLE         git could not say what the base contains
#        BASE_HISTORY_UNREADABLE      git could not traverse the base ancestry
#        BASE_HISTORY_INCOMPLETE      shallow/grafted/replaced, so "never added
#                                     in its history" would not be provable
#        HEAD_TREE_UNREADABLE         git could not say what the head commit
#                                     contains
#        BASE_PROTOCOL_NOT_A_FILE     the protocol PARSED, but through a path
#        HEAD_PROTOCOL_NOT_A_FILE     that is not a regular file -- a symlink
#                                     the reader followed out of the commit
#        BASE_TRANSPORT_FAILED        the gate could not carry its OWN record
#        HEAD_TRANSPORT_FAILED        between its own processes; machinery, not
#                                     a statement about either tree
#      Two of the names below are RESERVED SOURCE-INTEGRITY GUARDS rather than
#      classifications a tree can provoke: with PROTOCOL_PATHS as shipped, no
#      input reaches them, and only an edit to this file that adds a path
#      without extending the form mapping can. They are published so a future
#      editor sees the contract, and fixtured by mutating a copy of the script:
#        BASE_FORM_UNMAPPED           PROTOCOL_PATHS grew a path the form
#        HEAD_FORM_UNMAPPED           mapping does not cover; refuse rather
#                                     than classify around it
#        HEAD_PROTOCOL_ABSENT         the tree under test has no mechanism
#        HEAD_PROTOCOL_INCOMPLETE     the tree under test has part of one
#        HEAD_PROTOCOL_UNMATERIALIZED the commit has it, the working tree does
#                                     not: an incomplete/sparse checkout
#        HEAD_PROTOCOL_UNREADABLE     complete at HEAD, but unparseable
#
#      Two more are raised by the closure re-check that guards the exit
#      (section 51). They say nothing about the protocol and nothing about the
#      range; they say this gate could not hold its own subject still long
#      enough to certify it, so the operator action is to re-run, or to repair
#      the checkout, rather than to re-point the base.
#        CLOSURE_MOVED_UNDER_GATE     a closure member changed between the
#                                     measurement and the decision, so the
#                                     protocol evidence describes a tree that
#                                     is no longer here
#        CLOSURE_UNVERIFIABLE         a closure member could not be asked about
#                                     or hashed, or is not a regular file
#                                     reached through regular directories -- an
#                                     unanswered probe is never 'unchanged'
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

# The resolver behaviour closure. If every one of these is byte-identical
# base-vs-head there is nothing for a resolver differential to measure, so the
# gate early-exits. The closure must be COMPLETE or the early exit becomes the
# hole: a change to any file that can alter a verdict but is missing from this
# list would skip the walk entirely (Codex design review, section 16).
# THIS IS A "MUST THE GATE RUN?" TRIGGER LIST, NOT A "WHAT IS DIFFERENTIALLY
# COVERED?" LIST. The two are not the same and conflating them advertises
# coverage that does not exist (Codex consistency, section 16). Membership is
# decided by whether a change can alter a verdict, and the entries fall into
# two kinds:
#
#   DIFFERENTIALLY EXECUTED -- base and head versions both run, so a behaviour
#   change between them IS what the comparison measures:
#       corpus_resolution_snapshot.py, ref_resolution.py, resolve_symbol.py,
#       cache_schema.py -- imported by the snapshot since section 17, and it
#       decides whether the snapshot RUNS AT ALL (it can turn a completed walk
#       into an rc 3 infrastructure refusal, or the reverse). Omitting it would
#       have let a cache-schema-only change satisfy the byte-identical early
#       exit and execute NEITHER reader (Codex design review, section 17).
#
#   AFFECTS THE COMPARISON BUT IS NOT ITSELF DIFFERENTIALLED -- only the head
#   version ever runs, so a change forces the walk (conservative, never a false
#   pass) while this gate proves nothing about the change itself:
#       build.py          produces the ONE cache both walks read, so a change
#                         moves the shared input under both sides equally. That
#                         blind spot is now CLOSED by the producer differential
#                         above -- build.py stays listed here because a change
#                         to it must still force the resolver walk.
#       identity-gate.sh  is the driver; a change to it must re-run the gate.
#
# Contrast scripts/lint/check_stub_behind_stamp.py, which is deliberately
# ABSENT: it is neither executed by the walks nor an input to them, so listing
# it would force a walk that adjudicates nothing about it. That is the same
# false-coverage test applied consistently, not a different rule.
#
# EXEC_CLOSURE is the subset that can RUN CODE, and the split is load-bearing
# (section 18). The protocol values now live in the inert
# snapshot_protocol.json, so a protocol migration is a data-only diff -- and
# "every executable file is byte-identical, therefore no verdict-affecting
# change rode along" becomes a mechanical guarantee rather than a guess. That
# inference is the ONLY thing that lets a protocol migration through, so the
# .json is in CLOSURE (it must still trigger the gate) but deliberately NOT in
# EXEC_CLOSURE, while snapshot_protocol.py -- the loader, which does run -- is
# in both.
EXEC_CLOSURE=(
    "scripts/todo-graph/corpus_resolution_snapshot.py"
    "scripts/todo-graph/ref_resolution.py"
    "scripts/todo-graph/resolve_symbol.py"
    "scripts/todo-graph/cache_schema.py"
    "scripts/todo-graph/build.py"
    "scripts/todo-graph/snapshot_protocol.py"
    "scripts/todo-graph/producer_differential.py"
    "scripts/todo-graph/identity-gate.sh"
    # THE RETIREMENT PROOF ITSELF (section 20). This gate asks
    # check_bucket_emission.py which buckets the resolver declares it can emit,
    # and approves a data-only retirement on that answer -- so a commit that
    # weakened the checker WHILE retiring a bucket would be adjudicating its own
    # evidence. Inside the closure it must be byte-identical for the data-only
    # inference to hold, which forces the weakening to land as a separate,
    # differentialled change.
    "scripts/lint/check_bucket_emission.py"
)
CLOSURE=(
    "${EXEC_CLOSURE[@]}"
    "scripts/todo-graph/snapshot_protocol.json"
)

BASE_SHA=""
BASE_EXPLICIT=0
HEAD_SHA="HEAD"
KEEP_TMP=0

die_usage() {
    # A CALLER MISTAKE IS 2, not 3. The other three tools in this family
    # (corpus_resolution_snapshot, producer_differential,
    # check_consumer_delegation) all reserve 2 for a bad invocation, and this
    # driver collapsed argument errors into the infrastructure channel --
    # against the very non-collapsing contract section 16 established when it
    # separated rc 2 from rc 3 in the OTHER direction (Codex consistency,
    # section 18 review).
    printf '[identity-gate] USAGE: %s\n' "$1" >&2
    exit 2
}

die_infra() {
    printf '[identity-gate] INFRASTRUCTURE: %s\n' "$1" >&2
    printf '[identity-gate] this is NOT a pass -- the gate could not run.\n' >&2
    exit 3
}

log() { printf '[identity-gate] %s\n' "$1"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE_SHA="${2:-}"; BASE_EXPLICIT=1; shift 2 || die_usage "--base needs a value" ;;
        --head) HEAD_SHA="${2:-}"; shift 2 || die_usage "--head needs a value" ;;
        --keep-tmp) KEEP_TMP=1; shift ;;
        *) die_usage "unknown argument: $1" ;;
    esac
done

cd "$REPO_ROOT" || die_infra "cannot cd to repo root $REPO_ROOT"

# VALIDATED ONCE, BEFORE ANY PHASE SPAWNS ANYTHING.
BUDGET_SECS="${IDENTITY_GATE_BUDGET_SECS:-600}"
case "$BUDGET_SECS" in
    ''|*[!0-9]*) die_usage "IDENTITY_GATE_BUDGET_SECS must be a whole number of seconds, got '$BUDGET_SECS'" ;;
esac
[ "$BUDGET_SECS" -ge 1 ] || die_usage "IDENTITY_GATE_BUDGET_SECS must be >= 1 (0 DISABLES timeout(1) outright, which removes the bound entirely)"

# ONE MONOTONIC DEADLINE FOR THE WHOLE GATE. Granting each sequential phase the
# full budget meant the producer differential, the cache build, the two walks
# and the comparison could together run ~4x it -- roughly 40 minutes against
# build.yml's 20-minute job ceiling, so GitHub would kill the job before the
# gate could report its own bounded failure (Codex adversarial, section 18).
#
# AND IT IS A MONOTONIC CLOCK, NOT `date +%s`. A wall clock can step BACKWARD,
# which makes `remaining()` GROW and hands the later phases more time than the
# budget allows -- restoring the exact overrun this deadline exists to prevent.
# That is not hypothetical on this host: `test_build.sh` records three observed
# backward steps under WSL2 on 2026-08-06 (-1158ms, -262ms, -246ms) after a
# suspend/resync, and refuses such readings for the same reason.
# `time.monotonic()` cannot step backward by definition.
GATE_MONO_START="$(python3 -c 'import time; print(int(time.monotonic()))')" \
    || die_infra "cannot read a monotonic clock"
remaining() {
    local _now _r
    _now="$(python3 -c 'import time; print(int(time.monotonic()))')" || _now=""
    if [ -z "$_now" ]; then
        # A clock we cannot read is INFRASTRUCTURE, never "plenty of time".
        printf '1'
        return
    fi
    _r=$(( BUDGET_SECS - (_now - GATE_MONO_START) ))
    [ "$_r" -lt 1 ] && _r=1   # timeout(1) treats 0 as "no timeout"
    printf '%s' "$_r"
}

HEAD_RESOLVED="$(git rev-parse --verify "${HEAD_SHA}^{commit}" 2>/dev/null)" \
    || die_infra "head '$HEAD_SHA' is not a commit"

# ---------------------------------------------------------------------------
# Establish the base. Prefer the last SHA this gate actually PASSED (see the
# range rationale in the header); fall back to the push event's before-SHA, and
# then to HEAD~1. An unresolvable base is an INFRASTRUCTURE failure, never a
# silent pass: "we could not tell what changed" must not read as "nothing did".
# ---------------------------------------------------------------------------
# An EXPLICIT --base that does not resolve is an error, never a fallback. The
# fallback chain below exists to DERIVE a base nobody supplied; silently
# substituting HEAD~1 for a base a caller did name means the gate answers a
# different question than it was asked, and reports the answer as if it were
# the one requested. Found by fixture 22e, which passed a zero SHA and got a
# green run over an entirely different range.
if [ "$BASE_EXPLICIT" -eq 1 ]; then
    git rev-parse --verify --quiet "${BASE_SHA}^{commit}" >/dev/null 2>&1 \
        || die_infra "--base '$BASE_SHA' is not a commit in this repository"
fi

if [ -z "$BASE_SHA" ]; then
    BASE_SHA="${IDENTITY_GATE_LAST_GATED_SHA:-}"
fi
if [ -z "$BASE_SHA" ] || ! git rev-parse --verify --quiet "${BASE_SHA}^{commit}" >/dev/null 2>&1; then
    # The zero sentinel is what GitHub sends for a branch's first push and can
    # also appear after a force-push; treat it as absent rather than as a SHA.
    CAND="${GITHUB_EVENT_BEFORE:-}"
    case "$CAND" in
        0000000000000000000000000000000000000000|"") CAND="" ;;
    esac
    if [ -n "$CAND" ] && git rev-parse --verify --quiet "${CAND}^{commit}" >/dev/null 2>&1; then
        BASE_SHA="$CAND"
    elif git rev-parse --verify --quiet "${HEAD_RESOLVED}~1^{commit}" >/dev/null 2>&1; then
        BASE_SHA="$(git rev-parse "${HEAD_RESOLVED}~1")"
        log "no gated/event base reachable; falling back to HEAD~1"
    else
        die_infra "no usable base commit (no last-gated SHA, no reachable event before-SHA, no HEAD~1)"
    fi
fi
BASE_SHA="$(git rev-parse "${BASE_SHA}^{commit}")"

log "base $BASE_SHA"
log "head $HEAD_RESOLVED"

if [ "$BASE_SHA" = "$HEAD_RESOLVED" ]; then
    log "base == head; nothing to differentiate."
    exit 0
fi

# ---------------------------------------------------------------------------
# Measure the closure here; DECIDE on it further down, after the protocol has
# been read and classified (section 51).
# ---------------------------------------------------------------------------
# The head side hashes the WORKING-TREE file, not the committed blob. In CI
# they are the same thing; locally they are not, and hashing the commit would
# let an uncommitted resolver edit early-exit unexamined -- the gate must
# measure the code it is actually about to execute.
#
# THE CLOSURE EXIT ANSWERS TO THE PROTOCOL, NOT THE OTHER WAY ROUND. This
# check used to `exit 0` right here, before a single protocol byte had been
# read, so two commits carrying the SAME unloadable protocol and differing only
# in a `todo/` file returned 0 in silence -- and the head of that range became
# the next gated baseline. Section 18 had already answered the identical
# question the other way at the schema fast path ("the gate would report PASS
# for a protocol nothing can actually load"), so the two entry points
# contradicted each other; this early exit is the remaining door into that room.
#
# THE DECISION IS EXPRESSED BY POSITION, NOT BY A SECOND PREDICATE. Re-asking
# "is the protocol usable?" beside the exit means composing a weaker copy of a
# contract this file has already had to repair for disagreeing with itself, and
# a clean seven-field record is NOT that contract: `protocol_source_is_regular`
# refuses a protocol that parsed through a symlink -- an ancestor DIRECTORY
# symlink included -- and it runs only once the record is already in hand. A
# record-shaped predicate here would therefore have exited 0 on bytes from
# outside the commit, because `git hash-object` reads through that same symlink
# and leaves the closure byte-identical (Codex design review, section 51,
# [high]; the component walk it turns on is `path_is_regular_in_worktree`,
# itself written for the section 50 round 14 incident). So the exit MOVES to
# the far side of the whole adjudication instead, where every existing check
# has already run and there is no second copy to drift.
#
# It costs a base worktree and two protocol probes on a range that will exit 0
# -- seconds, in a job that already checks out at fetch-depth 0 under a
# 20-minute budget.
#
# IT MANUFACTURES NEW REFUSALS ON PURPOSE, AND THIS COMMENT HAS NOW BEEN WRONG
# ABOUT THEM TWICE. The first draft claimed none were possible, reasoning that
# every base-side token names a protocol-bearing file that differs base-vs-head
# while a range reaching here has proved none does. The second claimed EXACTLY
# ONE. Both were wrong in the same direction: making a refusal reachable where
# rc 0 used to be IS the change, so the honest statement is WHICH CLASSES, not
# how few (Codex test-coverage section 51 [high], then Codex adversarial
# section 51 round 2 [medium]).
#
# Newly reachable over a byte-identical closure, every one of them a protocol
# this gate cannot adjudicate:
#   BASE_/HEAD_PROTOCOL_UNREADABLE      present on both sides, unloadable  (22cd)
#   HEAD_PROTOCOL_NOT_A_FILE            parsed through a symlink           (22ce)
#   BASE_PROTOCOL_REMOVED               absent on both sides               (22cg)
#   BASE_PREDATES_PROTOCOL              absent, and never in this history
#   BASE_/HEAD_PROTOCOL_INCOMPLETE      part of a form, expressing neither
#
# What is NOT newly reachable is the ancient-base wedge section 50 was written
# to cure: a base predating the protocol hashes MISSING against a real blob, so
# it changes the closure and never arrives here at all.
CLOSURE_CHANGED=0
for f in "${CLOSURE[@]}"; do
    b="$(git rev-parse --quiet --verify "$BASE_SHA:$f" 2>/dev/null || echo MISSING)"
    h="$(git hash-object "$REPO_ROOT/$f" 2>/dev/null || echo MISSING)"
    if [ "$b" != "$h" ]; then
        log "closure changed: $f"
        CLOSURE_CHANGED=1
    fi
done
TMP_DIR="$(mktemp -d -t identity-gate.XXXXXX)" || die_infra "mktemp failed"

# A GATE-OWNED BYTECODE CACHE FOR EVERY PYTHON PHASE THIS GATE RUNS, not just
# the protocol read. `spec_from_file_location` + `exec_module` execute a
# timestamp-valid `.pyc` in preference to the source they were handed, and a
# poisoned one IS executed instead of the checked file (reproduced directly: a
# spliced cache returned its own value). That let untracked bytes decide a
# verdict about a commit, which is the one thing every check here exists to
# prevent -- and scoping the fix to `proto_of` alone would have left every
# other phase importing the same modules through the same poisoned cache.
# `PYTHONPYCACHEPREFIX` relocates BOTH lookup and write (Codex adversarial,
# section 50 round 14).
export PYTHONPYCACHEPREFIX="$TMP_DIR/pycache"
BASE_TREE="$TMP_DIR/base"
WALK_PIDS=""
cleanup() {
    # REAP BEFORE REMOVING. cleanup ran only on EXIT and never touched the
    # background walks, so a TERM/INT after they were spawned deleted the base
    # worktree and TMP_DIR while two `timeout` wrappers and their Python
    # children kept running against those paths for up to the budget -- leaked
    # processes writing into a directory that no longer exists (Codex
    # adversarial, section 18 round 3).
    if [ -n "$WALK_PIDS" ]; then
        # SIGNAL THE PROCESS GROUP, then ESCALATE, then give up -- never block.
        # TERM to the `timeout` wrapper alone left the python grandchild
        # running, and an unbounded `wait` afterwards meant a child ignoring
        # TERM hung cleanup forever: the worktree and TMP_DIR were never
        # removed and CI cancellation stalled until an outer job-level kill
        # (Codex adversarial, section 18 round 4, reproduced with a child
        # ignoring SIGTERM). `setsid` below puts each walk in its own group so
        # `kill -- -PGID` reaches the wrapper AND its children.
        for _p in $WALK_PIDS; do kill -TERM -- "-$_p" 2>/dev/null || kill -TERM "$_p" 2>/dev/null || true; done
        for _i in 1 2 3 4 5 6 7 8 9 10; do
            _alive=0
            for _p in $WALK_PIDS; do kill -0 "$_p" 2>/dev/null && _alive=1; done
            [ "$_alive" -eq 0 ] && break
            sleep 0.5
        done
        for _p in $WALK_PIDS; do kill -KILL -- "-$_p" 2>/dev/null || kill -KILL "$_p" 2>/dev/null || true; done
        for _p in $WALK_PIDS; do wait "$_p" 2>/dev/null || true; done
        WALK_PIDS=""
    fi
    if [ -d "$BASE_TREE" ]; then
        git worktree remove --force "$BASE_TREE" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_TMP" -eq 0 ]; then
        rm -rf "$TMP_DIR" 2>/dev/null || true
    else
        log "kept working files in $TMP_DIR"
    fi
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

git worktree add --detach "$BASE_TREE" "$BASE_SHA" >/dev/null 2>&1 \
    || die_infra "cannot materialize base worktree at $BASE_SHA"

# ---------------------------------------------------------------------------
# PROTOCOL SKEW. The base process WRITES with its own SNAPSHOT_SCHEMA and
# bucket enum; the head process REFUSES a baseline whose schema it does not
# recognise. That fails closed, which is correct, but it also means a change
# that legitimately evolves the protocol can never go green. Rather than
# adapting between protocols -- a breaking bump is precisely the case where the
# old form is NOT expressible in the new one -- the two kinds of change are
# mechanically SEPARATED: a protocol migration must carry no resolver change,
# so the full-population comparison always runs on the resolver change itself
# (Codex design review, section 16).
# ---------------------------------------------------------------------------
# The protocol probe reads a handful of constants, so it gets a small fixed
# share of the budget rather than all of it -- bounded by `remaining()` so the
# global deadline still dominates.
PROTOCOL_PROBE_CAP_SECS=60
probe_budget() {
    local _r
    _r="$(remaining)"
    if [ "$_r" -gt "$PROTOCOL_PROBE_CAP_SECS" ]; then
        printf '%s' "$PROTOCOL_PROBE_CAP_SECS"
    else
        printf '%s' "$_r"
    fi
}

proto_of() {  # $1 = tree root
    # EVALUATE the constants, never scrape their source text. The first cut
    # regex-captured the RHS of `ALL_BUCKETS` -- which is literally
    # `PRE_RESOLUTION_BUCKETS + POST_RESOLUTION_BUCKETS` and NEVER changes when
    # a bucket NAME does, so the protocol check compared a constant string to
    # itself and the fail-closed branch could not fire. The same regex also
    # swept up any comment line following the assignment, so an unrelated
    # comment edit read as a protocol change and wedged the gate (Codex
    # adversarial, section 16). Importing in a throwaway process per tree gives
    # the real ordered tuple and is immune to both.
    #
    # AND SINCE SECTION 18, READ THE VALUES AS DATA WHERE THE TREE OFFERS THEM.
    # snapshot_protocol.json is parsed with json.load and never executed, which
    # is what makes the pure-protocol-migration inference below sound: the file
    # that changed provably cannot have run code. A tree PREDATING the
    # extraction has no .json and its constants are only reachable by importing
    # -- that still works, but it is reported as `legacy` so the inference is
    # refused for it rather than silently extended to a tree it does not hold
    # for.
    # THE RECORD TRAVELS ON ITS OWN CHANNEL, NOT ON STDOUT. Capturing stdout
    # meant anything the imported modules emitted became part of the record:
    # `redirect_stdout` moved python-level prints aside and a newline check
    # caught framed contamination, but an UNTERMINATED direct write to fd 1
    # (`os.write(1, b"noise|")`) simply concatenated with the record, shifting
    # every field and producing a confident, wrong token (Codex adversarial,
    # section 50 round 18). Patching that variant would have invited a third,
    # so the record now goes to a file this function owns and stdout is
    # discarded entirely -- there is no longer a channel for a tree to write on.
    # A TRANSPORT FAILURE IS NOT A PROTOCOL VERDICT. Everything below moves the
    # record between two processes, and every step of that can fail for reasons
    # that say nothing about the tree: no temp file, no interpreter, an empty
    # write, an unreadable file. Folding those into UNREADABLE labelled a
    # perfectly valid base as malformed, and letting `head` fail silently
    # produced the generic un-tokened malformed-line message -- both the exact
    # failed-question-as-negative-answer fault this section fixed elsewhere,
    # reappearing in the machinery this section itself introduced (Codex
    # adversarial, section 50 round 20).
    local _rec _prc
    _rec="$(mktemp "$TMP_DIR/proto.XXXXXX" 2>/dev/null)" \
        || { printf 'TRANSPORT\n'; return 0; }
    # THE RECORD'S ADDRESS IS NEVER HANDED TO THE TREE. Passing the path as
    # argv[2] told the very code being imported where the gate's own answer
    # lived, and an `atexit` handler registered during import could replace a
    # finished record with a valid-looking one (Codex adversarial, section 50
    # round 19). The file is opened by the SHELL on fd 3, so no path reaches
    # the subprocess, and argv is scrubbed before any tree code runs.
    # NO `setsid` HERE, deliberately, unlike the walk phases below. Detaching
    # the probe into its own session put it outside the gate's process group,
    # so a TERM or INT delivered to the gate did not reach it: the probe could
    # outlive the signal, hold the gate for the remaining budget, and an
    # external escalation then skipped `cleanup` entirely, stranding the linked
    # base worktree and the tempdir (Codex adversarial, section 50 round 24,
    # [high]). The walk phases need `setsid` because they spawn children a bare
    # `timeout` could not reap; this probe spawns none, so it can stay in the
    # gate's own process group -- and `--foreground` is what actually keeps it
    # there. Dropping `setsid` alone was NOT enough: GNU `timeout` puts the
    # command in a new process group of its own unless asked not to, which
    # leaves the probe just as unreachable by a signal sent to the gate's group
    # (measured directly on coreutils 9.4: shell pgid 1005288, probe pgid
    # 1005292). `--foreground`'s caveat -- that children of the command are not
    # timed out -- costs nothing here precisely because this probe has none.
    # A SMALL SHARE OF THE BUDGET, not all of it. Reading a handful of
    # constants is constant-size work, but the probe was granted the entire
    # remaining budget -- so one stalled import could spend the whole 600s and
    # leave the producer, cache and corpus phases nothing (Codex perf, section
    # 50 post-commit review). The cap is the SMALLER of the phase cap and what
    # is left, so it can never extend the global deadline.
    timeout --foreground --kill-after=10s "$(probe_budget)" \
        python3 - "$1" 3>"$_rec" >/dev/null 2>/dev/null <<'PY'
import base64, contextlib, hashlib, importlib.util, json, os, sys, pathlib
# THE RECORD GOES OUT ON FD 3, which the shell opened. Rebinding sys.stdout to
# it keeps every `print` below -- including the UNREADABLE sentinels --
# unchanged, and leaves `redirect_stdout` in `_load` working as before, while
# a write straight to fd 1 goes to the discarded real stdout.
sys.stdout = os.fdopen(3, "w", encoding="utf-8", buffering=1)
root = pathlib.Path(sys.argv[1])
# NOTHING IMPORTED BELOW LEARNS THE SUBJECT PATH OR ANY OUTPUT PATH FROM argv.
sys.argv = [sys.argv[0]]
tg = root / "scripts/todo-graph"
sys.path.insert(0, str(tg))
source = "data"


def _load(name):
    # IMPORT-TIME OUTPUT GOES TO STDERR, NEVER INTO THE RECORD. This function's
    # stdout IS the protocol line the caller parses, so a module that prints
    # while being imported prepends a line to it -- the caller's `read` then
    # takes the noise as the record and dies with a generic "malformed" rc 3
    # carrying none of the stable tokens this section publishes (Codex
    # adversarial, section 50 round 17). An import-time print is noise, not a
    # protocol defect, so it is moved out of the way rather than classified.
    # A write straight to fd 1 escapes this, which is why the caller ALSO
    # refuses a multi-line record.
    spec = importlib.util.spec_from_file_location(name, tg / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    with contextlib.redirect_stdout(sys.stderr):
        spec.loader.exec_module(mod)
    return mod



data_path = tg / "snapshot_protocol.json"
if data_path.is_file():
    try:
        raw = json.loads(data_path.read_text(encoding="utf-8"))
        schema = raw["snapshot_schema"]
        # RAW TYPE FIRST. `list(...)` normalises a JSON OBJECT to its ordered
        # keys, so `{"unresolved_calllike": 1, ...}` produced a digest
        # identical to the real list while snapshot_protocol.load rejected it
        # outright -- bundled with a schema bump that left BUCKETS_CHANGED
        # false, the fast path exited 0 for a protocol nothing can load
        # (Codex adversarial, section 18 round 3; digest collision confirmed).
        pre_raw = raw["pre_resolution_buckets"]
        post_raw = raw["post_resolution_buckets"]
        renamed = raw.get("renamed_buckets") or {}
        retired = raw.get("retired_buckets") or []
        if not isinstance(renamed, dict) or not isinstance(retired, list):
            print("UNREADABLE")
            raise SystemExit(0)
        if not isinstance(pre_raw, list) or not isinstance(post_raw, list):
            print("UNREADABLE")
            raise SystemExit(0)
        pre, post = list(pre_raw), list(post_raw)
        # BIND THE LOADER TO THE DATA. Reading only the JSON left the two free
        # to disagree: a loader edit could filter or rename a DORMANT bucket
        # while the JSON still declared it, and with no live mapping in that
        # bucket the two snapshots stay verdict-identical, so the differential
        # runs and passes. Runtime consumers would then no longer recognise a
        # bucket the published protocol declares -- the latent-retirement
        # failure again, reached from the supported unused-bucket introduction
        # (Codex adversarial, section 18 ship gate). The JSON is the contract
        # and the loader is how it reaches the code; if they disagree, neither
        # is authoritative and this is INFRASTRUCTURE.
        _mod = _load("snapshot_protocol")
        if (_mod.SNAPSHOT_SCHEMA != schema
                or list(_mod.PRE_RESOLUTION_BUCKETS) != pre
                or list(_mod.POST_RESOLUTION_BUCKETS) != post
                or list(_mod.ALL_BUCKETS) != pre + post):
            print("UNREADABLE")
            raise SystemExit(0)
    except Exception:
        print("UNREADABLE")
        raise SystemExit(0)
else:
    source = "legacy"
    try:
        _r = _load("ref_resolution")
        schema = _load("corpus_resolution_snapshot").SNAPSHOT_SCHEMA
        # Same raw-type floor on the legacy path: a module is free to declare
        # these as any object, and the contract is a sequence of names.
        pre_raw = _r.PRE_RESOLUTION_BUCKETS
        post_raw = _r.POST_RESOLUTION_BUCKETS
        renamed, retired = {}, []
        if not isinstance(pre_raw, (list, tuple)) \
                or not isinstance(post_raw, (list, tuple)):
            print("UNREADABLE")
            raise SystemExit(0)
        pre, post = list(pre_raw), list(post_raw)
    except Exception:
        print("UNREADABLE")
        raise SystemExit(0)
# ENFORCE THE FULL LOADER CONTRACT, not a weaker subset. proto_of previously
# accepted any non-boolean int, while snapshot_protocol.load requires a
# POSITIVE one -- so a data-only change from 2 to 0 produced a well-formed
# protocol line, passed the executable-identity checks, and exited 0 through
# the schema fast path without ever importing the loader that would have
# rejected the tree. The gate would report PASS for a protocol nothing can
# actually load (Codex adversarial, section 18 round 2).
if isinstance(schema, bool) or not isinstance(schema, int) or schema < 1:
    print("UNREADABLE")
    raise SystemExit(0)
for _half in (pre, post):
    if not isinstance(_half, (list, tuple)) or not _half \
            or not all(isinstance(b, str) and b for b in _half) \
            or len(set(_half)) != len(_half):
        print("UNREADABLE")
        raise SystemExit(0)
if set(pre) & set(post):
    # A name in BOTH halves is not a vocabulary, it is an ambiguity -- and the
    # consumer branches on POST membership specifically.
    print("UNREADABLE")
    raise SystemExit(0)
# THE TWO HALVES ARE FINGERPRINTED SEPARATELY, not as one flattened tuple.
# `ALL_BUCKETS` is `PRE + POST`, so moving the first POST bucket to the end of
# PRE leaves the concatenation byte-identical -- and that move is BEHAVIOURAL:
# `check_stub_behind_stamp` branches on POST membership to decide whether to
# report the EFFECTIVE path or the authored one, so a repaired ref would start
# naming a file nobody can inspect while both resolver snapshots compared
# perfectly clean (Codex adversarial, section 18).
#
# HEX DIGESTS, not the JSON itself. The caller splits this line on `|`, and a
# bucket name is only validated as a non-empty string -- one containing a pipe
# would silently corrupt every field after it. A digest cannot.
def _fp(seq):
    return hashlib.sha256(json.dumps(list(seq)).encode("utf-8")).hexdigest()


# ORDER-SENSITIVE within each half: the tuples are a published contract, so a
# reorder is a protocol change even when the membership is identical.
print("%d|%s|%s|%s|%s|%s|%s" % (
    schema, _fp(pre), _fp(post), source,
    base64.b64encode(json.dumps(pre).encode("utf-8")).decode("ascii"),
    base64.b64encode(json.dumps(post).encode("utf-8")).decode("ascii"),
    base64.b64encode(json.dumps([renamed, retired]).encode("utf-8")).decode("ascii")))
PY
    _prc=$?
    if [ "$_prc" -ne 0 ]; then
        # The probe catches its OWN exceptions and reports UNREADABLE itself,
        # so a nonzero status here is the interpreter failing to run at all --
        # including 124/137, the budget killing a probe that would not finish.
        # This was the ONE phase in the file running unbounded, so a stalled
        # import or a blocked read could burn the whole gate budget and be
        # terminated externally instead of returning a token (Codex
        # adversarial, section 50 round 22).
        rm -f "$_rec"
        printf 'TRANSPORT\n'
        return 0
    fi
    # THE FIRST LINE IS THE RECORD, and anything after it is discarded rather
    # than trusted. `proto_of` writes its line before interpreter shutdown, so
    # an `atexit` handler can only APPEND -- taking the head makes a late write
    # inert instead of authoritative. The framing is still checked rather than
    # assumed: an empty file is not a record.
    if [ ! -s "$_rec" ]; then
        rm -f "$_rec"
        printf 'TRANSPORT\n'
        return 0
    fi
    local _line
    _line="$(timeout --foreground --kill-after=10s "$(probe_budget)" head -n 1 "$_rec" 2>/dev/null)" \
        || { rm -f "$_rec"; printf 'TRANSPORT\n'; return 0; }
    rm -f "$_rec"
    [ -n "$_line" ] || { printf 'TRANSPORT\n'; return 0; }
    printf '%s\n' "$_line"
}
BASE_PROTO="$(proto_of "$BASE_TREE")"
# EXACTLY ONE RECORD PER SIDE. `redirect_stdout` moves PYTHON-level import
# noise aside, but a module writing straight to fd 1 goes around it -- and a
# contaminated capture makes the caller's `read` consume the noise line and die
# with a generic malformed-line message carrying no stable token. Folding the
# multi-line case into UNREADABLE routes it through the classification below,
# which names the side and emits BASE_PROTOCOL_UNREADABLE or
# HEAD_PROTOCOL_UNREADABLE like every other unusable protocol.
case "$BASE_PROTO" in *$'\n'*) BASE_PROTO=UNREADABLE ;; esac
# `?` means a constant could not be READ -- a missing or renamed snapshot tool,
# or a declaration this parser no longer recognises. It must be an
# INFRASTRUCTURE failure, not a value: treated as a value it merely differs
# from the other side, which sent the run down the protocol-migration path and
# exited 0 having built no cache and walked nothing (Codex adversarial, s16).
#
# ---------------------------------------------------------------------------
# BUT `UNREADABLE` IS FOUR DIFFERENT FACTS WEARING ONE NAME (section 50), and
# they do not have the same operator action. `proto_of` runs inside ONE tree
# and cannot tell them apart -- only the caller has the repository, the base
# SHA and the history:
#
#   a. the constants are there and this parser cannot make sense of them
#      -- a real defect in the tree under test, the original meaning;
#   b. the protocol-bearing files were never in this commit's history at all
#      -- the base simply PREDATES the mechanism, and nothing is wrong with
#      any tree;
#   c. the files existed in this commit's history and are gone from the
#      commit -- a deletion or a rename, which IS a defect;
#   d. the commit carries them but the working tree does not -- an
#      incompletely materialized checkout, which is a defect in the CHECKOUT.
#
# Case (b) is what cost the diagnosis on 2026-08-19: todo-graph.yml run
# 32255498885 gated against `5b1ebb7f` from 2026-08-04, 440 commits back, and
# reported "the snapshot tool is missing or renamed". Nothing was missing and
# nothing was renamed; the base predated the extraction by two days
# (`corpus_resolution_snapshot.py` first appears at `cc05f2e90`,
# `ref_resolution.py` at `10cdda4be`, `snapshot_protocol.json` at
# `9ef13a2b3`).
#
# THE DISCRIMINATOR IS HISTORY, NOT THE FILESYSTEM (Codex design review,
# section 50, [high]). "None of the three paths is present" proves absence and
# nothing more: a POST-genesis base that deleted or renamed all three
# satisfies that predicate exactly as an ancient one does, and would be handed
# the ancient-base diagnosis -- which tells an operator to re-point the base
# and skip a range that was mutilated rather than old. So the question asked
# is "did any ancestor-or-self of this commit ever ADD one of these paths?",
# which distinguishes never-existed from existed-and-removed and needs no
# pinned genesis commit to do it.
#
# AND THE BASE SIDE IS ASKED THROUGH GIT, NOT THROUGH THE MATERIALIZED
# WORKTREE. A sparse or partially-populated checkout can hide a path the
# commit really contains, and this classification is what decides whether an
# operator is told their tree is broken.
#
# WHAT THE GATE DOES WITH AN UNADJUDICATABLE RANGE: it REFUSES, at rc 3, and
# says which of the four it is. It does not clamp the base forward to the
# extraction commit (that silently narrows the adjudicated range and would let
# the unexamined prefix become the next green baseline -- precisely the
# laundering section 16 exists to prevent), it does not derive a fresh base
# (section 16 already refuses that fallback for the same reason), and it never
# passes (a range nothing adjudicated must not read as green). Recovery is NOT
# this gate's to offer: `--base` and IDENTITY_GATE_LAST_GATED_SHA cannot
# create the successful workflow record that moves the CI baseline, so
# advertising them as the remedy would be advice that does not work where the
# failure happens (Codex design review, section 50, [high]). In CI the
# situation is already bounded and already has an honest answer -- the
# workflow walks at most 60 first-parent commits and, finding no successful
# run among them, refuses with "fix the oldest failure first"
# (.github/workflows/todo-graph.yml) rather than reaching for an ancient base.
#
# The leading token on each message is STABLE and greppable so the workflow
# can branch on the classification without parsing prose (Codex design
# review, section 50, Q3).
# ---------------------------------------------------------------------------
# A COMPLETE PROTOCOL IS A SET, NOT A FILE, and treating any one path as
# evidence of the whole was a confidently-wrong classification (Codex
# adversarial, section 50). `proto_of` reads EITHER form:
#     data   -- snapshot_protocol.json  (section 18 onward)
#     legacy -- ref_resolution.py AND corpus_resolution_snapshot.py, together
# In this repository's own history `cc05f2e90` introduced the snapshot file
# and `10cdda4be` the resolver FIFTEEN COMMITS LATER, so every base in that
# window carries one legacy half and no json. `proto_of` cannot import the
# missing half and reports UNREADABLE -- and an any-path test would call that
# tree's protocol "present but malformed", which is exactly backwards: nothing
# is malformed, the extraction was still in progress. That window gets its own
# token rather than being forced into one of the neighbouring answers.
#
# THE LOADER IS ONE OF THE PROTOCOL-BEARING PATHS, not a side-check. It was
# added to the data form's file CONTRACT (round 13) but not to the set the
# form CLASSIFICATION walks, so the two could disagree -- and a HEAD whose
# worktree had the JSON but not the loader classified as `complete`, earning
# HEAD_PROTOCOL_UNREADABLE when the honest answer was an incomplete checkout
# (Codex adversarial, section 50 round 16). That is the THIRD time one set
# knowing something another did not produced a wrong token here, so the sets
# are now one set.
PROTOCOL_JSON="scripts/todo-graph/snapshot_protocol.json"
PROTOCOL_DATA_LOADER="scripts/todo-graph/snapshot_protocol.py"
PROTOCOL_LEGACY_RESOLVER="scripts/todo-graph/ref_resolution.py"
PROTOCOL_LEGACY_SNAPSHOT="scripts/todo-graph/corpus_resolution_snapshot.py"
PROTOCOL_PATHS=(
    "$PROTOCOL_JSON"
    "$PROTOCOL_DATA_LOADER"
    "$PROTOCOL_LEGACY_RESOLVER"
    "$PROTOCOL_LEGACY_SNAPSHOT"
)

# A FAILED QUESTION IS NOT A NEGATIVE ANSWER. Both probes below discard git's
# stderr but NEVER its exit status: swallowing a lookup or traversal failure
# into "absent" / "never added" would let an unreadable repository state
# manufacture a confident classification -- a broken object store would read as
# a pristine ancient base (Codex adversarial + test-coverage, section 50).
# `git ls-tree` is the presence primitive precisely because it separates the
# two: rc 0 with empty output means "asked and absent", rc != 0 means "could
# not ask" (`git cat-file -e` returns 128 for BOTH a missing path and a bad
# object, so it cannot).
# MEMBERSHIP IS EXACT AND TYPE-AWARE, never a substring of a joined string
# (Codex adversarial, section 50 round 3). `git ls-tree -r` expands a
# DIRECTORY pathspec to its descendants, so replacing `snapshot_protocol.json`
# with a directory of that name yields `.../snapshot_protocol.json/README`,
# which a substring test reads as the file being present -- the tree then looks
# `complete` and earns the UNREADABLE token instead of the removed or
# incomplete one it deserves. Any future protocol path containing an existing
# one collides the same way. Presence is therefore recorded as a FLAG PER PATH,
# decided from that path's own `ls-tree` entry, and the entry must be a `blob`.
#
# AND THE FLAG ARRAY IS SIZED FROM THE PATH ARRAY, never from a literal count.
# `PROTO_HAS` was fixed at three entries with every writer and reader looping
# over literal 0, 1, 2, so appending a fourth protocol path would have compiled
# and run while that path was invisible to presence, form and missing-path --
# a commit carrying ONLY the new path would classify as `none` and could earn
# BASE_PROTOCOL_REMOVED (Codex adversarial, section 50 round 4). The form
# mapping below knows exactly three names, so a path set it does not cover is
# refused rather than silently mis-mapped.
PROTO_HAS=()         # parallel to PROTOCOL_PATHS: 1 present as a file, else 0
PROTO_PROBE_ERR=""   # non-empty when a probe could not be answered at all

proto_index_of() {  # $1 = path; echoes its index in PROTOCOL_PATHS, or nothing
    local _i
    for _i in "${!PROTOCOL_PATHS[@]}"; do
        [ "${PROTOCOL_PATHS[$_i]}" = "$1" ] && { printf '%s' "$_i"; return 0; }
    done
    return 1
}

# THE SINGLE-PATH TYPE TEST, shared by the presence probe and the form
# contract. Both need "is this exact path a regular file here", and the second
# of them asks about a path that is NOT in PROTOCOL_PATHS, so the test cannot
# live inside the array walk.
#
# NOT `-r`: the exact entry at that path is the question, and its TYPE is half
# the answer. MODE AS WELL AS TYPE, because git stores a SYMLINK as mode
# 120000, type blob -- accepting every blob made a committed dangling symlink
# look like a present protocol file while the worktree probe's `-f` correctly
# rejected it, and the two sides then disagreed (Codex adversarial, section 50
# round 5). The mode and type are the first two space-separated fields whatever
# quoting git applies to the path.
path_is_regular_in_commit() {  # $1 commit, $2 path; 0 regular, 1 not, 2 cannot ask
    local _out _rc _mode _type
    _out="$(git ls-tree "$1" -- "$2" 2>/dev/null)"; _rc=$?
    [ "$_rc" -ne 0 ] && return 2
    [ -n "$_out" ] || return 1
    _mode="${_out%% *}"
    _type="${_out#* }"; _type="${_type%% *}"
    [ "$_type" = "blob" ] || return 1
    case "$_mode" in
        100644|100755) return 0 ;;
    esac
    return 1
}

# EVERY COMPONENT, NOT JUST THE LEAF. `-f` follows symlinks in every path
# component while `-L` tests only the last one, so moving `scripts/todo-graph`
# outside the checkout and symlinking the DIRECTORY back left each protocol
# file `-f` and not `-L` -- and the gate read and executed bytes from outside
# the repository while the contract passed (Codex adversarial, section 50
# round 14). The commit side needs no equivalent: a git tree cannot be
# traversed through a symlink entry, so `ls-tree` on such a path returns
# nothing and the file already reads as absent.
path_is_regular_in_worktree() {  # $1 root, $2 path; 0 regular file, else 1
    local _cur _c
    [ -f "$1/$2" ] || return 1
    [ -L "$1/$2" ] && return 1
    _cur="$1"
    local IFS=/
    for _c in $2; do
        [ -n "$_c" ] || continue
        _cur="$_cur/$_c"
        [ -L "$_cur" ] && return 1
    done
    return 0
}

protocol_present_in_commit() {  # $1 = commit-ish
    local _c="$1" _i
    PROTO_HAS=()
    for _i in "${!PROTOCOL_PATHS[@]}"; do PROTO_HAS[$_i]=0; done
    for _i in "${!PROTOCOL_PATHS[@]}"; do
        path_is_regular_in_commit "$_c" "${PROTOCOL_PATHS[$_i]}"
        case "$?" in
            0) PROTO_HAS[$_i]=1 ;;
            1) ;;
            *) PROTO_PROBE_ERR="git ls-tree failed for $_c"
               PROTO_HAS=()
               for _i in "${!PROTOCOL_PATHS[@]}"; do PROTO_HAS[$_i]=0; done
               return 1 ;;
        esac
    done
    return 0
}

protocol_present_in_worktree() {  # $1 = tree root
    local _i
    PROTO_HAS=()
    for _i in "${!PROTOCOL_PATHS[@]}"; do PROTO_HAS[$_i]=0; done
    for _i in "${!PROTOCOL_PATHS[@]}"; do
        # A REGULAR FILE, exactly as the commit side requires. `-e` accepted a
        # directory, and `-f` alone accepted a symlink -- which the commit side
        # (mode 100644/100755) does not, and a disagreement between the two
        # probes is what produced a wrong token rather than a wrong answer.
        path_is_regular_in_worktree "$1" "${PROTOCOL_PATHS[$_i]}" \
            && PROTO_HAS[$_i]=1
    done
    return 0
}

# `complete` = the subject can express a protocol in at least one of the two
# forms; `none` = it carries no protocol path at all; `partial` = it carries
# some but cannot express either form.
protocol_form() {  # reads PROTO_HAS
    local _i _json _load _res _snap
    _json="$(proto_index_of "$PROTOCOL_JSON")" || { printf 'UNMAPPED\n'; return 0; }
    _load="$(proto_index_of "$PROTOCOL_DATA_LOADER")" || { printf 'UNMAPPED\n'; return 0; }
    _res="$(proto_index_of "$PROTOCOL_LEGACY_RESOLVER")" || { printf 'UNMAPPED\n'; return 0; }
    _snap="$(proto_index_of "$PROTOCOL_LEGACY_SNAPSHOT")" || { printf 'UNMAPPED\n'; return 0; }
    [ "${#PROTOCOL_PATHS[@]}" -eq 4 ] || { printf 'UNMAPPED\n'; return 0; }
    # THE SELECTION RULE MIRRORS THE READER, it does not merely inventory what
    # is available. `proto_of` takes the data form whenever the JSON exists and
    # falls back to legacy ONLY when it does not -- so with the JSON present
    # and the loader missing, the subject's form is a BROKEN DATA form, not a
    # complete legacy one, however intact the legacy pair happens to be.
    # Reporting "complete" there described a form the reader would not have
    # chosen, and turned an incomplete checkout into a claim that the protocol
    # was malformed (Codex adversarial, section 50 round 16).
    if [ "${PROTO_HAS[$_json]}" = 1 ]; then
        [ "${PROTO_HAS[$_load]}" = 1 ] && { printf 'complete\n'; return 0; }
        printf 'partial\n'; return 0
    fi
    [ "${PROTO_HAS[$_res]}" = 1 ] && [ "${PROTO_HAS[$_snap]}" = 1 ] \
        && { printf 'complete\n'; return 0; }
    for _i in "${!PROTOCOL_PATHS[@]}"; do
        [ "${PROTO_HAS[$_i]}" = 1 ] && { printf 'partial\n'; return 0; }
    done
    printf 'none\n'
}

# HISTORY IS THE DISCRIMINATOR the filesystem cannot supply: "none of these
# paths is present" proves absence and nothing more, and a post-genesis commit
# that deleted or renamed them satisfies it identically (Codex design review,
# section 50). The question asked is "did any ancestor-or-self of this commit
# ever ADD one of these paths?", which needs no pinned genesis commit.
#
# --full-history IS REQUIRED, not decoration. Default path-limited traversal
# applies history simplification and can prune a TREESAME merge parent that
# carries the add, which would answer "never added" about a commit whose
# history plainly did (Codex adversarial, section 50).
protocol_ever_added() {  # $1 = commit-ish, rest = paths; 0 yes, 1 no, 2 cannot ask
    local _c="$1"; shift
    local _out _rc
    # NO PATHS MEANS NO QUESTION. Left unguarded this degrades into an
    # unfiltered traversal whose first commit reads as a match, which is the
    # subshell bug above wearing its consequence.
    [ "$#" -gt 0 ] || return 2
    _out="$(git log --full-history --diff-filter=A --format=%H -1 "$_c" -- "$@" 2>/dev/null)"
    _rc=$?
    [ "$_rc" -ne 0 ] && return 2
    [ -n "$_out" ] && return 0
    return 1
}

# AND A TRUNCATED HISTORY CANNOT PROVE A NEGATIVE. A shallow clone, a graft or
# a replace ref all make "no add anywhere in the ancestry" a statement about
# what this checkout can SEE rather than about what happened, so the ancient-
# base inference is refused for them outright instead of being made unsoundly.
#
# EVERY PROBE FAILS CLOSED, and the metadata paths are resolved by git rather
# than assembled by hand. Two ways this was unsound (Codex adversarial, section
# 50 round 2): ignoring the exit status of `--is-shallow-repository` or
# `git replace -l` let a FAILED probe read as "not shallow / no replacements",
# which is the failed-question-as-negative-answer fault this change exists to
# remove; and `--git-dir` in a LINKED worktree names the per-worktree
# administrative directory while `info/grafts` and `shallow` live in the COMMON
# directory, so a real graft was invisible from exactly the kind of checkout
# this gate creates for itself. `--git-path` resolves both correctly from
# either.
# TRI-STATE HERE TOO: 0 readable, 1 provably truncated, 2 could not ask. Both
# outcomes returned 1, so a FAILED probe was reported as a shallow or grafted
# ancestry -- the same failed-question-as-negative-answer collapse this file
# fixes elsewhere, pointing an operator at a clone that is not the problem
# (Codex consistency, section 50 post-commit review).
history_is_complete() {  # $1 = subject; 0 readable, 1 truncated, 2 cannot ask
    local _shallow _replaced _p _rc
    _shallow="$(git rev-parse --is-shallow-repository 2>/dev/null)"; _rc=$?
    [ "$_rc" -ne 0 ] && return 2
    [ "$_shallow" = "true" ] && return 1
    _p="$(git rev-parse --git-path shallow 2>/dev/null)" || return 2
    [ -s "$_p" ] && return 1
    # THE EFFECTIVE GRAFT SOURCE, not merely git's default one. `GIT_GRAFT_FILE`
    # overrides `info/grafts` and is inherited by every git call this function
    # clears the way for, so checking only the default let a custom graft hide
    # the commit that introduced a protocol path while the default file sat
    # empty -- and the negative traversal that followed then read as proof
    # (Codex adversarial, section 50 round 4). This repo already knows git
    # honours it: `cache_schema.py` points it at the null device to get a
    # graft-free walk.
    if [ -n "${GIT_GRAFT_FILE:-}" ]; then
        [ -s "$GIT_GRAFT_FILE" ] && return 1
    else
        _p="$(git rev-parse --git-path info/grafts 2>/dev/null)" || return 2
        [ -s "$_p" ] && return 1
    fi
    # REPLACEMENT REFS ARE SCOPED TO THE ANCESTRY, not counted globally. Any
    # `git replace` entry used to condemn the whole repository, so a
    # replacement for a commit sitting on an unrelated orphan branch -- which
    # `git log "$1"` can neither traverse nor consult -- produced
    # BASE_HISTORY_INCOMPLETE for a base whose history was entirely intact,
    # blocking a range on metadata that cannot touch it (Codex adversarial,
    # section 50 round 6). The ancestry test runs under
    # `--no-replace-objects` so it reports the TRUE topology rather than the
    # one the replacement asserts.
    #
    # THE SHALLOW AND GRAFT CHECKS ABOVE STAY REPOSITORY-WIDE, deliberately.
    # Scoping them is circular: both rewrite what a traversal can see, so any
    # ancestry query used to decide whether they apply would already be running
    # under them. A replacement ref has no such problem because git offers a
    # switch that turns it off for one query. Repository-wide there is a false
    # REFUSAL, never a false classification -- it fails closed, and it names
    # exactly why.
    # REPLACEMENTS ARE NOT ADJUDICATED HERE. They are decided ONCE, before any
    # form classification, by `base_reads_are_substituted` -- see the base case
    # below for why that had to move out of this function.
    return 0
}

# Does any ACTIVE replacement change what a read of $1's tree or history
# returns? ONE question, asked ONCE, before the base is classified at all.
#
# It lived inside history_is_complete and was therefore only asked by the arms
# that make a never-added inference -- so the `complete` arm, which dies
# immediately, never asked it. Replacing a base's reachable
# snapshot_protocol.json blob with a malformed one of the same type then made
# `proto_of` fail while `ls-tree` still reported a perfectly good regular blob,
# and the gate called a VALID stored base malformed (Codex adversarial, section
# 50 round 10). The same shape had already been paid for once at the commit
# level in round 7; hoisting the question is what stops it recurring per arm.
#
# REACHABILITY IS THE RIGHT TEST for every object type at once: a commit, a
# tree or a blob that the base's own object walk reaches is an object some read
# below will resolve THROUGH the replacement. ONE walk answers for all of them,
# and when there are no replacements at all it costs nothing.
# A LISTED REPLACEMENT IS NOT NECESSARILY AN ACTIVE ONE. `GIT_NO_REPLACE_OBJECTS`
# and `core.useReplaceRefs=false` turn replacement reads OFF, so every read the
# gate makes resolves stored objects while `git replace -l` still enumerates the
# refs -- and a range whose base was never substituted was refused as
# manufactured (Codex adversarial, section 50 round 11).
#
# THE AMBIGUOUS CASE FAILS CLOSED RATHER THAN GUESSING WHICH WAY GIT READ IT.
# Only a plainly-true value counts as "disabled"; anything else is undecided,
# because being wrong here in the other direction would mean trusting a
# substituted tree, and an over-refusal is the cheaper error.
replacements_are_in_effect() {  # 0 in effect, 1 disabled, 2 cannot tell
    local _v _rc
    # PRESENCE, NOT TRUTHINESS. git disables replacement reads when this
    # variable is SET, whatever its value -- confirmed directly: with
    # `GIT_NO_REPLACE_OBJECTS=0`, `git cat-file -p` on a replaced object prints
    # the ORIGINAL. Recognising only plainly-true values and calling the rest
    # undecidable refused ranges git was definitively not substituting (Codex
    # adversarial, section 50 round 14).
    [ "${GIT_NO_REPLACE_OBJECTS+set}" = set ] && return 1
    _v="$(git config --bool --get core.useReplaceRefs 2>/dev/null)"; _rc=$?
    case "$_rc" in
        0) [ "$_v" = "false" ] && return 1
           return 0 ;;
        1) return 0 ;;   # unset: git's default is to use them
        *) return 2 ;;
    esac
}

base_reads_are_substituted() {  # $1 = commit; 0 yes, 1 no, 2 cannot tell
    local _replaced _rc _list _r
    replacements_are_in_effect
    case "$?" in
        1) return 1 ;;   # git is not reading them: nothing is substituted
        2) return 2 ;;
    esac
    _replaced="$(git replace -l 2>/dev/null)"; _rc=$?
    [ "$_rc" -ne 0 ] && return 2
    [ -n "$_replaced" ] || return 1
    _list="$TMP_DIR/replace-reach.$$"
    # No `setsid`, unlike the python phases below: those spawn children a bare
    # `timeout` could not reap, and `git rev-list` does not.
    timeout --kill-after=10s "$(remaining)" \
        git --no-replace-objects rev-list --objects "$1" > "$_list" 2>/dev/null
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        rm -f "$_list"
        return 2
    fi
    while IFS= read -r _r; do
        [ -n "$_r" ] || continue
        # A FLAG, NOT `exit 0` INSIDE THE RULE. awk runs END even after a
        # rule's `exit`, so an `END { exit 1 }` overrides the success and every
        # object reads as unreachable -- which silently disables this guard
        # entirely. That shipped once and its paired reachable-object control
        # is what caught it.
        if awk -v id="$_r" '$1 == id { found = 1; exit } END { exit found ? 0 : 1 }' \
               "$_list"; then
            rm -f "$_list"
            return 0
        fi
    done <<< "$_replaced"
    rm -f "$_list"
    return 1
}

# Fills PROTO_MISSING (an ARRAY, so no caller has to word-split an unquoted
# expansion to pass the paths on) and PROTO_MISSING_TEXT for the diagnostic.
#
# IT MUST BE CALLED DIRECTLY, NEVER THROUGH `$(...)`. A command substitution
# runs in a SUBSHELL, so the array it fills dies with that subshell and the
# caller reads an EMPTY one -- which turned `git log ... -- "${PROTO_MISSING[@]}"`
# into an unfiltered traversal that matched the first commit it saw and
# answered "history DID add these" about paths whose history added nothing.
# Caught live on `cc05f2e90`, the exact partial-introduction base this branch
# exists to classify.
PROTO_MISSING=()
PROTO_MISSING_TEXT=""
# A DIRECT `refs/replace/<sha>` LOOKUP USED TO LIVE HERE, and it is gone
# deliberately: `base_reads_are_substituted` subsumes it, because the base
# commit is reachable from its own object walk. That also keeps the round-8
# lesson without re-implementing it -- a hardcoded `refs/replace/` prefix
# missed a relocated `GIT_REPLACE_REF_BASE` namespace, while `git replace -l`
# enumerates whatever namespace git is actually honouring.

missing_protocol_paths() {  # reads PROTO_HAS
    local _i
    PROTO_MISSING=()
    PROTO_MISSING_TEXT=""
    for _i in "${!PROTOCOL_PATHS[@]}"; do
        [ "${PROTO_HAS[$_i]}" = 1 ] && continue
        PROTO_MISSING+=("${PROTOCOL_PATHS[$_i]}")
        PROTO_MISSING_TEXT="$PROTO_MISSING_TEXT ${PROTOCOL_PATHS[$_i]}"
    done
}

# THE BASE IS ADJUDICATED FIRST, and that order is part of the contract: when
# both endpoints are unreadable the base answer is the one that decides whether
# the RANGE can be adjudicated at all, so it is the more useful thing to say.
case "$BASE_PROTO" in
    TRANSPORT)
        die_infra "BASE_TRANSPORT_FAILED: the gate could not carry the base $BASE_SHA protocol record between its own processes (no temp file, no interpreter, an empty write, or an unreadable record). That is this gate's machinery failing, not a statement about the base, and no classification is being guessed from it" ;;
esac
case "$BASE_PROTO" in
    UNREADABLE|*"?"*)
        # BEFORE ANY TREE CLASSIFICATION, AND FOR EVERY ARM. Each probe below
        # reads the base's tree or its history, and a replacement of ANY object
        # those reads resolve -- the commit, one of its trees, one of its blobs
        # -- substitutes something the base does not store. Asked once here
        # rather than per arm, because asking it per arm is how the `complete`
        # arm came to skip it entirely.
        base_reads_are_substituted "$BASE_SHA"
        case "$?" in
            0) die_infra "BASE_HISTORY_INCOMPLETE: an active refs/replace entry substitutes an object reachable from the base $BASE_SHA, so the tree and history this gate would read are manufactured by repository metadata rather than stored at that commit, and no classification of them would describe the base. Remove the replacement or re-run against a clean clone" ;;
            2) die_infra "BASE_HISTORY_INCOMPLETE: could not determine whether repository replacement metadata substitutes anything reachable from the base $BASE_SHA, so whether its tree and history are the stored ones is unknown and no classification is being guessed from it" ;;
        esac
        if ! protocol_present_in_commit "$BASE_SHA"; then
            die_infra "BASE_TREE_UNREADABLE: could not ask what the base $BASE_SHA contains ($PROTO_PROBE_ERR) -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it"
        fi
        BASE_FORM="$(protocol_form)"
        [ "$BASE_FORM" = "UNMAPPED" ] && die_infra "BASE_FORM_UNMAPPED: PROTOCOL_PATHS declares a set this gate's form mapping does not cover (${PROTOCOL_PATHS[*]}), so no classification can be made from it. Extend protocol_form alongside PROTOCOL_PATHS"
        missing_protocol_paths
        BASE_MISSING="$PROTO_MISSING_TEXT"
        case "$BASE_FORM" in
            complete)
                die_infra "BASE_PROTOCOL_UNREADABLE: cannot read the snapshot protocol constants at the base $BASE_SHA (got '$BASE_PROTO') -- the protocol files are present in that commit but declare SNAPSHOT_SCHEMA/ALL_BUCKETS in a shape this gate cannot parse" ;;
            partial)
                # THE SAME BAR AS THE `none` ARM BELOW. This arm reaches the
                # SAME never-added inference over the missing half, so a
                # truncated ancestry invalidates it identically -- and it was
                # asymmetric: a shallow boundary whose base happened to retain
                # one protocol path was told "the extraction was still in
                # progress. Nothing is wrong with the tree under test", from a
                # horizon rather than from history (Codex adversarial, section
                # 50 round 2).
                history_is_complete "$BASE_SHA"
                case "$?" in
                    0) ;;
                    2) die_infra "BASE_HISTORY_UNREADABLE: could not determine whether the base $BASE_SHA has a complete ancestry -- a git probe failed, so this says nothing about the clone and no classification is being guessed from it" ;;
                    *) die_infra "BASE_HISTORY_INCOMPLETE: the base $BASE_SHA carries only part of the snapshot protocol, but this checkout is shallow, grafted or replaced, so whether$PROTO_MISSING_TEXT was ever added would be a statement about what is visible here rather than about what happened. Re-run against a complete clone" ;;
                esac
                protocol_ever_added "$BASE_SHA" "${PROTO_MISSING[@]}"
                case "$?" in
                    2) die_infra "BASE_HISTORY_UNREADABLE: could not traverse the history of the base $BASE_SHA, so whether it ever carried$BASE_MISSING is unknown and no classification is being guessed from it" ;;
                    0) die_infra "BASE_PROTOCOL_INCOMPLETE: the base $BASE_SHA carries part of the snapshot protocol but cannot express either form of it -- missing$BASE_MISSING, which its own history DID add, so this base is mutilated rather than merely early" ;;
                    *) die_infra "BASE_PROTOCOL_INCOMPLETE: the base $BASE_SHA predates the COMPLETION of the snapshot protocol -- missing$BASE_MISSING, never added anywhere in its history, so the extraction was still in progress at that commit and this range cannot be adjudicated. Nothing is wrong with the tree under test" ;;
                esac ;;
            *)
                history_is_complete "$BASE_SHA"
                case "$?" in
                    0) ;;
                    2) die_infra "BASE_HISTORY_UNREADABLE: could not determine whether the base $BASE_SHA has a complete ancestry -- a git probe failed, so this says nothing about the clone and no classification is being guessed from it" ;;
                    *) die_infra "BASE_HISTORY_INCOMPLETE: the base $BASE_SHA carries none of ${PROTOCOL_PATHS[*]}, but this checkout is shallow, grafted or replaced, so 'never added anywhere in its history' would be a statement about what is visible here rather than about what happened. Re-run against a complete clone" ;;
                esac
                protocol_ever_added "$BASE_SHA" "${PROTOCOL_PATHS[@]}"
                case "$?" in
                    2) die_infra "BASE_HISTORY_UNREADABLE: could not traverse the history of the base $BASE_SHA, so whether it ever carried the snapshot protocol is unknown and no classification is being guessed from it" ;;
                    0) die_infra "BASE_PROTOCOL_REMOVED: the base $BASE_SHA carries none of ${PROTOCOL_PATHS[*]}, but its own history ADDED at least one of them -- they were deleted or renamed rather than never written, so this base is mutilated and its range must not be skipped as merely old" ;;
                    *) die_infra "BASE_PREDATES_PROTOCOL: the base $BASE_SHA predates the snapshot protocol -- none of ${PROTOCOL_PATHS[*]} was ever added anywhere in its history, so there is no base-side resolver to differential and this range cannot be adjudicated. Nothing is wrong with the tree under test. Do not re-point the base to skip it: adjudicate the range or fix the older failure that left the last green this far back" ;;
                esac ;;
        esac ;;
esac
# TRI-STATE, because "git could not answer" and "that is not a regular file"
# are different facts with different operator actions -- and collapsing them
# through `|| return 1` made a failed `ls-tree` on the loader emit
# BASE_PROTOCOL_NOT_A_FILE, telling an operator to fix a symlink that does not
# exist (Codex adversarial, section 50 round 15). Every other probe in this
# file already separates the two; this one had quietly stopped.
protocol_source_is_regular() {  # $1 = source, $2 = side; 0 ok, 1 not a file, 2 cannot ask
    local _i
    case "$1" in
        data)
            # BOTH files, read from the SAME flags the form classification
            # used -- re-probing here is what let the two disagree.
            _i="$(proto_index_of "$PROTOCOL_JSON")" || return 1
            [ "${PROTO_HAS[$_i]}" = 1 ] || return 1
            _i="$(proto_index_of "$PROTOCOL_DATA_LOADER")" || return 1
            [ "${PROTO_HAS[$_i]}" = 1 ] || return 1 ;;
        legacy)
            _i="$(proto_index_of "$PROTOCOL_LEGACY_RESOLVER")" || return 1
            [ "${PROTO_HAS[$_i]}" = 1 ] || return 1
            _i="$(proto_index_of "$PROTOCOL_LEGACY_SNAPSHOT")" || return 1
            [ "${PROTO_HAS[$_i]}" = 1 ] || return 1 ;;
        *) return 1 ;;   # an unrecognised source word is not a contract
    esac
    return 0
}

# THE BASE IS FINISHED BEFORE THE HEAD IS EVEN ASKED. Moving the head PROBE
# down was only half the rule: the base's success-path contract still ran
# after it, so a base whose protocol parses through a resolvable symlink --
# whose answer is BASE_PROTOCOL_NOT_A_FILE -- reported HEAD_TRANSPORT_FAILED
# whenever the head probe stalled or failed (Codex adversarial, section 50
# round 23). Base-first now means every base question, not merely the
# unreadable ones.
IFS='|' read -r BASE_SCHEMA BASE_PRE BASE_POST BASE_SOURCE BASE_PRE_B64 BASE_POST_B64 BASE_MIG_B64 <<< "$BASE_PROTO"
for _f in "$BASE_SCHEMA" "$BASE_PRE" "$BASE_POST" "$BASE_SOURCE" \
          "$BASE_PRE_B64" "$BASE_POST_B64" "$BASE_MIG_B64"; do
    [ -n "$_f" ] || die_infra "BASE_PROTOCOL_UNREADABLE: the base protocol record is malformed (got '$BASE_PROTO') -- it did not carry the seven fields this gate's own probe emits, so nothing in it can be adjudicated"
done
if ! protocol_present_in_commit "$BASE_SHA"; then
    die_infra "BASE_TREE_UNREADABLE: could not ask what the base $BASE_SHA contains ($PROTO_PROBE_ERR) -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it"
fi
protocol_source_is_regular "$BASE_SOURCE" base
case "$?" in
    0) ;;
    1) die_infra "BASE_PROTOCOL_NOT_A_FILE: the base $BASE_SHA parsed its protocol through the '$BASE_SOURCE' form, but that form's files are not all regular files in that commit -- a symlink parses (the reader follows it) while resolving to bytes the commit may not even contain, so the protocol this gate would adjudicate is not the one stored at the base" ;;
    *) die_infra "BASE_TREE_UNREADABLE: could not ask whether the base $BASE_SHA stores the '$BASE_SOURCE' form's files as regular files -- git failed to answer, which is a repository failure and not a statement about the tree, so no classification is being guessed from it" ;;
esac

# ACQUIRED HERE, NOT BESIDE THE BASE PROBE. Running it up front meant a head
# probe that STALLED could stop a perfectly classifiable base from ever
# emitting its token -- base-first ordering has to hold for the acquisition as
# well as the adjudication, or the base's answer waits on the head's machinery
# (Codex adversarial, section 50 round 22).
HEAD_PROTO="$(proto_of "$REPO_ROOT")"
case "$HEAD_PROTO" in *$'\n'*) HEAD_PROTO=UNREADABLE ;; esac

# THE HEAD SIDE IS REACHED ONLY ONCE THE BASE HAS NOTHING TO SAY. Placing this
# transport check beside the base one put a HEAD machinery failure AHEAD of a
# perfectly classifiable BASE failure, contradicting the base-first rule
# published above -- a malformed base plus a failing head read reported
# HEAD_TRANSPORT_FAILED and said nothing about the base at all (Codex
# adversarial, section 50 round 21).
case "$HEAD_PROTO" in
    TRANSPORT)
        die_infra "HEAD_TRANSPORT_FAILED: the gate could not carry the head protocol record between its own processes (no temp file, no interpreter, an empty write, or an unreadable record). That is this gate's machinery failing, not a statement about the tree under test, and no classification is being guessed from it" ;;
esac
case "$HEAD_PROTO" in
    UNREADABLE|*"?"*)
        protocol_present_in_worktree "$REPO_ROOT"
        HEAD_FORM="$(protocol_form)"
        [ "$HEAD_FORM" = "UNMAPPED" ] && die_infra "HEAD_FORM_UNMAPPED: PROTOCOL_PATHS declares a set this gate's form mapping does not cover (${PROTOCOL_PATHS[*]}), so no classification can be made from it. Extend protocol_form alongside PROTOCOL_PATHS"
        missing_protocol_paths
        HEAD_MISSING="$PROTO_MISSING_TEXT"
        HEAD_WT_HAS=("${PROTO_HAS[@]}")
        # ANY PATH THE COMMIT HAS AND THE WORKTREE LACKS IS AN INCOMPLETE
        # CHECKOUT, whatever form the worktree happens to satisfy on its own.
        # Deciding that from the worktree's form alone missed a whole family:
        # remove ONLY snapshot_protocol.json and the surviving legacy pair
        # still reads as a complete form, so the tree was called malformed --
        # when `proto_of` had failed precisely because a file the commit DOES
        # carry was not there to read (Codex adversarial, section 50 round 26).
        # Asked as a path-by-path comparison this is one question rather than a
        # form-by-form case analysis, so a further member cannot slip past it.
        # AND THIS PROBE'S FAILURE IS NOT DISCARDED EITHER. Wrapped in a bare
        # `if`, a git failure here merely skipped the comparison and execution
        # ran on to blame the tree -- the same lost tri-state this file has now
        # fixed three times, reappearing in the comparison added one round
        # earlier (Codex adversarial, section 50 round 27).
        if ! protocol_present_in_commit "$HEAD_RESOLVED"; then
            die_infra "HEAD_TREE_UNREADABLE: could not ask what the head commit $HEAD_RESOLVED contains ($PROTO_PROBE_ERR) -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it"
        fi
        for _i in "${!PROTOCOL_PATHS[@]}"; do
            if [ "${PROTO_HAS[$_i]}" = 1 ] && [ "${HEAD_WT_HAS[$_i]}" != 1 ]; then
                die_infra "HEAD_PROTOCOL_UNMATERIALIZED: the commit $HEAD_RESOLVED carries ${PROTOCOL_PATHS[$_i]} but the working tree does not -- this checkout is incomplete (sparse checkout, or a partial clone), so the gate is reading a tree that is not the commit it claims to test"
            fi
        done
        PROTO_HAS=("${HEAD_WT_HAS[@]}")
        if [ "$HEAD_FORM" = "complete" ]; then
            die_infra "HEAD_PROTOCOL_UNREADABLE: cannot read the snapshot protocol constants at HEAD (got '$HEAD_PROTO') -- the snapshot tool is present but declares SNAPSHOT_SCHEMA/ALL_BUCKETS in a shape this gate cannot parse"
        fi
        # The commit is consulted only to separate a BROKEN TREE from a
        # BROKEN CHECKOUT. They need opposite actions -- fix the tree, or fix
        # the materialization -- and the filesystem alone cannot tell them
        # apart under a sparse or partial checkout.
        # AND A FAILED PROBE IS NOT A NEGATIVE ANSWER HERE EITHER. Folded into
        # an `&&` condition, a git failure merely made the test false and
        # execution fell through to a HEAD_PROTOCOL_* token -- attributing a
        # repository failure to the tree under test, which is the exact
        # invariant this change asserts (Codex adversarial, section 50 round 2).
        if ! protocol_present_in_commit "$HEAD_RESOLVED"; then
            die_infra "HEAD_TREE_UNREADABLE: could not ask what the head commit $HEAD_RESOLVED contains ($PROTO_PROBE_ERR) -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it"
        fi
        HEAD_COMMIT_FORM="$(protocol_form)"
        [ "$HEAD_COMMIT_FORM" = "UNMAPPED" ] && die_infra "HEAD_FORM_UNMAPPED: PROTOCOL_PATHS declares a set this gate's form mapping does not cover (${PROTOCOL_PATHS[*]}), so no classification can be made from it. Extend protocol_form alongside PROTOCOL_PATHS"
        if [ "$HEAD_COMMIT_FORM" = "complete" ]; then
            die_infra "HEAD_PROTOCOL_UNMATERIALIZED: the commit $HEAD_RESOLVED carries a complete snapshot protocol but the working tree does not (missing$HEAD_MISSING) -- this checkout is incomplete (sparse checkout, or a partial clone), so the gate is reading a tree that is not the commit it claims to test"
        fi
        if [ "$HEAD_FORM" = "partial" ]; then
            die_infra "HEAD_PROTOCOL_INCOMPLETE: the tree under test carries part of the snapshot protocol but cannot express either form of it -- missing$HEAD_MISSING, so nothing here can be gated"
        fi
        die_infra "HEAD_PROTOCOL_ABSENT: the tree under test carries none of ${PROTOCOL_PATHS[*]} -- the snapshot mechanism this gate adjudicates is missing or renamed at HEAD, so nothing here can be gated" ;;
esac
IFS='|' read -r HEAD_SCHEMA HEAD_PRE HEAD_POST HEAD_SOURCE HEAD_PRE_B64 HEAD_POST_B64 HEAD_MIG_B64 <<< "$HEAD_PROTO"
# A MALFORMED RECORD IS STILL A PROTOCOL THIS GATE CANNOT READ, so it answers
# in the published vocabulary rather than through a generic message carrying no
# token at all. The header promises a machine-branchable refusal for every
# protocol read; this path was the one exception (Codex consistency, section 50
# post-commit review).
for _f in "$HEAD_SCHEMA" "$HEAD_PRE" "$HEAD_POST" "$HEAD_SOURCE" \
          "$HEAD_PRE_B64" "$HEAD_POST_B64" "$HEAD_MIG_B64"; do
    [ -n "$_f" ] || die_infra "HEAD_PROTOCOL_UNREADABLE: the head protocol record is malformed (got '$HEAD_PROTO') -- it did not carry the seven fields this gate's own probe emits, so nothing in it can be adjudicated"
done

# ---------------------------------------------------------------------------
# THE REGULAR-FILE CONTRACT APPLIES TO A PROTOCOL THAT PARSED, NOT ONLY TO ONE
# THAT DID NOT (Codex adversarial, section 50 round 12, [high]).
#
# The type checks above live inside the UNREADABLE branches, so they only ever
# ran once `proto_of` had already failed. `proto_of` reads with
# `pathlib.is_file()` and `read_text()`, both of which FOLLOW SYMLINKS
# (confirmed directly), so a protocol path that is a symlink to valid JSON
# parses perfectly, never enters those branches, and the gate goes on to
# adjudicate a range through a path this file explicitly declares is not a
# protocol file. A symlink to a target OUTSIDE the commit is worse still: the
# verdict then depends on bytes the commit does not contain, which is exactly
# the property every other check here exists to deny.
#
# So the form `proto_of` ACTUALLY USED is validated against regular files on
# both sides: `data` needs the JSON, `legacy` needs BOTH python files. The base
# is checked in the COMMIT (an ls-tree mode), the head in the working tree,
# each matching what that side's reader actually opened.
# THE DATA FORM IS TWO FILES, NOT ONE. `proto_of`'s data branch validates the
# JSON against the LOADER by importing `snapshot_protocol.py` -- so that file is
# executed on this path and belongs to the form's file set. Checking only the
# JSON left the loader free to be a symlink to a byte-identical copy: it runs,
# the JSON is regular, and the contract passes while code executes through a
# path this gate declares is not a protocol file (Codex adversarial, section 50
# round 13, [high]). The loader is declared with the other protocol paths above
# and walked by the same probes, so the form contract and the form
# classification cannot disagree about it.
#
protocol_present_in_worktree "$REPO_ROOT"
protocol_source_is_regular "$HEAD_SOURCE" head \
    || die_infra "HEAD_PROTOCOL_NOT_A_FILE: the tree under test parsed its protocol through the '$HEAD_SOURCE' form, but that form's files are not all regular files in the working tree -- a symlink parses (the reader follows it) while resolving to bytes outside the commit, so the protocol this gate would adjudicate is not the one under test"

# ---------------------------------------------------------------------------
# THE BYTE-IDENTICAL CLOSURE EXIT (measured far above, decided here).
# ---------------------------------------------------------------------------
# Both sides have now cleared every protocol question this gate knows how to
# ask -- readable, complete, materialized, and reached through regular files:
# in the commit on the base side, in the working tree on the head side. Only
# now is "no verdict-affecting file changed" a statement worth exiting 0 on.
# A range that fails any of those questions never arrives here; it has already
# died carrying the stable token that names WHICH question it failed, which is
# what section 50 published that vocabulary for.
# AND THE MEASUREMENT IS RE-TAKEN HERE, because moving the decision widened the
# window between hashing the closure and acting on it from nothing to a base
# worktree plus two protocol probes. The head side hashes the WORKING TREE on
# purpose -- that is the code about to execute -- so in a shared local checkout
# a resolver file saved inside that window would be measured in its old form,
# take this exit, and make an UNEXAMINED change the next baseline (Codex
# adversarial, section 51, [high]).
#
# A DETECTED MOVEMENT IS rc 3, NOT A FALL-THROUGH. The first fix walked the
# differential instead, which reads like the conservative choice and is not:
# the protocol constants that vocabulary depends on were read BEFORE the
# movement, so the walk would run on mixed-time evidence and could return a
# real-looking rc 0 or rc 1 about a tree that never existed. "The thing I was
# measuring moved" is not a verdict about the range; it is this gate failing to
# hold its subject still -- the same class as an unresolvable base, and it gets
# the same answer (Codex adversarial, section 51 round 2, [high]).
#
# AND AN UNANSWERABLE PROBE IS NOT CONFIRMED ABSENCE. The measurement above
# collapses every git failure to the string MISSING, so a member absent at the
# base and merely UNREADABLE at head -- an unmaterialized sparse checkout, a
# directory, a dangling symlink, an object-store failure -- compares equal and
# would certify an unexamined change. That collapse predates this section and
# is left alone above, where it can only force a walk; it must not survive into
# the DECISION, so this pass asks tri-state questions through the same probes
# the protocol contract uses. `path_is_regular_in_worktree` additionally walks
# every path component, so a closure member reached through a symlinked
# ancestor cannot be certified here either.
#
# THE WINDOW IS NARROWED, NOT CLOSED, and saying otherwise would be the third
# wrong claim in this block. A member can still move after its own re-hash;
# only adjudicating an immutable snapshot removes that, which reverses the
# deliberate working-tree choice this gate was built on. That question is
# section 54's, not this section's.
if [ "$CLOSURE_CHANGED" -eq 0 ]; then
    for f in "${CLOSURE[@]}"; do
        _bt="$(git ls-tree "$BASE_SHA" -- "$f" 2>/dev/null)" \
            || die_infra "CLOSURE_UNVERIFIABLE: could not ask the base $BASE_SHA about the closure member $f -- the repository, not the range, is what failed, and an unanswered probe must never read as 'unchanged'"
        if [ -n "$_bt" ]; then
            # THE ENTRY'S TYPE, NOT MERELY ITS PRESENCE. A blob OID says what
            # the entry CONTAINS and nothing about what it IS: git stores a
            # symlink as mode 120000 whose blob is the target PATH TEXT, so a
            # base symlink pointing at `pass` and a head regular file
            # containing `pass` carry the SAME OID -- equal by content, wholly
            # different in what executes, and certified byte-identical (Codex
            # adversarial, section 51 round 3, [high]). `path_is_regular_in_commit`
            # already asks this question for the protocol contract, checking
            # type and mode together, and it rejects a submodule entry
            # (type `commit`) by the same test.
            path_is_regular_in_commit "$BASE_SHA" "$f"
            case "$?" in
                0) ;;
                1) die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA carries the closure member $f as something other than a regular file (a symlink, a directory, or a submodule) -- its blob may match byte-for-byte while naming rather than being the code, so nothing here can be certified unchanged" ;;
                *) die_infra "CLOSURE_UNVERIFIABLE: could not ask the base $BASE_SHA how it stores the closure member $f -- the repository, not the range, is what failed" ;;
            esac
            _b="$(git rev-parse --quiet --verify "$BASE_SHA:$f" 2>/dev/null)" \
                || die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA lists the closure member $f but its object could not be resolved"
            # THE EXECUTABLE BIT IS PART OF THE IDENTITY, and the argument that
            # it is not was REFUTED here rather than merely doubted. The first
            # answer was that every closure member is imported by Python or run
            # as `bash <path>`, so the bit cannot change what executes -- true
            # of the members, and irrelevant, because it was reasoning about
            # the file instead of about its CALLER. `.githooks/pre-push:194`
            # enters the identity-gate block only `[ -x ... identity-gate.sh ]`,
            # so a push that flips this script 100755 -> 100644 alongside a real
            # closure change skips the entire local gate, and CI only catches it
            # after main has already moved (Codex adversarial, section 51 round
            # 4, [high]). Mode therefore joins the OID in the comparison.
            # AND THE OBJECT IS PRESENT, not merely named. A tree entry resolves
            # to an OID from the TREE; in a partial clone the blob behind it can
            # be absent, and comparing OIDs would then certify bytes nothing in
            # this repository can produce.
            git cat-file -e "$_b" 2>/dev/null \
                || die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA names object $_b for the closure member $f but that object is not present in this repository"
        else
            _b=ABSENT
        fi
        _hw=1
        if path_is_regular_in_worktree "$REPO_ROOT" "$f"; then
            _h="$(git hash-object "$REPO_ROOT/$f" 2>/dev/null)" \
                || die_infra "CLOSURE_UNVERIFIABLE: the closure member $f is a regular file in the working tree but could not be hashed"
        elif [ -e "$REPO_ROOT/$f" ] || [ -L "$REPO_ROOT/$f" ]; then
            die_infra "CLOSURE_UNVERIFIABLE: the closure member $f exists in the working tree but is not a regular file reached through regular directories -- the gate cannot certify bytes it may be reading from outside this checkout"
        else
            _h=ABSENT
            _hw=0
        fi
        # THE MODE IS ASKED OF BOTH COMMITS, NEVER OF THE FILESYSTEM. The first
        # cut took the base mode from `ls-tree` and the head mode from
        # `[ -x ]`, which is not the same question: git supports checkouts
        # whose filesystem does not carry the bit at all (`core.fileMode=false`,
        # and mounts that report every file executable), so a PERFECTLY HEALTHY
        # byte-identical range there compares 100644 against 100755 and dies at
        # rc 3 -- wedging every resolver push on that host. It passed here only
        # because this checkout has `core.fileMode=true`, which is precisely why
        # a fixture could not find it (Codex adversarial, section 51 round 5,
        # [high]). A PUSH carries committed modes, so committed modes are what
        # this compares; whether the file is executable ON DISK is the pre-push
        # caller's own integrity question and is filed against that hook.
        #
        # Only when BOTH commits carry the path is a mode claim meaningful. With
        # the presence check above in force, the remaining case is a member
        # absent from BOTH the base and the head commit -- and absent from the
        # working tree too, so there is no mode to compare and nothing to hide.
        _ht="$(git ls-tree "$HEAD_RESOLVED" -- "$f" 2>/dev/null)" \
            || die_infra "CLOSURE_UNVERIFIABLE: could not ask the head commit $HEAD_RESOLVED how it stores the closure member $f -- the repository, not the range, is what failed"
        # PRESENCE MUST AGREE BETWEEN THE HEAD COMMIT AND THE WORKING TREE, and
        # that is a SEPARATE question from content. Content is read from the
        # worktree on purpose, so an unexamined edit cannot early-exit -- but
        # applying that to PRESENCE opened a hole the mode work then hid behind
        # its both-commits condition: a commit that DELETES a closure member,
        # with a dirty worktree recreating it from the base bytes, produced a
        # head hash equal to the base's, an empty head tree entry that skipped
        # the mode comparison, and rc 0 over a push that removed a
        # verdict-affecting file (Codex adversarial, section 51 round 6,
        # [high]). The inverse -- committed addition, worktree missing -- is the
        # closure analogue of HEAD_PROTOCOL_UNMATERIALIZED, which section 50
        # already refuses for the protocol paths.
        if [ -n "$_ht" ] && [ "$_hw" -eq 0 ]; then
            die_infra "CLOSURE_UNVERIFIABLE: the head commit $HEAD_RESOLVED carries the closure member $f but the working tree does not -- this checkout is not the commit it claims to test, so nothing here can be certified unchanged"
        fi
        if [ -z "$_ht" ] && [ "$_hw" -eq 1 ]; then
            die_infra "CLOSURE_UNVERIFIABLE: the head commit $HEAD_RESOLVED does not carry the closure member $f but the working tree does -- the range being pushed removes it, and the bytes this gate can read are not in that commit"
        fi
        if [ -n "$_bt" ] && [ -n "$_ht" ]; then
            path_is_regular_in_commit "$HEAD_RESOLVED" "$f"
            case "$?" in
                0) ;;
                1) die_infra "CLOSURE_UNVERIFIABLE: the head commit $HEAD_RESOLVED carries the closure member $f as something other than a regular file (a symlink, a directory, or a submodule), so its mode cannot be compared with the base's" ;;
                *) die_infra "CLOSURE_UNVERIFIABLE: could not ask the head commit $HEAD_RESOLVED how it stores the closure member $f -- the repository, not the range, is what failed" ;;
            esac
            _b="${_bt%% *}:$_b"
            _h="${_ht%% *}:$_h"
        fi
        [ "$_b" = "$_h" ] \
            || die_infra "CLOSURE_MOVED_UNDER_GATE: the closure member $f differs base-vs-head in content or in mode ($_b vs $_h), or changed between the measurement above and this decision, so the protocol evidence already gathered describes a tree that is no longer here -- re-run the gate against a tree that holds still"
    done
fi
if [ "$CLOSURE_CHANGED" -eq 0 ]; then
    log "resolver closure byte-identical base..head; nothing to differentiate."
    exit 0
fi

# ---------------------------------------------------------------------------
# THE SEPARATION RULE (section 18, closing the section 16 refusals).
#
# Section 16 could only FAIL CLOSED here, and said so: both constants lived
# beside verdict-affecting logic, so "the protocol changed" and "the resolver
# changed" were indistinguishable. Extracting them makes the question decidable
# -- but ONLY through the executable/inert split. The inference is exactly:
# every file that can RUN is byte-identical base..head, and the only thing that
# differs is data, therefore no verdict-affecting change rode along. Nothing
# weaker is safe, because snapshot_protocol.py is imported by the resolver: if
# THAT file were allowed to change here, a schema bump could carry import-time
# behaviour or monkeypatch resolve_symbol while every checked file stayed
# byte-identical, and this gate would skip the only differential capable of
# exposing it (Codex design review, section 18).
# ---------------------------------------------------------------------------
# THE RETIREMENT PROOF, READ FROM A DECLARATION (section 20). Until now the
# only evidence that a bucket is no longer produced was a SOURCE-TEXT search for
# its quoted literal in the emitters, which an indexed or concatenated emission
# defeats -- so every active retirement was refused rather than approved on
# unsound evidence. `ref_resolution.py` now DECLARES the set it can emit
# (`EMITTED_BUCKETS`), `check_bucket_emission.py` pins that declaration's AST
# shape, and the resolver's own outcome constructors refuse at runtime any
# bucket outside it. Absence from the declared set is therefore a real proof of
# non-emission, and the retirement path can open.
#
# A VIOLATED OR UNREADABLE CONTRACT IS INFRASTRUCTURE, never absence. rc 1 means
# the declaration is contradicted and proves nothing; rc 3 means the check could
# not run. Both must refuse -- reading either as "the bucket is not there" is
# the same fail-open inversion that made an unreadable emitter approve a
# retirement (fixture 22af).
    EMITTED_SET="$(python3 "$REPO_ROOT/scripts/lint/check_bucket_emission.py" \
        --emitter "$REPO_ROOT/scripts/todo-graph/ref_resolution.py" \
        --protocol "$REPO_ROOT/scripts/todo-graph/snapshot_protocol.json" \
        --allow-undeclared --emitted-set 2>"$TMP_DIR/bucket-contract.err")"
    EMITTED_RC=$?
    if [ "$EMITTED_RC" -ne 0 ]; then
        die_infra "the bucket-emission contract does not hold at HEAD (rc=$EMITTED_RC), so the set of buckets the resolver can emit is unknown and no retirement can be adjudicated: $(head -3 "$TMP_DIR/bucket-contract.err" 2>/dev/null | tr '\n' ' ')"
    fi
    EMITTED_B64="$(printf '%s' "$EMITTED_SET" | base64 -w0)" \
        || die_infra "could not encode the declared emitted-bucket set"
    RELOC="$(python3 - "$BASE_PRE_B64" "$BASE_POST_B64" "$HEAD_PRE_B64" "$HEAD_POST_B64" "$HEAD_MIG_B64" "$BASE_MIG_B64" \
        "$EMITTED_B64" <<'RELOCPY'
import base64, json, sys
from collections import Counter

# THE DECLARED EMITTED SET, computed by check_bucket_emission.py before this
# block runs. It is only ever passed in on a rc-0 contract check, so reaching
# here at all means the declaration was verified against the emitter's AST; a
# violated or unreadable contract died as infrastructure in the shell above and
# never becomes an empty set here. That ordering is the whole safety property:
# an empty set would approve every retirement at once.
EMITTED = set(json.loads(base64.b64decode(sys.argv[7])))


def halves(a, b):
    return (json.loads(base64.b64decode(a)), json.loads(base64.b64decode(b)))


bp, bq = halves(sys.argv[1], sys.argv[2])
hp, hq = halves(sys.argv[3], sys.argv[4])
b = {n: "pre" for n in bp}
b.update({n: "post" for n in bq})
h = {n: "pre" for n in hp}
h.update({n: "post" for n in hq})
renamed, retired = json.loads(base64.b64decode(sys.argv[5]))
base_renamed, base_retired = json.loads(base64.b64decode(sys.argv[6]))
# AN ACTIVE DECLARATION MUST BE NEW ON THIS EDGE. Without this, a declaration
# already present in BASE authorised a removal made in HEAD -- pre-authorising
# a future migration, which is exactly what the earlier "reject stale
# declarations" rule was reaching for. Comparing BASE and HEAD metadata gets
# both properties with no history at all: a declaration retained AFTER its
# migration completed stays inert here, because its source is then absent from
# both endpoint vocabularies and it is skipped before this check (Codex
# adversarial, section 18 round 7 -- which refuted the residual this section
# had accepted, so the residual is closed rather than documented).
# VALIDATE EVERY DECLARATION AGAINST THE REAL DELTA BEFORE APPLYING ANY OF IT.
# A declaration is written by the same commit the gate is judging, so an
# unvalidated one is a self-authored exemption. Applying them with a bare dict
# comprehension was worse than that: with base A=pre, B=post and a declared
# A -> B, both base entries collapsed onto B and the later value overwrote the
# earlier, erasing a genuine same-name move with no trace (Codex adversarial,
# section 18 round 5).
bad = []
active_renames = {}
for src, dst in sorted(renamed.items()):
    if not isinstance(src, str) or not isinstance(dst, str) or not src or not dst:
        bad.append("rename %r -> %r is not a pair of names" % (src, dst))
        continue
    if src in h:
        bad.append("rename source %r still exists at HEAD, so nothing was renamed" % src)
        continue
    if src not in b:
        # COMPLETED MIGRATION HISTORY, not an error. The declaration a valid
        # migration must carry stays in the file afterwards; on the NEXT commit
        # the renamed-from name is absent from both sides, and treating that as
        # a stale declaration rejected every subsequent verdict-safe commit
        # until someone made a metadata-cleanup commit no lifecycle described
        # (Codex adversarial, section 18 round 6). Declarations are EDGE-BOUND:
        # they say something about THIS base..head delta or they say nothing.
        continue
    if base_renamed.get(src) == dst:
        bad.append("rename %r -> %r was already declared in BASE, so it "
                   "pre-authorises this edge's change rather than declaring "
                   "it" % (src, dst))
    elif dst not in h:
        bad.append("rename target %r is not in the HEAD vocabulary" % dst)
    elif dst in b:
        bad.append("rename target %r already existed in BASE, so this collides "
                   "with an existing bucket rather than renaming into a new one" % dst)
    else:
        # A RENAME IS NOT A DATA-ONLY CHANGE, and the gate must not let one
        # take the pure-protocol path. `ref_resolution.py` emits bucket names
        # as hardcoded strings and `check_stub_behind_stamp.py` pre-keys its
        # coverage dict on them, so renaming the vocabulary alone leaves both
        # still producing and expecting the RETIRED name. If the bucket is
        # dormant the gate sees nothing and it ships green; the day a ref lands
        # in it, the emitted verdict names a bucket the published vocabulary
        # does not contain and every snapshot becomes invalid (Codex
        # adversarial, section 18 final round). Refuse, and say what a rename
        # actually requires.
        active_renames[src] = dst
        bad.append("rename %r -> %r cannot be a data-only migration: the "
                   "resolver names the bucket in its declared emitted set "
                   "(ref_resolution.EMITTED_BUCKETS), which is executable "
                   "code, so the rename and the resolver change would land in "
                   "one commit -- and this gate cannot adjudicate that as a "
                   "protocol migration. Retire the old bucket and introduce "
                   "the new one as separate, individually-gated steps"
                   % (src, dst))
for dst, n in sorted(Counter(active_renames.values()).items()):
    if n > 1:
        bad.append("rename target %r is claimed by %d sources" % (dst, n))
for r in retired:
    if not isinstance(r, str) or not r:
        bad.append("retirement %r is not a name" % (r,))
    elif r in h:
        bad.append("retired bucket %r still exists at HEAD" % r)
    elif r not in b:
        # Same edge-bound rule: a retirement that already happened is history.
        continue
    elif r in base_retired:
        bad.append("retirement of %r was already declared in BASE, so it "
                   "pre-authorises this edge's removal rather than declaring "
                   "it" % r)
    else:
        # PROVE THE RESOLVER CAN NO LONGER PRODUCE IT. The risk is a STALE
        # RESOLVER PATH: a branch that still references or computes the removed
        # member. If that branch is DORMANT the two snapshots hold no affected
        # mapping and the gate passes -- then a later TODO-only commit reaches
        # that condition and the resolver produces a name the published
        # vocabulary no longer contains, so every snapshot is invalid for a
        # reachable input. That is precisely the latent-failure argument that
        # rejects renames, so it applies here too (Codex adversarial, section 18
        # ship gate).
        #
        # The CONSUMER is no longer part of this question. Until section 20
        # `check_stub_behind_stamp.py` pre-keyed its coverage dict on hardcoded
        # bucket names, so a retirement could leave it raising KeyError; it now
        # derives every key, count and report line from the CURRENT vocabulary,
        # so it simply follows a removal. Only the resolver needs proving.
        #
        # It is CHECKABLE rather than blanket-refused: if the resolver no longer
        # DECLARES the name in `ref_resolution.EMITTED_BUCKETS`, the
        # executable-only migration has already happened and the data-only
        # retirement is safe. That is the documented two-step sequence, and this
        # is what verifies step one.
        #
        # SECTION 20 REPLACED THE EVIDENCE HERE, not the rule. The proof used to
        # be a SOURCE-TEXT search for the quoted bucket literal in the emitters,
        # which is not proof of non-emission: an executable-only refactor to
        # `PRE_RESOLUTION_BUCKETS[1]`, a concatenation, or a table lookup
        # preserves behaviour, passes the differential, and leaves the literal
        # absent -- so a following data-only retirement passed the search while
        # the resolver could still emit a name the protocol no longer declares.
        # Every active retirement was therefore refused outright, which made
        # this a wedge rather than a gate.
        #
        # The declared set is a real proof because it is backed twice: the AST
        # check pins the declaration's shape so it cannot be computed or
        # rebound, and the resolver's outcome constructors refuse at RUNTIME any
        # bucket outside it -- including the aliased, iterated and reflective
        # shapes no static rule can enumerate. Removing a member is what makes
        # the resolver unable to produce it; removing the name here is then
        # bookkeeping.
        if r in EMITTED:
            bad.append("retirement of %r cannot be a data-only migration: the "
                       "resolver still declares it in EMITTED_BUCKETS, so it "
                       "can still produce a verdict the retired vocabulary "
                       "does not declare -- dormant today, invalid for every "
                       "snapshot the day a ref reaches it. Remove the member "
                       "from ref_resolution.EMITTED_BUCKETS first, as an "
                       "executable change put through this gate's resolver "
                       "differential, and retire the name in a following "
                       "data-only commit" % r)
if bad:
    print(json.dumps({"moved": [], "undeclared_removals": [],
                      "bad_declarations": bad}))
    raise SystemExit(0)
# Now the mapping is provably injective and delta-consistent, so applying it
# cannot collapse two entries onto one.
b = {active_renames.get(n, n): half for n, half in b.items()}
moved = sorted(n for n in b if n in h and b[n] != h[n])
# AN UNDECLARED REMOVAL IS AMBIGUOUS WITH A RENAME and nothing downstream can
# resolve it: an UNUSED bucket renamed across the boundary changes no mapping,
# so it would ship green and then apply post-resolution semantics the day a ref
# lands in it. Declaring it is the clearing path -- and it is a path, which is
# what the rejected Cartesian rule lacked.
undeclared = sorted(n for n in b if n not in h and n not in retired)
# NO CARTESIAN RENAME INFERENCE. Pairing every removed name with every added
# one failed a legitimate atomic migration (retire an unused post bucket, add
# an unrelated pre bucket) as a relocation, with no way to clear it -- and it
# was unnecessary: this branch is only reachable with the resolver
# byte-identical, so it keeps EMITTING the retired name and the head snapshot
# fails its own vocabulary validation at rc 3. Reproduced 2026-08-06 (Codex
# adversarial, section 18 round 4).
print(json.dumps({"moved": moved, "undeclared_removals": undeclared,
                  "bad_declarations": []}))
RELOCPY
)" || die_infra "could not compare the bucket halves base..head"
    RELOC_N="$(printf '%s' "$RELOC" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d["moved"]) + len(d["undeclared_removals"]) + len(d["bad_declarations"]))')" \
        || die_infra "could not read the bucket-halves comparison"
    if [ "$RELOC_N" != "0" ]; then
        printf '[identity-gate] FAIL: the bucket vocabulary was RELOCATED across the pre/post-resolution boundary: %s\n' "$RELOC" >&2
        printf '[identity-gate] Every bucket STRING is unchanged, so no mapping differs -- but POST membership decides whether a repaired ref reports its effective path or its authored one, so this moves real verdicts. For a MOVE, ground-truth it by hand. For an undeclared REMOVAL, declare it in snapshot_protocol.json as `renamed_buckets` (old -> new) or `retired_buckets`: a removal is otherwise indistinguishable from a rename that also crossed the boundary.\n' >&2
        exit 1
    fi

BUCKETS_CHANGED=0
[ "$BASE_PRE" = "$HEAD_PRE" ] && [ "$BASE_POST" = "$HEAD_POST" ] || BUCKETS_CHANGED=1
if [ "$BUCKETS_CHANGED" -eq 1 ] || [ "$BASE_SCHEMA" != "$HEAD_SCHEMA" ]; then
    if [ "$BASE_SOURCE" != "data" ] || [ "$HEAD_SOURCE" != "data" ]; then
        # A tree predating the extraction holds its protocol in EXECUTABLE
        # code, so the inert-data premise does not hold for it. Refuse rather
        # than extend an inference to a tree it was never true of.
        die_infra "the protocol changed base..head, but one side predates the snapshot_protocol.json extraction (base=$BASE_SOURCE head=$HEAD_SOURCE), so its constants are only reachable by executing code and a protocol-only change cannot be proven inert. Re-run once both sides carry the extracted protocol."
    fi
    EXEC_CHANGED=""
    for f in "${EXEC_CLOSURE[@]}"; do
        b="$(git rev-parse --quiet --verify "$BASE_SHA:$f" 2>/dev/null || echo MISSING)"
        h="$(git hash-object "$REPO_ROOT/$f" 2>/dev/null || echo MISSING)"
        [ "$b" = "$h" ] || EXEC_CHANGED="$EXEC_CHANGED $f"
    done
    if [ -n "$EXEC_CHANGED" ]; then
        die_infra "the protocol changed base..head AND executable closure file(s) changed with it:$EXEC_CHANGED. This gate cannot separate a protocol migration from a resolver change when both land together. Split the commit: land the protocol data change alone (this gate then proves it verdict-neutral), then the resolver change on top."
    fi
    if [ "$BASE_SCHEMA" != "$HEAD_SCHEMA" ] && [ "$BUCKETS_CHANGED" -eq 1 ]; then
        # BOTH CHANGED. The schema fast path below cannot be taken: it exits 0
        # WITHOUT walking, and a bucket rename is a real verdict change that
        # only the differential can surface -- but the differential is
        # impossible across schemas. Neither half of the answer is available,
        # so this fails closed rather than letting the schema branch swallow a
        # bundled vocabulary change (Codex adversarial, section 18).
        die_infra "BOTH the snapshot schema ($BASE_SCHEMA -> $HEAD_SCHEMA) and the bucket vocabulary changed base..head. A cross-schema differential is impossible, and a vocabulary change needs one, so nothing here can adjudicate the pair. Land them as separate commits: the bucket migration first (this gate differentials it), then the schema bump (this gate proves it data-only)."
    fi
    if [ "$BASE_SCHEMA" != "$HEAD_SCHEMA" ]; then
        # A schema change alters the snapshot FILE FORMAT, so the differential
        # is mechanically impossible: the base process writes schema N and the
        # head process refuses it (rc 3, "regenerate with write"). There is
        # nothing left to measure -- and nothing to fear either, because every
        # executable file is byte-identical, which is the whole content of the
        # claim the differential would otherwise have to establish.
        log "PASS -- snapshot schema migrated $BASE_SCHEMA -> $HEAD_SCHEMA as a DATA-ONLY change; every executable closure file is byte-identical base..head, so no resolver change can have ridden along. A cross-schema differential is not possible and is not needed."
        exit 0
    fi
    # RELOCATION IS ADJUDICATED HERE, from the two TREES. A bucket moved across
    # the pre/post boundary changes behaviour (the lint consumer branches on
    # POST membership to choose the effective vs the authored path) while
    # leaving every bucket STRING -- and therefore every mapping -- identical.
    # A rename PAIRED with a move is the same hazard in disguise: the old name
    # reads as a removal and the new one as an addition, so a name-by-name
    # comparison sees no move at all (Codex adversarial, section 18 round 3,
    # reproduced at rc 0). Adjudicating from the TREES rather than from the
    # snapshots is strictly stronger: it holds even when the base tree predates
    # the snapshot half metadata.
    # BUCKETS-ONLY: the differential still RUNS, and must. A rename changes the
    # verdict strings a consumer sees, so it is a real verdict change even
    # under a byte-identical resolver -- it has to surface as CHANGED and be
    # ground-truthed, not be waved through as "just a protocol migration".
    # `compare` reads each snapshot's own declared vocabulary, which is what
    # lets the old names reach the diff instead of dying at validation.
    log "protocol: bucket vocabulary migrated as a DATA-ONLY change; every executable closure file is byte-identical. Running the differential anyway -- a rename alters real verdicts and must be ground-truthed as CHANGED."
fi

# ---------------------------------------------------------------------------
# PRODUCER CONTRACT IDENTITY, base vs head.
#
# `cache_schema` refuses any cache whose corpus binding declares a producer
# identity other than the reader's own, and that check is deliberately NOT a
# profile axis -- no reader can opt out of it. Correct for every ordinary
# consumer, and exactly incompatible with the one-cache design below: the
# shared cache is built by the HEAD producer, so the moment a commit moves the
# identity, the BASE reader refuses it and the whole gate dies rc 3.
#
# OBSERVED, NOT PROJECTED. ebb3faf6e put the field MODES into the digest
# payload; every push after it failed todo-graph.yml with "producer contract
# digest 'sha256:a7cd42b3...' does not match this reader's 'sha256:c068f799...'
# at the same cache_format_version 1" (run 31287999668, 2026-08-09). It cannot
# self-clear either: IDENTITY_GATE_LAST_GATED_SHA only advances on a PASS, so
# the base stays pinned before the change and every later push reproduces it.
#
# SPLITTING THE COMMIT CANNOT FIX IT, which is why this is not the
# "land the data change alone" branch the protocol migration uses. The identity
# is DECLARED IN cache_schema.py, and cache_schema.py is in the resolver
# closure -- so a contract change is bundled with a closure change BY
# CONSTRUCTION. A rule that refused to adjudicate the pair would refuse every
# contract change that can ever be written.
#
# SO EACH SIDE READS ITS OWN PRODUCER'S CACHE, and the producer differential is
# what makes that sound rather than a widened assertion. Its projection is the
# stamped-ref population keyed per occurrence with `kind`/`file`/`symbol` --
# which is EXACTLY and entirely what the walk consumes from the cache:
# `collect()` reads `file_path` plus `stamped_items[*].{section_n,item_idx,refs}`
# and `SectionScope` reads nothing beyond those same refs. Its `[head-corpus]`
# leg compares the two producers over this very corpus, so a PASS says the two
# caches are indistinguishable to the walks -- the same key-set guarantee the
# single file gave, established by measurement instead of by construction. The
# differential is therefore REQUIRED here, not skippable.
# ---------------------------------------------------------------------------
contract_of() {  # $1 = tree root -> "<version>|<digest>" | ABSENT | UNREADABLE
    python3 - "$1" <<'PY' 2>/dev/null || echo UNREADABLE
import importlib.util, pathlib, sys
tg = pathlib.Path(sys.argv[1]) / "scripts/todo-graph"
mod_path = tg / "cache_schema.py"
# ABSENT IS A VALUE, NOT A FAILURE. A tree predating cache_schema, or predating
# the identity fields inside it, has no producer identity to compare and no
# check that can refuse anything -- reporting that as UNREADABLE would wedge
# this gate against every older base, which is the failure it is here to end.
if not mod_path.is_file():
    print("ABSENT")
    raise SystemExit(0)
sys.path.insert(0, str(tg))
spec = importlib.util.spec_from_file_location("cache_schema", mod_path)
mod = importlib.util.module_from_spec(spec)
sys.modules["cache_schema"] = mod
spec.loader.exec_module(mod)
ver = getattr(mod, "CACHE_FORMAT_VERSION", None)
dig = getattr(mod, "PRODUCER_CONTRACT_DIGEST", None)
if ver is None or dig is None:
    print("ABSENT")
    raise SystemExit(0)
# `bool` is an `int`, and a blank digest would compare equal across two trees
# that both failed to compute one -- either would certify a divergence away.
if (isinstance(ver, bool) or not isinstance(ver, int)
        or not isinstance(dig, str) or not dig):
    print("UNREADABLE")
    raise SystemExit(0)
print("%d|%s" % (ver, dig))
PY
}
BASE_CONTRACT="$(contract_of "$BASE_TREE")"
HEAD_CONTRACT="$(contract_of "$REPO_ROOT")"
# An identity this gate cannot READ is infrastructure, never "the same as the
# other side" -- the same rule proto_of applies to the protocol constants, and
# for the same reason: read as a value it merely compares equal and sends the
# run down the single-cache path that cannot work.
for _c in "$BASE_CONTRACT" "$HEAD_CONTRACT"; do
    if [ "$_c" = "UNREADABLE" ]; then
        die_infra "cannot read the producer contract identity from one of the trees (base='$BASE_CONTRACT' head='$HEAD_CONTRACT'); cache_schema is present but its CACHE_FORMAT_VERSION/PRODUCER_CONTRACT_DIGEST could not be evaluated"
    fi
done
CONTRACT_DIVERGED=0
if [ "$BASE_CONTRACT" != "$HEAD_CONTRACT" ]; then
    CONTRACT_DIVERGED=1
    log "producer contract identity CHANGED base..head (base=$BASE_CONTRACT head=$HEAD_CONTRACT): no single cache is readable by both readers, so each walk reads a cache built by its own producer and the producer differential below is REQUIRED."
fi

# ---------------------------------------------------------------------------
# PRODUCER DIFFERENTIAL, BEFORE the resolver differential (section 18).
#
# The resolver differential below builds the cache ONCE and feeds it to both
# walks. That is what makes its keys stable, and it is also a blind spot it
# CANNOT close from the inside: a `build.py` that stops emitting a stamped ref
# removes that ref from both walks equally, so the two sides agree perfectly and
# no DROPPED is produced. It has to be caught where the cache is made.
#
# It runs FIRST because a producer regression invalidates the population the
# resolver differential then reports on -- a PASS over a quietly shrunken corpus
# is exactly the false assurance this gate exists to prevent.
#
# SKIPPED when build.py is byte-identical base..head: there is then no producer
# change to differentiate, and four extra corpus builds prove nothing. This is
# the same reasoning as the whole-closure early exit above, scoped to one file.
# ---------------------------------------------------------------------------
PRODUCER_BASE="$(git rev-parse --quiet --verify "$BASE_SHA:scripts/todo-graph/build.py" 2>/dev/null || echo MISSING)"
PRODUCER_HEAD="$(git hash-object "$REPO_ROOT/scripts/todo-graph/build.py" 2>/dev/null || echo MISSING)"
PRODUCER_DIFF_OK=0
# THE SKIP IS ALSO CONDITIONED ON THE CONTRACT. A byte-identical build.py still
# emits a DIFFERENT artifact identity when cache_schema moved under it (the
# digest is declared there, not here), and the per-side caches that divergence
# forces are only sound because this differential proves their populations
# equal -- so skipping here would leave the two walks comparing unproven inputs.
if [ "$PRODUCER_BASE" = "$PRODUCER_HEAD" ] && [ "$CONTRACT_DIVERGED" -eq 0 ]; then
    log "producer differential: skipped, build.py is byte-identical base..head."
elif [ ! -f "$BASE_TREE/scripts/todo-graph/producer_differential.py" ] \
        && [ ! -f "$REPO_ROOT/scripts/todo-graph/producer_differential.py" ]; then
    die_infra "build.py changed base..head but producer_differential.py is missing from both trees"
else
    if [ "$PRODUCER_BASE" = "$PRODUCER_HEAD" ]; then
        log "producer differential: build.py is byte-identical but the contract identity moved; running it anyway, because the per-side caches depend on its verdict..."
    else
        log "producer differential: build.py changed; comparing both producers over both corpora..."
    fi
    # SAME BOUNDED, GROUP-WIDE LIFECYCLE AS THE WALKS. A plain `timeout`
    # waits forever when the monitored command ignores TERM, and this phase
    # executes the CHANGED head producer -- the code most likely to hang -- so
    # without this the budget was not a bound on the gate at all (Codex
    # adversarial, section 18 round 5).
    setsid timeout --kill-after=10s "$(remaining)" python3 \
        "$REPO_ROOT/scripts/todo-graph/producer_differential.py" \
        "$BASE_TREE" "$REPO_ROOT" --strict >"$TMP_DIR/producer.log" 2>&1 &
    PROD_PID=$!
    WALK_PIDS="$PROD_PID"
    wait "$PROD_PID"; PROD_RC=$?
    WALK_PIDS=""
    sed 's/^/    /' "$TMP_DIR/producer.log"
    case "$PROD_RC" in
        0) PRODUCER_DIFF_OK=1
           log "producer differential PASS -- both producers emit the same stamped-ref population." ;;
        1)
            printf '[identity-gate] FAIL: the producer change altered the stamped-ref population.\n' >&2
            printf '[identity-gate] The resolver differential CANNOT see this: a ref missing from the one shared cache is missing from BOTH of its walks.\n' >&2
            exit 1
            ;;
        124|137) die_infra "the producer differential exceeded the ${BUDGET_SECS}s budget and was killed" ;;
        2)  die_infra "the producer differential rejected this driver's own invocation (rc=2, usage). This is a bug in identity-gate.sh, not in the tree under test." ;;
        3)  die_infra "the producer differential could not run (rc=3, infrastructure: a producer failed or emitted an unusable cache)" ;;
        *)  die_infra "the producer differential exited with an undocumented status (rc=$PROD_RC)" ;;
    esac
fi

# ---------------------------------------------------------------------------
# Build the cache ONCE. Both walks read this exact file, which is what makes
# the key set stable. Absolute paths: the base walk runs with a different CWD,
# and a relative STUB_LINT_CACHE would silently resolve against the wrong tree.
# ---------------------------------------------------------------------------
CACHE_ABS="$TMP_DIR/todo-cache.json"
setsid timeout --kill-after=10s "$(remaining)" \
    python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet --output "$CACHE_ABS" \
    >"$TMP_DIR/build.log" 2>&1 &
CACHE_PID=$!
WALK_PIDS="$CACHE_PID"
wait "$CACHE_PID"; CACHE_RC=$?
WALK_PIDS=""
case "$CACHE_RC" in
    0) ;;
    124|137) die_infra "the cache build exceeded the ${BUDGET_SECS}s budget and was killed" ;;
    *) die_infra "cache build failed (see $TMP_DIR/build.log)" ;;
esac
[ -s "$CACHE_ABS" ] || die_infra "cache build produced an empty file"

# THE BASE SIDE'S CACHE. Identical to $CACHE_ABS in the ordinary case -- one
# file, both walks, key set stable by construction. When the producer contract
# identity moved, the base reader CANNOT read the head-built artifact at all,
# so it gets one built by its own producer over THIS SAME corpus. The corpus is
# the head corpus in both branches ($REPO_ROOT), so the only thing that varies
# is which producer wrote the bytes -- which is exactly what the differential
# above measured.
BASE_CACHE_ABS="$CACHE_ABS"
if [ "$CONTRACT_DIVERGED" -eq 1 ]; then
    if [ "$PRODUCER_DIFF_OK" -ne 1 ]; then
        die_infra "the producer contract identity diverged base..head, so the base walk needs a cache of its own -- but the producer differential did not pass, so nothing establishes that the two caches carry the same stamped-ref population. Refusing to differential two walks over unproven inputs."
    fi
    BASE_CACHE_ABS="$TMP_DIR/todo-cache-base.json"
    # --root/--repo-root EXPLICITLY at $REPO_ROOT: the base build.py lives in
    # the base worktree and would otherwise default to the BASE corpus, which
    # would make the two walks read different TODO text and attribute every
    # corpus edit to the resolver.
    setsid timeout --kill-after=10s "$(remaining)" \
        python3 "$BASE_TREE/scripts/todo-graph/build.py" --quiet \
        --root "$REPO_ROOT/todo" --repo-root "$REPO_ROOT" \
        --output "$BASE_CACHE_ABS" >"$TMP_DIR/build-base.log" 2>&1 &
    BCACHE_PID=$!
    WALK_PIDS="$BCACHE_PID"
    wait "$BCACHE_PID"; BCACHE_RC=$?
    WALK_PIDS=""
    case "$BCACHE_RC" in
        0) ;;
        124|137) die_infra "the base-side cache build exceeded the ${BUDGET_SECS}s budget and was killed" ;;
        *) die_infra "the base-side cache build failed (see $TMP_DIR/build-base.log)" ;;
    esac
    [ -s "$BASE_CACHE_ABS" ] || die_infra "the base-side cache build produced an empty file"
    log "base-side cache built by the BASE producer over the HEAD corpus; the two caches are population-identical per the differential above."
fi

BASELINE="$TMP_DIR/baseline.json"
HEADSHOT="$TMP_DIR/head.json"

# ---------------------------------------------------------------------------
# THE TWO WALKS RUN CONCURRENTLY (section 18).
#
# They were serial because `compare` had no way to read two finished snapshots
# -- it walked the live tree itself, so the head walk could not start until the
# base walk had produced its file. With the two-file `compare` mode the walks
# are genuinely independent: separate processes, separate output files, and one
# IMMUTABLE cache read by both (nothing writes to $CACHE_ABS after it is
# built). Cost drops from roughly double a single walk to roughly one, against
# a corpus that only grows.
#
# AND IT IS BOUNDED. Section 16's perf review flagged the curve, not a present
# failure: ~80-120s at 1,626 refs against a 20-minute job ceiling. A budget
# that fires while headroom REMAINS turns "the corpus outgrew CI" into a
# legible failure instead of a 20-minute timeout that reads like an
# infrastructure flake.
# ---------------------------------------------------------------------------

log "walking with BASE and HEAD resolver code concurrently (budget ${BUDGET_SECS}s)..."
# Monotonic here too: a backward wall-clock step would otherwise print a
# negative duration in the log line below.
WALK_START="$(python3 -c 'import time; print(int(time.monotonic()))' 2>/dev/null || echo 0)"

STUB_LINT_CACHE="$BASE_CACHE_ABS" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    setsid timeout --kill-after=10s "$(remaining)" python3 \
    "$BASE_TREE/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$BASELINE" >"$TMP_DIR/base-walk.log" 2>&1 &
BASE_PID=$!
WALK_PIDS="$BASE_PID"
STUB_LINT_CACHE="$CACHE_ABS" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    setsid timeout --kill-after=10s "$(remaining)" python3 \
    "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$HEADSHOT" >"$TMP_DIR/head-walk.log" 2>&1 &
HEAD_PID=$!
WALK_PIDS="$WALK_PIDS $HEAD_PID"

# REAP BOTH, ALWAYS. `wait`ing on only the first and bailing on its failure
# would leave the other walk running against a $TMP_DIR the EXIT trap is about
# to delete -- an orphan writing into a removed directory, and a exit status
# nobody read. Each `timeout` kills its own child at the budget, and both are
# waited on here before any verdict is formed.
wait "$BASE_PID"; BASE_RC=$?
wait "$HEAD_PID"; HEAD_RC=$?
WALK_PIDS=""
WALK_SECS=$(( $(python3 -c 'import time; print(int(time.monotonic()))' 2>/dev/null || echo "$WALK_START") - WALK_START ))

# 124 is timeout(1)'s "the budget fired". Report it as its own thing: it is not
# a resolver defect and must never be read as one.
for pair in "BASE:$BASE_RC:$TMP_DIR/base-walk.log" "HEAD:$HEAD_RC:$TMP_DIR/head-walk.log"; do
    side="${pair%%:*}"; rest="${pair#*:}"; rc="${rest%%:*}"; logf="${rest#*:}"
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        die_infra "the $side walk exceeded the ${BUDGET_SECS}s budget and was killed. The gate is approaching the CI job ceiling: shard the corpus or raise IDENTITY_GATE_BUDGET_SECS deliberately, but do not discover this as a job timeout."
    fi
    if [ "$rc" -ne 0 ]; then
        sed 's/^/    /' "$logf" >&2 || true
        die_infra "the $side resolver could not complete its walk (rc=$rc)"
    fi
done
sed 's/^/    /' "$TMP_DIR/base-walk.log"
sed 's/^/    /' "$TMP_DIR/head-walk.log"
log "both walks finished in ${WALK_SECS}s of the ${BUDGET_SECS}s budget."

log "comparing the two snapshots (--strict)..."
setsid timeout --kill-after=10s "$(remaining)" \
    python3 "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
    compare "$BASELINE" "$HEADSHOT" --strict \
    --base-halves-b64 "$(python3 -c 'import base64,json,sys; print(base64.b64encode(json.dumps([json.loads(base64.b64decode(sys.argv[1])), json.loads(base64.b64decode(sys.argv[2]))]).encode()).decode())' "$BASE_PRE_B64" "$BASE_POST_B64")" \
    --head-halves-b64 "$(python3 -c 'import base64,json,sys; print(base64.b64encode(json.dumps([json.loads(base64.b64decode(sys.argv[1])), json.loads(base64.b64decode(sys.argv[2]))]).encode()).decode())' "$HEAD_PRE_B64" "$HEAD_POST_B64")" \
    >"$TMP_DIR/compare.log" 2>&1 &
CMP_PID=$!
WALK_PIDS="$CMP_PID"
wait "$CMP_PID"; CMP_RC=$?
WALK_PIDS=""
sed 's/^/    /' "$TMP_DIR/compare.log"

case "$CMP_RC" in
    0)
        log "PASS -- every prior verdict survived the resolver change."
        exit 0
        ;;
    1)
        printf '[identity-gate] FAIL: the resolver change altered a prior verdict, or added an unreviewed mapping.\n' >&2
        printf '[identity-gate] Ground-truth each line above by hand. This gate has deliberately no self-serve override: an approval token the committer can mint is not approval.\n' >&2
        exit 1
        ;;
    2)
        # rc 2 is the snapshot tool's USAGE error, which here can only mean
        # this driver invoked it wrongly. Folding it into the generic
        # infrastructure bucket made a driver bug indistinguishable from a
        # cache/worktree/resolver failure, against the very non-collapsing
        # contract this gate advertises (Codex consistency, section 16). Still
        # fatal -- but say which thing broke.
        die_infra "the snapshot tool rejected this driver's own invocation (rc=2, usage). This is a bug in identity-gate.sh, not in the tree under test."
        ;;
    3)
        die_infra "compare could not complete its walk (rc=3, infrastructure: cache, baseline or resolver input)"
        ;;
    124|137)
        die_infra "the comparison exceeded the ${BUDGET_SECS}s budget and was killed"
        ;;
    *)
        die_infra "compare exited with an undocumented status (rc=$CMP_RC)"
        ;;
esac
