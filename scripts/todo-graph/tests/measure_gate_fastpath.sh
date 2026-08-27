#!/usr/bin/env bash
# Measure what the identity gate SPENDS on the byte-identical fast path.
#
# Section 55 exists because that path is the COMMON one -- most pushes do not
# touch the resolver closure -- and it was paying for two full worktree
# checkouts and a per-closure-member git probe fan-out before deciding it had
# nothing to differentiate. An assertion that the cost fell is not evidence;
# this script is what produces the number, in a throwaway clone so the same
# number can be reproduced on any host and by anyone reading the record.
#
# WHAT IT COUNTS. `git` invocations are counted by putting a shim FIRST on PATH
# that appends one line per call and then execs the real git, so it counts what
# the gate actually spawns rather than what reading the source suggests.
#
# MATERIALIZATION IS COUNTED AT SPAWN TIME, NOT BY WALKING TMP_DIR AFTERWARDS.
# The first cut of this script did the latter and reported 42 files against two
# 2,577-file checkouts, because `cleanup` removes both worktrees before the gate
# exits -- `--keep-tmp` keeps TMP_DIR, not the trees inside it. A post-hoc walk
# therefore measures what SURVIVED, which is not the question. The shim sees
# every `worktree add` and every subtree assembly as it happens, and the file
# count per tree is read from the clone, so both inputs are measured; their
# product is labelled `derived` because arithmetic is not a measurement.
#
# USAGE: bash scripts/todo-graph/tests/measure_gate_fastpath.sh [label]
# Prints one JSON object. Exit 0 on a measured run, 3 if the fixture itself
# could not be built -- never a silent zero, which would read as a free gate.
set -uo pipefail

# THE HARNESS RE-EXECS ITSELF INTO A CLEAN ENVIRONMENT, ONCE, BEFORE IT MEASURES
# ANYTHING.
#
# The first fix here de-functionized `git`, because an exported shell function of
# that name resolves ahead of PATH and would have let the counting shim be
# bypassed. That fixed the INSTANCE and not the CLASS: this script also produces
# its evidence with `sha256sum`, `grep`, `awk` and `wc`, and an exported function
# for any of them forges the result rather than breaking it -- a constant
# `sha256sum` makes all three byte comparisons agree and prints a digest
# unrelated to what ran; a `grep` intercepting one pattern returns any
# materialization count you like while the gate completes normally and this exits
# 0 (Codex adversarial, section 55, measurement-harness pass).
#
# Chasing utilities one at a time cannot end -- the list is however many the
# script grows. `env -i` drops the whole environment, including every
# `BASH_FUNC_*` export, and `--noprofile --norc` stops an rc file putting them
# back. PATH, HOME and TMPDIR are carried across deliberately and named here, so
# what survives is a short list a reader can check rather than a residue.
# WHAT THIS DEFENDS AGAINST, AND WHAT IT DOES NOT -- stated first, because four
# rounds of hardening here each closed one route and left the next open, and the
# useful conclusion is about the BOUNDARY rather than about aliases.
#
# It defends against inherited shell FUNCTIONS and ALIASES: the ones an operator
# has for their own convenience, or that a customised environment installed.
# That is the common way a measurement gets quietly forged, and the re-exec
# closes it.
#
# It does NOT defend against an executable WRAPPER earlier on PATH. `env -i`
# carries PATH across deliberately -- the harness needs the real tools -- so a
# `git` or `sha256sum` shim in a dotfile-prepended directory is selected by
# `type -P` like any other, and an earlier version of this comment claimed
# otherwise (Codex adversarial, section 55, [medium]). Sanitizing PATH to a
# fixed list would break on any host whose tools live elsewhere, which is a
# worse trade than saying so; the resolved `git` is printed in the JSON instead,
# so a reader can see which binary produced the number.
#
# It does NOT defend against a hostile `BASH_ENV`, and it cannot. `BASH_ENV` is
# read before the first line of this script is parsed, so it can alias `builtin`
# to a no-op, and then `exec` as well -- each fix here was defeated by aliasing
# the primitive the fix depended on, which is not a race that can be won from
# inside a bash process that reads `BASH_ENV`. Nor is it worth winning: anyone
# who can set `BASH_ENV` for this run can equally edit this harness, the gate it
# measures, or the record the number lands in. A measurement harness is evidence
# about the gate's COST; it is not a boundary against someone who already
# controls the machine, and pretending otherwise would be the same overclaim
# this section has had to withdraw several times already.
#
# THE SENTINEL ALONE CANNOT SKIP THIS, and that took three attempts to get
# right because each fix closed the instance in front of it. Presetting the
# sentinel left `BASH_ENV` free to run before any of the hardening below is even
# parsed -- and it can `alias builtin=':'`, at which point every
# `builtin`-prefixed verb is a no-op and `builtin declare -F` reports nothing.
# The route is real and was measured. What ends it is noticing what the route
# REQUIRES: a non-interactive bash reads no rc files, so an alias can only come
# from `BASH_ENV`. A set `BASH_ENV` therefore forces the clean re-exec whatever
# the sentinel says, and `env -i` drops it, so the second stage runs with
# neither -- and the recursion terminates because after `env -i` both are gone.
if [ "${GATE_MEASURE_SANITIZED:-0}" != "1" ] || [ -n "${BASH_ENV:-}" ]; then
    _self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
    exec env -i \
        GATE_MEASURE_SANITIZED=1 \
        PATH="$PATH" HOME="${HOME:-/tmp}" TERM="${TERM:-dumb}" \
        TMPDIR="${TMPDIR:-/tmp}" KEEP_MEASURE_LOG="${KEEP_MEASURE_LOG:-0}" \
        /bin/bash --noprofile --norc "$_self" "$@"
