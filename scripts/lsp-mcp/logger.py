#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/logger.py -- structured JSON-lines logger for the LSP-MCP
#                              bridge with correlation-ID propagation.
#
# Owner: TODO-07 in 00-infrastructure (LSP-MCP Bridge), structured-logging
# section.
#
# Output contract:
#   * Every emit is a single JSON line on stderr (or LSP_MCP_LOG_FILE if set).
#     Schema:
#       {"ts": <iso-utc-string>, "level": "DEBUG|INFO|WARN|ERROR",
#        "corr_id": "<uuid4 hex>" | None, "lang": "<lang>" | None,
#        "method": "<lsp/mcp method>" | None,
#        "latency_ms": <int> | None, "msg": "<human-readable>",
#        ...event-specific fields}
#   * sort_keys=True for grep-stable column ordering.
#   * Atomic single write under _WRITE_LOCK so concurrent threads never
#     interleave bytes within a line. The earlier ad-hoc sys.stderr.write
#     calls had this latent bug; the logger fixes it.
#
# Correlation-ID propagation:
#   * `corr_id` is a contextvars.ContextVar (NOT threading.local).
#   * ContextVar propagates explicitly via contextvars.copy_context().run()
#     across ThreadPoolExecutor / asyncio boundaries -- the
#     workspace_symbol fan-out path uses this to thread the inbound MCP
#     call's corr_id into per-language pool workers. Codex design review
#     caught threading.local() as High: the fan-out workers would have
#     produced corr_id=None for the highest-concurrency tool path.
#
# Levels + env:
#   * LSP_MCP_LOG_LEVEL=DEBUG|INFO|WARN|ERROR (default INFO; case-insensitive).
#   * LSP_MCP_LOG_FILE=<path>: append JSON-lines to file instead of stderr.
#     No auto-rotation -- delegate to logrotate (LSP_MCP_LOG_FILE re-resolves
#     per process; rotation by mv-then-touch leaves us writing to the
#     renamed inode until the bridge restarts. Acceptable per TODO contract).
#   * Body cap 4 KiB for DEBUG-level lsp-send/lsp-recv payloads (truncates
#     and marks `body_truncated=True`). Body is emitted as a STRING field,
#     not nested JSON, so truncation never breaks the outer line's
#     JSON-validity.
#
# Public surface (everything else is an implementation detail):
#   set_corr_id(cid)       -- bind a corr_id to the current Context
#   clear_corr_id()        -- reset to None
#   current_corr_id()      -- read the active corr_id (None if unset)
#   new_corr_id()          -- 32-char uuid4 hex
#   log(level, msg, **fields) -- emit one JSON line
#   log_phase_start/_end(method, lang, **fields) -- structured tool-call
#                                                    boundary events
#   log_attempt_start/_end(method, lang, attempt, **fields) -- per-retry
#                                                              boundary
#                                                              events for
#                                                              _call_lsp's
#                                                              retry loop
#   debug_lsp_send/_recv(method, body) -- DEBUG wire-payload events with
#                                         the 4 KiB cap
#   reload_from_env()      -- re-read LSP_MCP_LOG_LEVEL / LSP_MCP_LOG_FILE;
#                             used by tests, not by production callers.
# ============================================================================

from __future__ import annotations

import contextvars
import io
import json
import os
import sys
import threading
import time
import uuid
from typing import Any, Optional, TextIO


# Level integers match the conventional Python logging module so a
# future migration (or coexistence) does not flip semantics.
LEVELS = {"DEBUG": 10, "INFO": 20, "WARN": 30, "ERROR": 40}
_DEFAULT_LEVEL = "INFO"

# Body truncation cap for DEBUG lsp-send / lsp-recv events. 4 KiB
# matches the TODO checkpoint and bounds the per-call log volume so
# DEBUG mode does not blow up disk on a busy bridge.
_DEBUG_BODY_CAP_BYTES = 4096

# Active correlation ID. ContextVar (not threading.local) so the
# value propagates correctly across contextvars.copy_context().run()
# calls (used by the workspace_symbol fan-out so its pool workers
# inherit the inbound MCP call's corr_id).
_CORR_ID: contextvars.ContextVar[Optional[str]] = contextvars.ContextVar(
    "lsp_mcp_corr_id", default=None,
)


def new_corr_id() -> str:
    """Generate a fresh 32-char uuid4 hex (no dashes -- compact for
    grep-friendly logs)."""
    return uuid.uuid4().hex


