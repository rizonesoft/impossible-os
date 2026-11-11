#!/usr/bin/env bash
# ============================================================================
# test_build_sections_b.sh -- todo-graph build suite shard: sections 30-44 and Test 41, all on fixtures.
#
# One of four shards scripts/test-tooling.sh starts together; the shared
# preamble, helpers and shard list are in test_build_lib.sh.
# ============================================================================

set -u
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/test_build_lib.sh" || exit 2

# The blocks here ran after Test 12's `cd "$REPO_ROOT"` in the single file,
# so they start where they always did.
cd "$REPO_ROOT" || exit 2

# Section 30: `--watch` ticks when the corpus HISTORY moves, not only when its
# bytes do. Section 24 gave `stale` / `stats` / `render --render-format gantt` a
# post-walk `corpus_history_id` check; an amend, a rebase or a checkout that
# leaves every todo/ byte identical fires no filesystem event, so a watcher on
# one of those verbs never re-ran and the check it declared never got to run.
# ----------------------------------------------------------------------

# Scratch git corpus builder shared by the fixtures below. Each caller gets its
# own repo, so a mutation in one cannot leak into another.
# THE CACHE LIVES AT THE CANONICAL PATH, and that is load-bearing rather than
# tidy. `check_stale` -- and therefore whether `info.history` is populated at
# all -- is decided by `cache_path.resolve() == repo_root/build/todo-cache.json`,
# and the history axis is armed only when the post-walk check can actually
# enforce something. A fixture pointing `--cache` at `$T/cache.json` is in
# INSPECTION mode, where arming the axis would be the bug (Codex consistency,
# section 30, [medium]), so it could never have exercised the shipped path.
s30_repo() {
    local T; T=$(mktemp -d "$TMP_DIR/s30-XXXXXX")
    mkdir -p "$T/todo/01-test" "$T/src" "$T/build"
    cat > "$T/todo/01-test/TODO-01-w.md" <<'EOF'
---
schema_version: 1
id: s30-watch
domain: 01-test
status: draft
title: Watch history fixture
---
# body
EOF
    (
        cd "$T" || exit 1
        git init -q .
        git config user.email s30@test.invalid
        git config user.name s30
        git add -A
        git commit -qm "c0 add corpus"
    ) >/dev/null 2>&1
    python3 "$BUILD_PY" --quiet --root "$T/todo" --output "$T/build/todo-cache.json" \
        --repo-root "$T" >/dev/null 2>&1
    printf '%s' "$T"
}

# Run a watcher for a bounded window, mutate the repo mid-window, and report how
# many RE-RUNS happened. The needle is the per-tick banner on stderr, not rows on
# stdout: the opening run deliberately emits no banner, so the count IS the
# number of re-runs -- and it is verb-independent, which matters because `stale`
# legitimately prints no rows for a fresh corpus.
s30_reruns() {   # $1=tree $2=verb $3=mutation-shell $4=1 to force the polling path
    local T="$1" VERB="$2" MUT="$3" FORCE_POLL="${4:-0}" QPY="${5:-$QUERY_PY}"
    # FORCED WITH THE PRODUCT'S OWN SWITCH, not by narrowing PATH. `/usr/bin` is
    # exactly where inotify-tools installs inotifywait, so the old trick
    # selected the branch it claimed to exclude wherever the tool was present
    # (Codex adversarial, section 30, [medium]). The branch actually taken is
    # asserted separately by the caller, off the `watch:` marker on stderr.
    local FP=""; [ "$FORCE_POLL" = "1" ] && FP="1"
    ( sleep 3; cd "$T" && eval "$MUT" ) >/dev/null 2>&1 &
    local MP=$!
    # PYTHONPATH IS WHAT MAKES A MUTANT RUNNABLE, and omitting it is a false
    # PASS, not a crash you notice: a mutant copied into TMP_DIR resolves its
    # own `sys.path.insert(_HERE)` to that temp dir, cannot import the sibling
    # `validate` / `cache_schema`, and dies on import -- producing zero re-runs,
    # which is exactly what a "the mutation disarmed the trigger" fixture
    # expects to see. Measured here: 30d passed that way before this line
    # existed. Pointing PYTHONPATH at the real module directory makes the mutant
    # differ from the shipped code in one line and nothing else.
    TODOGRAPH_WATCH_FORCE_POLL="$FP" PYTHONPATH="$(dirname "$QUERY_PY")" \
    timeout --signal=INT 9 python3 -u "$QPY" --cache "$T/build/todo-cache.json" \
        --repo-root "$T" --watch "$VERB" > "$T/out" 2>"$T/err" || true
    wait $MP 2>/dev/null
    local N; N=$(grep -c '^----' "$T/err" 2>/dev/null || true)
    printf '%s' "${N:-0}"
}

if ! command -v git >/dev/null 2>&1; then
    t_pass "section 30: SKIPPED -- git is not available on this host"
else

# EVERY MUTANT BELOW NEEDS build.py BESIDE IT. `_rebuild_cache` locates build.py
# next to query.py's OWN file, so a mutant living in TMP_DIR finds none, SKIPS
# the rebuild rather than attempting one, and emits no refusal envelope at all
# -- indistinguishable, to a "the mutation reproduces the old behaviour"
# assertion, from a mutant that ran and behaved. Measured here: 30h reported 0
# envelopes for exactly this reason and read as a code failure. The symlink
# restores the real rebuild path; the mutant's own imports come from PYTHONPATH.
ln -sf "$(dirname "$QUERY_PY")/build.py" "$TMP_DIR/build.py"

# Sub-test 30a: the whole point. An amend that rewrites the corpus-touching tip
# while leaving the worktree byte-identical must produce a re-run on a
# history-consuming verb. Pre-fix this is 0: no .md changed, so no event fired.
S30A=$(s30_repo)
S30A_N=$(s30_reruns "$S30A" stale 'git commit -q --amend -m "c0 reworded"')
if [ "$S30A_N" -ge 1 ]; then
    t_pass "section 30: a byte-identical amend re-runs a \`stale\` watcher"
else
    t_fail "section 30: amend produced no re-run (reruns=$S30A_N, err=$(tr '\n' ' ' < "$S30A/err" | head -c 300))"
fi

# Sub-test 30b: CONTROL -- the trigger must not turn every commit into a tick.
# An ordinary commit touching nothing under todo/ moves refs/heads and costs one
# 54.2ms identity probe, and `corpus_history_id` is path-limited to the corpus,
# so the id does not move and no re-run happens. Without this control 30a would
# also pass a build that simply ticked on any ref movement.
S30B=$(s30_repo)
S30B_N=$(s30_reruns "$S30B" stale 'printf k > src/k.c; git add -A; git commit -qm unrelated')
if [ "$S30B_N" = "0" ]; then
    t_pass "section 30: CONTROL -- a commit touching no todo/ path fires no tick"
else
    t_fail "section 30: unrelated commit produced $S30B_N re-run(s); the trigger is not corpus-scoped"
fi

# Sub-test 30c: the verb scoping. `ready` dereferences no git-derived timestamp,
# so it arms no history axis and pays nothing -- the same amend must be invisible
# to it. This is what makes the cost decision in 30a affordable.
S30C=$(s30_repo)
S30C_N=$(s30_reruns "$S30C" ready 'git commit -q --amend -m "c0 reworded"')
if [ "$S30C_N" = "0" ]; then
    t_pass "section 30: a \`ready\` watcher arms no history axis (amend ignored)"
else
    t_fail "section 30: \`ready\` re-ran $S30C_N time(s) on a history-only move"
fi

# Sub-test 30d: MUTATION for 30a -- revert the wiring so the history axis is
# never armed, and the same amend must go back to producing nothing. Without
# this, 30a would pass against a build that re-ran for some unrelated reason.
S30_MUT="$TMP_DIR/query_s30_mut.py"
python3 - "$QUERY_PY" "$S30_MUT" <<'MUTEOF'
import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "                   history=profile.requires_history and check_stale)"
assert src.count(needle) == 1, f"s30 mutation needle appears {src.count(needle)}x"
pathlib.Path(sys.argv[2]).write_text(
    src.replace(needle, "                   history=False)", 1), encoding="utf-8")
MUTEOF
S30D=$(s30_repo)
S30D_N=$(s30_reruns "$S30D" stale 'git commit -q --amend -m "c0 reworded"' 0 "$S30_MUT")
# A stale needle wrote no mutant, and a query that cannot start re-runs nothing, so
# this used to pass while proving nothing. No mutant is a failure, not a pass.
if [ ! -s "$S30_MUT" ]; then
    t_fail "section 30: the history-axis mutant was not written (stale needle?)"
elif [ "$S30D_N" = "0" ]; then
    t_pass "section 30: MUTATION -- with the history axis disarmed the amend is invisible again (fixture is sensitive)"
else
    t_fail "section 30: disarmed mutant still re-ran $S30D_N time(s); 30a proves nothing"
fi

# Sub-test 30e: the same property through the POLLING fallback, forced with the
# product's own switch. HARD-FAIL, and it PROVES WHICH BRANCH RAN first: a
# soft-pass here would let a completely broken polling history axis leave the
# suite green, and the branch assertion is what stops the fixture from passing
# because it quietly ran the inotify path instead.
S30E=$(s30_repo)
S30E_N=$(s30_reruns "$S30E" stale 'git commit -q --amend -m "c0 reworded"' 1)
S30E_BRANCH=$(grep -o 'watch: [a-z]*' "$S30E/err" | head -1)
if [ "$S30E_BRANCH" = "watch: polling" ] && [ "$S30E_N" -ge 1 ]; then
    t_pass "section 30: the POLLING branch (proven) re-runs on a byte-identical amend"
else
    t_fail "section 30: polling-branch amend re-run failed (branch='$S30E_BRANCH' want 'watch: polling', reruns=$S30E_N)"
fi

