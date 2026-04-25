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
import re
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


# Registry of spawned LSPs. KEYED BY (lang, resolved_workspace_root)
# so a future bridge instance used across two checkouts cannot hand
# caller B the LspSubprocess that caller A initialized against a
# different compile_commands.json / root_uri. Today the bridge takes
# one workspace root per process, so the tuple's root half is
# effectively constant; defending now keeps the invariant explicit
# and makes multi-root future work a one-line change.
_LIVE_LSPS: dict[tuple[str, str], LspSubprocess] = {}
_LIVE_LSPS_LOCK = threading.Lock()

# Per-key spawn-in-progress Events. A second caller for the same
# (lang, root) tuple waits on the Event rather than racing the spawn.
# Using Event instead of a per-key Lock lets the waiter share the
# eventually-spawned instance via _LIVE_LSPS after the first spawner
# publishes it.
_SPAWN_EVENTS: dict[tuple[str, str], threading.Event] = {}

# Cached spawner import failures so a broken per-language module does
# not look identical to "language not wired". Populated by
# _autoregister_spawners; read by _get_or_spawn to emit
# lsp-spawner-import-failed instead of lsp-language-unsupported.
_SPAWNER_IMPORT_ERRORS: dict[str, str] = {}

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


def _autoregister_spawners() -> None:
    """Import the per-language server modules and register their
    spawn recipes. Each import is independent: if one language's
    module fails (syntax error, missing optional dep), the others
    still register. The spawn functions themselves do the binary-
    presence check via `is_available()`; registering here does NOT
    require the LSP to be installed.

    Broken imports are CACHED, not swallowed, so _get_or_spawn can
    distinguish "integration broken" from "language not wired" --
    the earlier revision collapsed both into lsp-language-unsupported,
    hiding real regressions behind a generic error."""
    # clangd-19 (C / H) -- first language wired, per the bridge's
    # end-to-end bootstrap plan.
    try:
        from servers import clangd_server as _clangd
        register_spawner("c", _clangd.spawn)
    except Exception as exc:
        detail = f"{exc.__class__.__name__}: {exc}"
        _SPAWNER_IMPORT_ERRORS["c"] = detail
        sys.stderr.write(
            f"[lsp-mcp] warn: clangd spawner not registered: {detail}\n"
        )

    # asm-lsp (NASM x86-64) -- .asm + .S extensions.
    try:
        from servers import asm_server as _asm
        register_spawner("asm", _asm.spawn)
    except Exception as exc:
        detail = f"{exc.__class__.__name__}: {exc}"
        _SPAWNER_IMPORT_ERRORS["asm"] = detail
        sys.stderr.write(
            f"[lsp-mcp] warn: asm-lsp spawner not registered: {detail}\n"
        )

    # bash-language-server (.sh / .bash) -- shell LSP with shellcheck
    # delegated diagnostics when shellcheck is on PATH.
    try:
        from servers import bash_server as _bash
        register_spawner("sh", _bash.spawn)
    except Exception as exc:
        detail = f"{exc.__class__.__name__}: {exc}"
        _SPAWNER_IMPORT_ERRORS["sh"] = detail
        sys.stderr.write(
            f"[lsp-mcp] warn: bash-language-server spawner not registered: {detail}\n"
        )

    # pyright (.py) -- Microsoft Python LSP, type-inference focused.
    try:
        from servers import python_server as _py
        register_spawner("py", _py.spawn)
    except Exception as exc:
        detail = f"{exc.__class__.__name__}: {exc}"
        _SPAWNER_IMPORT_ERRORS["py"] = detail
        sys.stderr.write(
            f"[lsp-mcp] warn: pyright spawner not registered: {detail}\n"
        )

    # PowerShellEditorServices (.ps1 / .psm1 / .psd1) -- pwsh module
    # loaded via Start-EditorServices.ps1, not a standalone binary.
    try:
        from servers import powershell_server as _ps1
        register_spawner("ps1", _ps1.spawn)
    except Exception as exc:
        detail = f"{exc.__class__.__name__}: {exc}"
        _SPAWNER_IMPORT_ERRORS["ps1"] = detail
        sys.stderr.write(
            f"[lsp-mcp] warn: PSES spawner not registered: {detail}\n"
        )


_autoregister_spawners()


def _get_or_spawn(lang: str, workspace_root: Path) -> LspSubprocess:
    """Return a running LspSubprocess for (lang, workspace_root),
    spawning if needed.

    Concurrency contract:
      * Steady state (LSP cached + alive): no locks held, returns
        the instance.
      * First-spawn for a (lang, root) key: ONE thread performs the
        spawn; concurrent callers for the SAME key wait on a
        threading.Event and then share the result.
      * Unrelated languages or unrelated roots run in parallel --
        the earlier global _CALL_LOCK around the spawner call
        serialized all first-spawns across every language.

    Raises LspError for language-not-wired (lsp-language-unsupported),
    broken spawner imports (lsp-spawner-import-failed, preserves the
    original exception), and propagates anything the spawner itself
    raises."""
    try:
        root_key = str(workspace_root.resolve())
    except (OSError, RuntimeError):
        root_key = str(workspace_root)
    key = (lang, root_key)

    # Fast path under _LIVE_LSPS_LOCK only. If a spawn is already
    # under way for this key, attach to its Event and wait outside
    # the lock.
    with _LIVE_LSPS_LOCK:
        inst = _LIVE_LSPS.get(key)
        if inst is not None and inst.alive:
            return inst
        ev = _SPAWN_EVENTS.get(key)
        if ev is not None:
            waiter_event = ev
            owner = False
        else:
            import_err = _SPAWNER_IMPORT_ERRORS.get(lang)
            if import_err is not None:
                raise LspError(
                    "lsp-spawner-import-failed",
                    f"spawner module for {lang!r} failed to import",
                    lang=lang,
                    original_error=import_err,
                )
            spawner = _LSP_SPAWNERS.get(lang)
            if spawner is None:
                raise LspError(
                    "lsp-language-unsupported",
                    f"no LSP registered for {lang!r}",
                    lang=lang,
                )
            waiter_event = threading.Event()
            _SPAWN_EVENTS[key] = waiter_event
            owner = True

    if not owner:
        waiter_event.wait()
        with _LIVE_LSPS_LOCK:
            inst = _LIVE_LSPS.get(key)
        if inst is None or not inst.alive:
            raise LspError(
                "lsp-spawn-failed",
                "concurrent spawn owner did not publish a live instance",
                lang=lang,
            )
        return inst

    # Owner path: spawn OUTSIDE the registry lock so a slow clangd
    # (~2-3s startup + 15s initialize timeout) does not block
    # unrelated-language callers.
    try:
        inst = spawner(workspace_root)
    except BaseException:
        with _LIVE_LSPS_LOCK:
            _SPAWN_EVENTS.pop(key, None)
        waiter_event.set()
        raise
    with _LIVE_LSPS_LOCK:
        _LIVE_LSPS[key] = inst
        _SPAWN_EVENTS.pop(key, None)
    waiter_event.set()
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


MCP_TOOL_NAMES = (
    "hover",
    "definition",
    "references",
    "diagnostics",
    "workspace_symbol",
    "document_symbol",
)

# Extension -> language tag dispatch table. Drives _dispatch_path()
# routing. Keep in sync with the five spawner modules under
# scripts/lsp-mcp/servers/.
_EXT_TO_LANG: dict[str, str] = {
    ".c": "c", ".h": "c",
    ".asm": "asm", ".S": "asm",
    ".sh": "sh", ".bash": "sh",
    ".py": "py",
    ".ps1": "ps1", ".psm1": "ps1", ".psd1": "ps1",
}

