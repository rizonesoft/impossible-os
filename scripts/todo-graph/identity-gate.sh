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
#        BASE_OBJECTS_SUBSTITUTED     replacement metadata makes the `--base`
#                                     argument resolve to a DIFFERENT commit
#                                     than the stored objects do -- an
#                                     annotated tag whose tag object is
#                                     replaced, say. Distinct from
#                                     BASE_HISTORY_INCOMPLETE, which is about
#                                     objects reachable FROM an agreed base;
#                                     this one says the two sides do not agree
#                                     which commit the base IS (section 52).
#                                     ALSO emitted when the question could not
#                                     be ANSWERED -- an uncapturable peel -- and
#                                     the message says which of the two it is:
#                                     an unanswered probe must refuse, and it
#                                     shares the token because the operator
#                                     action (clear the metadata, or use a
#                                     clean clone) is the same
#        HEAD_OBJECTS_SUBSTITUTED     replacement metadata makes the head not
#                                     the head. TWO routes, one token, because
#                                     the operator action is identical and the
#                                     message says which: (a) an entry
#                                     substitutes an object in the head commit
#                                     itself, or (b) it makes the `--head`
#                                     argument PEEL to a different commit than
#                                     the stored objects do -- an annotated tag
#                                     whose tag object is replaced, say, which
#                                     is not in the head commit at all. The
#                                     gate pins replacement reads OFF, so in
#                                     neither case is it reading the substitute
#                                     -- it is refusing to certify a commit
#                                     that git shows the operator differently
#                                     (section 52). A THIRD emission is the
#                                     indeterminate one: either probe may fail
#                                     to answer, and an unanswered question
#                                     refuses rather than passes. The message
#                                     distinguishes all three; the token does
#                                     not, because the operator action is the
#                                     same for each
#                                     Named for object substitution rather
#                                     than for a history claim: unlike the
#                                     base, the head side makes no never-added
#                                     inference, so it has no history to call
#                                     incomplete
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
#        HEAD_PROTOCOL_UNMATERIALIZED the commit has it, the tree the gate
#                                     read does not: an incomplete/sparse
#                                     checkout. Since section 54 that tree is
#                                     the gate's OWN materialization of the
#                                     head commit, so this is defence in depth
#                                     over a checkout it made itself rather
#                                     than a statement about the operator's
#                                     working tree
#        HEAD_PROTOCOL_UNREADABLE     complete at HEAD, but unparseable
#
#      Two belong to the ENDPOINTS (section 52) -- resolving the arguments,
#      probing the fallback chain, and revalidating the replacement state and
#      the endpoint peels. They are a separate family from the closure tokens
#      below even though both are about the gate failing to hold its subject
#      still.
#
#      THEIR PRECEDENCE IS NOT UNIFORM, and two drafts of this paragraph
#      overclaimed it in opposite directions (Codex consistency, section 52
#      rounds 23 and 24). What is actually true:
#        - ACQUISITION failures come first, before anything else is read, and
#          that includes HEAD ones: the head argument is resolved before the
#          base is even established, so `--head <garbage>` reports ahead of
#          every base and closure token. Base-first governs CLASSIFICATION, not
#          the act of working out which commits were named.
#        - REVALIDATION runs base before head within itself, so a base fault
#          wins there.
#        - The two head SUBSTITUTION results are the ones deliberately deferred
#          to the head's own site, which is after the closure MEASUREMENT and
#          after base protocol adjudication -- so those report first. It is
#          NOT after the closure's structural re-check, which runs later still,
#          so a non-regular or moved closure member reports AFTER a substituted
#          head rather than before it. Saying "a closure fault reports first"
#          without that distinction was wrong for exactly the structural half
#          (Codex consistency, section 52 round 25).
#        ENDPOINT_UNRESOLVABLE        the gate could not ASK what an endpoint
#                                     argument resolves to: the probe failed,
#                                     or the shared budget expired while it
#                                     ran. Machinery, not a statement about
#                                     the argument -- distinct from "'X' is
#                                     not a commit", which is an answer
#                                     (section 52)
#        ENDPOINT_MOVED_UNDER_GATE    the repository moved under the endpoint
#                                     snapshot: a spec resolved to two
#                                     different commits while being read, a
#                                     spec no longer resolves to what it did,
#                                     the replacement metadata itself changed,
#                                     or `core.useReplaceRefs` was turned on or
#                                     off -- four causes, plus an INDETERMINATE
#                                     fifth below for a probe that could not
#                                     answer at all. All four say the captured
#                                     state no longer describes the repository
#                                     and all take the same repair, so they
#                                     share a token and the message says
#                                     which. The FIFTH emission is the
#                                     indeterminate one: any of those probes
#                                     may fail or run out of budget, and an
#                                     unanswered question refuses rather than
#                                     passes -- the message distinguishes "it
#                                     changed" from "it could not be asked".
#                                     Sibling of the closure token
#                                     below. Published separately from the
#                                     substitution tokens because blaming
#                                     replacement metadata for a branch that
#                                     simply advanced sends the operator to
#                                     the wrong repair (section 52)
#
#      Two more are raised by the closure re-check that guards the exit
#      (section 51). They say nothing about the protocol and nothing about the
#      range; they say this gate could not hold its own subject still long
#      enough to certify it, so the operator action is to re-run, or to repair
#      the checkout, rather than to re-point the base.
#        CLOSURE_MOVED_UNDER_GATE     NO LONGER EMITTED (section 55). It meant
#                                     a closure member changed between the
#                                     measurement and the decision, so the
#                                     protocol evidence describes a tree that
#                                     is no longer here
#        CLOSURE_UNVERIFIABLE         a closure member could not be asked about
#                                     or hashed, or is not a regular file
#                                     reached through regular directories -- an
#                                     unanswered probe is never 'unchanged'
#        GATE_SCRIPT_NOT_EXECUTABLE   the head commit stores identity-gate.sh
#                                     as 100644, which silently disables the
#                                     pre-push hook's gating block. An
#                                     INVARIANT about the head, not a
#                                     base-vs-head diff: restore the bit and
#                                     commit, and it clears
#
#      WHICH TREE THE HEAD SIDE READS (section 54). Every head-side read is
#      taken from $HEAD_RESOLVED: the closure and protocol readings from the
#      commit's own objects, and everything that must EXECUTE from a worktree
#      the gate materializes for that commit. The operator's working tree is
#      never adjudicated, so a verdict always describes the commit it names --
#      which is what the pre-push caller needs, since it is pushing commits.
#        MATERIALIZATION_UNFAITHFUL   the gate made a checkout for one of the
#                                     endpoints and that checkout does not
#                                     represent the commit: an entry git
#                                     skipped (sparse or assume-unchanged),
#                                     something that modified the tree after
#                                     the write, a committed symlink flattened
#                                     into a regular file by a host that cannot
#                                     represent one, or a checkout filter that
#                                     would make the gate execute or parse
#                                     bytes the commit does not store. ALSO
#                                     emitted when any of those questions could
#                                     not be asked, and when the budget expired
#                                     while asking -- an unanswered question
#                                     refuses. This one is about the GATE'S OWN
#                                     tree, not the operator's: it says the
#                                     gate could not obtain the commit, which
#                                     is why its repairs are host and
#                                     repository settings rather than edits
#      Exactly one head-side read cannot be redirected, and it refuses instead:
#        GATE_DRIVER_NOT_AT_HEAD      the running identity-gate.sh is not the
#                                     copy the selected head carries. The hook
#                                     executes the working-tree copy, so the
#                                     certifier itself is the one thing that
#                                     cannot be read from the commit.
#                                     Re-executing the committed copy would
#                                     change how the gate is INVOKED, which is
#                                     section 16's surface; refusing does not
#                                     and fails closed. THREE causes, and they
#                                     do NOT share a repair, which is why the
#                                     message distinguishes them: with the
#                                     default head an uncommitted edit, which
#                                     committing or stashing clears; with an
#                                     explicit `--head`, a clean tree that
#                                     simply belongs to another commit, where
#                                     the repair is to select or check out a
#                                     matching head; and a commit that does not
#                                     carry the driver at all, or a probe that
#                                     could not answer, which is repository
#                                     repair. It cannot wedge: any push whose
#                                     driver matches its own commit passes
#
#      One NOTE is published alongside the refusals. It is NOT an rc 3 token
#      and never changes the verdict; it exists so that "which tree was
#      adjudicated" is never something a reader has to infer:
#        HEAD_WORKTREE_DIVERGES       the working tree differs from the head
#                                     commit in one or more paths, none of
#                                     which were adjudicated. Non-fatal on
#                                     purpose: the verdict is correct about the
#                                     range either way, and refusing here would
#                                     wedge a legitimate push made from a tree
#                                     carrying unrelated uncommitted work
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"  # launch-exempt: runs before the gate's clock exists, and reads a shell variable rather than the filesystem
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
# ONE BOUNDED READER FOR EVERY CLOCK READ IN THIS FILE. Bounding only the
# newest caller left the initial reading and `remaining()` launching an
# interpreter with no limit, so a wedged python could hang the gate BEFORE the
# first bounded probe ever started -- and hard-killing a child cannot help when
# the child is never spawned (Codex adversarial AND perf, section 52 round 17,
# [medium], found independently by both). A FIXED small limit, never the
# budget: this is what measures the budget, so it cannot spend it.
# ASSIGNS, NEVER PRINTS. A caller writing `x="$(budget_left)"` forks a subshell,
# and `budget_left` calling `$(mono_now)` inside it forked a second -- so the
# clock that reads a file with builtins still cost two processes per bounded
# probe, measured at ~2.19ms each and ~22% of the common unchanged path, while
# this file claimed it cost nothing (Codex perf, section 52 round 24, [medium]).
# Both now assign through a global and run in the current shell.
mono_now() {   # sets MONO_NOW to integer monotonic seconds, or "" ; rc 1 if unreadable
    local _up
    MONO_NOW=""
    # NO FORK ON THE PATH THAT RUNS PER QUERY. Every bounded probe asks the
    # clock first, so an interpreter start per ask is a cost the deadline
    # imposes on HEALTHY runs: measured at ~1.7s added to an ordinary
    # unchanged-closure run, about half of it (Codex perf, section 52 round
    # 21, [medium]). `/proc/uptime` is a counter that cannot step backward,
    # which is the only property this deadline needs, and bash reads it with
    # builtins alone -- no process at all.
    if [ -r /proc/uptime ] && read -r _up _ < /proc/uptime; then
        _up="${_up%%.*}"
        case "$_up" in
            ''|*[!0-9]*) ;;          # not a number: fall through to the probe
            *) MONO_NOW="$_up"; return 0 ;;
        esac
    fi
    # THE FALLBACK KEEPS ITS BOUND. Where /proc is absent the clock is a
    # subprocess again, and a subprocess that never starts would stop the very
    # probe it is meant to bound.
    MONO_NOW="$(timeout --foreground -s KILL 5 \
        python3 -c 'import time; print(int(time.monotonic()))' 2>/dev/null)" || MONO_NOW=""  # fixed-allowance: this IS the clock probe, so it cannot be bounded by the budget it is measuring
    [ -n "$MONO_NOW" ]
}
mono_now
GATE_MONO_START="$MONO_NOW"
[ -n "$GATE_MONO_START" ] || die_infra "cannot read a monotonic clock"
remaining() {
    local _now _r
    mono_now && _now="$MONO_NOW" || _now=""
    if [ -z "$_now" ]; then
        # A clock we cannot read is INFRASTRUCTURE, never "plenty of time".
        printf '1'
        return
    fi
    _r=$(( BUDGET_SECS - (_now - GATE_MONO_START) ))
    [ "$_r" -lt 1 ] && _r=1   # timeout(1) treats 0 as "no timeout"
    printf '%s' "$_r"
}

# THE SAME CLOCK, WITHOUT THE FLOOR -- and the floor is exactly why this exists.
# `remaining` never returns less than 1, which is correct for a SINGLE call
# (timeout(1) reads 0 as "no timeout at all") and wrong for a LOOP: every probe
# in a batch is then granted a fresh second regardless of how long the batch has
# already run, so a bounded-looking loop overruns the gate deadline in
# proportion to how many probes it spawns. Measured on the staged script with a
# 1s budget: a stalling revision search ran 6.19s (Codex perf, section 52 round
# 13, [medium]). Callers that spawn repeatedly ask THIS one and refuse to spawn
# at all once it answers 0.
#
# `?` when the clock is unreadable, which callers must treat as expired: a
# budget that cannot be measured is infrastructure, never "plenty of time".
budget_left() {   # sets BUDGET_LEFT to seconds remaining (>= 0), or `?`
    local _r
    # THE CLOCK ITSELF IS BOUNDED. Every probe below is wrapped in a deadline
    # this function computes, so an interpreter that never starts would stop
    # the wrapped probe from ever running and the gate would hang exactly where
    # it claims to be bounded (Codex adversarial, section 52 round 16,
    # [medium]). A FIXED small bound, not the budget: it is measuring the
    # budget, so it cannot spend it, and an unreadable clock is `?` -- which
    # every caller treats as expired.
    if ! mono_now; then
        BUDGET_LEFT="?"
        return 0
    fi
    _r=$(( BUDGET_SECS - (MONO_NOW - GATE_MONO_START) ))
    [ "$_r" -lt 0 ] && _r=0
    BUDGET_LEFT="$_r"
}

# A LISTED REPLACEMENT IS NOT NECESSARILY AN ACTIVE ONE. `GIT_NO_REPLACE_OBJECTS`
# and `core.useReplaceRefs=false` turn replacement reads OFF, so every read the
# gate makes resolves stored objects while `git replace -l` still enumerates the
# refs -- and a range whose base was never substituted was refused as
# manufactured (Codex adversarial, section 50 round 11).
#

# SNAPSHOT FIRST, PIN SECOND, ADJUDICATE THIRD -- and the ORDER is the fix.
#
# The first cut asked the question after both endpoints were resolved, which
# left the RESOLUTION itself running under substituted semantics: the fallback
# base is `HEAD~1`, and reading HEAD's parent resolves THROUGH a replacement of
# the head commit, so the gate could derive a base from an object the
# repository manufactured. Removing the replacement before the guard's own
# `git replace -l` then made the metadata look clean, and the manufactured
# range was adjudicated and recorded as a baseline while the stored one went
# unexamined (Codex adversarial, section 52 round 2, [high]).
#
# So the effective replacement state is captured HERE, before a single
# revision is peeled or a parent traversed, and replacement reads are pinned
# off immediately afterwards. Everything downstream -- endpoint resolution, the
# closure measurement, the base worktree, every `ls-tree` -- then resolves
# STORED objects, and the adjudication below tests the captured set against
# those stored walks rather than re-asking a question whose answer could have
# moved underneath it.
# ONE READ OF THE SETTING, DERIVED INTO BOTH THE STATE AND THE BASELINE.
# Reading it twice -- once to decide the state, once to record what to
# revalidate against -- opened a window between the two in which they could
# already disagree, which is the exact hazard the revalidation exists to close.
#
# AND IT IS CAPTURED IN EVERY STATE, not only the enabled one.
# `core.useReplaceRefs` can be turned ON while the gate runs: with it initially
# false the capture is skipped, the listing is never read, and the revalidation
# was guarded on the enabled state -- so a concurrent enable left ordinary git
# resolving through pre-existing replace refs while this gate stayed pinned to
# stored objects and reported clean, returning rc 0 and advancing its baseline
# over a substituted endpoint (Codex adversarial, section 52 round 16, [high]).
# The same shape as the zero-to-one listing race, one level up.
#
# PRESENCE, NOT TRUTHINESS, for the environment override. git disables
# replacement reads when `GIT_NO_REPLACE_OBJECTS` is SET, whatever its value --
# confirmed directly: with `GIT_NO_REPLACE_OBJECTS=0`, `git cat-file -p` on a
# replaced object prints the ORIGINAL. Recognising only plainly-true values and
# calling the rest undecidable refused ranges git was definitively not
# substituting (Codex adversarial, section 50 round 14).
#
# THE AMBIGUOUS CASE FAILS CLOSED RATHER THAN GUESSING WHICH WAY GIT READ IT.
# Only a plainly-false value counts as "disabled"; a value that could not be
# read is undecided, because being wrong in the other direction would mean
# trusting a substituted tree, and an over-refusal is the cheaper error.
budget_left; _left="$BUDGET_LEFT"
if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
    REPLACE_SETTING="<unreadable>"
else
    REPLACE_SETTING="$(timeout -s KILL "$_left" \
                           git config --bool --get core.useReplaceRefs 2>/dev/null)"
    case "$?" in
        0) ;;
        1) REPLACE_SETTING="<unset>" ;;   # git's default is to use them
        *) REPLACE_SETTING="<unreadable>" ;;
    esac
fi
if [ "${GIT_NO_REPLACE_OBJECTS+set}" = set ]; then
    REPLACE_STATE=1
elif [ "$REPLACE_SETTING" = "false" ]; then
    REPLACE_STATE=1
elif [ "$REPLACE_SETTING" = "<unreadable>" ]; then
    REPLACE_STATE=2
else
    REPLACE_STATE=0
fi
REPLACED_LIST=""
if [ "$REPLACE_STATE" -eq 0 ]; then
    # A FAILED LISTING IS NOT AN EMPTY ONE. `|| REPLACE_STATE=2` already said
    # that; the bound makes it reachable rather than something the gate hangs
    # before deciding.
    budget_left; _left="$BUDGET_LEFT"
    if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
        REPLACE_STATE=2
    else
        REPLACED_LIST="$(timeout -s KILL "$_left" \
                             git replace -l 2>/dev/null)" || REPLACE_STATE=2
    fi
fi

# THE ACTIVE PEEL OF EACH ENDPOINT SPEC IS CAPTURED HERE TOO, in the SAME
# unpinned moment as the list above, and that adjacency is the point.
#
# The peel from a commit-ish to a commit is itself a substitutable read, and no
# object walk can see it: `--head <annotated-tag>` resolves through the tag
# OBJECT, and a tag object is not reachable from the commit it points at, so it
# is in neither the history walk nor the snapshot walk. Under the pin the gate
# peels the STORED tag while the operator's git peels the replacement to a
# different commit, and the gate would certify a range nobody asked about
# (Codex adversarial AND consistency, section 52 round 7, found independently
# by both).
#
# ASKING GIT AGAIN LATER IS NOT THE SAME QUESTION. The first fix re-peeled with
# the pin lifted at adjudication time, which read whatever refs existed THEN --
# so a replacement deleted after capture and restored afterwards made the two
# peels agree while the operator still resolved elsewhere, defeating the
# point-in-time capture this block exists to take (Codex adversarial, section
# 52 round 8). Captured here instead, the operator's answer and the replacement
# metadata are read as one snapshot, and adjudication is pure comparison with
# no further git call.
#
# NOTHING CAPTURED HERE IS EVER USED AS A RESOLUTION. It is only ever compared
# against the stored peel taken after the pin, which is the value the gate
# keeps -- so an endpoint the repository manufactured cannot become the range,
# which is what asking before the pin cost the first time (round 2).
#
# ONLY THE SPECS KNOWABLE NOW. The derived `HEAD~1` base is not among them: it
# is not formed until the head is resolved. It needs no entry, because reading
# the head commit's parent is a read of the HEAD COMMIT OBJECT, and a
# replacement of that object is already refused by the reachability
# adjudication below.
# PARALLEL ARRAYS, NOT A DELIMITED TABLE, because a revision spec is an
# arbitrary string and git accepts whitespace inside one -- `HEAD^{/some
# message}` is a legal endpoint. A `spec peel` table looked up by whitespace
# field made every such spec MISS its own row, and a miss is deliberately read
# as "no divergence", so the substituted-tag bypass reopened for exactly the
# specs nobody would think to test (Codex adversarial, section 52 round 9,
# [medium]). Exact-key comparison has no such failure mode.
# BOTH PEELS OF A SPEC ARE TAKEN HERE, ADJACENTLY, AND COMPARED TO EACH OTHER.
# The first cut captured only the ACTIVE peel and compared it against the
# resolution taken after the pin -- two reads separated by the whole endpoint
# resolution, so an endpoint ref that MOVED in between (a commit landing on the
# branch under test) diverged for a reason that has nothing to do with
# replacement metadata, and the gate emitted a token whose published meaning
# says metadata caused it. A false rc 3 pointing the operator at the wrong
# repair (Codex consistency, section 52 round 11, [medium]).
#
# MOVEMENT IS DETECTED RATHER THAN ASSUMED AWAY. The active peel is taken
# TWICE, once on each side of the stored one; if those two disagree the spec
# moved while this block was reading it, which is a different fact with a
# different repair (re-run) and gets its own token. Only when they agree is a
# difference against the stored peel attributable to replacement metadata --
# which is the whole claim the substitution tokens make.
ACTIVE_PEEL_SPECS=()
ACTIVE_PEEL_VALS=()
ACTIVE_PEEL_OIDS=()
if [ "$REPLACE_STATE" -eq 0 ] && [ -n "$REPLACED_LIST" ]; then
    for _spec in "$HEAD_SHA" "$BASE_SHA" \
                 "${IDENTITY_GATE_LAST_GATED_SHA:-}" "${GITHUB_EVENT_BEFORE:-}"; do
        [ -n "$_spec" ] || continue
        # A FAILED PEEL IS RECORDED AS `?`, NOT AS ABSENT: absent means "no
        # entry, nothing to compare", and a probe that could not answer must
        # refuse rather than read as agreement. `!` is the moved-under-us
        # sentinel; neither can collide with an oid, which is hex.
        # BOUNDED LIKE EVERY OTHER LONG STEP. A revision spec is not always a
        # cheap peel: `<rev>^{/regex}` SEARCHES history, and this file's own
        # fixtures use that syntax, so three unbounded calls per spec across
        # four specs is up to twelve history traversals ahead of any deadline
        # -- measured at 118ms for a hit and 181ms for a miss on a 5,696-commit
        # checkout, and unbounded on a larger or degraded one (Codex perf,
        # section 52 round 12, [medium]).
        budget_left; _left="$BUDGET_LEFT"
        if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
            # NO SPAWN PAST THE DEADLINE. The batch shares one budget, so a
            # probe that could only start by borrowing time the gate no longer
            # has is not run; the spec records an unanswerable peel and the
            # refusal says so.
            _peel="" _stored="" _again=""
        else
            _peel="$(timeout -s KILL "$_left" \
                         git rev-parse --verify --quiet "${_spec}^{commit}" 2>/dev/null)" || _peel=""
            budget_left; _left="$BUDGET_LEFT"
            if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
                _stored="" _again=""
            else
                _stored="$(GIT_NO_REPLACE_OBJECTS=1 timeout -s KILL "$_left" \
                             git rev-parse --verify --quiet "${_spec}^{commit}" 2>/dev/null)" || _stored=""
                budget_left; _left="$BUDGET_LEFT"
                if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
                    _again=""
                else
                    _again="$(timeout -s KILL "$_left" \
                                 git rev-parse --verify --quiet "${_spec}^{commit}" 2>/dev/null)" || _again=""
                fi
            fi
        fi
        # ALL THREE PROBES ARE TESTED FOR AN ANSWER BEFORE ANY OF THEM IS
        # COMPARED. Checking only the first two let a failed THIRD peel -- an
        # empty string -- read as "differs from the first", so an unanswered
        # probe was reported as the endpoint having MOVED: a definite token for
        # an indeterminate observation, sending the operator to re-run a
        # repository that was never moving (Codex adversarial AND consistency,
        # section 52 round 12, [medium], found independently by both).
        if [ -z "$_peel" ] || [ -z "$_stored" ] || [ -z "$_again" ]; then
            _peel="?"
        elif [ "$_peel" != "$_again" ]; then
            _peel="!"
        elif [ "$_peel" = "$_stored" ]; then
            _peel=""     # agrees: no divergence to record
        fi
        ACTIVE_PEEL_SPECS+=("$_spec")
        ACTIVE_PEEL_VALS+=("${_peel:-=}")
        ACTIVE_PEEL_OIDS+=("$_again")
    done
fi
export GIT_NO_REPLACE_OBJECTS=1


# EVERY ENDPOINT PEEL IS BOUNDED, INCLUDING THE ONES THAT RESOLVE THE RANGE.
# A revision spec is arbitrary and `<rev>^{/regex}` searches history, so the
# handful of `rev-parse` calls that establish the endpoints are as capable of
# stalling as the capture batch below -- and they had no bound at all. A
# 20s-per-call stall against a 2s budget spent 100s in the gate before any
# section-52 probe ran, which is the same overrun the capture batch was just
# repaired for and would have made that repair look ineffective.
#
# THREE OUTCOMES, KEPT APART. rc 0 is an answer; rc 1 is `--verify` saying the
# spec does not resolve, which callers legitimately branch on; anything else --
# including the timeout's 124/137 and an expired budget -- is the gate failing
# to ASK, which must never read as "does not resolve" (this file's oldest
# recurring fault).
# THE SAME DISCIPLINE FOR ANY GIT QUERY THIS GATE DEPENDS ON. The closure
# passes spawn `ls-tree`, `rev-parse`, `cat-file` and `hash-object` per member
# and had no limit at all, so stalled repository or filesystem I/O could wedge
# a local pre-push run indefinitely while the file advertised one shared
# deadline (Codex adversarial AND perf, section 52 round 20, [medium], found
# independently by both). Callers keep their existing `||` handling: rc 2 lands
# in the same arm a git failure already did, so the bound is added without
# changing what any failure MEANS.
# NO `--foreground` ON THE GIT PROBES, DELIBERATELY -- AND THE FLAG STAYS
# EVERYWHERE ELSE. Removing it wholesale reopened a documented section 50
# round 24 [high]: `proto_of`, the transport read, the base checkout and its
# cleanup removals are kept IN the gate's own process group on purpose, so a
# TERM or INT delivered to the gate reaches them and `cleanup` can reap the
# linked tree and the tempdir; the children caveat costs nothing there because
# those probes spawn none (Codex adversarial, section 52 round 30, [high]). It
# costs plenty here, which is why the git probes are the exception:
#
# That flag's documented meaning is that the
# command is NOT put in its own process group, which excludes its CHILDREN from
# the timeout -- measured directly: a 1s foreground timeout around a process
# with a 5s child took the full 5s, and the same command without the flag took
# 1s (Codex adversarial, section 52 round 29, [medium]). Git does spawn
# descendants (hooks, promisor and lazy-fetch helpers), and a stalled one holds
# the command substitution's pipe open past the budget. Without the flag the
# whole group is signalled, which is what a shared deadline has to mean.
# REFUSE TO LAUNCH A PHASE PAST THE DEADLINE, rather than launch it with a
# floor (section 55). `remaining()` never answers less than 1, which is correct
# as a `timeout` ARGUMENT -- 0 disables the bound outright -- and wrong as a
# DECISION: every phase launched with it was granted a fresh second no matter
# how long the run had already taken, so a gate at its ceiling could still start
# six more processes. Section 52 gave the git probes this refusal and did not
# widen into the python phases; `contract_of` got it when section 55 bounded it;
# these six are the rest of the set, and leaving them was the residue that
# section's own item recorded.
PHASE_BUDGET=""
phase_budget() {   # $1 = phase name; ANSWERS in PHASE_BUDGET, refuses if expired
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then
        die_infra "the gate's budget expired before the $1 phase could start, so it was never launched and no verdict is being guessed from it"
    fi
    # AND THE ANCHOR IS RE-ASKED AT EVERY PHASE BOUNDARY. This is DETECTION, not
    # containment, and saying so is the point: `phase_budget` runs BEFORE a
    # phase, so a descendant can still substitute the directory after this check
    # and before the redirection that follows it. What it buys is that the
    # substitution becomes an infrastructure REFUSAL at the next boundary
    # instead of a PASS computed over files the gate never wrote -- the window
    # narrows from the whole run to a single phase. Closing it properly means
    # binding every name-resolved use to the descriptor, which is parked in this
    # section's own checklist rather than claimed here (Codex design review,
    # section 56, [high]). The check is builtins only, so a boundary costs
    # nothing; it is skipped before the anchor exists, which is the only state
    # in which TMP_FD is unset.
    if [ -n "${TMP_FD:-}" ] && ! tmp_dir_anchor_ok; then
        die_infra "the gate's own temporary directory is no longer reached by the name it was created under, so the $1 phase was never launched: $TMP_DIR does not name the directory this run anchored a descriptor on, and every path under it is therefore untrusted"
    fi
    PHASE_BUDGET="$BUDGET_LEFT"
}