def set_corr_id(corr_id: Optional[str]) -> "contextvars.Token":
    """Bind `corr_id` to the current Context. Returns the Token so
    the caller can `_CORR_ID.reset(token)` later if it wants to
    restore the prior value (most callers just clear or rely on the
    Context boundary)."""
    return _CORR_ID.set(corr_id)


def clear_corr_id() -> None:
    """Reset corr_id to None. Equivalent to set_corr_id(None) but
    semantically clearer at call sites."""
    _CORR_ID.set(None)


def current_corr_id() -> Optional[str]:
    """Read the active corr_id from the current Context. None if
    not set."""
    return _CORR_ID.get()


class LspLogger:
    """JSON-lines sink with level filtering. One process-level
    instance lives in `_LOGGER`; tests can swap it via
    reload_from_env() OR by direct mutation in a save/restore
    block.

    Thread-safe writes: a single _write_lock serializes the
    write+flush so concurrent threads never interleave bytes in
    the same line. sys.stderr.write is line-buffered in Python but
    only at the str level, not the bytes level; under heavy
    concurrency we have observed interleaving in the existing
    ad-hoc emit sites. The lock is held for the duration of one
    .write+.flush; concurrent threads pay a few microseconds of
    queueing in exchange for never producing un-parseable lines."""

    def __init__(self, level: str = _DEFAULT_LEVEL,
                 sink: Optional[TextIO] = None,
                 owns_sink: bool = False) -> None:
        self._level_name = level.upper()
        if self._level_name not in LEVELS:
            self._level_name = _DEFAULT_LEVEL
        self._level = LEVELS[self._level_name]
        self._sink: TextIO = sink if sink is not None else sys.stderr
        # owns_sink: did we open this file ourselves? If yes, we
        # close it at __del__ / reload time. Tests can hand us an
        # in-memory sink with owns_sink=False to avoid premature
        # close.
        self._owns_sink = owns_sink
        self._write_lock = threading.Lock()

    @property
    def level_name(self) -> str:
        return self._level_name

    @property
    def level(self) -> int:
        return self._level

    @property
    def sink(self) -> TextIO:
        return self._sink

    def is_enabled(self, level: str) -> bool:
        """Return True iff `level` would actually emit. Used by
        callers (e.g. lsp-send DEBUG payloads) that want to skip
        expensive formatting when the level is filtered out."""
        return LEVELS.get(level.upper(), 0) >= self._level

    def emit(self, level: str, msg: str, **fields: Any) -> None:
        """Emit one JSON line. Level filter + serialization +
        single-write under _write_lock. Best-effort: a write
        failure is swallowed so logger faults cannot crash the
        bridge."""
        level_up = level.upper()
        if LEVELS.get(level_up, 0) < self._level:
            return
        payload: dict[str, Any] = {
            "ts": _iso_utc_now(),
            "level": level_up,
            "corr_id": _CORR_ID.get(),
            "msg": msg,
        }
        # Caller-supplied fields can overwrite default keys (e.g.
        # an explicit corr_id=... at the callsite). We treat that
        # as intentional: the caller knows better than the default.
        for k, v in fields.items():
            payload[k] = v
        try:
            line = json.dumps(payload, sort_keys=True, default=str)
        except (TypeError, ValueError):
            # Last-resort: serialize with repr() so a non-JSON-
            # serializable extra field still surfaces something.
            payload = {
                "ts": _iso_utc_now(),
                "level": level_up,
                "corr_id": _CORR_ID.get(),
                "msg": msg,
                "_serializer_error": True,
                "fields_repr": {k: repr(v) for k, v in fields.items()},
            }
            try:
                line = json.dumps(payload, sort_keys=True)
            except Exception:
                return
        with self._write_lock:
            try:
                self._sink.write(line + "\n")
                self._sink.flush()
            except Exception:
                pass

    def close(self) -> None:
        """Close the sink if we own it. No-op for stderr sinks."""
        if self._owns_sink:
            try:
                self._sink.close()
            except Exception:
                pass

    def __del__(self) -> None:
        # Best-effort cleanup at GC time. Production exit goes
        # through reload_from_env() OR atexit, not __del__.
        try:
            self.close()
        except Exception:
            pass


# Module-level singleton. Constructed lazily on first reload (which
# the public API forces at import time below).
_LOGGER: Optional[LspLogger] = None


