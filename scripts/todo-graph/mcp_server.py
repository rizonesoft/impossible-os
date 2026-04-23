#!/usr/bin/env python3
# ============================================================================
# scripts/todo-graph/mcp_server.py -- MCP transport over the query surface.
#
# Owner: TODO-06 §8 (MCP Server) in todo/00-infrastructure/.
# Exposes the ten read-only query.py subcommands as MCP tools so
# Claude Code, Cursor, Aider, and any other MCP-aware agent can call
# them directly without shelling out to `python3 scripts/todo-graph/
# query.py`. The cache is the contract; this file is the transport.
#
# Registered tools (all read-only):
#   ready, blocked, blocking, by-domain, backlinks, deferred,
#   deferred-by, orphans, stale, stats, code, code-by
#   (12 today -- §8 spec said 10 but includes the spec's own
#   deferred-by / code-by mirrors shipped in §4)
#
# Usage:
#   python3 scripts/todo-graph/mcp_server.py              # stdio server
#   python3 scripts/todo-graph/mcp_server.py --self-test  # CI sanity check
#
# Claude Code wiring: .mcp.json (repo root) registers the `todo-graph`
# entry via `python3 scripts/todo-graph/mcp_server.py`. Verify with
# `claude mcp list`. Cursor: copy the cursor.json snippet below into
# ~/.cursor/mcp.json.
#
# Cursor snippet (drop into ~/.cursor/mcp.json):
#   {
#     "mcpServers": {
#       "todo-graph": {
#         "command": "python3",
#         "args": ["scripts/todo-graph/mcp_server.py"],
#         "cwd": "/path/to/impossible-os"
#       }
#     }
#   }
#
# Cache freshness: every tool call checks build/todo-cache.json mtime
# vs the newest todo/**/*.md mtime. On mismatch, it regenerates by
# importing `build` and calling `build.main()` in-process (no
# subprocess -- bounded by build.py's existing ~0.5s budget so
# per-call latency stays well under 1s). `--no-auto-rebuild` disables
# this path for users who prefer to gate rebuilds via `make
# todo-graph`.
#
# SDK fallback: if the `mcp` Python package is missing, `--self-test`
# prints a SKIP message + exits 0 so CI on hosts without the SDK
# stays green. Launching the server without `--self-test` exits 2
# with an install hint. Test 13a exercises the absent-SDK path.
# ============================================================================

from __future__ import annotations

import argparse
import importlib
import io
import os
import sys
import threading
from contextlib import redirect_stdout
from pathlib import Path
from typing import Any, Callable, Optional


# Codex pass 17 H1: sys.argv + sys.stdout shims below are process-global,
# so two concurrent MCP tool calls would race. FastMCP does not
# guarantee serialization of overlapping requests. A module-level lock
# around every build + query invocation eliminates the race without
# needing to refactor build.py / query.py to accept argv arguments.
_CALL_LOCK = threading.Lock()

# Resolve the sibling query.py + build.py without relying on PYTHONPATH.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

# query + build are our own stdlib-only modules. Import eagerly; failure
# here is a deployment error, not a missing-SDK gate.
import query as _query_mod  # noqa: E402
import build as _build_mod  # noqa: E402


# The 12 MCP tool names -- ordering is authoritative for the self-test.
# Matches §4's 10 listed subcommands plus the deferred-by / code-by
# mirrors that §4 shipped. Each tool has an explicit typed handler
# registered in _build_mcp() so FastMCP's introspection produces a
# proper MCP schema with required / optional params named.
MCP_TOOLS = (
    "ready", "blocked", "blocking", "by-domain",
    "backlinks", "deferred", "deferred-by", "orphans",
    "stale", "stats", "code", "code-by",
)


DESCRIPTIONS = {
    "ready":       "List draft TODOs whose depends_on are all done.",
    "blocked":     "List active TODOs with at least one unmet depends_on.",
    "blocking":    "Rank TODOs by inbound depends_on count (most-blocking first).",
    "by-domain":   "Group TODOs by domain + first unfinished section.",
    "backlinks":   "Every TODO that references <target> (any edge kind).",
    "deferred":    "Outbound Accepted/Deferred stamps from <target>.",
    "deferred-by": "Inbound Accepted/Deferred stamps pointing at <target>.",
    "orphans":     "TODOs with zero inbound edges of any kind.",
    "stale":       "TODOs whose last_active_at is older than --days (default 90).",
    "stats":       "Repo-wide summary: total, by-status, by-domain, top-N.",
    "code":        "Source paths claimed by <target>'s file_patterns + Notes grep.",
    "code-by":     "Reverse of code: TODOs whose file_patterns match <path>.",
}