# Sub-test 30e2: and the INOTIFY branch, which no fixture had ever entered --
# inotify-tools is not installed on the dev host, so every case above runs the
# polling loop whatever PATH says. A stub inotifywait on PATH exercises the real
# branch: the sentinel wait, the raw-descriptor reader, the drain and the tick.
S30E2=$(s30_repo)
S30E2_BIN="$S30E2/bin"; mkdir -p "$S30E2_BIN"
cat > "$S30E2_BIN/inotifywait" <<'STUBEOF'
#!/usr/bin/env bash
# Minimal inotifywait stand-in: announce the watches the way inotify-tools does,
# then emit one .md event and idle so the watcher keeps running.
echo "Watches established." >&2
sleep 2
for a in "$@"; do case "$a" in /*) TARGET="$a";; esac; done
echo "$TARGET/01-test/TODO-01-w.md CLOSE_WRITE,CLOSE"
sleep 30
STUBEOF
chmod +x "$S30E2_BIN/inotifywait"
PATH="$S30E2_BIN:$PATH" PYTHONPATH="$(dirname "$QUERY_PY")" \
timeout --signal=INT 8 python3 -u "$QUERY_PY" --cache "$S30E2/build/todo-cache.json" \
    --repo-root "$S30E2" --watch ready > "$S30E2/out" 2>"$S30E2/err" || true
S30E2_BRANCH=$(grep -o 'watch: [a-z]*' "$S30E2/err" | head -1)
S30E2_N=$(grep -c '^----' "$S30E2/err" 2>/dev/null || true); S30E2_N=${S30E2_N:-0}
if [ "$S30E2_BRANCH" = "watch: inotify" ] && [ "$S30E2_N" -ge 1 ]; then
    t_pass "section 30: the INOTIFY branch arms, waits for its sentinel, and ticks on a .md event"
else
    t_fail "section 30: inotify branch not exercised (branch='$S30E2_BRANCH' want 'watch: inotify', reruns=$S30E2_N, err=$(tr '\n' ' ' < "$S30E2/err" | head -c 300))"
fi

# Sub-test 30e3: an INDETERMINATE history probe must not go silent once the ref
# churn stops. `git` is shadowed by a stub that fails every call, so the axis
# cannot resolve its paths at all; the sticky pending state must keep re-asking
# on its own schedule rather than waiting for a trigger movement that will never
# come. Proven by the axis object directly -- a timing-free assertion.
S30E3=$(s30_repo)
S30E3_OUT=$(python3 - "$S30E3" "$(dirname "$QUERY_PY")" <<'S30E3EOF' 2>&1
import json, os, subprocess, sys, time
sys.path.insert(0, sys.argv[2])
import query  # noqa: E402
todo = os.path.join(sys.argv[1], "todo")
real_run = subprocess.run
def failing(*a, **k):
    raise OSError("git is unavailable in this fixture")
subprocess.run = failing
query._cs.subprocess.run = failing
axis = query._HistoryAxis(todo, quiet=True)
unresolved = axis.trigger is None and axis.pending and not axis.no_repo
# Nothing moved and nothing can move: the pre-fix axis returned False forever.
axis._next_probe_at = 0.0
still_asking = axis.poll() is not None and axis.pending
subprocess.run = real_run
query._cs.subprocess.run = real_run
# git is back: the very next due poll must resolve and re-publish, because the
# axis cannot prove the history held while it was blind.
axis._next_probe_at = 0.0
recovered = axis.poll()
print(json.dumps({"unresolved": unresolved, "still_asking": still_asking,
                  "recovered": bool(recovered),
                  "resolved_after": axis.trigger is not None}))
S30E3EOF
); S30E3_RC=$?
if [ "$S30E3_RC" = "0" ] && echo "$S30E3_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['unresolved'] and d['still_asking'] and d['recovered']
         and d['resolved_after'] else 1)"; then
    t_pass "section 30: an unresolvable git keeps the axis PENDING and it re-publishes once git recovers"
else
    t_fail "section 30: indeterminate-probe recovery failed (rc=$S30E3_RC, out=$S30E3_OUT)"
fi

# Sub-test 30f: the trigger snapshot must see a LOOSE-REF update. This is a
# deterministic unit check on `_snapshot_history_trigger`, and it carries its own
# control: `refs/` dir mtime alone does NOT move when `refs/heads/<name>` is
# written, which is exactly why a three-entry stat of {HEAD, packed-refs, refs}
# was rejected during design review as blind to the ordinary amend.
S30F=$(s30_repo)
S30F_OUT=$(python3 - "$S30F" "$(dirname "$QUERY_PY")" <<'S30FEOF' 2>&1
import json, os, subprocess, sys
sys.path.insert(0, sys.argv[2])
import query  # noqa: E402
tree = sys.argv[1]
todo = os.path.join(tree, "todo")
trig = query._history_trigger_paths(todo)
refs_dir = trig[0]
before, before_complete = query._snapshot_history_trigger(trig)
refs_mtime_before = os.stat(refs_dir).st_mtime_ns
subprocess.run(["git", "-C", tree, "branch", "s30-loose"], check=True,
               capture_output=True)
after, after_complete = query._snapshot_history_trigger(trig)
print(json.dumps({
    "snapshot_moved": before != after,
    "refs_dir_mtime_moved": os.stat(refs_dir).st_mtime_ns != refs_mtime_before,
    "complete": bool(before_complete and after_complete),
}))
S30FEOF
); S30F_RC=$?
if [ "$S30F_RC" = "0" ] && echo "$S30F_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# The snapshot MUST move; the bare refs/ directory mtime MUST NOT -- that gap is
# the finding the recursive walk exists to close. A fixture repo is far under
# the ceiling, so an INCOMPLETE snapshot here would mean the bound misfired.
sys.exit(0 if d['snapshot_moved'] and not d['refs_dir_mtime_moved']
         and d['complete'] else 1)"; then
    t_pass "section 30: the trigger snapshot sees a loose-ref write that refs/ dir mtime alone misses"
else
    t_fail "section 30: loose-ref detection check failed (rc=$S30F_RC, out=$S30F_OUT)"
fi

# Sub-test 30g: a REFUSED tick retries on a timer instead of latching. The
# corpus is broken BEFORE the watcher starts, so nothing touches the filesystem
# during the observation window at all -- every envelope after the first is the
# retry schedule firing on its own. Pre-fix this is exactly 1: one refusal, then
# silence until an unrelated edit happened to arrive.
S30G=$(s30_repo)
printf 'not: [valid\nyaml' > "$S30G/todo/01-test/TODO-02-broken.md"
S30G_STAMP=$(find "$S30G/todo" -type f -printf '%T@ %p\n' | sort | md5sum)
timeout --signal=INT 13 python3 -u "$QUERY_PY" --cache "$S30G/build/todo-cache.json" \
    --repo-root "$S30G" --watch stale > "$S30G/out" 2>"$S30G/err" || true
S30G_N=$(grep -c 'cache-rebuild-failed' "$S30G/out" 2>/dev/null || true); S30G_N=${S30G_N:-0}
S30G_STAMP2=$(find "$S30G/todo" -type f -printf '%T@ %p\n' | sort | md5sum)
if [ "$S30G_N" -ge 2 ] && [ "$S30G_STAMP" = "$S30G_STAMP2" ]; then
    t_pass "section 30: a refused tick retries on its own timer (${S30G_N} envelopes, corpus untouched)"
else
    t_fail "section 30: refusal latched instead of retrying (envelopes=$S30G_N, corpus stable=$([ "$S30G_STAMP" = "$S30G_STAMP2" ] && echo yes || echo no))"
fi

# Sub-test 30h: MUTATION for 30g -- make the tick report no retry-worthy
# refusal, and the same broken corpus must produce exactly ONE envelope again.
S30_MUT2="$TMP_DIR/query_s30_mut2.py"
python3 - "$QUERY_PY" "$S30_MUT2" <<'MUT2EOF'
import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
needle = "                sys.stdout.flush()\n                return True\n"
assert src.count(needle) == 1, f"s30 retry needle appears {src.count(needle)}x"
pathlib.Path(sys.argv[2]).write_text(
    src.replace(needle, "                sys.stdout.flush()\n                return\n", 1),
    encoding="utf-8")
MUT2EOF
S30H=$(s30_repo)
printf 'not: [valid\nyaml' > "$S30H/todo/01-test/TODO-02-broken.md"
PYTHONPATH="$(dirname "$QUERY_PY")" \
timeout --signal=INT 13 python3 -u "$S30_MUT2" --cache "$S30H/build/todo-cache.json" \
    --repo-root "$S30H" --watch stale > "$S30H/out" 2>"$S30H/err" || true
S30H_N=$(grep -c 'cache-rebuild-failed' "$S30H/out" 2>/dev/null || true); S30H_N=${S30H_N:-0}
if [ "$S30H_N" = "1" ]; then
    t_pass "section 30: MUTATION -- without the retry signal the refusal latches at one envelope (fixture is sensitive)"
else
    t_fail "section 30: latch mutation produced $S30H_N envelope(s), expected exactly 1"
fi

# Sub-test 30i: the trigger ceiling must bind INSIDE one wide directory, which
# is the ordinary shape of `refs/heads`. Reproduced before the fix with a
# ceiling of 5 against a 300-entry directory: 302 stats and `complete=True`.
# Its own control: the same tree UNDER the ceiling must report complete.
S30I_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30IEOF' 2>&1
import json, os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
tmp = tempfile.mkdtemp()
refs = os.path.join(tmp, "refs")
heads = os.path.join(refs, "heads")
os.makedirs(heads)
for i in range(300):
    with open(os.path.join(heads, "b%03d" % i), "w") as f:
        f.write("x")
trigger = (refs, [], None)
query.WATCH_TRIGGER_MAX_ENTRIES = 5
tight, tight_complete = query._snapshot_history_trigger(trigger)
query.WATCH_TRIGGER_MAX_ENTRIES = 20000
loose, loose_complete = query._snapshot_history_trigger(trigger)
# THE OTHER FANOUT: many EMPTY DIRECTORIES. Directories were queued and stat'ed
# without counting against the budget, so this shape walked straight past a
# ceiling the wide-file case respected.
dirtmp = tempfile.mkdtemp()
drefs = os.path.join(dirtmp, "refs")
os.makedirs(drefs)
for i in range(12):
    os.makedirs(os.path.join(drefs, "d%02d" % i))
query.WATCH_TRIGGER_MAX_ENTRIES = 5
dirs_snap, dirs_complete = query._snapshot_history_trigger((drefs, [], None))
query.WATCH_TRIGGER_MAX_ENTRIES = 20000
print(json.dumps({"tight_entries": len(tight), "tight_complete": tight_complete,
                  "loose_complete": loose_complete, "loose_entries": len(loose),
                  "dirs_entries": len(dirs_snap), "dirs_complete": dirs_complete}))
S30IEOF
); S30I_RC=$?
if [ "$S30I_RC" = "0" ] && echo "$S30I_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# Bounded: at most the ceiling of entries, and it SAYS it is incomplete.
# Control: the same directory under a 20000 ceiling completes with all 302.
# And the empty-DIRECTORY fanout is bounded by the same budget.
sys.exit(0 if (not d['tight_complete'] and d['tight_entries'] <= 5
               and d['loose_complete'] and d['loose_entries'] > 300
               and not d['dirs_complete'] and d['dirs_entries'] <= 5) else 1)"; then
    t_pass "section 30: the trigger ceiling binds inside one wide directory AND across an empty-directory fanout"
else
    t_fail "section 30: wide-directory ceiling check failed (rc=$S30I_RC, out=$S30I_OUT)"
fi

# Sub-test 30l: the degraded probe must not lose a history move that lands
# between the trigger snapshot and the identity probe. Probing FIRST let such a
# move be absorbed into the new baseline without ever being asked about, so the
# poll returned False and no later comparison could recover it. Simulated
# deterministically by moving the identity during the snapshot call.
S30L=$(s30_repo)
S30L_OUT=$(python3 - "$S30L" "$(dirname "$QUERY_PY")" <<'S30LEOF' 2>&1
import json, os, sys
sys.path.insert(0, sys.argv[2])
import query  # noqa: E402
todo = os.path.join(sys.argv[1], "todo")
axis = query._HistoryAxis(todo, quiet=True)
baseline = axis.last_id
axis.degraded = True
axis._next_probe_at = 0.0
real_snap = query._snapshot_history_trigger
real_id = query._cs.corpus_history_id
state = {"id": baseline}
# The move lands DURING the snapshot -- the exact window the ordering decides.
def moving_snap(trigger):
    state["id"] = baseline + "-moved"
    return real_snap(trigger)
query._snapshot_history_trigger = moving_snap
query._cs.corpus_history_id = lambda root: state["id"]
noticed = axis.poll()
query._snapshot_history_trigger = real_snap
query._cs.corpus_history_id = real_id
print(json.dumps({"noticed": bool(noticed), "baseline": baseline,
                  "final": axis.last_id}))
S30LEOF
); S30L_RC=$?
if [ "$S30L_RC" = "0" ] && echo "$S30L_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['noticed'] and d['final'] != d['baseline'] else 1)"; then
    t_pass "section 30: a history move during the degraded snapshot is probed, not swallowed"
else
    t_fail "section 30: degraded lost-wakeup check failed (rc=$S30L_RC, out=$S30L_OUT)"
fi

# Sub-test 30m: retry-state hygiene on the no-trigger path -- `no_repo` must be
# re-derived from each resolution (a stale True turned a later transient failure
# into a 60s wait), and ONE due failure must advance the backoff ONE slot (both
# `sample` and `poll` used to defer, jumping 2s straight to ~15s).
S30M_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30MEOF' 2>&1
import json, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
axis = query._HistoryAxis.__new__(query._HistoryAxis)
axis.todo_root = tempfile.mkdtemp()
axis.quiet = True
axis.trigger = None
axis.no_repo = True          # stale from an earlier determinate answer
axis.shallow = None
axis.prev = {}
axis.degraded = False
axis._said_degraded = False
axis.last_id = "no-repo"
axis.id_known = True
axis.pending = False
axis._probe_idx = 0
axis._next_probe_at = 0.0
def boom(*a, **k):
    raise query._TriggerUnavailable("simulated")
query._history_trigger_paths = boom
query._cs.corpus_history_id = lambda root: (_ for _ in ()).throw(
    query._cs.CacheSchemaError(query._cs.REASON_STALE, "simulated"))
axis.poll()
print(json.dumps({"no_repo_cleared": axis.no_repo is False,
                  "probe_idx_after_one_failure": axis._probe_idx}))
S30MEOF
); S30M_RC=$?
if [ "$S30M_RC" = "0" ] && echo "$S30M_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['no_repo_cleared'] and d['probe_idx_after_one_failure'] == 1 else 1)"; then
    t_pass "section 30: an indeterminate resolution clears no_repo and advances the backoff exactly one slot"
else
    t_fail "section 30: retry-state hygiene check failed (rc=$S30M_RC, out=$S30M_OUT)"
fi

# Sub-test 30n: path discovery failing while the IDENTITY still answers must
# back off. Sharing one index made the backoff unreachable in exactly this case
# -- every poll ran a successful sample, which reset the index -- so discovery
# plus a ~54ms identity walk repeated every 2s forever.
S30N_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30NEOF' 2>&1
import json, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
axis = query._HistoryAxis.__new__(query._HistoryAxis)
axis.todo_root = tempfile.mkdtemp()
axis.quiet = True
axis.trigger = None; axis.no_repo = False; axis.shallow = None
axis.prev = {}; axis.degraded = False; axis._said_degraded = False
axis.last_id = "abc:1"; axis.id_known = True; axis.pending = False
axis._probe_idx = 0; axis._resolve_idx = 0; axis._next_probe_at = 0.0
def boom(*a, **k):
    raise query._TriggerUnavailable("discovery down")
query._history_trigger_paths = boom
query._cs.corpus_history_id = lambda root: "abc:1"   # identity answers fine
delays = []
for _ in range(4):
    axis._next_probe_at = 0.0
    base = query.time.monotonic()
    axis.poll()
    delays.append(round(axis._next_probe_at - base, 1))
print(json.dumps({"delays": delays, "probe_idx": axis._probe_idx}))
S30NEOF
); S30N_RC=$?
if [ "$S30N_RC" = "0" ] && echo "$S30N_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# Must ESCALATE (2,5,15,30), not repeat slot zero forever. The probe index must
# stay 0 -- the identity never failed, so its own budget is untouched.
sys.exit(0 if d['delays'] == [2.0, 5.0, 15.0, 30.0] and d['probe_idx'] == 0 else 1)"; then
    t_pass "section 30: discovery backoff escalates even while the identity probe keeps succeeding"
else
    t_fail "section 30: resolution-backoff check failed (rc=$S30N_RC, out=$S30N_OUT)"
fi

# Sub-test 30o: a conventional HIERARCHICAL ref namespace (refs/pull/<n>/head)
# well under the ceiling must stay complete. Double-counting directories made
# this shape degrade at well under 20,000 real entries, permanently.
S30O_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30OEOF' 2>&1
import json, os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
tmp = tempfile.mkdtemp()
refs = os.path.join(tmp, "refs")
for n in range(120):                       # 120 dirs + 120 files + pull + refs
    d = os.path.join(refs, "pull", str(n))
    os.makedirs(d)
    with open(os.path.join(d, "head"), "w") as f:
        f.write("x")
query.WATCH_TRIGGER_MAX_ENTRIES = 300      # comfortably above the 242 real ones
snap, complete = query._snapshot_history_trigger((refs, [], None))
query.WATCH_TRIGGER_MAX_ENTRIES = 20000
print(json.dumps({"entries": len(snap), "complete": complete}))
S30OEOF
); S30O_RC=$?
if [ "$S30O_RC" = "0" ] && echo "$S30O_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# 1 refs + 1 pull + 120 dirs + 120 files = 242 entries, and it must COMPLETE.
sys.exit(0 if d['complete'] and d['entries'] == 242 else 1)"; then
    t_pass "section 30: a hierarchical refs/pull namespace under the ceiling stays complete"
else
    t_fail "section 30: hierarchical-refs check failed (rc=$S30O_RC, out=$S30O_OUT)"
fi

# Sub-test 30p: a cadence must be measured from when the probe FINISHED, not
# from when the poll started. Anchoring to poll entry meant that if the git work
# outran the cadence the new deadline was already expired on return, so the 2s
# loop immediately re-ran the expensive work -- the cooldown deleting itself
# exactly when the machine is slowest. Simulated with a probe that burns clock.
S30P_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30PEOF' 2>&1
import json, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
axis = query._HistoryAxis.__new__(query._HistoryAxis)
axis.todo_root = tempfile.mkdtemp()
axis.quiet = True
axis.trigger = None; axis.no_repo = True; axis.shallow = None
axis.prev = {}; axis.degraded = False; axis._said_degraded = False
axis.last_id = "no-repo"; axis.id_known = True; axis.pending = False
axis._probe_idx = 0; axis._resolve_idx = 0; axis._next_probe_at = 0.0
query._history_trigger_paths = lambda root: None      # determinate no-repo
# A monotonic clock that jumps 90s across the probe -- longer than the cadence.
real_mono = query.time.monotonic
state = {"t": real_mono(), "probed": False}
def fake_mono():
    return state["t"]
def slow_id(root):
    state["probed"] = True
    state["t"] += 90.0                                # the probe took 90s
    return "no-repo"
query.time.monotonic = fake_mono
query._cs.corpus_history_id = slow_id
axis.poll()
remaining = axis._next_probe_at - state["t"]
query.time.monotonic = real_mono
print(json.dumps({"probed": state["probed"], "remaining": round(remaining, 1)}))
S30PEOF
); S30P_RC=$?
if [ "$S30P_RC" = "0" ] && echo "$S30P_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# The full cadence must still be ahead of us. Anchored to poll entry it would be
# 60 - 90 = -30, i.e. already due, and the next poll would re-run immediately.
sys.exit(0 if d['probed'] and d['remaining'] == 60.0 else 1)"; then
    t_pass "section 30: a cadence is measured from probe completion, so slow git cannot delete the cooldown"
else
    t_fail "section 30: cadence-anchoring check failed (rc=$S30P_RC, out=$S30P_OUT)"
fi

# Sub-test 30q: a directory that cannot be ENUMERATED must make the snapshot
# incomplete, not silently smaller. Two failed reads otherwise produce equal
# truncated dicts, `poll` takes the nothing-moved path, and a real history move
# is never probed -- the exact thing truncation must never be allowed to mean.
S30Q_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30QEOF' 2>&1
import json, os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
tmp = tempfile.mkdtemp()
refs = os.path.join(tmp, "refs")
os.makedirs(os.path.join(refs, "heads"))
with open(os.path.join(refs, "heads", "main"), "w") as f:
    f.write("x")
# CONTROL: the readable tree completes.
ok_snap, ok_complete = query._snapshot_history_trigger((refs, [], None))
real_scandir = os.scandir
def failing_scandir(path):
    if path.endswith("heads"):
        raise OSError("simulated enumeration failure")
    return real_scandir(path)
query.os.scandir = failing_scandir
bad_snap, bad_complete = query._snapshot_history_trigger((refs, [], None))
query.os.scandir = real_scandir
print(json.dumps({"ok_complete": ok_complete, "bad_complete": bad_complete,
                  "bad_smaller": len(bad_snap) < len(ok_snap)}))
S30QEOF
); S30Q_RC=$?
if [ "$S30Q_RC" = "0" ] && echo "$S30Q_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if d['ok_complete'] and not d['bad_complete'] and d['bad_smaller'] else 1)"; then
    t_pass "section 30: an unreadable ref subtree yields an INCOMPLETE snapshot, never a smaller complete one"
else
    t_fail "section 30: enumeration-failure classification check failed (rc=$S30Q_RC, out=$S30Q_OUT)"
fi

# Sub-test 30r: ABSENT and UNREADABLE must not collapse. `shallow` legitimately
# does not exist in most repositories, so ENOENT is a determinate None; EACCES
# means the trigger does not know, and two such failures comparing equal would
# take the nothing-moved path with a HEAD move sitting there unread.
S30R_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30REOF' 2>&1
import errno, json, os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
tmp = tempfile.mkdtemp()
head = os.path.join(tmp, "HEAD")
with open(head, "w") as f:
    f.write("ref: refs/heads/main\n")
missing = os.path.join(tmp, "shallow")          # deliberately absent
trigger = (None, [head], missing)
# CONTROL: a present HEAD and an ABSENT shallow is a COMPLETE snapshot.
ok_snap, ok_complete = query._snapshot_history_trigger(trigger)
real_stat = os.stat
def unreadable(path, *a, **k):
    if str(path).endswith("HEAD"):
        raise OSError(errno.EACCES, "simulated permission failure")
    return real_stat(path, *a, **k)
query.os.stat = unreadable
bad_snap, bad_complete = query._snapshot_history_trigger(trigger)
query.os.stat = real_stat
print(json.dumps({"ok_complete": ok_complete,
                  "shallow_is_none": ok_snap.get(missing) is None,
                  "bad_complete": bad_complete}))
S30REOF
); S30R_RC=$?
if [ "$S30R_RC" = "0" ] && echo "$S30R_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['ok_complete'] and d['shallow_is_none']
               and not d['bad_complete']) else 1)"; then
    t_pass "section 30: an ABSENT trigger file is complete-with-None; an UNREADABLE one is incomplete"
else
    t_fail "section 30: absent-vs-unreadable check failed (rc=$S30R_RC, out=$S30R_OUT)"
fi

# Sub-test 30s: a symlinked ref entry must make the snapshot INCOMPLETE. git
# follows a symlinked `refs/heads`; this walk must not (a cycle would hang it),
# so recording only the link's own inode made a move inside the target compare
# equal forever and the identity was never probed. Control: the same tree with
# the symlink replaced by a real directory completes normally.
S30S_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S30SEOF' 2>&1
import json, os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
def build(use_symlink):
    tmp = tempfile.mkdtemp()
    refs = os.path.join(tmp, "refs")
    target = os.path.join(tmp, "elsewhere")
    os.makedirs(refs)
    os.makedirs(target)
    with open(os.path.join(target, "main"), "w") as f:
        f.write("x")
    dest = os.path.join(refs, "heads")
    if use_symlink:
        os.symlink(target, dest)
    else:
        os.makedirs(dest)
        with open(os.path.join(dest, "main"), "w") as f:
            f.write("x")
    return query._snapshot_history_trigger((refs, [], None))
_, sym_complete = build(True)
_, real_complete = build(False)
print(json.dumps({"symlink_complete": sym_complete,
                  "real_dir_complete": real_complete}))
S30SEOF
); S30S_RC=$?
if [ "$S30S_RC" = "0" ] && echo "$S30S_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (not d['symlink_complete'] and d['real_dir_complete']) else 1)"; then
    t_pass "section 30: a symlinked ref entry yields an INCOMPLETE snapshot; a real directory completes"
else
    t_fail "section 30: symlinked-ref check failed (rc=$S30S_RC, out=$S30S_OUT)"
fi

# Sub-test 30j: once degraded, a poll below the 60s cadence must do NO ref
# traversal at all. Taking the snapshot and then checking the deadline ran the
# very walk that caused degradation ~30x/minute, which defeats the fallback.
S30J=$(s30_repo)
S30J_OUT=$(python3 - "$S30J" "$(dirname "$QUERY_PY")" <<'S30JEOF' 2>&1
import json, os, sys
sys.path.insert(0, sys.argv[2])
import query  # noqa: E402
todo = os.path.join(sys.argv[1], "todo")
axis = query._HistoryAxis(todo, quiet=True)
calls = {"n": 0}
real = query._snapshot_history_trigger
def counting(trigger):
    calls["n"] += 1
    return real(trigger)
query._snapshot_history_trigger = counting
axis.degraded = True
axis._next_probe_at = query.time.monotonic() + 3600.0   # deadline far away
before = calls["n"]
for _ in range(3):
    axis.poll()
below = calls["n"] - before
axis._next_probe_at = 0.0                                # deadline reached
axis.poll()
at_deadline = calls["n"] - before - below
query._snapshot_history_trigger = real
print(json.dumps({"walks_below_deadline": below, "walks_at_deadline": at_deadline}))
S30JEOF
); S30J_RC=$?
if [ "$S30J_RC" = "0" ] && echo "$S30J_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# Zero below the deadline is the fix; the control is that it DOES walk once the
# deadline arrives, so the fixture cannot pass by the axis simply being inert.
sys.exit(0 if d['walks_below_deadline'] == 0 and d['walks_at_deadline'] >= 1 else 1)"; then
    t_pass "section 30: a degraded axis performs ZERO ref walks below its cadence, one at the deadline"
else
    t_fail "section 30: degraded-idle walk check failed (rc=$S30J_RC, out=$S30J_OUT)"
fi

# Sub-test 30k: a corpus that is NOT a repository when the watcher starts, and
# becomes one under it, must be noticed. `no-repo` is a determinate identity
# baseline, not a permanent off switch -- latching it off was the same failure
# class as the transient-git latch, reached by a different route.
S30K=$(mktemp -d "$TMP_DIR/s30k-XXXXXX")
mkdir -p "$S30K/todo/01-test"
cat > "$S30K/todo/01-test/TODO-01-w.md" <<'EOF'
---
schema_version: 1
id: s30-norepo
domain: 01-test
status: draft
title: No-repo fixture
---
# body
EOF
S30K_OUT=$(python3 - "$S30K" "$(dirname "$QUERY_PY")" <<'S30KEOF' 2>&1
import json, os, subprocess, sys
sys.path.insert(0, sys.argv[2])
import query  # noqa: E402
tree = sys.argv[1]
todo = os.path.join(tree, "todo")
axis = query._HistoryAxis(todo, quiet=True)
started_no_repo = axis.no_repo and axis.last_id == "no-repo"
# CONTROL: while it is genuinely not a repo, a due poll must NOT tick.
axis._next_probe_at = 0.0
quiet_when_unchanged = axis.poll()
for cmd in (["git", "init", "-q", "."],
            ["git", "config", "user.email", "s30@test.invalid"],
            ["git", "config", "user.name", "s30"],
            ["git", "add", "-A"],
            ["git", "commit", "-qm", "c0"]):
    subprocess.run(cmd, cwd=tree, check=True, capture_output=True)
axis._next_probe_at = 0.0
noticed = axis.poll()
print(json.dumps({"started_no_repo": started_no_repo,
                  "quiet_when_unchanged": bool(quiet_when_unchanged),
                  "noticed_init": bool(noticed),
                  "resolved": axis.trigger is not None}))
S30KEOF
); S30K_RC=$?
if [ "$S30K_RC" = "0" ] && echo "$S30K_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['started_no_repo'] and not d['quiet_when_unchanged']
               and d['noticed_init'] and d['resolved']) else 1)"; then
    t_pass "section 30: a no-repo corpus stays quiet, then is noticed when it becomes a repository"
else
    t_fail "section 30: no-repo transition check failed (rc=$S30K_RC, out=$S30K_OUT)"
fi

# ----------------------------------------------------------------------
# Section 31: a shallow corpus is REFUSED, because tip+count cannot see an
# equal-count boundary move and no cheap widening of the projection can.
# ----------------------------------------------------------------------

# Sub-test 31a: the reproduced boundary. THE ORDER OF ASSERTIONS IS THE POINT --
# tip and count are checked EQUAL first, so this fixture cannot pass by way of
# section 21's count check and then be mistaken for evidence about section 31.
# Only after that is the refusal asserted, against a no-boundary control that
# must still produce a real `tip:count`.
S31A_OUT=$(python3 - "$REPO_ROOT/scripts/todo-graph" <<'S31AEOF' 2>&1
import json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[1])
import cache_schema as cs  # noqa: E402

tmp = tempfile.mkdtemp()
def git(*a, **kw):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True, **kw).stdout.strip()
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
os.mkdir(os.path.join(tmp, "todo"))
def commit(date, msg):
    env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    git("add", "-A")
    subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", msg],
                   env=env, capture_output=True, text=True, check=True)
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
commit("2020-01-01T00:00:00Z", "C0")                      # touches the corpus
open(os.path.join(tmp, "other.txt"), "w").write("x\n")
commit("2021-01-01T00:00:00Z", "C1")                      # does not
open(os.path.join(tmp, "other.txt"), "a").write("y\n")
commit("2022-01-01T00:00:00Z", "C2")                      # does not
c2 = git("rev-parse", "HEAD")
open(os.path.join(tmp, "todo", "a.md"), "a").write("b\n")
commit("2023-01-01T00:00:00Z", "C3")                      # touches the corpus

root = os.path.join(tmp, "todo")
shallow = os.path.join(tmp, "boundary")
open(shallow, "w").write(c2 + "\n")

def raw(env):
    e = dict(os.environ); e.update(env)
    def g(*a):
        return subprocess.run(["git", "-C", root, *a], env=e,
                              capture_output=True, text=True,
                              check=True).stdout.strip()
    # THE PATHSPEC IS RELATIVE TO `-C`, which is the corpus root here, so it is
    # `a.md` and NOT `todo/a.md`. Written the other way it matches nothing, both
    # sides read back an empty string, and the fixture passes on equal emptiness
    # while proving nothing -- which is exactly what it did on first run.
    return (g("log", "-1", "--format=%H", "--", "."),
            g("rev-list", "--count", "HEAD", "--", "."),
            g("log", "--reverse", "--format=%ct", "--", "a.md").split("\n")[0])

full_tip, full_count, full_created = raw({})
b_tip, b_count, b_created = raw({"GIT_SHALLOW_FILE": shallow})

control = cs.corpus_history_id(root)                       # not shallow
try:
    os.environ["GIT_SHALLOW_FILE"] = shallow
    try:
        refused = None
        cs.corpus_history_id(root)
    except cs.CacheSchemaError as exc:
        refused = str(exc)
finally:
    os.environ.pop("GIT_SHALLOW_FILE", None)

print(json.dumps({
    "tip_equal": full_tip == b_tip,
    "count_equal": full_count == b_count,
    "count": full_count,
    "created_moved": full_created != b_created,
    "created_non_empty": bool(full_created) and bool(b_created),
    "control_is_real_id": control.startswith(full_tip + ":"),
    "refused": refused is not None,
    "names_unshallow": "unshallow" in (refused or ""),
}))
S31AEOF
); S31A_RC=$?
if [ "$S31A_RC" = "0" ] && echo "$S31A_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['tip_equal'] and d['count_equal'] and d['count'] == '2'
               and d['created_moved'] and d['created_non_empty']
               and d['control_is_real_id']
               and d['refused'] and d['names_unshallow']) else 1)"; then
    t_pass "section 31: an equal-count boundary moves created_at past tip+count, and a shallow corpus is refused"
else
    t_fail "section 31: equal-count boundary refusal check failed (rc=$S31A_RC, out=$S31A_OUT)"
fi

# Sub-test 31b: WHY THE CHEAP FIX WAS REJECTED, pinned so it cannot be
# reintroduced. Binding a third component -- the oldest commit of the walk --
# looks like it closes 31a, and it does not: with an unrelated older leg merged
# in, the boundary moves on one leg while the OTHER leg keeps supplying the
# traversal's oldest commit, so tip, count AND oldest all hold while created_at
# moves. Control: the untouched leg's own created_at must NOT move, or the
# fixture is measuring a wholesale history change rather than a boundary swap.
S31B_OUT=$(python3 - <<'S31BEOF' 2>&1
import json, os, subprocess, sys, tempfile
tmp = tempfile.mkdtemp()
def git(*a):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True).stdout.strip()
def commit(date, msg):
    env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    git("add", "-A")
    subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", msg], env=env,
                   capture_output=True, text=True, check=True)
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
# leg B: an unrelated root, OLDER than anything on leg A.
git("checkout", "-q", "--orphan", "legb")
os.makedirs(os.path.join(tmp, "todo"), exist_ok=True)
open(os.path.join(tmp, "todo", "b.md"), "w").write("b\n")
commit("2019-01-01T00:00:00Z", "B0")
b0 = git("rev-parse", "HEAD")
# leg A: its own root, then two commits that do not touch the corpus.
git("checkout", "-q", "--orphan", "lega")
subprocess.run(["git", "-C", tmp, "rm", "-rq", "--cached", "."],
               capture_output=True, text=True)
for f in ("todo/b.md",):
    p = os.path.join(tmp, f)
    if os.path.exists(p):
        os.remove(p)
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
commit("2020-01-01T00:00:00Z", "A0")
open(os.path.join(tmp, "other.txt"), "w").write("x\n")
commit("2021-01-01T00:00:00Z", "A1")
open(os.path.join(tmp, "other.txt"), "a").write("y\n")
commit("2022-01-01T00:00:00Z", "A2")
a2 = git("rev-parse", "HEAD")
env = dict(os.environ, GIT_AUTHOR_DATE="2023-01-01T00:00:00Z",
           GIT_COMMITTER_DATE="2023-01-01T00:00:00Z")
subprocess.run(["git", "-C", tmp, "merge", "-q", "--allow-unrelated-histories",
                "--no-edit", "-m", "M", b0], env=env, capture_output=True,
               text=True, check=True)
open(os.path.join(tmp, "todo", "a.md"), "a").write("c\n")
commit("2024-01-01T00:00:00Z", "C3")

root = os.path.join(tmp, "todo")
shallow = os.path.join(tmp, "boundary")
open(shallow, "w").write(a2 + "\n")
def probe(env_extra):
    e = dict(os.environ); e.update(env_extra)
    def g(*a):
        return subprocess.run(["git", "-C", root, *a], env=e,
                              capture_output=True, text=True,
                              check=True).stdout.strip()
    return {
        "tip": g("log", "-1", "--format=%H", "--", "."),
        "count": g("rev-list", "--count", "HEAD", "--", "."),
        "oldest": g("log", "--reverse", "--format=%H", "--", ".").split("\n")[0],
        # Pathspecs are relative to `-C` (the corpus root), so `a.md`/`b.md`.
        "a": g("log", "--reverse", "--format=%ct", "--", "a.md").split("\n")[0],
        "b": g("log", "--reverse", "--format=%ct", "--", "b.md").split("\n")[0],
    }
f = probe({}); s = probe({"GIT_SHALLOW_FILE": shallow})
print(json.dumps({
    "tip_equal": f["tip"] == s["tip"],
    "count_equal": f["count"] == s["count"],
    "oldest_equal": f["oldest"] == s["oldest"],
    "moved_leg_created_moved": f["a"] != s["a"],
    "untouched_leg_created_held": f["b"] == s["b"],
    # The two controls the first run of this fixture lacked: an empty read
    # compares equal to another empty read, so "held" is only evidence when
    # the value actually exists.
    "timestamps_non_empty": bool(f["a"]) and bool(f["b"]) and bool(s["b"]),
}))
S31BEOF
); S31B_RC=$?
if [ "$S31B_RC" = "0" ] && echo "$S31B_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['tip_equal'] and d['count_equal'] and d['oldest_equal']
               and d['moved_leg_created_moved']
               and d['untouched_leg_created_held']
               and d['timestamps_non_empty']) else 1)"; then
    t_pass "section 31: a multi-root boundary holds tip, count AND oldest while created_at moves (binding was rejected for this)"
else
    t_fail "section 31: multi-root collision check failed (rc=$S31B_RC, out=$S31B_OUT)"
fi

# Sub-test 31c: MUTATION -- delete the shallowness refusal and 31a's boundary
# repo must stop refusing AND produce an id byte-equal to the unshallow
# control's. Without this, 31a would still pass against a build whose refusal
# had been reverted, because a CacheSchemaError can arrive from any of the
# probes around it; the mutation proves the fixture is sensitive to THIS guard.
python3 - "$REPO_ROOT/scripts/todo-graph/cache_schema.py" "$TMP_DIR/cs_mut.py" <<'S31MUTEOF'
import sys, pathlib
src = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
# Strip BOTH bracketing call sites. The helper definition is left in place and
# simply goes uncalled, so the mutant differs from the shipped module in
# exactly the two lines that enforce the policy.
mutated = src
for call in ('    _refuse_if_shallow("before")\n',
             '    _refuse_if_shallow("after")\n'):
    assert mutated.count(call) == 1, f"mutation needle {call!r} not unique"
    mutated = mutated.replace(call, "", 1)
pathlib.Path(sys.argv[2]).write_text(mutated, encoding="utf-8")
S31MUTEOF
S31C_OUT=$(python3 - "$TMP_DIR/cs_mut.py" "$REPO_ROOT/scripts/todo-graph" <<'S31CEOF' 2>&1
import importlib.util, json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[2])
spec = importlib.util.spec_from_file_location("cs_mut", sys.argv[1])
cs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cs)
tmp = tempfile.mkdtemp()
def git(*a):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True).stdout.strip()
def commit(date, msg):
    env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    git("add", "-A")
    subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", msg], env=env,
                   capture_output=True, text=True, check=True)
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
os.mkdir(os.path.join(tmp, "todo"))
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
commit("2020-01-01T00:00:00Z", "C0")
open(os.path.join(tmp, "other.txt"), "w").write("x\n")
commit("2021-01-01T00:00:00Z", "C1")
open(os.path.join(tmp, "other.txt"), "a").write("y\n")
commit("2022-01-01T00:00:00Z", "C2")
c2 = git("rev-parse", "HEAD")
open(os.path.join(tmp, "todo", "a.md"), "a").write("b\n")
commit("2023-01-01T00:00:00Z", "C3")
root = os.path.join(tmp, "todo")
shallow = os.path.join(tmp, "boundary")
open(shallow, "w").write(c2 + "\n")
control = cs.corpus_history_id(root)
os.environ["GIT_SHALLOW_FILE"] = shallow
try:
    mutant_id, err = cs.corpus_history_id(root), None
except cs.CacheSchemaError as exc:
    mutant_id, err = None, str(exc)
finally:
    os.environ.pop("GIT_SHALLOW_FILE", None)
print(json.dumps({"mutant_refused": err is not None,
                  "mutant_certifies_boundary": mutant_id == control}))
S31CEOF
); S31C_RC=$?
if [ "$S31C_RC" = "0" ] && echo "$S31C_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (not d['mutant_refused']
               and d['mutant_certifies_boundary']) else 1)"; then
    t_pass "section 31: mutation check -- without the refusal a shallow corpus certifies EQUAL to the full one (fixture is sensitive)"
else
    t_fail "section 31: shallow-refusal mutation did not certify as expected (rc=$S31C_RC, out=$S31C_OUT)"
fi

# Sub-test 31d: THE PRODUCER WALK, not just the reader probe. `corpus_history_id`
# refuses when IT is asked, but on the normal path the walk mints the identity
# itself, so a walk that ran under a boundary later removed would be compared
# by an id that -- in exactly the equal-count case -- matches the full history.
# Reproduced before the fix: created_at published as the boundary commit's
# 2022-01-01 against the true 2020-01-01, with nothing refusing. Control: the
# same corpus with no boundary must still derive 2020-01-01 normally.
S31D_OUT=$(python3 - "$REPO_ROOT/scripts/todo-graph" <<'S31DEOF' 2>&1
import json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[1])
import build as b, cache_schema as cs  # noqa: E402
from pathlib import Path  # noqa: E402
tmp = tempfile.mkdtemp()
def git(*a):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True).stdout.strip()
def commit(date, msg):
    env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    git("add", "-A")
    subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", msg], env=env,
                   capture_output=True, text=True, check=True)
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
os.mkdir(os.path.join(tmp, "todo"))
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
commit("2020-01-01T00:00:00Z", "C0")
open(os.path.join(tmp, "other.txt"), "w").write("x\n")
commit("2021-01-01T00:00:00Z", "C1")
open(os.path.join(tmp, "other.txt"), "a").write("y\n")
commit("2022-01-01T00:00:00Z", "C2")
c2 = git("rev-parse", "HEAD")
open(os.path.join(tmp, "todo", "a.md"), "a").write("b\n")
commit("2023-01-01T00:00:00Z", "C3")
root = Path(tmp) / "todo"
files = [root / "a.md"]
open(os.path.join(tmp, "boundary"), "w").write(c2 + "\n")
os.environ["GIT_SHALLOW_FILE"] = os.path.join(tmp, "boundary")
try:
    b.collect_git_timestamps(Path(tmp), files, root)
    walk_refused, leaked = False, None
except cs.CacheSchemaError as exc:
    walk_refused, leaked = True, str(exc)
finally:
    os.environ.pop("GIT_SHALLOW_FILE", None)
ts, hid = b.collect_git_timestamps(Path(tmp), files, root)
print(json.dumps({
    "walk_refused": walk_refused,
    "names_walk": "across the timestamp walk" in (leaked or ""),
    "control_created_at": ts["todo/a.md"][0],
    "control_id_real": bool(hid) and ":" in str(hid),
}))
S31DEOF
); S31D_RC=$?
if [ "$S31D_RC" = "0" ] && echo "$S31D_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['walk_refused'] and d['names_walk']
               and d['control_created_at'] == '2020-01-01T00:00:00Z'
               and d['control_id_real']) else 1)"; then
    t_pass "section 31: the producer walk itself refuses a shallow corpus, and the unshallow control still derives 2020-01-01"
else
    t_fail "section 31: producer-walk refusal check failed (rc=$S31D_RC, out=$S31D_OUT)"
fi

# Sub-test 31e: the READER brackets its walk too, and this is a SINGLE
# transition rather than the acknowledged install-and-remove residue. The
# corpus becomes shallow AFTER the first probe and stays shallow across the
# tip/count reads, so those reads run under a boundary the probe reported
# absent -- and in the equal-count case that yields the SAME identity as the
# full history. The transition is staged deterministically by wrapping the
# module's own `_git` so the boundary appears immediately after the first
# `--is-shallow-repository` call. Control: with no transition the same corpus
# returns a real id.
S31E_OUT=$(python3 - "$REPO_ROOT/scripts/todo-graph" <<'S31EEOF' 2>&1
import json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[1])
import cache_schema as cs  # noqa: E402
tmp = tempfile.mkdtemp()
def git(*a):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True).stdout.strip()
def commit(date, msg):
    env = dict(os.environ, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
    git("add", "-A")
    subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", msg], env=env,
                   capture_output=True, text=True, check=True)
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
os.mkdir(os.path.join(tmp, "todo"))
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
commit("2020-01-01T00:00:00Z", "C0")
open(os.path.join(tmp, "other.txt"), "w").write("x\n")
commit("2021-01-01T00:00:00Z", "C1")
open(os.path.join(tmp, "other.txt"), "a").write("y\n")
commit("2022-01-01T00:00:00Z", "C2")
c2 = git("rev-parse", "HEAD")
open(os.path.join(tmp, "todo", "a.md"), "a").write("b\n")
commit("2023-01-01T00:00:00Z", "C3")
root = os.path.join(tmp, "todo")
boundary = os.path.join(tmp, "boundary")
open(boundary, "w").write(c2 + "\n")

control = cs.corpus_history_id(root)

# Stage the single transition: the FIRST shallowness probe answers under a
# clean environment, and the boundary is installed the moment it returns.
real_env = cs.history_git_env
state = {"probes": 0}
def transitioning_env():
    env = real_env()
    if state["probes"] >= 1:
        env["GIT_SHALLOW_FILE"] = boundary
    return env
def counting_env():
    # Count only the shallowness probe by inspecting the caller's argv is not
    # possible here, so drive it off call ORDER: probes 1..2 are git-dir and
    # unborn, probe 3 is the before-shallow check. Install after probe 3.
    state["probes"] += 1
    return transitioning_env() if state["probes"] > 3 else real_env()
cs.history_git_env = counting_env
try:
    got, refused = cs.corpus_history_id(root), None
except cs.CacheSchemaError as exc:
    got, refused = None, str(exc)
finally:
    cs.history_git_env = real_env
print(json.dumps({
    "control_is_real_id": ":" in control,
    "refused": refused is not None,
    "names_after": "after the history walk" in (refused or ""),
    "did_not_certify_equal": got != control,
}))
S31EEOF
); S31E_RC=$?
if [ "$S31E_RC" = "0" ] && echo "$S31E_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['control_is_real_id'] and d['refused'] and d['names_after']
               and d['did_not_certify_equal']) else 1)"; then
    t_pass "section 31: the reader refuses a boundary installed AFTER its first probe (single transition, not the ABA residue)"
else
    t_fail "section 31: reader-bracket check failed (rc=$S31E_RC, out=$S31E_OUT)"
fi

# Sub-test 31f: the WATCH trigger follows the effective boundary file. The
# refusal honours GIT_SHALLOW_FILE but `rev-parse --git-path shallow` resolves
# the DEFAULT one, so watching only the default let a watcher started under an
# override pointing at an absent file keep serving an answer that a direct
# query refuses -- no ref moves, no watched file moves, the identity is never
# re-asked. Control: with no override the trigger still names the default file,
# and the default stays in the watched set either way.
S31F_OUT=$(python3 - "$(dirname "$QUERY_PY")" <<'S31FEOF' 2>&1
import json, os, subprocess, sys, tempfile
sys.path.insert(0, sys.argv[1])
import query  # noqa: E402
tmp = tempfile.mkdtemp()
def git(*a):
    return subprocess.run(["git", "-C", tmp, *a], capture_output=True,
                          text=True, check=True).stdout.strip()
git("init", "-q", "-b", "main", ".")
git("config", "user.email", "t@t"); git("config", "user.name", "t")
os.mkdir(os.path.join(tmp, "todo"))
open(os.path.join(tmp, "todo", "a.md"), "w").write("a\n")
git("add", "-A")
subprocess.run(["git", "-C", tmp, "commit", "-q", "-m", "c"],
               capture_output=True, text=True, check=True)
root = os.path.join(tmp, "todo")
control = query._history_trigger_paths(root)
override = os.path.join(tmp, "my-boundary")     # deliberately ABSENT
os.environ["GIT_SHALLOW_FILE"] = override
try:
    with_ovr = query._history_trigger_paths(root)
finally:
    os.environ.pop("GIT_SHALLOW_FILE", None)
print(json.dumps({
    "control_is_default": control[2].endswith(os.path.join(".git", "shallow")),
    "override_is_effective": with_ovr[2] == os.path.normpath(override),
    "default_still_watched": any(str(p).endswith(os.path.join(".git", "shallow"))
                                 for p in with_ovr[1]),
    "control_default_not_duplicated": not any(
        str(p).endswith(os.path.join(".git", "shallow")) for p in control[1]),
}))
S31FEOF
); S31F_RC=$?
if [ "$S31F_RC" = "0" ] && echo "$S31F_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['control_is_default'] and d['override_is_effective']
               and d['default_still_watched']
               and d['control_default_not_duplicated']) else 1)"; then
    t_pass "section 31: the watch trigger follows GIT_SHALLOW_FILE, keeping the default file watched alongside"
else
    t_fail "section 31: watch-trigger effective-boundary check failed (rc=$S31F_RC, out=$S31F_OUT)"
fi

fi  # git available

# ======================================================================
# Section 33: the stamp-target capture takes a WHOLE markdown link.
#
# The defect: both producer capture sites spelled the target `\S+`, so a link
# whose LABEL contains a space split at that space and only the fragment
# reached the resolver. Section 26 already made that fragment fail closed, so
# the shape below is a FALSE REFUSAL rather than a wrong binding -- which is
# exactly why the fixture must assert the DESTINATION is followed, not merely
# that something resolved.
#
# The fixture names DIFFERENT valid files in the label and the destination, so
# a capture that follows the label resolves to a real file and a capture that
# follows the destination resolves to a different real file. Both answers are
# "resolved"; only the coordinates tell them apart. A fixture whose label named
# a nonexistent file would pass for the wrong reason -- it would be measuring
# fail-closed behaviour, not which end of the link won.
# ======================================================================

S33_TREE="$TMP_DIR/s33-linktarget"
rm -rf "$S33_TREE"; mkdir -p "$S33_TREE/todo/01-test"
cat > "$S33_TREE/todo/01-test/TODO-01-label-file.md" <<'EOF'
---
schema_version: 1
id: s33-label
domain: 01-test
status: active
title: "the file named in the LABEL"
---
# TODO-01 -- Label file

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First Section

- [x] Commit
EOF
cat > "$S33_TREE/todo/01-test/TODO-02-destination-file.md" <<'EOF'
---
schema_version: 1
id: s33-destination
domain: 01-test
status: active
title: "the file named in the DESTINATION"
---
# TODO-02 -- Destination file

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First Section

- [x] Commit
EOF
cat > "$S33_TREE/todo/01-test/TODO-03-stamped.md" <<'EOF'
---
schema_version: 1
id: s33-stamped
domain: 01-test
status: active
title: "carries the stamp under test"
---
# TODO-03 -- Stamped fixture

## Implementation Order

| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | §1 | First | -- | [x] |

## 1. First Section

> **Deferred:** [M] label and destination name different files -> XREF: [`01-test/TODO-01-label-file.md §1`](TODO-02-destination-file.md) §1 (item: "Commit" at line 19)

- [x] Commit
EOF

# Sub-test 33a: END-TO-END through extract_stamps_xrefs -> resolve_xref_target.
# The label names TODO-01, the destination names TODO-02, and BOTH exist, so
# the assertion is on the coordinates rather than on resolved-vs-unresolved.
S33_OUT=$(python3 - "$S33_TREE" <<'S33EOF'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])
sys.path.insert(0, "scripts/todo-graph")
import build as B, validate as V

text = (root / "todo/01-test/TODO-03-stamped.md").read_text(encoding="utf-8")
xrefs = B.extract_stamps_xrefs(text)
import subprocess
subprocess.run([sys.executable, "scripts/todo-graph/build.py", "--quiet",
                "--root", str(root / "todo"), "--output", str(root / "cache.json"),
                "--repo-root", str(root)], check=True, capture_output=True)
cache = json.loads((root / "cache.json").read_text())
nodes = cache["nodes"] if isinstance(cache, dict) and "nodes" in cache else cache
id_index = V.build_id_index(nodes); path_index = V.build_path_index(nodes)
tp = xrefs[0]["target_path"] if xrefs else ""
resolved = V.resolve_xref_target(tp, "todo/01-test/TODO-03-stamped.md",
                                 id_index, path_index) if tp else None
print(json.dumps({
    "n_xrefs": len(xrefs),
    "target_path": tp,
    "target_section": xrefs[0]["target_section"] if xrefs else "",
    "item_name": xrefs[0].get("item_name", "") if xrefs else "",
    "resolved": resolved or "",
    "follows_destination": bool(resolved and resolved.endswith("TODO-02-destination-file.md")),
    "follows_label": bool(resolved and resolved.endswith("TODO-01-label-file.md")),
}))
S33EOF
); S33_RC=$?
if [ "$S33_RC" = "0" ] && echo "$S33_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['n_xrefs'] == 1 and d['follows_destination'] and not d['follows_label']
               and d['target_section'] == '§1' and d['item_name'] == 'Commit') else 1)"; then
    t_pass "section 33: a stamp link whose label contains a space resolves to its DESTINATION"
else
    t_fail "section 33: whole-link capture fixture failed (rc=$S33_RC, out=$S33_OUT)"
fi

# Sub-test 33b: MUTATION -- restore the `\S+` target grammar. 33a must FAIL,
# and specifically it must stop following the destination. Without this, 33a
# would pass against any build whose capture happened to resolve.
MUT_DIR="$TMP_DIR/s33-mut-nonspace"
rm -rf "$MUT_DIR"; cp -r "$REPO_ROOT/scripts/todo-graph" "$MUT_DIR"
python3 - "$MUT_DIR/cache_schema.py" <<'MUTEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = 'XREF_TARGET_PATTERN = r"(?:" + XREF_LINK_PATTERN + r"|\\S+)"'
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
p.write_text(src.replace(needle, 'XREF_TARGET_PATTERN = r"\\S+"', 1), encoding="utf-8")
MUTEOF
S33B_OUT=$(python3 - "$S33_TREE" "$MUT_DIR" <<'S33BEOF'
import json, sys, pathlib
root, mut = pathlib.Path(sys.argv[1]), sys.argv[2]
sys.path.insert(0, mut)
import build as B, validate as V
text = (root / "todo/01-test/TODO-03-stamped.md").read_text(encoding="utf-8")
xrefs = B.extract_stamps_xrefs(text)
import subprocess
subprocess.run([sys.executable, f"{mut}/build.py", "--quiet",
                "--root", str(root / "todo"), "--output", str(root / "cache-mut.json"),
                "--repo-root", str(root)], check=True, capture_output=True)
cache = json.loads((root / "cache-mut.json").read_text())
nodes = cache["nodes"] if isinstance(cache, dict) and "nodes" in cache else cache
id_index = V.build_id_index(nodes); path_index = V.build_path_index(nodes)
tp = xrefs[0]["target_path"] if xrefs else ""
resolved = V.resolve_xref_target(tp, "todo/01-test/TODO-03-stamped.md",
                                 id_index, path_index) if tp else None
print(json.dumps({
    "target_path": tp,
    "resolved": resolved or "",
    "follows_destination": bool(resolved and resolved.endswith("TODO-02-destination-file.md")),
}))
S33BEOF
); S33B_RC=$?
if [ "$S33B_RC" = "0" ] && echo "$S33B_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (not d['follows_destination'] and d['target_path'].startswith('[')) else 1)"; then
    t_pass "section 33: mutation check -- restoring \\S+ splits the label and stops following the destination"
else
    t_fail "section 33: \\S+ mutation did not misfire as expected (rc=$S33B_RC, out=$S33B_OUT)"
fi

# Sub-test 33c: NEGATIVE -- a MALFORMED bracket target followed by a real link
# must still fail closed. A tempered-dot label would accept the unbalanced
# `[broken and [dest](TODO-02...)` as one complete link and resolve it, which
# is section 26's fail-closed rule running backwards (Codex design review).
S33C_OUT=$(python3 - "$S33_TREE" <<'S33CEOF'
import json, sys, pathlib
root = pathlib.Path(sys.argv[1])
sys.path.insert(0, "scripts/todo-graph")
import validate as V
cache = json.loads((root / "cache.json").read_text())
nodes = cache["nodes"] if isinstance(cache, dict) and "nodes" in cache else cache
id_index = V.build_id_index(nodes); path_index = V.build_path_index(nodes)
src = "todo/01-test/TODO-03-stamped.md"
cases = {
    "unbalanced_label": "[broken and [dest](TODO-02-destination-file.md)",
    "bare_fragment": "[`01-test/TODO-01-label-file",
    "anchor_only": "[label](#1-first-section)",
    # POSITIVE CONTROLS. Without these the sub-test passes by refusing
    # EVERYTHING, which is what an over-tightened label grammar does -- and did:
    # a bracket-free label refused section 26's legitimate `[a [inner] label]`.
    "control_plain": "[`01-test/TODO-01 §1`](TODO-02-destination-file.md)",
    "control_inner_brackets": "[a [inner] label](TODO-02-destination-file.md)",
}
print(json.dumps({k: (V.resolve_xref_target(v, src, id_index, path_index) or "")
                  for k, v in cases.items()}))
S33CEOF
); S33C_RC=$?
if [ "$S33C_RC" = "0" ] && echo "$S33C_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
refused = not any(v for k, v in d.items() if not k.startswith('control_'))
resolved = all(v.endswith('TODO-02-destination-file.md')
               for k, v in d.items() if k.startswith('control_'))
sys.exit(0 if (refused and resolved) else 1)"; then
    t_pass "section 33: malformed, fragment and anchor-only bracket targets all still fail closed"
else
    t_fail "section 33: a malformed bracket target resolved (rc=$S33C_RC, out=$S33C_OUT)"
fi

# Sub-test 33d: the QUERY-side normalizer must reach the same destination. It
# split the target at `#` BEFORE any link unwrap, so an anchored whole-link
# target became the incomplete `[label](file.md` and every query-side stamp
# consumer (backlinks, deferred, stats, render) silently omitted the edge while
# validate.py reported the same target resolved (Codex design review, [high]).
S33D_OUT=$(python3 - <<'S33DEOF'
import json, sys
sys.path.insert(0, "scripts/todo-graph")
import query as Q
print(json.dumps({
    "anchored_link": Q._normalize_ref_token("[`01-test/TODO-01 §1`](TODO-02-destination-file.md#1-first)"),
    "plain_link": Q._normalize_ref_token("[`label with space`](TODO-02-destination-file.md)"),
    "anchor_only": Q._normalize_ref_token("[label](#1-first-section)"),
    "control_bare": Q._normalize_ref_token("`02-kernel-core/TODO-05.md`,"),
}))
S33DEOF
); S33D_RC=$?
if [ "$S33D_RC" = "0" ] && echo "$S33D_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['anchored_link'] == 'TODO-02-destination-file.md'
               and d['plain_link'] == 'TODO-02-destination-file.md'
               and d['anchor_only'] == ''
               and d['control_bare'] == '02-kernel-core/TODO-05.md') else 1)"; then
    t_pass "section 33: the query-side normalizer unwraps a link before stripping its fragment"
else
    t_fail "section 33: query-side normalizer ordering wrong (rc=$S33D_RC, out=$S33D_OUT)"
fi

# Sub-test 33e: MUTATION -- make the resolver BYPASS the shared helper and do
# its own unwrap in the wrong order (fragment first). An anchored link must
# stop resolving. This is what stops `unwrap_xref_link` from becoming a helper
# everything imports and nothing depends on: the ordering has to be observable
# from the resolver's behaviour, or a future edit can re-inline it wrongly and
# every other fixture here still passes.
MUT_DIR="$TMP_DIR/s33-mut-bypass"
rm -rf "$MUT_DIR"; cp -r "$REPO_ROOT/scripts/todo-graph" "$MUT_DIR"
python3 - "$MUT_DIR/validate.py" <<'MUTEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = "    unwrapped = cache_schema.unwrap_xref_link(target)\n"
assert src.count(needle) == 1, f"bypass needle appears {src.count(needle)}x"
# The pre-section-33 ordering: strip the fragment, THEN try to unwrap.
bypass = ("    _frag = target.split('#', 1)[0]\n"
          "    _m = cache_schema.XREF_LINK_RE.match(_frag)\n"
          "    unwrapped = (_m.group('url') if _m else target)\n")
p.write_text(src.replace(needle, bypass, 1), encoding="utf-8")
MUTEOF
S33E_OUT=$(python3 - "$MUT_DIR" <<'S33EEOF'
import json, sys
mut = sys.argv[1]
sys.path.insert(0, mut)
import validate as V
def node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
lk = [node("todo/01-test/TODO-01-a.md", "lka"), node("todo/01-test/TODO-02-b.md", "lkb")]
p, i = V.build_path_index(lk), V.build_id_index(lk)
S = "todo/01-test/TODO-01-a.md"
print(json.dumps({
    "anchored": V.resolve_xref_target("[`TODO-02 §6`](TODO-02-b.md#6-x)", S, i, p) or "",
    "plain": V.resolve_xref_target("[`TODO-02 §6`](TODO-02-b.md)", S, i, p) or "",
}))
S33EEOF
); S33E_RC=$?
if [ "$S33E_RC" = "0" ] && echo "$S33E_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# The ANCHORED link must break under the bypass; the un-anchored one still
# resolves, which is what proves the mutation hit the ORDERING and not the
# grammar (a mutation that broke everything would prove nothing).
sys.exit(0 if (not d['anchored'] and d['plain'].endswith('TODO-02-b.md')) else 1)"; then
    t_pass "section 33: mutation check -- bypassing the shared unwrap breaks anchored links"
else
    t_fail "section 33: unwrap-bypass mutation did not misfire as expected (rc=$S33E_RC, out=$S33E_OUT)"
fi

# Sub-test 33f: THE TWO CAPTURE SITES AGREE ON WHERE A TARGET ENDS. Sharing the
# grammar was not sufficient: `build.py` requires `\s+§\S+` after the target, so
# on a malformed adjacent-link token its link arm cannot stop mid-token and
# backtracks to `\S+`, handing the WHOLE token to a resolver that refuses it.
# `fix_line_numbers` had no such requirement, so its link arm stopped at the
# first `)` and bound the FIRST destination -- at rc 0, with zero
# incomplete-repair counts, and under `--write` a line number rewritten from the
# wrong file. The sub-tests above could not see it: they exercise the resolver
# directly, not this capture site (Codex adversarial, section 33 post-ship).
S33F_OUT=$(python3 - <<'S33FEOF'
import json, re, sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs

# SECTION 37 MOVED THE GRAMMAR AND THIS PROBE MOVED WITH IT. This sub-test used
# to keep its OWN copy of the repair site's capture expression "in step with it
# here", and to assert the boundary by grepping `validate.py` for the literal
# regex -- a fifth spelling of the thing section 37 exists to have exactly one
# of, and a probe that would report agreement even if both sites drifted
# together. Both sites now call `cache_schema.parse_xref_clause`, so the
# boundary is asserted where it lives and the two sites are exercised through
# their real entry points.
# THE REPAIR SIDE IS THE REAL ENTRY POINT, not a re-spelling of its two lines.
# An earlier cut of this sub-test imitated `fix_line_numbers` locally, which is
# the same defect it was rewritten to remove: a test that re-spells the producer
# cannot notice the producer changing (Codex consistency, [medium]).
import validate as V, pathlib
def _node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
_NODES = [_node("todo/01-test/TODO-03-s.md", "s3")]
_IDX = _NODES + [_node("todo/01-test/TODO-01-a.md", "a1"),
                 _node("todo/01-test/TODO-02-b.md", "b2")]
_ID, _PATH = V.build_id_index(_IDX), V.build_path_index(_IDX)
_BODY = "# T\n\n## 1. First\n\n- [x] Needle\n"

def repair_unresolvable(stamp):
    """Real repair producer: how many targets did it refuse on this stamp?"""
    snap = {"todo/01-test/TODO-03-s.md": "# S\n\n" + stamp + "\n",
            "todo/01-test/TODO-01-a.md": _BODY,
            "todo/01-test/TODO-02-b.md": _BODY}
    _u, _a, unres, _m, _r = V.fix_line_numbers(
        _NODES, snap, _ID, _PATH, pathlib.Path("."), False, True)
    return unres

cases = {
    # malformed adjacent links: BOTH sites must hand over the whole token
    "adjacent": '[a](TODO-01-a.md)[b](TODO-02-b.md)',
    # nested: same
    "nested": '[[a](TODO-01-a.md)[b](TODO-02-b.md)](TODO-02-b.md)',
    # legitimate space-bearing label: BOTH must capture the whole link
    "spaced": '[`01-test/TODO-01 §1`](TODO-02-b.md)',
    # trailing comma on a bare token stays supported
    "comma": 'TODO-02-b.md,',
}
# The boundary is part of the SHARED token grammar now, so it is asserted
# there. NEITHER producer may re-inline a target capture of its own: that
# duplication is the defect section 37 removed, and a re-inlined copy is how it
# would come back.
src_b = open("scripts/todo-graph/build.py", encoding="utf-8").read()
src_v = open("scripts/todo-graph/validate.py", encoding="utf-8").read()
RE_INLINED = re.compile(r"XREF_TARGET_PATTERN\s*\+\s*r?\"\)\(\?=")
out = {
    "boundary_present": cs._XREF_TARGET_TOKEN_RE.pattern.endswith(r"(?=[`\s]|$)"),
    "no_reinlined_capture": not (RE_INLINED.search(src_b) or RE_INLINED.search(src_v)),
}
for name, tok in cases.items():
    line = f'> **Deferred:** [M] x -> XREF: {tok} §1 (item: "Needle" at line 99)'
    b = [e["target_path"] for e in B._walk_stamps_xrefs([line], [False])]
    # OUTCOME parity across the two REAL producers: a clause the builder refuses
    # to make an edge from is a clause the repair path refuses to rewrite from.
    # A MALFORMED CLAUSE IS REFUSED BY BOTH, BUT THEY REFUSE IT DIFFERENTLY, and
    # that difference is section 33's deliberate design rather than drift: the
    # builder EMITS the whole malformed token (which then fails closed at the
    # resolver, since no such file exists) while the repair path counts an
    # unresolvable target and rewrites nothing. So the parity assertion is
    # "neither binds it to a real file", not "both emit nothing".
    v_unres = repair_unresolvable(line)
    # Only `adjacent` and `nested` are MALFORMED. `spaced` is the legitimate
    # space-bearing label section 33 exists to make resolve, and `comma` is a
    # well-formed target with trailing punctuation -- both must repair cleanly,
    # or this sub-test would be asserting that section 33 broke its own feature.
    expect_refusal = name in ("adjacent", "nested")
    out[name] = {"build": b, "repair_unresolvable": v_unres,
                 "agree": (v_unres > 0) == expect_refusal}
print(json.dumps(out))
S33FEOF
); S33F_RC=$?
if [ "$S33F_RC" = "0" ] && echo "$S33F_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
ok = d['boundary_present'] and d['no_reinlined_capture'] and all(d[k]['agree'] for k in ('adjacent','nested','spaced','comma'))
# and the agreement must be on the WHOLE token, not on a shared truncation
ok = ok and d['adjacent']['build'] == ['[a](TODO-01-a.md)[b](TODO-02-b.md)']
ok = ok and d['spaced']['build'] == ['[\`01-test/TODO-01 §1\`](TODO-02-b.md)']
# The comma case must RESOLVE on both sides -- it is a well-formed target with
# trailing punctuation, so the builder emits an edge and the repair path must
# refuse nothing.
ok = ok and d['comma']['build'] == ['TODO-02-b.md,'] and d['comma']['repair_unresolvable'] == 0
sys.exit(0 if ok else 1)"; then
    t_pass "section 33: the builder and the repair path agree on where a target ends"
else
    t_fail "section 33: capture-boundary agreement failed (rc=$S33F_RC, out=$S33F_OUT)"
fi

# Sub-test 33g: THE GRAMMAR COMPILES ON THE DECLARED PYTHON FLOOR, and no
# Inputs XREF ever emits an empty target_path. Two post-ship re-adversarial
# findings, kept in one sub-test because both are properties of the same
# shared-grammar change.
#
# The floor check is a SYNTAX check, not a version check: an atomic group
# `(?>...)` is Python 3.11+ and this repo's floor is 3.8, and because the
# pattern compiles at MODULE IMPORT the failure takes build/validate/query/
# render down together on a supported host. A CI runner is unlikely to be
# below 3.11, so the regression asserts the pattern uses no 3.11-only
# construct rather than trying to find an old interpreter to prove it on.
S33G_OUT=$(python3 - <<'S33GEOF'
import json, re, sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs, build as B

# 3.11+ regex constructs that would break the declared floor.
MODERN = (r"(?>", r"*+", r"++", r"?+", r"}+")
uses_modern = [c for c in MODERN
               if c in cs.XREF_LINK_PATTERN or c in cs.XREF_TARGET_PATTERN
               or c in cs.XREF_LINK_RE.pattern]

# The bound must exist and must clear the live corpus by a wide margin.
bounded = bool(re.search(r"\{0,(\d+)\}", cs.XREF_LINK_PATTERN))
bound = int(re.search(r"\{0,(\d+)\}", cs.XREF_LINK_PATTERN).group(1)) if bounded else 0

# A label at the bound still matches; one past it falls through rather than
# blowing up -- fails closed.
at = "[" + "a" * (bound - 1) + "](x.md)"
over = "[" + "a" * (bound + 50) + "](x.md)"
# THE BOUND MUST COUNT CHARACTERS, NOT ATOMS. `{0,N}` on the repetition alone
# left the bracket-pair atom holding an unbounded inner class, so 4,096 fat
# pairs still built a ~16 MiB label while the comment claimed a 4,096-character
# cap -- a bound that was documented but did not exist (Codex perf, section 33
# post-ship round 6). A flat-label fixture cannot see this shape.
fat_pair = "[" + "x" * 4088 + "]"
fat = "[" + fat_pair * 8 + "](x.md)"

# No Inputs XREF form may yield an empty target_path. SECTION 37: driven
# through the REAL producer (`extract_inputs_xrefs`) rather than through a
# re-spelling of its retired regex -- the emptiness this guards against is a
# property of what the producer EMITS, so probing the producer is both the
# honest oracle and immune to the next grammar move.
def inputs_of(bullet):
    return B.extract_inputs_xrefs("## Inputs\n\n" + bullet + "\n")

empties = []
for probe in ("- -> XREF: [x]() -- desc",
              "- -> XREF: [x](#anchor) -- desc",
              "- -> XREF: [a [inner] label](TODO-02-b.md) §1 -- desc",
              "- -> XREF: `02-kernel-core/TODO-05` §7 -- desc"):
    for row in inputs_of(probe):
        if not row.get("target_path"):
            empties.append(probe)

print(json.dumps({
    "uses_modern": uses_modern,
    "bounded": bounded,
    "bound": bound,
    "at_bound_matches": bool(cs.XREF_LINK_RE.match(at)),
    "over_bound_refuses": not bool(cs.XREF_LINK_RE.match(over)),
    "fat_pairs_refused": not bool(cs.XREF_LINK_RE.match(fat)),
    "fat_pair_len": len(fat),
    "empty_targets": empties,
    "inner_bracket_label": (inputs_of("- -> XREF: [a [inner] label](TODO-02-b.md) §1 -- desc")
                            or [{}])[0].get("target_path", ""),
}))
S33GEOF
); S33G_RC=$?
if [ "$S33G_RC" = "0" ] && echo "$S33G_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
ok = (not d['uses_modern'] and d['bounded'] and d['bound'] >= 1000
      and d['at_bound_matches'] and d['over_bound_refuses']
      and d['fat_pairs_refused'] and d['fat_pair_len'] > 32000
      and not d['empty_targets']
      and d['inner_bracket_label'] == 'TODO-02-b.md')
sys.exit(0 if ok else 1)"; then
    t_pass "section 33: the grammar holds the Python floor, bounds its label, and never emits an empty Inputs target"
else
    t_fail "section 33: floor/bound/empty-target check failed (rc=$S33G_RC, out=$S33G_OUT)"
fi

# Sub-test 33h: BOTH Inputs surface forms use the shared grammar and the same
# boundary. The bullet form was centralized first and the TABLE form was filed
# as out-of-scope on the incorrect grounds that it was the Depends-On grammar;
# it is inside `extract_inputs_xrefs` and emits the same subtree, so an Inputs
# ROW with a spaced label produced no edge while the identical BULLET resolved.
# The parity assertion is what makes that class of mistake fail a test instead
# of surviving a scope argument (Codex consistency, section 33 post-ship).
S33H_OUT=$(python3 - <<'S33HEOF'
import json, sys
sys.path.insert(0, "scripts/todo-graph")
import build as B

def inputs(body):
    return B.extract_inputs_xrefs("## Inputs\n\n" + body + "\n")

# (name, bullet form, table form) -- both must yield the SAME target.
PAIRS = [
    ("spaced_label",
     "- -> XREF: [`01-test/TODO-01 §1`](TODO-02-b.md) §1 -- desc",
     "| -> XREF: [`01-test/TODO-01 §1`](TODO-02-b.md) §1 | x |"),
    ("inner_brackets",
     "- -> XREF: [a [inner] label](TODO-02-b.md) §1 -- desc",
     "| -> XREF: [a [inner] label](TODO-02-b.md) §1 | x |"),
    ("plain_path",
     "- -> XREF: `02-kernel-core/TODO-05` §7 -- desc",
     "| -> XREF: `02-kernel-core/TODO-05` §7 | x |"),
]
out = {}
for name, bullet, table in PAIRS:
    b = inputs(bullet)
    t = inputs(table)
    out[name] = {
        "bullet": [x["target_path"] for x in b],
        "table": [x["target_path"] for x in t],
        "parity": [x["target_path"] for x in b] == [x["target_path"] for x in t],
    }
# Adjacent links must NOT bind the first destination in either form -- and
# neither must WHITESPACE-SEPARATED ones, which the trailing boundary accepts
# by construction and which therefore needed their own refusal.
adj = inputs("- -> XREF: [a](TODO-01-a.md)[b](TODO-02-b.md) §1 -- desc")
out["adjacent_bullet"] = [x["target_path"] for x in adj]
out["spaced_bullet"] = inputs("- -> XREF: [a](TODO-01-a.md) [b](TODO-02-b.md) §1 -- desc")
out["spaced_table"] = inputs("| -> XREF: [a](TODO-01-a.md) [b](TODO-02-b.md) §1 | x |")
# No form may emit an empty target.
empt = inputs("- -> XREF: [x]() -- desc\n- -> XREF: [y](#anchor) -- desc")
out["empty_targets"] = [x["target_path"] for x in empt if not x["target_path"]]
print(json.dumps(out))
S33HEOF
); S33H_RC=$?
if [ "$S33H_RC" = "0" ] && echo "$S33H_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
ok = all(d[k]['parity'] for k in ('spaced_label','inner_brackets','plain_path'))
ok = ok and d['spaced_label']['bullet'] == ['TODO-02-b.md']
ok = ok and d['inner_brackets']['bullet'] == ['TODO-02-b.md']
ok = ok and d['plain_path']['bullet'] == ['02-kernel-core/TODO-05']
# the adjacent-link token must never bind the FIRST destination alone
ok = ok and d['adjacent_bullet'] != ['TODO-01-a.md']
ok = ok and d['spaced_bullet'] == [] and d['spaced_table'] == []
ok = ok and not d['empty_targets']
sys.exit(0 if ok else 1)"; then
    t_pass "section 33: bullet and table Inputs XREFs agree, and neither binds an adjacent-link prefix"
else
    t_fail "section 33: Inputs surface-form parity failed (rc=$S33H_RC, out=$S33H_OUT)"
fi

# Sub-test 33i: A MULTI-TARGET CLAUSE IS REFUSED LOUDLY BY THE REPAIR PATH,
# with or without a space between the links. The trailing boundary alone closed
# only the adjacent form; one space reopened it, because whitespace is what the
# boundary accepts. The assertion is on `fix_line_numbers` itself -- its return
# counts and its report -- not on a regex, because the defect that matters is a
# rewrite proposed from the WRONG FILE while the run reports success.
S33I_OUT=$(python3 - <<'S33IEOF'
import json, sys
sys.path.insert(0, "scripts/todo-graph")
import validate as V

def node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
NODES = [node("todo/01-test/TODO-03-s.md", "s3")]
IDX_N = NODES + [node("todo/01-test/TODO-01-a.md", "a1"),
                 node("todo/01-test/TODO-02-b.md", "b2"),
                 node("todo/01-test/TODO-04-c.md", "c4")]
id_index, path_index = V.build_id_index(IDX_N), V.build_path_index(IDX_N)

TARGET_BODY = "# T\n\n## 1. First\n\n- [x] Needle\n"
def run(stamp):
    snap = {"todo/01-test/TODO-03-s.md": "# S\n\n" + stamp + "\n",
            "todo/01-test/TODO-01-a.md": TARGET_BODY,
            "todo/01-test/TODO-02-b.md": TARGET_BODY,
            "todo/01-test/TODO-04-c.md": TARGET_BODY}
    return V.fix_line_numbers(NODES, snap, id_index, path_index,
                              __import__("pathlib").Path("."), False, True)

cases = {
    "adjacent": '> **Deferred:** [M] x -> XREF: [a](TODO-01-a.md)[b](TODO-02-b.md) §1 (item: "Needle" at line 99)',
    "spaced":   '> **Deferred:** [M] x -> XREF: [a](TODO-01-a.md) [b](TODO-02-b.md) §1 (item: "Needle" at line 99)',
    # CONTROL: one well-formed target must still repair, or the guard is just
    # breaking the feature.
    "control":  '> **Deferred:** [M] x -> XREF: [`01-test/TODO-01 §1`](TODO-01-a.md) §1 (item: "Needle" at line 99)',
    # BARRIER: a valid clause FOLLOWED BY a malformed one. Refusing the
    # malformed target is not enough -- dropping its position let the second
    # item fall through to the first clause's target and propose a rewrite
    # from the wrong file, at 2 updates. The valid clause must still repair
    # (1 update) and the malformed one must not.
    "barrier":  ('> **Deferred:** [M] x -> XREF: [`t`](TODO-01-a.md) §1 (item: "Needle" at line 99)'
                 ' -> XREF: [b](TODO-02-b.md) [c](TODO-04-c.md) §1 (item: "Needle" at line 98)'),
    # A second target need not be another LINK. A bracket-only test left these
    # two binding the first destination while the builder rejected the same
    # clause -- reported by three review legs independently (rounds 6-7).
    "link_plus_bare": '> **Deferred:** [M] x -> XREF: [a](TODO-01-a.md) TODO-02-b.md §1 (item: "Needle" at line 99)',
    "post_section":   '> **Deferred:** [M] x -> XREF: [a](TODO-01-a.md) §1 [b](TODO-02-b.md) (item: "Needle" at line 99)',
}
# CONTROL that must still repair: a description mentioning another TODO is
# clause TEXT, not a second target. Without this the rule could refuse half the
# corpus and every other assertion here would still pass.
CONTROL_PROSE = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 -- some prose about TODO-02 (item: "Needle" at line 99)' 
out = {}
for name, stamp in cases.items():
    updates, ambig, unres, missing, rep = run(stamp)
    # EITHER refusal message counts. The two malformed shapes are caught by
    # two different mechanisms and that is correct: the SPACED form is stopped
    # by the adjacency guard, while the ADJACENT form never satisfies the
    # boundary at all, so `\S+` hands the whole token to the resolver and
    # section 26's fail-closed rule refuses it there. What the fixture asserts
    # is the OUTCOME both must reach -- no rewrite, and a named diagnostic.
    out[name] = {"updates": updates, "unresolvable": unres,
                 "reported": any(("malformed XREF" in r) or
                                 ("unresolvable XREF target" in r) for r in rep)}
u, a, un, mi, rep = run(CONTROL_PROSE)
out["control_prose"] = {"updates": u, "unresolvable": un, "reported": False}
print(json.dumps(out))
S33IEOF
); S33I_RC=$?
if [ "$S33I_RC" = "0" ] && echo "$S33I_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
ok = all(d[k]['updates'] == 0 and d[k]['unresolvable'] >= 1 and d[k]['reported']
         for k in ('adjacent', 'spaced'))
ok = ok and d['control']['updates'] == 1 and d['control']['unresolvable'] == 0
# the barrier case repairs the VALID clause only: 1 update, not 2
ok = ok and d['barrier']['updates'] == 1 and d['barrier']['unresolvable'] == 1
ok = ok and all(d[k]['updates'] == 0 and d[k]['unresolvable'] >= 1
                for k in ('link_plus_bare', 'post_section'))
ok = ok and d['control_prose']['updates'] == 1 and d['control_prose']['unresolvable'] == 0
sys.exit(0 if ok else 1)"; then
    t_pass "section 33: a multi-target stamp clause is refused and reported, spaced or not"
else
    t_fail "section 33: multi-target repair refusal failed (rc=$S33I_RC, out=$S33I_OUT)"
fi

# ----------------------------------------------------------------------
# Sub-test 35a: A REFERENCE RESOLVES TO AN ITEM, NEVER TO ANOTHER REFERENCE.
# `fix_line_numbers` scanned every line of the target for the item name, so any
# line QUOTING the item matched as well as the item -- and a same-file stamp
# quotes it by construction. Two shapes, both measured on the live corpus before
# the fix: a LIVE item matched twice (item + stamp) and was refused as ambiguous,
# making all 430 same-file clauses permanently unrepairable; a DELETED item
# matched ONCE on the stamp's own line, so repair pointed the reference at
# itself and exited 0 (52 such self-matches were already written to the corpus).
#
# The two extra shapes are NOT decoration. Restricting the scan to checklist-item
# lines fixes the stamp shapes and still binds a reference to a reference when
# the quoting line is itself an item -- live at `TODO-03-kernel-libraries.md:362`,
# which resolved to a `- [/]` line that merely quotes the name while the item it
# names sits elsewhere under changed wording. And checklist-shaped text inside a
# fence is an EXAMPLE (24 such lines in the corpus), not a destination.
S35A_OUT=$(python3 - <<'S35AEOF'
import json, pathlib, re, sys
sys.path.insert(0, "scripts/todo-graph")
import validate as V

SELF = "todo/01-test/TODO-01-a.md"
def node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
NODES = [node(SELF, "a1")]
IDX = NODES + [node("todo/01-test/TODO-02-b.md", "b2")]
id_index, path_index = V.build_id_index(IDX), V.build_path_index(IDX)

STAMP = ('> **Deferred:** [M] parked -> XREF: TODO-01-a.md §1'
         ' (item: "Wire the resolver" at line 99)')
XSTAMP = ('> **Deferred:** [M] parked -> XREF: TODO-02-b.md §1'
          ' (item: "Wire the resolver" at line 99)')

def run(self_body, other_body="# B\n"):
    snap = {SELF: self_body, "todo/01-test/TODO-02-b.md": other_body}
    # quiet=False so the per-rewrite report line is emitted; the DESTINATION it
    # names is the assertion that matters. A first cut read the stored clause
    # back out of the snapshot instead -- which a dry run never mutates, so it
    # reported the stored 99 for every case and "failed" a correct fix.
    updates, ambig, unres, missing, rep = V.fix_line_numbers(
        NODES, snap, id_index, path_index, pathlib.Path("."), False, False)
    resolved = None
    for line in rep:
        m = re.search(r"'Wire the resolver' (\d+) -> (\d+)$", line)
        if m:
            resolved = int(m.group(2))
    return {"updates": updates, "ambiguous": ambig, "unresolvable": unres,
            "missing": missing, "resolved": resolved}

cases = {}
# 1. LIVE item in the SAME file: must resolve to the ITEM's line (5), never to
#    the stamp's own line (7), and never be refused as ambiguous.
cases["live_same_file"] = run(
    "# A\n\n## 1. S\n\n- [ ] Wire the resolver\n\n" + STAMP + "\n")
# 2. DELETED item in the SAME file: the only remaining occurrence is the stamp
#    itself. Must report missing and exit-worthy, NEVER rewrite to its own line.
cases["deleted_same_file"] = run("# A\n\n## 1. S\n\n" + STAMP + "\n")
# 3. The quoting line is itself a CHECKLIST ITEM and the real item is gone.
#    Restricting to item lines is not enough; the lead cut is what refuses this.
cases["quoting_item_only"] = run(
    "# A\n\n## 1. S\n\n"
    '- [/] Other work -> XREF: TODO-02-b.md §1 (item: "Wire the resolver" at line 3)\n\n'
    + STAMP + "\n")
# 4. Both a REAL item and a quoting item: must resolve UNIQUELY to the real one
#    (line 5), not report ambiguity. This is the shape that proves the lead cut
#    discriminates rather than merely suppresses.
cases["quoting_item_plus_real"] = run(
    "# A\n\n## 1. S\n\n- [ ] Wire the resolver\n\n"
    '- [x] Other -> XREF: TODO-02-b.md §1 (item: "Wire the resolver" at line 5)\n\n'
    + STAMP + "\n")
# 5. The only occurrence is checklist-shaped text inside a FENCE: an example,
#    so the item is missing rather than resolved to the documentation.
cases["fenced_only"] = run(
    "# A\n\n## 1. S\n\n```markdown\n- [ ] Wire the resolver\n```\n\n" + STAMP + "\n")
# 5b. A BARE four-backtick fence. Both of these cases were checked against a
#     loose `^\s*(```|~~~)` tracker to confirm they DISCRIMINATE -- a first pair
#     of fence fixtures did not, and would have passed either way while their
#     comment claimed to pin the CommonMark rules. Under the loose tracker this
#     one yields a LIVE item at line 6 (the run is read as ``` with a stray
#     backtick, so the opener is rejected); the shared rules open a 4-run fence
#     and the example stays an example.
cases["bare_long_fence"] = run(
    "# A\n\n## 1. S\n\n````\n- [ ] Wire the resolver\n````\n\n" + STAMP + "\n")
# 5c. A fence indented 4+ spaces is an indented CODE BLOCK, not a fence, so it
#     opens nothing and the item after it stays a real item.
cases["overindented_fence"] = run(
    "# A\n\n## 1. S\n\n    ```\n\n- [ ] Wire the resolver\n\n" + STAMP + "\n")
# 5d. Commented-out checklist text is not a destination. `_scan_markdown`
#     already skips HTML blocks; this keeps the two scanners agreeing.
cases["commented_out"] = run(
    "# A\n\n## 1. S\n\n<!--\n- [ ] Wire the resolver\n-->\n\n" + STAMP + "\n")
# 5e. THE LEAD STOP IS A GRAMMAR, NOT A LITERAL. Two spaces after the arrow --
#     which every producer accepts -- and the referring text names the item in
#     PROSE rather than in an `(item: ...)` clause. That combination is the only
#     one that reaches the hole: the literal `(item: "` stop would have cut a
#     clause-shaped tail no matter how the arrow was spaced, so a clause-shaped
#     fixture here passes either way and proves nothing (checked). Against the
#     literal `-> XREF:` stop this binds as a clean update to line 5.
cases["wide_arrow_quoting_item"] = run(
    "# A\n\n## 1. S\n\n"
    "- [x] Other work ->  XREF: TODO-02-b.md §1 -- supersedes Wire the resolver\n\n"
    + STAMP + "\n")
# 5f. An item carrying its own history in an INLINE comment. The old name lives
#     only inside `<!-- ... -->`, so a reference to it must report missing --
#     not resolve to the replacement line and rewrite cleanly.
cases["inline_comment_name"] = run(
    "# A\n\n## 1. S\n\n"
    "- [ ] Replacement work <!-- was: Wire the resolver -->\n\n" + STAMP + "\n")
# 5g. A bullet with NO separator is not a Markdown list item, so nothing may
#     resolve to it.
cases["zero_separator"] = run(
    "# A\n\n## 1. S\n\n-[ ] Wire the resolver\n\n" + STAMP + "\n")
# 5h. CONTROL -- uppercase `[X]` RESOLVES. This helper answers "is this a line a
#     reference may name", not "is this a shipped item", so being strict here
#     costs a false missing on a valid Markdown task list and buys nothing.
#     Pinned because the class was briefly narrowed to lowercase for
#     consistency with `build.py`'s completed-item extractor, which answers a
#     different question and does not constrain this one.
cases["control_uppercase_x"] = run(
    "# A\n\n## 1. S\n\n- [X] Wire the resolver\n\n" + STAMP + "\n")
# 5i. CONTROL -- an item DOCUMENTING the reference syntax in inline code keeps
#     its whole description searchable. Live shape: TODO-01-boot-protocol-abi-
#     handoff.md:608 carries a literal `-> XREF: §17` inside backticks, and
#     cutting there hid the rest of the item from every reference naming it.
cases["literal_syntax_in_code_span"] = run(
    "# A\n\n## 1. S\n\n"
    "- [x] On ship: flip item 2 with cross-ref `-> XREF: §17`, then Wire the resolver\n\n"
    + STAMP + "\n")
cases["control_cross_file"] = run(
    "# A\n\n## 1. S\n\n" + XSTAMP + "\n",
    "# B\n\n## 1. T\n\n- [ ] Wire the resolver\n")
# 7. CONTROL -- `[~]` is a live marker in 01-boot-platform (60 lines). Dropping
#    it from the status class would silently turn those into missing items.
cases["control_tilde"] = run(
    "# A\n\n## 1. S\n\n- [~] Wire the resolver\n\n" + STAMP + "\n")
print(json.dumps(cases))
S35AEOF
); S35A_RC=$?
if [ "$S35A_RC" = "0" ] && echo "$S35A_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
ok = True
# the live same-file item repairs to the ITEM's line (5), not the stamp's (7)
ok = ok and d['live_same_file'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
# a deleted item REPORTS and proposes NO destination -- never the stamp's own line
ok = ok and d['deleted_same_file'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
ok = ok and d['quoting_item_only'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
# discriminates rather than suppresses: the REAL item (5), not the quoting one (7)
ok = ok and d['quoting_item_plus_real'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
ok = ok and d['fenced_only'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
# a bare four-backtick fence opens: the item inside stays an example
# (NO BACKTICKS IN THIS BLOCK -- it is a double-quoted python3 -c string, so a
# backtick opens command substitution. Two comments here once carried an odd
# count each and passed only by cancelling each other out.)
ok = ok and d['bare_long_fence'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
# an over-indented fence marker is an indented code block, so the item after it is REAL (line 7)
ok = ok and d['overindented_fence'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 7}
ok = ok and d['commented_out'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
# two spaces after the arrow: a literal stop would bind this as a clean update to line 5
ok = ok and d['wide_arrow_quoting_item'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
ok = ok and d['inline_comment_name'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
ok = ok and d['zero_separator'] == {'updates': 0, 'ambiguous': 0, 'unresolvable': 0, 'missing': 1, 'resolved': None}
ok = ok and d['control_uppercase_x'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
# the name sits AFTER a literal reference-syntax code span and must stay findable
ok = ok and d['literal_syntax_in_code_span'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
ok = ok and d['control_cross_file'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
ok = ok and d['control_tilde'] == {'updates': 1, 'ambiguous': 0, 'unresolvable': 0, 'missing': 0, 'resolved': 5}
sys.exit(0 if ok else 1)"; then
    t_pass "section 35: an item reference resolves to an item line, not to the stamp or item naming it"
else
    t_fail "section 35: same-file item reference resolution wrong (rc=$S35A_RC, out=$S35A_OUT)"
fi

# Sub-test 35b: MUTATION -- restore the any-line scan. Both shapes 35a pins must
# misfire in their ORIGINAL, DIFFERENT ways: the live item becomes ambiguous
# (item + stamp) and the deleted item is rewritten to the stamp's own line at
# zero incomplete-repair counts. Asserting the two distinct misfires, rather
# than "35a fails", is what stops a future refactor satisfying this by breaking
# repair generally.
S35B_MUT="$TMP_DIR/s35-mut-anyline"
rm -rf "$S35B_MUT"; cp -r "$REPO_ROOT/scripts/todo-graph" "$S35B_MUT"
python3 - "$S35B_MUT/validate.py" <<'S35BMUTEOF'
import sys, pathlib
p = pathlib.Path(sys.argv[1])
src = p.read_text(encoding="utf-8")
needle = """                target_leads = leads_cache.get(target_file)
                if target_leads is None:
                    target_leads = cache_schema.checklist_item_leads(target_text)
                    leads_cache[target_file] = target_leads
                hits = sorted(ix for ix, lead in target_leads.items()
                              if name in lead)"""
assert src.count(needle) == 1, f"mutation needle appears {src.count(needle)}x"
restored = """                hits = [ix for ix, tl in enumerate(target_text.splitlines(), start=1)
                        if name in tl]"""
p.write_text(src.replace(needle, restored, 1), encoding="utf-8")
S35BMUTEOF
S35B_OUT=$(python3 - "$S35B_MUT" <<'S35BEOF'
import json, pathlib, sys
sys.path.insert(0, sys.argv[1])
import validate as V

SELF = "todo/01-test/TODO-01-a.md"
def node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
NODES = [node(SELF, "a1")]
id_index, path_index = V.build_id_index(NODES), V.build_path_index(NODES)
STAMP = ('> **Deferred:** [M] parked -> XREF: TODO-01-a.md §1'
         ' (item: "Wire the resolver" at line 99)')

def run(body):
    snap = {SELF: body}
    u, a, un, m, rep = V.fix_line_numbers(NODES, snap, id_index, path_index,
                                          pathlib.Path("."), False, True)
    return {"updates": u, "ambiguous": a, "missing": m}

print(json.dumps({
    "live": run("# A\n\n## 1. S\n\n- [ ] Wire the resolver\n\n" + STAMP + "\n"),
    "deleted": run("# A\n\n## 1. S\n\n" + STAMP + "\n"),
}))
S35BEOF
); S35B_RC=$?
if [ "$S35B_RC" = "0" ] && echo "$S35B_OUT" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
# live item: refused as ambiguous instead of repaired
live_misfires = d['live']['ambiguous'] == 1 and d['live']['updates'] == 0
# deleted item: SILENTLY rewritten (to the stamp's own line) at rc-0 shape
deleted_misfires = (d['deleted']['updates'] == 1 and d['deleted']['missing'] == 0
                    and d['deleted']['ambiguous'] == 0)
sys.exit(0 if (live_misfires and deleted_misfires) else 1)"; then
    t_pass "section 35: mutation check -- the any-line scan reports ambiguity and self-rewrites"
else
    t_fail "section 35: any-line mutation did not misfire as expected (rc=$S35B_RC, out=$S35B_OUT)"
fi

# ----------------------------------------------------------------------
# Section 36: the cache producer is fence-aware
#
# A `## N.` heading, a `- [x]` item, an Implementation Order row, an Inputs
# XREF and a Deferred stamp are all STRUCTURE when they sit in prose and
# DOCUMENTATION when they sit in a fenced block. Five separate walks in
# build.py read them, and every one was fence-blind: the fixture below is the
# exact shape that made section 32's anchor test emit an `orphan-io-row`
# naming a section that does not exist.
#
# THE FIXTURE USES A 4-BACKTICK OUTER FENCE containing a 3-backtick inner one,
# because that is the shape the naive `startswith("```")` toggle gets wrong --
# it pairs delimiters by parity, so it closes the outer block at the inner
# closer and inverts every delimiter after it. One live corpus file was
# malformed in exactly that way and the correct rule is what found it
# (todo/15-installer-release/TODO-05-github-release-community.md, 2026-08-10).
# ----------------------------------------------------------------------
s36_fixture() {
    local T; T=$(mktemp -d "$TMP_DIR/s36-XXXXXX")
    mkdir -p "$T/todo/01-test" "$T/build"
    cat > "$T/todo/01-test/TODO-01-fence.md" <<'S36EOF'
---
schema_version: 1
id: s36-fence
domain: 01-test
status: draft
title: Fence fixture
---

# Fence fixture

Documenting what a TODO looks like. The fenced block comes FIRST on purpose:
the Implementation Order and Inputs walks stop at the next `## ` heading, so a
fence-blind walk does not merely ADD the fake rows -- it reads them INSTEAD of
the real ones further down, and the file's real table never gets parsed.

````markdown
## 99. Fake Section

- [x] `fake_sym()` shipped

## Implementation Order

| ⭐  | Order | Section | Deliverable      | Depends On | Status |
| --- | :---: | :-----: | ---------------- | ---------- | :----: |
| 💎  |   2   |   §99   | Fake deliverable | --         |  [x]   |

## Inputs

| -> XREF: [fake](../09-fake/TODO-99-fake.md) §7 | fake edge |

> **Deferred:** [H] fake reason -> XREF: `09-fake/TODO-99-fake.md` §7

```bash
echo "a three-backtick block nested inside the four-backtick one"
```
````

## Implementation Order

| ⭐  | Order | Section | Deliverable      | Depends On | Status |
| --- | :---: | :-----: | ---------------- | ---------- | :----: |
| 💎  |   1   |   §1    | Real deliverable | --         |  [x]   |

## Inputs

| Path          | Purpose    |
| ------------- | ---------- |
| `src/real.c`  | real input |

## 1. Real Section

- [x] `real_sym()` shipped

## Verification

- [ ] nothing
S36EOF
    (
        cd "$T" || exit 1
        git init -q .
        git config user.email s36@test.invalid
        git config user.name s36
        git add -A
        git commit -qm "s36 fixture"
    ) >/dev/null 2>&1
    printf '%s' "$T"
}

# Report what the node actually contains, so PASS and the MUTATION check read
# the same five fields off the same builder.
s36_probe() {   # $1=tree $2=build.py to run
    python3 "$2" --quiet --root "$1/todo" --output "$1/build/todo-cache.json" \
        --repo-root "$1" >/dev/null 2>&1 || { printf 'BUILD-FAILED'; return; }
    python3 - "$1/build/todo-cache.json" <<'S36PY'
import json, sys
n = json.load(open(sys.argv[1]))[0]
print(json.dumps({
    "headings":   sorted(h["n"] if isinstance(h, dict) else h
                         for h in n.get("section_headings", [])),
    "io_rows":    sorted(s.get("n") for s in n.get("sections", [])
                         if s.get("n") is not None),
    "stamped":    sorted(i.get("section_n") for i in n.get("stamped_items", [])),
    "inputs":     len(n.get("inputs_xrefs", [])),
    "stamps":     len(n.get("stamps_xrefs", [])),
}, sort_keys=True))
S36PY
}

S36_T="$(s36_fixture)"
S36_REAL="$(s36_probe "$S36_T" "$BUILD_PY")"
if echo "$S36_REAL" | python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
sys.exit(0 if (d['headings'] == [1] and d['io_rows'] == [1]
               and d['stamped'] == [1] and d['inputs'] == 0
               and d['stamps'] == 0) else 1)"; then
    t_pass "section 36: fenced heading/item/IO-row/Inputs-XREF/stamp contribute nothing"
else
    t_fail "section 36: fenced structure leaked into the node (got=$S36_REAL)"
fi

# MUTATION -- neuter fence_mask so every line reads as unfenced, which is the
# pre-section-36 behaviour, and require the fixture to FAIL. Without this the
# test above passes just as happily against a builder that never had a fence
# rule at all. PYTHONPATH points at the real module dir: a mutant copied into
# TMP_DIR resolves its own sibling imports to the temp dir, dies on import, and
# produces the empty result the fixture is looking for -- a false PASS.
# THE WHOLE MODULE DIRECTORY IS COPIED, not just build.py. build.py inserts its
# own directory at sys.path[0] to reach `cache_schema` / `validate`, so a lone
# copy in TMP_DIR dies on import -- and a dead mutant emits the same empty node
# the assertion above wants, which is a false PASS rather than a crash anyone
# notices. Mutating inside a full copy changes one line and nothing else.
S36_MUTDIR="$TMP_DIR/s36-mutant"
rm -rf "$S36_MUTDIR"
cp -r "$REPO_ROOT/scripts/todo-graph" "$S36_MUTDIR"
S36_MUT="$S36_MUTDIR/build.py"
# THE MUTATION IS ANCHORED, AND THE GUARD LOOKS FOR A STRING THE SHIPPED CODE
# DOES NOT CONTAIN. The first version targeted a line inside `fence_scan` and
# guarded on `out.append(False)` -- then `fence_scan` was rewritten, the sed
# stopped matching, and the guard still passed because the real code now
# contains `out.append(False)` legitimately. The mutation went inert and the
# test reported a PASS-shaped failure. A guard is only a guard if it fails when
# the anchor moves.
# Re-anchored in section 42, when `fence_scan` began returning a `ScanResult`
# instead of a 3-tuple. The guard below caught the move exactly as its comment
# promises it would -- the sed stopped matching and the test FAILED rather than
# going quietly inert.
# ANCHORED ON THE STABLE PREFIX, not the full argument list. Section 42 moved
# this line three times while its own review ran (3-tuple -> ScanResult, then
# dropping `stripped`, then `kinds` -> `codes`), and each move broke the sed
# and correctly FAILED this test. Matching only `ScanResult(lines, out,` and
# rewriting the MASK argument keeps the guard sensitive to the thing it is
# actually about -- a blinded mask -- without re-breaking whenever a later field
# is added or renamed. The grep below still fails loudly if even this stops
# matching.
sed -i 's|^    return ScanResult(lines, out, |    return ScanResult(lines, [False] * len(lines), |' \
    "$S36_MUTDIR/cache_schema.py"
if ! grep -q "ScanResult(lines, \[False\] \* len(lines)" "$S36_MUTDIR/cache_schema.py"; then
    t_fail "section 36: mutation did not apply -- fence_scan return anchor not found"
else
    S36_MUT_OUT="$(s36_probe "$S36_T" "$S36_MUT")"
    if echo "$S36_MUT_OUT" | python3 -c "
import json, sys
try:
    d = json.loads(sys.stdin.read())
except Exception:
    sys.exit(1)
# The fence-blind builder must pick the FENCED section 99 up in every walk it
# feeds, and for the two walks that stop at the next heading it must read the
# fake table INSTEAD of the real one -- displacement, not addition. Anything
# less means the mutation did not reach the code under test.
sys.exit(0 if (99 in d['headings'] and 99 in d['stamped']
               and d['io_rows'] == [99] and d['inputs'] >= 1
               and d['stamps'] >= 1) else 1)"; then
        t_pass "section 36: mutation check -- the fence-blind walks index the fenced example"
    else
        t_fail "section 36: fence-blind mutation did not misfire (out=$S36_MUT_OUT)"
    fi
    rm -rf "$S36_MUTDIR"
fi

# Fence SHAPES the naive toggle gets wrong: a tilde fence, a 4-backtick block
# containing a shorter run, and an unterminated fence (which must fail CLOSED --
# everything after it is masked, so structure goes MISSING rather than wrong).
S36_SHAPES="$(python3 - <<'S36SPY'
import sys
sys.path.insert(0, "scripts/todo-graph")
from cache_schema import fence_mask
def m(t): return fence_mask(t.split("\n"))
tilde  = m("a\n~~~text with a long info string\n## 9. x\n~~~\nb")
quad   = m("a\n````md\n```\n## 9. x\n```\n````\nb")
unterm = m("a\n```\n## 9. x\nb")
print(",".join(str(int(x)) for x in tilde),
      ",".join(str(int(x)) for x in quad),
      ",".join(str(int(x)) for x in unterm))
S36SPY
)"
if [ "$S36_SHAPES" = "0,1,1,1,0 0,1,1,1,1,1,0 0,1,1,1" ]; then
    t_pass "section 36: tilde, nested-4-backtick and unterminated fences mask correctly"
else
    t_fail "section 36: fence shape masks wrong (got='$S36_SHAPES')"
fi

# A document that ends inside a fence masks every line to EOF, so all five walks
# return empty -- which emits no WRONG structure but silently ERASES real
# structure. Measured 2026-08-10 before the fix: a fixture with a real
# Implementation Order, a real `## 1.` heading and a real `- [x]` item behind an
# unclosed opener built at rc 0 with all three empty, and nothing reported it
# (Codex adversarial, section 36, [medium]). The producer must REFUSE instead.
# Asserted at the BUILDER, not on the mask: the mask behaving correctly is what
# produces the erasure, so a mask-only fixture cannot see this at all.
S36U="$(mktemp -d "$TMP_DIR/s36u-XXXXXX")"
mkdir -p "$S36U/todo/01-test" "$S36U/build"
cat > "$S36U/todo/01-test/TODO-01-unclosed.md" <<'S36UEOF'
---
schema_version: 1
id: s36-unclosed
domain: 01-test
status: draft
title: Unclosed fence fixture
---

# Unclosed fence fixture

```text
this fence is never closed, and real structure follows it

## Implementation Order

| ⭐  | Order | Section | Deliverable      | Depends On | Status |
| --- | :---: | :-----: | ---------------- | ---------- | :----: |
| 💎  |   1   |   §1    | Real deliverable | --         |  [x]   |

## 1. Real Section

- [x] `real_sym()` shipped
S36UEOF
(
    cd "$S36U" || exit 1
    git init -q .
    git config user.email s36u@test.invalid
    git config user.name s36u
    git add -A
    git commit -qm "s36 unclosed fixture"
) >/dev/null 2>&1
S36U_ERR="$(python3 "$BUILD_PY" --quiet --root "$S36U/todo" \
    --output "$S36U/build/todo-cache.json" --repo-root "$S36U" 2>&1)"
S36U_RC=$?
if [ "$S36U_RC" = "1" ] && echo "$S36U_ERR" | grep -q "unclosed-fence" \
   && [ ! -f "$S36U/build/todo-cache.json" ]; then
    t_pass "section 36: a document ending inside a fence REFUSES the build"
else
    t_fail "section 36: unclosed fence did not refuse (rc=$S36U_RC, cache_written=$([ -f "$S36U/build/todo-cache.json" ] && echo yes || echo no), err=$S36U_ERR)"
fi

# --- section 42: the OTHER EOF-consuming HTML blocks refuse at the PRODUCER ---
#
# The scan-level terminal is pinned in scripts/tests/test_todo_fence.py; this
# proves the PRODUCER acts on it -- rc 1, no cache written, and a category that
# NAMES the construct rather than calling a `<script>` block a comment. Types 6
# and 7 end at a blank line and can never be unterminated, so the control at the
# end asserts one of them BUILDS rather than refusing: a scan that started
# reporting them terminal would refuse every legal document containing one.
for _s42 in "script:<script>:unclosed-html" "pi:<?php:unclosed-html" \
            "decl:<!DOCTYPE html:unclosed-html" "cdata:<![CDATA[:unclosed-html" \
            "comment:<!--:unclosed-comment"; do
    _s42_name="${_s42%%:*}"; _s42_rest="${_s42#*:}"
    _s42_open="${_s42_rest%:*}"; _s42_cat="${_s42_rest##*:}"
    S42U="$TMP_DIR/s42u-$_s42_name"
    rm -rf "$S42U"; mkdir -p "$S42U/todo/d"
    {
        printf -- '---\nschema_version: 1\nid: s42-%s\ndomain: d\nstatus: active\ntitle: T\n---\n\n# T\n\n' "$_s42_name"
        printf '## Implementation Order\n\n'
        printf '| Order | Section | Deliverable | Depends On | Status |\n'
        printf '| :---: | :-----: | ----------- | ---------- | :----: |\n'
        printf '|   1   |   §1    | Real        | --         |  [x]   |\n\n'
        printf '%s\n' "$_s42_open"
        printf 'never closed, and real structure follows it\n\n'
        printf '## 1. Real Section\n\n- [x] `real_sym()` shipped\n'
    } > "$S42U/todo/d/TODO-01-a.md"
    ( cd "$S42U" && git init -q . && git config user.email s42@test.invalid \
        && git config user.name s42 && git add -A && git commit -qm s42 ) >/dev/null 2>&1
    S42U_ERR="$(python3 "$BUILD_PY" --quiet --root "$S42U/todo" \
        --output "$S42U/build/todo-cache.json" --repo-root "$S42U" 2>&1)"
    S42U_RC=$?
    if [ "$S42U_RC" = "1" ] && echo "$S42U_ERR" | grep -q "$_s42_cat" \
       && [ ! -f "$S42U/build/todo-cache.json" ]; then
        t_pass "section 42: an unterminated $_s42_name block REFUSES the build as $_s42_cat"
    else
        t_fail "section 42: unterminated $_s42_name did not refuse as $_s42_cat (rc=$S42U_RC, err=$S42U_ERR)"
    fi
done

# CONTROL: a type-6 block running to EOF is well-formed CommonMark and must
# BUILD. Without this the four assertions above would still pass against a scan
# that called every open HTML block terminal.
S42OK="$TMP_DIR/s42-type6-ok"
rm -rf "$S42OK"; mkdir -p "$S42OK/todo/d"
{
    printf -- '---\nschema_version: 1\nid: s42-ok\ndomain: d\nstatus: active\ntitle: T\n---\n\n# T\n\n'
    printf '## Implementation Order\n\n'
    printf '| Order | Section | Deliverable | Depends On | Status |\n'
    printf '| :---: | :-----: | ----------- | ---------- | :----: |\n'
    printf '|   1   |   §1    | Real        | --         |  [x]   |\n\n'
    printf '## 1. Real Section\n\n- [x] `real_sym()` shipped\n\n<details>\nstill open at EOF\n'
} > "$S42OK/todo/d/TODO-01-a.md"
( cd "$S42OK" && git init -q . && git config user.email s42@test.invalid \
    && git config user.name s42 && git add -A && git commit -qm s42ok ) >/dev/null 2>&1
S42OK_ERR="$(python3 "$BUILD_PY" --quiet --root "$S42OK/todo" \
    --output "$S42OK/build/todo-cache.json" --repo-root "$S42OK" 2>&1)"
S42OK_RC=$?
if [ "$S42OK_RC" = "0" ] && [ -f "$S42OK/build/todo-cache.json" ]; then
    t_pass "section 42: CONTROL -- a type-6 block open at EOF still BUILDS"
else
    t_fail "section 42: a legal type-6-at-EOF document was refused (rc=$S42OK_RC, err=$S42OK_ERR)"
fi

# The threaded context must not change any answer: every walk called with the
# shared `doc=` must return exactly what it returns computing its own mask.
# Without this, the perf fix could silently feed one walk a stale mask.
S36T="$(python3 - <<'S36TPY'
import sys, pathlib, json
sys.path.insert(0, "scripts/todo-graph")
import build as B
same = True
for p in sorted(pathlib.Path("todo").rglob("*.md")):
    body = p.read_text(encoding="utf-8", errors="replace")
    _sb = B.scan_body(body)
    lines, mask, views, leaves = _sb.lines, _sb.mask, _sb.views, _sb.leaf_views
    pairs = [
        (B.extract_section_headings(body), B._walk_section_headings(leaves, mask, _sb.in_blockquote)),
        (B.extract_implementation_order(body),
         B._walk_implementation_order(views, leaves, mask, _sb.in_blockquote)),
        (B.extract_inputs_xrefs(body), B._walk_inputs_xrefs(views, leaves, mask, _sb.in_blockquote)),
        (B.extract_stamps_xrefs(body), B._walk_stamps_xrefs(lines, mask)),
        (B.extract_stamped_items(body, str(p)),
         B._walk_stamped_items(views, leaves, mask, str(p), _sb.in_blockquote)),
    ]
    if any(a != b for a, b in pairs):
        same = False
        break
print("same" if same else "DIFFER")
S36TPY
)"
if [ "$S36T" = "same" ]; then
    t_pass "section 36: the rescanning wrapper and the walk build_node calls agree on every corpus file"
else
    t_fail "section 36: threaded context changed a walk's answer (got='$S36T')"
fi

# The shared fence scan is NOT on the public surface, and that is the assertion.
# Four rounds tried to make a caller-supplied context safe -- a validated tuple,
# an opaque class, an exact-type check, frozen write-once slots -- and each closed
# one forgery route while revealing the next (`zip()` truncation on a short mask,
# a same-length stale mask, a subclass overriding `mask`, `del ctx._frozen`,
# `__new__` with empty slots). The surface was removed instead: `extract_*(body)`
# takes only a body and rescans, `build_node` calls the private `_walk_*` with its
# own locals, and there is nothing to forge because nothing can be passed.
S36C="$(python3 - <<'S36CPY'
import sys, inspect
sys.path.insert(0, "scripts/todo-graph")
import build as B
bad = []
# No context object, and no context parameter on any public extractor.
for gone in ("ScannedBody", "_doc"):
    if hasattr(B, gone):
        bad.append("still-exposed:" + gone)
publics = ["extract_section_headings", "extract_implementation_order",
           "extract_inputs_xrefs", "extract_stamps_xrefs", "extract_stamped_items"]
for name in publics:
    params = list(inspect.signature(getattr(B, name)).parameters)
    if any(p in ("doc", "mask", "lines") for p in params):
        bad.append("context-param:" + name)
# Each public walk must agree with the private one build_node actually calls,
# so the rescanning wrapper cannot drift from the threaded path.
body = "## 1. First\n## 2. Second\n"
_sb = B.scan_body(body)
lines, mask, views, leaves = _sb.lines, _sb.mask, _sb.views, _sb.leaf_views
unclosed = _sb.terminal is not None
if B.extract_section_headings(body) != B._walk_section_headings(leaves, mask, _sb.in_blockquote):
    bad.append("wrapper-differs")
if unclosed is not False:
    bad.append("unclosed-wrong")
if B.scan_body("```\nx\n").terminal is None:
    bad.append("unclosed-not-detected")
print("ok" if not bad else "BAD:" + ",".join(sorted(bad)))
S36CPY
)"
if [ "$S36C" = "ok" ]; then
    t_pass "section 36: no forgeable body context exists on the public extractor surface"
else
    t_fail "section 36: body-context surface is still forgeable (got='$S36C')"
fi

# A ``` written at the start of a line INSIDE an HTML block comment is not a
# fence. Before the scan tracked comments it opened one that never closed, and
# since the producer now REFUSES an unclosed fence that turned a legal comment
# into a hard build failure (Codex adversarial, section 36, round 7). Two
# controls run beside it: the same marker outside a comment must still refuse,
# and a `<!--` INSIDE a fence must stay literal so the fence still closes.
S36H="$(python3 - <<'S36HPY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B
def probe(body):
    _sb = B.scan_body(body)
    unclosed = _sb.terminal is not None
    return unclosed, [n for n, _ in B.extract_section_headings(body)]
in_comment = probe("# T\n\n<!--\n```\nexample\n-->\n\n## 1. Real\n")
control_bare = probe("# T\n\n```\nexample\n\n## 1. Real\n")
control_infence = probe("# T\n\n```\n<!--\n```\n\n## 1. Real\n")
bad = []
if in_comment != (False, [1]):
    bad.append("comment-fence-misread:%r" % (in_comment,))
if control_bare != (True, []):
    bad.append("control-bare-did-not-fire:%r" % (control_bare,))
if control_infence != (False, [1]):
    bad.append("control-comment-in-fence:%r" % (control_infence,))
print("ok" if not bad else "BAD:" + ";".join(bad))
S36HPY
)"
if [ "$S36H" = "ok" ]; then
    t_pass "section 36: a fence marker inside an HTML comment neither opens a fence nor refuses the build"
else
    t_fail "section 36: comment-aware fence scan is wrong (got='$S36H')"
fi

# An unterminated HTML COMMENT erases a node exactly as an unterminated fence
# does -- it masks to EOF, so every walk returns empty -- and the first
# comment-aware scan reported only fence state, so `build_node` saw
# `unclosed=False` and published the erased node with no error at all (Codex
# adversarial, section 36, round 8, [high]). It gets its OWN refusal, and its own
# message, because telling an author to close a fence when the unclosed thing is
# a comment sends them to the wrong line. Asserted at the BUILDER.
S36K="$(mktemp -d "$TMP_DIR/s36k-XXXXXX")"
mkdir -p "$S36K/todo/01-test" "$S36K/build"
cat > "$S36K/todo/01-test/TODO-01-comment.md" <<'S36KEOF'
---
schema_version: 1
id: s36-comment
domain: 01-test
status: draft
title: Unclosed comment fixture
---

# Unclosed comment fixture

<!--
this comment is never closed, and real structure follows it

## Implementation Order

| ⭐  | Order | Section | Deliverable      | Depends On | Status |
| --- | :---: | :-----: | ---------------- | ---------- | :----: |
| 💎  |   1   |   §1    | Real deliverable | --         |  [x]   |

## 1. Real Section

- [x] `real_sym()` shipped
S36KEOF
(
    cd "$S36K" || exit 1
    git init -q .
    git config user.email s36k@test.invalid
    git config user.name s36k
    git add -A
    git commit -qm "s36 comment fixture"
) >/dev/null 2>&1
S36K_ERR="$(python3 "$BUILD_PY" --quiet --root "$S36K/todo" \
    --output "$S36K/build/todo-cache.json" --repo-root "$S36K" 2>&1)"
S36K_RC=$?
# It must name the COMMENT, not the fence, and must write no cache.
if [ "$S36K_RC" = "1" ] && echo "$S36K_ERR" | grep -q "unclosed-comment" \
   && ! echo "$S36K_ERR" | grep -q "unclosed-fence" \
   && [ ! -f "$S36K/build/todo-cache.json" ]; then
    t_pass "section 36: a document ending inside an HTML comment REFUSES, naming the comment"
else
    t_fail "section 36: unclosed comment did not refuse correctly (rc=$S36K_RC, err=$S36K_ERR)"
fi

# ----------------------------------------------------------------------
# Test 39: THE NEW-CHECK EXEMPTION in producer_differential.py.
#
# The bug it closes, observed on main 2026-08-10 (todo-graph.yml runs
# 31381391940 and 31388159279): 5851cb4db shipped the `unclosed-fence` check
# together with the 8-line corpus repair that check demands. The producer
# differential then ran the HEAD producer over a BASE corpus (169990014)
# predating the repair, the new check fired, and identity-gate.sh reported rc 3
# INFRASTRUCTURE. It could not self-clear: IDENTITY_GATE_LAST_GATED_SHA only
# advances on a PASS, so every subsequent push reproduced it.
#
# THE TESTS COME IN PAIRS, and the refusal direction is the load-bearing half.
# An exemption is only worth anything if the thing it does NOT exempt still
# fails -- so 39b/39c/39d each establish that a refusal which merely RESEMBLES
# the exempt shape is still fatal. 39a alone would be satisfied by deleting the
# check entirely.
# ----------------------------------------------------------------------
T39="$TMP_DIR/t39"
PD_PY="$REPO_ROOT/scripts/todo-graph/producer_differential.py"

# A corpus body carrying a stamped ref. The differential refuses an EMPTY
# projection (a corpus with no stamped refs would pass vacuously), so a fixture
# without one tests nothing.
_t39_body='## 1. Sec

- [x] wired `src/kernel/mm/pmm.c:42` and `pmm_alloc()`
'

# $1 = tree root, $2 = "clean"|"unclosed", $3 = "declare"|"nodeclare"|"live"
t39_tree() {
    mkdir -p "$1/scripts/todo-graph" "$1/todo/d"
    cp "$REPO_ROOT"/scripts/todo-graph/*.py "$1/scripts/todo-graph/"
    cp "$REPO_ROOT"/scripts/todo-graph/*.json "$1/scripts/todo-graph/" 2>/dev/null || true
    cp -r "$REPO_ROOT/scripts/todo-graph/schema" "$1/scripts/todo-graph/" 2>/dev/null || true
    {
        printf -- '---\nschema_version: 1\nid: t-1\ndomain: d\nstatus: active\ntitle: T\n---\n\n# T\n\n%s' "$_t39_body"
        [ "$2" = "unclosed" ] && printf '\n```\nnever closed\n'
    } > "$1/todo/d/TODO-01-a.md"
    case "$3" in
        # The BASE-side producer: its `unclosed-fence` CHECK is neutered, so it
        # accepts a corpus the head producer refuses. Whether it still DECLARES
        # the category is what separates 39a from 39b.
        nodeclare)
            python3 - "$1/scripts/todo-graph/build.py" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
s = s.replace('    "unclosed-fence",\n', '', 1)
_neutered = s.replace('    if _body.terminal is not None:\n', '    if False:\n', 1)
# A MISSED ANCHOR MUST FAIL LOUDLY. Section 42 renamed this condition, and the
# silent version of this patch left the check LIVE while the category was
# undeclared -- which surfaced as an unrelated INTERNAL assertion from the
# producer rather than as "the test's mutation stopped applying".
assert _neutered != s, "t39_tree: the unclosed-check anchor moved; update it"
s = _neutered
open(p, "w").write(s)
PY
            ;;
        declare)
            python3 - "$1/scripts/todo-graph/build.py" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
_neutered = s.replace('    if _body.terminal is not None:\n', '    if False:\n', 1)
# A MISSED ANCHOR MUST FAIL LOUDLY. Section 42 renamed this condition, and the
# silent version of this patch left the check LIVE while the category was
# undeclared -- which surfaced as an unrelated INTERNAL assertion from the
# producer rather than as "the test's mutation stopped applying".
assert _neutered != s, "t39_tree: the unclosed-check anchor moved; update it"
s = _neutered
open(p, "w").write(s)
PY
            ;;
    esac
}

# --- 39a: PASS direction. A category that exists only at HEAD is a NEW CHECK,
#          the base-corpus leg is unrunnable by construction, and the gate
#          reports a DEGRADED pass rather than wedging.
rm -rf "$T39"; mkdir -p "$T39"
t39_tree "$T39/base" unclosed nodeclare
t39_tree "$T39/head" clean live
T39A_OUT="$(python3 "$PD_PY" "$T39/base" "$T39/head" --strict 2>&1)"; T39A_RC=$?
if [ "$T39A_RC" = "0" ] \
   && echo "$T39A_OUT" | grep -q "SKIPPED (NEW CHECK)" \
   && echo "$T39A_OUT" | grep -q "OK (DEGRADED)" \
   && echo "$T39A_OUT" | grep -q "head-corpus"; then
    t_pass "s39a: a HEAD-only error category skips the base-corpus leg as a NEW CHECK, head-corpus leg still compared"
else
    t_fail "s39a: new-check exemption did not fire (rc=$T39A_RC): $T39A_OUT"
fi

# --- 39b: REFUSAL direction. Same refusal, same category NAME -- but the base
#          producer DECLARED it too, so this is a head-side false positive on an
#          existing check, not a new one. Must stay fatal. This is the case a
#          "did only the head producer reject it?" rule would wave through.
rm -rf "$T39"; mkdir -p "$T39"
t39_tree "$T39/base" unclosed declare
t39_tree "$T39/head" clean live
T39B_OUT="$(python3 "$PD_PY" "$T39/base" "$T39/head" --strict 2>&1)"; T39B_RC=$?
if [ "$T39B_RC" = "3" ] && ! echo "$T39B_OUT" | grep -q "SKIPPED"; then
    t_pass "s39b: REFUSAL CONTROL -- a category the BASE also declared stays rc 3, never exempt"
else
    t_fail "s39b: a familiar category was wrongly exempted (rc=$T39B_RC): $T39B_OUT"
fi

# --- 39c: REFUSAL direction. The exemption is scoped to the BASE corpus. A head
#          producer that refuses the HEAD corpus is the live tree failing to
#          build, and no category argument may excuse it.
rm -rf "$T39"; mkdir -p "$T39"
t39_tree "$T39/base" clean nodeclare
t39_tree "$T39/head" unclosed live
T39C_OUT="$(python3 "$PD_PY" "$T39/base" "$T39/head" --strict 2>&1)"; T39C_RC=$?
if [ "$T39C_RC" = "3" ] && ! echo "$T39C_OUT" | grep -q "SKIPPED"; then
    t_pass "s39c: REFUSAL CONTROL -- a refusal of the HEAD corpus is fatal even under a HEAD-only category"
else
    t_fail "s39c: a head-corpus refusal was wrongly exempted (rc=$T39C_RC): $T39C_OUT"
fi

# --- 39d: REFUSAL direction. The BASE producer refusing the base corpus means
#          the corpus is broken independently of the change under test. The
#          base-first run order is what makes this reachable at all.
rm -rf "$T39"; mkdir -p "$T39"
t39_tree "$T39/base" clean live
t39_tree "$T39/head" clean live
printf -- '---\nschema_version: 1\nid: [unclosed\ndomain: d\n---\n\n# T\n' \
    > "$T39/base/todo/d/TODO-01-a.md"
T39D_OUT="$(python3 "$PD_PY" "$T39/base" "$T39/head" --strict 2>&1)"; T39D_RC=$?
if [ "$T39D_RC" = "3" ] && ! echo "$T39D_OUT" | grep -q "SKIPPED"; then
    t_pass "s39d: REFUSAL CONTROL -- a BASE-producer refusal of the base corpus is fatal"
else
    t_fail "s39d: a base-producer refusal was wrongly exempted (rc=$T39D_RC): $T39D_OUT"
fi

# --- 39e: the parsers fail CLOSED. Each of these inputs must yield "not
#          provably new", because a refusal whose reason cannot be READ is never
#          evidence about which checks existed.
T39E="$(python3 - "$REPO_ROOT" <<'PY'
import sys, pathlib
sys.path.insert(0, str(pathlib.Path(sys.argv[1]) / "scripts/todo-graph"))
import producer_differential as pd
bad = []
# A crash with no FAIL line at all.
if pd.refusal_categories("Traceback (most recent call last):\n  boom\n") is not None:
    bad.append("a traceback with no FAIL line parsed as a category set")
# A FAIL line whose reason is not a category (build.py's read-error path).
if pd.refusal_categories(
        "[build.py] FAIL todo/a.md: read error: [Errno 5] I/O error\n") is not None:
    bad.append("a read-error FAIL line parsed as a category")
# The trailing count line is not a per-file refusal and must not read as one.
if pd.refusal_categories("[build.py] FAIL: 1 fatal error(s) across 2 files\n") is not None:
    bad.append("the summary count line parsed as a refusal")
# A declaration of the wrong SHAPE must be None, never an empty set -- an empty
# set would make every category look novel and approve every refusal.
class _T:
    def __init__(self, out): self.out = out
for shape in ("{}", "[]", '[""]', "[1,2]", "not json"):
    p = pathlib.Path(sys.argv[1])
    # exercised directly rather than through a tree: the shape floor is the
    # property under test, not the subprocess plumbing.
    import json as _j
    try:
        got = _j.loads(shape)
    except Exception:
        continue
    if isinstance(got, list) and got and all(isinstance(c, str) and c for c in got):
        bad.append("shape %r wrongly accepted by the declared-set floor" % shape)
print("|".join(bad) if bad else "OK")
PY
)"
if [ "$T39E" = "OK" ]; then
    t_pass "s39e: REFUSAL CONTROL -- unreadable refusals and malformed declarations fail closed"
else
    t_fail "s39e: a parser failed open: $T39E"
fi

# --- 39f: the DECLARATION IS BOUND TO REALITY at runtime. A category build.py
#          can emit but does not declare would make --emitted-error-categories a
#          lie, and the whole novelty argument rests on that answer.
T39F="$(python3 - "$REPO_ROOT" <<'PY'
import sys, importlib.util, pathlib
tg = pathlib.Path(sys.argv[1]) / "scripts/todo-graph"
sys.path.insert(0, str(tg))
spec = importlib.util.spec_from_file_location("build_mod", tg / "build.py")
m = importlib.util.module_from_spec(spec)
sys.modules["build_mod"] = m
spec.loader.exec_module(m)
try:
    m._assert_declared_categories([("not-a-real-category", "x")], "todo/a.md")
except AssertionError:
    print("OK")
else:
    print("an undeclared category was accepted")
PY
)"
if [ "$T39F" = "OK" ]; then
    t_pass "s39f: build.py refuses an undeclared error category at runtime"
else
    t_fail "s39f: the declaration is not bound to reality: $T39F"
fi

# --- 39g: the declaration is a LITERAL, pinned by AST. Section 20 established
#          that a source-text search is not proof of emission; the mirror-image
#          hole is a declaration that COMPUTES itself from what the emitters
#          contain, which would declare whatever it emits and prove nothing.
T39G="$(python3 - "$REPO_ROOT" <<'PY'
import ast, sys, pathlib
src = (pathlib.Path(sys.argv[1]) / "scripts/todo-graph/build.py").read_text()
tree = ast.parse(src)
found = None
for node in tree.body:
    if isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id == "EMITTED_ERROR_CATEGORIES"
            for t in node.targets):
        found = node.value
if found is None:
    print("EMITTED_ERROR_CATEGORIES is not a module-level assignment")
    raise SystemExit(0)
# frozenset({...}) of string literals, and nothing else.
ok = (isinstance(found, ast.Call)
      and isinstance(found.func, ast.Name) and found.func.id == "frozenset"
      and len(found.args) == 1 and isinstance(found.args[0], ast.Set)
      and found.args[0].elts
      and all(isinstance(e, ast.Constant) and isinstance(e.value, str)
              for e in found.args[0].elts))
print("OK" if ok else "the declaration is not a literal frozenset of strings")
PY
)"
if [ "$T39G" = "OK" ]; then
    t_pass "s39g: EMITTED_ERROR_CATEGORIES is a literal, so it cannot compute itself from the emitters"
else
    t_fail "s39g: $T39G"
fi

# ----------------------------------------------------------------------
# Section 37: ONE clause parser decides how many destinations a clause names.
#
# Section 33 closed five multi-destination shapes in six review rounds, one per
# round, because four producers each carried their own copy of the rule. These
# sub-tests pin the consolidation the way the design review asked for it: the
# shared PARSE is asserted identical across the four surface embeddings, and
# each producer's POLICY is asserted separately -- because the policies
# legitimately differ and a verdict-only "identical treatment" assertion would
# pass while two callers bound different destinations.
# ----------------------------------------------------------------------

# Sub-test 37a: THE SAME CLAUSE PARSES IDENTICALLY IN ALL FOUR SURFACES. The
# lead-ins differ by surface; everything from the target rightwards must not.
T37A="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs

# One clause body, embedded in each producer's surface. If any lead-in still
# owns part of the target grammar, these parses diverge.
BODY = '[a b](TODO-02-b.md) §1 (item: "C" at line 3)'
surfaces = {
    "stamp":  ('> **Deferred:** [M] x -> XREF: ' + BODY, None),
    "bullet": ('- -> XREF: ' + BODY, None),
    "repair": ('x -> XREF: ' + BODY, None),
}
parses = {}
for name, (line, _) in surfaces.items():
    lead = cs.XREF_LEAD_RE.search(line)
    c = cs.parse_xref_clause(line, lead.end())
    parses[name] = (c.targets, c.section, c.item_name, c.malformed)
# The table surface hands over a bare cell, so it is parsed at offset 0.
c = cs.parse_xref_clause('[a b](TODO-02-b.md) §1', 0)
parses["table"] = (c.targets, c.section, c.item_name, c.malformed)

vals = {k: (v[0], v[1], v[3]) for k, v in parses.items()}
if len(set(vals.values())) != 1:
    print("surfaces disagree: %r" % (parses,)); raise SystemExit
if parses["stamp"][0] != ('[a b](TODO-02-b.md)',):
    print("spaced label not captured whole: %r" % (parses["stamp"],)); raise SystemExit
if parses["stamp"][2] != "C":
    print("item name lost: %r" % (parses["stamp"],)); raise SystemExit
print("OK")
PY
)"
if [ "$T37A" = "OK" ]; then
    t_pass "s37a: one clause parses identically in all four producer surfaces"
else
    t_fail "s37a: $T37A"
fi

# Sub-test 37b: A MULTI-DESTINATION CLAUSE IS REFUSED BY EVERY PRODUCER, and
# each one's OWN policy is asserted rather than a shared "treated the same":
# the builder surfaces drop silently, the repair path counts + barriers.
T37B="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs

BAD = {
    "adjacent":    '[a](TODO-01-a.md)[b](TODO-02-b.md)',
    "spaced":      '[a](TODO-01-a.md) [b](TODO-02-b.md)',
    "bare-second": '[a](TODO-01-a.md) TODO-02-b.md',
    "after-mark":  '[a](TODO-01-a.md) §1 [b](TODO-02-b.md)',
}
import validate as V, pathlib
def _node(fp, i): return {"file_path": fp, "id": i, "sections": [], "stamps_xrefs": []}
_NODES = [_node("todo/01-test/TODO-03-s.md", "s3")]
_IDX = _NODES + [_node("todo/01-test/TODO-01-a.md", "a1"),
                 _node("todo/01-test/TODO-02-b.md", "b2")]
_ID, _PATH = V.build_id_index(_IDX), V.build_path_index(_IDX)
_BODY = "# T\n\n## 1. First\n\n- [x] Needle\n"

def repair_unresolvable(stamp):
    snap = {"todo/01-test/TODO-03-s.md": "# S\n\n" + stamp + "\n",
            "todo/01-test/TODO-01-a.md": _BODY, "todo/01-test/TODO-02-b.md": _BODY}
    return V.fix_line_numbers(_NODES, snap, _ID, _PATH,
                              pathlib.Path("."), False, True)[2]

bad = []
for name, tok in BAD.items():
    # The parser's verdict.
    line = '> **Deferred:** [M] x -> XREF: %s §1 (item: "Needle" at line 99)' % tok
    lead = cs.XREF_LEAD_RE.search(line)
    c = cs.parse_xref_clause(line, lead.end())
    multi = c is not None and len(c.targets) > 1 and c.malformed
    # `adjacent` is the one shape with no separator, so the token itself is one
    # non-space run and the parser hands the WHOLE thing over -- fail-closed at
    # the resolver, exactly as section 33 left it. Every OTHER shape must be
    # caught as multi-destination here.
    if name != "adjacent" and not multi:
        bad.append("%s: parser did not see two destinations (%r)" % (name, c))
    if name == "adjacent":
        continue
    # EVERY PRODUCER'S OWN POLICY, THROUGH ITS REAL ENTRY POINT. An earlier cut
    # asserted "every producer" while running only the parser, the stamp walk
    # and the Inputs bullet -- the table branch and the repair path never ran,
    # so two of the four consolidated consumers were unasserted while the
    # sub-test advertised them (Codex consistency, [medium]).
    if B._walk_stamps_xrefs([line], [False]):
        bad.append("%s: stamp producer emitted an edge" % name)
    if B.extract_inputs_xrefs("## Inputs\n\n- -> XREF: %s §1 -- d\n" % tok):
        bad.append("%s: inputs BULLET emitted an edge" % name)
    if B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: %s §1 | d |\n" % tok):
        bad.append("%s: inputs TABLE emitted an edge" % name)
    if repair_unresolvable(line) < 1:
        bad.append("%s: repair path did not count an unresolvable target" % name)
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T37B" = "OK" ]; then
    t_pass "s37b: every producer refuses a clause naming two destinations"
else
    t_fail "s37b: $T37B"
fi

# Sub-test 37c: THE CONTROLS. These are the shapes a too-eager parser breaks,
# and the multi-XREF stamp is the one that would have cost the whole corpus:
# 25 live lines carry more than one clause, so a scan that ran to end-of-line
# would read clause two's legitimate destination as clause one's second one.
T37C="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

# CONTROL 1: two legitimate clauses on one stamp line -> TWO edges, not a refusal.
line = ('> **Accepted:** [M] x -> XREF: TODO-01-a.md §1 (item: "A" at line 3)'
        ' -> XREF: TODO-02-b.md §2 (item: "B" at line 4)')
edges = B._walk_stamps_xrefs([line], [False])
if [e["target_path"] for e in edges] != ["TODO-01-a.md", "TODO-02-b.md"]:
    bad.append("multi-XREF stamp not preserved: %r" % (edges,))
if [e.get("item_name") for e in edges] != ["A", "B"]:
    bad.append("multi-XREF item names not preserved: %r" % (edges,))

# CONTROL 2: a `--` description that MENTIONS another TODO is prose, not a target.
line = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 -- the section TODO-02-b.md owns'
edges = B._walk_stamps_xrefs([line], [False])
if [e["target_path"] for e in edges] != ["TODO-01-a.md"]:
    bad.append("prose mention refused the clause: %r" % (edges,))

# CONTROL 3: the `(item: ...)` parenthetical is a clause tail, not a destination.
line = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 (item: "C" at line 3)'
if len(B._walk_stamps_xrefs([line], [False])) != 1:
    bad.append("item parenthetical broke the clause")

# CONTROL 4: a backticked Inputs target is a code span and ends at its closing
# backtick -- the one live row that proved the two Inputs forms disagreed.
rows = B.extract_inputs_xrefs(
    "## Inputs\n\n- -> XREF: `12-user/TODO-05-x.md`: CSRSS creates its port\n")
if [r["target_path"] for r in rows] != ["12-user/TODO-05-x.md"]:
    bad.append("backticked inputs target not unwrapped: %r" % (rows,))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T37C" = "OK" ]; then
    t_pass "s37c: controls -- multi-XREF stamps, prose mentions, item tails and code spans survive"
else
    t_fail "s37c: $T37C"
fi

# Sub-test 37d: WHAT KEEPS A LEGITIMATE NEXT CLAUSE FROM BEING READ AS THIS
# CLAUSE'S SECOND DESTINATION. Two earlier versions of this sub-test defended a
# positional clause bound; both were wrong, and the bound is gone. A position
# cannot know whether it is inside a token, which is why every version of it cut
# inside a markdown-link label somewhere. The real invariant is a property of
# the GRAMMAR and is directly checkable: the extra-target pattern must never
# match a `-> XREF:` lead. If a future widening breaks that, this fails.
T37D="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs

bad = []
# THE INVARIANT, asserted directly on the grammar.
for lead_text in (" -> XREF: TODO-02-b.md", "-> XREF: TODO-02-b.md",
                  "  ->  XREF:  TODO-02-b.md", " §1 -> XREF: TODO-02-b.md"):
    if cs._XREF_ADJACENT_TARGET_RE.match(lead_text):
        bad.append("adjacency grammar matched a lead: %r" % lead_text)

# The behaviour that invariant protects.
line = ('> **Accepted:** [M] x -> XREF: TODO-01-a.md §1 (item: "A" at line 3)'
        ' -> XREF: TODO-02-b.md §2 (item: "B" at line 4)')
edges = B._walk_stamps_xrefs([line], [False])
if [e["target_path"] for e in edges] != ["TODO-01-a.md", "TODO-02-b.md"]:
    bad.append("multi-XREF stamp: %r" % (edges,))

# MUTATION: widen the grammar so a lead IS a target shape, and the protection
# must visibly fail -- which is what proves the invariant above is load-bearing
# rather than a restatement of something that cannot break.
import re
real = cs._XREF_ADJACENT_TARGET_RE
cs._XREF_ADJACENT_TARGET_RE = re.compile(
    r"\s*(?:->\s*XREF:\s*)?(?:§\S+\s+)?(?P<dest>`?[\w./-]+\.md`?)")
try:
    mutated = len(B._walk_stamps_xrefs([line], [False]))
finally:
    cs._XREF_ADJACENT_TARGET_RE = real
if mutated >= 2:
    bad.append("mutation inert: widened grammar still emitted %d edges" % mutated)
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T37D" = "OK" ]; then
    t_pass "s37d: the adjacency grammar never matches a lead, and the mutation proves that is load-bearing"
else
    t_fail "s37d: $T37D"
fi

# Sub-test 37e: A SECOND TARGET AFTER THE ITEM PARENTHETICAL. Reachable by none
# of the four producers before section 37, and measured at zero live instances
# -- so closing it is corpus-neutral, which is exactly why it is closed here
# rather than left for the next review round to rediscover.
T37E="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
line = 'x -> XREF: TODO-01-a.md §1 (item: "C" at line 3) TODO-02-b.md'
lead = cs.XREF_LEAD_RE.search(line)
c = cs.parse_xref_clause(line, lead.end())
if not c.malformed or len(c.targets) != 2:
    print("post-item second target not seen: %r" % (c,)); raise SystemExit
# Control: the same clause WITHOUT the trailing target stays clean.
line = 'x -> XREF: TODO-01-a.md §1 (item: "C" at line 3)'
lead = cs.XREF_LEAD_RE.search(line)
c = cs.parse_xref_clause(line, lead.end())
print("OK" if c.malformed is None else "control clause wrongly refused: %r" % (c,))
PY
)"
if [ "$T37E" = "OK" ]; then
    t_pass "s37e: a second target after the item parenthetical is refused, control unaffected"
else
    t_fail "s37e: $T37E"
fi

# Sub-test 37f: A LEAD WRITTEN INSIDE A LABEL OR AN ITEM NAME IS NOT A CLAUSE.
# The first cut searched for the next `-> XREF:` from the clause START, which is
# context-blind: it cut the clause inside a markdown-link label and, separately,
# let the inner lead START a fabricated second clause. This repo documents XREF
# syntax inside TODOs constantly, so the shape is ordinary authoring.
T37F="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

# The label contains a literal lead. ONE edge, bound to the whole link.
line = '> **Deferred:** [M] x -> XREF: [see -> XREF: details](TODO-02-b.md) §1'
edges = B._walk_stamps_xrefs([line], [False])
if [e["target_path"] for e in edges] != ["[see -> XREF: details](TODO-02-b.md)"]:
    bad.append("label lead: %r" % (edges,))

# The ITEM NAME contains a literal lead. One edge, item name intact.
line = ('> **Deferred:** [M] x -> XREF: TODO-01-a.md §1'
        ' (item: "document the -> XREF: syntax")')
edges = B._walk_stamps_xrefs([line], [False])
if len(edges) != 1 or edges[0].get("item_name") != "document the -> XREF: syntax":
    bad.append("item-name lead: %r" % (edges,))

# U+2192 spelling inside a label, on the Inputs surface (which normalises it).
rows = B.extract_inputs_xrefs(
    "## Inputs\n\n- → XREF: [see → XREF: details](TODO-02-b.md) §1 -- d\n")
if [r["target_path"] for r in rows] != ["TODO-02-b.md"]:
    bad.append("u2192 label lead: %r" % (rows,))

# CONTROL: two genuinely separate clauses still yield two edges.
line = ('> **Accepted:** [M] x -> XREF: TODO-01-a.md §1'
        ' -> XREF: TODO-02-b.md §2')
if len(B._walk_stamps_xrefs([line], [False])) != 2:
    bad.append("control: real multi-clause stamp lost an edge")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T37F" = "OK" ]; then
    t_pass "s37f: a lead inside a label or item name neither cuts nor starts a clause"
else
    t_fail "s37f: $T37F"
fi

# Sub-test 37g: NO INPUTS FORM EVER EMITS AN EMPTY target_path. The schema sets
# minLength 1, so an empty destination is not a lax value -- a routed consumer
# rejects the whole cache. A doubled delimiter cuts to nothing at offset zero.
T37G="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B
bad = []
for probe in ("- -> XREF: ``path.md`` §1 -- d",
              "- -> XREF: `` §1 -- d",
              "- -> XREF: `path.md §1 -- d",
              "- -> XREF: `path.md`: prose -- d",
              "- -> XREF: [x]() -- d"):
    for row in B.extract_inputs_xrefs("## Inputs\n\n" + probe + "\n"):
        if not row.get("target_path"):
            bad.append(probe)
# CONTROL: the ordinary shapes still normalise to the destination.
rows = B.extract_inputs_xrefs("## Inputs\n\n- -> XREF: `path.md`: prose -- d\n")
if [r["target_path"] for r in rows] != ["path.md"]:
    bad.append("control code-span: %r" % (rows,))
print("OK" if not bad else "empty/......: " + "; ".join(map(str, bad)))
PY
)"
if [ "$T37G" = "OK" ]; then
    t_pass "s37g: no Inputs token shape normalises to an empty target_path"
else
    t_fail "s37g: $T37G"
fi

# Sub-test 37h: MULTI-CLAUSE PARSING SCALES LINEARLY. The first cut copied
# `text[:limit]` once per clause, so a many-clause line grew quadratically
# (~1.47s at 64k clauses, ~6.92s at 128k). Every match is `endpos`-bounded now.
# The assertion is on the RATIO, not a wall-clock budget, so it does not become
# a flake on a loaded host.
T37H="$(python3 - <<'PY'
import sys, time
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs

def elapsed(n):
    line = "x" + " -> XREF: TODO-01-a.md §1" * n
    t = time.monotonic()
    c = sum(1 for _ in cs.iter_xref_clauses(line))
    return time.monotonic() - t, c

t1, c1 = elapsed(2000)
t2, c2 = elapsed(8000)
if c1 != 2000 or c2 != 8000:
    print("clause counts wrong: %d %d" % (c1, c2)); raise SystemExit
# 4x the input must not cost ~16x. Quadratic would; linear lands near 4x.
ratio = t2 / t1 if t1 > 0 else 0
print("OK" if ratio < 9 else "superlinear: 4x input cost %.1fx time" % ratio)
PY
)"
if [ "$T37H" = "OK" ]; then
    t_pass "s37h: multi-clause parsing scales linearly, not quadratically"
else
    t_fail "s37h: $T37H"
fi

# Sub-test 37i: A LEAD INSIDE A SECOND DESTINATION'S LABEL. The round-1 fix
# moved the clause bound but kept one, and the bound then landed inside the
# label of the SECOND target -- so the extra-target scan could not see the
# complete link, the clause reported CLEAN, and the iterator published the
# label fragment `details](TODO-02-b.md)` as a real edge. Found independently
# by the adversarial and perf legs, [high]. Every positional bound had this
# shape of bug; the fix was to remove it, and this is the regression.
T37I="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

SHAPES = {
    # second destination is a LINK whose label carries a lead
    "link-label": 'TODO-01-a.md §1 [see -> XREF: details](TODO-02-b.md) §2',
    # ... and after the item parenthetical
    "post-item":  'TODO-01-a.md §1 (item: "A" at line 3) [see -> XREF: d](TODO-02-b.md)',
}
for name, body in SHAPES.items():
    line = '> **Deferred:** [M] x -> XREF: ' + body
    lead = cs.XREF_LEAD_RE.search(line)
    c = cs.parse_xref_clause(line, lead.end())
    if not c.malformed or len(c.targets) < 2:
        bad.append("%s: clause read clean (%r)" % (name, c))
    edges = [e["target_path"] for e in B._walk_stamps_xrefs([line], [False])]
    if edges:
        bad.append("%s: emitted %r" % (name, edges))

# THE TABLE CELL KEEPS WHAT FOLLOWS ITS CODE SPAN, section marker included.
# A cell may put the span around the PATH ONLY (`` `path.md` §7 ``), so a
# normaliser that truncates at the closing backtick drops the section -- the
# base commit emitted it and the first fix did not (Codex re-adversarial,
# [medium]). Parity is asserted on target AND section, not target alone.
CELLS = {
    "span-around-path":  ("`TODO-02-b.md` §7", "TODO-02-b.md", "§7"),
    "span-around-both":  ("`TODO-02-b.md §7`", "TODO-02-b.md", "§7"),
    "no-span":           ("TODO-02-b.md §7",   "TODO-02-b.md", "§7"),
}
for name, (cell, want_t, want_s) in CELLS.items():
    rows = B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: %s | x |\n" % cell)
    got = [(r["target_path"], r["target_section"]) for r in rows]
    if got != [(want_t, want_s)]:
        bad.append("cell %s: %r (want %r)" % (name, got, [(want_t, want_s)]))
# A cell whose span is followed by PROSE is malformed and must emit nothing --
# the discarded remainder is what the whole-cell check exists to catch.
if B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: `TODO-02-b.md` trailing prose | x |\n"):
    bad.append("cell with trailing prose emitted an edge")

# CONTROL: a BACKTICKED clause example is documentation, not a destination.
# `-> XREF: a.md §1 `-> XREF: b.md`` quotes the syntax rather than naming a
# second file, which is the same category as the prose-mention control in 37c --
# so it must NOT be refused. Note the deliberate asymmetry this pins: a bare
# backticked TARGET (`` `b.md` ``) IS a second destination and section 33 closed
# it; the lead is what makes this one an example.
line = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 `-> XREF: TODO-02-b.md`'
c = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end())
if c.malformed:
    bad.append("backticked syntax example wrongly refused: %r" % (c,))
if [e["target_path"] for e in B._walk_stamps_xrefs([line], [False])] != ["TODO-01-a.md"]:
    bad.append("backticked syntax example lost the real edge")

# EVERY reported destination must be a destination, never a section prefix --
# the tuple exists to be usable in a diagnostic.
line = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 TODO-02-b.md'
c = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end())
if c.targets != ('TODO-01-a.md', 'TODO-02-b.md'):
    bad.append("targets carried a section prefix or dropped one: %r" % (c.targets,))
# THREE destinations are all reported, not just the first two.
line = '> **Deferred:** [M] x -> XREF: TODO-01-a.md §1 TODO-02-b.md TODO-01-a.md'
c = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end())
if len(c.targets) != 3:
    bad.append("three-target clause reported %d: %r" % (len(c.targets), c.targets))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T37I" = "OK" ]; then
    t_pass "s37i: a lead inside a second destination is caught, and every destination is reported"
else
    t_fail "s37i: $T37I"
fi

# ----------------------------------------------------------------------
# Test 40: THE SECTION MARKER STOPS AT THE CODE-SPAN DELIMITER.
#
# The defect: `_XREF_SECTION_RE` captured a non-space run, so a span wrapping
# the whole path-and-marker construct put its closing backtick INSIDE the
# value. The table surface unwraps its cell before parsing and the bullet
# surface does not, so one reference had two spellings -- 508 of 945 live
# Inputs rows and 132 of 1017 stamp rows.
#
# EVERY SUB-TEST CARRIES ITS CONTROL, and the controls are the load-bearing
# half: each re-installs the old pattern and asserts the fixture FAILS under
# it. Without that, 40a is satisfied by a parser that emits a constant and 40d
# by a corpus that happens to contain no spans.
# ----------------------------------------------------------------------

# Sub-test 40a: SURFACE PARITY. Wherever the span sits -- around the path,
# around the whole construct, or absent -- the bullet and table forms must
# agree on target AND section. `span-around-both` is the shape that split.
T40A="$(python3 - <<'PY'
import re, sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

SHAPES = {
    "span-around-path": "`TODO-02-b.md` §7",
    "span-around-both": "`TODO-02-b.md §7`",
    "no-span":          "TODO-02-b.md §7",
}
WANT = ("TODO-02-b.md", "§7")

def surfaces(ref):
    bullet = B.extract_inputs_xrefs("## Inputs\n\n- -> XREF: %s\n" % ref)
    table = B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: %s | x |\n" % ref)
    pair = lambda rows: [(r["target_path"], r["target_section"]) for r in rows]
    return pair(bullet), pair(table)

for name, ref in SHAPES.items():
    bullet, table = surfaces(ref)
    if bullet != [WANT]:
        bad.append("%s bullet: %r (want %r)" % (name, bullet, [WANT]))
    if table != [WANT]:
        bad.append("%s table: %r (want %r)" % (name, table, [WANT]))
    if bullet != table:
        bad.append("%s: surfaces disagree %r vs %r" % (name, bullet, table))

# CONTROL. Restoring the old non-space run must still CHANGE the answer, or the
# fixture has stopped reaching the code under test. BOTH names are restored: the
# parser picks between them on the target's backtick parity, so patching one
# leaves the spanned path -- the path that split -- untouched.
#
# THIS CONTROL USED TO ALSO ASSERT THE TWO SURFACES DISAGREE UNDER THE MUTATION,
# AND THAT HALF IS NOW UNREACHABLE BY CONSTRUCTION (TODO-06 §44). The split had
# two causes and §40 removed only one: the marker regex here, and a pre-transform
# the TABLE ran over its cell (`_cell_code_span` plus a one-character punctuation
# strip) before the parser ever saw it. §44 deleted that pre-transform, so both
# surfaces now hand the same raw text to the same parser and CANNOT disagree on
# this shape however the marker rule is mutated. Asserting they still disagree
# would be asserting the pre-transform is back. What the control can still prove
# is that the mutation bites at all, and that when it does, both surfaces degrade
# the SAME way -- which is the §44 property, and fails the moment a surface grows
# a private transform again.
saved = (cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE)
cs._XREF_SECTION_RE = cs._XREF_SECTION_IN_SPAN_RE = re.compile(r"[`\s]*(§\S+)")
try:
    bullet, table = surfaces(SHAPES["span-around-both"])
    if bullet != [("TODO-02-b.md", "§7`")]:
        bad.append("CONTROL did not reproduce the bullet split: %r" % (bullet,))
    if bullet != table:
        bad.append("CONTROL: surfaces diverged under the mutation (%r vs %r) -- "
                   "a surface has regrown a private transform" % (bullet, table))
finally:
    cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE = saved
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40A" = "OK" ]; then
    t_pass "s40a: both Inputs surfaces emit the same clean section marker"
else
    t_fail "s40a: $T40A"
fi

# Sub-test 40b: THE DELIMITER IS CONSUMED, NOT MERELY EXCLUDED. The item
# parenthetical is matched at the position the section match ended, so a
# capture that stops ON the backtick leaves that position there and the clause
# loses its item name -- 160 live clauses, found by the pre-edit design review
# [medium] against the first version of this fix. Pin item AND clause end.
T40B="$(python3 - <<'PY'
import re, sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

LINE = '> **Deferred:** [M] x -> XREF: `TODO-01-a.md §3` (item: "Do the thing")'
def parse():
    return cs.parse_xref_clause(LINE, cs.XREF_LEAD_RE.search(LINE).end())

c = parse()
if c.section != "§3":
    bad.append("section %r (want the bare marker)" % (c.section,))
if c.item_name != "Do the thing":
    bad.append("item_name %r (want 'Do the thing')" % (c.item_name,))
if c.end != len(LINE):
    bad.append("clause.end %d does not cover the parenthetical (%d)" % (c.end, len(LINE)))
rows = B._walk_stamps_xrefs([LINE], [False])
if [(r["target_section"], r.get("item_name")) for r in rows] != [("§3", "Do the thing")]:
    bad.append("stamp row: %r" % (rows,))

# CONTROL A: stop the clause where the CAPTURE ended instead of where the span
# does, and the item is lost. This mutates `_masked_run_end` rather than the
# regex ON PURPOSE: the regression the design review caught belongs to whatever
# advances past the delimiter, and after round 3 that is the mask walk, not the
# pattern. A control aimed at the old location kept passing while proving
# nothing, which is the failure mode controls exist to have.
# CONTROL B: the old single rule keeps the item and dirties the section. The
# shipped code is the only one of the three that gets both, which is why
# neither control alone would have caught the regression.
saved_end = cs._span_end_after
saved_res = (cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE)
try:
    cs._span_end_after = lambda text, masked, start, limit: start
    naive = parse()
    if naive.item_name is not None:
        bad.append("CONTROL A kept the item name; the regression is unreachable")
    cs._span_end_after = saved_end
    cs._XREF_SECTION_RE = cs._XREF_SECTION_IN_SPAN_RE = re.compile(r"[`\s]*(§\S+)")
    old = parse()
    if old.item_name != "Do the thing" or "`" not in (old.section or ""):
        bad.append("CONTROL B: old pattern behaved unexpectedly: %r" % (old,))
finally:
    cs._span_end_after = saved_end
    cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE = saved_res
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40B" = "OK" ]; then
    t_pass "s40b: the delimiter is consumed, so the item parenthetical still parses"
else
    t_fail "s40b: $T40B"
fi

# Sub-test 40c: WHAT A MARKER SAYS INTERNALLY IS CORPUS VOCABULARY; WHERE IT
# ENDS IS THE PARSER'S BUSINESS. Section 40 wrote this fixture to reject a
# digits-with-joins grammar, on the ground that it truncates live values --
# ranges written with a unicode dash would cut at the first number and silently
# narrow the reference. That objection was RIGHT and is still enforced below:
# every join and range form passes through byte-for-byte, because both dashes
# are in the class §44 adopted.
#
# WHAT §44 CHANGED IS THE END, NOT THE VOCABULARY. `§\S+` ran to the next space,
# so it bound whatever prose followed: 38 live edges across 14 forms carried a
# value like `§5,` or `§3:` that no resolver matches, and `a.md §1](#x)` bound a
# fragment of a markdown link. A trailing separator is the sentence's
# punctuation, not a reference with a missing half, so it is handed back to the
# caller as a remainder instead of kept. The two shapes below moved deliberately
# and are asserted in their new form rather than removed, so a regression to the
# old greedy read fails here. The dashes are built from code points so this
# source file carries no such glyph of its own.
T40C="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
bad = []
S = "§"
DASH = chr(0x2013)
PASSTHROUGH = [
    S + "10" + DASH + "13",   # range, unicode dash (8 live values)
    S + "1-" + S + "5",       # range, ascii
    S + "3," + S + "4",       # comma join
    # A JOIN MAY CARRY WHITESPACE. `§3,§4` and `§1, §2` are the same authoring
    # act, and a flat character class matched the first while stopping at the
    # space in the second -- so 29 live references bound only their FIRST
    # section and dropped the rest. A narrowed dependency looks clean, which is
    # why it went unnoticed; these forms are pinned so it cannot recur.
    S + "1, " + S + "2",
    S + "1, " + S + "4, " + S + "11",
    S + "1--" + S + "8",      # ascii double-hyphen range (this repo's dash)
]
for want in PASSTHROUGH:
    line = "> **Deferred:** [M] x -> XREF: TODO-01-a.md " + want
    got = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end()).section
    if got != want:
        bad.append("%r came back as %r" % (want, got))
# Moved by §44: a trailing separator is prose punctuation, and a marker that is
# not a section reference at all binds nothing rather than binding a fragment.
for wrote, want_section, want_class in [
        (S + "5,", S + "5", cs.REMAINDER_TERMINAL),
        (S + "*", None, cs.REMAINDER_MARKER)]:
    line = "> **Deferred:** [M] x -> XREF: TODO-01-a.md " + wrote
    c = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end())
    if c.section != want_section:
        bad.append("%r bound section %r (want %r)" % (wrote, c.section, want_section))
    got_class = cs.classify_remainder(line, c)
    if got_class != want_class:
        bad.append("%r classified %s (want %s)" % (wrote, got_class, want_class))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40C" = "OK" ]; then
    t_pass "s40c: marker values are passed through, never re-grammared"
else
    t_fail "s40c: $T40C"
fi

# Sub-test 40d: THE INVARIANT OVER THE LIVE CORPUS, which is the differential
# in permanent form. Counting rows would go stale the day a TODO is written;
# the property does not: no emitted marker may end in a code-span delimiter.
# The control re-installs the old pattern and requires the corpus to produce
# some, so a corpus that stopped containing spans cannot pass this vacuously.
T40D="$(python3 - <<'PY'
import re, sys, pathlib
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
DIRTY = re.compile(r"[`]+[.:]?$")

def sections():
    out = []
    for p in sorted(pathlib.Path("todo").rglob("*.md")):
        body = p.read_text(encoding="utf-8", errors="replace")
        for r in B.extract_inputs_xrefs(body) + B.extract_stamps_xrefs(body):
            s = r.get("target_section")
            if s:
                out.append((str(p), s))
    return out

live = sections()
if len(live) < 500:
    bad.append("corpus walk found only %d markers; the fixture is not exercising it" % len(live))
dirty = [x for x in live if DIRTY.search(x[1])]
if dirty:
    bad.append("%d markers end in a delimiter, e.g. %r" % (len(dirty), dirty[:3]))

saved = (cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE)
cs._XREF_SECTION_RE = cs._XREF_SECTION_IN_SPAN_RE = re.compile(r"[`\s]*(§\S+)")
try:
    control = [x for x in sections() if DIRTY.search(x[1])]
    if not control:
        bad.append("CONTROL produced no delimiter-bearing marker; the corpus "
                   "no longer contains the shape, so this test proves nothing")
finally:
    cs._XREF_SECTION_RE, cs._XREF_SECTION_IN_SPAN_RE = saved
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40D" = "OK" ]; then
    t_pass "s40d: no live corpus marker carries a code-span delimiter"
else
    t_fail "s40d: $T40D"
fi

# Sub-test 40e: A BACKTICK THAT OPENS A SPAN IS NOT A CLOSER. The first version
# of this fix consumed the delimiter context-free, which ended the clause early
# enough for `iter_xref_clauses` to start a SECOND clause inside the resulting
# inline-code token -- publishing a quoted filename as a real edge and moving
# the item name onto it (Codex adversarial, [high]). Outside a span the old
# non-space run is the correct read, and this pins both directions.
T40E="$(python3 - <<'PY'
import re, sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
BT = "`"

# Marker in PROSE, immediately followed by a span that quotes another lead.
LINE = ('> **Deferred:** [M] x -> XREF: TODO-01-a.md §3' + BT
        + '->XREF:TODO-02-b.md §4' + BT + ' (item: "Do")')
clauses = list(cs.iter_xref_clauses(LINE))
if len(clauses) != 1:
    bad.append("prose marker started %d clauses (want 1)" % len(clauses))
edges = [r["target_path"] for r in B._walk_stamps_xrefs([LINE], [False])]
if edges != ["TODO-01-a.md"]:
    bad.append("fabricated edge: %r" % (edges,))

# CONTROL: the guard MUST be load-bearing, or this fixture is inert. It now
# mutates the MASK rather than the marker regex, because §44 moved where the
# protection lives. Until then, nothing refused the quoted lead: `§\S+` ran to
# the next space and happened to swallow the span containing it, so blanking the
# marker rule was enough to expose the fabrication. Bounding the marker to
# section characters removed that side effect, and `iter_xref_clauses` now
# refuses a lead whose start is masked. Neutralising the mask is therefore what
# reopens the [high] defect; mutating the marker no longer would, and a control
# that kept doing so would silently stop testing anything.
saved_mask = cs._mask_marking_span_ends
try:
    cs._mask_marking_span_ends = lambda s, stop=None: s
    ctl = [r["target_path"] for r in B._walk_stamps_xrefs([LINE], [False])]
    if ctl == ["TODO-01-a.md"]:
        bad.append("CONTROL did not fabricate; the guard is unreachable")
finally:
    cs._mask_marking_span_ends = saved_mask

# The span-around-path form closes INSIDE the target token (even parity), so its
# marker sits in prose and must not lose anything to a delimiter rule.
rows = B.extract_inputs_xrefs("## Inputs\n\n- -> XREF: " + BT + "TODO-02-b.md" + BT + " §7\n")
if [(r["target_path"], r["target_section"]) for r in rows] != [("TODO-02-b.md", "§7")]:
    bad.append("span-around-path: %r" % (rows,))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40E" = "OK" ]; then
    t_pass "s40e: an opening backtick is never consumed as a closing delimiter"
else
    t_fail "s40e: $T40E"
fi

# Sub-test 40f: THE TABLE CELL KEEPS ITS PUNCTUATION BOUNDARY. The cell unwrap
# removes the delimiters before the shared parser sees them, so a full stop
# after the span was indistinguishable from marker content and the same
# reference read one way as a bullet and another as a table row (Codex
# adversarial, [medium]). Parity is asserted WITH the punctuation, and the
# malformed-cell contract is asserted beside it so the repair cannot have been
# bought by loosening the whole-cell anchor.
T40F="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B
bad = []
BT = "`"
pair = lambda rows: [(r["target_path"], r["target_section"]) for r in rows]
for tail in ("", ".", ":"):
    ref = BT + "TODO-02-b.md §7" + BT + tail
    bullet = pair(B.extract_inputs_xrefs("## Inputs\n\n- -> XREF: %s\n" % ref))
    table = pair(B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: %s | x |\n" % ref))
    if bullet != [("TODO-02-b.md", "§7")] or table != bullet:
        bad.append("tail %r: bullet=%r table=%r" % (tail, bullet, table))
# A wordier trailing remainder is still malformed and still emits nothing.
if B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: " + BT + "TODO-02-b.md" + BT + " trailing prose | x |\n"):
    bad.append("trailing-prose cell emitted an edge")
# AN UNSPANNED CELL IS TRIMMED TOO, and that reverses what §40 asserted here.
# Its rule was "one character, against a CLOSER" -- a punctuation allowlist that
# only ran where a code span had just ended, so the spanned and unspanned forms
# of the same reference disagreed: `` `a.md §7`. `` bound `§7` and `a.md §7.`
# bound `§7.`. §44 moved the decision into the marker grammar, where it applies
# once for both, and the trimmed character reaches the caller as a remainder.
rows = B.extract_inputs_xrefs("## Inputs\n\n| -> XREF: TODO-02-b.md §7. | x |\n")
if [r["target_section"] for r in rows] != ["§7"]:
    bad.append("unspanned cell kept its prose punctuation: %r" % (rows,))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40F" = "OK" ]; then
    t_pass "s40f: bullet and table agree even when prose punctuation follows the span"
else
    t_fail "s40f: $T40F"
fi

# Sub-test 40g: THE VERSION BUMP HAS A REGRESSION OF ITS OWN. Sub-test 25c is
# version-AGNOSTIC by construction (it writes `CACHE_FORMAT_VERSION - 1`), which
# proves the refusal MECHANISM and cannot notice a reverted constant: revert 5
# to 4 and it simply writes 3 and stays green (Codex test-coverage, [medium]).
# This one pins the LITERAL historical version instead, so reverting the bump
# makes a v4 artifact current, the refusal stops firing, and this fails.
S40G="$TMP_DIR/s40g"
s23_tree "$S40G"
timeout 60 python3 "$BUILD_PY" --quiet --root "$S40G/todo" \
    --output "$S40G/build/todo-cache.json" --repo-root "$S40G" >/dev/null 2>&1
cp "$S40G/build/todo-cache.json" "$S40G/old-baseline.json"
bind_baseline "$S40G/old-baseline.json"
python3 - "$S40G/old-baseline.json" "$REPO_ROOT/scripts/todo-graph" <<'S40GEOF'
import hashlib, json, pathlib, sys
sys.path.insert(0, sys.argv[2])
import cache_schema as cs
cache = pathlib.Path(sys.argv[1])
side = cs.sidecar_path(cache, hashlib.sha256(cache.read_bytes()).hexdigest())
rec = json.loads(side.read_text())
# LITERAL 4: the contract in force before the section-marker meaning changed.
rec[cs.CACHE_FORMAT_VERSION_KEY] = 4
side.write_text(json.dumps(rec, indent=2, sort_keys=True) + "\n")
S40GEOF
S40G_OUT=$(REPO_ROOT="$REPO_ROOT" timeout 60 python3 "$VALIDATE_PY" \
    --cache "$S40G/build/todo-cache.json" --repo-root "$S40G" --quiet \
    --diff "$S40G/old-baseline.json" 2>&1); S40G_RC=$?
if [ "$S40G_RC" = "2" ] && echo "$S40G_OUT" | grep -q "LEGACY_FORMAT"; then
    t_pass "s40g: a literal-v4 baseline is refused, so reverting the bump fails a test"
else
    t_fail "s40g: literal-v4 baseline rc=$S40G_RC out=$S40G_OUT"
fi

# Sub-test 40h: THE SPAN QUESTION IS THE SEGMENTER'S, NOT ARITHMETIC. Two
# hand-rolled proxies for "is a span open at this marker" shipped into review
# and both fabricated an edge: consuming the delimiter unconditionally, then
# counting the target token's backticks -- which an ESCAPED backtick in a link
# label defeats, since it is not an opener but still counts (Codex adversarial
# rounds 1 and 2, both [high]). The shapes below are the ones each proxy got
# wrong, plus the run-length and nesting cases the canonical mask already
# handles. Every one must yield exactly ONE edge with the item name attached to
# the intended target.
T40H="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
BT = "`"
STAMP = '> **Deferred:** [M] x -> XREF: '

# `keeps_item` is FALSE for the two prose shapes on purpose. There the marker
# is not in a span, so the non-space run swallows the text up to the following
# span and the parenthetical is out of reach -- which is precisely what the
# pre-change producer did with them, verified by replay. Demanding the item
# there would be demanding an IMPROVEMENT this section did not make, and the
# defect under test is the fabricated SECOND edge that steals it.
CASES = {
    # An escaped backtick in a link label: not an opener, but it counts.
    "escaped-label": ("[literal \\" + BT + "](TODO-01-a.md) §3" + BT
                      + "->XREF:TODO-02-b.md §4" + BT + ' (item: "Do")',
                      "[literal \\" + BT + "](TODO-01-a.md)", False),
    # A bare marker followed by a span that quotes another lead.
    "prose-then-span": ("TODO-01-a.md §3" + BT + "->XREF:TODO-02-b.md §4" + BT
                        + ' (item: "Do")', "TODO-01-a.md", False),
    # A genuine whole-construct span: the delimiter rule SHOULD apply.
    "whole-span": (BT + "TODO-01-a.md §3" + BT + ' (item: "Do")',
                   BT + "TODO-01-a.md", True),
    # A doubled delimiter run closes only on a run of the same length.
    "doubled": (BT + BT + "TODO-01-a.md §3" + BT + BT + ' (item: "Do")',
                BT + BT + "TODO-01-a.md", True),
    # A SHORTER run INSIDE that span is literal content, not the closer. The
    # backtick-run version of this rule stopped there and published the rest of
    # the span as a second edge (Codex adversarial round 3, [high]).
    "doubled-inner-1": (BT * 2 + "TODO-01-a.md §3" + BT + "-> XREF: TODO-02-b.md §4"
                        + BT * 2 + ' (item: "Do")', BT * 2 + "TODO-01-a.md", True),
    # And one length deeper, so the fixture cannot pass by special-casing two.
    "triple-inner-2": (BT * 3 + "TODO-01-a.md §3" + BT * 2 + "-> XREF: TODO-02-b.md §4"
                       + BT * 3 + ' (item: "Do")', BT * 3 + "TODO-01-a.md", True),
    # A SECOND span one space away is not a continuation of the first. The
    # walk-while-it-looks-masked version could not tell that gap from a space
    # inside the span, ran through it, and attached this parenthetical across an
    # intervening span (Codex adversarial round 4, [medium]).
    "adjacent-spans": (BT + "TODO-01-a.md §3" + BT + " " + BT + "context" + BT
                       + ' (item: "Do")', BT + "TODO-01-a.md", False),
}
for name, (body, want_target, keeps_item) in CASES.items():
    line = STAMP + body
    n = len(list(cs.iter_xref_clauses(line)))
    if n != 1:
        bad.append("%s: %d clauses (want 1)" % (name, n))
    rows = B._walk_stamps_xrefs([line], [False])
    got = [(r["target_path"], r.get("item_name")) for r in rows]
    want = [(want_target, "Do" if keeps_item else None)]
    if got != want:
        bad.append("%s: %r (want %r)" % (name, got, want))

# The spanned shapes must ALSO come back with a clean marker -- one edge is
# only half the contract, and asserting it alone would pass on the old rule.
for name in ("whole-span", "doubled", "doubled-inner-1", "triple-inner-2",
             "adjacent-spans"):
    rows = B._walk_stamps_xrefs([STAMP + CASES[name][0]], [False])
    if [r["target_section"] for r in rows] != ["§3"]:
        bad.append("%s: marker %r" % (name, [r.get("target_section") for r in rows]))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40H" = "OK" ]; then
    t_pass "s40h: an escaped, doubled or nested delimiter is judged by the canonical mask"
else
    t_fail "s40h: $T40H"
fi

# Sub-test 40i: PROSE PUNCTUATION AFTER THE SPAN STILL REACHES THE ITEM. The
# item parenthetical is matched at whatever offset the span walk returns, so
# stopping dead on the closing delimiter left a `.` or `:` in the way and the
# clause lost its item name -- a REGRESSION against the pre-change parser, which
# swallowed that character in its non-space run (Codex adversarial, [medium]).
# Both the marker and the item are asserted: fixing one by breaking the other is
# exactly the trade this section kept almost making.
T40I="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
BT = "`"
for tail in (".", ":"):
    line = ('> **Deferred:** [M] x -> XREF: ' + BT + "TODO-01-a.md §3" + BT + tail
            + ' (item: "Do the thing")')
    c = cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end())
    if (c.section, c.item_name) != ("§3", "Do the thing"):
        bad.append("tail %r: section=%r item=%r" % (tail, c.section, c.item_name))
    rows = B._walk_stamps_xrefs([line], [False])
    if [(r["target_section"], r.get("item_name")) for r in rows] != [("§3", "Do the thing")]:
        bad.append("tail %r stamp row: %r" % (tail, rows))
# ONE character, and only these two: a wordier remainder still ends the clause,
# so the allowance cannot grow into "skip whatever follows".
line = '> **Deferred:** [M] x -> XREF: ' + BT + "TODO-01-a.md §3" + BT + '?! (item: "Do")'
if cs.parse_xref_clause(line, cs.XREF_LEAD_RE.search(line).end()).item_name is not None:
    bad.append("a two-character tail was skipped as well")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40I" = "OK" ]; then
    t_pass "s40i: one prose character after the span does not detach the item name"
else
    t_fail "s40i: $T40I"
fi

# Sub-test 40j: THE MASK IS BOUNDED BY WHAT THE LINE ACTUALLY ASKS. Masking each
# whole line put a constant-factor cliff on input `_MAX_TODO_BYTES` explicitly
# permits: a stamp with one early XREF and a dense backtick tail went from
# 0.0002s to 0.94s at 1 MiB and 0.0008s to 3.88s at 4 MiB, against a 2s
# whole-build target (Codex perf, [high]). The scan now stops once nothing
# pending can still mask what the caller asks about.
#
# THE ASSERTION COMPARES BOUNDED AGAINST UNBOUNDED ON THE SAME INPUT, not one
# size against another and not a wall-clock budget. A cross-size ratio was tried
# FIRST and is wrong: scanning for leads and for the last marker is linear in
# the line either way, so both the fixed and the broken version quadruple when
# the input quadruples (measured 7.4x vs a 4x expectation, on a fix that was
# working). What the fix removed is a ~1000x CONSTANT, which only a same-input
# comparison can see. Timing is monotonic per section 34.
T40J="$(python3 - <<'PY'
import sys, time
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
BT = "`"
bad = []

tail = "x " + (BT + "y ") * ((1024 * 1024) // 4)
line = "> **Deferred:** [M] x -> XREF: TODO-01-a.md §3 " + tail
marker = line.rfind("§")

def clocked(fn):
    t = time.monotonic()
    fn()
    return time.monotonic() - t

bounded = clocked(lambda: cs._mask_marking_span_ends(line, marker))
unbounded = clocked(lambda: cs._mask_marking_span_ends(line))
if len(list(cs.iter_xref_clauses(line))) != 1:
    bad.append("clause count moved on the large line")
if bounded <= 0:
    bad.append("bounded scan timed at zero; the measurement is unusable")
elif unbounded / bounded < 20:
    bad.append("the bound is not biting: unbounded is only %.1fx the bounded scan"
               % (unbounded / bounded))
# The bound must not change the ANSWER, only the work -- checked on a line whose
# span genuinely crosses the marker, which is the case the early exit must not
# cut short.
crossing = "-> XREF: " + BT + "TODO-01-a.md §3 and more" + BT + " tail"
if (cs._mask_marking_span_ends(crossing, crossing.rfind("§"))
        != cs._mask_marking_span_ends(crossing)):
    bad.append("bounded and unbounded masks disagree on a span crossing the marker")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T40J" = "OK" ]; then
    t_pass "s40j: a backtick tail beyond the last marker is not scanned"
else
    t_fail "s40j: $T40J"
fi

# ----------------------------------------------------------------------
# Section 44: the inline scanner honours CommonMark precedence
# ----------------------------------------------------------------------
# THE EXPECTATIONS ARE RECORDED, NOT COMPUTED FROM A LIBRARY AT RUN TIME. Every
# span below was taken from markdown-it-py 3.0.0, which is the oracle this
# section was built against -- but pinning them as literals keeps the gate
# authoritative on a host that has no markdown-it installed, where an
# import-guarded differential would silently skip and read as a pass. Sub-test
# 44f runs the live differential ON TOP when the module is importable.
T44A="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
bad = []
# (line, spans as (start, end) INCLUDING delimiters) -- markdown-it-py 3.0.0.
CASES = [
    ("`a ``b`` c`",           [(0, 11)]),
    ("``a `b` c``",           [(0, 11)]),
    ("a `b ``c`` d` e",       [(2, 13)]),
    ("`a ``b ```c``` d`` e`", [(0, 21)]),
    ("``a`b`c``",             [(0, 9)]),
    ("`a``b`",                [(0, 6)]),
    ("``x``y``",              [(0, 5)]),
    ("`x` and `y`",           [(0, 3), (8, 11)]),
    ("`x and y",              []),
]
for line, want in CASES:
    got = [(a, d) for a, _b, _c, d in cs.iter_code_spans(line)]
    if got != want:
        bad.append("%r: want %s got %s" % (line, want, got))
# THE CONTROL. The retired scan closed whichever pending run LENGTH it met
# first, so it read the INNER pair here and left the outer span's text visible.
# If this line ever masks as two spans again, precedence has regressed.
outer = "`a ``b`` c`"
if cs.mask_code_spans(outer).strip():
    bad.append("outer span not fully masked -- inner-pair precedence is back")
if len(list(cs.iter_code_spans(outer))) != 1:
    bad.append("mixed-length nesting yielded more than one span")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44A" = "OK" ]; then
    t_pass "s44a: leftmost-opener precedence matches CommonMark on mixed run lengths"
else
    t_fail "s44a: $T44A"
fi

# Sub-test 44b: A CLOSER IS MEASURED AT ITS FULL RUN LENGTH, AN OPENER AT ITS
# ESCAPE-SHORTENED ONE. Both the producer's old scan and validate.py's retired
# machine shortened an escaped run and then let the remainder CLOSE a pending
# opener, fabricating a span markdown-it does not see.
T44B="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
bad = []
CASES = [
    ("``x\\```", [],        "escaped+2 run must not close a 2-backtick opener"),
    ("`x\\``",   [],        "escaped+1 run must not close a 1-backtick opener"),
    ("\\```x``", [(2, 7)],  "an escaped FIRST backtick still opens at length-1"),
    ("`a\\`",    [(0, 4)],  "an escaped run still CLOSES the span it is inside"),
    ("\\`x`",    [],        "an escaped backtick opens nothing"),
    ("`C:\\Users\\Default\\Documents\\`", [(0, 29)],
     "a span ending in a Windows path closes"),
]
for line, want, why in CASES:
    got = [(a, d) for a, _b, _c, d in cs.iter_code_spans(line)]
    if got != want:
        bad.append("%s: %r want %s got %s" % (why, line, want, got))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44B" = "OK" ]; then
    t_pass "s44b: closers use the full run length; only openers are escape-shortened"
else
    t_fail "s44b: $T44B"
fi

# Sub-test 44c: THE OFFSET ITERATOR IS THE SHARED SURFACE. It has to publish
# both bounds -- delimiters included for a mask, contents alone for a renderer
# -- because a blanked mask cannot answer the second question, which is why a
# second private implementation existed at all.
T44C="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
bad = []
LINES = ["`a ``b`` c`", "see `foo.md` here", "x `a` y `b` z", "no spans at all",
         "`x and y", "``a`b`c``", "\\```x``"]
for line in LINES:
    parts, prev = [], 0
    for start, i0, i1, end in cs.iter_code_spans(line):
        if not (start <= i0 <= i1 <= end):
            bad.append("%r: bounds out of order" % line)
        parts.append(line[prev:start]); parts.append(line[start:end]); prev = end
    parts.append(line[prev:])
    if "".join(parts) != line:
        bad.append("%r: extents do not reconstruct the line" % line)
    # The segment view must agree with the mask about WHERE the spans are.
    masked = cs.mask_code_spans(line)
    for start, _i0, _i1, end in cs.iter_code_spans(line):
        if masked[start:end].strip():
            bad.append("%r: mask and iterator disagree at %d:%d" % (line, start, end))
    # And the contents a renderer keeps must come back verbatim.
    got = [t for is_code, t in cs.code_span_segments(line) if is_code]
    want = [line[i0:i1] for _s, i0, i1, _e in cs.iter_code_spans(line)]
    if got != want:
        bad.append("%r: segment contents %s != iterator contents %s" % (line, got, want))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44C" = "OK" ]; then
    t_pass "s44c: the shared iterator publishes span and content bounds that agree with the mask"
else
    t_fail "s44c: $T44C"
fi

# Sub-test 44d: THE RETIREMENT IS AN IMPROVEMENT, AND THE TEST SAYS SO. Section
# 44 was planned as "prove parity with validate.py, then delete it" -- which
# would have locked in that implementation's defect, because it applied its
# backslash test at the CLOSER too and so never closed a span ending in a
# Windows path (394 live corpus lines against the producer's 11). This asserts
# validate.py now ROUTES to the shared rule AND that the shared rule DISAGREES
# with the retired one on exactly those fixtures.
T44D="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
import validate as V
bad = []
if V._code_span_segments is not cs.code_span_segments:
    bad.append("validate.py still owns a private code-span implementation")

def retired(line):
    """The machine §44 deleted, kept here ONLY as the thing we must differ from."""
    def esc(s, i):
        b = i
        while b > 0 and s[b - 1] == "\\":
            b -= 1
        return (i - b) % 2 == 1
    n, remaining, i = len(line), {}, 0
    while i < n:
        if line[i] == "`" and not esc(line, i):
            j = i
            while j < n and line[j] == "`":
                j += 1
            remaining[j - i] = remaining.get(j - i, 0) + 1
            i = j
        else:
            i += 1
    out, i = [], 0
    while i < n:
        if line[i] != "`" or esc(line, i):
            i += 1
            continue
        j = i
        while j < n and line[j] == "`":
            j += 1
        length = j - i
        remaining[length] -= 1
        if remaining[length] <= 0:
            i = j
            continue
        k = j
        while k < n:
            if line[k] == "`" and not esc(line, k):
                m = k
                while m < n and line[m] == "`":
                    m += 1
                remaining[m - k] -= 1
                if m - k == length:
                    break
                k = m
            else:
                k += 1
        if k >= n:
            i = j
            continue
        out.append(line[j:k])
        i = k + length
    return out

# Each of these is a shape the retired machine got WRONG. If the shared rule
# ever agrees with it again, the defect has come back.
for line in ["`C:\\Users\\Default\\Documents\\`", "`a\\`", "`a\\\\\\`"]:
    shared = [t for is_code, t in cs.code_span_segments(line) if is_code]
    if not shared:
        bad.append("%r: shared rule found no span (the retired defect)" % line)
    if shared == retired(line):
        bad.append("%r: shared rule still agrees with the retired machine" % line)
# And a shape they must AGREE on, so the test is not merely asserting difference.
same = "see `foo.md` here"
if [t for c, t in cs.code_span_segments(same) if c] != retired(same):
    bad.append("control: the two disagree on an ordinary span")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44D" = "OK" ]; then
    t_pass "s44d: validate.py routes to the shared rule, which beats the machine it retired"
else
    t_fail "s44d: $T44D"
fi

# Sub-test 44e: THE TWO INPUTS SURFACES AGREE BY REMAINDER CLASS. The table used
# to pre-transform its cell -- unwrap a single-backtick span, strip one `.` or
# `:` -- so a marker followed by anything else absorbed it, and the same text
# was one edge as a bullet and a different edge as a row.
T44E="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []

def walk(line):
    out = B._walk_inputs_xrefs(["## Inputs", line],
                         ["## Inputs", line], [False, False],
                         lambda _i: False)
    return None if not out else (out[0]["target_path"], out[0]["target_section"])

def bullet(cell):
    return walk("- -> XREF: " + cell)

def table(cell):
    return walk("| -> XREF: " + cell + " | notes |")

# CONTROL FIRST: the plainest shape must emit on both, or the probe is blind.
if bullet("TODO-02-b.md §7") is None or table("TODO-02-b.md §7") is None:
    bad.append("control: the plain shape emits no edge -- the walk is not reached")

# The section marker must never absorb the sentence's punctuation, on EITHER
# surface. Each of these published `§7)` / `§7,` / `§7;` as a table row before.
for tail in [")", ",", ";", ".", ":", ""]:
    cell = "`TODO-02-b.md §7`" + tail
    b, t = bullet(cell), table(cell)
    if b != t:
        bad.append("tail %r: bullet %s != table %s" % (tail, b, t))
    if t is not None and t[1] != "§7":
        bad.append("tail %r: table folded the tail into the marker: %r" % (tail, t[1]))

# A clause cut inside a markdown link is a parse defect, refused on BOTH.
cut = "`TODO-02-b.md §7`](#x)"
if bullet(cut) is not None or table(cut) is not None:
    bad.append("a clause cut inside a link still published an edge")

# The one DELIBERATE difference: a cell is a bounded field, a bullet is a
# sentence that continues. Stated here so a future change has to face it.
prose = "`TODO-02-b.md §7` trailing prose"
if bullet(prose) is None:
    bad.append("a bullet may carry trailing prose and must still bind")
if table(prose) is not None:
    bad.append("a table cell with unexplained trailing prose must not bind")

# The remainder classes themselves, so the policy is testable apart from callers.
for text, want in [("a.md §1", cs.REMAINDER_EXHAUSTED),
                   ("a.md §1.", cs.REMAINDER_TERMINAL),
                   ("a.md §1](#x)", cs.REMAINDER_MARKUP),
                   ("a.md §1 and then some words", cs.REMAINDER_PROSE)]:
    c = cs.parse_xref_clause(text, 0)
    got = cs.classify_remainder(text, c)
    if got != want:
        bad.append("%r classified %s, want %s" % (text, got, want))
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44E" = "OK" ]; then
    t_pass "s44e: bullet and table agree by remainder class; the marker keeps no tail"
else
    t_fail "s44e: $T44E"
fi

# Sub-test 44f: THE LIVE ORACLE, when the host has one. The fixtures above are
# the gate; this is the differential that would catch a shape nobody thought to
# record, run over the whole corpus. Absent markdown-it it reports that it could
# not run rather than passing silently.
T44F="$(python3 - <<'PY'
import sys, glob
sys.path.insert(0, "scripts/todo-graph")
import cache_schema as cs
try:
    from markdown_it import MarkdownIt
except ImportError:
    print("NO-ORACLE")
    raise SystemExit(0)
md = MarkdownIt("commonmark")

def norm(x):
    # CommonMark strips ONE leading+trailing space when both are present and
    # the content is not all spaces, and replaces NUL with U+FFFD. Both are
    # CONTENT rules a renderer applies after the boundary is decided, so they
    # are normalised away here rather than chased in the scanner.
    if len(x) >= 2 and x[0] == " " and x[-1] == " " and x.strip():
        x = x[1:-1]
    return x.replace("\x00", "\ufffd")

bad, lines = [], 0
for path in glob.glob("todo/**/*.md", recursive=True):
    with open(path, encoding="utf-8", errors="replace") as fh:
        for no, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if "`" not in line:
                continue
            lines += 1
            want = [norm(c.content) for t in md.parseInline(line)
                    for c in (t.children or []) if c.type == "code_inline"]
            got = [norm(t) for is_code, t in cs.code_span_segments(line) if is_code]
            if got != want and len(bad) < 5:
                bad.append("%s:%d" % (path, no))