# LSP Initialize-documented languageId per spec:
#   https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#textDocumentItem
# Each LSP accepts a specific identifier; the LANG_TAG we use for
# spawner routing (e.g. "sh", "ps1") is shorthand and does NOT match
# the LSP languageId for bash/python/powershell. Codex pre-
# implementation review of the MCP tools surface flagged this as High
# -- reusing LANG_TAG as languageId for bash-language-server /
# pyright / PSES would produce empty hover / diagnostics responses.
_LANG_TO_LSP_LANGUAGE_ID: dict[str, str] = {
    "c": "c",
    "asm": "asm",
    "sh": "shellscript",
    "py": "python",
    "ps1": "powershell",
}

# File-size cap for tool-time did_open. Mirrors the per-message cap
# in lsp_client._MAX_BODY_BYTES (32 MiB). A file larger than this
# would either be rejected by the LSP protocol layer or allocate
# unbounded memory; returning lsp-path-too-large is better than
# hiding the behavior.
_TOOL_MAX_READ = 32 * 1024 * 1024


def _dispatch_path(path_str: str, workspace_root: Path) -> tuple[Path, str, str]:
    """Resolve a tool-arg path + read its content + determine which
    LSP should handle it, ALL UNDER A SINGLE FILE DESCRIPTOR so an
    attacker cannot TOCTOU the stat/read window.

    Returns (resolved_path, lang_tag, text). Raises LspError for:
      - lsp-path-not-found            -- path does not exist
      - lsp-path-outside-workspace    -- path resolves outside workspace_root
      - lsp-path-not-regular-file     -- not a regular file (mode check on fstat)
      - lsp-path-too-large            -- file exceeds _TOOL_MAX_READ
      - lsp-path-unsupported-extension -- extension not in _EXT_TO_LANG
      - lsp-path-unreadable           -- I/O error reading fd
      - lsp-path-invalid-utf8         -- file not valid UTF-8

    TOCTOU hardening (Codex adversarial review finding): the early
    revision validated path + size on Path.stat() then let
    _ensure_open_for reopen by pathname -- a concurrent local
    replacement could swap the file for a symlink outside the
    workspace or a larger file between the two operations. We now
    open the file via os.open() ONCE, fstat() the resulting fd (which
    is bound to the specific inode resolve() picked), read under the
    cap, close, and only then hand the bytes forward. No later code
    re-reads by pathname.

    Relative paths are resolved against workspace_root, NOT the
    process CWD. MCP hosts launch the bridge from arbitrary
    directories; a CWD-based walk would let `hover("main.c", ...)`
    bind to an unrelated file in the host's launch directory. Codex
    pre-implementation review flagged this as High.
    """
    p = Path(path_str)
    candidate = p if p.is_absolute() else (workspace_root / p)
    try:
        resolved = candidate.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise LspError(
            "lsp-path-not-found",
            f"path {path_str!r} does not exist: {exc}",
            path=path_str,
        ) from exc
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise LspError(
            "lsp-workspace-root-invalid",
            f"workspace_root {workspace_root!r} not resolvable: {exc}",
        ) from exc
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        raise LspError(
            "lsp-path-outside-workspace",
            f"{path_str!r} resolves to {resolved} which is outside "
            f"workspace {workspace_resolved}",
            path=path_str,
            resolved=str(resolved),
            workspace=str(workspace_resolved),
        )
    ext = resolved.suffix
    lang = _EXT_TO_LANG.get(ext)
    if lang is None:
        # Fail extension-check BEFORE opening the fd -- no point
        # paying an open() for a file we will not route anywhere.
        raise LspError(
            "lsp-path-unsupported-extension",
            f"{resolved} extension {ext!r} is not wired (wired: "
            f"{sorted(set(_EXT_TO_LANG))})",
            path=path_str,
            extension=ext,
        )

    # Single-fd read: resolve() + relative_to() + open() all bind to
    # the inode we validated; once we have the fd a concurrent
    # replace of the pathname does not affect us.
    #
    # O_NOFOLLOW on the final component is NOT used -- resolve()
    # already follows every symlink to a canonical non-symlink path,
    # and the relative_to() check runs against that canonical path.
    # Re-opening with O_NOFOLLOW would false-reject legitimate
    # workspace symlinks (e.g. a `src/foo -> ../shared/foo` symlink
    # that resolves to a path inside the workspace).
    import os as _os
    try:
        fd = _os.open(str(resolved), _os.O_RDONLY)
    except OSError as exc:
        raise LspError(
            "lsp-path-unreadable",
            f"open {resolved}: {exc}",
            path=path_str,
        ) from exc
    try:
        import stat as _stat
        try:
            st = _os.fstat(fd)
        except OSError as exc:
            raise LspError(
                "lsp-path-unreadable",
                f"fstat {resolved}: {exc}",
                path=path_str,
            ) from exc
        if not _stat.S_ISREG(st.st_mode):
            raise LspError(
                "lsp-path-not-regular-file",
                f"{resolved} is not a regular file (mode={oct(st.st_mode)})",
                path=path_str,
                resolved=str(resolved),
            )
        if st.st_size > _TOOL_MAX_READ:
            raise LspError(
                "lsp-path-too-large",
                f"{resolved} size {st.st_size} exceeds "
                f"{_TOOL_MAX_READ}-byte tool cap",
                path=path_str,
                size=st.st_size,
                cap=_TOOL_MAX_READ,
            )
        # Read the full file via the bound fd. _os.read() may return
        # fewer bytes per call on large files; loop until EOF or the
        # fstat-reported size is consumed. Any growth past st.st_size
        # in a concurrent appender IS the upper bound we already
        # validated -- we stop at that size so a live appender cannot
        # sneak past the cap.
        chunks = []
        remaining = st.st_size
        while remaining > 0:
            try:
                chunk = _os.read(fd, min(remaining, 64 * 1024))
            except OSError as exc:
                raise LspError(
                    "lsp-path-unreadable",
                    f"read {resolved}: {exc}",
                    path=path_str,
                ) from exc
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
        raw = b"".join(chunks)
    finally:
        try:
            _os.close(fd)
        except OSError:
            pass
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise LspError(
            "lsp-path-invalid-utf8",
            f"{resolved} is not valid UTF-8: {exc}",
            path=path_str,
            resolved=str(resolved),
        ) from exc
    return resolved, lang, text


def _ensure_open_for(lsp: LspSubprocess, resolved: Path, lang: str,
                     text: str) -> str:
    """Shared did_open helper for the MCP tools. Accepts the pre-read
    bytes from _dispatch_path() so this helper does NOT re-read the
    file (Codex adversarial-review TOCTOU finding). Forwards an
    idempotent didOpen via LspSubprocess.ensure_open() and returns
    the file URI."""
    uri = resolved.as_uri()
    language_id = _LANG_TO_LSP_LANGUAGE_ID.get(lang, lang)
    lsp.ensure_open(uri, language_id, text, version=1)
    return uri


# LSP spec: Position.line and Position.character are `uinteger`
# (0 <= v < 2**31). Validate at tool entry so a malicious agent
# cannot push negative or oversized values into the LSP server.
_POSITION_MAX = (2 ** 31) - 1


def _validate_position(line: Any, character: Any) -> tuple[int, int]:
    """Coerce + validate an LSP Position. Returns (line, character)
    as ints. Raises LspError('lsp-position-invalid') for:
      - bool inputs (int(True) silently coerces to 1 -- reject so
        agents passing booleans by mistake get a clean error)
      - non-integer inputs
      - negative values
      - values >= 2**31"""
    for name, v in (("line", line), ("character", character)):
        if isinstance(v, bool):
            raise LspError(
                "lsp-position-invalid",
                f"{name}={v!r} is a bool; LSP Position requires uinteger",
                field=name,
                value=v,
            )
        try:
            iv = int(v)
        except (TypeError, ValueError) as exc:
            raise LspError(
                "lsp-position-invalid",
                f"{name}={v!r} is not an integer",
                field=name,
                value=repr(v),
            ) from exc
        if iv < 0 or iv > _POSITION_MAX:
            raise LspError(
                "lsp-position-invalid",
                f"{name}={iv} is outside LSP uinteger range "
                f"[0, {_POSITION_MAX}]",
                field=name,
                value=iv,
            )
    return int(line), int(character)


