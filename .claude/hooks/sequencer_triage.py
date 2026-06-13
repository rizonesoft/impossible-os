#!/usr/bin/env python3
"""
sequencer_triage.py -- triage/traversal oracle for the overnight sequencer.

Pure read-only classifier over the todo-graph cache (build/todo-cache.json)
plus a markdown scan for `> **Verified:**` stamps. The overnight-sequencer
skill and run_phase_guard.py use it to decide, deterministically:

  * which TODO file to work next (full-repo numeric traversal), and
  * for a given file, which sections are DONE / DONE_UNSTAMPED / NEEDS_WORK.

Classes
  DONE           section [x] (or shipped-with-deferral [/]) AND Verified-stamped
  DONE_UNSTAMPED section [x] but no Verified stamp (likely old-system work)
  NEEDS_WORK     [ ] / blank, or a [/] with no Verified stamp (still in progress)

File class = DONE if every section DONE; DONE_UNSTAMPED if every section is
DONE/DONE_UNSTAMPED and at least one is unstamped; else NEEDS_WORK.

Blocked-vs-runnable on a NEEDS_WORK section is intentionally NOT decided here:
the implement/review skills already defer blocked-with-XREF items, and the
sequencer's fixpoint loop retries them on a later pass. Triage only has to
avoid re-touching finished work and to pick a stable traversal order.

CLI
  sequencer_triage.py --next            -> next file to work (path + class), or DONE
  sequencer_triage.py --classify PATH   -> JSON: per-section classes for a file
  sequencer_triage.py --summary         -> table: every file + class
  sequencer_triage.py --selftest        -> structural self-checks, exit 0/1

Honors --cache PATH (default build/todo-cache.json) and --repo-root.
"""
import argparse
import json
import os
import re
import sys

SECTION_HEADING_RE = re.compile(r"^##\s+(\d+)\.")
VERIFIED_RE = re.compile(r"^>\s*\*\*Verified:\*\*")
TODO_NUM_RE = re.compile(r"/TODO-(\d+)-")
# An implementation TODO lives in a numbered domain dir and is named TODO-NN-*.
# This excludes INDEX.md and non-TODO doctrine files (e.g. the runner-doctrine
# TODO-Claude-Overnight-Runner.md), which the runner must never "work".
IMPL_TODO_RE = re.compile(r"(?:^|/)todo/\d\d-[^/]+/TODO-\d+-[^/]*\.md$")


def is_impl_todo(file_path):
    return bool(IMPL_TODO_RE.search(file_path.replace("\\", "/")))

DONE = "DONE"
DONE_UNSTAMPED = "DONE_UNSTAMPED"
NEEDS_WORK = "NEEDS_WORK"


def repo_root(override=None):
    if override:
        return os.path.abspath(override)
    # walk up from cwd looking for a .git dir
    d = os.path.abspath(os.getcwd())
    while d != "/":
        if os.path.isdir(os.path.join(d, ".git")):
            return d
        d = os.path.dirname(d)
    return os.path.abspath(os.getcwd())


def load_cache(cache_path, root):
    path = cache_path if os.path.isabs(cache_path) else os.path.join(root, cache_path)
    if not os.path.exists(path):
        raise FileNotFoundError(
            f"todo-graph cache not found: {path} "
            "(run: bash scripts/todo-graph/build-and-validate.sh --keep-cache)"
        )
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    if not isinstance(data, list):
        raise ValueError("unexpected cache shape: expected a top-level list")
    return data


def verified_sections(md_path):
    """Return the set of section numbers (n) carrying a `> **Verified:**` stamp."""
    out = set()
    if not os.path.exists(md_path):
        return out
    cur = None
    with open(md_path, encoding="utf-8") as fh:
        for line in fh:
            m = SECTION_HEADING_RE.match(line)
            if m:
                cur = int(m.group(1))
                continue
            if cur is not None and VERIFIED_RE.match(line):
                out.add(cur)
    return out


def classify_section(section, verified):
    st = (section.get("status") or "").strip()
    n = section.get("n")
    if st == "x":
        return DONE if n in verified else DONE_UNSTAMPED
    if st == "/":
        # A stamped [/] is shipped-with-a-deferral (done for now); an
        # unstamped [/] is still in progress.
        return DONE if n in verified else NEEDS_WORK
    return NEEDS_WORK


def classify_file(entry, root):
    md_path = os.path.join(root, entry["file_path"])
    verified = verified_sections(md_path)
    sections = entry.get("sections") or []
    if not sections:
        return NEEDS_WORK, []
    per = [(s.get("n"), classify_section(s, verified)) for s in sections]
    classes = [c for _, c in per]
    if all(c == DONE for c in classes):
        return DONE, per
    if all(c in (DONE, DONE_UNSTAMPED) for c in classes):
        return DONE_UNSTAMPED, per
    return NEEDS_WORK, per


