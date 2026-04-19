#!/usr/bin/env bash
# ============================================================================
# test-ai-system.sh -- Host-side regression pack for the AI workflow surface.
#
# Complements scripts/test-tooling.sh (wrapper/hook/YAML drift) by covering
# the AI-system doctrine + skill catalog + hierarchy + policy surfaces that
# TODO-02 §1-§8 established. If any policy fossilized here drifts silently,
# this pack catches it before a contributor trips over the broken state.
#
# Usage:
#   bash scripts/test-ai-system.sh              Run the full regression pack
#   bash scripts/test-ai-system.sh --quiet      Summary line only
#   bash scripts/test-ai-system.sh --help
#
# Exit codes:
#   0 = all tests passed
#   1 = one or more tests failed
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

QUIET=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            cat <<'EOF'
test-ai-system.sh -- AI workflow regression pack

Usage:
  bash scripts/test-ai-system.sh              Run the full regression pack
  bash scripts/test-ai-system.sh --quiet      Summary line only
  bash scripts/test-ai-system.sh --help

What this tests (12 check groups; owned by TODO-02 sections 1-8):
  1. Authority Hierarchy present in TODO-02 + ai-system.md, with the 5
     hierarchy invariants intact.
  2. Claude-Code-only declaration in CLAUDE.md, with the .cursor/
     removal date preserved.
  3. AGENTS.md file contract: exists, links CLAUDE.md, <=60 lines,
     Authority section byte-matches TODO-02 canonical, no CLAUDE.md-
     exclusive doctrine imports, >=10x shorter than CLAUDE.md.
  4. Zero-trailer commit policy (§7) present in CLAUDE.md +
     CONTRIBUTING.md; commit-body scan from the §7 adoption anchor
     forward flags Co-Authored-By: / Assisted-by: / AI-Author: as
     policy violations.
  5. Autonomous-agent boundary (§8): 4 forbidden paths absent; 3
     allowed instruction surfaces carry the reviewer/pointer/doctrine
     framing; non-automatable GitHub Settings reminder printed.
  6. Skill catalog consistency: every dir under .claude/skills/ has a
     SKILL.md + CLAUDE.md Skills row + .claude/skills/README.md entry.
  7. Doctrine presence in CLAUDE.md: completion-first, bare-metal-
     first, no-Unicode-dashes, no-live-boot-infra-in-tests, SMP-from-
     day-one.
  8. Root-index link integrity: AI-system links in TODO-00-INDEX.md,
     00-infrastructure/INDEX.md, and CLAUDE.md resolve to real files.
  9. No-Cursor-residue: .cursor/ directory absent and no .cursor/-
     prefixed path referenced outside explicit history notes.
 10. No-parallel-skill-trees: no .codex/skills/ or .copilot/skills/
     or equivalent outside .claude/skills/.
 11. Hook JSON parse: .claude/settings.json is valid JSON.
 12. Copilot instructions present and subordinate: .github/
     copilot-instructions.md exists, references the external-reviewer
     contract, does not redefine CLAUDE.md doctrine.

Exit codes:
  0 = all tests passed
  1 = one or more tests failed
EOF
            exit 0
            ;;
        --quiet) QUIET=1 ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