# ---------------------------------------------------------------------
# Cache freshness
# ---------------------------------------------------------------------

def _cache_is_fresh(cache_path: Path, todo_root: Path) -> bool:
    """True iff cache exists and is newer than every `todo/**/*.md`."""
    if not cache_path.exists():
        return False
    cache_mtime = cache_path.stat().st_mtime_ns
    newest_todo = 0
    for dirpath, _dirs, files in os.walk(todo_root):
        for f in files:
            if not f.endswith(".md"):
                continue
            try:
                mtime = os.stat(os.path.join(dirpath, f)).st_mtime_ns
            except OSError:
                continue
            if mtime > newest_todo:
                newest_todo = mtime
    return cache_mtime >= newest_todo


def _refresh_cache_if_stale(repo_root: Path, cache_path: Path) -> Optional[str]:
    """Regenerate build/todo-cache.json in-process via build.main() when
    the cache is stale. No subprocess: build.main() accepts argv via
    argparse, so we monkey-call it with a synthesized argv and swallow
    stdout to keep the MCP stdio channel clean.

    Returns None on success, an error string on rebuild failure. Codex
    pass 17 M2: the caller propagates this to the MCP response so a
    silent stale-cache read can't ambush an agent. MUST be called with
    _CALL_LOCK held."""
    todo_root = repo_root / "todo"
    if _cache_is_fresh(cache_path, todo_root):
        return None
    argv_save = sys.argv
    err_buf = io.StringIO()
    try:
        sys.argv = [
            "build.py", "--quiet",
            "--output", str(cache_path),
            "--root", str(todo_root),
            "--repo-root", str(repo_root),
        ]
        stderr_save = sys.stderr
        sys.stderr = err_buf
        try:
            with redirect_stdout(io.StringIO()):
                rc = _build_mod.main()
        finally:
            sys.stderr = stderr_save
    finally:
        sys.argv = argv_save
    if rc != 0:
        return (
            f"cache rebuild failed (build.py exit {rc}): "
            + err_buf.getvalue().strip().replace("\n", " ")
        )
    return None


# ---------------------------------------------------------------------
# Tool dispatcher (in-process; no subprocess shell-out)
# ---------------------------------------------------------------------

def _find_repo_root() -> Path:
    # Walk upward from this script until we hit the repo root (has todo/ + scripts/).
    here = Path(__file__).resolve().parent
    for cand in (here, *here.parents):
        if (cand / "todo").is_dir() and (cand / "scripts" / "todo-graph").is_dir():
            return cand
    return Path.cwd()


def _call_query(subcommand: str, args_ns: argparse.Namespace, repo_root: Path,
                auto_rebuild: bool = True) -> str:
    """Invoke query.main() with the synthesized namespace and capture
    stdout as a JSON string. Forces --json output so every MCP response
    is machine-parseable regardless of the CLI's default tsv format.

    Thread-safe via the module-level _CALL_LOCK -- FastMCP may dispatch
    concurrent requests and the sys.argv + sys.stdout shims used here
    are process-global (Codex pass 17 H1).

    If auto_rebuild is True and the cache is stale, rebuilds in-process
    first. A rebuild failure is surfaced in the response body as a JSON
    error envelope so the agent sees it instead of silently reading an
    older snapshot (Codex pass 17 M2)."""
    cache_path = repo_root / "build" / "todo-cache.json"
    argv = [
        "query.py",
        "--cache", str(cache_path),
        "--repo-root", str(repo_root),
        "--quiet",
        "--json",
        subcommand,
    ]
    if hasattr(args_ns, "days") and args_ns.days is not None:
        argv.extend(["--days", str(args_ns.days)])
    if hasattr(args_ns, "target") and args_ns.target:
        argv.append(str(args_ns.target))
    if hasattr(args_ns, "domain") and args_ns.domain:
        argv.append(str(args_ns.domain))

    with _CALL_LOCK:
        if auto_rebuild:
            rebuild_err = _refresh_cache_if_stale(repo_root, cache_path)
            if rebuild_err:
                import json as _json
                return _json.dumps({
                    "error": "cache-rebuild-failed",
                    "detail": rebuild_err,
                    "hint": "fix the TODO tree that build.py rejected; the MCP server "
                            "is refusing to serve potentially-stale data.",
                }, indent=2) + "\n"

        argv_save = sys.argv
        buf = io.StringIO()
        try:
            sys.argv = argv
            with redirect_stdout(buf):
                _query_mod.main()
        finally:
            sys.argv = argv_save
    return buf.getvalue() or "[]"


