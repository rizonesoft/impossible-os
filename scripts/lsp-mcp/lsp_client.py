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
import errno
import io
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
import uuid
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


# ----------------------------------------------------------------------
# Ownership identity: which LSP processes did THIS bridge start?
# ----------------------------------------------------------------------
#
# "Did the bridge leak a language server?" is only answerable if the
# asker can tell OUR children from every other language server on the
# host. The obvious shell answer -- `pgrep -u UID -f clangd-19`, diffed
# against a snapshot -- cannot, and measurably misfires three ways
# (2026-08-03, 15 harness runs while the agent fleet ran alongside):
#
#   * it matches command-line TEXT, so a `node codex-companion.mjs`
#     process whose argv merely QUOTED the string "clangd-19" was
#     reported as a leaked language server;
#   * it matches FOREIGN language servers owned by the same user -- one
#     clangd from another session's bridge lived across four harness
#     runs and was blamed on each of them;
#   * it matches processes that are already exiting, so the reported PID
#     can be gone before the reporter can even print its command line.
#
# Identity therefore comes from the spawner, not from a pattern match.
# Every spawn stamps LSP_BRIDGE_RUN_ID into the child's environment and,
# when LSP_BRIDGE_PID_LEDGER names a file, appends one record for the
# child. A reader (the test harness, an operator) then asks a precise
# question -- "is a process WE recorded still alive?" -- instead of an
# imprecise one. /proc/PID/stat field 22 (start time in clock ticks) is
# recorded alongside the PID so a recycled PID cannot masquerade as a
# survivor.
_RUN_ID = os.environ.get("LSP_BRIDGE_RUN_ID") or uuid.uuid4().hex
# ATTRIBUTION and OWNERSHIP are different questions and need different ids.
#
# _RUN_ID answers "which RUN started this server", is inherited from the
# environment when present, and is what the test harness attributes leaks
# with -- a harness that exports it wants every bridge it launches to share
# it.
#
# _OWNER_ID answers "did THIS PROCESS start it", so it is minted fresh here
# and never read from the environment. Reaping must use this one: the first
# version swept on _RUN_ID, and under a harness that exports it that set
# included the harness shell, its other bridges and the test driver itself
# -- a bridge exiting normally SIGKILLed its own siblings, which is how the
# suite caught it (7 sub-tests failed at once).
_OWNER_ID = uuid.uuid4().hex
_PID_LEDGER = os.environ.get("LSP_BRIDGE_PID_LEDGER") or None
_LEDGER_LOCK = threading.Lock()

# Identity records for every language server this process has spawned, appended
# the instant Popen returns -- before the ledger write, before the reader
# threads exist, and before the instance is published anywhere. Each entry is
# (pid, start_ticks, pidfd): enough to kill THAT process and no other.
#
# A bare PID is not an identity. It is a slot number the kernel reuses, so a
# record kept for the lifetime of a long-running bridge eventually names
# somebody else's process -- and a force-kill that trusts it will SIGKILL an
# unrelated process group during shutdown (Codex adversarial review, High).
# So identity is pinned two ways: a pidfd where the kernel supports one (it
# refers to the process itself and can never be recycled), and the process
# start time otherwise, which changes the moment the slot is reused.
#
# Plain list, read WITHOUT a lock, deliberately. Every other record of a
# subprocess is reachable only by taking a lock, and the caller who needs this
# one most -- a process about to die, unwinding from a signal, possibly
# interrupting a thread that already holds those very locks -- cannot afford to
# wait for one. list.append and list(...) are atomic under the GIL, so a
# snapshot is always a consistent prefix of what has been spawned.
#
# THE RECORDED pidfd HAS EXACTLY ONE OWNER: _retire_spawn, the function that
# removes the record. Nothing else may signal through it, close it, or even
# assume it is still open. A lock-free snapshot hands out (pid, ticks, fd)
# tuples that outlive the record they came from, so a sweep holding one can
# have that descriptor closed underneath it by a concurrent retirement -- and
# the number then names whatever unrelated file the process has since opened.
# Every other consumer calls _own_pidfd() for a descriptor of its own.
class _SpawnRecord:
    """One spawned child, identified by OBJECT IDENTITY.

    Deliberately not a tuple. Every membership and removal test here is
    an ownership decision, and a tuple compares by VALUE: retirement
    could remove `(pid, ticks, fd)` and close it, a replacement could be
    handed the same PID inside the same 100 Hz tick and the same
    descriptor number, and its freshly appended tuple would be equal to
    the snapshot a sweep still holds. The sweep would then conclude its
    record was untouched and signal the replacement -- a textbook ABA,
    which the GIL does nothing about because it serializes each
    operation and not the protocol (Codex adversarial review, High).

    A plain object compares by identity, so `entry in _SPAWNED` and
    `_SPAWNED.remove(entry)` mean THIS record and no other."""

    __slots__ = ("pid", "ticks", "fd")

    def __init__(self, pid: int, ticks: Optional[int],
                 fd: Optional[int]) -> None:
        self.pid = pid
        self.ticks = ticks
        self.fd = fd


_SPAWNED: list = []

# Spawns that have begun but are not yet recorded above. A signal can be
# delivered between Popen returning and the append -- the handler freezes that
# frame, and a sweep taken right then sees an empty list and exits over a live
# child. The count lets the sweep WAIT for the in-flight spawn to record itself
# instead of concluding there is nothing to kill.
#
# Mutated under a lock (the spawn path can afford one), read without it (the
# sweep cannot). Reading an int binding is atomic under the GIL.
#
# The obvious alternative -- PR_SET_PDEATHSIG, so the kernel kills the child
# when the parent dies -- is WRONG here: pdeathsig fires when the spawning
# THREAD exits, and warm-start spawns from worker threads that exit
# immediately afterwards. It would reap healthy language servers seconds
# after a successful warm start.
_SPAWN_INFLIGHT = 0
_SPAWN_INFLIGHT_LOCK = threading.Lock()

# The SECOND ownership signal, for a descendant whose environment cannot
# be read at all.
#
# _carries_our_owner_id is the broad ownership question and it has one
# blind spot it cannot close from the inside: a process that called
# PR_SET_DUMPABLE(0) (directly, or implicitly by exec'ing a setuid binary)
# has its /proc/<pid> reassigned to root, so neither its environ nor any
# task's environ is readable by us. The probe then answers None -- unknown
# -- forever, the stamp-wide pass never claims it, and a hardened
# descendant of a language server escapes every sweep and leaks.
#
# Every spawn uses start_new_session=True, so each language server is a
# session leader whose session id equals its own pid, and a descendant
# inherits that session id unless it calls setsid itself. Session
# membership is read from /proc/<pid>/stat, which stays world-readable
# when the environment does not -- which is exactly the property the
# environ probe lacks.
#
# {session id -> the _SpawnRecord of the leader that created it}. The
# value is the RECORD, not its start ticks: ticks have 100 Hz granularity
# and this file already measured 32 concurrent spawns sharing seven
# values, so a recycled session id whose new leader started inside the
# same tick would satisfy a ticks comparison and authorize a SIGKILL
# against a stranger (Codex adversarial review, High). The record carries
# the leader's own pidfd, which names the process itself and can never
# retarget -- see _session_marks_ours.
#
# READ lock-free (dict.get is atomic under the GIL), because the caller
# who needs it most is a process unwinding from a signal that cannot wait
# for anything. WRITERS differ by path, deliberately:
#
#   * publication and the prune, both in _record_spawn, take the lock
#     BLOCKING. An unguarded store was tried and removed -- landing
#     between retirement's identity check and its pop, it let a stale
#     generation delete the live replacement's marker, which is the exact
#     race the lock exists to stop. Blocking is safe here because nothing
#     that can INTERRUPT this frame takes the lock: the sweep only reads.
#   * retirement takes it NON-BLOCKING and skips when contended. A missed
#     pop leaves a stale entry, and a stale entry fails closed, so the
#     wait is not worth the exposure.
#
# Publication happens in the same breath as the _SPAWNED append, NOT after
# the descriptor is obtained: a termination in that gap left the sweep
# with no marker while the leader was killed (Codex adversarial review,
# High, Medium x2).
_OWNED_SESSIONS: dict = {}
_OWNED_SESSIONS_LOCK = threading.Lock()


class _Clock:
    """The reap's only source of time, passed EXPLICITLY.

    Every deadline in the module-level sweep is an absolute value on some
    clock, and a test that wants to pin the five break points (settle
    wait, pre-scan, ownership enumeration, pre-signal, post-target) needs
    to be the one deciding when each is crossed -- otherwise it can only
    assert that the sweep as a whole stayed inside a wall-clock budget,
    which one deleted check still satisfies.

    It is a PARAMETER and deliberately not a module global that tests
    monkeypatch. A global would put two clock domains in one call: the
    graceful reap derives a per-instance budget from the deadline and
    hands it to LspSubprocess.shutdown(), whose own waits are real, and
    _atexit_kill_all derives a Thread.join timeout the same way. A
    scripted clock still installed at interpreter exit would turn that
    nominal 2s atexit bound into an arbitrary real join (Codex design
    review, Medium). So the seam covers only the procfs sweep, which does
    nothing but read /proc, signal, and sleep; atexit, reap_all_live and
    every instance method stay bound to the real clock forever."""

    __slots__ = ("now", "sleep")

    def __init__(self, now=time.monotonic, sleep=time.sleep) -> None:
        self.now = now
        self.sleep = sleep


_REAL_CLOCK = _Clock()

# Signalling by NUMBER is not available on a supported host.
#
# os.pidfd_open and signal.pidfd_send_signal both arrived in python3 3.9.
# The bare-PID fallback in _kill_verified/_signal_recorded was shipped in
# section 20 to keep the pidfd-less 3.8 host from leaking a child on every
# teardown, citing the python3 3.8 floor in scripts/setup.sh -- but THIS
# module declares "Stdlib-only (Python 3.10+)" at the top of the file, so
# a host that can meet its documented runtime always has both functions.
# The fallback was therefore protecting a configuration this module does
# not support, at the price of the one race it cannot rule out: the target
# exits, is reaped, and its number is reused between the identity check
# and the signal, so an unrelated process is killed.
#
# The decision (section 23) is that destructive signalling NEVER names a
# target by number on a supported host. When the descriptor path is
# genuinely unavailable the sweep refuses and says so, which loses a child
# instead of killing a stranger -- and a leak is recoverable where a kill
# is not. LSP_BRIDGE_ALLOW_PID_SIGNAL=1 restores the old behaviour for an
# out-of-profile host that would otherwise leak on every teardown; it is
# named, opt-in, and reported, rather than the silent default it was.
_PID_SIGNAL_REFUSALS = 0
_PID_SIGNAL_WARNED = False

# Two descriptors, opened at import and never used, so the reap can still
# get the exact handles it needs when the process has run out of them.
#
# Refusing to signal by number is only safe if the descriptor path stays
# reachable, and the failure it must survive is exactly the one that makes
# teardown urgent: under EMFILE/ENFILE os.pidfd_open fails at spawn (so the
# record carries no descriptor) AND fails again at teardown (so _own_pidfd
# cannot make one) -- and the refusal then turns the deterministic reap
# sections 20 and 22 built into a leak (Codex adversarial review, High).
# Closing this reserve buys back exactly the one descriptor a pidfd needs;
# it is re-armed immediately afterwards.
#
# NOT a general-purpose allocator: a slot is claimed by REMOVING it from
# the list (list.pop is atomic under the GIL), so two callers can never
# both believe they hold the same one, and nobody waits.
# LOCK-FREE, by the same rule the _SPAWNED comment states: the caller who
# needs this most is a process unwinding from a signal, possibly having
# interrupted a frame that holds the very lock it would wait for. A
# threading.Lock here deadlocked exactly that way -- _record_spawn reaches
# _pidfd_open on the main thread, a termination handler interrupts it
# inside the locked region, and the handler's force sweep hits EMFILE and
# blocks forever on a non-reentrant lock its own interrupted frame holds
# (Codex adversarial review, High). list.pop() and list.append() are
# atomic under the GIL, so the slot is claimed by REMOVING it: exactly one
# caller can win, and nobody waits.
_FD_RESERVE: list = []

# TWO, not one. Pass 0 holds a leader-proof handle and a target handle at
# the SAME time -- that is the protocol, not an accident of ordering -- so
# a single reserved descriptor deterministically failed the second
# acquisition at a hard RLIMIT_NOFILE wall and skipped the SIGKILL, with
# no concurrency required (Codex adversarial review, High).
_FD_RESERVE_SIZE = 2


def _arm_fd_reserve() -> None:
    """Best-effort: put a descriptor back in the reserve slot.

    Called both after spending the reserve and at the top of every later
    open, because the re-arm attempted immediately after a successful
    pidfd_open runs while that pidfd still occupies the freed slot -- so
    under a genuinely exhausted RLIMIT_NOFILE it fails, and a re-arm that
    only ever ran there left the reserve empty forever (Codex adversarial
    review, Medium). Retrying on the next call succeeds as soon as any
    descriptor has been released."""
    while len(_FD_RESERVE) < _FD_RESERVE_SIZE:
        try:
            fd = os.open(os.devnull, os.O_RDONLY)
        except Exception:
            return                  # table is full; try again next call
        if len(_FD_RESERVE) >= _FD_RESERVE_SIZE:
            try:                    # somebody topped it up first
                os.close(fd)
            except Exception:
                pass
            return
        _FD_RESERVE.append(fd)


_arm_fd_reserve()


def _with_fd_reserve(make):
    """Run `make()`, and if it fails for want of a DESCRIPTOR, spend the
    emergency reserve and try once more.

    Shared by every exact-handle acquisition in the reap. It was first
    written into _pidfd_open alone, which left _borrow_pidfd's os.dup
    uncovered -- so at RLIMIT_NOFILE the LEADER duplication failed,
    _session_marks_ours answered False, the session scan rejected every
    opaque descendant, and the leader signal then killed the one process
    that could still
    prove them ours (Codex adversarial review, High).

    A descriptor SHORTAGE is a different thing from an absent capability:
    the shortage is transient and self-inflicted (the bridge is the
    process holding the descriptors), so it is the one worth retrying."""
    _arm_fd_reserve()               # cheap no-op while the slot is full
    try:
        return make()
    except OSError as exc:
        if exc.errno not in (errno.EMFILE, errno.ENFILE):
            return None
    except Exception:
        return None
    # Out of descriptors. Claim the reserve by removing it -- atomically,
    # so a second caller (or a signal handler that interrupted this very
    # frame) simply finds the slot empty instead of waiting on a lock.
    try:
        spare = _FD_RESERVE.pop()
    except IndexError:
        return None                 # no reserve, or another caller has it
    try:
        os.close(spare)
    except Exception:
        pass
    try:
        return make()
    except Exception:
        return None
    finally:
        # Usually fails right here, because the descriptor just returned is
        # sitting in the slot the close freed. _arm_fd_reserve is therefore
        # also called at the TOP of the next acquisition, which is where it
        # actually succeeds.
        _arm_fd_reserve()


