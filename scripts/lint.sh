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
BODY_END_RE = re.compile(
    r"^##\s+(?:\d+\.|Format Quick Reference|OS Comparison|Unit Tests|"
    r"Verification|History)\b",
    re.MULTILINE,
)

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
                # Single-quoted body: bash never evaluates, safe.
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
    CITATION_LEGACY_FILES=(
        src/boot/uefi/bootx64.c
        src/kernel/klog.c
        src/kernel/main/boot_hw.c
        src/kernel/main/boot_progress.c
        src/kernel/nt/nt_sync.c
        src/kernel/nt/nt_syscall.c
        src/kernel/ob/ob.c
        src/kernel/ob/ob_file.c
        src/kernel/printk.c
        src/kernel/smbios.c
        src/kernel/test/input_record.c
        src/kernel/test/test_alpc.c
        src/kernel/test/test_boot_decision.c
        src/kernel/test/test_desktop.c
        src/kernel/test/test_desktop_reset.c
        src/kernel/test/test_firmware_platform.c
        src/kernel/test/test_firmware_tables.c
        src/kernel/test/test_uefi_boot.c
        src/kernel/test/test_usermode.c
        src/kernel/test/test_usermode_launcher.c
        src/kernel/uefi_config.c
        src/kernel/uefi_runtime.c
    )

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
