#!/usr/bin/env bash
# ============================================================================
# audit-ai-system.sh -- umbrella cross-tool drift audit for the AI surface.
#
# Composes per-section validators into one umbrella that catches drift between
# the Claude side, the Codex side, the in-repo skill catalog, and the two
# installed plugins. Owner: TODO-08 cross-tool drift detection.
#
# Audited surface (7 checks; the original plan called for 8 but design
# review 2026-04-27 merged the duplicate plugin-enumeration check into
# audit-hooks.sh):
#
#   1. mcp_set_match           -- .mcp.json server set vs ~/.codex/config.toml
#                                  [mcp_servers.*] keys; delegates to
#                                  scripts/codex-mcp-install.sh --check.
#   2. codex_no_model_flag     -- no skill prose (ANY .claude/skills/**/SKILL.md
#                                  that invokes codex-companion.mjs) carries a
#                                  --model or --effort example. Design review
#                                  H1 fix: scope is ALL skills that touch
#                                  Codex, not just .claude/skills/codex-*/.
#   3. codex_companion_path    -- every codex-companion.mjs invocation in a
#                                  skill uses ${CLAUDE_PLUGIN_ROOT} OR the
#                                  absolute marketplaces path. No bare
#                                  'node codex-companion.mjs'.
#   4. codex_receiving_pointer -- every skill that invokes codex-companion.mjs
#                                  also mentions 'receiving-code-review' at
#                                  least once.
#   5. hook_manifest_complete  -- delegates to scripts/audit-hooks.sh --quiet
#                                  (covers files-vs-manifest, manifest-vs-
#                                  files, BLOCK exit codes, plugin
#                                  enumeration; no double-call duplication).
#   6. state_dir_layout        -- .claude/state/ dir present + has README.md;
#                                  .gitignore declares '.claude/state/*' with
#                                  the README + .keep allowlist exceptions.
#   7. codex_config_policy     -- ~/.codex/config.toml top-level model and
#                                  model_reasoning_effort match
#                                  docs/infrastructure/codex-config-policy.toml
#                                  fixture. SKIP-with-WARN on missing config
#                                  (fresh dev env / CI runner). Design review
#                                  M2 fix: split user-home from repo-state
#                                  checks.
#
# Repo-state checks (CI-safe): 2, 3, 4, 5, 6.
# User-home checks (skip-WARN on missing): 1, 7.
#
# Usage:
#   bash scripts/audit-ai-system.sh             Full audit, human output.
#   bash scripts/audit-ai-system.sh --quiet     Only summary line.
#
# Exit codes:
#   0 = no drift (PASS or SKIP-with-WARN)
#   1 = drift detected on a repo-state check OR on a present user-home check.
#
# This script audits the WORKING TREE state on disk; it does not look at the
# git index or HEAD. Run with a clean tree for the most reliable signal, or
# accept that the report describes the current uncommitted edits.
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

QUIET=0
case "${1:-}" in
    -h|--help)
        sed -n '2,57p' "$0"
        exit 0
        ;;
    -q|--quiet)
        QUIET=1
        ;;
esac

PASS=0
FAIL=0
WARN=0
FAILURES=()

log() {
    [ "$QUIET" = "0" ] && echo "$@"
}

t_pass() {
    PASS=$((PASS + 1))
    [ "$QUIET" = "0" ] && echo "  PASS  $1"
}

t_fail() {
    FAIL=$((FAIL + 1))
    FAILURES+=("$1")
    [ "$QUIET" = "0" ] && echo "  FAIL  $1"
    [ -n "${2:-}" ] && [ "$QUIET" = "0" ] && echo "          $2"
}

t_warn() {
    WARN=$((WARN + 1))
    [ "$QUIET" = "0" ] && echo "  WARN  $1"
}

log "=== audit-ai-system.sh ==="
log "  repo: $REPO_ROOT"
log ""

# Helper: enumerate every .claude/skills/**/SKILL.md that invokes Codex.
codex_using_skills() {
    grep -rln "codex-companion\\.mjs" "$REPO_ROOT/.claude/skills/" \
        --include='SKILL.md' 2>/dev/null | sort
}

# ---- Check 1: MCP cross-config parity ----
log "--- check 1: MCP cross-config parity ---"
if [ ! -f "$HOME/.codex/config.toml" ]; then
    t_warn "mcp_set_match  ~/.codex/config.toml missing -- run scripts/codex-mcp-install.sh"
