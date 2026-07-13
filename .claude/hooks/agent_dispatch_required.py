#!/usr/bin/env python3
# block-via: exit 2 (kernel/boot source in overnight SECTIONS phase only; WARN elsewhere)
"""PreToolUse: enforce agent-dispatch discipline for overnight SECTIONS-phase
source edits. With no recent read-only agent dispatch: BLOCK (exit 2) for
kernel/boot source (.c/.h/.asm/.S under src/ or include/), WARN for other
source. Invisible in interactive sessions; fail-open on any error. BLOCK
promotion authorized 2026-07-02 (operator instruction; was WARN-first WS1b).
Escape: SKIP_AGENT_DISPATCH_HOOK=1 for genuinely tiny mechanical edits.
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

FRESH_NS = 1800 * 1_000_000_000  # 30 min


def _is_source_target(path: str) -> bool:
    if not path:
        return False
    if path.endswith(".md"):
        return False
    for seg in (".claude/", "build/", "docs/"):
        if seg in path:
            return False
    return path.endswith((".c", ".h", ".asm", ".S", ".py", ".sh"))


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _guard_in_sections(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "sequencer-run.json").read_text())
    except Exception:
        return False
    return bool(st.get("active")) and st.get("phase") == "SECTIONS"


def _pass_started_ts(root: Path) -> int:
    """started_ts of the live implement-todo-section pass, or 0.

    Section-pass binding: a dispatch made at the top of the section (skill
    step 3) must stay valid for the WHOLE pass -- the fixed 30-min TTL alone
    re-blocked long sections mid-pass (~once per section, measured 23x in
    one run) even though the mandated dispatch had happened.
    """
    try:
        prog = json.loads((root / ".claude" / "state" / "skill-progress.json").read_text())
        entry = prog.get("implement-todo-section")
        if isinstance(entry, dict) and not entry.get("compaction_orphaned"):
            ts = entry.get("started_ts")
            if isinstance(ts, int):
                return ts
    except Exception:
        pass
    return 0


def _recent_dispatch(root: Path) -> bool:
    try:
        st = json.loads((root / ".claude" / "state" / "last-agent-dispatch.json").read_text())
    except Exception:
        return False
    ts = st.get("timestamp_ns")
    if not isinstance(ts, int):
        return False
    if (time.time_ns() - ts) <= FRESH_NS:
        return True
    # Older than the TTL but newer than the live section pass start: the
    # step-3 dispatch covers its own section regardless of section length.
    pass_start = _pass_started_ts(root)
    return pass_start > 0 and ts >= pass_start


def _fresh_section_pack(root: Path) -> bool:
    """A section-pack run IS the deterministic equivalent of the discovery
    agent, so a FRESH pack satisfies this gate. 'Fresh' is content-bound: the
    receipt's recorded working-tree digest must still equal a recomputed key
    over the same inputs -- ANY edit since the pack (staged, unstaged, or
    untracked) invalidates it. Fail-closed: a missing/stale/mismatched receipt
    returns False and the agent stays required.

    Also requires the pack to cover the CURRENT cursor TODO file, so a pack for
    an unrelated section can never satisfy the gate."""
    try:
        rc = json.loads((root / ".claude/state/last-section-pack.json").read_text())
    except Exception:
        return False
    key_inputs = rc.get("key_inputs")
    digest = rc.get("digest")
    todo = rc.get("todo") or ""
    if not (isinstance(key_inputs, list) and isinstance(digest, str) and todo):
        return False
    # bind to the section under work: the pack's TODO must be the live cursor's
    try:
        st = json.loads((root / ".claude/state/sequencer-run.json").read_text())
        cursor_file = st.get("file") or ""
    except Exception:
        cursor_file = ""
    if cursor_file and todo != cursor_file:
        return False
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent
                              / "scripts/overnight"))
        from worktree_hash import worktree_key
    except Exception:
        return False
    try:
        return worktree_key(root, key_inputs) == digest
    except Exception:
        return False


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not isinstance(d, dict):
        return 0
    # Invisible outside a headless overnight run.
    if not os.environ.get("OVERNIGHT_SEQUENCER_RUN"):
        return 0
    if os.environ.get("SKIP_AGENT_DISPATCH_HOOK") == "1":
        return 0
    if d.get("tool_name") not in ("Edit", "Write", "MultiEdit"):
        return 0
    ti = d.get("tool_input")
    if not isinstance(ti, dict):
        return 0
    if not _is_source_target(ti.get("file_path") or ti.get("path") or ""):
        return 0
    root = _repo_root()
    if root is None or not _guard_in_sections(root):
        return 0
    if _recent_dispatch(root):
        return 0
    # A fresh, content-bound section-pack is the deterministic equivalent of the
    # discovery agent (WS2) -- accept it in lieu of a dispatch. Fail-closed: a
    # stale/absent/mismatched receipt does not satisfy the gate.
    if _fresh_section_pack(root):
        try:
            import _offload_log
            _offload_log.log_event(root, "receipt-accepted",
                                   "agent_dispatch_required",
                                   "fresh section-pack satisfied the gate")
        except Exception:
            pass
        return 0
    path = ti.get("file_path") or ti.get("path") or ""
    rel = path.split("/impossible-os/", 1)[-1] if "/impossible-os/" in path else path
    kernelish = (
        (rel.startswith(("src/", "include/")) or "/src/" in path or "/include/" in path)
        and rel.endswith((".c", ".h", ".asm", ".S"))
    )
    try:
        import _offload_log
        _offload_log.log_event(root, "fire", "agent_dispatch_required",
                               ("block " if kernelish else "warn ") + rel)
    except Exception:
        pass
    if kernelish:
        sys.stderr.write(
            "[agent-dispatch BLOCK] kernel/boot source edit in the SECTIONS phase "
            "with no read-only agent dispatch in the last 30 min. Dispatch the "
            "explorer/auditor the skill names (kernel-explorer, ssdt-auditor, "
            "test-coverage-mapper, ...) FIRST -- they keep the main context lean "
            "at no quality cost (you verify their findings at file:line). For a "
            "genuinely tiny mechanical edit set SKIP_AGENT_DISPATCH_HOOK=1 and "
            "state the reason in your message.\n"
        )
        return 2
    sys.stderr.write(
        "[agent-dispatch reminder] a source edit in the SECTIONS phase with "
        "no read-only explorer/auditor dispatch in the last 30 min. Default-on "
        "agents keep the main context lean. Dispatch one, or set "
        "SKIP_AGENT_DISPATCH_HOOK=1 with a logged reason if this section is tiny.\n"
    )
    return 0  # WARN for non-kernel source


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:
        sys.exit(0)  # fail-open: a broken gate must never block real work
