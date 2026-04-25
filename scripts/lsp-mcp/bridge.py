#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/bridge.py -- FastMCP stdio server that proxies up to five
#                              language servers (clangd / asm-lsp / bash-lsp
#                              / pyright / PSES) and exposes their read-only
#                              capabilities as MCP tools.
#
# Owner: TODO-07 in 00-infrastructure (LSP-MCP Bridge).
#
# Architecture:
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
#   * Six read-only MCP tools registered via _build_mcp():
#     hover / definition / references / diagnostics /
#     workspace_symbol / document_symbol. Each handler sandboxes the
#     path arg via _dispatch_path(), routes to the right LSP through
#     _EXT_TO_LANG, and returns a normalized dict; errors land as the
#     LspError.to_envelope() shape instead of raising.
#   * --self-test exits 0 with
#     "[lsp-mcp] OK: 0 LSPs spawned, 6 tools registered, bridge ready"
#     even when the `mcp` SDK is absent (SKIP path, CI-friendly).
#     --self-test --tools dumps the six tool schemas as JSON.
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


# Per-(lang, root) crash + restart bookkeeping. Lives on the BRIDGE
# side (NOT on LspSubprocess) so it survives instance replacement
# during respawn AND accumulates across spawner/initialize failures
# that never publish a live instance to _LIVE_LSPS. Codex design
# review caught the naive "store on the LspSubprocess" trap: a
# persistently-crashing LSP would never reach the FAILED threshold
# because every fresh instance starts with restart_count=0.
#
# Schema per key:
#   {"restart_count": int,
#    "recent_crash_times": [float],     # monotonic seconds, sliding window
#    "failed": bool,                    # set True on 4 crashes in 60s; never auto-clears
#    "last_crash_reason": Optional[str],
#    "last_restart_at": Optional[float], # monotonic seconds
#    "status": "spawned" | "healthy" | "backoff" | "failed"}
#
# Read/written under _LIVE_LSPS_LOCK so the per-key health snapshot
# stays consistent with the live-instance lookup it gates.
_LSP_HEALTH: dict[tuple[str, str], dict[str, Any]] = {}

# Crash policy constants for the watchdog + respawn path. Documented
# in TODO-07 in 00-infrastructure (LSP Subprocess Health Monitoring).
_RESPAWN_BACKOFF_BASE_S = 1.0          # 2**N: 1, 2, 4, 8, ...
_RESPAWN_BACKOFF_CAP_S = 30.0          # max sleep between attempts
_RESPAWN_FAILED_THRESHOLD = 4          # crashes within window
_RESPAWN_FAILED_WINDOW_S = 60.0        # sliding window for FAILED detection

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


def _emit_health_event(event: str, lang: str, root: str,
                       **fields: Any) -> None:
    """Emit a single JSON-lines crash / restart event to stderr.

    Schema: {ts, level, event, lang, root, ...fields}. The
    correlation-ID thread (structured-logging follow-up section)
    is not yet shipped; once it lands the corr_id field will be
    added by a retrofit. Best-effort: a write failure must not
    crash the bridge during teardown."""
    import time as _time
    payload = {
        "ts": _time.time(),
        "level": "WARN" if event in ("crash", "restart-failed",
                                      "failed") else "INFO",
        "event": event,
        "lang": lang,
        "root": root,
    }
    payload.update(fields)
    try:
        sys.stderr.write(_json_dumps(payload) + "\n")
        sys.stderr.flush()
    except Exception:
        pass


def _json_dumps(obj: Any) -> str:
    """Stable single-line JSON dump (no trailing newline). sort_keys
    keeps log lines greppable when the schema grows."""
    import json as _json
    return _json.dumps(obj, sort_keys=True, default=str)


def _get_or_init_health(key: tuple[str, str]) -> dict[str, Any]:
    """Return (lazily creating) the _LSP_HEALTH entry for key. MUST
    be called under _LIVE_LSPS_LOCK so the read-then-init is atomic
    against concurrent _get_or_spawn callers."""
    h = _LSP_HEALTH.get(key)
    if h is None:
        h = {
            "restart_count": 0,
            "recent_crash_times": [],
            "failed": False,
            "last_crash_reason": None,
            "last_restart_at": None,
            "status": "spawned",
        }
        _LSP_HEALTH[key] = h
    return h


def _record_crash(key: tuple[str, str], reason: str) -> bool:
    """Record a crash for `key` and update the FAILED state. Returns
    True if the key has now entered FAILED state (and the caller
    should NOT respawn). Caller MUST hold _LIVE_LSPS_LOCK."""
    import time as _time
    h = _get_or_init_health(key)
    now = _time.monotonic()
    # Sliding window: drop crash times older than the failure
    # window so a long-running healthy LSP that experiences one
    # transient crash does not stay one-strike-from-FAILED forever.
    cutoff = now - _RESPAWN_FAILED_WINDOW_S
    h["recent_crash_times"] = [t for t in h["recent_crash_times"]
                                if t >= cutoff]
    h["recent_crash_times"].append(now)
    h["last_crash_reason"] = reason
    if len(h["recent_crash_times"]) >= _RESPAWN_FAILED_THRESHOLD:
        h["failed"] = True
        h["status"] = "failed"
        return True
    h["status"] = "backoff"
    return False


def _backoff_delay_s(restart_count: int) -> float:
    """Exponential backoff: 1, 2, 4, 8, ... capped at the per-policy
    ceiling. restart_count is the number of PRIOR restarts (so the
    first restart sleeps _RESPAWN_BACKOFF_BASE_S, the next 2x, etc).
    Ceilinged so a long-running session that has accumulated many
    historical restarts does not sleep for hours after one new
    crash."""
    if restart_count < 0:
        restart_count = 0
    raw = _RESPAWN_BACKOFF_BASE_S * (2 ** min(restart_count, 16))
    return min(raw, _RESPAWN_BACKOFF_CAP_S)


def _inst_usable(inst: Optional[LspSubprocess]) -> bool:
    """An instance is usable IFF subprocess is alive AND the reader
    thread has not died AND no unexpected crash has been flagged.
    Codex post-implementation review caught the gap where a reader
    thread that died on a protocol error left proc.alive=True but
    subsequent lsp.request() calls rejected with lsp-reader-crashed
    -- _get_or_spawn returning the alive-but-broken instance trapped
    _call_lsp's retry loop (it would keep getting the same dead
    instance back). The composite check covers all three failure
    modes: subprocess exit, reader crash, and unexpected
    teardown."""
    if inst is None:
        return False
    if not inst.alive:
        return False
    if getattr(inst, "_reader_dead", False):
        return False
    if getattr(inst, "crashed", False):
        return False
    return True


def _force_dispose_dead_inst(inst: Optional[LspSubprocess]) -> None:
    """Walk an instance's cleanup_paths and reap a zombie
    subprocess (alive but reader dead). Idempotent: relies on
    LspSubprocess.shutdown's own early-exit when already closed.

    Called by _respawn_locked before publishing the replacement
    instance so spawner-owned tempdirs (PSES LogPath /
    SessionDetailsPath) do not leak across respawn generations
    (Codex post-implementation review Medium)."""
    if inst is None:
        return
    try:
        inst.shutdown(timeout=0.5)
    except Exception:
        pass