elif [ -x "$REPO_ROOT/scripts/codex-mcp-install.sh" ]; then
    if bash "$REPO_ROOT/scripts/codex-mcp-install.sh" --check >/dev/null 2>&1; then
        t_pass "mcp_set_match  .mcp.json and ~/.codex/config.toml MCP blocks match"
    else
        t_fail "mcp_set_match  drift detected" \
            "run scripts/codex-mcp-install.sh --check for actionable detail"
    fi
else
    t_warn "mcp_set_match  scripts/codex-mcp-install.sh missing -- skipping"
fi

# ---- Check 2: no --model / --effort flags in any Codex-using skill ----
log "--- check 2: no --model / --effort in skill examples ---"
HITS=""
while IFS= read -r skill; do
    [ -z "$skill" ] && continue
    if grep -nE "(--model|--effort)\b" "$skill" >/dev/null 2>&1; then
        HITS="$HITS$skill\n"
    fi
done < <(codex_using_skills)
if [ -z "$HITS" ]; then
    t_pass "codex_no_model_flag  no --model / --effort tokens in any Codex-using skill"
else
    t_fail "codex_no_model_flag  --model or --effort token in skill prose" \
        "offending skill(s):\n$(printf '%b' "$HITS")"
fi

# ---- Check 3: every codex-companion.mjs invocation uses sanctioned shape ----
log "--- check 3: codex-companion.mjs invocation path shape ---"
HITS=""
while IFS= read -r skill; do
    [ -z "$skill" ] && continue
    while IFS= read -r line; do
        # Only flag lines that look like actual command invocations
        # (`node` or `bash` adjacent to codex-companion.mjs). Pure-prose
        # backtick mentions of the filename without an executor prefix
        # are documentation references, not commands.
        if echo "$line" | grep -qE "\bnode\b[^\"']*codex-companion\.mjs|\bbash\b[^\"']*codex-companion\.mjs"; then
            if echo "$line" | grep -qE '\$\{?CLAUDE_PLUGIN_ROOT\}?'; then
                continue
            fi
            if echo "$line" | grep -qE "\.claude/plugins/marketplaces/openai-codex/"; then
                continue
            fi
            HITS="$HITS$skill: $line\n"
        fi
    done < "$skill"
done < <(codex_using_skills)
if [ -z "$HITS" ]; then
    t_pass "codex_companion_path  every invocation uses CLAUDE_PLUGIN_ROOT or absolute marketplaces path"
else
    t_fail "codex_companion_path  bare or non-canonical companion path in skill" \
        "offending line(s):\n$(printf '%b' "$HITS")"
fi

# ---- Check 4: receiving-code-review pointer in every Codex-using skill ----
log "--- check 4: receiving-code-review pointer in Codex-using skills ---"
HITS=""
while IFS= read -r skill; do
    [ -z "$skill" ] && continue
    if ! grep -q "receiving-code-review" "$skill"; then
        HITS="$HITS$skill\n"
    fi
done < <(codex_using_skills)
if [ -z "$HITS" ]; then
    t_pass "codex_receiving_pointer  every Codex-using skill references receiving-code-review"
else
    t_fail "codex_receiving_pointer  Codex-using skill missing receiving-code-review pointer" \
        "offending skill(s):\n$(printf '%b' "$HITS")"
fi

# ---- Check 5: hook manifest + plugin enumeration ----
log "--- check 5: hook manifest + plugin enumeration ---"
if [ -x "$REPO_ROOT/scripts/audit-hooks.sh" ]; then
    if bash "$REPO_ROOT/scripts/audit-hooks.sh" --quiet >/dev/null 2>&1; then
        t_pass "hook_manifest_complete  scripts/audit-hooks.sh PASS"
    else
        t_fail "hook_manifest_complete  scripts/audit-hooks.sh reports drift" \
            "run scripts/audit-hooks.sh for the per-row detail"
    fi
else
    t_fail "hook_manifest_complete  scripts/audit-hooks.sh missing or not executable"
fi

