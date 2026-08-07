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
# `claude mcp list`. Other MCP-aware agents can register via the
# standard `mcpServers` block in scripts/todo-graph/mcp.json.
#
# Cache freshness: every tool call checks build/todo-cache.json mtime
# vs the newest todo/**/*.md mtime. THIS IS THIS SERVER'S OWN RULE, and it
# is deliberately NOT the shared one: `cache_schema.check_freshness`
# replaced mtime ordering with a recorded corpus binding (TODO-06 section
# 21) precisely because a wall-clock comparison is invertible by a clock
# step. Routing this transport is owned by the query-transport section of
# the TODO-metadata-layer plan, whose "Make the MCP transport propagate the
# failure instead of swallowing it" item covers this file. On mismatch, it
# regenerates by
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
import json
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


def _clamp_limit(raw: Any) -> int:
    """Map any caller-supplied limit into [1, MCP_MAX_ROW_LIMIT].

    Absent, zero, negative and non-integer all collapse to the default.
    Zero is query.py's 'complete set' sentinel, so collapsing it here is
    what makes an unbounded result unreachable from the MCP surface."""
    try:
        v = int(raw)
    except (TypeError, ValueError):
        return _query_mod.DEFAULT_ROW_LIMIT
    if v <= 0:
        return _query_mod.DEFAULT_ROW_LIMIT
    return min(v, _query_mod.MCP_MAX_ROW_LIMIT)


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

    # --- Output bound (always applied, never opt-out) -------------------
    # The MCP surface serves a model context, so an unbounded result set
    # can never be requested through it. query.py's `--limit 0` (complete
    # set) is deliberately unreachable here: _clamp_limit maps 0, a
    # negative, or a non-integer onto the default, and caps the maximum.
    # Verbs that are unbounded by shape (stats) ignore these flags in
    # query.py, so passing them is harmless and keeps this path uniform.
    if subcommand not in _query_mod.UNBOUNDED_SUBCOMMANDS:
        argv.extend(["--limit", str(_clamp_limit(getattr(args_ns, "limit", None)))])
        scope = getattr(args_ns, "scope", None)
        if scope:
            argv.extend(["--scope", str(scope)])
        fields = getattr(args_ns, "fields", None)
        if fields:
            argv.extend(["--fields", str(fields)])
        offset = getattr(args_ns, "offset", None)
        if offset:
            argv.extend(["--offset", str(int(offset))])

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
        rc = 0
        try:
            sys.argv = argv
            with redirect_stdout(buf):
                try:
                    # THE RETURN VALUE IS THE VERDICT and was previously
                    # discarded entirely. `main()` RETURNS its exit code
                    # (2 = unusable cache, 3 = ceiling breach) and only
                    # `sys.exit()`s on some paths, so catching SystemExit
                    # alone saw a fail-closed refusal as success.
                    rc = _query_mod.main() or 0
                except SystemExit as exc:
                    # query.py exits nonzero for an unresolvable target and
                    # for a fail-closed ceiling breach. Both already wrote
                    # their machine-readable body to the captured stdout,
                    # so keep that body instead of letting SystemExit
                    # escape and kill the tool call -- but keep the CODE too.
                    rc = exc.code if isinstance(exc.code, int) else 1
        finally:
            sys.argv = argv_save
    return _envelope_or_body(buf.getvalue(), rc)


def _looks_like_error_envelope(body: str) -> bool:
    """True when the body query.py already wrote IS a machine-readable error.

    Only a JSON object carrying an `error` key counts. A successful `[]` result
    array is a legitimate empty answer and must NOT be mistaken for one, which
    is the whole distinction this module previously could not make."""
    stripped = body.strip()
    if not stripped.startswith("{"):
        return False
    try:
        parsed = json.loads(stripped)
    except ValueError:
        return False
    return isinstance(parsed, dict) and "error" in parsed