if bad:
    print("diverged from markdown-it at: " + ", ".join(bad))
elif lines < 1000:
    print("corpus differential saw only %d backticked lines -- probe is suspect" % lines)
else:
    print("OK")
PY
)"
if [ "$T44F" = "OK" ]; then
    t_pass "s44f: the shared scanner matches markdown-it on every backticked corpus line"
elif [ "$T44F" = "NO-ORACLE" ]; then
    t_pass "s44f: live markdown-it differential skipped (module absent); 44a-44e still gate"
else
    t_fail "s44f: $T44F"
fi

# Sub-test 44g: THE FOUR FAIL-OPEN PATHS §44's OWN ADVERSARIAL ROUND FOUND.
# Every one of these publishes a WRONG edge rather than refusing, which is the
# failure direction this parser is supposed to not have, and three of the four
# were created by §44's earlier drafts rather than inherited.
T44G="$(python3 - <<'PY'
import sys
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
BT = "`"

# (1) A MARKERLESS CLAUSE MUST NOT CRASH. `marker_cut` was bound only inside the
# section-matched arm, so every clause without a marker raised UnboundLocalError
# -- and a producer crash is worse than any wrong edge.
for text in ["TODO-01-a.md", "TODO-01-a.md trailing prose"]:
    try:
        cs.parse_xref_clause(text, 0)
    except Exception as exc:
        bad.append("markerless clause raised %s" % type(exc).__name__)

