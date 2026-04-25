#!/usr/bin/env bash
# ============================================================================
# scripts/lsp-mcp/tests/test_boundary.sh -- read-only LSP boundary audit
# (TODO-07 in 00-infrastructure).
#
# The bridge proxies up to five LSPs through 6 read-only MCP tools. Per the
# autonomous-agent boundary in TODO-02, write-capable LSP methods must NEVER
# be reachable from any MCP tool handler. This test enforces that contract
# at source-audit time:
#
#   1. Imports _FORBIDDEN_LSP_METHODS from scripts/lsp-mcp/lsp_client.py
#      (single source of truth -- the runtime gate's own deny set is the
#      same set we audit against).
#   2. Greps the bridge + servers for those exact method literals appearing
#      in lsp.request() / .notify() call sites. Any hit is a violation.
#   3. Confirms the documented contract (4 methods minimum: rename,
#      codeAction, applyEdit, executeCommand). Adds tolerated-grow-only
#      semantics: extending the deny set is fine; shrinking it requires an
#      explicit policy change in TODO-02.
#
# Defense in depth:
#   - Runtime gate: LspSubprocess.request() rejects any forbidden method
#     even if a future contributor's code constructs the name dynamically.
#   - Source audit (this test): catches literal method-name uses our code
#     might cargo-cult into a tool handler.
#   - Test 7e in test_bridge.sh exercises the runtime gate; this script
#     exercises the source audit.
#
# Run: `bash scripts/lsp-mcp/tests/test_boundary.sh`
# Exits 0 with `[boundary] OK: 0 write-capable LSP methods reachable
# from MCP surface`. Exits 1 on any source-audit violation.
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$REPO_ROOT"

# Pull the deny set from lsp_client.py at runtime so the test can never
# drift from the runtime gate. Codex design review of the LSP-MCP
# integration proposed this rather than hand-maintaining a duplicate
# list -- keeps the source of truth in one place.
FORBIDDEN_METHODS_RAW=$(python3 -c "
import sys
sys.path.insert(0, 'scripts/lsp-mcp')
from lsp_client import _FORBIDDEN_LSP_METHODS
for m in sorted(_FORBIDDEN_LSP_METHODS):
    print(m)
")

# Canonical minimum set: extending is fine, shrinking flags a policy
# change. Every method here MUST appear in _FORBIDDEN_LSP_METHODS.
EXPECTED_MIN=(
    "textDocument/rename"
    "textDocument/codeAction"
    "workspace/applyEdit"
    "workspace/executeCommand"
)
for required in "${EXPECTED_MIN[@]}"; do
    if ! grep -qxF "$required" <<< "$FORBIDDEN_METHODS_RAW"; then
        printf '[boundary] FAIL: required-deny method %q is NOT in _FORBIDDEN_LSP_METHODS.\n' "$required" >&2
        printf '            See lsp_client.py and TODO-02 autonomous-agent boundary.\n' >&2
        exit 1
    fi
done

# Now audit the source for literal method-name use in lsp.request() /
# .notify() call sites. Build the regex from the runtime deny set so a
# future addition to the set is automatically picked up.
PATTERN=""
while IFS= read -r m; do
    [ -z "$m" ] && continue
    # Escape any regex metachars (slashes are fine, but be defensive).
    esc=$(printf '%s' "$m" | sed 's/[][\.^$*+?()|{}]/\\&/g')
    if [ -z "$PATTERN" ]; then
        PATTERN="\"$esc\""
    else
        PATTERN="${PATTERN}|\"$esc\""
    fi
done <<< "$FORBIDDEN_METHODS_RAW"

# Files in scope: bridge + per-language spawners. Exclude lsp_client.py
# from the bridge audit because it OWNS _FORBIDDEN_LSP_METHODS (the
# strings appear there as legitimate set members, not as call-site
# arguments).
SOURCE_GLOB=(
    scripts/lsp-mcp/bridge.py
    scripts/lsp-mcp/servers/*.py
)

VIOLATIONS=$(grep -nE "$PATTERN" "${SOURCE_GLOB[@]}" 2>/dev/null || true)
if [ -n "$VIOLATIONS" ]; then
    printf '[boundary] FAIL: write-capable LSP method literal(s) found in MCP-tool source:\n' >&2
    printf '%s\n' "$VIOLATIONS" >&2
    printf '\n' >&2
    printf 'Per TODO-02 autonomous-agent boundary, no MCP tool handler may issue\n' >&2
    printf 'rename / codeAction-execute / applyEdit / executeCommand. The runtime\n' >&2
    printf '_FORBIDDEN_LSP_METHODS gate at lsp_client.py:request() will reject the\n' >&2
    printf 'call at runtime; this source audit prevents the literal from shipping\n' >&2
    printf 'in the first place. Remove the literal or, if a legitimate read-only\n' >&2
    printf 'use exists (currently none), update _FORBIDDEN_LSP_METHODS together\n' >&2
    printf 'with TODO-02 boundary policy.\n' >&2
    exit 1
fi

n_methods=$(printf '%s\n' "$FORBIDDEN_METHODS_RAW" | grep -c .)
printf '[boundary] OK: 0 write-capable LSP methods reachable from MCP surface '
printf '(%d method(s) in _FORBIDDEN_LSP_METHODS deny set).\n' "$n_methods"
exit 0
