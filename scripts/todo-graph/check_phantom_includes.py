#!/usr/bin/env python3
"""scripts/todo-graph/check_phantom_includes.py -- Check 8 worker.

Lint-side consumer of the lsp-bridge MCP `diagnostics` tool. Iterates
every `src/kernel/**/*.c` file (excluding test/ and synthetic/), calls
the bridge's `diagnostics(path)` tool, filters clangd findings whose
code is in PHANTOM_CODES, and emits one line per phantom include in the
canonical `path:line: warning: unused include "<header>"` shape that
scripts/lint.sh Check 8 dispatches to error()/warn().

Architecture: spawn `python3 scripts/lsp-mcp/bridge.py` as an MCP
stdio subprocess and speak newline-delimited JSON-RPC. The bridge
warm-starts clangd-19 once and answers diagnostics calls per file.
clangd publishes diagnostics asynchronously after didOpen, so the
wrapper polls per-file with a bounded deadline; an empty diagnostics
list with `note` is "not yet published", not "clean".

Exit codes:
  0 -- ran successfully (with or without findings); findings on stdout
  2 -- MCP server unavailable (bridge spawn failed, mcp SDK missing)
  3 -- clangd-19 not installed (graceful skip; lint emits skip-WARN)

Allowlist marker: `// PHANTOM-INCLUDE-OK: <reason>` on the same source
line as the `#include` suppresses a finding (mirrors Check 6's
`/* TEST-TAUTOLOGY-OK: ... */` and Check 7's `/* INTENTIONAL-STUB: */`).

Usage:
  python3 scripts/todo-graph/check_phantom_includes.py [--root <dir>]
                                                       [--limit N]
                                                       [--paths file ...]
"""

import argparse
import json
import os
import re
import select
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
BRIDGE = REPO_ROOT / "scripts" / "lsp-mcp" / "bridge.py"

# clangd diagnostic codes that mean "this include contributes no
# referenced symbols to the TU". The clangd unused-includes check
# emits `unused-includes`; the clang-tidy misc-include-cleaner check
# emits `misc-include-cleaner`. Both are phantom-include for our
# lint policy (drop the include or annotate it).
PHANTOM_CODES = {
    "unused-includes",
    "unused-include",
    "unused-header",
    "misc-include-cleaner",
}

# Two-phase polling for clangd publishDiagnostics. clangd indexes
# asynchronously after didOpen; a sequential per-file 6s wait would
# multiply across 287 TUs into ~30 minutes worst case. Codex perf
# review caught this. Strategy:
#   Round 1 -- fire diagnostics(path) for every TU. Keep results
#              that are already published; queue the rest.
#   Round 2..N -- re-poll only the still-unpublished set; each round
#              sleeps once globally then re-issues diagnostics calls.
# Each round caps at MAX_ROUNDS, gated by GLOBAL_DEADLINE_S so a
# hung clangd can never wedge the sweep past the bar. Per-file
# wait equals only the ROUND_INTERVAL cumulative across rounds.
MAX_ROUNDS = 20
ROUND_INTERVAL = 0.3
GLOBAL_DEADLINE_S = 300.0
RPC_TIMEOUT = 90.0

ALLOWLIST_RE = re.compile(r"//\s*PHANTOM-INCLUDE-OK\b")
INCLUDE_RE = re.compile(r'#\s*include\s+[<"]([^<>"]+)[>"]')
# clangd unused-includes: "included header foo.h is not used directly"
# misc-include-cleaner: "included header foo.h is not used"
CLANGD_HEADER_RE = re.compile(
    r'header\s+([^\s\']+?)(?:\s+is\s+not\s+used|\'?\s*$)'
)


def _eprint(msg: str) -> None:
    sys.stderr.write(f"[check_phantom_includes] {msg}\n")
    sys.stderr.flush()