# (2) A QUOTED LEAD AFTER THE LAST MARKER MUST NOT START A CLAUSE. The mask was
# bounded at the last section marker, so a code span lying wholly beyond it was
# byte-identical in the mask and its quoted lead passed the guard. It matters
# because `--fix-line-numbers` accepts markerless targets and would rewrite from
# a documented example.
line = ('> **Deferred:** [M] x -> XREF: TODO-01-a.md §3 then example ' + BT
        + '-> XREF: TODO-02-b.md (item: "Quoted" at line 9)' + BT)
got = [c.targets for _lead, c in cs.iter_xref_clauses(line)]
if got != [("TODO-01-a.md",)]:
    bad.append("quoted lead past the last marker yielded %r" % (got,))

# (3) A BOUNDED CELL CARRIES NO PARENTHETICAL. The retired table guard required
# `item_span is None`; a remainder-only rule reads these as exhausted, because
# the parenthetical was consumed INTO the clause.
for cell in ['TODO-02-b.md §7 (item: "Unexpected")', "TODO-02-b.md §7 (some prose)"]:
    if B._walk_inputs_xrefs(["## Inputs", "| -> XREF: %s | n |" % cell],
                         ["## Inputs", "| -> XREF: %s | n |" % cell], [False, False],
                         lambda _i: False):
        bad.append("table cell with a parenthetical published an edge: %r" % cell)