bounded_fs() {   # a filesystem command whose failure is tolerated, but not its duration
    # THE EXEMPTION THIS REPLACES WAS WRONG, WHICH IS WORSE THAN ABSENT. Every
    # `rm -f` here carried `launch-exempt: unlink does not open the file, so it
    # cannot block`, and that reasoning does not hold: `unlink` updates
    # DIRECTORY METADATA, which blocks on degraded, networked or FUSE-backed
    # storage exactly as any other write does, and `mkdir` is the same (Codex
    # adversarial, section 55 round 17, [medium]). A marker states a claim about
    # the code; a false one is a hole with a justification written over it, and
    # it survives review precisely because it looks considered.
    #
    # These calls are cleanup-shaped, so their FAILURE stays tolerated -- what is
    # no longer tolerated is their duration.
    #
    # AND AN EXPIRED BUDGET DECLINES THE LAUNCH, which reverses this function's
    # first version. That one gave a spent budget a fixed ten seconds, reasoning
    # that declining to tidy up was worse than tidying up slowly. It is not:
    # every file these calls remove lives under TMP_DIR, and the exit cleanup
    # removes TMP_DIR whole, under its own bound. So the post-expiry launch
    # bought nothing that was not already covered, and it kept a push alive for
    # ten seconds past a ceiling this file advertises -- which made section 55's
    # own deadline claim false while it was written (Codex adversarial, section
    # 55 round 18, [medium]). A cleanup that is redundant is the easiest kind to
    # decline.
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then
        return 0
    fi
    timeout --foreground -s KILL "$BUDGET_LEFT" "$@" 2>/dev/null || true
    return 0
}

tmp_dir_anchor_ok() {   # 0 = the NAME still names the anchored directory itself
    # A PATHNAME IS NOT AN IDENTITY, and section 55 only closed half of that.
    # The narrow assembler reaches everything it writes descriptor-relative from
    # `TMP_FD`, so a rename cannot redirect it. Every OTHER use of TMP_DIR
    # resolves the NAME -- the cache paths, the walk logs, the two full
    # checkouts, and `cleanup`'s own removal -- so a descendant the gate could
    # not reap can rename the directory, leave something else at the name, and
    # have the gate delete the replacement while the original leaks.
    #
    # NOT `-ef` ALONE. `test -ef` STATS both operands, so a symlink left at
    # TMP_DIR pointing back at the anchored directory satisfies it -- and
    # `rm -rf "$TMP_DIR"` would then unlink the symlink and leave the directory
    # it names behind, which is the very leak this check exists to catch (Codex
    # design review, section 56, [high]). The name must BE the directory, not a
    # route to it, so the symlink test comes first.
    #
    # NO FORK. `-L`, `-e` and `-ef` are all shell builtins, which is what makes
    # it affordable to ask at every phase boundary.
    [ -n "${TMP_FD:-}" ] || return 1
    [ ! -L "$TMP_DIR" ] || return 1
    [ -e "/proc/self/fd/$TMP_FD" ] || return 1
    [ "$TMP_DIR" -ef "/proc/self/fd/$TMP_FD" ]
}

reap_walk_group() {   # $@ = leader pids already `wait`ed for
    # ONE REAP, ELEVEN SITES. Every supervised phase in this file ends the same
    # way: capture the status, declare the leader waited, reap whatever the
    # phase left in its process group, untrack. Sections 53 and 55 wrote that
    # block five times by hand and the copies were byte-identical apart from the
    # pid variable name, which is the argument for extracting it.
    #
    # AND IT IS SAFE TO EXTRACT because the block never reads or writes the
    # caller's STATUS. Every site captures its own `$?` into a named variable
    # BEFORE calling this, and the file runs without `errexit`, so this
    # function's return value cannot reach a caller's control flow. The things
    # the sites genuinely differ in -- which status they preserve, what they
    # have already waited for, which tokens they raise -- all stay at the site.
    # That is the answer to this section's own question about whether a shared
    # abstraction could quietly change a status somewhere it is not read: the
    # extracted region does not touch one (Codex design review, section 56).
    #
    # `WALK_PIDS` AND `WALK_WAITED` STAY THE CALLER'S. This function only
    # signals; the caller keeps its tracking assignments around the call, so a
    # trap firing inside the reap still finds a target and `cleanup` can finish
    # what this started.
    #
    # EVERY SCRATCH NAME IS `local`, because bash has dynamic scope: an
    # unlocalized loop variable here would overwrite a caller's local of the
    # same name, and two of the call sites are inside functions that use
    # `local` (Codex design review, section 56).
    local _rg _i
    for _rg in "$@"; do
        [ -n "$_rg" ] || continue
        # ONLY WHILE THE NUMBER STILL MEANS WHAT IT MEANT. `wait` has released
        # the leader pid, so the group id is a number the kernel may reissue. A
        # leader that is ALIVE again has been recycled and the group belongs to
        # a stranger, so the reap is skipped rather than aimed at one; a leader
        # that is gone while the negative group still answers means the members
        # are what the tree left behind (section 53). Probing first also makes
        # the honest path cost exactly one failed signal.
        if ! kill -0 "$_rg" 2>/dev/null && kill -0 -- "-$_rg" 2>/dev/null; then
            kill -TERM -- "-$_rg" 2>/dev/null || true
            # THE GRACE ANSWERS TO THE GLOBAL DEADLINE like every other wait in
            # this file. Left unbounded at up to 3s a site, eleven sites could
            # push the gate far past its advertised budget (section 53 perf).
            for _i in 1 2 3 4 5 6; do
                kill -0 -- "-$_rg" 2>/dev/null || break
                [ "$(remaining)" -gt 1 ] || break
                sleep 0.5  # launch-exempt: a fixed sub-second sleep does no I/O, and the loop re-checks the budget each turn
            done
            kill -KILL -- "-$_rg" 2>/dev/null || true
        fi
    done
    # EXPLICIT, so the status of the last `kill` in the loop is never what a
    # caller sees if one is ever written that reads it.
    return 0
}

bounded_git() {   # args after `git`; 0 ok, 1 git said no, 2 could not ask
    local _left _out _rc
    budget_left; _left="$BUDGET_LEFT"
    { [ "$_left" = "?" ] || [ "$_left" -le 0 ]; } && return 2
    _out="$(timeout -s KILL "$_left" git "$@" 2>/dev/null)"
    _rc=$?
    case "$_rc" in
        0) printf '%s' "$_out"; return 0 ;;
        1) return 1 ;;
        *) return 2 ;;
    esac
}

bounded_rev_parse() {   # args after `git rev-parse`; 0 ok, 1 no-resolve, 2 could not ask
    local _left _out _rc
    budget_left; _left="$BUDGET_LEFT"
    { [ "$_left" = "?" ] || [ "$_left" -le 0 ]; } && return 2
    _out="$(timeout -s KILL "$_left" git rev-parse "$@" 2>/dev/null)"
    _rc=$?
    case "$_rc" in
        0) printf '%s' "$_out"; return 0 ;;
        1) return 1 ;;
        *) return 2 ;;
    esac
}

HEAD_SPEC="$HEAD_SHA"
# `--quiet` IS LOAD-BEARING HERE, not tidiness. Without it `--verify` exits 128
# on an invalid revision, which the helper classifies as "could not ask" -- so
# `--head definitely-not-a-ref`, a plain caller error, was reported as failed
# machinery under ENDPOINT_UNRESOLVABLE and sent the operator to repair
# infrastructure (Codex adversarial, section 52 round 14, [medium]). With it,
# a non-resolving revision is rc 1, which is an ANSWER.
HEAD_RESOLVED="$(bounded_rev_parse --verify --quiet "${HEAD_SHA}^{commit}")"
case "$?" in
    0) ;;
    1) die_infra "head '$HEAD_SHA' is not a commit" ;;
    *) die_infra "ENDPOINT_UNRESOLVABLE: the gate could not ask what '$HEAD_SHA' resolves to -- the probe failed or the budget expired while it ran, which is the gate's own machinery and not a statement about the argument, and no range is being guessed from it" ;;
esac

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
    bounded_rev_parse --verify --quiet "${BASE_SHA}^{commit}" >/dev/null
    case "$?" in
        0) ;;
        1) die_infra "--base '$BASE_SHA' is not a commit in this repository" ;;
        *) die_infra "ENDPOINT_UNRESOLVABLE: the gate could not ask what '--base $BASE_SHA' resolves to -- the probe failed or the budget expired while it ran, which is the gate's own machinery and not a statement about the argument, and no range is being guessed from it" ;;
    esac
fi

# THE SPEC IS KEPT, NOT JUST THE RESOLUTION. A commit-ish is not always a
# commit oid, and the peel from one to the other is itself a read git can
# substitute, so the string that was peeled has to survive -- as the KEY that
# locates its snapshot-time captured peel, which the check below compares
# against the stored one. It is peeled again only by the revalidation pass,
# which asks whether the spec still points where it did -- never to decide the
# substitution verdict itself.
BASE_SPEC="$BASE_SHA"
BASE_SPEC_DERIVED=0
if [ -z "$BASE_SHA" ]; then
    BASE_SHA="${IDENTITY_GATE_LAST_GATED_SHA:-}"
    BASE_SPEC="$BASE_SHA"
fi
BASE_CAND_RC=0
if [ -n "$BASE_SHA" ]; then
    bounded_rev_parse --verify --quiet "${BASE_SHA}^{commit}" >/dev/null
    BASE_CAND_RC=$?
    # A PROBE THAT COULD NOT RUN IS NOT "THIS BASE IS NO GOOD". Folded into the
    # `||` chain it silently advanced to the next candidate and answered about
    # a different range -- the failed-question-as-negative-answer fault, here in
    # the one place that chooses WHICH range is adjudicated.
    [ "$BASE_CAND_RC" -eq 2 ] && die_infra "ENDPOINT_UNRESOLVABLE: the gate could not ask whether the last-gated base '$BASE_SHA' is still a commit here -- the probe failed or the budget expired while it ran, so no fallback is being taken and no range is being guessed"
fi
if [ -z "$BASE_SHA" ] || [ "$BASE_CAND_RC" -ne 0 ]; then
    # The zero sentinel is what GitHub sends for a branch's first push and can
    # also appear after a force-push; treat it as absent rather than as a SHA.
    CAND="${GITHUB_EVENT_BEFORE:-}"
    case "$CAND" in
        0000000000000000000000000000000000000000|"") CAND="" ;;
    esac
    # ONLY rc 1 PERMITS A FALLBACK. These three probes CHOOSE WHICH RANGE IS
    # ADJUDICATED, so folding their status into a boolean is the worst place in
    # the file for the failed-question-as-negative-answer fault: a stalled or
    # failed event-base lookup read as "that candidate is unusable" and the
    # chain quietly advanced to HEAD~1, adjudicating only the last commit of a
    # multi-commit push and able to return GREEN over resolver changes it never
    # compared (Codex adversarial AND consistency, section 52 round 14, [high],
    # found independently by both). They were also the last unbounded spawns in
    # the endpoint batch, so a stall here escaped the shared deadline entirely.
    CAND_RC=1
    if [ -n "$CAND" ]; then
        bounded_rev_parse --verify --quiet "${CAND}^{commit}" >/dev/null
        CAND_RC=$?
        [ "$CAND_RC" -eq 2 ] && die_infra "ENDPOINT_UNRESOLVABLE: the gate could not ask whether the push event's before-SHA '$CAND' is a commit here -- the probe failed or the budget expired while it ran. No fallback is being taken, because falling back would silently adjudicate a narrower range than the one requested"
    fi
    PARENT_RC=1
    if [ "$CAND_RC" -ne 0 ]; then
        bounded_rev_parse --verify --quiet "${HEAD_RESOLVED}~1^{commit}" >/dev/null
        PARENT_RC=$?
        [ "$PARENT_RC" -eq 2 ] && die_infra "ENDPOINT_UNRESOLVABLE: the gate could not ask whether '$HEAD_RESOLVED~1' is a commit -- the probe failed or the budget expired while it ran, so no base is being derived and no range is being guessed"
    fi
    if [ "$CAND_RC" -eq 0 ]; then
        BASE_SHA="$CAND"
        BASE_SPEC="$CAND"
    elif [ "$PARENT_RC" -eq 0 ]; then
        BASE_SHA="$(bounded_rev_parse "${HEAD_RESOLVED}~1")" \
            || die_infra "ENDPOINT_UNRESOLVABLE: the gate could not resolve '$HEAD_RESOLVED~1' after confirming it exists -- the probe failed or the budget expired while it ran, and no range is being guessed from it"
        BASE_SPEC="${HEAD_RESOLVED}~1"
        BASE_SPEC_DERIVED=1
        log "no gated/event base reachable; falling back to HEAD~1"
    else
        die_infra "no usable base commit (no last-gated SHA, no reachable event before-SHA, no HEAD~1)"
    fi
fi
BASE_SHA="$(bounded_rev_parse "${BASE_SHA}^{commit}")" \
    || die_infra "ENDPOINT_UNRESOLVABLE: the gate could not peel the base '$BASE_SPEC' to a commit after confirming it is one -- the probe failed or the budget expired while it ran, and no range is being guessed from it"

log "base $BASE_SHA"
log "head $HEAD_RESOLVED"

# THE PEEL FROM A COMMIT-ISH TO A COMMIT IS ITSELF A SUBSTITUTABLE READ, and
# the walks above cannot see it. Endpoints are peeled UNDER the pin, so
# `--head <annotated-tag>` resolves through the STORED tag object -- while the
# operator's own git, and every other tool they run, resolves it through the
# replacement to a different commit. The tag object is not reachable from the
# commit it points at (tags point toward commits, not the reverse), so neither
# the history walk nor the snapshot walk contains it, and the gate would
# certify a range nobody asked about and advance its confidence over it (Codex
# adversarial AND consistency, section 52 round 7, [medium], found
# independently by both).
#
# ASKED AS A COMPARISON RATHER THAN A CHAIN WALK. Enumerating a peel chain
# means re-implementing what `rev-parse` already does, including nested tags;
# comparing the SNAPSHOT-TIME peel of the same spec against the stored one
# answers the question that actually matters -- does this gate resolve the
# endpoint the repository resolves -- and covers ref chains and nested tags
# without knowing their shape. Nothing manufactured is ever USED: the stored
# resolution is what the gate keeps, and a divergence is refused outright.
#
# THE VERDICT IS LOOKED UP, NOT RE-DERIVED, and that is the round-8 repair
# rather than a detail: deciding it here read whatever refs existed at THIS
# moment, so a replacement removed after capture and restored afterwards made
# the two peels agree while the operator still resolved elsewhere. This
# function spawns nothing. The freshness of what it reads back is not its job
# either: the revalidation pass above has already confirmed the captured state
# still describes the repository, which is why `ACTIVE_PEEL_OIDS` is retained.
endpoint_peel_diverges() {  # $1 = spec, $2 = stored resolution, $3 = 1 if derived; 0 diverges, 1 same, 2 cannot tell
    local _i _a=""
    for _i in "${!ACTIVE_PEEL_SPECS[@]}"; do
        if [ "${ACTIVE_PEEL_SPECS[$_i]}" = "$1" ]; then
            _a="${ACTIVE_PEEL_VALS[$_i]}"
            break
        fi
    done
    if [ -z "$_a" ]; then
        # A MISS IS ONLY BENIGN FOR THE ONE SPEC THAT CANNOT BE CAPTURED. The
        # derived `HEAD~1` base is not formed until the head is resolved, and
        # it needs no entry: reading the head commit's parent is a read of the
        # HEAD COMMIT OBJECT, which the reachability adjudication refuses. Any
        # OTHER miss means the capture and the resolution disagree about what
        # was asked, and that is not something to assume away -- it is how a
        # whitespace-bearing spec quietly skipped this check once already.
        [ "$3" = "1" ] && return 1
        return 2
    fi
    # THE VERDICT WAS DECIDED AT CAPTURE, comparing two peels taken in the same
    # moment; this only reads it back. Comparing against the post-pin
    # resolution instead is what let a moving ref look like a substitution.
    case "$_a" in
        "=") return 1 ;;   # the two peels agreed
        "?") return 2 ;;   # a peel could not be taken
        "!") return 3 ;;   # the spec moved while it was being read
        *)   return 0 ;;   # they differed: replacement metadata is the cause
    esac
}

# GUARDED ON THE CAPTURE, NOT ON REPLACEMENT SUPPORT BEING ENABLED. `REPLACE_STATE`
# is 0 whenever git WOULD honour replacements, which is the default everywhere,
# so testing it alone spawned two probes on every ordinary run for a repository
# with no replacements at all -- measured at 2.69ms and flatly contradicting the
# no-replacement fast path this block advertises (Codex perf, section 52 round
# 8, [low]). An empty capture means there was nothing to diverge.
# THE SNAPSHOT IS REVALIDATED BEFORE ANYTHING IS DECIDED FROM IT.
#
# Everything above was captured before the endpoints were resolved, and the
# capture is what every substitution answer rests on -- so if the repository
# moved in between, those answers describe a repository that is no longer
# there. Two things can move, and BOTH were unchecked:
#
#   - THE REPLACEMENT METADATA ITSELF. An entry added after `git replace -l`
#     was read is in no captured set and, when it targets a tag object, in
#     neither commit walk either -- so the gate could return a GREEN verdict
#     while ordinary git resolves the endpoint through a substitute it never
#     saw. That is a false pass, not merely a mis-named refusal, which is why
#     it is checked rather than argued away (Codex adversarial, section 52
#     round 12, [medium]).
#   - THE ENDPOINT REFS. A spec can move after its third peel and before it is
#     resolved, leaving an `=` verdict recorded about a commit the gate is no
#     longer adjudicating. Re-taking the peel is cheap and the captured oid was
#     retained for exactly this comparison.
#
# WHAT THIS STILL CANNOT SEE, stated rather than implied: an A-B-A movement
# INSIDE the capture triple is indistinguishable from a substitution, and this
# check cannot separate them either. The outcome there is a refusal carrying
# the substitution token rather than the movement one -- fail-closed, wrongly
# named. Separating them needs an atomic read of refs and metadata together,
# which git does not offer.
# THE LISTING RE-READ IS NOT GUARDED ON THE CAPTURE HAVING FOUND ANYTHING, and
# tying it to that was a false pass rather than an optimisation: a repository
# that starts with NO replacements captures an empty set, so the guard skipped
# the re-read entirely and the FIRST replacement created after capture was
# never seen -- the gate pinned reads to stored objects, adjudicated an empty
# captured set, and could return rc 0 while ordinary git resolved a substituted
# endpoint (Codex adversarial, section 52 round 13, [medium]). Zero-to-one is
# precisely the transition an attacker or a concurrent tool makes.
# THE ENABLEMENT IS REVALIDATED WHATEVER STATE IT STARTED IN, and it is a
# FUNCTION because once is not enough. Checked at a single point mid-run, an
# enable landing AFTER that point still slipped through: the state stayed
# "disabled", the listing check stayed skipped, and the gate could exit 0 while
# ordinary git had begun resolving through pre-existing replace refs (Codex
# adversarial, section 52 round 17, [high]). So it is asserted here AND
# immediately before every successful exit -- the gate never returns green
# without confirming the replacement state it captured still holds, which is
# the same discipline section 51 applied to the closure.
replacement_state_still_holds() {
    local _left _now_setting _now_replaced _rc _i _re _spec _was
    # THREE QUESTIONS, IN THIS ORDER, AND THE ORDER IS THE WHOLE REPAIR HISTORY
    # OF THIS FUNCTION:
    #
    #   1. Do the ENDPOINTS still resolve to what the gate is adjudicating?
    #      Asked FIRST, and unconditionally, because it is not a replacement
    #      question at all. Behind the state gate below it was skipped whenever
    #      `core.useReplaceRefs=false` or the operator's own
    #      `GIT_NO_REPLACE_OBJECTS` opt-out was in force, so a branch could
    #      advance and the gate certified a range it was not asked about (Codex
    #      adversarial, section 52 round 21, [high]).
    #   2. Does the captured replacement METADATA still match? Only meaningful
    #      where replacements are in effect, so it is the one part the state
    #      gates -- as an `if`, never an early return, because an early return
    #      skips question 3 as well.
    #   3. Is the replacement SETTING still what was captured? Asked LAST.
    #      Asked first and compared first, a flip landing during questions 1
    #      and 2 was invisible: those reads stay pinned to stored objects, the
    #      saved values compared equal, and the function returned success over
    #      a repository that had begun substituting (Codex adversarial, section
    #      52 round 22, [high]). Asked last, the only window left is between
    #      this read and the caller's own exit, which no snapshot can close.

    # ---- 1. the endpoints
    for _i in base head; do
        case "$_i" in
            head) _spec="$HEAD_SPEC"; _was="$HEAD_RESOLVED" ;;
            base) _spec="$BASE_SPEC"; _was="$BASE_SHA" ;;
        esac
        [ -n "$_spec" ] || continue
        _re="$(bounded_rev_parse --verify --quiet "${_spec}^{commit}")"
        case "$?" in
            0|1) ;;
            *) die_infra "ENDPOINT_MOVED_UNDER_GATE: could not re-resolve '$_spec' to confirm it still points where it did, so whether this gate is still adjudicating the range it was asked about is unknown and no verdict is being guessed from it" ;;
        esac
        if [ "$_re" != "$_was" ]; then
            die_infra "ENDPOINT_MOVED_UNDER_GATE: '$_spec' no longer resolves to $_was, which is the commit this gate has been adjudicating, so the range it was asked about has moved. Nothing is wrong with the tree under test; re-run against a repository that holds still"
        fi
    done

    # ---- 2. the replacement metadata, and the peels taken through it
    if [ "$REPLACE_STATE" -eq 0 ]; then
        # PROBE FAILURE IS NOT AN OBSERVED CHANGE. Collapsing a failed listing
        # into a synthetic mismatch emitted a message asserting the metadata
        # CHANGED when the gate had merely failed to ask (Codex consistency,
        # section 52 round 13, [medium]). Bounded like every other spawn, and
        # an expired budget is itself unanswerable rather than clean.
        budget_left; _left="$BUDGET_LEFT"
        if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
            die_infra "ENDPOINT_MOVED_UNDER_GATE: the gate's budget expired before it could confirm the repository's replacement metadata still matches what it captured, so whether the endpoint answers still describe this repository is unknown and none is being guessed from it"
        fi
        _now_replaced="$(timeout -s KILL "$_left" \
                             git replace -l 2>/dev/null)"
        _rc=$?
        if [ "$_rc" -ne 0 ]; then
            die_infra "ENDPOINT_MOVED_UNDER_GATE: could not re-read the repository's replacement metadata to confirm it still matches what this gate captured, so whether the endpoint answers still describe this repository is unknown and none is being guessed from it"
        fi
        if [ "$_now_replaced" != "$REPLACED_LIST" ]; then
            die_infra "ENDPOINT_MOVED_UNDER_GATE: the repository's replacement metadata changed while this gate was working, so the snapshot every endpoint answer was taken from no longer describes it. Nothing is wrong with the tree under test; re-run against a repository that holds still"
        fi
        for _i in "${!ACTIVE_PEEL_SPECS[@]}"; do
            [ -n "${ACTIVE_PEEL_OIDS[$_i]}" ] || continue
            # RE-PEELED WITH REPLACEMENTS ACTIVE, because `ACTIVE_PEEL_OIDS`
            # holds the peels git gives the OPERATOR, not the stored ones.
            # Routing this through the plain bounded helper dropped the `env -u`
            # and compared a stored peel against an active capture, so every
            # replaced-tag fixture began refusing as MOVEMENT -- three of them
            # caught it in the same run.
            budget_left; _left="$BUDGET_LEFT"
            if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
                die_infra "ENDPOINT_MOVED_UNDER_GATE: the gate's budget expired before it could confirm '${ACTIVE_PEEL_SPECS[$_i]}' still resolves to what it did, so whether the endpoint answers still describe this repository is unknown and none is being guessed from it"
            fi
            _re="$(timeout -s KILL "$_left" \
                       env -u GIT_NO_REPLACE_OBJECTS git rev-parse --verify --quiet \
                       "${ACTIVE_PEEL_SPECS[$_i]}^{commit}")"
            # rc 1 IS AN ANSWER: `--verify --quiet` reporting "this does not
            # resolve" is, for a spec that resolved a moment ago, movement -- a
            # deleted ref, say. Anything else is the probe failing, and that is
            # unanswerable rather than movement.
            case "$?" in
                0|1) ;;
                *) die_infra "ENDPOINT_MOVED_UNDER_GATE: could not re-resolve '${ACTIVE_PEEL_SPECS[$_i]}' to confirm it still points where it did, so whether the endpoint answers still describe this repository is unknown and none is being guessed from it" ;;
            esac
            if [ "$_re" != "${ACTIVE_PEEL_OIDS[$_i]}" ]; then
                die_infra "ENDPOINT_MOVED_UNDER_GATE: '${ACTIVE_PEEL_SPECS[$_i]}' no longer resolves to what it did when this gate read it, so the endpoint answers were taken about a repository state that has since moved. Nothing is wrong with the tree under test; re-run against a repository that holds still"
            fi
        done
    fi

    # ---- 3. the setting
    budget_left; _left="$BUDGET_LEFT"
    if [ "$_left" = "?" ] || [ "$_left" -le 0 ]; then
        _now_setting="<unreadable>"
    else
        _now_setting="$(timeout -s KILL "$_left" \
                            git config --bool --get core.useReplaceRefs 2>/dev/null)"
        case "$?" in
            0) ;;
            1) _now_setting="<unset>" ;;
            *) _now_setting="<unreadable>" ;;
        esac
    fi
    # EITHER SIDE UNREADABLE IS INDETERMINATE. Guarding only the re-read left
    # the opposite direction claiming `changed from '<unreadable>'` -- movement
    # asserted from a value that was never observed (Codex consistency, section
    # 52 round 20, [medium]).
    if [ "$_now_setting" = "<unreadable>" ] || [ "$REPLACE_SETTING" = "<unreadable>" ]; then
        die_infra "ENDPOINT_MOVED_UNDER_GATE: could not read core.useReplaceRefs on at least one side of this gate's work (captured '$REPLACE_SETTING', now '$_now_setting'), so whether git substitutes objects is not known to be what it was, and no verdict is being guessed from it"
    fi
    if [ "$_now_setting" != "$REPLACE_SETTING" ]; then
        die_infra "ENDPOINT_MOVED_UNDER_GATE: core.useReplaceRefs changed from '$REPLACE_SETTING' to '$_now_setting' while this gate was working, so whether git substitutes objects is not what it was when the endpoint answers were taken. Nothing is wrong with the tree under test; re-run against a repository that holds still"
    fi
    return 0
}
replacement_state_still_holds


