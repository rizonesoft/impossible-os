#!/usr/bin/env python3
# PostToolUse Bash hook: when the just-completed Bash invocation
# created a `git commit` that touched any kernel/source file,
# run the kernel-test smoketest (scripts/test.sh QUIET=1) and
# report the result via systemMessage.
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). M1 design-review fix: detection no longer relies
# on `cmd.startswith("git commit")` -- it uses the same
# alias-aware classifier as section_commit_gate so wrapped
# commit invocations (`gci`, `cm`, `git -c alias.X='...' X`,
# wrapper chains) are detected correctly. Without this fix the
# smoketest silently skipped on every commit done through an
# alias.
#
# Hook category: REMINDER (no exit-2 path; the test result is a
# systemMessage). NOT a candidate for wrap.sh marker-prefilter
# because the hook needs the full alias classifier; a substring
# prefilter would re-introduce the wrapped-commit gap M1 fixes.
import json
import os
import subprocess
import sys
from pathlib import Path

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
import section_commit_gate as scg  # noqa: E402
import _postcommit_lock as pclock  # noqa: E402


_SOURCE_EXTS = (".c", ".h", ".asm", ".ld")


def _read_skip_prefix(root: str, parent: str) -> str:
    """If HEAD's parent SHA matches a SKIP_REVIEW_HOOK record, return
    a `[SKIP_REVIEW_HOOK reason: ...] ` prefix to put on the
    smoketest message so the user sees what was bypassed.

    SKIP records the parent SHA at SKIP-time (before commit creates
    the new HEAD), so post-commit lookup compares against HEAD~1
    (Codex M1 fix from prior round).
    """
    if not parent:
        return ""
    log_path = root + "/.claude/state/skip-log.jsonl"
    if not os.path.exists(log_path):
        return ""
    try:
        with open(log_path, "r", encoding="utf-8") as f:
            for line in reversed(f.readlines()):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if (rec.get("head_sha", "").startswith(parent[:12])
                        and rec.get("reason")):
                    return ("[SKIP_REVIEW_HOOK reason: "
                            + rec["reason"][:160] + "] ")
    except Exception:
        pass
    return ""


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    cmd = (d.get("tool_input", {}) or {}).get("command", "") or ""

    # M1 fix: alias-aware detection via section_commit_gate's
    # _bash_is_git_commit. Returns (is_commit, no_verify); we only
    # care about is_commit here. _ensure_crc populates the lazy
    # crc helper module the classifier depends on.
    try:
        scg._ensure_crc()
        is_commit, _ = scg._bash_is_git_commit(cmd)
    except Exception:
        return 0
    if not is_commit:
        return 0

    try:
        changed = subprocess.check_output(
            ["git", "show", "--name-only", "--format=", "HEAD"],
            text=True, timeout=5, stderr=subprocess.DEVNULL,
        ).splitlines()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return 0
    if not any(f.endswith(_SOURCE_EXTS) for f in changed if f):
        return 0

    try:
        root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return 0

    parent = ""
    try:
        parent = subprocess.check_output(
            ["git", "rev-parse", "HEAD~1"],
            cwd=root, text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    skip_prefix = _read_skip_prefix(root, parent)

    # Serialize against post_commit_smoketest_boot.py and any other
    # post-commit hook touching build/.  Back-to-back commits stomp
    # each other's intermediate object directories without this lock.
    with pclock.try_acquire() as (acquired, _fd):
        if not acquired:
            head_short = ""
            try:
                head_short = subprocess.check_output(
                    ["git", "rev-parse", "--short", "HEAD"],
                    cwd=root, text=True, timeout=2,
                    stderr=subprocess.DEVNULL,
                ).strip()
            except Exception:
                pass
            return pclock.emit_deferred(
                skip_prefix + "[smoketest]", head_short)

        try:
            result = pclock.run_with_group_timeout(
                ["bash", root + "/scripts/test.sh", "QUIET=1"],
                60,
                env={**os.environ, "TIMEOUT": "30"},
            )
        except subprocess.TimeoutExpired:
            # Process group already SIGKILL'd + reaped by
            # run_with_group_timeout before this except branch
            # runs, so the lock can release safely without
            # leaving QEMU/make descendants stomping build/.
            print(json.dumps({
                "systemMessage": (skip_prefix
                                  + "[smoketest] TIMED OUT after 60s"),
                "continue": True,
            }))
            return 0

    if result.returncode == 0:
        line = [l for l in result.stdout.splitlines() if "PASS" in l]
        msg = line[-1].strip() if line else "Tests passed"
        print(json.dumps({
            "systemMessage": skip_prefix + "[smoketest] " + msg,
        }))
    else:
        fails = [l for l in result.stdout.splitlines() if "FAIL" in l]
        msg = fails[-1].strip() if fails else "Tests failed"
        print(json.dumps({
            "systemMessage": skip_prefix + "[smoketest] " + msg,
            "continue": True,
        }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