def _pidfd_open(pid: int) -> Optional[int]:
    """os.pidfd_open, reserve-backed. None when the syscall is
    unavailable, denied, or the process is gone."""
    if not hasattr(os, "pidfd_open"):
        return None
    return _with_fd_reserve(lambda: os.pidfd_open(pid, 0))


def _bare_pid_signal_allowed() -> bool:
    """True when this host is permitted to signal by number.

    Read from the environment on every call rather than cached at import:
    the harness sets it per sub-test, and a value frozen at import cannot
    be exercised without one subprocess per case."""
    return os.environ.get("LSP_BRIDGE_ALLOW_PID_SIGNAL") == "1"


def _refuse_bare_pid_signal(pid: int, sig: int) -> bool:
    """Record -- and once per process, report -- a destructive signal
    declined for want of a pidfd. Always returns False so a caller can
    `return _refuse_bare_pid_signal(pid, sig)`.

    Reported because the alternative outcome is a surviving language
    server, and a leak nobody was told about is exactly the
    unattributable leak the ownership stamps exist to prevent."""
    global _PID_SIGNAL_REFUSALS, _PID_SIGNAL_WARNED
    _PID_SIGNAL_REFUSALS += 1
    # Reported once, but the COUNT rides along, so a later refusal for a
    # different pid is not silently swallowed by the once-flag -- the
    # first message hardcoded SIGKILL even when the refused signal was
    # the SIGTERM of a graceful stop (Codex consistency review, Medium).
    if not _PID_SIGNAL_WARNED:
        _PID_SIGNAL_WARNED = True
        try:
            try:
                name = signal.Signals(sig).name
            except Exception:
                name = f"signal {sig}"
            have = hasattr(os, "pidfd_open") and hasattr(
                signal, "pidfd_send_signal")
            why = ("no pidfd support in python3 "
                   f"{sys.version_info.major}.{sys.version_info.minor} "
                   "(this module documents 3.10+)" if not have
                   else "no pidfd could be opened for this target")
            sys.stderr.write(
                f"[lsp-mcp] refusing to send {name} to pid {pid} by "
                f"number: {why}. The server may survive teardown; further "
                f"refusals are counted in _PID_SIGNAL_REFUSALS rather "
                f"than reported. Set LSP_BRIDGE_ALLOW_PID_SIGNAL=1 to "
                f"accept the PID-reuse race instead.\n")
        except Exception:
            pass
    return False


def run_id() -> str:
    """The identity stamped into every LSP this process spawns."""
    return _RUN_ID


def proc_start_ticks(pid: int) -> Optional[int]:
    """Field 22 of /proc/<pid>/stat -- process start time in clock
    ticks since boot. Pinning it beside the PID is what makes a
    survivor check immune to PID reuse. None when procfs is absent
    (non-Linux) or the process is already gone.

    The stat line embeds comm in parens and comm may itself contain
    spaces or parens, so parse from the LAST ')' rather than splitting
    the whole line."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            raw = fh.read().decode("utf-8", "replace")
        tail = raw[raw.rindex(")") + 1:].split()
        # tail[0] is state (field 3); start time is field 22.
        return int(tail[19])
    except Exception:
        return None


def _ledger_record(pid: int, lang: str, argv0: str) -> None:
    """Append one ownership record: run id, pid, start ticks, lang,
    binary. Best-effort -- a bridge must never fail to serve because a
    diagnostic ledger is unwritable.

    Written with a single O_APPEND write() of a short line so that
    concurrent bridges sharing one ledger cannot interleave partial
    records (POSIX guarantees atomicity for an append under PIPE_BUF;
    every field here is bounded well below it)."""
    if not _PID_LEDGER:
        return
    ticks = proc_start_ticks(pid)
    line = "\t".join((
        _RUN_ID,
        str(pid),
        str(ticks if ticks is not None else -1),
        lang.replace("\t", " "),
        os.path.basename(argv0).replace("\t", " "),
    ))[:4000] + "\n"
    try:
        with _LEDGER_LOCK:
            fd = os.open(_PID_LEDGER, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
            try:
                os.write(fd, line.encode("utf-8", "replace"))
            finally:
                os.close(fd)
    except Exception:
        pass


def reap_all_live(timeout: float = 0.5, deadline: Optional[float] = None) -> int:
    """Shut down every LspSubprocess still registered as live, and
    report how many were still running when asked.

    This is the graceful backstop for the window between Popen returning
    and the instance being published into the bridge's own _LIVE_LSPS
    registry: a shutdown path that only walks the published registry
    cannot see a child spawned microseconds ago. Callers that terminate
    the process themselves (a signal handler, the idle watchdog's
    os._exit) MUST call this, because those paths bypass atexit.

    `deadline` is an absolute time.monotonic() value bounding the WHOLE
    sweep, not each instance: without it, per-instance timeouts multiply
    by the number of language servers and overrun whatever budget the
    caller thought it had (Codex adversarial review, High)."""
    reaped = 0
    for lsp in list(_LIVE_SUBPROCS):
        if deadline is not None and time.monotonic() >= deadline:
            # Budget spent. Continuing with a token 50ms per instance is how
            # a bounded cleanup quietly overruns its bound; the caller's
            # force sweep is the correct next step, not more graceful work.
            break
        budget = timeout
        if deadline is not None:
            budget = min(timeout, max(0.05, deadline - time.monotonic()))
        try:
            if lsp.alive:
                reaped += 1
            lsp.shutdown(timeout=budget)
        except Exception:
            pass
    return reaped


def _record_spawn(pid: int) -> "_SpawnRecord":
    """Pin a spawned child's identity as tightly as the OS allows.

    The pidfd names the process exactly and can never retarget. It does
    NOT reserve the number: free_pid() returns that to the allocator when
    the task is reaped, however many struct pid references survive. The
    record's start time, not its descriptor, is what identifies the
    NUMBER later."""
    # PUBLISHED FIRST, descriptor second, and NOT under a signal mask.
    #
    # A pthread_sigmask around this window was tried and removed: it masks
    # only the CALLING thread, and CPython runs the Python-level handler
    # on the MAIN thread at the next bytecode boundary even when main has
    # the signal blocked, because the C handler can be delivered to any
    # unblocked worker -- the reader, stderr-drain and warm-start threads
    # all qualify. It therefore did not close the race it claimed to, and
    # a false claim in a teardown path is worse than a stated gap (Codex
    # adversarial review, High).
    #
    # What actually helps is publication ORDER: the record and its session
    # marker go out immediately after the append, so the sweep sees an
    # in-flight child and the session it leads as early as it can be shown
    # one. That is EARLIER, not atomic -- the three stores are separate
    # statements and a termination can still land between them.
    #
    # The residual is NOT gated on FD exhaustion. Between the append and
    # the assignment of record.fd a few statements later, the record has
    # no descriptor -- on EVERY spawn -- so _borrow_pidfd fails closed and
    # the session proof cannot be made. A termination landing in that
    # interval means the session scan skips a hardened descendant before
    # the leader signal
    # kills its leader. Exhaustion only makes it worse, by denying the
    # fallback acquisition that would otherwise still work. Parked in the
    # section rather than papered over.
    record = _SpawnRecord(pid, None, None)
    _SPAWNED.append(record)
    # Marker published in the SAME breath as the record. It used to
    # wait until after the descriptor and start time were read, and a
    # termination landing in that gap left the session scan with no
    # marker at all while the leader signal ran -- so a descendant
    # that had already hardened itself was unreachable. That window
    # needed no FD exhaustion to open (Codex adversarial review,
    # High). Publishing a record whose fd is still None is safe: the
    # session proof borrows that descriptor, so an unfinished record
    # fails CLOSED and authorizes nothing.
    try:
        if pid != os.getsid(0):
            with _OWNED_SESSIONS_LOCK:
                _OWNED_SESSIONS[pid] = record
    except Exception:
        pass
    # Reserve-aware: a spawn that lands under FD exhaustion would
    # otherwise record no descriptor at all, and a record with no
    # descriptor is exactly the one the reap can no longer signal.
    record.fd = _pidfd_open(pid)    # Linux 5.3+; never recycled
    record.ticks = proc_start_ticks(pid)
    try:
        if pid != os.getsid(0):
            # Hygiene only -- the marker itself went out above. Drops
            # entries whose record has already left _SPAWNED through
            # the two descriptor-claim paths, which run in a dying
            # process and may not take a lock.
            with _OWNED_SESSIONS_LOCK:
                live = {id(e) for e in list(_SPAWNED)}
                for sid in [k for k, v in list(_OWNED_SESSIONS.items())
                            if id(v) not in live]:
                    _OWNED_SESSIONS.pop(sid, None)
    except Exception:
        pass

    # Handed back so the owner can retire THIS generation later. Retiring
    # by number instead conflates generations: two of them can answer to
    # one PID, and a by-number retirement then removes and closes the
    # LIVE one's record (Codex adversarial review, High). The respawn
    # path used to make that routine by creating the replacement before
    # disposing the dead instance; since section 25 it disposes and
    # confirms first, which narrows the overlap without removing it --
    # the force sweep and the atexit hook still walk records for
    # generations nobody is holding.
    return record


def _retire_spawn(target) -> None:
    """Drop a confirmed-dead child's record so its PID can never be
    matched again once the kernel reuses the number, and release the
    pidfd. Best-effort: a missing record is not an error.

    The successful list removal is the ownership token for the close.
    Closing regardless -- as the first version did -- lets two concurrent
    retirements of the same pid close the same descriptor twice, and the
    second close lands on whatever unrelated file has since been handed
    that number (Codex adversarial review, Medium)."""
    for entry in list(_SPAWNED):
        if isinstance(target, _SpawnRecord):
            if entry is not target:
                continue
        elif entry.pid != target:
            continue
        try:
            _SPAWNED.remove(entry)
        except ValueError:
            continue          # another thread retired it; its close, not ours
        if entry.fd is not None:
            try:
                os.close(entry.fd)
            except Exception:
                pass
        # Drop the session marker with the record that justified it, by
        # OBJECT IDENTITY and under the lock. A stale generation and a live
        # replacement can share a PID; comparing start ticks instead let a
        # stale retirement pop the replacement's marker, after which the
        # replacement's non-dumpable descendants became unidentifiable
        # (Codex adversarial review, High).
        if _OWNED_SESSIONS_LOCK.acquire(blocking=False):
            try:
                if _OWNED_SESSIONS.get(entry.pid) is entry:
                    _OWNED_SESSIONS.pop(entry.pid, None)
            finally:
                _OWNED_SESSIONS_LOCK.release()
        # By NUMBER, retire exactly one generation. Sweeping every record
        # sharing a PID is how a live replacement lost its descriptor.
        if not isinstance(target, _SpawnRecord):
            return


def _borrow_pidfd(entry: tuple) -> Optional[int]:
    """A private duplicate of a RECORD's own pidfd, or None.

    This is the only exact identity available for a recorded child, and
    it is exact in the way ticks are not: the descriptor is bound to the
    process, so no PID recycle -- not even one that lands on the same
    clock tick -- can redirect a signal sent through it.

    The dup is race-safe against retirement by ORDERING, without a lock
    the dying-process path could not afford. _retire_spawn removes the
    record BEFORE it closes the descriptor, so if the entry is still in
    _SPAWNED *after* the dup returned, the close had not begun when the
    dup ran, and the dup therefore names the recorded descriptor rather
    than whatever unrelated file has since been given that number. If the
    entry is gone, another thread already confirmed this child dead and
    this caller has nothing to signal.

    The original stays owned by _retire_spawn; the caller closes only the
    duplicate (Codex adversarial review, High)."""
    fd = entry.fd
    if fd is None:
        return None
    # Reserve-backed: this dup is how a session leader proves it is still
    # alive, and it is taken during teardown -- precisely when the process
    # may be out of descriptors.
    borrowed = _with_fd_reserve(lambda: os.dup(fd))
    if borrowed is None:
        return None
    if entry in _SPAWNED:
        return borrowed
    try:
        os.close(borrowed)
    except Exception:
        pass
    return None


def _own_pidfd(pid: int, still_ours) -> Optional[int]:
    """A pidfd for `pid` that the CALLER owns outright, or None when one
    cannot be proven to name the process we meant. The caller closes it.

    This exists because the descriptor in a _SPAWNED record belongs to
    _retire_spawn and to nobody else (see the _SPAWNED comment). A sweep
    that snapshots the record and then signals through its fd can have
    that fd closed by a concurrent retirement first, at which point the
    number names an unrelated file -- and if that file happens to be
    another pidfd, the signal lands on a stranger.

    Verifying identity AFTER the open, rather than before, is what makes
    this useful: the descriptor is bound to whatever process answered to
    that number at open time and can never retarget, so a `still_ours`
    that agrees afterwards is talking about the same process the signal
    will reach. Checking first and opening second would leave exactly the
    window this function exists to remove (Codex design review, pidfd
    ownership).

    It is NOT a claim that the number stopped moving -- an open pidfd
    does not reserve one; free_pid() releases it at reap. If the number
    was recycled before this open, the descriptor names the STRANGER, and
    only `still_ours` stands between that and a signal. Give it real
    evidence (Codex adversarial review, High)."""
    fd = _pidfd_open(pid)
    if fd is None:
        return None                 # no pidfd support, or already gone
    try:
        if still_ours(pid):
            return fd
    except Exception:
        pass
    try:
        os.close(fd)
    except Exception:
        pass
    return None


def _stat_fields_at(path: str) -> Optional[list[str]]:
    """As _stat_fields, for any /proc stat path -- a thread's, not just a
    process's."""
    try:
        with open(path, "rb") as fh:
            raw = fh.read().decode("utf-8", "replace")
        return raw[raw.rindex(")") + 1:].split()
    except Exception:
        return None


def _stat_fields(pid: int) -> Optional[list[str]]:
    """/proc/<pid>/stat from the last ')' onward, so index 0 is state
    (field 3), 1 is ppid (field 4), 2 is pgrp (field 5). comm is
    parenthesised and may itself contain spaces or parens, which is why
    the split starts after the LAST ')'."""
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            raw = fh.read().decode("utf-8", "replace")
        return raw[raw.rindex(")") + 1:].split()
    except Exception:
        return None


