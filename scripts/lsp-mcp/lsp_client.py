#!/usr/bin/env python3
# ============================================================================
# scripts/lsp-mcp/lsp_client.py -- minimal LSP JSON-RPC 2.0 client for the
#                                  LSP-MCP bridge (TODO-07 in 00-infrastructure).
#
# Owner: TODO-07 TODO-07-lsp-mcp-bridge.md section 1 (Bridge Skeleton).
#
# Implements the narrow slice of Language Server Protocol 3.17 needed to
# route MCP tool calls at a spawned LSP:
#
#   * Header framing: "Content-Length: N\r\n\r\n<body>"
#   * Core methods this layer knows about:
#       initialize, initialized, textDocument/didOpen,
#       shutdown, exit
#     (every other LSP method is passed through verbatim by request().)
#   * Monotonic request id + response demultiplexing so two concurrent
#     requests on the same LspSubprocess do not swap replies.
#   * Timeout-bounded shutdown with SIGTERM-then-SIGKILL fallback.
#   * atexit cleanup so an unexpected bridge teardown does not leak
#     child processes.
#
# Stdlib-only (Python 3.10+). No `mcp` SDK dependency here -- that
# lives in bridge.py. This file exists on its own so the LSP wire
# layer is testable without pulling FastMCP into the import graph
# (self-test 10f: "pgrep empty after shutdown" uses this module
# directly).
# ============================================================================

from __future__ import annotations

import atexit
import io
import json
import os
import shutil
import signal
import subprocess
import threading
import time
import weakref
from concurrent.futures import Future, TimeoutError as FutureTimeoutError
from pathlib import Path
from typing import Any, Optional


# Bounds on untrusted LSP output. An LSP (buggy or hostile) could send a
# header line with no CRLF, forcing Python to buffer arbitrarily large
# input before `Content-Length` body-cap checks fire. Cap the raw
# readline size so we detect the pathology before it becomes an OOM.
_MAX_HEADER_LINE = 8192          # bytes; a single LSP header is ~60 bytes
_MAX_HEADER_BLOCK = 32 * 1024    # bytes; total of every header in one frame
_MAX_BODY_BYTES = 32 * 1024 * 1024  # 32 MiB per LSP message

# Default request timeout when caller passes timeout=None. The env var
# LSP_MCP_TIMEOUT overrides this on a per-process basis -- read at
# CALL TIME (not import time) so an MCP host can adjust the budget
# without restarting the bridge. Codex design review of the per-LSP
# concurrency surface flagged the import-time read as Medium: invalid
# values should raise a structured LspError instead of crashing the
# interpreter at module load.
_DEFAULT_REQUEST_TIMEOUT_S = 5.0
_TIMEOUT_ENV = "LSP_MCP_TIMEOUT"


def _resolve_request_timeout(t: Optional[float]) -> float:
    """Resolve the effective per-request timeout.

    Caller's explicit `timeout=` wins; `timeout=None` reads
    LSP_MCP_TIMEOUT from the environment. Either path is validated
    BEFORE the Future is registered: positive, finite, non-bool
    numeric. Invalid values raise LspError("lsp-timeout-config-
    invalid") so the bridge surfaces a clean envelope rather than
    letting a ValueError propagate or registering a Future that
    immediately times out (timeout=0) or never times out (timeout=
    inf raises OverflowError inside Future.result, leaving the
    Future stuck in _pending). Codex post-implementation review
    flagged the explicit-path as Medium."""
    import math as _math
    if t is not None:
        # bool is a subclass of int -- reject explicitly so True/False
        # don't silently coerce to 1.0/0.0 and slip past the gate.
        if isinstance(t, bool):
            raise LspError(
                "lsp-timeout-config-invalid",
                f"timeout={t!r} is bool; expected positive finite float",
                value=str(t),
                source="explicit",
            )
        if not isinstance(t, (int, float)):
            raise LspError(
                "lsp-timeout-config-invalid",
                f"timeout={t!r} is not numeric",
                value=str(t),
                source="explicit",
            )
        v = float(t)
        if not _math.isfinite(v) or v <= 0.0:
            raise LspError(
                "lsp-timeout-config-invalid",
                f"timeout={v!r} must be a positive finite number",
                value=str(t),
                source="explicit",
            )
        return v
    raw = os.environ.get(_TIMEOUT_ENV)
    # Treat both "unset" (None) and empty string as "fall back to
    # default". Empty-string env vars are a quirk of shell quoting
    # (`LSP_MCP_TIMEOUT="" ./bridge.py`) and almost always mean "I
    # forgot to set this"; promoting them to validation errors would
    # surprise users without catching real misconfiguration. Only
    # set-and-non-empty values go through the parse + bounds gate.
    if raw is None or not raw.strip():
        return _DEFAULT_REQUEST_TIMEOUT_S
    try:
        v = float(raw)
    except ValueError:
        raise LspError(
            "lsp-timeout-config-invalid",
            f"{_TIMEOUT_ENV}={raw!r} is not a number; falling back disabled",
            env_var=_TIMEOUT_ENV,
            value=raw,
            source="env",
        )
    if not _math.isfinite(v) or v <= 0.0:
        raise LspError(
            "lsp-timeout-config-invalid",
            f"{_TIMEOUT_ENV}={v!r} must be a positive finite number",
            env_var=_TIMEOUT_ENV,
            value=raw,
            source="env",
        )
    return v

# Read-only boundary: LSP methods that MUTATE files or execute server
# commands are FORBIDDEN. Enforced at request() entry so a new MCP tool
# or a future refactor cannot reach them through LspSubprocess even
# if someone writes the call. The TODO-02 autonomous-agent boundary
# classifies write-capable MCP servers as forbidden; this list is the
# runtime enforcement (grep-audit alone is insufficient since it only
# catches our code, not a future contributor's refactor). Codex
# pre-implementation review of the MCP tools surface flagged this as
# High.
#
# Note on textDocument/codeAction: per LSP 3.17 the method itself is
# read-only -- it returns a list of (Command | CodeAction) describing
# what fixes are AVAILABLE; applying them flows through
# workspace/applyEdit (server-initiated) or workspace/executeCommand
# (client-initiated). The extended-tools `code_action` MCP handler
# calls textDocument/codeAction to enumerate available actions and
# returns the metadata as-is; it never invokes applyEdit /
# executeCommand, both of which remain forbidden here.
# textDocument/rename is kept forbidden even though it ALSO returns
# a WorkspaceEdit rather than applying it -- by intent it is a
# mutation-producing call, and we have no read-only consumer that
# needs the edit list.
_FORBIDDEN_LSP_METHODS = frozenset({
    "textDocument/rename",
    "workspace/applyEdit",
    "workspace/executeCommand",
})


# Every live LspSubprocess instance registers itself here so the
# atexit hook can kill any that are still running when the bridge
# process exits. WeakSet: a GC'd instance drops off without a manual
# deregister, which is correct because __del__ closes the pipes.
_LIVE_SUBPROCS: "weakref.WeakSet[LspSubprocess]" = weakref.WeakSet()


def _atexit_kill_all() -> None:
    """Kill every LspSubprocess still alive at interpreter shutdown.

    Runs on every exit path (normal, SystemExit, unhandled exception).
    Using weak refs here means we never hold the subprocess alive past
    its natural lifetime -- the set just records who might still be
    running. A missing/GC'd entry is silently skipped."""
    for lsp in list(_LIVE_SUBPROCS):
        try:
            lsp.shutdown(timeout=0.5)
        except Exception:
            pass


atexit.register(_atexit_kill_all)