HEAD_PEEL_RC=1
if [ "${#ACTIVE_PEEL_SPECS[@]}" -gt 0 ]; then
    endpoint_peel_diverges "$BASE_SPEC" "$BASE_SHA" "$BASE_SPEC_DERIVED"
    case "$?" in
        3) die_infra "ENDPOINT_MOVED_UNDER_GATE: '$BASE_SPEC' resolved to two different commits while this gate was reading it, so the base moved under the question rather than being substituted by repository metadata. Nothing is wrong with the tree under test; re-run against a repository that holds still" ;;
        0) die_infra "BASE_OBJECTS_SUBSTITUTED: repository replacement metadata makes '$BASE_SPEC' resolve to a different commit than the stored objects do, so the base this gate would adjudicate ($BASE_SHA) is not the base the repository resolves for that argument. Remove the replacement or re-run against a clean clone" ;;
        2) die_infra "BASE_OBJECTS_SUBSTITUTED: the gate holds no usable snapshot of how '$BASE_SPEC' resolves with replacement metadata active, so whether it and the repository agree on which commit the base IS is unknown, and no classification is being guessed from it" ;;
    esac
    # RECORDED, EMITTED LATE, for the same base-first reason as the head
    # substitution answer beside it.
    endpoint_peel_diverges "$HEAD_SPEC" "$HEAD_RESOLVED" 0
    HEAD_PEEL_RC=$?
fi


# ---------------------------------------------------------------------------
# OBJECT SUBSTITUTION IS A REPOSITORY PRECONDITION, SETTLED BEFORE EITHER
# ENDPOINT IS CLASSIFIED (section 52).
#
# Precisely: the replacement metadata is CAPTURED and replacement reads are
# PINNED before the endpoints are resolved at all, so no resolution can be
# manufactured; the endpoint-specific substitution questions are adjudicated
# after that resolution and before any protocol classification. Saying the
# whole precondition ran "before either endpoint is read" was a description of
# the first draft that survived three redesigns (Codex consistency, section 52
# round 14, [low]).
# ---------------------------------------------------------------------------
# `git replace` makes git hand back one object's bytes when another is asked
# for. WITHOUT the pin installed far above, every read here would resolve
# THROUGH those refs -- the base worktree checkout, `ls-tree` on either
# endpoint, `rev-parse <sha>:<path>`, both protocol probes -- and a substituted
# endpoint would make this gate adjudicate a tree the repository manufactured
# rather than one stored at the commit named on the command line.
#
# WITH IT, THE READS ARE ALREADY SAFE AND THIS BLOCK DECIDES SOMETHING ELSE:
# whether the repository's own effective state, as captured before anything was
# resolved, WOULD have substituted an endpoint. That is a question about the
# repository rather than about the reads, and it is asked because a verdict
# reached over objects the operator's own git will not show them is not a
# verdict they can act on. Describing this paragraph in the pre-pin tense read
# as a live trust boundary that no longer exists (Codex consistency, section
# 52 round 5, [medium]).
#
# SECTION 50 ASKED THIS IN ONE ARM; SECTION 52 ASKS IT ONCE, HERE. It sat
# inside the base's UNREADABLE arm, so it ran only once `proto_of` had already
# FAILED -- and the whole point of a substitution is that the synthetic tree
# can parse perfectly. A replacement whose tree carries a clean protocol
# therefore skipped the guard entirely and earned a real verdict, which is the
# more consequential half of the family because it changes a PASS rather than a
# refusal (named by section 50's round-8 reviewer, filed as section 52).
#
# THE HEAD ENDPOINT IS ASKED THE SAME QUESTION, which it never was at all: the
# head side reads the head COMMIT for closure-member presence and mode, for the
# executable-bit invariant, and -- since section 54 -- for every content and
# protocol reading as well, and none of those reads asked whether that commit is
# substituted.
#
# THE TWO SIDES GET DIFFERENT SCOPES BECAUSE THEY READ DIFFERENT THINGS, and
# giving the head the base's scope would have been a deterministic false
# refusal (Codex design review, section 52, [medium]; measured there at 70,526
# objects against 2,799). The base's reads include its HISTORY --
# `protocol_ever_added` and `history_is_complete` traverse the ancestry -- so
# its read set is the full object walk. The head is read only as a SNAPSHOT:
# `ls-tree` of the head commit's own tree, and nothing of its parents. So a
# replacement targeting an ancestor of the head cannot change one head-side
# answer, and `--no-walk` scopes the question to what is actually consulted.
#
# THE STATE THIS ADJUDICATES WAS CAPTURED AND PINNED FAR ABOVE, before either
# endpoint was resolved. `git replace -l` is a point-in-time read, so asking it
# HERE would leave the answer able to move behind the gate -- and, worse, would
# leave endpoint RESOLUTION itself running under substituted semantics, where
# the `HEAD~1` fallback base can be read out of a replaced head commit (Codex
# design review, section 52, [high]; sharpened by section 52 round 2). What
# runs below is therefore a test of the CAPTURED set against stored-object
# walks rather than a fresh query -- `REPLACE_STATE` and `REPLACED_LIST` are
# settled facts by the time this block is reached. They are settled, not
# assumed: the revalidation above re-reads the listing and re-peels every
# captured spec before any of it is used, and refuses if either moved.
#
# IT IS OPT-OUTABLE RATHER THAN A WEDGE. An operator who deliberately wants
# stored-object adjudication sets `GIT_NO_REPLACE_OBJECTS` themselves, which
# `GIT_NO_REPLACE_OBJECTS` is read as not-in-effect above, and the gate
# then adjudicates the objects a fresh CI clone would read -- replace refs are
# not fetched by default, so stored objects are what CI sees either way.
#
# Does any ACTIVE replacement change what this gate's reads of $1 return?
#
# REACHABILITY IS THE RIGHT TEST for every object type at once: a commit, a
# tree or a blob that the endpoint's own object walk reaches is an object some
# read below would resolve THROUGH the replacement. ONE walk answers for all of
# them, and when there are no replacements at all it costs nothing -- an empty
# `REPLACED_LIST` short-circuits before any walk is spawned, which is what
# keeps this free on every real run.
#
# NO TEMP FILE. The first cut wrote the walk to a file under TMP_DIR and
# re-scanned it once per replacement; this block now runs BEFORE TMP_DIR
# exists, and a file created here would outlive an interrupt taken before the
# cleanup trap is installed. Streaming it also removes the re-scan.
commit_objects_are_substituted() {  # $1 = commit, $2 = history|snapshot, $3 = 1 if $1 is itself replaced, $4 = candidate oids
    local _replaced _rc _hit _left _walk=()   # _replaced is the CAPTURED set, never a fresh query
    local LIST
    case "$2" in
        history)  _walk=() ;;
        snapshot) _walk=(--no-walk) ;;
        *) return 2 ;;   # an unrecognised scope word is not a question
    esac
    case "$REPLACE_STATE" in
        1) return 1 ;;   # git was not reading them: nothing is substituted
        2) return 2 ;;
    esac
    # THE ENDPOINT IS ANSWERED WITHOUT A WALK, and BOTH endpoints are taken out
    # of the candidate set rather than only this one. A commit is always
    # reachable from its own object walk, so a replacement naming it needs no
    # traversal to decide -- and this is the shape most likely to be present,
    # since substituting an endpoint is the whole point of the attack.
    #
    # Scanning for `$1` ALONE was not enough, and the residue was measurable: a
    # sole replacement naming the HEAD is not in the base's endpoint test, so
    # the base call fell through and walked the entire history (70,513 objects,
    # 184ms on this checkout) to find nothing, and only the head call that
    # followed short-circuited. Near the shared deadline that walk can spend the
    # budget and report an indeterminate BASE failure in place of the immediate
    # HEAD refusal it was about to reach (Codex perf, section 52, rounds 1 and
    # 5, [medium]). `REPLACED_NONENDPOINT` is what remains once both are
    # removed, so an endpoint-only replacement set spawns no walk on either
    # side.
    [ "$3" = "1" ] && return 0
    _replaced="$4"
    [ -n "$_replaced" ] || return 1
    LIST="$_replaced"; export LIST
    # No `setsid`, unlike the python phases below: those spawn children a bare
    # `timeout` could not reap, and `git rev-list` does not.
    #
    # AND IT ANSWERS TO THE SHARED DEADLINE, not to `remaining`'s one-second
    # floor. This function is called for BOTH endpoints, so the floor let a
    # second full object walk start after the budget was already spent -- the
    # same defect the capture loop was repaired for, one phase later (Codex
    # adversarial, section 52 round 15, [medium]).
    budget_left; _left="$BUDGET_LEFT"
    { [ "$_left" = "?" ] || [ "$_left" -le 0 ]; } && return 2
    # THE WHOLE PIPELINE IS SUPERVISED, not just the producer. `timeout` wraps
    # the command it is given, so wrapping only `git rev-list` left the awk
    # CONSUMER outside the deadline: killing git does not finish the command
    # substitution while the consumer holds the pipe, and the gate would hang
    # past its own budget (Codex adversarial, section 52 round 19, [medium]).
    # Wrapping the pipeline in one shell puts both under one bound.
    # `set -o pipefail` INSIDE the nested shell, because shell options do not
    # cross `bash -c`. Without it the pipeline reported awk's status, so a
    # FAILED `rev-list` with no hit came back rc 0 and the function answered
    # "not substituted" -- the replacement precondition failing OPEN, which is
    # the one direction it must never fail (Codex adversarial, section 52 round
    # 24, [high]).
    _hit="$(timeout -s KILL "$_left" bash -c '
                set -o pipefail
                git --no-replace-objects rev-list --objects "$@" 2>/dev/null \
                | awk -v list="$LIST" '"'"'
                    BEGIN { n = split(list, a, "\n")
                            for (i = 1; i <= n; i++) if (a[i] != "") want[a[i]] = 1 }
                    ($1 in want) { print "HIT"; exit }'"'"'
            ' _ ${_walk[@]+"${_walk[@]}"} "$1")"
    _rc=$?
    # A HIT IS CONCLUSIVE WHATEVER THE PRODUCER'S STATUS, and testing it FIRST
    # is what makes the early `exit` safe. awk leaving the stream closes the
    # pipe and SIGPIPEs git, which under `pipefail` is indistinguishable from a
    # real probe failure -- so the answer is read before the status, and the
    # status only decides the case where NOTHING was found. The sibling mistake
    # this file already paid for, an `END { exit 1 }` overriding a rule's
    # success, is gone with the END block (section 50; ordering per Codex perf,
    # section 52).
    [ "$_hit" = "HIT" ] && return 0
    [ "$_rc" -ne 0 ] && return 2
    return 1
}

# ONE PASS OVER THE CAPTURED SET, BEFORE EITHER SIDE IS ASKED. It answers both
# endpoints and leaves behind exactly the candidates that still need a walk.
BASE_ENDPOINT_REPLACED=0
HEAD_ENDPOINT_REPLACED=0
REPLACED_NONENDPOINT=""
while IFS= read -r _r; do
    [ -n "$_r" ] || continue
    case "$_r" in
        "$BASE_SHA")      BASE_ENDPOINT_REPLACED=1 ;;
        "$HEAD_RESOLVED") HEAD_ENDPOINT_REPLACED=1 ;;
        *) REPLACED_NONENDPOINT="$REPLACED_NONENDPOINT$_r"$'\n' ;;
    esac
done <<< "$REPLACED_LIST"

# THE CANDIDATE SETS ARE PER QUERY, because ENDPOINT ROLES ARE NOT EXCLUSIVE.
# Removing both endpoints from ONE shared residue looked symmetric and was
# wrong: this gate takes arbitrary `--base`/`--head`, and after a rewind the
# last-gated base can be a DESCENDANT of the head, so the head commit is an
# object the BASE's history walk reaches. With it discarded globally the base
# call answered "not substituted" without walking, and the range earned a base
# classification where the promised BASE_HISTORY_INCOMPLETE was due (Codex
# adversarial, section 52 round 6, [medium]).
#
# The head side needs no such care: its scope is `--no-walk` over the head
# commit's own tree, and a DIFFERENT commit can never appear in that listing,
# so dropping the base oid there is safe unconditionally.
#
# The base side decides with an ANCESTRY probe rather than an object walk --
# `merge-base --is-ancestor` is a commit-graph question and costs nothing
# beside the full `rev-list --objects` this exists to avoid. A provable
# non-ancestor is dropped; an ancestor, or a probe that could not answer, is
# kept and walked, so the cheap path is only taken where it is provably right.
BASE_CANDIDATES="$REPLACED_NONENDPOINT"
HEAD_CANDIDATES="$REPLACED_NONENDPOINT"
if [ "$HEAD_ENDPOINT_REPLACED" -eq 1 ]; then
    # BOUNDED, LIKE EVERY OTHER SPAWN IN THIS PHASE. It is cheap against a
    # healthy commit-graph and unbounded against a degraded one, and it sits
    # ahead of an ALREADY-KNOWN substitution refusal -- so a wedge here does
    # not merely cost time, it suppresses a verdict the gate had already
    # reached (Codex adversarial, section 52 round 15, [medium]).
    budget_left; _mb_left="$BUDGET_LEFT"
    if [ "$_mb_left" = "?" ] || [ "$_mb_left" -le 0 ]; then
        # Cannot ask, so cannot drop it: keep the candidate and let the walk
        # (which will refuse on the same spent budget) answer.
        BASE_CANDIDATES="$BASE_CANDIDATES$HEAD_RESOLVED"$'\n'
    else
        timeout -s KILL "$_mb_left" \
            git merge-base --is-ancestor "$HEAD_RESOLVED" "$BASE_SHA" >/dev/null 2>&1
        case "$?" in
            1) ;;   # provably unreachable from the base: not a candidate there
            *) BASE_CANDIDATES="$BASE_CANDIDATES$HEAD_RESOLVED"$'\n' ;;
        esac
    fi
fi

# THE BASE IS ASKED FIRST, so a base substitution still wins over a head one --
# the same base-first precedence the protocol classification below publishes.
commit_objects_are_substituted "$BASE_SHA" history "$BASE_ENDPOINT_REPLACED" "$BASE_CANDIDATES"
case "$?" in
    0) die_infra "BASE_HISTORY_INCOMPLETE: an active refs/replace entry substitutes an object reachable from the base $BASE_SHA, so the tree and history this gate would read are manufactured by repository metadata rather than stored at that commit, and no classification of them would describe the base. Remove the replacement or re-run against a clean clone" ;;
    2) die_infra "BASE_HISTORY_INCOMPLETE: could not determine whether repository replacement metadata substitutes anything reachable from the base $BASE_SHA, so whether its tree and history are the stored ones is unknown and no classification is being guessed from it" ;;
esac
# A SEPARATE TOKEN, NOT A BASE NAME REUSED. The base's is HISTORY_INCOMPLETE
# because a truncated or substituted ancestry is what invalidates its
# never-added inference. The head makes no such inference: what a substitution
# breaks there is the reading of the head commit's own tree, so the refusal
# says that instead of borrowing a history claim the head side never makes.
# RECORDED HERE, EMITTED AFTER THE BASE IS FINISHED. Dying here put a HEAD
# refusal ahead of every base-side classification, so a malformed base plus a
# substituted head reported the head and said nothing about the base --
# contradicting the base-first precedence this file's header publishes, and
# sending the operator to repair the wrong end of the range (Codex adversarial,
# section 52 round 2, [medium]). The ANSWER is safe to compute now: the
# captured replacement set and the pin above make it a stable fact rather than
# a fresh reading, so moving only the emission changes no verdict.
commit_objects_are_substituted "$HEAD_RESOLVED" snapshot "$HEAD_ENDPOINT_REPLACED" "$HEAD_CANDIDATES"
HEAD_SUBST_RC=$?

# THE EMPTY-RANGE EXIT COMES LAST OF THE PRECONDITIONS, and it took two
# findings to put it here. It compares the STORED resolutions, so:
#
#   - with a diverging head PEEL it was reached first and returned 0 --
#     "nothing to differentiate" about a pair the repository does not even
#     agree on, and the caller then advances its baseline over it. Caught by
#     fixture 22da's base leg, which resolved a replaced tag to the current
#     head and got rc 0 where BASE_OBJECTS_SUBSTITUTED was due;
#   - and sitting ABOVE the substitution adjudication it did the same thing for
#     a selfsame range: `--base X --head X` where X, or an object reachable
#     from it, is replaced does not move `rev-parse X^{commit}`, so the peels
#     agreed and a green rc 0 was returned over a commit git shows the operator
#     differently (Codex adversarial, section 52 round 10, [medium]).
#
# So every substitution question is settled before "there is nothing here to
# compare" is allowed to mean anything. A base fault has already died above; a
# head divergence merely SUPPRESSES the exit, so base-first still holds and the
# head refusal is emitted at its own site. The exit stays ABOVE every protocol
# read, which is the placement fixture 22ci pins.
if [ "$BASE_SHA" = "$HEAD_RESOLVED" ] && [ "$HEAD_PEEL_RC" -eq 1 ]; then
    log "base == head; nothing to differentiate."
    replacement_state_still_holds
    exit 0
fi

# ---------------------------------------------------------------------------
# Measure the closure here; DECIDE on it further down, after the protocol has
# been read and classified (section 51).
# ---------------------------------------------------------------------------
# The head side hashes the COMMITTED BLOB, not the working-tree file (section
# 54). It hashed the working tree until then, on the reasoning that an
# uncommitted resolver edit would otherwise early-exit unexamined -- but that
# reasoning had the subject wrong. An uncommitted edit is not in the range
# being pushed, so it is not this gate's to examine; what the gate must not do
# is certify a range from bytes that range does not carry. The one piece of
# uncommitted code that CAN decide a verdict, this script itself, is refused
# outright at the head site rather than measured here.
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
#   HEAD_/BASE_PROTOCOL_NOT_A_FILE      parsed through a symlink           (22ce)
#                                       -- BOTH sides, and the base one was
#                                       missing from this list: base-first
#                                       adjudication reaches it whenever a base
#                                       protocol file is a symlink whose link
#                                       text equals the head file's bytes
#   BASE_PROTOCOL_REMOVED               absent on both sides               (22cg)
#   BASE_PREDATES_PROTOCOL              absent, and never in this history
#   BASE_/HEAD_PROTOCOL_INCOMPLETE      part of a form, expressing neither
#
# What is NOT newly reachable is the ancient-base wedge section 50 was written
# to cure: a base predating the protocol hashes MISSING against a real blob, so
# it changes the closure and never arrives here at all.
CLOSURE_CHANGED=0
# CREATED BEFORE THE FIRST CLOSURE READING (section 55), because the batched
# probe below writes NUL-delimited `ls-tree` output to a file: bash DISCARDS NUL
# bytes in a command substitution, so a variable cannot carry that output and a
# non-NUL listing would C-QUOTE any path holding a tab, newline, quote or
# backslash. Nothing between the old creation site and here used TMP_DIR, and
# `cleanup` is installed further down in both orderings, so this is a pure move.
# BOUNDED AND CHECKED, LIKE EVERY OTHER LAUNCH. All three `mktemp` calls in
# this file ran unbounded and without a budget check, so degraded filesystem
# I/O -- which is exactly the condition the rest of this deadline machinery
# exists for -- could hold the gate past its advertised ceiling before a single
# python phase started (Codex adversarial, section 55 round 11, [medium]).
# `budget_left` rather than `remaining()`, so an expired budget refuses instead
# of buying a floored second.
budget_left
{ [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
    && die_infra "the gate's budget expired before it could create its own temporary directory, so nothing was started and no verdict is being guessed from it"
TMP_DIR="$(timeout --foreground -s KILL "$BUDGET_LEFT" mktemp -d -t identity-gate.XXXXXX)" \
    || die_infra "mktemp failed (or the gate's remaining budget expired while it ran)"
# AND A DESCRIPTOR ON IT, OPENED NOW, BEFORE ANY TREE CODE HAS RUN.
#
# A PATHNAME IS NOT AN IDENTITY. The narrow assembly below resolves everything
# it writes from strings, and a descendant that the base protocol probe detached
# can rename a directory the assembly just created and put a symlink in its
# place -- so a check that the path was created by this run stays true of a name
# that now refers to somewhere else, and `O_NOFOLLOW` on the leaf does not help
# when the PARENT was swapped (Codex adversarial, section 55 round 2, [high]).
# A descriptor cannot be swapped: it refers to the directory it was opened on,
# whatever later happens to the name. Every path the assembler touches is
# reached descriptor-relative from this one, so the whole chain is anchored to a
# directory that existed before the tree under test had executed anything.
# AND A CLEANUP EXISTS FROM THIS MOMENT, not from where the full one is
# installed. Moving the creation earlier widened the window in which TMP_DIR
# exists with no trap behind it: the descriptor open below, the hook-free
# directory's own failure paths, and both closure batches all run in it, so a
# refusal or a cancellation there leaked the directory outright. Calling that
# move "pure" was wrong -- it moved the creation without moving what protects
# it (Codex adversarial, section 55, [medium]). This minimal trap is replaced by
# the full `cleanup` once the state it needs exists; until then, removing the
# directory is the whole job.
trap 'timeout --foreground -s KILL 30 rm -rf "$TMP_DIR" 2>/dev/null || true' EXIT INT TERM  # fixed-allowance: an emergency trap that runs before the budget machinery is fully wired
exec {TMP_FD}<"$TMP_DIR" || die_infra "cannot hold a descriptor on the gate's own temporary directory, so the narrow materializations below could not be anchored and nothing is being guessed from this range"
# AND THE ANCHOR IS PROVED USABLE HERE, once, rather than at whichever phase
# boundary first consults it. `tmp_dir_anchor_ok` compares the name against the
# descriptor through `/proc/self/fd`, so a host where that does not resolve
# would make every later check fail identically to a real substitution -- an
# infrastructure limitation reported as an attack. Asking once at the anchor
# separates the two: a failure HERE says this host cannot support the check, and
# a failure later says the name stopped naming the directory.
tmp_dir_anchor_ok || die_infra "cannot verify the gate's own temporary directory through its descriptor on this host (/proc/self/fd did not resolve to $TMP_DIR), so a later substitution could not be distinguished from this limitation and no verdict is being guessed from it"

# EVERY CLOSURE MEMBER IN ONE PROBE PER COMMIT, NOT FOUR PER MEMBER (section 55).
#
# The two loops below asked, per member and per side: does the commit list it,
# how does it store it, what OID does it name, is that object present. On a
# ten-member closure that is roughly 70 `git` processes to decide a question
# whose answer is one tree listing. Section 51 declined to batch it and said why
# -- `-z` parsing is the subtle-probe-semantics surface every defect in this file
# has come from, and it was not worth adding to a stamp commit. It is worth
# adding to a commit whose whole subject is what this path costs.
#
# NO `-r`. A recursive listing EXPANDS a directory pathspec to its descendants,
# so a member replaced by a directory of the same name would be reported through
# its children and read as present -- the exact membership fault section 50
# round 3 fixed for the protocol paths. Without `-r` the directory arrives as its
# own `tree` entry and the type test refuses it.
#
# A FAILED BATCH IS NOT A NEGATIVE ANSWER, and it does not raise here either.
# `ENT_ERR` records that the question could not be asked, and each member then
# raises its OWN existing token at its OWN point in the loops below, so the
# base-first classification order and the token precedence this file publishes
# are exactly what they were (Codex design review, section 55).
#
# TWO READINGS, BY DIFFERENT PLUMBING, AND THAT IS DELIBERATE. `ENT_OID` comes
# from parsing the tree LISTING; `ENT_VOID` comes from resolving the path
# EXPRESSION `<commit>:<path>`, which is what the per-member `rev-parse` did
# before. Collapsing the two makes `CLOSURE_MOVED_UNDER_GATE` compare a value
# with itself -- which the first cut of this section did, while its comment
# claimed a cross-probe (Codex adversarial, section 55 round 1, [medium]). The
# measurement loop reads the expression, the decision loop reads the listing, so
# a mis-attribution in either parser surfaces as a disagreement instead of
# riding the exit.
ENT_ERR=0
ENT_MODE=(); ENT_TYPE=(); ENT_OID=(); ENT_PRESENT=(); ENT_OBJ=(); ENT_VOID=()
closure_entries() {   # $1 = commit, $2 = label
    local _z _rec _meta _path _i _mode _type _oid _oids _line
    ENT_ERR=0
    ENT_MODE=(); ENT_TYPE=(); ENT_OID=(); ENT_PRESENT=(); ENT_OBJ=(); ENT_VOID=()
    for _i in "${!CLOSURE[@]}"; do
        ENT_MODE[$_i]=""; ENT_TYPE[$_i]=""; ENT_OID[$_i]=""
        ENT_PRESENT[$_i]=0; ENT_OBJ[$_i]=0; ENT_VOID[$_i]=""
    done
    _z="$TMP_DIR/closure-$2.z"
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then ENT_ERR=1; return 0; fi
    timeout -s KILL "$BUDGET_LEFT" git ls-tree -z "$1" -- "${CLOSURE[@]}" >"$_z" 2>/dev/null \
        || { ENT_ERR=1; return 0; }
    while IFS= read -r -d '' _rec; do
        _meta="${_rec%%$'\t'*}"
        _path="${_rec#*$'\t'}"
        _mode="${_meta%% *}"
        _type="${_meta#* }"; _type="${_type%% *}"
        _oid="${_meta##* }"
        # MATCHED BY EXACT PATH. `ls-tree` emits in tree order, not argument
        # order, so position says nothing about which member an entry is.
        for _i in "${!CLOSURE[@]}"; do
            [ "${CLOSURE[$_i]}" = "$_path" ] || continue
            ENT_MODE[$_i]="$_mode"; ENT_TYPE[$_i]="$_type"
            ENT_OID[$_i]="$_oid"; ENT_PRESENT[$_i]=1
            break
        done
    done <"$_z"
    bounded_fs rm -f "$_z"
    # AND THE OBJECTS ARE PRESENT, not merely named. A tree entry resolves to an
    # OID from the TREE; in a partial clone the blob behind it can be absent, and
    # comparing OIDs would then certify bytes nothing in this repository can
    # produce. One `--batch-check` answers for every member at once.
    _oids=""
    for _i in "${!CLOSURE[@]}"; do
        [ "${ENT_PRESENT[$_i]}" -eq 1 ] || continue
        [ "${ENT_TYPE[$_i]}" = "blob" ] || continue
        _oids="$_oids${ENT_OID[$_i]}"$'\n'
    done
    [ -n "$_oids" ] || return 0
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then ENT_ERR=1; return 0; fi
    _z="$TMP_DIR/closure-$2.chk"
    printf '%s' "$_oids" | timeout -s KILL "$BUDGET_LEFT" \
        git cat-file --batch-check >"$_z" 2>/dev/null \
        || { bounded_fs rm -f "$_z"; ENT_ERR=1; return 0; }
    while IFS= read -r _line; do
        # "<oid> <type> <size>" for a present object, "<oid> missing" otherwise.
        case "$_line" in
            *" missing"|*" ambiguous") continue ;;
        esac
        _oid="${_line%% *}"
        for _i in "${!CLOSURE[@]}"; do
            [ "${ENT_OID[$_i]}" = "$_oid" ] && ENT_OBJ[$_i]=1
        done
    done <"$_z"
    bounded_fs rm -f "$_z"
    # THE SECOND, INDEPENDENT READING. One `--batch-check` over the path
    # EXPRESSIONS, in CLOSURE order, consumed POSITIONALLY and never matched
    # back by the OID it is meant to verify -- matching by OID would reintroduce
    # the very dependence this exists to break. It replaces the per-member
    # `rev-parse --quiet --verify <commit>:<path>` exactly: a line that does not
    # resolve leaves the entry empty, which the callers read as MISSING, which
    # is section 51's documented collapse and can only force a walk.
    _oids=""
    for _i in "${!CLOSURE[@]}"; do
        _oids="$_oids$1:${CLOSURE[$_i]}"$'\n'
    done
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then ENT_ERR=1; return 0; fi
    _z="$TMP_DIR/closure-$2.expr"
    printf '%s' "$_oids" | timeout -s KILL "$BUDGET_LEFT" \
        git cat-file --batch-check >"$_z" 2>/dev/null
    # `--batch-check` exits nonzero when ANY line failed to resolve, which is an
    # ordinary answer here, so the STATUS is not the error signal -- the line
    # count is. A truncated listing would otherwise be consumed positionally and
    # silently attribute every later member to the wrong entry.
    _i=0
    while IFS= read -r _line; do
        case "$_line" in
            *" missing"|*" ambiguous"|*"dangling"*) ;;
            *)
                _oid="${_line%% *}"
                case "$_oid" in
                    *[!0-9a-f]*|"") ;;
                    *) ENT_VOID[$_i]="$_oid" ;;
                esac
                ;;
        esac
        _i=$((_i + 1))
    done <"$_z"
    bounded_fs rm -f "$_z"
    if [ "$_i" -ne "${#CLOSURE[@]}" ]; then ENT_ERR=1; fi
    return 0
}