def _ppid_of(pid: int) -> Optional[int]:
    f = _stat_fields(pid)
    try:
        return int(f[1]) if f else None
    except Exception:
        return None


def _is_zombie(pid: int) -> bool:
    """True only when EVERY thread of this process has exited.

    A zombie still has a /proc entry and still answers pidfd_open, so a
    sweep that does not exclude it reports killing processes that were
    already dead -- which is how the strengthened 9c assertion caught it:
    the force sweep returned 1 having sent no signal at all.

    But the leader's own state is not the process's state. A
    thread-group leader that calls pthread_exit sits in Z while its
    workers keep running, so reading only /proc/PID/stat would classify
    a live process as dead and skip it in both scans -- turning a
    reporting bug into a leak (Codex adversarial review, Medium,
    reproduced). Every task must be Z, and anything unreadable or racing
    counts as ALIVE: the cost of a redundant signal to a dead process is
    nothing, and the cost of skipping a live one is the leak this whole
    section exists to prevent."""
    f = _stat_fields(pid)
    if not f or f[0] != "Z":
        return False
    try:
        tasks = os.listdir(f"/proc/{pid}/task")
    except Exception:
        return False
    if not tasks:
        return False
    for tid in tasks:
        tf = _stat_fields_at(f"/proc/{pid}/task/{tid}/stat")
        if tf is None or tf[0] != "Z":
            return False
    return True


def _pgrp_of(pid: int) -> Optional[int]:
    f = _stat_fields(pid)
    try:
        return int(f[2]) if f else None
    except Exception:
        return None


def _session_of(pid: int) -> Optional[int]:
    """Field 6 of /proc/<pid>/stat -- the session id.

    Readable when the environment is not, which is the entire reason this
    marker exists: PR_SET_DUMPABLE(0) reassigns /proc/<pid> to root and
    closes the environ, but leaves stat world-readable."""
    f = _stat_fields(pid)
    try:
        return int(f[3]) if f else None
    except Exception:
        return None


def _session_marks_ours(pid: int, sid: Optional[int]) -> bool:
    """True when `pid`'s session is one we created AND that session is
    still provably the one we created.

    A session id is a pid, and a pid is reusable, so membership alone is
    not ownership -- the same objection that makes a bare process-group
    number an unsafe address. The kernel frees a struct pid only once
    EVERY pid_type hlist is empty, so the number cannot be recycled while
    any member of that session lives; but "cannot be recycled while a
    member lives" is not "was never recycled". Our session can empty, the
    number be handed to a stranger who calls setsid, fork an opaque
    member and exit -- leaving a live session with our recorded number
    whose leader is GONE (Codex design review, High).

    So the authorization is CONTINUITY, not membership: the leader we
    recorded must still be ALIVE. While it is, it holds the number itself
    and no recycle can have happened.

    And "still alive" is proven through the leader's OWN pidfd, not
    through its start time. A ticks comparison looks equivalent and is
    not: field 22 has 100 Hz granularity, this file measured 32 concurrent
    spawns sharing seven values, and a session id recycled to a leader
    that started inside the same tick would satisfy it -- authorizing a
    SIGKILL against a stranger (Codex adversarial review, High). A pidfd
    is bound to the process, so signal 0 through it answers the exact
    question: is THAT process, not that number, still there.

    No descriptor on the record means no exact handle, and that fails
    closed rather than falling back to the number.

    That is deliberately narrower than the marker could be. It covers a
    hardened descendant of a LIVE language server, which is the shape the
    section set out to reap. It does NOT cover a hardened descendant whose
    leader already exited, and it cannot cover a descendant that called
    setsid itself -- both need a kernel-enforced container (a cgroup) that
    this bridge cannot create unprivileged."""
    fd = _session_handle(sid)
    if fd is None:
        return False
    try:
        return _leader_alive(fd)
    finally:
        try:
            os.close(fd)
        except Exception:
            pass


def _session_handle(sid: Optional[int]) -> Optional[int]:
    """A borrowed pidfd for the recorded leader of session `sid`, or None.

    Split out of _session_marks_ours so a caller can HOLD one proof handle
    across several steps instead of re-acquiring it. Pass 0 needs exactly
    that: it proves the session, opens the target, and re-proves after the
    target is pinned -- three acquisitions where two descriptors exist, so
    at a full table the last one failed and the reap was skipped (Codex
    adversarial review, High). The caller closes it."""
    if sid is None or sid <= 0:
        return None
    record = _OWNED_SESSIONS.get(sid)
    if record is None:
        return None
    # A dup of the record's own descriptor, taken the same race-safe way
    # every other reader takes one: _borrow_pidfd validates that the record
    # is still in _SPAWNED after the dup, so a concurrent retirement cannot
    # have closed it and handed the number to an unrelated file.
    return _borrow_pidfd(record)


def _leader_alive(fd: int) -> bool:
    """Is the process behind this borrowed leader handle still there?

    Signal 0 delivers nothing and checks everything: it raises
    ProcessLookupError once the leader has been reaped, which is the only
    moment its session id can begin to move."""
    try:
        signal.pidfd_send_signal(fd, 0)
        return True
    except Exception:
        return False


def _provably_ours(pid: int, deadline: Optional[float] = None,
                   clock: _Clock = _REAL_CLOCK) -> bool:
    """The authorization every destructive path asks: can this bridge
    PROVE it started `pid`?

    Two independent signals, consulted in that order:

    * the LSP_BRIDGE_OWNER stamp in the process environment, which is
      authoritative in both directions when it can be read at all;
    * failing that, session continuity.

    The session marker is consulted ONLY when the environ probe answered
    None -- unreadable -- and never when it answered False. A readable
    environment that does not carry the stamp is positive evidence the
    process is not ours, and a marker that could override it would widen
    "ours" from a stamp we placed to a number we once used.

    There is deliberately NO "proved earlier" shortcut here. One was
    written -- a frozen set of (pid, start_ticks) pairs captured while the
    leader was alive -- and removed on review: start ticks have 100 Hz
    granularity, so an opaque process inheriting the PID inside the same
    tick satisfied the pair and was signalled (Codex adversarial review,
    High). Authorization proven at one moment and spent at another needs
    an exact HANDLE, and a handle is not something a predicate taking a
    bare pid can hold -- which is why the capture that needs it signals
    through its own pidfd in force_kill_spawned instead of asking here."""
    answer = _carries_our_owner_id(pid, deadline=deadline, clock=clock)
    if answer is True:
        return True
    if answer is not None:
        return False                # readable and unstamped: not ours
    return _session_marks_ours(pid, _session_of(pid))


def _provably_ours_and_gen(pid: int, gen: Optional[str],
                           deadline: Optional[float] = None,
                           clock: _Clock = _REAL_CLOCK) -> tuple[bool, bool]:
    """_provably_ours and _carries_gen_id from ONE environment traversal.

    Identical answers to asking them separately, including the rule that
    session continuity is consulted ONLY when the owner probe read
    nothing at all. What changes is the cost: the group sweep's
    post-pidfd recheck asked both back to back, so one many-threaded
    candidate paid two full task walks for a single authorization
    (section 24).

    The second element is True when no generation was asked for, so a
    caller can consume it unconditionally."""
    owner, generation = _read_env_markers(pid, gen, deadline=deadline,
                                          clock=clock)
    if owner is True:
        ours = True
    elif owner is not None:
        ours = False                # readable and unstamped: not ours
    else:
        ours = _session_marks_ours(pid, _session_of(pid))
    return ours, (True if gen is None else generation is True)


def _signal_recorded(target, sig: int) -> bool:
    """Send `sig` to a RECORDED child through its pinned identity.

    `Popen.terminate()` / `.kill()` look identity-safe and are not: on
    POSIX they end in os.kill(self.pid, ...), and a Popen is not a pidfd.
    The reader thread calls proc.poll() concurrently, so the leader can
    be reaped between the caller's liveness check and the signal, leaving
    a bare number that may already belong to somebody else (Codex
    adversarial review, High). Routing through the record fixes that:
    with a pidfd the signal is exact, and without one the start-time
    check is re-run immediately before signalling. Fails closed -- an
    unrecorded pid gets no signal from here.

    `target` is a _SpawnRecord whenever the caller has one, because a
    PID does not name a generation: the respawn path can leave a stale
    record ahead of a live replacement carrying the same number, and a
    scan that stops at the first match then signals the dead one, reports
    failure, and never reaches the replacement -- which survives the
    shutdown that thought it had asked (Codex adversarial review, High).
    A bare number is accepted only when exactly ONE record answers to it;
    an ambiguous number fails closed."""
    if isinstance(target, _SpawnRecord):
        candidates = [e for e in list(_SPAWNED) if e is target]
    else:
        candidates = [e for e in list(_SPAWNED) if e.pid == target]
        if len(candidates) > 1:
            # Two generations share this number and the caller did not
            # say which. Guessing is how the live one gets missed.
            return False
    for entry in candidates:
        pid = entry.pid
        ticks = entry.ticks
        # FAIL CLOSED on absent identity. `ticks is None` means the start
        # time could not be read when the child was recorded -- reachable
        # under the FD exhaustion that also causes post-spawn setup
        # failure -- and treating that as permission to signal a bare
        # number is backwards: no evidence is a reason not to act (Codex
        # adversarial review, High). The recorded pidfd is NOT a way
        # around that: it belongs to _retire_spawn, which can close it
        # between this snapshot and the send, so a graceful stop is the
        # last place to reach for a descriptor somebody else may have
        # just handed to an unrelated file.
        # EXACT first. A duplicate of the record's own pidfd identifies
        # the process itself, so it needs no start time and cannot be
        # redirected by a recycle -- including one that lands on the same
        # clock tick, which is a real shape rather than a theoretical
        # one: field 22 has 100 Hz granularity here, and a reviewer
        # measured 32 concurrent spawns sharing seven values (Codex
        # adversarial review, High).
        borrowed = _borrow_pidfd(entry)
        if borrowed is not None:
            try:
                signal.pidfd_send_signal(borrowed, sig)
                return True
            except (ProcessLookupError, OSError):
                return False
            finally:
                try:
                    os.close(borrowed)
                except Exception:
                    pass
        if entry.fd is not None:
            # The dup failed under FD exhaustion. A numeric fallback would
            # hand back the equal-tick recycle the descriptor rules out,
            # and refusing outright leaves the server running -- which the
            # health-restart path then compounds by publishing a
            # replacement over it (Codex adversarial review, High). So
            # CLAIM the original: a successful remove transfers ownership
            # from _retire_spawn to us, and needs no new descriptor.
            try:
                _SPAWNED.remove(entry)
            except ValueError:
                return False            # another thread owns it now
            try:
                signal.pidfd_send_signal(entry.fd, sig)
                return True
            except (ProcessLookupError, OSError):
                return False
            finally:
                try:
                    os.close(entry.fd)
                except Exception:
                    pass
                entry.fd = None
        # FAIL CLOSED on absent identity. `ticks is None` means the start
        # time could not be read when the child was recorded, and nothing
        # else here identifies the NUMBER.
        #
        # The recorded pidfd is emphatically not that evidence. It
        # guarantees the DESCRIPTOR never retargets, which is a different
        # claim: the kernel returns the number to its allocator in
        # free_pid(), reached from detach_pid() when the task is reaped,
        # however many struct pid references survive. So a record left
        # behind after the reader thread reaped its child through
        # Popen.poll() can name a number that now belongs to a stranger,
        # and treating record-presence as proof would SIGTERM them (Codex
        # adversarial review, High). A graceful stop of ONE named server
        # has no safe fallback, so it refuses.
        if ticks is None:
            return False
        # The record must STILL be present at signal time, checked after
        # our own pidfd is open: retirement is what closes the recorded
        # descriptor, so its absence means another thread already
        # confirmed this child dead and nothing here should signal.
        still = (lambda p, a=entry: (
            a in _SPAWNED and proc_start_ticks(p) == ticks))
        own = _own_pidfd(pid, still)
        if own is not None:
            try:
                signal.pidfd_send_signal(own, sig)
                return True
            except (ProcessLookupError, OSError):
                return False
            finally:
                try:
                    os.close(own)
                except Exception:
                    pass
        # No descriptor could be obtained for a record that never had
        # one. The remaining option is a bare number, which section 23
        # rules out on any host meeting this module's documented runtime
        # -- see _kill_verified for why the 3.8 justification does not
        # hold. Graceful stops already failed closed on absent identity;
        # this closes the last path that did not.
        if not _bare_pid_signal_allowed():
            return _refuse_bare_pid_signal(pid, sig)
        try:
            if not still(pid):
                return False
            os.kill(pid, sig)
            return True
        except Exception:
            return False
    return False


def _escalate_recorded(target, is_dead, sigs=None,
                       deadline: Optional[float] = None,
                       wait_after_last: bool = False) -> bool:
    """Escalate a recorded child to death through ONE pinned handle.

    Sends each signal in `sigs` (default SIGTERM then SIGKILL) through a
    SINGLE descriptor held across the whole escalation, polling `is_dead`
    BETWEEN them and stopping as soon as it answers True.

    Nothing is waited for after the LAST signal unless `wait_after_last`
    asks for it, because for a `Popen` child that wait IS a reap -- and a
    caller whose next act is a group sweep needs the leader unreaped,
    since a same-session hardened descendant is authorized by that leader
    still being there (section 23).

    Holding one handle is the entire point. `_signal_recorded` acquires
    its own per call, and under descriptor pressure its acquisition ends
    in the CLAIM path -- which removes the record from `_SPAWNED` and
    closes its descriptor to send that one signal. Two calls in sequence
    therefore spend the only identity the child had on the SIGTERM, and
    the SIGKILL that follows finds no record and fails closed, leaving a
    server that ignores SIGTERM alive exactly when descriptors are scarce
    (Codex adversarial review, High -- reproduced). Fails closed the same
    way `_signal_recorded` does: no handle, no signal, and the refusal is
    counted rather than downgraded to a bare number."""
    if sigs is None:
        sigs = (signal.SIGTERM, signal.SIGKILL)
    if isinstance(target, _SpawnRecord):
        candidates = [e for e in list(_SPAWNED) if e is target]
    else:
        candidates = [e for e in list(_SPAWNED) if e.pid == target]
        if len(candidates) > 1:
            return False
    for entry in candidates:
        pid, ticks = entry.pid, entry.ticks
        own = _borrow_pidfd(entry)
        claimed = False
        if own is None and entry.fd is not None:
            try:
                _SPAWNED.remove(entry)
            except ValueError:
                continue                # another thread owns it now
            own, claimed = entry.fd, True
        elif own is None:
            if ticks is None:
                continue                # no identity at all; fail closed
            still = (lambda p, a=entry, t=ticks: (
                a in _SPAWNED and proc_start_ticks(p) == t))
            own = _own_pidfd(pid, still)
        if own is None:
            continue
        sent = False
        try:
            for idx, sig in enumerate(sigs):
                if is_dead():
                    break
                try:
                    signal.pidfd_send_signal(own, sig)
                    sent = True
                except (ProcessLookupError, OSError):
                    break
                # Wait only BETWEEN signals, never after the last one.
                # The wait is a poll of the leader, and polling reaps it
                # -- which destroys the identity a same-session hardened
                # descendant's ownership proof depends on (section 23:
                # authorization is the recorded leader still being
                # there). The caller collects the group FIRST and reaps
                # the leader after; polling here reversed that and let a
                # surviving old-generation worker read as a clean group
                # (Codex re-adversarial, High).
                last = idx + 1 >= len(sigs)
                if deadline is None or (last and not wait_after_last):
                    continue
                # Each signal gets a SHARE of what is left, never all of
                # it: spending the whole budget waiting out a SIGTERM the
                # process ignored is how the SIGKILL that would have
                # worked never gets sent.
                remaining = max(0.0, deadline - time.monotonic())
                until = time.monotonic() + remaining * (1.0 if last else 0.5)
                while not is_dead() and time.monotonic() < until:
                    time.sleep(min(0.02, max(0.0, until - time.monotonic())))
        finally:
            try:
                os.close(own)
            except Exception:
                pass
            if claimed:
                entry.fd = None         # nobody else may close it now
        return sent
    return False