def _replay_open_uris(old: LspSubprocess, new: LspSubprocess,
                      workspace_root: Path) -> int:
    """Re-open every URI the OLD instance had tracked, on the NEW
    instance. Walks each URI back through _dispatch_path so the
    workspace-bounded sandbox + TOCTOU-safe single-fd read happen
    again -- the file may have changed during the crash window, and
    we want the new LSP to see the current bytes, not the OLD
    instance's last cached text. Returns the count of URIs replayed.

    Best-effort: a single URI that fails to re-validate (file
    deleted, moved outside workspace, no longer regular) is skipped
    with a log entry; other URIs continue. This matches editor LSP
    client behavior on restart -- closed files do not stop the
    rest from re-opening."""
    if old is None or not getattr(old, "open_uri_meta", None):
        return 0
    snapshot = list(old.open_uri_meta.items())
    replayed = 0
    for uri, meta in snapshot:
        resolved_path = meta.get("resolved_path")
        lang_tag = meta.get("lang")
        if not resolved_path or not lang_tag:
            # Legacy meta without snapshot fields. Skip; the next
            # tool call will didOpen via the normal path.
            continue
        try:
            # Re-walk through _dispatch_path to revalidate the
            # workspace boundary and capture fresh text + mtime.
            _resolved, _lang, text, mtime_ns = _dispatch_path(
                resolved_path, workspace_root,
            )
        except LspError as exc:
            _emit_health_event("replay-skip", lang_tag,
                                str(workspace_root),
                                uri=uri, reason=exc.kind)
            continue
        language_id = _LANG_TO_LSP_LANGUAGE_ID.get(lang_tag, lang_tag)
        force_did_save = lang_tag in _DIDSAVE_ON_REFRESH_LANGS
        try:
            new.apply_text(uri, language_id, text, mtime_ns,
                           force_did_save=force_did_save,
                           resolved_path=str(_resolved),
                           lang=lang_tag)
            replayed += 1
        except LspError as exc:
            _emit_health_event("replay-skip", lang_tag,
                                str(workspace_root),
                                uri=uri, reason=exc.kind)
    return replayed