class MCPClient:
    """Minimal MCP stdio JSON-RPC client. Wraps a long-running
    bridge.py subprocess; tools/call is synchronous (the bridge
    serializes per-LSP via _CALL_LOCK)."""

    def __init__(self, proc: subprocess.Popen) -> None:
        self._proc = proc
        self._next_id = 1

    def _send(self, msg: dict) -> None:
        line = json.dumps(msg) + "\n"
        assert self._proc.stdin is not None
        self._proc.stdin.write(line)
        self._proc.stdin.flush()

    def _recv(self, want_id: Optional[int], timeout: float) -> Optional[dict]:
        assert self._proc.stdout is not None
        deadline = time.time() + timeout
        while time.time() < deadline:
            remaining = max(0.05, deadline - time.time())
            r, _, _ = select.select([self._proc.stdout], [], [], remaining)
            if not r:
                continue
            line = self._proc.stdout.readline()
            if not line:
                return None
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue
            if want_id is None or obj.get("id") == want_id:
                return obj
        return None

    def initialize(self) -> bool:
        nid = self._next_id
        self._next_id += 1
        self._send({
            "jsonrpc": "2.0",
            "id": nid,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {
                    "name": "check-phantom-includes",
                    "version": "1",
                },
            },
        })
        resp = self._recv(nid, RPC_TIMEOUT)
        if resp is None or "result" not in resp:
            return False
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return True

    def call_tool(self, name: str, arguments: dict) -> Optional[dict]:
        nid = self._next_id
        self._next_id += 1
        self._send({
            "jsonrpc": "2.0",
            "id": nid,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        })
        resp = self._recv(nid, RPC_TIMEOUT)
        if resp is None or "result" not in resp:
            return None
        result = resp["result"]
        # FastMCP wraps tool returns in
        # {content:[{type:"text",text:JSON}], isError}.
        content = result.get("content")
        if isinstance(content, list) and content and "text" in content[0]:
            try:
                return json.loads(content[0]["text"])
            except json.JSONDecodeError:
                return None
        return result

    def shutdown(self) -> None:
        try:
            self._proc.terminate()
            self._proc.wait(timeout=3.0)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            try:
                self._proc.kill()
            except ProcessLookupError:
                pass


def _kernel_c_files(root: Path) -> list:
    """Every src/kernel/**/*.c that is a real translation unit.
    Excludes test/ (legacy lint pattern) and synthetic/."""
    base = root / "src" / "kernel"
    if not base.is_dir():
        return []
    out = []
    for c in base.rglob("*.c"):
        rel = c.relative_to(root).as_posix()
        if "/test/" in rel or rel.endswith("_synthetic.c"):
            continue
        out.append(c)
    return sorted(out)


def _allowlisted_line(path: Path, line_idx: int) -> bool:
    """True iff the source line at `line_idx` (0-based, matching LSP
    range.start.line) carries the PHANTOM-INCLUDE-OK marker."""
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                if i == line_idx:
                    return bool(ALLOWLIST_RE.search(line))
                if i > line_idx:
                    break
    except OSError:
        return False
    return False


def _read_line_at(path: Path, line_idx: int) -> str:
    try:
        with path.open("r", encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f):
                if i == line_idx:
                    return line
                if i > line_idx:
                    break
    except OSError:
        return ""
    return ""


def _extract_header(diag_message: str, source_line: str) -> str:
    """Extract the header path from the clangd unused-include message
    or, failing that, from the cited source line itself."""
    m = CLANGD_HEADER_RE.search(diag_message or "")
    if m:
        return m.group(1)
    m2 = INCLUDE_RE.search(source_line or "")
    if m2:
        return m2.group(1)
    return "<unknown>"


def _diagnostics_once(client, rel):
    """Single diagnostics call; returns (status, payload_or_detail)
    where status is "ok" / "pending" / "error". 'payload_or_detail' is
    the response dict on ok/pending, the error kind string on error."""
    payload = client.call_tool("diagnostics", {"path": rel})
    if payload is None:
        return "error", "rpc-recv-failed"
    # Bridge returns {error: kind, detail: ..., **extra} on LSP
    # failure (see scripts/lsp-mcp/lsp_client.py LspError.to_envelope).
    # Codex adversarial review High.
    if "error" in payload:
        return "error", str(payload.get("error"))
    if payload.get("note"):
        return "pending", payload
    return "ok", payload


def _findings_from_payload(c_path, rel, payload):
    """Filter PHANTOM_CODES diagnostics, drop allowlisted lines,
    return the formatted warning lines."""
    diagnostics = payload.get("diagnostics") or []
    findings = []
    for d in diagnostics:
        code = d.get("code")
        if code not in PHANTOM_CODES:
            continue
        rng = d.get("range") or {}
        start = rng.get("start") or {}
        line_idx = start.get("line")
        if not isinstance(line_idx, int):
            continue
        if _allowlisted_line(c_path, line_idx):
            continue
        source_line = _read_line_at(c_path, line_idx)
        header = _extract_header(d.get("message", ""), source_line)
        findings.append(
            f'{rel}:{line_idx + 1}: warning: unused include "{header}"'
        )
    return findings