def _envelope_or_body(body: str, rc: int) -> str:
    """Return query.py's body, or a structured error envelope when it failed.

    THE `or "[]"` THIS REPLACES WAS THE DEFECT (TODO-06 section 22). When
    query.py refused and wrote nothing to stdout, `buf.getvalue() or "[]"`
    handed the agent an EMPTY RESULT ARRAY -- indistinguishable from "your
    query matched nothing". An agent cannot tell "no results" from "the cache
    was unusable", so it proceeds on an answer that was never computed. Making
    the CLI fail closed without fixing this would have shipped the appearance
    of safety and nothing else.

    A non-empty body that is already an error envelope is passed through
    unchanged: query.py owns the richer `reason`/`detail`, and re-wrapping it
    would bury the reason one level deeper for no gain.
    """
    if rc != 0:
        if _looks_like_error_envelope(body):
            return body
        return json.dumps({
            "error": "query-failed",
            "reason": "EXIT",
            "exit_code": rc,
            "detail": (body.strip() or
                       "query.py exited nonzero without writing a body"),
            "hint": "run the same query on the CLI for the full diagnostic; "
                    "the MCP server is refusing to present this as a result.",
        }, indent=2, sort_keys=True) + "\n"
    if not body.strip():
        # rc 0 with no body is not a known query.py path. Reporting it as an
        # empty array would be the same silent-success failure one code path
        # over, so it is surfaced rather than smoothed.
        return json.dumps({
            "error": "empty-response",
            "reason": "NO_BODY",
            "exit_code": rc,
            "detail": "query.py exited 0 but wrote no output",
            "hint": "run the same query on the CLI to reproduce.",
        }, indent=2, sort_keys=True) + "\n"
    return body


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

    # --- Bounding params, shared by every row-returning tool -----------
    # Every response carries returned / total_matching / truncated, so a
    # caller always knows whether it is looking at a page or the whole
    # set, and `narrow_with` names the flags to shrink an over-ceiling
    # query. `limit` is clamped: an unbounded set is not requestable.
    def _bounds(limit, scope, fields, offset) -> dict:
        return {
            "limit": limit, "scope": scope,
            "fields": fields, "offset": offset,
        }

    # --- No-arg tools --------------------------------------------------
    def ready(limit: int = _query_mod.DEFAULT_ROW_LIMIT,
              scope: Optional[str] = None, fields: Optional[str] = None,
              offset: int = 0) -> str:
        """List draft TODOs whose depends_on are all done."""
        return _run("ready", argparse.Namespace(**_bounds(limit, scope, fields, offset)))
    srv.tool(name="ready", description=DESCRIPTIONS["ready"])(ready)

    def blocked(limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                scope: Optional[str] = None, fields: Optional[str] = None,
                offset: int = 0) -> str:
        """List active TODOs with at least one unmet depends_on."""
        return _run("blocked", argparse.Namespace(**_bounds(limit, scope, fields, offset)))
    srv.tool(name="blocked", description=DESCRIPTIONS["blocked"])(blocked)

    def blocking(limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                 scope: Optional[str] = None, fields: Optional[str] = None,
                 offset: int = 0) -> str:
        """Rank TODOs by inbound depends_on count (most-blocking first)."""
        return _run("blocking", argparse.Namespace(**_bounds(limit, scope, fields, offset)))
    srv.tool(name="blocking", description=DESCRIPTIONS["blocking"])(blocking)

    def orphans(limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                scope: Optional[str] = None, fields: Optional[str] = None,
                offset: int = 0) -> str:
        """TODOs with zero inbound edges of any kind."""
        return _run("orphans", argparse.Namespace(**_bounds(limit, scope, fields, offset)))
    srv.tool(name="orphans", description=DESCRIPTIONS["orphans"])(orphans)

    def stats() -> str:
        """Repo-wide summary: total, by-status, by-domain, top-N."""
        return _run("stats", argparse.Namespace())
    srv.tool(name="stats", description=DESCRIPTIONS["stats"])(stats)

    # --- Optional single-argument tools --------------------------------
    def by_domain(domain: Optional[str] = None,
                  limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                  scope: Optional[str] = None, fields: Optional[str] = None,
                  offset: int = 0) -> str:
        """Group TODOs by domain + first unfinished section. When domain
        is None, returns every domain; when supplied, returns only that
        domain's rows."""
        ns = argparse.Namespace(domain=domain or None,
                                **_bounds(limit, scope, fields, offset))
        return _run("by-domain", ns)
    srv.tool(name="by-domain", description=DESCRIPTIONS["by-domain"])(by_domain)

    def stale(days: int = 90, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
              scope: Optional[str] = None, fields: Optional[str] = None,
              offset: int = 0) -> str:
        """TODOs whose last_active_at is older than `days` days (default 90)."""
        ns = argparse.Namespace(days=int(days),
                                **_bounds(limit, scope, fields, offset))
        return _run("stale", ns)
    srv.tool(name="stale", description=DESCRIPTIONS["stale"])(stale)

    # --- Required single-argument tools --------------------------------
    def backlinks(target: str, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                  scope: Optional[str] = None, fields: Optional[str] = None,
                  offset: int = 0) -> str:
        """Every TODO that references `target` (id / filename stem / slug)."""
        ns = argparse.Namespace(target=target,
                                **_bounds(limit, scope, fields, offset))
        return _run("backlinks", ns)
    srv.tool(name="backlinks", description=DESCRIPTIONS["backlinks"])(backlinks)

    def deferred(target: str, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                 fields: Optional[str] = None, offset: int = 0) -> str:
        """Outbound Accepted/Deferred stamps from `target`."""
        ns = argparse.Namespace(target=target,
                                **_bounds(limit, None, fields, offset))
        return _run("deferred", ns)
    srv.tool(name="deferred", description=DESCRIPTIONS["deferred"])(deferred)

    def deferred_by(target: str, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                    fields: Optional[str] = None, offset: int = 0) -> str:
        """Inbound Accepted/Deferred stamps pointing at `target`."""
        ns = argparse.Namespace(target=target,
                                **_bounds(limit, None, fields, offset))
        return _run("deferred-by", ns)
    srv.tool(name="deferred-by", description=DESCRIPTIONS["deferred-by"])(deferred_by)

    def code(target: str, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
             fields: Optional[str] = None, offset: int = 0) -> str:
        """Source paths claimed by `target`'s file_patterns + Notes grep."""
        ns = argparse.Namespace(target=target,
                                **_bounds(limit, None, fields, offset))
        return _run("code", ns)
    srv.tool(name="code", description=DESCRIPTIONS["code"])(code)

    def code_by(path: str, limit: int = _query_mod.DEFAULT_ROW_LIMIT,
                scope: Optional[str] = None, fields: Optional[str] = None,
                offset: int = 0) -> str:
        """TODOs whose file_patterns match `path` (a source/header path).

        Codex pass 18 M1: the MCP schema exposes this argument as `path`
        rather than `target` because code-by's semantic input is a
        filesystem path (e.g. `src/kernel/foo.c`), not a TODO id. The
        tool description already described it as a path, so the schema
        now matches. Internally we still forward to query.py via
        args_ns.target since that's the CLI positional name."""
        ns = argparse.Namespace(target=path,
                                **_bounds(limit, scope, fields, offset))
        return _run("code-by", ns)
    srv.tool(name="code-by", description=DESCRIPTIONS["code-by"])(code_by)

    return srv