def _normalize_hover(hover: Any) -> str:
    """Collapse LSP's three hover response shapes into a flat markdown
    string. Returns empty string for null / empty / missing contents.

    LSP 3.17 textDocument/hover returns:
      - null (no hover available) -> ""
      - {contents: MarkupContent{kind, value}} -> value
      - {contents: MarkedString} where MarkedString is str or
        {language, value} -> value
      - {contents: MarkedString[]} -> each value joined by \\n\\n"""
    if hover is None:
        return ""
    contents = hover.get("contents") if isinstance(hover, dict) else None
    if contents is None:
        return ""
    if isinstance(contents, dict):
        value = contents.get("value")
        return value if isinstance(value, str) else ""
    if isinstance(contents, str):
        return contents
    if isinstance(contents, list):
        parts: list[str] = []
        for item in contents:
            if isinstance(item, str):
                parts.append(item)
            elif isinstance(item, dict):
                value = item.get("value")
                if isinstance(value, str):
                    parts.append(value)
        return "\n\n".join(parts)
    return ""


def _normalize_locations(result: Any) -> list[dict]:
    """Normalize LSP definition/references/typeDefinition/implementation
    result into a list of {uri, range} dicts.

    Input shapes per LSP 3.17:
      - null -> []
      - Location {uri, range}
      - LocationLink {targetUri, targetRange, ...}
      - list[Location | LocationLink]
    We pick targetUri + targetRange for LocationLink (the symbol's
    destination, not the origin the click came from). originSelection
    Range and targetSelectionRange are intentionally dropped; tools
    that need them can read the raw response separately."""
    if result is None:
        return []
    if isinstance(result, dict):
        result = [result]
    if not isinstance(result, list):
        return []
    out: list[dict] = []
    for loc in result:
        if not isinstance(loc, dict):
            continue
        if "targetUri" in loc:
            out.append({
                "uri": loc.get("targetUri"),
                "range": loc.get("targetRange"),
            })
        elif "uri" in loc:
            out.append({
                "uri": loc.get("uri"),
                "range": loc.get("range"),
            })
    return out


def _normalize_symbols(result: Any) -> list:
    """workspace/symbol and textDocument/documentSymbol responses are
    already typed as lists of SymbolInformation | DocumentSymbol |
    WorkspaceSymbol. Pass through as-is; we only guard against null
    (legitimate per spec when the server has nothing to return) and
    non-list types (protocol violation)."""
    if result is None:
        return []
    if not isinstance(result, list):
        return []
    return result


