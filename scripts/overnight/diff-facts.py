#!/usr/bin/env python3
"""diff-facts.py -- deterministic, worktree-aware facts about the current diff.

The review-evidence-mapper agent's job is to build a file:line evidence map of a
change before Codex review. Most of that map is MECHANICAL and needs no model:
changed definitions/callers/includers/tests, the concurrency surface (locks,
atomics, IRQ state, allocations/frees, refcounts, teardown), user-pointer
boundaries, SSDT/syscall/ABI touchpoints, and cppcheck's static findings. This
tool produces all of it deterministically so the mapper dispatch can be replaced
by a content-bound receipt (see review_dispatch_gate); the Opus/Sonnet JUDGMENT
nets (kernel-quality-auditor, adversarial Codex) are unchanged -- this bounds the
EVIDENCE GATHERING, never the review verdicts.

Worktree-aware: analyzes WORKING TREE vs HEAD (staged + unstaged) plus untracked
source, so a mid-implementation diff is covered. Writes a content-bound receipt
(.claude/state/last-diff-facts.json) keyed to the changed files' current bytes.

Usage: diff-facts.py [--project DIR] [--range REF] [--no-cppcheck]
Output: bounded JSON on stdout.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import changed_paths, worktree_key  # noqa: E402

# Added-line (diff `+`) signal patterns, grouped by review dimension.
SIGNALS = {
    "locks": re.compile(r"\b(spin_lock\w*|spin_unlock\w*|mutex_(?:lock|unlock)|"
                        r"__atomic_\w+|smp_mb|barrier\(|acquire|release)\b"),
    "irq_state": re.compile(r"\b(cli\b|sti\b|irqsave|irqrestore|local_irq|"
                            r"KeRaiseIrql|KeLowerIrql|IRQL|disable_interrupts|"
                            r"enable_interrupts)\b"),
    "alloc": re.compile(r"\b(kmalloc|pmm_alloc\w*|vmm_map\w*|kcalloc|"
                        r"alloc_pages?)\b"),
    "free": re.compile(r"\b(kfree|pmm_free\w*|vmm_unmap\w*|free_pages?)\b"),
    "refcount": re.compile(r"\b(refcount\w*|\w+_get\(|\w+_put\(|ref_inc|ref_dec|"
                           r"atomic_(?:inc|dec)\w*|ObReference|ObDereference)\b"),
    "teardown": re.compile(r"\bgoto\s+\w*(?:err|fail|cleanup|out|undo)\w*|"
                           r"\b(cleanup|teardown|rollback|unwind)\b"),
    "user_ptr": re.compile(r"\b(copy_from_user|copy_to_user|access_ok|__user\b|"
                           r"vmm_set_user\w*|usercopy|probe_user|ProbeFor\w+)\b"),
    "abi": re.compile(r"\b(NTSTATUS|SSDT|boot_info|BOOT_INFO_VERSION|PEB|TEB|"
                      r"syscall\s+number|struct\s+offset|_Static_assert)\b"),
}


def _sh(cmd, cwd, timeout=90):
    try:
        return subprocess.run(cmd, cwd=str(cwd), capture_output=True,
                              text=True, timeout=timeout)
    except Exception:
        return None


def _git(root, *args, timeout=60):
    r = _sh(["git", "-C", str(root), *args], root, timeout=timeout)
    return r.stdout if (r and r.returncode == 0) else ""


def _diff_text(root, rng):
    if rng:
        return _git(root, "diff", rng, "--unified=0")
    diff = _git(root, "diff", "HEAD", "--unified=0")
    # untracked source files: synthesize all-added hunks
    untracked = [f for f in _git(root, "ls-files", "--others",
                                 "--exclude-standard").splitlines()
                 if f.endswith((".c", ".h", ".asm", ".S"))]
    for uf in untracked:
        r = _sh(["git", "-C", str(root), "diff", "--no-index", "--unified=0",
                 "/dev/null", uf], root, timeout=30)
        if r is not None:
            diff += r.stdout
    return diff


def _added_lines(diff):
    """(file, line_no, text) for each added (`+`) line, tracking the current
    file + new-line number from hunk headers."""
    out = []
    cur = None
    newln = 0
    hunk = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)")
    for ln in diff.splitlines():
        if ln.startswith("+++ b/"):
            cur = ln[6:]
            continue
        if ln.startswith("+++ "):
            cur = ln[4:].lstrip("b/")
            continue
        m = hunk.match(ln)
        if m:
            newln = int(m.group(1))
            continue
        if ln.startswith("+") and not ln.startswith("+++"):
            out.append((cur, newln, ln[1:]))
            newln += 1
        elif not ln.startswith("-"):
            newln += 1
    return out


def _cppcheck(root, files, timeout=120):
    if not files or not _sh(["which", "cppcheck"], root, timeout=5):
        return [], "cppcheck-unavailable"
    r = _sh(["cppcheck", "--enable=warning,portability", "--quiet",
             "--inline-suppr",
             "--template={file}:{line}: {severity}: {id}: {message}",
             *files], root, timeout=timeout)
    if r is None:
        return [], "cppcheck-timeout"
    findings = []
    for ln in (r.stderr or "").splitlines():
        ln = ln.strip()
        if ln and ":" in ln:
            findings.append(ln[:200])
    return findings[:30], "ok"


def main(argv) -> int:
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    rng = argv[argv.index("--range") + 1] if "--range" in argv else None
    do_cpp = "--no-cppcheck" not in argv
    here = Path(__file__).resolve().parent

    files = changed_paths(root, rng)
    src = [f for f in files if f.endswith((".c", ".h", ".asm", ".S"))]

    # 1. impact cone (changed defs/symbols/callers/includers/tests/registrations)
    cone_args = [sys.executable, str(here / "impact-cone.py"),
                 "--project", str(root)]
    if rng:
        cone_args += ["--range", rng]
    cr = _sh(cone_args, root, timeout=90)
    try:
        cone = json.loads(cr.stdout) if cr else {}
    except Exception:
        cone = {}

    # 2. concurrency / safety / ABI / user-ptr inventory from added lines
    diff = _diff_text(root, rng)
    added = _added_lines(diff)
    dims = {k: [] for k in SIGNALS}
    for f, lno, text in added:
        if not f or not f.endswith((".c", ".h", ".asm", ".S")):
            continue
        for dim, rx in SIGNALS.items():
            if rx.search(text):
                dims[dim].append(f"{f}:{lno}: {text.strip()[:120]}")
    dims = {k: v[:20] for k, v in dims.items() if v}

    # 3. cppcheck static findings on changed C sources
    cpp_findings, cpp_status = ([], "skipped")
    if do_cpp:
        cfiles = [f for f in src if f.endswith((".c", ".h"))
                  and (root / f).exists()]
        cpp_findings, cpp_status = _cppcheck(root, cfiles)

    # 4. content-bound receipt (WS2b: a fresh diff-facts run == the mapper's
    #    deterministic evidence-gathering over THIS exact diff).
    digest = worktree_key(root, files) if files else "empty"
    try:
        rp = root / ".claude/state/last-diff-facts.json"
        rp.parent.mkdir(parents=True, exist_ok=True)
        tmp = rp.with_suffix(f".{os.getpid()}.tmp")
        tmp.write_text(json.dumps({
            "digest": digest, "changed_files": files, "ts_ns": time.time_ns()}))
        os.replace(str(tmp), str(rp))
    except Exception:
        pass

    out = {
        "changed_files": files,
        "changed_symbols": cone.get("changed_symbols", []),
        "callers": cone.get("callers", []),
        "header_includers": cone.get("header_includers", []),
        "tests": cone.get("tests", []),
        "registration_touchpoints": cone.get("registration_touches", []),
        "global_state_escalation": cone.get("global_state_escalation", False),
        "concurrency_safety": dims,
        "cppcheck": {"status": cpp_status, "findings": cpp_findings},
        "digest": digest,
        "cone_size": cone.get("cone_size", len(files)),
    }
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