closure_entries "$BASE_SHA" base
B_ERR="$ENT_ERR"; B_MODE=("${ENT_MODE[@]}"); B_TYPE=("${ENT_TYPE[@]}")
B_OID=("${ENT_OID[@]}"); B_PRESENT=("${ENT_PRESENT[@]}"); B_OBJ=("${ENT_OBJ[@]}")
B_VOID=("${ENT_VOID[@]}")
closure_entries "$HEAD_RESOLVED" head
H_ERR="$ENT_ERR"; H_MODE=("${ENT_MODE[@]}"); H_TYPE=("${ENT_TYPE[@]}")
H_OID=("${ENT_OID[@]}"); H_PRESENT=("${ENT_PRESENT[@]}"); H_OBJ=("${ENT_OBJ[@]}")
H_VOID=("${ENT_VOID[@]}")

# The head-side hash of each member AS FIRST SEEN, kept so that "this member
# moved while the gate was working" stays a different question from "this
# member differs base-vs-head". Conflating them gave one token two meanings and
# one repair action that was wrong for half of them.
FIRST_H=()
for _i in "${!CLOSURE[@]}"; do
    f="${CLOSURE[$_i]}"
    # BOTH READINGS COME FROM THE BATCH ABOVE, and the MISSING collapse on a
    # failed probe is section 51's deliberate, documented choice -- it can only
    # force a walk, never a pass -- so it is preserved exactly, now applied to
    # the batch's failure rather than to two per-member spawns.
    #
    # THE HEAD READING COMES FROM THE COMMIT, SYMMETRICALLY WITH THE BASE
    # (section 54). This was `git hash-object "$REPO_ROOT/$f"` -- the LIVE
    # WORKING TREE -- so the measurement deciding whether the closure changed
    # described bytes that are not the ones being pushed. Reading the commit is
    # also what lets the head side be measured BEFORE any head worktree exists,
    # which is what keeps materialization deferred until every base
    # classification has cleared (Codex design review, section 54).
    if [ "$B_ERR" -eq 1 ] || [ -z "${B_VOID[$_i]}" ]; then b=MISSING; else b="${B_VOID[$_i]}"; fi
    if [ "$H_ERR" -eq 1 ] || [ -z "${H_VOID[$_i]}" ]; then h=MISSING; else h="${H_VOID[$_i]}"; fi
    FIRST_H[$_i]="$h"
    if [ "$b" != "$h" ]; then
        log "closure changed: $f"
        CLOSURE_CHANGED=1
    fi
done

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

# NO HOOKS RUN DURING A MATERIALIZATION (section 54, Codex adversarial [high]).
# `core.hooksPath` in this repository is the RELATIVE path `.githooks`, so it
# resolves INSIDE whatever worktree git is populating -- and `git worktree add`
# runs `post-checkout` once the files are there. A head commit carrying an
# executable `.githooks/post-checkout` would therefore execute arbitrary code
# from the tree under test, unbounded, before a single check had run, and could
# rewrite `todo/**` in the tree both resolver walks are about to read. The
# protocol and closure checks would not notice: they compare closure and
# protocol paths, not the corpus.
#
# An EMPTY DIRECTORY rather than a bogus path, so the setting is unambiguous
# whatever git does with a hooks path that does not exist. Both checkouts get
# it: the base has always had the same exposure, and fixing one while leaving
# the other would be a hole with a comment over it.
NOHOOKS_DIR="$TMP_DIR/nohooks"
budget_left
{ [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
    && die_infra "the gate's budget expired before it could create the hook-free directory for its checkouts"
timeout --foreground -s KILL "$BUDGET_LEFT" mkdir -p "$NOHOOKS_DIR" \
    || die_infra "cannot create the hook-free directory for the gate's checkouts (or the gate's remaining budget expired while it ran)"
BASE_TREE="$TMP_DIR/base"
# THE HEAD IS MATERIALIZED TOO (section 54), but NOT here -- the path is only
# named here so `cleanup` can reap it whatever point the run dies at. The
# checkout itself is deferred to the head site far below, after every base
# classification has had its chance, because a failed head checkout arriving
# ahead of a classifiable base token would report head machinery for a range
# whose real fault is the base (Codex design review, section 54).
HEAD_TREE="$TMP_DIR/head"
# THE NARROW TREES THE FAST PATH READS INSTEAD (section 55).
#
# Nothing before the byte-identical exit needs a repository. `proto_of` needs
# `scripts/todo-graph/` because it EXECUTES the protocol loader even on the JSON
# data path (it imports snapshot_protocol.py to bind the loader to the data, and
# ref_resolution.py + corpus_resolution_snapshot.py on the legacy path);
# `contract_of` needs cache_schema.py from the same directory; the bucket
# emission contract needs `scripts/lint/`. Measured on this repository:
# 27 files and 2.8 MB against 2,577 files and 87 MiB for a full checkout, and
# the gate materialized TWO of the latter before deciding it had nothing to
# differentiate. Most pushes do not touch the resolver closure, so that was the
# COMMON case paying for the rare one (section 51 measured the regression and
# declined to own it; this section owns the cost and never the verdicts).
#
# These are NOT registered worktrees. They are assembled by the gate from the
# named commit's blobs, so they take no `git worktree` administrative entry, no
# lock, and no prune -- `cleanup` removes them with TMP_DIR and nothing else has
# to know about them.
MIN_SUBTREES=("scripts/todo-graph" "scripts/lint")
BASE_MIN="$TMP_DIR/base-min"
HEAD_MIN="$TMP_DIR/head-min"
# AND THE ROOT EACH PHASE USES IS A DESCRIPTOR, NOT A NAME. Anchoring the
# verifier on TMP_DIR and reopening `head-min` beneath it closes a SYMLINK
# substitution and nothing else: tree code can rename the assembled root, put a
# clean real directory at that name, and go on operating from the renamed
# original. `O_NOFOLLOW` has no opinion about a different directory. The
# verifier would then hash the clean replacement while the result being consumed
# came from the original -- which is worse than not checking, because it
# certifies the wrong tree (Codex adversarial, section 55, [high]).
#
# So a descriptor is held on each root the moment it is assembled, and BOTH the
# phases that execute from it and the check that re-binds it address it through
# that descriptor. Execution and verification then cannot be talking about
# different directories, which is the property the check exists to have. The
# descriptor is read-only and points at a directory the tree is already
# executing from, so handing it across grants nothing it did not have.
BASE_MIN_ADDR=""
HEAD_MIN_ADDR=""
# WHICH LINKED TREES THIS RUN HAS ASKED GIT TO REGISTER. `cleanup` consults
# this rather than the filesystem, because a registration outlives its
# directory and the directory is exactly what an interrupted `add` leaves
# behind (Codex adversarial, section 54 round 2).
#
# AN ARRAY, NOT A SPACE-SEPARATED STRING. `mktemp -t` honours TMPDIR, so
# TMP_DIR can legitimately contain a space -- and a flat string then
# word-splits, so neither attempted path is ever tested intact and the
# confirmation below silently finds nothing (Codex adversarial, section 54
# round 5, [medium], measured).
WT_ATTEMPTED=()
WALK_PIDS=""
# Leaders in WALK_PIDS that have ALREADY been `wait`ed, so their pid is a
# number the kernel may have reissued. `cleanup` may still address their
# GROUP, guarded by a liveness check, but must never fall back to signalling
# the positive pid: that is how a recycled stranger gets killed (Codex
# adversarial, section 53 round 3).
WALK_WAITED=""
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
        # AND NEVER POSITIVELY, FOR AN UN-WAITED LEADER EITHER. The fallback
        # here used to signal the bare pid when the group signal failed, and
        # that is reachable on the ORDINARY path: `wait "$X"` and the
        # `WALK_WAITED="$X"` beside it are separate simple commands, so a
        # pending TERM can run this trap after the leader has been reaped and
        # before it is declared waited -- at which point the pid is released,
        # the group is gone, the group signal fails, and the positive fallback
        # lands on whoever the kernel reissued the number to (Codex adversarial,
        # section 56, [medium]).
        #
        # IT COSTS NOTHING TO DROP, because the fallback was for a shape this
        # file does not have. MEASURED: a plain background child is NOT its own
        # process-group leader, and every leader tracked in `WALK_PIDS` is
        # launched through `setsid` -- all eleven of them -- so `kill -- -$p` is
        # the complete route to a live one and the positive spelling could only
        # ever reach a stranger. `WALK_WAITED` therefore no longer selects
        # BETWEEN two signal shapes; it is what the reap helper and the grace
        # loop below still consult.
        for _p in $WALK_PIDS; do
            kill -0 "$_p" 2>/dev/null || kill -0 -- "-$_p" 2>/dev/null || continue
            kill -TERM -- "-$_p" 2>/dev/null || true
        done
        for _i in 1 2 3 4 5 6 7 8 9 10; do
            _alive=0
            for _p in $WALK_PIDS; do kill -0 "$_p" 2>/dev/null && _alive=1; done
            [ "$_alive" -eq 0 ] && break
            sleep 0.5  # launch-exempt: a fixed sub-second sleep does no I/O, and the loop re-checks the budget each turn
        done
        for _p in $WALK_PIDS; do
            kill -0 "$_p" 2>/dev/null || kill -0 -- "-$_p" 2>/dev/null || continue
            kill -KILL -- "-$_p" 2>/dev/null || true
        done
        for _p in $WALK_PIDS; do wait "$_p" 2>/dev/null || true; done
        WALK_PIDS=""
        WALK_WAITED=""
    fi
    # BOTH LINKED TREES, AND THE PRUNE RUNS ONCE (section 54). The head tree
    # is removed FIRST because it is the one the gate may still have been
    # executing code from; each removal keeps its own bound so a stalled one
    # cannot consume the other's.
    #
    # BOUNDED, AND ITS FAILURE IS NOT DISCARDED. The removal ran unbounded
    # with `|| true`, so a checkout that stalled on repository or
    # filesystem I/O -- exactly the case the creation timeout now catches
    # -- stalls AGAIN here and defeats the bound it was added to enforce.
    # A partially initialized worktree also leaves an administrative entry
    # registered after TMP_DIR is deleted, so a failed removal is pruned
    # rather than assumed harmless (Codex re-adversarial, section 51
    # review, [medium]). ONE prune covers both entries, so a second tree adds
    # a removal but no second fallback traversal.
    # DRIVEN BY WHAT WAS ATTEMPTED, NEVER BY WHAT IS STILL ON DISK. Skipping a
    # path because its directory is absent skipped the unlock, the removal AND
    # the prune -- so a registration whose directory had already vanished (an
    # interrupted `add`, or a module executed from the head tree deleting it)
    # survived forever, with its lock if it had taken one. Absence of the
    # directory is exactly the state prune exists for, so it must SET the flag
    # rather than clear it (Codex adversarial, section 54 round 2, [medium]).
    local _wt_list _wt_listed _wt_seen _wt_a
    _wt_prune=0
    for _wt in "$HEAD_TREE" "$BASE_TREE"; do
        _wt_seen=0
        for _wt_a in ${WT_ATTEMPTED[@]+"${WT_ATTEMPTED[@]}"}; do
            [ "$_wt_a" = "$_wt" ] && _wt_seen=1
        done
        [ "$_wt_seen" -eq 1 ] || continue
        if [ ! -d "$_wt" ]; then
            _wt_prune=1
            timeout --foreground -s KILL 60 git worktree unlock "$_wt" >/dev/null 2>&1 || true  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
            continue
        fi
        timeout --foreground -s KILL 60 \
            git worktree remove --force "$_wt" >/dev/null 2>&1 && continue  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
        # AND A LOCKED WORKTREE IS UNLOCKED AND RETRIED. `--force` overrides a
        # dirty tree, not a LOCK, and `prune` skips a locked entry too -- so a
        # single forced removal leaves it registered forever. A lock is
        # reachable from the tree under test, because until this cleanup was
        # hardened a `post-checkout` hook could take one (Codex adversarial,
        # section 54, [medium]).
        timeout --foreground -s KILL 60 git worktree unlock "$_wt" >/dev/null 2>&1 || true  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
        # `--force` TWICE, because once is not enough for a LOCKED tree -- git
        # requires the flag repeated for exactly that case. Relying on the
        # unlock above to have worked was the gap: its failure is discarded (it
        # legitimately fails when the tree was never locked), so a lock that
        # survived met a single `--force`, the removal failed, and `prune`
        # SKIPS a locked entry too. The registration then outlived every
        # collection route this gate or a later `git gc` offers, which made the
        # recovery note below untrue for that one case (Codex adversarial,
        # section 54 round 6, [medium]).
        timeout --foreground -s KILL 60 \
            git worktree remove --force --force "$_wt" >/dev/null 2>&1 \
            || { _wt_prune=1  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
                 log "NOTE: could not remove the linked worktree $_wt. If it is locked, no prune will collect it: run 'git worktree unlock $_wt' then 'git worktree remove --force --force $_wt'." ; }
    done
    if [ "$KEEP_TMP" -eq 0 ]; then
        # WHAT THE DESCRIPTOR NAMES, NEVER WHAT THE NAME SAYS. This removal is
        # the one place where trusting the NAME is irreversible: a descendant
        # the gate could not reap renames TMP_DIR and leaves something else
        # there, and `rm -rf "$TMP_DIR"` then destroys the replacement -- which
        # may be anything the invoking user can write -- while the directory
        # this run actually created leaks. `TMP_FD` has been held on that
        # directory since before any tree code ran, so `/proc/self/fd` resolves
        # to it wherever it now lives, and the mismatch is REPORTED rather than
        # silently repaired (Codex design review, section 56, [high]).
        #
        # THE ALREADY-GONE CASE IS SILENT, deliberately. `cleanup` runs twice on
        # a signal (the INT/TERM handler calls it, then `exit` fires the EXIT
        # trap), so on the second pass neither the name nor the descriptor
        # resolves -- there is nothing to remove and nothing to report, and a
        # NOTE there would fire on every ordinary cancellation.
        local _rm_target=""
        if [ -n "${TMP_FD:-}" ]; then
            # ONCE THE DESCRIPTOR HAS EXISTED, THE NAME IS NEVER A FALLBACK.
            # An earlier cut fell back to `$TMP_DIR` whenever the descriptor no
            # longer resolved, and that inverted the whole fix on the SECOND
            # cleanup pass: the INT/TERM handler calls `cleanup` and then
            # `exit`, which fires the EXIT trap and calls it again, so pass 1
            # removes the relocated anchored directory, and pass 2 -- finding
            # the descriptor pointing at something deleted -- deleted the
            # untrusted replacement sitting at the name, which is exactly the
            # destruction this block exists to prevent (Codex adversarial,
            # section 56, [high]). An unresolvable descriptor now means the
            # anchored directory is gone: there is nothing to remove, and the
            # silence is deliberate, because that is also the ORDINARY second
            # pass after a successful first one.
            if [ -e "/proc/self/fd/$TMP_FD" ]; then
                if tmp_dir_anchor_ok; then
                    _rm_target="$TMP_DIR"
                else
                    _rm_target="$(timeout --foreground -s KILL 30 readlink -f "/proc/self/fd/$TMP_FD" 2>/dev/null)"  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
                    if [ -n "$_rm_target" ] && [ -d "$_rm_target" ]; then
                        log "NOTE: the gate's temporary directory is no longer reached by the name it was created under. Removing what the descriptor still holds ($_rm_target) and leaving $TMP_DIR alone, because whatever is at that name now was not created by this run."
                    else
                        _rm_target=""
                        log "NOTE: the gate's temporary directory is no longer reached by the name it was created under ($TMP_DIR) and the descriptor could not be resolved to its current path, so nothing was removed. Whatever is at that name was not created by this run and is deliberately left alone."
                    fi
                fi
            fi
        elif [ -e "$TMP_DIR" ]; then
            # NO DESCRIPTOR AT ALL means the run failed before the anchor was
            # opened, so no tree code has executed and the name is all there is
            # -- the pre-section-55 behaviour, correct for exactly that window.
            _rm_target="$TMP_DIR"
        fi
        if [ -n "$_rm_target" ]; then
            # BOUNDED, because a recursive removal is the one cleanup step that
            # can block on a stalled filesystem -- the same condition every
            # other bound in this file exists for. `budget_left` rather than a
            # fixed number, and a failure is still tolerated: cleanup must never
            # be the thing that fails.
            budget_left
            if [ "$BUDGET_LEFT" != "?" ] && [ "$BUDGET_LEFT" -gt 0 ]; then
                timeout --foreground -s KILL "$BUDGET_LEFT" rm -rf "$_rm_target" 2>/dev/null || true
            else
                timeout --foreground -s KILL 30 rm -rf "$_rm_target" 2>/dev/null || true  # fixed-allowance: the expired-budget branch of cleanup, which by definition has none left
            fi
        fi
    else
        log "kept working files in $TMP_DIR"
    fi
    # THE PRUNE RUNS AFTER THE DIRECTORY IS GONE, and that ordering is the whole
    # point of it. `prune` only reaps entries whose directory is MISSING, so
    # running it first -- while a failed removal's directory was still there --
    # did nothing at all, and the `rm -rf` immediately below then created the
    # very stale entry nothing would ever collect (Codex adversarial, section
    # 54, [medium]). `--expire now` because the default expiry keeps a recent
    # entry that this gate knows for certain is dead. Skipped under
    # `--keep-tmp`, where the directories deliberately survive -- and skipped in
    # two further cases the block below adds, so this paragraph describes WHEN
    # the prune runs relative to the removal, not whether it runs at all.
    # AND THE PRUNE IS GATED ON OUR OWN ENTRY STILL BEING REGISTERED, because
    # `prune` has no path filter: it is a REPOSITORY-WIDE operation, so running
    # it whenever this gate merely suspects a leak can collect an unrelated
    # worktree whose storage happens to be unavailable at that moment. Asking
    # first costs one `list` and makes the sweep conditional on the leak this
    # run actually caused (Codex adversarial, section 54 round 3, [medium]).
    if [ "$_wt_prune" -eq 1 ] && [ "$KEEP_TMP" -eq 0 ]; then
        # CAPTURED WHOLE, THEN MATCHED IN THE SHELL -- never `list | grep -q`.
        # This file runs under `pipefail`, and `grep -q` exits the moment it
        # matches, so `git worktree list` takes SIGPIPE and the PIPELINE reports
        # failure precisely when the answer was YES. The confirmation would then
        # read a present registration as absent and skip the sweep, leaving the
        # exact debris this run created. The same expression also collapsed a
        # genuine list failure into "not listed" (Codex adversarial, section 54
        # round 4, [medium]).
        _wt_listed=0
        _wt_list="$(timeout --foreground -s KILL 60 git worktree list --porcelain 2>/dev/null)"  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
        if [ $? -ne 0 ]; then
            # UNANSWERABLE MEANS SAY SO, not sweep -- and the first version of
            # this branch had the asymmetry backwards. `prune` is
            # REPOSITORY-WIDE and destructive: git treats an unlocked worktree
            # whose storage is momentarily unavailable, a removable disk or a
            # network mount, as prunable, so sweeping on an unanswerable listing
            # can deregister somebody else's live worktree. What this gate might
            # have leaked is a stale administrative entry, which is inert and
            # which any later `git worktree prune` collects for free. Destroying
            # real state to avoid leaving benign state is the wrong trade (Codex
            # adversarial, section 54 round 5, [medium]).
            #
            # AND THAT COLLECTION IS NOT AUTOMATIC, which an earlier draft of
            # this comment claimed. A HEALTHY later run of this gate removes
            # both of its trees successfully, leaves `_wt_prune` at 0, and never
            # reaches the prune at all -- so it will not collect an entry an
            # earlier run left. The note below says so; the comment now agrees
            # with it (Codex consistency, section 54 post-ship, [low]).
            log "NOTE: could not enumerate linked worktrees during cleanup, so no prune was attempted. If this run left a stale entry, it persists until someone runs 'git worktree prune' -- a later run of this gate will not collect it."
        else
            for _wt in ${WT_ATTEMPTED[@]+"${WT_ATTEMPTED[@]}"}; do
                case $'\n'"$_wt_list"$'\n' in
                    *$'\n'"worktree $_wt"$'\n'*) _wt_listed=1 ;;
                esac
            done
        fi
        if [ "$_wt_listed" -eq 1 ]; then
            timeout --foreground -s KILL 60 \
                git worktree prune --expire now >/dev/null 2>&1 || true  # fixed-allowance: cleanup runs on the exit trap, after the budget is spent by definition
        fi
    fi
    # IDEMPOTENT, because this function runs TWICE on a signal: the INT and TERM
    # handlers call it and then `exit`, which fires the EXIT trap and calls it
    # again. With the attempt list still populated, that second pass saw its own
    # already-removed directories as absent and scheduled the repository-wide
    # prune even though the first pass had succeeded (Codex adversarial, section
    # 54 round 3, [medium]). Cleared last, so a cleanup interrupted part-way
    # still has its list on the next pass.
    WT_ATTEMPTED=()
}
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

