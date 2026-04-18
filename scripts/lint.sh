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

Checks (4):
  1. #pragma once or include guard in every .h
  2. Lines <= 120 characters (excludes comment lines)
  3. No trailing whitespace
  4. No numeric TODO shorthand (TODO-NN sectionN, DNN TNN) outside todo/**
     except a baseline allowlist of pre-existing files (see the developer
     tooling stack roadmap -- Tooling Doctor section -- for the legacy-XREF
     sweep).

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
        # Skip templates that TEACH the shorthand intentionally.
        case "$relpath" in
            .github/PULL_REQUEST_TEMPLATE.md) continue ;;
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