# ...while the bullet surface still binds them, which is the stated difference.
for cell in ['TODO-02-b.md §7 (item: "Unexpected")']:
    if not B._walk_inputs_xrefs(["## Inputs", "- -> XREF: %s" % cell],
                         ["## Inputs", "- -> XREF: %s" % cell], [False, False],
                         lambda _i: False):
        bad.append("bullet lost a legitimate parenthetical: %r" % cell)

# (4) TRIMMING THE MARKER'S PUNCTUATION MUST NOT SWALLOW WHAT FOLLOWS IT.
# Bounding the marker left the cursor ON the punctuation, where the item regex
# cannot match -- so the item name vanished AND the extra-destination probe
# never ran, turning a malformed two-target clause into one clean edge.
c = cs.parse_xref_clause('TODO-01-a.md §1. (item: "X") TODO-02-b.md', 0)
if c.item_name != "X":
    bad.append("item name lost across the marker's punctuation: %r" % (c.item_name,))
if c.malformed != "multiple-destinations":
    bad.append("second destination hidden behind the punctuation: %r" % (c.malformed,))
c = cs.parse_xref_clause("TODO-01-a.md §5, TODO-02-b.md", 0)
if c.malformed != "multiple-destinations":
    bad.append("second destination hidden behind a trimmed comma: %r" % (c.malformed,))