def _respawn_locked(lang: str, workspace_root: Path,
                    spawner: "LspSpawnFn",
                    key: tuple[str, str],
                    old_inst: Optional[LspSubprocess]) -> LspSubprocess:
    """Re-spawn the LSP for `key` after a detected crash.

    Caller contract: holds _SPAWN_EVENTS gate (so concurrent
    requests on the same key wait via the Event); does NOT hold
    _LIVE_LSPS_LOCK during the spawn (slow path, mirrors the
    first-spawn release pattern). Bumps restart_count, applies
    backoff, runs the spawner, replays didOpens. Raises
    lsp-persistently-crashing if the key has already entered FAILED
    state (caller MUST check this BEFORE invoking respawn).

    A spawner failure during respawn is itself recorded as a crash
    (Codex design review caught this -- spawner/initialize failures
    must accumulate FAILED state too, otherwise a server that dies
    during handshake loops forever)."""
    import time as _time
    with _LIVE_LSPS_LOCK:
        h = _get_or_init_health(key)
        restart_count = h["restart_count"]

    delay = _backoff_delay_s(restart_count)
    _emit_health_event("restart-attempt", lang, str(workspace_root),
                       restart_count=restart_count,
                       backoff_s=delay)
    if delay > 0.0:
        _time.sleep(delay)

    try:
        new_inst = spawner(workspace_root)
    except BaseException as exc:
        # Spawner failure on respawn IS a crash -- record it so the
        # FAILED state can fire even when the LSP never publishes a
        # live instance. Re-raise after recording.
        kind = exc.kind if isinstance(exc, LspError) else (
            f"spawner-error: {type(exc).__name__}"
        )
        with _LIVE_LSPS_LOCK:
            entered_failed = _record_crash(key, kind)
        if entered_failed:
            _emit_health_event("failed", lang, str(workspace_root),
                                reason=kind)
        else:
            _emit_health_event("restart-failed", lang,
                                str(workspace_root),
                                reason=kind)
        raise

    # Spawn succeeded; dispose of the old instance (walks its
    # cleanup_paths -- e.g. PSES tempdir -- so spawner-owned scratch
    # state does not leak across respawn generations) and replay
    # tracked URIs on the new one.
    _force_dispose_dead_inst(old_inst)
    replayed = 0
    try:
        replayed = _replay_open_uris(old_inst, new_inst, workspace_root)
    except Exception:
        # Replay is best-effort -- a partial replay is better than
        # leaving the new instance unpublished after a successful
        # spawner. Log and continue.
        _emit_health_event("replay-error", lang, str(workspace_root))

    # Publish under the lock; bump restart_count + status.
    with _LIVE_LSPS_LOCK:
        _LIVE_LSPS[key] = new_inst
        h = _get_or_init_health(key)
        h["restart_count"] = restart_count + 1
        h["last_restart_at"] = _time.monotonic()
        h["status"] = "healthy"

    _emit_health_event("restart-success", lang, str(workspace_root),
                       restart_count=restart_count + 1,
                       replayed_uris=replayed)
    return new_inst


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
    original exception), persistent crashes (lsp-persistently-crashing
    once the per-key FAILED state has been set), and propagates
    anything the spawner itself raises.

    Crash detection: when a cached instance exists but inst.alive is
    False (the reader thread saw EOF / subprocess exit), this path
    records a crash on _LSP_HEALTH and either respawns under the
    same _SPAWN_EVENTS gate (if not yet at FAILED threshold) or
    raises lsp-persistently-crashing. Health-state and respawn
    helpers live above; this function is the single decision
    point."""
    try:
        root_key = str(workspace_root.resolve())
    except (OSError, RuntimeError):
        root_key = str(workspace_root)
    key = (lang, root_key)

    # Fast path under _LIVE_LSPS_LOCK only. Three sub-paths:
    #   (A) cached + alive               -> return
    #   (B) cached + dead (crashed)      -> record crash, then either
    #                                       FAILED-out OR set up
    #                                       respawn through the
    #                                       _SPAWN_EVENTS gate
    #   (C) not cached                   -> first-spawn path
    # An in-flight _SPAWN_EVENTS entry means another thread is
    # spawning/respawning the SAME key; attach to its Event and wait
    # outside the lock.
    crashed_old: Optional[LspSubprocess] = None
    with _LIVE_LSPS_LOCK:
        h = _get_or_init_health(key)
        if h["failed"]:
            raise LspError(
                "lsp-persistently-crashing",
                f"{lang!r} LSP entered FAILED state after "
                f"{h['restart_count']} restarts; restart the bridge",
                lang=lang,
                hint="restart the bridge",
                last_crash_reason=h.get("last_crash_reason"),
            )
        inst = _LIVE_LSPS.get(key)
        if _inst_usable(inst):
            return inst
        # (B) Cached but dead OR reader-crashed (alive subprocess but
        # reader thread exited on protocol error). Record the crash
        # event ONCE per generation: if the cached instance is the
        # same one we already crash-recorded against, the next caller
        # skips re-recording (we mark the instance via
        # _crash_recorded so repeated callers all see the same
        # restart_count). Codex post-implementation review High:
        # alive-but-reader-dead instances would otherwise keep
        # being returned and trap _call_lsp's retry loop.
        if inst is not None and not getattr(inst, "_crash_recorded",
                                              False):
            reason = inst._crash_reason or "unknown"
            entered_failed = _record_crash(key, reason)
            inst._crash_recorded = True  # type: ignore[attr-defined]
            _emit_health_event("crash", lang, root_key,
                                reason=reason)
            if entered_failed:
                _emit_health_event("failed", lang, root_key,
                                    reason=reason)
                raise LspError(
                    "lsp-persistently-crashing",
                    f"{lang!r} LSP crossed the FAILED threshold "
                    f"({_RESPAWN_FAILED_THRESHOLD} crashes within "
                    f"{int(_RESPAWN_FAILED_WINDOW_S)}s); restart "
                    "the bridge",
                    lang=lang,
                    hint="restart the bridge",
                    last_crash_reason=reason,
                )
        crashed_old = inst  # may be None; passed to replay
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
    # unrelated-language callers. Two flavors:
    #   * crashed_old is None -> first-spawn (fresh process, no
    #     replay needed).
    #   * crashed_old is set  -> respawn (apply backoff, replay
    #     tracked URIs from the dead instance's open_uri_meta).
    try:
        if crashed_old is not None:
            inst = _respawn_locked(lang, workspace_root, spawner,
                                   key, crashed_old)
        else:
            inst = spawner(workspace_root)
    except BaseException as exc:
        # Record the first-spawn failure too. Codex post-impl review
        # High: a server that fails during cold spawn / initialize
        # would otherwise be retried on every tool call forever
        # because _record_crash was only invoked on the
        # cached-dead-instance path. With the fix, repeated cold-
        # spawn failures cross _RESPAWN_FAILED_THRESHOLD and convert
        # to lsp-persistently-crashing, matching the contract for
        # post-publish crashes.
        first_spawn_failed = crashed_old is None
        if first_spawn_failed:
            kind = exc.kind if isinstance(exc, LspError) else (
                f"spawner-error: {type(exc).__name__}"
            )
            with _LIVE_LSPS_LOCK:
                entered_failed = _record_crash(key, kind)
                _SPAWN_EVENTS.pop(key, None)
            waiter_event.set()
            if entered_failed:
                _emit_health_event("failed", lang, root_key,
                                    reason=kind)
                # Convert to the terminal envelope so the caller
                # sees the FAILED state instead of the underlying
                # spawner exception (which the caller would
                # otherwise re-experience on every retry).
                raise LspError(
                    "lsp-persistently-crashing",
                    f"{lang!r} LSP crossed the FAILED threshold "
                    f"during cold spawn / initialize; restart "
                    "the bridge",
                    lang=lang,
                    hint="restart the bridge",
                    last_crash_reason=kind,
                ) from exc
            else:
                _emit_health_event("restart-failed", lang, root_key,
                                    reason=kind)
        else:
            with _LIVE_LSPS_LOCK:
                _SPAWN_EVENTS.pop(key, None)
            waiter_event.set()
        raise
    with _LIVE_LSPS_LOCK:
        _LIVE_LSPS[key] = inst
        _SPAWN_EVENTS.pop(key, None)
        # First-spawn: flip status from initial "spawned" to
        # "healthy" so _health reports the post-handshake state.
        # _respawn_locked already set "healthy" on its own success
        # path, so this assignment is a no-op for respawn cases.
        h = _get_or_init_health(key)
        if h["status"] in ("spawned", "backoff"):
            h["status"] = "healthy"
    waiter_event.set()
    return inst


def _call_lsp(fn: Callable[[], Any]) -> Any:
    """Wrap an LSP-touching callable; convert LspError into the JSON
    error envelope shape that the tool-wiring commit's handlers will
    return to MCP agents. Exported so the six read-only tool handlers
    (and the tests) can depend on a stable error-envelope contract.

    Non-LspError exceptions bubble up -- a bug in the bridge itself
    should be loud, not silently wrapped.

    Crash-retry contract: if the wrapped callable raises a transient
    LSP-died LspError (lsp-subprocess-exited, lsp-shutdown after
    crash, lsp-reader-crashed), retry the callable ONCE. The retry
    invokes the same closure -- which re-calls _get_or_spawn -- so
    the second attempt observes the dead instance, triggers
    _respawn_locked through the normal _SPAWN_EVENTS gate, and runs
    against the freshly-replaced instance with the replayed open_uris.
    This makes "first hover after kill -9 returns a normalized
    result" the user-visible contract, not the per-tool handler's
    job. Single retry: a second crash within the retry window means
    the LSP is genuinely broken; return the envelope. The
    lsp-persistently-crashing envelope (FAILED state) is NOT
    retried (it is a terminal contract)."""
    _RETRY_KINDS = ("lsp-subprocess-exited",
                    "lsp-reader-crashed",
                    "lsp-shutdown")
    try:
        return fn()
    except LspError as exc:
        if exc.kind not in _RETRY_KINDS:
            return exc.to_envelope()
    # Retry path: the closure must re-call _get_or_spawn so it sees
    # the dead instance and triggers respawn. Any exception on the
    # retry returns its envelope as-is (no second retry).
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
    # Extended tools (TODO-07 in 00-infrastructure, second-tier
    # capabilities). All read-only; code_action returns the action
    # metadata list without invoking workspace/applyEdit or
    # workspace/executeCommand (those stay in _FORBIDDEN_LSP_METHODS).
    "completion",
    "signature_help",
    "type_definition",
    "implementation",
    "declaration",
    "call_hierarchy_incoming",
    "call_hierarchy_outgoing",
    "code_action",
    # Health + auto-restart (LSP Subprocess Health Monitoring).
    # Read-only meta tool: returns per-LSP status + restart count
    # without touching any LSP wire path. Works even when every
    # registered LSP is in FAILED state -- the tool walks the
    # bridge's _LSP_HEALTH registry directly.
    "_health",
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


def _dispatch_path(path_str: str, workspace_root: Path
                   ) -> tuple[Path, str, str, int]:
    """Resolve a tool-arg path + read its content + determine which
    LSP should handle it, ALL UNDER A SINGLE FILE DESCRIPTOR so an
    attacker cannot TOCTOU the stat/read window.

    Returns (resolved_path, lang_tag, text, mtime_ns). The mtime_ns
    is captured from the same fstat() that validates the file mode +
    size; it feeds the file-change-lifecycle path in
    LspSubprocess.apply_text() so subsequent MCP tool calls on the
    same URI can detect external edits and forward didChange. Using
    the bound-fd fstat (instead of a re-stat by pathname) keeps the
    TOCTOU guarantee intact -- the mtime we report is for the same
    inode whose bytes we just read.

    Raises LspError for:
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
    # st was taken from the bound fd before the read; mtime_ns
    # corresponds to the inode we actually read. nanosecond
    # resolution avoids missing same-second edits that a 1-second
    # st_mtime would lose.
    return resolved, lang, text, st.st_mtime_ns


# Languages whose LSPs only re-lint or re-index on save (per LSP
# specs and observed behavior across pyright + clangd). For these,
# apply_text() additionally forwards a synthetic didSave whenever
# external mtime drift triggers a didChange, so diagnostics stay
# fresh even when the agent (or a parallel VS Code session) wrote
# the file outside the LSP's own save flow. Languages NOT in this
# set re-lint on every didChange and do not need the extra
# notification.
_DIDSAVE_ON_REFRESH_LANGS: frozenset[str] = frozenset({"c", "py"})


