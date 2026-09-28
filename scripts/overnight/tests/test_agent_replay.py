#!/usr/bin/env python3
"""agent-replay.py scoring, agent loading and verdict, offline (no model calls).

Run: python3 scripts/overnight/tests/test_agent_replay.py
"""
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("replay", REPO / "scripts/overnight/agent-replay.py")
R = importlib.util.module_from_spec(spec)
spec.loader.exec_module(R)

LOC = {"kind": "locs", "values": ["src/kernel/drivers/serial.c:1097"]}
SHA = {"kind": "commit", "values": ["41d4fd39b"]}
CASES = [
    ("exact file:line", LOC, "HIGH src/kernel/drivers/serial.c:1097 lock dropped early", True),
    ("basename only, within tolerance", LOC, "serial.c:1103 races the ISR", True),
    ("a range covering the line", LOC, "serial.c:1080-1095 unlocked window", True),
    ("wrong line, outside tolerance", LOC, "serial.c:1200 unrelated", False),
    ("right line, wrong file", LOC, "src/kernel/drivers/uart.c:1097 same line other file", False),
    ("a longer name ending in the basename", LOC, "myserial.c:1097", False),
    ("no location at all", LOC, "the serial driver has a race", False),
    ("findings envelope JSON fields", LOC,
     '{"findings": [{"severity": "high", "file": "src/kernel/drivers/serial.c", "line": 1099, "summary": "x"}]}', True),
    ("envelope with the line far away", LOC,
     '{"findings": [{"severity": "high", "file": "src/kernel/drivers/serial.c", "line": 40, "summary": "x"}]}', False),
    ("envelope naming another file", LOC,
     '{"findings": [{"file": "src/kernel/drivers/uart.c", "line": 1097}]}', False),
    ("a wide region cited as correct is not a finding", LOC,
     "the publication path (`serial.c:555-1200`) correctly serializes every writer", False),
    ("envelope present: prose mentions do not count", LOC,
     'checked serial.c:1097 and it is fine\n```json\n{"findings": [{"file": "src/kernel/drivers/serial.c", "line": 40}]}\n```', False),
    ("envelope present: the finding's own summary can carry the location", LOC,
     '```json\n{"findings": [{"file": "src/kernel/drivers/serial.c", "line": 1, "summary": "see serial.c:1095-1098"}]}\n```', True),
    ("short sha present", SHA, "It was added in 41d4fd3 (acpica: vendor)", True),
    ("full sha present", SHA, "41d4fd39b2c0ffee added it", True),
    ("different sha", SHA, "added in 41d4fd4aa", False),
]


def main():
    fails = []
    for name, expect, text, want in CASES:
        if R.score(expect, text) is not want:
            fails.append(f"score: {name}: got {not want}, want {want}")
    d = R.agent_def("kernel-quality-auditor", "sonnet")["kernel-quality-auditor"]
    if d["model"] != "sonnet" or d["tools"] != ["Read", "Grep", "Glob"] or "SMP" not in d["prompt"]:
        fails.append(f"agent_def did not load the auditor faithfully: {d['tools']} {d['model']}")
    with tempfile.TemporaryDirectory() as t:
        def write(name, rows):
            p = Path(t) / name
            p.write_text("".join(json.dumps(r) + "\n" for r in rows))
            return str(p)
        row = lambda i, model, hit, sev: {"id": i, "model": model, "hit": hit, "severity": sev, "cost_usd": 1.0, "seconds": 1}
        base = write("b.jsonl", [row("a", "opus", True, "high"), row("b", "opus", True, "low"), row("c", "opus", False, "low")])
        good = write("g.jsonl", [row("a", "sonnet", True, "high"), row("b", "sonnet", False, "low"), row("c", "sonnet", True, "low")])
        bad = write("x.jsonl", [row("a", "sonnet", False, "high"), row("b", "sonnet", True, "low"), row("c", "sonnet", True, "low")])
        worse = write("w.jsonl", [row("a", "sonnet", True, "high"), row("b", "sonnet", False, "low"), row("c", "sonnet", False, "low")])
        import contextlib, io
        def verdict(b, c):
            with contextlib.redirect_stdout(io.StringIO()):
                return R.cmd_compare(type("A", (), {"baseline": b, "candidate": c})())
        if verdict(base, good) != 0:
            fails.append("equal recall, no serious miss should PASS")
        if verdict(base, bad) != 1:
            fails.append("missing a high task the baseline caught must FAIL even at equal recall")
        if verdict(base, worse) != 1:
            fails.append("lower recall must FAIL")
    if fails:
        print("test_agent_replay FAIL:\n  " + "\n  ".join(fails))
        return 1
    print(f"test_agent_replay OK ({len(CASES) + 4} cases)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
