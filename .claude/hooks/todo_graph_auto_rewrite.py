#!/usr/bin/env python3
# PostToolUse Edit/Write/MultiEdit hook: when the agent edits a
# TODO markdown file, run scripts/todo-graph/validate.py
# --fix-line-numbers --write to keep stamp parentheticals' line
# numbers fresh. Any result other than "every stamp is correct" is
# surfaced as a systemMessage (the exit contract in validate.py's --fix-line-numbers header):
#   rc 0  every stamp examined is correct -> silent
#   rc 1  the repair RAN and is INCOMPLETE (ambiguous, unresolvable or
#         missing-item stamps; the safe rewrites were still applied)
#   rc 2  REFUSED (e.g. a resolver-key collision); files written before a
#         late refusal are named on its ALREADY REWRITTEN line
#   an rc 1 without the completion summary (a Python crash), any other rc,
#   a timeout or a launch failure: the repair did not finish
# Until 2026-11-13 only an rc 1 whose output said "ambiguous" was shown,
# so the unresolvable and missing-item classes reached nobody.
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original (same
# 15-second timeout, same ambiguity message shape).
#
# Hook category: SIDE-EFFECT (writes the TODO file when
# unambiguous; never blocks). NOT a wrap.sh candidate -- the
# validate.py invocation is the actual work; prefilter savings
# would be negligible vs the validator runtime, and the markers
# (`todo/`, `.md`) need full-path inspection.
import json
import os
import re
import signal
import subprocess
import sys
import time


def _left(deadline):
    """Seconds left before the hook's one deadline (never below a tenth, so a call still gets a bounded try)."""
    return max(0.1, deadline - time.monotonic())


def _registered_worktrees(where, deadline):
    """The canonical roots of every worktree git has registered for the repository holding `where`, or an empty
    set when that cannot be read. A tree that merely points at the repository's metadata is not among them. A
    lookup that runs out of time raises subprocess.TimeoutExpired: the caller reports a repair not run."""
    try:
        out = subprocess.check_output(["git", "-C", where, "worktree", "list", "--porcelain"],
                                      stderr=subprocess.DEVNULL, timeout=_left(deadline)).decode("utf-8", "surrogateescape")
    except subprocess.TimeoutExpired:
        raise
    except Exception:  # fail-direction: no list means no repair, which is silent and safe
        return set()
    return {os.path.realpath(line[len("worktree "):]) for line in out.splitlines() if line.startswith("worktree ")}


def _not_run(why):
    print(json.dumps({"systemMessage": f"{PREFIX} was not run: {why}, so stamp line numbers were not checked. "
                      "Run it by hand: python3 scripts/todo-graph/validate.py --fix-line-numbers --write"}))
    return 0


def _run_group(cmd, cwd, timeout):
    """subprocess.run in its own process group, the whole group stopped on timeout: the validator can start a
    cache rebuild and its git, which must not outlive this hook. Raises subprocess.TimeoutExpired."""
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.communicate()
        raise
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:  # fail-direction: the group already ended, which is the goal
            pass
    return subprocess.CompletedProcess(cmd, proc.returncode, out, err)


def main() -> int:
    deadline = time.monotonic() + BUDGET_S              # one deadline for every step, inside the harness's
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if not path or not path.endswith(".md"):
        return 0
    p = path.replace(chr(92), "/")
    if "todo/" not in p:
        return 0

    # The tree that holds the edited file, never the hook's cwd: the harness runs hooks in the session's primary
    # tree, so an edit in another worktree was repaired in the primary tree (2026-11-23, 22 files under a live run).
    # The file itself, resolved: an edit through a TODO symlink changes the target, so its tree is the one repaired.
    here = os.path.dirname(os.path.realpath(path))
    while not os.path.isdir(here) and os.path.dirname(here) != here:
        here = os.path.dirname(here)                   # a Write may have named a directory not yet on disk
    try:
        root = subprocess.check_output(
            ["git", "-C", here, "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL, timeout=_left(deadline),
        ).decode().strip()
    except subprocess.TimeoutExpired:
        return _not_run(f"git rev-parse timed out inside the hook's {BUDGET_S}s budget")
    except Exception:  # fail-direction: a file outside any repository has no stamps to repair
        return 0
    # Only a registered worktree of the repository that installed this hook: the validator is code from the edited
    # tree, so an edit in an unrelated checkout, or in a tree that only points at this repository's metadata, must
    # never run that tree's code.
    try:
        registered = _registered_worktrees(os.path.dirname(os.path.abspath(__file__)), deadline)
    except subprocess.TimeoutExpired:
        return _not_run(f"git worktree list timed out inside the hook's {BUDGET_S}s budget")
    if os.path.realpath(root) not in registered:
        return 0

    validator = root + "/scripts/todo-graph/validate.py"
    if not os.path.exists(validator):
        return 0

    if deadline - time.monotonic() < MIN_REPAIR_S:
        return _not_run(f"the git lookups spent the hook's {BUDGET_S}s budget")
    try:
        result = _run_group(["python3", validator, "--fix-line-numbers", "--write", "--quiet"],
                            root, _left(deadline))
    except subprocess.TimeoutExpired:
        msg = message_for(None, "", "", timed_out=True)
    except Exception as e:  # fail-direction: a repair that never ran is reported, not hidden
        msg = (f"{PREFIX} could not be started ({type(e).__name__}: {_clip(str(e))}), "
               "so stamp line numbers were not checked.")
    else:
        msg = message_for(result.returncode, result.stdout, result.stderr)
    if msg:
        print(json.dumps({"systemMessage": msg}))
    return 0


# The harness allows this hook 20 s (.claude/settings.json); every step shares BUDGET_S of it, the rest is kept
# to print the result. A repair with less than MIN_REPAIR_S left is reported as not run.
BUDGET_S = 17
MIN_REPAIR_S = 2
TIMEOUT_S = BUDGET_S
PREFIX = "[todo-graph auto-rewrite] validate.py --fix-line-numbers"
SUMMARY = re.compile(r"\[validate\.py\] fix-line-numbers: \d+ update\(s\), .*")


def _clip(text: str, n: int = 240) -> str:
    return text.strip().replace(chr(10), " ")[:n]


def message_for(rc, stdout: str, stderr: str, timed_out: bool = False):
    """The systemMessage for one repair run, or None when nothing needs a person."""
    combined = (stdout or "") + (stderr or "")
    if timed_out:
        return (f"{PREFIX} did not finish within {TIMEOUT_S}s, so stamp line "
                "numbers were not checked. Run it by hand: python3 "
                "scripts/todo-graph/validate.py --fix-line-numbers --write")
    if rc == 0:
        return None
    m = SUMMARY.search(combined)
    if rc == 1 and m:
        # The summary line is printed only when the repair pass finished, so
        # an rc 1 without it is an ordinary crash (Python exits 1 on an
        # uncaught exception), not an incomplete repair.
        fails = [ln for ln in combined.splitlines() if "FAIL fix-line-numbers:" in ln]
        first = f" First: {_clip(fails[0], 200)}" if fails else ""
        return (f"{PREFIX} left stamps it could not repair ({_clip(m.group(0))}).{first} "
                "Fix each named stamp parenthetical by hand; the safe rewrites were applied.")
    if rc == 2:
        # A refusal can come after earlier files were already rewritten
        # (validate.py's exit contract); its ALREADY REWRITTEN list is the
        # recovery evidence, so it is quoted whole.
        done = [ln for ln in combined.splitlines() if "ALREADY REWRITTEN" in ln]
        # Never clipped: it is bounded by the corpus's file count, and a
        # rerun cannot recover the previous run's write history.
        partial = (" Some files may already be rewritten: "
                   + done[0][done[0].index("ALREADY REWRITTEN"):].strip() if done else "")
        return (f"{PREFIX} REFUSED: {_clip(combined)}.{partial} "
                "Resolve the refusal, then re-run it.")
    return (f"{PREFIX} failed (exit {rc}): {_clip(combined) or 'no output'}. "
            "It did not finish, so which stamps it repaired is unknown.")


if __name__ == "__main__":
    sys.exit(main())