def _build_mcp(FastMCP, workspace_root: Path):
    """Instantiate FastMCP + register the six read-only tools.

    Typed-handler pattern per TODO-06 precedent at scripts/todo-graph/
    mcp_server.py:240-340 (one explicit function per tool with real
    named params -- NOT **kwargs, which produces empty MCP schemas
    that leave agents guessing). Each handler:
      1. calls _dispatch_path() to sandbox the path and route to the
         right LSP language tag,
      2. calls _get_or_spawn() to cold-start or reuse the LSP,
      3. calls _ensure_open_for() for the idempotent didOpen,
      4. issues the LSP request,
      5. normalizes the response,
      6. returns a plain dict that FastMCP serializes to JSON.

    Any LspError (path escape, language not wired, LSP binary
    missing, request timeout) gets converted to the error-envelope
    dict shape by _call_lsp() -- MCP-agent clients see a structured
    `{"error": "kind", "detail": "...", ...}` instead of a protocol
    exception."""
    srv = FastMCP("lsp-bridge")

    def _forbidden_method_set() -> frozenset:
        """Read-only boundary: surface the same frozenset the LSP
        client uses at request() entry. The MCP tools themselves
        never call any of these methods; the set is imported for the
        boundary-audit test (7f) to assert consistency."""
        from lsp_client import _FORBIDDEN_LSP_METHODS
        return _FORBIDDEN_LSP_METHODS

    _forbidden_method_set()  # bind symbol so the boundary test can find it

    def hover(path: str, line: int, character: int) -> dict:
        """Hover at (line, character) in the given file. Returns
        {uri, lang, line, character, markdown, raw} where `markdown`
        is the flat normalized string and `raw` is the unmodified LSP
        response (for clients that want to inspect Markup kind etc).
        Empty markdown means the LSP has nothing to say at that
        position, NOT an error."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text)
            raw = lsp.request(
                "textDocument/hover",
                {"textDocument": {"uri": uri},
                 "position": {"line": line_v, "character": char_v}},
                timeout=15.0,
            )
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                "markdown": _normalize_hover(raw),
                "raw": raw,
            }
        return _call_lsp(_op)
    srv.tool(name="hover",
             description="LSP textDocument/hover at (line, character). "
                         "Returns normalized markdown + raw response.")(hover)

    def definition(path: str, line: int, character: int) -> dict:
        """Go-to-definition. Returns {uri, lang, line, character,
        locations} where locations is a list of {uri, range}
        normalized across Location / LocationLink shapes."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text)
            raw = lsp.request(
                "textDocument/definition",
                {"textDocument": {"uri": uri},
                 "position": {"line": line_v, "character": char_v}},
                timeout=15.0,
            )
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                "locations": _normalize_locations(raw),
            }
        return _call_lsp(_op)
    srv.tool(name="definition",
             description="LSP textDocument/definition at (line, "
                         "character). Returns locations as [{uri, "
                         "range}] normalized across Location and "
                         "LocationLink response shapes.")(definition)

    def references(path: str, line: int, character: int,
                   include_declaration: bool = True) -> dict:
        """Find references. include_declaration is forwarded as the
        context.includeDeclaration bool per LSP spec. Returns the
        same shape as definition()."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text)
            raw = lsp.request(
                "textDocument/references",
                {"textDocument": {"uri": uri},
                 "position": {"line": line_v, "character": char_v},
                 "context": {"includeDeclaration": bool(include_declaration)}},
                timeout=15.0,
            )
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                "include_declaration": bool(include_declaration),
                "locations": _normalize_locations(raw),
            }
        return _call_lsp(_op)
    srv.tool(name="references",
             description="LSP textDocument/references at (line, "
                         "character). include_declaration forwards "
                         "to context.includeDeclaration.")(references)

    def diagnostics(path: str) -> dict:
        """Pull cached publishDiagnostics for a file. LSPs push
        diagnostics asynchronously after didOpen; this tool returns
        the most recent set the bridge has cached per URI. If no
        publish has arrived yet, returns {diagnostics: [], note:
        ...} instead of erroring -- agents can poll."""
        def _op() -> Any:
            resolved, lang, text = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text)
            cached = lsp.diagnostics_by_uri.get(uri)
            if cached is None:
                return {
                    "uri": uri,
                    "lang": lang,
                    "diagnostics": [],
                    "note": "no publishDiagnostics received for this URI "
                            "yet; LSP may still be indexing. Poll again "
                            "after a short delay.",
                }
            return {
                "uri": uri,
                "lang": lang,
                "diagnostics": cached if isinstance(cached, list) else [],
            }
        return _call_lsp(_op)
    srv.tool(name="diagnostics",
             description="Pull cached publishDiagnostics for a file. "
                         "Returns empty list + note when the LSP has "
                         "not published yet.")(diagnostics)

    def workspace_symbol(query: str, lang: Optional[str] = None) -> dict:
        """Query workspace symbols. If `lang` is given, routes to
        that single LSP (spawning on demand); if None, queries every
        already-spawned LSP in parallel (does NOT cold-spawn the
        remaining four).

        Response shape:
          lang given    -> {query, lang, symbols: [...]}
          lang is None  -> {query, per_lang: {lang: [symbols...]},
                            total: N, errors: {lang: "detail"}}

        The None-lang path is deliberately silent about languages
        that are NOT spawned: it only walks _LIVE_LSPS. Cold-spawning
        every LSP on a single MCP call would blow a multi-second
        budget even for languages the agent is not looking at."""
        if lang is not None:
            def _op() -> Any:
                lsp = _get_or_spawn(lang, workspace_root)
                raw = lsp.request(
                    "workspace/symbol",
                    {"query": str(query)},
                    timeout=15.0,
                )
                return {
                    "query": str(query),
                    "lang": lang,
                    "symbols": _normalize_symbols(raw),
                }
            return _call_lsp(_op)

        # lang is None: snapshot the live LSPs (not spawn recipes)
        # under _LIVE_LSPS_LOCK, then fan out in parallel. Codex
        # pre-implementation review flagged serializing under
        # _CALL_LOCK as a Medium; per-instance request locks in
        # LspSubprocess already protect each LSP's wire I/O.
        import concurrent.futures
        with _LIVE_LSPS_LOCK:
            snapshot = [
                (key, inst) for key, inst in _LIVE_LSPS.items()
                if inst.alive
            ]
        per_lang: dict[str, list] = {}
        errors: dict[str, str] = {}
        if not snapshot:
            return {"query": str(query), "per_lang": {}, "total": 0, "errors": {}}

        def _one(key, inst):
            (lang_tag, _root) = key
            try:
                raw = inst.request(
                    "workspace/symbol",
                    {"query": str(query)},
                    timeout=5.0,
                )
                return lang_tag, _normalize_symbols(raw), None
            except LspError as exc:
                return lang_tag, [], f"{exc.kind}: {exc.detail}"

        # Track which futures correspond to which lang_tag so the
        # overall-timeout path can mark the un-completed ones with a
        # structured error rather than raising (fail-soft contract:
        # partial results + per-lang error detail beats all-or-nothing
        # for an agent that asked across every language).
        futures_by_fut: dict = {}
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=max(1, len(snapshot))) as pool:
            for key, inst in snapshot:
                (lang_tag, _root) = key
                fut = pool.submit(_one, key, inst)
                futures_by_fut[fut] = lang_tag
            try:
                for fut in concurrent.futures.as_completed(
                        list(futures_by_fut.keys()), timeout=10.0):
                    lang_tag, syms, err = fut.result()
                    if err is None:
                        per_lang[lang_tag] = syms
                    else:
                        errors[lang_tag] = err
            except concurrent.futures.TimeoutError:
                # Overall deadline hit. Walk unfinished futures and
                # mark each one "lsp-overall-timeout"; any that did
                # finish before the deadline still populated per_lang
                # / errors on their own. The executor context exit
                # still waits for running workers (cannot cancel
                # mid-request without breaking LSP protocol state),
                # but agents see structured results immediately.
                for fut, lang_tag in futures_by_fut.items():
                    if fut.done():
                        try:
                            lt, syms, err = fut.result()
                            if err is None and lt not in per_lang:
                                per_lang[lt] = syms
                            elif err is not None and lt not in errors:
                                errors[lt] = err
                        except Exception:
                            pass
                    elif lang_tag not in per_lang and lang_tag not in errors:
                        errors[lang_tag] = (
                            "lsp-overall-timeout: workspace_symbol 10s "
                            "deadline exceeded while this LSP was still in-flight"
                        )
        total = sum(len(v) for v in per_lang.values())
        return {
            "query": str(query),
            "per_lang": per_lang,
            "total": total,
            "errors": errors,
        }
    srv.tool(name="workspace_symbol",
             description="LSP workspace/symbol query. Optional `lang` "
                         "routes to one spawner; without it, queries "
                         "every already-spawned LSP in parallel.")(workspace_symbol)

    def document_symbol(path: str) -> dict:
        """LSP textDocument/documentSymbol. Returns the hierarchical
        DocumentSymbol tree or flat SymbolInformation list depending
        on what the LSP advertised. The response is passed through
        as-is inside the `symbols` key."""
        def _op() -> Any:
            resolved, lang, text = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text)
            raw = lsp.request(
                "textDocument/documentSymbol",
                {"textDocument": {"uri": uri}},
                timeout=15.0,
            )
            return {
                "uri": uri,
                "lang": lang,
                "symbols": _normalize_symbols(raw),
            }
        return _call_lsp(_op)
    srv.tool(name="document_symbol",
             description="LSP textDocument/documentSymbol. Returns the "
                         "server's hierarchical or flat symbol list "
                         "pass-through.")(document_symbol)

    return srv


def _introspect_tools(srv: Any) -> dict[str, dict]:
    """Enumerate the registered MCP tools + their parameter schemas.

    Uses srv.list_tools() first (public API on current SDK); falls
    back to _tool_manager.list_tools() and finally the private
    _tool_manager._tools dict. Codex pre-implementation review
    preferred public APIs over the private _tools dict for schema
    introspection.

    Returns {tool_name: {required: [...], optional: [...],
    inputSchema: {...}}}. Optional properties are inferred from the
    JSON Schema (everything in `properties` not in `required`)."""
    import asyncio
    import inspect as _inspect
    tools_obj: Any
    try:
        tools_obj = srv.list_tools()
    except (AttributeError, TypeError):
        try:
            tools_obj = srv._tool_manager.list_tools()  # type: ignore[attr-defined]
        except Exception:
            try:
                tools_obj = list(srv._tool_manager._tools.values())  # type: ignore[attr-defined]
            except Exception:
                return {}
    if _inspect.isawaitable(tools_obj):
        tools_obj = asyncio.run(tools_obj)
    if not isinstance(tools_obj, (list, tuple)):
        try:
            tools_obj = list(tools_obj)
        except Exception:
            return {}
    out: dict[str, dict] = {}
    for t in tools_obj:
        name = getattr(t, "name", None) or (
            t.get("name") if isinstance(t, dict) else None
        )
        if not name:
            continue
        schema = getattr(t, "inputSchema", None)
        if schema is None and isinstance(t, dict):
            schema = t.get("inputSchema")
        if schema is None:
            # FastMCP tool objects sometimes expose parameters via a
            # parameters attribute on the function; we cannot always
            # reconstruct a JSON schema, but we CAN list the names.
            params = getattr(t, "parameters", None)
            schema = {"properties": params or {}, "required": []}
        properties = schema.get("properties", {}) if isinstance(schema, dict) else {}
        required = schema.get("required", []) if isinstance(schema, dict) else []
        optional = sorted(set(properties) - set(required))
        out[name] = {
            "required": list(required),
            "optional": optional,
            "inputSchema": schema,
        }
    return out


# ---------------------------------------------------------------------
# Self-test (CI-friendly; works with or without the SDK)
# ---------------------------------------------------------------------

def _self_test(workspace_root: Path, lang: Optional[str] = None) -> int:
    """Self-test contract:
      * Without `--lang`: exit 0 with
        `[lsp-mcp] OK: 0 LSPs spawned, bridge ready` when the bridge
        skeleton is healthy. SKIP with exit 0 when the mcp SDK is
        missing (CI-friendly). Exit 1 only on a real bridge-internal
        failure (import of lsp_client failed, FastMCP build raised).
      * With `--lang=<tag>`: spawn that language's LSP, do a minimal
        round-trip, and print a per-language OK/SKIP line. SKIP paths
        still exit 0 so CI hosts without the LSP stay green.

    Zero MCP tool registration is the correct count for this stage --
    the six read-only MCP tools land in the tool-wiring commit. The
    count assertion defends against a future commit forgetting to
    update this check."""
    # Dispatch --lang BEFORE the FastMCP gate: the per-language smoke
    # path does not need the mcp SDK, and a CI host with clangd-19
    # but no `pip install mcp` should still exercise the clangd
    # integration rather than silently SKIPping behind the SDK gate.
    if lang is not None:
        return _self_test_language(lang, workspace_root)

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

    expected = len(MCP_TOOL_NAMES)
    if count != expected:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: expected {expected} MCP tools, got {count}. "
            "Did a handler registration drift from MCP_TOOL_NAMES?\n"
        )
        return 1

    # Per-language smoke already ran above (before the FastMCP gate);
    # the zero-lang path falls through to the bridge-ready banner.
    live = len(_LIVE_LSPS)
    sys.stdout.write(
        f"[lsp-mcp] OK: {live} LSPs spawned, {count} tools registered, "
        "bridge ready\n"
    )
    return 0


def _self_test_tools(workspace_root: Path) -> int:
    """Print the registered tool schemas as JSON and exit 0. Driven
    by `--self-test --tools`. Satisfies the TODO test checkpoint:
    "6 tool schemas with required params (hover/definition/references:
    path, line, character; diagnostics/document_symbol: path;
    workspace_symbol: query)"."""
    FastMCP = _try_import_mcp()
    if FastMCP is None:
        sys.stdout.write(
            "[lsp-mcp] SKIP: mcp SDK not installed; tool schema "
            "introspection unavailable. Install with `pip install mcp`.\n"
        )
        return 0
    try:
        srv = _build_mcp(FastMCP, workspace_root)
    except Exception as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: build_mcp raised: {exc}\n")
        return 1
    schemas = _introspect_tools(srv)
    expected = set(MCP_TOOL_NAMES)
    missing = expected - set(schemas.keys())
    if missing:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: MCP_TOOL_NAMES claims {sorted(expected)} "
            f"but introspection only sees {sorted(schemas.keys())}; "
            f"missing {sorted(missing)}\n"
        )
        return 1
    import json as _json
    sys.stdout.write(_json.dumps(
        {name: schemas[name] for name in MCP_TOOL_NAMES},
        indent=2, sort_keys=True,
    ))
    sys.stdout.write("\n")
    sys.stdout.write(
        f"[lsp-mcp] OK: {len(schemas)} tools registered "
        f"({', '.join(MCP_TOOL_NAMES)})\n"
    )
    return 0