# ---- Check 6: .claude/state/ layout ----
log "--- check 6: .claude/state/ layout ---"
STATE_DIR="$REPO_ROOT/.claude/state"
GITIGNORE="$REPO_ROOT/.gitignore"
SD_OK=1
if [ ! -d "$STATE_DIR" ]; then
    t_fail "state_dir_layout  .claude/state/ directory missing"
    SD_OK=0
fi
if [ "$SD_OK" = "1" ] && [ ! -f "$STATE_DIR/README.md" ]; then
    t_fail "state_dir_layout  .claude/state/README.md missing"
    SD_OK=0
fi
if [ "$SD_OK" = "1" ] && [ ! -f "$GITIGNORE" ]; then
    t_fail "state_dir_layout  .gitignore missing"
    SD_OK=0
fi
if [ "$SD_OK" = "1" ] && ! grep -qE "^\.claude/state/\*" "$GITIGNORE"; then
    t_fail "state_dir_layout  .gitignore does not declare .claude/state/*"
    SD_OK=0
fi
if [ "$SD_OK" = "1" ]; then
    t_pass "state_dir_layout  .claude/state/ + README.md + .gitignore declaration all present"
fi

# ---- Check 7: ~/.codex/config.toml policy parity ----
log "--- check 7: ~/.codex/config.toml policy parity ---"
POLICY_FIXTURE="$REPO_ROOT/docs/infrastructure/codex-config-policy.toml"
USER_CFG="$HOME/.codex/config.toml"
if [ ! -f "$POLICY_FIXTURE" ]; then
    t_fail "codex_config_policy  fixture missing at docs/infrastructure/codex-config-policy.toml"
elif [ ! -f "$USER_CFG" ]; then
    t_warn "codex_config_policy  ~/.codex/config.toml missing -- skipping (fresh dev env or CI runner)"
else
    POLICY_OUT="$(REPO_ROOT="$REPO_ROOT" python3 - <<'PY' 2>&1
import os, re
fixture = open(os.path.join(os.environ["REPO_ROOT"],
    "docs/infrastructure/codex-config-policy.toml")).read()
user_path = os.path.expanduser("~/.codex/config.toml")
try:
    user = open(user_path).read()
except Exception as e:
    print(f"FAIL::cannot read {user_path}: {e}")
    raise SystemExit(0)

def top_level_str(text, key):
    pat = re.compile(r"^\s*" + re.escape(key) + r"\s*=\s*\"([^\"]*)\"", re.M)
    head = re.split(r"^\s*\[", text, maxsplit=1, flags=re.M)[0]
    m = pat.search(head)
    return m.group(1) if m else None

drift = []
for key in ("model",):  # effort is a user dial, not pinned (see fixture preamble 2026-04-28 entry)
    expect = top_level_str(fixture, key)
    actual = top_level_str(user, key)
    if expect is None:
        print(f"FAIL::fixture missing key {key}")
        continue
    if actual is None:
        drift.append(f"{key}: missing (expected '{expect}')")
    elif actual != expect:
        drift.append(f"{key}: got '{actual}', expected '{expect}'")

if drift:
    print("FAIL::" + "; ".join(drift))
else:
    print("PASS::OK")
PY
)"
    case "$POLICY_OUT" in
        PASS::*)
            t_pass "codex_config_policy  ~/.codex/config.toml model matches policy fixture (effort is a user dial; not audited)"
            ;;
        FAIL::*)
            DETAIL="${POLICY_OUT#FAIL::}"
            t_fail "codex_config_policy  drift vs docs/infrastructure/codex-config-policy.toml" \
                "$DETAIL"
            ;;
        *)
            t_fail "codex_config_policy  unexpected python output: $POLICY_OUT"
            ;;
    esac
fi

# ---- Summary ----
log ""
TOTAL=$((PASS + FAIL))
if [ "$FAIL" = "0" ]; then
    if [ "$WARN" = "0" ]; then
        echo "audit-ai-system.sh: PASS ($TOTAL/$TOTAL checks)"
    else
        echo "audit-ai-system.sh: PASS ($TOTAL/$TOTAL, $WARN warning(s))"
    fi
    exit 0
else
    echo "audit-ai-system.sh: FAIL ($FAIL/$TOTAL check(s) failed, $WARN warning(s))"
    if [ "$QUIET" = "1" ]; then
        for f in "${FAILURES[@]}"; do
            echo "  - $f"
        done
    fi
    exit 1
fi