def _kill_verified(pid: int, fd: Optional[int], still_ours) -> bool:
    """SIGKILL `pid`, but only while it is provably still the process we
    meant. Returns True if the signal was sent.

    With a pidfd this is exact and raceless. Without one there is no
    raceless option at all: re-verifying identity immediately before
    signalling still leaves a window in which the process exits, is
    reaped, and its number is handed to somebody else.

    Section 20 accepted that window because the pidfd-less host was
    believed to be supported -- scripts/setup.sh floors python3 at 3.8 and
    os.pidfd_open arrived in 3.9. It is not: this module documents
    "Stdlib-only (Python 3.10+)" and uses 3.10 behaviour, so the
    configuration the fallback existed for cannot import it. Section 23
    therefore decides the question the fallback left open -- destructive
    signalling does not name a target by number -- and refuses instead,
    unless an out-of-profile host explicitly opts back in. A refusal
    leaks a child, which is recoverable and now reported; a mis-aimed
    SIGKILL is neither."""
    if fd is not None:
        try:
            signal.pidfd_send_signal(fd, signal.SIGKILL)
            return True
        except (ProcessLookupError, OSError):
            return False
    if not _bare_pid_signal_allowed():
        return _refuse_bare_pid_signal(pid, signal.SIGKILL)
    try:
        if not still_ours(pid):
            return False
        os.kill(pid, signal.SIGKILL)
        return True
    except Exception:
        return False


def _read_env_markers(
    pid: int, gen: Optional[str] = None,
    deadline: Optional[float] = None,
    clock: _Clock = _REAL_CLOCK,
) -> tuple[Optional[bool], Optional[bool]]:
    """`(owner, generation)` from ONE traversal of `pid`'s environments.

    Each answer is True, False when an environment was read and did not
    carry that marker, or None when nothing authoritative could be read.
    `generation` is always None when no `gen` was asked for.

    Both markers live in the SAME environment -- they are placed together
    by the env passed to Popen -- so asking for them separately read the
    same file twice. A group sweep asked for both in its pre-filter and
    then again in its post-pidfd recheck, so an unstamped many-threaded
    candidate could pay FOUR full task walks, one read per task each,
    all charged to a teardown budget (section 24).

    None is per-marker, and it is NOT merely "every read failed". A walk
    cut short by the deadline has proven nothing about the tasks it never
    reached, so every UNRESOLVED marker stays None even when an earlier
    task WAS readable: collapsing that partial result to False would stop
    _provably_ours consulting session continuity, and a dead-leader /
    live-worker child would then be skipped and survive teardown under
    exactly the load that truncated the walk (Codex design review, High).

    For the same reason the walk does not stop at the first task carrying
    the owner stamp while the generation is still unresolved -- an early
    return would answer one marker from a read environment and downgrade
    the other to a guess."""
    owner_marker = b"LSP_BRIDGE_OWNER=" + _OWNER_ID.encode()
    gen_marker = (b"LSP_BRIDGE_GEN=" + gen.encode()
                  if gen is not None else None)

    def expired() -> bool:
        return deadline is not None and clock.now() >= deadline

    if expired():
        return None, None               # budget spent before we even read
    try:
        with open(f"/proc/{pid}/environ", "rb") as fh:
            env = fh.read().split(b"\0")
        return (owner_marker in env,
                None if gen_marker is None else gen_marker in env)
    except Exception:
        pass
    # Only now walk the threads. The leader environ answers for almost
    # every process, and reading EVERY task environment before trusting it
    # made an unstamped multithreaded process (a browser, a JVM, a build
    # worker) cost one read per thread -- paid out of the shutdown budget,
    # for a process we were about to reject anyway (Codex post-commit perf).
    #
    # Read per-TASK as well as per-process, for the same reason the
    # liveness check is thread-group-aware: once the thread-group leader
    # has exited, /proc/PID/environ answers EACCES while a surviving
    # worker's /proc/PID/task/<tid>/environ still carries the stamp.
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except Exception:
        return None, None
    owner: Optional[bool] = None
    generation: Optional[bool] = None
    read_any = False
    for tid in tids:
        if expired():
            # Truncated: a resolved positive stands, everything else is
            # still UNKNOWN rather than negative.
            return (True if owner is True else None,
                    True if generation is True else None)
        try:
            with open(f"/proc/{pid}/task/{tid}/environ", "rb") as fh:
                data = fh.read().split(b"\0")
        except Exception:
            continue
        read_any = True
        if owner_marker in data:
            owner = True
        if gen_marker is not None and gen_marker in data:
            generation = True
        if owner is True and (gen_marker is None or generation is True):
            break                       # every asked marker answered
    if not read_any:
        # Every task refused to be read: nothing was proven either way.
        return None, None
    return (owner is True,
            None if gen_marker is None else generation is True)


def _carries_our_owner_id(
    pid: int, deadline: Optional[float] = None,
    clock: _Clock = _REAL_CLOCK,
) -> Optional[bool]:
    """True if this process was started by THIS bridge process, False if
    an environment was read and did NOT carry the stamp, and None when no
    environment could be read at all.

    The three answers are NOT two-plus-a-caching-detail. False is an
    authoritative rejection: an environment was read and did not carry the
    stamp, and nothing may override it. None is UNRESOLVED, and it is the
    only answer that may go on to reach a separate proof -- _provably_ours
    and _ProcSnapshot.markers consult session continuity on None and never
    on False, which is what lets a child whose /proc entry became
    unreadable still be collected. Neither answer authorizes destructive
    signalling by itself; that always takes a live re-read behind a pidfd.

    The distinction also keeps a CACHE from storing a transient read
    failure as an authoritative negative: one racing read would otherwise
    suppress a live child of ours in every later round of a sweep, and the
    loop would reach two empty scans and exit over it (Codex adversarial
    review, Medium).

    Checks LSP_BRIDGE_OWNER, minted per process, NOT the inheritable
    LSP_BRIDGE_RUN_ID: a run id shared across a whole harness tree makes
    "ours" mean "anything in the tree", and a reap acting on that answer
    kills its own siblings. Not-provably-ours is not-killed.

    `deadline` bounds the task walk. Without it a single unstamped
    thousand-thread process could spend a whole shutdown budget proving
    something we were about to reject anyway (Codex adversarial review,
    High); a walk cut short answers None, not False, because a partial
    read has proven nothing.

    Read per-TASK as well as per-process, for the same reason the
    liveness check is thread-group-aware: once the thread-group leader
    has exited, /proc/PID/environ answers EACCES while a surviving
    worker's /proc/PID/task/<tid>/environ still carries the stamp. Asking
    only the leader therefore disowned exactly the dead-leader,
    live-worker process the previous fix had just taught the scan to keep
    (Codex adversarial review, Medium, reproduced).

    The traversal itself lives in _read_env_markers, which answers this
    question and the generation question from the same read."""
    return _read_env_markers(pid, None, deadline=deadline, clock=clock)[0]


class _ProcSnapshot:
    """One walk of /proc, reusable by every pass of a sweep.

    The cheap facts -- state, ppid, pgrp, start ticks -- come from a
    SINGLE read of each /proc/<pid>/stat. The expensive one, whether the
    process carries this bridge's owner stamp, is read lazily and cached.

    There is deliberately NO cheap pre-filter on the /proc directory's
    owner. It looks like a free authoritative negative and is not one:
    the kernel reassigns /proc/<pid> to root when a process calls
    PR_SET_DUMPABLE(0) or execs a setuid binary, so a hardened child of
    ours would be disowned before its stamp was ever read (Codex
    adversarial review, High). Cost is bounded by the caller's deadline
    and by caching, not by guessing.

    Two rules keep the cache from ever becoming an authorization:

    * It is keyed by (pid, start_ticks), NEVER by pid alone. A process
      that exits between rounds can have its number handed to a stranger,
      and a pid-keyed answer would let that stranger inherit the previous
      occupant's ownership (Codex design review, High).
    * Only an AUTHORITATIVE answer is stored. _carries_our_owner_id
      returns None when nothing could be read, and caching that as a
      negative would suppress a live child of ours for the rest of the
      sweep on one racing read (Codex adversarial review, Medium).

    And in every case the cache only NARROWS the candidate set --
    authorization to signal is a live re-read after the pidfd is open."""

    __slots__ = ("procs", "_ours", "_gen")

    def __init__(
        self,
        procs: list[tuple[int, Optional[int], Optional[int],
                          Optional[int], Optional[int]]],
        ours_cache: Optional[dict] = None,
        gen_cache: Optional[dict] = None,
    ) -> None:
        self.procs = procs
        # A caller looping over several snapshots shares one cache: the
        # (pid, ticks) key pins identity, and a live process cannot gain
        # a stamp it did not inherit at exec, so a stored answer stays
        # true for as long as that process does.
        self._ours: dict[tuple[int, int], bool] = (
            ours_cache if ours_cache is not None else {})
        # The generation answer earns the same treatment for the same
        # reason, keyed by (pid, ticks, gen) because one pid can be asked
        # about two generations across a respawn.
        self._gen: dict[tuple[int, int, str], bool] = (
            gen_cache if gen_cache is not None else {})

    def ours(self, pid: int, ticks: Optional[int],
             deadline: Optional[float] = None,
             sid: Optional[int] = None,
             clock: _Clock = _REAL_CLOCK) -> bool:
        # No start time means no cache KEY -- the pair is what pins
        # identity, and a bare pid would let a recycled number inherit
        # the previous occupant's answer. Such a row is probed afresh
        # every round rather than dropped (Codex adversarial review,
        # High).
        key = (pid, ticks) if ticks is not None else None
        if key is not None and self._ours.get(key):
            return True
        answer = _carries_our_owner_id(pid, deadline=deadline, clock=clock)
        # The environ probe could not read ANYTHING. Before giving up on
        # a candidate, ask the one ownership question that survives an
        # unreadable environment -- session continuity. Consulted only on
        # None, never on a readable-and-unstamped False.
        if answer is None and _session_marks_ours(pid, sid):
            # NOT cached. Its truth depends on the recorded leader still
            # being alive, which a later round can falsify; the cache
            # stores only facts that cannot change under a live process.
            return True
        # POSITIVES only. Ownership is not immutable for a (pid, ticks)
        # pair the way it looks: the stamp reaches the child through the
        # env passed to Popen, so a probe landing between fork and exec
        # reads an UNSTAMPED environment for a process that is about to
        # carry the stamp -- same pid, same start ticks. Caching that
        # False meant every later round reused it and the newly exec'd
        # server was never signalled (Codex adversarial review, Medium).
        # A negative is therefore re-asked each round; it is also the
        # cheap case, since an unreadable process answers None and was
        # never cacheable anyway.
        if answer is True and key is not None:
            self._ours[key] = True
        return answer is True

    def markers(self, pid: int, ticks: Optional[int], gen: Optional[str],
                deadline: Optional[float] = None,
                sid: Optional[int] = None,
                clock: _Clock = _REAL_CLOCK) -> tuple[bool, bool]:
        """`(ours, matches_gen)` for one candidate from ONE traversal.

        The group filter needs both answers about the same process, and
        asking ours() and then _carries_gen_id() read the same
        environment twice. Every rule ours() enforces still holds here:
        the cache is keyed by (pid, start_ticks) so a recycled number
        cannot inherit an answer, only POSITIVES are stored because a
        probe landing between fork and exec reads an unstamped
        environment for a process that is about to carry the stamp, and
        the session fallback is consulted only on an unreadable owner
        probe and never cached (its truth depends on a leader that a
        later round can find gone).

        `matches_gen` is True when no generation was asked for."""
        key = (pid, ticks) if ticks is not None else None
        gkey = ((pid, ticks, gen)
                if (key is not None and gen is not None) else None)
        have_ours = key is not None and self._ours.get(key) is True
        have_gen = gkey is not None and self._gen.get(gkey) is True
        if have_ours and (gen is None or have_gen):
            return True, True
        owner, generation = _read_env_markers(pid, gen, deadline=deadline,
                                              clock=clock)
        ours = have_ours or owner is True
        # Consulted only on None, never on a readable-and-unstamped
        # False, and never cached -- see ours().
        if not ours and owner is None and _session_marks_ours(pid, sid):
            ours = True
        if owner is True and key is not None:
            self._ours[key] = True
        if generation is True and gkey is not None:
            self._gen[gkey] = True
        return ours, (True if gen is None
                      else have_gen or generation is True)