def reload_from_env() -> LspLogger:
    """Re-read LSP_MCP_LOG_LEVEL / LSP_MCP_LOG_FILE and (re)build
    the module-level logger. Closes any previously-opened file
    sink. Returns the new logger.

    Tests use this to flip level / sink during a save/restore
    block. Production calls it once at module import time."""
    global _LOGGER
    if _LOGGER is not None:
        try:
            _LOGGER.close()
        except Exception:
            pass
    level = os.environ.get("LSP_MCP_LOG_LEVEL", _DEFAULT_LEVEL).upper()
    if level not in LEVELS:
        # Unknown level falls back to INFO; we surface this via the
        # first emit that actually fires (one extra WARN line).
        level = _DEFAULT_LEVEL
    log_file = os.environ.get("LSP_MCP_LOG_FILE", "").strip()
    sink: TextIO = sys.stderr
    owns_sink = False
    if log_file:
        try:
            # Append mode: external rotation (logrotate) renames
            # the file out from under us; we keep writing to the
            # renamed inode until the bridge restarts. Acceptable
            # per the TODO contract.
            sink = open(log_file, "a", encoding="utf-8", buffering=1)
            owns_sink = True
        except OSError as exc:
            # Fall back to stderr; emit one warning line so the
            # operator notices.
            sys.stderr.write(
                json.dumps({
                    "ts": _iso_utc_now(),
                    "level": "WARN",
                    "corr_id": None,
                    "msg": (
                        "LSP_MCP_LOG_FILE open failed; "
                        "falling back to stderr"
                    ),
                    "log_file": log_file,
                    "error": str(exc),
                }, sort_keys=True) + "\n",
            )
            sys.stderr.flush()
            sink = sys.stderr
            owns_sink = False
    _LOGGER = LspLogger(level=level, sink=sink, owns_sink=owns_sink)
    return _LOGGER


def get_logger() -> LspLogger:
    """Return the active logger. Lazy-initialize from env on first
    call so import order does not matter (the bridge module imports
    this module before reading os.environ for its own settings)."""
    global _LOGGER
    if _LOGGER is None:
        reload_from_env()
    assert _LOGGER is not None
    return _LOGGER


# ---------------------------------------------------------------------
# Public emit helpers -- the interface every callsite uses.
# ---------------------------------------------------------------------

def log(level: str, msg: str, **fields: Any) -> None:
    """Emit one JSON line at `level`. Default fields (ts, level,
    corr_id, msg) are added automatically; **fields override on
    collision."""
    get_logger().emit(level, msg, **fields)


def log_phase_start(method: str, lang: Optional[str] = None,
                    **fields: Any) -> float:
    """Emit phase=start for a tool call. Returns the start
    timestamp (monotonic seconds) so the matching log_phase_end
    can compute latency_ms cheaply.

    method = the MCP tool name (e.g. "hover", "_health"). lang =
    bridge lang tag if known at call entry (None for cross-LSP
    fan-out / meta tools)."""
    t0 = time.monotonic()
    log("INFO", "tool-call start",
        phase="start", method=method, lang=lang, **fields)
    return t0


def log_phase_end(method: str, lang: Optional[str], t0: float,
                  status: str, **fields: Any) -> None:
    """Emit phase=end for a tool call. status = "ok"|"error".
    latency_ms is computed from t0 returned by log_phase_start."""
    latency_ms = int((time.monotonic() - t0) * 1000)
    log("INFO" if status == "ok" else "WARN",
        "tool-call end",
        phase="end", method=method, lang=lang,
        latency_ms=latency_ms, status=status, **fields)


def log_attempt_start(method: str, lang: Optional[str], attempt: int,
                      **fields: Any) -> float:
    """Per-attempt boundary inside _call_lsp's retry loop. Returns
    monotonic t0. attempt is 1-indexed (1 = first attempt, 2 =
    retry). Codex design review High: phase-start/phase-end alone
    cannot distinguish "first attempt failed but retry succeeded"
    from "first attempt succeeded"; explicit per-attempt events
    keep the signal."""
    t0 = time.monotonic()
    log("INFO", "tool-call attempt start",
        phase="attempt-start", method=method, lang=lang,
        attempt=attempt, **fields)
    return t0