# BOUNDED BY THE GLOBAL BUDGET, like every other long step. This is the single
# largest thing the gate does -- a full checkout of the base tree -- and it was
# the ONE step outside `remaining()`, so the advertised deadline did not
# constrain it and the CI job timeout was the only bound (Codex perf, section
# 51 review). Slimming or deferring the materialization itself is a design
# change tracked separately; bounding it is not.
# AND IT IS NOT LAUNCHED AT ALL PAST THE DEADLINE. `remaining` floors its
# answer at 1 because timeout(1) reads 0 as "no timeout", so an ALREADY
# EXPIRED budget still spawned a checkout with a one-second bound -- work
# started after the deadline, and a failure that reads as a broken repository
# rather than as an expired run. `budget_left` is the same clock without the
# floor, and `?` (an unreadable clock) counts as expired everywhere it is used
# (Codex design review, section 54).
# A CHECKOUT THAT RETURNED 0 IS NOT YET A FAITHFUL COPY OF THE COMMIT, and
# every read below assumes it is (section 54, Codex adversarial [high]). Two
# ways it can differ while git reports success:
#   - SPARSE or assume-unchanged entries, which are simply NOT WRITTEN. The
#     protocol and closure checks would still pass (those paths would be
#     present) while `todo/**` was thinned, and a corpus both walks cannot see
#     is a corpus both walks AGREE about -- a fail-open PASS over a shrunken
#     population, which is the exact assurance this gate exists to deny;
#   - anything that modified the tree after the write, which with hooks now
#     disabled should be nothing, but "should be nothing" is not a check.
# Asked of the tree itself rather than inferred from configuration, so a route
# nobody thought of still refuses. `ls-files -v` marks a skipped entry `S` and
# an assume-unchanged one with a lowercase letter; `H` is the ordinary case.
materialize_subtree() {   # $1 = commit, $2 = dest, $3 = side label, $4.. = path prefixes
    # ASSEMBLE THE NAMED COMMIT'S BLOBS, AND NOTHING ELSE (section 55).
    #
    # `git worktree add` gives four things this needs: bytes from the commit,
    # the entry's type, the entry's mode, and no interference from the working
    # tree. It also gives a repository, a registration, a lock, a prune
    # obligation, an index, hooks that must be disabled, and gitattributes that
    # can filter the very bytes about to be executed -- every one of which this
    # file has had to defend against, and none of which the protocol probe
    # wants. Reading the tree directly gives the first four and none of the
    # rest: one `ls-tree` says type and mode, one `cat-file --batch` says
    # content, no attribute is consulted (unlike `git archive`, whose
    # `export-ignore` could silently OMIT a protocol file and earn a confident
    # wrong classification), no smudge filter runs, and no hook exists to run.
    #
    # THE ASSEMBLY IS FAITHFUL, NOT NARROWED, and that distinction is the whole
    # of a design-review finding. The first draft refused every symlink and
    # submodule inside the subtree, which would have made a byte-identical range
    # that merely added an unrelated documentation symlink under
    # `scripts/todo-graph/` REFUSE where it passes today -- a verdict change, in
    # a section whose declared scope is cost alone (Codex design review, section
    # 55, [medium]; measured: all 21 entries under that path are 100644/100755
    # today, so the new refusal would have been latent and invisible). So every
    # entry is reproduced as what it IS -- a symlink as a symlink from its blob's
    # target text, a submodule as the empty directory a plain checkout leaves --
    # and the EXISTING type checks (`path_is_regular_in_worktree`,
    # `protocol_source_is_regular`, `path_is_regular_in_commit`) decide verdicts
    # exactly as before, on exactly the paths they already covered.
    #
    # A MISSING BLOB IS A REFUSAL, NOT AN ABSENT FILE. In a partial clone a tree
    # entry can name an object this repository does not have; writing nothing
    # there would hand the protocol checks a tree that looks incomplete and earn
    # the INCOMPLETE token, which is a statement about the COMMIT. It is not one
    # -- it is a statement about this repository -- so it refuses as
    # infrastructure, the same separation every other probe here keeps.
    # THE DESTINATION IS CREATED, NEVER ADOPTED, and that is not a detail --
    # it is the property `git worktree add` was providing for free. `mkdir -p`
    # SUCCEEDS on a path that already exists, INCLUDING a symlink to a
    # directory, and every write below then follows it. The base-side protocol
    # probe executes tree code before the head side is assembled, and that code
    # can derive TMP_DIR from its own `__file__` or from the exported
    # `PYTHONPYCACHEPREFIX`, so it could pre-plant `head-min` as a symlink to
    # the live repository and have the head assembly overwrite and chmod the
    # operator's working files. `add` refused an occupied destination, so
    # adopting one was a concrete weakening rather than a new risk (Codex
    # adversarial, section 55 round 1, [high]).
    #
    # AND THE CREATION ITSELF IS ANCHORED, which a `mkdir "$TMP_DIR/head-min"`
    # in this shell is not. That spelling resolves TMP_DIR by NAME, so a detached
    # descendant that renames TMP_DIR and leaves a symlink behind it redirects
    # the creation before the descriptor ever gets to refuse anything -- the
    # anchor would then be faithfully protecting a directory that is already the
    # wrong one (Codex adversarial, section 55 round 3, [high]).
    # `os.mkdir(dest_name, dir_fd=...)` is the same atomic test carried out
    # relative to the descriptor, so no name resolution is left in the creation
    # path at all.
    #
    # WHAT IS STILL NAME-BASED, SAID PLAINLY: every OTHER use of TMP_DIR in this
    # file -- the cache paths, the walk logs, the two full checkouts, `cleanup`
    # -- resolves it by name, and always has. This section did not introduce
    # that and does not close it; closing it is a change to how the whole file
    # addresses its own scratch space.
    # -> XREF: [`TODO-06 section 56`](#56-the-resolver-walks-leak-what-the-tree-spawns-on-the-path-where-they-succeed)
    #    owns what the tree spawns and what it can still reach.
    local _dest="$2" _side="$3" _commit="$1" _mat_pid _mat_rc _i
    shift 3
    local _destname="${_dest##*/}"
    budget_left
    { [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
        && die_infra "MATERIALIZATION_UNFAITHFUL: the gate's budget expired before the $_side side of $_commit could be materialized, so no bytes were read and no verdict is being guessed from it"
    # ITS OWN INTERPRETER, BOUNDED, RUNNING THE GATE'S CODE AND NEVER THE TREE'S.
    # Nothing below imports anything from the commit under test; it copies bytes.
    #
    # SUPERVISED ANYWAY, AND THE REASON IS NOT TRUST -- IT IS ARITHMETIC. An
    # earlier cut used `timeout --foreground`, on the ground that this spawns no
    # descendant of the TREE's choosing. That is true and it is the wrong
    # question: `--foreground` documents that it does not time out CHILDREN of
    # the command, and this command has two of its own -- `git ls-tree` and
    # `git cat-file --batch`. A KILL delivered to python while it is blocked
    # reading from a stalled `cat-file` orphans that git process, unrecorded and
    # unbounded, past the deadline this whole file advertises (Codex adversarial,
    # section 55 round 4, [medium]). Whose child it is has no bearing on whether
    # it outlives the bound.
    #
    # So it takes the shape sections 18, 50 and 53 already settled on: its own
    # session, backgrounded, `wait`ed for its status, recorded in `WALK_PIDS` so
    # a trap can reach it, and its GROUP reaped on the success path as well as
    # the failing one.
    setsid timeout -s KILL "$BUDGET_LEFT" \
        python3 - "$_commit" "$_destname" "$_destname.id" "$TMP_FD" "$@" <<'MATPY' &
import os, posixpath, subprocess, sys, pathlib

commit, dest_name, id_name, tmp_fd = (sys.argv[1], sys.argv[2], sys.argv[3],
                                      int(sys.argv[4]))
prefixes = sys.argv[5:]


# TWO FAILURE KINDS, TWO EXIT CODES, because they are two different statements
# and this file's whole discipline is not to collapse them. 2 means the
# REPOSITORY could not be asked what the commit contains, which is what
# `<SIDE>_TREE_UNREADABLE` has always meant; 1 means it answered and the answer
# could not be faithfully reproduced, which is `MATERIALIZATION_UNFAITHFUL`.
# Collapsing them would have re-blamed the tree under test for a question git
# refused to answer -- the exact fault fixtures 22ba and 22cc pin.
def fail(msg):
    sys.stderr.write(msg + "\n")
    raise SystemExit(1)


def cannot_ask(msg):
    sys.stderr.write(msg + "\n")
    raise SystemExit(2)


try:
    raw = subprocess.run(["git", "ls-tree", "-r", "-z", commit, "--", *prefixes],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
except OSError as exc:
    cannot_ask("could not run git ls-tree: %s" % exc)
if raw.returncode != 0:
    cannot_ask("git ls-tree failed for %s" % commit)

entries = []
for rec in raw.stdout.split(b"\0"):
    if not rec:
        continue
    # "<mode> <type> <oid>\t<path>", and the path is everything after the
    # first tab, so a path holding a space, quote or newline survives exactly.
    # `-z` emits raw bytes with no C-quoting, which is why it is used here.
    try:
        meta, path = rec.split(b"\t", 1)
        mode, typ, oid = meta.split(b" ")
    except ValueError:
        fail("unparsable ls-tree record from %s" % commit)
    entries.append((mode.decode(), typ.decode(), oid.decode(), path))

# NO ENTRY MAY ESCAPE THE DESTINATION. git itself refuses to record a `..`
# component or an absolute path in a tree, so this cannot fire on a tree git
# produced -- which is exactly why it is cheap to assert and unsafe to omit.
for _mode, _typ, _oid, path in entries:
    parts = pathlib.PurePosixPath(path.decode("utf-8", "surrogateescape")).parts
    if not parts or any(p in ("..", "") for p in parts) or path.startswith(b"/"):
        fail("refusing to write an entry that escapes the destination: %r" % path)

blobs = [(m, o, p) for (m, t, o, p) in entries if t == "blob" and m != "120000"]
links = [(o, p) for (m, t, o, p) in entries if t == "blob" and m == "120000"]
subs = [p for (m, t, o, p) in entries if t == "commit"]

# EVERY WRITE IS DESCRIPTOR-RELATIVE, AND NO PATHNAME IS RESOLVED TWICE.
#
# The destination is fresh and empty, so every directory under it is one this
# process makes -- but "this process made a directory of that NAME" is not the
# same claim as "that name still refers to it". A detached descendant can rename
# a created directory and leave a symlink behind it, and a name-keyed record
# would go on trusting it, while `O_NOFOLLOW` on the leaf says nothing about a
# swapped PARENT. So each directory is opened as it is created and the
# DESCRIPTOR is what is kept; a name swapped afterwards refers to somewhere this
# assembly simply never writes to again.
try:
    os.mkdir(dest_name, 0o700, dir_fd=tmp_fd)
except FileExistsError:
    fail("something already exists at the destination %s" % dest_name)
except OSError as exc:
    fail("cannot create the destination %s: %s" % (dest_name, exc))
try:
    root_fd = os.open(dest_name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                      dir_fd=tmp_fd)
except OSError as exc:
    fail("cannot open the destination %s: %s" % (dest_name, exc))

# THE ROOT'S IDENTITY IS RECORDED FOR THE PARENT TO CHECK.
#
# The parent opens this directory by NAME after this process exits, and between
# those two moments the name is substitutable: a detached descendant can rename
# the assembled root away and put a replacement at that name, and the parent
# would then bind both execution and verification to the replacement (Codex
# adversarial, section 55, [high]). A descriptor cannot be handed back across an
# exit, so the identity is written down instead and the parent refuses unless
# what it opened is this same directory.
#
# THAT RAISES THE BAR; IT DOES NOT CLOSE THE WINDOW, and an earlier version of
# this comment said it did. The reference record is itself reached by a
# pathname, so a descendant able to win the window can forge it too -- the
# comparison defeats a substitution that does not ALSO rewrite the record, and
# nothing more. `root_fd_is_the_assembly` states the same limit at the reading
# end, so the two ends of this channel agree about what it is worth.
# -> XREF: [`TODO-06 section 56`](#56-the-resolver-walks-leak-what-the-tree-spawns-on-the-path-where-they-succeed)
# CREATED HERE, EXCLUSIVELY AND DESCRIPTOR-RELATIVELY -- not opened by the shell.
#
# It was a `4>"$_dest.id"` redirection, and bash resolves that BEFORE `timeout`
# even starts. On the head side the base protocol probe has already executed
# commit-controlled code by then, and that code can derive TMP_DIR from
# `PYTHONPYCACHEPREFIX` and pre-create the predictable `head-min.id`: a FIFO
# there hangs the redirection past the gate deadline, and a symlink makes it
# TRUNCATE whatever the operator can write (Codex adversarial, section 55,
# [high]). That is a SYNCHRONOUS pre-plant by code the gate itself invoked --
# not the detached-descendant class section 56 owns -- so it is closed here
# rather than filed there.
#
# `O_EXCL` refuses anything already at the name, `O_NOFOLLOW` refuses a symlink,
# and `dir_fd` means the name is never resolved from this process cwd.
_st = os.fstat(root_fd)
try:
    _idfd = os.open(id_name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                    0o600, dir_fd=tmp_fd)
except OSError as exc:
    fail("refusing to write the root identity record %s: %s" % (id_name, exc))
os.write(_idfd, b"%d:%d\n" % (_st.st_dev, _st.st_ino))
os.close(_idfd)

dir_fds = {"": root_fd}


def dir_fd_for(rel):
    if rel in dir_fds:
        return dir_fds[rel]
    fd, cur = root_fd, ""
    for part in rel.split("/"):
        if not part:
            continue
        cur = posixpath.join(cur, part) if cur else part
        if cur in dir_fds:
            fd = dir_fds[cur]
            continue
        try:
            os.mkdir(part, dir_fd=fd)
        except FileExistsError:
            fail("refusing to write through %s, which this assembly did not create" % cur)
        except OSError as exc:
            fail("cannot create %s: %s" % (cur, exc))
        try:
            fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
        except OSError as exc:
            fail("cannot open %s after creating it: %s" % (cur, exc))
        dir_fds[cur] = fd
    return fd


for path in subs:
    # What a plain checkout leaves for an uninitialised submodule.
    dir_fd_for(os.fsdecode(path))

want = [o for (_m, o, _p) in blobs] + [o for (o, _p) in links]
content = {}
if want:
    try:
        proc = subprocess.Popen(["git", "cat-file", "--batch"],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL)
    except OSError as exc:
        cannot_ask("could not run git cat-file: %s" % exc)
    proc.stdin.write(("\n".join(want) + "\n").encode())
    proc.stdin.close()
    out = proc.stdout
    for oid in want:
        header = out.readline()
        if not header:
            fail("git cat-file ended before object %s" % oid)
        fields = header.split()
        if len(fields) != 3 or fields[1] != b"blob":
            # "<oid> missing" is a partial clone, not an absent path.
            fail("object %s for the %s side is not present in this repository "
                 "(git said: %s)" % (oid, commit, header.decode(errors="replace").strip()))
        size = int(fields[2])
        body = out.read(size)
        if len(body) != size:
            fail("short read for object %s" % oid)
        out.read(1)   # the trailing newline cat-file appends
        content[oid] = body
    proc.stdout.close()
    if proc.wait() != 0:
        cannot_ask("git cat-file --batch failed for %s" % commit)

for mode, oid, path in blobs:
    rel = os.fsdecode(path)
    pfd = dir_fd_for(posixpath.dirname(rel))
    name = posixpath.basename(rel)
    # O_EXCL, so a file planted at this name during the assembly is a refusal
    # rather than a target; O_NOFOLLOW, so a symlink planted there is not
    # followed even for one write; `dir_fd`, so neither question is asked of a
    # parent that may have been swapped since it was created.
    try:
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=pfd)
    except OSError as exc:
        fail("refusing to write %s: %s" % (rel, exc))
    with os.fdopen(fd, "wb") as fh:
        fh.write(content[oid])
        fh.flush()
        # THE MODE IS THE COMMIT'S, not the umask's. `[ -x ]` is asked of these
        # files later, and a checkout that lost the bit would answer differently
        # from every other checkout of the same commit. Applied THROUGH THE
        # DESCRIPTOR, which cannot be redirected between the write and the mode.
        os.fchmod(fh.fileno(), 0o755 if mode == "100755" else 0o644)

for oid, path in links:
    rel = os.fsdecode(path)
    pfd = dir_fd_for(posixpath.dirname(rel))
    name = posixpath.basename(rel)
    # A 120000 blob's CONTENT IS THE TARGET PATH TEXT. Reproducing it as a real
    # symlink is what keeps the type questions asked later answerable; writing
    # the text as a regular file is precisely the `core.symlinks=false`
    # emulation `tree_is_faithful` refuses for a checkout.
    #
    # NOT UNLINKED FIRST. The first cut removed anything already at the path and
    # then created the link, which is the same adopt-what-is-there fault the
    # destination check exists to refuse, one level down. `os.symlink` fails on
    # an occupied name, which is the answer wanted.
    try:
        os.symlink(os.fsdecode(content[oid]), name, dir_fd=pfd)
    except OSError as exc:
        fail("refusing to link %s: %s" % (rel, exc))
MATPY
    _mat_pid=$!
    WALK_PIDS="$_mat_pid"
    wait "$_mat_pid"
    _mat_rc=$?
    WALK_WAITED="$_mat_pid"
    # THE GROUP IS REAPED WHETHER OR NOT THE ASSEMBLY SUCCEEDED. A `cat-file
    # --batch` that outlived a completed python is the same orphan as one that
    # outlived a killed python. Guarded on the leader being gone AND the group
    # still answering, so a recycled pid is never signalled -- the discipline
    # section 53 established.
    # THE GROUP IS REAPED WHETHER OR NOT THE ASSEMBLY SUCCEEDED. A `cat-file
    # --batch` that outlived a completed python is the same orphan as one that
    # outlived a killed python.
    reap_walk_group "$_mat_pid"
    WALK_PIDS=""
    WALK_WAITED=""
    if [ "$_mat_rc" -eq 2 ]; then
        # A QUESTION GIT REFUSED TO ANSWER IS NEVER A STATEMENT ABOUT THE TREE.
        # `<SIDE>_TREE_UNREADABLE` is the published token for exactly this and
        # has been since section 50; the assembly reaching it first, before any
        # protocol classification, is the same ordering the probe it replaced
        # had (fixtures 22ba, 22cc).
        case "$_side" in
            base) die_infra "BASE_TREE_UNREADABLE: could not ask what the base $_commit contains -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it" ;;
            *)    die_infra "HEAD_TREE_UNREADABLE: could not ask what the head commit $_commit contains -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it" ;;
        esac
    fi
    if [ "$_mat_rc" -ne 0 ]; then
        die_infra "MATERIALIZATION_UNFAITHFUL: git said what the $_side side of $_commit contains, but the gate could not reproduce it (rc=$_mat_rc; an object this repository does not have, an entry it will not write, or the gate's remaining budget expiring while it ran), so the gate has no bytes it can attribute to that commit and no verdict is being guessed from it"
    fi
    budget_left
    { [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
        && die_infra "MATERIALIZATION_UNFAITHFUL: the gate's budget expired while assembling the $_side side of $_commit, so the materialization never finished and no verdict is being guessed from it"
    return 0
}

root_fd_is_the_assembly() {   # $1 = fd, $2 = side, $3 = dir (for the message)
    # WHAT THE PARENT OPENED SHOULD BE WHAT THE ASSEMBLER MADE -- and this
    # RAISES THE BAR rather than closing the hole, which is the honest statement
    # and replaces one that was not.
    #
    # The parent cannot create a directory relative to a descriptor from bash,
    # so it opens the root by name after the assembler exits, and that name is
    # substitutable in between. The assembler records its root's device and
    # inode and this compares them, which defeats a substitution that does not
    # also rewrite the record. It does NOT defeat one that does: an earlier
    # version of this comment claimed exploitation was DETECTED, and the
    # reference record is itself reached by a pathname, so a descendant that can
    # win the window can forge it too (Codex adversarial, section 55, [high]).
    # The record is read through the anchored TMP_DIR descriptor with
    # `O_NOFOLLOW`, which stops it being redirected by a symlink -- not a
    # descendant that simply overwrites it.
    #
    # THE ACTOR THIS NEEDS IS ONE THE GATE ALREADY CANNOT CONTAIN: a descendant
    # that DETACHED itself from the process group every phase here reaps. That
    # is a filed, owned limit, not a new one, and the only boundary a detached
    # child cannot leave is a cgroup killed as a unit -- a host decision, not a
    # shell change.
    # -> XREF: [`TODO-06 section 56`](#56-the-resolver-walks-leak-what-the-tree-spawns-on-the-path-where-they-succeed)
    local _fd="$1" _side="$2" _dir="$3"
    phase_budget "$_side root identity"
    timeout --foreground -s KILL "$PHASE_BUDGET" \
        python3 -c '
import os, sys
fd, idname, tmp_fd = int(sys.argv[1]), sys.argv[2], int(sys.argv[3])
try:
    # Through the anchor, no-follow: the record cannot be redirected by a
    # symlink planted at its name. It can still be overwritten by whatever can
    # win the window, which is stated above rather than papered over.
    rfd = os.open(idname, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=tmp_fd)
    with os.fdopen(rfd) as rf:
        want = rf.read().strip()
except OSError as exc:
    sys.stderr.write("cannot read the recorded root identity: %s\n" % exc)
    raise SystemExit(2)
st = os.fstat(fd)
got = "%d:%d" % (st.st_dev, st.st_ino)
if got != want:
    sys.stderr.write("root identity %s is not the assembled %s\n" % (got, want))
    raise SystemExit(1)
' "$_fd" "${_dir##*/}.id" "$TMP_FD"
    case "$?" in
        0) return 0 ;;
        2) die_infra "could not establish whether the $_side tree the gate is holding is the one it assembled, so nothing is being guessed from this range" ;;
        *) die_infra "MATERIALIZATION_UNFAITHFUL: the directory the gate is holding for the $_side side is NOT the one it assembled from that commit -- something replaced it between the assembly finishing and the gate taking hold of it, so every phase that would read it would be reading a tree the gate never built" ;;
    esac
}

min_tree_still_matches() {   # $1 = assembled dir, $2 = commit, $3 = side label
    # THE ASSEMBLED TREE IS RE-BOUND TO ITS COMMIT AFTER ANYTHING RUNS FROM IT,
    # THROUGH THE SAME DESCRIPTOR THE ASSEMBLY USED.
    #
    # Section 55 took the closure reading from the commit's own tree entry and
    # removed the `hash-object` over the materialized tree -- and that
    # `hash-object` was the ONLY thing tying the bytes the gate EXECUTES to the
    # commit it names. Both readings became commit plumbing, so they agree by
    # construction and notice nothing about the directory in between, while
    # `proto_of`, the bucket-emission checker and `contract_of` all execute
    # modules from that directory (Codex adversarial, section 55, [high]).
    #
    # THREE VERSIONS, AND THE FIRST TWO WERE WEAKER IN WAYS THAT MATTER:
    #   * one filtered its comparison set by what the directory CURRENTLY held,
    #     so a member replaced by a symlink or deleted outright was silently
    #     omitted -- it passed over exactly the substitution it exists to catch;
    #   * the next took the set from the commit but still resolved every path
    #     afresh BY NAME, so a detached descendant could rename an assembled
    #     ancestor and leave a symlink to a clean mirror, and the check verified
    #     the mirror while the result being consumed came from the original.
    # A pathname is not an identity -- the same lesson the assembler learned,
    # relearned by its own verifier.
    #
    # So this walks descriptor-relatively from the anchor held on TMP_DIR, with
    # `O_NOFOLLOW` at every component, and hashes bytes read from the opened
    # descriptor rather than from a path. It computes git's blob hash itself,
    # which also means the whole check is one process rather than a `hash-object`
    # spawn on top of it.
    local _dir="$1" _commit="$2" _side="$3" _rootfd="$4"
    local _unreadable="HEAD_TREE_UNREADABLE"
    [ "$_side" = "base" ] && _unreadable="BASE_TREE_UNREADABLE"
    phase_budget "$_side assembled-tree re-binding"
    setsid timeout -s KILL "$PHASE_BUDGET" \
        python3 - "$_commit" "$_rootfd" "${CLOSURE[@]}" "${PROTOCOL_PATHS[@]}" <<'REBINDPY' &
import hashlib, os, subprocess, sys

commit, root = sys.argv[1], int(sys.argv[2])
# Duplicates are harmless but wasteful; the closure and protocol lists overlap.
wanted, seen = [], set()
for p in sys.argv[3:]:
    if p not in seen:
        seen.add(p)
        wanted.append(p)


def cannot_ask(msg):
    sys.stderr.write(msg + "\n")
    raise SystemExit(2)


def unfaithful(msg):
    sys.stderr.write(msg + "\n")
    raise SystemExit(1)


try:
    raw = subprocess.run(["git", "ls-tree", "-z", commit, "--", *wanted],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
except OSError as exc:
    cannot_ask("could not run git ls-tree: %s" % exc)
if raw.returncode != 0:
    cannot_ask("git ls-tree failed for %s" % commit)

expect = {}
for rec in raw.stdout.split(b"\0"):
    if not rec:
        continue
    try:
        meta, path = rec.split(b"\t", 1)
        mode, _typ, oid = meta.split(b" ")
    except ValueError:
        cannot_ask("unparsable ls-tree record from %s" % commit)
    expect[os.fsdecode(path)] = (mode.decode(), oid.decode())

# THE ROOT IS THE INHERITED DESCRIPTOR, never reopened by name. Reopening it --
# even `O_NOFOLLOW` beneath an anchored TMP_DIR -- would let a rename plus a
# clean real directory at the same name send this check to a mirror while the
# consumed result came from the original.


def walk(rel):
    """Return (parent_fd, leaf) reached with no-follow at every component."""
    parts = rel.split("/")
    fd = root
    opened = []
    try:
        for part in parts[:-1]:
            nxt = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=fd)
            opened.append(nxt)
            fd = nxt
        return fd, parts[-1], opened
    except OSError:
        for x in opened:
            os.close(x)
        return None, parts[-1], []


def blob_hash(data):
    h = hashlib.sha1()
    h.update(b"blob %d\0" % len(data))
    h.update(data)
    return h.hexdigest()


for rel in wanted:
    want = expect.get(rel)
    pfd, leaf, opened = walk(rel)
    try:
        if pfd is None:
            if want is None:
                continue
            unfaithful("%s cannot be reached in the assembled tree without "
                       "following something that is not a directory" % rel)
        try:
            st = os.stat(leaf, dir_fd=pfd, follow_symlinks=False)
        except FileNotFoundError:
            if want is None:
                continue
            unfaithful("%s is carried by %s but is missing from the assembled "
                       "tree -- something removed it after the gate read it" % (rel, commit))
        except OSError as exc:
            # NOT `cannot_ask`. That maps to `<SIDE>_TREE_UNREADABLE`, whose
            # published meaning is that GIT could not answer -- so a permissions
            # change or a filesystem error after git answered perfectly well
            # sent the operator to repair a repository that was never broken
            # (Codex consistency, section 55, [medium]). Failing to inspect the
            # materialization is a materialization failure.
            unfaithful("could not stat %s in the assembled tree: %s" % (rel, exc))
        if want is None:
            unfaithful("%s is not carried by %s, but the assembled tree now holds "
                       "it -- something created it after the gate read the tree" % (rel, commit))
        mode, oid = want
        import stat as _stat
        if mode in ("100644", "100755"):
            if not _stat.S_ISREG(st.st_mode):
                unfaithful("%s stores %s as a regular file but the assembled tree now "
                           "holds something else there -- something replaced it after "
                           "the gate read it, and a later phase would execute or parse "
                           "whatever that is" % (commit, rel))
            try:
                fd = os.open(leaf, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=pfd)
            except OSError as exc:
                unfaithful("could not open %s in the assembled tree: %s" % (rel, exc))
            with os.fdopen(fd, "rb") as fh:
                got = blob_hash(fh.read())
            if got != oid:
                unfaithful("%s in the assembled tree no longer matches %s (%s became %s) "
                           "-- something changed it after the gate read it, so every check "
                           "that has already passed describes bytes that are no longer there"
                           % (rel, commit, oid, got))
        elif mode == "120000":
            if not _stat.S_ISLNK(st.st_mode):
                unfaithful("%s stores %s as a symlink but the assembled tree no longer "
                           "holds one there" % (commit, rel))
            got = blob_hash(os.readlink(leaf, dir_fd=pfd).encode())
            if got != oid:
                unfaithful("the symlink %s in the assembled tree no longer points where "
                           "%s says (%s became %s)" % (rel, commit, oid, got))
    finally:
        for x in opened:
            os.close(x)
REBINDPY
    local _rb_pid=$!
    WALK_PIDS="$_rb_pid"
    wait "$_rb_pid"
    local _rb_rc=$?
    WALK_WAITED="$_rb_pid"
    # THE SUCCESS PATH REAPS HERE TOO: the re-binding probe imports tree code,
    # and a module that starts a child and returns normally leaves it behind.
    reap_walk_group "$_rb_pid"
    WALK_PIDS=""
    WALK_WAITED=""
    case "$_rb_rc" in
        0) return 0 ;;
        # A FAILED PROBE IS NOT AN UNFAITHFUL TREE -- the distinction fixture
        # 22bq pins one level up, and which this check got wrong on its first
        # run by reporting a git failure as a bad directory.
        2) die_infra "$_unreadable: could not establish whether the $_side tree assembled for $_commit still holds that commit's bytes -- the repository or the probe, not the tree, is what failed here" ;;
        *) die_infra "MATERIALIZATION_UNFAITHFUL: the $_side tree assembled for $_commit no longer holds that commit's bytes (rc=$_rb_rc; the detail is on stderr above), so every check that has already passed describes bytes that are no longer there" ;;
    esac
}

