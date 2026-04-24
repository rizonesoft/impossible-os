#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/bridge.py -- FastMCP stdio server that proxies up to five
#                              language servers (clangd / asm-lsp / bash-lsp
#                              / pyright / PSES) and exposes their read-only
#                              capabilities as MCP tools.
#
# Owner: TODO-07 in 00-infrastructure (LSP-MCP Bridge).
# This commit (Bridge Skeleton): FastMCP server + LSP JSON-RPC client +
# on-demand subprocess lifecycle + JSON error envelope.
#
# Architecture (skeleton scope):
#   * FastMCP stdio server modeled on scripts/todo-graph/mcp_server.py.
#   * Module-level _CALL_LOCK narrowly serializes the first-spawn path
#     in _get_or_spawn so concurrent FastMCP requests cannot race the
#     per-language handshake. Steady-state tool dispatch does NOT hold
#     _CALL_LOCK -- per-LspSubprocess locks handle single-LSP safety
#     and unrelated languages run in parallel. (Differs from
#     mcp_server.py, which holds its lock on every query because
#     build.main() mutates sys.argv/sys.stdout; the bridge has no
#     such shared process state.)
#   * LSPs are spawned ON DEMAND: first request for a language boots
#     that LSP; subsequent requests reuse it. Prevents idle overhead
#     when an agent never touches a language.
#   * Zero MCP tools registered in this skeleton -- the hover / definition /
#     references / diagnostics / workspace-symbol / document-symbol tools
#     land in the tool-wiring commit; this file is the foundation they
#     build on.
#   * --self-test exits 0 with "[lsp-mcp] OK: 0 LSPs spawned, bridge ready"
#     even when the `mcp` SDK is absent (SKIP path, CI-friendly).
#
# Usage:
#   python3 scripts/lsp-mcp/bridge.py              # stdio server
#   python3 scripts/lsp-mcp/bridge.py --self-test  # CI sanity check
#
# Sibling pattern: scripts/todo-graph/mcp_server.py is the canonical
# example. This file intentionally mirrors its shape (same _CALL_LOCK,
# same _try_import_mcp path, same --self-test exit contract) so a
# contributor who has read one understands the other at a glance.
# ============================================================================

from __future__ import annotations

import argparse
import sys
import threading
from pathlib import Path
from typing import Any, Callable, Optional


# Resolve the sibling lsp_client.py without relying on PYTHONPATH.
_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from lsp_client import LspError, LspSubprocess  # noqa: E402


# Narrow serialization: held ONLY during the first-spawn path in
# _get_or_spawn so two concurrent MCP tool calls for the same language
# cannot race the `initialize`/`initialized` handshake. Steady-state
# dispatch does NOT hold this lock -- per-LspSubprocess _io_lock +
# _pending_lock handle single-LSP safety, and unrelated languages run
# in parallel. Differs from scripts/todo-graph/mcp_server.py, which
# holds its _CALL_LOCK on every query because build.main() monkey-
# patches sys.argv and sys.stdout (process-global state); the LSP
# bridge has no such shared state.
_CALL_LOCK = threading.Lock()


# Registry of spawned LSPs keyed by language tag (e.g. "c", "asm",
# "sh", "py", "ps1"). Populated lazily by _get_or_spawn(); empty at
# bridge startup. Per-language integrations each add a spawn recipe
# that registers here; the skeleton only provides the infrastructure.
_LIVE_LSPS: dict[str, LspSubprocess] = {}
_LIVE_LSPS_LOCK = threading.Lock()

# Language spawn registry: language tag -> callable returning an
# LspSubprocess instance. The per-language integration commits
# append to this. The skeleton leaves it empty so --self-test
# reports "0 LSPs spawned" (the skeleton's test checkpoint).
LspSpawnFn = Callable[[Path], LspSubprocess]
_LSP_SPAWNERS: dict[str, LspSpawnFn] = {}