def main(argv=None):
    p = argparse.ArgumentParser(
        description=("Phantom-include detector: calls lsp-bridge "
                     "diagnostics(path) on every kernel TU and "
                     "reports clangd unused-include findings."),
    )
    p.add_argument("--root", default=str(REPO_ROOT),
                   help="repo root (default: auto-detected)")
    p.add_argument("--limit", type=int, default=0,
                   help="cap the number of files processed (0 = all)")
    p.add_argument("--paths", nargs="*", default=None,
                   help="explicit list of TU paths (overrides default sweep)")
    args = p.parse_args(argv)

    repo_root = Path(args.root).resolve()
    if not BRIDGE.is_file():
        _eprint(f"bridge.py not found at {BRIDGE}; is the repo intact?")
        return 2

    # The bridge spawns `clangd-19` specifically (see
    # scripts/lsp-mcp/servers/clangd_server.py CLANGD_BIN), not a
    # generic `clangd`. Accepting either here would let a host with
    # only generic clangd through the preflight, then the bridge
    # would fail with `lsp-binary-missing` and return an error
    # envelope -- which the diagnostics path would otherwise treat
    # as clean. Codex adversarial review High.
    if shutil.which("clangd-19") is None:
        _eprint("clangd-19 not installed; skipping phantom-include check")
        return 3

    if args.paths:
        files = [Path(p_).resolve() for p_ in args.paths]
    else:
        files = _kernel_c_files(repo_root)
    if args.limit and args.limit > 0:
        files = files[: args.limit]
    if not files:
        return 0

    env = os.environ.copy()
    env.setdefault("PYTHONUNBUFFERED", "1")
    try:
        proc = subprocess.Popen(
            [sys.executable, str(BRIDGE),
             "--warm-start=c", "--warm-start-mode=blocking"],
            cwd=str(repo_root),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            env=env,
        )
    except OSError as e:
        _eprint(f"failed to spawn bridge.py: {e}")
        return 2

    client = MCPClient(proc)
    try:
        if not client.initialize():
            _eprint("MCP initialize handshake failed; bridge may lack the "
                    "fastmcp SDK or crashed on warm-start")
            return 2

        # Build the (rel, c_path) list, dropping anything outside the
        # workspace root.
        targets = []
        for c_path in files:
            try:
                rel = c_path.resolve().relative_to(repo_root).as_posix()
            except ValueError:
                continue
            targets.append((rel, c_path))

        # Round 1: fire diagnostics for every target. Files that come
        # back ok or error are settled; pending files queue for round 2+.
        # Two-phase strategy bounds total wait time at MAX_ROUNDS *
        # ROUND_INTERVAL globally, instead of per-file. Codex perf
        # review medium.
        pending = []
        errors = []
        deadline = time.time() + GLOBAL_DEADLINE_S
        for rel, c_path in targets:
            status, payload_or_detail = _diagnostics_once(client, rel)
            if status == "ok":
                for line in _findings_from_payload(c_path, rel, payload_or_detail):
                    print(line)
            elif status == "pending":
                pending.append((rel, c_path))
            else:  # status == "error"
                errors.append((rel, payload_or_detail))

        # Rounds 2..MAX_ROUNDS: sleep once per round, then re-poll the
        # pending set. Each successful publish leaves the pending list.
        # clangd has been indexing in the background while we issued
        # round 1, so most files publish on round 2.
        round_idx = 1
        while pending and round_idx < MAX_ROUNDS and time.time() < deadline:
            time.sleep(ROUND_INTERVAL)
            round_idx += 1
            still_pending = []
            for rel, c_path in pending:
                if time.time() >= deadline:
                    still_pending.append((rel, c_path))
                    continue
                status, payload_or_detail = _diagnostics_once(client, rel)
                if status == "ok":
                    for line in _findings_from_payload(c_path, rel, payload_or_detail):
                        print(line)
                elif status == "pending":
                    still_pending.append((rel, c_path))
                else:
                    errors.append((rel, payload_or_detail))
            pending = still_pending

        unpublished = [rel for rel, _ in pending]

        if unpublished:
            _eprint(
                f"clangd never published diagnostics for "
                f"{len(unpublished)} file(s) within "
                f"{POLL_ATTEMPTS * POLL_INTERVAL:.1f}s each; "
                f"first: {unpublished[0]}"
            )
        if errors:
            # Bridge reported an LSP error for at least one TU. The
            # most common shape is `lsp-binary-missing` (clangd-19
            # vanished mid-run) or `lsp-crashed`. Surface the first
            # one and exit 2 so lint.sh emits a deferred-WARN rather
            # than a false-clean result.
            first_rel, first_kind = errors[0]
            _eprint(
                f"lsp-bridge returned error for {len(errors)} file(s); "
                f"first: {first_rel} ({first_kind})"
            )
            return 2
        return 0
    finally:
        client.shutdown()


if __name__ == "__main__":
    sys.exit(main())
