#!/usr/bin/env bash
# ============================================================================
# lint.sh -- Code style linter for Impossible OS
#
# Checks all C source and header files against the project's coding standards.
# Uses grep and awk for pattern matching (no external dependencies).
#
# Usage:
#   bash scripts/lint.sh              # Lint all files
#   bash scripts/lint.sh src/kernel/  # Lint specific directory
#
# Exit codes:
#   0 = clean (no violations)
#   1 = violations found
# ============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---- Help ----
case "${1:-}" in
    -h|--help)
        cat <<'EOF'
Impossible OS -- code style linter

Usage:
  bash scripts/lint.sh                    Lint full repo (default scope)
  bash scripts/lint.sh src/kernel/mm/     Lint a specific path (skips xref check)
  bash scripts/lint.sh --help             Show this help

Checks (5):
  1. #pragma once or include guard in every .h
  2. Lines <= 120 characters (excludes comment lines)
  3. No trailing whitespace
  4. No numeric TODO shorthand (TODO-NN sectionN, DNN TNN) outside todo/**
     except a baseline allowlist of pre-existing files (see the developer
     tooling stack roadmap -- Tooling Doctor section -- for the legacy-XREF
     sweep).
  5. No bare "section sign + number" (§N) in code comments outside todo/**
     unless the same line carries an external-spec qualifier (UEFI, Intel
     SDM, NTFS, ACPI, RFC, etc.). Pre-existing files are warn-listed;
     anything new is an error. Code comments must name the FEATURE, not
     the TODO section number, because numbers drift silently on renumber.

Removed 2026-04-17: "functions > 50 lines" and "lowercase #define" warnings
(warn-only, never triaged; duplicated Codex + domain code-quality coverage).
Removed 2026-04-18: camelCase-function warning -- unsafe for a Win11-native
OS where NT/Rtl/Ob/Se/Ke/Mm/Io/Ex API names are deliberately CamelCase.
Style review belongs with Codex adversarial review + domain code-quality
skills; lint stays focused on drift-catching structural checks.

Exit codes:
  0 = clean
  1 = errors found
EOF
        exit 0
        ;;
esac

# ---- Colors ----
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
DIM='\033[0;90m'
NC='\033[0m'

# ---- Counters ----
ERRORS=0
WARNINGS=0

error() {
    local file=$1 line=$2 msg=$3
    echo -e "${RED}error${NC}: ${file}:${line}: ${msg}"
    ERRORS=$((ERRORS + 1))
}

warn() {
    local file=$1 line=$2 msg=$3
    echo -e "${YELLOW}warn${NC}: ${file}:${line}: ${msg}"
    WARNINGS=$((WARNINGS + 1))
}

# ---- Determine files to lint ----
SEARCH_PATH="${1:-$REPO_ROOT/src $REPO_ROOT/include}"

# Exclude third-party and auto-generated files. Third-party libraries
# carry their upstream style and line-length conventions -- we do not own
# them, so we do not gate on them. Auto-generated headers are pure data
# dumps with mechanical formatting.
EXCLUDE_PATTERNS=(
    "stb_truetype"
    "stb_image"
    "cJSON"
    "build_info.h"
    "os_logo.h"
    "bsod_icon.h"
    "boot_splash_font_data.h"
)

build_exclude_args() {
    local args=""
    for pat in "${EXCLUDE_PATTERNS[@]}"; do
        args="$args ! -name '*${pat}*'"
    done
    # Vendored third-party libraries under src/libs/ carry their own upstream
    # style and line lengths -- we do not own them, so exclude the whole tree
    # (monocypher, cjson, lz4, miniz, mbedtls). Our kernel-side wrappers live in
    # src/kernel/ and are still linted.
    args="$args ! -path '*/src/libs/*'"
    echo "$args"
}

# Find all .c and .h files
mapfile -t FILES < <(eval "find $SEARCH_PATH -type f \\( -name '*.c' -o -name '*.h' \\) $(build_exclude_args)" 2>/dev/null | sort)

if [ ${#FILES[@]} -eq 0 ]; then
    echo -e "${RED}No source files found.${NC}"
    exit 1
fi

echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "${CYAN}  Impossible OS -- Code Style Linter${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
echo -e "  ${DIM}Checking ${#FILES[@]} files...${NC}"
echo ""

# ============================================================================
# Check 1: #pragma once or include guard in .h files
# ============================================================================
for file in "${FILES[@]}"; do
    [[ "$file" != *.h ]] && continue
    relpath="${file#"$REPO_ROOT"/}"

    if ! grep -q '#pragma once' "$file" 2>/dev/null; then
        # Check for traditional include guards
        if ! grep -qE '#ifndef\s+\w+_H' "$file" 2>/dev/null; then
            error "$relpath" 1 "missing #pragma once or include guard"
        fi
    fi
done

# ============================================================================
# Check 2: Lines > 120 characters
# ============================================================================
for file in "${FILES[@]}"; do
    relpath="${file#"$REPO_ROOT"/}"
    awk -v file="$relpath" '
        length($0) > 120 {
            # Skip comments and long string literals
            if ($0 ~ /^\s*\*/ || $0 ~ /^\s*\/\*/ || $0 ~ /^\s*\/\//) next
            printf "'"${RED}"'error'"${NC}"': %s:%d: line exceeds 120 characters (%d chars)\n", file, NR, length($0)
            errors++
        }
        END { exit errors > 0 ? 1 : 0 }
    ' "$file" 2>/dev/null && true
    # Count errors from awk output
    long_lines=$(awk 'length($0) > 120 && $0 !~ /^\s*\*/ && $0 !~ /^\s*\/\*/ && $0 !~ /^\s*\/\//' "$file" 2>/dev/null | wc -l)
    ERRORS=$((ERRORS + long_lines))
done

# ============================================================================
# Check 3: Trailing whitespace
# ============================================================================
for file in "${FILES[@]}"; do
    relpath="${file#"$REPO_ROOT"/}"
    while IFS= read -r match; do
        linenum=$(echo "$match" | cut -d: -f1)
        error "$relpath" "$linenum" "trailing whitespace"
    done < <(grep -n '[[:space:]]$' "$file" 2>/dev/null || true)
done