# ---------------------------------------------------------------------
# Stdio JSON-RPC compatibility server
# ---------------------------------------------------------------------

def _tool_schema(name: str) -> dict[str, Any]:
    props: dict[str, Any] = {}
    required: list[str] = []
    if name == "by-domain":
        props["domain"] = {"type": "string"}
    elif name == "stale":
        props["days"] = {"type": "integer", "default": 90}
    elif name in {"backlinks", "deferred", "deferred-by", "code"}:
        props["target"] = {"type": "string"}
        required = ["target"]
    elif name == "code-by":
        props["path"] = {"type": "string"}
        required = ["path"]

    # Bounding params on every row-returning tool. `stats` is unbounded by
    # shape (see query.UNBOUNDED_SUBCOMMANDS) so it advertises none: an
    # agent must not be told it can page a verb that has no rows to page.
    if name not in _query_mod.UNBOUNDED_SUBCOMMANDS:
        props["limit"] = {
            "type": "integer",
            "default": _query_mod.DEFAULT_ROW_LIMIT,
            "minimum": 1,
            "maximum": _query_mod.MCP_MAX_ROW_LIMIT,
            "description": (
                f"max rows (default {_query_mod.DEFAULT_ROW_LIMIT}, hard cap "
                f"{_query_mod.MCP_MAX_ROW_LIMIT}). Values outside the range "
                f"are clamped; an unbounded set cannot be requested."
            ),
        }
        props["offset"] = {"type": "integer", "default": 0,
                           "description": "skip N rows (page through a large set)"}
        props["fields"] = {"type": "string",
                           "description": "comma-separated column subset"}
        if name in {"ready", "blocked", "blocking", "by-domain",
                    "backlinks", "orphans", "stale", "code-by"}:
            props["scope"] = {"type": "string",
                              "description": "restrict to one domain, e.g. 02-kernel-core"}
    return {
        "type": "object",
        "properties": props,
        "required": required,
        "additionalProperties": False,
    }


def _tool_list() -> list[dict[str, Any]]:
    return [
        {
            "name": name,
            "description": DESCRIPTIONS[name],
            "inputSchema": _tool_schema(name),
        }
        for name in MCP_TOOLS
    ]


def _bound_ns(arguments: dict[str, Any], **extra) -> argparse.Namespace:
    """Namespace carrying the caller's narrowing request plus any
    tool-specific fields. `limit` is passed through verbatim here and
    clamped in _call_query, so both server paths share one enforcement
    point and neither can widen past MCP_MAX_ROW_LIMIT."""
    return argparse.Namespace(
        limit=arguments.get("limit"),
        offset=arguments.get("offset") or 0,
        scope=arguments.get("scope") or None,
        fields=arguments.get("fields") or None,
        **extra,
    )


