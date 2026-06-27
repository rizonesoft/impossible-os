#!/usr/bin/env python3
# block-via: warning-only (stuck-detect nudge; never exits 2)
"""PostToolUse on Bash: count consecutive same-target build/test/smoke FAILURES and
nudge the loop into the Claude->Codex->Fusion ladder at threshold. Overnight-only
(gates on OVERNIGHT_SEQUENCER_RUN); fail-open; never blocks."""
from __future__ import annotations

import json
import os
import sys
from pathlib import Path

THRESHOLD = 3
_TARGETS = (("scripts/build.sh", "build"), ("scripts/test.sh", "test"),
            ("scripts/test-smoke.sh", "smoke"))
_OK = ("=== BUILD OK ===", "SMOKE TEST PASSED", "All tests passed")
_FAIL = ("BUILD FAILED", "SMOKE TEST FAILED", " FAIL", "error:", "Traceback")


def _repo_root():
    here = Path(__file__).resolve()
    for p in here.parents:
        if (p / ".claude").is_dir():
            return p
    return None


def _classify(command: str, response_text: str):
    target = None
    for needle, name in _TARGETS:
        if needle in command:
            target = name
            break
    if target is None:
        return None, None
    txt = response_text or ""
    if any(s in txt for s in _OK):
        return target, "ok"
    if any(s in txt for s in _FAIL):
        return target, "fail"
    return target, "unknown"


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    if not os.environ.get("OVERNIGHT_SEQUENCER_RUN"):
        return 0
    if d.get("tool_name") != "Bash":
        return 0
    root = _repo_root()
    if root is None:
        return 0
    command = (d.get("tool_input") or {}).get("command", "") or ""
    resp = d.get("tool_response")
    text = resp if isinstance(resp, str) else json.dumps(resp)
    target, verdict = _classify(command, text)
    if target is None or verdict == "unknown":
        return 0
    state_p = root / ".claude" / "state" / "fusion-stuck.json"
    try:
        st = json.loads(state_p.read_text())
    except Exception:
        st = {}
    if verdict == "ok" or st.get("target") != target:
        st = {"target": target, "consecutive_failures": 0}
    if verdict == "fail":
        st["target"] = target
        st["consecutive_failures"] = int(st.get("consecutive_failures", 0)) + 1
    try:
        state_p.parent.mkdir(parents=True, exist_ok=True)
        state_p.write_text(json.dumps(st))
    except Exception:
        pass
    if st.get("consecutive_failures", 0) >= THRESHOLD:
        sys.stderr.write(
            f"[fusion-ladder] STUCK: {st['consecutive_failures']} consecutive '{target}' "
            f"failures. Escalate: Tier 1 Skill(codex:rescue) (up to 2 rounds); if still "
            f"stuck, Tier 2 'python3 .fusion/ladder.py dispatch --mode stuck --target "
            f"{target} --brief-file <brief> --prior codex=<rescue-output>' then DEFER the "
            f"section and collect later (python3 .fusion/ladder.py poll <id>).\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