# ============================================================================
# Check 4: Drift-prone TODO numeric shorthand outside todo/
# ============================================================================
# Numeric XREFs (e.g. TODO-<num> <section-sign><num>, D<num> T<num> <section-sign><num>)
# go stale silently when TODOs renumber. Inside todo/** the create-todo / validate-todo-file skills
# enforce the shorthand deliberately. Outside todo/** we want anchor links to a
# canonical doc or named-capability references instead.
#
# Known-dirty files (pre-existing refs) get a warning; everything else is an
# error so new drift cannot sneak in. Legacy cleanup is tracked in
# todo/00-infrastructure/TODO-01-developer-tooling-stack.md "tooling doctor"
# section.
#
# Only runs on the default lint scope (whole repo). If a path argument was
# given, skip -- per-subsystem runs should not scan outside their path.
if [ "$#" -eq 0 ]; then
    # ERE regex matching all four drift-prone shorthand flavors.
    TODO_XREF_REGEX='(TODO-[0-9]+[[:space:]]*§[0-9]+|TODO-[0-9]+[[:space:]]+section[[:space:]]+[0-9]+|D[0-9]+[[:space:]]*T[0-9]+[[:space:]]*§?[0-9]+|\bT[0-9]+[[:space:]]*§[0-9]+)'

    # Paths with pre-existing legacy refs (warn-only until the sweep lands).
    # Keep sorted alphabetically for easy audit. The legacy sweep landed
    # with the developer-tooling stack Legacy-XREF Sweep item; this list
    # is intentionally empty so any new numeric-TODO shorthand is flagged
    # as an error.
    TODO_XREF_LEGACY_FILES=()

    is_todo_xref_legacy() {
        local path="$1"
        local legacy
        for legacy in "${TODO_XREF_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    # Top-level roots to scan. Keep narrow so unrelated repos (node_modules,
    # build artifacts, plugin marketplaces) are not traversed. `.github/`
    # is included so workflow YAML cannot sneak in shorthand that would
    # then outlive a TODO renumbering. The PULL_REQUEST_TEMPLATE.md skip
    # below preserves the intentional teaching template.
    TODO_XREF_ROOTS=(
        "$REPO_ROOT/src"
        "$REPO_ROOT/include"
        "$REPO_ROOT/scripts"
        "$REPO_ROOT/docs"
        "$REPO_ROOT/user"
        "$REPO_ROOT/tools"
        "$REPO_ROOT/.github"
        "$REPO_ROOT/README.md"
        "$REPO_ROOT/CONTRIBUTING.md"
        "$REPO_ROOT/CLAUDE.md"
        "$REPO_ROOT/Makefile"
        "$REPO_ROOT/CHANGELOG.md"
    )

    # Existing roots only (some may not exist in every checkout).
    TODO_XREF_EXISTING=()
    for root in "${TODO_XREF_ROOTS[@]}"; do
        [ -e "$root" ] && TODO_XREF_EXISTING+=("$root")
    done

    while IFS=: read -r file linenum rest; do
        [ -z "$file" ] && continue
        relpath="${file#"$REPO_ROOT"/}"
        # Skip templates that TEACH the shorthand intentionally, and the
        # AI-workflow regression script whose whole purpose is to enforce
        # the AI Development System roadmap section structure (references
        # are load-bearing -- if those sections renumber, the script must
        # be updated too; the rule against drift does not apply to the
        # file that IS the drift-detector).
        case "$relpath" in
            .github/PULL_REQUEST_TEMPLATE.md) continue ;;
            scripts/test-ai-system.sh) continue ;;
            scripts/todo-graph/*) continue ;;
            *.claude/*) continue ;;
            # Append-only historical run log (archived out of the doctrine
            # file 2026-07-11): entries reference TODO numbers/sections AS
            # THEY WERE at the time; renumbering-drift rules are for live
            # references, not history.
            docs/overnight/run-log.md) continue ;;
        esac
        if is_todo_xref_legacy "$relpath"; then
            warn "$relpath" "$linenum" "numeric TODO shorthand (legacy; tracked for cleanup)"
        else
            error "$relpath" "$linenum" "numeric TODO shorthand outside todo/ (breaks on TODO renumbering; use anchor link or capability name)"
        fi
    done < <(
        grep -rnHE "$TODO_XREF_REGEX" \
            --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
            --include='*.sh' --include='*.md' --include='*.json' \
            --include='*.bat' --include='*.ps1' --include='*.py' \
            --include='*.yml' --include='*.yaml' \
            --include='Makefile' --include='*.mk' \
            --exclude-dir='.git' --exclude-dir='build' --exclude-dir='node_modules' \
            "${TODO_XREF_EXISTING[@]}" 2>/dev/null \
        || true
    )
fi

# ============================================================================
# Check 5: Bare "section sign + number" in code comments without external-spec
# qualifier. A bare section-sign glyph followed by a digit inside a .c / .h /
# .asm / .py / .sh file almost always refers to a TODO section; those break
# silently when the owning TODO renumbers. External-spec citations (UEFI,
# Intel SDM, NTFS, ACPI) stay legal because they point at stable published
# standards.
#
# Allowed tokens on the same line: UEFI, Intel, SDM, AMD, APM, RFC <n>,
# ACPI <n>, NTFS, FAT32/16, NVMe, PCI / PCIe, PE/COFF, PE32, COFF, USB <n>,
# xHCI / EHCI / OHCI / UHCI, VirtIO, SMBIOS, IEEE, NIST, TCG, WHEA, HPET,
# MP Spec, "spec ", "specification".
#
# The legacy allowlist lists every file that already contains bare §N refs
# today (WARN) so the check does not block commits while the cleanup is in
# progress. Any file NOT on the allowlist must not introduce new bare §N --
# that is the permanent drift gate for new code.
#
# Only runs on the default lint scope (whole repo).
if [ "$#" -eq 0 ]; then
    BARE_SECTION_RE='§ ?[0-9]'
    SPEC_TOKENS_RE='(UEFI|Intel|SDM|AMD|APM|RFC [0-9]|ACPI [0-9]|NTFS|FAT[0-9]|NVMe|PCIe?|PE/COFF|PE32|COFF|USB [0-9]|xHCI|EHCI|OHCI|UHCI|VirtIO|SMBIOS|IEEE|NIST|TCG|WHEA|HPET|MP Spec|spec |specification)'

    # Path-based spec-code exemption: files whose WHOLE JOB is implementing
    # an external standard carry §N refs to that standard per-line without
    # repeating the standard's name (the file scope provides the context).
    # Add directories/paths here only when every §N ref in the file truly
    # points at the named external standard.
    is_bare_section_spec_code() {
        local path="$1"
        case "$path" in
            # NTFS on-disk format spec (Linux-NTFS + Microsoft NTFS reference).
            src/kernel/fs/ntfs/*|include/kernel/fs/ntfs.h) return 0 ;;
            # Vendored third-party libraries cite RFC/standard sections with the
            # section glyph; we do not own their comments (lz4, miniz, mbedtls).
            src/libs/*) return 0 ;;
        esac
        return 1
    }

    # Files with pre-existing bare §N refs (WARN-only; tracked for cleanup
    # in todo/00-infrastructure/TODO-01-developer-tooling-stack.md). Keep
    # sorted alphabetically for easy audit. Any new file must NOT land
    # here -- check the lint output and replace the §N with a feature
    # name or a doc anchor link before adding.
    BARE_SECTION_LEGACY_FILES=()

    is_bare_section_legacy() {
        local path="$1"
        local legacy
        for legacy in "${BARE_SECTION_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    # Same scan roots as Check 4; reuse the existing TODO_XREF_EXISTING array.
    while IFS=: read -r file linenum rest; do
        [ -z "$file" ] && continue
        # Line passes if it carries an external-spec qualifier.
        if echo "$rest" | grep -qE "$SPEC_TOKENS_RE"; then
            continue
        fi
        relpath="${file#"$REPO_ROOT"/}"
        case "$relpath" in
            .github/PULL_REQUEST_TEMPLATE.md) continue ;;
            scripts/test-ai-system.sh) continue ;;
            scripts/todo-graph/*) continue ;;
            *.claude/*) continue ;;
            # Append-only historical run log (archived out of the doctrine
            # file 2026-07-11): entries reference TODO numbers/sections AS
            # THEY WERE at the time; renumbering-drift rules are for live
            # references, not history.
            docs/overnight/run-log.md) continue ;;
        esac
        # Path-based spec-code exemption (NTFS etc.).
        if is_bare_section_spec_code "$relpath"; then
            continue
        fi
        if is_bare_section_legacy "$relpath"; then
            warn "$relpath" "$linenum" "bare section ref (legacy; tracked for cleanup)"
        else
            error "$relpath" "$linenum" "bare section ref in code (breaks on TODO renumbering; name the feature or cite external spec)"
        fi
    done < <(
        grep -rnHE "$BARE_SECTION_RE" \
            --include='*.c' --include='*.h' --include='*.asm' --include='*.S' \
            --include='*.sh' --include='*.bat' --include='*.ps1' --include='*.py' \
            --include='*.yml' --include='*.yaml' \
            --include='Makefile' --include='*.mk' \
            --include='*.ld' --include='*.lds' --include='*.inc' \
            --exclude-dir='.git' --exclude-dir='build' --exclude-dir='node_modules' \
            "${TODO_XREF_EXISTING[@]}" 2>/dev/null \
        || true
    )
fi

# ============================================================================
# Check 6: Tautological-test detection (TODO-08 #12)
# ============================================================================
# Walks src/kernel/test/test_*.c and flags two AI-slop anti-patterns that
# build green and pass the test runner but verify nothing:
#   (a) TEST_ASSERT_EQ(X, X, ...) where LHS == RHS string-identical
#   (b) TEST_ASSERT(true, ...) / TEST_ASSERT(1, ...)
# Allowlist: a /* TEST-TAUTOLOGY-OK: <reason> */ marker on the same line.
# Skip the whole check via SKIP_LINT_TAUTOLOGY=1 on the lint invocation;
# the skip prints a visible WARN line so bypasses are auditable.
if [ "${SKIP_LINT_TAUTOLOGY:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 6 (tautological-test) skipped via SKIP_LINT_TAUTOLOGY=1"
    WARNINGS=$((WARNINGS + 1))
else
    # Legacy-allowlist pattern: pre-existing tautological tests get WARN
    # (tracked for cleanup in todo/00-infrastructure/TODO-08-automation
    # -hardening.md #12 follow-up); any NEW tautological test in any
    # other file is ERROR. Mirrors Check 5's BARE_SECTION_LEGACY_FILES.
    TAUTOLOGY_LEGACY_FILES=(
        "src/kernel/test/test_alpc.c"
        "src/kernel/test/test_boot_init.c"
        "src/kernel/test/test_bulletproof.c"
        "src/kernel/test/test_cpu_security.c"
        "src/kernel/test/test_crashdump.c"
        "src/kernel/test/test_desktop.c"
        "src/kernel/test/test_exec.c"
        "src/kernel/test/test_ixfs.c"
        "src/kernel/test/test_klog.c"
        "src/kernel/test/test_nt_types.c"
        "src/kernel/test/test_peb_teb.c"
        "src/kernel/test/test_usermode_launcher.c"
        "src/kernel/test/test_vfs.c"
    )
    is_tautology_legacy() {
        local path="$1"
        local legacy
        for legacy in "${TAUTOLOGY_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    # Collect all test files and run the python checker once -- saves
    # ~50 fork-execs vs the prior per-file awk loop AND handles three
    # patterns the awk version missed (Codex review 2026-04-28):
    # multiline assertions, inner-comma tautologies, and SYM-vs-literal
    # -of-SYM comparisons. Output is one finding per line in the
    # `relpath:line:detail` format that the parent loop dispatches via
    # error()/warn() based on TAUTOLOGY_LEGACY_FILES membership.
    TAUTOLOGY_FILES=()
    while IFS= read -r file; do
        [ -n "$file" ] && TAUTOLOGY_FILES+=("$file")
    done < <(find "$REPO_ROOT/src/kernel/test" -maxdepth 1 -type f -name 'test_*.c' 2>/dev/null)
    if [ "${#TAUTOLOGY_FILES[@]}" -gt 0 ]; then
        while IFS=: read -r f l rest; do
            [ -z "$f" ] && continue
            if is_tautology_legacy "$f"; then
                warn "$f" "$l" "$rest (legacy; tracked for cleanup)"
            else
                error "$f" "$l" "$rest"
            fi
        done < <(python3 "$REPO_ROOT/scripts/lint/check_tautological_test.py" \
            "${TAUTOLOGY_FILES[@]}" 2>/dev/null)
    fi
fi

# ============================================================================
# Check 7: Stub-behind-stamp detection (TODO-08 #12 + TODO-06 #9)
# ============================================================================
# Reads build/todo-cache.json for the stamped-item -> {file, symbol} map
# emitted by the per-item cache extension and flags any function whose
# body is a <=3-line `return CONSTANT;` placeholder under an [x] stamp
# (AI-slop "marked done but doesn't actually do anything" anti-pattern).
# Allowlist: /* INTENTIONAL-STUB: <reason> */ on the same line as the
# body-opening `{` (mirrors Check 6's TEST-TAUTOLOGY-OK marker).
# Skip the whole check via SKIP_LINT_STUB_BEHIND_STAMP=1.
#
# Cache freshness: the cache is rebuilt automatically by the PostToolUse
# hook on TODO edits and by scripts/todo-graph/build-and-validate.sh. To
# protect against a stale cache silently passing the lint, this check
# WARNs (without erroring) when the cache mtime is older than the newest
# todo/**/*.md mtime; the contributor sees a clear "rebuild and rerun"
# pointer rather than a false-clean.
if [ "${SKIP_LINT_STUB_BEHIND_STAMP:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) skipped via SKIP_LINT_STUB_BEHIND_STAMP=1"
    WARNINGS=$((WARNINGS + 1))
else
    # Single-spawn helper: probe + staleness + walk in one Python process.
    # Distinct exit codes (per check_stub_behind_stamp.py header):
    #   0 = clean walk        (route findings via error())
    #   2 = cache missing     -> WARN (contributor hasn't built it)
    #   3 = no stamped_items  -> WARN (cache pre-extension; run build). This is
    #       the LEGACY case only: the field absent from every node. A cache
    #       that has the field but an empty population is a producer
    #       regression and reaches the rc 7 gate instead (section 17).
    #   4 = cache stale       -> ERROR (false-clean risk; rebuild required)
    #   5 = cache malformed   -> ERROR (corrupt JSON, a shape violating
    #       scripts/todo-graph/schema/cache.schema.json, or an empty node
    #       array; investigate)
    #   6 = internal failure  -> ERROR
    #   7 = coverage floor breached -> ERROR (the check went blinder)
    #   8 = baseline invalid  -> ERROR (tracked floor file missing/corrupt)
    #   9 = resolver input refused -> ERROR (oversized file, or one rewritten
    #       mid-run so returned coordinates no longer describe it -- NEVER a
    #       coverage finding, NEVER silently downgraded to "unresolved")
    CACHE="$REPO_ROOT/build/todo-cache.json"
    STUB_ERR_FILE="$(mktemp -t lint-stub-stderr.XXXXXX)"
    STUB_OUT_FILE="$(mktemp -t lint-stub-stdout.XXXXXX)"
    # Run helper outside `set -e` failure boundary: nonzero rc on
    # missing/stale/corrupt cache is informational, not a fatal lint
    # error -- the case statement below routes each rc to error()/warn().
    STUB_RC=0
    STUB_LINT_CACHE="$CACHE" STUB_LINT_REPO_ROOT="$REPO_ROOT" \
        python3 "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" \
        >"$STUB_OUT_FILE" 2>"$STUB_ERR_FILE" || STUB_RC=$?
    STUB_OUT="$(cat "$STUB_OUT_FILE")"
    case "$STUB_RC" in
        0)
            while IFS=: read -r f l rest; do
                [ -z "$f" ] && continue
                error "$f" "$l" "$rest"
            done <<< "$STUB_OUT"
            # SURFACE THE COVERAGE LINE. The helper writes it to stderr (stdout
            # is the findings channel), and this branch used to read stdout
            # only -- so a normal lint run published no coverage ratio at all
            # and the whole point of counting the blind spot was invisible to
            # the caller.
            # SEVERITY IS WARN WHILE A GAP STANDS, INFO ONLY AT FULL COVERAGE.
            # The roadmap chose "WARN on the standing gap, ERROR on a
            # REGRESSION"; emitting the gap as INFO made the shipped severity
            # weaker than the committed decision. It stays a WARN (not an
            # ERROR) for the original reason: the gap is pre-existing and
            # erroring would block every commit in the repo until it hit zero.
            STUB_COV="$(grep -m1 'stub-behind-stamp:coverage' "$STUB_ERR_FILE" 2>/dev/null || true)"
            if [ -n "$STUB_COV" ]; then
                STUB_R="${STUB_COV#*resolved=}"; STUB_R="${STUB_R%%/*}"
                STUB_T="${STUB_COV#*resolved=*/}"; STUB_T="${STUB_T%% *}"
                if [ -n "$STUB_R" ] && [ "$STUB_R" = "$STUB_T" ]; then
                    echo -e "${CYAN}info${NC}: Check 7 ${STUB_COV#stub-behind-stamp:}"
                else
                    echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) ${STUB_COV#stub-behind-stamp:coverage } -- refs the check could not examine are NOT findings and NOT clean"
                    WARNINGS=$((WARNINGS + 1))
                fi
            fi
            # A DELIBERATE BYPASS MUST BE SEEN. STUB_LINT_ALLOW_NO_BASELINE=1
            # disables the only coverage floor; announcing it on the helper's
            # stderr while the caller discards stderr is not "visibly reported".
            if grep -q 'baseline floor SKIPPED' "$STUB_ERR_FILE" 2>/dev/null; then
                echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) coverage floor BYPASSED via STUB_LINT_ALLOW_NO_BASELINE=1 -- the resolver could go blind without this run noticing"
                WARNINGS=$((WARNINGS + 1))
            fi
            ;;
        2)
            echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) skipped -- build/todo-cache.json missing; run bash scripts/todo-graph/build-and-validate.sh"
            WARNINGS=$((WARNINGS + 1))
            ;;
        3)
            echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) deferred -- build/todo-cache.json lacks per-item file:symbol map; rebuild with bash scripts/todo-graph/build-and-validate.sh"
            WARNINGS=$((WARNINGS + 1))
            ;;
        4)
            echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) cache stale relative to todo/**/*.md; rebuild with bash scripts/todo-graph/build-and-validate.sh and re-run lint"
            ERRORS=$((ERRORS + 1))
            ;;
        7)
            # rc 7 carries TWO contracts, and the handler must route on the
            # emitted tag rather than assume one. Coverage floor breached:
            # something made the check BLINDER -- an unresolved symbol yields no
            # finding, so losing resolution is indistinguishable from passing.
            # POPULATION moved: the floor is a RATIO, so a shifting denominator
            # changes what passing means. Grepping only for COVERAGE REGRESSION
            # printed an effectively BLANK error on a population change and
            # threw away the helper's actionable message (Codex consistency,
            # section 14).
            if grep -q 'COVERAGE REGRESSION' "$STUB_ERR_FILE" 2>/dev/null; then
                echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) COVERAGE REGRESSION -- $(grep -m1 'COVERAGE REGRESSION' "$STUB_ERR_FILE" 2>/dev/null | sed 's/.*REGRESSION: //' | cut -c1-140)"
            elif grep -q 'POPULATION ' "$STUB_ERR_FILE" 2>/dev/null; then
                echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) $(grep -m1 'POPULATION ' "$STUB_ERR_FILE" 2>/dev/null | sed 's/.*\] //' | cut -c1-200)"
            else
                echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) rc 7 with no recognized diagnostic; see $STUB_ERR_FILE"
            fi
            ERRORS=$((ERRORS + 1))
            ;;
        8)
            # The coverage floor's own bookkeeping file is tracked repo state.
            # Treating it as absent-means-no-floor let a delete silently
            # disable the gate, so an invalid baseline is an ERROR with a
            # named, reported bypass rather than a quiet degrade to warn.
            echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) BASELINE INVALID -- $(grep -m1 'BASELINE INVALID' "$STUB_ERR_FILE" 2>/dev/null | sed 's/.*BASELINE INVALID: //' | cut -c1-140)"
            ERRORS=$((ERRORS + 1))
            ;;
        9)
            # Named case, not the wildcard: a resolver-refused input is
            # infrastructure trouble with a specific, actionable cause
            # (oversized file or a mid-run rewrite), not a generic helper
            # crash -- routing it through `*)` would bury that guidance.
            echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) RESOLVER INPUT REFUSED -- $(grep -m1 'RESOLVER INPUT REFUSED' "$STUB_ERR_FILE" 2>/dev/null | sed 's/.*RESOLVER INPUT REFUSED: //' | cut -c1-140)"
            ERRORS=$((ERRORS + 1))
            ;;
        *)
            echo -e "${RED}error${NC}: Check 7 (stub-behind-stamp) helper exited $STUB_RC: $(head -3 "$STUB_ERR_FILE" 2>/dev/null | tr '\n' ' ')"
            ERRORS=$((ERRORS + 1))
            ;;
    esac
    rm -f "$STUB_ERR_FILE" "$STUB_OUT_FILE"
