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
  2. Claude-primary declaration in CLAUDE.md, with the historical
     .cursor/ removal date preserved in the AI-system docs.
  3. AGENTS.md file contract: exists, links CLAUDE.md, <=60 lines,
     Authority section full-sentence phrases byte-match TODO-02 +
     ai-system.md canonical block, no CLAUDE.md-exclusive doctrine
     imports, >=5x shorter than CLAUDE.md (10x aspirational).
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
 12. Copilot CLI reviewer retired: .github/copilot-instructions.md
     and scripts/copilot-review.sh are absent (deleted 2026-04-28),
     and no live wrapper caller remains in scripts / hooks / settings.

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
    local master="Claude Code is the primary interactive orchestrator"
    assert_fixed_string "TODO-02 carries primary-orchestrator statement" "$todo" "$master"
    assert_fixed_string "ai-system.md carries primary-orchestrator statement" "$doc" "$master"
    # 5 hierarchy invariants (first phrase of each).
    local inv1="Doctrine lives in"
    local inv2="Skills live in"
    local inv3="External reviewers return findings"
    local inv4="CLAUDE.md wins on conflict"
    local inv5="Claude Code remains the only implementation agent"
    for file in "$todo" "$doc"; do
        for phrase in "$inv1" "$inv2" "$inv3" "$inv4" "$inv5"; do
            assert_fixed_string "invariant in $file: $phrase" "$file" "$phrase"
        done
    done
}

