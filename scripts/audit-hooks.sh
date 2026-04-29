#!/usr/bin/env bash
# ============================================================================
# audit-hooks.sh -- cross-check hook system for drift.
#
# Compares four sources of truth:
#   1. Files on disk under .claude/hooks/
#   2. Rows in .claude/hooks/MANIFEST.md
#   3. Hook command paths in .claude/settings.json
#   4. Plugin-side hooks under ~/.claude/plugins/cache/*/hooks/hooks.json
#      (cross-checked against the per-plugin `### <name>@<marketplace>`
#      subsections of MANIFEST.md)
#
# Reports:
#   - Hook file present in dir but not in MANIFEST.md
#   - MANIFEST.md row pointing at a missing file
#   - settings.json command referencing a non-existent hook
#   - settings.json command referencing a hook absent from MANIFEST.md
#   - BLOCK hook whose file lacks a sys.exit(2) / return 2 path despite
#     containing stderr.write (allowlist: top-of-file `# block-via:
#     permissionDecision` comment)
#   - Installed plugin lacking a matching MANIFEST subsection heading
#   - Plugin event not enumerated in its MANIFEST subsection
#
# Usage:
#   bash scripts/audit-hooks.sh             Full audit, human-readable output
#   bash scripts/audit-hooks.sh --quiet     Only summary line + non-zero on drift
#
# Exit codes:
#   0 = no drift
#   1 = drift detected
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
export REPO_ROOT

QUIET=0
case "${1:-}" in
    -h|--help)
        sed -n '2,32p' "$0"
        exit 0
        ;;
    -q|--quiet)
        QUIET=1
        ;;
esac

HOOKS_DIR="$REPO_ROOT/.claude/hooks"
MANIFEST="$HOOKS_DIR/MANIFEST.md"
SETTINGS="$REPO_ROOT/.claude/settings.json"

DRIFT=0
DRIFT_LINES=()

log() {
    [ "$QUIET" = "0" ] && echo "$@"
}

drift() {
    DRIFT=$((DRIFT + 1))
    DRIFT_LINES+=("$1")
    [ "$QUIET" = "0" ] && echo "  DRIFT  $1"
}

if [ ! -f "$MANIFEST" ]; then
    echo "FATAL: MANIFEST.md missing at $MANIFEST" >&2
    exit 1
fi
if [ ! -f "$SETTINGS" ]; then
    echo "FATAL: settings.json missing at $SETTINGS" >&2
    exit 1
fi

log "=== audit-hooks.sh ==="
log "  hooks dir: $HOOKS_DIR"
log "  manifest:  $MANIFEST"
log "  settings:  $SETTINGS"
log ""

# ----------------------------------------------------------------------------
# Check 1: every file in .claude/hooks/ has a manifest row.
# ----------------------------------------------------------------------------
log "--- check 1: files-in-dir vs manifest ---"
while IFS= read -r f; do
    base="$(basename "$f")"
    case "$base" in
        __pycache__|README*|MANIFEST.md) continue ;;
        # Underscore-prefixed files are shared helper modules
        # (Python private-module convention), not standalone hooks.
        # Examples: _review_kind.py -- shared classifier consumed by
        # phase1_evidence_gate + skill_step_map (review-pipeline pre-Codex
        # enforcement work). Skip; manifest rows describe hook-as-hook.
        _*) continue ;;
    esac
    if ! grep -qF "\`.claude/hooks/$base\`" "$MANIFEST"; then
        drift "hook file present but not in MANIFEST.md: $base"
    fi
done < <(find "$HOOKS_DIR" -maxdepth 1 -type f \( -name "*.py" -o -name "*.sh" \) | sort)

# ----------------------------------------------------------------------------
# Check 2: every manifest row points at an existing file.
# ----------------------------------------------------------------------------
log "--- check 2: manifest rows vs files-in-dir ---"
while IFS= read -r ref; do
    rel="${ref//\`/}"
    if [ ! -f "$REPO_ROOT/$rel" ]; then
        drift "MANIFEST.md row references missing file: $rel"
    fi
done < <(grep -oE '`\.claude/hooks/[A-Za-z0-9_./-]+`' "$MANIFEST" | sort -u)

