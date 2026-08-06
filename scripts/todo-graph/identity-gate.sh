#!/usr/bin/env bash
# ============================================================================
# identity-gate.sh -- run corpus_resolution_snapshot.py as a GATE instead of
# as a thing a human remembers to invoke.
#
# WHAT THIS IS. A RESOLVER-CODE DIFFERENTIAL gate. It answers exactly one
# question: "does the resolver code in this range still produce the same
# verdict for every symbol ref it produced a verdict for before?" It is NOT a
# replacement for the full manual snapshot contract, and section 16 documents
# that boundary rather than letting a reader assume the wider claim.
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
#   3  INFRASTRUCTURE -- the gate could not run: no usable base, a protocol
#      migration bundled with a resolver change, a cache/baseline failure, or a
#      worktree it could not materialize
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
#                         moves the shared input under both sides equally --
#                         which is exactly the blind spot section 18 owns.
#       identity-gate.sh  is the driver; a change to it must re-run the gate.
#
# Contrast scripts/lint/check_stub_behind_stamp.py, which is deliberately
# ABSENT: it is neither executed by the walks nor an input to them, so listing
# it would force a walk that adjudicates nothing about it. That is the same
# false-coverage test applied consistently, not a different rule.
CLOSURE=(
    "scripts/todo-graph/corpus_resolution_snapshot.py"
    "scripts/todo-graph/ref_resolution.py"
    "scripts/todo-graph/resolve_symbol.py"
    "scripts/todo-graph/cache_schema.py"
    "scripts/todo-graph/build.py"
    "scripts/todo-graph/identity-gate.sh"
)

BASE_SHA=""
BASE_EXPLICIT=0
HEAD_SHA="HEAD"
KEEP_TMP=0

die_infra() {
    printf '[identity-gate] INFRASTRUCTURE: %s\n' "$1" >&2
    printf '[identity-gate] this is NOT a pass -- the gate could not run.\n' >&2
    exit 3
}

log() { printf '[identity-gate] %s\n' "$1"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --base) BASE_SHA="${2:-}"; BASE_EXPLICIT=1; shift 2 || die_infra "--base needs a value" ;;
        --head) HEAD_SHA="${2:-}"; shift 2 || die_infra "--head needs a value" ;;
        --keep-tmp) KEEP_TMP=1; shift ;;
        *) die_infra "unknown argument: $1" ;;
    esac
done

cd "$REPO_ROOT" || die_infra "cannot cd to repo root $REPO_ROOT"

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
# Early exit ONLY when the whole closure is byte-identical.
# ---------------------------------------------------------------------------
# The head side hashes the WORKING-TREE file, not the committed blob. In CI
# they are the same thing; locally they are not, and hashing the commit would
# let an uncommitted resolver edit early-exit unexamined -- the gate must
# measure the code it is actually about to execute.
CLOSURE_CHANGED=0
for f in "${CLOSURE[@]}"; do
    b="$(git rev-parse --quiet --verify "$BASE_SHA:$f" 2>/dev/null || echo MISSING)"
    h="$(git hash-object "$REPO_ROOT/$f" 2>/dev/null || echo MISSING)"
    if [ "$b" != "$h" ]; then
        log "closure changed: $f"
        CLOSURE_CHANGED=1
    fi
done
if [ "$CLOSURE_CHANGED" -eq 0 ]; then
    log "resolver closure byte-identical base..head; nothing to differentiate."
    exit 0
fi

TMP_DIR="$(mktemp -d -t identity-gate.XXXXXX)" || die_infra "mktemp failed"
BASE_TREE="$TMP_DIR/base"
cleanup() {
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
    python3 - "$1" <<'PY' 2>/dev/null || echo UNREADABLE
import importlib.util, json, sys, pathlib
root = pathlib.Path(sys.argv[1])
tg = root / "scripts/todo-graph"
sys.path.insert(0, str(tg))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, tg / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


try:
    schema = _load("corpus_resolution_snapshot").SNAPSHOT_SCHEMA
    buckets = _load("ref_resolution").ALL_BUCKETS
except Exception:
    print("UNREADABLE")
    raise SystemExit(0)
if not isinstance(schema, int) or not isinstance(buckets, (list, tuple)):
    print("UNREADABLE")
    raise SystemExit(0)
# ORDER-SENSITIVE: the bucket tuple is a published contract, so a reorder is a
# protocol change even when the membership is identical.
print("%d|%s" % (schema, json.dumps(list(buckets))))
PY
}
BASE_PROTO="$(proto_of "$BASE_TREE")"
HEAD_PROTO="$(proto_of "$REPO_ROOT")"
# `?` means a constant could not be READ -- a missing or renamed snapshot tool,
# or a declaration this parser no longer recognises. It must be an
# INFRASTRUCTURE failure, not a value: treated as a value it merely differs
# from the other side, which sent the run down the protocol-migration path and
# exited 0 having built no cache and walked nothing (Codex adversarial, s16).
for prot in "$BASE_PROTO" "$HEAD_PROTO"; do
    case "$prot" in
        UNREADABLE|*"?"*)
            die_infra "cannot read the snapshot protocol constants (got '$prot') -- the snapshot tool is missing or renamed, or declares SNAPSHOT_SCHEMA/ALL_BUCKETS in a shape this gate cannot parse" ;;
    esac