# ============================================================================
# 2. Claude-primary declaration (§1)
# ============================================================================
check_claude_code_only() {
    section "[2/12] Claude-primary declaration (TODO-02 §1)"
    assert_fixed_string "CLAUDE.md names Claude as primary interactive orchestrator" \
        "CLAUDE.md" "Claude-primary for interactive work"
    assert_fixed_string "skill-authoring.md references .cursor/ removal date 2026-04-18" \
        "docs/infrastructure/skill-authoring.md" "2026-04-18"
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
    # Substantially shorter than CLAUDE.md. Threshold: >=5x floor
    # (aspirational 10x target per TODO-02 §6; floor of 5x keeps a hard
    # proxy for "AGENTS.md is not a doctrine copy" while allowing AGENTS.md
    # to grow in proportion to CLAUDE.md without false failures). Combined
    # with the must-not-copy phrase check below and the byte-match check
    # that follows, 5x is the operative guardrail.
    claude_lines=$(wc -l < CLAUDE.md | tr -d ' ')
    agents_lines=$(echo "$agents_lines" | tr -d ' ')
    if [ "${agents_lines:-0}" -le 0 ]; then
        t_fail "AGENTS.md has non-zero line count" "wc -l returned '$agents_lines'"
        return
    fi
    local ratio=$((claude_lines / agents_lines))
    if [ "$ratio" -ge 5 ]; then
        t_pass "AGENTS.md at least 5x shorter than CLAUDE.md (ratio: ${ratio}x)"
    else
        t_fail "AGENTS.md at least 5x shorter than CLAUDE.md" \
               "actual ratio: ${ratio}x (CLAUDE.md=$claude_lines, AGENTS.md=$agents_lines) -- trim AGENTS.md or document why CLAUDE.md shrank"
    fi
    # Must-match: Authority section full-sentence phrases byte-identical in
    # both files. Substring checks like "Doctrine lives in" pass even if the
    # sentence is reworded around the first phrase; these full-sentence
    # phrases fail on any wording drift between AGENTS.md and the canonical
    # 5-invariant block in TODO-02 / ai-system.md. Dropping each to a sub-
    # string check hid a real drift (AGENTS.md "not edits" vs TODO-02
    # "never edits") until the §9 review surfaced it.
    local todo="todo/00-infrastructure/TODO-02-ai-development-system.md"
    local doc="docs/infrastructure/ai-system.md"
    # Full-sentence canonical phrases that must appear verbatim in AGENTS.md,
    # TODO-02, AND ai-system.md. Pulled from the canonical 5-invariant block
    # (TODO-02 lines 21, 23 and the parallel paragraph in ai-system.md).
    local canonical_phrases=(
        "Doctrine files tell Claude what to do, skills tell Claude how to do it, and external reviewers tell Claude what might be wrong."
        "There is no sibling executor in this repo."
        "Doctrine lives in \`CLAUDE.md\`. Nowhere else."
        "External reviewers return findings, never edits."
    )
    for phrase in "${canonical_phrases[@]}"; do
        for f in AGENTS.md "$todo" "$doc"; do
            if grep -qF -- "$phrase" "$f" 2>/dev/null; then
                t_pass "byte-match: $(basename "$f") carries \"${phrase:0:50}...\""
            else
                t_fail "byte-match: $(basename "$f") carries canonical phrase" \
                       "phrase not found verbatim in $f: \"$phrase\""
            fi
        done
    done
    # No-parallel-skill-trees phrasing differs between AGENTS.md (bullet)
    # and TODO-02 (table cell). Check only that AGENTS.md has it.
    assert_fixed_string "AGENTS.md Authority has invariant: no parallel skill trees" \
        "AGENTS.md" "No parallel skill trees"
    # Primary-orchestrator statement (shared verbatim in AGENTS.md + TODO-02).
    local master="Claude Code is the primary interactive orchestrator."
    assert_fixed_string "AGENTS.md carries primary-orchestrator statement" "AGENTS.md" "$master"
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
        "e4454a2b:Copilot-authored PR merged 2026-04-22 (re-enabled fastpath probe + trimmed CI artifacts) carrying Co-authored-by trailer; not rewritten because -17 WHPX validation stamp + -18 fast-path observability work built on top, and rebasing would destroy that chain."
    )
    # Inclusive range: scan the anchor commit itself plus every commit after.
    # Earlier version used `$anchor..HEAD` which EXCLUDES the anchor, so a
    # forbidden trailer ON the §7 adoption commit would slip through. Use
    # `${anchor}^..HEAD` when the anchor has a parent; otherwise scan HEAD
    # back to (and including) the root commit.
    local range
    if git rev-parse --verify --quiet "${anchor}^" >/dev/null 2>&1; then
        range="${anchor}^..HEAD"
    else
        range="HEAD"
    fi
    # Parse trailers via `git interpret-trailers --parse` per commit. This is
    # the Git-official definition of "trailer" (last paragraph, RFC-822-style
    # "Key: value" lines). Unlike regex-on-raw-body it ignores
    # Co-Authored-By: occurrences inside code blocks or quoted diffs, and is
    # robust to arbitrary body content (no awk multi-char RS hazard).
    local trailer_hits=""
    local commit
    while IFS= read -r commit; do
        [ -z "$commit" ] && continue
        local trailers
        trailers=$(git log -1 --format='%B' "$commit" 2>/dev/null \
                   | git interpret-trailers --parse 2>/dev/null \
                   | grep -E '^(Co-[Aa]uthored-[Bb]y|Assisted-[Bb]y|AI-Author):' || true)
        if [ -n "$trailers" ]; then
            trailer_hits="$trailer_hits ${commit:0:8}"
        fi
    done < <(git log --format='%H' "$range" 2>/dev/null)
    # Filter out declared exceptions.
    local real_hits=""
    for hit in $trailer_hits; do
        local matched=0
        for excp in "${trailer_exceptions[@]}"; do
            if [[ "${excp%%:*}" == "$hit" ]]; then
                matched=1
                break
            fi
        done
        [ "$matched" = "0" ] && real_hits="$real_hits $hit"
    done
    # Report exceptions (advisory) and real drift (hard fail).
    if [ -n "$real_hits" ]; then
        local count
        count=$(echo "$real_hits" | wc -w | tr -d ' ')
        t_fail "no NEW AI-attribution trailers since §7 adoption" \
               "found $count post-anchor commit(s) with Co-Authored-By: / Assisted-by: / AI-Author: trailer:$real_hits"
    else
        t_pass "no NEW AI-attribution trailers since §7 adoption (inclusive of anchor)"
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
    # Copilot CLI reviewer was retired wholesale 2026-04-28 by the
    # Copilot-CLI-removal automation-hardening sweep; the
    # .github/copilot-instructions.md file is now also forbidden, not just
    # the autonomous-agent enablement files above.
    assert_path_absent "forbidden path absent: .github/copilot-instructions.md (retired)" \
        ".github/copilot-instructions.md"
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
    # Set-based check: extract the sorted set of skill names from the
    # directory, the CLAUDE.md Skills table, and .claude/skills/README.md,
    # then diff the sets. Count-only comparison (the earlier form) passed
    # if any rename + add-stale-row kept the totals equal, silently letting
    # drift ship. Set compare names the exact offenders.
    local tmpdir
    if ! tmpdir=$(mktemp -d 2>/dev/null) || [ -z "$tmpdir" ] || [ ! -d "$tmpdir" ]; then
        t_fail "catalog: tempdir creation" "mktemp -d failed -- cannot run set-compare"
        return
    fi
    # Cleanup on all return paths (early fail or success). Local trap:
    # bash traps are function-global, so we use a trailing rm -rf and also
    # guard with a return-tracker function-level flag.
    # Directories under .claude/skills/.
    if ! find .claude/skills -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null \
        | sort > "$tmpdir/dirs"; then
        t_fail "catalog: dirs extraction" "find failed"
        rm -rf "$tmpdir"
        return
    fi
    # CLAUDE.md table rows look like:  | `/skill-name` | description |
    # Extract the `/skill-name` token. Anchor the block between `## Skills`
    # and the next `##`-level heading (tolerates the sentinel `> Adding...`
    # paragraph moving or being renamed).
    awk '/^## Skills/{flag=1; next} /^## /{flag=0} flag' CLAUDE.md \
        | grep -oE '^\| `/[a-z0-9:-]+`' \
        | sed -E 's@^\| `/@@; s@`$@@' \
        | sort > "$tmpdir/claude" || {
            t_fail "catalog: CLAUDE.md extraction" "pipeline failed"
            rm -rf "$tmpdir"
            return
        }
    # README.md table rows look like:  | [`skill-name`](skill-name/) | ...
    grep -oE '^\| \[`[a-z0-9:-]+`\]' .claude/skills/README.md 2>/dev/null \
        | sed -E 's@^\| \[`@@; s@`\]$@@' \
        | sort > "$tmpdir/readme" || {
            t_fail "catalog: README.md extraction" "pipeline failed"
            rm -rf "$tmpdir"
            return
        }
    # Require that each extracted file has at least one entry. Empty sets
    # trivially satisfy comm -23/-13, so a silent extraction failure (no
    # matches due to regex drift, missing file, reformatted table) would
    # produce a false "set-equal" PASS.
    if [ ! -s "$tmpdir/dirs" ] || [ ! -s "$tmpdir/claude" ] || [ ! -s "$tmpdir/readme" ]; then
        t_fail "catalog: non-empty extraction" \
               "dirs=$(wc -l < "$tmpdir/dirs") claude=$(wc -l < "$tmpdir/claude") readme=$(wc -l < "$tmpdir/readme") -- one or more extractions empty (regex drift or file reformatted?)"
        rm -rf "$tmpdir"
        return
    fi
    local dirs_only claude_only readme_only
    dirs_only=$(comm -23 "$tmpdir/dirs" "$tmpdir/claude" | head -5 | tr '\n' ' ')
    claude_only=$(comm -13 "$tmpdir/dirs" "$tmpdir/claude" | head -5 | tr '\n' ' ')
    readme_dir_diff=$(comm -23 "$tmpdir/dirs" "$tmpdir/readme" | head -5 | tr '\n' ' ')
    readme_extra=$(comm -13 "$tmpdir/dirs" "$tmpdir/readme" | head -5 | tr '\n' ' ')
    local dir_count claude_count readme_count
    dir_count=$(wc -l < "$tmpdir/dirs" | tr -d ' ')
    claude_count=$(wc -l < "$tmpdir/claude" | tr -d ' ')
    readme_count=$(wc -l < "$tmpdir/readme" | tr -d ' ')
    local synced=1
    if [ -n "$dirs_only" ]; then
        t_fail "catalog: skill dirs missing from CLAUDE.md Skills table" \
               "unlisted: $dirs_only"
        synced=0
    fi
    if [ -n "$claude_only" ]; then
        t_fail "catalog: CLAUDE.md Skills table has rows without a dir" \
               "stale: $claude_only"
        synced=0
    fi
    if [ -n "$readme_dir_diff" ]; then
        t_fail "catalog: skill dirs missing from .claude/skills/README.md" \
               "unlisted: $readme_dir_diff"
        synced=0
    fi
    if [ -n "$readme_extra" ]; then
        t_fail "catalog: README.md rows without a dir" \
               "stale: $readme_extra"
        synced=0
    fi
    if [ "$synced" = "1" ]; then
        t_pass "catalog sync: $dir_count skills == $claude_count CLAUDE.md rows == $readme_count README.md rows (set-equal)"
    fi
    rm -rf "$tmpdir"
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
    section "[7/12] Doctrine presence in CLAUDE.md and canonical docs"
    # Completion-first
    assert_fixed_string "CLAUDE.md has 'Completion-first'" "CLAUDE.md" "Completion-first"
    # Bare metal first
    assert_fixed_string "CLAUDE.md has 'Bare Metal First'" "CLAUDE.md" "Bare Metal First"
    # No Unicode Dashes -- canonical text moved to
    # docs/infrastructure/code-style-policies.md per progressive-
    # disclosure trim (commit b3c823f3); CLAUDE.md keeps a one-line
    # pointer with lowercase phrasing ("No Unicode dashes"), and
    # the canonical heading carries the title-case form.
    assert_fixed_string "code-style-policies.md has 'No Unicode Dashes'" \
        "docs/infrastructure/code-style-policies.md" "No Unicode Dashes"
    assert_fixed_string "CLAUDE.md points at code-style-policies.md" \
        "CLAUDE.md" "code-style-policies.md"
    # No live boot infra in tests -- canonical text moved to
    # docs/infrastructure/test-policy.md per the same trim; CLAUDE.md
    # has a "Test Code Policy" section pointing at it.
    assert_fixed_string "test-policy.md has 'No Live Boot Infrastructure Calls'" \
        "docs/infrastructure/test-policy.md" "No Live Boot Infrastructure Calls"
    assert_fixed_string "CLAUDE.md points at test-policy.md" \
        "CLAUDE.md" "test-policy.md"
    # MCP Usage doctrine
    assert_fixed_string "mcp-usage.md has 'When to call'" \
        "docs/infrastructure/mcp-usage.md" "When to call"
    assert_fixed_string "mcp-usage.md names todo-graph server" \
        "docs/infrastructure/mcp-usage.md" "todo-graph"
    assert_fixed_string "mcp-usage.md names lsp-bridge server" \
        "docs/infrastructure/mcp-usage.md" "lsp-bridge"
    assert_fixed_string "CLAUDE.md has 'MCP Usage' section" \
        "CLAUDE.md" "MCP Usage"
    assert_fixed_string "CLAUDE.md points at mcp-usage.md" \
        "CLAUDE.md" "mcp-usage.md"
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
            todo/00-infrastructure/TODO-09-repository-transfer-rizonetech.md) continue ;;
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
# 12. Copilot CLI reviewer absence (retired 2026-04-28)
# ============================================================================
# The Copilot CLI was retired as a subordinate reviewer wholesale; Codex
# GPT-5.5 is now the sole external reviewer. Test 5
# (check_autonomous_agent_boundary) already asserts the file is absent;
# this check is the explicit positive coverage that the retirement
# policy stays enforced.
check_copilot_retired() {
    section "[12/12] Copilot CLI reviewer retired wholesale"
    assert_path_absent ".github/copilot-instructions.md is gone (retired)" \
        ".github/copilot-instructions.md"
    assert_path_absent "scripts/copilot-review.sh is gone (retired)" \
        "scripts/copilot-review.sh"
    # Live-invocation check: scan executable script files (.sh / .py
    # / .mjs / .js) under scripts/ and .claude/hooks/, AND
    # .claude/settings.json (a live hook-command surface that could
    # re-introduce the wrapper through a hook entry). Only flag
    # occurrences that look like invocation lines (start with "bash "
    # or contain a "subprocess." or "exec" call to the wrapper, OR
    # appear inside a settings.json string value). Plain prose mentions
    # in markdown / SKILL.md / this script's own assertion strings are
    # not invocation paths and not flagged.
    local invocation_hits
    invocation_hits="$(grep -RIE \
        '(bash|sh|exec[a-z]*|run|subprocess\.[a-zA-Z_]+\([^)]*)[[:space:]]*[\"'"'"']?scripts/copilot-review\.sh' \
        .claude/hooks scripts 2>/dev/null \
        | grep -v "test-ai-system\.sh:" || true)"
    if [ -n "$invocation_hits" ]; then
        t_fail "no live caller of scripts/copilot-review.sh remains" \
               "found invocation: $invocation_hits"
    else
        t_pass "no live caller of scripts/copilot-review.sh remains"
    fi
    # Additional: settings.json hook-command surface
    if grep -q "scripts/copilot-review\.sh" .claude/settings.json 2>/dev/null; then
        t_fail "no copilot-review.sh reference in .claude/settings.json hook commands" \
               "found wrapper path in settings.json"
    else
        t_pass "no copilot-review.sh reference in .claude/settings.json hook commands"
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
check_copilot_retired

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
        for f in "${FAILURES[@]}"; do
            echo "  FAIL: $f"
        done
    fi
    exit 1
fi
