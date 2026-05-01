#!/usr/bin/env python3
# Test for _postcommit_lock.py (TODO-08 §31).
#
# Coverage:
#   1. First acquirer wins; second gets acquired=False.
#   2. Lock auto-releases when first context manager exits.
#   3. emit_deferred() prints valid JSON with continue=True.
#   4. Override via IMPOSSIBLE_OS_POSTCOMMIT_LOCK env honored.
#
# Run: python3 .claude/hooks/test_postcommit_lock.py
import importlib
import io
import json
import os
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

_HOOK_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(_HOOK_DIR))


def _fresh_module(lock_path: str):
    """Re-import _postcommit_lock with the given _LOCK_PATH override
    so each subtest starts from a clean module-state baseline."""
    os.environ["IMPOSSIBLE_OS_POSTCOMMIT_LOCK"] = lock_path
    if "_postcommit_lock" in sys.modules:
        del sys.modules["_postcommit_lock"]
    return importlib.import_module("_postcommit_lock")


def _run() -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        lock_path = os.path.join(td, "test.lock")

        # 1. First acquirer wins.
        m = _fresh_module(lock_path)
        with m.try_acquire() as (acquired_a, fd_a):
            if not acquired_a:
                print("[FAIL] first acquirer did not get the lock")
                failures += 1
            elif fd_a is None:
                print("[FAIL] first acquirer got acquired=True but fd=None")
                failures += 1
            else:
                print("[PASS] first acquirer wins")

            # 2. Second acquirer (concurrent context) sees lock held.
            with m.try_acquire() as (acquired_b, fd_b):
                if acquired_b:
                    print("[FAIL] second acquirer got the lock while held")
                    failures += 1
                elif fd_b is not None:
                    print("[FAIL] second acquirer got fd != None on contention")
                    failures += 1
                else:
                    print("[PASS] second acquirer sees lock held")

        # 3. After first context exit, lock is releasable.
        with m.try_acquire() as (acquired_c, _fd):
            if not acquired_c:
                print("[FAIL] lock did not release after first context exit")
                failures += 1
            else:
                print("[PASS] lock auto-releases on context exit")

        # 4. emit_deferred prints valid JSON with continue=True.
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = m.emit_deferred("[smoketest]", "abcd1234")
        if rc != 0:
            print(f"[FAIL] emit_deferred returned {rc}, want 0")
            failures += 1
        try:
            payload = json.loads(buf.getvalue().strip())
        except Exception as exc:
            print(f"[FAIL] emit_deferred JSON parse: {exc}")
            failures += 1
        else:
            ok = (
                payload.get("continue") is True
                and "another post-commit" in payload.get("systemMessage", "")
                and "abcd1234" in payload.get("systemMessage", "")
                and payload["systemMessage"].startswith("[smoketest]")
            )
            if ok:
                print("[PASS] emit_deferred shape")
            else:
                print(f"[FAIL] emit_deferred shape: {payload}")
                failures += 1

        # 5. Env override path: a different lock file should not
        # collide with the previous one.
        alt_path = os.path.join(td, "alt.lock")
        m2 = _fresh_module(alt_path)
        if m2._LOCK_PATH != alt_path:
            print(f"[FAIL] env override ignored: {m2._LOCK_PATH}")
            failures += 1
        else:
            with m2.try_acquire() as (ok, _f):
                if not ok:
                    print("[FAIL] alt lock path could not acquire")
                    failures += 1
                else:
                    print("[PASS] env override path used")

        # 6. run_with_group_timeout reaps the whole process group
        # on TimeoutExpired -- a parent shell that backgrounds a
        # long-running grandchild must NOT leave the grandchild
        # alive after the timeout fires.
        m3 = _fresh_module(lock_path)
        import subprocess
        # Parent shell: spawn a 30s sleep grandchild in background,
        # then sleep 30s itself.  killpg should reap both within
        # the inner 5s wait.
        argv = ["bash", "-c",
                "sleep 30 & echo $! > /tmp/.test_pgid_grandchild; "
                "sleep 30"]
        try:
            m3.run_with_group_timeout(argv, 1)
            print("[FAIL] run_with_group_timeout did not raise on timeout")
            failures += 1
        except subprocess.TimeoutExpired:
            # Grandchild PID was recorded; kill(pid, 0) probes
            # whether it is still alive.  killpg should have
            # reaped it.
            try:
                with open("/tmp/.test_pgid_grandchild", "r") as f:
                    grand_pid = int(f.read().strip())
                os.kill(grand_pid, 0)
                # Reachable means grandchild still alive -> FAIL.
                print(f"[FAIL] grandchild {grand_pid} survived killpg")
                failures += 1
                try:
                    os.kill(grand_pid, 9)
                except OSError:
                    pass
            except FileNotFoundError:
                # Grandchild may not have been recorded yet --
                # acceptable if the parent was killed before write.
                print("[PASS] run_with_group_timeout (no grandchild recorded)")
            except ProcessLookupError:
                print("[PASS] run_with_group_timeout reaped grandchild")
            finally:
                try:
                    os.unlink("/tmp/.test_pgid_grandchild")
                except OSError:
                    pass

    if failures:
        print(f"FAIL: {failures} subtest(s) failed")
        return 1
    print("OK: all subtests passed")
    return 0


if __name__ == "__main__":
    sys.exit(_run())
