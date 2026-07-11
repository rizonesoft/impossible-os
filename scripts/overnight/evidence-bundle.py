#!/usr/bin/env python3
"""Shared evidence bundle -- build the source views once, all agents consume.

Several analysts (parity, coverage, XREF, quality, context mappers) used to
independently re-read the same source + TODO files. This builds ONE bundle
per section -- structural views, not whole files -- that every agent prompt
references, each applying its own lens. Full source stays available for the
main session's verification reads; the bundle bounds ORIENTATION, not proof.

Bundle contents (under .claude/overnight/bundles/<key12>/):
  manifest.json     the section-manifest output (or the file list given)
  structure.md      per-file compact structural view: function signatures,
                    structs/enums/defines, includes, lock/atomic touchpoints,
                    call edges (callee names per function, regex-derived)
  diff.patch        staged diff of the bundle files (when any)
  bundle.json       index: files, blob hashes, sizes, bundle key

The bundle key is content-derived (blob hashes), so an unchanged section
reuses the same bundle across agents, sessions, and relaunches.

Usage:
  evidence-bundle.py --manifest <(section-manifest.py ...)   (or a file path)
  evidence-bundle.py --files src/a.c include/a.h ...
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import content_hashes  # noqa: E402  (sibling helper)

BUNDLE_DIR_REL = ".claude/overnight/bundles"
MAX_BUNDLES = 12
SIG_RE = re.compile(
    r"^(?:static\s+|inline\s+)*[\w]+[\w\s*]*[\s*]\**(\w+)\s*\([^;{]*\)\s*\{?\s*$")
DEF_RE = re.compile(r"^#define\s+\w+")
STRUCT_RE = re.compile(r"^(?:typedef\s+)?(?:struct|enum|union)\s+\w*")
INCLUDE_RE = re.compile(r'^#include\s+["<]')
LOCK_RE = re.compile(r"spin_lock|spin_unlock|mutex_|__atomic|smp_mb|barrier\(")
CALL_RE = re.compile(r"\b(\w{4,})\s*\(")
C_KEYWORDS = {"sizeof", "return", "while", "switch", "_Static_assert",
              "typeof", "offsetof", "defined"}


def git(root, *args):
    r = subprocess.run(["git", "-C", str(root), *args],
                       capture_output=True, text=True, timeout=60)
    return r.stdout if r.returncode == 0 else ""


def structural_view(path: Path, rel: str) -> str:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return f"## {rel}\n(unreadable)\n"
    out = [f"## {rel} ({len(lines)} lines)"]
    includes, defines, structs, funcs, locks = [], [], [], [], []
    current_fn, callees = None, set()

    def flush_fn():
        if current_fn:
            named = sorted(c for c in callees if c not in C_KEYWORDS)[:12]
            funcs.append(f"  {current_fn[1]}: {current_fn[0]}"
                         + (f"  -> calls: {', '.join(named)}" if named else ""))

    depth = 0
    for i, ln in enumerate(lines, 1):
        s = ln.strip()
        if INCLUDE_RE.match(s):
            includes.append(s)
        elif DEF_RE.match(s) and depth == 0:
            defines.append(f"  {i}: {s[:140]}")
        elif STRUCT_RE.match(s) and depth == 0:
            structs.append(f"  {i}: {s[:140]}")
        elif depth == 0 and SIG_RE.match(s) and not s.startswith(("if", "for")):
            flush_fn()
            current_fn, callees = (s[:160].rstrip("{").strip(), i), set()
        elif current_fn and depth > 0:
            callees.update(CALL_RE.findall(ln))
        if LOCK_RE.search(ln):
            locks.append(f"  {i}: {s[:140]}")
        depth += ln.count("{") - ln.count("}")
    flush_fn()

    if includes:
        out.append("includes: " + ", ".join(
            i.split()[-1].strip('"<>') for i in includes[:20]))
    for label, items, cap in (("defines", defines, 40), ("types", structs, 30),
                              ("functions (line: signature -> callees)", funcs, 60),
                              ("lock/atomic touchpoints", locks, 40)):
        if items:
            out.append(f"{label}:")
            out.extend(items[:cap])
            if len(items) > cap:
                out.append(f"  ... more_available: {len(items) - cap}")
    return "\n".join(out) + "\n"


def main(argv) -> int:
    root = Path(".").resolve()
    files: list = []
    manifest_obj = None
    if "--manifest" in argv:
        mref = argv[argv.index("--manifest") + 1]
        # "-" reads the manifest from stdin so `section-manifest.py ... |
        # evidence-bundle.py --manifest -` works without a temp file.
        raw = sys.stdin.read() if mref == "-" else Path(mref).read_text()
        manifest_obj = json.loads(raw)
        files = (manifest_obj.get("likely_files", [])
                 + manifest_obj.get("input_files", [])
                 + manifest_obj.get("relevant_tests", []))
        if manifest_obj.get("todo"):
            files.append(manifest_obj["todo"])
    elif "--files" in argv:
        files = argv[argv.index("--files") + 1:]
    files = [f for f in dict.fromkeys(files) if (root / f).exists()]
    if not files:
        print(json.dumps({"error": "no existing files to bundle"}))
        return 1

    # WORKING-TREE content hashes (untracked + unstaged included), NOT git-index
    # blobs. The bundle key must change the instant any input's bytes change,
    # or a mid-implementation session reuses a stale bundle (2026-07-11 fix).
    blobs = content_hashes(root, files)
    key = hashlib.sha256(json.dumps(sorted(blobs.items())).encode()
                         ).hexdigest()[:12]
    bdir = root / BUNDLE_DIR_REL / key
    if (bdir / "bundle.json").exists():
        print((bdir / "bundle.json").read_text())
        return 0
    bdir.mkdir(parents=True, exist_ok=True)

    views = [structural_view(root / f, f) for f in files
             if f.endswith((".c", ".h", ".asm", ".S"))]
    (bdir / "structure.md").write_text(
        "# Structural views (signatures/types/locks/call edges -- NOT full "
        "source; the main session slice-reads full source to verify claims)\n\n"
        + "\n".join(views), encoding="utf-8")
    diff = git(root, "diff", "HEAD", "--", *files)
    if diff:
        (bdir / "diff.patch").write_text(diff, encoding="utf-8")
    if manifest_obj:
        (bdir / "manifest.json").write_text(json.dumps(manifest_obj, indent=1))

    index = {"bundle_key": key, "dir": str(bdir.relative_to(root)),
             "files": files, "blob_hashes": blobs,
             "structure_bytes": (bdir / "structure.md").stat().st_size,
             "has_diff": bool(diff)}
    (bdir / "bundle.json").write_text(json.dumps(index, indent=1))
    # prune old bundles
    all_b = sorted((root / BUNDLE_DIR_REL).iterdir(),
                   key=lambda p: p.stat().st_mtime, reverse=True)
    for old in all_b[MAX_BUNDLES:]:
        subprocess.run(["rm", "-rf", str(old)], timeout=30)
    print(json.dumps(index, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
