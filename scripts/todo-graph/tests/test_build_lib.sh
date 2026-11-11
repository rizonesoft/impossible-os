# ============================================================================
# test_build_lib.sh -- shared preamble of the todo-graph build suite.
#
# SOURCED, never run. The suite is split into four shards that
# scripts/test-tooling.sh starts together:
#
#   test_build.sh            Tests 1-15 plus every block that writes or reads
#                            the REAL build/todo-cache.json (9aa5c3 and 11c/11d
#                            rebuild and delete it; 14, 23r-w, 25, 28 and 45
#                            read it), kept in one file and in their original
#                            order so no reader can race a writer.
#   test_build_gate.sh       Test 22, the identity gate (the longest shard).
#   test_build_sections_a.sh Tests 7, 23, 24, 27, 29 and the producer window.
#   test_build_sections_b.sh Sections 30-45 that need no real cache, and T41.
#
# Each shard sources this file, runs its blocks and ends with tb_finish. A
# name one shard defines and another uses belongs HERE: under `set -u` an
# unset name aborts the shard part-way, which is how the design review found
# SNAP (defined in Test 14, used by Test 22).
#
# Original suite header, still accurate for the coverage it lists:
#
#
# Wired into scripts/test-tooling.sh via the [todo-graph] block. Runs
# under `make test-tooling` and on every CI push.
#
# Spec: the Unit Tests block of the TODO-metadata-layer plan in
# todo/00-infrastructure/.
#
# Coverage (mirrors the spec's Unit Tests checklist):
#   1. Generator exits 0 on a repo with valid frontmatter (uses the live
#      todo/ tree which today has zero frontmatter; "no-frontmatter" mode
#      is the back-compat path validated here).
#   2. Generator exits 1 with a clear file:line message on a single
#      deliberately-malformed frontmatter (synthesizes a fixture under
#      $tmp; deletes after).
#   3. Running twice produces byte-identical cache (determinism).
#   4. Cache node count matches `find todo -name 'TODO-*.md' -not -name
#      'TODO-00-INDEX.md' | wc -l`.
#
# Output style: t_pass / t_fail (matches scripts/test-tooling.sh).
# ============================================================================


# INHERITED GIT ENVIRONMENT IS A REPO-CORRUPTING HAZARD FOR THIS SUITE. Dozens
# of fixtures below run `git init` / `git add -A` / `git commit` inside a temp
# tree, and every one of them uses the ambient environment. With `GIT_DIR` set
# -- which is exactly what git exports to its own hooks -- those commits go to
# the repository GIT_DIR names while the temp tree acts as the work tree, so a
# fixture silently rewrites the REAL repo's HEAD, index and reflog. Observed
# 2026-08-08: running this suite with GIT_DIR exported left the checkout on a
# fixture commit ("both dirs in one commit"), every tracked file reading as
# untracked, and 124 fixtures failing against the wrong repository. The suite
# never needs an inherited git context -- every fixture names its own path or
# uses `-C` -- so clear them once here rather than trusting each caller.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE GIT_OBJECT_DIRECTORY \
      GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_COMMON_DIR GIT_PREFIX \
      GIT_CEILING_DIRECTORIES GIT_NAMESPACE 2>/dev/null || true

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# CLOSE INHERITED DESCRIPTORS 3-8 (2026-09-28). The identity gate opens fd 3 as
# the probe's record file, and the 22ed/22ec fixtures detect "I am the probe"
# by fd 3 being open. A caller that already holds fd 3 (git's transport hands
# the pre-push hook extra pipes) makes EVERY phase look like the probe, so the
# 22ed mutation "did not leak" on every pre-push run and passed standalone --
# four refused pushes on 2026-09-27 alone. With fd 3 held open, this suite also
# stopped silently part-way through. It must not depend on what it inherits.
# (fd 9 is the tooling pack's flock; closing the child's copy leaves the lock
# held by the parent, and 9 is not touched here anyway.)
for _tb_fd in 3 4 5 6 7 8; do eval "exec ${_tb_fd}<&-" 2>/dev/null; done
unset _tb_fd

REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
BUILD_PY="$REPO_ROOT/scripts/todo-graph/build.py"
# Tools more than one shard calls.
VALIDATE_PY="$REPO_ROOT/scripts/todo-graph/validate.py"
QUERY_PY="$REPO_ROOT/scripts/todo-graph/query.py"
SNAP="$REPO_ROOT/scripts/todo-graph/corpus_resolution_snapshot.py"

PASS=0
FAIL=0

t_pass() {
    PASS=$((PASS + 1))
    printf '  [PASS] %s\n' "$1"
}

t_fail() {
    FAIL=$((FAIL + 1))
    printf '  [FAIL] %s\n' "$1" >&2
}