def _self_test_language(lang: str, workspace_root: Path) -> int:
    """Dispatch to a per-language smoke routine. Each routine decides
    SKIP-vs-OK internally and prints the banner line itself."""
    if lang == "c":
        return _self_test_clangd(workspace_root)
    if lang == "asm":
        return _self_test_asm(workspace_root)
    if lang == "sh":
        return _self_test_bash(workspace_root)
    if lang == "py":
        return _self_test_pyright(workspace_root)
    if lang == "ps1":
        return _self_test_pses(workspace_root)
    sys.stderr.write(
        f"[lsp-mcp] FAIL: --lang={lang!r} is not wired yet. "
        "Supported today: c (clangd-19), asm (asm-lsp), sh (bash-language-server), "
        "py (pyright), ps1 (PSES via pwsh 7.x).\n"
    )
    return 1


# Bound the self-test file read so a misconfigured --repo-root cannot
# point us at a symlinked huge file and OOM the bridge. 8 MiB is ~100x
# the size of any real source file in this repo.
_SELF_TEST_MAX_READ = 8 * 1024 * 1024


def _caps_missing(server_caps: dict, required: tuple) -> list:
    """Return the subset of required capability keys that the server
    has NOT advertised as supported.

    Per LSP 3.17 each capability value is `boolean | XxxOptions`.
    `True` or any `XxxOptions` dict (including `{}`, which means
    "supported with default options") signals support; `False` /
    `None` / key-absent signal unsupported.

    The naive `if not server_caps.get(cap)` check we used initially
    treats empty options dicts as falsy and falsely reports them as
    missing. PowerShellEditorServices advertises every provider as
    `{}` (default options) -- that's spec-correct, but it tripped
    the smoke until this helper landed. clangd/asm/bash/pyright
    happen to advertise booleans, so they passed the naive check by
    luck; the helper makes all five smokes use the spec-correct
    rule."""
    out = []
    for cap in required:
        if cap not in server_caps:
            out.append(cap)
            continue
        val = server_caps[cap]
        if val is False or val is None:
            out.append(cap)
    return out


def _utf16_code_units(s: str) -> int:
    """LSP Position.character is defined in UTF-16 code units. For
    strings containing non-BMP characters, one Python code point may
    map to two UTF-16 code units. Encode + divide so the count is
    correct regardless of the character set before the identifier."""
    return len(s.encode("utf-16-le")) // 2


def _self_test_clangd(workspace_root: Path) -> int:
    """Clangd end-to-end smoke: SKIP when clangd-19 is missing, else
    spawn + initialize + didOpen(src/kernel/main.c) + hover and print
    the byte-count of the hover response.

    Fail-closed: every transport / IO / protocol failure after the
    SKIP branch becomes an explicit FAIL with exit 1. Best-effort
    text scans that could hover a wrong location (and falsely report
    OK) are rejected."""
    # Import lazily so an import-time failure in clangd_server.py
    # (unlikely, but possible if someone breaks it) does not wedge
    # the zero-lang self-test.
    try:
        from servers import clangd_server
    except Exception as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: could not import servers.clangd_server: {exc}\n"
        )
        return 1

    if not clangd_server.is_available():
        sys.stdout.write(
            f"[lsp-mcp] SKIP: clangd not installed "
            f"({clangd_server.install_hint()})\n"
        )
        return 0

    # Resolve main.c + reject non-regular files / symlinks that
    # escape the workspace. Closes the "hostile --repo-root" vector:
    # a malformed repo cannot trick us into read_text()'ing an
    # arbitrary file. 8 MiB cap bounds memory even on a real file.
    main_c = workspace_root / "src" / "kernel" / "main.c"
    try:
        resolved = main_c.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {main_c}: {exc}\n")
        return 1
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: resolving {workspace_root}: {exc}\n"
        )
        return 1
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {main_c} escapes workspace "
            f"{workspace_resolved} (symlink?); refusing to read.\n"
        )
        return 1
    try:
        st = resolved.stat()
    except OSError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: stat {resolved}: {exc}\n")
        return 1
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} is not a regular file "
            f"(mode={oct(st.st_mode)})\n"
        )
        return 1
    if st.st_size > _SELF_TEST_MAX_READ:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} size {st.st_size} exceeds "
            f"{_SELF_TEST_MAX_READ}-byte self-test cap\n"
        )
        return 1

    try:
        lsp = _get_or_spawn("c", workspace_root)
    except LspError as exc:
        # Installed-but-spawn-failed is genuine trouble; do NOT
        # coerce to SKIP. Surface a FAIL so the CI gate bites.
        sys.stderr.write(f"[lsp-mcp] FAIL: clangd spawn: {exc}\n")
        return 1

    # server_caps is normalized to a dict by LspSubprocess.initialize,
    # but guard defensively in case a future refactor regresses.
    server_caps = lsp.server_caps
    if not isinstance(server_caps, dict):
        sys.stderr.write(
            "[lsp-mcp] FAIL: clangd server_caps is not a dict "
            f"({type(server_caps).__name__}); protocol violation.\n"
        )
        return 1
    missing = _caps_missing(server_caps, clangd_server.required_capabilities())
    if missing:
        sys.stderr.write(
            "[lsp-mcp] FAIL: clangd handshake missing required "
            f"capabilities: {missing}. Advertised: "
            f"{sorted(server_caps.keys())}\n"
        )
        return 1

    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    try:
        lsp.did_open(resolved.as_uri(), "c", text, version=1)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: clangd didOpen: {exc}\n")
        return 1

    # Find `kernel_main` and fail HARD if missing. Falling through to
    # position (0,0) would hover the file comment and still produce a
    # non-empty byte count -- a silent false positive on a rename.
    target_line: Optional[int] = None
    target_char: Optional[int] = None
    for idx, line in enumerate(text.splitlines()):
        col = line.find("kernel_main")
        if col >= 0 and "void" in line:
            target_line = idx
            # LSP Position.character is UTF-16 code units. For ASCII
            # text this equals the byte offset, but encode-count
            # defensively so non-BMP chars before the identifier on
            # the same line do not mis-position the hover request.
            target_char = _utf16_code_units(line[:col]) + 1
            break
    if target_line is None or target_char is None:
        sys.stderr.write(
            "[lsp-mcp] FAIL: could not locate `void kernel_main(` in "
            f"{resolved}. Was the kernel entry renamed?\n"
        )
        return 1

    try:
        hover = lsp.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": resolved.as_uri()},
                "position": {"line": target_line, "character": target_char},
            },
            timeout=15.0,
        )
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: clangd hover: {exc}\n")
        return 1

    byte_len = _hover_content_bytes(hover)
    if byte_len == 0:
        sys.stderr.write(
            "[lsp-mcp] FAIL: clangd hover returned empty contents. "
            "Either the symbol was not resolved (check compile_commands "
            "has src/kernel/main.c) or clangd is mis-configured.\n"
        )
        return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: clangd spawned, hover on kernel_main "
        f"returned {byte_len} bytes\n"
    )
    return 0