fi

# AND THE SENTINEL IS NOT TRUSTED ON ITS OWN. Presetting
# `GATE_MEASURE_SANITIZED=1` skips the re-exec above entirely, and an exported
# `wc` or `sha256sum` then survives to forge the very numbers this script exists
# to produce (Codex adversarial, section 55, [medium], measured). So the second
# stage CHECKS the property rather than assuming the first stage established it:
# after a clean re-exec there are no shell functions at all, and if there are,
# this refuses instead of measuring. `builtin declare` cannot itself be shadowed
# by a function, which is why the check is written that way.
# FUNCTIONS ARE NOT THE ONLY WAY IN. With the sentinel preset, bash still
# sources `BASH_ENV`, and that file can `shopt -s expand_aliases` and alias `wc`
# or `sha256sum` -- an alias is not a function, so `declare -F` stays empty and
# the contaminated shell was accepted (Codex adversarial, section 55, [medium],
# measured). The second stage therefore RESTORES the properties rather than
# testing for one of them: aliases removed and their expansion disabled, the
# command hash cleared so a poisoned path lookup cannot persist, and only then
# the function check. Each verb is `builtin`-prefixed, because a function named
# `unalias` or `shopt` would otherwise be the next way in.
builtin unalias -a 2>/dev/null || true
builtin shopt -u expand_aliases 2>/dev/null || true
builtin hash -r 2>/dev/null || true
if [ -n "$(builtin declare -F)" ]; then
    echo '{"error":"inherited shell functions are present, so any number this produced could be forged rather than measured"}'
    exit 3
fi

LABEL="${1:-unlabelled}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"

