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
import subprocess
import sys


def main() -> int:
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

    try:
        root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return 0

    validator = root + "/scripts/todo-graph/validate.py"
    if not os.path.exists(validator):
        return 0

    try:
        result = subprocess.run(
            [
                "python3", validator,
                "--fix-line-numbers",
                "--write",
                "--quiet",
            ],
            cwd=root, capture_output=True, text=True, timeout=TIMEOUT_S,
        )
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


TIMEOUT_S = 15
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
        # recovery evidence, so it is quoted whole rather than clipped away.
        done = [ln for ln in combined.splitlines() if "ALREADY REWRITTEN" in ln]
        partial = (f" Some files may already be rewritten: {_clip(done[0][done[0].index('ALREADY REWRITTEN'):], 600)}"
                   if done else "")
        return (f"{PREFIX} REFUSED: {_clip(combined)}.{partial} "
                "Resolve the refusal, then re-run it.")
    return (f"{PREFIX} failed (exit {rc}): {_clip(combined) or 'no output'}. "
            "It did not finish, so which stamps it repaired is unknown.")


if __name__ == "__main__":
    sys.exit(main())