def _proc_snapshot(deadline: Optional[float] = None,
                   ours_cache: Optional[dict] = None,
                   clock: _Clock = _REAL_CLOCK) -> _ProcSnapshot:
    """One row per live process on the host:
    `(pid, ppid, pgrp, session, start_ticks)`.

    Only `pid` is guaranteed non-None. A row whose stat could not be read
    or parsed is KEPT with the other four set to None -- see the comment
    on the append below for why dropping it is a leak -- so every consumer
    and every test fake must unpack five OPTIONAL fields.

    One listdir and one stat read per process, shared by every pass that
    needs it. The sweep used to walk /proc once per recorded language
    server -- inside _reap_group, itself looped -- and then once more for
    the stamp-wide pass, and each candidate cost three separate reads of
    the SAME stat file, because _pgrp_of, _is_zombie and
    proc_start_ticks each opened it independently. On a host with a few
    hundred processes that is O(records x processes x 3) charged to a
    budget meant to bound the whole teardown.

    Fully-zombie processes are dropped here, by the same task-aware rule
    _is_zombie applies: a leader sitting in Z while its workers run is
    ALIVE, and filtering on the leader's state alone would drop exactly
    the pthread_exit shape the thread-group reap coverage exists for
    (Codex design review, High). Anything unreadable or racing is kept,
    for the same reason it is kept there -- a redundant signal costs
    nothing and a skipped live process is the leak."""
    procs: list[tuple[int, Optional[int], Optional[int], Optional[int],
                      Optional[int]]] = []
    try:
        entries = os.listdir("/proc")
    except Exception:
        return _ProcSnapshot(procs, ours_cache)
    for name in entries:
        if deadline is not None and clock.now() >= deadline:
            break
        if not name.isdigit():
            continue
        pid = int(name)
        fields = _stat_fields(pid)
        state: Optional[str] = None
        ppid: Optional[int] = None
        pgrp: Optional[int] = None
        sid: Optional[int] = None
        ticks: Optional[int] = None
        if fields:
            try:
                state = fields[0]
                ppid = int(fields[1])
                pgrp = int(fields[2])
                # Field 6. Read from the SAME stat line the other three
                # come from, so the second ownership signal costs no
                # extra procfs read at all.
                sid = int(fields[3])
                ticks = int(fields[19])
            except (IndexError, ValueError):
                state = ppid = pgrp = sid = ticks = None
        # A pid whose stat could not be read or parsed is KEPT, with its
        # fields left None. Dropping it silently removed an owner-stamped
        # descendant from the only broad sweep there is now that the group
        # and stamp passes are one -- and the read most likely to fail is
        # the one taken during an emergency teardown under FD exhaustion
        # (Codex adversarial review, High). Group filtering needs pgrp and
        # the ownership cache needs ticks, so both simply decline to act
        # on a degraded row; the stamp-wide pass still probes and signals
        # it through a caller-owned pidfd.
        if state == "Z" and _is_zombie(pid):
            continue
        procs.append((pid, ppid, pgrp, sid, ticks))
    return _ProcSnapshot(procs, ours_cache)


def _carries_gen_id(pid: int, gen: str,
                    deadline: Optional[float] = None,
                    clock: _Clock = _REAL_CLOCK) -> bool:
    """True if this process was started by one specific SPAWN.

    The owner stamp answers "did this bridge start it", which two
    generations sharing a PID both satisfy. This narrower question is
    what a group sweep needs, because a process group is addressed by a
    number the replacement may now own (Codex adversarial review, High).
    Unreadable is False, as everywhere else: not-provably-ours is
    not-killed. `deadline` bounds the task walk for the same reason it
    bounds the owner probe: one many-threaded replacement could otherwise
    hold a teardown open past its budget inside a single call.

    The tri-state _read_env_markers returns is coerced HERE, at the public
    edge, and not inside the traversal: the merged reader has to keep
    "unresolved" distinct from "read and absent" so the owner answer can
    still reach session continuity, while this question has always failed
    closed on both."""
    return _read_env_markers(pid, gen, deadline=deadline,
                             clock=clock)[1] is True


def _group_members(
    pgid: int, exclude: int = -1, deadline: Optional[float] = None,
    snapshot: Optional[_ProcSnapshot] = None, gen: Optional[str] = None,
    clock: _Clock = _REAL_CLOCK,
) -> list[tuple[int, Optional[int], Optional[int]]]:
    """(pid, pidfd) for every live process in process group `pgid` that
    this bridge can prove it started.

    A process group is addressed by a bare number -- the leader's PID --
    and that number stops meaning anything the moment the group empties:
    it can be reused by a group this bridge never created, and a sweep
    that trusts `pgrp == pgid` alone would SIGKILL strangers (Codex
    adversarial review, High). Membership is therefore necessary but NOT
    sufficient; the run-id stamp is what establishes ownership.

    This exists because `killpg` addresses a group by a bare number, and
    that number is the leader's PID -- which stops being a safe address
    the moment the leader exits. The dead-leader case is not academic:
    the reader thread can reap the leader through Popen.poll() while a
    forked helper of its own survives, and a group kill issued after that
    can land on a stranger (Codex adversarial review, High). Enumerating
    the members and signalling each through a captured identity keeps the
    guarantee without ever trusting the number.

    Identity is captured then re-verified, so an entry that changed
    underneath the scan is dropped rather than signalled.

    `snapshot` lets a caller sweeping several groups pay for ONE /proc
    walk instead of one per group; without it this takes its own."""
    members: list[tuple[int, Optional[int], Optional[int]]] = []
    snap = (snapshot if snapshot is not None
            else _proc_snapshot(deadline, clock=clock))
    me = os.getpid()
    for pid, _ppid, pgrp, sid, ticks in snap.procs:
        if deadline is not None and clock.now() >= deadline:
            break
        if pid == exclude or pid == me:
            continue
        if pgrp is None or pgrp != pgid:
            continue        # unreadable stat: not groupable, but the
                            # stamp-wide pass still sees it
        # BOTH stamps from one environment traversal, and one cache entry
        # per answer: asking ours() and then _carries_gen_id() read the
        # same file twice per candidate per round (section 24).
        is_ours, gen_ok = snap.markers(pid, ticks, gen, deadline=deadline,
                                       sid=sid, clock=clock)
        if not is_ours:
            continue
        if not gen_ok:
            continue        # a namesake generation, not this one
        fd = _pidfd_open(pid)
        # Recheck BOTH facts after opening the fd, not just the group: a
        # PID that was replaced between the stamp check and the open would
        # otherwise be captured with a valid pidfd pointing at a stranger.
        # This re-read is LIVE and uncached -- it is the authorization,
        # not a filter -- but it is still ONE traversal for both stamps.
        if fd is not None:
            live_ours, live_gen = _provably_ours_and_gen(
                pid, gen, deadline=deadline, clock=clock)
            if _pgrp_of(pid) != pgid or not live_ours or not live_gen:
                try:
                    os.close(fd)
                except Exception:
                    pass
                continue
        # Carry the start time so the pidfd-less path has an identity to
        # verify at signal time; group membership alone is a reusable
        # number and cannot authorize a kill (Codex adversarial review,
        # High -- the rule this file applies everywhere else).
        members.append((pid, fd, proc_start_ticks(pid)))
    return members


def _reap_predicate(pgid: int, ticks: Optional[int], gen: Optional[str],
                    deadline: Optional[float],
                    clock: _Clock = _REAL_CLOCK):
    """The pidfd-less authorization used by the group reaper.

    Every probe it makes is bounded, and the budget is re-checked AFTER
    the ownership read as well as before: that read walks every task when
    the leader environment is unreadable -- the dead-leader/live-worker
    shape this file goes out of its way to keep -- so the budget can
    expire inside the one call whose answer then authorizes the signal
    (Codex adversarial review, Medium)."""

    def still_ours(p: int) -> bool:
        if deadline is not None and clock.now() >= deadline:
            return False
        if ticks is None or proc_start_ticks(p) != ticks:
            return False
        if _pgrp_of(p) != pgid:
            return False
        # One traversal for both stamps; the budget re-check below is why
        # they are read together rather than in sequence.
        ours, gen_ok = _provably_ours_and_gen(p, gen, deadline=deadline,
                                              clock=clock)
        if not ours or not gen_ok:
            return False
        return not (deadline is not None and clock.now() >= deadline)

    return still_ours


def _reap_group(pgid: int, deadline: Optional[float] = None,
                gen: Optional[str] = None,
                clock: _Clock = _REAL_CLOCK) -> tuple[int, bool]:
    """SIGKILL every live member of a process group we own, by verified
    identity rather than by killpg, REPEATEDLY until the group is empty.

    Returns `(killed, emptied)`. `emptied` is True ONLY when the loop
    reached its two-consecutive-empty-scans fixpoint -- an exit on the
    deadline or the round cap answers False, because neither proves the
    group is gone. Reporting it is what lets a caller stop asking the
    question separately: the group emptiness check ran its own full
    /proc walk before each reap, so a shutdown alternating the two
    nested walks inside walks (section 24).

    One pass is not enough: the enumeration completes before the first
    signal is sent, so a leader that forks a helper in between produces a
    helper that is in no snapshot -- and once the leader dies that helper
    is reparented away and no later search for our own children can find
    it (Codex adversarial review, High). Looping until the group is empty
    closes it, bounded so a pathological forker cannot hold shutdown
    open forever."""
    killed = 0
    empty_scans = 0
    emptied = False
    for _ in range(20):                     # ~ bounded; see deadline below
        if deadline is not None and clock.now() >= deadline:
            break                           # checked BEFORE the scan, not after
        members = _group_members(pgid, deadline=deadline, gen=gen,
                                 clock=clock)
        if not members:
            # TWO consecutive empty scans, not one. A scan snapshots
            # /proc before examining it, so a helper forked after the
            # listdir -- while the leader exits before its own entry is
            # read -- is absent from a single scan that looks clean
            # (Codex adversarial review, Medium). A second look after a
            # scheduling interval sees it.
            # An empty RESULT is not an empty group. _group_members stops
            # where its deadline runs out, so a budget that expired partway
            # returns nothing at all -- and counting that as an empty scan
            # let `emptied` report the group gone over descendants the scan
            # never reached, which is the retirement decision this flag now
            # carries. Incomplete means unknown, and unknown fails toward
            # still-there (Codex adversarial review, Medium; the rule the
            # deleted _group_is_empty preflight held).
            if deadline is not None and clock.now() >= deadline:
                break                       # incomplete, not empty
            empty_scans += 1
            if empty_scans >= 2:
                emptied = True
                break
            clock.sleep(0.02)
            continue
        empty_scans = 0
        for pid, fd, ticks in members:
            if deadline is None or clock.now() < deadline:
                if _kill_verified(
                    pid, fd,
                    _reap_predicate(pgid, ticks, gen, deadline, clock=clock),
                ):
                    killed += 1
            if fd is not None:
                try:
                    os.close(fd)
                except Exception:
                    pass
        if deadline is not None and clock.now() >= deadline:
            break
        clock.sleep(0.02)
    return killed, emptied