tree_is_faithful() {   # $1 = tree, $2 = commit, $3 = side label
    local _odd _dirty
    _odd="$(bounded_git -C "$1" ls-files -v)" \
        || die_infra "MATERIALIZATION_UNFAITHFUL: could not ask the $3 checkout of $2 which of its entries were actually written, so whether the gate is reading that commit is unknown and no verdict is being guessed from it"
    _odd="$(printf '%s\n' "$_odd" | grep -cv '^H ' || true)"  # launch-exempt: filters a shell variable already in memory
    if [ "${_odd:-0}" -ne 0 ]; then
        die_infra "MATERIALIZATION_UNFAITHFUL: the $3 checkout of $2 has $_odd entr(y/ies) git did not write normally (skip-worktree or assume-unchanged), so the gate would adjudicate fewer bytes than that commit carries. Clear sparse-checkout and assume-unchanged settings, or run the gate from a repository that has none"
    fi
    _dirty="$(bounded_git -C "$1" status --porcelain)" \
        || die_infra "MATERIALIZATION_UNFAITHFUL: could not ask the $3 checkout of $2 whether it still matches that commit, so no verdict is being guessed from it"
    if [ -n "$_dirty" ]; then
        die_infra "MATERIALIZATION_UNFAITHFUL: the $3 checkout of $2 differs from that commit immediately after being created, so something modified it between the checkout and this check and the gate is not reading the commit it names"
    fi
    # SYMLINK EMULATION IS THE ONE UNFAITHFULNESS GIT CALLS CLEAN. With
    # `core.symlinks` false, a committed 120000 entry is written as a small
    # REGULAR file holding the target path text, deliberately and without
    # complaint -- so both probes above pass while an entry's TYPE is wrong, and
    # every type question this gate asks of the filesystem gets the wrong answer
    # (Codex adversarial, section 54 round 2, [high]). Asked directly, and only
    # of the entries where it can apply: a tree whose commit stores no symlinks
    # pays one `ls-files` and stops, which is every tree in this repository
    # today.
    # CAPTURED, THEN CHECKED, THEN WALKED -- never `probe | filter` in one
    # substitution. A `die_infra` inside a pipeline runs in a SUBSHELL and
    # cannot end this script, so a failed probe there would print a refusal and
    # let execution carry on: the same lost-tri-state fault this file has
    # already fixed several times, in a new place.
    #
    # NUL-DELIMITED, AND PARSED IN THE SHELL RATHER THAN THROUGH `grep | cut`.
    # Two faults in one line otherwise (Codex adversarial, section 54 round 3,
    # [medium]): a trailing `|| true` cannot tell grep's ordinary "no symlinks
    # here" from grep or cut actually FAILING, so a broken filter produced an
    # empty inventory and every emulated symlink walked through; and non-NUL
    # `ls-files` output C-QUOTES a path containing a tab, newline, quote or
    # backslash, so such a path would be tested under its quoted spelling and
    # refused for existing. `-z` emits raw bytes and no quoting at all.
    # THROUGH A FILE, NEVER THROUGH A VARIABLE. Bash DISCARDS NUL bytes in a
    # command substitution, so capturing `-z` output into a variable -- which is
    # exactly what `bounded_git` does internally -- silently concatenates every
    # record into one. The first draft did that and the check stopped firing
    # while still looking correct: the fixture caught it, nothing else would
    # have. A file preserves the delimiters AND lets the probe's exit status be
    # tested before anything is parsed, which is the property the pipeline this
    # replaced could not offer either.
    local _rec _mode _path _z
    _z="$TMP_DIR/staged-$3.z"
    budget_left
    { [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
        && die_infra "MATERIALIZATION_UNFAITHFUL: the gate's budget expired before it could ask the $3 checkout of $2 how it stores its entries, so the question was never asked and no verdict is being guessed from it"
    timeout -s KILL "$BUDGET_LEFT" git -C "$1" ls-files -s -z >"$_z" 2>/dev/null \
        || die_infra "MATERIALIZATION_UNFAITHFUL: could not ask the $3 checkout of $2 how it stores its entries, so whether the gate can read that commit faithfully is unknown and no verdict is being guessed from it"
    # THE PARSE AND THE STATS ARE INSIDE THE DEADLINE, not merely followed by a
    # check that one was exceeded. `timeout` bounds the git PRODUCER only; the
    # record loop and the per-symlink stat run in this shell, so on a tree with
    # many symlinked entries the advertised ceiling could be passed and only
    # noticed afterwards (Codex perf, section 54 post-ship, [medium]). Checked
    # per symlink rather than per record, because the record loop is a
    # parameter expansion over an already-materialized string while the stat is
    # the filesystem call that can actually block.
    while IFS= read -r -d '' _rec; do
        _mode="${_rec%% *}"  # launch-exempt: filters a shell variable already in memory, so nothing here opens a file or can block
        [ "$_mode" = "120000" ] || continue
        budget_left  # launch-exempt: filters a shell variable already in memory, so nothing here opens a file or can block
        { [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
            && die_infra "MATERIALIZATION_UNFAITHFUL: the gate's budget expired while checking whether the $3 checkout of $2 represents that commit's symlinks faithfully, so the question was never finished and no verdict is being guessed from it"
        # <mode> <oid> <stage>TAB<path>, and the path is everything after the
        # first tab, so a path containing spaces or quotes survives exactly.
        _path="${_rec#*$'\t'}"
        [ -n "$_path" ] || continue
        [ -L "$1/$_path" ] && continue
        die_infra "MATERIALIZATION_UNFAITHFUL: the commit $2 stores $_path as a symlink, but the $3 checkout wrote it as an ordinary file -- this host cannot represent symlinks (core.symlinks is false), so the gate cannot materialize that commit faithfully, and every type question it would then ask of the filesystem would get an answer no symlink-capable checkout, and no CI run, would give"
    done <"$_z"
    # THE WALK IS INSIDE THE DEADLINE TOO. It is a filesystem stat per symlink,
    # which is nothing on this corpus and unbounded in principle.
    # NO PATH THAT EXECUTES OR PARSES MAY BE FILTERED. A checkout applies
    # smudge and working-tree-encoding filters, so the bytes bash and python
    # actually run can differ from the blob the commit stores -- and every
    # check here would still report clean, because `hash-object --path` and
    # `status` apply the matching CLEAN filter and round-trip back to that same
    # blob. Disabling hooks does not disable filters (Codex adversarial,
    # section 54 post-ship, [high]). It is the same shape as the symlink
    # emulation above: content faithful by git's measure, different by the only
    # measure that matters here.
    #
    # ASKED OF THE PATHS WHOSE BYTES DECIDE THE VERDICT, not of the whole tree.
    # `check-attr` over the closure plus the protocol paths is one process and
    # covers everything this gate executes or parses. The CORPUS is deliberately
    # NOT covered: it is read as data by both sides equally, so a filter there
    # cannot make the two walks disagree, and refusing on it would wedge any
    # repository that legitimately normalises its markdown.
    local _attr
    _attr="$(bounded_git -C "$1" check-attr filter text eol working-tree-encoding -- \
                "${CLOSURE[@]}" "${PROTOCOL_PATHS[@]}")" \
        || die_infra "MATERIALIZATION_UNFAITHFUL: could not ask the $3 checkout of $2 whether any verdict-affecting path is filtered, so whether the gate would execute that commit's own bytes is unknown and no verdict is being guessed from it"
    _attr="$(printf '%s\n' "$_attr" | grep -v ': \(unspecified\|unset\)$' || true)"  # launch-exempt: filters a shell variable already in memory
    if [ -n "$_attr" ]; then
        local _attr_excerpt
        _attr_excerpt="$(printf '%s' "$_attr" | tr '\n' ';' | cut -c1-300)"  # launch-exempt: reformats a shell variable already in memory
        die_infra "MATERIALIZATION_UNFAITHFUL: a checkout filter applies to a verdict-affecting path in the $3 tree of $2, so the bytes this gate would execute or parse are not the bytes that commit stores: $_attr_excerpt. Clear the filter, text, eol or working-tree-encoding attribute for the resolver closure and the protocol paths"
    fi
    budget_left
    { [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; } \
        && die_infra "MATERIALIZATION_UNFAITHFUL: the gate's budget expired while checking whether the $3 checkout of $2 represents that commit faithfully, so the question was never answered and no verdict is being guessed from it"
    return 0
}

# THE BASE SIDE IS ASSEMBLED NARROW HERE AND CHECKED OUT IN FULL ONLY IF THE
# DIFFERENTIAL IS ACTUALLY GOING TO RUN (section 55). Everything between here
# and the byte-identical exit reads the protocol and the closure, both of which
# live under the subtrees below; the corpus is not read until a walk needs it.
materialize_subtree "$BASE_SHA" "$BASE_MIN" base "${MIN_SUBTREES[@]}"
exec {BASE_MIN_FD}<"$BASE_MIN" \
    || die_infra "cannot hold a descriptor on the base tree just assembled, so the phases that read it and the check that re-binds it could not be tied to one directory"
root_fd_is_the_assembly "$BASE_MIN_FD" base "$BASE_MIN"
BASE_MIN_ADDR="/proc/self/fd/$BASE_MIN_FD"

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
# AND IT REFUSES ON AN EXPIRED BUDGET, like every other phase. This was built on
# `remaining()`, which floors to 1 -- correct as a `timeout` ARGUMENT, wrong as a
# DECISION -- so the protocol probe could START after the global deadline had
# passed and then execute tree-controlled imports, which is the one phase where
# starting late is least defensible. It was the last floor-as-a-decision left in
# the file after section 55 gave the other seven `phase_budget` (Codex
# adversarial, section 55 round 8, [medium]).
#
# ANSWERS IN A VARIABLE, NOT ON STDOUT, because a function that can refuse must
# not be called from a command substitution: `die_infra` inside one runs in a
# SUBSHELL and cannot end this script, so the refusal would print and execution
# would carry on -- the lost-tri-state fault this file has already fixed several
# times, in a new place.
PROBE_BUDGET=""
probe_budget() {   # $1 = what it is about to do; ANSWERS in PROBE_BUDGET
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then
        die_infra "the gate's budget expired before it could $1, so nothing was started and no verdict is being guessed from it"
    fi
    if [ "$BUDGET_LEFT" -gt "$PROTOCOL_PROBE_CAP_SECS" ]; then
        PROBE_BUDGET="$PROTOCOL_PROBE_CAP_SECS"
    else
        PROBE_BUDGET="$BUDGET_LEFT"
    fi
}

proto_of() {  # $1 = tree root; ANSWERS IN $PROTO_RESULT, never on a path
    # THE RESULT DOES NOT TRAVEL THROUGH A NAMED FILE, and this is the
    # SECOND channel this section had to close in its own fix. Round 2
    # replaced the tree-writable pid file with a redirection into
    # `$TMP_DIR/<side>-proto.txt` -- and the BASE probe runs tree code
    # BEFORE the head redirection is opened, so it could derive TMP_DIR
    # from the exported `PYTHONPYCACHEPREFIX` and pre-plant that name as a
    # symlink to any file this user can write, which `>` then TRUNCATES
    # (Codex adversarial, section 53 round 3, [high]). A caller variable
    # has no name in the filesystem, so there is nothing to pre-plant.
    PROTO_RESULT=""
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
    local _rec _prc _prb _i
    probe_budget "create the file its protocol record travels on"
    _rec="$(timeout --foreground -s KILL "$PROBE_BUDGET" mktemp "$TMP_DIR/proto.XXXXXX" 2>/dev/null)" \
        || { PROTO_RESULT=TRANSPORT; return 0; }
    # THE RECORD'S ADDRESS IS NEVER HANDED TO THE TREE. Passing the path as
    # argv[2] told the very code being imported where the gate's own answer
    # lived, and an `atexit` handler registered during import could replace a
    # finished record with a valid-looking one (Codex adversarial, section 50
    # round 19). The file is opened by the SHELL on fd 3, so no path reaches
    # the subprocess, and argv is scrubbed before any tree code runs.
    # SUPERVISED, NOT FOREGROUND -- and the premise that made it foreground was
    # false (section 53). Section 50 round 24 kept the probe inside the gate's
    # own process group so a TERM to the gate would reach it, on the stated
    # ground that "the walk phases need `setsid` because they spawn children a
    # bare `timeout` could not reap; this probe spawns none". This probe
    # IMPORTS THE TREE UNDER TEST, so what it spawns is whatever that tree's
    # code spawns, and `--foreground`'s documented caveat -- children of the
    # command are not timed out -- then applies to a process the gate does not
    # control. Measured on the previous shape: a loader that starts one
    # descendant and returns leaves that descendant running after the probe is
    # killed at its budget, after `cleanup` completes, and after the gate has
    # exited (`sleep 300` still live, in the gate's own now-dead process
    # group). It is the ONLY unbounded leak here; the probe itself is always
    # bounded by `timeout -s KILL`.
    # SO THE PROBE JOINS THE SAME SUPERVISION THE WALK PHASES USE: its own
    # session via `setsid`, launched in the background, `wait`ed for its
    # status, and reaped by `cleanup` through the TERM -> escalate -> KILL loop
    # that already exists there. That RESTORES round 24's property rather than
    # reversing it: `wait` is interruptible, so a TERM to the gate now runs the
    # trap IMMEDIATELY instead of being deferred until the foreground command
    # returns, and `cleanup` then signals the probe's whole group. The old
    # shape did not even manage prompt cancellation -- a TERM-ignoring loader
    # held the gate for the remainder of the probe budget (measured: TERM at
    # t=3s against a 12s cap, gate exited at t=12s) because bash cannot run a
    # trap while it is blocked in a foreground command.
    # THE PID TRAVELS BY FILE, NOT BY VARIABLE. `proto_of` is called inside a
    # command substitution, so it runs in a subshell and anything it assigns to
    # `WALK_PIDS` is discarded with that subshell -- `cleanup` runs in the
    # parent and would see nothing. TMP_DIR is shared on disk, so the pid goes
    # there and both sides can see it.
    # A SMALL SHARE OF THE BUDGET, not all of it. Reading a handful of
    # constants is constant-size work, but the probe was granted the entire
    # remaining budget -- so one stalled import could spend the whole 600s and
    # leave the producer, cache and corpus phases nothing (Codex perf, section
    # 50 post-commit review). The cap is the SMALLER of the phase cap and what
    # is left, so it can never extend the global deadline.
    # THE ANCHOR DESCRIPTOR IS CLOSED FOR ANYTHING THAT RUNS TREE CODE. This
    # probe IMPORTS the tree under test, and an inherited directory descriptor on
    # TMP_DIR would hand that code exactly the swap-proof handle the assembly
    # uses to defend itself -- a channel of precisely the kind section 53 had to
    # close for this function's pid file.
    probe_budget "read the protocol constants"
    setsid timeout -s KILL "$PROBE_BUDGET" \
        python3 - "$1" 3>"$_rec" {TMP_FD}<&- >/dev/null 2>/dev/null <<'PY' &
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
    _prb=$!
    # TRACKED THE SAME WAY THE WALKS ARE, in `WALK_PIDS`, and deliberately NOT
    # through a file. The first cut of this section published the pid to
    # `$TMP_DIR/probe.pid` so `cleanup` could read it across the command
    # substitution this function used to be called in -- and that handed the
    # TREE a channel into the gate's own control state, because
    # `PYTHONPYCACHEPREFIX` is exported as `$TMP_DIR/pycache`, so imported code
    # can derive TMP_DIR, then delete the file to defeat cancellation or
    # substitute a pid to make the gate signal an unrelated process group
    # (Codex adversarial, section 53 round 2, [high]). That is precisely the
    # class this section exists to reason about, reintroduced by its own fix.
    # The channel is REMOVED rather than validated: `proto_of` is now called
    # with a REDIRECTION instead of a command substitution, so it runs in the
    # parent shell and can set `WALK_PIDS` directly, which no tree can write.
    # ONE WINDOW STAYS OPEN and is recorded rather than papered over: a TERM
    # landing between the `&` and the line below finds `WALK_PIDS` unset. It is
    # the same single-statement window every walk phase carries
    # (`... & BASE_PID=$!` then `WALK_PIDS=...`), because a pid cannot be
    # recorded before the fork that creates it.
    WALK_PIDS="$_prb"
    wait "$_prb"
    _prc=$?
    # DECLARED WAITED for the duration of the reap below, so a trap firing in
    # that window addresses the group and never the released pid.
    WALK_WAITED="$_prb"
    # AND THE SUCCESS PATH REAPS TOO, which is the ordering the first cut of
    # this section missed. `timeout` bounds the PROBE; it does not bound what
    # the imported tree code spawned. A loader that starts a background process
    # and then RETURNS NORMALLY leaves that descendant behind at a clean exit,
    # with the pid file already removed and nothing left that can reach it --
    # the same leak the stalled case shows, on an ordering the stalled case
    # cannot reach (Codex test-coverage, section 53, [high]). Probing the GROUP
    # for liveness first costs nothing on the ordinary path: the leader is gone
    # and an honest tree left no members, so this is one failed signal.
    reap_walk_group "$_prb"
    # ONLY NOW is the probe untracked: a TERM landing mid-reap must still find
    # a target, so `cleanup` can finish what this block started.
    WALK_PIDS=""
    WALK_WAITED=""
    if [ "$_prc" -ne 0 ]; then
        # The probe catches its OWN exceptions and reports UNREADABLE itself,
        # so a nonzero status here is the interpreter failing to run at all --
        # including 124/137, the budget killing a probe that would not finish.
        # This was the ONE phase in the file running unbounded, so a stalled
        # import or a blocked read could burn the whole gate budget and be
        # terminated externally instead of returning a token (Codex
        # adversarial, section 50 round 22).
        bounded_fs rm -f "$_rec"
        PROTO_RESULT=TRANSPORT
        return 0
    fi
    # THE FIRST LINE IS THE RECORD, and anything after it is discarded rather
    # than trusted. `proto_of` writes its line before interpreter shutdown, so
    # an `atexit` handler can only APPEND -- taking the head makes a late write
    # inert instead of authoritative. The framing is still checked rather than
    # assumed: an empty file is not a record.
    if [ ! -s "$_rec" ]; then
        bounded_fs rm -f "$_rec"
        PROTO_RESULT=TRANSPORT
        return 0
    fi
    local _line
    # THE ALLOWANCE IS REFRESHED FOR THE READ, not reused from the launch: the
    # probe has just spent an unknown amount of it.
    probe_budget "read the protocol record it just wrote"
    _line="$(timeout --foreground -s KILL "$PROBE_BUDGET" head -n 1 "$_rec" 2>/dev/null)" \
        || { bounded_fs rm -f "$_rec"; PROTO_RESULT=TRANSPORT; return 0; }
    bounded_fs rm -f "$_rec"
    [ -n "$_line" ] || { PROTO_RESULT=TRANSPORT; return 0; }
    PROTO_RESULT="$_line"
}
# CALLED PLAINLY, so it runs in THIS shell. A `$(...)` would put it in a
# subshell, where the probe pid it records dies with that subshell and
# `cleanup` -- which runs here -- could never reach the probe. The answer comes
# back in `PROTO_RESULT` rather than on stdout precisely so that neither a
# subshell nor a tree-addressable pathname is involved (section 53 rounds 2, 3).
proto_of "$BASE_MIN_ADDR"
BASE_PROTO="$PROTO_RESULT"
min_tree_still_matches "$BASE_MIN" "$BASE_SHA" base "$BASE_MIN_FD"
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
#   d. the commit carries them but the tree the gate read does not -- an
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
    _out="$(bounded_git ls-tree "$1" -- "$2")"; _rc=$?
    [ "$_rc" -eq 2 ] && return 2
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
    # BOUNDED: this is a FULL-HISTORY traversal, the most expensive read in the
    # file after the object walks, and it had no limit at all.
    _out="$(bounded_git log --full-history --diff-filter=A --format=%H -1 "$_c" -- "$@")"
    _rc=$?
    [ "$_rc" -eq 2 ] && return 2
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
    _shallow="$(bounded_git rev-parse --is-shallow-repository)"; _rc=$?
    [ "$_rc" -ne 0 ] && return 2
    [ "$_shallow" = "true" ] && return 1
    _p="$(bounded_git rev-parse --git-path shallow)" || return 2
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
        _p="$(bounded_git rev-parse --git-path info/grafts)" || return 2
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
    # form classification, by `commit_objects_are_substituted`, which runs for
    # BOTH endpoints after they are resolved and before either is CLASSIFIED --
    # the metadata capture and the read pin are what precede resolution. See
    # the precondition block far above for why that had to move out of this
    # function.
    return 0
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
# deliberately: `commit_objects_are_substituted` subsumes it, because the base
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
        # THE SUBSTITUTION QUESTION IS NOT ASKED HERE ANY MORE. It ran in this
        # arm alone, which meant it was asked only once `proto_of` had already
        # failed -- so a replacement whose synthetic tree PARSES was never
        # questioned at all. It is now a repository precondition adjudicated
        # far above -- the metadata captured and reads pinned before the
        # endpoints are resolved, the endpoint-specific questions adjudicated
        # after that resolution and before any classification -- and for both
        # endpoints (section 52). Asking it again here would be a second copy of a
        # contract this file has already had to repair for disagreeing with
        # itself, and the export up there has since made every read below
        # resolve stored objects regardless.
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

# THE BASE IS NOW FINISHED, so the head substitution answers recorded far above
# can finally be emitted -- after every base token has had its chance, and
# before the head side CLASSIFIES anything from the head commit. Not before
# every head-side READ: the precondition's own reachability walk necessarily
# read head objects to reach these answers. It is the classification that base-
# first orders, and no head token is emitted until the base has none to give.
case "$HEAD_PEEL_RC" in
    3) die_infra "ENDPOINT_MOVED_UNDER_GATE: '$HEAD_SPEC' resolved to two different commits while this gate was reading it, so the head moved under the question rather than being substituted by repository metadata. Nothing is wrong with the tree under test; re-run against a repository that holds still" ;;
    0) die_infra "HEAD_OBJECTS_SUBSTITUTED: repository replacement metadata makes '$HEAD_SPEC' resolve to a different commit than the stored objects do, so the tree under test ($HEAD_RESOLVED) is not the one the repository resolves for that argument. Remove the replacement or re-run against a clean clone" ;;
    2) die_infra "HEAD_OBJECTS_SUBSTITUTED: the gate holds no usable snapshot of how '$HEAD_SPEC' resolves with replacement metadata active, so whether it and the repository agree on which commit the head IS is unknown, and no verdict is being guessed from it" ;;
esac
case "$HEAD_SUBST_RC" in
    0) die_infra "HEAD_OBJECTS_SUBSTITUTED: an active refs/replace entry substitutes an object in the head commit $HEAD_RESOLVED. This gate pins replacement reads off, so it is not reading the substitute -- but what it would certify about that commit (which closure members it carries, how it stores them, whether it carries the snapshot protocol) is then not what the repository shows for it, and a verdict the operator cannot see is not one they can act on. Remove the replacement or re-run against a clean clone" ;;
    2) die_infra "HEAD_OBJECTS_SUBSTITUTED: could not determine whether repository replacement metadata substitutes anything in the head commit $HEAD_RESOLVED, so whether a verdict about it would describe the commit the repository shows is unknown and none is being guessed from it" ;;
esac

# ---------------------------------------------------------------------------
# THE HEAD SIDE READS THE COMMIT (section 54).
# ---------------------------------------------------------------------------
# Every head-side read below used to come from $REPO_ROOT, the LIVE WORKING
# TREE, while the verdict was published about $HEAD_RESOLVED. Those are not the
# same bytes, and a dirty checkout could satisfy every check here while the
# commit being pushed failed them: commit a resolver-closure change together
# with a broken protocol, restore valid protocol files in the worktree only,
# and the closure no longer early-exits, the head read succeeds, and the
# differential runs on the restored bytes. CI then rejects the same commit on a
# clean checkout, from a main that has already moved -- which is the precise
# failure the local gate exists to prevent.
#
# Reading the commit also removes, rather than narrows, the measure-to-act
# window section 51 documented and deliberately left open: a materialized
# commit cannot be edited between the measurement and the decision, so the
# closure's evidence describes a tree that is still there by construction.
# Section 51's CLOSURE_MOVED_UNDER_GATE was not retired by that -- it guarded
# the materialized tree instead of the live one, which is a weaker threat but
# not an impossible one, and retiring a published token was not section 54's to
# do. SECTION 55 THEN REMOVED THE LAST TREE IT COULD GUARD: the fast path
# materializes no checkout at all, so there is no longer anything that can move
# under the gate, and the token has NO EMITTER. That is left standing rather
# than deleted for the same reason section 54 left it: retiring a published
# token belongs to the section that owns the token surface.
# -> XREF: [`TODO-06 section 58`](#58-head-versus-tree-checks-section-54-made-unreachable-are-retired-or-re-fixtured)
#
# MATERIALIZED HERE, NOT BESIDE THE BASE CHECKOUT, for exactly the reason the
# protocol probe below is acquired here: base-first governs classification, so
# a head checkout that fails on disk, on a lock, or on the budget must not
# report ahead of a base that could have been classified (Codex design review,
# section 54).
#
# THE ONE HEAD-SIDE READ THAT CANNOT BE REDIRECTED IS THIS SCRIPT ITSELF.
# The gate is executed from the working tree by `.githooks/pre-push`, so the
# code doing the certifying is the live copy whatever the rest of this file
# reads. Re-executing the committed copy would change how the gate is invoked,
# which is section 16's surface; refusing does not, and it fails closed. It
# also cannot wedge: any push whose driver matches its own commit passes, and
# the repair (commit or stash the driver edit) is attainable and obvious. Until
# section 54 the closure loop caught this incidentally, by hashing the live
# driver -- so redirecting the head reads without this check would have REMOVED
# a net rather than moved it (Codex design review, section 54).
GATE_DRIVER_PATH="scripts/todo-graph/identity-gate.sh"
DRIVER_AT_HEAD="$(bounded_git rev-parse --quiet --verify "$HEAD_RESOLVED:$GATE_DRIVER_PATH")"
case "$?" in
    0) ;;
    1) die_infra "GATE_DRIVER_NOT_AT_HEAD: the head commit $HEAD_RESOLVED does not carry $GATE_DRIVER_PATH, so the running gate is not code that commit contains and nothing it concluded would describe the range being pushed" ;;
    *) die_infra "GATE_DRIVER_NOT_AT_HEAD: could not ask the head commit $HEAD_RESOLVED how it stores $GATE_DRIVER_PATH -- the repository, not the tree under test, is what failed, and no verdict is being guessed from it" ;;
esac
# `--path`, NOT A RAW HASH. `git hash-object <file>` hashes the bytes on disk;
# a commit stores the CLEAN form of them. Where a filter, `text=auto`, `ident`
# or an encoding attribute applies, those two differ for a file nobody has
# touched -- `git status` reports the tree clean and this check would refuse a
# push with a repair the operator cannot perform, because there is nothing to
# commit or stash. `--path` hashes the file as git would store it AT that path,
# so both sides of the comparison are in the same canonical space and the
# question is the one actually being asked: does this commit carry this code
# (Codex adversarial, section 54, [medium]).
DRIVER_LIVE="$(bounded_git hash-object --path "$GATE_DRIVER_PATH" "$REPO_ROOT/$GATE_DRIVER_PATH")" \
    || die_infra "GATE_DRIVER_NOT_AT_HEAD: could not hash the running gate at $REPO_ROOT/$GATE_DRIVER_PATH, so whether it is the code the head commit carries is unknown and no verdict is being guessed from it"
if [ "$DRIVER_AT_HEAD" != "$DRIVER_LIVE" ]; then
    # TWO CAUSES, TWO REPAIRS, and naming the wrong one sends the operator
    # somewhere that cannot work. With the default head the mismatch is an
    # uncommitted edit, which committing or stashing clears. With an EXPLICIT
    # `--head` the working tree can be perfectly clean and simply belong to a
    # different commit than the one selected -- there is no edit to put away,
    # and the message used to demand one anyway (Codex consistency, section 54,
    # [medium]).
    if [ "$HEAD_SPEC" = "HEAD" ]; then
        die_infra "GATE_DRIVER_NOT_AT_HEAD: the running $GATE_DRIVER_PATH differs from the copy in the head commit $HEAD_RESOLVED, so the code adjudicating this range is not the code being pushed. Every other head-side read comes from that commit; this one cannot, because the hook executes the working-tree copy. Commit or stash the driver edit and re-run"
    fi
    die_infra "GATE_DRIVER_NOT_AT_HEAD: the running $GATE_DRIVER_PATH is not the copy carried by '$HEAD_SPEC' ($HEAD_RESOLVED), which this run was explicitly pointed at. The working tree may be perfectly clean; the selected head simply carries a different driver, and this gate will not certify a range with code that range does not contain. Re-run from a checkout whose $GATE_DRIVER_PATH matches that commit, or select a head that matches this one"