def register_spawner(lang: str, spawn_fn: LspSpawnFn) -> None:
    """Public hook for per-language server modules to register spawn
    recipes. Keeping this explicit (instead of auto-discovery) makes
    it obvious from reading bridge.py which languages are wired."""
    _LSP_SPAWNERS[lang] = spawn_fn


def _get_or_spawn(lang: str, workspace_root: Path) -> LspSubprocess:
    """Return a running LspSubprocess for `lang`, spawning if needed.

    Raises LspError when the language is unknown or the spawn recipe
    is missing. Callers translate that into the JSON error envelope.

    Thread-safety: _CALL_LOCK is taken here (structurally, not advisory)
    so the first-request-for-a-language path cannot race a concurrent
    caller into a double-spawn or a half-initialized handshake. Steady-
    state tool dispatch (after the LSP is cached) only needs the per-
    instance locks inside LspSubprocess, so this narrow scope keeps
    unrelated-language requests from serializing on each other."""
    with _CALL_LOCK:
        with _LIVE_LSPS_LOCK:
            inst = _LIVE_LSPS.get(lang)
            if inst is not None and inst.alive:
                return inst
            spawner = _LSP_SPAWNERS.get(lang)
            if spawner is None:
                raise LspError(
                    "lsp-language-unsupported",
                    f"no LSP registered for {lang!r}",
                    lang=lang,
                )
            inst = spawner(workspace_root)
            _LIVE_LSPS[lang] = inst
            return inst


def _call_lsp(fn: Callable[[], Any]) -> Any:
    """Wrap an LSP-touching callable; convert LspError into the JSON
    error envelope shape that the tool-wiring commit's handlers will
    return to MCP agents. Exported so the six read-only tool handlers
    (and the tests) can depend on a stable error-envelope contract.

    Non-LspError exceptions bubble up -- a bug in the bridge itself
    should be loud, not silently wrapped."""
    try:
        return fn()
    except LspError as exc:
        return exc.to_envelope()


def _find_repo_root() -> Path:
    """Walk upward from THIS file until a repo marker appears.

    Matches the pattern in scripts/todo-graph/mcp_server.py: MCP hosts
    launch the bridge from arbitrary working directories, so a CWD-
    based walk can silently bind to the wrong checkout (or a parent
    home directory). Walking from __file__ is deterministic -- it
    resolves to whatever tree the script itself lives in."""
    here = Path(__file__).resolve().parent
    for cand in (here, *here.parents):
        if (cand / "todo").is_dir() and (cand / "scripts" / "lsp-mcp").is_dir():
            return cand
    return Path.cwd()


def _workspace_root_from_argv(args: argparse.Namespace) -> Path:
    """Resolve the workspace root: --repo-root override wins, else
    _find_repo_root() walks upward from this script's location."""
    if args.repo_root:
        return Path(args.repo_root).resolve()
    return _find_repo_root()


def _shutdown_all_lsps() -> None:
    """Clean shutdown of every live LSP. Called on --self-test exit
    and at bridge teardown. Idempotent; safe to call twice."""
    with _LIVE_LSPS_LOCK:
        insts = list(_LIVE_LSPS.values())
        _LIVE_LSPS.clear()
    for inst in insts:
        try:
            inst.shutdown(timeout=2.0)
        except Exception:
            pass


# ---------------------------------------------------------------------
# FastMCP wiring (guarded behind optional import, same as mcp_server.py)
# ---------------------------------------------------------------------

def _try_import_mcp():
    """Return FastMCP class, or None if the SDK isn't installed.

    SKIP rather than FATAL mirrors mcp_server.py so --self-test stays
    green on CI hosts that don't install the mcp Python package. A
    real `python3 bridge.py` without --self-test still exits 2 with
    an install hint (main() below enforces that)."""
    try:
        from mcp.server.fastmcp import FastMCP  # type: ignore
        return FastMCP
    except Exception:
        return None


