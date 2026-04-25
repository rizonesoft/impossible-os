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

    if count != 0:
        sys.stderr.write(
            f"[lsp-mcp] FAIL: expected 0 tools in the skeleton, got {count}. "
            "Did a later commit land without updating _self_test()?\n"
        )
        return 1

    # Per-language smoke already ran above (before the FastMCP gate);
    # the zero-lang path falls through to the bridge-ready banner.
    live = len(_LIVE_LSPS)
    sys.stdout.write(
        f"[lsp-mcp] OK: {live} LSPs spawned, bridge ready\n"
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
    sys.stderr.write(
        f"[lsp-mcp] FAIL: --lang={lang!r} is not wired yet. "
        "Supported today: c (clangd-19), asm (asm-lsp), sh (bash-language-server).\n"
    )
    return 1


# Bound the self-test file read so a misconfigured --repo-root cannot
# point us at a symlinked huge file and OOM the bridge. 8 MiB is ~100x
# the size of any real source file in this repo.
_SELF_TEST_MAX_READ = 8 * 1024 * 1024


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
    missing = [
        cap for cap in clangd_server.required_capabilities()
        if not server_caps.get(cap)
    ]
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
    missing = [
        cap for cap in asm_server.required_capabilities()
        if not server_caps.get(cap)
    ]
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
    missing = [
        cap for cap in bash_server.required_capabilities()
        if not server_caps.get(cap)
    ]
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
    args = p.parse_args(argv)

    workspace_root = _workspace_root_from_argv(args)

    try:
        if args.self_test:
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
