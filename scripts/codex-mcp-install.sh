#!/usr/bin/env bash
# =====================================================================
# scripts/codex-mcp-install.sh
#
# Install (or re-install) the impossible-os MCP server blocks into
# `~/.codex/config.toml`. Idempotent: running twice is a no-op.
#
# The canonical block content lives in
# `docs/infrastructure/codex-mcp.config.toml`; this script merges those
# two `[mcp_servers.<name>]` blocks into the user's Codex CLI config,
# preserving any non-MCP keys (model, project trust, etc.) the user has
# set.
#
# Owner: TODO-08-automation-hardening section 1.
# Validator: scripts/test-tooling.sh `mcp_drift` sub-test asserts the
# fixture + the user's `~/.codex/config.toml` agree on every key.
#
# Usage:
#   bash scripts/codex-mcp-install.sh         # merge / refresh
#   bash scripts/codex-mcp-install.sh --check # exit 0 if in sync, 1 if not
#
# The --check mode is the dry-run probe the drift validator delegates to.
# =====================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FIXTURE="$REPO_ROOT/docs/infrastructure/codex-mcp.config.toml"
TARGET="${HOME}/.codex/config.toml"

CHECK_ONLY=0
if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=1
elif [ -n "${1:-}" ]; then
    echo "usage: $0 [--check]" >&2
    exit 2
fi

if [ ! -f "$FIXTURE" ]; then
    echo "[codex-mcp-install] FATAL: fixture missing at $FIXTURE" >&2
    echo "[codex-mcp-install] hint: run from repo root, or restore" \
         "docs/infrastructure/codex-mcp.config.toml" >&2
    exit 2
fi

# Use python3 + tomllib (stdlib in Python 3.11+) for the merge so we
# do not pull a TOML dependency. Ubuntu 24.04 ships python3.12; the
# bridge already requires python3.10+ for ContextVar copy semantics
# in the structured-logging path, so the floor is effectively 3.11
# here too.
exec python3 - "$FIXTURE" "$TARGET" "$CHECK_ONLY" <<'PY'
import sys
import os
import re
from pathlib import Path

try:
    import tomllib
except ImportError:
    sys.stderr.write(
        "[codex-mcp-install] FATAL: python3 missing tomllib (need 3.11+)\n"
    )
    sys.exit(2)

fixture_path = Path(sys.argv[1])
target_path = Path(sys.argv[2])
check_only = sys.argv[3] == "1"

fixture_bytes = fixture_path.read_bytes()
fixture_doc = tomllib.loads(fixture_bytes.decode("utf-8"))
expected = fixture_doc.get("mcp_servers") or {}
expected_names = sorted(expected.keys())
if not expected_names:
    sys.stderr.write(
        "[codex-mcp-install] FATAL: fixture has no [mcp_servers.*] tables\n"
    )
    sys.exit(2)

target_path.parent.mkdir(parents=True, exist_ok=True)
if target_path.exists():
    target_bytes = target_path.read_bytes()
else:
    target_bytes = b""

# Parse the target so we can diff per-server. Tolerate empty file.
# Catch parse errors with a structured envelope per Codex post-ship
# consistency review L: the previous revision propagated
# tomllib.TOMLDecodeError as an unstructured Python traceback,
# breaking the documented [codex-mcp-install] FATAL/DRIFT/OK
# envelope contract that the test-tooling drift validator embeds
# in its t_fail message.
try:
    target_doc = tomllib.loads(target_bytes.decode("utf-8") or "")
except tomllib.TOMLDecodeError as exc:
    sys.stderr.write(
        f"[codex-mcp-install] FATAL: target config is invalid TOML "
        f"({target_path}): {exc}\n"
    )
    sys.stderr.write(
        "[codex-mcp-install] hint: open the file, fix the TOML "
        "syntax error, and re-run. The installer never overwrites "
        "an unparsable target.\n"
    )
    sys.exit(2)
actual = target_doc.get("mcp_servers") or {}

