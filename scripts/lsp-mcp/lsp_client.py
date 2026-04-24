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
import json
import os
import signal
import subprocess
import sys
import threading
import time
import weakref
from concurrent.futures import Future, TimeoutError as FutureTimeoutError
from pathlib import Path
from typing import Any, Optional


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
        self._reader_thread: Optional[threading.Thread] = None
        self._stderr_thread: Optional[threading.Thread] = None
        self._io_lock = threading.Lock()
        self._pending: dict[int, "Future[Any]"] = {}
        self._pending_lock = threading.Lock()
        self._next_id = 1
        self._initialized = False
        self._init_lock = threading.Lock()
        self._shutdown_called = False
        # Teardown bypass flag: shutdown() sets this so the internal
        # shutdown-RPC can slip past the _shutdown_called guard in
        # request()/notify(). Without this, shutdown() would mark
        # itself closed BEFORE sending the graceful LSP shutdown,
        # turning every clean teardown into an EOF/SIGTERM fallback.
        self._teardown_in_progress = False
        self.server_caps: dict[str, Any] = {}
        # Diagnostics get pushed by the server asynchronously; the
        # MCP `diagnostics` tool (section 7) will consume the most
        # recent set per URI from here. Exposed (not _private) so
        # the tool handler can read it directly.
        self.diagnostics_by_uri: dict[str, Any] = {}
        # Tracked URIs for didChange forwarding (section 12 adds the
        # mtime check; we record them now so subsequent sections can
        # extend this without touching __init__.).
        self.open_uris: set[str] = set()

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
        except FileNotFoundError as exc:
            raise LspError(
                "lsp-binary-missing",
                f"{self.cmd[0]!r} not found on PATH",
                lang=self.lang,
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

    def _reader_loop(self) -> None:
        """Parse LSP framed messages from stdout and route replies.

        Two message classes arrive here:
          * Responses (have an `id` matching a pending request)
          * Notifications (no `id`; method like publishDiagnostics).

        A malformed frame fails every pending request with
        lsp-protocol-error so callers do not hang on the Future."""
        proc = self._proc
        assert proc is not None and proc.stdout is not None
        stream = proc.stdout
        try:
            while True:
                headers = self._read_headers(stream)
                if headers is None:
                    # EOF: subprocess exited.
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
                if length < 0 or length > (32 * 1024 * 1024):
                    # Sanity cap: a single LSP message over 32 MiB
                    # means either a protocol desync or a server
                    # trying to dump its entire workspace symbol
                    # table at us. Don't allocate the buffer; treat
                    # as protocol error (fail pending + drain loop).
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
                    msg = json.loads(body.decode("utf-8"))
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
            # Last-chance: any pending request when the reader stops
            # must be failed so callers don't block forever on a
            # child that has already exited.
            self._fail_all_pending(LspError(
                "lsp-subprocess-exited",
                f"{self.lang} LSP reader thread ended",
                lang=self.lang,
            ))

    @staticmethod
    def _read_headers(stream) -> Optional[dict[str, str]]:
        """Read LSP header block (one header per line, \\r\\n-terminated,
        empty line ends). Returns None on EOF."""
        headers: dict[str, str] = {}
        while True:
            line = stream.readline()
            if not line:
                return None  # EOF
            # LSP headers are ASCII. Reject non-ASCII early rather
            # than crashing on a later decode.
            try:
                s = line.decode("ascii").rstrip("\r\n")
            except UnicodeDecodeError:
                return None
            if s == "":
                return headers
            if ":" not in s:
                # Malformed header -- treat same as EOF.
                return None
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
        # Notification. publishDiagnostics is the one we cache now;
        # everything else is logged-and-discarded until a later
        # section wires it up (section 13 uses $/progress, etc.).
        method = msg.get("method")
        params = msg.get("params") or {}
        if method == "textDocument/publishDiagnostics":
            uri = params.get("uri")
            if isinstance(uri, str):
                self.diagnostics_by_uri[uri] = params.get("diagnostics") or []

    def _resolve_pending(self, msg: dict[str, Any]) -> None:
        rid = msg.get("id")
        if not isinstance(rid, int):
            return
        with self._pending_lock:
            fut = self._pending.pop(rid, None)
        if fut is None:
            return
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

    def _fail_all_pending(self, err: LspError) -> None:
        with self._pending_lock:
            pending = self._pending
            self._pending = {}
        for fut in pending.values():
            if not fut.done():
                fut.set_exception(err)

    # ------------------------------------------------------------------
    # Wire I/O
    # ------------------------------------------------------------------

    def _send_frame(self, payload: dict[str, Any]) -> None:
        """Write one LSP-framed JSON-RPC message to the LSP's stdin.
        MUST hold _io_lock while writing -- a partial interleave from
        two threads would corrupt the Content-Length framing."""
        proc = self._proc
        if proc is None or proc.stdin is None:
            raise LspError(
                "lsp-subprocess-exited",
                "stdin unavailable",
                lang=self.lang,
            )
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
        try:
            proc.stdin.write(header)
            proc.stdin.write(body)
            proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise LspError(
                "lsp-subprocess-exited",
                f"write failed: {exc.__class__.__name__}",
                lang=self.lang,
            ) from exc

    def _next_request_id(self) -> int:
        # Serialized under _io_lock in the caller; a raw int bump is
        # fine there. Kept here for clarity + to make the invariant
        # explicit.
        rid = self._next_id
        self._next_id += 1
        return rid

    def request(self, method: str, params: Optional[dict[str, Any]] = None,
                timeout: float = 5.0) -> Any:
        """Send a JSON-RPC request + wait for its reply.

        Raises LspError on timeout, subprocess crash, or LSP-reported
        error. Callers in bridge.py translate this into the JSON
        error envelope returned to the MCP agent.

        During shutdown() teardown, the internal `shutdown` RPC must
        bypass the _shutdown_called guard: _teardown_in_progress
        flips True for the duration of that single RPC so the
        graceful handshake can land before we mark the instance
        fully closed."""
        if self._shutdown_called and not self._teardown_in_progress:
            raise LspError(
                "lsp-shutdown",
                "request after shutdown",
                lang=self.lang,
                method=method,
            )
        fut: "Future[Any]" = Future()
        with self._io_lock:
            rid = self._next_request_id()
            with self._pending_lock:
                self._pending[rid] = fut
            payload = {
                "jsonrpc": "2.0",
                "id": rid,
                "method": method,
            }
            if params is not None:
                payload["params"] = params
            try:
                self._send_frame(payload)
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

        Same teardown bypass as request(): shutdown() sends `exit`
        while _shutdown_called is already True. `initialized` is
        also allowed during teardown since the handshake may not
        have completed yet when shutdown fires."""
        if (self._shutdown_called and not self._teardown_in_progress
                and method != "exit"):
            raise LspError(
                "lsp-shutdown",
                "notification after shutdown",
                lang=self.lang,
                method=method,
            )
        payload: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            payload["params"] = params
        with self._io_lock:
            self._send_frame(payload)

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
            if isinstance(result, dict):
                self.server_caps = result.get("capabilities") or {}
            self.notify("initialized", {})
            self._initialized = True

    def did_open(self, uri: str, language_id: str, text: str,
                 version: int = 1) -> None:
        """Notify the LSP that a document is open. Required before
        most textDocument/* requests; LSPs typically reject requests
        against URIs they have not seen a didOpen for."""
        self.notify("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": language_id,
                "version": version,
                "text": text,
            },
        })
        self.open_uris.add(uri)

    # ------------------------------------------------------------------
    # Shutdown
    # ------------------------------------------------------------------

    def shutdown(self, timeout: float = 5.0) -> None:
        """LSP shutdown + exit + process reap.

        Best-effort: if the LSP ignores `shutdown` we SIGTERM; if it
        ignores that we SIGKILL. Exits without raising so atexit-driven
        cleanup can't crash the interpreter.

        Idempotent: safe to call twice. Second call returns immediately.

        Ordering matters: we send the graceful LSP shutdown RPC
        FIRST (with _teardown_in_progress bypassing the
        _shutdown_called guard in request/notify), and only then
        flip _shutdown_called to True so further public calls are
        rejected. Flipping the flag up front (as the earlier
        revision did) made the graceful RPC unreachable on every
        teardown path."""
        if self._shutdown_called:
            return

        proc = self._proc
        if proc is None:
            self._shutdown_called = True
            return

        # (1) Graceful LSP shutdown: request + exit notification.
        # _teardown_in_progress lets these slip past the guard that
        # normal callers of request()/notify() see.
        if proc.poll() is None and self._initialized:
            self._teardown_in_progress = True
            try:
                try:
                    self.request("shutdown", None, timeout=min(timeout, 1.5))
                except Exception:
                    pass
                try:
                    self.notify("exit", None)
                except Exception:
                    pass
            finally:
                self._teardown_in_progress = False

        # From here on, no new requests/notifications from callers.
        self._shutdown_called = True

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

    # ------------------------------------------------------------------
    # Introspection
    # ------------------------------------------------------------------

    @property
    def pid(self) -> Optional[int]:
        return self._proc.pid if self._proc else None

    @property
    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

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
