#!/usr/bin/env bash
# Pre-push static-analysis gate for one pushed ref.
#
#   bash scripts/hooks/static-analysis-gate.sh [--plan] <remote> <local-oid> <remote-oid>
#
# The three values are what git hands a pre-push hook: the remote's name and
# the ref update's two ids. When the update adds or changes kernel or loader C
# sources, those sources are analysed as they are IN <local-oid>
# (scripts/static-analysis.sh --commit) and the push is refused on a finding.
# A missing analyzer or an unreadable build refuses too: the only way past is
# SKIP_STATIC_ANALYSIS=1, said out loud.
#
# The base of the comparison is what that remote already holds: the ref's old
# id, or for a new ref the merge base with that remote's own main. A remote
# with no main we know of has nothing, so everything pushed is analysed.
# --plan prints the base and the source count and analyses nothing.
# STATIC_ANALYSIS_GATE_SEEN names a file shared by the refs of one push, so a
# source analysed clean at a commit is analysed once.
#
# Exit: 0 nothing to analyse or clean, 1 refused.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EMPTY_TREE="4b825dc642cb6eb9a060e54bf8d69288fbee4904"
PLAN=0
if [ "${1:-}" = "--plan" ]; then PLAN=1; shift; fi
REMOTE="${1:-}"; HEAD_OID="${2:-}"; REMOTE_OID="${3:-}"
[ -n "$REMOTE" ] && [ -n "$HEAD_OID" ] && [ -n "$REMOTE_OID" ] || {
    echo "static-analysis-gate: usage: [--plan] <remote> <local-oid> <remote-oid>" >&2; exit 1; }
case "$HEAD_OID" in *[!0]*) : ;; *) exit 0 ;; esac            # a deletion pushes no source
# An annotated tag is peeled: the commit is what is analysed and remembered.
HEAD_OID="$(git -C "$REPO_ROOT" rev-parse --quiet --verify "$HEAD_OID^{commit}" 2>/dev/null)" || {
    echo "static-analysis-gate: ${2:-} is not a commit here" >&2; exit 1; }

BASE="$EMPTY_TREE"
case "$REMOTE_OID" in
    *[!0]*) git -C "$REPO_ROOT" rev-parse --quiet --verify "$REMOTE_OID^{commit}" >/dev/null 2>&1 \
                && BASE="$REMOTE_OID" ;;
    *)  if git -C "$REPO_ROOT" rev-parse --quiet --verify "refs/remotes/$REMOTE/main^{commit}" >/dev/null 2>&1; then
            BASE="$(git -C "$REPO_ROOT" merge-base "$HEAD_OID" "refs/remotes/$REMOTE/main" 2>/dev/null)" \
                || BASE="$EMPTY_TREE"
            [ -n "$BASE" ] || BASE="$EMPTY_TREE"
        fi ;;
esac

# Everything but a deletion: a source whose type changed (a file replaced by a
# symlink) is still a source the build compiles.
# NUL-delimited, so a name git would quote arrives as its real bytes; written to
# a file so a failed diff refuses instead of reading as "no sources".
CHANGED="$(mktemp -t static-analysis-gate.XXXXXX)" || exit 1
trap 'rm -f "$CHANGED"' EXIT
git -C "$REPO_ROOT" diff -z --name-only --diff-filter=d "$BASE" "$HEAD_OID" -- 'src/*.c' >"$CHANGED" || {
    echo "static-analysis-gate: cannot diff $BASE..$HEAD_OID" >&2; exit 1; }
FILES=()
while IFS= read -r -d '' f; do
    case "$f" in
        ""|src/kernel/test/*|src/libs/*|src/kernel/acpica/*) ;;   # the analysis leaves these out
        *) FILES+=("$f") ;;
    esac
done <"$CHANGED"
# One push can carry several refs at the same commit (a branch and a tag). The
# hook passes one STATIC_ANALYSIS_GATE_SEEN file for the whole push; a source
# already analysed clean at this commit is not analysed again.
SEEN="${STATIC_ANALYSIS_GATE_SEEN:-}"
if [ -n "$SEEN" ] && [ -s "$SEEN" ] && [ "${#FILES[@]}" -gt 0 ]; then
    REST=()
    for f in "${FILES[@]}"; do
        grep -qxF "$HEAD_OID $f" "$SEEN" || REST+=("$f")
    done
    FILES=("${REST[@]}")
fi
if [ "$PLAN" = "1" ]; then echo "base=$BASE sources=${#FILES[@]}"; exit 0; fi
[ "${#FILES[@]}" -gt 0 ] || exit 0

if [ "${SKIP_STATIC_ANALYSIS:-}" = "1" ]; then
    echo "static-analysis-gate: SKIPPED by SKIP_STATIC_ANALYSIS=1 (${#FILES[@]} changed source(s) not analysed)" >&2
    exit 0
fi

echo "static-analysis-gate: analysing ${#FILES[@]} changed source(s) at ${HEAD_OID:0:12}" >&2
bash "$REPO_ROOT/scripts/hooks/heavy.sh" bash "$REPO_ROOT/scripts/static-analysis.sh" --commit "$HEAD_OID" "${FILES[@]}"
rc=$?
case "$rc" in
    0) [ -z "$SEEN" ] || printf "$HEAD_OID %s\n" "${FILES[@]}" >>"$SEEN"
       exit 0 ;;
    1) echo "static-analysis-gate: push refused, the findings above are in the commit being pushed." >&2
       echo "  Fix each one, or mark an intended line NOLINT(<check>) with its reason." >&2 ;;
    *) echo "static-analysis-gate: push refused, the analysis could not run (status $rc)." >&2
       echo "  Install the analyzer (bash scripts/setup.sh) and build once (bash scripts/build.sh)." >&2 ;;
esac
echo "  Override for one push: SKIP_STATIC_ANALYSIS=1 git push" >&2
exit 1