def _ensure_open_for(lsp: LspSubprocess, resolved: Path, lang: str,
                     text: str, mtime_ns: Optional[int] = None) -> str:
    """Shared open-or-refresh helper for the MCP tools. Accepts the
    pre-read bytes + mtime from _dispatch_path() so this helper does
    NOT re-read the file (Codex adversarial review TOCTOU finding).

    Routes through LspSubprocess.apply_text() which decides between
    didOpen (first time we see the URI), didChange + optional
    didSave (mtime drift since last call), or no-op (URI already
    open at the same mtime). Returns the URI.

    mtime_ns=None preserves the legacy contract for callers that do
    not have a captured mtime (no production callsite today).

    Function name kept as `_ensure_open_for` for backwards
    compatibility with sub-test 7g and the existing internal
    callers; the open-or-refresh promotion is internal."""
    uri = resolved.as_uri()
    language_id = _LANG_TO_LSP_LANGUAGE_ID.get(lang, lang)
    force_did_save = lang in _DIDSAVE_ON_REFRESH_LANGS
    # Pass resolved_path + lang so the LspSubprocess respawn snapshot
    # (open_uri_meta) carries enough information for the bridge to
    # re-walk through _dispatch_path on respawn -- without these the
    # respawn replay would have to reconstruct paths from URI strings,
    # bypassing the workspace-bounded sandbox.
    lsp.apply_text(uri, language_id, text, mtime_ns,
                   force_did_save=force_did_save,
                   resolved_path=str(resolved),
                   lang=lang)
    return uri


# LSP spec: Position.line and Position.character are `uinteger`
# (0 <= v < 2**31). Validate at tool entry so a malicious agent
# cannot push negative or oversized values into the LSP server.
_POSITION_MAX = (2 ** 31) - 1


def _validate_position(line: Any, character: Any) -> tuple[int, int]:
    """Validate an LSP Position. Returns (line, character) as ints.
    Strict int-only -- does NOT coerce numeric strings or truncate
    floats. Raises LspError('lsp-position-invalid') for:
      - bool inputs (bool is subclass of int; reject explicitly so
        True/False don't slip past the int check)
      - any non-int type (str like "10", float like 1.9, None, etc.)
      - negative values
      - values >= 2**31

    Codex post-implementation review of the per-LSP concurrency
    surface caught the prior implementation's `int(v)` coercion as
    Low: `int("10")` accepted a string and `int(1.9)` truncated to 1
    silently, both of which violate the documented contract. The
    JSON-RPC layer hands us native Python ints from MCP-client JSON
    payloads, so non-int inputs are always a caller bug -- reject
    them rather than coercing."""
    out: list[int] = []
    for name, v in (("line", line), ("character", character)):
        if isinstance(v, bool):
            raise LspError(
                "lsp-position-invalid",
                f"{name}={v!r} is a bool; LSP Position requires uinteger",
                field=name,
                value=v,
            )
        if not isinstance(v, int):
            raise LspError(
                "lsp-position-invalid",
                f"{name}={v!r} (type {type(v).__name__}) is not an int; "
                "no coercion of strings / floats / None",
                field=name,
                value=repr(v),
            )
        if v < 0 or v > _POSITION_MAX:
            raise LspError(
                "lsp-position-invalid",
                f"{name}={v} is outside LSP uinteger range "
                f"[0, {_POSITION_MAX}]",
                field=name,
                value=v,
            )
        out.append(v)
    return out[0], out[1]


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


# Bound the per-call completion payload so an LSP that returns
# thousands of items (clangd on a header inside <iostream>, pyright
# on `import _`) does not blow MCP-client buffers or agent context.
# 50 mirrors the documented checkpoint and matches the truncation
# every production bridge uses.
_COMPLETION_MAX_ITEMS = 50

# Bound the call-hierarchy follow-up fan-out. callHierarchy/prepare
# can return many CallHierarchyItems on overloaded symbols, generated
# code, or a misbehaving LSP. Each anchor triggers one
# incomingCalls/outgoingCalls request, so an unbounded walk could
# turn a single MCP call into thousands of sequential LSP requests.
# We bound the operation TWO ways:
#   - _CALL_HIERARCHY_MAX_ANCHORS: hard cap on count (generous for
#     legitimate overload sets; clangd typically returns 1-3).
#   - _CALL_HIERARCHY_DEADLINE_S: total wall-clock budget across the
#     whole operation (prepare + every follow-up). When exhausted we
#     stop issuing further follow-ups and return what we have with
#     `deadline_exceeded=True`. Cap-only protection still allowed a
#     wedged LSP to tie up an interactive MCP call for ~8 minutes
#     (32 * 15 s per-request timeout); the deadline keeps the
#     handler interactive even when the LSP is degraded. Codex
#     post-commit perf review flagged the cap-only fix as High and
#     recommended a wall-clock budget.
_CALL_HIERARCHY_MAX_ANCHORS = 32
_CALL_HIERARCHY_DEADLINE_S = 30.0
# Per-follow-up timeout: tighter than the default 15s used elsewhere
# so any single wedged anchor cannot consume the whole deadline.
_CALL_HIERARCHY_FOLLOW_TIMEOUT_S = 5.0


def _normalize_completion(result: Any) -> dict:
    """Normalize textDocument/completion response.

    LSP 3.17 shapes:
      - null                                  -> {items: [], isIncomplete: False, truncated: False}
      - CompletionItem[]                      -> wrap as items
      - CompletionList {isIncomplete, items}  -> use as-is
    Per item we keep label / kind / detail / documentation only --
    insertText/textEdit/additionalTextEdits/command are dropped
    because the read-only MCP boundary forbids applying edits anyway,
    so leaving them in would mislead the agent into thinking they are
    actionable here. Truncates to _COMPLETION_MAX_ITEMS and reports
    that fact via the truncated/total fields so a caller can ask for
    a narrower position to see the rest."""
    if result is None:
        return {"items": [], "isIncomplete": False, "truncated": False, "total": 0}
    if isinstance(result, list):
        items_in = result
        is_incomplete = False
    elif isinstance(result, dict):
        items_in = result.get("items") or []
        is_incomplete = bool(result.get("isIncomplete", False))
    else:
        return {"items": [], "isIncomplete": False, "truncated": False, "total": 0}
    if not isinstance(items_in, list):
        items_in = []
    # Filter to dict CompletionItem entries BEFORE applying the cap.
    # If we slice first and filter second, a malformed LSP response
    # whose first 50 entries are non-dict would return zero usable
    # items even when valid entries exist later in the response. Codex
    # adversarial review flagged this as Low; we want the cap to bound
    # the output of useful items, not raw input slots.
    valid_items = [it for it in items_in if isinstance(it, dict)]
    raw_total = len(items_in)
    valid_total = len(valid_items)
    truncated = valid_total > _COMPLETION_MAX_ITEMS
    items_out: list[dict] = []
    for item in valid_items[:_COMPLETION_MAX_ITEMS]:
        out_item = {
            "label": item.get("label"),
            "kind": item.get("kind"),
            "detail": item.get("detail"),
            "documentation": item.get("documentation"),
        }
        items_out.append(out_item)
    return {
        "items": items_out,
        "isIncomplete": is_incomplete,
        "truncated": truncated,
        # `total` is the count of usable (dict-shaped) CompletionItems.
        # `raw_total` exposes the unfiltered input length so a caller
        # can detect "mostly garbage from a misbehaving LSP" vs "real
        # 50+ items withheld".
        "total": valid_total,
        "raw_total": raw_total,
    }


def _normalize_signature_help(result: Any) -> dict:
    """Normalize textDocument/signatureHelp response.

    LSP 3.17 SignatureHelp { signatures: SignatureInformation[],
    activeSignature?: uinteger, activeParameter?: uinteger }.
    Returns {signatures: [...], active_signature: int|None,
    active_parameter: int|None}. Empty signatures -> active_* set to
    None so the caller doesn't read garbage indices."""
    if result is None or not isinstance(result, dict):
        return {"signatures": [], "active_signature": None,
                "active_parameter": None}
    sigs = result.get("signatures")
    if not isinstance(sigs, list):
        sigs = []
    active_sig = result.get("activeSignature")
    active_param = result.get("activeParameter")
    if not isinstance(active_sig, int) or len(sigs) == 0:
        active_sig = None
    if not isinstance(active_param, int):
        active_param = None
    return {
        "signatures": sigs,
        "active_signature": active_sig,
        "active_parameter": active_param,
    }