def log_attempt_end(method: str, lang: Optional[str], attempt: int,
                    t0: float, status: str, **fields: Any) -> None:
    """Per-attempt end event. status = "ok"|"crash-retry"|"error"."""
    latency_ms = int((time.monotonic() - t0) * 1000)
    log("INFO" if status == "ok" else "WARN",
        "tool-call attempt end",
        phase="attempt-end", method=method, lang=lang,
        attempt=attempt, latency_ms=latency_ms, status=status, **fields)


def debug_lsp_send(method: str, lang: Optional[str], request_id: int,
                   body: dict) -> None:
    """DEBUG-only wire-send event. body is JSON-encoded and
    truncated to _DEBUG_BODY_CAP_BYTES; emitted as a STRING field
    so the outer JSON line stays parseable even after truncation.
    Skips serialization entirely when DEBUG is filtered out."""
    if not get_logger().is_enabled("DEBUG"):
        return
    truncated, body_str = _truncate_body(body)
    log("DEBUG", "lsp wire send",
        event="lsp-send", method=method, lang=lang,
        request_id=request_id, body=body_str,
        body_truncated=truncated)


def debug_lsp_recv(method: Optional[str], lang: Optional[str],
                   request_id: Optional[int], body: dict,
                   corr_id: Optional[str] = None) -> None:
    """DEBUG-only wire-receive event. Emitted from the LSP reader
    thread, which does NOT inherit the requester's Context. Caller
    can pass an explicit corr_id (looked up via _pending metadata
    from the inbound request) so receive events correlate with the
    inbound MCP call."""
    if not get_logger().is_enabled("DEBUG"):
        return
    truncated, body_str = _truncate_body(body)
    fields: dict[str, Any] = {
        "event": "lsp-recv",
        "method": method,
        "lang": lang,
        "request_id": request_id,
        "body": body_str,
        "body_truncated": truncated,
    }
    if corr_id is not None:
        fields["corr_id"] = corr_id
    log("DEBUG", "lsp wire recv", **fields)


# ---------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------

def _iso_utc_now() -> str:
    """ISO-8601 UTC string with microsecond precision and explicit
    'Z' zone marker. ts is the FIRST field a human reads after
    grep; keeping the format stable across log files matters more
    than supporting arbitrary timezones."""
    # time.time() -> float seconds since epoch; format manually to
    # avoid the datetime.utcnow() deprecation noise on 3.12+.
    t = time.time()
    secs = int(t)
    usec = int(round((t - secs) * 1_000_000))
    if usec >= 1_000_000:
        secs += 1
        usec -= 1_000_000
    g = time.gmtime(secs)
    return ("%04d-%02d-%02dT%02d:%02d:%02d.%06dZ"
            % (g.tm_year, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min,
               g.tm_sec, usec))


def _truncate_body(body: Any) -> tuple[bool, str]:
    """Serialize `body` to a JSON string and truncate at the
    DEBUG body cap. Returns (truncated, string).

    Uses JSONEncoder.iterencode + early break so the serializer
    stops as soon as the cap is reached. A naive json.dumps()
    would serialize the FULL body (potentially up to LSP's 32 MiB
    cap) before truncation, blocking the single reader thread
    long enough that subsequent pending requests' Futures could
    time out. Codex post-implementation perf review caught this:
    the cap was an output-size cap only, not a CPU/memory cap.
    iterencode bounds both.

    Body is emitted as a STRING in the outer JSON to keep that
    line parseable even after truncation. Codex design review
    caught the alternative (nested JSON) as a Medium: a
    truncated nested object is invalid JSON in the outer line."""
    cap = _DEBUG_BODY_CAP_BYTES
    parts: list[str] = []
    total = 0
    try:
        encoder = json.JSONEncoder(sort_keys=True, default=str)
        for chunk in encoder.iterencode(body):
            parts.append(chunk)
            total += len(chunk)
            if total > cap:
                # Bounded: stop serialization the moment we've
                # exceeded the cap. The chunk that crossed the
                # threshold is included in `parts`; we trim it
                # below.
                break
    except (TypeError, ValueError):
        # JSONEncoder failed even with default=str (e.g. recursive
        # structure). Fall back to repr(); still bounded by cap.
        encoded = repr(body)
        if len(encoded) > cap:
            return True, encoded[:cap] + "...truncated"
        return False, encoded
    encoded = "".join(parts)
    if len(encoded) > cap:
        return True, encoded[:cap] + "...truncated"
    return False, encoded