def _force_kill_spawned_sweep(settle: float = 1.0,
                              deadline: Optional[float] = None,
                              clock: _Clock = _REAL_CLOCK) -> int:
    """SIGKILL every recorded language server still alive, taking no lock
    that a wedged thread could be holding. Returns how many DISTINCT
    processes were signalled -- a leader reached by both the recorded
    pass and the stamp-wide sweep counts once, where the older two-pass
    shape counted it twice and made the number unusable as a leak count.

    THE SWEEP BODY, and never the entry point: every caller imports the
    single-flight wrapper `force_kill_spawned` below. Two of these
    running at once each take one slot of the two-descriptor reserve and
    neither can complete the proof-plus-target pair one scan holds at
    once (section 25).

    This is the last thing a dying bridge does, and the only reap whose
    success does not depend on the state of the process running it.
    Graceful shutdown acquires per-instance locks, and a termination can
    arrive precisely while a thread holds one -- an interrupted
    initialize() owns _init_lock, which shutdown() also wants. A cleanup
    that waits for such a lock waits forever and the children outlive the
    bridge.

    Two things it will NOT do. It will not signal a process it cannot
    prove is ours: a recycled PID belongs to somebody else, and killing
    its process group would be a far worse bug than the leak this
    function exists to prevent. And it will not conclude "nothing to do"
    while a spawn is in flight -- it waits (bounded by `settle`) for the
    Popen-to-record window to close first, because a snapshot taken
    inside that window is empty for a reason that has nothing to do with
    whether a child exists."""
    waited = 0.0
    while _SPAWN_INFLIGHT > 0 and waited < settle:
        if deadline is not None and clock.now() >= deadline:
            break
        clock.sleep(0.02)
        waited += 0.02

    signalled: set[int] = set()
    me = os.getpid()
    me0 = me

    def signal_recorded_leaders() -> None:
        """Signal every RECORDED leader -- the cheap pass, and the one
        that is deliberately NOT deadline-gated.
        Signalling a recorded leader through its pinned identity is a
        couple of syscalls; there are at most a handful of records, and
        these are the processes we most certainly own. Doing this
        interleaved with the expensive per-record group scans meant a
        small budget was spent entirely on the FIRST record's /proc
        walks, and every later language server was skipped and survived
        (Codex adversarial review, High). Cheap and certain first;
        expensive and speculative with what is left."""
        for entry in list(_SPAWNED):
            pid = entry.pid
            ticks = entry.ticks
            # `entry in _SPAWNED`, checked at SIGNAL time rather than scan
            # time, means no retirement has run: nobody has yet confirmed
            # this child dead, so the record is still worth acting on. It
            # is a necessary condition and NOT an identity -- the recorded
            # pidfd keeps the DESCRIPTOR from retargeting but does not
            # reserve the NUMBER, which free_pid() returns to the
            # allocator when the task is reaped no matter how many struct
            # pid references remain (Codex adversarial review, High).
            #
            # Identity is therefore the start time, or -- when the record
            # never got one -- the owner stamp, which is enough HERE and
            # only here: this sweep kills everything this bridge started,
            # so "provably one of ours" is exactly the authorization it
            # needs. _signal_recorded, which stops ONE named server, fails
            # closed. The owner-stamp fallback is NOT cheap -- it can read
            # one environment per task -- so unlike the rest of this pass
            # it is deadline-bound, checked before and after the probe
            # exactly as the stamp-wide pass does. Leaving it ungated let
            # a 200ms probe signal 150ms past a 50ms budget (Codex
            # adversarial review, High).
            def still(p, t=ticks, a=entry, _dl=deadline):
                if a not in _SPAWNED:
                    return False
                # NECESSARY, never SUFFICIENT. This used to return True on
                # a tick match alone, and _own_pidfd then pinned and
                # SIGKILLed whatever answered to the number -- so a
                # recycle landing in the same 100 Hz tick killed a
                # stranger, by DEFAULT rather than only under the bare-PID
                # opt-in. It is the one place this section had not yet
                # applied its own rule that a start time is not an
                # identity (Codex adversarial review, High).
                if t is not None and proc_start_ticks(p) != t:
                    return False
                if _dl is not None and clock.now() >= _dl:
                    return False
                ok = _provably_ours(p, deadline=_dl, clock=clock)
                if _dl is not None and clock.now() >= _dl:
                    return False
                return ok
            # A duplicate of the record's own descriptor is exact; the
            # predicate below is only for records that never got one.
            # _borrow_pidfd already validated membership after its dup; a
            # second check here could only drop the descriptor on the
            # floor without closing it (Codex adversarial review, Medium).
            own = _borrow_pidfd(entry)
            claimed = False
            if own is None and entry.fd is not None:
                # The dup failed -- and it fails under exactly the FD
                # exhaustion that makes this sweep urgent. Falling back to
                # a numeric signal would restore the equal-tick recycle
                # the descriptor rules out, and refusing outright would
                # orphan the child at the worst possible moment (Codex
                # adversarial review, High then Medium). So CLAIM it
                # instead: a successful remove transfers ownership of the
                # original descriptor from _retire_spawn to this caller,
                # which is the one operation that needs no new file
                # descriptor at all.
                try:
                    _SPAWNED.remove(entry)
                except ValueError:
                    continue            # another thread owns it now
                own, claimed = entry.fd, True
            elif own is None:
                own = _own_pidfd(pid, still)
            try:
                if own is not None:
                    try:
                        signal.pidfd_send_signal(own, signal.SIGKILL)
                        signalled.add(pid)
                    except (ProcessLookupError, OSError):
                        pass
                elif _kill_verified(pid, None, still):
                    signalled.add(pid)
            finally:
                # Closed either way: a borrowed dup is ours by
                # construction, and a CLAIMED original is ours because the
                # remove that produced it took ownership from
                # _retire_spawn.
                if own is not None:
                    try:
                        os.close(own)
                    except Exception:
                        pass
                    if claimed:
                        entry.fd = None     # nobody else may close it now

    # ONE /proc walk per round, shared by both scans that need one.
    #
    # These were three separate loops -- session-owned descendants, then
    # the recorded leaders, then the stamp-wide sweep -- and the first and
    # third each took their OWN snapshots. In a production teardown, where
    # every _record_spawn populates _OWNED_SESSIONS, that cost at least
    # two extra full enumerations, and a persistent session target could
    # spend whole rounds before detached owner-stamped helpers were
    # considered at all (section 24).
    #
    # The ORDER the merge has to preserve is descendants-before-leaders.
    # Session ownership is authorized by the recorded leader still being
    # there -- its presence is what stops the kernel recycling the session
    # number -- so once the leaders die, the reader thread reaps one
    # through Popen.poll(), the leader pidfd answers ESRCH, and exactly
    # the opaque descendant this collects is rejected (Codex adversarial
    # review, High, reproduced). Two things follow, and both are load-
    # bearing:
    #
    #   * the leaders fire the INSTANT the session scan reaches its own
    #     two-consecutive-empty-rounds fixpoint, before this round's
    #     stamped work. Running the stamped scan in between would leave
    #     the leader alive across a walk that can read one environment per
    #     task, and a descendant forked in that window is in no snapshot
    #     and loses its proof the moment the leader is reaped (Codex
    #     design review, High);
    #   * until they have fired, the stamped scan SKIPS the recorded
    #     session leaders themselves -- they carry the stamp, so it would
    #     otherwise kill the very handle every session proof depends on.
    #
    # If the deadline or the round cap arrives first, the leaders are
    # signalled unconditionally between the two phases, preserving that
    # pass's deliberately ungated behavior.
    owner_cache: dict = {}
    leaders_done = False
    session_empty = 0

    def fire_leaders() -> None:
        nonlocal leaders_done
        if not leaders_done:
            signal_recorded_leaders()
            leaders_done = True

    def stamped_round(snap) -> bool:
        """Signal every OTHER live process carrying this run's stamp:
        descendants of a recorded leader, a helper reparented away after
        its leader died, one that called setsid and left both the group
        and the child list, and a spawn interrupted before it could write
        a record. Returns whether the scan found anything.

        This and the per-leader group sweep were once separate passes
        asking the same question: _group_members requires the owner stamp
        exactly as this scan does and adds only a pgrp filter, so the
        group set is a strict SUBSET of the stamped set."""
        # Deadline-checked, NOT a comprehension. Every snap.ours() miss
        # can cost an environ read plus one per task, so a few hundred
        # unstamped processes are enough to overrun the budget between
        # the walk and the first signal -- the enumeration was bounded
        # and the ownership probing behind it was not (Codex adversarial
        # review, High).
        targets: list[tuple[int, Optional[int]]] = []
        for pid, _ppid, _pgrp, sid, ticks in snap.procs:
            if deadline is not None and clock.now() >= deadline:
                break
            if pid == me:
                continue
            # The recorded session leaders are the handles every session
            # proof depends on, and they carry this run's stamp, so until
            # they have been signalled deliberately this scan must not
            # take them (section 24).
            if not leaders_done and sid == pid and pid in _OWNED_SESSIONS:
                continue
            if snap.ours(pid, ticks, deadline=deadline, sid=sid,
                         clock=clock):
                targets.append((pid, ticks))
        for pid, _ticks in targets:
            if deadline is not None and clock.now() >= deadline:
                break
            # The snapshot's ownership answer only NARROWED the candidate
            # set. Authorization is this live re-read, taken after the
            # pidfd pins the process, so a PID recycled between the walk
            # and the signal cannot inherit the previous occupant's stamp
            # (Codex design review, High).
            #
            # It carries the deadline itself, and re-checks it: the loop's
            # check happens BEFORE _own_pidfd opens, and the predicate
            # then runs twice -- once inside _own_pidfd and again on the
            # pidfd-less fallback -- so a slow procfs read could authorize
            # a signal well past a budget the caller is holding to (Codex
            # adversarial review, High; a 200ms probe against a 50ms
            # deadline still signalled).
            def still(p, _dl=deadline):
                if _dl is not None and clock.now() >= _dl:
                    return False
                # _provably_ours, not the raw stamp probe: a descendant
                # that made its /proc entry unreadable answers None to
                # the stamp forever, so authorizing on the stamp alone
                # would let the snapshot nominate a target the live
                # re-read could never confirm -- the leak this section
                # exists to close, reintroduced one layer down.
                ok = _provably_ours(p, deadline=_dl, clock=clock)
                # Re-checked AFTER the probe as well. Checking only
                # before it is what makes a slow read dangerous rather
                # than merely slow: the leader's environ read is not
                # interruptible, so the budget can expire inside the one
                # call whose answer then authorizes the signal.
                if _dl is not None and clock.now() >= _dl:
                    return False
                return ok
            own = _own_pidfd(pid, still)
            try:
                if _kill_verified(pid, own, still):
                    signalled.add(pid)
            finally:
                if own is not None:
                    try:
                        os.close(own)
                    except Exception:
                        pass
        return bool(targets)

    # TWO fixpoint phases, each taking ONE /proc walk per round.
    #
    # They cannot share a fixpoint, and that is the whole ordering
    # constraint. The session scan is only meaningful BEFORE the recorded
    # leaders die -- their liveness is what stops the kernel recycling the
    # session number, so once they are reaped the opaque descendants it
    # collects can no longer be proven ours (Codex adversarial review,
    # High, reproduced). The stamped scan is only conclusive AFTER they
    # die, because a leader can fork an owner-stamped helper on its way
    # out and no pre-leader snapshot can contain it.
    #
    # So: phase A runs the session scan to its own two-empty-round
    # fixpoint and SHARES each round's snapshot with the stamped scan --
    # sharing is the point of this section, and a detached owner-stamped
    # helper must not have to wait for the session fixpoint to be
    # considered at all. Phase B then runs the stamped fixpoint over
    # post-leader snapshots only. Phase A's stamped results kill what they
    # find but never count toward phase B's fixpoint; mixing the two
    # accounting sets is what let a pre-leader empty scan plus one stale
    # one reach the two-empty bound and exit over a helper the leader
    # forked on its way out (Codex adversarial review, High).
    #
    # Every exit from phase A -- fixpoint, sessions retired underneath it,
    # deadline, round cap -- funnels through the same fire_leaders() and
    # on into phase B, so no path can signal the leaders and then skip the
    # post-leader scans (Codex adversarial + perf review, High).
    if not _OWNED_SESSIONS:
        fire_leaders()

    for _ in range(20):                     # PHASE A -- session fixpoint
        if leaders_done or not _OWNED_SESSIONS:
            break
        if deadline is not None and clock.now() >= deadline:
            break                           # checked BEFORE the scan
        snap = _proc_snapshot(deadline=deadline, ours_cache=owner_cache,
                              clock=clock)

        # Opaque descendants sitting in a session one of our recorded
        # leaders owns, signalled through EXACT handles.
        #
        # Signalled HERE rather than handed on as a set of proven pids: a
        # (pid, start_ticks) pair carried across the sweep is not an
        # identity, for the same 100 Hz reason a session id is not one,
        # and an opaque process inheriting the number inside the tick was
        # signalled through it (Codex adversarial review, High). A pidfd
        # opened while the proof holds IS an identity, so the proof and
        # the signal travel together.
        hit = 0
        for pid, _ppid, _pgrp, sid, _ticks in snap.procs:
            if deadline is not None and clock.now() >= deadline:
                break
            # NEVER the leader itself. For a recorded leader sid ==
            # pid, so it passes the session check and this scan used
            # to kill it -- destroying the very handle every LATER row's
            # ownership proof depends on, after which the reader
            # reaps it and its remaining descendants survive (Codex
            # adversarial review, High). The leaders belong to
            # signal_recorded_leaders(), which fires once THIS scan
            # has been empty twice.
            if pid == me0 or pid == sid:
                continue
            # ONE proof handle, HELD across the target acquisition and
            # the final re-check. Proving, closing, opening the target
            # and proving again is three acquisitions against two
            # descriptors, so at a full table the last one failed and
            # the SIGKILL was skipped -- deterministically, without any
            # concurrency (Codex adversarial review, High).
            proof = _session_handle(sid)
            if proof is None:
                continue
            # EVERYTHING from here to the close is inside the finally.
            # The probe-refusal and expired-budget branches below used
            # to `continue`/`break` BEFORE entering it, leaking one
            # pidfd per scrubbed-environment candidate per scan -- in
            # a teardown, which is precisely when descriptors are
            # scarce enough for the leak to disable the reserve
            # (Codex adversarial review, High).
            try:
                if not _leader_alive(proof):
                    continue
                # The environ probe gets first refusal, as everywhere
                # else: a readable environment that does not carry our
                # stamp is positive evidence the process is NOT ours,
                # and the session marker must never override it.
                if _carries_our_owner_id(pid, deadline=deadline,
                                         clock=clock) is False:
                    continue
                # Re-checked AFTER the probe, for the reason the
                # stamped scan re-checks: an environ read is not
                # interruptible, so
                # the budget can expire inside the one call whose
                # answer then authorizes the signal (Codex adversarial
                # review, Medium).
                if deadline is not None and clock.now() >= deadline:
                    break
                fd = _pidfd_open(pid)
                if fd is None:
                    continue    # no exact handle, no signal
                try:
                    # Re-prove AFTER the descriptor pins the process,
                    # so a PID recycled between the walk and the open
                    # cannot inherit the previous occupant's session --
                    # through the handle ALREADY held, which needs no
                    # further descriptor. The session id is re-read
                    # because the candidate may have left our session
                    # in between; the leader is re-checked through the
                    # same proof.
                    if (_session_of(pid) == sid
                            and _leader_alive(proof)
                            and not (deadline is not None
                                     and clock.now() >= deadline)):
                        try:
                            signal.pidfd_send_signal(fd, signal.SIGKILL)
                            signalled.add(pid)
                            hit += 1
                        except (ProcessLookupError, OSError):
                            pass
                finally:
                    try:
                        os.close(fd)
                    except Exception:
                        pass
            finally:
                try:
                    os.close(proof)
                except Exception:
                    pass
        # To a FIXPOINT, not one shot: a descendant forked after this
        # snapshot is in no round that saw it, and once the leaders die
        # its session can no longer be proven. Two consecutive empty
        # rounds, the same rule the stamped phase uses.
        if hit == 0:
            session_empty += 1
        else:
            session_empty = 0
        if session_empty >= 2:
            break                           # the leaders fire below, now
        # Shares THIS round's snapshot. Whatever it kills is real work --
        # a detached owner-stamped helper reachable now rather than only
        # after the session fixpoint -- but an EMPTY result here says
        # nothing about the post-leader world, so it is not counted.
        stamped_round(snap)
        if deadline is not None and clock.now() >= deadline:
            break
        clock.sleep(0.02)

    # The single place the recorded leaders are signalled. That pass has
    # always been deliberately ungated: these are a handful of processes
    # this sweep is most certain it owns, and signalling one through its
    # pinned identity is a couple of syscalls.
    fire_leaders()

    stamped_empty = 0
    for _ in range(20):                     # PHASE B -- stamped fixpoint
        if deadline is not None and clock.now() >= deadline:
            break                           # checked BEFORE the scan
        if stamped_round(_proc_snapshot(deadline=deadline,
                                        ours_cache=owner_cache,
                                        clock=clock)):
            stamped_empty = 0
        else:
            # TWO consecutive empty scans, not one, because a helper
            # forked after the listdir is absent from a single scan that
            # looks clean.
            stamped_empty += 1
            if stamped_empty >= 2:
                break
        # Re-checked BEFORE the sleep, not only before the scan: the
        # round's signalling can carry the budget past its end, and
        # sleeping on top of that spends time the caller was promised
        # (Codex adversarial review, Medium -- 9l break points).
        if deadline is not None and clock.now() >= deadline:
            break
        clock.sleep(0.02)
    return len(signalled)


class SweepCount(int):
    """The sweep's kill count, carrying whether a sweep actually FINISHED.

    An int subclass rather than a tuple because every existing caller and
    sub-test treats the return value as a number (`if killed:`, `killed
    == 1`, arithmetic) -- and because the completion verdict must travel
    WITH the count instead of being read from module state afterwards. A
    caller that is about to end the process needs its own answer, not the
    last answer anybody got: `completed` is False only for a caller that
    gave up waiting on another thread's in-flight sweep, which is exactly
    the case where killing the process now would orphan whatever that
    sweep had not yet reached (Codex design review, High)."""

    completed: bool

    def __new__(cls, killed: int, completed: bool) -> "SweepCount":
        obj = super().__new__(cls, killed)
        obj.completed = bool(completed)
        return obj


