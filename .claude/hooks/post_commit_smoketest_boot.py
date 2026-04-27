#!/usr/bin/env python3
# PostToolUse Bash hook: when the just-completed Bash invocation
# created a `git commit` that touched any boot-path file, run the
# end-to-end boot smoke test (scripts/test-smoke.sh) and report
# the result via systemMessage.
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). M1 design-review fix: detection no longer relies on
# `cmd.startswith("git commit")` -- it uses the same alias-aware
# classifier as section_commit_gate so wrapped commit invocations
# are detected correctly.
#
# Hook category: REMINDER. Not a wrap.sh candidate (same reason
# as post_commit_smoketest.py -- wrapped-commit detection requires
# the full classifier, not a substring prefilter).
import json
import os
import subprocess
import sys
from pathlib import Path

_HOOK_DIR = Path(__file__).resolve().parent
if str(_HOOK_DIR) not in sys.path:
    sys.path.insert(0, str(_HOOK_DIR))

# pylint: disable=wrong-import-position
import section_commit_gate as scg  # noqa: E402


_BOOT_PATHS = (
    "src/boot/",
    "src/kernel/main/boot_",
    "src/kernel/idt.c",
    "src/kernel/gdt.c",
    "src/kernel/msr.c",
    "src/kernel/smp/",
    "src/kernel/mm/pmm.c",
    "src/kernel/mm/vmm.c",
    "src/kernel/mm/heap.c",
    "src/kernel/drivers/lapic.c",
    "src/kernel/drivers/ioapic.c",
    "src/kernel/drivers/acpi.c",
    "src/kernel/drivers/timer.c",
    "src/kernel/drivers/rtc.c",
    "src/kernel/drivers/pic.c",
    "src/kernel/drivers/pit.c",
)


def _read_skip_prefix(root: str, parent: str) -> str:
    if not parent:
        return ""
    log_path = root + "/.claude/state/skip-log.jsonl"
    if not os.path.exists(log_path):
        return ""
    try:
        with open(log_path, "r", encoding="utf-8") as f:
            for line in reversed(f.readlines()):
                line = line.strip()
                if not line:
                    continue
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if (rec.get("head_sha", "").startswith(parent[:12])
                        and rec.get("reason")):
                    return ("[SKIP_REVIEW_HOOK reason: "
                            + rec["reason"][:160] + "] ")
    except Exception:
        pass
    return ""


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    cmd = (d.get("tool_input", {}) or {}).get("command", "") or ""

    try:
        scg._ensure_crc()
        is_commit, _ = scg._bash_is_git_commit(cmd)
    except Exception:
        return 0
    if not is_commit:
        return 0

    try:
        changed = subprocess.check_output(
            ["git", "show", "--name-only", "--format=", "HEAD"],
            text=True, timeout=5, stderr=subprocess.DEVNULL,
        ).splitlines()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return 0
    if not any(f.startswith(_BOOT_PATHS) for f in changed if f):
        return 0

    try:
        root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return 0

    parent = ""
    try:
        parent = subprocess.check_output(
            ["git", "rev-parse", "HEAD~1"],
            cwd=root, text=True, timeout=2, stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        pass

    skip_prefix = _read_skip_prefix(root, parent)

    try:
        result = subprocess.run(
            ["bash", root + "/scripts/test-smoke.sh"],
            capture_output=True, text=True, timeout=60,
            env={**os.environ, "TIMEOUT_SEC": "30"},
        )
    except subprocess.TimeoutExpired:
        print(json.dumps({
            "systemMessage": (skip_prefix
                              + "[smoketest-boot] TIMED OUT after "
                              "60s -- rerun bash scripts/test-smoke.sh "
                              "manually"),
            "continue": True,
        }))
        return 0

    out = result.stdout or ""
    if result.returncode == 0 and "SMOKE TEST PASSED" in out:
        accel = "KVM" if "KVM acceleration enabled" in out else "TCG"
        print(json.dumps({
            "systemMessage": (skip_prefix
                              + "[smoketest-boot] SMOKE TEST PASSED "
                              "-- boot reached user shell ("
                              + accel + ")"),
        }))
    else:
        missing = [l.strip() for l in out.splitlines() if "MISSING:" in l]
        msg = missing[-1] if missing else "see build/smoke-test.stripped.log"
        print(json.dumps({
            "systemMessage": (skip_prefix
                              + "[smoketest-boot] SMOKE TEST FAILED -- "
                              + msg[:200]),
            "continue": True,
        }))
    return 0


if __name__ == "__main__":
    sys.exit(main())
