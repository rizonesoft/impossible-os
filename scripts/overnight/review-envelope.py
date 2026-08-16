#!/usr/bin/env python3
"""Combine broker-dispatched Codex review legs into ONE findings envelope.

The full review transcripts stay on disk; the envelope carries everything
decision-relevant -- per-kind status, every severity-marked finding line,
artifact path + sha256 + size -- so the main session ingests one bounded JSON
instead of N multi-thousand-line transcripts. Nothing is lost: the envelope
names the artifact for any finding that needs its full surrounding context
(slice-read the artifact at need).

Usage: review-envelope.py [PROJECT_DIR] [--kinds adversarial,consistency,perf]
                          [--todo <todo-path>] [--section <n>] [--since <epoch>]
Reads the newest manifest entry PER KIND from
.claude/overnight/reviews/manifest.jsonl; exits 1 if any requested leg is
missing or incomplete (envelope still printed, with the gap named).

E3 SCOPING: the manifest ACCUMULATES across sections and runs (append-only), so
"newest per kind" would otherwise pull a PRIOR section's stale review (measured:
a TODO-22 self-check aggregated stale TODO-12 morning reviews). Pass `--todo
<path>` to restrict to the section under review (manifest carries `todo` per
entry) and/or `--since <epoch>` to restrict to this dispatch round. Both default
off for back-compat, but the sequencer/skills SHOULD pass `--todo`.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

COMPLETE_MARKERS = ("Turn completed", "Review complete", "turn.completed")
# The broker (review-broker-codex-dispatch.sh) appends "Turn completed (rc=N)"
# where N is the Codex process exit code. rc != 0 means the leg CRASHED (the
# recurring "codex app-server exited unexpectedly, rc=1" is the live case) and
# produced no usable verdict -- it must be RE-DISPATCHED, but ONLY that leg,
# not the whole 3-leg bundle. Without parsing rc, leg_summary marked a crashed
# leg `complete: True` (the marker is present regardless of rc), so the runner
# could not tell a crash from a clean verdict and re-ran every leg. Measured
# 2026-07-12: 16 rc=1 crash lines / 5+ full re-dispatches in one section.
RC_RE = re.compile(r"Turn completed \(rc=(\d+)\)")
CRASH_SIGNATURES = ("app-server exited unexpectedly", "exited unexpectedly, rc=")
FINDING_RE = re.compile(
    r"(?i)^\s*(?:[-*\d.\s]*)?\[?(critical|high|medium|low|C\d|H\d|M\d|L\d)\b")
MAX_FINDING_LINES = 120
TAIL_LINES = 30


def leg_summary(entry: dict) -> dict:
    path = Path(entry.get("logFile") or "")
    out = {"kind": entry.get("kind"), "jobId": entry.get("jobId"),
           "artifact": str(path), "complete": False, "crashed": False,
           "rc": None, "findings": [], "sha256": None, "bytes": 0, "tail": []}
    try:
        data = path.read_bytes()
    except OSError:
        out["error"] = "artifact unreadable"
        return out
    out["bytes"] = len(data)
    out["sha256"] = hashlib.sha256(data).hexdigest()
    text = data.decode("utf-8", "replace")
    out["complete"] = any(m in text for m in COMPLETE_MARKERS)
    # Crash detection: an explicit rc != 0 in the completion marker, OR a known
    # Codex crash signature anywhere in the leg output. A crashed leg is NOT a
    # valid verdict even though its "Turn completed" marker is present.
    rc_m = RC_RE.search(text)
    if rc_m:
        out["rc"] = int(rc_m.group(1))
    if (out["rc"] is not None and out["rc"] != 0) or \
            any(sig in text for sig in CRASH_SIGNATURES):
        out["crashed"] = True
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
    # E3: scope filters so "newest per kind" cannot pull a prior section's review.
    want_todo = None
    if "--todo" in argv:
        want_todo = str(argv[argv.index("--todo") + 1]).replace("\\", "/").lstrip("./")
    # v14 close-out (2026-08-16): FILE scope alone silently ingested an
    # earlier section's legs from the same TODO (observed: a section-26 wave
    # returned section-25 consistency+perf legs, whose findings were about
    # code the change never touched). With --section, an entry that carries a
    # DIFFERENT section is skipped; an entry with NO section field is skipped
    # too -- fail toward redispatch, which costs one dispatch, never a wrong
    # verdict ingested as this section's.
    want_section = None
    if "--section" in argv:
        try:
            want_section = int(argv[argv.index("--section") + 1])
        except (ValueError, IndexError):
            want_section = None
    since = None
    if "--since" in argv:
        try:
            since = int(argv[argv.index("--since") + 1])
        except (ValueError, IndexError):
            since = None
    manifest = project / ".claude/overnight/reviews/manifest.jsonl"
    latest: dict = {}
    try:
        for ln in manifest.read_text(encoding="utf-8").splitlines():
            try:
                e = json.loads(ln)
            except ValueError:
                continue
            if want_todo is not None:
                etodo = str(e.get("todo") or "").replace("\\", "/").lstrip("./")
                if etodo != want_todo:
                    continue  # a different TODO's review -- not ours
            if want_section is not None:
                esec = e.get("section")
                if not isinstance(esec, int) or esec != want_section:
                    continue  # different or unattributed section -- not ours
            if since is not None:
                ets = e.get("ts")
                if not isinstance(ets, int) or ets < since:
                    continue
            latest[e.get("kind")] = e  # later lines win (append order)
    except OSError:
        print(json.dumps({"error": "no broker manifest", "kinds": {}}))
        return 1
    # all_complete: every leg produced a completion marker (back-compat).
    # all_clean:    every leg completed AND did not crash (rc==0) -- the real
    #               "the review is usable" signal.
    # needs_redispatch: exactly the legs to re-run (missing OR crashed). The
    #               runner re-dispatches ONLY these, never the whole bundle;
    #               a completed clean leg is reused as-is.
    env = {"kinds": {}, "all_complete": True, "all_clean": True,
           "missing": [], "crashed": [], "needs_redispatch": []}
    for k in kinds:
        if k not in latest:
            env["missing"].append(k)
            env["all_complete"] = False
            env["all_clean"] = False
            env["needs_redispatch"].append(k)
            continue
        leg = leg_summary(latest[k])
        env["kinds"][k] = leg
        if not leg["complete"]:
            env["all_complete"] = False
            env["all_clean"] = False
            env["needs_redispatch"].append(k)
        elif leg["crashed"]:
            # Completed-but-crashed: has a marker, no usable verdict. Re-run
            # this leg only; do NOT let it greenlight the review.
            env["crashed"].append(k)
            env["all_clean"] = False
            if k not in env["needs_redispatch"]:
                env["needs_redispatch"].append(k)
    print(json.dumps(env, indent=1))
    # Exit 0 only when the review is genuinely usable (all legs clean). A
    # crashed leg used to slip through on all_complete; gating on all_clean
    # stops the runner proceeding on a non-verdict.
    return 0 if env["all_clean"] else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
