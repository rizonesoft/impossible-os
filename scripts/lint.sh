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

# Exclude third-party and auto-generated files
EXCLUDE_PATTERNS=(
    "stb_truetype"
    "stb_image"
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
# Check 4: Functions > 50 lines (warning only)
# ============================================================================
for file in "${FILES[@]}"; do
    [[ "$file" != *.c ]] && continue
    relpath="${file#"$REPO_ROOT"/}"

    # Use awk to track brace depth and function length
    awk -v file="$relpath" '
        # Detect function definition: line with { at end, not a struct/enum/if/etc
        /^[a-zA-Z_].*\)$/ || /^[a-zA-Z_].*\)\s*{/ {
            if ($0 !~ /typedef|struct|enum|if|else|while|for|switch|#/) {
                func_start = NR
                func_name = $0
                sub(/\(.*/, "", func_name)
                sub(/.*[ \t\*]/, "", func_name)
            }
        }
        /^{/ && func_start > 0 && func_start == NR - 1 {
            brace_depth = 1
            body_start = NR
            next
        }
        /\)\s*{/ && func_start == NR {
            brace_depth = 1
            body_start = NR
            next
        }
        brace_depth > 0 {
            # Count braces
            for (i = 1; i <= length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") brace_depth++
                if (c == "}") brace_depth--
                if (brace_depth == 0) {
                    body_len = NR - body_start
                    if (body_len > 50) {
                        printf "'"${YELLOW}"'warn'"${NC}"': %s:%d: function '\''%s'\'' is %d lines (> 50)\n", file, func_start, func_name, body_len
                    }
                    func_start = 0
                    break
                }
            }
        }
    ' "$file" 2>/dev/null || true
    # Count warnings
    func_warns=$(awk '
        /^[a-zA-Z_].*\)$/ || /^[a-zA-Z_].*\)\s*{/ {
            if ($0 !~ /typedef|struct|enum|if|else|while|for|switch|#/) {
                func_start = NR
                func_name = $0
            }
        }
        /^{/ && func_start > 0 && func_start == NR - 1 { brace_depth = 1; body_start = NR; next }
        /\)\s*{/ && func_start == NR { brace_depth = 1; body_start = NR; next }
        brace_depth > 0 {
            for (i = 1; i <= length($0); i++) {
                c = substr($0, i, 1)
                if (c == "{") brace_depth++
                if (c == "}") brace_depth--
                if (brace_depth == 0) {
                    body_len = NR - body_start
                    if (body_len > 50) count++
                    func_start = 0
                    break
                }
            }
        }
        END { print count+0 }
    ' "$file" 2>/dev/null || echo 0)
    WARNINGS=$((WARNINGS + func_warns))
done

# ============================================================================
# Check 5: camelCase function definitions (should be snake_case)
# ============================================================================
for file in "${FILES[@]}"; do
    [[ "$file" != *.c ]] && continue
    relpath="${file#"$REPO_ROOT"/}"

    # Look for function definitions with camelCase names
    # Pattern: type funcName( at start of line (not in comments)
    while IFS= read -r match; do
        linenum=$(echo "$match" | cut -d: -f1)
        line=$(echo "$match" | cut -d: -f2-)
        # Extract function name
        func=$(echo "$line" | sed -E 's/.*[ \t\*]([a-zA-Z_][a-zA-Z0-9_]*)\s*\(.*/\1/')
        # Check for camelCase: lowercase letter followed by uppercase
        if echo "$func" | grep -qP '[a-z][A-Z]' 2>/dev/null; then
            # Exclude known exceptions (Win32 API names like RegSetValueEx)
            if echo "$func" | grep -qE '^(Reg|Hkey|HKEY)' 2>/dev/null; then
                continue
            fi
            warn "$relpath" "$linenum" "function '$func' uses camelCase (prefer snake_case)"
        fi
    done < <(grep -nE '^[a-zA-Z_].*[a-zA-Z_][a-zA-Z0-9_]*\s*\(' "$file" 2>/dev/null \
        | grep -vE '^\s*(//|/\*|\*|#|if|else|while|for|switch|return|typedef|struct|enum)' \
        || true)
done

# ============================================================================
# Check 6: #define macros should be UPPER_CASE (warning only)
# ============================================================================
for file in "${FILES[@]}"; do
    relpath="${file#"$REPO_ROOT"/}"
    while IFS= read -r match; do
        linenum=$(echo "$match" | cut -d: -f1)
        line=$(echo "$match" | cut -d: -f2-)
        # Extract macro name
        macro=$(echo "$line" | sed -E 's/#\s*define\s+([a-zA-Z_][a-zA-Z0-9_]*).*/\1/')
        # Skip function-like macros with lowercase (common pattern: static inline wrappers)
        # Only flag pure lowercase macros (not mixed like TEST_ASSERT)
        if echo "$macro" | grep -qP '^[a-z][a-z0-9_]+$' 2>/dev/null; then
            # Skip include guards and common patterns
            if echo "$macro" | grep -qE '_H$|_h$|_INCLUDED' 2>/dev/null; then
                continue
            fi
            warn "$relpath" "$linenum" "macro '$macro' is lowercase (prefer UPPER_CASE)"
        fi
    done < <(grep -nE '^\s*#\s*define\s+[a-zA-Z_]' "$file" 2>/dev/null || true)
done

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
