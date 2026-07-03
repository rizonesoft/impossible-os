#!/usr/bin/env python3
"""Runner-improvement -> kit-sync delegation reminder (PostToolUse).

The canonical runner architecture lives at ~/runner-kit; this project runs
vendored copies. Whenever an Edit/Write/MultiEdit touches a runner-owned file,
inject a reminder to dispatch Agent(subagent_type="kit-sync") -- which runs
`bash ~/runner-kit/bin/kit-diff.sh /home/derickpayne/impossible-os` -- for a
read-only two-way drift report against ~/runner-kit (design authority). This
hook is reminder-only: it never blocks and it never upstreams anything itself;
kit-sync is drift-report-only too (upstreaming stays the operator's manual
ritual in the kit repo). Stdlib only. Always exits 0.

Selftest: --selftest runs 3 cases (runner-owned path fires, unrelated path
silent, malformed stdin silent) with no real stdin/hook plumbing required.
"""
from __future__ import annotations

import json
import re
import sys

# Runner-owned surfaces (mirrors the kit's core/ + doctrine). Deliberately NOT
# .claude/state/ (run artifacts) or todo/*.md files other than the doctrine
# file itself (other todo edits are a separate concern, not runner-kit drift).
_RUNNER_PATH_RE = re.compile(
    r"(\.claude/(?:hooks|agents|skills/overnight-sequencer)/|scripts/overnight/|"
    r"todo/TODO-Claude-Overnight-Runner\.md$|\.claude/settings\.json$)"
)


def _reminder_for(file_path: str) -> dict | None:
    if not _RUNNER_PATH_RE.search(file_path.replace("\\", "/")):
        return None
    return {
        "hookSpecificOutput": {
            "hookEventName": "PostToolUse",
            "additionalContext": (
                f"KIT-SYNC RULE: {file_path} is a runner-owned file and was just "
                "edited. Before ending the turn, dispatch Agent(subagent_type="
                "\"kit-sync\") to run the ~/runner-kit drift report. kit-sync is "
                "read-only/report-only; if the drift report surfaces a genuine "
                "improvement, upstream it by hand in ~/runner-kit (copy the file, "
                "add a LESSONS.md entry if it is a new law, bump VERSION, commit). "
                "One reminder covers all runner edits this turn."
            ),
        }
    }


def main() -> int:
    if "--selftest" in sys.argv:
        return _selftest()
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0

    if (event.get("tool_name") or "") not in ("Edit", "Write", "MultiEdit"):
        return 0

    file_path = (event.get("tool_input") or {}).get("file_path") or ""
    out = _reminder_for(file_path)
    if out is None:
        return 0

    print(json.dumps(out))
    return 0


def _selftest() -> int:
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    # 1. runner-owned path fires (several representative surfaces).
    for p in (
        "/home/derickpayne/impossible-os/.claude/hooks/sequencer_triage.py",
        "/home/derickpayne/impossible-os/.claude/agents/kit-sync.md",
        "/home/derickpayne/impossible-os/.claude/skills/overnight-sequencer/arm-sequencer.sh",
        "/home/derickpayne/impossible-os/scripts/overnight/notify.sh",
        "/home/derickpayne/impossible-os/todo/TODO-Claude-Overnight-Runner.md",
        "/home/derickpayne/impossible-os/.claude/settings.json",
    ):
        out = _reminder_for(p)
        check(f"fires:{p}", out is not None and "KIT-SYNC RULE" in out["hookSpecificOutput"]["additionalContext"])

    # 2. unrelated path silent.
    for p in (
        "/home/derickpayne/impossible-os/src/kernel/mm/pmm.c",
        "/home/derickpayne/impossible-os/todo/02-kernel-core/TODO-12-native-api-ssdt.md",
        "/home/derickpayne/impossible-os/.claude/state/live-gotchas.md",
        "/home/derickpayne/impossible-os/docs/infrastructure/ai-system.md",
    ):
        check(f"silent:{p}", _reminder_for(p) is None)

    # 3. malformed stdin silent (simulated via main()'s json.load failure path;
    #    exercised directly here since main() reads real sys.stdin).
    class _BadStdin:
        def read(self):
            raise ValueError("not json")

    old_stdin = sys.stdin
    old_argv = sys.argv
    try:
        sys.stdin = _BadStdin()  # type: ignore[assignment]
        sys.argv = ["kit_sync_reminder.py"]
        rc = main()
        check("malformed-stdin-exit0", rc == 0)
    finally:
        sys.stdin = old_stdin
        sys.argv = old_argv

    if failures:
        print("selftest FAIL: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
