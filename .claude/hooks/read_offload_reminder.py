#!/usr/bin/env python3
# block-via: warning-only (PostToolUse reminder; never blocks -- analyst
# subagents' own Read calls traverse this hook too, and their sessions have
# no sequencer state, so they stay silent by construction)
"""PostToolUse (Read): analyst-offload reminder for overnight bulk reading.

When the overnight sequencer run is ACTIVE and the main loop has fully read
several distinct big files in a short window with no fresh Agent dispatch,
inject one systemMessage pointing at the read-only analyst routes
(kernel-explorer / Explore / todo-validation-mapper / review-evidence-mapper).
Companion to build_offload_reminder.py (heavy COMMANDS) and
slice_read_reminder.py (whole-file RE-reads of one file); this hook covers
bulk exploration ACROSS files, the pattern that cost the retired Codex
runner 8.27M input tokens in a single 16-minute session (2026-07-04
post-mortem: ~120 sequential full-file reads, zero subagent dispatches).

Fires when: tool is Read with no offset/limit, file > BIG_BYTES, sequencer
run active, >= THRESHOLD distinct big files fully read within WINDOW, and no
Agent dispatch within FRESH_NS. Reminds once per WARN_COOLDOWN. State:
.claude/state/read-offload.json (bounded). Fail-open on any error.

Selftest: python3 read_offload_reminder.py --selftest
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

BIG_BYTES = 32 * 1024
WINDOW_NS = 10 * 60 * 1_000_000_000     # bulk-read detection window
FRESH_NS = 15 * 60 * 1_000_000_000      # a dispatch this recent counts
WARN_COOLDOWN_NS = 15 * 60 * 1_000_000_000
THRESHOLD = 4                           # distinct big files fully read
MAX_EVENTS = 32

_MSG = (
    "[read-offload -- not a block] The main context has fully read {n} "
    "distinct big files in the last {win} min with no Agent dispatch. "
    "Bulk exploration belongs in a read-only analyst whose digest you "
    "consume: kernel/boot call-path walks -> Agent(kernel-explorer), "
    "generic multi-file sweeps -> Agent(Explore), TODO structure -> "
    "Agent(todo-validation-mapper), review evidence -> "
    "Agent(review-evidence-mapper). Measured cost of inline bulk reading: "
    "8.27M input tokens in ONE 16-minute session (2026-07-04 post-mortem)."
)


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _state_path() -> Path:
    return _repo_root() / ".claude/state/read-offload.json"


def _run_active(root: Path) -> bool:
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active"))


def _recent_agent_dispatch(root: Path, now: int) -> bool:
    try:
        st = json.loads(
            (root / ".claude" / "state" /
             "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    ts = st.get("timestamp_ns")
    return isinstance(ts, int) and (now - ts) <= FRESH_NS


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict) or d.get("tool_name") != "Read":
        return 0
    ti = d.get("tool_input") or {}
    fp = ti.get("file_path") or ""
    if not fp or ti.get("offset") is not None or ti.get("limit") is not None:
        return 0
    try:
        if os.path.getsize(fp) <= BIG_BYTES:
            return 0
    except Exception:
        return 0
    root = _repo_root()
    if not _run_active(root):
        return 0

    now = time.time_ns()
    sp = _state_path()
    try:
        st = json.loads(sp.read_text())
        if not isinstance(st, dict):
            st = {}
    except Exception:
        st = {}
    events = [e for e in st.get("events") or []
              if isinstance(e, dict) and isinstance(e.get("ts"), int)
              and (now - e["ts"]) <= WINDOW_NS]
    events = [e for e in events if e.get("path") != fp]
    events.append({"path": fp, "ts": now})
    events = events[-MAX_EVENTS:]
    warned = st.get("warned") if isinstance(st.get("warned"), int) else 0

    warn = (len({e["path"] for e in events}) >= THRESHOLD
            and (now - warned) > WARN_COOLDOWN_NS
            and not _recent_agent_dispatch(root, now))

    try:
        sp.parent.mkdir(parents=True, exist_ok=True)
        sp.write_text(json.dumps(
            {"events": events, "warned": now if warn else warned}))
    except Exception:
        pass

    if warn:
        print(json.dumps({"systemMessage": _MSG.format(
            n=len({e["path"] for e in events}),
            win=WINDOW_NS // 60_000_000_000)}))
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

    def set_active(active):
        (tmp / ".claude/state/sequencer-run.json").write_text(
            json.dumps({"active": active, "phase": "SECTIONS"}))

    def mkbig(name):
        p = tmp / name
        p.write_text("x" * (BIG_BYTES + 1))
        return p

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

    bigs = [mkbig(f"big{i}.c") for i in range(5)]

    # inactive run: always silent
    set_active(False)
    for b in bigs[:4]:
        check("inactive-silent", run({"file_path": str(b)}) == "")
    (tmp / ".claude/state/read-offload.json").unlink(missing_ok=True)

    # active run: silent below threshold, warns at the 4th distinct big read
    set_active(True)
    check("1st-silent", run({"file_path": str(bigs[0])}) == "")
    check("2nd-silent", run({"file_path": str(bigs[1])}) == "")
    check("3rd-silent", run({"file_path": str(bigs[2])}) == "")
    out = run({"file_path": str(bigs[3])})
    check("4th-warns", "read-offload" in out)
    check("cooldown-silences", run({"file_path": str(bigs[4])}) == "")

    # re-reading the SAME file does not accumulate distinct paths
    (tmp / ".claude/state/read-offload.json").unlink(missing_ok=True)
    for _ in range(6):
        check("same-file-no-warn", run({"file_path": str(bigs[0])}) == "")

    # a fresh agent dispatch suppresses the warning
    (tmp / ".claude/state/read-offload.json").unlink(missing_ok=True)
    (tmp / ".claude/state/last-agent-dispatch.json").write_text(
        json.dumps({"timestamp_ns": time.time_ns()}))
    for b in bigs:
        check("dispatch-suppresses", run({"file_path": str(b)}) == "")

    # slice reads and small files never count
    (tmp / ".claude/state/read-offload.json").unlink(missing_ok=True)
    (tmp / ".claude/state/last-agent-dispatch.json").unlink(missing_ok=True)
    small = tmp / "small.c"
    small.write_text("x" * 100)
    for b in bigs:
        check("slice-silent",
              run({"file_path": str(b), "offset": 1, "limit": 10}) == "")
        check("small-silent", run({"file_path": str(small)}) == "")

    _repo_root = orig
    if fails:
        sys.stderr.write("read_offload_reminder selftest FAIL: "
                         + "; ".join(sorted(set(fails))) + "\n")
        return 1
    print("read_offload_reminder selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    sys.exit(main())