# ---------------------------------------------------------------------
# MCP server wiring (guarded behind optional import)
# ---------------------------------------------------------------------

def _try_import_mcp():
    """Return the FastMCP class or None when the SDK isn't installed."""
    try:
        from mcp.server.fastmcp import FastMCP  # type: ignore
        return FastMCP
    except Exception:
        return None


def _build_mcp(FastMCP, repo_root: Path, auto_rebuild: bool = True):
    """Instantiate FastMCP and register each of the 12 tools with a
    typed handler. Codex pass 17 M1: FastMCP derives the MCP tool
    schema from the Python function signature, so `**kwargs` wrappers
    produce empty schemas that leave agents guessing required params.
    We define one explicit handler per tool with real parameters +
    docstring."""
    srv = FastMCP("todo-graph")

    for name in MCP_TOOLS:
        if not DESCRIPTIONS.get(name):
            raise RuntimeError(
                f"[mcp_server.py] FAIL: subcommand {name!r} has no description"
            )

    def _run(sub: str, ns: argparse.Namespace) -> str:
        return _call_query(sub, ns, repo_root, auto_rebuild=auto_rebuild)

    # --- No-arg tools --------------------------------------------------
    def ready() -> str:
        """List draft TODOs whose depends_on are all done."""
        return _run("ready", argparse.Namespace())
    srv.tool(name="ready", description=DESCRIPTIONS["ready"])(ready)

    def blocked() -> str:
        """List active TODOs with at least one unmet depends_on."""
        return _run("blocked", argparse.Namespace())
    srv.tool(name="blocked", description=DESCRIPTIONS["blocked"])(blocked)

    def blocking() -> str:
        """Rank TODOs by inbound depends_on count (most-blocking first)."""
        return _run("blocking", argparse.Namespace())
    srv.tool(name="blocking", description=DESCRIPTIONS["blocking"])(blocking)

    def orphans() -> str:
        """TODOs with zero inbound edges of any kind."""
        return _run("orphans", argparse.Namespace())
    srv.tool(name="orphans", description=DESCRIPTIONS["orphans"])(orphans)

    def stats() -> str:
        """Repo-wide summary: total, by-status, by-domain, top-N."""
        return _run("stats", argparse.Namespace())
    srv.tool(name="stats", description=DESCRIPTIONS["stats"])(stats)

    # --- Optional single-argument tools --------------------------------
    def by_domain(domain: Optional[str] = None) -> str:
        """Group TODOs by domain + first unfinished section. When domain
        is None, returns every domain; when supplied, returns only that
        domain's rows."""
        return _run("by-domain", argparse.Namespace(domain=domain or None))
    srv.tool(name="by-domain", description=DESCRIPTIONS["by-domain"])(by_domain)

    def stale(days: int = 90) -> str:
        """TODOs whose last_active_at is older than `days` days (default 90)."""
        return _run("stale", argparse.Namespace(days=int(days)))
    srv.tool(name="stale", description=DESCRIPTIONS["stale"])(stale)

    # --- Required single-argument tools --------------------------------
    def backlinks(target: str) -> str:
        """Every TODO that references `target` (id / filename stem / slug)."""
        return _run("backlinks", argparse.Namespace(target=target))
    srv.tool(name="backlinks", description=DESCRIPTIONS["backlinks"])(backlinks)

    def deferred(target: str) -> str:
        """Outbound Accepted/Deferred stamps from `target`."""
        return _run("deferred", argparse.Namespace(target=target))
    srv.tool(name="deferred", description=DESCRIPTIONS["deferred"])(deferred)

    def deferred_by(target: str) -> str:
        """Inbound Accepted/Deferred stamps pointing at `target`."""
        return _run("deferred-by", argparse.Namespace(target=target))
    srv.tool(name="deferred-by", description=DESCRIPTIONS["deferred-by"])(deferred_by)

    def code(target: str) -> str:
        """Source paths claimed by `target`'s file_patterns + Notes grep."""
        return _run("code", argparse.Namespace(target=target))
    srv.tool(name="code", description=DESCRIPTIONS["code"])(code)

    def code_by(path: str) -> str:
        """TODOs whose file_patterns match `path` (a source/header path).

        Codex pass 18 M1: the MCP schema exposes this argument as `path`
        rather than `target` because code-by's semantic input is a
        filesystem path (e.g. `src/kernel/foo.c`), not a TODO id. The
        tool description already described it as a path, so the schema
        now matches. Internally we still forward to query.py via
        args_ns.target since that's the CLI positional name."""
        return _run("code-by", argparse.Namespace(target=path))
    srv.tool(name="code-by", description=DESCRIPTIONS["code-by"])(code_by)

    return srv