def _dispatch_tool(name: str, arguments: dict[str, Any],
                   repo_root: Path, auto_rebuild: bool) -> str:
    if name not in MCP_TOOLS:
        raise ValueError(f"unknown tool: {name}")
    if name == "stats":
        # Unbounded by shape; takes no narrowing params.
        return _call_query(name, argparse.Namespace(), repo_root, auto_rebuild)
    if name in {"ready", "blocked", "blocking", "orphans"}:
        return _call_query(name, _bound_ns(arguments), repo_root, auto_rebuild)
    if name == "by-domain":
        return _call_query(
            "by-domain",
            _bound_ns(arguments, domain=arguments.get("domain") or None),
            repo_root,
            auto_rebuild,
        )
    if name == "stale":
        return _call_query(
            "stale",
            _bound_ns(arguments, days=int(arguments.get("days", 90))),
            repo_root,
            auto_rebuild,
        )
    if name in {"backlinks", "deferred", "deferred-by", "code"}:
        target = arguments.get("target")
        if not isinstance(target, str) or not target:
            raise ValueError(f"{name} requires string argument 'target'")
        return _call_query(
            name,
            _bound_ns(arguments, target=target),
            repo_root,
            auto_rebuild,
        )
    if name == "code-by":
        path = arguments.get("path")
        if not isinstance(path, str) or not path:
            raise ValueError("code-by requires string argument 'path'")
        return _call_query(
            "code-by",
            _bound_ns(arguments, target=path),
            repo_root,
            auto_rebuild,
        )
    raise ValueError(f"unhandled tool: {name}")


def _write_jsonrpc_response(response: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def _jsonrpc_error(req_id: Any, code: int, message: str) -> dict[str, Any]:
    return {
        "jsonrpc": "2.0",
        "id": req_id,
        "error": {"code": code, "message": message},
    }


def _handle_jsonrpc_request(req: dict[str, Any], repo_root: Path,
                            auto_rebuild: bool) -> dict[str, Any] | None:
    req_id = req.get("id")
    method = req.get("method")
    params = req.get("params") or {}
    if not isinstance(params, dict):
        return _jsonrpc_error(req_id, -32602, "params must be an object")

    if method == "initialize":
        requested = str(params.get("protocolVersion") or "2024-11-05")
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "protocolVersion": requested,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "todo-graph", "version": "1.0"},
            },
        }
    if method == "notifications/initialized":
        return None
    if method == "ping":
        return {"jsonrpc": "2.0", "id": req_id, "result": {}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": _tool_list()}}
    if method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if not isinstance(name, str):
            return _jsonrpc_error(req_id, -32602, "tools/call requires tool name")
        if not isinstance(arguments, dict):
            return _jsonrpc_error(req_id, -32602, "tool arguments must be an object")
        try:
            text = _dispatch_tool(name, arguments, repo_root, auto_rebuild)
        except Exception as exc:
            return _jsonrpc_error(req_id, -32602, str(exc))
        return {
            "jsonrpc": "2.0",
            "id": req_id,
            "result": {
                "content": [{"type": "text", "text": text}],
                "isError": False,
            },
        }
    if req_id is None:
        return None
    return _jsonrpc_error(req_id, -32601, f"method not found: {method}")


def _serve_stdio_jsonrpc(repo_root: Path, auto_rebuild: bool = True) -> int:
    """Serve the todo-graph MCP surface over newline-delimited JSON-RPC.

    FastMCP still owns schema registration and self-test coverage, but this
    repo-owned loop owns serving so stdio behavior stays deterministic across
    SDK releases. The MCP SDK's stdio transport is newline-delimited JSON, so
    this implements the small read-only method subset this server exposes while
    reusing the same query/build functions as the FastMCP registration path.
    """
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as exc:
            _write_jsonrpc_response(_jsonrpc_error(None, -32700, str(exc)))
            continue
        if not isinstance(req, dict):
            _write_jsonrpc_response(_jsonrpc_error(None, -32600, "request must be an object"))
            continue
        response = _handle_jsonrpc_request(req, repo_root, auto_rebuild)
        if response is not None:
            _write_jsonrpc_response(response)
    return 0


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

    _build_mcp(FastMCP, repo_root, auto_rebuild=not args.no_auto_rebuild)
    return _serve_stdio_jsonrpc(repo_root, auto_rebuild=not args.no_auto_rebuild)


if __name__ == "__main__":
    sys.exit(main())