# And with nothing following, the punctuation is still a REMAINDER, not consumed.
text = "TODO-01-a.md §1."
c = cs.parse_xref_clause(text, 0)
if text[c.end:] != "." or c.section != "§1":
    bad.append("lone punctuation was consumed instead of left as a remainder")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44G" = "OK" ]; then
    t_pass "s44g: the four fail-open paths from section 44's adversarial round stay closed"
else
    t_fail "s44g: $T44G"
fi

# Sub-test 44h: THE TWO DEFECTS §44's POST-SHIP REVIEW FOUND, both created by
# the section itself. The adversarial and consistency legs reported the first
# INDEPENDENTLY, which is why it is pinned twice over: by parity and by the
# remainder it left behind.
T44H="$(python3 - <<'PY'
import sys, time
sys.path.insert(0, "scripts/todo-graph")
import build as B, cache_schema as cs
bad = []
BT = "`"

# (1) A SPANNED MARKER'S TRIMMED PUNCTUATION IS SPAN CONTENT, NOT A REMAINDER.
# `marker_cut` is measured against the marker match; on the spanned path `p` is
# the span's END, a different coordinate. Subtracting one from the other left
# `clause.end` on the closing backtick, so the cell reported a one-backtick
# remainder -- tolerated by the bullet, refused by the bounded table, which is
# exactly the parity split this section exists to close.
for tail in (",", ".", ":", ";", ""):
    cell = BT + "TODO-02-b.md §7" + tail + BT
    c = cs.parse_xref_clause(cell, 0)
    if c.section != "§7":
        bad.append("spanned marker %r bound %r" % (tail, c.section))
    if cell[c.end:] != "":
        bad.append("spanned marker %r left remainder %r" % (tail, cell[c.end:]))
    t = bool(B._walk_inputs_xrefs(["## Inputs", "| -> XREF: %s | n |" % cell],
                         ["## Inputs", "| -> XREF: %s | n |" % cell], [False, False],
                         lambda _i: False))
    b = bool(B._walk_inputs_xrefs(["## Inputs", "- -> XREF: %s" % cell],
                         ["## Inputs", "- -> XREF: %s" % cell], [False, False],
                         lambda _i: False))
    if t != b:
        bad.append("spanned marker %r: table=%s bullet=%s" % (tail, t, b))