done
BASE_SCHEMA="${BASE_PROTO%%|*}"; HEAD_SCHEMA="${HEAD_PROTO%%|*}"
BASE_BUCKETS="${BASE_PROTO#*|}"; HEAD_BUCKETS="${HEAD_PROTO#*|}"

if [ "$BASE_BUCKETS" != "$HEAD_BUCKETS" ]; then
    # ALL_BUCKETS is defined INSIDE ref_resolution.py, so a bucket migration
    # NECESSARILY edits the very file the separation rule would need to hold
    # byte-identical -- the rule is unsatisfiable in the current layout, and
    # pretending otherwise would either reject every bucket migration or wave
    # a bundled resolver change through (Codex design review, section 16). It
    # fails closed and names the prerequisite instead of guessing.
    die_infra "ALL_BUCKETS changed base..head. This gate cannot separate a bucket migration from a resolver change while ALL_BUCKETS lives inside ref_resolution.py. Extract the protocol constants into their own module first -- filed as a section 16 follow-up item."
fi

if [ "$BASE_SCHEMA" != "$HEAD_SCHEMA" ]; then
    # THERE IS NO PROTOCOL-ONLY EXIT, deliberately. An earlier revision let a
    # schema-only migration exit 0 after checking that ref_resolution.py and
    # resolve_symbol.py were unchanged. That was fail-open: SNAPSHOT_SCHEMA
    # lives in corpus_resolution_snapshot.py ALONGSIDE `collect()` and
    # `--strict`, both verdict-affecting, so a schema bump bundled with a
    # `collect()` change took the exit and walked nothing (Codex adversarial,
    # section 16). It is the same constant-beside-logic problem ALL_BUCKETS has
    # above, and it has the same prerequisite -- so it gets the same answer,
    # rather than a second mechanism that is wrong in a subtler way.
    die_infra "SNAPSHOT_SCHEMA changed base..head ($BASE_SCHEMA -> $HEAD_SCHEMA). This gate cannot separate a protocol migration from a resolver change while SNAPSHOT_SCHEMA lives in corpus_resolution_snapshot.py beside collect() and --strict. Extract the protocol constants into their own module first -- filed as a section 18 follow-up item."
fi

# ---------------------------------------------------------------------------
# Build the cache ONCE. Both walks read this exact file, which is what makes
# the key set stable. Absolute paths: the base walk runs with a different CWD,
# and a relative STUB_LINT_CACHE would silently resolve against the wrong tree.
# ---------------------------------------------------------------------------
CACHE_ABS="$TMP_DIR/todo-cache.json"
python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet --output "$CACHE_ABS" \
    >"$TMP_DIR/build.log" 2>&1 \
    || die_infra "cache build failed (see $TMP_DIR/build.log)"
[ -s "$CACHE_ABS" ] || die_infra "cache build produced an empty file"

BASELINE="$TMP_DIR/baseline.json"

log "walking with BASE resolver code..."
STUB_LINT_CACHE="$CACHE_ABS" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    python3 "$BASE_TREE/scripts/todo-graph/corpus_resolution_snapshot.py" \
    write "$BASELINE" >"$TMP_DIR/base-walk.log" 2>&1
BASE_RC=$?
if [ "$BASE_RC" -ne 0 ]; then
    sed 's/^/    /' "$TMP_DIR/base-walk.log" >&2 || true
    die_infra "the BASE resolver could not complete its walk (rc=$BASE_RC)"
fi
sed 's/^/    /' "$TMP_DIR/base-walk.log"

log "comparing with HEAD resolver code (--strict)..."
STUB_LINT_CACHE="$CACHE_ABS" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
    python3 "$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py" \
    compare "$BASELINE" --strict >"$TMP_DIR/compare.log" 2>&1
CMP_RC=$?
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
    *)
        die_infra "compare exited with an undocumented status (rc=$CMP_RC)"
        ;;
esac