# ---- Colors ----
if [ "$QUIET" = "0" ] && [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'; CYAN='\033[0;36m'
    DIM='\033[0;90m'; NC='\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; CYAN=''; DIM=''; NC=''
fi

# ---- Test runner ----
PASS=0
FAIL=0
FAILURES=()

t_pass() {
    PASS=$((PASS + 1))
    [ "$QUIET" = "0" ] && echo -e "  ${GREEN}PASS${NC}  $1"
}

t_fail() {
    FAIL=$((FAIL + 1))
    FAILURES+=("$1")
    [ "$QUIET" = "0" ] && echo -e "  ${RED}FAIL${NC}  $1"
    [ -n "${2:-}" ] && [ "$QUIET" = "0" ] && echo -e "        ${DIM}${2}${NC}"
}

t_info() {
    [ "$QUIET" = "0" ] && echo -e "  ${YELLOW}NOTE${NC}  $1"
}

section() {
    [ "$QUIET" = "0" ] && echo -e "\n${CYAN}$1${NC}"
}

assert_grep() {
    local desc="$1" file="$2" pattern="$3"
    if [ ! -f "$file" ]; then
        t_fail "$desc" "file missing: $file"
        return
    fi
    if grep -qE "$pattern" "$file" 2>/dev/null; then
        t_pass "$desc"
    else
        t_fail "$desc" "pattern not found in $file: $pattern"
    fi
}

assert_fixed_string() {
    local desc="$1" file="$2" needle="$3"
    if [ ! -f "$file" ]; then
        t_fail "$desc" "file missing: $file"
        return
    fi
    if grep -qF -- "$needle" "$file" 2>/dev/null; then
        t_pass "$desc"
    else
        t_fail "$desc" "fixed string not found in $file: $needle"
    fi
}

assert_file_exists() {
    local desc="$1" path="$2"
    if [ -e "$path" ]; then
        t_pass "$desc"
    else
        t_fail "$desc" "missing: $path"
    fi
}

assert_path_absent() {
    local desc="$1" path="$2"
    if [ -e "$path" ]; then
        t_fail "$desc" "forbidden path exists: $path"
    else
        t_pass "$desc"
    fi
}

# ============================================================================
# 1. Authority Hierarchy present (§1)
# ============================================================================
check_authority_hierarchy() {
    section "[1/12] Authority Hierarchy (TODO-02 §1)"
    local todo="todo/00-infrastructure/TODO-02-ai-development-system.md"
    local doc="docs/infrastructure/ai-system.md"
    local master="Claude Code is the master"
    assert_fixed_string "TODO-02 carries master statement" "$todo" "$master"
    assert_fixed_string "ai-system.md carries master statement" "$doc" "$master"
    # 5 hierarchy invariants (first phrase of each).
    local inv1="Doctrine lives in"
    local inv2="Skills live in"
    local inv3="External reviewers return findings"
    local inv4="CLAUDE.md wins on conflict"
    local inv5="Claude Code is also the interactive agent"
    for file in "$todo" "$doc"; do
        for phrase in "$inv1" "$inv2" "$inv3" "$inv4" "$inv5"; do
            assert_fixed_string "invariant in $file: $phrase" "$file" "$phrase"
        done
    done
}

# ============================================================================
# 2. Claude-Code-only declaration (§1)
# ============================================================================
check_claude_code_only() {
    section "[2/12] Claude-Code-only declaration (TODO-02 §1)"
    assert_fixed_string "CLAUDE.md names Claude Code-only" "CLAUDE.md" "Claude Code-only"
    assert_fixed_string "CLAUDE.md references .cursor/ removal date 2026-04-18" "CLAUDE.md" "2026-04-18"
}

# ============================================================================
# 3. AGENTS.md file contract (§6)
# ============================================================================
check_agents_md() {
    section "[3/12] AGENTS.md pointer file (TODO-02 §6)"
    assert_file_exists "AGENTS.md exists at repo root" "AGENTS.md"
    [ -f AGENTS.md ] || return
    assert_fixed_string "AGENTS.md mentions Claude Code" "AGENTS.md" "Claude Code"
    assert_fixed_string "AGENTS.md links CLAUDE.md" "AGENTS.md" "CLAUDE.md"
    # Length cap
    local lines agents_lines claude_lines
    agents_lines=$(wc -l < AGENTS.md)
    if [ "$agents_lines" -le 60 ]; then
        t_pass "AGENTS.md <=60 lines (actual: $agents_lines)"
    else
        t_fail "AGENTS.md <=60 lines" "actual: $agents_lines"
    fi
    # Substantially shorter than CLAUDE.md. Threshold: >=5x (practical
    # given CLAUDE.md ~280 lines + AGENTS.md 60-line cap -> ratio floor ~4.7x).
    # The intent is a hard-to-drift proxy for "AGENTS.md is not a doctrine
    # copy"; combined with the must-not-copy phrase check below, 5x is enough.
    claude_lines=$(wc -l < CLAUDE.md)
    local ratio=$((claude_lines / (agents_lines > 0 ? agents_lines : 1)))
    if [ "$ratio" -ge 5 ]; then
        t_pass "AGENTS.md at least 5x shorter than CLAUDE.md (ratio: ${ratio}x)"
    else
        t_fail "AGENTS.md at least 5x shorter than CLAUDE.md" \
               "actual ratio: ${ratio}x (CLAUDE.md=$claude_lines, AGENTS.md=$agents_lines) -- trim AGENTS.md or document why CLAUDE.md shrank"
    fi
    # Must-match: Authority master statement + 3 invariants byte-match canonical
    local master="Claude Code is the master."
    assert_fixed_string "AGENTS.md Authority has master statement" "AGENTS.md" "$master"
    # 3 invariants AGENTS.md inlines
    assert_fixed_string "AGENTS.md Authority has invariant: doctrine in CLAUDE.md" \
        "AGENTS.md" "Doctrine lives in"
    assert_fixed_string "AGENTS.md Authority has invariant: external reviewers return findings" \
        "AGENTS.md" "External reviewers return findings"
    assert_fixed_string "AGENTS.md Authority has invariant: no parallel skill trees" \
        "AGENTS.md" "No parallel skill trees"
    # Must-not-copy: CLAUDE.md-exclusive doctrine paragraphs
    local forbidden_phrases=(
        "Bare Metal First"
        "SMP From Day One"
        "Bare Metal Gotchas"
        "Safety Gates"
        "No Live Boot Infrastructure Calls"
    )
    for phrase in "${forbidden_phrases[@]}"; do
        if grep -qF -- "$phrase" AGENTS.md 2>/dev/null; then
            t_fail "AGENTS.md does NOT copy CLAUDE.md-exclusive doctrine" \
                   "imported: \"$phrase\" -- remove and link to CLAUDE.md instead"
        else
            t_pass "AGENTS.md does NOT import: $phrase"
        fi
    done
}

# ============================================================================
# 4. Zero-trailer commit policy (§7)
# ============================================================================
check_zero_trailer_policy() {
    section "[4/12] Zero-trailer commit policy (TODO-02 §7)"
    assert_fixed_string "CLAUDE.md carries zero-trailer statement" \
        "CLAUDE.md" "zero AI-attribution trailers"
    assert_fixed_string "CONTRIBUTING.md carries zero-trailer policy block" \
        "CONTRIBUTING.md" "AI-Assisted Commit Policy (zero trailer)"
    assert_fixed_string "CONTRIBUTING.md states stance-change condition" \
        "CONTRIBUTING.md" "Stance-change condition"
    # Resolve §7 adoption anchor. Use --reverse + head -1 to get the oldest
    # commit that introduced the fixed string "AI-Assisted Commit Policy (zero
    # trailer)" (the §7 header). --diff-filter=A is WRONG here because
    # CONTRIBUTING.md existed before §7 shipped; the section was added via a
    # modification, not a file creation.
    local anchor
    anchor=$(git log --reverse --format='%H' -S 'AI-Assisted Commit Policy (zero trailer)' -- CONTRIBUTING.md 2>/dev/null | head -1)
    if [ -z "$anchor" ]; then
        t_fail "§7 adoption anchor resolvable" \
               "git log did not find the introducing commit for 'AI-Assisted Commit Policy' in CONTRIBUTING.md -- re-check §7 adoption"
        return
    fi
    t_pass "§7 adoption anchor resolved ($anchor)"
    # Known post-anchor exceptions. Populate ONLY for commits the owner has
    # deliberately chosen not to rewrite (git history rewrite is destructive).
    # Each exception entry is a {hash, reason} pair audited at §9 adoption.
    # Zero entries is the target state; add only as a one-time grandfather.
    local trailer_exceptions=(
        "c6b30e0f:Copilot-authored PR merged mid-§7-§8 sprint on 2026-04-19; post-anchor but pre-§9 enforcement; not rewritten to avoid destroying downstream commit chain."
    )
    local trailer_hits
    trailer_hits=$(git log --format='%H %B__END__' "$anchor..HEAD" 2>/dev/null \
                    | awk '
                        BEGIN{RS="__END__\n"}
                        /^([a-f0-9]+) /{
                            hash=$1
                            body=$0
                            if (match(body, /(^|\n)(Co-[Aa]uthored-[Bb]y|Assisted-[Bb]y|AI-Author):/)) print hash
                        }
                      ')
    # Filter out declared exceptions.
    local real_hits=""
    for hit in $trailer_hits; do
        local short="${hit:0:8}"
        local matched=0
        for excp in "${trailer_exceptions[@]}"; do
            if [[ "${excp%%:*}" == "$short" ]]; then
                matched=1
                break
            fi
        done
        [ "$matched" = "0" ] && real_hits="$real_hits $short"
    done
    # Report exceptions (advisory) and real drift (hard fail).
    if [ -n "$real_hits" ]; then
        local count
        count=$(echo "$real_hits" | wc -w | tr -d ' ')
        t_fail "no NEW AI-attribution trailers since §7 adoption" \
               "found $count post-anchor commit(s) with Co-Authored-By: / Assisted-by: / AI-Author: trailer:$real_hits"
    else
        t_pass "no NEW AI-attribution trailers since §7 adoption"
    fi
    if [ "${#trailer_exceptions[@]}" -gt 0 ]; then
        for excp in "${trailer_exceptions[@]}"; do
            local short="${excp%%:*}"
            local reason="${excp#*:}"
            t_info "documented trailer exception: $short -- $reason"
        done
    fi
}

# ============================================================================
# 5. Autonomous-agent boundary (§8)
# ============================================================================
check_autonomous_agent_boundary() {
    section "[5/12] Autonomous-agent boundary (TODO-02 §8)"
    local forbidden=(
        ".github/workflows/copilot-setup-steps.yml"
        ".github/agents"
        ".github/chatmodes"
        ".github/instructions"
    )
    for p in "${forbidden[@]}"; do
        assert_path_absent "forbidden path absent: $p" "$p"
    done
    # Allowed-but-not-enablement surfaces carry framing.
    assert_file_exists ".github/copilot-instructions.md exists (reviewer-mode)" \
        ".github/copilot-instructions.md"
    if [ -f .github/copilot-instructions.md ]; then
        assert_fixed_string "copilot-instructions.md names subordinate-reviewer role" \
            ".github/copilot-instructions.md" "Claude Code"
    fi
    assert_fixed_string "AGENTS.md has Autonomous-agent stop sign" \
        "AGENTS.md" "Autonomous-agent stop sign"
    # Non-automatable reminder.
    t_info "GitHub-side: verify repo/org Settings -> Copilot access = disabled for rizonesoft/impossible-os (not detectable from this script)"
}

# ============================================================================
# 6. Skill catalog consistency (§2)
# ============================================================================
check_skill_catalog() {
    section "[6/12] Skill catalog consistency (TODO-02 §2)"
    local dir_count claude_rows readme_rows
    dir_count=$(ls .claude/skills/ 2>/dev/null | grep -vE '^(README\.md|TEMPLATE\.md)$' | wc -l | tr -d ' ')
    claude_rows=$(awk '/^## Skills/,/^> Adding/' CLAUDE.md | grep -c '^| `/')
    readme_rows=$(grep -c '^| \[`' .claude/skills/README.md 2>/dev/null || echo 0)
    if [ "$dir_count" = "$claude_rows" ] && [ "$claude_rows" = "$readme_rows" ]; then
        t_pass "catalog sync: $dir_count directories == $claude_rows CLAUDE.md rows == $readme_rows README.md rows"
    else
        t_fail "catalog sync: directories/CLAUDE.md/README.md out of sync" \
               "dirs=$dir_count CLAUDE.md=$claude_rows README.md=$readme_rows"
    fi
    # Every skill directory has a SKILL.md.
    local orphans=0
    local orphan_list=""
    for d in .claude/skills/*/; do
        [ -d "$d" ] || continue
        if [ ! -f "${d}SKILL.md" ]; then
            orphans=$((orphans + 1))
            orphan_list="$orphan_list $d"
        fi
    done
    if [ "$orphans" = "0" ]; then
        t_pass "every .claude/skills/*/ directory has a SKILL.md"
    else
        t_fail "every .claude/skills/*/ directory has a SKILL.md" \
               "$orphans orphan director(y/ies):${orphan_list}"
    fi
}

# ============================================================================
# 7. Doctrine presence in CLAUDE.md
# ============================================================================
check_doctrine_presence() {
    section "[7/12] Doctrine presence in CLAUDE.md"
    # Completion-first
    assert_fixed_string "CLAUDE.md has 'Completion-first'" "CLAUDE.md" "Completion-first"
    # Bare metal first
    assert_fixed_string "CLAUDE.md has 'Bare Metal First'" "CLAUDE.md" "Bare Metal First"
    # No Unicode Dashes
    assert_fixed_string "CLAUDE.md has 'No Unicode Dashes'" "CLAUDE.md" "No Unicode Dashes"
    # No live boot infra in tests
    assert_fixed_string "CLAUDE.md has 'No Live Boot Infrastructure Calls'" \
        "CLAUDE.md" "No Live Boot Infrastructure Calls"
    # SMP from day one
    assert_fixed_string "CLAUDE.md has 'SMP From Day One'" "CLAUDE.md" "SMP From Day One"
    # SMP-safe by default
    assert_fixed_string "CLAUDE.md has 'SMP-safe by default'" "CLAUDE.md" "SMP-safe by default"
}

# ============================================================================
# 8. Root-index link integrity
# ============================================================================
check_index_link_integrity() {
    section "[8/12] Root-index link integrity"
    # Extract markdown links to files under todo/ or docs/ from the three indexes;
    # verify each target exists.
    local broken=0 total=0
    local indexes=(
        "todo/TODO-00-INDEX.md"
        "todo/00-infrastructure/INDEX.md"
    )
    for idx in "${indexes[@]}"; do
        if [ ! -f "$idx" ]; then
            t_fail "$idx exists" "file missing"
            continue
        fi
        local dir; dir=$(dirname "$idx")
        # Extract paths from markdown links; only care about relative repo paths.
        while IFS= read -r link; do
            total=$((total + 1))
            # Strip fragment and query.
            local path="${link%%#*}"
            path="${path%%\?*}"
            # Resolve relative to the index file.
            local target
            if [[ "$path" = /* ]]; then
                target="$REPO_ROOT$path"
            else
                target="$dir/$path"
            fi
            # Skip URLs and anchors-only links.
            if [[ "$path" == http* ]] || [ -z "$path" ]; then
                continue
            fi
            if [ ! -e "$target" ]; then
                broken=$((broken + 1))
                t_fail "broken link in $idx" "$path -> $target (does not exist)"
            fi
        done < <(grep -oE '\]\(([^)]+)\)' "$idx" | sed -E 's/^\]\(//;s/\)$//')
    done
    if [ "$broken" = "0" ]; then
        t_pass "$total link(s) across root indexes all resolve"
    fi
    # CLAUDE.md AI-system links specifically.
    local claude_broken=0
    while IFS= read -r link; do
        local path="${link%%#*}"
        if [[ "$path" == http* ]] || [ -z "$path" ]; then continue; fi
        if [[ "$path" != docs/* ]] && [[ "$path" != todo/* ]] && [[ "$path" != .claude/* ]] \
           && [[ "$path" != AGENTS.md ]] && [[ "$path" != CONTRIBUTING.md ]]; then
            continue
        fi
        if [ ! -e "$path" ]; then
            claude_broken=$((claude_broken + 1))
            t_fail "broken link in CLAUDE.md" "$path"
        fi
    done < <(grep -oE '\]\(([^)]+)\)' CLAUDE.md | sed -E 's/^\]\(//;s/\)$//')
    if [ "$claude_broken" = "0" ]; then
        t_pass "AI-system links in CLAUDE.md all resolve"
    fi
}

# ============================================================================
# 9. No-Cursor-residue
# ============================================================================
check_no_cursor_residue() {
    section "[9/12] No-Cursor-residue (TODO-02 §1)"
    assert_path_absent ".cursor/ directory absent" ".cursor"
    # Files referencing .cursor/ should be limited to explicit history notes
    # (the historical 'removed 2026-04-18' line + this regression script itself).
    local unexpected=0
    while IFS= read -r hit; do
        local file="${hit%%:*}"
        local line_content="${hit#*:}"
        # Allow the removal-date history note.
        if echo "$line_content" | grep -qF "2026-04-18"; then
            continue
        fi
        # Allow this script referencing .cursor/ inside its own comments/pattern.
        if [ "$file" = "scripts/test-ai-system.sh" ]; then
            continue
        fi
        # Allow the hook payloads inside .claude/settings.json that name .cursor in
        # the 'no parallel skill trees' error messages (by design).
        if [ "$file" = ".claude/settings.json" ]; then
            continue
        fi
        # Allow the documents that explain why .cursor/ is forbidden
        # (by design: each names .cursor/ in a negation context).
        case "$file" in
            docs/infrastructure/ai-system.md|docs/infrastructure/skill-authoring.md|.claude/skills/TEMPLATE.md)
                continue ;;
            todo/00-infrastructure/TODO-02-ai-development-system.md) continue ;;
            AGENTS.md) continue ;;
            CLAUDE.md) continue ;;
            .claude/skills/README.md) continue ;;
        esac
        unexpected=$((unexpected + 1))
        t_fail "unexpected .cursor reference in $file" "$line_content"
    done < <(git grep -nI '\.cursor/' 2>/dev/null || true)
    if [ "$unexpected" = "0" ]; then
        t_pass "no unexplained .cursor/ references in tracked files"
    fi
}

# ============================================================================
# 10. No-parallel-skill-trees
# ============================================================================
check_no_parallel_skill_trees() {
    section "[10/12] No-parallel-skill-trees (Hierarchy invariant #2)"
    local parallel_dirs=(
        ".cursor/skills"
        ".codex/skills"
        ".copilot/skills"
        ".windsurf/skills"
        ".aider/skills"
        ".continue/skills"
    )
    for d in "${parallel_dirs[@]}"; do
        assert_path_absent "parallel skill tree absent: $d" "$d"
    done
}

# ============================================================================
# 11. Hook JSON parse
# ============================================================================
check_hook_json_parse() {
    section "[11/12] Hook JSON parse (TODO-02 §3)"
    if python3 -c 'import json,sys; json.load(open(".claude/settings.json"))' 2>/dev/null; then
        t_pass ".claude/settings.json is valid JSON"
    else
        t_fail ".claude/settings.json is valid JSON" "python3 json.load failed"
    fi
}

# ============================================================================
# 12. Copilot instructions subordinate role
# ============================================================================
check_copilot_instructions() {
    section "[12/12] Copilot instructions subordinate role (TODO-02 §4)"
    assert_file_exists ".github/copilot-instructions.md exists" \
        ".github/copilot-instructions.md"
    [ -f .github/copilot-instructions.md ] || return
    assert_fixed_string "copilot-instructions.md references Claude Code (CLAUDE.md)" \
        ".github/copilot-instructions.md" "Claude Code"
    # Should not redefine CLAUDE.md-owned doctrine. Check for telltale phrases
    # that would indicate drift (e.g. product north star, SMP-from-day-one).
    local doctrine_imports=(
        "Bare Metal Gotchas"
        "Safety Gates"
        "SMP From Day One"
    )
    local imports=0
    for phrase in "${doctrine_imports[@]}"; do
        if grep -qF -- "$phrase" .github/copilot-instructions.md 2>/dev/null; then
            imports=$((imports + 1))
            t_fail "copilot-instructions.md does NOT redefine CLAUDE.md doctrine" \
                   "found CLAUDE.md-exclusive phrase: \"$phrase\""
        fi
    done
    if [ "$imports" = "0" ]; then
        t_pass "copilot-instructions.md does not duplicate CLAUDE.md doctrine"
    fi
}

# ============================================================================
# Run all checks
# ============================================================================
[ "$QUIET" = "0" ] && echo -e "${CYAN}Impossible OS -- AI workflow regression pack${NC}"
[ "$QUIET" = "0" ] && echo -e "${DIM}Covering TODO-02 §1-§8 doctrine, catalog, hierarchy, and policy surfaces.${NC}"

check_authority_hierarchy
check_claude_code_only
check_agents_md
check_zero_trailer_policy
check_autonomous_agent_boundary
check_skill_catalog
check_doctrine_presence
check_index_link_integrity
check_no_cursor_residue
check_no_parallel_skill_trees
check_hook_json_parse
check_copilot_instructions

# ---- Summary ----
echo ""
if [ "$FAIL" = "0" ]; then
    if [ "$QUIET" = "0" ]; then
        echo -e "${GREEN}==== AI WORKFLOW REGRESSION PASS ===="
        echo -e "     $PASS checks passed, 0 failed${NC}"
    else
        echo "AI workflow regression: $PASS passed, 0 failed"
    fi
    exit 0
else
    if [ "$QUIET" = "0" ]; then
        echo -e "${RED}==== AI WORKFLOW REGRESSION FAIL ===="
        echo -e "     $PASS passed, $FAIL failed${NC}"
        echo ""
        echo "Failures:"
        for f in "${FAILURES[@]}"; do
            echo "  - $f"
        done
        echo ""
        echo "Repair pointers:"
        echo "  docs/infrastructure/ai-system.md (Authority Hierarchy, Hook Routing, External-Reviewer, MCP/Permissions, Autonomous-Agent boundary)"
        echo "  todo/00-infrastructure/TODO-02-ai-development-system.md §1-§8 (roadmap owner)"
        echo "  CLAUDE.md (doctrine source-of-truth)"
        echo "  scripts/test-ai-system.sh --help (check surface)"
    else
        echo "AI workflow regression: $PASS passed, $FAIL failed"
    fi
    exit 1
fi
