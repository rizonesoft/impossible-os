#!/usr/bin/env python3
"""Block the unguarded plugin overnight skills in impossible-os.

This repo's overnight runs MUST be armed via
`.claude/skills/overnight-sequencer/arm-sequencer.sh`, which engages
`run_phase_guard.py` plus the sequencer pipeline (triage -> validate ->
gap-audit -> per-section implement/review -> close -> advance) and defaults
to ChromeMCP-off + watchdog `*:0/10`.

The generic plugin skills `overnight-runner:schedule` and
`overnight-runner:start` launch `/overnight-runner:start --resume`, which
arms NO repo guard: the headless agent final-answers at the first
background-review wait, the `claude -p` process exits, and every watchdog
relaunch repeats the death. This hook makes that wrong path unreachable
in this repo.

Hook category: BLOCKING (sys.exit(2) on hit). PreToolUse / matcher: Skill.
Opt-out: OVERNIGHT_PLUGIN_SKILL_OVERRIDE=1 (for deliberate plugin use in a
non-impossible-os context that somehow routes through these settings).
"""
from __future__ import annotations

import json
import os
import sys

BLOCKED = ("overnight-runner:schedule", "overnight-runner:start")


def main() -> int:
    if os.environ.get("OVERNIGHT_PLUGIN_SKILL_OVERRIDE") == "1":
        return 0
    try:
        d = json.load(sys.stdin)
    except (json.JSONDecodeError, OSError, ValueError):
        return 0
    if d.get("tool_name") != "Skill":
        return 0
    ti = d.get("tool_input", {}) or {}
    skill = (ti.get("skill") or ti.get("name") or "").strip()
    if any(b in skill for b in BLOCKED):
        sys.stderr.write(
            f"[overnight-plugin-block] '{skill}' is the UNGUARDED plugin path; "
            "do not arm impossible-os overnight runs with it.\n"
            "Use the repo-canonical, run_phase_guard-protected launcher:\n"
            "  bash .claude/skills/overnight-sequencer/arm-sequencer.sh             # kernel run (ChromeMCP off)\n"
            "  bash .claude/skills/overnight-sequencer/arm-sequencer.sh --with-browser   # gh-pages run\n"
            "  bash .claude/skills/overnight-sequencer/arm-sequencer.sh --disarm\n"
            "The plugin path arms no guard, so the headless run dies at the first "
            "background-review wait and thrashes on every watchdog relaunch.\n"
            "Deliberate override: OVERNIGHT_PLUGIN_SKILL_OVERRIDE=1\n"
        )
        return 2
    return 0


def _selftest() -> int:
    """3 checks (runner-kit shape, adapted to this hook's substring match +
    override env). Run: python3 overnight_plugin_skill_block.py --selftest"""
    import io
    fails = []

    def run(payload, env_override=None):
        old_stdin, old_env = sys.stdin, os.environ.get("OVERNIGHT_PLUGIN_SKILL_OVERRIDE")
        try:
            if env_override is not None:
                os.environ["OVERNIGHT_PLUGIN_SKILL_OVERRIDE"] = env_override
            else:
                os.environ.pop("OVERNIGHT_PLUGIN_SKILL_OVERRIDE", None)
            sys.stdin = io.StringIO(json.dumps(payload))
            return main()
        finally:
            sys.stdin = old_stdin
            if old_env is None:
                os.environ.pop("OVERNIGHT_PLUGIN_SKILL_OVERRIDE", None)
            else:
                os.environ["OVERNIGHT_PLUGIN_SKILL_OVERRIDE"] = old_env

    if run({"tool_name": "Skill", "tool_input": {"skill": "overnight-runner:start"}}) != 2:
        fails.append("plugin start not blocked")
    if run({"tool_name": "Skill", "tool_input": {"skill": "plugin:overnight-runner:schedule x"}}) != 2:
        fails.append("namespaced plugin schedule not blocked (substring match)")
    if run({"tool_name": "Skill", "tool_input": {"skill": "overnight-sequencer"}}) != 0:
        fails.append("repo-canonical sequencer skill blocked")
    if run({"tool_name": "Skill", "tool_input": {"skill": "overnight-runner:start"}}, env_override="1") != 0:
        fails.append("override env not honored")
    if fails:
        sys.stderr.write("overnight_plugin_skill_block selftest FAIL: " + "; ".join(fails) + "\n")
        return 1
    print("overnight_plugin_skill_block selftest OK")
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        raise SystemExit(_selftest())
    raise SystemExit(main())