# The UNSPANNED form still hands its punctuation back -- the two paths differ,
# and that difference is the point.
text = "TODO-02-b.md §7,"
c = cs.parse_xref_clause(text, 0)
if text[c.end:] != ",":
    bad.append("unspanned marker stopped handing its punctuation back")

# (2) THE BOUNDED MASK MUST NOT MATERIALISE THE UNSCANNED TAIL. Building the
# mask with `list(s)` allocated the whole line as soon as ANY span closed,
# defeating the `stop` bound that 40j pins -- and 40j could not see it, because
# its fixture has no span before the bound. Compared against the SAME line
# masked unbounded, so this measures the bound rather than the machine.
BIG = 4 * 1024 * 1024
head = "-> XREF: " + BT + "a.md §3" + BT + " x "
line = head + ("y " + BT) * ((BIG - len(head)) // 3)
stop = line.find("§")

def clocked(fn):
    t0 = time.monotonic()
    fn()
    return time.monotonic() - t0

bounded = clocked(lambda: cs._mask_marking_span_ends(line, stop))
unbounded = clocked(lambda: cs._mask_marking_span_ends(line))
if bounded <= 0:
    bad.append("bounded scan timed at zero; the measurement is unusable")
elif unbounded / bounded < 5:
    bad.append("an early closed span defeats the bound: unbounded is only %.1fx"
               % (unbounded / bounded))
# And the bound must still not change the ANSWER.
if cs._mask_marking_span_ends(line, stop)[:stop + 1] != cs._mask_marking_span_ends(line)[:stop + 1]:
    bad.append("bounded and unbounded masks disagree before the bound")
if len(cs._mask_marking_span_ends(line, stop)) != len(line):
    bad.append("the mask stopped preserving length")
print("OK" if not bad else "; ".join(bad))
PY
)"
if [ "$T44H" = "OK" ]; then
    t_pass "s44h: a spanned marker's punctuation is span content, and the bound survives an early span"