fi

# ============================================================================
# Check 9: Stamp-region placeholder leak detection (post-2026-04-28)
# ============================================================================
# Recurring failure mode: when writing a Verified/Quality-reviewed stamp
# before the corresponding commit lands, an `<this-commit>` (or `<hash>`,
# `<TBD>`, `<insert-hash>`, etc.) placeholder gets written into the stamp
# and never replaced after the commit. The earlier lint checks did not
# catch it because the placeholder uses ASCII angle brackets and contains
# no Unicode dashes, no bare section signs, no numeric TODO shorthand.
# Hit twice in the 2026-04-28 session (one boot-platform stamp + one
# automation-hardening stamp) before the user noticed. ERRORs on any
# literal placeholder inside `> **Verified:**` / `> **Quality
# reviewed:**` lines anywhere under todo/.
#
# Allowed (explicit "still-TBD" markers): a placeholder is OK when the
# enclosing line explicitly says `TBD:` -- e.g. `(TBD: original review
# commit hash unrecorded; placeholder preserved for git-log audit)`. The
# check matches the literal placeholder pattern and excludes lines that
# contain `TBD:` so deliberate "this slot is intentionally empty" notes
# are not flagged.
if [ "${SKIP_LINT_PLACEHOLDER:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 9 (stamp placeholder) skipped via SKIP_LINT_PLACEHOLDER=1"
    WARNINGS=$((WARNINGS + 1))
else
    # Match any "<lowercase-word>" pattern inside a stamp line. This
    # catches the canonical `<this-commit>` / `<hash>` placeholders
    # plus any free-form variant a future writer invents (e.g.
    # `<this-and-following-commit>`, `<insert-hash>`). False positives
    # avoided by: (a) anchoring on the stamp prefix `> **Verified:**`
    # / `> **Quality reviewed:**`; (b) requiring the bracket content
    # to be lowercase + hyphen only (excludes XML-like `<details>`,
    # markdown comparisons `<value>`, and natural-language `<key>`
    # patterns); (c) excluding lines containing `TBD:`.
    placeholder_hits="$(grep -rnE \
        '> \*\*(Verified|Quality reviewed):\*\*.*<[a-z][a-z0-9_-]+>' \
        todo 2>/dev/null \
        | grep -vE 'TBD:' || true)"
    if [ -n "$placeholder_hits" ]; then
        while IFS= read -r entry; do
            [ -n "$entry" ] || continue
            # grep -rn returns "path:line:content"
            file="${entry%%:*}"
            tail_field="${entry#*:}"
            ln="${tail_field%%:*}"
            content="${tail_field#*:}"
            placeholder="$(printf '%s\n' "$content" | grep -oE '<[a-zA-Z0-9_-]+>' | head -1)"
            echo -e "${RED}error${NC}: $file:$ln: stamp placeholder leak: $placeholder"
            ERRORS=$((ERRORS + 1))
        done <<< "$placeholder_hits"
    fi
fi

# ============================================================================
# Check 10: Stamp-region completeness (TODO-08 stamp-region completeness lints)
# Check 11: OS Comparison cross-table sync (TODO-08 stamp-region completeness lints)
# ============================================================================
# Both checks walk every todo/**/*.md IO table, find [x] rows, and verify
# downstream invariants in the matching `## N.` section + OS Comparison
# row. Sentinel-date scoped: only enforce on sections whose Verified
# stamp date is on/after LINT_LAND_DATE. Older sections are grandfathered
# (existing TODOs ship pre-doctrine and would all WARN otherwise).
#
# Doctrine:
#   Check 10 -- review-todo-section/SKILL.md step 16 (canonical stamp shape)
#   Check 11 -- feedback_os_comparison_update memory (flip OS row to checkmark
#               every shipped IO row)
#
# Skip flags:
#   SKIP_LINT_STAMP_REGION=1   -- Check 10 emits WARN with reason logged
#   SKIP_LINT_OS_COMPARISON=1  -- Check 11 emits WARN with reason logged
if [ "${SKIP_LINT_STAMP_REGION:-}" = "1" ] && [ "${SKIP_LINT_OS_COMPARISON:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 10 + Check 11 both skipped via SKIP_LINT_STAMP_REGION + SKIP_LINT_OS_COMPARISON"
    WARNINGS=$((WARNINGS + 2))
else
    LINT_OUT="$(python3 - <<'PYEOF'
import os
import re
import sys
from pathlib import Path

# Sentinel date: stamp-region completeness lints land date. Sections
# whose `> **Verified:**` carries an earlier date are grandfathered
# (pre-doctrine ship).
LINT_LAND_DATE = "2026-04-29"

SKIP_C10 = os.environ.get("SKIP_LINT_STAMP_REGION", "") == "1"
SKIP_C11 = os.environ.get("SKIP_LINT_OS_COMPARISON", "") == "1"

todo_root = Path("todo")
errors = []
warnings = []

# Parse the IO table for [x] rows: capture section number + deliverable.
IO_ROW_RE = re.compile(
    r"^\|[^|]*\|[^|]*\|\s*§?\s*(\d+)\s*\|([^|]+)\|[^|]*\|\s*\[x\]\s*\|",
    re.MULTILINE,
)

# Section header anchor.
SEC_HDR_RE = re.compile(r"^##\s+(\d+)\.\s+(.+?)$", re.MULTILINE)

# Body end-sentinels: stop scanning a section body at any of these.
# A section body ends at the NEXT level-2 heading of ANY kind. This used to
# be an allowlist of the five known trailing heading names, which silently
# over-captured on the 4 TODO files whose trailing heading was not one of
# them (Bare Metal Testing Plan / Shadow SSDT slot registry / Codex
# Adversarial Review / Completed (Reference)). Sections use `### ` for their
# own sub-headings, so a bare `## ` boundary needs no allowlist and cannot
# go stale as new trailing blocks are added. Same defect class as the
# section_block over-capture fixed in scripts/overnight/section_slice.py.
BODY_END_RE = re.compile(r"^##\s+\S", re.MULTILINE)

# Verified date extraction.
VERIFIED_DATE_RE = re.compile(
    r"^>\s*\*\*Verified:\*\*\s*(\d{4}-\d{2}-\d{2})",
    re.MULTILINE,
)

# Stamp-region required pieces (Check 10).
NOTES_RE = re.compile(r"^>\s*\*\*Notes:\*\*", re.MULTILINE)
TEST_RUNNER_RE = re.compile(r"^>\s*\*\*Test runner:\*\*", re.MULTILINE)
VERIFIED_RE = re.compile(r"^>\s*\*\*Verified:\*\*", re.MULTILINE)
QUALITY_RE = re.compile(r"^>\s*\*\*Quality reviewed:\*\*", re.MULTILINE)
NO_TEST_SURFACE_RE = re.compile(
    r"\*\*Note:\*\*\s*No\s+\w+\s+test\s+surface", re.IGNORECASE
)

OS_SHIPPED_GLYPHS = ("✅", "✓")
OS_NOT_SHIPPED_GLYPHS = ("⬜", "⚠️", "Planned")


def section_body(text, section_num):
    m = re.search(
        r"^##\s+" + str(section_num) + r"\.\s+(.+?)$",
        text, re.MULTILINE,
    )
    if not m:
        return None
    start = m.end()
    end_m = BODY_END_RE.search(text, start)
    end = end_m.start() if end_m else len(text)
    return text[start:end]


def os_table_block(text):
    m = re.search(r"^##\s+OS Comparison\b", text, re.MULTILINE)
    if not m:
        return ""
    start = m.end()
    end_m = re.search(r"^##\s+\w", text[start:], re.MULTILINE)
    end = start + end_m.start() if end_m else len(text)
    return text[start:end]


def os_row_for_section(os_block, section_num, deliverable_substring):
    if not os_block:
        return None
    sec_marker = "§" + str(section_num)
    deliv_lower = deliverable_substring.lower().strip()
    for line in os_block.splitlines():
        if not line.lstrip().startswith("|"):
            continue
        if sec_marker in line:
            return line
    if deliv_lower:
        words = re.findall(r"\b[a-zA-Z][a-zA-Z-]{3,}\b", deliv_lower)
        if words:
            needle = words[0]
            for line in os_block.splitlines():
                if not line.lstrip().startswith("|"):
                    continue
                if needle in line.lower():
                    return line
    return None


for md_path in sorted(todo_root.rglob("*.md")):
    try:
        text = md_path.read_text(encoding="utf-8")
    except Exception:
        continue
    rel = str(md_path)
    os_block = os_table_block(text)
    for m in IO_ROW_RE.finditer(text):
        sec_num = int(m.group(1))
        deliv = m.group(2).strip()
        body = section_body(text, sec_num)
        if body is None:
            continue
        ver_m = VERIFIED_DATE_RE.search(body)
        if not ver_m:
            continue
        ver_date = ver_m.group(1)
        if ver_date < LINT_LAND_DATE:
            continue

        if not SKIP_C10:
            missing_pieces = []
            if not NOTES_RE.search(body):
                missing_pieces.append("Notes")
            has_test_runner = bool(TEST_RUNNER_RE.search(body))
            has_no_test_exemption = bool(NO_TEST_SURFACE_RE.search(body))
            if not has_test_runner and not has_no_test_exemption:
                missing_pieces.append("Test runner")
            if not VERIFIED_RE.search(body):
                missing_pieces.append("Verified")
            if not QUALITY_RE.search(body):
                missing_pieces.append("Quality reviewed")
            if missing_pieces:
                deliv_short = deliv[:40].strip()
                errors.append(
                    f"{rel}: section-{sec_num} ({deliv_short}) stamp-region "
                    f"missing: {', '.join(missing_pieces)} (Check 10)"
                )

        if not SKIP_C11:
            os_row = os_row_for_section(os_block, sec_num, deliv)
            if os_row is None:
                deliv_short = deliv[:40].strip()
                warnings.append(
                    f"{rel}: section-{sec_num} ({deliv_short}) has no "
                    f"matchable OS Comparison row (Check 11)"
                )
            else:
                shipped = any(g in os_row for g in OS_SHIPPED_GLYPHS)
                not_shipped = any(g in os_row for g in OS_NOT_SHIPPED_GLYPHS)
                if not shipped and not_shipped:
                    errors.append(
                        f"{rel}: section-{sec_num} IO row [x] but matching "
                        f"OS Comparison row is not shipped (Check 11)"
                    )

for w in warnings:
    print(f"WARN {w}")
for e in errors:
    print(f"ERROR {e}")
PYEOF
)"
    if [ -n "$LINT_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                ERROR\ *)
                    echo -e "${RED}error${NC}: ${line#ERROR }"
                    ERRORS=$((ERRORS + 1))
                    ;;
                WARN\ *)
                    echo -e "${YELLOW}warn${NC}: ${line#WARN }"
                    WARNINGS=$((WARNINGS + 1))
                    ;;
            esac
        done <<< "$LINT_OUT"
    fi
    if [ "${SKIP_LINT_STAMP_REGION:-}" = "1" ]; then
        echo -e "${YELLOW}warn${NC}: Check 10 (stamp-region) skipped via SKIP_LINT_STAMP_REGION=1"
        WARNINGS=$((WARNINGS + 1))
    fi
    if [ "${SKIP_LINT_OS_COMPARISON:-}" = "1" ]; then
        echo -e "${YELLOW}warn${NC}: Check 11 (OS comparison) skipped via SKIP_LINT_OS_COMPARISON=1"
        WARNINGS=$((WARNINGS + 1))
    fi
fi

# ============================================================================
# Check 12: Codex prompt argument escaping (per the prompt-argument escaping
# doctrine section of 00-infrastructure/TODO-08-automation-hardening). Source
# text is the LOAD-BEARING safety: every documented Codex dispatch in
# .claude/skills/, scripts/, docs/ MUST single-quote its prompt body so bash
# never evaluates the dispatch text. Patterns flagged at WARN:
#   - prompt body in DOUBLE quotes contains $(...) / ${...} / un-escaped
#     `<` that bash would parse as redirect
# WARN-first per the partial-enforcement-heuristics promotion doctrine; can
# graduate to ERROR after FP ratio observed.
if [ "${SKIP_LINT_PROMPT_ESCAPING:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 12 (codex-prompt-escaping) skipped via SKIP_LINT_PROMPT_ESCAPING=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT12_OUT="$(python3 - <<'PYEOF'
import pathlib, re, sys

# Match a Codex dispatch invocation on a single line. Two shapes:
#   1. node ".../codex-companion.mjs" adversarial-review <body>
#   2. bash scripts/codex-dispatch.sh <body>
# For each, capture the body argument (everything after the
# subcommand / wrapper script up to end of line). The body MAY be
# wrapped in single quotes, double quotes, or unquoted (latter is a
# different lint failure -- whitespace splits multi-word body across
# argv).
CODEX_LINE_RE = re.compile(
    r"(?:codex-companion\.mjs[^\n]*?adversarial-review|codex-dispatch\.sh)"
    r"\s+(.*)$"
)

# Inside a captured body, detect:
#   - $( or ${ inside DOUBLE quotes (would expand)
#   - un-escaped < that looks like a redirect AND is OUTSIDE quotes
DANGER_DOUBLE = re.compile(r'"[^"]*?(\$\(|\$\{)[^"]*?"')

ROOTS = ["scripts", ".claude/skills", "docs"]
EXTENSIONS = (".sh", ".md", ".py", ".mjs")
warnings = []

for root in ROOTS:
    p = pathlib.Path(root)
    if not p.exists():
        continue
    for f in p.rglob("*"):
        if not f.is_file() or not f.name.endswith(EXTENSIONS):
            continue
        # Skip the wrapper script + this lint scanner itself + the
        # canonical doctrine doc -- they document the patterns we
        # flag elsewhere.
        rel = str(f)
        if rel.endswith("scripts/codex-dispatch.sh"):
            continue
        if rel.endswith("scripts/lint.sh"):
            continue
        if rel.endswith(".claude/skills/codex-prompt-shape.md"):
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        for ln_no, ln in enumerate(text.splitlines(), 1):
            m = CODEX_LINE_RE.search(ln)
            if not m:
                continue
            body = m.group(1)
            # Skip lines that are clearly prose talking about the
            # pattern (e.g. lint check 12 description, the doctrine
            # rule statement). Heuristic: prose lines start with
            # markdown emphasis markers or are inside a code-fence
            # for documentation. Cheaper heuristic: body must look
            # like an actual bash arg (starts with " or ' or a
            # placeholder like <).
            stripped = body.lstrip()
            if not stripped:
                continue
            if stripped[0] not in ('"', "'", "<"):
                # Prose mention, not an actual invocation. Skip.
                continue
            if stripped[0] == "'":
                # Single-quoted body: bash never EXPANDS it, but a literal INNER
                # apostrophe (e.g. "exec'd", "wrapper's") closes the quote EARLY
                # and exposes the rest of the line to the parser (E1: a following
                # ( or ` then syntax-errors). Flag the premature-close shape --
                # an apostrophe immediately followed by an alnum -- after removing
                # the VALID escape sequence '\''. Conservative: legitimate string
                # concatenation ('a' 'b') and trailing comments are not alnum.
                probe = stripped.replace("'\\''", "")
                first = probe.find("'")
                second = probe.find("'", first + 1)
                if (second != -1 and second + 1 < len(probe)
                        and probe[second + 1].isalnum()):
                    warnings.append(f"{rel}:{ln_no}: Codex prompt single-quoted body has an INNER apostrophe (e.g. \"exec'd\") that closes the quote early -- bash then parses the rest and can syntax-error on a following ( or backtick. Use the heredoc form (PROMPT=$(cat <<'EOF' ... EOF); codex-dispatch.sh \"$PROMPT\") or escape as '\\''. (E1)")
                continue
            if stripped[0] == "<":
                # Unquoted placeholder body. Real invocation would
                # fail to parse -- catch as separate lint at the
                # caller site, not here.
                continue
            # Double-quoted body: scan for $(/${
            if DANGER_DOUBLE.search(body):
                warnings.append(f"{rel}:{ln_no}: Codex prompt in double quotes contains $( or ${{ that bash will expand. Single-quote the prompt: '<body>' instead of \"<body>\".")

for w in warnings:
    print(f"WARN {w}")
PYEOF
)"
    if [ -n "$LINT12_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                WARN\ *)
                    echo -e "${YELLOW}warn${NC}: ${line#WARN }"
                    WARNINGS=$((WARNINGS + 1))
                    ;;
            esac
        done <<< "$LINT12_OUT"
    fi
fi

# ============================================================================
# Check 13: Review/task citations in source comments
# ============================================================================
# C / asm comments must not carry review-archaeology attributions like
# "(Codex H1 ...)", "Codex M3 fix:", "incident YYYY-MM-DD", "see commit
# <hash>", or "per Codex review". Per CLAUDE.md "Don't reference the
# current task, fix, or callers ... those belong in the PR description
# and rot as the codebase evolves."
#
# Files already on the legacy allowlist below WARN (cleanup is in
# progress); any other file ERRORS so new citations cannot land.
#
# Skip the whole check via SKIP_LINT_CITATIONS=1; the skip prints a
# visible WARN line so bypasses are auditable.
if [ "${SKIP_LINT_CITATIONS:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 13 (citation guard) skipped via SKIP_LINT_CITATIONS=1"
    WARNINGS=$((WARNINGS + 1))
elif [ "$#" -eq 0 ]; then
    # Citation patterns. Each pattern's match anchors must align with
    # the actual citation form, not random uses of the underlying
    # word. "Codex" alone is too loose (it matches the OpenAI product
    # name in legitimate API docs); require a citation cue (Mn/Hn/Fn,
    # fix, review, finding, round) within ~40 chars to reduce false
    # positives.  Per-line opt-out: /* CITATION-OK: <reason> */ marker.
    CITATION_RE='(\bCodex\b[^A-Za-z0-9_]{1,40}([HMFCS][0-9]|fix|review|finding|round|adversarial|consistency)|review caught|adversarial review|consistency review|design review|post-impl review|incident 20[0-9]{2}-[0-9]{2}-[0-9]{2}|see commit [0-9a-f]{7,}|per Codex review)'

    # Vendored / third-party paths that we do not own.
    is_citation_vendored() {
        local path="$1"
        case "$path" in
            include/stb_truetype.h) return 0 ;;
            src/libs/*) return 0 ;;
        esac
        return 1
    }

    # Files with pre-existing citations -- WARN-only while the manual
    # cleanup sweep is in progress. Keep sorted for diff hygiene; remove
    # entries as files are scrubbed clean.
    CITATION_LEGACY_FILES=()

    is_citation_legacy() {
        local path="$1"
        local legacy
        for legacy in "${CITATION_LEGACY_FILES[@]}"; do
            [ "$path" = "$legacy" ] && return 0
        done
        return 1
    }

    while IFS=: read -r file linenum rest; do
        [ -z "$file" ] && continue
        relpath="${file#"$REPO_ROOT"/}"
        if is_citation_vendored "$relpath"; then
            continue
        fi
        # Inline allowlist marker: a same-line `CITATION-OK: <reason>`
        # token (with at least one word of reason) opts the line out.
        # Use sparingly -- the marker must justify why the citation is
        # real-and-not-archaeology (e.g. "OpenAI Codex API name",
        # "upstream commit ref needed for repro").
        if echo "$rest" | grep -qE 'CITATION-OK:[[:space:]]*[A-Za-z]'; then
            continue
        fi
        # Restrict matches to lines that look like comments (start with
        # *, //, or /*, or sit inside a known comment block). The grep
        # pattern is loose; this gate prevents false positives on code
        # identifiers that happen to share a token (e.g. a variable
        # literally named codex_review_completed in a hook is a path,
        # not a citation).
        if ! echo "$rest" | grep -qE '^\s*(\*|//|/\*)'; then
            # Allow content after a // on the same line as code.
            if ! echo "$rest" | grep -qE '//.*'"$CITATION_RE"; then
                continue
            fi
        fi
        if is_citation_legacy "$relpath"; then
            warn "$relpath" "$linenum" "review/task citation in comment (legacy; tracked for manual cleanup)"
        else
            error "$relpath" "$linenum" "review/task citation in comment (CLAUDE.md: belongs in PR description, not source; or add /* CITATION-OK: <reason> */ if genuine)"
        fi
    done < <(
        grep -rnHE "$CITATION_RE" \
            --include='*.c' --include='*.h' --include='*.cc' --include='*.cpp' --include='*.hpp' \
            --include='*.asm' --include='*.S' --include='*.s' \
            --exclude-dir='.git' --exclude-dir='build' --exclude-dir='node_modules' \
            "$REPO_ROOT/src" "$REPO_ROOT/include" 2>/dev/null \
        || true
    )
fi

# ============================================================================
# Check 14: Agent tool-allowlist invariant (specialist-agents design 2026-06-20;
#           runner class added 2026-07-02)
# ============================================================================
# Two sanctioned agent classes, each capability-bounded:
#   ANALYST (default): `tools:` must be a subset of the read-only allowlist
#     {Read, Grep, Glob, WebSearch, WebFetch}. Sensors, not actuators.
#   RUNNER (opt-in via `<!-- agent-class: runner -->` on its own line): may add
#     Bash for named idempotent command execution (build/test/lint runs, git/gh
#     READ queries); allowlist is {Bash, Read, Grep, Glob} -- no Edit/Write/
#     Skill/Agent (cannot mutate files or spawn work) and no Web tools (research
#     stays with the analysts). The main session remains the sole
#     editor/committer/Codex-dispatcher and re-verifies runner-reported
#     artifacts (build.log tails, test summaries) itself.
# A missing `tools:` line (the agent would inherit ALL tools) is a violation in
# both classes. Per-file opt-out for a deliberately-broader future agent: an
# HTML comment `<!-- agent-tools-exempt: <reason> -->` on its own line.
# Whole-check skip: SKIP_LINT_AGENT_TOOLS=1 (prints an auditable WARN).
if [ "${SKIP_LINT_AGENT_TOOLS:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 14 (agent tool-allowlist) skipped via SKIP_LINT_AGENT_TOOLS=1"
    WARNINGS=$((WARNINGS + 1))
elif [ "$#" -eq 0 ]; then
    AGENT_TOOLS_OUT=$(python3 - "$REPO_ROOT" <<'PY'
import sys, glob, os, re
root = sys.argv[1]
ANALYST_ALLOWED = {"Read", "Grep", "Glob", "WebSearch", "WebFetch"}
RUNNER_ALLOWED = {"Bash", "Read", "Grep", "Glob"}
# Runner class is roster-gated: the marker ALONE cannot mint a runner. Adding a
# runner is a deliberate act that edits this roster (and gets reviewed).
RUNNER_ROSTER = {"checks-runner.md", "git-historian.md", "gh-query-runner.md"}
viol = []
for path in sorted(glob.glob(os.path.join(root, ".claude/agents/*.md"))):
    rel = os.path.relpath(path, root)
    base = os.path.basename(path)
    with open(path, encoding="utf-8") as f:
        text = f.read()
    if re.search(r'^<!--\s*agent-tools-exempt:\s*\S', text, re.M):
        continue
    has_marker = bool(re.search(r'^<!--\s*agent-class:\s*runner\s*-->', text, re.M))
    is_runner = has_marker and base in RUNNER_ROSTER
    if has_marker and base not in RUNNER_ROSTER:
        viol.append((rel, 1, "agent-class runner marker but not in Check 14 RUNNER_ROSTER (adding a runner requires editing the roster in scripts/lint.sh)"))
    if is_runner and not re.search(r'^##\s*Forbidden\s*--\s*hard rules', text, re.M):
        viol.append((rel, 1, "runner agent missing the required '## Forbidden -- hard rules' section"))
    allowed = RUNNER_ALLOWED if is_runner else ANALYST_ALLOWED
    klass = "runner" if is_runner else "read-only"
    m = re.search(r'^tools:\s*(.*)$', text, re.M)
    if not m:
        viol.append((rel, 1, "no `tools:` allowlist (agent would inherit ALL tools; restrict to read-only set)"))
        continue
    inline = m.group(1).strip().strip('[]')
    toks = []
    if inline:
        toks = [t.strip().strip('"').strip("'") for t in inline.split(',') if t.strip()]
    else:
        for line in text[m.end():].splitlines():
            lm = re.match(r'\s*-\s*(.+?)\s*$', line)
            if lm:
                toks.append(lm.group(1).strip('"').strip("'"))
            elif line.strip() == "":
                continue
            else:
                break
    bad = [t for t in toks if t not in allowed]
    ln = text[:m.start()].count('\n') + 1
    if not toks:
        viol.append((rel, ln, "empty `tools:` allowlist (could not parse a read-only tool set)"))
    elif bad:
        viol.append((rel, ln, "tools not in " + klass + " allowlist " + ("{Bash,Read,Grep,Glob}" if is_runner else "{Read,Grep,Glob,WebSearch,WebFetch}") + ": " + ", ".join(bad)))
for rel, ln, msg in viol:
    print(f"{rel}\t{ln}\t{msg}")
PY
)
    if [ -n "$AGENT_TOOLS_OUT" ]; then
        while IFS=$'\t' read -r at_rel at_ln at_msg; do
            [ -z "$at_rel" ] && continue
            error "$at_rel" "$at_ln" "$at_msg"
        done <<< "$AGENT_TOOLS_OUT"
    fi
fi

# --- Check 15: no &-bundled Codex dispatch in documented examples -----------
# Two codex-dispatch.sh in one command (joined by & / && / ;) records only the
# first review-kind and breaks the section-commit gate. Run each dispatch as its
# own Bash call. WARN-only: catches drift in skills/scripts/docs examples.
if [ "${SKIP_LINT_CODEX_BUNDLE:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 15 (codex-&-bundle) skipped via SKIP_LINT_CODEX_BUNDLE=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT15_OUT="$(python3 - "$REPO_ROOT" <<'PYEOF'
import os, re, sys
root = sys.argv[1]
roots = ["scripts", ".claude/skills", "docs"]
exts = (".sh", ".md", ".py", ".mjs")
self_name = "lint.sh"
two = re.compile(r"codex-(?:bg-)?dispatch\.sh.*(?:&&|&|;).*codex-(?:bg-)?dispatch\.sh")
bg = re.compile(r"codex-(?:bg-)?dispatch\.sh[^\n]*\s&\s*$")
warns = []
for base in roots:
    for dirpath, _dirs, files in os.walk(os.path.join(root, base)):
        for fn in files:
            if not fn.endswith(exts) or fn == self_name:
                continue
            rel = os.path.relpath(os.path.join(dirpath, fn), root)
            # design docs (specs/plans) legitimately discuss the anti-pattern.
            if rel.startswith(("docs/specs/", "docs/plans/")):
                continue
            try:
                lines = open(os.path.join(dirpath, fn), encoding="utf-8",
                             errors="replace").read().splitlines()
            except OSError:
                continue
            for i, ln in enumerate(lines, 1):
                if two.search(ln) or bg.search(ln):
                    warns.append(f"{rel}:{i}: &-bundled codex dispatch -- run each "
                                 f"dispatch as its own Bash call")
for w in warns:
    print(f"WARN {w}")
PYEOF
)"
    if [ -n "$LINT15_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                WARN\ *)
                    echo -e "${YELLOW}warn${NC}: ${line#WARN }"
                    WARNINGS=$((WARNINGS + 1))
                    ;;
            esac
        done <<< "$LINT15_OUT"
    fi
fi

# ============================================================================
# Check 16: Tracked-secret guard (TODO-08 automation hardening)
# ============================================================================
# A live OpenRouter key was once committed inside a tracked *.example
# template (2026-06 incident; scrubbed from history). Two rules over
# TRACKED content, replacing the guard lost with the Conclave removal:
#   - basename `secret` or `secrets.json` must never be tracked
#     (templates ship as *.example with placeholder values);
#   - live API-key material (OpenRouter sk-or-v1-<hex>, OpenAI sk-proj-,
#     Anthropic sk-ant-) must never appear in tracked text. The regexes
#     require real key length, so pattern DOCUMENTATION does not trip them.
# Skip via SKIP_LINT_SECRETS=1 (visible WARN so bypasses stay auditable).
if [ "${SKIP_LINT_SECRETS:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 16 (tracked-secret guard) skipped via SKIP_LINT_SECRETS=1"
    WARNINGS=$((WARNINGS + 1))
else
    # parameter expansion, not a basename subprocess: the per-file spawn cost
    # ~2.5s over ~2000 tracked files on every pre-commit run (perf finding)
    while IFS= read -r sfile; do
        sbase=${sfile##*/}
        if [ "$sbase" = "secret" ] || [ "$sbase" = "secrets.json" ]; then
            echo -e "${RED}error${NC}: $sfile: tracked-secret: basename '$sbase' must not be tracked (gitignore it; ship a *.example template with placeholders)"
            ERRORS=$((ERRORS + 1))
        fi
    done < <(git ls-files)
    # scan BOTH the index (--cached: what a commit would ship, closing the
    # stage-then-clean-worktree bypass) and the worktree (hygiene), dedup by path
    SECRET_RE='sk-or-v1-[a-f0-9]{48,}|sk-proj-[A-Za-z0-9_-]{40,}|sk-ant-[A-Za-z0-9_-]{40,}'
    SECRET_HITS=$( { git grep --cached -l -I -E "$SECRET_RE" -- . 2>/dev/null; \
                     git grep -l -I -E "$SECRET_RE" -- . 2>/dev/null; } | sort -u || true)
    if [ -n "$SECRET_HITS" ]; then
        while IFS= read -r shit; do
            echo -e "${RED}error${NC}: ${shit}: tracked-secret: live API-key pattern in staged/tracked content (rotate the key, scrub the file, gitignore the real secret)"
            ERRORS=$((ERRORS + 1))
        done <<< "$SECRET_HITS"
    fi
fi

# ============================================================================
# Check 17: TODO table column alignment (informational -- WARN only)
# ============================================================================
# Cosmetic-only: renderers ignore source whitespace in tables entirely, so a
# ragged table is never a correctness problem, only harder to skim in a raw
# editor/terminal. scripts/format-md-tables.py already skips prose-heavy
# tables (History/Notes logs) on its own -- see its docstring -- so this
# check only flags the short, genuinely tabular tables worth aligning.
# Skip via SKIP_LINT_TABLE_ALIGN=1.
if [ "${SKIP_LINT_TABLE_ALIGN:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 17 (table-column-align) skipped via SKIP_LINT_TABLE_ALIGN=1"
    WARNINGS=$((WARNINGS + 1))
else
    # One summary line, not one per file -- this is repo-wide-scan cosmetic
    # debt (measured 2026-07-05: 213/251 todo/*.md files), and per-file WARN
    # spam here would drown out every other check on every future commit.
    TABLE_ALIGN_COUNT=$( { python3 "$REPO_ROOT/scripts/format-md-tables.py" --check "$REPO_ROOT/todo" 2>/dev/null || true; } | wc -l | tr -d ' ')
    if [ "${TABLE_ALIGN_COUNT:-0}" -gt 0 ]; then
        # PROMOTED TO ERROR 2026-08-02. It sat as a cosmetic warning and 22
        # files drifted, which is a real daily cost for anyone who READS these
        # files raw -- the rendering is unaffected, the source skim is not.
        # Warnings do not stop drift; the whole corpus was aligned in one pass
        # (whitespace-only inside table rows; the tool refuses prose tables
        # with cells over --max-cell) and gating is what keeps it aligned.
        # One command fixes it, and the message says so.
        echo -e "${RED}error${NC}: Check 17 (table-column-align) $TABLE_ALIGN_COUNT todo/*.md file(s) have unaligned table columns -- fix with: python3 scripts/format-md-tables.py todo/"
        ERRORS=$((ERRORS + 1))
    fi
fi

# ============================================================================
# Check 18: no bespoke session-lifecycle code in the runner control plane
# ============================================================================
# The "structural-wait" apparatus -- a session that exited and relied on a
# bespoke detached watcher to relaunch it -- broke the overnight runner
# repeatedly (cgroup escape, pid-1 watchers, deadlocked flock) before it was
# ripped out 2026-07-11. The ONLY sanctioned relaunch is the *:0/10 systemd
# watchdog timer; reviews are polled IN-SESSION. This check flags any detach
# primitive (setsid / nohup / disown / systemd-run) added to a control-plane
# file (per scripts/overnight/control-plane-manifest.txt) unless it carries a
# `LIFECYCLE-WAIVER:` comment on its line or one of the two lines above -- so
# re-introducing the fragile pattern is a conscious, reviewed act.
# Skip via SKIP_LINT_LIFECYCLE=1 (visible WARN so bypasses stay auditable).
if [ "${SKIP_LINT_LIFECYCLE:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 18 (runner-lifecycle) skipped via SKIP_LINT_LIFECYCLE=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT18_OUT="$(python3 - "$REPO_ROOT" <<'PYEOF'
import os, re, subprocess, sys
root = sys.argv[1]
manifest = os.path.join(root, "scripts/overnight/control-plane-manifest.txt")
prefixes = []
try:
    for ln in open(manifest, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip()
        if ln:
            prefixes.append(ln)
except OSError:
    prefixes = []

def in_cp(rel):
    return any(rel == p or (p.endswith("/") and rel.startswith(p)) for p in prefixes)

# Shell files: the bare detach COMMANDS are real invocations. A trailing colon
# means a label string (DETACH="setsid:$!"), not a command -- exclude it.
sh_prim = re.compile(r"(?<![\w.-])(setsid|nohup|disown|systemd-run)(?![\w.:-])")
# Python/node: match REAL detach signals, not bare words -- so a string token in
# an allowlist (e.g. section_commit_gate's _WRAPPER_TOKENS {"setsid","nohup"})
# is not a false positive, while os.setsid / start_new_session=True /
# preexec_fn / os.fork / a systemd-run argv ARE flagged.
py_prim = re.compile(
    r"os\.setsid|start_new_session\s*=\s*True|preexec_fn|os\.fork\b|"
    r"(?<![\w.-])systemd-run(?![\w.:-])")

def waived_above(lines, i):
    """A LIFECYCLE-WAIVER on the invocation line or anywhere in the contiguous
    comment/blank block immediately above it."""
    if "LIFECYCLE-WAIVER:" in lines[i]:
        return True
    j = i - 1
    while j >= 0:
        s = lines[j].lstrip()
        if s == "" or s.startswith("#") or s.startswith("//"):
            if "LIFECYCLE-WAIVER:" in lines[j]:
                return True
            j -= 1
            continue
        break
    return False

try:
    tracked = subprocess.run(["git", "-C", root, "ls-files"],
                             capture_output=True, text=True).stdout.splitlines()
except Exception:
    tracked = []
errs = []
for rel in tracked:
    if not in_cp(rel):
        continue
    # The tests dir NAMES these primitives to forbid them; the manifest/matcher
    # list them as data. None are invocations.
    if "/tests/" in rel or rel.endswith(("control-plane-manifest.txt",
                                         "control-plane-match.sh")):
        continue
    is_py = rel.endswith((".py", ".mjs", ".js"))
    is_sh = rel.endswith(".sh")
    if not (is_py or is_sh):
        continue
    prim = py_prim if is_py else sh_prim
    try:
        lines = open(os.path.join(root, rel), encoding="utf-8",
                     errors="replace").read().splitlines()
    except OSError:
        continue
    for i, ln in enumerate(lines):
        s = ln.lstrip()
        if s.startswith("#") or s.startswith("//"):
            continue  # comment -- discussion, not an invocation
        m = prim.search(ln)
        if not m or waived_above(lines, i):
            continue
        errs.append(f"{rel}:{i+1}: control-plane '{m.group(0)}' without a "
                    f"LIFECYCLE-WAIVER -- the *:0/10 watchdog is the only "
                    f"sanctioned relaunch; add a waiver comment if this "
                    f"detaches a REVIEW/job (not the session)")
for e in errs:
    print(f"ERROR {e}")
PYEOF
)"
    if [ -n "$LINT18_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                ERROR\ *)
                    echo -e "${RED}error${NC}: ${line#ERROR }"
                    ERRORS=$((ERRORS + 1))
                    ;;
            esac
        done <<< "$LINT18_OUT"
    fi
fi

# ============================================================================
# Check 19: TODO prose shape -- item-length cap and hard-wrapped prose
# ============================================================================
# WHY THIS IS HERE AND NOT ONLY IN A HOOK. Both rules already have PreToolUse
# hooks (todo_item_line_length, todo_wrap_reminder) and both hooks work -- on
# Edit/Write. Neither sees a `python3` heredoc or rewrite script run through
# Bash, which is how the operator AND the unattended runner most often write
# TODO content. Measured 2026-07-28: five over-cap items landed after the
# item-length hook shipped, in exactly the two commits that used the script
# path. lint.sh is the only layer that reaches CI (build.yml, release.yml), so
# without an entry here the rules are invisible to CI entirely.
#
# WARN-only, and one summary line per rule, following Check 17's precedent:
# this is repo-wide legacy debt (measured 2026-07-28: 1,407 over-cap items
# across 141 files; 35 files with hard-wrapped prose), so erroring would fail
# every commit until a sweep lands and per-item WARNs would drown every other
# check. Enforcement on NEW content is scripts/todo-staged-check.py at
# pre-commit, which judges only ADDED lines.
# Skip via SKIP_LINT_TODO_PROSE=1.
if [ "${SKIP_LINT_TODO_PROSE:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 19 (todo-prose) skipped via SKIP_LINT_TODO_PROSE=1"
    WARNINGS=$((WARNINGS + 1))
else
    # The trailing /dev/null is load-bearing, not defensive noise: awk with
    # an EMPTY file-operand list falls back to stdin and blocks there
    # forever. `find` yields nothing whenever this runs against a tree with
    # no todo/ directory -- which is exactly what the tooling suite's
    # scratch-repo fixtures do -- so without it a lint invocation hangs
    # indefinitely instead of reporting. Measured 2026-07-30: an 18-minute
    # stall in scripts/test-tooling.sh with awk parked in
    # unix_stream_read_generic, which in an unattended run is a hang with
    # no verdict rather than a slow gate. /dev/null contributes no lines,
    # so the counts are unchanged.
    LINT19_ITEMS=$(awk '/^ *- \[[ x\/]\]/ && length>250 {c++} END{print c+0}' \
        $(find "$REPO_ROOT/todo" -name '*.md' 2>/dev/null) /dev/null 2>/dev/null || echo 0)
    LINT19_FILES=$(awk '/^ *- \[[ x\/]\]/ && length>250 {print FILENAME}' \
        $(find "$REPO_ROOT/todo" -name '*.md' 2>/dev/null) /dev/null 2>/dev/null | sort -u | wc -l | tr -d ' ')
    if [ "${LINT19_ITEMS:-0}" -gt 0 ]; then
        echo -e "${YELLOW}warn${NC}: Check 19 (todo-item-length) $LINT19_ITEMS checklist item(s) over the 250-char cap across $LINT19_FILES file(s) (legacy debt; NEW ones are blocked at commit by scripts/todo-staged-check.py)"
        WARNINGS=$((WARNINGS + 1))
    fi
    LINT19_WRAP=$( { python3 "$REPO_ROOT/scripts/todo-reflow.py" --check \
        $(find "$REPO_ROOT/todo" -name '*.md' 2>/dev/null) 2>/dev/null || true; } \
        | grep -c 'would reflow' || true)
    if [ "${LINT19_WRAP:-0}" -gt 0 ]; then
        echo -e "${YELLOW}warn${NC}: Check 19 (todo-hard-wrap) $LINT19_WRAP todo/*.md file(s) contain hard-wrapped prose (one paragraph per line is the convention; repair with: python3 scripts/todo-reflow.py --write <file>)"
        WARNINGS=$((WARNINGS + 1))
    fi
fi

# ============================================================================
# Check 20: documented `bash <a python file>` invocations
# ============================================================================
# A file with a `#!/usr/bin/env python3` shebang is NOT runnable by bash: bash
# reads the docstring as code and dies with "syntax error near unexpected
# token", exit 2. Documentation that tells the agent to run one that way is a
# defect, and a SILENT one where the caller reads the exit code -- the
# convergence gate's contract is exit 1 = CONVERGED / exit 0 = redispatch, so a
# bash-induced exit 2 FAILS OPEN and silently pays for a Codex round the gate
# would have suppressed.
#
# This recurred five times before it was fixed at source (live-gotchas entries
# 2026-07-15, 07-17, 07-25, plus two live occurrences on 2026-07-28), because a
# gotcha note expires and the wrong doc does not. Hence a lint check rather
# than a sixth note.
#
# WARN-first per the partial-enforcement promotion doctrine.
if [ "${SKIP_LINT_BASH_PY:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 20 (bash-on-python) skipped via SKIP_LINT_BASH_PY=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT20_OUT="$(python3 - <<'PYEOF'
import pathlib, re

# `bash <path>.py` / `sh <path>.py`, allowing the line-wrap the skills use
# (`bash\n  .claude/hooks/x.py`) and an optional opening backtick or quote.
BASH_PY = re.compile(
    r"(?<![.\w])(?:bash|sh)\s+[`\"']?((?:\S*/)?[A-Za-z0-9_./-]+\.py)\b")

ROOTS = [".claude", "scripts", "docs"]
out = []
for root in ROOTS:
    p = pathlib.Path(root)
    if not p.exists():
        continue
    for f in p.rglob("*"):
        if not f.is_file() or f.suffix not in (".md", ".sh", ".py", ".mjs"):
            continue
        rel = str(f)
        if rel.endswith("scripts/lint.sh"):        # this scanner documents it
            continue
        if "/state/" in rel:                        # live-gotchas records the bug
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        # Join wrapped lines so `bash\n  path.py` is caught, keeping line nos.
        lines = text.splitlines()
        for i, ln in enumerate(lines, 1):
            probe = ln
            if ln.rstrip().endswith(("bash", "sh")) and i < len(lines):
                probe = ln.rstrip() + " " + lines[i].strip()
            m = BASH_PY.search(probe)
            if not m:
                continue
            target = pathlib.Path(m.group(1).lstrip("`\"'"))
            # Only a REAL python-shebang file in this repo is a defect; a
            # generic `bash foo.py` in prose about some other tree is not.
            cand = pathlib.Path(m.group(1))
            if not cand.exists():
                continue
            try:
                first = cand.read_text(encoding="utf-8", errors="replace").split("\n", 1)[0]
            except Exception:
                continue
            if "python" not in first:
                continue
            out.append(f"WARN {rel}:{i}: documents `bash {target}`, but that file "
                       f"has a python shebang -- bash exits 2 on it. The exit code "
                       f"is load-bearing for gate callers (2 fails OPEN), so use "
                       f"`python3 {target}`.")
for line in out:
    print(line)
PYEOF
)"
    if [ -n "$LINT20_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                WARN\ *)
                    echo -e "${YELLOW}warn${NC}: ${line#WARN }"
                    WARNINGS=$((WARNINGS + 1))
                    ;;
            esac
        done <<< "$LINT20_OUT"
    fi
fi

# ============================================================================
# Check 21: no $(info) in the root Makefile -- parse-time stdout pollution
# ============================================================================
# $(info) writes to make's STDOUT and, at parse time, fires on EVERY make
# invocation -- including the `print-abi-config` / `print-abi-cppflags` /
# `print-user-cflags` query targets that scripts/gen-user-abi.py and
# scripts/test-tooling.sh read as a machine-readable one-record-per-line
# channel. A parse-time $(info) therefore lands INSIDE the flag vector, and
# gen-user-abi.py hands the diagnostic to clang as a filename:
#   clang-19: error: no such file or directory: '[SIGN] cert fingerprint
#   changed (desired 5f8dddb8..., recorded ); invalidating stamp'
# That failed build/.abi-check.stamp and reddened the CI release-flavor build
# on 2026-07-30 (run 30516845328) while every local build stayed green,
# because the local tree's recorded fingerprint matched and the branch never
# fired. The consumer cannot defend against this -- a flag vector is
# free-form text, so there is no shape a diagnostic could be rejected by.
#
# $(warning) writes to stderr, keeps the message visible in the build log,
# and cannot reach the query channel. So the rule is absolute for this file:
# parse-time diagnostics use $(warning), never $(info). ERROR, not WARN --
# the failure mode is a red CI build that no local gate reproduces.
# Both make delimiters and any function whitespace are covered, and the paths
# are anchored at $REPO_ROOT. A grep for the literal `$(info ` in a
# cwd-relative `Makefile` is trivially bypassed by syntax that pollutes stdout
# identically: GNU make treats `${info x}` exactly like `$(info x)` and accepts
# a TAB after the function name (probed against GNU make 4.4 -- `${info
# BRACE_POLLUTION}` and `$(info<TAB>TAB_POLLUTION)` both print), and lint.sh
# never cd's, so a relative operand silently skipped the whole check whenever
# lint ran from anywhere but the repo root.
if [ "${SKIP_LINT_MAKE_INFO:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 21 (Makefile \$(info)) skipped via SKIP_LINT_MAKE_INFO=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT21_FILES=()
    [ -f "$REPO_ROOT/Makefile" ] && LINT21_FILES+=("$REPO_ROOT/Makefile")
    while IFS= read -r _l21f; do
        [ -n "$_l21f" ] && LINT21_FILES+=("$_l21f")
    done < <(find "$REPO_ROOT" -maxdepth 2 \( -name '*.mk' -o -name 'Makefile.*' \) \
                 -not -path "$REPO_ROOT/build/*" -not -path "$REPO_ROOT/.git/*" \
                 2>/dev/null | sort)
    for _l21f in "${LINT21_FILES[@]}"; do
        while IFS=: read -r lno _rest; do
            [ -n "$lno" ] || continue
            # A comment line (first non-whitespace char is `#`) mentioning the
            # syntax in prose -- like the ones directly above this very block
            # -- is not a live parse-time call; only a code line pollutes
            # stdout when make evaluates it.
            _l21_line="$(sed -n "${lno}p" "$_l21f")"
            case "$_l21_line" in
                [[:space:]]*'#'*|'#'*) continue ;;
            esac
            echo -e "${RED}error${NC}: ${_l21f#"$REPO_ROOT"/}:$lno: \$(info ...) / \${info ...} writes to make's stdout and pollutes the machine-readable print-abi-config / print-abi-cppflags / print-user-cflags query channel (gen-user-abi.py feeds it to clang as a filename). Use \$(warning ...) -- stderr -- instead."
            ERRORS=$((ERRORS + 1))
        done < <(grep -nE '\$[({]info([[:space:]]|[)}])' "$_l21f" || true)
    done
fi

# ============================================================================
# Check 22: TODO `## N.` section bodies must run in numeric order
# ============================================================================
# Sections were being filed dependency-adjacent -- a section discovered while
# reviewing an earlier one was inserted directly after its parent and given the
# next free number -- so TODO-04's bodies ran ... 28, 32, 29, 30, 31, 35, 39,
# 33, 36, 37, 34, 38. Nothing BROKE (the Implementation Order table stayed
# right, and section_slice.py matches by number rather than position), which is
# exactly why it went unnoticed: the only cost is a reader scrolling for a
# section, walking straight past it, and concluding it is missing. That
# happened, and it cost real operator time.
#
# TODO-06 showed the worse form: sections 12 and 13 had been appended AFTER the
# file's OS Comparison / Unit Tests / Verification / History blocks.
#
# The convention: a new section takes the next free number AND its body goes
# LAST. Repair is one command and is a proven pure move (the tool refuses any
# rewrite that is not a reordering of whole blocks).
#
# ERROR, not warn: the debt was fully cleared 2026-07-30, so there is no legacy
# backlog to grandfather and any new hit is a fresh regression.
if [ "${SKIP_LINT_SECTION_ORDER:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 22 (todo-section-order) skipped via SKIP_LINT_SECTION_ORDER=1"
    WARNINGS=$((WARNINGS + 1))
elif [ -f "$REPO_ROOT/scripts/todo-section-order.py" ]; then
    LINT22_OUT="$(cd "$REPO_ROOT" && python3 scripts/todo-section-order.py --check 2>/dev/null || true)"
    if [ -n "$LINT22_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            echo -e "${RED}error${NC}: Check 22 (todo-section-order) $line -- repair with: python3 scripts/todo-section-order.py --fix"
            ERRORS=$((ERRORS + 1))
        done <<< "$LINT22_OUT"
    fi
fi

# Check 24: reachability -- can a future pass still SEE this item?
# ============================================================================
# One question, replacing three partial views (orphan-check, validate's
# dangling/orphan rows, and the classifier they police). Split by severity
# because the two classes differ in kind, not degree:
#
#   open-in-done      ERROR. A bare `- [ ]` in a section the ORACLE calls DONE
#                     and which carries no Deferred stamp is unreachable
#                     outright -- nothing will ever look at it again. Currently
#                     ZERO, so gating costs nothing and keeps it that way.
#   open-in-deferred  WARN. 333 exist (2026-08-02). The section is parked with
#                     a recorded blocker, so the work is not lost, but a bare
#                     `- [ ]` there is seen by NOTHING: the oracle skips DONE
#                     sections, orphan-check exempts Deferred ones by design,
#                     and stranded_deferrals tracks `[/]` ITEMS rather than
#                     items inside a Deferred SECTION. The repair is per-item
#                     (`- [/]` naming the blocker), so this cannot be a blocking
#                     gate until the backlog is triaged -- blocking on 333
#                     pre-existing findings would just teach everyone the
#                     opt-out.
if [ -f "$REPO_ROOT/scripts/todo-reachability.py" ]; then
    # NO `|| echo '{}'` HERE. The tool exits 1 when findings EXIST, so that
    # fallback fired precisely when there was something to report, appending
    # `{}` to valid JSON and breaking the parse -- the check then silently
    # emitted nothing (2026-08-02). Capture the output and ignore the status.
    # DISTINGUISH THE VERDICT CODES FROM THE INFRASTRUCTURE CODE. The tool
    # exits 0 (clean) or 1 (findings) as verdicts, and 2 when the cache exists
    # but cannot be trusted. Blanket-ignoring the status turned that refusal
    # into a clean result: stderr went to /dev/null, `|| true` erased the code,
    # and an empty stdout became `{}` -- so STALE, malformed, oversized,
    # unreadable and timeout ALL reported zero findings on the every-commit
    # path, silently disabling this gate (Codex adversarial, section 19,
    # [high]). Routing the reader fail-closed is worthless while its only
    # caller fails open.
    # `&& ... || ...` rather than a bare assignment: this file runs under
    # `set -euo pipefail`, where a command substitution that exits non-zero
    # kills the script -- and rc 1 (findings exist) is the NORMAL case. The
    # original `|| true` was shielding exactly that; it is replaced here, not
    # dropped, because the status still has to be READ.
    LINT24_ERRTXT="$(mktemp)"
    LINT24_RC=0
    LINT24_OUT="$(cd "$REPO_ROOT" && timeout 600 python3 scripts/todo-reachability.py --json 2>"$LINT24_ERRTXT")" \
        || LINT24_RC=$?
    # THE EXIT CODE ALONE IS NOT ENOUGH. Python exits 1 for an UNCAUGHT
    # EXCEPTION as well as for this tool's findings verdict, so accepting every
    # rc 1 and letting malformed or empty stdout fall through to `{}` reports a
    # CRASH as zero findings -- the gate stays fail-open for the most likely
    # case (reproduced with an invalid-UTF-8 TODO: rc 1, empty stdout; Codex
    # adversarial round 2, [high]). The verdict is the PAIR (code, envelope):
    # rc 0 must carry an empty object, rc 1 a non-empty one, anything else is
    # infrastructure.
    LINT24_SHAPE="$(printf '%s' "$LINT24_OUT" | python3 -c "
import json,sys
raw = sys.stdin.read()
try:
    d = json.loads(raw)
except Exception as exc:
    print('stdout is not valid JSON'); raise SystemExit(0)
if not isinstance(d, dict):
    print('stdout is ' + type(d).__name__ + ', expected an object'); raise SystemExit(0)
print('EMPTY' if not d else 'NONEMPTY')" 2>/dev/null || echo 'could not parse stdout')"
    LINT24_BAD=''
    case "$LINT24_RC:$LINT24_SHAPE" in
        0:EMPTY|1:NONEMPTY) ;;                       # the two honest verdicts
        0:NONEMPTY) LINT24_BAD="exited 0 (clean) but reported findings" ;;
        1:EMPTY)    LINT24_BAD="exited 1 (findings) but reported none -- an uncaught exception exits 1 too" ;;
        *)          LINT24_BAD="rc $LINT24_RC, $LINT24_SHAPE" ;;
    esac
    if [ -n "$LINT24_BAD" ]; then
        # Surface the tool's OWN diagnostic, which names WHICH shared-validator
        # rule refused, rather than a generic message.
        error "scripts/todo-reachability.py" "0" \
            "reachability audit could not run ($LINT24_BAD): $(head -c 300 "$LINT24_ERRTXT" 2>/dev/null | tr '\n' ' ')"
        LINT24_OUT=''
    fi
    rm -f "$LINT24_ERRTXT"
    LINT24_ERR="$(printf '%s' "$LINT24_OUT" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
n=sum(1 for v in d.values() for k,_,_ in v if k=='open-in-done')
print(n)" 2>/dev/null || echo 0)"
    # BOTH numbers. The record is per SECTION and carries its own item count in
    # the message; reporting the record count while calling it "items" understated
    # the backlog 4.8x (333 sections vs 1595 items, measured 2026-08-05) and that
    # wrong figure had propagated into the capture files and the doctrine.
    LINT24_WARN="$(printf '%s' "$LINT24_OUT" | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
n=sum(1 for v in d.values() for k,_,_ in v if k=='open-in-deferred')
print(n)" 2>/dev/null || echo 0)"
    LINT24_ITEMS="$(printf '%s' "$LINT24_OUT" | python3 -c "
import json,re,sys
try: d=json.load(sys.stdin)
except Exception: d={}
n=0
for v in d.values():
    for k,_,msg in v:
        if k=='open-in-deferred':
            m=re.match(r'^(\\d+) open', msg)
            n+= int(m.group(1)) if m else 1
print(n)" 2>/dev/null || echo 0)"
    if [ "${LINT24_ERR:-0}" -gt 0 ] 2>/dev/null; then
        echo -e "${RED}error${NC}: Check 24 (reachability) ${LINT24_ERR} open item(s) in DONE sections are unreachable -- nothing will revisit them. Run: python3 scripts/todo-reachability.py"
        ERRORS=$((ERRORS + LINT24_ERR))
    fi
    if [ "${LINT24_WARN:-0}" -gt 0 ] 2>/dev/null; then
        echo -e "${YELLOW}warn${NC}: Check 24 (reachability) ${LINT24_ITEMS} open \`- [ ]\` item(s) across ${LINT24_WARN} Deferred section(s); repair shape is \`- [/]\` naming the blocker"
        WARNINGS=$((WARNINGS + 1))
    fi
fi

# Check 22b: a `## N.` body must not sit AFTER the closing matter.
# Check 22 tests that section numbers ASCEND; it is silent when sections are
# appended past `## OS Comparison` / `## Unit Tests` / `## Verification` /
# `## History`, because those sections are still numerically ordered among
# themselves. Measured 2026-08-01: TODO-04 carried sections 53-59 after all
# three closing blocks and lint reported 0 errors -- the exact TODO-06 shape
# CLAUDE.md already names, invisible to the checker that exists to catch it.
# PROMOTED TO ERROR 2026-08-02, once the tree was clean. It shipped as a
# warning because promoting a pre-existing violation would have wedged the
# commit of the run that was mid-section in the offending file; all three
# affected files (TODO-04 usermode x10, TODO-07 lsp-mcp x1, TODO-04
# system-logging x1) were then repaired by `--fix` as verified pure block moves
# -- identical line count, byte count and sorted content -- and the tree-wide
# scan returns clean. The 60-section hard cap prevents the condition from being
# reintroduced at scale, so blocking is now safe rather than merely correct.
if [ -f "$REPO_ROOT/scripts/todo-section-order.py" ]; then
    LINT22B_OUT="$(cd "$REPO_ROOT" && python3 scripts/todo-section-order.py --check-placement 2>/dev/null || true)"
    if [ -n "$LINT22B_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] && echo -e "${RED}error${NC}: Check 22b (todo-section-placement) $line"
            ERRORS=$((ERRORS + 1))
        done <<< "$LINT22B_OUT"
    fi
fi

# ============================================================================
# Check 23: no open `- [ ]` item orphaned behind a stamped (DONE) TODO section
# ============================================================================
# The triage oracle classifies a section from its Implementation Order row +
# stamps and never reads the body, so a `- [ ]` appended to a stamped section is
# invisible to every later pass: the runner never routes back, the item cannot
# hold fixpoint open, and the run reports the repo complete over it. The
# overnight run was actively creating these by following the completion-first
# rule ("file adjacent work in the owning section") into closed sections.
#
# PRECISION: sections carrying a `> **Deferred:**` stamp are EXEMPT -- their
# items are deliberately parked with a re-open path (owner-side
# stranded_deferrals sweep + the P6.3 fixpoint gate). The naive rule without
# that exemption over-matched 14.6x (1,712 hits, 1,595 of them parked).
#
# ERROR, not warn: the 2026-07-31 cohort (117 items / 59 sections) was fully
# triaged and cleared the same day, so there is no legacy debt and any hit is a
# fresh regression. Repair guidance is printed by the checker itself.
if [ "${SKIP_LINT_ORPHAN_ITEMS:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 23 (todo-orphan-items) skipped via SKIP_LINT_ORPHAN_ITEMS=1"
    WARNINGS=$((WARNINGS + 1))
elif [ -f "$REPO_ROOT/scripts/todo-orphan-check.py" ]; then
    LINT23_OUT="$(cd "$REPO_ROOT" && python3 scripts/todo-orphan-check.py 2>/dev/null || true)"
    if [ -n "$LINT23_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in
                "    "*) ;;   # item detail lines -- show but do not double-count
                *)
                    echo -e "${RED}error${NC}: Check 23 (todo-orphan-items) $line"
                    ERRORS=$((ERRORS + 1))
                    ;;
            esac
        done <<< "$LINT23_OUT"
    fi
fi

# ============================================================================
# Check 24: every UTEST_RSN_* reason must be measured by UTEST_REASON_REFUSAL
# ============================================================================
# The tree is not documentation. It feeds UTEST_REASON_MAX, which every record
# format subtracts from UTEST_RECORD_LINE_MAX to derive UTEST_MAX_BINARY_NAME
# -- so a reason string that is defined but never added to the tree understates
# the maximum, leaves the derived name bound too generous, and sends the record
# formatters to their truncation fallback on inputs the build proved could not
# overflow. Nothing failed; only a comment guarded the relationship.
#
# ERROR, not warn: the set is equal today, so any hit is a fresh regression --
# and the whole point is to fail the build the day a reason is added without
# being measured, which is precisely when it is cheap to fix.
if [ "${SKIP_LINT_UTEST_REASONS:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 24 (utest-reason tree) skipped via SKIP_LINT_UTEST_REASONS=1"
    WARNINGS=$((WARNINGS + 1))
elif [ -f "$REPO_ROOT/scripts/utest-reason-lint.py" ]; then
    # `|| true` INSIDE the substitution, matching Check 22. This script runs
    # under `set -euo pipefail`, so a bare failing command substitution in an
    # assignment exits the whole lint immediately -- the checker's own
    # diagnostic would be discarded and the summary never printed, leaving a
    # blocked commit with nothing naming the unmeasured reason. Both streams
    # are captured because violations go to stdout and extraction failures to
    # stderr; any output at all is a problem to report.
    LINT24_OUT="$(cd "$REPO_ROOT" && python3 scripts/utest-reason-lint.py --check 2>&1 || true)"
    if [ -n "$LINT24_OUT" ]; then
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            echo -e "${RED}error${NC}: Check 24 (utest-reason tree) $line"
            ERRORS=$((ERRORS + 1))
        done <<< "$LINT24_OUT"
    fi
fi


# ============================================================================
# Check 25: the lint consumer must DELEGATE to the shared resolution rule
# ============================================================================
# section 18. The identity gate walks corpus_resolution_snapshot.py and
# adjudicates NOTHING about scripts/lint/check_stub_behind_stamp.py, even though
# section 14 unified the two behind one `resolve_ref` precisely so a fallback
# added to one could not diverge from the other. This is the invariant standing
# in for a full consumer differential: the consumer routes every symbol
# occurrence through the shared rule exactly once and propagates its verdicts
# verbatim.
#
# GATED ON THE FILES THAT CAN BREAK IT. The semantic half drives a real corpus
# walk (~2s), and Check 7 already pays for one; charging every commit in the
# repo a second walk to re-prove a property only these files can affect is not
# a trade worth making. SKIP via SKIP_LINT_CONSUMER_DELEGATION=1; force a run
# with LINT_CONSUMER_DELEGATION_ALWAYS=1.
if [ "${SKIP_LINT_CONSUMER_DELEGATION:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 25 (consumer-delegation) skipped via SKIP_LINT_CONSUMER_DELEGATION=1"
    WARNINGS=$((WARNINGS + 1))
else
    LINT25_TOUCHED=0
    if [ "${LINT_CONSUMER_DELEGATION_ALWAYS:-}" = "1" ]; then
        LINT25_TOUCHED=1
    else
        LINT25_DIFF="$( { git -C "$REPO_ROOT" diff --cached --name-only 2>/dev/null; \
                          git -C "$REPO_ROOT" diff --name-only 2>/dev/null; } || true)"
        # A CLEAN TREE MEANS CI, AND CI IS THE POINT. GitHub Actions checks out
        # the committed revision with an empty index and worktree, so both
        # diffs above are empty and the trigger was ALWAYS false there: the one
        # check that can detect the lint consumer diverging from the shared
        # resolution rule never ran in the environment it exists to protect,
        # and the identity gate explicitly does not cover the consumer (Codex
        # adversarial, section 18). Deriving paths from a commit RANGE was the
        # alternative and is worse: a PR spans several commits and the range is
        # a second thing to get wrong. On a clean tree just run it -- ~7s, and
        # only where there is a committed state to adjudicate.
        # ...but ONLY where this check is meaningful. `scripts/lint.sh` is run
        # against scratch repos by the tooling suite, which have neither the
        # consumer nor a todo/ corpus -- and since Check 25 fails CLOSED, a
        # clean-tree trigger there turned every such sub-test red (measured: 4
        # tooling failures). Absence of the subject is not a finding about it.
        if [ -z "$(printf '%s' "$LINT25_DIFF" | tr -d '[:space:]')" ] \
           && [ -f "$REPO_ROOT/scripts/lint/check_stub_behind_stamp.py" ] \
           && [ -d "$REPO_ROOT/todo" ]; then
            LINT25_TOUCHED=1
        fi
        case "$LINT25_DIFF" in
            *scripts/lint/check_stub_behind_stamp.py*|\
            *scripts/lint/check_consumer_delegation.py*|\
            *scripts/todo-graph/ref_resolution.py*|\
            *scripts/todo-graph/snapshot_protocol.py*|\
            *scripts/todo-graph/snapshot_protocol.json*) LINT25_TOUCHED=1 ;;
        esac
    fi
    if [ "$LINT25_TOUCHED" -eq 1 ]; then
        LINT25_OUT="$(mktemp -t lint-delegation.XXXXXX)"
        LINT25_RC=0
        # BUILD THE CACHE IF IT IS NOT THERE. `build/todo-cache.json` is
        # gitignored, so it is ABSENT in a fresh checkout -- which is exactly
        # what CI is. Without this the check returned rc 3 (infrastructure),
        # the branch below turned that into a WARNING, and lint passed: the
        # only gate that can detect the consumer diverging from the shared
        # resolution rule was fail-open in the one environment that matters,
        # and the identity gate explicitly does not cover the consumer (Codex
        # adversarial, section 18).
        if [ ! -f "$REPO_ROOT/build/todo-cache.json" ]; then
            python3 "$REPO_ROOT/scripts/todo-graph/build.py" --quiet \
                --output "$REPO_ROOT/build/todo-cache.json" >/dev/null 2>&1 || true
        fi
        STUB_LINT_CACHE="$REPO_ROOT/build/todo-cache.json" \
            python3 "$REPO_ROOT/scripts/lint/check_consumer_delegation.py" \
            >"$LINT25_OUT" 2>&1 || LINT25_RC=$?
        case "$LINT25_RC" in
            0) echo -e "${CYAN}info${NC}: Check 25 (consumer-delegation) the lint consumer delegates every symbol occurrence to the shared rule" ;;
            1)
                # A REAL VIOLATION: the two gates have diverged.
                while IFS= read -r _l25; do
                    [ -z "$_l25" ] && continue
                    echo -e "${RED}error${NC}: Check 25 (consumer-delegation) ${_l25#  VIOLATION }"
                done < <(grep '  VIOLATION ' "$LINT25_OUT" || true)
                ERRORS=$((ERRORS + 1))
                ;;
            *)
                # FAIL CLOSED. This branch runs ONLY because a file that can
                # break the invariant was changed, and the cache is built above
                # if it was missing -- so "the check could not run" here is not
                # a fresh-clone inconvenience, it is the gate being unable to
                # adjudicate the very change that triggered it. Warning was
                # fail-open in a fresh checkout, which is what CI always is.
                echo -e "${RED}error${NC}: Check 25 (consumer-delegation) could not run (rc=$LINT25_RC) while a file that can break the invariant was changed: $(tail -1 "$LINT25_OUT")"
                ERRORS=$((ERRORS + 1))
                ;;
        esac
        rm -f "$LINT25_OUT"
    fi
fi

# ============================================================================
# Check 26: the resolver must DECLARE the bucket set it can emit
# ============================================================================
# section 20. `identity-gate.sh` adjudicates bucket RETIREMENT, and its proof
# that a bucket is no longer produced is now `ref_resolution.EMITTED_BUCKETS`
# rather than a source-text search for the quoted literal (which an indexed or
# concatenated emission defeats). That proof is only as good as the declaration,
# so the declaration's shape is checked here on every commit that can move it.
#
# UNCONDITIONAL WHERE THE SUBJECT EXISTS, unlike Check 25. This one is a pure
# AST read of two files -- no corpus walk, no cache, ~30ms measured -- so there
# is no cost argument for gating it on a diff, and a gate that only runs when
# someone remembered to touch the right file is how the contract rots. Absent
# subject (the tooling suite's scratch repos) is not a finding, exactly as
# Check 25 learned. SKIP via SKIP_LINT_BUCKET_EMISSION=1.
if [ "${SKIP_LINT_BUCKET_EMISSION:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 26 (bucket-emission) skipped via SKIP_LINT_BUCKET_EMISSION=1"
    WARNINGS=$((WARNINGS + 1))
elif [ -f "$REPO_ROOT/scripts/lint/check_bucket_emission.py" ] \
     && [ -f "$REPO_ROOT/scripts/todo-graph/ref_resolution.py" ] \
     && [ -f "$REPO_ROOT/scripts/todo-graph/snapshot_protocol.json" ]; then
    LINT26_OUT="$(mktemp -t lint-bucket-emission.XXXXXX)"
    LINT26_RC=0
    python3 "$REPO_ROOT/scripts/lint/check_bucket_emission.py" \
        --emitter "$REPO_ROOT/scripts/todo-graph/ref_resolution.py" \
        --protocol "$REPO_ROOT/scripts/todo-graph/snapshot_protocol.json" \
        >"$LINT26_OUT" 2>&1 || LINT26_RC=$?
    case "$LINT26_RC" in
        0) echo -e "${CYAN}info${NC}: Check 26 (bucket-emission) the resolver declares its emitted bucket set" ;;
        *)
            # FAIL CLOSED on both violation and infrastructure. A contract that
            # could not be read proves nothing about what the resolver emits,
            # and the retirement gate consults it -- treating "could not run" as
            # a pass is the same inversion that once let an unreadable emitter
            # approve a retirement (identity-gate fixture 22af).
            while IFS= read -r _l26; do
                [ -z "$_l26" ] && continue
                echo -e "${RED}error${NC}: Check 26 (bucket-emission) ${_l26#\[check_bucket_emission\] }"
            done < <(grep '^\[check_bucket_emission\]' "$LINT26_OUT" || tail -1 "$LINT26_OUT")
            ERRORS=$((ERRORS + 1))
            ;;
    esac
    rm -f "$LINT26_OUT"
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"
TOTAL=$((ERRORS + WARNINGS))
if [ "$ERRORS" -eq 0 ] && [ "$WARNINGS" -eq 0 ]; then
    echo -e "  ${CYAN}LINT CLEAN${NC} -- no violations found"
elif [ "$ERRORS" -eq 0 ]; then
    echo -e "  ${YELLOW}$WARNINGS warning(s)${NC}, 0 errors"
else
    echo -e "  ${RED}$ERRORS error(s)${NC}, ${YELLOW}$WARNINGS warning(s)${NC}"
fi
echo -e "  ${DIM}${#FILES[@]} files checked${NC}"
echo -e "${CYAN}══════════════════════════════════════════════════${NC}"

if [ "$ERRORS" -gt 0 ]; then
    exit 1
else
    exit 0
fi
