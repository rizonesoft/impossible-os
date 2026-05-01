#!/usr/bin/env python3
# Shared post-commit hook serializer for the
# `post_commit_smoketest.py` and `post_commit_smoketest_boot.py`
# hooks (TODO-08 §31).
#
# Both hooks fire from the PostToolUse Bash event after every git
# commit, and both run scripts that touch the build/ tree
# (scripts/test.sh -> build/ artifacts, scripts/test-smoke.sh ->
# build/ artifacts + smoke logs).  When two commits land within
# ~3 seconds (typical for review + hash-fill commit pairs), the
# two hook processes race on build/, clobber each other's
# intermediate object directories, and produce spurious
# `[smoketest] [FAIL] Build failed` / `SMOKE TEST FAILED` banners
# even though HEAD compiles green on a clean rebuild.
#
# This helper exposes a single non-blocking flock primitive on
# /tmp/impossible-os-postcommit.lock.  Whichever hook process
# acquires the lock first runs to completion; the second process
# emits a `[smoketest] another build in progress, deferring`
# systemMessage and exits 0 silently.  fcntl.flock() releases on
# process exit, so a crashed hook never wedges the lock.
#
# Cross-process semantics on WSL2 are confirmed via
# scripts/lsp-mcp/lsp_client.py's stress tests (test 8a,
# 100 concurrent hovers); fcntl.flock at the OS layer is
# WSL2-safe.
import fcntl
import json
import os
import signal
import subprocess
import sys
from contextlib import contextmanager

_LOCK_PATH = os.environ.get(
    "IMPOSSIBLE_OS_POSTCOMMIT_LOCK",
    "/tmp/impossible-os-postcommit.lock",
)


@contextmanager
def try_acquire():
    """Yield (acquired: bool, lock_fd: int|None).

    On `acquired=True`, the lock is held for the duration of the
    context manager and released on exit.  On `acquired=False`,
    the caller should emit a deferred systemMessage and exit
    without running the build/test subprocess."""
    fd = None
    try:
        fd = os.open(
            _LOCK_PATH,
            os.O_CREAT | os.O_WRONLY,
            0o644,
        )
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            yield False, None
            return
        # Best-effort PID record so an operator can see who owns
        # the lock; not load-bearing for correctness (flock is).
        try:
            os.ftruncate(fd, 0)
            os.write(fd, f"{os.getpid()}\n".encode("ascii"))
        except OSError:
            pass
        yield True, fd
    finally:
        if fd is not None:
            try:
                fcntl.flock(fd, fcntl.LOCK_UN)
            except OSError:
                pass
            try:
                os.close(fd)
            except OSError:
                pass


def run_with_group_timeout(argv, timeout_sec, env=None):
    """Run argv in its own process group; on timeout, SIGKILL the
    whole group and reap before returning.

    Returns a `subprocess.CompletedProcess`-shaped object on
    completion, or raises `subprocess.TimeoutExpired` AFTER the
    process group has been killed and reaped.  The reap is the
    point of this wrapper: a plain `subprocess.run(timeout=...)`
    only kills the direct child, leaving QEMU / make descendants
    free to keep writing build/ artifacts after the lock releases.

    The wrapper buffers stdout+stderr fully (capture_output=True
    semantics) so callers can parse `.stdout` / `.returncode` the
    same way they did with `subprocess.run`."""
    p = subprocess.Popen(
        argv,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=env,
        start_new_session=True,
    )
    try:
        out, _err = p.communicate(timeout=timeout_sec)
    except subprocess.TimeoutExpired:
        # Kill the whole process group, then reap.  Without the
        # group kill, scripts/test.sh's QEMU descendants survive
        # and keep mutating build/.
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            out, _err = p.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            out = ""
        # Re-raise so caller still sees the timeout signal.
        raise subprocess.TimeoutExpired(argv, timeout_sec, output=out)
    return subprocess.CompletedProcess(argv, p.returncode, out, "")


def emit_deferred(banner_prefix: str, head_short: str = "") -> int:
    """Emit a `continue: True` systemMessage indicating the lock
    is held by another post-commit hook process and exit 0.

    Caller should pass its banner prefix (e.g. `[smoketest]`,
    `[smoketest-boot]`) plus an optional short HEAD sha for
    operator context."""
    suffix = f" (HEAD {head_short})" if head_short else ""
    msg = (
        banner_prefix
        + " another post-commit build/smoke run in progress, "
        + "deferring" + suffix
    )
    print(json.dumps({"systemMessage": msg, "continue": True}))
    return 0