# mono_ns -- nanoseconds from a MONOTONIC source, or empty output on failure.
#
# NEVER time a duration with `date +%s` / `+%s%N` / `+%s.%N`. Those read
# CLOCK_REALTIME, which WSL2 resyncs after a suspend or under load, and a
# BACKWARD step makes an elapsed reading NEGATIVE -- which silently SATISFIES
# every upper-bound assertion in this suite and turns a budget into a
# guaranteed green. Observed on 2026-08-06 and twice on 2026-08-09 in Test 7
# below. `time.monotonic_ns()` cannot go backwards by definition, so the
# refusals guarding those assertions stop firing spuriously while still
# catching a genuinely over-budget run. Same source, and the same reasoning, as
# scripts/todo-graph/identity-gate.sh:204.
#
# bash has no builtin monotonic clock, so a python3 call is the cheapest
# correct source (python3 is already a hard dependency of every test here). It
# costs ~25ms of interpreter startup per reading, and only the CLOSING reading
# of a pair lands inside the measured interval: ~1.25% of Test 7's 2s budget,
# well inside the margin every ceiling in this suite carries.
#
# It never returns non-zero: a caller under `set -e` must reach its valid_ns
# check and report an unreadable clock as a test failure, not die before the
# guard runs.
mono_ns() {
    python3 -c 'import time; print(time.monotonic_ns())' 2>/dev/null || true
}

# valid_ns <reading> -- 0 when <reading> is safe to feed to bash arithmetic.
#
# Bash arithmetic re-evaluates a variable's CONTENTS as an expression, so
# unchecked helper output is not merely wrong, it is executable -- and three
# separate shapes have to be refused, not one:
#
#   * an EXPRESSION (`2000+1`) evaluates to a plausible positive number and
#     sails straight through a non-positive refusal into a green budget;
#   * a bare IDENTIFIER (`foo`) aborts the whole suite under `set -u` instead
#     of failing one test;
#   * a DIGITS-ONLY string is still not safe. `08` is invalid octal to bash and
#     kills the caller mid-function -- in test_bridge.sh under `set -e` that is
#     before its cleanup runs, leaking driver children -- and a value past
#     INT64 wraps silently, which can turn a corrupt huge interval back into a
#     small non-negative one that PASSES an upper bound.
#
# So: non-empty, all digits, no non-canonical leading zero, bounded to 18
# digits (~31 years of uptime in ns, comfortably inside INT64, so no difference
# of two ACCEPTED readings can overflow either). The empty case also covers the
# ordinary failure mode of a missing or broken python3.
valid_ns() {
    case "${1-}" in
        '' | *[!0-9]*) return 1 ;;
        0) return 0 ;;
        0*) return 1 ;;
    esac
    [ "${#1}" -le 18 ] || return 1
    return 0
}

# elapsed_ms <start_ns> <end_ns> -- milliseconds between two mono_ns readings.
#
# Prints the delta and returns 0; prints NOTHING and returns 1 when either
# reading is unusable. This exists so validate-then-subtract is ONE
# behaviourally testable thing rather than a pattern re-typed at six call
# sites: 7b/7c mutation-check it, and 7d then checks that no harness subtracts
# a mono_ns reading behind its back. Every timing site in this suite, in
# scripts/test-tooling.sh, and in scripts/lsp-mcp/tests/test_bridge.sh goes
# through it.
#
# Callers use `if ! MS=$(elapsed_ms "$a" "$b"); then <report unreadable>`, which
# keeps the refusal inside an `if` condition -- so `set -e` cannot kill the
# harness before it reports, and a bad reading can never reach a comparison.
elapsed_ms() {
    valid_ns "${1-}" || return 1
    valid_ns "${2-}" || return 1
    printf '%s\n' "$(( (${2} - ${1}) / 1000000 ))"
}