# Single-flight state for the sweep. Four independent entry points can
# reach it -- the termination signal handler, the idle watchdog, main()'s
# finally (all three through the bridge's bounded coordinator) and this
# module's atexit hook -- and before section 25 nothing stopped two of
# them running at once.
# RLock, not Lock, and for the reason the bridge's own _SHUTDOWN_LOCK
# is one: a Python signal handler runs on the main thread, so a
# termination arriving while that thread holds this lock re-enters
# the wrapper through the handler and would block forever on a lock
# only the interrupted frame can release -- reproduced to an exit-137
# timeout despite a 50ms budget (Codex re-adversarial, High).
_SWEEP_LOCK = threading.RLock()
_SWEEP_OWNER: Optional[int] = None
# The in-flight sweep, or None. A record rather than a bare Event because
# the Event alone cannot tell "the sweep finished" apart from "the sweep
# RAISED and the owner's finally woke you anyway" -- and a waiter that
# reads the second as the first goes on to call mark_teardown_complete(),
# disarming the atexit fallback on the strength of a sweep that failed
# (Codex adversarial + test-coverage review, High). `ok` is published
# under _SWEEP_LOCK before the event fires; the event only WAKES waiters.
#
# `deadline` is the OWNER's, which is what a waiter waits against.
# Waiting on its own shorter one would let a loser conclude "gave up" --
# and then terminate the process -- while the owner was still inside its
# budget doing legitimate work (Codex design review, High). Same rule the
# bridge's _graceful_shutdown already applies to its own coordinator.
_SWEEP_FLIGHT: Optional[dict] = None
# CAP on the slice reserved inside the caller's budget for the owner's
# one deliberately ungated step (the recorded-leader pass: a couple of
# syscalls per record). The actual reservation is a fraction of what
# remains, so a short budget still leaves the sweep a usable slice.
_SWEEP_OVERRUN_GRACE_S = 0.5


def force_kill_spawned(settle: float = 1.0,
                       deadline: Optional[float] = None,
                       clock: _Clock = _REAL_CLOCK) -> SweepCount:
    """SINGLE-FLIGHT force sweep: one owner runs it, everybody else waits.

    Returns a SweepCount -- the number of processes THIS call signalled,
    plus `completed`, which says whether a sweep ran to its conclusion
    from this caller's point of view. A waiter reports `completed` True
    with a count of 0: it signalled nothing itself, and the owner's count
    is the owner's to report.

    The reserve is why this exists. `_FD_RESERVE_SIZE` is 2 because ONE
    scan holds exactly two handles at once -- the session proof and the
    target -- so two concurrent sweeps take one slot each and neither can
    complete a pair. Both then skip every opaque session descendant until
    their deadlines expire, which is a silent leak of precisely the
    processes the sweep exists to collect.

    Three arrival shapes, three answers:

      * FIRST caller -- owns the sweep, masks the termination signals for
        its duration, publishes completion on the way out.
      * ANOTHER thread -- waits on the owner's completion against the
        OWNER's deadline, then reports completed.
      * THE OWNER re-entering itself (a termination handler landing on
        the sweeping thread) -- runs the sweep BODY directly, because
        waiting on its own event is a deadlock and returning nothing
        leaves the children the interrupted sweep had not yet reached
        with no collector at all.

    The nested run costs a degraded reserve -- the interrupted frame is
    holding descriptors -- and that was the first design review's reason
    to reject it. What that reasoning missed is that the interrupted
    frame NEVER RESUMES: the only caller that re-enters this way is a
    handler that ends in `os.kill`/`os._exit`. So the choice is a
    degraded sweep against no sweep, not two sweeps competing.

    Deferring the death instead was implemented and REMOVED. It has to
    guarantee delivery from every path that can own a sweep -- the atexit
    hook and any direct caller included -- and each round of that produced
    another way to lose or double-deliver a termination (Codex
    re-adversarial, High x3). Blocking termination process-wide before
    any worker starts and routing it through `sigwait` is the real fix
    and is a bridge-lifecycle change, already owned by section 23."""
    global _SWEEP_OWNER, _SWEEP_FLIGHT
    me = threading.get_ident()
    with _SWEEP_LOCK:
        if _SWEEP_OWNER is None:
            _SWEEP_OWNER = me
            flight = {
                "event": threading.Event(),
                "ok": False,
                # A caller with no deadline of its own still publishes
                # one, so a waiter is never unbounded.
                "deadline": (deadline if deadline is not None
                             else clock.now() + max(settle, 1.0) + 10.0),
                # The deadline is a number in the OWNER's time domain. A
                # waiter on a different clock cannot compare against it
                # -- mixed domains either expire instantly or spin for a
                # fake eternity -- so it falls back to its own (Codex
                # perf review, High).
                "clock": clock,
            }
            _SWEEP_FLIGHT = flight
            role = "owner"
        elif _SWEEP_OWNER == me:
            flight, role = None, "reentry"
        else:
            flight, role = _SWEEP_FLIGHT, "waiter"

    if role == "reentry":
        # Runs, but NEVER claims completeness. It collects everything it
        # can see, which is strictly better than returning empty-handed
        # -- but it cannot see what the interrupted frame is holding. The
        # claim path removes a record from _SPAWNED and carries its
        # descriptor on that suspended stack, so a record caught between
        # the remove and its signal is invisible here and _borrow_pidfd
        # will not reissue it.
        #
        # What completed=False buys, precisely: the paths that RETURN
        # through _shutdown_bounded (idle watchdog, main-finally) stop
        # calling mark_teardown_complete() over a sweep that cannot be
        # whole. It buys NOTHING on the signal path, which self-signals
        # and os._exit()s -- both bypass atexit entirely, so there is no
        # fallback left to preserve there (Codex re-adversarial, High:
        # the earlier wording claimed protection it does not provide).
        # The window itself is older than this section -- the pre-section
        # -25 handler ran its own sweep over the same list(_SPAWNED) and
        # equally could not see a claimed entry -- and closing it needs
        # section 23's process-wide signal design.
        return SweepCount(
            _force_kill_spawned_sweep(settle=settle, deadline=deadline,
                                      clock=clock),
            False)

    if role == "waiter":
        done = flight["event"]
        if flight["clock"] is clock:
            owner_deadline = flight["deadline"]
        else:
            # Different time domain; the owner's number is meaningless
            # here. Fall back to our own bound rather than comparing
            # across clocks.
            owner_deadline = deadline
        # NOT extended by any grace. The caller's deadline is the budget
        # an external `timeout --kill-after` is counting in, so adding to
        # it here bought the owner room out of somebody else's pocket: a
        # waiter measured 0.506s past a shared 0.2s deadline, during
        # which the killer can fire and orphan exactly what the sweep had
        # not reached (Codex perf review, High). The grace is instead
        # RESERVED INSIDE the budget below -- the owner works to an
        # earlier deadline so its overrun still lands inside the bound.
        # Polled rather than Event.wait(timeout=...) so an INJECTED clock
        # bounds the wait in the same units it bounds everything else in
        # this module; a real-time wait against fake-time deadlines is a
        # hang waiting to happen in the sub-tests.
        while not done.is_set():
            if owner_deadline is not None and clock.now() >= owner_deadline:
                break
            clock.sleep(0.02)
        # `ok`, never `is_set()`: the owner wakes its waiters from a
        # finally, so the event fires just as surely when the sweep threw.
        with _SWEEP_LOCK:
            return SweepCount(0, bool(flight["ok"]))

    # The owner works to an EARLIER deadline than the one it published,
    # keeping the grace inside the budget: its one deliberately ungated
    # step (the recorded-leader pass) can then overrun without the
    # waiters -- who stop at the real deadline -- cutting it short, and
    # without anybody exceeding the bound the caller was promised.
    work_deadline = deadline
    if work_deadline is not None:
        # A FRACTION of what is left, capped -- never a fixed floor that
        # can exceed the budget. Subtracting a flat 0.5s handed the sweep
        # an ALREADY-EXPIRED deadline whenever less than that remained,
        # so it skipped the in-flight-spawn settle and both descendant
        # fixpoint phases and ran only the ungated leader pass, while the
        # wrapper still answered completed=True (Codex re-adversarial,
        # High). This is the same floor-exceeds-budget bug the bridge's
        # own `_shutdown_bounded` reserve was already fixed for; nothing
        # here may allocate more time than exists.
        _left = max(0.0, work_deadline - clock.now())
        work_deadline -= min(_SWEEP_OVERRUN_GRACE_S, _left * 0.3)
    killed = 0
    finished_in_budget = False
    try:
        killed = _force_kill_spawned_sweep(settle=settle,
                                           deadline=work_deadline,
                                           clock=clock)
        # Returning is NOT finishing. The recorded-leader pass is ungated
        # by design (section 22: cheap and certain first), so the body
        # can hand back a full result well past the deadline it was
        # given -- and publishing ok=True on that told a waiter, who had
        # already stopped at the hard deadline and answered False, that
        # somebody else had completed the work. `completed` therefore
        # means finished INSIDE the budget, which is the only reading a
        # caller about to end the process can act on (Codex
        # re-adversarial, High).
        finished_in_budget = (deadline is None or clock.now() <= deadline)
        with _SWEEP_LOCK:
            flight["ok"] = finished_in_budget
    finally:
        with _SWEEP_LOCK:
            _SWEEP_OWNER = None
            _SWEEP_FLIGHT = None
        # Set AFTER the ownership release, so a waiter that wakes on the
        # event can immediately become the next owner rather than finding
        # the slot still held by a finished sweep.
        flight["event"].set()
    return SweepCount(killed, finished_in_budget)


_TEARDOWN_COMPLETE = False


def mark_teardown_complete() -> None:
    """Record that a DEADLINE-BOUND teardown already ran to completion.

    The atexit hook is registered unconditionally, so on a normal return
    it fired AFTER the bounded coordinator had already finished -- adding
    a fresh graceful attempt, a 2s join and an unbounded force sweep on
    top of the budget the caller was promised. Bounding the coordinator
    means nothing if a second, unbounded teardown follows it."""
    global _TEARDOWN_COMPLETE
    _TEARDOWN_COMPLETE = True