fi

# THE HEAD SIDE, SAME TREATMENT AND FOR THE SAME REASON (section 55). Section
# 54's property is untouched: every head-side read still comes from
# $HEAD_RESOLVED and never from the working tree. What changes is HOW those
# bytes are put where the protocol loader can execute them -- assembled from
# that commit's own blobs rather than checked out with the other 2,550 files
# this phase never opens.
materialize_subtree "$HEAD_RESOLVED" "$HEAD_MIN" head "${MIN_SUBTREES[@]}"
exec {HEAD_MIN_FD}<"$HEAD_MIN" \
    || die_infra "cannot hold a descriptor on the head tree just assembled, so the phases that read it and the check that re-binds it could not be tied to one directory"
root_fd_is_the_assembly "$HEAD_MIN_FD" head "$HEAD_MIN"
HEAD_MIN_ADDR="/proc/self/fd/$HEAD_MIN_FD"
log "head $HEAD_RESOLVED adjudicated from its own blobs, assembled at $HEAD_MIN"

# AND THE WORKING TREE IS NEVER SILENTLY PREFERRED OR SILENTLY IGNORED. A
# reader who runs this gate from a dirty checkout would otherwise have to infer
# from the invocation that their uncommitted edits were not examined. It is a
# NOTE and not a refusal on purpose: the verdict is correct about the range
# either way, so refusing here would wedge a legitimate push made from a tree
# carrying unrelated uncommitted work. The one divergence that IS fatal --
# this script -- was refused above, before the checkout.
# A NOTE MAY NOT SPEND THE VERDICT'S DEADLINE. `bounded_git` bounds every probe
# by the gate's WHOLE remaining budget, which is right for a question the
# verdict depends on and wrong for one it explicitly does not: a large or
# degraded working tree could burn the adjudication budget listing untracked
# files, and the protocol checks that actually matter would then fail for want
# of time -- blocking a push over a disclosure the code calls non-fatal (Codex
# perf, section 54, [medium]). Its own small allowance, and its failure is
# already handled as "could not be determined".
DIVERGE_CAP_SECS=10
DIVERGE_LIST_MAX=40
# ONE ALLOWANCE SHARED BY BOTH PROBES, not one each. Giving each a fresh cap
# made the advertised 10 seconds mean up to 20, and every second of it comes
# off the same `GATE_MONO_START` the protocol checks are measured against -- so
# an explicitly non-fatal disclosure could spend budget the verdict then lacks
# (Codex perf, section 54 post-ship, [medium]).
DIVERGE_START=""
mono_now && DIVERGE_START="$MONO_NOW"
diverge_left() {   # seconds this disclosure may still spend, 0 when spent
    local _r _rem
    [ -n "$DIVERGE_START" ] || { printf '0'; return; }
    mono_now || { printf '0'; return; }
    _r=$(( DIVERGE_CAP_SECS - (MONO_NOW - DIVERGE_START) ))
    [ "$_r" -lt 1 ] && { printf '0'; return; }
    # AND NEVER MORE THAN THE GATE HAS LEFT, so the disclosure cannot outlive
    # the run it is describing.
    #
    # CLAMPED AGAINST THE UNFLOORED BUDGET. This read `remaining()`, which
    # answers 1 on an expired or unreadable clock -- so once the gate's global
    # budget was spent, the cap above still had room and this clamp still handed
    # back a second, and a SECOND disclosure probe launched for it. The
    # disclosure is explicitly non-fatal, which is exactly why it must not be
    # the one step that outlives the deadline (Codex adversarial, section 55
    # round 10, [medium]).
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then
        printf '0'
        return
    fi
    _rem="$BUDGET_LEFT"
    [ "$_r" -gt "$_rem" ] && _r="$_rem"
    printf '%s' "$_r"
}
# NO PIPE ON EITHER PROBE. `... | head -N` under `pipefail` reports FAILURE the
# moment head closes the pipe and the producer takes SIGPIPE, which is the same
# trap the prune confirmation had to be rewritten to avoid -- and here it would
# turn a perfectly good listing into "could not be determined". The enumeration
# is bounded by the shared allowance above; the DISPLAY is bounded where it is
# printed.
_dv_left="$(diverge_left)"
if [ "$_dv_left" -eq 0 ]; then
    DIVERGE_TRACKED=""; DIVERGE_RC=1
else
    DIVERGE_TRACKED="$(timeout -s KILL "$_dv_left" git diff --name-only "$HEAD_RESOLVED" 2>/dev/null)"; DIVERGE_RC=$?
fi
_dv_left="$(diverge_left)"
if [ "$_dv_left" -eq 0 ]; then
    DIVERGE_UNTRACKED=""; DIVERGE_URC=1
else
    DIVERGE_UNTRACKED="$(timeout -s KILL "$_dv_left" git ls-files --others --exclude-standard 2>/dev/null)"; DIVERGE_URC=$?
fi
if [ "$DIVERGE_RC" -ne 0 ] || [ "$DIVERGE_URC" -ne 0 ]; then
    log "NOTE: whether the working tree differs from $HEAD_RESOLVED could not be determined (the probe failed or the budget expired). The verdict below is about that commit regardless; only this disclosure is missing."
else
    # COUNTED BY LINE, NEVER BY WORD SPLITTING. A path carrying a space is
    # rare in this corpus and would still have to be reported correctly; `git
    # diff --name-only` emits one path per line (quoting the exotic ones), so
    # lines are the unit and an unquoted expansion would be a second bug in a
    # disclosure whose whole job is to be accurate.
    DIVERGE_ALL="$(printf '%s\n%s\n' "$DIVERGE_TRACKED" "$DIVERGE_UNTRACKED" | grep -c . || true)"  # launch-exempt: counts a shell variable already in memory
    if [ "${DIVERGE_ALL:-0}" -gt 0 ]; then
        log "NOTE HEAD_WORKTREE_DIVERGES: the working tree differs from $HEAD_RESOLVED in $DIVERGE_ALL path(s), none of which were adjudicated. This gate reports on the commit, not on what is on disk:"
        # THE COUNT IS COMPLETE, THE LIST IS BOUNDED. A tree with thousands of
        # untracked files would otherwise bury the verdict under its own
        # disclosure; the number above is the answer, the names are the
        # convenience (Codex perf, section 54, [medium]).
        # Every stage below reads a pipe fed from a shell variable already in
        # memory, so none can block on I/O and a deadline around them would
        # bound nothing. The marker sits on the command's own logical line
        # because that is where the inventory reads it -- a reason written above
        # the call is a reason the checker never sees.
        printf '%s\n%s\n' "$DIVERGE_TRACKED" "$DIVERGE_UNTRACKED" \
            | grep . | head -"$DIVERGE_LIST_MAX" | sed 's/^/    diverges: /'  # launch-exempt: in-memory pipeline, nothing here can block
        if [ "$DIVERGE_ALL" -gt "$DIVERGE_LIST_MAX" ]; then
            log "    ... and $(( DIVERGE_ALL - DIVERGE_LIST_MAX )) more (list capped; the count above is complete)"
        fi
    fi
fi

# ACQUIRED HERE, NOT BESIDE THE BASE PROBE. Running it up front meant a head
# probe that STALLED could stop a perfectly classifiable base from ever
# emitting its token -- base-first ordering has to hold for the acquisition as
# well as the adjudication, or the base's answer waits on the head's machinery
# (Codex adversarial, section 50 round 22).
proto_of "$HEAD_MIN_ADDR"
HEAD_PROTO="$PROTO_RESULT"
case "$HEAD_PROTO" in *$'\n'*) HEAD_PROTO=UNREADABLE ;; esac
# RE-BOUND BEFORE ANYTHING CONSUMES THE RESULT. The probe above executed modules
# from this tree; the closure decision, the protocol adjudication and every
# later phase that reads it are downstream of that.
min_tree_still_matches "$HEAD_MIN" "$HEAD_RESOLVED" head "$HEAD_MIN_FD"

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
        protocol_present_in_worktree "$HEAD_MIN_ADDR"
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
                die_infra "HEAD_PROTOCOL_UNMATERIALIZED: the commit $HEAD_RESOLVED carries ${PROTOCOL_PATHS[$_i]} but the tree the gate assembled for it at $HEAD_MIN does not -- that materialization is incomplete (a partial clone missing the blob), so the gate is reading a tree that is not the commit it claims to test"
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
            die_infra "HEAD_PROTOCOL_UNMATERIALIZED: the commit $HEAD_RESOLVED carries a complete snapshot protocol but the tree the gate assembled for it at $HEAD_MIN does not (missing$HEAD_MISSING) -- that materialization is incomplete (a partial clone missing the blob), so the gate is reading a tree that is not the commit it claims to test"
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
# both sides: `data` needs the JSON, `legacy` needs BOTH python files. BOTH are
# checked in the COMMIT, as an ls-tree mode. The head was checked in its
# materialized checkout until section 54 round 2, on the reasoning that a side
# should be asked about what its reader actually opened -- but a filesystem
# cannot answer that question reliably about TYPE, and the commit can.
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
# ASKED OF THE COMMIT, NOT OF THE FILESYSTEM, and now symmetrically with the
# base (Codex adversarial, section 54 round 2, [high]). The filesystem is not a
# reliable witness to an entry's TYPE even in a tree git has just written: with
# `core.symlinks` false -- the default wherever the host cannot create symlinks,
# which is an ordinary Windows checkout -- git materializes a committed 120000
# entry as a small REGULAR file holding the target path text, and considers the
# result clean. `ls-files` reports it `H` and `status` reports nothing, so the
# faithfulness check above cannot see it either: the CONTENT is faithful and the
# TYPE is not. The head side would then classify a symlinked protocol file as a
# regular one and adjudicate a range on semantics that no symlink-capable
# checkout, and no CI run, shares. The mode recorded in the commit is the one
# answer that does not vary by host.
if ! protocol_present_in_commit "$HEAD_RESOLVED"; then
    die_infra "HEAD_TREE_UNREADABLE: could not ask the head commit $HEAD_RESOLVED how it stores its protocol files ($PROTO_PROBE_ERR) -- the repository state, not the tree under test, is what failed here, and no classification is being guessed from it"
fi
protocol_source_is_regular "$HEAD_SOURCE" head \
    || die_infra "HEAD_PROTOCOL_NOT_A_FILE: the tree under test parsed its protocol through the '$HEAD_SOURCE' form, but the head commit $HEAD_RESOLVED does not store all of that form's files as regular files -- a symlink parses (the reader follows it) while resolving to bytes the commit may not contain, so the protocol this gate would adjudicate is not the one under test"

# ---------------------------------------------------------------------------
# THE BYTE-IDENTICAL CLOSURE EXIT (measured far above, decided here).
# ---------------------------------------------------------------------------
# Both sides have now cleared every protocol question this gate knows how to
# ask -- readable, complete, materialized, and reached through regular files:
# in the commit on the base side, and since section 54 in the gate's own
# materialization of the head commit on the head side. Only
# now is "no verdict-affecting file changed" a statement worth exiting 0 on.
# A range that fails any of those questions never arrives here; it has already
# died carrying the stable token that names WHICH question it failed, which is
# what section 50 published that vocabulary for.
# AND THE CLOSURE IS RE-EXAMINED HERE, UNCONDITIONALLY.
#
# Moving the decision widened the window between hashing the closure and acting
# on it from nothing to a base worktree plus two protocol probes. When the head
# side hashed the WORKING TREE, a resolver file saved inside that window in a
# shared local checkout would be measured in its old form, ride the exit, and
# make an UNEXAMINED change the next baseline (Codex adversarial, section 51,
# [high]). Section 54 moved the head reading to the commit, which closes that
# window rather than narrowing it: the measurement and the decision now read
# objects that cannot be edited. The re-check stays, and stays unconditional,
# because it still guards the tree the gate materialized for itself and
# because the structural questions below are preconditions in their own right.
#
# THIS BLOCK IS NOT GUARDED BY `CLOSURE_CHANGED`, and making it so was a real
# defect rather than an optimisation. Hung under `if [ "$CLOSURE_CHANGED" -eq 0 ]`
# every structural check here -- entry type, object presence, committed mode,
# head-vs-worktree presence -- was skipped the moment ANY content differed. So
# the exact scenario the mode check was added for still worked: pair a benign
# closure edit with identity-gate.sh flipping 100755 to 100644, and the local
# hook skips the now-non-executable script while CI runs it through `bash`,
# takes this branch, and can return rc 0 from an unchanged differential. A
# symlinked closure member with differing content likewise reached the Python
# walks and was EXECUTED (Codex adversarial, section 51 review, [high]).
# Structural validity is a precondition for adjudicating the range at all;
# only the CONTENT comparison decides which way the range is adjudicated.
#
# THREE DISTINCT FAULTS, THREE DISTINCT TOKENS, because they have three
# different repair actions and a caller branching on them needs to tell them
# apart:
#   CLOSURE_UNVERIFIABLE      the gate could not establish what it is looking
#                             at -- fix the repository or the checkout;
#   CLOSURE_MOVED_UNDER_GATE  a member changed WHILE the gate ran -- re-run
#                             against a tree that holds still. NO LONGER
#                             EMITTED since section 55: the fast path holds no
#                             checkout for a member to move in, and a
#                             cross-probe disagreement about an immutable
#                             commit is a repository or parser fault, which is
#                             CLOSURE_UNVERIFIABLE. Retiring it is section 58's;
# A third token, CLOSURE_MODE_CHANGED, briefly lived here for a base-vs-head
# mode difference. It was withdrawn in the same review: deterministic, so it had
# no business sharing the movement token (Codex consistency, [medium]) -- and
# then, as a base-vs-head DIFF, a permanent wedge with no attainable repair
# (Codex re-adversarial, [high]). The one mode that is load-bearing is now asked
# as GATE_SCRIPT_NOT_EXECUTABLE, an invariant about the head, after this loop.
#
# AN UNANSWERABLE PROBE IS NOT CONFIRMED ABSENCE. The measurement far above
# collapses every git failure to the string MISSING, so a member absent at the
# base and merely UNREADABLE at head compares equal there. That collapse
# predates this section and is left alone, where it can only force a walk; it
# must not survive into the DECISION, so this pass asks tri-state questions
# through the same probes the protocol contract uses.
#
# THE WINDOW SECTION 51 LEFT OPEN IS NOW CLOSED, and section 51 was right to
# refuse to claim it. Its text said a member could still move after its own
# re-hash, that only adjudicating an immutable snapshot removes that, and that
# the question belonged to section 54. Section 54 took it: the head side reads
# $HEAD_RESOLVED, an immutable snapshot, so there is no live file left for a
# member to move under. What survives is the residual case of the gate's own
# materialized checkout being disturbed, which is what the re-hash below now
# measures.
for _i in "${!CLOSURE[@]}"; do
    f="${CLOSURE[$_i]}"
    # THE BATCH ABOVE ALREADY ASKED. Its failure is raised HERE, per member and
    # in this loop's own order, so nothing about which token a range earns or
    # which side is classified first has moved (section 55).
    [ "$B_ERR" -eq 0 ] \
        || die_infra "CLOSURE_UNVERIFIABLE: could not ask the base $BASE_SHA about the closure member $f -- the repository, not the range, is what failed, and an unanswered probe must never read as 'unchanged'"
    _bt=""
    [ "${B_PRESENT[$_i]}" -eq 1 ] && _bt=present
    if [ -n "$_bt" ]; then
        # THE ENTRY'S TYPE, NOT MERELY ITS PRESENCE. A blob OID says what the
        # entry CONTAINS and nothing about what it IS: git stores a symlink as
        # mode 120000 whose blob is the target PATH TEXT, so a base symlink
        # pointing at `pass` and a head regular file containing `pass` carry the
        # SAME OID -- equal by content, wholly different in what executes
        # (Codex adversarial, section 51 round 3, [high]).
        # `path_is_regular_in_commit` already asks this for the protocol
        # contract, checking type and mode together, and rejects a submodule
        # entry (type `commit`) by the same test.
        # THE TYPE AND MODE COME FROM THE SAME ENTRY, asked exactly as
        # `path_is_regular_in_commit` asks them (type `blob`, mode 100644 or
        # 100755), which is what rejects a symlink whose blob equals a regular
        # file's bytes and what rejects a submodule entry.
        if [ "${B_TYPE[$_i]}" != "blob" ] \
           || { [ "${B_MODE[$_i]}" != "100644" ] && [ "${B_MODE[$_i]}" != "100755" ]; }; then
            die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA carries the closure member $f as something other than a regular file (a symlink, a directory, or a submodule) -- its blob may match byte-for-byte while naming rather than being the code, so nothing here can be certified and nothing here may be executed"
        fi
        # THE EXPRESSION READING, exactly as the `rev-parse --quiet --verify
        # <commit>:<path>` this replaced: the listing said the entry is there,
        # so a path expression that will not resolve is a repository fault.
        _b="${B_VOID[$_i]}"
        [ -n "$_b" ] \
            || die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA lists the closure member $f but its object could not be resolved"
        # AND THE OBJECT IS PRESENT, not merely named. A tree entry resolves to
        # an OID from the TREE; in a partial clone the blob behind it can be
        # absent, and comparing OIDs would then certify bytes nothing in this
        # repository can produce. The batch `--batch-check` above answered this
        # for every member in one process.
        [ "${B_OBJ[$_i]}" -eq 1 ] \
            || die_infra "CLOSURE_UNVERIFIABLE: the base $BASE_SHA names object $_b for the closure member $f but that object is not present in this repository"
    else
        _b=ABSENT
    fi
    # THE HEAD READING IS THE COMMIT'S OWN BLOB, TAKEN FROM THE ENTRY ITSELF
    # (section 55). It was `git hash-object` over the materialized checkout,
    # which cost a process per member and required that checkout to exist here
    # -- and the checkout is the thing this section is removing from the fast
    # path. The tree entry read on the very next line already carries the OID,
    # so the same answer is now free, and it is the answer section 54 wanted:
    # the bytes the COMMIT stores, with no filesystem in between at all.
    [ "$H_ERR" -eq 0 ] \
        || die_infra "CLOSURE_UNVERIFIABLE: could not ask the head commit $HEAD_RESOLVED how it stores the closure member $f -- the repository, not the range, is what failed"
    if [ "${H_PRESENT[$_i]}" -eq 1 ]; then
        _h="${H_OID[$_i]}"
        # THE HEAD TYPE IS ASKED WHENEVER THE HEAD CARRIES THE MEMBER, not only
        # when the base does too. Previously `path_is_regular_in_worktree` ran
        # unconditionally over the checkout and refused a symlink or directory
        # there, while the commit-side type test below is guarded on BOTH sides
        # being present -- so dropping the worktree read without this would have
        # left a head-only closure member's type unchecked. Asked here, the
        # coverage is the same as before and it is asked of the commit.
        if [ "${H_TYPE[$_i]}" != "blob" ] \
           || { [ "${H_MODE[$_i]}" != "100644" ] && [ "${H_MODE[$_i]}" != "100755" ]; }; then
            die_infra "CLOSURE_UNVERIFIABLE: the head commit $HEAD_RESOLVED carries the closure member $f as something other than a regular file (a symlink, a directory, or a submodule) -- its blob may match byte-for-byte while naming rather than being the code, so nothing here can be certified and nothing here may be executed"
        fi
        _ht=present
    else
        _h=ABSENT
        _ht=""
    fi
    # THE TWO INDEPENDENT READS OF THE SAME COMMIT MUST AGREE, and they are
    # independent by construction rather than by assertion. `FIRST_H` is the
    # PATH EXPRESSION reading (`<commit>:<path>`, resolved by `cat-file
    # --batch-check`) and `$_h` is the TREE LISTING reading (`ls-tree`, parsed
    # in this file), so a member attributed to the wrong entry, or silently
    # dropped, by either parser surfaces here instead of riding the exit. An
    # earlier cut of this section took BOTH from the listing and kept a comment
    # claiming a cross-probe, which made the branch unable to fire (Codex
    # adversarial, section 55 round 1, [medium]).
    #
    # AND IT IS `CLOSURE_UNVERIFIABLE`, NOT THE MOVEMENT TOKEN, which is a
    # distinction about OWNERSHIP rather than wording. What this branch now
    # detects is a parser or repository fault: two probes disagreeing about one
    # immutable commit. That is not "a member changed while the gate ran", which
    # is what `CLOSURE_MOVED_UNDER_GATE` is published as meaning, and quietly
    # re-pointing a published token at a different fault would hand every caller
    # a movement diagnosis for a repository fault. This section owns the fast
    # path's COST; the token surface belongs to section 58, so the check reports
    # under the token that already covers "this member cannot be certified" and
    # leaves the other one alone (Codex adversarial, section 55 round 5,
    # [medium]).
    # -> XREF: [`TODO-06 section 58`](#58-head-versus-tree-checks-section-54-made-unreachable-are-retired-or-re-fixtured)
    #    owns the published-token surface. This section leaves
    #    CLOSURE_MOVED_UNDER_GATE with no emitter at all, which is exactly the
    #    retire-or-re-fixture decision that section exists to make; the two
    #    checkout-presence refusals removed just below are the same question.
    if [ "$_h" != "${FIRST_H[$_i]}" ] \
       && ! { [ "$_h" = ABSENT ] && [ "${FIRST_H[$_i]}" = MISSING ]; }; then
        die_infra "CLOSURE_UNVERIFIABLE: the two independent readings of the closure member $f in $HEAD_RESOLVED disagree ('${FIRST_H[$_i]}' then '$_h') -- one resolved the path expression, the other parsed the tree listing, and an immutable commit cannot answer them differently, so the repository or this gate's own parsing is what is failing and nothing here can be certified"
    fi
    # THE MODE IS ASKED OF BOTH COMMITS, NEVER OF THE FILESYSTEM, and only to
    # establish that the head entry is a regular file at all. Taking the head
    # mode from `[ -x ]` instead asked a different question: git supports
    # checkouts whose filesystem does not carry the bit (`core.fileMode=false`,
    # and mounts that report everything executable), so a PERFECTLY HEALTHY
    # range there refused -- wedging every resolver push on that host, and
    # invisible locally because this checkout preserves modes (Codex
    # adversarial, section 51 round 5, [high]).
    #
    # ASKED ONCE, ABOVE, AND NOW OF STRICTLY MORE RANGES (section 55). This test
    # stood here guarded on BOTH sides carrying the member, because the head's
    # type was separately covered by `path_is_regular_in_worktree` over the
    # checkout. That checkout is gone from this path, so the test moved up to the
    # point the head entry is read and lost the base-side guard with it: a
    # member the head ADDS is now type-checked too, where before neither probe
    # reached it. Repeating it here would spend a process per member to re-derive
    # an answer already in hand, which is the fan-out this section removes.
done

# THE ONE MODE THAT IS LOAD-BEARING, ASKED AS AN INVARIANT RATHER THAN A DIFF.
#
# A base-vs-head mode COMPARISON was tried here and withdrawn, because it is a
# permanent wedge and the repair it recommended provably cannot work: the base
# is the last SUCCESSFULLY gated SHA, so a mode change refuses, the baseline
# never advances past it, and every later head carrying that mode refuses
# again -- "land the mode change on its own commit" reaches the same branch,
# and only REVERTING clears the gate (Codex re-adversarial, section 51 review,
# [high]). That is a gate with no exit, which is worse than the hole it closed.
#
# The hole is also narrower than a general mode comparison implies. Mode is not
# resolver behaviour, and the differential cannot see it -- but exactly ONE
# mode matters to anything: `.githooks/pre-push:194` enters its identity-gate
# block only `[ -x ... identity-gate.sh ]`, so a head where this script is not
# executable has SILENTLY disabled the gate for every subsequent local push,
# while CI keeps passing because it invokes the script through `bash`. Asked as
# an invariant about the head alone, that has an attainable repair -- restore
# the bit and commit -- and it cannot wedge, because any head where the file IS
# executable passes. Mode changes on every other closure member are no longer
# refused at all; they were never the threat.
# THE PROBE'S STATUS IS NOT DISCARDED, and discarding it is exactly what the
# refactor that removed a `cut` from this line did. `bounded_git` answers 2 for
# "could not ask", and the result was read as a plain string -- so ONE timed-out
# or failed `ls-tree` left `_GATE_MODE` empty, the 100644 branch was skipped,
# and a head storing this file non-executable reached the byte-identical exit.
# That head then has the local hook's identity-gate block disabled for every
# subsequent push, which is the precise failure this check exists to catch, and
# it would have been reached by an unreadable probe rather than by a decision
# (Codex adversarial, section 55, [high]). A failed question is never a negative
# answer -- the rule this file states in a dozen other places, broken here by a
# change whose only intent was to remove a process.
_GATE_MODE_ENTRY="$(bounded_git ls-tree "$HEAD_RESOLVED" -- "scripts/todo-graph/identity-gate.sh")"
case "$?" in
    0) ;;
    *) die_infra "could not ask the head commit $HEAD_RESOLVED how it stores scripts/todo-graph/identity-gate.sh -- the repository, not the range, is what failed, and whether this gate would still run locally at that head is therefore unknown" ;;
esac
[ -n "$_GATE_MODE_ENTRY" ] \
    || die_infra "the head commit $HEAD_RESOLVED does not carry scripts/todo-graph/identity-gate.sh at all, so there is no mode to check and nothing here can be certified"
_GATE_MODE="${_GATE_MODE_ENTRY%% *}"   # launch-exempt: parameter expansion, no process at all
if [ "$_GATE_MODE" = "100644" ]; then
    die_infra "GATE_SCRIPT_NOT_EXECUTABLE: the head commit $HEAD_RESOLVED stores scripts/todo-graph/identity-gate.sh as mode 100644. The pre-push hook enters its identity-gate block only when that file is executable, so at this head the gate is silently skipped for every local push and only CI still adjudicates -- after main has already moved. Restore the bit (git update-index --chmod=+x) and commit"
fi