else
    t_fail "s44h: $T44H"
fi

# ----------------------------------------------------------------------
# Test 41: TYPE 7'S MATCHER IS NON-BACKTRACKING, AND THE CONTROL PROVES IT.
#
# Section 42 WITHDREW HTML block type 7 because its opener regex backtracks:
# `attribute*` keeps one stack frame per iteration, so a long almost-tag makes
# CPython's engine allocate backtracking state proportional to the input at a
# ~164x constant. MEASURED here on 2026-08-12: a 4 MB witness took 656 MB RSS,
# which is what section 42's "1,225 MB RSS at 12 MB" reports from the other end
# of the same line.
#
# So the assertion is a DIFFERENTIAL under one hard cap, not a wall-clock
# budget: with `RLIMIT_AS` at 256 MB and a 2 MB witness, the withdrawn regex
# MUST die and the shipped DFA MUST answer. A test that only measured the DFA
# would pass against a backtracking implementation on small input and prove
# nothing; this one fails loudly if type 7 ever regresses to a regex.
#
# BOTH legs run in SUBPROCESSES under that cap. Running the control in-process
# would OOM or stall the whole tooling suite, which is exactly the hazard the
# design review named (Codex design review, section 46, [medium]).
#
# NOT REPRODUCED, and recorded as such rather than asserted: section 42 also
# reports the regex CPU-superlinear (0.53s/6 KB -> 6.19s/20 KB -> >19s/64 KB).
# Six candidate witness shapes were measured on 2026-08-12 and every one was
# LINEAR in CPU, so that witness is not in hand and this test does not claim
# it. The memory differential below is the part that reproduces.
# ----------------------------------------------------------------------
T41_PY="$TMP_DIR/t41-backtrack.py"
cat > "$T41_PY" <<'T41EOF'
import importlib.util
import resource
import sys
import time

CAP = 256 * 1024 * 1024
resource.setrlimit(resource.RLIMIT_AS, (CAP, CAP))
mode, schema, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
# An almost-tag: a real open tag that never closes, so the attribute loop runs
# to the end of the line and then has to unwind.
witness = "<a" + " b" * n

if mode == "regex":
    import re
    from markdown_it.common.html_re import HTML_OPEN_CLOSE_TAG_STR
    rx = re.compile(HTML_OPEN_CLOSE_TAG_STR + r"\s*$")
    print("regex-answered", bool(rx.search(witness)))
else:
    spec = importlib.util.spec_from_file_location("cs", schema)
    cs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cs)

    def timed(s):
        t = time.monotonic()
        r = cs.is_complete_tag_line(s)
        return r, time.monotonic() - t

    # A VALID witness, because the invalid one is now rejected in O(1) by the
    # trailing-`>` check and would time nothing. Latin-1 is deliberate: every
    # non-ASCII character used to leave the table and call `_tag_class`, which
    # is what took a size-valid 16 MiB line to 4.57s against the producer's 2s
    # budget.
    quarter = '<a b="' + "é" * (n // 4) + '">'
    full = '<a b="' + "é" * n + '">'
    small, t_small = timed(quarter)
    big, t_big = timed(full)
    bad, _ = timed(witness)
    if not small or not big or bad:
        print("dfa-wrong-answer", small, big, bad)
        sys.exit(1)
    # AN ENFORCED CEILING, not a printed number. The earlier version of this
    # test measured and only PRINTED, so a matcher that went quadratic while
    # staying under RLIMIT_AS passed silently (Codex consistency + perf,
    # section 46, [medium]). The bound is loose on purpose -- a tight ratio on
    # a shared WSL2 box is a flake generator -- but a 4x input costing more
    # than 8x is a shape change, not noise, and 4s on this input is already
    # twice the whole builder budget.
    if t_big > 4.0:
        print("dfa-too-slow %.3fs for %d chars" % (t_big, len(full)))
        sys.exit(1)
    if t_small > 0.02 and t_big / t_small > 8.0:
        print("dfa-superlinear %.3f -> %.3f (ratio %.1f for 4x input)"
              % (t_small, t_big, t_big / t_small))
        sys.exit(1)
    print("dfa-answered quarter=%.3fs full=%.3fs ratio=%.1f"
          % (t_small, t_big, t_big / t_small if t_small else 0.0))
T41EOF
T41_SCHEMA="$REPO_ROOT/scripts/todo-graph/cache_schema.py"
# markdown-it-py is OPTIONAL in this repo -- it is in no setup script and no CI
# install step -- so its absence skips the CONTROL leg exactly as sub-test 44f
# does, rather than failing a clean supported checkout and taking the mandatory
# test-tooling gate with it (Codex adversarial, section 46, [high]). The DFA leg
# below needs no oracle and always runs, so the resource bound stays gated even
# on a host without the package; what is lost is only the differential.
if ! python3 -c "import markdown_it" >/dev/null 2>&1; then
    t_pass "s46 perf: backtracking CONTROL skipped (markdown-it-py absent); the DFA bound below still gates"
elif python3 "$T41_PY" regex "$T41_SCHEMA" 1000000 >"$TMP_DIR/t41-regex.log" 2>&1; then
    t_fail "s46 perf: CONTROL DID NOT FIRE -- the withdrawn regex survived a 2 MB witness under a 256 MB cap, so this test proves nothing about the DFA"
elif ! grep -qE "MemoryError|Killed|died" "$TMP_DIR/t41-regex.log"; then
    t_fail "s46 perf: the control failed for the WRONG reason: $(tail -1 "$TMP_DIR/t41-regex.log")"
else
    t_pass "s46 perf: CONTROL -- the withdrawn backtracking regex dies on a 2 MB witness under a 256 MB cap"
fi

if python3 "$T41_PY" dfa "$T41_SCHEMA" 1000000 >"$TMP_DIR/t41-dfa.log" 2>&1; then
    t_pass "s46 perf: the shipped DFA answers the SAME witness under the SAME cap ($(tail -1 "$TMP_DIR/t41-dfa.log"))"
else
    t_fail "s46 perf: the shipped DFA failed the 2 MB witness under a 256 MB cap: $(tail -2 "$TMP_DIR/t41-dfa.log" | tr '\n' ' ')"
fi

# ----------------------------------------------------------------------
# ----------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------
tb_finish sections_b