class LspError(Exception):
    """Raised when an LSP request fails in a way the bridge cares
    about: timeout, subprocess crash, unsupported method, protocol
    violation. Callers translate this into the JSON error envelope
    documented in bridge.py _call_lsp()."""

    def __init__(self, kind: str, detail: str, **extra: Any) -> None:
        super().__init__(f"{kind}: {detail}")
        self.kind = kind
        self.detail = detail
        self.extra = extra

    def to_envelope(self) -> dict[str, Any]:
        """Serialize to the JSON error envelope shape the bridge
        returns to MCP callers. Shape matches mcp_server.py's
        cache-rebuild-failed envelope so agents see a consistent
        error contract across both tools.

        `error` + `detail` are reserved: we write `extra` first, then
        overwrite `error`/`detail` so a bad caller cannot silently
        corrupt the contract by passing `error=`/`detail=` in extra."""
        out: dict[str, Any] = dict(self.extra)
        out["error"] = self.kind
        out["detail"] = self.detail
        return out


class LspSubprocess:
    """Spawn one LSP over stdio, speak minimal JSON-RPC 2.0, and
    survive concurrent tool dispatch + clean shutdown.

    Lifecycle:
        __init__(cmd, lang)            -- spawn; start reader thread.
        initialize(root_uri, caps)     -- LSP handshake, fills .server_caps.
        request(method, params, t=5)   -- synchronous RPC, returns result dict.
        notify(method, params)         -- one-way notification, no reply.
        shutdown(timeout=5)            -- LSP shutdown + exit; SIGTERM/SIGKILL fallback.

    Thread-safety: request() is callable from multiple threads. The
    per-instance `_io_lock` serializes writes to the LSP's stdin; the
    reader thread demultiplexes replies via a `{id: Future}` map, so
    two callers do not see each other's responses. Per-instance, not
    global: the bridge's _CALL_LOCK handles the cross-LSP side; this
    lock bounds single-LSP concurrency.
    """

    def __init__(self, cmd: list[str], lang: str,
                 cwd: Optional[Path] = None,
                 env: Optional[dict[str, str]] = None) -> None:
        self.cmd = list(cmd)
        self.lang = lang
        self.cwd = Path(cwd) if cwd else None
        self._env = dict(env) if env is not None else None

        self._proc: Optional[subprocess.Popen] = None
        # Buffered view of proc.stdout. bufsize=0 on Popen is required
        # so the LSP does not block waiting for its write to drain
        # through a 4 KiB Python buffer, but that makes readline() on
        # the raw stream one-syscall-per-byte. io.BufferedReader gives
        # us block-sized reads + efficient readline() on the parse side
        # while keeping the child's write semantics unchanged.
        self._stdout_buf: Optional[io.BufferedReader] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self._io_lock = threading.Lock()
        # Per-request metadata. Was `dict[int, Future]`; expanded
        # to carry corr_id + method + send_ts so the reader thread
        # (which does NOT inherit the requester's contextvars
        # Context) can attach the right correlation ID to lsp-recv
        # / timeout / crash events. Codex design review of the
        # structured-logging plan caught this: send-side-only
        # corr_id leaves DEBUG mode unable to prove which response
        # matches which request under concurrent load.
        # Schema: {"future": Future, "corr_id": str|None,
        #          "method": str, "send_ts": float, "lang": str|None}
        self._pending: dict[int, dict[str, Any]] = {}
        self._pending_lock = threading.Lock()
        self._next_id = 1
        self._next_id_lock = threading.Lock()
        self._initialized = False
        self._init_lock = threading.Lock()
        self._shutdown_called = False
        # Transport-dead signal: _reader_loop sets this in its finally
        # block so request()/notify() can reject immediately on a
        # known-bad channel instead of enqueuing a Future that will
        # only resolve on timeout. Without this, a protocol-error
        # that killed the reader still left the demux path looking
        # live to new callers.
        self._reader_dead = False
        # Teardown bypass: shutdown() sets this to its own thread id
        # so its internal shutdown-RPC, didClose, and exit traffic can
        # slip past the _shutdown_called guard in request()/notify().
        # Stored as a thread id (NOT a process-wide bool) so a
        # concurrent MCP-tool thread that already passed the
        # apply_text gate cannot also bypass during the teardown
        # window. Codex post-implementation review of the
        # file-change-lifecycle work flagged a global bool as High:
        # ordinary tool traffic could interleave after
        # _shutdown_called was committed, recreating the ordering
        # race the file-change-lifecycle fix claimed to close.
        self._teardown_thread_id: Optional[int] = None
        self.server_caps: dict[str, Any] = {}
        # SCAFFOLD (consumed by the per-language integration commits
        # and the diagnostics tool handler): diagnostics get pushed by
        # the server asynchronously via textDocument/publishDiagnostics;
        # the MCP `diagnostics` tool reads the most recent set per URI
        # from here. Exposed (not _private) so the tool handler reads
        # it directly.
        self.diagnostics_by_uri: dict[str, Any] = {}
        # SCAFFOLD (consumed by the file-change-lifecycle commit):
        # tracked URIs for didChange forwarding. Populated by
        # did_open() and ensure_open(); read by a future mtime-check
        # path that decides whether to send a didChange before
        # forwarding the tool request. Exposed so that path can
        # attach without touching __init__.
        self.open_uris: set[str] = set()
        # Lock covering open_uris mutation + the paired didOpen
        # notification. Without this, two concurrent MCP tool calls
        # on the same URI would both read the set, both see it
        # missing, both send textDocument/didOpen, and LSPs may treat
        # the duplicate as a protocol error. ensure_open() holds this
        # lock across the check/send/add sequence so at most one
        # didOpen per URI reaches the LSP. Codex pre-implementation
        # review of the MCP tools surface flagged this as High.
        self._open_uris_lock = threading.Lock()
        # Per-URI metadata for the file-change-lifecycle + respawn
        # paths. Schema:
        #   {uri: {"version": int, "mtime_ns": int,
        #          "resolved_path": str | None,
        #          "lang": str | None,
        #          "language_id": str}}
        # Populated by apply_text() (the open-or-refresh driver used
        # by every MCP tool handler). Read under _open_uris_lock
        # alongside open_uris so the open/refresh decision is atomic.
        # resolved_path / lang are stored so the bridge respawn path
        # can re-walk every tracked URI through _dispatch_path's
        # workspace-bounded sandbox after a crash, instead of
        # reconstructing paths ad hoc from URI strings (Codex design
        # review: replay needs a first-class snapshot schema).
        self.open_uri_meta: dict[str, dict[str, Any]] = {}
        # Filesystem paths owned by this LspSubprocess that the
        # bridge atexit hook should `shutil.rmtree` after the LSP
        # has shut down cleanly. Spawners append to this list (e.g.
        # PowerShellEditorServices' per-spawn LogPath/SessionDetails
        # tempdir). Empty by default; opt-in per spawner.
        self.cleanup_paths: list[str] = []
        # Crash detection: set True by _reader_loop's finally block
        # when the reader exits WITHOUT _shutdown_called (i.e. the
        # subprocess died unexpectedly). Distinct from a clean
        # shutdown so the bridge respawn path can decide whether to
        # restart this LSP. The crash REASON (the LspError kind that
        # the reader saw last) is captured separately so the bridge
        # can surface it via the _health tool. Health-state
        # accumulation (restart_count, FAILED detection) lives on
        # the BRIDGE side in _LSP_HEALTH (per-(lang, root)) so it
        # survives instance replacement -- the per-LspSubprocess
        # crashed/crash_reason fields are one-shot snapshots, not
        # rolling counters (Codex design review caught the
        # per-instance-counter trap).
        self._crashed: bool = False
        self._crash_reason: Optional[str] = None

        # Background-progress observation for warm-start readiness
        # detection. The bridge's --warm-start path needs to know when
        # an LSP that does background indexing (clangd, pyright) has
        # finished its initial pass, otherwise warm-start would report
        # "ready" the moment initialize returns and the first
        # workspace_symbol call would still pay the cold-index cost
        # (Codex design review High). We observe `$/progress`
        # notifications and accept server-initiated
        # `window/workDoneProgress/create` requests so progress-emitting
        # servers (pyright; some clangd configs) don't stall waiting
        # for an ack.
        #
        # Schema:
        #   _progress_by_token[<token-str>] = {
        #       "kind": "begin" | "report" | "end",
        #       "title": Optional[str],
        #       "message": Optional[str],
        #       "percentage": Optional[int],
        #       "last_update_ts": float monotonic,
        #   }
        # _progress_seen_at: monotonic timestamp of the FIRST progress
        #   notification ever observed for this LSP, or None. Lets the
        #   warm-start poller distinguish "indexing not finished yet"
        #   from "this LSP doesn't index" (no progress traffic at all
        #   within a small post-handshake window).
        # _handshake_done_at: monotonic timestamp set in initialize()
        #   after `notify("initialized")`. Anchors the no-progress
        #   timeout so we don't measure from process spawn.
        # All four guarded by _progress_lock; reader writes, bridge-
        # side warm poller reads via snapshot_progress() (returns deep
        # copy under the lock so the poller cannot observe partial
        # updates or trip dictionary-changed-size-during-iteration).
        self._progress_by_token: dict[str, dict[str, Any]] = {}
        self._progress_seen_at: Optional[float] = None
        self._handshake_done_at: Optional[float] = None
        self._progress_lock = threading.Lock()

        self._spawn()
        _LIVE_SUBPROCS.add(self)

    # ------------------------------------------------------------------
    # Subprocess lifecycle
    # ------------------------------------------------------------------

    def _spawn(self) -> None:
        """Launch the LSP under stdin/stdout pipes + start reader +
        stderr-drain threads.

        If Popen succeeds but post-spawn setup (thread creation /
        start) raises, the child would otherwise leak AND escape the
        _LIVE_SUBPROCS atexit hook (we only register after _spawn
        returns). Wrap the post-Popen setup in a reap-on-failure
        guard so any early failure leaves the tree clean."""
        try:
            # bufsize=0: raw byte pipes. LSP framing does its own
            # Content-Length accounting; Python-level buffering would
            # deadlock a server that blocks on partial reads.
            # start_new_session=True: detaches from terminal so a
            # Ctrl-C in the bridge does not propagate to the LSP and
            # trip its crash-handler before our clean shutdown runs.
            self._proc = subprocess.Popen(
                self.cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                bufsize=0,
                cwd=str(self.cwd) if self.cwd else None,
                env=self._env,
                start_new_session=True,
            )
            # Buffered reader wrapper only for the read side. The child
            # still writes into an unbuffered pipe (bufsize=0), but our
            # parser benefits from block-sized reads + efficient
            # readline(limit=...).
            if self._proc.stdout is not None:
                self._stdout_buf = io.BufferedReader(
                    self._proc.stdout, buffer_size=65536
                )
        except FileNotFoundError as exc:
            raise LspError(
                "lsp-binary-missing",
                f"{self.cmd[0]!r} not found on PATH",
                lang=self.lang,
                install_hint="ensure the LSP binary is on PATH; "
                             "per-language server modules set a more "
                             "specific hint via LspError directly.",
            ) from exc
        except OSError as exc:
            raise LspError(
                "lsp-spawn-failed",
                f"{exc.__class__.__name__}: {exc}",
                lang=self.lang,
            ) from exc

        try:
            # Reader thread: drains stdout, parses frames, resolves
            # the matching pending Future. Daemon so it never blocks
            # interpreter exit (we also do a join() in shutdown()).
            self._reader_thread = threading.Thread(
                target=self._reader_loop,
                name=f"lsp-reader-{self.lang}",
                daemon=True,
            )
            self._reader_thread.start()

            # Stderr drain thread: without this, a verbose LSP
            # (clangd/pyright commonly emit megabytes of logs on
            # startup) fills the pipe buffer and blocks on its next
            # stderr write -- requests then time out against a
            # running-but-stuck subprocess. We discard the output;
            # the structured-logging follow-up will replace this
            # with a capped ring buffer consumer.
            self._stderr_thread = threading.Thread(
                target=self._stderr_drain_loop,
                name=f"lsp-stderr-{self.lang}",
                daemon=True,
            )
            self._stderr_thread.start()
        except Exception:
            # Thread creation / start failed. Reap the child so it
            # does not outlive the LspSubprocess ctor failure.
            proc = self._proc
            self._proc = None
            try:
                if proc is not None:
                    try:
                        proc.terminate()
                    except Exception:
                        pass
                    try:
                        proc.wait(timeout=1.0)
                    except subprocess.TimeoutExpired:
                        try:
                            proc.kill()
                        except Exception:
                            pass
                        try:
                            proc.wait(timeout=1.0)
                        except subprocess.TimeoutExpired:
                            pass
                    for f in (proc.stdin, proc.stdout, proc.stderr):
                        try:
                            if f is not None:
                                f.close()
                        except Exception:
                            pass
            except Exception:
                pass
            raise

    def _stderr_drain_loop(self) -> None:
        """Discard stderr output to prevent the pipe from filling
        and blocking the LSP. Runs until EOF or pipe close."""
        proc = self._proc
        if proc is None or proc.stderr is None:
            return
        try:
            while True:
                chunk = proc.stderr.read(8192)
                if not chunk:
                    break
        except Exception:
            # Pipe closed during shutdown -- expected, not an error.
            pass

    _READ_HEADERS_MALFORMED = object()  # sentinel; !=None so EOF and bad framing are distinct

    def _reader_loop(self) -> None:
        """Parse LSP framed messages from stdout and route replies.

        Two message classes arrive here:
          * Responses (have an `id` matching a pending request)
          * Notifications (no `id`; method like publishDiagnostics).

        Malformed framing (bad headers, over-length headers, bad
        Content-Length, oversized body, bad JSON) fails every pending
        request with lsp-protocol-error so callers do not hang on a
        Future. EOF is distinct and surfaces as lsp-subprocess-exited.
        _reader_dead flips True in the finally so subsequent
        request()/notify() calls reject immediately instead of
        enqueuing a Future on a dead channel."""
        stream = self._stdout_buf
        if stream is None:
            # _spawn() failed post-Popen and reset _proc; nothing to
            # read.
            self._reader_dead = True
            return
        try:
            while True:
                headers = self._read_headers(stream)
                if headers is None:
                    # Genuine EOF: subprocess exited.
                    break
                if headers is self._READ_HEADERS_MALFORMED:
                    self._fail_all_pending(LspError(
                        "lsp-protocol-error",
                        "malformed LSP header block",
                        lang=self.lang,
                    ))
                    break
                length_str = headers.get("content-length")
                if length_str is None:
                    self._fail_all_pending(LspError(
                        "lsp-protocol-error",
                        "missing Content-Length header",
                        lang=self.lang,
                    ))
                    break
                try:
                    length = int(length_str)
                except ValueError:
                    self._fail_all_pending(LspError(
                        "lsp-protocol-error",
                        f"non-numeric Content-Length: {length_str!r}",
                        lang=self.lang,
                    ))
                    break
                if length < 0 or length > _MAX_BODY_BYTES:
                    # A single LSP message over 32 MiB means either a
                    # protocol desync or a server trying to dump its
                    # entire workspace-symbol table at us. Don't
                    # allocate the buffer; treat as protocol error.
                    self._fail_all_pending(LspError(
                        "lsp-protocol-error",
                        f"Content-Length {length} outside [0, 32MiB]",
                        lang=self.lang,
                    ))
                    break
                body = self._read_exact(stream, length)
                if body is None:
                    break
                try:
                    # json.loads accepts bytes directly on Python 3.6+;
                    # skipping the explicit UTF-8 decode removes one
                    # full-body copy on the reader hot path.
                    msg = json.loads(body)
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    self._fail_all_pending(LspError(
                        "lsp-protocol-error",
                        f"bad JSON body: {exc}",
                        lang=self.lang,
                    ))
                    break
                self._dispatch_message(msg)
        except Exception as exc:
            self._fail_all_pending(LspError(
                "lsp-reader-crashed",
                f"{exc.__class__.__name__}: {exc}",
                lang=self.lang,
            ))
        finally:
            # Set reader_dead BEFORE the final fail_all so any thread
            # racing in to request() during teardown sees the dead
            # flag on next check instead of enqueuing a Future.
            self._reader_dead = True
            # Crash detection: reader thread ended WITHOUT a
            # corresponding shutdown() call -> subprocess died
            # unexpectedly. Snap the flag + reason here so the
            # bridge respawn path can detect it via the public
            # `crashed` property without racing the GC. The bridge
            # owns rolling state (restart_count, FAILED detection);
            # this object only records "I died, here's why".
            if not self._shutdown_called:
                self._crashed = True
                # Best-effort reason: prefer a poll() exit code if
                # available (subprocess died on its own) over a
                # generic "reader exited" string.
                try:
                    rc = self._proc.poll() if self._proc else None
                except Exception:
                    rc = None
                if rc is not None:
                    self._crash_reason = (
                        f"subprocess exited with code {rc}"
                    )
                else:
                    self._crash_reason = "reader thread ended"
            self._fail_all_pending(LspError(
                "lsp-subprocess-exited",
                f"{self.lang} LSP reader thread ended",
                lang=self.lang,
            ))

    @classmethod
    def _read_headers(cls, stream) -> Optional[dict[str, str]]:
        """Read LSP header block (one header per line, CRLF-terminated,
        empty line ends).

        Returns:
          * None on EOF before any header.
          * _READ_HEADERS_MALFORMED on bounds violation or bad shape.
          * dict of headers on success.

        Bounds: each readline is capped at _MAX_HEADER_LINE bytes so a
        line without CRLF cannot force unbounded buffering before the
        body-size cap kicks in. Cumulative header bytes are capped at
        _MAX_HEADER_BLOCK so a pathological server cannot stream
        infinite short headers either."""
        headers: dict[str, str] = {}
        total = 0
        while True:
            # readline(size+1) returns at most size+1 bytes; we treat
            # anything >= size+1 OR not ending in LF as over-length.
            line = stream.readline(_MAX_HEADER_LINE + 1)
            if not line:
                return None  # EOF
            if len(line) > _MAX_HEADER_LINE or not line.endswith(b"\n"):
                return cls._READ_HEADERS_MALFORMED
            total += len(line)
            if total > _MAX_HEADER_BLOCK:
                return cls._READ_HEADERS_MALFORMED
            try:
                s = line.decode("ascii").rstrip("\r\n")
            except UnicodeDecodeError:
                return cls._READ_HEADERS_MALFORMED
            if s == "":
                return headers
            if ":" not in s:
                return cls._READ_HEADERS_MALFORMED
            name, _, value = s.partition(":")
            headers[name.strip().lower()] = value.strip()

    @staticmethod
    def _read_exact(stream, n: int) -> Optional[bytes]:
        """Read exactly n bytes; returns None on EOF."""
        buf = bytearray()
        while len(buf) < n:
            chunk = stream.read(n - len(buf))
            if not chunk:
                return None
            buf.extend(chunk)
        return bytes(buf)

    def _dispatch_message(self, msg: dict[str, Any]) -> None:
        """Route one decoded LSP message to the right consumer."""
        if "id" in msg and ("result" in msg or "error" in msg):
            # Response to a prior request.
            self._resolve_pending(msg)
            return
        method = msg.get("method")
        params = msg.get("params") or {}
        # Server-initiated REQUEST (id present, method present, no
        # result/error). The only one the bridge currently honors is
        # window/workDoneProgress/create; pyright (and some clangd
        # configs) require an ack BEFORE they emit $/progress
        # notifications, and the warm-start readiness signal depends
        # on that progress traffic. Anything else gets method-not-
        # found so a forward-rev server cannot stall waiting for us.
        if "id" in msg and method:
            self._handle_server_request(msg.get("id"), method, params)
            return
        # Notification.
        if method == "textDocument/publishDiagnostics":
            uri = params.get("uri")
            if isinstance(uri, str):
                self.diagnostics_by_uri[uri] = params.get("diagnostics") or []
            return
        if method == "$/progress":
            self._record_progress(params)
            return

    def _handle_server_request(self, rid: Any, method: str,
                               params: dict[str, Any]) -> None:
        """Respond to a server-initiated JSON-RPC request. Currently
        accepts window/workDoneProgress/create (returns null result
        per LSP spec) and rejects everything else with method-not-
        found. Best-effort: a write failure here is non-fatal -- the
        reader will surface the broken pipe on the next response."""
        try:
            if method == "window/workDoneProgress/create":
                self._send_response(rid, result=None)
            else:
                # JSON-RPC 2.0 method-not-found = -32601.
                self._send_response(rid, error={
                    "code": -32601,
                    "message": f"server-initiated {method!r} not handled",
                })
        except Exception:
            pass

    def _send_response(self, rid: Any, result: Any = None,
                       error: Optional[dict[str, Any]] = None) -> None:
        """Send a JSON-RPC response to a server-initiated request.
        Distinct from notify() (no method) and from request() (no
        pending Future); bypasses the shutdown gate because these
        replies are only ever fired from the reader thread, which
        exits before the shutdown gate flips.

        Try-acquire with a short timeout instead of an unbounded
        wait: Codex adversarial review of the warm-start change
        flagged the unbounded acquire as a potential deadlock vector
        when an LSP that waits on the ACK before draining its stdin
        backpressures a concurrent client write that already holds
        _io_lock. If we cannot acquire within the timeout, we drop
        the ACK -- the server may stall progress for that one token,
        but the bridge stays responsive, and the warm-start
        no-progress timeout absorbs the missing event without
        blocking serving."""
        payload: dict[str, Any] = {"jsonrpc": "2.0", "id": rid}
        if error is not None:
            payload["error"] = error
        else:
            payload["result"] = result
        header, body = self._encode_frame(payload)
        if not self._io_lock.acquire(timeout=0.5):
            return
        try:
            self._send_frame_bytes(header, body)
        finally:
            self._io_lock.release()

    def _record_progress(self, params: dict[str, Any]) -> None:
        """Update _progress_by_token from a $/progress notification.
        Schema per LSP 3.16: params = {token, value: {kind, title?,
        message?, percentage?, cancellable?}}.

        Best-effort: malformed payloads are silently dropped (never
        kill the reader thread on a server-protocol bug). Holds the
        progress lock for the minimum critical section: dict update
        + last_update_ts + first-seen anchor."""
        import time as _time
        token = params.get("token")
        value = params.get("value")
        if token is None or not isinstance(value, dict):
            return
        token_str = str(token)
        kind = value.get("kind")
        if kind not in ("begin", "report", "end"):
            return
        # Sanitize percentage at ingest. Python's JSON decoder accepts
        # NaN/Infinity, and int(float('nan')) raises ValueError while
        # int(float('inf')) raises OverflowError -- a buggy or
        # hostile LSP could otherwise crash the warm-start summary
        # logger when it casts the value. Drop anything that is not
        # a finite int/float; clamp the rest to 0..100 to match the
        # LSP 3.16 contract. (Codex post-ship adversarial review M.)
        import math as _math
        raw_pct = value.get("percentage")
        pct: Optional[int] = None
        if isinstance(raw_pct, bool):
            # bool is a subclass of int -- exclude it explicitly so
            # `True` / `False` do not silently become 1 / 0.
            pct = None
        elif isinstance(raw_pct, int):
            pct = max(0, min(100, raw_pct))
        elif isinstance(raw_pct, float) and _math.isfinite(raw_pct):
            pct = max(0, min(100, int(raw_pct)))
        now = _time.monotonic()
        entry: dict[str, Any] = {
            "kind": kind,
            "title": value.get("title"),
            "message": value.get("message"),
            "percentage": pct,
            "last_update_ts": now,
        }
        with self._progress_lock:
            if self._progress_seen_at is None:
                self._progress_seen_at = now
            existing = self._progress_by_token.get(token_str)
            if existing is not None:
                # Carry forward title from the begin frame so a later
                # report/end with title=None still surfaces a
                # human-readable label to the warm-start log.
                if entry["title"] is None:
                    entry["title"] = existing.get("title")
                # Likewise for message: an end frame often omits it.
                if entry["message"] is None and kind == "end":
                    entry["message"] = existing.get("message")
            self._progress_by_token[token_str] = entry

    def snapshot_progress(self) -> dict[str, Any]:
        """Return a snapshot of progress state for the warm-start
        poller. Copy-under-lock so the poller cannot trip
        RuntimeError(dictionary changed size during iteration) and
        never observes a half-updated entry. The warm-start poller
        polls every 2s while the reader thread fires progress
        notifications without holding the poller's lock, so the
        snapshot contract is required."""
        with self._progress_lock:
            return {
                "tokens": {k: dict(v) for k, v
                            in self._progress_by_token.items()},
                "first_seen_at": self._progress_seen_at,
                "handshake_done_at": self._handshake_done_at,
            }

    def _resolve_pending(self, msg: dict[str, Any]) -> None:
        rid = msg.get("id")
        if not isinstance(rid, int):
            return
        with self._pending_lock:
            meta = self._pending.pop(rid, None)
        if meta is None:
            return
        fut = meta["future"]
        # DEBUG receive event tagged with the originating call's
        # Complete the waiting Future FIRST -- the debug log
        # serializes the full response body (potentially up to
        # _MAX_BODY_BYTES = 32 MiB) which can block the single
        # reader thread, and a slow LSP_MCP_LOG_FILE sink
        # extends the wait. Codex post-implementation review
        # caught the original "log first, set Future second"
        # ordering as a Medium concern: in DEBUG + large response
        # + slow sink it could push the caller's Future past its
        # request timeout even though the data was already in
        # the reader thread.
        if "error" in msg:
            err = msg["error"] or {}
            fut.set_exception(LspError(
                "lsp-request-failed",
                f"code={err.get('code')} message={err.get('message')!r}",
                lang=self.lang,
                lsp_error=err,
            ))
        else:
            fut.set_result(msg.get("result"))
        # DEBUG receive event tagged with the originating call's
        # corr_id (looked up from per-request metadata, NOT from
        # the reader thread's Context which is its own). Emitted
        # AFTER Future completion so a slow log sink cannot
        # extend the request's wall-clock latency.
        try:
            from logger import debug_lsp_recv as _dbg_recv  # noqa: WPS433
            _dbg_recv(method=meta.get("method"),
                      lang=meta.get("lang"),
                      request_id=rid,
                      body=msg,
                      corr_id=meta.get("corr_id"))
        except Exception:
            pass

    def _fail_all_pending(self, err: LspError) -> None:
        with self._pending_lock:
            pending = self._pending
            self._pending = {}
        for meta in pending.values():
            fut = meta["future"]
            if not fut.done():
                fut.set_exception(err)

    # ------------------------------------------------------------------
    # Wire I/O
    # ------------------------------------------------------------------

    def _send_frame_bytes(self, header: bytes, body: bytes) -> None:
        """Write a pre-encoded LSP frame. MUST be called with
        _io_lock held -- a partial interleave from two threads would
        corrupt the Content-Length framing. Encoding + json.dumps
        happens in request() outside the lock to keep the critical
        section to only the syscalls that must be ordered."""
        proc = self._proc
        if proc is None or proc.stdin is None or proc.stdin.closed:
            raise LspError(
                "lsp-subprocess-exited",
                "stdin unavailable",
                lang=self.lang,
            )
        try:
            proc.stdin.write(header)
            proc.stdin.write(body)
            proc.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as exc:
            # ValueError is raised when writing to a closed file
            # object (shutdown closed stdin mid-request).
            raise LspError(
                "lsp-subprocess-exited",
                f"write failed: {exc.__class__.__name__}",
                lang=self.lang,
            ) from exc

    @staticmethod
    def _encode_frame(payload: dict[str, Any]) -> tuple[bytes, bytes]:
        """Serialize a JSON-RPC payload into (header, body) bytes. Pure
        function; no I/O, no lock. Split out so request() can encode
        OUTSIDE the _io_lock critical section."""
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        return header, body

    def _next_request_id(self) -> int:
        # Self-locking so callers can allocate an id + register the
        # pending Future OUTSIDE _io_lock. That keeps json.dumps and
        # stdin.flush off the critical path and lets concurrent
        # callers overlap id allocation with I/O.
        with self._next_id_lock:
            rid = self._next_id
            self._next_id += 1
        return rid

    def request(self, method: str, params: Optional[dict[str, Any]] = None,
                timeout: Optional[float] = None) -> Any:
        """Send a JSON-RPC request + wait for its reply.

        Raises LspError on timeout, subprocess crash, or LSP-reported
        error. Callers in bridge.py translate this into the JSON
        error envelope returned to the MCP agent.

        Timeout resolution: `timeout=<float>` is used as-is. `timeout=
        None` resolves to the LSP_MCP_TIMEOUT env var (read at CALL
        time so an MCP host can adjust mid-process), or the
        _DEFAULT_REQUEST_TIMEOUT_S fallback. Invalid env values raise
        LspError("lsp-timeout-config-invalid"). All existing in-tree
        callers pass an explicit positive timeout; the env path is
        for ad-hoc / test / tool-handler-default callers that opt in
        via timeout=None.

        During shutdown() teardown, the internal `shutdown` RPC must
        bypass the _shutdown_called guard: _teardown_thread_id is
        set to the shutdown owner's thread id for the duration of
        the teardown sequence; only that thread can call request()
        past the guard. A concurrent MCP-tool thread that already
        passed apply_text's gate cannot also bypass -- its thread
        id will not match.

        Read-only boundary enforcement: method names in
        _FORBIDDEN_LSP_METHODS are REJECTED at entry. This is the
        runtime gate for the TODO-02 autonomous-agent-boundary
        classification that write-capable MCP servers are forbidden.
        A new MCP tool handler or future refactor cannot bypass the
        gate without explicitly removing an entry from the set."""
        timeout = _resolve_request_timeout(timeout)
        if method in _FORBIDDEN_LSP_METHODS:
            raise LspError(
                "lsp-method-forbidden",
                f"{method!r} is a write-capable LSP method; rejected by "
                "the read-only boundary. See _FORBIDDEN_LSP_METHODS in "
                "scripts/lsp-mcp/lsp_client.py and TODO-02 autonomous-"
                "agent-boundary.",
                lang=self.lang,
                method=method,
            )
        if self._shutdown_called and \
                self._teardown_thread_id != threading.get_ident():
            raise LspError(
                "lsp-shutdown",
                "request after shutdown",
                lang=self.lang,
                method=method,
            )
        if self._reader_dead:
            # Reader thread already exited (subprocess crash, protocol
            # error, etc.). Reject immediately instead of enqueuing a
            # Future that will only resolve on timeout.
            raise LspError(
                "lsp-subprocess-exited",
                "reader thread is dead",
                lang=self.lang,
                method=method,
            )
        fut: "Future[Any]" = Future()
        # Allocate id + register pending OUTSIDE _io_lock so concurrent
        # callers do not serialize on json.dumps / stdin.flush. Payload
        # encoding is also pre-lock; the critical section covers only
        # the two ordered pipe writes + flush.
        rid = self._next_request_id()
        # Capture the active correlation ID from the requester's
        # Context so the reader thread (which has its own Context)
        # can attach the right corr_id to lsp-recv / timeout / crash
        # events. Best-effort: logger import failure must not break
        # the request path.
        corr_id: Optional[str] = None
        try:
            from logger import current_corr_id as _ccid  # noqa: WPS433
            corr_id = _ccid()
        except Exception:
            pass
        with self._pending_lock:
            self._pending[rid] = {
                "future": fut,
                "corr_id": corr_id,
                "method": method,
                "lang": self.lang,
                "send_ts": time.monotonic(),
            }
        payload: dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": rid,
            "method": method,
        }
        if params is not None:
            payload["params"] = params
        header, body = self._encode_frame(payload)
        # DEBUG send event BEFORE the wire write -- the corr_id is
        # already captured into _pending so receive-side correlation
        # works even if this thread races a ctx switch.
        try:
            from logger import debug_lsp_send as _dbg_send  # noqa: WPS433
            _dbg_send(method=method, lang=self.lang,
                      request_id=rid, body=payload)
        except Exception:
            pass
        try:
            with self._io_lock:
                self._send_frame_bytes(header, body)
        except LspError:
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise
        try:
            return fut.result(timeout=timeout)
        except FutureTimeoutError as exc:
            # Future.result raises concurrent.futures.TimeoutError on
            # Python 3.10; on 3.11+ it's aliased to the builtin
            # TimeoutError. Catch the concrete name so the fallback
            # works on every target interpreter. The pending entry
            # is evicted so a late reply does not set a dead Future
            # (harmless but noisy in logs).
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise LspError(
                "lsp-timeout",
                f"{method} timed out after {timeout:.1f}s",
                lang=self.lang,
                method=method,
            ) from exc

    def notify(self, method: str, params: Optional[dict[str, Any]] = None) -> None:
        """Send a one-way JSON-RPC notification. No reply expected.

        Same teardown bypass as request(): shutdown's own exit
        notification rides through because the shutdown thread holds
        _teardown_thread_id == threading.get_ident(). NO method-name
        carveout: a non-owner thread that races shutdown cannot
        smuggle `exit` (or any other notification) onto the wire
        ahead of the shutdown thread's didClose / shutdown RPC.
        Codex re-adversarial flagged a stale `method != \"exit\"`
        carveout from the original boolean teardown flag as a
        Medium risk for ordering breakage."""
        if (self._shutdown_called
                and self._teardown_thread_id != threading.get_ident()):
            raise LspError(
                "lsp-shutdown",
                "notification after shutdown",
                lang=self.lang,
                method=method,
            )
        if self._reader_dead and method != "exit":
            raise LspError(
                "lsp-subprocess-exited",
                "reader thread is dead",
                lang=self.lang,
                method=method,
            )
        payload: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        header, body = self._encode_frame(payload)
        with self._io_lock:
            self._send_frame_bytes(header, body)

    # ------------------------------------------------------------------
    # Handshake
    # ------------------------------------------------------------------

    def initialize(self, root_uri: str,
                   capabilities: Optional[dict[str, Any]] = None,
                   initialization_options: Optional[dict[str, Any]] = None,
                   timeout: float = 10.0) -> None:
        """LSP handshake. Sends `initialize`, waits for capabilities,
        sends `initialized`. Idempotent: a second call is a no-op.

        _init_lock serializes concurrent callers: without it, two
        threads could both observe _initialized=False and both send
        initialize/initialized, corrupting server_caps and tripping
        the LSP's own "already initialized" rejection on the second
        handshake."""
        with self._init_lock:
            if self._initialized:
                return
            params: dict[str, Any] = {
                "processId": os.getpid(),
                "rootUri": root_uri,
                "capabilities": capabilities or {},
            }
            if initialization_options is not None:
                params["initializationOptions"] = initialization_options
            result = self.request("initialize", params, timeout=timeout)
            # Normalize server_caps to dict. LSP spec says result is a
            # dict with `capabilities`, but a version-skewed server or
            # wrapper script could return null / non-dict; downstream
            # callers .get() on server_caps so a non-dict turns into a
            # confusing AttributeError far from the protocol violation.
            caps: Any = None
            if isinstance(result, dict):
                caps = result.get("capabilities")
            if caps is None:
                caps = {}
            elif not isinstance(caps, dict):
                raise LspError(
                    "lsp-protocol-error",
                    f"initialize.capabilities is {type(caps).__name__}, "
                    "expected dict",
                    lang=self.lang,
                )
            self.server_caps = caps
            self.notify("initialized", {})
            self._initialized = True
            # Anchor for warm-start "no progress within N seconds
            # post-handshake => nothing to index" heuristic. Set under
            # _progress_lock so the warm poller's snapshot is
            # consistent with the lazy-init.
            import time as _time
            with self._progress_lock:
                self._handshake_done_at = _time.monotonic()

    def did_open(self, uri: str, language_id: str, text: str,
                 version: int = 1) -> None:
        """Notify the LSP that a document is open. Required before
        most textDocument/* requests; LSPs typically reject requests
        against URIs they have not seen a didOpen for.

        Unconditional: always sends a textDocument/didOpen even if
        this URI was previously opened. Callers that want the
        de-duplicated path should use ensure_open() instead; it gates
        on open_uris under the _open_uris_lock. The self-tests use
        did_open() directly because each self-test spawns a fresh
        LSP and opens exactly one file."""
        self.notify("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": language_id,
                "version": version,
                "text": text,
            },
        })
        with self._open_uris_lock:
            self.open_uris.add(uri)

    def ensure_open(self, uri: str, language_id: str, text: str,
                    version: int = 1) -> bool:
        """Idempotent didOpen. Sends textDocument/didOpen ONLY if the
        URI has not been opened on this LspSubprocess before.

        Returns True if a didOpen was sent this call, False if the
        URI was already open. Thread-safe: the check, notification,
        and open_uris update happen under _open_uris_lock so two
        concurrent MCP tool calls on the same URI produce at most
        one didOpen wire message.

        Used by the MCP tool handlers in bridge.py where a single
        LSP may see many tool calls against the same file and each
        re-open would be both wasteful (re-index) and protocol-
        dubious (some LSPs reject duplicate didOpen).

        Teardown gate matches apply_text(): rejects with
        lsp-shutdown once shutdown() commits, so legacy callers
        cannot interleave a didOpen between the shutdown gate flip
        and the didClose snapshot."""
        with self._open_uris_lock:
            if self._shutdown_called:
                raise LspError(
                    "lsp-shutdown",
                    "ensure_open after shutdown",
                    lang=self.lang,
                    uri=uri,
                )
            if uri in self.open_uris:
                return False
            # Send inside the lock so a concurrent caller waiting on
            # the lock sees open_uris already populated. The notify
            # path holds _io_lock separately; nested lock order is
            # _open_uris_lock -> _io_lock, and no code path takes
            # them in the reverse order.
            self.notify("textDocument/didOpen", {
                "textDocument": {
                    "uri": uri,
                    "languageId": language_id,
                    "version": version,
                    "text": text,
                },
            })
            self.open_uris.add(uri)
            return True

    def did_change(self, uri: str, language_id: str, text: str,
                   version: int) -> None:
        """Send a textDocument/didChange notification with a full-file
        replace (no `range` -- the change covers the entire document).

        Full-file replacement is the simplest contract that every LSP
        supports; range-based incremental sync would require we keep
        the canonical document text on the bridge side AND compute a
        Range for every diff, neither of which buys us latency wins
        for a tool surface where each MCP call already reads the file
        fresh. version MUST be monotonically increasing per URI per
        LSP 3.17 -- the apply_text() driver guards that contract."""
        self.notify("textDocument/didChange", {
            "textDocument": {
                "uri": uri,
                "version": version,
            },
            "contentChanges": [{"text": text}],
        })

    def did_save(self, uri: str, text: Optional[str] = None) -> None:
        """Send a textDocument/didSave notification. Some LSPs
        (pyright, clangd) only re-lint or re-index on save; the
        bridge sends a synthetic didSave when external mtime
        indicates the file was rewritten by another process so the
        LSP's diagnostics stay current.

        text is optional per LSP 3.17 -- only sent when the server
        advertised `textDocumentSync.save.includeText: true` in its
        initialize result. We accept it as a parameter but the
        apply_text() driver is responsible for deciding whether to
        pass it (avoids wasted bandwidth for the common case)."""
        params: dict[str, Any] = {"textDocument": {"uri": uri}}
        if text is not None:
            params["text"] = text
        self.notify("textDocument/didSave", params)

    def did_close(self, uri: str) -> None:
        """Send a textDocument/didClose notification. Drops the URI
        from open_uris so a subsequent ensure_open() / apply_text()
        re-opens cleanly. Does NOT clear open_uri_meta: keeping the
        last-known version means a future re-open + change sequence
        starts past the LSP's prior version count, which some servers
        (PSES) require for stale-document rejection.

        Lock order: _open_uris_lock acquired BEFORE notify() (which
        takes _io_lock) -- the same order apply_text() and
        ensure_open() use. The earlier draft acquired the locks in
        the reverse order (notify first, then _open_uris_lock) and
        Codex flagged it as Medium: a real lock-order inversion
        deadlock with apply_text on the same URI. Mutate state and
        send the notification under the single critical section so
        no caller can observe an open URI that has already been
        closed on the wire (or vice versa)."""
        with self._open_uris_lock:
            self.notify("textDocument/didClose", {
                "textDocument": {"uri": uri},
            })
            self.open_uris.discard(uri)

    def apply_text(self, uri: str, language_id: str, text: str,
                   mtime_ns: Optional[int],
                   force_did_save: bool = False,
                   resolved_path: Optional[str] = None,
                   lang: Optional[str] = None) -> int:
        """Open-or-refresh driver: ensure the LSP's in-memory view of
        `uri` matches the on-disk text identified by `mtime_ns`.

        Returns the document version after this call (1 for first
        open, N+1 after a change). Thread-safe: the open-vs-refresh
        decision + the wire notification + the meta update happen
        under _open_uris_lock as a single atomic step, so two
        concurrent MCP tool calls on the same URI cannot race the
        version counter or duplicate-send didOpen.

        Behavior matrix:
          * URI never opened  -> didOpen, version=1, record mtime.
          * URI open, mtime matches  -> no-op, return cached version.
          * URI open, mtime changed  -> didChange, version+=1, record
                                        new mtime; if force_did_save
                                        (or the LSP only re-lints on
                                        save -- caller policy), also
                                        send didSave.
          * mtime_ns is None         -> treat as "skip refresh check";
                                        used by self-tests that don't
                                        care about external edits.

        Lock order: _open_uris_lock acquired first, then notify()
        takes _io_lock internally. No callsite reverses that order.

        Teardown gate: rejects external callers once shutdown() has
        committed to closing the LSP. Codex post-implementation
        review caught a race where a concurrent apply_text() between
        shutdown's snapshot of open_uris and its didClose loop could
        register a new URI that the snapshot would never see, AND
        could interleave normal traffic into the teardown sequence.
        The fix: shutdown() now sets _shutdown_called BEFORE the
        snapshot; we honor it here under _open_uris_lock so the
        check + state read are atomic. _teardown_thread_id is the
        internal-bypass flag for shutdown's own didClose/shutdown/
        exit traffic and is NOT honored by apply_text() (no internal
        caller of apply_text() exists during teardown)."""
        with self._open_uris_lock:
            if self._shutdown_called:
                raise LspError(
                    "lsp-shutdown",
                    "apply_text after shutdown",
                    lang=self.lang,
                    uri=uri,
                )
            meta = self.open_uri_meta.get(uri)
            if uri not in self.open_uris:
                # First open. Send didOpen under the lock so a
                # concurrent caller sees open_uris populated and
                # falls through to the change path on its own
                # mtime check.
                self.notify("textDocument/didOpen", {
                    "textDocument": {
                        "uri": uri,
                        "languageId": language_id,
                        "version": 1,
                        "text": text,
                    },
                })
                self.open_uris.add(uri)
                self.open_uri_meta[uri] = {
                    "version": 1,
                    "mtime_ns": mtime_ns,
                    "resolved_path": resolved_path,
                    "lang": lang,
                    "language_id": language_id,
                }
                return 1
            # Already open: refresh on mtime drift.
            if meta is None:
                # ensure_open() opened this URI without recording
                # meta (legacy code path, self-tests). Adopt
                # version=1 + current mtime without re-sending
                # didOpen.
                self.open_uri_meta[uri] = {
                    "version": 1,
                    "mtime_ns": mtime_ns,
                    "resolved_path": resolved_path,
                    "lang": lang,
                    "language_id": language_id,
                }
                return 1
            tracked_mtime = meta.get("mtime_ns")
            current_version = meta.get("version", 1)
            if mtime_ns is None or tracked_mtime is None or \
                    mtime_ns == tracked_mtime:
                # No mtime info OR no drift -- the LSP's view is
                # already current relative to what we last sent.
                # No didSave here: Codex review caught the earlier
                # revision unconditionally rebroadcasting didSave
                # for every C/Python tool call, which would push
                # clangd/pyright into pointless re-lint cycles on
                # every hover/definition. didSave only fires on
                # the drift branch below, where it actually pairs
                # with new content.
                return current_version
            # Drift: send didChange with full-file replace, then
            # optionally didSave (force_did_save targets LSPs that
            # only re-lint or re-index on save -- pyright, clangd).
            new_version = current_version + 1
            self.did_change(uri, language_id, text, new_version)
            if force_did_save:
                self.did_save(uri)
            self.open_uri_meta[uri] = {
                "version": new_version,
                "mtime_ns": mtime_ns,
                "resolved_path": (resolved_path
                                  if resolved_path is not None
                                  else meta.get("resolved_path")),
                "lang": (lang if lang is not None
                         else meta.get("lang")),
                "language_id": language_id,
            }
            return new_version

    def flush_notifications(self, timeout: float = 0.05) -> None:
        """Best-effort drain of any pending publishDiagnostics (or
        other server-initiated notifications) the reader thread has
        not yet consumed.

        Implementation: yield to the reader thread for `timeout`
        seconds. The reader thread reads the LSP's stdout in a tight
        loop and dispatches messages as they're parsed, so
        publishDiagnostics that arrived during the calling MCP tool's
        wire round-trip are typically already in self.diagnostics_by_uri
        by the time the round-trip's reply is processed (LSP wire
        ordering guarantees notifications emitted before the response
        are parsed first). The brief sleep covers the narrow race
        where a notification arrived AFTER the response and would
        otherwise miss the cache snapshot the calling tool returns to
        the agent. 50 ms is below human latency perception and well
        under the per-call timeout floor; not adjustable because
        callers should not need to pick this number."""
        time.sleep(max(0.0, timeout))

    # ------------------------------------------------------------------
    # Shutdown
    # ------------------------------------------------------------------

    def shutdown(self, timeout: float = 5.0) -> None:
        """LSP shutdown + exit + process reap.

        Best-effort: if the LSP ignores `shutdown` we SIGTERM; if it
        ignores that we SIGKILL. Exits without raising so atexit-driven
        cleanup can't crash the interpreter.

        Idempotent: safe to call twice. Second call returns immediately.

        Ordering matters: we set _shutdown_called BEFORE the teardown
        traffic so external request/notify/apply_text calls reject
        immediately during teardown (Codex post-implementation review
        High finding: a concurrent apply_text between the open-uris
        snapshot and the didClose loop could otherwise register a new
        URI invisible to the snapshot and interleave normal traffic
        into the teardown sequence). _teardown_thread_id is the
        thread-id-bound bypass that lets ONLY this shutdown thread's
        didClose/shutdown/exit traffic slip past the guard; tool
        threads that already passed apply_text's gate cannot bypass
        because their thread id will not match (Codex follow-up
        High: a global bool was bypassable by ordinary tool traffic;
        binding to threading.get_ident() closes the loophole)."""
        if self._shutdown_called:
            return

        # Serialize teardown against the first initialize() handshake by
        # HOLDING _init_lock across the entire decision AND commit --
        # draining once and releasing (the earlier revision) let a
        # second thread enter initialize() between our release and the
        # _shutdown_called commit, producing a half-initialized
        # subprocess. With the lock held across the commit, any new
        # initialize() call blocks until shutdown finishes marking the
        # instance closed and subsequently rejects itself via the
        # _shutdown_called guard.
        with self._init_lock:
            proc = self._proc
            if proc is None:
                self._shutdown_called = True
                return

            # Commit-up-front: external callers (request, notify,
            # apply_text) all check `_shutdown_called and not
            # _teardown_thread_id != current thread` and reject. Set
            # _teardown_thread_id to OUR thread id in the same window
            # we set _shutdown_called so only this shutdown thread
            # can issue the didClose / shutdown / exit RPCs below.
            # A concurrent MCP-tool thread that already passed
            # apply_text's gate will get a different thread id from
            # threading.get_ident() and reject at request()/notify()
            # entry. Take _open_uris_lock so apply_text's atomic
            # check sees the flag flip without a torn read.
            with self._open_uris_lock:
                self._shutdown_called = True
                self._teardown_thread_id = threading.get_ident()
            try:
                # (1) Graceful LSP shutdown: didClose for every tracked
                # URI, then the shutdown request, then exit notification.
                # didClose ordering matters -- some LSPs hold per-document
                # state (PSES caches script analysis) that they release
                # only on explicit didClose, NOT on shutdown alone, so
                # without this loop we leak document state at the LSP
                # side until the process is fully reaped.
                if proc.poll() is None and self._initialized:
                    with self._open_uris_lock:
                        tracked_uris = list(self.open_uris)
                    for uri in tracked_uris:
                        try:
                            self.notify(
                                "textDocument/didClose",
                                {"textDocument": {"uri": uri}},
                            )
                        except Exception:
                            # didClose is best-effort -- one bad URI
                            # must not block subsequent ones or the
                            # shutdown RPC itself.
                            pass
                    try:
                        self.request(
                            "shutdown", None, timeout=min(timeout, 1.5)
                        )
                    except Exception:
                        pass
                    try:
                        self.notify("exit", None)
                    except Exception:
                        pass
            finally:
                self._teardown_thread_id = None

        # (2) Close stdin so the LSP sees EOF even if it ignored exit.
        try:
            if proc.stdin is not None:
                proc.stdin.close()
        except Exception:
            pass

        # (3) Wait for clean exit; escalate if it hangs.
        deadline = time.monotonic() + max(timeout, 0.1)
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
        if proc.poll() is None:
            try:
                if hasattr(os, "killpg") and proc.pid:
                    os.killpg(proc.pid, signal.SIGTERM)
                else:
                    proc.terminate()
            except Exception:
                pass
            try:
                proc.wait(timeout=min(timeout, 1.0))
            except subprocess.TimeoutExpired:
                try:
                    if hasattr(os, "killpg") and proc.pid:
                        os.killpg(proc.pid, signal.SIGKILL)
                    else:
                        proc.kill()
                except Exception:
                    pass
                try:
                    proc.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    # Child is stuck in uninterruptible sleep; give up
                    # and let the OS reap it at our exit.
                    pass

        # (4) Close remaining pipes + let the reader thread notice EOF.
        for f in (getattr(proc, "stdout", None), getattr(proc, "stderr", None)):
            try:
                if f is not None:
                    f.close()
            except Exception:
                pass
        if self._reader_thread is not None and self._reader_thread.is_alive():
            self._reader_thread.join(timeout=1.0)
        if self._stderr_thread is not None and self._stderr_thread.is_alive():
            self._stderr_thread.join(timeout=1.0)

        # (5) Fail any requests still waiting.
        self._fail_all_pending(LspError(
            "lsp-shutdown",
            f"{self.lang} LSP shut down",
            lang=self.lang,
        ))

        # (6) Walk cleanup_paths -- spawner-owned filesystem state
        # (e.g. PSES per-spawn LogPath / SessionDetailsPath tempdir)
        # that the bridge must remove now that the LSP can no longer
        # be writing into it. Best-effort: a missing path or an
        # already-removed tree must not crash the shutdown path,
        # which runs from atexit hooks where any exception would be
        # swallowed by the interpreter shutdown anyway. shutil.rmtree
        # with ignore_errors=True covers the common case (race with
        # an OS-level temp cleanup, partial directory) without
        # masking legitimate code bugs (the path list is set by
        # spawners; a typo there would skip cleanup but not crash).
        for path in list(self.cleanup_paths):
            try:
                shutil.rmtree(path, ignore_errors=True)
            except Exception:
                pass
        self.cleanup_paths.clear()

    # ------------------------------------------------------------------
    # Introspection
    # ------------------------------------------------------------------

    @property
    def pid(self) -> Optional[int]:
        return self._proc.pid if self._proc else None

    @property
    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    @property
    def crashed(self) -> bool:
        """True iff the reader thread ended without a corresponding
        shutdown() call -- i.e. the subprocess died unexpectedly.
        Set by _reader_loop's finally block; read by the bridge
        respawn path."""
        return self._crashed

    def __repr__(self) -> str:
        state = "alive" if self.alive else "dead"
        return f"<LspSubprocess lang={self.lang!r} pid={self.pid} {state}>"

    def __del__(self) -> None:
        # GC fallback. Primary cleanup is shutdown() + atexit; this
        # only matters if the bridge drops all refs without calling
        # shutdown (e.g. a test creates the instance, lets it fall
        # out of scope). Must be exception-free.
        try:
            if not self._shutdown_called:
                self.shutdown(timeout=0.2)
        except Exception:
            pass
