#!/usr/bin/env python3
# block-via: warning-only (SessionStart context injection; never blocks)
"""SessionStart hook -- re-inject the situational brief after a compaction/resume.

On source in {compact, resume} for an ACTIVE overnight run, recompute the
runner_status brief and emit it as a systemMessage so the post-compaction context
starts oriented. Interactive sessions (no active run) get nothing. Fail-open.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path


def _repo_root() -> Path | None:
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def brief_for_event(event: dict, root: Path) -> str | None:
    if not isinstance(event, dict):
        return None
    if event.get("source") not in ("compact", "resume"):
        return None
    try:
        st = json.loads(
            (root / ".claude" / "state" / "sequencer-run.json").read_text(encoding="utf-8"))
    except Exception:
        return None
    if not st.get("active"):
        return None
    try:
        sys.path.insert(0, str(root / ".claude" / "hooks"))
        import runner_status
        brief = runner_status.full_brief(root)
    except Exception:
        return None
    cp = checkpoint_block(root, st)
    return brief + cp if cp else brief


def checkpoint_block(root: Path, run: dict) -> str:
    """T1-3 (1c): surface the enriched section checkpoint in the resume brief.

    `section-checkpoint.py gather()` already captures the mid-section reasoning
    state a resumed worker would otherwise re-derive -- current phase, open
    Codex findings with their triage decisions, whether a review is still
    outstanding, the section pack path, and a derived next_action (the P4.2
    enrichment). NOTHING READ IT. `full_brief` recomputes run state from
    scratch and never opened the checkpoint, so every one of those fields was
    dead weight and each resumed session paid to re-derive facts already on
    disk. That is the whole cost case for the checkpoint.

    BOUND TO THE CURRENT CURSOR. A checkpoint written for a different TODO or a
    different section is worse than none -- it would hand the worker confident
    stale facts about work it is not doing. On any mismatch, or any read error,
    emit nothing and let `full_brief` stand alone.
    """
    try:
        cp = json.loads(
            (root / ".claude" / "state" / "section-checkpoint.json")
            .read_text(encoding="utf-8"))
    except Exception:
        return ""
    if not isinstance(cp, dict):
        return ""
    cur = cp.get("cursor")
    if not isinstance(cur, dict):
        return ""
    # Same file AND same section index, or the checkpoint is not about this work.
    if (cur.get("file") or "") != (run.get("file") or ""):
        return ""
    if cur.get("section_idx") != run.get("section_idx"):
        return ""

    lines = ["", "[resume checkpoint -- state already on disk, do NOT re-derive]"]
    if cp.get("phase"):
        lines.append(f"  phase: {cp['phase']}")
    if cp.get("next_action"):
        lines.append(f"  next action: {cp['next_action']}")
    if cp.get("outstanding_review") is True:
        lines.append("  a Codex review is OUTSTANDING (not yet received)")
    pack = cp.get("section_pack")
    if isinstance(pack, dict) and pack.get("pack_path"):
        fresh = "fresh" if pack.get("fresh") else "STALE -- re-run section-pack.py"
        lines.append(f"  section pack ({fresh}): {pack['pack_path']}")
    finds = cp.get("open_findings")
    if isinstance(finds, list) and finds:
        total = cp.get("findings_recorded", len(finds))
        lines.append(f"  findings on record for this TODO: {total}"
                     + (f" (newest {len(finds)} below)" if total > len(finds) else ""))
        for f in finds:
            if not isinstance(f, dict):
                continue
            loc = f.get("loc") or "?"
            dec = f.get("decision") or "open"
            lines.append(f"    - [{dec}] {loc} {(f.get('title') or '')[:80]}")
    if cp.get("decisions_indexed"):
        lines.append(f"  settled decisions indexed: {cp['decisions_indexed']} "
                     f"(query via scripts/overnight/decision-registry.py)")
    # Only emit when there is something beyond the header worth reading.
    return "\n".join(lines) if len(lines) > 2 else ""


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    root = _repo_root()
    if root is None:
        return 0
    brief = brief_for_event(d, root)
    if brief:
        print(json.dumps({"systemMessage":
                          "[post-compaction re-orient -- situational brief]\n" + brief}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
