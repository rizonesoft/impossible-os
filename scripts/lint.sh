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
    BARE_SECTION_RE='§[0-9]'
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
# Check 7: Stub-behind-stamp detection (TODO-08 #12)
# ============================================================================
# Designed to read build/todo-cache.json (TODO-06 #2 derived cache) for the
# stamped-item -> file:symbol map and verify each [x]-stamped function has
# a non-stub body. The TODO-06 #2 cache extension that exposes per-item
# file:symbol mapping has not shipped yet, so this check skip-with-warns
# until the cache contract is updated. Allowlist marker (when active):
# /* INTENTIONAL-STUB: <reason> */ on the same line as the function body.
# Skip the whole check via SKIP_LINT_STUB_BEHIND_STAMP=1.
if [ "${SKIP_LINT_STUB_BEHIND_STAMP:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) skipped via SKIP_LINT_STUB_BEHIND_STAMP=1"
    WARNINGS=$((WARNINGS + 1))
else
    CACHE="$REPO_ROOT/build/todo-cache.json"
    if [ ! -f "$CACHE" ]; then
        echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) skipped -- build/todo-cache.json missing; run bash scripts/todo-graph/build-and-validate.sh"
        WARNINGS=$((WARNINGS + 1))
    elif ! python3 -c "import json,sys; d=json.load(open('$CACHE')); sys.exit(0 if any('stamped_items' in (e or {}) for e in d) else 2)" 2>/dev/null; then
        echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) deferred -- build/todo-cache.json lacks per-item file:symbol map; awaiting TODO-06 cache extension"
        WARNINGS=$((WARNINGS + 1))
    else
        # When the cache extension lands, this branch walks each stamped
        # function and flags <=3-line return-constant bodies without an
        # INTENTIONAL-STUB marker. Implementation lands when the dep does.
        echo -e "${YELLOW}warn${NC}: Check 7 (stub-behind-stamp) cache extension present but check implementation pending"
        WARNINGS=$((WARNINGS + 1))
    fi
fi

# ============================================================================
# Check 8: Phantom-include detection (TODO-08 #12)
# ============================================================================
# Designed to verify every #include in src/kernel/**/*.c references at least
# one symbol from that header. Accurate detection requires clangd-grade
# unused-include analysis via the lsp-bridge MCP server (TODO-07 #7
# diagnostics tool). The MCP server runs as a JSON-RPC subprocess and is
# not callable from this bash script today; design review 2026-04-28
# deferred this check rather than ship a heuristic grep that produces
# false positives on transitive macro-only headers. Skip via
# SKIP_LINT_PHANTOM_INCLUDE=1 once the MCP wiring lands.
if [ "${SKIP_LINT_PHANTOM_INCLUDE:-}" = "1" ]; then
    echo -e "${YELLOW}warn${NC}: Check 8 (phantom-include) skipped via SKIP_LINT_PHANTOM_INCLUDE=1"
    WARNINGS=$((WARNINGS + 1))
else
    echo -e "${YELLOW}warn${NC}: Check 8 (phantom-include) deferred -- requires lsp-bridge MCP integration; tracked in TODO-07 follow-up"
    WARNINGS=$((WARNINGS + 1))
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
    placeholder_hits="$(grep -rnE \
        '> \*\*(Verified|Quality reviewed):\*\*.*<(this-commit|hash|insert-hash|insert-commit|TODO|FILL|FIXME|TBD-hash)>' \
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
