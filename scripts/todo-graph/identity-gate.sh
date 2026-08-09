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
    python3 - "$1" <<'PY' 2>/dev/null || echo UNREADABLE
import base64, hashlib, importlib.util, json, sys, pathlib
root = pathlib.Path(sys.argv[1])
tg = root / "scripts/todo-graph"
sys.path.insert(0, str(tg))
source = "data"


def _load(name):
    spec = importlib.util.spec_from_file_location(name, tg / (name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
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
IFS='|' read -r BASE_SCHEMA BASE_PRE BASE_POST BASE_SOURCE BASE_PRE_B64 BASE_POST_B64 BASE_MIG_B64 <<< "$BASE_PROTO"
IFS='|' read -r HEAD_SCHEMA HEAD_PRE HEAD_POST HEAD_SOURCE HEAD_PRE_B64 HEAD_POST_B64 HEAD_MIG_B64 <<< "$HEAD_PROTO"
for _f in "$BASE_SCHEMA" "$BASE_PRE" "$BASE_POST" "$BASE_SOURCE" \
          "$BASE_PRE_B64" "$BASE_POST_B64" "$BASE_MIG_B64" \
          "$HEAD_SCHEMA" "$HEAD_PRE" "$HEAD_POST" "$HEAD_SOURCE" \
          "$HEAD_PRE_B64" "$HEAD_POST_B64" "$HEAD_MIG_B64"; do
    [ -n "$_f" ] || die_infra "the protocol line is malformed (base='$BASE_PROTO' head='$HEAD_PROTO')"
done

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