def _atexit_kill_all() -> None:
    """Collect every LspSubprocess still alive at interpreter shutdown.

    Runs on every NORMAL exit path (return from main, SystemExit,
    unhandled exception) -- but NOT on signal death or os._exit, which is
    why force_kill_spawned() exists as an explicit call for those.

    BOUNDED, because this is the last code to run before the process is
    allowed to die. Calling the graceful reap directly here re-opened the
    hang the bounded coordinator had just closed: shutdown() takes
    _init_lock untimed, so a thread wedged in a handshake kept the
    interpreter alive indefinitely AFTER a bounded main-finally pass had
    already force-killed the children (Codex adversarial review, High --
    reproduced, exit blocked until an external 3s timeout intervened).
    Nothing may block here; a graceful attempt is worth 2 seconds and no
    more, and the force sweep is what actually guarantees the exit."""
    if _TEARDOWN_COMPLETE:
        # UNCONDITIONALLY. A bounded teardown deliberately LEAVES records
        # behind when its budget expires, so "records remain" is not
        # evidence of work owed -- it is evidence the bound was enforced.
        # Gating on an empty registry therefore re-entered exactly the
        # case the bound existed for, and added ~4s past it (Codex
        # adversarial review, measured 4.001s).
        return
    # Sole fallback: ONE absolute deadline, with a slice RESERVED for the
    # force sweep. Letting the graceful attempt spend the whole deadline
    # hands the sweep an already-expired one, and an expired sweep
    # declines every record -- so a wedged graceful teardown meant the
    # child survived interpreter exit outright (Codex adversarial review,
    # High). The same allocation bug _shutdown_bounded already had; the
    # force sweep is in a finally because it is the guarantee.
    deadline = time.monotonic() + 2.0
    graceful_deadline = deadline - 0.6
    try:
        worker = threading.Thread(
            target=reap_all_live,
            kwargs={"timeout": 0.5, "deadline": graceful_deadline},
            name="atexit-reap", daemon=True,
        )
        worker.start()
        worker.join(timeout=max(0.0, graceful_deadline - time.monotonic()))
    except Exception:
        pass
    finally:
        force_kill_spawned(settle=0.0, deadline=deadline)


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
        # THIS instance's row in _SPAWNED. Retirement takes the object,
        # not the number: a by-number retirement can remove and close a
        # live generation's record when the PID was reused (Codex
        # adversarial review, High). Section 25's dispose-confirm-spawn
        # order narrows that overlap; it does not remove it, since
        # unretired records for older generations outlive the instances
        # that made them.
        self._spawn_record: Optional[_SpawnRecord] = None
        # Identifies THIS spawn generation in the child environment, so a
        # group sweep can address descendants of this instance and not of
        # a namesake that reused its PID.
        self._gen_id = uuid.uuid4().hex
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
        # Confirmed-dead verdict for shutdown(). Memoised ONLY once True:
        # a False answer means death was not confirmed BEFORE this call
        # ran out of budget, not that the process is immortal, and
        # replaying it forever from the idempotent early return would
        # leave a since-exited server permanently un-retired and its key
        # permanently unrespawnable (Codex design review, High).
        self._confirmed_dead = False
        # The post-death group collection runs exactly once per instance,
        # whichever call first observes the leader dead -- the shutdown
        # that killed it, or a later idempotent call that re-polls.
        self._post_death_done = False
        # Set while one thread is INSIDE the collection, so a concurrent
        # late confirmer waits for the real answer instead of reading a
        # claim as a completion.
        self._post_death_running: Optional[threading.Event] = None
        self._post_death_lock = threading.Lock()
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

        # _spawn records the child itself, as early as it can (see the
        # _SPAWNED comment): registering only after _spawn RETURNED left the
        # child unreachable to every reap for the duration of the ledger
        # write and the reader-thread creation.
        self._spawn()

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
            # Stamp ownership EXPLICITLY rather than relying on
            # inheritance: a spawner that passes a filtered env (PSES
            # builds one) would otherwise produce an unattributable
            # child, and an unattributable child is exactly what makes
            # a leak argument unfalsifiable.
            child_env = dict(self._env if self._env is not None else os.environ)
            child_env["LSP_BRIDGE_RUN_ID"] = _RUN_ID
            child_env["LSP_BRIDGE_OWNER"] = _OWNER_ID
            # Per-SPAWN, not per-process. The owner stamp says "this
            # bridge started it", which stops being specific enough the
            # moment two generations share a PID -- which section 25's
            # dispose-confirm-spawn order makes rarer, not impossible,
            # since a record outlives its instance. Both generations use
            # start_new_session, so old-instance cleanup can
            # find the REPLACEMENT in what it believes is its own process
            # group and kill it -- a crash loop where each restart shoots
            # its successor (Codex adversarial review, High).
            child_env["LSP_BRIDGE_GEN"] = self._gen_id
            global _SPAWN_INFLIGHT
            with _SPAWN_INFLIGHT_LOCK:
                _SPAWN_INFLIGHT += 1
            try:
                proc = subprocess.Popen(
                    self.cmd,
                    stdin=subprocess.PIPE,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    bufsize=0,
                    cwd=str(self.cwd) if self.cwd else None,
                    env=child_env,
                    start_new_session=True,
                )
                # FIRST statement after Popen, before the attribute store
                # and everything else: this is the window a termination can
                # land in, so make it one bytecode wide.
                self._spawn_record = _record_spawn(proc.pid)
            finally:
                with _SPAWN_INFLIGHT_LOCK:
                    _SPAWN_INFLIGHT -= 1
            self._proc = proc
            # Identity is already pinned (above, immediately after Popen).
            # These are the further-from-the-kernel records: the instance
            # registry the graceful reap walks, then the diagnostic ledger.
            _LIVE_SUBPROCS.add(self)
            _ledger_record(self._proc.pid, self.lang, self.cmd[0])
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
                    # Identity-pinned like every other signal in this
                    # file. No reader thread exists yet on this path, so
                    # nothing can reap the child underneath us -- but
                    # "safe because of who else happens to be running" is
                    # the assumption that produced the bugs above.
                    _signal_recorded(self._spawn_record or proc.pid,
                                 signal.SIGTERM)
                    try:
                        proc.wait(timeout=1.0)
                    except subprocess.TimeoutExpired:
                        _signal_recorded(self._spawn_record or proc.pid,
                                     signal.SIGKILL)
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
                    # Retire the identity record unconditionally. Without
                    # this, every failed construction leaves a dead record
                    # holding a pidfd, and the failure most likely to
                    # repeat here is thread/resource exhaustion -- so the
                    # leak compounds exactly the condition that caused it,
                    # until the bridge hits EMFILE (Codex adversarial
                    # review, Medium: 20 forced failures took the fd count
                    # 4 -> 24). Gating on poll() left the same leak for a
                    # child that died just after the final poll: the ctor
                    # is raising, so no LspSubprocess will ever exist to
                    # retire it later. We have already sent SIGKILL and
                    # taken the group with it below, so the record has no
                    # remaining job.
                    _reap_group(proc.pid, gen=self._gen_id)
                    _retire_spawn(self._spawn_record or proc.pid)
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
                # available. Briefly wait for reaping when the reader
                # races ahead of the kernel (SIGKILL delivered but
                # proc not yet reaped); EOF on the read pipe already
                # implies the subprocess exited, so the fallback
                # reason still says "subprocess exited" -- only the
                # exit code may be unknown.
                rc = None
                if self._proc:
                    for _ in range(20):  # up to ~100ms
                        try:
                            rc = self._proc.poll()
                        except Exception:
                            rc = None
                        if rc is not None:
                            break
                        time.sleep(0.005)
                if rc is not None:
                    self._crash_reason = (
                        f"subprocess exited with code {rc}"
                    )
                else:
                    self._crash_reason = (
                        "subprocess exited (reader saw EOF; "
                        "exit code not yet reaped)"
                    )
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

    def _collect_after_death(self, proc, deadline: Optional[float]) -> bool:
        """Group collection + record retirement, once per instance.

        Runs for whichever call first OBSERVES the leader dead. A polite
        leader answers `shutdown`, exits, and leaves behind any worker it
        forked that did not -- that orphan is reparented away from us, so
        the pgid we still hold is the only remaining handle on it. Retire
        the record only after the group has been swept, because
        retirement throws that handle away.

        Returns True only when the collection has FINISHED. Three states,
        not a bool, because the two-state version answered "done" the
        instant it claimed the work: a concurrent late confirmer saw the
        flag, skipped the collection, and reported a confirmed death
        while the first thread was still sweeping descendants -- so the
        respawn gate could publish a replacement over a live
        old-generation worker, which is the exact duplication section 25
        exists to prevent. A raised collection also left the flag set
        forever, permanently suppressing the retry and the retirement
        (Codex adversarial + test-coverage review, High).

        Never raises: shutdown() is reachable from atexit, where an
        exception is swallowed by interpreter teardown anyway. A failure
        is reported as "not collected" and stays retryable."""
        with self._post_death_lock:
            if self._post_death_done:
                return True
            running = self._post_death_running
            if running is None:
                running = self._post_death_running = threading.Event()
                mine = True
            else:
                mine = False
        if not mine:
            # Wait for the thread that claimed it, bounded by OUR
            # deadline -- an unbounded wait here would put a teardown
            # path at the mercy of another thread's sweep.
            while not running.is_set():
                if deadline is not None and time.monotonic() >= deadline:
                    return False
                time.sleep(0.02)
            return self._post_death_done
        ok = False
        try:
            # `emptied` is the whole answer, and discarding it was the
            # bug: _reap_group returns False when its deadline or round
            # cap stopped it short of the two-consecutive-empty-scans
            # fixpoint, so treating any non-raising return as success let
            # the respawn gate publish a replacement beside a surviving
            # old-generation worker -- the duplication section 25 exists
            # to prevent, re-entered through the gate meant to close it
            # (Codex adversarial + consistency + perf review, High).
            _killed, emptied = _reap_group(proc.pid, gen=self._gen_id,
                                           deadline=deadline)
            # Retired UNCONDITIONALLY, incomplete sweep included. The
            # record is a descriptor pinning the LEADER, which is already
            # dead; the group sweep addresses members by pgid plus the
            # owner and generation stamps, none of which live in it. So a
            # retry loses nothing by retiring now, while KEEPING it is
            # what accumulated one descriptor per dead generation until
            # EMFILE (section 24, Codex adversarial review, Medium).
            _retire_spawn(self._spawn_record or proc.pid)
            ok = bool(emptied)
        except Exception:
            ok = False
        finally:
            with self._post_death_lock:
                self._post_death_done = ok
                self._post_death_running = None
            running.set()
        return ok
        # Retired UNCONDITIONALLY once the leader is confirmed dead. Its
        # descriptor can identify nothing further, and the group sweep
        # addresses members by pgid plus the owner and generation stamps
        # -- none of which live in this record. The earlier version kept
        # it whenever the sweep had not finished, which on an expired
        # deadline meant forever: shutdown is idempotent and the force
        # sweep only ever borrows a duplicate, so a respawn loop on a
        # loaded host accumulated one descriptor per dead generation
        # until EMFILE (Codex adversarial review, Medium).

    def _retry_pinned_kill(self, proc, deadline: float) -> None:
        """Re-attempt the identity-pinned termination, nothing else.

        The escalation in shutdown() runs once, and its two signals are
        exactly what a full descriptor table defeats: `_signal_recorded`
        fails CLOSED rather than naming the target by number (section
        23), so under transient exhaustion the leader is never signalled
        at all. Polling alone would then wait forever on a process
        nobody ever asked to die, while the respawn gate records one
        disposal failure after another and walks the key into FAILED
        state (Codex adversarial review, High).

        Deliberately does NOT repeat the LSP protocol exchange:
        `shutdown`/`exit` were already sent and the transport is torn
        down. Only the signal is retried.

        ONE signal, and it is SIGKILL. Re-running the SIGTERM-then-
        SIGKILL escalation looks more polite and is strictly worse:
        under descriptor pressure `_signal_recorded` reaches its CLAIM
        path, which removes the record from _SPAWNED and closes its
        descriptor to send that one signal -- so a stubborn server
        ignores the SIGTERM and the SIGKILL that follows finds no record
        and fails closed, having spent the only identity we had (Codex
        adversarial review, High, reproduced). Politeness has already
        been spent anyway: the caller's first shutdown() sent `shutdown`,
        `exit`, and a SIGTERM before this retry exists."""
        if proc.poll() is not None:
            return
        try:
            # wait_after_last: this caller has no group sweep of its own
            # to run first -- `_confirm_dead_late` collects immediately
            # after -- so it does want the death observed before it
            # returns. `shutdown()` deliberately does NOT, because its
            # unconditional `_reap_group` has to see an unreaped leader.
            _escalate_recorded(self._spawn_record or proc.pid,
                               lambda: proc.poll() is not None,
                               sigs=(signal.SIGKILL,), deadline=deadline,
                               wait_after_last=True)
        except Exception:
            pass

    def _confirm_dead_late(self, timeout: float) -> bool:
        """Re-answer the confirmed-dead question on an idempotent call.

        The verdict a shutdown() reaches is a snapshot of one bounded
        attempt: a SIGKILL whose target had not yet been reaped inside
        the budget, or a signal a full descriptor table stopped us
        sending, both answer False while the process is either dying or
        never asked to. So a repeat call retries the pinned signals,
        re-polls, and -- when it is the first to see the death --
        performs the collection the original call never reached (Codex
        design review + adversarial review, High)."""
        if self._confirmed_dead:
            return True
        proc = self._proc
        if proc is None:
            self._confirmed_dead = True
            return True
        # The CALLER's budget, with no floor under it. A 0.2s minimum
        # looked harmless and made every idempotent call cost at least
        # that: reap_all_live enters with whatever the shared deadline
        # has left, sometimes under 50ms, and this path then polled for
        # 200 -- overrunning the very budget it was handed a slice of
        # (Codex perf review, Medium, measured 204-208ms for 1-50ms
        # requests).
        deadline = time.monotonic() + max(0.0, timeout)
        if proc.poll() is None and timeout > 0.0:
            self._retry_pinned_kill(proc, deadline)
        if proc.poll() is None:
            return False
        # Confirmed dead means the LEADER exited AND its group has been
        # collected: publishing a replacement over surviving descendants
        # is the same duplication as publishing over a live leader.
        self._confirmed_dead = self._collect_after_death(proc, deadline)
        return self._confirmed_dead

    def shutdown(self, timeout: float = 5.0) -> bool:
        """LSP shutdown + exit + process reap.

        Best-effort: if the LSP ignores `shutdown` we SIGTERM; if it
        ignores that we SIGKILL. Exits without raising so atexit-driven
        cleanup can't crash the interpreter.

        RETURNS the confirmed-dead verdict: True when this instance's
        leader is known to have exited (or never existed), False when the
        attempt finished without proving it. The caller that matters is
        the bridge's respawn path, which must not publish a replacement
        language server while the old one might still be running -- an
        unconfirmed disposal that publishes anyway is how one crash turns
        into two live servers on the same workspace (section 25).

        Idempotent: safe to call twice. A second call re-polls the
        process and returns the verdict; it does not repeat the teardown
        traffic.

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
        # ONE absolute budget for the WHOLE shutdown, taken at entry.
        shutdown_deadline = time.monotonic() + max(0.2, timeout)
        if self._shutdown_called:
            return self._confirm_dead_late(timeout)

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
                # Nothing was ever spawned, so nothing can be alive. This
                # is a CONFIRMED death, not an unknown one -- answering
                # False here would make an instance that failed before
                # Popen permanently unrespawnable.
                self._confirmed_dead = True
                return True

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
        # ONE absolute budget for every group sweep this shutdown runs.
        # Created HERE, before the first one can fire: the stubborn
        # escalation below reaped a group with no deadline at all, and
        # that helper takes up to twenty full procfs scans with sleeps --
        # spent before the collection deadline further down even existed
        # (Codex adversarial review, Medium).
        # Derived from the deadline taken at ENTRY, not from `timeout`
        # again: recomputing it here re-allocates the caller's whole
        # budget partway through, so a shutdown called with 0.5s could
        # spend that on the polite exchange and another 0.5s on group
        # collection (Codex perf review, Medium).
        group_deadline = shutdown_deadline
        if proc.poll() is None:
            # Signal the leader through its PINNED identity, not by
            # number. proc.poll() was checked above, and between that
            # check and the signal the reader thread can reap the leader
            # -- after which proc.terminate() is just os.kill() on a
            # number that may already belong to somebody else (Codex
            # adversarial review, High; the killpg pair this replaced had
            # the same defect at the group level).
            #
            # ONE handle across the whole SIGTERM-to-SIGKILL escalation.
            # Two independent _signal_recorded calls spend the record on
            # the first signal whenever acquisition takes the claim path,
            # so a server that ignores SIGTERM survived the SIGKILL that
            # could not then be addressed (Codex adversarial review,
            # High).
            try:
                _escalate_recorded(self._spawn_record or proc.pid,
                                   lambda: proc.poll() is not None,
                                   deadline=min(group_deadline,
                                                time.monotonic()
                                                + max(timeout, 0.1)))
            except Exception:
                pass
            # DESCENDANTS FIRST, leader reap second. Unconditional: a
            # leader that died to the SIGKILL is exactly the case whose
            # workers need collecting, and its identity is still provable
            # only while it is unreaped. Anything the leader forked goes
            # through the ownership-verified group sweep.
            _reap_group(proc.pid, gen=self._gen_id,
                        deadline=group_deadline)
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                # Child is stuck in uninterruptible sleep; give up and
                # let the OS reap it at our exit.
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

        # (7) Collect the rest of the group, THEN retire the record.
        #
        # A leader that shuts down politely is the dangerous case, not the
        # stubborn one: it answers `shutdown`, exits, and leaves behind any
        # worker it forked that did not. That orphan is reparented away
        # from us, so it is no longer one of our children and no later
        # sweep can find it -- and retiring the record here would throw
        # away the pgid that is the only remaining handle on it (Codex
        # adversarial review, High). So signal the group while we still
        # hold that handle.
        if proc.poll() is not None:
            # ONE absolute deadline and ONE loop. This used to alternate
            # a group-emptiness question with a reap, up to 20 times --
            # and each emptiness question walked /proc, while each reap
            # ran its OWN bounded loop of walks, so a single shutdown
            # nested walks inside walks and the deadline bounded the
            # wall-clock but not the work (section 24). _reap_group
            # already loops to a two-consecutive-empty-scans fixpoint;
            # asking it separately was asking the same question twice.
            self._confirmed_dead = self._collect_after_death(
                proc, group_deadline)
        return self._confirmed_dead

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
        #
        # Gated on _confirmed_dead, NOT _shutdown_called: an attempt that
        # finished without proving the death has already set the latter,
        # so gating on it meant finalization skipped the one path that
        # can still retry the signal and collect the group -- the object
        # simply disappeared with its child running (Codex consistency
        # review, Medium).
        try:
            if not self._confirmed_dead:
                self.shutdown(timeout=0.2)
        except Exception:
            pass
