#!/usr/bin/env python3
# block-via: warning-only (PostToolUse reminder; never blocks)
"""PostToolUse (Read): whole-file re-read reminder for big files.

Measured waste shape (run-20260702-141810.log): the active TODO (1441
lines) was fully Read 59 times and task.c (3216 lines) 40 times in one
run -- several back-to-back within the same second -- because the Edit
tool's read-freshness gate keeps forcing re-reads and each one happened
whole-file. The FIRST full read of a file is legitimate orientation; the
waste is the whole-file RE-read. A slice read (offset/limit around the
region being edited) satisfies the Edit freshness gate just as well.

Fires when: tool is Read, no offset/limit given, on-disk size exceeds
BIG_BYTES, and the SAME path was already read within WINDOW. Reminds once
per path per window. State: .claude/state/read-history.json (bounded).
Fail-open on any error.

Selftest: python3 slice_read_reminder.py --selftest
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

BIG_BYTES = 32 * 1024          # ~800+ lines of typical source
WINDOW_NS = 30 * 60 * 1_000_000_000
MAX_ENTRIES = 64

_MSG = (
    "[slice-read -- not a block] Whole-file re-read of {path} ({kb} KB), "
    "already read {ago}s ago. Re-reads exist to satisfy the Edit freshness "
    "gate -- a slice read does that too: Read(offset, limit) around the "
    "section / IO-table row / function being edited, and after an edit "
    "re-read only the mutated slice. (Measured: the active TODO was fully "
    "re-read 59x and a 3216-line source 40x in one overnight run.)"
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _state_path() -> Path:
    return _repo_root() / ".claude/state/read-history.json"


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Read":
        return 0
    ti = d.get("tool_input") or {}
    fp = ti.get("file_path") or ""
    if not fp:
        return 0
    now = time.time_ns()
    sp = _state_path()
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict):
            st = {}
    except Exception:
        st = {}

    ent = st.get(fp) or {}
    prev_ts = ent.get("ts") if isinstance(ent.get("ts"), int) else 0
    warned_ts = ent.get("warned") if isinstance(ent.get("warned"), int) else 0

    sliced = ti.get("offset") is not None or ti.get("limit") is not None
    warn = False
    if not sliced:
        try:
            big = os.path.getsize(fp) > BIG_BYTES
        except Exception:
            big = False
        if big and prev_ts and (now - prev_ts) <= WINDOW_NS \
           and (now - warned_ts) > WINDOW_NS:
            warn = True

    st[fp] = {"ts": now, "warned": now if warn else warned_ts}
    if len(st) > MAX_ENTRIES:
        for k, _ in sorted(st.items(),
                           key=lambda kv: kv[1].get("ts", 0))[: len(st) - MAX_ENTRIES]:
            st.pop(k, None)
    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(st))
    except Exception:
        pass

    if warn:
        ago = (now - prev_ts) // 1_000_000_000
        try:
            kb = os.path.getsize(fp) // 1024
        except Exception:
            kb = 0
        print(json.dumps({"systemMessage": _MSG.format(
            path=os.path.basename(fp), kb=kb, ago=ago)}))
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
    big = tmp / "big.md"
    big.write_text("x" * (BIG_BYTES + 1))
    small = tmp / "small.md"
    small.write_text("x" * 100)

    global _repo_root
    orig = _repo_root
    _repo_root = lambda: tmp

    def run(ti):
        old = sys.stdin
        try:
            sys.stdin = io.StringIO(json.dumps(
                {"tool_name": "Read", "tool_input": ti}))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                main()
            return buf.getvalue().strip()
        finally:
            sys.stdin = old

    check("first-full-read-silent", run({"file_path": str(big)}) == "")
    out = run({"file_path": str(big)})
    check("second-full-read-warns", "slice-read" in out)
    check("third-read-not-renagged", run({"file_path": str(big)}) == "")
    check("small-file-silent", run({"file_path": str(small)}) == "" and
          run({"file_path": str(small)}) == "")
    # slice reads never warn and still refresh the timestamp
    big2 = tmp / "big2.md"
    big2.write_text("x" * (BIG_BYTES + 1))
    run({"file_path": str(big2)})
    check("slice-read-silent",
          run({"file_path": str(big2), "offset": 100, "limit": 40}) == "")
    check("missing-file-silent", run({"file_path": str(tmp / "gone.md")}) == ""
          and run({"file_path": str(tmp / "gone.md")}) == "")

    _repo_root = orig
    if fails:
        sys.stderr.write("slice_read_reminder selftest FAIL: "
                         + "; ".join(fails) + "\n")
        return 1
    print("slice_read_reminder selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
