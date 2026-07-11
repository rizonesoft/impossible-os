#!/usr/bin/env python3
"""Combine broker-dispatched Codex review legs into ONE findings envelope.

The full review transcripts stay on disk; the envelope carries everything
decision-relevant -- per-kind status, every severity-marked finding line,
artifact path + sha256 + size -- so the main session ingests one bounded JSON
instead of N multi-thousand-line transcripts. Nothing is lost: the envelope
names the artifact for any finding that needs its full surrounding context
(slice-read the artifact at need).

Usage: review-envelope.py [PROJECT_DIR] [--kinds adversarial,consistency,perf]
Reads the newest manifest entry PER KIND from
.claude/overnight/reviews/manifest.jsonl; exits 1 if any requested leg is
missing or incomplete (envelope still printed, with the gap named).
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

COMPLETE_MARKERS = ("Turn completed", "Review complete", "turn.completed")
FINDING_RE = re.compile(
    r"(?i)^\s*(?:[-*\d.\s]*)?\[?(critical|high|medium|low|C\d|H\d|M\d|L\d)\b")
MAX_FINDING_LINES = 120
TAIL_LINES = 30


def leg_summary(entry: dict) -> dict:
    path = Path(entry.get("logFile") or "")
    out = {"kind": entry.get("kind"), "jobId": entry.get("jobId"),
           "artifact": str(path), "complete": False, "findings": [],
           "sha256": None, "bytes": 0, "tail": []}
    try:
        data = path.read_bytes()
    except OSError:
        out["error"] = "artifact unreadable"
        return out
    out["bytes"] = len(data)
    out["sha256"] = hashlib.sha256(data).hexdigest()
    text = data.decode("utf-8", "replace")
    out["complete"] = any(m in text for m in COMPLETE_MARKERS)
    lines = text.splitlines()
    findings = [ln.strip()[:400] for ln in lines if FINDING_RE.match(ln)]
    if len(findings) > MAX_FINDING_LINES:
        out["findings_truncated"] = len(findings) - MAX_FINDING_LINES
        findings = findings[:MAX_FINDING_LINES]
    out["findings"] = findings
    out["tail"] = [ln[:400] for ln in lines[-TAIL_LINES:]]
    return out


def main(argv) -> int:
    project = Path(argv[0]) if argv and not argv[0].startswith("-") else Path(".")
    kinds = ["adversarial", "consistency", "perf"]
    if "--kinds" in argv:
        kinds = [k.strip() for k in argv[argv.index("--kinds") + 1].split(",")]
    manifest = project / ".claude/overnight/reviews/manifest.jsonl"
    latest: dict = {}
    try:
        for ln in manifest.read_text(encoding="utf-8").splitlines():
            try:
                e = json.loads(ln)
                latest[e.get("kind")] = e  # later lines win (append order)
            except ValueError:
                continue
    except OSError:
        print(json.dumps({"error": "no broker manifest", "kinds": {}}))
        return 1
    env = {"kinds": {}, "all_complete": True, "missing": []}
    for k in kinds:
        if k not in latest:
            env["missing"].append(k)
            env["all_complete"] = False
            continue
        leg = leg_summary(latest[k])
        env["kinds"][k] = leg
        if not leg["complete"]:
            env["all_complete"] = False
    print(json.dumps(env, indent=1))
    return 0 if env["all_complete"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