# THE HARNESS NEVER CALLS `git` BY NAME EITHER, and finding that out took a
# control. With an exported shell function named `git` in the environment, bash
# resolves it ahead of PATH -- so `command -v git` answers `git`, the shim is
# bypassed for shell-level calls, and the count comes back plausible and low.
# Hardening only the SHIM left the harness's own `git clone` hijacked by the
# same function, which is how the control failed: it could not even build its
# fixture. `type -P` searches PATH for an executable and ignores functions and
# aliases; every git call below goes through the result.
unset -f git 2>/dev/null || true
REAL_GIT="$(type -P git)" || exit 3
[ -x "$REAL_GIT" ] || { echo '{"error":"no git executable on PATH"}'; exit 3; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/gate-measure.XXXXXX")" || exit 3
trap 'rm -rf "$WORK" 2>/dev/null || true' EXIT

CLONE="$WORK/clone"
"$REAL_GIT" clone --quiet --no-hardlinks --local "$REPO_ROOT" "$CLONE" >/dev/null 2>&1 \
    || { echo '{"error":"clone failed"}'; exit 3; }
# The clone is seeded from the WORKING TREE copy of the gate, so this measures
# the code being changed rather than whatever is committed -- the same choice
# the 22-series fixtures make, and for the same reason.
#
# AND THE RESULT IS BOUND TO THE BYTES IT MEASURED. Copying and then checking
# that the CLONE is clean proves only that the clone matches its own commit; it
# says nothing about whether the source still holds what was copied. An edit
# landing during or after the copy would let this report a confident number for
# a snapshot nobody reviewed. The source is hashed BEFORE the copy, the copy is
# checked against that hash, the source is hashed AGAIN after the run, and all
# three must agree -- and the digest goes into the JSON, so a recorded number
# can be tied back to the bytes it describes (Codex adversarial, section 55,
# measurement-harness pass).
GATE_SRC="$REPO_ROOT/scripts/todo-graph/identity-gate.sh"
SRC_HASH_BEFORE="$(sha256sum "$GATE_SRC" | cut -d" " -f1)" || exit 3
cp "$GATE_SRC" "$CLONE/scripts/todo-graph/identity-gate.sh" || exit 3
COPY_HASH="$(sha256sum "$CLONE/scripts/todo-graph/identity-gate.sh" | cut -d" " -f1)" || exit 3
if [ "$COPY_HASH" != "$SRC_HASH_BEFORE" ]; then
    echo '{"error":"the gate changed between hashing it and copying it, so this run would measure a snapshot nobody named"}'
    exit 3
fi
# THE COMMIT FAILING IS ONLY HARMLESS IF THERE WAS NOTHING TO COMMIT. It was
# swallowed with `|| true`, so a hook, an identity or a signing failure left the
# clone measuring the COMMITTED gate while the JSON claimed to describe the
# working-tree one -- a plausible number about the wrong code, which is worse
# than no number at all.
"$REAL_GIT" -C "$CLONE" -c user.email=m@example.invalid -c user.name=measure \
    commit --quiet --no-verify -m "gate: measure the working-tree copy" \
    -- scripts/todo-graph/identity-gate.sh >/dev/null 2>&1
if ! "$REAL_GIT" -C "$CLONE" diff --quiet -- scripts/todo-graph/identity-gate.sh; then
    echo '{"error":"the working-tree gate could not be committed into the fixture clone, so this run would measure the committed one"}'
    exit 3
fi

# A TODO-ONLY COMMIT, which is exactly the shape the fast path is for: the
# resolver closure is byte-identical base..head, so the gate must reach its
# early exit having differentiated nothing.
BASE_SHA="$("$REAL_GIT" -C "$CLONE" rev-parse HEAD)" || { echo '{"error":"no base"}'; exit 3; }
printf '\n<!-- gate cost measurement, %s -->\n' "$LABEL" \
    >> "$CLONE/todo/00-infrastructure/TODO-06-todo-metadata-layer.md"
"$REAL_GIT" -C "$CLONE" -c user.email=m@example.invalid -c user.name=measure \
    commit --quiet -m "todo: fast-path cost measurement" \
    -- todo/00-infrastructure/TODO-06-todo-metadata-layer.md >/dev/null 2>&1 \
    || { echo '{"error":"commit failed"}'; exit 3; }
HEAD_SHA="$("$REAL_GIT" -C "$CLONE" rev-parse HEAD)"

# THE SHIM COUNTS, IT DOES NOT INTERPRET. Anything cleverer here would become a
# second implementation of the thing being measured.
SHIM="$WORK/bin"
mkdir -p "$SHIM" || exit 3
COUNTER="$WORK/git-calls"
: > "$COUNTER"
cat > "$SHIM/git" <<SHIMEOF
#!/usr/bin/env bash
printf '%s\n' "\$*" >> "$COUNTER"
exec "$REAL_GIT" "\$@"
SHIMEOF
chmod +x "$SHIM/git"

GATE_TMP="$WORK/gate-tmp"
mkdir -p "$GATE_TMP" || exit 3

START_NS="$(date +%s%N)"
env -u BASH_ENV -u "BASH_FUNC_git%%" \
    PATH="$SHIM:$PATH" TMPDIR="$GATE_TMP" \
    bash --noprofile --norc "$CLONE/scripts/todo-graph/identity-gate.sh" \
    --base "$BASE_SHA" --head "$HEAD_SHA" --keep-tmp \
    > "$WORK/gate.out" 2>&1
GATE_RC=$?
END_NS="$(date +%s%N)"

SRC_HASH_AFTER="$(sha256sum "$GATE_SRC" | cut -d" " -f1)" || exit 3
if [ "$SRC_HASH_AFTER" != "$SRC_HASH_BEFORE" ]; then
    echo '{"error":"the gate changed while it was being measured, so this number describes neither the before nor the after"}'
    exit 3
fi

GIT_CALLS="$(wc -l < "$COUNTER" | tr -d ' ')"
WT_ADDS="$(grep -c 'worktree add' "$COUNTER" || true)"
# The subtree assembly this section introduces reads the tree with one
# `ls-tree` under a `todo-graph` pathspec; counting it separately keeps the two
# materialization kinds distinguishable in the record.
SUBTREE_READS="$(grep -c 'ls-tree -r -z' "$COUNTER" || true)"
FILES_PER_TREE="$("$REAL_GIT" -C "$CLONE" ls-tree -r --name-only HEAD | wc -l | tr -d ' ')"
BYTES_PER_TREE="$("$REAL_GIT" -C "$CLONE" ls-tree -r -l HEAD | awk '{s+=$4} END{print s+0}')"
# THE SAME PREFIX SET THE GATE ASSEMBLES, not just the todo-graph half. Counting
# one of the two undercounted the assembly by six files, which is small and is
# still a wrong number in a record whose whole point is the number.
SUBTREE_FILES="$("$REAL_GIT" -C "$CLONE" ls-tree -r --name-only HEAD -- scripts/todo-graph scripts/lint | wc -l | tr -d ' ')"
SUBTREE_BYTES="$("$REAL_GIT" -C "$CLONE" ls-tree -r -l HEAD -- scripts/todo-graph scripts/lint | awk '{s+=$4} END{print s+0}')"
FASTPATH=0
grep -q 'byte-identical base..head; nothing to differentiate' "$WORK/gate.out" && FASTPATH=1
[ "${KEEP_MEASURE_LOG:-0}" = "1" ] && cp "$WORK/gate.out" "${TMPDIR:-/tmp}/gate-measure-$LABEL.log"

# A MEASUREMENT OF A REFUSAL IS NOT A MEASUREMENT. The gate must have reached
# the byte-identical exit for these numbers to describe the path this harness
# exists to measure; a run that died in infrastructure spawns fewer git
# processes and materializes less, so reporting it as a successful measurement
# is a number that argues in the wrong direction.
if [ "$GATE_RC" -ne 0 ] || [ "$FASTPATH" -ne 1 ]; then
    printf '{"error":"the gate did not reach the byte-identical exit","rc":%d,"took_fast_path":%d}\n' \
        "$GATE_RC" "$FASTPATH"
    [ "${KEEP_MEASURE_LOG:-0}" = "1" ] || cp "$WORK/gate.out" "${TMPDIR:-/tmp}/gate-measure-$LABEL-failed.log"
    exit 3
fi

printf '{"label":"%s","gate_sha256":"%s","git_binary":"%s","rc":%d,"took_fast_path":%d,"git_invocations":%s,"worktree_adds":%s,"subtree_reads":%s,"files_per_tree":%s,"bytes_per_tree":%s,"subtree_files":%s,"subtree_bytes":%s,"derived_files_created":%s,"wall_ms":%d}\n' \
    "$LABEL" "$SRC_HASH_BEFORE" "$REAL_GIT" "$GATE_RC" "$FASTPATH" "$GIT_CALLS" "$WT_ADDS" "$SUBTREE_READS" \
    "$FILES_PER_TREE" "$BYTES_PER_TREE" "$SUBTREE_FILES" "$SUBTREE_BYTES" \
    "$(( WT_ADDS * FILES_PER_TREE + SUBTREE_READS * SUBTREE_FILES ))" \
    "$(( (END_NS - START_NS) / 1000000 ))"
