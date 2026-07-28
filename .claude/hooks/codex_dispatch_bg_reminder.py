#!/usr/bin/env python3
# block-via: warning-only (the command is CORRECT, just costly -- never blocks)
"""PreToolUse (Bash): backgrounding a Codex dispatch echoes its whole prompt back.

MEASURED 2026-07-28 (token-saver v02) on the costliest canary segment,
11 `review-broker-codex-dispatch.sh` calls split by invocation shape:

    shape                          n   prompt chars   RESULT chars   avg result
    `... & wait`                   4         19,185         19,740        4,935
    `... 2>&1 | tail -N`           7         17,719          5,960          851

Same script, same work, 5.8x the result volume. The mechanism is bash job
control: `&` makes the dispatch a background job, and the completion notice
prints the ENTIRE command line back -- and a review dispatch's command line is
a 2-5 KB Codex prompt. So the prompt is paid TWICE, once as the tool_use input
and again echoed in the tool result, and every one of those tokens is then
re-read on every remaining turn of the segment (cache-read is quadratic in
segment length -- see cost-summary.py).

The `&` buys nothing. review-broker-codex-dispatch.sh already detaches the
review into its own `systemd-run` transient scope and returns immediately after
printing one JSON line; there is no foreground work to background. The piped
shape above is the same dispatch with the receipt kept and the echo dropped.

WHY NOT BUNDLE THE KINDS INSTEAD: that is explicitly forbidden. The broker's
header records "Never bundle several dispatches behind one opaque shell
command: the recorder can only attribute what is on the Bash command line" --
per-kind gate receipts (`.claude/hooks/_codex_dispatch.py`) parse the command
line, so each kind must stay its own visible invocation. Sequential piped
dispatches keep that attribution; `& wait` is not needed to run them together
because each returns immediately anyway.

WARNING-ONLY, deliberately, for the same reason as cd_prefix_reminder: the
backgrounded command is CORRECT -- it dispatches, it registers, the review
runs. Blocking a correct dispatch would trade a real review for a token saving,
and hooks here cannot rewrite `tool_input` (no hook in this repo modifies it;
the only documented envelope is `permissionDecision: deny`, a block).

BUDGETED via `_advisory_budget` (cap 2/session): the habit is one line long,
and a per-dispatch nag would itself become the cost problem this file describes
(the failure T1-4 measured on `interactive_offload_router`: 487 injections,
zero follows).
"""
from __future__ import annotations

import json
import os
import re
import sys
from pathlib import Path

# A Codex dispatch of any flavour: the broker leg, or the foreground wrapper.
_DISPATCH_RE = re.compile(r"\bcodex-dispatch\.sh\b")
# Backgrounding followed by a bare `wait`. The negative look-around keeps `&&`
# out, and `2>&1` cannot match because `&` there is followed by `1`, not `wait`.
_BG_WAIT_RE = re.compile(r"(?<!&)&(?!&)\s*(?:;)?\s*wait\b")

_MSG = (
    "[codex-dispatch backgrounding -- not a block] Dropping the `&` + `wait` "
    "around a Codex dispatch saves ~4 KB of context per call. Bash's job-"
    "completion notice prints the whole command line back, and a dispatch's "
    "command line IS the 2-5 KB review prompt -- so you pay it twice and re-read "
    "it on every remaining turn. Measured 2026-07-28 on one segment: `& wait` "
    "averaged 4,935 result chars against 851 for `2>&1 | tail -1`, same script, "
    "same work. The `&` buys nothing: the broker already detaches the review "
    "into its own systemd-run scope and returns immediately. Use one piped "
    "dispatch per kind, e.g. `bash scripts/overnight/review-broker-codex-"
    "dispatch.sh '<prompt>' 2>&1 | tail -1`, and keep each kind on its own "
    "command line so its gate receipt still attributes."
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
    cmd = str((d.get("tool_input") or {}).get("command") or "")
    if not _DISPATCH_RE.search(cmd) or not _BG_WAIT_RE.search(cmd):
        return 0
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import _advisory_budget
        if not _advisory_budget.should_emit(
                _repo_root(), "codex_dispatch_bg_reminder", cap=2,
                session_id=str(d.get("session_id") or "")):
            return 0
    except Exception:
        pass          # fail-open: a budget bug must never silence the hook
    print(json.dumps({"systemMessage": _MSG}))
    return 0


def _selftest() -> int:
    import contextlib
    import io
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    B = "bash scripts/overnight/review-broker-codex-dispatch.sh"

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

    # Warns on the measured costly shape, in each spelling seen in the logs.
    check("amp-newline-wait-warns", "backgrounding" in run(f"{B} 'p' &\nwait", "a")[1])
    check("amp-space-wait-warns", "backgrounding" in run(f"{B} 'p' & wait", "b")[1])
    check("two-legs-one-wait-warns",
          "backgrounding" in run(f"{B} 'p1' &\n{B} 'p2' &\nwait", "c")[1])
    check("foreground-wrapper-warns",
          "backgrounding" in run("bash scripts/codex-dispatch.sh 'p' & wait", "d")[1])

    # Silent on the CHEAP shape and on everything unrelated.
    check("piped-silent", run(f"{B} 'p' 2>&1 | tail -1", "e")[1] == "")
    check("bare-silent", run(f"{B} 'p'", "f")[1] == "")
    check("and-and-silent", run(f"{B} 'p' && echo ok", "g")[1] == "")
    check("redirect-not-mistaken-for-bg", run(f"{B} 'p' 2>&1 | tail -2", "h")[1] == "")
    check("unrelated-bg-silent", run("bash scripts/build.sh &\nwait", "i")[1] == "")
    check("not-bash-tool-silent", run("", "j")[1] == "")

    # NEVER blocks: every path returns 0.
    for c in (f"{B} 'p' & wait", f"{B} 'p'", "bash scripts/build.sh &\nwait"):
        check(f"never-blocks: {c[:20]}", run(c, "k")[0] == 0)

    # Budgeted: a session goes quiet after the cap, a fresh session speaks again.
    outs = [run(f"{B} 'p{i}' & wait", session="budget")[1] for i in range(5)]
    check("budget-caps-at-2", sum(1 for o in outs if o) == 2)
    check("budget-per-session", run(f"{B} 'p' & wait", session="fresh")[1] != "")

    if fails:
        sys.stderr.write("codex_dispatch_bg_reminder selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("codex_dispatch_bg_reminder selftest OK")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(_selftest() if "--selftest" in sys.argv else main())
    except Exception:
        sys.exit(0)