drift = []
for name in expected_names:
    exp = expected[name]
    act = actual.get(name)
    if act is None:
        drift.append(f"missing block [mcp_servers.{name}]")
        continue
    # Compare BOTH directions per Codex post-impl adversarial review M:
    # missing keys (target lacks something fixture has) AND extra keys
    # (target has something fixture does not). The fixture is the
    # full contract for OUR servers; an extra key inside an owned
    # block is drift even when the fixture's keys all match. Personal
    # MCP servers (different name) are still allowed unconditionally
    # since they fall outside the expected_names loop.
    exp_keys = set(exp.keys())
    act_keys = set(act.keys())
    missing = exp_keys - act_keys
    extra = act_keys - exp_keys
    for key in sorted(missing):
        drift.append(
            f"[mcp_servers.{name}].{key}: missing "
            f"(expected {exp[key]!r})"
        )
    for key in sorted(extra):
        drift.append(
            f"[mcp_servers.{name}].{key}: unexpected key "
            f"(value {act[key]!r}; not in fixture)"
        )
    for key in sorted(exp_keys & act_keys):
        if act[key] != exp[key]:
            drift.append(
                f"[mcp_servers.{name}].{key}: "
                f"expected {exp[key]!r}, got {act[key]!r}"
            )
# Extra MCP servers in the target are allowed -- the user may have
# wired a personal credentialed server that's outside this repo's
# scope. We only enforce that OUR two blocks match the fixture
# exactly. (Drift validator separately enforces no rogue server
# appears on the .mcp.json side.)

if check_only:
    if drift:
        sys.stderr.write(
            "[codex-mcp-install] DRIFT detected against the fixture:\n"
        )
        for d in drift:
            sys.stderr.write(f"  - {d}\n")
        sys.stderr.write(
            "[codex-mcp-install] hint: bash scripts/codex-mcp-install.sh "
            "(without --check) to refresh\n"
        )
        sys.exit(1)
    print(f"[codex-mcp-install] OK: {len(expected_names)} servers in sync "
          f"({', '.join(expected_names)})")
    sys.exit(0)

if not drift:
    print(f"[codex-mcp-install] OK: {len(expected_names)} servers already "
          f"in sync ({', '.join(expected_names)}) -- no-op")
    sys.exit(0)

# Apply the merge: rewrite the target file by stripping any existing
# blocks for our names and appending the fixture's block content
# verbatim. We do NOT re-emit via tomllib.dumps (no such function in
# stdlib) because (a) we want byte-for-byte fidelity to the fixture's
# comments and formatting, (b) tomli_w is not stdlib.
def strip_blocks_for_names(text, names):
    """Remove every `[mcp_servers.<name>]` table block whose name is
    in `names`. A block runs from its header line until the next
    TOML table boundary -- which the spec defines as ANY line
    starting with `[` (table) or `[[` (array-of-tables), with the
    key allowed to be a bare `[A-Za-z0-9_-]+`, a dotted bare key,
    OR a quoted key containing arbitrary characters. The previous
    revision only matched the bare-key form, which meant a user
    config containing a quoted-key table or an array-of-tables
    header right after one of our owned blocks could have its data
    silently dropped during a refresh. (Codex post-impl adversarial
    review M.)

    Boundary detector accepts any of:
        [bare.dotted.key]
        [[bare.dotted.key]]
        ["quoted key with spaces"]
        [[ "quoted" .key ]]
        [ key . "mixed" ]
    Implemented by scanning for the unindented `[` opener and then
    consuming through the matched `]` (or `]]`) on the same line --
    TOML headers cannot span lines, so a single-line scan is
    sufficient. Comments (#) and whitespace inside the header are
    legal per TOML 1.0, but not multi-line content.
    """
    name_set = set(names)
    lines = text.splitlines(keepends=True)
    out = []
    i = 0
    drop_block = False

    # Match the start of ANY TOML table or array-of-tables header
    # on a line. We don't need to fully parse the key to know it's
    # a boundary -- presence of `[` or `[[` at line start (after
    # optional whitespace) is enough.
    boundary_start = re.compile(r'^\s*\[\[?')

    def _parse_owned_table_key(stripped):
        """Return the owned-server name if `stripped` is a single-table
        TOML header `[mcp_servers . <name>]` whose name part is in the
        owned set, else None. Handles all four TOML-equivalent forms
        the spec allows for a 2-segment dotted key:
            [mcp_servers.todo-graph]              (bare . bare)
            [mcp_servers."todo-graph"]            (bare . quoted)
            ["mcp_servers".todo-graph]            (quoted . bare)
            ["mcp_servers"."todo-graph"]          (quoted . quoted)
        With arbitrary whitespace around the dot. Comments after the
        `]` are tolerated. Array-of-tables (`[[..]]`) is NOT treated
        as an owned block (we only own single tables) but still acts
        as a block boundary at the caller. Codex post-ship adversarial
        review High caught the previous detector missing the quoted-
        prefix and whitespace forms, which would have left a stale
        owned block in place during a refresh and produced invalid
        TOML after the fixture was appended."""
        # Quick reject: array-of-tables headers.
        if stripped.startswith("[["):
            return None
        # Strip optional trailing comment.
        head = re.split(r'\s*#', stripped, maxsplit=1)[0].rstrip()
        if not (head.startswith("[") and head.endswith("]")):
            return None
        inside = head[1:-1].strip()
        # Match exactly two segments separated by `.` with optional
        # whitespace around the dot. Each segment is bare
        # ([A-Za-z0-9_-]+) or quoted ("..." with no embedded ").
        seg = r'(?:[A-Za-z0-9_-]+|"[^"]+")'
        m = re.match(rf'^\s*({seg})\s*\.\s*({seg})\s*$', inside)
        if not m:
            return None
        prefix, name = m.group(1), m.group(2)
        # Strip surrounding quotes if present.
        if prefix.startswith('"'):
            prefix = prefix[1:-1]
        if name.startswith('"'):
            name = name[1:-1]
        if prefix != "mcp_servers":
            return None
        if name not in name_set:
            return None
        return name

    while i < len(lines):
        line = lines[i]
        if boundary_start.match(line):
            stripped = line.strip()
            owned = _parse_owned_table_key(stripped)
            if owned is not None:
                drop_block = True
                i += 1
                continue
            else:
                drop_block = False
        if not drop_block:
            out.append(line)
        i += 1
    return "".join(out)

