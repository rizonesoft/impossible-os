#!/usr/bin/env python3
"""
sequencer_triage.py -- triage/traversal oracle for the overnight sequencer.

Pure read-only classifier over the todo-graph cache (build/todo-cache.json)
plus a markdown scan for section review stamps. The overnight-sequencer skill
and run_phase_guard.py use it to decide, deterministically:

  * which TODO file to work next (full-repo numeric traversal), and
  * for a given file, which sections are DONE / NEEDS_WORK.

A section is DONE only if it is genuinely finished by the operator's definition:
either fully shipped-AND-reviewed (BOTH a `> **Verified:**` stamp AND a
`> **Quality reviewed:**` stamp), or naturally deferred (a `> **Deferred:**`
stamp parks it -- a deferred section legitimately carries neither Verified nor
Quality-reviewed). Everything else is NEEDS_WORK.

Classes
  DONE        shipped [x]/[/] with BOTH Verified AND Quality-reviewed stamps,
              OR a [x]/[/] section carrying a Deferred stamp (parked).
  NEEDS_WORK  anything else -- [ ]/blank, an in-progress [/], OR a shipped [x]
              that is missing either stamp (shipped-but-unreviewed is NOT done).

There is deliberately no "DONE_UNSTAMPED" class: a shipped section with no
review stamps is unreviewed work, not done. (Removed 2026-06-16 -- the prior
oracle trusted unstamped [x] as "old-system work" and silently skipped its
review; the operator's bar is shipped AND reviewed.)

File class = DONE if every section is DONE; else NEEDS_WORK.

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
QUALITY_RE = re.compile(r"^>\s*\*\*Quality reviewed:\*\*")
DEFERRED_RE = re.compile(r"^>\s*\*\*Deferred:\*\*")
# File-level lifecycle stamps -- live in the preamble (before the first
# `## N.` section) and record that the per-file pipeline Stages 1-2 ran:
# validate-todo-file and gap-audit-todo (+ its mandatory codex-gap-audit).
VALIDATED_RE = re.compile(r"^>\s*\*\*Validated:\*\*")
GAP_AUDITED_RE = re.compile(r"^>\s*\*\*Gap-audited:\*\*")
TODO_NUM_RE = re.compile(r"/TODO-(\d+)-")
# An implementation TODO lives in a numbered domain dir and is named TODO-NN-*.
# This excludes INDEX.md and non-TODO doctrine files (e.g. the runner-doctrine
# TODO-Claude-Overnight-Runner.md), which the runner must never "work".
IMPL_TODO_RE = re.compile(r"(?:^|/)todo/\d\d-[^/]+/TODO-\d+-[^/]*\.md$")


def is_impl_todo(file_path):
    return bool(IMPL_TODO_RE.search(file_path.replace("\\", "/")))

DONE = "DONE"
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


def section_stamps(md_path):
    """Map section number -> set of stamp kinds present: 'V', 'Q', 'D'.

    V = `> **Verified:**`, Q = `> **Quality reviewed:**`, D = `> **Deferred:**`.
    Stamps are attributed to the section heading they follow.
    """
    out = {}
    if not os.path.exists(md_path):
        return out
    cur = None
    with open(md_path, encoding="utf-8") as fh:
        for line in fh:
            m = SECTION_HEADING_RE.match(line)
            if m:
                cur = int(m.group(1))
                out.setdefault(cur, set())
                continue
            if cur is None:
                continue
            if VERIFIED_RE.match(line):
                out[cur].add("V")
            elif QUALITY_RE.match(line):
                out[cur].add("Q")
            elif DEFERRED_RE.match(line):
                out[cur].add("D")
    return out


def file_lifecycle(md_path):
    """File-level lifecycle: did the per-file pipeline Stages 1-2 run?

    Scans only the preamble (everything before the first `## N.` section
    heading) for `> **Validated:**` and `> **Gap-audited:**` stamps. Returns
    {'validated': bool, 'gap_audited': bool}. The sequencer uses this to SKIP
    re-running validate-todo-file / gap-audit-todo on a file that already
    carries both stamps -- the root-cause guard against the fixpoint loop
    re-auditing a mature file every sweep. Orthogonal to section DONE-ness:
    a file can be validated+gap-audited while sections are still in progress.
    """
    out = {"validated": False, "gap_audited": False}
    if not os.path.exists(md_path):
        return out
    with open(md_path, encoding="utf-8") as fh:
        for line in fh:
            if SECTION_HEADING_RE.match(line):
                break  # lifecycle stamps live in the preamble only
            if VALIDATED_RE.match(line):
                out["validated"] = True
            elif GAP_AUDITED_RE.match(line):
                out["gap_audited"] = True
    return out


def classify_section(section, stamps):
    """DONE iff shipped AND (Verified AND Quality-reviewed) OR Deferred-parked.

    `stamps` maps section number -> set of stamp kinds (see section_stamps).
    A bare [ ] is never done. A shipped [x]/[/] is done only with BOTH the
    Verified and Quality-reviewed stamps; either alone (or none) is NEEDS_WORK.
    A Deferred stamp parks a [x]/[/] section as done-for-now (it legitimately
    carries neither Verified nor Quality-reviewed).
    """
    st = (section.get("status") or "").strip()
    n = section.get("n")
    if st not in ("x", "/"):
        return NEEDS_WORK
    kinds = stamps.get(n, frozenset())
    if "D" in kinds:
        return DONE
    if "V" in kinds and "Q" in kinds:
        return DONE
    return NEEDS_WORK


def classify_file(entry, root):
    md_path = os.path.join(root, entry["file_path"])
    stamps = section_stamps(md_path)
    sections = entry.get("sections") or []
    if not sections:
        return NEEDS_WORK, []
    per = [(s.get("n"), classify_section(s, stamps)) for s in sections]
    cls = DONE if all(c == DONE for _, c in per) else NEEDS_WORK
    return cls, per


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
    lc = file_lifecycle(os.path.join(root, entry["file_path"]))
    print(json.dumps({
        "status": cls,
        "file": entry["file_path"],
        "domain": entry["domain"],
        "lifecycle": lc,
        "stages_1_2_done": lc["validated"] and lc["gap_audited"],
    }))
    return 0


def cmd_classify(cache, root, target):
    target = target.replace("\\", "/")
    match = [e for e in cache if e["file_path"].endswith(target) or target.endswith(e["file_path"])]
    if not match:
        print(json.dumps({"error": f"file not in cache: {target}"}))
        return 1
    entry = match[0]
    cls, per = classify_file(entry, root)
    lc = file_lifecycle(os.path.join(root, entry["file_path"]))
    print(json.dumps({
        "file": entry["file_path"],
        "file_class": cls,
        "lifecycle": lc,
        "stages_1_2_done": lc["validated"] and lc["gap_audited"],
        "sections": [{"n": n, "class": c} for n, c in per],
    }, indent=1))
    return 0


def cmd_summary(cache, root):
    counts = {DONE: 0, NEEDS_WORK: 0}
    for entry in traversal_order(cache):
        cls, _ = classify_file(entry, root)
        counts[cls] += 1
        print(f"{cls:14s} {entry['file_path']}")
    print(f"\n-- {counts[DONE]} DONE | {counts[NEEDS_WORK]} NEEDS_WORK --")
    return 0


def cmd_selftest(cache, root):
    failures = []
    # 1. every entry classifies without raising and yields a known class.
    valid = {DONE, NEEDS_WORK}
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
    # 4. classify_section truth table. stamps maps n -> set of 'V'/'Q'/'D'.
    tt = [
        # shipped [x] needs BOTH stamps to be DONE
        ({"n": 1, "status": "x"}, {1: {"V", "Q"}}, DONE),
        ({"n": 1, "status": "x"}, {1: {"V"}}, NEEDS_WORK),       # verified-only is NOT done
        ({"n": 1, "status": "x"}, {1: {"Q"}}, NEEDS_WORK),       # quality-only is NOT done
        ({"n": 1, "status": "x"}, {}, NEEDS_WORK),               # unstamped shipped is NOT done
        # shipped-with-deferral [/] needs both stamps too
        ({"n": 2, "status": "/"}, {2: {"V", "Q"}}, DONE),
        ({"n": 2, "status": "/"}, {2: {"V"}}, NEEDS_WORK),
        ({"n": 2, "status": "/"}, {}, NEEDS_WORK),
        # naturally-deferred section: Deferred stamp parks it, no V/Q needed
        ({"n": 4, "status": "/"}, {4: {"D"}}, DONE),
        ({"n": 4, "status": "x"}, {4: {"D"}}, DONE),
        # bare [ ]/blank is never done, even with stray stamps
        ({"n": 3, "status": ""}, {3: {"V", "Q"}}, NEEDS_WORK),
        ({"n": 3, "status": " "}, {}, NEEDS_WORK),
    ]
    for sec, st, want in tt:
        got = classify_section(sec, st)
        if got != want:
            failures.append(f"truth-table {sec} stamps={st}: want {want} got {got}")
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
