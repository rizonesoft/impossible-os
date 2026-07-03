#!/usr/bin/env python3
"""Kit version-gap notice (SessionStart) — the PULL half of the kit-sync loop.

kit_sync_reminder.py pushes (project edits → upstream to ~/runner-kit); this
hook pulls: at interactive session start, if the canonical kit's VERSION is
ahead of this project's vendored .runner-kit-version stamp, emit a notice to
dispatch Agent(kit-sync) in PULL mode for a per-file, judgment-gated update.

NEVER fires during an overnight run (OVERNIGHT_SEQUENCER_RUN=1) — updating the
runner underneath a live unattended run is forbidden. Silent when versions
match, when the kit is absent, or on any read error. Stdlib only, exits 0.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KIT = Path.home() / "runner-kit"


def build_notice(env: dict, kit_version: str, vendored: str, recent: str) -> str:
    """Pure: the stderr text to emit (empty string => emit nothing)."""
    if env.get("OVERNIGHT_SEQUENCER_RUN") == "1":
        return ""
    if not kit_version or kit_version == vendored:
        return ""
    return (
        f"[runner-kit: update available] canonical kit is v{kit_version}; this "
        f"project vendored v{vendored or 'unstamped'}. Recent kit changes:\n"
        f"{recent or '  (log unavailable)'}\n"
        "When convenient (NOT mid-task): dispatch Agent(kit-sync) for the drift "
        "report, hand-approve per file (PORTABLE may be pulled; TUNABLE is "
        "hand-merged; see ~/runner-kit/tunable.list), re-run the selftest "
        "battery via test-runner, then update .runner-kit-version."
    )


def main(argv: list) -> int:
    if "--selftest" in argv:
        return _selftest()
    try:
        sys.stdin.read()
    except Exception:
        pass
    try:
        kit_version = (KIT / "VERSION").read_text(encoding="utf-8").strip() \
            if (KIT / "VERSION").exists() else ""
        vendored = (ROOT / ".runner-kit-version").read_text(encoding="utf-8").strip() \
            if (ROOT / ".runner-kit-version").exists() else ""
        recent = ""
        if kit_version and kit_version != vendored:
            out = subprocess.run(
                ["git", "-C", str(KIT), "log", "--oneline", "-3"],
                capture_output=True, text=True, timeout=10,
            )
            recent = "\n".join("  " + line for line in (out.stdout or "").splitlines())
    except Exception:
        return 0
    text = build_notice(dict(os.environ), kit_version, vendored, recent)
    if text:
        print(text, file=sys.stderr)
    return 0


def _selftest() -> int:
    failures = []

    def check(name, cond):
        if not cond:
            failures.append(name)

    check("silent-when-equal", build_notice({}, "0.5.0", "0.5.0", "") == "")
    check("silent-no-kit", build_notice({}, "", "0.4.0", "") == "")
    check("silent-overnight",
          build_notice({"OVERNIGHT_SEQUENCER_RUN": "1"}, "0.5.0", "0.4.0", "x") == "")
    gap = build_notice({}, "0.5.0", "0.4.0", "  abc fix")
    check("notice-on-gap", "update available" in gap and "v0.5.0" in gap and "v0.4.0" in gap)
    check("notice-unstamped", "unstamped" in build_notice({}, "0.5.0", "", ""))
    check("notice-names-pull-flow", "kit-sync" in gap and "tunable" in gap.lower())

    if failures:
        print("selftest FAIL: " + ", ".join(failures), file=sys.stderr)
        return 1
    print("selftest OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
