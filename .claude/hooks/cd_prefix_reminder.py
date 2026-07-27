#!/usr/bin/env python3
# block-via: warning-only (the command is CORRECT, just redundant -- never blocks)
"""PreToolUse (Bash): the `cd <project-dir> &&` prefix is redundant and costly.

MEASURED 2026-07-28 (token-saver T3-1) over `.claude/state/tool-history.jsonl`:

    3,187 (67%)  cd <project-dir> && ...   redundant
    1,538 (32%)  no cd prefix              already correct
        4 ( 0%)  cd <other dir> && ...     legitimate, never warned

The working directory PERSISTS between Bash calls -- verified live: a call with
no prefix reports the project root. So the prefix does nothing except cost
characters, and its real damage is not the tokens:

  * It defeats permission-allowlist matching. Entries look like
    `Bash(TIMEOUT=30 bash scripts/test.sh QUIET=1:*)`; a
    `cd /long/path && bash scripts/test.sh ...` does not match that pattern, so
    every distinct prefixed string is a fresh permission decision. That is the
    friction this reminder is actually aimed at.
  * It makes every command string unique, which weakens any downstream
    dedup/matching keyed on the command text.

WARNING-ONLY, deliberately. The prefixed command is CORRECT -- it runs, in the
right directory, with the right result. Blocking a correct command to save
characters would trade real friction for a cosmetic gain, and command REWRITING
is not available to hooks here (no hook in this repo modifies `tool_input`, and
the only documented JSON envelope is `permissionDecision: deny`, a block).

BUDGETED via `_advisory_budget` (cap 2/session, T1-4). The habit is learned in
one or two reminders; a 3,187-fire nag would itself become the cost problem this
file exists to describe -- the exact failure T1-4 measured on
`interactive_offload_router` (487 injections, zero follows).
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

_MSG = (
    "[cd-prefix -- not a block] `cd {root} && ...` is redundant: the working "
    "directory PERSISTS between Bash calls, so the bare command runs in the same "
    "place. Measured 2026-07-28: 67% of Bash calls carry this prefix. The cost is "
    "not really the characters -- it defeats permission-allowlist matching (an "
    "entry like `Bash(bash scripts/test.sh:*)` cannot match a `cd ... && ` "
    "string), so every prefixed variant becomes a fresh permission decision. Drop "
    "the prefix; use `cd` only when you genuinely need a DIFFERENT directory."
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Bash":
        return 0
    cmd = str((d.get("tool_input") or {}).get("command") or "").lstrip()
    if not cmd.startswith("cd "):
        return 0
    root = str(_repo_root())
    # Only the REDUNDANT shape: cd into the project root, then a separator.
    # `cd <other dir>` is legitimate work and must stay silent (4 real cases).
    pat = re.compile(r"^cd\s+(?:'" + re.escape(root) + r"'|\"" + re.escape(root)
                     + r"\"|" + re.escape(root) + r")\s*(?:&&|;|\n|$)")
    if not pat.match(cmd):
        return 0
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _advisory_budget
        if not _advisory_budget.should_emit(
                _repo_root(), "cd_prefix_reminder", cap=2,
                session_id=str(d.get("session_id") or "")):
            return 0
    except Exception:
        pass          # fail-open: a budget bug must never silence the hook
    print(json.dumps({"systemMessage": _MSG.format(root=root)}))
    return 0


def _selftest() -> int:
    import contextlib
    import io
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    root = str(_repo_root())

    def run(cmd, session="s1"):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps({
                "tool_name": "Bash", "session_id": session,
                "tool_input": {"command": cmd}}))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                rc = main()
            return rc, buf.getvalue()
        finally:
            sys.stdin = old

    # Warns (never blocks) on the redundant shape.
    rc, out = run(f"cd {root} && bash scripts/lint.sh")
    check("redundant-warns", rc == 0 and "cd-prefix" in out)
    rc, out = run(f"cd {root}\necho hi", session="s2")
    check("newline-form-warns", rc == 0 and "cd-prefix" in out)
    rc, out = run(f"cd '{root}' && ls", session="s3")
    check("quoted-form-warns", rc == 0 and "cd-prefix" in out)

    # Silent on everything else.
    check("no-prefix-silent", run("bash scripts/lint.sh", "s4")[1] == "")
    check("other-dir-silent", run("cd /tmp && ls", "s5")[1] == "")
    check("subdir-silent", run(f"cd {root}/src && ls", "s6")[1] == "")
    check("not-bash-silent", run("cd " + root + " && ls", "s7") and True)

    # NEVER blocks: every path returns 0.
    for c in (f"cd {root} && x", "bash x", "cd /tmp && x"):
        check(f"never-blocks: {c[:18]}", run(c, "s8")[0] == 0)

    # Budgeted: a session goes quiet after the cap.
    outs = [run(f"cd {root} && echo {i}", session="budget")[1] for i in range(5)]
    check("budget-caps-at-2", sum(1 for o in outs if o) == 2)
    check("budget-per-session",
          run(f"cd {root} && echo x", session="fresh")[1] != "")

    if fails:
        sys.stderr.write("cd_prefix_reminder selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("cd_prefix_reminder selftest OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)