# ----------------------------------------------------------------------------
# Check 3: every settings.json hook command references an existing hook,
# and that hook is enumerated in MANIFEST.md.
# ----------------------------------------------------------------------------
log "--- check 3: settings.json commands vs manifest ---"
PY_OUT="$(python3 - <<'PY' 2>&1
import json, os, re
root = os.environ.get("REPO_ROOT", ".")
try:
    with open(os.path.join(root, ".claude/settings.json")) as f:
        cfg = json.load(f)
except Exception as e:
    print(f"DRIFT::settings.json parse error: {e}")
    raise SystemExit(0)
with open(os.path.join(root, ".claude/hooks/MANIFEST.md")) as f:
    manifest_text = f.read()

def walk(node):
    if isinstance(node, dict):
        if "command" in node and isinstance(node["command"], str):
            yield node["command"]
        for v in node.values():
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)

hook_re = re.compile(r"\.claude/hooks/([A-Za-z0-9_.-]+\.(?:py|sh))")
seen = set()
for cmd in walk(cfg):
    for hit in hook_re.findall(cmd):
        rel = f".claude/hooks/{hit}"
        if rel in seen:
            continue
        seen.add(rel)
        if not os.path.exists(os.path.join(root, rel)):
            print(f"DRIFT::settings.json references missing hook file: {rel}")
            continue
        if f"`{rel}`" not in manifest_text:
            print(f"DRIFT::settings.json references hook not in MANIFEST.md: {rel}")
PY
)"
while IFS= read -r line; do
    case "$line" in
        DRIFT::*) drift "${line#DRIFT::}" ;;
    esac
done <<<"$PY_OUT"