def _self_test_asm(workspace_root: Path) -> int:
    """asm-lsp end-to-end smoke: SKIP when asm-lsp is missing, else
    spawn + initialize + didOpen(src/boot/entry.asm) + hover on the
    first `mov` mnemonic and print the byte-count of the instruction-
    reference response.

    Mirrors _self_test_clangd's fail-closed discipline. The hover
    target file is src/boot/entry.asm; the TODO draft said
    src/kernel/entry.asm, but the real tree puts the kernel entry
    stub under src/boot/ (the kernel-side NASM files are ISR stubs,
    SIMD helpers, etc., none of which carries the canonical `mov`
    we want to exercise on asm-lsp's instruction-reference payload)."""
    try:
        from servers import asm_server
    except Exception as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: could not import servers.asm_server: {exc}\n"
        )
        return 1

    if not asm_server.is_available():
        sys.stdout.write(
            f"[lsp-mcp] SKIP: asm-lsp not installed "
            f"({asm_server.install_hint()})\n"
        )
        return 0

    # Sandbox the self-test file read: resolve strictly, verify the
    # target stays inside workspace_root, reject non-regular files,
    # enforce the 8 MiB cap. Same guarantees as _self_test_clangd.
    entry_asm = workspace_root / "src" / "boot" / "entry.asm"
    try:
        resolved = entry_asm.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {entry_asm}: {exc}\n")
        return 1
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: resolving {workspace_root}: {exc}\n"
        )
        return 1
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {entry_asm} escapes workspace "
            f"{workspace_resolved} (symlink?); refusing to read.\n"
        )
        return 1
    try:
        st = resolved.stat()
    except OSError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: stat {resolved}: {exc}\n")
        return 1
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} is not a regular file "
            f"(mode={oct(st.st_mode)})\n"
        )
        return 1
    if st.st_size > _SELF_TEST_MAX_READ:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} size {st.st_size} exceeds "
            f"{_SELF_TEST_MAX_READ}-byte self-test cap\n"
        )
        return 1

    try:
        lsp = _get_or_spawn("asm", workspace_root)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: asm-lsp spawn: {exc}\n")
        return 1

    server_caps = lsp.server_caps
    if not isinstance(server_caps, dict):
        sys.stderr.write(
            "[lsp-mcp] FAIL: asm-lsp server_caps is not a dict "
            f"({type(server_caps).__name__}); protocol violation.\n"
        )
        return 1
    missing = _caps_missing(server_caps, asm_server.required_capabilities())
    if missing:
        sys.stderr.write(
            "[lsp-mcp] FAIL: asm-lsp handshake missing required "
            f"capabilities: {missing}. Advertised: "
            f"{sorted(server_caps.keys())}\n"
        )
        return 1

    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    try:
        lsp.did_open(resolved.as_uri(), "asm", text, version=1)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: asm-lsp didOpen: {exc}\n")
        return 1

    # Find the first standalone `mov` mnemonic. NASM lines take the
    # shape `[label:] mnemonic operands... [; comment]`; matching
    # anything else would silently accept a `mov` inside a comment
    # or a label substring and still report OK on a refactor.
    # Strip the `;`-introduced comment FIRST so commented-out mov
    # lines never match; then anchor the regex to require an
    # optional label + whitespace + `mov` followed by a whitespace
    # boundary. Capture the mnemonic start so target_char is the
    # column of the actual mnemonic (not of a substring that
    # happens to appear earlier on the line).
    mov_re = re.compile(
        r"^(?P<prefix>\s*(?:[A-Za-z_.$][\w.$]*\s*:\s*)?)"
        r"(?P<mov>mov)(?=\s|$)",
        re.IGNORECASE,
    )
    target_line: Optional[int] = None
    target_char: Optional[int] = None
    for idx, raw_line in enumerate(text.splitlines()):
        # Strip comment (`;` introduces the comment in NASM syntax;
        # no escape rules inside comments).
        semi = raw_line.find(";")
        effective = raw_line[:semi] if semi >= 0 else raw_line
        m = mov_re.match(effective)
        if m is None:
            continue
        target_line = idx
        # m.start("mov") is a Python character index; convert to
        # UTF-16 code units for LSP Position.character.
        target_char = _utf16_code_units(effective[:m.start("mov")]) + 1
        break
    if target_line is None or target_char is None:
        sys.stderr.write(
            "[lsp-mcp] FAIL: could not locate a standalone `mov` "
            f"mnemonic in {resolved}. Has the file been rewritten?\n"
        )
        return 1

    try:
        hover = lsp.request(
            "textDocument/hover",
            {
                "textDocument": {"uri": resolved.as_uri()},
                "position": {"line": target_line, "character": target_char},
            },
            timeout=10.0,
        )
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: asm-lsp hover: {exc}\n")
        return 1

    byte_len = _hover_content_bytes(hover)
    if byte_len == 0:
        sys.stderr.write(
            "[lsp-mcp] FAIL: asm-lsp hover on `mov` returned empty "
            "contents. asm-lsp may be misconfigured (check "
            ".asm-lsp.toml pins assembler=\"nasm\") or the binary is "
            "from an unreleased / broken version.\n"
        )
        return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: asm-lsp spawned, hover on mov returned "
        f"instruction reference ({byte_len} bytes)\n"
    )
    return 0