def _normalize_call_hierarchy_items(result: Any) -> list:
    """callHierarchy/prepare returns CallHierarchyItem[] | null.
    Pass-through as a list; null becomes []."""
    if result is None:
        return []
    if not isinstance(result, list):
        return []
    return [it for it in result if isinstance(it, dict)]


def _normalize_call_hierarchy_calls(result: Any, key: str) -> list:
    """Normalize callHierarchy/incomingCalls or outgoingCalls.

    Incoming: CallHierarchyIncomingCall { from: CallHierarchyItem,
    fromRanges: Range[] }.
    Outgoing: CallHierarchyOutgoingCall { to: CallHierarchyItem,
    fromRanges: Range[] }.

    `key` is "from" for incoming, "to" for outgoing. Returns a flat
    list of {item, ranges} dicts; the agent can read item.uri / .name
    / .range without picking apart the LSP variant tag."""
    if result is None or not isinstance(result, list):
        return []
    out: list[dict] = []
    for entry in result:
        if not isinstance(entry, dict):
            continue
        item = entry.get(key)
        if not isinstance(item, dict):
            continue
        ranges = entry.get("fromRanges")
        if not isinstance(ranges, list):
            ranges = []
        out.append({"item": item, "ranges": ranges})
    return out


def _normalize_code_actions(result: Any) -> list:
    """Normalize textDocument/codeAction response.

    LSP 3.17 returns (Command | CodeAction)[] | null. Pass through
    each entry as-is (with title / kind / diagnostics / isPreferred /
    edit / command intact -- the MCP tool surface returns metadata
    only and the read-only boundary blocks applyEdit / executeCommand
    at the runtime gate, so leaving the WorkspaceEdit visible lets a
    caller READ what the action would do without being able to apply
    it)."""
    if result is None:
        return []
    if not isinstance(result, list):
        return []
    return [a for a in result if isinstance(a, dict)]


