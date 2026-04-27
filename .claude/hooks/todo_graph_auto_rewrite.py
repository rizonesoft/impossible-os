#!/usr/bin/env python3
# PostToolUse Edit/Write/MultiEdit hook: when the agent edits a
# TODO markdown file, run scripts/todo-graph/validate.py
# --fix-line-numbers --write to keep stamp parentheticals' line
# numbers fresh. If the validator refuses an ambiguous rewrite,
# emit a systemMessage explaining what needs manual disambiguation.
#
# Extracted from .claude/settings.json inline-Python (TODO-08
# section 7). Behavior is byte-equivalent to the original (same
# 15-second timeout, same ambiguity message shape).
#
# Hook category: SIDE-EFFECT (writes the TODO file when
# unambiguous; never blocks). NOT a wrap.sh candidate -- the
# validate.py invocation is the actual work; prefilter savings
# would be negligible vs the validator runtime, and the markers
# (`todo/`, `.md`) need full-path inspection.
import json
import os
import subprocess
import sys


def main() -> int:
    try:
        d = json.load(sys.stdin)
    except Exception:
        return 0
    path = (d.get("tool_input", {}) or {}).get("file_path", "") or ""
    if not path or not path.endswith(".md"):
        return 0
    p = path.replace(chr(92), "/")
    if "todo/" not in p:
        return 0

    try:
        root = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.DEVNULL,
        ).decode().strip()
    except Exception:
        return 0

    validator = root + "/scripts/todo-graph/validate.py"
    if not os.path.exists(validator):
        return 0

    try:
        result = subprocess.run(
            [
                "python3", validator,
                "--fix-line-numbers",
                "--write",
                "--quiet",
            ],
            cwd=root, capture_output=True, text=True, timeout=15,
        )
    except Exception:
        return 0

    # Success = nothing to fix OR rewrites applied cleanly.
    # --fix-line-numbers exits 1 only on ambiguity.
    combined = (result.stdout + result.stderr)
    if result.returncode == 1 and "ambiguous" in combined:
        msg = (
            "[todo-graph auto-rewrite] validate.py --fix-line-numbers "
            "refused an ambiguous item-name rewrite: "
            + combined.strip().replace(chr(10), " ")[:200]
            + ". Disambiguate the stamp parenthetical by hand."
        )
        print(json.dumps({"systemMessage": msg}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
