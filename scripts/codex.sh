#!/usr/bin/env bash
# =====================================================================
# scripts/codex.sh
#
# Per-workspace Codex CLI wrapper. Injects the impossible-os MCP
# server set into every `codex` invocation via `-c 'mcp_servers.*='`
# overrides, sourced from the repo-tracked fixture
# `docs/infrastructure/codex-mcp.config.toml`.
#
# Why this exists: Codex CLI 0.126 has no per-project `.codex/
# config.toml` -- MCP wiring lives only in user-global
# `~/.codex/config.toml`. This wrapper makes the repo the single
# source of truth, no install step required:
#   bash scripts/codex.sh exec "..."   # MCP servers wired automatically
#   bash scripts/codex.sh mcp list     # shows wrapper-injected servers
#   bash scripts/codex.sh login        # passthrough; no MCP needed but no harm
#
# Coexists with the installer:
#   - Installer (`scripts/codex-mcp-install.sh`) writes to
#     `~/.codex/config.toml` so the interactive `codex` TUI sees the
#     servers without any wrapper. Use for daily TUI work.
#   - Wrapper (this script) injects per-invocation, no global write.
#     Use for `codex exec` from CI / scripts / shell aliases, or on
#     hosts where touching `~/.codex/config.toml` is undesirable.
#
# Both paths read from the SAME fixture
# `docs/infrastructure/codex-mcp.config.toml`, so they cannot drift.
#
# Owner: TODO-08-automation-hardening section 1 follow-up.
#
# IMPORTANT caveat: Codex CLI 0.126.0-alpha.1's `codex exec` mode
# rejects MCP tool calls client-side as "user cancelled MCP tool
# call" (the MCP subprocess is never spawned). This is upstream alpha
# behavior, not a wrapper bug. Interactive `bash scripts/codex.sh`
# (TUI) does work since the user manually approves each tool call.
# Track upstream Codex CLI for a stable release that fixes
# `codex exec` MCP-tool-approval semantics.
# =====================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURE="$REPO_ROOT/docs/infrastructure/codex-mcp.config.toml"

if [ ! -f "$FIXTURE" ]; then
    echo "[codex.sh] FATAL: fixture missing at $FIXTURE" >&2
    echo "[codex.sh] hint: run from a clean impossible-os checkout" >&2
    exit 2
fi

if ! command -v codex >/dev/null 2>&1; then
    echo "[codex.sh] FATAL: 'codex' binary not found on PATH" >&2
    echo "[codex.sh] hint: install Codex CLI per the official docs" >&2
    exit 2
fi

# Read the fixture and emit one `-c key=value` arg per server. The
# value is a TOML inline table (`{key1 = val1, key2 = [...], ...}`)
# which Codex CLI's -c parser accepts (per `codex --help`: "value
# is parsed as TOML").
#
# cwd is computed at INVOCATION time (REPO_ROOT) instead of being
# read from the fixture, so a contributor running this from a
# different checkout path on a different machine still gets the
# correct absolute cwd. The fixture's hardcoded cwd value is for
# the installer path; the wrapper always wins on cwd.
#
# Output format (one line per server, suitable for $() capture):
#   -c<TAB>mcp_servers.<name>={...}
# Tab separator avoids the bash word-splitting trap on TOML values
# that contain spaces.
# Capture the Python output through $() so its exit status is
# observable. The previous revision used `mapfile -t < <(python3 ...)`
# which loses the python exit status -- bash's process-substitution
# exit code is the mapfile's own (always 0), so a python exit 2
# would silently produce an empty array and the wrapper would
# fail-open into plain `codex` with no MCP overrides. (Codex
# adversarial review of the wrapper, High.)
PY_OUT=$(python3 - "$FIXTURE" "$REPO_ROOT" <<'PY'
import sys
from pathlib import Path

try:
    import tomllib
except ImportError:
    sys.stderr.write(
        "[codex.sh] FATAL: python3 missing tomllib (need 3.11+)\n"
    )
    sys.exit(2)

fixture_path = Path(sys.argv[1])
repo_root = sys.argv[2]

try:
    with open(fixture_path, "rb") as f:
        fixture = tomllib.load(f)
except tomllib.TOMLDecodeError as exc:
    sys.stderr.write(
        f"[codex.sh] FATAL: fixture is invalid TOML ({fixture_path}): "
        f"{exc}\n"
    )
    sys.exit(2)
servers = fixture.get("mcp_servers") or {}
if not servers:
    sys.stderr.write(
        f"[codex.sh] FATAL: fixture has no [mcp_servers.*] tables\n"
    )
    sys.exit(2)


def toml_inline_value(v):
    """Render a Python value as a TOML inline-table-compatible
    literal. Strings are double-quoted with backslash escapes; lists
    recurse; bool/int are str()'d; None / floats unsupported (not
    used in our fixture)."""
    if isinstance(v, str):
        # TOML basic-string escapes: \\, \", \b, \f, \n, \r, \t, \uXXXX
        out = v.replace("\\", "\\\\").replace('"', '\\"')
        out = (out.replace("\b", "\\b").replace("\f", "\\f")
                  .replace("\n", "\\n").replace("\r", "\\r")
                  .replace("\t", "\\t"))
        return f'"{out}"'
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, list):
        return "[" + ", ".join(toml_inline_value(x) for x in v) + "]"
    sys.stderr.write(
        f"[codex.sh] FATAL: fixture has unsupported value type "
        f"{type(v).__name__} for {v!r}\n"
    )
    sys.exit(2)


for name in sorted(servers.keys()):
    block = dict(servers[name])
    # Override cwd with the live REPO_ROOT so the wrapper works on
    # any checkout path / machine. The fixture's cwd is the installer
    # contract; the wrapper supersedes it per-invocation.
    block["cwd"] = repo_root
    parts = []
    for k in sorted(block.keys()):
        parts.append(f"{k} = {toml_inline_value(block[k])}")
    inline = "{" + ", ".join(parts) + "}"
    # Tab separator so bash mapfile + word-split doesn't choke on
    # spaces inside the inline table.
    sys.stdout.write(f"-c\tmcp_servers.{name}={inline}\n")
PY
)
PY_RC=$?
if [ "$PY_RC" != "0" ]; then
    echo "[codex.sh] FATAL: override renderer exited $PY_RC; refusing" \
         "to run codex without MCP overrides" >&2
    exit "$PY_RC"
fi
if [ -z "$PY_OUT" ]; then
    echo "[codex.sh] FATAL: override renderer produced no output;" \
         "fixture may be empty" >&2
    exit 2
fi

# Now feed the captured output into mapfile via a here-string. This
# preserves the lines AND lets us assert non-empty before exec.
mapfile -t MCP_OVERRIDES <<< "$PY_OUT"
if [ "${#MCP_OVERRIDES[@]}" -eq 0 ]; then
    echo "[codex.sh] FATAL: zero MCP override lines after parse;" \
         "wrapper would fail open" >&2
    exit 2
fi

# Build the final argv: for each line in MCP_OVERRIDES, split on TAB
# into TWO words (the literal "-c" and the "key=value" payload). Then
# append the user's args verbatim so any `codex` subcommand + flag
# combination passes through.
codex_argv=()
for line in "${MCP_OVERRIDES[@]}"; do
    # Bash split-on-TAB via IFS scoped to this read.
    IFS=$'\t' read -r flag value <<< "$line"
    codex_argv+=("$flag" "$value")
done
codex_argv+=("$@")

exec codex "${codex_argv[@]}"