# Read the fixture's BLOCK CONTENT (everything from the first
# [mcp_servers. table onwards). The fixture's leading prose comments
# are not propagated into the target -- they belong to the fixture's
# self-documentation, not to every user's Codex config.
fixture_text = fixture_bytes.decode("utf-8")
m = re.search(r'^\[mcp_servers\.', fixture_text, re.M)
if m is None:
    sys.stderr.write(
        "[codex-mcp-install] FATAL: fixture has no [mcp_servers.*] header\n"
    )
    sys.exit(2)
fixture_blocks = fixture_text[m.start():]

old_target = target_bytes.decode("utf-8") if target_bytes else ""
stripped = strip_blocks_for_names(old_target, expected_names).rstrip()
if stripped and not stripped.endswith("\n"):
    stripped += "\n"

merged = stripped
if merged and not merged.endswith("\n\n"):
    merged += "\n"
merged += fixture_blocks
if not merged.endswith("\n"):
    merged += "\n"

# Validate the merged TOML BEFORE the atomic replace. Codex post-ship
# adversarial review High: the previous order wrote .tmp, called
# os.replace, THEN parsed merged -- meaning a corrupt merge (e.g.
# duplicate-table failure when the boundary detector failed to strip
# a stale owned block in an unusual TOML form) would already have
# overwritten the user's good config. Now we parse first; only on
# successful parse + presence verification do we touch the target.
try:
    verify = tomllib.loads(merged)
except tomllib.TOMLDecodeError as exc:
    sys.stderr.write(
        f"[codex-mcp-install] FATAL: merged config would be invalid "
        f"TOML, ABORTED before writing {target_path}: {exc}\n"
    )
    sys.stderr.write(
        "[codex-mcp-install] hint: this usually means a stale "
        "[mcp_servers.<name>] block in your config used a TOML form "
        "the boundary detector missed; please report with the lines "
        "near the parse error.\n"
    )
    sys.exit(2)
verify_actual = verify.get("mcp_servers") or {}
for name in expected_names:
    if name not in verify_actual:
        sys.stderr.write(
            f"[codex-mcp-install] FATAL: merged config missing "
            f"[mcp_servers.{name}] after parse, ABORTED before "
            f"writing {target_path}\n"
        )
        sys.exit(2)

# Atomic write: write to .tmp, fsync, rename. The merged content
# has already been validated as TOML and confirmed to contain every
# expected server, so a successful rename guarantees the target is
# usable.
tmp_path = target_path.with_suffix(target_path.suffix + ".tmp")
tmp_path.write_text(merged, encoding="utf-8")
# Open the .tmp by fd to get a writeable handle for fsync. The
# previous O_RDONLY+fsync pattern flushes inode metadata + data on
# Linux but is non-portable; the explicit fd close + fsync of a
# writable fd is the documented POSIX pattern.
fd = os.open(str(tmp_path), os.O_RDONLY)
try:
    os.fsync(fd)
finally:
    os.close(fd)
os.replace(str(tmp_path), str(target_path))

print(f"[codex-mcp-install] OK: wrote {len(expected_names)} server "
      f"blocks ({', '.join(expected_names)}) to {target_path}")
print("[codex-mcp-install] hint: confirm with `codex mcp list`")
PY