def _self_test_bash(workspace_root: Path) -> int:
    """bash-language-server end-to-end smoke: SKIP when bash-language-
    server is missing, else spawn + initialize + didOpen(scripts/build.sh)
    + poll for asynchronous publishDiagnostics, then print the diagnostic
    count.

    Diagnostics are pushed by the server as one-way notifications, not
    request/response replies. LspSubprocess._dispatch_message caches them
    into lsp.diagnostics_by_uri[uri]; this function polls that dict with
    a 5-second deadline. If no publish arrives within the window we treat
    the result as zero diagnostics and still emit the OK banner -- the
    TODO contract accepts an empty array ("possibly empty if no issues").
    """
    try:
        from servers import bash_server
    except Exception as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: could not import servers.bash_server: {exc}\n"
        )
        return 1

    if not bash_server.is_available():
        sys.stdout.write(
            f"[lsp-mcp] SKIP: bash-language-server not installed "
            f"({bash_server.install_hint()})\n"
        )
        return 0

    # Sandbox the self-test file read: resolve strictly, verify the
    # target stays inside workspace_root, reject non-regular files,
    # enforce the 8 MiB cap. Same guarantees as the clangd path.
    build_sh = workspace_root / "scripts" / "build.sh"
    try:
        resolved = build_sh.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {build_sh}: {exc}\n")
        return 1
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: resolving {workspace_root}: {exc}\n"
        )
        return 1
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {build_sh} escapes workspace "
            f"{workspace_resolved} (symlink?); refusing to read.\n"
        )
        return 1
    try:
        st = resolved.stat()
    except OSError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: stat {resolved}: {exc}\n")
        return 1
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} is not a regular file "
            f"(mode={oct(st.st_mode)})\n"
        )
        return 1
    if st.st_size > _SELF_TEST_MAX_READ:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} size {st.st_size} exceeds "
            f"{_SELF_TEST_MAX_READ}-byte self-test cap\n"
        )
        return 1

    try:
        lsp = _get_or_spawn("sh", workspace_root)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: bash-language-server spawn: {exc}\n")
        return 1

    server_caps = lsp.server_caps
    if not isinstance(server_caps, dict):
        sys.stderr.write(
            "[lsp-mcp] FAIL: bash-language-server server_caps is not a dict "
            f"({type(server_caps).__name__}); protocol violation.\n"
        )
        return 1
    missing = _caps_missing(server_caps, bash_server.required_capabilities())
    if missing:
        sys.stderr.write(
            "[lsp-mcp] FAIL: bash-language-server handshake missing required "
            f"capabilities: {missing}. Advertised: "
            f"{sorted(server_caps.keys())}\n"
        )
        return 1

    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    uri = resolved.as_uri()
    try:
        # 'shellscript' is the LSP language id bash-language-server
        # accepts for .sh / .bash files (per its documentSelector).
        lsp.did_open(uri, "shellscript", text, version=1)
    except LspError as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: bash-language-server didOpen: {exc}\n"
        )
        return 1

    # Poll for the asynchronous publishDiagnostics notification. The
    # reader thread caches the most recent set per URI into
    # lsp.diagnostics_by_uri; we wait up to 5 seconds for the URI to
    # appear. CRITICAL: a publish that never arrives is NOT the same as
    # an empty diagnostic list. The TODO contract accepts an empty list
    # as a legitimate "no issues" outcome, but only when the transport
    # is still healthy at the moment we sample. If the subprocess died
    # or the reader thread crashed before publishing, the cached default
    # of [] would otherwise become a false-green OK banner. We track
    # whether a publish was actually observed and check transport
    # liveness before emitting OK; transport-broken paths FAIL.
    import time as _time
    deadline = _time.monotonic() + 5.0
    got_publish = False
    while _time.monotonic() < deadline:
        if uri in lsp.diagnostics_by_uri:
            got_publish = True
            break
        if not lsp.alive:
            sys.stderr.write(
                "[lsp-mcp] FAIL: bash-language-server subprocess exited "
                f"before publishing diagnostics for {uri}\n"
            )
            return 1
        if lsp._reader_dead:
            sys.stderr.write(
                "[lsp-mcp] FAIL: bash-language-server reader thread died "
                f"before publishing diagnostics for {uri}\n"
            )
            return 1
        _time.sleep(0.1)
    diagnostics = lsp.diagnostics_by_uri.get(uri, [])
    if not isinstance(diagnostics, list):
        sys.stderr.write(
            "[lsp-mcp] FAIL: bash-language-server diagnostics for "
            f"{uri} is not a list ({type(diagnostics).__name__}); "
            "protocol violation.\n"
        )
        return 1
    # Final transport-health gate: empty list with healthy transport is
    # legitimate; empty list with broken transport is the false-green
    # path Codex flagged. Distinguish the two before emitting OK.
    if not got_publish and not lsp.alive:
        sys.stderr.write(
            "[lsp-mcp] FAIL: bash-language-server died after the "
            f"diagnostic poll deadline without publishing for {uri}\n"
        )
        return 1
    if not got_publish and lsp._reader_dead:
        sys.stderr.write(
            "[lsp-mcp] FAIL: bash-language-server reader thread died "
            f"after the diagnostic poll deadline ({uri} never received "
            "a publishDiagnostics notification)\n"
        )
        return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: bash-language-server spawned, "
        f"diagnostics on build.sh returned {len(diagnostics)} items\n"
    )
    return 0


def _self_test_pyright(workspace_root: Path) -> int:
    """pyright end-to-end smoke: SKIP when pyright-langserver is missing,
    else spawn + initialize + didOpen(scripts/todo-graph/build.py) +
    workspace/symbol query for 'main', then assert at least one symbol
    came back.

    The smoke target is workspace symbols rather than hover because
    pyright's workspaceSymbolProvider is the highest-leverage cap we
    will route through MCP -- agents asking "where is symbol X defined
    across all .py files?" depend on it -- and exercising it end-to-end
    catches indexer regressions that a hover smoke would miss.

    Mirrors the fail-closed discipline of the clangd / asm / bash
    smokes: every transport / IO / protocol failure after the SKIP
    branch becomes an explicit FAIL with exit 1. Best-effort symbol
    scans that could falsely report OK on an empty index are rejected.
    """
    try:
        from servers import python_server
    except Exception as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: could not import servers.python_server: {exc}\n"
        )
        return 1

    if not python_server.is_available():
        sys.stdout.write(
            f"[lsp-mcp] SKIP: pyright not installed "
            f"({python_server.install_hint()})\n"
        )
        return 0

    # Sandbox the self-test file read: resolve strictly, verify the
    # target stays inside workspace_root, reject non-regular files,
    # enforce the 8 MiB cap. Same guarantees as the clangd path.
    build_py = workspace_root / "scripts" / "todo-graph" / "build.py"
    try:
        resolved = build_py.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {build_py}: {exc}\n")
        return 1
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: resolving {workspace_root}: {exc}\n"
        )
        return 1
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {build_py} escapes workspace "
            f"{workspace_resolved} (symlink?); refusing to read.\n"
        )
        return 1
    try:
        st = resolved.stat()
    except OSError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: stat {resolved}: {exc}\n")
        return 1
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} is not a regular file "
            f"(mode={oct(st.st_mode)})\n"
        )
        return 1
    if st.st_size > _SELF_TEST_MAX_READ:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} size {st.st_size} exceeds "
            f"{_SELF_TEST_MAX_READ}-byte self-test cap\n"
        )
        return 1

    try:
        lsp = _get_or_spawn("py", workspace_root)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: pyright spawn: {exc}\n")
        return 1

    server_caps = lsp.server_caps
    if not isinstance(server_caps, dict):
        sys.stderr.write(
            "[lsp-mcp] FAIL: pyright server_caps is not a dict "
            f"({type(server_caps).__name__}); protocol violation.\n"
        )
        return 1
    missing = _caps_missing(server_caps, python_server.required_capabilities())
    if missing:
        sys.stderr.write(
            "[lsp-mcp] FAIL: pyright handshake missing required "
            f"capabilities: {missing}. Advertised: "
            f"{sorted(server_caps.keys())}\n"
        )
        return 1

    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    uri = resolved.as_uri()
    try:
        # Pyright accepts 'python' as the LSP language id for .py files.
        lsp.did_open(uri, "python", text, version=1)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: pyright didOpen: {exc}\n")
        return 1

    # Pyright indexes the workspace asynchronously. workspace/symbol
    # against a freshly-spawned server can return an empty list before
    # the indexer finishes, even though the symbol exists. Poll with a
    # bounded deadline (10 s; pyright indexes the scripts/ tree in
    # 2-3 s on a warm cache).
    #
    # CRITICAL discrimination: query "main" matches every `main`
    # function in the workspace (bridge.py has one too, mcp_server.py
    # has one, etc.), so a non-empty result does NOT prove build.py
    # was indexed. Require at least one returned symbol to resolve to
    # the URI we just did_open'd. That is the only assertion that
    # actually validates the end-to-end path: did_open is forwarded ->
    # pyright indexer ingests our file -> workspace/symbol returns a
    # match in our file. Without this filter the smoke can go green
    # while the indexer is broken on build.py specifically.
    import time as _time
    deadline = _time.monotonic() + 10.0
    matching: list[Any] = []
    total_count = 0
    last_err: Optional[LspError] = None
    symbols: Any = None
    while _time.monotonic() < deadline:
        if not lsp.alive:
            sys.stderr.write(
                "[lsp-mcp] FAIL: pyright subprocess exited before "
                "workspace/symbol returned a result\n"
            )
            return 1
        if lsp._reader_dead:
            sys.stderr.write(
                "[lsp-mcp] FAIL: pyright reader thread died before "
                "workspace/symbol returned a result\n"
            )
            return 1
        try:
            symbols = lsp.request(
                "workspace/symbol",
                {"query": "main"},
                timeout=5.0,
            )
        except LspError as exc:
            last_err = exc
            symbols = None
        if isinstance(symbols, list) and symbols:
            total_count = len(symbols)
            matching = [
                s for s in symbols
                if isinstance(s, dict)
                and isinstance(s.get("location"), dict)
                and s["location"].get("uri") == uri
            ]
            if matching:
                break
        _time.sleep(0.25)

    if symbols is None:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: pyright workspace/symbol: {last_err}\n"
        )
        return 1
    if not isinstance(symbols, list):
        sys.stderr.write(
            "[lsp-mcp] FAIL: pyright workspace/symbol returned "
            f"{type(symbols).__name__}; expected list per LSP spec.\n"
        )
        return 1
    if not matching:
        sys.stderr.write(
            "[lsp-mcp] FAIL: pyright workspace/symbol query 'main' "
            f"returned {total_count} results after 10s, but NONE "
            f"resolved to {uri}. The indexer is producing matches "
            "from elsewhere in the workspace but did_open on "
            "scripts/todo-graph/build.py was not picked up. Either "
            "the indexer is still cold (raise the deadline), the "
            "did_open uri did not match what pyright expects, or "
            "scripts/todo-graph/build.py no longer defines a "
            "top-level `main` symbol.\n"
        )
        return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: pyright spawned, workspace-symbol main "
        f"returned {total_count} results\n"
    )
    return 0