# ----------------------------------------------------------------------------
# Check 4: every BLOCK hook's file uses sys.exit(2) / return 2 somewhere.
# Allowlist: hooks declaring `# block-via: permissionDecision` in top 5 lines.
# ----------------------------------------------------------------------------
log "--- check 4: BLOCK hook exit codes ---"
for f in "$HOOKS_DIR"/*.py; do
    [ -f "$f" ] || continue
    base="$(basename "$f")"
    if ! grep -q "stderr\.write" "$f"; then
        continue
    fi
    if head -5 "$f" | grep -qE "block-via: (permissionDecision|warning-only)"; then
        continue
    fi
    if ! grep -qE "(sys\.exit\(2\)|return 2)" "$f"; then
        drift "BLOCK hook missing sys.exit(2) / return 2: $base"
    fi
done

# ----------------------------------------------------------------------------
# Check 5: plugin-side hooks enumerated in MANIFEST.md.
# Strict mode: each installed plugin must have a `### <name>@<mkt>`
# subsection heading. If the plugin ships hooks.json, every event name
# in that file must appear in the subsection block.
# ----------------------------------------------------------------------------
log "--- check 5: plugin-side hooks ---"
PLUGINS_INDEX="$HOME/.claude/plugins/installed_plugins.json"
if [ -f "$PLUGINS_INDEX" ]; then
    PLUGIN_OUT="$(python3 - <<'PY' 2>&1
import json, os, re
home = os.path.expanduser("~/.claude/plugins")
index = os.path.join(home, "installed_plugins.json")
manifest = os.path.join(os.environ.get("REPO_ROOT", "."), ".claude/hooks/MANIFEST.md")
with open(manifest) as f:
    mtext = f.read()
try:
    with open(index) as f:
        data = json.load(f)
except Exception:
    raise SystemExit(0)

plugins = data.get("plugins", {}) if isinstance(data, dict) else {}

# Manifest convention: each installed plugin gets a subsection heading
# `### <name>@<marketplace>` (optionally backticked, optionally followed
# by version text). The @-form is unique by definition; substring on the
# bare plugin name can false-pass when one plugin's name is a substring
# of another.
heading_re = re.compile(r'^###\s+`?([A-Za-z0-9_.-]+@[A-Za-z0-9_.-]+)`?', re.MULTILINE)
manifest_headings = set(heading_re.findall(mtext))

for full_id, instances in plugins.items():
    if not isinstance(instances, list) or not instances:
        continue
    inst = instances[0]
    install_path = inst.get("installPath", "") if isinstance(inst, dict) else ""
    hooks_json = os.path.join(install_path, "hooks", "hooks.json") if install_path else ""
    has_hooks = bool(hooks_json) and os.path.exists(hooks_json)

    if full_id not in manifest_headings:
        if has_hooks:
            print(f"DRIFT::plugin {full_id} ships hooks/hooks.json but no '### {full_id}' heading in MANIFEST.md")
        else:
            print(f"DRIFT::installed plugin {full_id} has no '### {full_id}' heading in MANIFEST.md (add a 'no hooks shipped' subsection)")
        continue

    if not has_hooks:
        continue

    try:
        with open(hooks_json) as f:
            hooks_cfg = json.load(f)
    except Exception:
        print(f"DRIFT::plugin {full_id} hooks.json failed to parse: {hooks_json}")
        continue
    events = list((hooks_cfg.get("hooks", {}) or {}).keys())

    # Extract the subsection block: from this heading to next ### or end.
    pattern = r'^###\s+`?' + re.escape(full_id) + r'`?[^\n]*\n(.*?)(?=^###\s|\Z)'
    m = re.search(pattern, mtext, re.MULTILINE | re.DOTALL)
    block = m.group(1) if m else ""
    for ev in events:
        if ev not in block:
            print(f"DRIFT::plugin {full_id} event {ev} not enumerated in its MANIFEST.md subsection")
PY
)"
    while IFS= read -r line; do
        case "$line" in
            DRIFT::*) drift "${line#DRIFT::}" ;;
        esac
    done <<<"$PLUGIN_OUT"
else
    log "  (no plugin index at $PLUGINS_INDEX -- skipping)"
fi

# ----------------------------------------------------------------------------
# Review-pipeline passthrough drift check (TODO-08 prefix-allowlist
# standardization section). The shared helper at
# .claude/hooks/_review_pipeline_passthrough.py owns the canonical
# review-pipeline prefix list; PreToolUse gates that need that policy
# MUST consume it via is_review_pipeline_passthrough() rather than
# reimplementing the prefix tuple inline. Drift = a hook with its own
# `cmd.startswith((...git...bash scripts/...))` tuple that didn't
# migrate to the helper.
# ----------------------------------------------------------------------------
log ""
log "Checking review-pipeline-passthrough consumers..."
RPP_DRIFT=$(python3 - <<'PYEOF'
import pathlib, re
# Multi-prefix allowlist tuple containing review-pipeline prefixes is
# the drift signature. The regex uses DOTALL so it matches multi-line
# tuples (a hand-formatted shape with `cmd.startswith((` on one line
# and prefix entries on later lines). Single-prefix startswith calls
# (`cmd.startswith("git commit")`) are NOT this drift -- the regex
# requires `((` (double-open) which only appears with a tuple arg.
# Closes the section-29 perf re-dispatch M finding.
TUPLE_RE = re.compile(
    r'startswith\s*\(\s*\((?:[^)]*?)'
    r'''(?:["']git ["']|["']bash scripts/["']|["']python3 ["']|["']node ["'])''',
    re.DOTALL,
)
for f in pathlib.Path(".claude/hooks").glob("*.py"):
    if f.name.startswith("_") or f.name.startswith("test_"):
        continue
    text = f.read_text()
    # Skip files that consume the helper -- they are migrated.
    if "_review_pipeline_passthrough" in text or \
       "is_review_pipeline_passthrough" in text:
        continue
    m = TUPLE_RE.search(text)
    if m:
        # Compute the line number of the match start.
        ln_no = text[:m.start()].count("\n") + 1
        print(f"DRIFT::review-pipeline prefix-allowlist tuple in {f}:{ln_no} not using _review_pipeline_passthrough helper")
PYEOF
)
while IFS= read -r line; do
    case "$line" in
        DRIFT::*) drift "${line#DRIFT::}" ;;
    esac
done <<<"$RPP_DRIFT"

# ----------------------------------------------------------------------------
# Summary
# ----------------------------------------------------------------------------
log ""
if [ "$DRIFT" = "0" ]; then
    echo "audit-hooks.sh: PASS (no drift)"
    exit 0
else
    echo "audit-hooks.sh: FAIL ($DRIFT drift item(s))"
    if [ "$QUIET" = "1" ]; then
        for d in "${DRIFT_LINES[@]}"; do
            echo "  - $d"
        done
    fi
    exit 1
fi