def _validate_range(rng: Any) -> dict:
    """Validate an LSP Range = {start: Position, end: Position}.
    Returns the canonical dict with int line/character on each end.
    Raises lsp-range-invalid for missing keys, bad types, or positions
    that fail _validate_position. Used by code_action which takes a
    Range argument."""
    if not isinstance(rng, dict):
        raise LspError(
            "lsp-range-invalid",
            f"range must be a dict {{start, end}}, got {type(rng).__name__}",
            value=repr(rng),
        )
    out: dict = {}
    for endpoint in ("start", "end"):
        pos = rng.get(endpoint)
        if not isinstance(pos, dict):
            raise LspError(
                "lsp-range-invalid",
                f"range.{endpoint} must be a Position dict, "
                f"got {type(pos).__name__}",
                value=repr(pos),
                endpoint=endpoint,
            )
        line_v, char_v = _validate_position(pos.get("line"),
                                            pos.get("character"))
        out[endpoint] = {"line": line_v, "character": char_v}
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
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
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
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
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
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
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
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
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
                # Return the structured error envelope, not a
                # flattened string, so the per-LSP errors dict has
                # the same shape as the single-LSP path does for its
                # caller via _call_lsp(). Codex review-pass
                # consistency finding.
                return lang_tag, [], exc.to_envelope()

        # Track which futures correspond to which lang_tag so the
        # overall-timeout path can mark the un-completed ones with a
        # structured error rather than raising (fail-soft contract:
        # partial results + per-lang error detail beats all-or-nothing
        # for an agent that asked across every language).
        futures_by_fut: dict = {}
        # Manual pool lifecycle: if we used `with ThreadPoolExecutor`
        # the context-manager __exit__ would block on shutdown(wait=
        # True) while a stuck worker (e.g. LSP pipe write blocked
        # past its own 5 s timeout) continued running -- defeating
        # the fail-soft contract. Explicit shutdown(wait=False,
        # cancel_futures=True) on the timeout path returns to the
        # caller immediately; stuck workers continue running in the
        # background and will unblock on their own timeout or LSP
        # shutdown. Codex adversarial review flagged the
        # with-block as High.
        pool = concurrent.futures.ThreadPoolExecutor(
            max_workers=max(1, len(snapshot))
        )
        try:
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
                # Overall deadline hit. Harvest done futures, record
                # in-flight workers as lsp-overall-timeout, then tell
                # the pool to stop waiting. cancel_futures=True
                # cancels the queued work; already-running workers
                # cannot be interrupted from the outside (Python
                # threads) but they carry their own per-call 5 s
                # request timeout and will exit shortly after.
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
                        # Match the per-call error shape produced by
                        # _one() via exc.to_envelope(): a dict with
                        # {error, detail, ...}. The overall-timeout
                        # branch previously emitted a plain string
                        # here, leaving `errors[lang]` union-typed.
                        # Codex consistency review caught it.
                        errors[lang_tag] = LspError(
                            "lsp-overall-timeout",
                            "workspace_symbol 10s deadline exceeded "
                            "while this LSP was still in-flight",
                            lang=lang_tag,
                            method="workspace/symbol",
                        ).to_envelope()
                pool.shutdown(wait=False, cancel_futures=True)
            else:
                pool.shutdown(wait=True)
        except BaseException:
            # Any other exception (KeyboardInterrupt, SystemExit,
            # unexpected errors): release the pool without blocking.
            pool.shutdown(wait=False, cancel_futures=True)
            raise
        # Drain any pending publishDiagnostics on every snapshot LSP
        # so a follow-on `diagnostics(path)` MCP call returns the
        # most recent set instead of a value the reader thread had
        # not yet stored. The LSP wire-order guarantee (notifications
        # emitted before a response are parsed first) covers
        # diagnostics generated by didOpen/didChange we already
        # forwarded; this brief yield covers the narrow race window
        # where publishDiagnostics arrived AFTER workspace/symbol's
        # response was queued but BEFORE the bridge returned to the
        # MCP caller.
        #
        # ONE 50 ms drain window for the whole fan-out -- not
        # per-instance. Each LSP has its own reader thread; a
        # single yield lets ALL of them drain concurrently. Codex
        # perf review of the per-instance loop flagged the original
        # implementation as Medium: 5 live LSPs would cost 250ms of
        # serial sleep after the parallel work was already done.
        if snapshot:
            try:
                snapshot[0][1].flush_notifications(timeout=0.05)
            except Exception:
                pass
        total = sum(len(v) for v in per_lang.values())
        return {
            "query": str(query),
            "per_lang": per_lang,
            "total": total,
            "errors": errors,
        }
    srv.tool(name="workspace_symbol",
             description="LSP workspace/symbol query. When `lang` is "
                         "given, routes to that one spawner and returns "
                         "{query, lang, symbols: [...]}. When `lang` is "
                         "None, queries every already-spawned LSP in "
                         "parallel and returns {query, per_lang: {lang: "
                         "[...]}, total: N, errors: {lang: envelope}}. "
                         "Does NOT cold-spawn LSPs for lang=None -- only "
                         "queries the languages currently spawned."
                         )(workspace_symbol)

    def document_symbol(path: str) -> dict:
        """LSP textDocument/documentSymbol. Returns the hierarchical
        DocumentSymbol tree or flat SymbolInformation list depending
        on what the LSP advertised. The response is passed through
        as-is inside the `symbols` key."""
        def _op() -> Any:
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
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

    # ---------------------------------------------------------------
    # Extended LSP tools (TODO-07 in 00-infrastructure, second tier).
    # All read-only. code_action enumerates available actions but
    # never invokes workspace/applyEdit or workspace/executeCommand
    # (still in _FORBIDDEN_LSP_METHODS). textDocument/codeAction
    # itself is read-only per LSP 3.17 and is intentionally NOT in
    # the deny set.
    # ---------------------------------------------------------------

    def completion(path: str, line: int, character: int,
                   trigger_character: Optional[str] = None) -> dict:
        """LSP textDocument/completion. Returns the top
        _COMPLETION_MAX_ITEMS items by server order. trigger_character,
        when supplied, sets context.triggerKind=2 (TriggerCharacter)
        and forwards the literal so completion-after-`.` style
        prompting works; otherwise the kind is 1 (Invoked)."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            tc: Optional[str] = None
            if trigger_character is not None:
                if not isinstance(trigger_character, str):
                    raise LspError(
                        "lsp-completion-trigger-invalid",
                        f"trigger_character must be a string, got "
                        f"{type(trigger_character).__name__}",
                        value=repr(trigger_character),
                    )
                # LSP CompletionTriggerKind.TriggerCharacter expects a
                # single character; reject obvious abuse but allow
                # multi-char tokens like "->" that some LSPs document.
                if len(trigger_character) == 0 or len(trigger_character) > 4:
                    raise LspError(
                        "lsp-completion-trigger-invalid",
                        "trigger_character length must be 1..4",
                        value=trigger_character,
                    )
                tc = trigger_character
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            params: dict = {
                "textDocument": {"uri": uri},
                "position": {"line": line_v, "character": char_v},
            }
            if tc is not None:
                params["context"] = {"triggerKind": 2,
                                     "triggerCharacter": tc}
            else:
                params["context"] = {"triggerKind": 1}
            raw = lsp.request(
                "textDocument/completion", params, timeout=15.0,
            )
            normalized = _normalize_completion(raw)
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                **normalized,
            }
        return _call_lsp(_op)
    srv.tool(name="completion",
             description="LSP textDocument/completion at (line, "
                         "character). Returns up to "
                         f"{_COMPLETION_MAX_ITEMS} items "
                         "{label, kind, detail, documentation}; "
                         "isIncomplete + truncated + total surface "
                         "whether more results were withheld.")(completion)

    def signature_help(path: str, line: int, character: int) -> dict:
        """LSP textDocument/signatureHelp. Useful while synthesizing
        a call: surfaces the active signature + active parameter
        index so an agent can fill the right argument slot."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            raw = lsp.request(
                "textDocument/signatureHelp",
                {"textDocument": {"uri": uri},
                 "position": {"line": line_v, "character": char_v}},
                timeout=15.0,
            )
            normalized = _normalize_signature_help(raw)
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                **normalized,
            }
        return _call_lsp(_op)
    srv.tool(name="signature_help",
             description="LSP textDocument/signatureHelp at (line, "
                         "character). Returns {signatures, "
                         "active_signature, active_parameter}; "
                         "active_* are None when the LSP returns "
                         "no signatures.")(signature_help)

    def type_definition(path: str, line: int, character: int) -> dict:
        """LSP textDocument/typeDefinition. Distinct from `definition`
        for typedef / using / class-alias redirects: clicking on a
        variable's type name lands on the type declaration, not the
        variable's own definition."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            raw = lsp.request(
                "textDocument/typeDefinition",
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
    srv.tool(name="type_definition",
             description="LSP textDocument/typeDefinition at (line, "
                         "character). Resolves a value's TYPE to its "
                         "declaration; distinct from `definition` "
                         "for typedef / using / aliases.")(type_definition)

    def implementation(path: str, line: int, character: int) -> dict:
        """LSP textDocument/implementation. For an interface or
        abstract method, returns concrete implementations; for a
        virtual method, returns overrides."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            raw = lsp.request(
                "textDocument/implementation",
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
    srv.tool(name="implementation",
             description="LSP textDocument/implementation at (line, "
                         "character). Returns concrete implementors / "
                         "overrides for interfaces, abstract methods, "
                         "vtables.")(implementation)

    def declaration(path: str, line: int, character: int) -> dict:
        """LSP textDocument/declaration. Useful when `definition`
        returns the .c body but the agent wants the .h prototype --
        for headers vs. translation-unit-local definitions."""
        def _op() -> Any:
            line_v, char_v = _validate_position(line, character)
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            raw = lsp.request(
                "textDocument/declaration",
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
    srv.tool(name="declaration",
             description="LSP textDocument/declaration at (line, "
                         "character). Returns the declaration site "
                         "(typically a header) -- useful when "
                         "`definition` lands on the .c body and the "
                         "agent wants the .h prototype.")(declaration)

    def _call_hierarchy_one_step(method: str, key: str,
                                 path: str, line: int,
                                 character: int) -> dict:
        """Shared body for incoming/outgoing call-hierarchy. The LSP
        contract is two-step: callHierarchy/prepare returns
        CallHierarchyItem[] anchoring the symbol; for each item, a
        follow-up callHierarchy/incomingCalls or outgoingCalls returns
        the actual edges. We collapse both into a single MCP call:
        run prepare, then issue the follow-up for EVERY prepared
        anchor and bundle the per-anchor results into `anchors`.

        Walking every prepared item (rather than just prepared[0])
        matters for overloaded methods, partial / generated symbols,
        and any server that returns multiple valid anchors -- if we
        only resolved the first, the resulting caller/callee graph
        would silently omit the others with no API to ask for them.
        Codex adversarial review of the initial implementation
        flagged this as Medium. The cumulative LSP cost is bounded
        by `prepared_count` (clangd typically returns 1-2 items)."""
        import time as _time
        line_v, char_v = _validate_position(line, character)
        resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
        lsp = _get_or_spawn(lang, workspace_root)
        uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
        deadline = _time.monotonic() + _CALL_HIERARCHY_DEADLINE_S
        prepared_raw = lsp.request(
            "textDocument/prepareCallHierarchy",
            {"textDocument": {"uri": uri},
             "position": {"line": line_v, "character": char_v}},
            timeout=15.0,
        )
        prepared = _normalize_call_hierarchy_items(prepared_raw)
        if not prepared:
            return {
                "uri": uri,
                "lang": lang,
                "line": line_v,
                "character": char_v,
                "prepared_count": 0,
                "prepared_total": 0,
                "anchors": [],
                "truncated": False,
                "deadline_exceeded": False,
                "note": "prepareCallHierarchy returned no items at "
                        "this position; the LSP does not see a "
                        "callable symbol here.",
            }
        # Cap the fan-out before issuing follow-up requests. Codex
        # post-commit review flagged unbounded prepared length as
        # High; a second perf-review High flagged the cap-only fix
        # as still allowing ~8 minute interactive latency on a
        # wedged LSP. Defense-in-depth: count cap PLUS wall-clock
        # deadline; whichever fires first stops the walk. Both
        # surface state in the response so the caller can tell
        # WHICH bound was hit.
        prepared_total = len(prepared)
        truncated = prepared_total > _CALL_HIERARCHY_MAX_ANCHORS
        anchors_to_walk = prepared[:_CALL_HIERARCHY_MAX_ANCHORS]
        anchors_out: list[dict] = []
        deadline_exceeded = False
        for anchor in anchors_to_walk:
            now = _time.monotonic()
            if now >= deadline:
                deadline_exceeded = True
                break
            # Shrink the per-follow-up timeout to whatever the
            # remaining deadline allows, so a slow LSP cannot
            # individually overshoot. Floor at a small positive
            # value (the request() validator rejects 0/negative);
            # if remaining < the floor we treat it as deadline-hit.
            remaining = deadline - now
            per_call_timeout = min(
                _CALL_HIERARCHY_FOLLOW_TIMEOUT_S, remaining,
            )
            if per_call_timeout < 0.1:
                deadline_exceeded = True
                break
            try:
                follow = lsp.request(
                    method, {"item": anchor}, timeout=per_call_timeout,
                )
            except LspError as exc:
                # A per-anchor timeout is not fatal: record the
                # anchor with an empty calls list + the error and
                # keep walking. Other anchors may still resolve
                # within the remaining budget.
                if exc.kind == "lsp-timeout":
                    anchors_out.append({
                        "anchor": anchor,
                        "calls": [],
                        "anchor_error": exc.to_envelope(),
                    })
                    continue
                raise
            anchors_out.append({
                "anchor": anchor,
                "calls": _normalize_call_hierarchy_calls(follow, key),
            })
        return {
            "uri": uri,
            "lang": lang,
            "line": line_v,
            "character": char_v,
            # prepared_count = anchors actually walked (resolved
            # follow-ups); prepared_total = anchors the LSP returned
            # before the cap. The two diverge on truncation OR on
            # deadline exhaustion.
            "prepared_count": len(anchors_out),
            "prepared_total": prepared_total,
            "truncated": truncated,
            "deadline_exceeded": deadline_exceeded,
            "anchors": anchors_out,
        }

    def call_hierarchy_incoming(path: str, line: int, character: int) -> dict:
        """LSP callHierarchy/prepare + callHierarchy/incomingCalls.
        Returns callers of the symbol at (line, character)."""
        def _op() -> Any:
            return _call_hierarchy_one_step(
                "callHierarchy/incomingCalls", "from",
                path, line, character,
            )
        return _call_lsp(_op)
    srv.tool(name="call_hierarchy_incoming",
             description="LSP callHierarchy/prepare then "
                         "incomingCalls. Returns {prepared_count, "
                         "prepared_total, truncated, anchors: "
                         "[{anchor, calls: [{item, ranges}]}]} -- "
                         "one entry per prepared CallHierarchyItem "
                         "so overloaded symbols / multi-anchor "
                         "positions don't drop callers. "
                         "prepared_count = anchors actually walked; "
                         "prepared_total = anchors the LSP returned "
                         f"before the cap of {_CALL_HIERARCHY_MAX_ANCHORS}; "
                         "truncated = True when prepared_total > "
                         "cap (a buggy or hostile LSP cannot drive "
                         "an unbounded fan-out).")(call_hierarchy_incoming)

    def call_hierarchy_outgoing(path: str, line: int, character: int) -> dict:
        """LSP callHierarchy/prepare + callHierarchy/outgoingCalls.
        Returns what the symbol at (line, character) calls."""
        def _op() -> Any:
            return _call_hierarchy_one_step(
                "callHierarchy/outgoingCalls", "to",
                path, line, character,
            )
        return _call_lsp(_op)
    srv.tool(name="call_hierarchy_outgoing",
             description="LSP callHierarchy/prepare then "
                         "outgoingCalls. Returns {prepared_count, "
                         "prepared_total, truncated, anchors: "
                         "[{anchor, calls: [{item, ranges}]}]} -- "
                         "one entry per prepared CallHierarchyItem "
                         "so overloaded symbols / multi-anchor "
                         "positions don't drop callees. Same cap "
                         f"semantics as call_hierarchy_incoming "
                         f"({_CALL_HIERARCHY_MAX_ANCHORS} anchors "
                         "max).")(call_hierarchy_outgoing)

    def code_action(path: str, range: dict,
                    diagnostic: Optional[dict] = None) -> dict:
        """LSP textDocument/codeAction. READ-ONLY: returns the list
        of available actions (Command | CodeAction). NEVER applies
        them -- the workspace/applyEdit and workspace/executeCommand
        methods that would actually mutate the file are blocked at
        the runtime gate in _FORBIDDEN_LSP_METHODS.

        `range` is an LSP Range = {start: {line, character},
        end: {line, character}}. `diagnostic`, if supplied, narrows
        the actions to fixes for that one diagnostic; without it the
        LSP returns every action available in the range."""
        def _op() -> Any:
            r = _validate_range(range)
            ctx: dict = {"diagnostics": []}
            if diagnostic is not None:
                if not isinstance(diagnostic, dict):
                    raise LspError(
                        "lsp-codeaction-diagnostic-invalid",
                        f"diagnostic must be a dict, got "
                        f"{type(diagnostic).__name__}",
                        value=repr(diagnostic),
                    )
                ctx["diagnostics"] = [diagnostic]
            resolved, lang, text, mtime_ns = _dispatch_path(path, workspace_root)
            lsp = _get_or_spawn(lang, workspace_root)
            uri = _ensure_open_for(lsp, resolved, lang, text, mtime_ns)
            raw = lsp.request(
                "textDocument/codeAction",
                {"textDocument": {"uri": uri},
                 "range": r,
                 "context": ctx},
                timeout=15.0,
            )
            return {
                "uri": uri,
                "lang": lang,
                "range": r,
                "actions": _normalize_code_actions(raw),
                "note": "READ-ONLY: actions are listed but never "
                        "applied. workspace/applyEdit + "
                        "workspace/executeCommand are blocked at the "
                        "runtime gate.",
            }
        return _call_lsp(_op)
    srv.tool(name="code_action",
             description="LSP textDocument/codeAction over a Range. "
                         "READ-ONLY: returns the available "
                         "(Command | CodeAction) list as metadata; "
                         "the bridge never invokes applyEdit or "
                         "executeCommand. Pass an optional "
                         "`diagnostic` dict to scope to one "
                         "diagnostic's fixes.")(code_action)

    # ---------------------------------------------------------------
    # Meta tools (no LSP wire traffic; safe to call when every LSP
    # is FAILED). Documented in the Subprocess Health Monitoring
    # section.
    # ---------------------------------------------------------------

    def health() -> dict:
        """Return per-(lang, root) crash + restart bookkeeping for
        every LSP the bridge has ever spawned in this session.

        Schema:
          {languages: {<lang>: {root, status, restart_count,
                                last_crash_reason, last_restart_at,
                                pid, alive}}}
        Where status is one of:
          spawned  -- entry exists but never finished its first
                      successful spawn (rare; usually transient).
          healthy  -- last spawn succeeded; subprocess alive.
          backoff  -- last attempt crashed; FAILED threshold not
                      yet reached; next request triggers respawn
                      with exponential backoff.
          failed   -- crossed the FAILED threshold; further
                      requests return lsp-persistently-crashing
                      until the bridge restarts.

        No LSP wire traffic; safe to call at any time, including
        when every LSP is dead. Read-only. Walks _LSP_HEALTH +
        _LIVE_LSPS under _LIVE_LSPS_LOCK so the snapshot is
        consistent."""
        out: dict[str, dict] = {}
        with _LIVE_LSPS_LOCK:
            keys = sorted(set(_LSP_HEALTH.keys()) | set(_LIVE_LSPS.keys()))
            for key in keys:
                lang_tag, root_str = key
                h = _LSP_HEALTH.get(key) or {
                    "restart_count": 0,
                    "recent_crash_times": [],
                    "failed": False,
                    "last_crash_reason": None,
                    "last_restart_at": None,
                    "status": "spawned",
                }
                inst = _LIVE_LSPS.get(key)
                # Detect a stale "healthy" status when the cached
                # instance has crashed but no caller has run
                # _get_or_spawn yet to record it. _health is a
                # read-only probe; it does NOT mutate state, just
                # surfaces the truth.
                effective_status = h["status"]
                alive = bool(inst is not None and inst.alive)
                if effective_status == "healthy" and not alive:
                    effective_status = "crashed (pending respawn)"
                out.setdefault(lang_tag, {})[root_str] = {
                    "status": effective_status,
                    "restart_count": h["restart_count"],
                    "last_crash_reason": h["last_crash_reason"],
                    "last_restart_at": h["last_restart_at"],
                    "failed": h["failed"],
                    "pid": inst.pid if inst is not None else None,
                    "alive": alive,
                }
        return {"languages": out}
    srv.tool(name="_health",
             description="Per-LSP crash + restart bookkeeping. "
                         "Returns {languages: {<lang>: {<root>: "
                         "{status, restart_count, last_crash_reason, "
                         "last_restart_at, pid, alive, failed}}}}. "
                         "status: spawned | healthy | backoff | "
                         "failed | 'crashed (pending respawn)'. "
                         "Read-only; no LSP wire traffic; safe to "
                         "call when every LSP is dead.")(health)

    return srv


def _introspect_tools(srv: Any) -> dict[str, dict]:
    """Enumerate the registered MCP tools + their parameter schemas.

    Uses srv.list_tools() first (public API on current SDK); falls
    back to _tool_manager.list_tools() and finally the private
    _tool_manager._tools dict. Codex design review
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


def _self_test_stress(workspace_root: Path) -> int:
    """Concurrent-call stress harness for the per-LSP serialization
    contract. Spawns clangd (SKIP if unavailable), opens
    src/kernel/main.c, then fans 100 parallel hover requests at
    multiple positions through `ThreadPoolExecutor`. Verifies:

      1. _io_lock + _pending_lock + _next_id_lock contention under
         100-way fan-in does not deadlock.
      2. Every Future completes within an overall 20 s deadline.
      3. No Future leaks: after the run, `lsp._pending` is empty.

    What this DOES NOT verify: request/response demux correctness.
    Clangd hover responses do not echo the request's (line,
    character), so a Future-swap bug between two callers requesting
    the same file at different positions cannot be detected from
    the response payload alone. Demux correctness is exercised by
    sub-test 8b (fake-LSP that DELIBERATELY reorders responses);
    this stress test exercises lock contention + leak detection
    under load.

    Pool lifecycle mirrors the workspace_symbol(lang=None) pattern in
    `_build_mcp` -- explicit `pool.shutdown(wait=False,
    cancel_futures=True)` on overall-deadline expiry, no `with`
    block that would block forever on stuck workers. Codex
    pre-implementation review of this section flagged the with-block
    pattern as Medium.
    """
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

    main_c = workspace_root / "src" / "kernel" / "main.c"
    try:
        resolved = main_c.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: resolving {main_c}: {exc}\n")
        return 1
    try:
        text = resolved.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: reading {resolved}: {exc}\n")
        return 1

    try:
        lsp = _get_or_spawn("c", workspace_root)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: clangd spawn: {exc}\n")
        return 1
    uri = resolved.as_uri()
    try:
        lsp.ensure_open(uri, "c", text, version=1)
    except LspError as exc:
        sys.stderr.write(f"[lsp-mcp] FAIL: clangd didOpen: {exc}\n")
        return 1

    # Pick a small pool of well-known positions that clangd hovers
    # cleanly. We rotate through them across 100 calls so different
    # in-flight requests carry different (line, character) values,
    # which exercises clangd's own request multiplexer (not the
    # bridge's -- bridge demux is sub-test 8b's job).
    candidate_positions: list[tuple[int, int]] = []
    for idx, line in enumerate(text.splitlines()):
        for needle in ("kernel_main", "magic", "mbi"):
            col = line.find(needle)
            if col >= 0:
                candidate_positions.append((idx, col + 1))
                break
        if len(candidate_positions) >= 5:
            break
    if not candidate_positions:
        sys.stderr.write(
            "[lsp-mcp] FAIL: could not find 5 hoverable positions in "
            f"{resolved}\n"
        )
        return 1

    n_calls = 100
    overall_deadline_s = 20.0
    requests: list[tuple[int, tuple[int, int]]] = []
    for i in range(n_calls):
        pos = candidate_positions[i % len(candidate_positions)]
        requests.append((i, pos))

    import concurrent.futures
    import time as _time

    def _one_call(req_id: int, pos: tuple[int, int]) -> tuple:
        line, character = pos
        try:
            lsp.request(
                "textDocument/hover",
                {"textDocument": {"uri": uri},
                 "position": {"line": line, "character": character}},
                timeout=10.0,
            )
            return req_id, None
        except LspError as exc:
            return req_id, f"{exc.kind}: {exc.detail}"

    pool = concurrent.futures.ThreadPoolExecutor(max_workers=n_calls)
    futures: dict = {}
    completed = 0
    errors: list[tuple[int, str]] = []
    start = _time.monotonic()
    try:
        for req_id, pos in requests:
            fut = pool.submit(_one_call, req_id, pos)
            futures[fut] = req_id
        try:
            for fut in concurrent.futures.as_completed(
                    list(futures.keys()), timeout=overall_deadline_s):
                rid, err = fut.result()
                if err is not None:
                    errors.append((rid, err))
                    continue
                completed += 1
        except concurrent.futures.TimeoutError:
            sys.stderr.write(
                f"[lsp-mcp] FAIL: stress overall deadline "
                f"({overall_deadline_s} s) exceeded; "
                f"completed={completed}/{n_calls}\n"
            )
            pool.shutdown(wait=False, cancel_futures=True)
            return 1
        pool.shutdown(wait=True)
    except BaseException:
        pool.shutdown(wait=False, cancel_futures=True)
        raise
    elapsed = _time.monotonic() - start

    # Pending dict must be empty (Future leak detector).
    pending_len = 0
    try:
        with lsp._pending_lock:  # type: ignore[attr-defined]
            pending_len = len(lsp._pending)  # type: ignore[attr-defined]
    except Exception:
        pass
    if pending_len != 0:
        sys.stderr.write(
            "[lsp-mcp] FAIL: stress left "
            f"{pending_len} Future(s) in lsp._pending; correlation leak\n"
        )
        return 1

    if errors:
        # Per-call errors are tolerated below a threshold (LSPs can
        # return null for some positions). Hard FAIL only if more
        # than 10% errored; otherwise note them.
        if len(errors) > n_calls // 10:
            sys.stderr.write(
                f"[lsp-mcp] FAIL: stress had {len(errors)}/{n_calls} "
                "per-call errors (>10% threshold). Examples:\n"
            )
            for r, e in errors[:5]:
                sys.stderr.write(f"  req {r}: {e}\n")
            return 1

    sys.stdout.write(
        f"[lsp-mcp] OK: stress {completed}/{n_calls} hover round-trips "
        f"in {elapsed:.2f}s "
        f"({completed/max(elapsed,0.001):.0f} req/s), "
        f"no Future leaks (demux contract validated by sub-test 8b)\n"
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
    p.add_argument(
        "--stress", action="store_true",
        help="with --self-test: run 100 concurrent hover calls against "
             "clangd to exercise per-LSP lock contention + Future-leak "
             "detection. SKIP when clangd is not installed. Demux "
             "correctness is exercised by sub-test 8b (fake-LSP reorder).",
    )
    args = p.parse_args(argv)

    workspace_root = _workspace_root_from_argv(args)

    try:
        if args.self_test:
            if args.tools:
                return _self_test_tools(workspace_root)
            if args.stress:
                return _self_test_stress(workspace_root)
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