def _self_test_pses(workspace_root: Path) -> int:
    """PowerShellEditorServices end-to-end smoke: SKIP when pwsh 7.x or
    PSES module is missing, else spawn + initialize + didOpen on a
    real .ps1 file + textDocument/documentSymbol, then assert the
    response is a list (LSP spec contract for documentSymbol).

    Smoke target is scripts/machines/run-qemu.ps1 -- the largest .ps1
    in the repo at section-implementation time, with multiple top-level
    functions so a healthy PSES indexer returns a non-trivial symbol
    tree. The TODO contract says "assert response is an array" not
    "non-empty"; we accept an empty list so a future shrunk file does
    not false-fail the smoke. Length is reported in the OK banner so
    a regression that drops symbols is still visible.

    Mirrors the fail-closed discipline of the clangd / asm / bash /
    pyright smokes: every transport / IO / protocol failure after the
    SKIP branch becomes an explicit FAIL with exit 1.
    """
    try:
        from servers import powershell_server
    except Exception as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: could not import servers.powershell_server: {exc}\n"
        )
        return 1

    if not powershell_server.is_available():
        sys.stdout.write(
            f"[lsp-mcp] SKIP: PSES not installed "
            f"({powershell_server.install_hint()})\n"
        )
        return 0

    # Sandbox the self-test file read: resolve strictly, verify the
    # target stays inside workspace_root, reject non-regular files,
    # enforce the 8 MiB cap. Same guarantees as the clangd path.
    target = workspace_root / "scripts" / "machines" / "run-qemu.ps1"
    try:
        resolved = target.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {target}: {exc}\n")
        return 1
    try:
        workspace_resolved = workspace_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: resolving {workspace_root}: {exc}\n"
        )
        return 1
    try:
        resolved.relative_to(workspace_resolved)
    except ValueError:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {target} escapes workspace "
            f"{workspace_resolved} (symlink?); refusing to read.\n"
        )
        return 1
    try:
        st = resolved.stat()
    except OSError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: stat {resolved}: {exc}\n")
        return 1
    import stat as _stat
    if not _stat.S_ISREG(st.st_mode):
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} is not a regular file "
            f"(mode={oct(st.st_mode)})\n"
        )
        return 1
    if st.st_size > _SELF_TEST_MAX_READ:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: {resolved} size {st.st_size} exceeds "
            f"{_SELF_TEST_MAX_READ}-byte self-test cap\n"
        )
        return 1

    try:
        lsp = _get_or_spawn("ps1", workspace_root)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: PSES spawn: {exc}\n")
        return 1

    server_caps = lsp.server_caps
    if not isinstance(server_caps, dict):
        sys.stderr.write(
            "[lsp-mcp] FAIL: PSES server_caps is not a dict "
            f"({type(server_caps).__name__}); protocol violation.\n"
        )
        return 1
    missing = _caps_missing(server_caps, powershell_server.required_capabilities())
    if missing:
        sys.stderr.write(
            "[lsp-mcp] FAIL: PSES handshake missing required "
            f"capabilities: {missing}. Advertised: "
            f"{sorted(server_caps.keys())}\n"
        )
        return 1

    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    uri = resolved.as_uri()
    try:
        # PSES accepts 'powershell' as the LSP language id for .ps1 /
        # .psm1 / .psd1 files (per its documentSelector).
        lsp.did_open(uri, "powershell", text, version=1)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: PSES didOpen: {exc}\n")
        return 1

    try:
        symbols = lsp.request(
            "textDocument/documentSymbol",
            {"textDocument": {"uri": uri}},
            timeout=15.0,
        )
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: PSES documentSymbol: {exc}\n")
        return 1

    if symbols is None:
        # The LSP spec permits null but the smoke contract is strict:
        # null on a real .ps1 with multiple top-level functions
        # signals that PSES failed to parse the file or that the
        # documentSymbol provider is not actually wired. Either is a
        # regression, not a benign no-symbols case (which would be an
        # empty list). FAIL.
        sys.stderr.write(
            "[lsp-mcp] FAIL: PSES documentSymbol returned null; expected "
            "an array per the smoke contract. The file has top-level "
            "functions; null indicates a parser or provider regression.\n"
        )
        return 1
    if not isinstance(symbols, list):
        sys.stderr.write(
            "[lsp-mcp] FAIL: PSES documentSymbol returned "
            f"{type(symbols).__name__}; expected list per LSP spec.\n"
        )
        return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: PSES spawned, document-symbol "
        f"returned {len(symbols)} results\n"
    )
    return 0


def _hover_content_bytes(hover: Any) -> int:
    """Return the byte-length of a hover response across LSP's three
    result shapes. Zero means the hover returned no usable content."""
    if hover is None:
        return 0
    contents = hover.get("contents") if isinstance(hover, dict) else None
    if contents is None:
        return 0
    # MarkupContent (dict with kind + value)
    if isinstance(contents, dict):
        value = contents.get("value")
        return len(value.encode("utf-8")) if isinstance(value, str) else 0
    # Single MarkedString (str)
    if isinstance(contents, str):
        return len(contents.encode("utf-8"))
    # List of MarkedString / MarkupContent
    if isinstance(contents, list):
        total = 0
        for item in contents:
            if isinstance(item, str):
                total += len(item.encode("utf-8"))
            elif isinstance(item, dict):
                value = item.get("value")
                if isinstance(value, str):
                    total += len(value.encode("utf-8"))
        return total
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
        "--lang", default=None,
        help="drive --self-test against a single language (e.g. 'c' for "
             "clangd). Without --lang, --self-test only validates the "
             "bridge skeleton.",
    )
    p.add_argument(
        "--repo-root", default=None,
        help="override workspace root (default: auto-detect from CWD).",
    )
    p.add_argument(
        "--tools", action="store_true",
        help="with --self-test: print registered tool schemas + exit. "
             "Without --self-test: ignored.",
    )
    args = p.parse_args(argv)

    workspace_root = _workspace_root_from_argv(args)

    try:
        if args.self_test:
            if args.tools:
                return _self_test_tools(workspace_root)
            return _self_test(workspace_root, lang=args.lang)
        if args.lang is not None:
            sys.stderr.write(
                "[lsp-mcp] FATAL: --lang requires --self-test. The stdio "
                "server dispatches by file extension, not CLI flag.\n"
            )
            return 2

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
