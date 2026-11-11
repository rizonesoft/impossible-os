#!/usr/bin/env bash
# Run clang-tidy (the Clang static analyzer plus defect-class bugprone checks,
# selected in .clang-tidy) over kernel and loader sources.
#
#   bash scripts/static-analysis.sh              files changed against origin/main
#   bash scripts/static-analysis.sh --all        the whole tree (about 10 minutes)
#   bash scripts/static-analysis.sh <file>...    the named sources
#   --commit <rev>                               read the named sources from that
#                                                commit, not from the checkout
#   --with-tests                                 include src/kernel/test/
#
# The compile database is derived on every run from a dry run of the build
# (scripts/gen-compile-db.py), so it covers the loader, the release-only
# sources and a file added a minute ago. A named or changed source under src/
# that the build does not compile is a setup failure, never a silent skip.
#
# --commit is what the pre-push gate uses: the sources, headers, .clang-tidy
# and Makefiles come from a snapshot of that commit, so nothing uncommitted in
# the checkout can hide a defect in the commit being pushed. Only the headers a
# build generates (under build/, and include/build_info.h) are borrowed from
# the checkout.
#
# Test sources are left out by default: on the first whole-tree run they were
# 108 of 157 findings, nearly all the analyzer not knowing that a failed
# TEST_ASSERT returns. Vendored code (src/libs/, src/kernel/acpica/) is out.
#
# Exit: 0 no findings, 1 findings (listed, deduplicated), 2 setup failure.
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 2

TIDY="${CLANG_TIDY:-clang-tidy-19}"