def _build_mcp(FastMCP, workspace_root: Path):
    """Instantiate FastMCP. The skeleton registers zero tools -- the
    read-only MCP tools (hover, definition, references, diagnostics,
    workspace-symbol, document-symbol) land in the tool-wiring commit.
    We still construct the server here so that the later addition is
    a one-function change (just register tools), and so --self-test
    can confirm the server-build path works.

    Exposing _get_or_spawn + _call_lsp + _CALL_LOCK from module scope
    is deliberate: the tool-wiring handlers will be closures over
    these pieces, so the integration surface stays grep-able."""
    srv = FastMCP("lsp-bridge")
    # No tools yet. Per-language integration commits register spawn
    # recipes via register_spawner(); the tool-wiring commit turns
    # those into MCP tools. The marker text "register_spawner" + the
    # _LSP_SPAWNERS registry above are the stable grep targets for
    # the tool-wiring integration.
    _ = workspace_root  # reserved for the tool-wiring handlers
    return srv


# ---------------------------------------------------------------------
# Self-test (CI-friendly; works with or without the SDK)
# ---------------------------------------------------------------------

def _self_test(workspace_root: Path) -> int:
    """Self-test contract (bridge skeleton):
      * Exit 0 with `[lsp-mcp] OK: 0 LSPs spawned, bridge ready` when
        the bridge skeleton is healthy.
      * SKIP with exit 0 when the mcp SDK is missing (CI-friendly).
      * Exit 1 only on a real bridge-internal failure (import of
        lsp_client failed, FastMCP build raised).
    Zero tool registration is the correct number for the skeleton --
    the six read-only MCP tools come in the tool-wiring commit. The
    count assertion defends against a future commit forgetting to
    update this check."""
    FastMCP = _try_import_mcp()
    if FastMCP is None:
        sys.stdout.write(
            "[lsp-mcp] SKIP: mcp SDK not installed; AI-agent integration "
            "unavailable. Install with `pip install mcp` to enable.\n"
        )
        return 0
    try:
        srv = _build_mcp(FastMCP, workspace_root)
    except Exception as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: build_mcp raised: {exc}\n")
        return 1

    # Count registered tools. FastMCP exposes a _tool_manager._tools
    # dict on current SDK versions; fall back to .tools if the shape
    # changes. mcp_server.py has the same probe.
    count: Optional[int] = None
    try:
        tools = srv._tool_manager._tools  # type: ignore[attr-defined]
        count = len(tools)
    except Exception:
        pass
    if count is None:
        try:
            count = len(getattr(srv, "tools"))
        except Exception:
            count = 0

    live = len(_LIVE_LSPS)
    if count != 0:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: expected 0 tools in the skeleton, got {count}. "
            "Did a later commit land without updating _self_test()?\n"
        )
        return 1
    sys.stdout.write(
        f"[lsp-mcp] OK: {live} LSPs spawned, bridge ready\n"
    )
    return 0


# ---------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------

def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="bridge.py",
        description="MCP stdio server over the LSP stack (clangd + asm-lsp + "
                    "bash-lsp + pyright + PSES).",
    )
    p.add_argument(
        "--self-test", action="store_true",
        help="build the FastMCP server, sanity-check, exit without serving stdio.",
    )
    p.add_argument(
        "--repo-root", default=None,
        help="override workspace root (default: auto-detect from CWD).",
    )
    args = p.parse_args(argv)

    workspace_root = _workspace_root_from_argv(args)

    try:
        if args.self_test:
            return _self_test(workspace_root)

        FastMCP = _try_import_mcp()
        if FastMCP is None:
            sys.stderr.write(
                "[lsp-mcp] FATAL: mcp SDK not installed. Install with "
                "`pip install mcp` (spec: TODO-07 in 00-infrastructure "
                "lists it as a REQUIRED dep for the stdio server entry "
                "point).\n"
            )
            return 2

        srv = _build_mcp(FastMCP, workspace_root)
        # Serve on stdio. FastMCP.run() picks the correct transport
        # based on context; default is stdio which is what Claude
        # Code launches.
        srv.run()
        return 0
    finally:
        _shutdown_all_lsps()


if __name__ == "__main__":
    sys.exit(main())
