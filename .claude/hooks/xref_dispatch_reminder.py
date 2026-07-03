#!/usr/bin/env python3
# block-via: warning-only (PostToolUse reminder; never blocks)
"""PostToolUse: xref-dependency-mapper dispatch reminder.

implement-todo-section step 2 says "3+ cross-TODO XREFs -> dispatch
xref-dependency-mapper BY DEFAULT", but it was text-only: the 2026-07-02
overnight run made 0 dispatches in 20h while several sections resolved 3+
cross-TODO owners through inline grep chains (log:24-36, 2672-2679,
5225-5310, 6532-6548). This hook counts DISTINCT foreign TODO-NN numbers
appearing in tool inputs while an implement-todo-section pass is live and
reminds ONCE at 3+ when no fresh xref-dependency-mapper dispatch exists.

State: .claude/state/xref-scan.json {key: session+todo, nums: [...],
reminded: bool}. Reset when the live implement entry changes. Reminder-only;
fail-open on any error.

Selftest: python3 xref_dispatch_reminder.py --selftest
"""
from __future__ import annotations

import json
import re
import sys
import time
from pathlib import Path

TODO_NUM_RE = re.compile(r"\bTODO-(\d+)\b")
THRESHOLD = 3
FRESH_NS = 2 * 3600 * 1_000_000_000

_MSG = (
    "[xref-dispatch -- not a block] This implement-todo-section pass has now "
    "touched {n} distinct cross-TODO references ({nums}) via inline tool "
    "calls with no xref-dependency-mapper dispatch. Step 2 doctrine: 3+ "
    "cross-TODO XREFs -> Agent(subagent_type=\"xref-dependency-mapper\") BY "
    "DEFAULT -- it returns the dependency-status brief without pulling the "
    "target TODO files into this context. (Measured: 0 dispatches in the "
    "20h 2026-07-02 run despite 4 qualifying sections.)"
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _state_path(root: Path) -> Path:
    return root / ".claude/state/xref-scan.json"


def _live_implement_entry(root: Path, session_id: str):
    try:
        prog = json.loads((root / ".claude/state/skill-progress.json").read_text())
    except Exception:
        return None
    entry = prog.get("implement-todo-section")
    if not isinstance(entry, dict) or entry.get("compaction_orphaned"):
        return None
    if session_id and entry.get("session_id") not in ("", None, session_id):
        return None
    return entry


def _own_todo_num(entry: dict) -> str:
    m = TODO_NUM_RE.search(entry.get("args") or "")
    return m.group(1) if m else ""


def _tool_text(ti: dict) -> str:
    parts = []
    for k in ("command", "pattern", "file_path", "path", "prompt"):
        v = ti.get(k)
        if isinstance(v, str):
            parts.append(v)
    return " ".join(parts)


def _mapper_fresh(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude/state/last-agent-dispatch.json").read_text())
        ts = ((st.get("by_type") or {}).get("xref-dependency-mapper") or {}).get(
            "timestamp_ns")
        return isinstance(ts, int) and (time.time_ns() - ts) <= FRESH_NS
    except Exception:
        return False


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    tool = d.get("tool_name") or ""
    if tool not in ("Bash", "Grep", "Read", "Glob"):
        return 0
    root = _repo_root()
    entry = _live_implement_entry(root, d.get("session_id") or "")
    if entry is None:
        return 0
    own = _own_todo_num(entry)
    text = _tool_text(d.get("tool_input") or {})
    nums = {n for n in TODO_NUM_RE.findall(text) if n != own}
    if not nums:
        return 0
    key = f"{entry.get('session_id','')}:{own}"
    sp = _state_path(root)
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict) or st.get("key") != key:
            st = {}
    except Exception:
        st = {}
    seen = set(st.get("nums") or []) | nums
    reminded = bool(st.get("reminded"))
    warn = (len(seen) >= THRESHOLD and not reminded
            and not _mapper_fresh(root))
    if warn:
        reminded = True
    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(
            {"key": key, "nums": sorted(seen), "reminded": reminded}))
    except Exception:
        pass
    if warn:
        print(json.dumps({"systemMessage": _MSG.format(
            n=len(seen), nums=", ".join("TODO-" + x for x in sorted(seen)[:6]))}))
    return 0


def _selftest() -> int:
    import contextlib
    import io
    import tempfile
    fails = []

    def check(name, cond):
        if not cond:
            fails.append(name)

    tmp = Path(tempfile.mkdtemp())
    (tmp / ".claude/state").mkdir(parents=True)
    global _repo_root
    orig = _repo_root
    _repo_root = lambda: tmp

    def run(tool, ti, sid="sess-1"):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps(
                {"tool_name": tool, "session_id": sid, "tool_input": ti}))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                main()
            return buf.getvalue().strip()
        finally:
            sys.stdin = old

    # no live implement entry -> silent.
    check("no-entry-silent",
          run("Bash", {"command": "grep TODO-05 todo/"}) == "")

    (tmp / ".claude/state/skill-progress.json").write_text(json.dumps({
        "implement-todo-section": {"session_id": "sess-1",
                                   "args": "TODO-13 section 4 in todo/..."}}))

    # 1st + 2nd distinct foreign numbers: silent.
    check("first-silent", run("Bash", {"command": "grep TODO-05 x"}) == "")
    check("second-silent", run("Grep", {"pattern": "TODO-12 owner"}) == "")
    # own number does not count.
    check("own-ignored", run("Bash", {"command": "cat TODO-13"}) == "")
    # 3rd distinct -> reminder fires once.
    out = run("Read", {"file_path": "todo/x/TODO-22-env.md"})
    check("third-fires", "xref-dispatch" in out and "3 distinct" in out)
    # 4th: already reminded -> silent.
    check("fourth-silent", run("Bash", {"command": "grep TODO-14 y"}) == "")

    # fresh mapper dispatch suppresses the reminder for a new pass.
    (tmp / ".claude/state/skill-progress.json").write_text(json.dumps({
        "implement-todo-section": {"session_id": "sess-1",
                                   "args": "TODO-20 section 1"}}))
    (tmp / ".claude/state/last-agent-dispatch.json").write_text(json.dumps({
        "by_type": {"xref-dependency-mapper": {"timestamp_ns": time.time_ns()}}}))
    for n in ("05", "12", "22", "14"):
        out = run("Bash", {"command": f"grep TODO-{n} z"})
    check("mapper-suppresses", out == "")

    _repo_root = orig
    if fails:
        sys.stderr.write("xref_dispatch_reminder selftest FAIL: "
                         + "; ".join(fails) + "\n")
        return 1
    print("xref_dispatch_reminder selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