# The build cannot compile a source whose name needs quoting, and neither make
# nor a diagnostic line carries such a name intact. Refused, not skipped.
check_name() {
    case "$1" in
        src/*.c) case "$1" in *[!A-Za-z0-9_./+-]*)
            printf 'static-analysis: unsupported source name (letters, digits and _ . / + - only): %q\n' "$1" >&2
            exit 2 ;; esac ;;
    esac
}

MODE="changed"; WITH_TESTS=0; NAMED=(); COMMIT=""; WANT_COMMIT=0
for a in "$@"; do
    if [ "$WANT_COMMIT" = "1" ]; then COMMIT="$a"; WANT_COMMIT=0; continue; fi
    case "$a" in
        --all) MODE="all" ;;
        --with-tests) WITH_TESTS=1 ;;
        --commit) WANT_COMMIT=1 ;;
        -h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "static-analysis: unknown option $a" >&2; exit 2 ;;
        *) MODE="named"; NAMED+=("$a") ;;
    esac
done
[ "$WANT_COMMIT" = "0" ] || { echo "static-analysis: --commit needs a revision" >&2; exit 2; }
if [ -n "$COMMIT" ] && [ "$MODE" != "named" ]; then
    echo "static-analysis: --commit needs the sources to read from it" >&2; exit 2
fi
# What is wrong with the input is reported before what the host lacks: a named
# source, and below a tracked symlink, is refused the same on every host, with
# or without the analyzer and a build.
for f in "${NAMED[@]}"; do check_name "$f"; done

WORK="$REPO_ROOT"; SNAP=""; OUT=""; ERR=""
cleanup() { rm -f "$OUT" "$ERR"; [ -z "$SNAP" ] || rm -rf "$SNAP"; }
trap cleanup EXIT

DB_DIR="${STATIC_ANALYSIS_DB_DIR:-$REPO_ROOT/build/static-analysis}"
if [ -n "$COMMIT" ]; then
    OID="$(git rev-parse --quiet --verify "$COMMIT^{commit}")" || {
        echo "static-analysis: $COMMIT is not a commit" >&2; exit 2; }
    # The repository tracks no symlink. One in the pushed tree could point the
    # analysis, or a file written below, at something outside the snapshot, so
    # it is refused by name before anything is extracted.
    LINKS="$(git ls-tree -r "$OID" | awk '$1 == "120000"' | cut -f2-)" || exit 2
    if [ -n "$LINKS" ]; then
        printf 'static-analysis: tracked symlink in %s, refusing to analyse: %s\n' "${OID:0:12}" "$LINKS" >&2
        exit 2
    fi
fi
command -v "$TIDY" >/dev/null 2>&1 || {
    echo "static-analysis: no $TIDY on PATH (bash scripts/setup.sh)" >&2; exit 2; }
if [ -n "$COMMIT" ]; then
    SNAP="$(mktemp -d -t static-analysis-snap.XXXXXX)" || exit 2
    SNAP="$(cd "$SNAP" && pwd -P)" || exit 2
    # The tracked tree of that commit: sources, headers, .clang-tidy and the
    # Makefiles (and resources/, which the Makefile names as prerequisites).
    git archive "$OID" -- . ':(exclude)todo' ':(exclude)docs' \
        | tar -x -C "$SNAP" || { echo "static-analysis: could not read $OID" >&2; exit 2; }
    # What only a build produces is borrowed from the checkout: the headers it
    # generates into the source directories and under build/. They are COPIED
    # into a build/ of the snapshot's own: nothing in the snapshot points at the
    # checkout, so nothing run here (a Makefile is evaluated) can write into it.
    # A path the commit itself tracks is never replaced by the checkout's file.
    while IFS= read -r -d '' g; do
        [ -e "$SNAP/$g" ] && continue          # the commit tracks it: the commit's copy stands
        case "$g" in *.h|*.inc) mkdir -p "$SNAP/$(dirname "$g")" && cp "$g" "$SNAP/$g" || exit 2 ;; esac
    done < <(git ls-files -z -o -i --exclude-standard -- include src)
    mkdir -p "$SNAP/build" || exit 2
    if [ -d "$REPO_ROOT/build" ]; then
        while IFS= read -r -d '' g; do
            [ -e "$SNAP/$g" ] && continue
            mkdir -p "$SNAP/$(dirname "$g")" && cp "$g" "$SNAP/$g" || exit 2
        done < <(find build \( -name '*.h' -o -name '*.inc' \) -type f -print0)
    fi
    # The compile commands come from that commit's own Makefiles.
    python3 scripts/gen-compile-db.py --root "$SNAP" \
        --output "$SNAP/.static-analysis/compile_commands.json" >/dev/null || {
        echo "static-analysis: could not derive the compile database of $OID" >&2
        echo "static-analysis: the tree needs a build first (bash scripts/build.sh)" >&2; exit 2; }
    WORK="$SNAP"; DB_DIR="$SNAP/.static-analysis"
else
    python3 scripts/gen-compile-db.py --output "$DB_DIR/compile_commands.json" >/dev/null || {
        echo "static-analysis: could not derive the compile database" >&2; exit 2; }
fi

# Every source the build compiles, repo-relative.
# Captured, then split: a process substitution would hide a parse failure and
# a malformed database would read as "nothing to analyze".
DB_LIST="$(python3 - "$DB_DIR/compile_commands.json" "$WORK" <<'PY'
import json, os, sys
root = sys.argv[2]
seen = set()
for e in json.load(open(sys.argv[1])):
    f = os.path.relpath(os.path.join(e.get("directory", root), e["file"]), root)
    if f.endswith(".c") and not f.startswith("..") and f not in seen:
        seen.add(f)
        print(f)
PY
)" || { echo "static-analysis: the compile database is not readable" >&2; exit 2; }
[ -n "$DB_LIST" ] || { echo "static-analysis: the compile database lists no C sources" >&2; exit 2; }
mapfile -t IN_DB <<<"$DB_LIST"

case "$MODE" in
    all)     CANDIDATES=("${IN_DB[@]}") ;;
    named)   CANDIDATES=("${NAMED[@]}") ;;
    changed)
        BASE="$(git merge-base HEAD origin/main 2>/dev/null || true)"
        mapfile -t CANDIDATES < <( { [ -n "$BASE" ] && git diff --name-only "$BASE" -- '*.c'; \
                                     git diff --name-only HEAD -- '*.c'; \
                                     git ls-files -o --exclude-standard -- '*.c'; } | sort -u)
        ;;
esac

# Membership by lookup, not `printf | grep -q`: under pipefail a grep that
# exits at its first match fails the pipeline with SIGPIPE on the writer.
declare -A DB_SET=()
for f in "${IN_DB[@]}"; do DB_SET["$f"]=1; done

FILES=(); UNCOMPILED=()
for f in "${CANDIDATES[@]}"; do
    [ -n "$f" ] || continue
    check_name "$f"
    if [ ! -f "$WORK/$f" ]; then
        # A changed-files run meets deleted sources; a source someone named must exist.
        [ "$MODE" = "changed" ] && continue
        echo "static-analysis: no such source${COMMIT:+ in $COMMIT}: $f" >&2; exit 2
    fi
    case "$f" in
        src/kernel/test/*) [ "$WITH_TESTS" = "1" ] || continue ;;
        src/libs/*|src/kernel/acpica/*) continue ;; # vendored
        src/*.c) ;;
        *) continue ;;                              # host tools, user code, the SDK
    esac
    if [ -z "${DB_SET[$f]:-}" ]; then UNCOMPILED+=("$f"); continue; fi
    FILES+=("$f")
done

if [ "${#UNCOMPILED[@]}" -ne 0 ]; then
    printf 'static-analysis: not compiled by the build, so it cannot be analysed: %s\n' "${UNCOMPILED[@]}" >&2
    echo "static-analysis: add the source to the build, or to this script's vendored list" >&2
    exit 2
fi
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "static-analysis: nothing to analyze ($MODE)"
    exit 0
fi

OUT="$(mktemp -t static-analysis.XXXXXX)" || exit 2
ERR="$(mktemp -t static-analysis-err.XXXXXX)" || exit 2
JOBS="${STATIC_ANALYSIS_JOBS:-$(nproc 2>/dev/null || echo 4)}"
# clang-tidy exits 0 when it only WARNS (WarningsAsErrors is empty), so a
# nonzero status here is the analyzer failing to run or to compile a file,
# and that must never read as "no findings".
TIDY_RC=0
# One analyzer per source, in its own compile directory. A diagnostic names its
# file relative to that directory (the loader builds in src/boot/uefi), so each
# path is resolved there and printed repo-relative.
python3 - "$DB_DIR/compile_commands.json" "$WORK" "$TIDY" "$JOBS" "${FILES[@]}" >"$OUT" 2>"$ERR" <<'PY' || TIDY_RC=$?
import json, os, re, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
db_path, work, tidy, jobs = sys.argv[1:5]
files = sys.argv[5:]
dirs = {}
for e in json.load(open(db_path)):
    dirs.setdefault(os.path.relpath(os.path.join(e["directory"], e["file"]), work), e["directory"])
diag = re.compile(r"^(?P<path>[^:\s][^:]*):(?P<rest>\d+:\d+: (?:warning|error|note): .*)$")

def one(f):
    d = dirs[f]
    try:
        # 30 minutes a file: the largest source takes under one.
        r = subprocess.run([tidy, "-p", os.path.dirname(db_path), "--quiet", os.path.join(work, f)],
                           cwd=d, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                           errors="replace", timeout=1800)
    except subprocess.TimeoutExpired:
        return 124, "", f"Error while processing {f}: analyzer timed out\n"
    except OSError as e:
        return 127, "", f"Error while processing {f}: {e}\n"
    out = []
    for line in r.stdout.splitlines():
        m = diag.match(line)
        if m:
            p = os.path.normpath(os.path.join(d, m.group("path")))
            if p == work or p.startswith(work + os.sep):
                p = os.path.relpath(p, work)
            line = f"{p}:{m.group('rest')}"
        out.append(line)
    return r.returncode, "\n".join(out) + ("\n" if out else ""), r.stderr

worst = 0
with ThreadPoolExecutor(max_workers=max(1, int(jobs))) as pool:
    for rc, out, err in pool.map(one, files):
        sys.stdout.write(out)
        sys.stderr.write(err)
        worst = worst or rc
sys.exit(1 if worst else 0)
PY
if [ "$TIDY_RC" -ne 0 ] || grep -qE '^.+:[0-9]+:[0-9]+: error: |^Error while processing ' "$OUT" "$ERR"; then
    grep -hE ': error: |^Error |^error: ' "$OUT" "$ERR" | sort -u | head -20 >&2
    echo "static-analysis: the analyzer failed (status $TIDY_RC); this is not a clean result" >&2
    echo "static-analysis: a missing generated header means the tree needs a build (bash scripts/build.sh)" >&2
    exit 2
fi

FINDINGS="$(grep -E '^.+:[0-9]+:[0-9]+: warning: ' "$OUT" \
    | grep -vE '^include/stb_' | sort -u)"
if [ -z "$FINDINGS" ]; then
    echo "static-analysis: ${#FILES[@]} file(s), no findings"
    exit 0
fi
printf '%s\n' "$FINDINGS"
echo "static-analysis: ${#FILES[@]} file(s), $(printf '%s\n' "$FINDINGS" | wc -l) finding(s)"
exit 1