if [ "$CLOSURE_CHANGED" -eq 0 ]; then
    log "resolver closure byte-identical base..head; nothing to differentiate."
    replacement_state_still_holds
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
    # SUPERVISED, BECAUSE THIS ONE RUNS THE TREE'S CODE. It was a bare command
    # substitution with no timeout and no budget check, so a checker that would
    # not finish held the gate past its published ceiling -- the same defect
    # section 55 fixed in `contract_of`, at a call site the first pass did not
    # reach (Codex adversarial, section 55 round 7, [medium]). It takes the
    # shape the other tree-executing phases use: its own session, backgrounded,
    # `wait`ed, tracked in `WALK_PIDS`, and its group reaped -- so what the
    # checker spawns is bounded too, which `timeout` alone would not give.
    phase_budget "bucket-emission contract"
    # THE ANCHOR DESCRIPTOR IS CLOSED HERE TOO. This checker executes code from
    # the tree under test, so it gets the same treatment `proto_of` and
    # `contract_of` do: an inherited directory descriptor on TMP_DIR would hand
    # it the swap-proof handle the assembly and its re-binding both rely on.
    setsid timeout -s KILL "$PHASE_BUDGET" \
        python3 "$HEAD_MIN_ADDR/scripts/lint/check_bucket_emission.py" {TMP_FD}<&- \
        --emitter "$HEAD_MIN_ADDR/scripts/todo-graph/ref_resolution.py" \
        --protocol "$HEAD_MIN_ADDR/scripts/todo-graph/snapshot_protocol.json" \
        --allow-undeclared --emitted-set \
        >"$TMP_DIR/bucket-contract.out" 2>"$TMP_DIR/bucket-contract.err" &
    _be_pid=$!
    WALK_PIDS="$_be_pid"
    wait "$_be_pid"
    EMITTED_RC=$?
    WALK_WAITED="$_be_pid"
    # THE SUCCESS PATH REAPS: this checker executes from the narrow head tree,
    # so what it spawned is the tree's, not the gate's.
    reap_walk_group "$_be_pid"
    WALK_PIDS=""
    WALK_WAITED=""
    # RE-BOUND BEFORE ITS RESULT IS READ, not merely before the walks. This
    # checker executes from the narrow head tree too, and its emitted set
    # AUTHORIZES the protocol-migration path -- so a checker that rewrites an
    # emitter or another closure member on its way out could have its own result
    # accepted, and the schema-migration branch can exit successfully long
    # before the later `contract_of` re-bindings run. The claim that every
    # tree-executing phase was followed by a re-binding was false while it was
    # written, and this is the phase it was false about (Codex adversarial,
    # section 55, [high]).
    min_tree_still_matches "$HEAD_MIN" "$HEAD_RESOLVED" head "$HEAD_MIN_FD"
    # READING THE OUTPUT IS A LAUNCH TOO, and this one reads a path the TREE'S
    # OWN CODE just had a handle on. The checker can rename its still-open
    # output and leave a FIFO at that name before exiting: its stdout keeps
    # writing to the renamed file, and a bare `cat` then opens the FIFO and
    # blocks forever, past the ceiling this file advertises -- a tree-controlled
    # hang reintroduced AFTER the supervision that was added to prevent one
    # (Codex adversarial, section 55 round 12, [medium]). Bounding the read does
    # not stop the substitution and is not meant to: the content was always the
    # checker's to choose. It stops the HANG, which was not.
    phase_budget "bucket-emission output read"
    EMITTED_SET="$(timeout --foreground -s KILL "$PHASE_BUDGET" \
        cat "$TMP_DIR/bucket-contract.out" 2>/dev/null)"
    if [ "$EMITTED_RC" -ne 0 ]; then
        phase_budget "bucket-emission error read"
        _be_excerpt="$(timeout --foreground -s KILL "$PHASE_BUDGET" head -3 "$TMP_DIR/bucket-contract.err" 2>/dev/null)"
        _be_excerpt="$(printf '%s' "$_be_excerpt" | tr '\n' ' ')"  # launch-exempt: reformats a shell variable already in memory
        die_infra "the bucket-emission contract does not hold at HEAD (rc=$EMITTED_RC), so the set of buckets the resolver can emit is unknown and no retirement can be adjudicated: $_be_excerpt"
    fi
    phase_budget "emitted-set encoding"
    EMITTED_B64="$(printf '%s' "$EMITTED_SET" | timeout --foreground -s KILL "$PHASE_BUDGET" base64 -w0)" \
        || die_infra "could not encode the declared emitted-bucket set"
    # BOUNDED, THOUGH IT RUNS ONLY THIS FILE'S OWN CODE. `--foreground` is
    # right here and would not be for the checker above: this interpreter is
    # written inline below, imports nothing from the tree, and spawns no child
    # for the documented no-children caveat to apply to. What it can still do is
    # not start, or not finish, on a loaded host -- and an unbounded step is
    # unbounded whoever wrote it.
    phase_budget "bucket relocation"
    RELOC="$(timeout --foreground -s KILL "$PHASE_BUDGET" \
        python3 - "$BASE_PRE_B64" "$BASE_POST_B64" "$HEAD_PRE_B64" "$HEAD_POST_B64" "$HEAD_MIG_B64" "$BASE_MIG_B64" \
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
    phase_budget "relocation count"
    RELOC_N="$(printf '%s' "$RELOC" | timeout --foreground -s KILL "$PHASE_BUDGET" \
        python3 -c 'import json,sys; d=json.load(sys.stdin); print(len(d["moved"]) + len(d["undeclared_removals"]) + len(d["bad_declarations"]))')" \
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
    # READ FROM THE BATCH THAT ALREADY ANSWERED THIS. Every member here is a
    # CLOSURE member, and `closure_entries` resolved the path expression for all
    # of them on both sides before the measurement loop ran -- so a per-member
    # `rev-parse` pair was eighteen redundant processes on the protocol-migration
    # path, in a section whose subject is exactly that fan-out (Codex perf,
    # section 55, [medium]). The MISSING collapse is preserved: an unresolved
    # expression is empty in `*_VOID`, and both sides empty compares equal, which
    # is what the `|| echo MISSING` pair did.
    EXEC_CHANGED=""
    for f in "${EXEC_CLOSURE[@]}"; do
        _ec_i=-1
        for _ci in "${!CLOSURE[@]}"; do
            [ "${CLOSURE[$_ci]}" = "$f" ] && { _ec_i="$_ci"; break; }
        done
        if [ "$_ec_i" -lt 0 ]; then
            die_infra "the executable closure lists $f, which is not a member of CLOSURE -- the two lists have drifted and no protocol inference can be made from them"
        fi
        b="${B_VOID[$_ec_i]}"; [ -n "$b" ] || b=MISSING
        h="${H_VOID[$_ec_i]}"; [ -n "$h" ] || h=MISSING
        { [ "$B_ERR" -eq 1 ] || [ "$H_ERR" -eq 1 ]; } && { b=MISSING; h=MISSING; }
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
        replacement_state_still_holds
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
contract_of() {  # $1 = tree root, $2 = side; ANSWERS IN $CONTRACT_RESULT
    # SUPERVISED, IN THIS SHELL, AND BEFORE ANY CORPUS EXISTS (section 55).
    #
    # This function EXECUTES cache_schema.py from the tree under test --
    # `PRODUCER_CONTRACT_DIGEST` is `_producer_contract_digest()`, a call, so
    # there is no inert route to the value and reading it means running the
    # module. It nevertheless ran in a `$(...)` command substitution with NO
    # timeout and no pid recorded anywhere, while the gate's published ceiling
    # says 600s: a module that blocked on import held the gate open forever, and
    # one that returned the expected constants could ALSO edit the corpus on the
    # way, because the import ran after the faithfulness and closure checks but
    # BEFORE both cache builds and both resolver walks. Both walks would then
    # agree over bytes the named commit does not carry (Codex design review,
    # section 55, [high]; the same call site imported from the live tree before
    # section 54, so this is not a section 54 regression).
    #
    # THREE CHANGES, AND THE THIRD IS THE ONE THAT MATTERS. It is bounded like
    # every other phase; it runs in the parent shell so its pid reaches
    # `cleanup`, exactly as section 53 had to do for `proto_of` and for the same
    # reason; and it is called BEFORE the full checkouts exist, so a synchronous
    # write during import has no corpus to reach -- only the narrow tree, which
    # is thrown away and never walked.
    #
    # WHAT THIS DOES NOT CLOSE, STATED RATHER THAN IMPLIED. A module that
    # `setsid`s its own descendant escapes the process group this reaps, so it
    # can outlive the probe, wait for the checkout below to appear, and write
    # into it. Reordering and supervision NARROW that from "write directly" to
    # "poll for a directory and win a race against the walks"; they do not
    # remove it, and claiming otherwise would be exactly the confident-wrong
    # answer this file keeps having to repair.
    # -> XREF: [`TODO-06 section 56`](#56-the-resolver-walks-leak-what-the-tree-spawns-on-the-path-where-they-succeed)
    #    owns what the tree spawns and survives on the SUCCESS path; the
    #    detached-descendant residue here is the same question at a second call
    #    site and belongs with it, not to a second half-fix here.
    CONTRACT_RESULT=UNREADABLE
    local _crec _crb _crc
    probe_budget "create the file its contract record travels on"
    _crec="$(timeout --foreground -s KILL "$PROBE_BUDGET" mktemp "$TMP_DIR/contract.XXXXXX" 2>/dev/null)" \
        || return 0
    budget_left
    if [ "$BUDGET_LEFT" = "?" ] || [ "$BUDGET_LEFT" -le 0 ]; then
        # REFUSE TO LAUNCH RATHER THAN LAUNCH WITH A FLOOR. `remaining()` never
        # answers less than 1 -- correct for a single `timeout` argument, wrong
        # as a decision -- so a phase could still START after the deadline had
        # passed. Section 52 gave the git probes this refusal; the python phases
        # were left out of it, which is the residue this item names.
        bounded_fs rm -f "$_crec"
        die_infra "the gate's budget expired before the $2 producer contract identity could be read, so the probe was never started and no identity is being guessed from it"
    fi
    # Same closure as `proto_of`, and for the same reason: this executes
    # cache_schema.py from the tree under test.
    setsid timeout -s KILL "$BUDGET_LEFT" \
        python3 - "$1" 3>"$_crec" {TMP_FD}<&- >/dev/null 2>/dev/null <<'PY' &
import importlib.util, os, pathlib, sys
# THE RECORD LEAVES ON FD 3, and the subject path is scrubbed from argv before
# the tree's module is executed -- the same two properties `proto_of` had to be
# repaired into having, for the same reasons (sections 50 and 53).
sys.stdout = os.fdopen(3, "w", encoding="utf-8", buffering=1)
tg = pathlib.Path(sys.argv[1]) / "scripts/todo-graph"
sys.argv = [sys.argv[0]]
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
    _crb=$!
    WALK_PIDS="$_crb"
    wait "$_crb"
    _crc=$?
    WALK_WAITED="$_crb"
    # THE SUCCESS PATH REAPS, for the reason section 53 established at the other
    # call site: `timeout` bounds the probe, never what the imported module
    # spawned, and a module that starts a child and returns normally leaves it
    # behind at a clean exit.
    reap_walk_group "$_crb"
    WALK_PIDS=""
    WALK_WAITED=""
    if [ "$_crc" -ne 0 ]; then
        # The probe reports ABSENT/UNREADABLE itself, so a nonzero status is the
        # interpreter failing to run at all -- including 124/137, the budget
        # killing an import that would not finish.
        bounded_fs rm -f "$_crec"
        CONTRACT_RESULT=UNREADABLE
        return 0
    fi
    if [ ! -s "$_crec" ]; then
        bounded_fs rm -f "$_crec"
        CONTRACT_RESULT=UNREADABLE
        return 0
    fi
    local _cline
    # THE FIRST LINE ONLY, so an `atexit` handler can append but not replace.
    probe_budget "read the producer contract record it just wrote"
    _cline="$(timeout --foreground -s KILL "$PROBE_BUDGET" head -n 1 "$_crec" 2>/dev/null)" \
        || { bounded_fs rm -f "$_crec"; CONTRACT_RESULT=UNREADABLE; return 0; }
    bounded_fs rm -f "$_crec"
    [ -n "$_cline" ] || { CONTRACT_RESULT=UNREADABLE; return 0; }
    CONTRACT_RESULT="$_cline"
}
contract_of "$BASE_MIN_ADDR" base
BASE_CONTRACT="$CONTRACT_RESULT"
min_tree_still_matches "$BASE_MIN" "$BASE_SHA" base "$BASE_MIN_FD"
contract_of "$HEAD_MIN_ADDR" head
HEAD_CONTRACT="$CONTRACT_RESULT"
min_tree_still_matches "$HEAD_MIN" "$HEAD_RESOLVED" head "$HEAD_MIN_FD"
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
# ONLY NOW ARE THE FULL TREES CHECKED OUT (section 55).
#
# Everything above reads the protocol, the closure and the producer contract,
# all of which live in the narrow trees assembled from each commit's own blobs.
# What follows is the first thing that needs a REPOSITORY: the producer
# differential, the cache builds and both resolver walks read `todo/**` and
# resolve paths against a repo root. A range that never gets here -- which is
# most of them, because most pushes do not touch the resolver closure -- now
# pays nothing for either checkout.
#
# MEASURED on this repository over a byte-identical todo-only range in a fixture
# clone (`scripts/todo-graph/tests/measure_gate_fastpath.sh`): 129 git
# invocations and 5,154 file creations across two 87 MiB checkouts before. The
# after numbers are recorded in section 55's Notes. The checkouts were the
# dominant term and neither was read.
#
# THE ORDER IS ALSO A CONTAINMENT PROPERTY, not only a cost one: `contract_of`
# executes a module from the tree under test, and it now runs while there is no
# corpus for a synchronous write during that import to reach.
#
# SECTION 54'S PROPERTY IS UNCHANGED. Both sides are still materialized from a
# named commit and never from the working tree, and `tree_is_faithful` still
# adjudicates each checkout the moment it exists.
# THE VALUE PASSED IS THE ONE THAT WAS CHECKED. These two sites inspected
# `BUDGET_LEFT` and then took a SECOND clock reading through `remaining()`,
# which floors an expired or unreadable budget to 1 -- so a deadline crossing
# between the check and the launch handed the checkout a fresh second anyway,
# and section 55's own claim that no floor is left as a decision was false while
# it was written (Codex adversarial, section 55 round 9, [medium]).
phase_budget "base worktree materialization"
WT_ATTEMPTED+=("$BASE_TREE")
timeout --foreground -s KILL "$PHASE_BUDGET" \
    git -c "core.hooksPath=$NOHOOKS_DIR" worktree add --detach "$BASE_TREE" "$BASE_SHA" >/dev/null 2>&1 \
    || die_infra "cannot materialize base worktree at $BASE_SHA (the checkout failed, or the gate's remaining budget expired while it ran)"
tree_is_faithful "$BASE_TREE" "$BASE_SHA" base

phase_budget "head worktree materialization"
WT_ATTEMPTED+=("$HEAD_TREE")
timeout --foreground -s KILL "$PHASE_BUDGET" \
    git -c "core.hooksPath=$NOHOOKS_DIR" worktree add --detach "$HEAD_TREE" "$HEAD_RESOLVED" >/dev/null 2>&1 \
    || die_infra "cannot materialize the head worktree at $HEAD_RESOLVED (the checkout failed, or the gate's remaining budget expired while it ran)"
tree_is_faithful "$HEAD_TREE" "$HEAD_RESOLVED" head
log "head $HEAD_RESOLVED walked from a materialized checkout at $HEAD_TREE"

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
PRODUCER_BASE="$(bounded_git rev-parse --quiet --verify "$BASE_SHA:scripts/todo-graph/build.py" || echo MISSING)"
PRODUCER_HEAD="$(bounded_git rev-parse --quiet --verify "$HEAD_RESOLVED:scripts/todo-graph/build.py" || echo MISSING)"
PRODUCER_DIFF_OK=0
# THE SKIP IS ALSO CONDITIONED ON THE CONTRACT. A byte-identical build.py still
# emits a DIFFERENT artifact identity when cache_schema moved under it (the
# digest is declared there, not here), and the per-side caches that divergence
# forces are only sound because this differential proves their populations
# equal -- so skipping here would leave the two walks comparing unproven inputs.
if [ "$PRODUCER_BASE" = "$PRODUCER_HEAD" ] && [ "$CONTRACT_DIVERGED" -eq 0 ]; then
    log "producer differential: skipped, build.py is byte-identical base..head."
elif [ ! -f "$BASE_TREE/scripts/todo-graph/producer_differential.py" ] \
        && [ ! -f "$HEAD_TREE/scripts/todo-graph/producer_differential.py" ]; then
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
    phase_budget "producer differential"
    setsid timeout -s KILL "$PHASE_BUDGET" python3 \
        "$HEAD_TREE/scripts/todo-graph/producer_differential.py" \
        "$BASE_TREE" "$HEAD_TREE" --strict >"$TMP_DIR/producer.log" 2>&1 &
    PROD_PID=$!
    WALK_PIDS="$PROD_PID"
    wait "$PROD_PID"; PROD_RC=$?; WALK_WAITED="$PROD_PID"
    # AND THE SUCCESS PATH REAPS. `timeout` bounds the process this phase
    # LAUNCHED; it does not bound what that process leaves behind, and a phase
    # that returns NORMALLY clears `WALK_PIDS` without signalling anything --
    # so `cleanup`, which is the only other thing that ever signals, has
    # nothing left to find (section 56).
    reap_walk_group "$PROD_PID"
    WALK_PIDS=""
    WALK_WAITED=""
    phase_budget "log read"
    timeout --foreground -s KILL "$PHASE_BUDGET" sed 's/^/    /' "$TMP_DIR/producer.log"
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
phase_budget "head cache build"
setsid timeout -s KILL "$PHASE_BUDGET" \
    python3 "$HEAD_TREE/scripts/todo-graph/build.py" --quiet --output "$CACHE_ABS" \
    >"$TMP_DIR/build.log" 2>&1 &
CACHE_PID=$!
WALK_PIDS="$CACHE_PID"
wait "$CACHE_PID"; CACHE_RC=$?; WALK_WAITED="$CACHE_PID"
# AND THE SUCCESS PATH REAPS. `timeout` bounds the process this phase
# LAUNCHED; it does not bound what that process leaves behind, and a phase
# that returns NORMALLY clears `WALK_PIDS` without signalling anything --
# so `cleanup`, which is the only other thing that ever signals, has
# nothing left to find (section 56).
reap_walk_group "$CACHE_PID"
WALK_PIDS=""
WALK_WAITED=""
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
# the head corpus in both branches ($HEAD_TREE), so the only thing that varies
# is which producer wrote the bytes -- which is exactly what the differential
# above measured.
BASE_CACHE_ABS="$CACHE_ABS"
if [ "$CONTRACT_DIVERGED" -eq 1 ]; then
    if [ "$PRODUCER_DIFF_OK" -ne 1 ]; then
        die_infra "the producer contract identity diverged base..head, so the base walk needs a cache of its own -- but the producer differential did not pass, so nothing establishes that the two caches carry the same stamped-ref population. Refusing to differential two walks over unproven inputs."
    fi
    BASE_CACHE_ABS="$TMP_DIR/todo-cache-base.json"
    # --root/--repo-root EXPLICITLY at $HEAD_TREE: the base build.py lives in
    # the base worktree and would otherwise default to the BASE corpus, which
    # would make the two walks read different TODO text and attribute every
    # corpus edit to the resolver.
    phase_budget "base cache build"
    setsid timeout -s KILL "$PHASE_BUDGET" \
        python3 "$BASE_TREE/scripts/todo-graph/build.py" --quiet \
        --root "$HEAD_TREE/todo" --repo-root "$HEAD_TREE" \
        --output "$BASE_CACHE_ABS" >"$TMP_DIR/build-base.log" 2>&1 &
    BCACHE_PID=$!
    WALK_PIDS="$BCACHE_PID"
    wait "$BCACHE_PID"; BCACHE_RC=$?; WALK_WAITED="$BCACHE_PID"
    # AND THE SUCCESS PATH REAPS. `timeout` bounds the process this phase
    # LAUNCHED; it does not bound what that process leaves behind, and a phase
    # that returns NORMALLY clears `WALK_PIDS` without signalling anything --
    # so `cleanup`, which is the only other thing that ever signals, has
    # nothing left to find (section 56).
    reap_walk_group "$BCACHE_PID"
    WALK_PIDS=""
    WALK_WAITED=""
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
mono_now; WALK_START="$MONO_NOW"
[ -n "$WALK_START" ] || die_infra "cannot read a monotonic clock before the resolver walks -- the gate's own deadline machinery is what failed here, and no classification is being guessed from it"

phase_budget "base resolver walk"
STUB_LINT_CACHE="$BASE_CACHE_ABS" STUB_LINT_REPO_ROOT="$HEAD_TREE" \
    setsid timeout -s KILL "$PHASE_BUDGET" python3 \
    "$BASE_TREE/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$BASELINE" >"$TMP_DIR/base-walk.log" 2>&1 &
BASE_PID=$!
WALK_PIDS="$BASE_PID"
phase_budget "head resolver walk"
STUB_LINT_CACHE="$CACHE_ABS" STUB_LINT_REPO_ROOT="$HEAD_TREE" \
    setsid timeout -s KILL "$PHASE_BUDGET" python3 \
    "$HEAD_TREE/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$HEADSHOT" >"$TMP_DIR/head-walk.log" 2>&1 &
HEAD_PID=$!
WALK_PIDS="$WALK_PIDS $HEAD_PID"

# REAP BOTH, ALWAYS. `wait`ing on only the first and bailing on its failure
# would leave the other walk running against a $TMP_DIR the EXIT trap is about
# to delete -- an orphan writing into a removed directory, and a exit status
# nobody read. Each `timeout` kills its own child at the budget, and both are
# waited on here before any verdict is formed.
# EACH LEADER IS MARKED WAITED THE MOMENT ITS OWN `wait` RETURNS, not when the
# pair is done. `wait "$HEAD_PID"` can block for the rest of the budget, and
# for all of it BASE_PID sat in WALK_PIDS looking active -- so a TERM in that
# window sent `cleanup` down the positive-pid fallback for a pid the kernel
# had already released (Codex consistency, section 53 review, [high]).
# AND EACH LEADER'S GROUP IS REAPED THE MOMENT ITS OWN `wait` RETURNS, not
# once the pair is done. Reaping both after the second wait leaves the BASE
# walk's surviving members running for the whole of the HEAD budget -- and
# `$BASELINE` is a path the tree knows, written by the base walk and read by
# the comparison phase below, so a descendant that outlives its leader can
# rewrite a verdict INPUT on the ordinary success path. The trap covers
# cancellation and does nothing here (Codex design review, section 56, [high]).
wait "$BASE_PID"; BASE_RC=$?; WALK_WAITED="$BASE_PID"
reap_walk_group "$BASE_PID"
# UNTRACKED ONLY AFTER ITS OWN REAP, and the head walk stays tracked alone, so
# a trap landing in the head wait still reaches the one leader that is live.
WALK_PIDS="$HEAD_PID"
WALK_WAITED=""
wait "$HEAD_PID"; HEAD_RC=$?; WALK_WAITED="$HEAD_PID"
reap_walk_group "$HEAD_PID"
WALK_PIDS=""
WALK_WAITED=""
mono_now; WALK_END="$MONO_NOW"
[ -n "$WALK_END" ] || die_infra "cannot read a monotonic clock after the resolver walks -- the gate's own deadline machinery is what failed here, and no classification is being guessed from it"
WALK_SECS=$(( WALK_END - WALK_START ))

# 124 is timeout(1)'s "the budget fired". Report it as its own thing: it is not
# a resolver defect and must never be read as one.
for pair in "BASE:$BASE_RC:$TMP_DIR/base-walk.log" "HEAD:$HEAD_RC:$TMP_DIR/head-walk.log"; do
    side="${pair%%:*}"; rest="${pair#*:}"; rc="${rest%%:*}"; logf="${rest#*:}"
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
        die_infra "the $side walk exceeded the ${BUDGET_SECS}s budget and was killed. The gate is approaching the CI job ceiling: shard the corpus or raise IDENTITY_GATE_BUDGET_SECS deliberately, but do not discover this as a job timeout."
    fi
    if [ "$rc" -ne 0 ]; then
        phase_budget "failed-walk log read"
        timeout --foreground -s KILL "$PHASE_BUDGET" sed 's/^/    /' "$logf" >&2 || true
        die_infra "the $side resolver could not complete its walk (rc=$rc)"
    fi
done
phase_budget "log read"
timeout --foreground -s KILL "$PHASE_BUDGET" sed 's/^/    /' "$TMP_DIR/base-walk.log"
phase_budget "log read"
timeout --foreground -s KILL "$PHASE_BUDGET" sed 's/^/    /' "$TMP_DIR/head-walk.log"
log "both walks finished in ${WALK_SECS}s of the ${BUDGET_SECS}s budget."

log "comparing the two snapshots (--strict)..."
# THE ARGUMENTS ARE PREPARED IN THEIR OWN BOUNDED PHASE, AND THE BUDGET IS
# REFRESHED AFTERWARDS. Written inline in the argument list, these two
# substitutions were evaluated BEFORE the supervised `timeout` process started,
# so they ran outside its bound entirely -- and the comparison then launched on
# a budget measured before they had spent any of it (Codex adversarial, section
# 55 round 7, [medium]). Two phases, each measured when it starts.
phase_budget "comparison arguments"
_CMP_BASE_HALVES="$(timeout --foreground -s KILL "$PHASE_BUDGET" \
    python3 -c 'import base64,json,sys; print(base64.b64encode(json.dumps([json.loads(base64.b64decode(sys.argv[1])), json.loads(base64.b64decode(sys.argv[2]))]).encode()).decode())' "$BASE_PRE_B64" "$BASE_POST_B64")" \
    || die_infra "could not encode the base bucket halves for the comparison (the encoder failed, or the gate's remaining budget expired while it ran)"
phase_budget "comparison arguments"
_CMP_HEAD_HALVES="$(timeout --foreground -s KILL "$PHASE_BUDGET" \
    python3 -c 'import base64,json,sys; print(base64.b64encode(json.dumps([json.loads(base64.b64decode(sys.argv[1])), json.loads(base64.b64decode(sys.argv[2]))]).encode()).decode())' "$HEAD_PRE_B64" "$HEAD_POST_B64")" \
    || die_infra "could not encode the head bucket halves for the comparison (the encoder failed, or the gate's remaining budget expired while it ran)"
phase_budget "snapshot comparison"
setsid timeout -s KILL "$PHASE_BUDGET" \
    python3 "$HEAD_TREE/scripts/todo-graph/corpus_resolution_snapshot.py" \
    compare "$BASELINE" "$HEADSHOT" --strict \
    --base-halves-b64 "$_CMP_BASE_HALVES" \
    --head-halves-b64 "$_CMP_HEAD_HALVES" \
    >"$TMP_DIR/compare.log" 2>&1 &
CMP_PID=$!
WALK_PIDS="$CMP_PID"
wait "$CMP_PID"; CMP_RC=$?; WALK_WAITED="$CMP_PID"
# AND THE SUCCESS PATH REAPS. `timeout` bounds the process this phase
# LAUNCHED; it does not bound what that process leaves behind, and a phase
# that returns NORMALLY clears `WALK_PIDS` without signalling anything --
# so `cleanup`, which is the only other thing that ever signals, has
# nothing left to find (section 56).
reap_walk_group "$CMP_PID"
WALK_PIDS=""
WALK_WAITED=""
phase_budget "log read"
timeout --foreground -s KILL "$PHASE_BUDGET" sed 's/^/    /' "$TMP_DIR/compare.log"

case "$CMP_RC" in
    0)
        log "PASS -- every prior verdict survived the resolver change."
        replacement_state_still_holds
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