def _todo_num(file_path):
    m = TODO_NUM_RE.search(file_path)
    return int(m.group(1)) if m else 9999


def traversal_order(cache):
    """Implementation TODOs only, domains numeric, files numeric within a domain.

    Non-TODO doctrine files (INDEX.md, the runner-doctrine file) are excluded:
    the runner never validates/gap-audits/implements them.
    """
    impl = [e for e in cache if is_impl_todo(e["file_path"])]
    return sorted(impl, key=lambda e: (e.get("domain", ""), _todo_num(e["file_path"])))


def next_file(cache, root):
    for entry in traversal_order(cache):
        cls, _ = classify_file(entry, root)
        if cls != DONE:
            return entry, cls
    return None, None


def cmd_next(cache, root):
    entry, cls = next_file(cache, root)
    if entry is None:
        print(json.dumps({"status": "DONE", "file": None}))
        return 0
    print(json.dumps({"status": cls, "file": entry["file_path"], "domain": entry["domain"]}))
    return 0


def cmd_classify(cache, root, target):
    target = target.replace("\\", "/")
    match = [e for e in cache if e["file_path"].endswith(target) or target.endswith(e["file_path"])]
    if not match:
        print(json.dumps({"error": f"file not in cache: {target}"}))
        return 1
    entry = match[0]
    cls, per = classify_file(entry, root)
    print(json.dumps({
        "file": entry["file_path"],
        "file_class": cls,
        "sections": [{"n": n, "class": c} for n, c in per],
    }, indent=1))
    return 0


def cmd_summary(cache, root):
    counts = {DONE: 0, DONE_UNSTAMPED: 0, NEEDS_WORK: 0}
    for entry in traversal_order(cache):
        cls, _ = classify_file(entry, root)
        counts[cls] += 1
        print(f"{cls:14s} {entry['file_path']}")
    print(f"\n-- {counts[DONE]} DONE | {counts[DONE_UNSTAMPED]} DONE_UNSTAMPED "
          f"| {counts[NEEDS_WORK]} NEEDS_WORK --")
    return 0


def cmd_selftest(cache, root):
    failures = []
    # 1. every entry classifies without raising and yields a known class.
    valid = {DONE, DONE_UNSTAMPED, NEEDS_WORK}
    for entry in cache:
        cls, per = classify_file(entry, root)
        if cls not in valid:
            failures.append(f"{entry['file_path']}: bad file class {cls!r}")
        for n, c in per:
            if c not in valid:
                failures.append(f"{entry['file_path']} sec {n}: bad class {c!r}")
    # 2. traversal covers exactly the implementation TODOs, no dupes, and
    #    excludes non-TODO doctrine files.
    order = traversal_order(cache)
    impl_count = sum(1 for e in cache if is_impl_todo(e["file_path"]))
    if len(order) != impl_count:
        failures.append("traversal dropped/added impl entries")
    if len({e["file_path"] for e in order}) != len(order):
        failures.append("traversal has duplicate entries")
    if any("TODO-Claude-Overnight-Runner" in e["file_path"] for e in order):
        failures.append("traversal included the runner-doctrine file")
    # 3. next_file is either None or a non-DONE file in the cache.
    entry, cls = next_file(cache, root)
    if entry is not None and cls == DONE:
        failures.append("next_file returned a DONE file")
    # 4. classify_section truth table.
    tt = [
        ({"n": 1, "status": "x"}, {1}, DONE),
        ({"n": 1, "status": "x"}, set(), DONE_UNSTAMPED),
        ({"n": 2, "status": "/"}, {2}, DONE),
        ({"n": 2, "status": "/"}, set(), NEEDS_WORK),
        ({"n": 3, "status": ""}, {3}, NEEDS_WORK),
        ({"n": 3, "status": " "}, set(), NEEDS_WORK),
    ]
    for sec, ver, want in tt:
        got = classify_section(sec, ver)
        if got != want:
            failures.append(f"truth-table {sec} ver={ver}: want {want} got {got}")
    if failures:
        for f in failures:
            print("FAIL:", f, file=sys.stderr)
        print(f"sequencer_triage selftest: {len(failures)} failure(s)", file=sys.stderr)
        return 1
    print(f"sequencer_triage selftest OK ({len(cache)} files classified)")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="overnight-sequencer triage oracle")
    ap.add_argument("--cache", default="build/todo-cache.json")
    ap.add_argument("--repo-root", default=None)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--next", action="store_true")
    g.add_argument("--classify", metavar="PATH")
    g.add_argument("--summary", action="store_true")
    g.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    root = repo_root(args.repo_root)
    cache = load_cache(args.cache, root)
    if args.next:
        return cmd_next(cache, root)
    if args.classify:
        return cmd_classify(cache, root, args.classify)
    if args.summary:
        return cmd_summary(cache, root)
    if args.selftest:
        return cmd_selftest(cache, root)
    return 2


if __name__ == "__main__":
    sys.exit(main())
