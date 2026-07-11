#!/usr/bin/env python3
"""advance-work.py -- assemble ONE bounded work packet BEFORE Opus starts.

A fresh or rolled-over worker used to spend its first 6-10 main-loop requests on
state-machine mechanics: run status, preflight, triage, phase selection, cursor
resolution, section classification, manifest generation. All of it is
deterministic and can run before a single model token is spent. This composes
the existing deterministic tools into one packet:

  oracle (sequencer_triage --next)  -> what to do (work / fixpoint / blocked)
  classify (sequencer_triage --classify) -> the next open section + phase
  section-pack.py                   -> the section's orientation pack
  section-checkpoint.py show        -> settled facts from the prior session
  preflight stamp                   -> the last preflight verdict

The Opus session starts from this packet instead of deriving it turn by turn.
Read-only and side-effect-light (it does generate the section pack, which is
itself a cache-keyed artifact); it never edits source, builds, or commits.

Usage: advance-work.py [--project DIR] [--no-pack]
Output: one bounded JSON packet on stdout.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOOKS = HERE.parent.parent / ".claude/hooks"


def _run(cmd, cwd, timeout=120):
    try:
        return subprocess.run(cmd, cwd=str(cwd), capture_output=True,
                              text=True, timeout=timeout)
    except Exception:
        return None


def _json_out(cmd, cwd, timeout=120):
    r = _run(cmd, cwd, timeout)
    if not r or r.returncode not in (0, 1):
        return None
    try:
        return json.loads(r.stdout)
    except Exception:
        return None


def main(argv) -> int:
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    do_pack = "--no-pack" not in argv
    triage = str(HOOKS / "sequencer_triage.py")

    packet: dict = {"action": "work"}

    # 1. oracle: what to do next
    nxt = _json_out(["python3", triage, "--next"], root, timeout=60) or {}
    status = nxt.get("status")
    if status == "DONE":
        packet["action"] = "fixpoint"
        packet["note"] = ("oracle reports DONE -- attempt "
                          "run_phase_guard.py fixpoint (it re-verifies)")
        print(json.dumps(packet, indent=1))
        return 0
    if status == "BLOCKED":
        packet["action"] = "blocked"
        packet["note"] = ("only recoverable deferrals remain; the launcher heal "
                          "probe / watchdog backoff governs -- no fresh section")
        packet["oracle"] = nxt
        print(json.dumps(packet, indent=1))
        return 0

    todo = nxt.get("file")
    packet["cursor"] = {"file": todo, "domain": nxt.get("domain"),
                        "stages_1_2_done": nxt.get("stages_1_2_done")}
    if not todo:
        packet["action"] = "unknown"
        packet["oracle"] = nxt
        print(json.dumps(packet, indent=1))
        return 0

    # 2. classify -> the first section that still needs work
    cls = _json_out(["python3", triage, "--classify", todo], root, timeout=60) or {}
    sections = cls.get("sections") or []
    nxt_sec = next((s for s in sections
                    if s.get("class") not in ("DONE", "DEFERRED")), None)
    packet["sections_summary"] = {
        "total": len(sections),
        "done": sum(1 for s in sections if s.get("class") == "DONE"),
        "deferred": sum(1 for s in sections if s.get("class") == "DEFERRED"),
    }
    if nxt_sec:
        packet["next_section"] = nxt_sec

    # 3. section pack for the next open section (the big orientation win)
    if do_pack and nxt_sec:
        pk = _json_out(["python3", str(HERE / "section-pack.py"), todo,
                        str(nxt_sec["n"]), "--project", str(root)], root,
                       timeout=120)
        if pk:
            packet["section_pack"] = pk  # already a bounded summary

    # 4. durable checkpoint from the prior session (if still valid)
    cp = _json_out(["python3", str(HERE / "section-checkpoint.py"), "show",
                    "--project", str(root)], root, timeout=30)
    if cp and cp.get("checkpoint") and not cp.get("stale"):
        packet["checkpoint"] = cp["checkpoint"]
    elif cp and cp.get("stale"):
        packet["checkpoint_stale"] = True

    # 5. last preflight verdict (cached stamp; do NOT re-run the build here)
    try:
        stamp = json.loads(
            (root / ".claude/overnight/state/preflight-stamp.json").read_text())
        packet["preflight"] = {"summary": stamp.get("summary"),
                               "key": (stamp.get("key") or "")[:12]}
    except Exception:
        packet["preflight"] = {"summary": "no cached preflight stamp"}

    print(json.dumps(packet, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
