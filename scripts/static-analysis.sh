#!/usr/bin/env bash
# Run clang-tidy (the Clang static analyzer plus defect-class bugprone checks,
# selected in .clang-tidy) over kernel and loader sources.
#
#   bash scripts/static-analysis.sh              files changed against origin/main
#   bash scripts/static-analysis.sh --all        the whole tree (about 10 minutes)
#   bash scripts/static-analysis.sh <file>...    the named sources
#   --with-tests                                 include src/kernel/test/
#
# It needs compile_commands.json, which `bash scripts/build.sh clean` writes
# (through bear). Test sources are left out by default: on the first whole-tree
# run they were 108 of 157 findings, nearly all the analyzer not knowing that a
# failed TEST_ASSERT returns.
#
# Exit: 0 no findings, 1 findings (listed, deduplicated), 2 setup failure.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 2

TIDY="${CLANG_TIDY:-clang-tidy-19}"
command -v "$TIDY" >/dev/null 2>&1 || {
    echo "static-analysis: no $TIDY on PATH (bash scripts/setup.sh)" >&2; exit 2; }
[ -s compile_commands.json ] || {
    echo "static-analysis: no compile_commands.json; run: bash scripts/build.sh clean" >&2; exit 2; }

MODE="changed"; WITH_TESTS=0; NAMED=()
for a in "$@"; do
    case "$a" in
        --all) MODE="all" ;;
        --with-tests) WITH_TESTS=1 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "static-analysis: unknown option $a" >&2; exit 2 ;;
        *) MODE="named"; NAMED+=("$a") ;;
    esac
done

# Every source the build actually compiles, repo-relative.
# Captured, then split: a process substitution would hide a parse failure and
# a malformed database would read as "nothing to analyze".
DB_LIST="$(python3 - <<'PY'
import json, os
root = os.getcwd()
seen = set()
for e in json.load(open("compile_commands.json")):
    f = os.path.relpath(os.path.join(e.get("directory", root), e["file"]), root)
    if f.endswith(".c") and not f.startswith("..") and f not in seen:
        seen.add(f)
        print(f)
PY
)" || { echo "static-analysis: compile_commands.json is not a readable compile database" >&2; exit 2; }
[ -n "$DB_LIST" ] || { echo "static-analysis: compile_commands.json lists no C sources" >&2; exit 2; }
mapfile -t IN_DB <<<"$DB_LIST"

case "$MODE" in
    all)     CANDIDATES=("${IN_DB[@]}") ;;
    named)   CANDIDATES=("${NAMED[@]}") ;;
    changed)
        BASE="$(git merge-base HEAD origin/main 2>/dev/null || true)"
        mapfile -t CANDIDATES < <( { [ -n "$BASE" ] && git diff --name-only "$BASE" -- '*.c'; \
                                     git diff --name-only HEAD -- '*.c'; } | sort -u)
        ;;
esac

# Membership by lookup, not `printf | grep -q`: under pipefail a grep that
# exits at its first match fails the pipeline with SIGPIPE on the writer.
declare -A DB_SET=()
for f in "${IN_DB[@]}"; do DB_SET["$f"]=1; done

FILES=()
for f in "${CANDIDATES[@]}"; do
    [ -f "$f" ] || continue
    case "$f" in
        src/kernel/test/*) [ "$WITH_TESTS" = "1" ] || continue ;;
        src/libs/*|tools/*|user/*) continue ;;      # vendored, host and user code
    esac
    [ -n "${DB_SET[$f]:-}" ] || continue
    FILES+=("$f")
done

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "static-analysis: nothing to analyze ($MODE)"
    exit 0
fi

OUT="$(mktemp -t static-analysis.XXXXXX)" || exit 2
ERR="$(mktemp -t static-analysis-err.XXXXXX)" || exit 2
trap 'rm -f "$OUT" "$ERR"' EXIT
JOBS="${STATIC_ANALYSIS_JOBS:-$(nproc 2>/dev/null || echo 4)}"
# clang-tidy exits 0 when it only WARNS (WarningsAsErrors is empty), so a
# nonzero status here is the analyzer failing to run or to compile a file,
# and that must never read as "no findings".
TIDY_RC=0
printf '%s\0' "${FILES[@]}" \
    | xargs -0 -P "$JOBS" -n 1 "$TIDY" -p . --quiet >"$OUT" 2>"$ERR" || TIDY_RC=$?
if [ "$TIDY_RC" -ne 0 ] || grep -qE '^[^ ]+:[0-9]+:[0-9]+: error: |^Error while processing ' "$OUT" "$ERR"; then
    grep -hE ': error: |^Error |^error: ' "$OUT" "$ERR" | sed "s|$REPO_ROOT/||" | sort -u | head -20 >&2
    echo "static-analysis: the analyzer failed (status $TIDY_RC); this is not a clean result" >&2
    exit 2
fi

FINDINGS="$(grep -E '^[^ ]+:[0-9]+:[0-9]+: warning: ' "$OUT" \
    | sed "s|^$REPO_ROOT/||" | grep -vE '^include/stb_' | sort -u)"
if [ -z "$FINDINGS" ]; then
    echo "static-analysis: ${#FILES[@]} file(s), no findings"
    exit 0
fi
printf '%s\n' "$FINDINGS"
echo "static-analysis: ${#FILES[@]} file(s), $(printf '%s\n' "$FINDINGS" | wc -l) finding(s)"
exit 1