# bind_cache <cache-path> <todo-root> -- write the corpus binding a hand-authored
# fixture cache needs in order to be READ (section 21).
#
# Freshness stopped asking "is the cache newer than the corpus?" and started
# asking "was this cache built from the corpus now on disk?", answered from a
# digest-named sidecar the producer publishes. `touch`ing a fixture cache
# therefore no longer makes it readable, and an unbound one is CORRECTLY refused
# as STALE by every reader.
#
# It calls the PRODUCTION machinery rather than restating the rule, so a fixture
# can never pass against a binding shape the readers would not accept -- the
# same reason build.py records `cache_schema._scan_corpus` output instead of its
# own corpus walk.
bind_cache() {
    BIND_TG="$REPO_ROOT/scripts/todo-graph" python3 - "$1" "$2" <<'PY'
import hashlib, json, os, pathlib, sys
sys.path.insert(0, os.environ["BIND_TG"])
import cache_schema as cs
cache, root = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
def boom(exc):
    raise exc
blob = cache.read_bytes()
sha = hashlib.sha256(blob).hexdigest()
cs.sidecar_path(cache, sha).write_text(json.dumps({
    "schema": cs.SIDECAR_SCHEMA,
    # Section 25: the producer-contract identity rides in the binding, so a
    # fixture binding that omitted it would be refused as LEGACY_FORMAT. Taken
    # from the PRODUCTION constants for the same reason the corpus scan is --
    # a fixture must never be able to pass against an identity the readers
    # would not accept.
    cs.CACHE_FORMAT_VERSION_KEY: cs.CACHE_FORMAT_VERSION,
    cs.PRODUCER_CONTRACT_DIGEST_KEY: cs.PRODUCER_CONTRACT_DIGEST,
    "cache_sha256": sha,
    "history_id": cs.corpus_history_id(root),
    "corpus": cs._scan_corpus(root, boom),
}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

# Bind a HAND-CRAFTED --diff baseline (section 25).
#
# A baseline is validated with freshness OFF, so its corpus and history are
# deliberately not compared -- but `validate.py --diff` now requires the
# artifact to declare which producer contract wrote it, which is the whole
# point of section 25. `bind_cache` above cannot serve here: it scans a real
# corpus and walks git for a history id, and a baseline in these fixtures is a
# JSON blob with no tree behind it. So this writes the identity ONLY, from the
# production constants, exactly as `bind_cache` takes its corpus rule from the
# production scanner -- a fixture must never be able to pass against an
# identity the readers would refuse.
bind_baseline() {
    BIND_TG="$REPO_ROOT/scripts/todo-graph" python3 - "$1" <<'PY'
import hashlib, json, os, pathlib, sys
sys.path.insert(0, os.environ["BIND_TG"])
import cache_schema as cs
cache = pathlib.Path(sys.argv[1])
blob = cache.read_bytes()
sha = hashlib.sha256(blob).hexdigest()
cs.sidecar_path(cache, sha).write_text(json.dumps({
    "schema": cs.SIDECAR_SCHEMA,
    cs.CACHE_FORMAT_VERSION_KEY: cs.CACHE_FORMAT_VERSION,
    cs.PRODUCER_CONTRACT_DIGEST_KEY: cs.PRODUCER_CONTRACT_DIGEST,
    "cache_sha256": sha,
}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

cleanup() {
    # EVERY FAILURE MESSAGE IN THIS SUITE POINTS AT A LOG UNDER $TMP_DIR, and
    # the trap deleted it before anyone could read it -- so diagnosing an
    # identity-gate fixture meant re-running the whole suite and racing the
    # trap. Opt-in retention, off by default so ordinary runs leave no litter.
    if [ "${TODOGRAPH_KEEP_TMP:-}" = "1" ]; then
        printf '[test_build] kept %s (TODOGRAPH_KEEP_TMP=1)\n' "${TMP_DIR:-}" >&2
        return
    fi
    rm -rf "${TMP_DIR:-}" 2>/dev/null || true
}
trap cleanup EXIT

TMP_DIR="$(mktemp -d -t todograph-build-test.XXXXXX)" \
    || { echo "[test_build] FAIL: mktemp failed"; exit 2; }

# s23_tree <dir> -- a minimal, valid one-file corpus. Several fixtures below
# need "a tree build.py can actually build", and inlining it five times is
# how the copies drift apart.
s23_tree() {
    local root="$1"
    rm -rf "$root"
    mkdir -p "$root/todo/01-test" "$root/build"
    cat > "$root/todo/01-test/TODO-01-s23.md" <<'S23EOF'
---
schema_version: 1
id: fm-s23
domain: 01-test
status: active
title: "s23 fixture"
---
# TODO-01 -- s23 fixture
## Inputs
| Path | Purpose |
| ---- | ------- |
| `src/foo` | example |
## Outcome
Fixture for the validator's rebuild-recovery and baseline-cache semantics.
## Implementation Order
| ⭐ | Order | Section | Deliverable | Depends On | Status |
| --- | --- | --- | --- | --- | --- |
| 💎 | 1 | 1 | First | -- | [x] |
## 1. First Section
Body for the first section.
- [x] Item one
- [x] Commit
S23EOF
}

# s23_tooldir <dir> -- a COPY of the todo-graph tool set, so a fixture can
# replace one of its pieces (a stub producer, a reverted guard) without
# touching the real tree. validate.py locates build.py next to ITSELF, which
# is exactly what makes this substitution possible.
s23_tooldir() {
    local tool="$1"
    rm -rf "$tool"
    mkdir -p "$tool/schema"
    cp "$REPO_ROOT/scripts/todo-graph/validate.py" \
       "$REPO_ROOT/scripts/todo-graph/cache_schema.py" \
       "$REPO_ROOT/scripts/todo-graph/build.py" "$tool/"
    cp "$REPO_ROOT/scripts/todo-graph/schema/cache.schema.json" "$tool/schema/"
}

# tb_finish <shard> -- the shard's summary line and exit status. The pack
# (scripts/test-tooling.sh) judges each shard by its exit status; the line is
# what it prints beside the verdict.
tb_finish() {
    local total=$((PASS + FAIL))
    echo
    echo "[test_build:$1] ${PASS}/${total} passed, ${FAIL} failed"
    [ "$FAIL" -gt 0 ] && exit 1
    exit 0
}
