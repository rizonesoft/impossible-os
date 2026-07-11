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

Usage: impact-cone.py [--project DIR] [--range REF]
       (default: WORKING TREE vs HEAD -- staged + unstaged + untracked)
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import changed_paths  # noqa: E402  (sibling helper)

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


def _diff_no_index(root, path):
    """All-added diff of an untracked file (git diff --no-index exits 1 when
    files differ, so the returncode-gated `git` helper can't be used)."""
    try:
        r = subprocess.run(
            ["git", "-C", str(root), "diff", "--no-index", "--unified=1",
             "/dev/null", path], capture_output=True, text=True, timeout=30)
        return r.stdout
    except Exception:
        return ""


def grep_files(root, pattern, paths):
    """Files under `paths` matching `pattern`. Uses ripgrep over the WORKING
    TREE (honors .gitignore, so untracked-but-not-ignored files ARE searched;
    `git grep` only saw the index/tracked tree and missed a caller in a new
    untracked file). Falls back to git grep if rg is unavailable."""
    try:
        r = subprocess.run(
            ["rg", "-l", "--no-messages", "-g", "*.c", "-g", "*.h",
             "-g", "*.asm", "-g", "*.S", "-e", pattern, "--", *paths],
            cwd=str(root), capture_output=True, text=True, timeout=60)
        return [ln for ln in r.stdout.splitlines() if ln.strip()]
    except FileNotFoundError:
        try:
            r = subprocess.run(
                ["git", "-C", str(root), "grep", "-l", "-E", pattern, "--", *paths],
                capture_output=True, text=True, timeout=60)
            return r.stdout.splitlines()
        except Exception:
            return []
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
        # Default = WORKING TREE vs HEAD (staged + unstaged) plus untracked.
        # `git diff --cached` (staged only) missed the unstaged/untracked
        # changes the runner actually executes (2026-07-11 fix).
        diff = git(root, "diff", "HEAD", "--unified=1")
        files = changed_paths(root)
    files = [f for f in files if f.strip()]
    src_files = [f for f in files if f.endswith((".c", ".h", ".asm", ".S"))]
    # Untracked new source files have no diff-vs-HEAD; synthesize an all-added
    # diff so their new symbols/globals/registrations enter the cone.
    if "--range" not in argv:
        tracked = set(git(root, "ls-files", "--", *src_files).splitlines()) \
            if src_files else set()
        for uf in src_files:
            if uf not in tracked:
                diff += _diff_no_index(root, uf)

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