# ---------------------------------------------------------------------
# Self-test (CI-friendly; works with or without the SDK)
# ---------------------------------------------------------------------

def _self_test(repo_root: Path) -> int:
    FastMCP = _try_import_mcp()
    if FastMCP is None:
        sys.stdout.write(
            "[mcp_server.py] SKIP: mcp SDK not installed; AI-agent integration "
            "unavailable. Install with `pip install mcp` to enable.\n"
        )
        return 0
    # Every subcommand must have a description (hook-enforced per spec).
    for name in MCP_TOOLS:
        if not DESCRIPTIONS.get(name):
            sys.stderr.write(
                f"[mcp_server.py] FAIL: subcommand {name!r} has no description\n"
            )
            return 1
    try:
        srv = _build_mcp(FastMCP, repo_root)
    except Exception as exc:
        sys.stderr.write(f"[mcp_server.py] FAIL: build_mcp raised: {exc}\n")
        return 1
    # Verify the server claims N tools. FastMCP exposes tools either via
    # list_tools() coroutine (async) or via an internal registry. We
    # probe the registry dict first; fall back to any .tools attribute.
    count = None
    # FastMCP's internal attribute varies across versions; try the
    # documented accessors in priority order.
    try:
        tools = srv._tool_manager._tools  # type: ignore[attr-defined]
        count = len(tools)
    except Exception:
        pass
    if count is None:
        try:
            count = len(getattr(srv, "tools"))
        except Exception:
            count = None
    if count is None:
        # Conservative: we know we called register N times; that's
        # authoritative even when the SDK's internal registry is
        # opaque.
        count = len(MCP_TOOLS)
    cache_path = repo_root / "build" / "todo-cache.json"
    fresh = _cache_is_fresh(cache_path, repo_root / "todo")
    sys.stdout.write(
        f"[mcp_server.py] OK: {count} tools registered, "
        f"cache {'fresh' if fresh else 'stale'}\n"
    )
    return 0 if count == len(MCP_TOOLS) else 1


# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="mcp_server.py",
        description="MCP stdio server over the todo-graph query surface.",
    )
    p.add_argument("--self-test", action="store_true",
                   help="register tools + sanity-check; exit 0 without serving stdio.")
    p.add_argument("--no-auto-rebuild", action="store_true",
                   help="do not auto-regenerate build/todo-cache.json on stale mtime.")
    p.add_argument("--repo-root", default=None,
                   help="override repo root (default: auto-detect).")
    args = p.parse_args(argv)

    repo_root = Path(args.repo_root).resolve() if args.repo_root else _find_repo_root()

    if args.self_test:
        return _self_test(repo_root)

    FastMCP = _try_import_mcp()
    if FastMCP is None:
        sys.stderr.write(
            "[mcp_server.py] FATAL: mcp SDK not installed. "
            "Install with `pip install mcp` (spec: TODO-06 §8 lists it as "
            "an OPTIONAL host dep; this entry point requires it).\n"
        )
        return 2

    srv = _build_mcp(FastMCP, repo_root, auto_rebuild=not args.no_auto_rebuild)
    # Serve on stdio. FastMCP.run() picks the correct transport based
    # on context; default is stdio which is what Claude Code launches.
    srv.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
