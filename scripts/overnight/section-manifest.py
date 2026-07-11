#!/usr/bin/env python3
"""Deterministic section manifest -- Opus starts oriented, not exploring.

Generated BEFORE the implementing session touches a section (and fed as the
seed of the Sonnet enrichment dispatch): exact section text coordinates, open
items, XREFs, likely files, relevant tests, required gates, and current blob
hashes -- everything derivable without a model. The Sonnet mapper
(section-context-mapper / kernel-explorer) enriches this into the behavioral
package; Opus verifies and decides.

Usage: section-manifest.py <todo-path> <section-n> [--project DIR]
Output: JSON manifest on stdout.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from worktree_hash import content_hashes  # noqa: E402  (sibling helper)

SECTION_RE = re.compile(r"^## (\d+)\.\s*(.*)")
ITEM_RE = re.compile(r"^- \[([ x/])\]\s*(.*)")
XREF_RE = re.compile(r"XREF:\s*`?([^`\n]+)`?")
FILE_RE = re.compile(
    r"\b((?:src|include|user|tools|scripts)/[A-Za-z0-9_/.-]+\.(?:c|h|asm|S|sh|py|ld))\b")
# Boot-path surfaces that make the smoke test mandatory (CLAUDE.md list).
BOOT_PATHS = ("src/boot/", "src/kernel/main/boot_", "src/kernel/idt.c",
              "src/kernel/gdt.c", "src/kernel/msr.c", "src/kernel/smp/",
              "src/kernel/mm/pmm.c", "src/kernel/mm/vmm.c",
              "src/kernel/mm/heap.c", "src/kernel/drivers/lapic.c",
              "src/kernel/drivers/ioapic.c", "src/kernel/drivers/acpi.c",
              "src/kernel/drivers/timer.c")


def _git(root, *args):
    try:
        r = subprocess.run(["git", "-C", str(root), *args],
                           capture_output=True, text=True, timeout=30)
        return r.stdout if r.returncode == 0 else ""
    except Exception:
        return ""


def section_block(text: str, n: int) -> str:
    lines = text.splitlines()
    start = end = None
    for i, ln in enumerate(lines):
        m = SECTION_RE.match(ln)
        if m:
            if int(m.group(1)) == n and start is None:
                start = i
            elif start is not None:
                end = i
                break
    if start is None:
        return ""
    return "\n".join(lines[start:end])


def main(argv) -> int:
    if len(argv) < 2:
        print("usage: section-manifest.py <todo-path> <section-n> [--project DIR]",
              file=sys.stderr)
        return 2
    todo_rel, n = argv[0], int(argv[1])
    root = Path(argv[argv.index("--project") + 1]).resolve() \
        if "--project" in argv else Path(".").resolve()
    todo = root / todo_rel
    try:
        text = todo.read_text(encoding="utf-8")
    except OSError as exc:
        print(json.dumps({"error": f"unreadable TODO: {exc}"}))
        return 1
    block = section_block(text, n)
    if not block:
        print(json.dumps({"error": f"section {n} not found in {todo_rel}"}))
        return 1

    heading = block.splitlines()[0]
    open_items, done_items = [], 0
    for ln in block.splitlines():
        mm = ITEM_RE.match(ln.strip())
        if not mm:
            continue
        if mm.group(1) == "x":
            done_items += 1
        else:
            open_items.append({"status": mm.group(1),
                               "text": mm.group(2)[:220]})

    xrefs = sorted({x.strip()[:160] for x in XREF_RE.findall(block)})
    likely_files = sorted({f for f in FILE_RE.findall(block)
                           if (root / f).exists()})
    # Also surface preamble Inputs paths (shared integration surface).
    preamble = text.split("## 1.", 1)[0]
    input_files = sorted({f for f in FILE_RE.findall(preamble)
                          if (root / f).exists() and f not in likely_files})

    # Relevant tests: any test file sharing a basename token with a likely file.
    tokens = {Path(f).stem for f in likely_files if f.startswith(("src/", "include/"))}
    tests = []
    for tf in sorted((root / "src/kernel/test").glob("test_*.c")):
        stem = tf.stem[5:]  # drop test_
        if any(stem in t or t in stem for t in tokens if len(t) > 3):
            tests.append(str(tf.relative_to(root)))

    kernelish = any(f.startswith(("src/kernel/", "include/kernel/"))
                    for f in likely_files)
    bootish = any(f.startswith("src/boot/") or
                  any(f.startswith(bp) for bp in BOOT_PATHS)
                  for f in likely_files)
    gates = ["codex design review (pre-edit)",
             "codex adversarial-impl + fix loop",
             "build (=== BUILD OK ===) + test.sh green",
             "review-todo-section: adversarial + consistency + perf "
             "(+ re-adversarial on triggers)",
             "receiving-code-review on every findings set",
             "section commit gate (content-bound receipts)"]
    if kernelish:
        gates.insert(0, "kernel-code-quality gates (auto)")
        gates.append("kernel-quality-auditor (review step 7)")
    if bootish:
        gates.insert(0, "boot-code-quality gates (auto)")
        gates.append("smoke test (boot-path change)")

    # Content hashes: TODO + likely files. WORKING-TREE bytes (untracked +
    # unstaged included), NOT git-index blobs -- a mid-implementation session
    # has unstaged/untracked changes, and the run executes the live tree, so
    # index-blob binding would reuse stale evidence (2026-07-11 fix).
    blobs = content_hashes(root, [todo_rel, *likely_files])

    # Complexity budget (authoring-time split signal): a section likely to
    # exceed one fresh worker context should be SPLIT before implementation;
    # the quality pipeline runs per resulting section either way.
    subsystems = sorted({"/".join(f.split("/")[:2]) for f in likely_files})
    abi_impact = bool(re.search(
        r"(?i)\b(ABI|NTSTATUS|SSDT|boot_info|struct offset|BOOT_INFO_VERSION"
        r"|syscall number|PEB|TEB)\b", block))
    split_reasons = []
    if len(likely_files) > 8:
        split_reasons.append(f"{len(likely_files)} files")
    if len(open_items) > 12:
        split_reasons.append(f"{len(open_items)} open items")
    if len(subsystems) > 3:
        split_reasons.append(f"{len(subsystems)} subsystems")
    if abi_impact and len(open_items) > 8:
        split_reasons.append("ABI impact + wide item list")
    complexity = {
        "files": len(likely_files) + len(input_files),
        "subsystems": subsystems,
        "open_items": len(open_items),
        "abi_impact": abi_impact,
        "verdict": ("SPLIT-RECOMMENDED (" + "; ".join(split_reasons) + ")")
        if split_reasons else "fits-one-context",
    }

    manifest = {
        "todo": todo_rel, "section": n, "heading": heading,
        "open_items": open_items, "done_items": done_items,
        "xrefs": xrefs, "likely_files": likely_files,
        "input_files": input_files, "relevant_tests": tests,
        "required_gates": gates,
        # Working-tree content hashes (see the content_hashes call above). Key
        # kept as blob_hashes for reader compatibility; values are sha256 of
        # current bytes, not index blobs.
        "blob_hashes": blobs,
        "complexity": complexity,
        "enrich_with": ("kernel-explorer" if (kernelish or bootish)
                        else "section-context-mapper"),
    }
    out = json.dumps(manifest, indent=1)
    # --out PATH persists the manifest so evidence-bundle.py --manifest <PATH>
    # can consume it (the chain was broken: the manifest was only ever printed,
    # and piping it through head truncated the JSON so no bundle was produced).
    if "--out" in argv:
        dest = Path(argv[argv.index("--out") + 1])
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(out, encoding="utf-8")
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
