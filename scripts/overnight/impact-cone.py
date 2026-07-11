#!/usr/bin/env python3
"""Deterministic impact cone of a diff -- review the cone, not the repo.

From the staged diff (or <ref>..HEAD), derives: changed files, changed
function symbols, direct callers of those symbols, headers changed + their
direct includers, tests referencing the symbols, registration-table touches
(SSDT/test-runner/init tables), and a GLOBAL-STATE escalation flag when the
diff adds/edits global mutable state (review scope must then widen).

The bounded set scopes Codex prompts and the main session's own reads; the
final full pipeline (build/tests/smoke, whole-diff Codex review) is unchanged
-- this bounds the READING, not the gates.

Usage: impact-cone.py [--project DIR] [--range REF]   (default: staged diff)
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

FUNC_HUNK_RE = re.compile(r"^@@ .* @@ (?:static\s+)?[\w*]+[\s*]+(\w+)\s*\(")
FUNC_DEF_RE = re.compile(r"^[+-](?:static\s+)?[\w*]+[\s*]+(\w+)\s*\([^;]*$")
GLOBAL_RE = re.compile(r"^\+\s*(?:static\s+)?(?!return|if|for|while|else|goto|break|continue)"
                       r"[\w]+[\s*]+g?_?\w+\s*(?:\[[^\]]*\])?\s*(?:=|;)")
REG_TABLE_MARKERS = ("ssdt_register", "test_suite_register", "_init(",
                    "register_", "dispatch_table", "LDR_DATA")
NOISE_SYMBOLS = {"if", "for", "while", "switch", "return", "sizeof", "defined"}


def git(root, *args, timeout=60):
    r = subprocess.run(["git", "-C", str(root), *args],
                       capture_output=True, text=True, timeout=timeout)
    return r.stdout if r.returncode == 0 else ""


def grep_files(root, pattern, globs):
    try:
        r = subprocess.run(
            ["git", "-C", str(root), "grep", "-l", "-E", pattern, "--", *globs],
            capture_output=True, text=True, timeout=60)
        return r.stdout.splitlines()
    except Exception:
        return []


def main(argv) -> int:
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    if "--range" in argv:
        rng = argv[argv.index("--range") + 1]
        diff = git(root, "diff", rng, "--unified=1")
        files = git(root, "diff", "--name-only", rng).splitlines()
    else:
        diff = git(root, "diff", "--cached", "--unified=1")
        files = git(root, "diff", "--cached", "--name-only").splitlines()
    files = [f for f in files if f.strip()]
    src_files = [f for f in files if f.endswith((".c", ".h", ".asm", ".S"))]

    # Changed symbols: hunk headers + added/removed definition lines.
    symbols = set()
    for ln in diff.splitlines():
        m = FUNC_HUNK_RE.match(ln) or FUNC_DEF_RE.match(ln)
        if m and m.group(1) not in NOISE_SYMBOLS and len(m.group(1)) > 3:
            symbols.add(m.group(1))
    symbols = sorted(symbols)

    # Callers: files referencing any changed symbol (excluding the diff files).
    callers = set()
    if symbols:
        pat = "|".join(re.escape(s) + r"\s*\(" for s in symbols[:40])
        for f in grep_files(root, pat, ["src/", "include/", "user/"]):
            if f not in src_files:
                callers.add(f)

    # Header cone: changed headers -> direct includers.
    headers = [f for f in src_files if f.endswith(".h")]
    includers = set()
    for h in headers:
        base = Path(h).name
        for f in grep_files(root, rf'#include\s+"[^"]*{re.escape(base)}"',
                            ["src/", "include/", "user/"]):
            if f not in src_files:
                includers.add(f)

    # Tests referencing the symbols or the changed files' stems.
    stems = {Path(f).stem for f in src_files}
    test_pat_parts = [re.escape(s) for s in list(symbols)[:40]] + \
                     [re.escape(s) for s in stems if len(s) > 3]
    tests = grep_files(root, "|".join(test_pat_parts) or "NOMATCH",
                       ["src/kernel/test/"]) if test_pat_parts else []

    # Registration-table touches inside the diff (ABI consumers).
    reg_touches = sorted({m for m in REG_TABLE_MARKERS if m in diff})

    # Escalation: new/changed global mutable state widens the review scope.
    globals_added = [ln[1:].strip()[:160] for ln in diff.splitlines()
                     if GLOBAL_RE.match(ln) and "(" not in ln.split("=")[0]]

    print(json.dumps({
        "changed_files": files,
        "changed_symbols": symbols,
        "callers": sorted(callers),
        "header_includers": sorted(includers),
        "tests": sorted(set(tests)),
        "registration_touches": reg_touches,
        "global_state_escalation": bool(globals_added),
        "globals_added": globals_added[:10],
        "cone_size": len(set(files) | callers | includers | set(tests)),
    }, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
