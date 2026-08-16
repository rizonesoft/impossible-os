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
              OR a [x]/[/] section carrying a TERMINAL Deferred stamp (parked).
  BLOCKED     a [x]/[/] section whose Deferred stamp carries an `awaiting-*`
              token (recoverable: operator answer, hardware, infra). The run
              stays armed; fixpoint refuses (3-state oracle, runner-kit
              2026-07-03 -- prevents a mass recoverable-park reading as DONE).
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
from pathlib import Path
import re
import sys

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

# THE SHARED PARSER (TODO-06 consolidation). This hook is the triage oracle --
# the authority every sequencer phase classifies from -- and it kept a private
# column-zero `## N.` grammar after the producer and gates moved onto the
# shared projection. Measured consequence (v14 finding, 2026-08-12): for
# `## 1. Root` / `- ## 2. Nested` / stamps under the nested heading, this walk
# never advanced past section 1 and assigned BOTH stamps to it, so an
# unstamped shipped root read DONE (unreviewed work skipped) while the stamped
# nested section read NEEDS_WORK (completed work looped).
#
# LOADED LAZILY, not at module import. `run_phase_guard.py` imports this module
# for verbs that never touch section parsing (cursor/phase/rotation), and the
# runner suite copies this file ALONE into stripped fixtures; a top-level
# `import todo_fence` would make the whole module unimportable there. The
# import happens inside `_scan_sections`, so only the stamp/lifecycle walks
# require the parser. Failure raises loudly -- a silent fallback to a private
# grammar would be the exact drift this consolidation removes.
_fence = None


def _load_fence():
    global _fence
    if _fence is None:
        scripts_dir = str(Path(__file__).resolve().parents[2] / "scripts")
        if scripts_dir not in sys.path:
            sys.path.insert(0, scripts_dir)
        import todo_fence as _f
        _fence = _f
    return _fence


def is_impl_todo(file_path):
    return bool(IMPL_TODO_RE.search(file_path.replace("\\", "/")))

DONE = "DONE"
NEEDS_WORK = "NEEDS_WORK"
BLOCKED = "BLOCKED"

# Recoverable-vs-terminal deferral split (runner-kit 3-state oracle, adopted
# 2026-07-03). A `> **Deferred:**` stamp carrying an `awaiting-<token>` word
# (awaiting-answer, awaiting-hardware, awaiting-infra, ...) marks a blocker
# that is expected to CLEAR (operator answers todo/answers.md, hardware
# arrives, infra lands) -- the section reads BLOCKED, the file can never
# reach DONE through it, and the run stays armed instead of reporting a
# false fixpoint. A Deferred stamp WITHOUT the token is terminal-parked
# (resolved out-of-band via its XREF owner) and stays DONE-equivalent,
# preserving the pre-2026-07-03 behavior for every existing stamp.
DEFERRED_RECOVERABLE_RE = re.compile(r"\bawaiting-[a-z0-9-]+\b", re.I)


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
    # VALIDATE, do not merely parse. A raw `json.load` accepts `[]` and any
    # half-written array, and an EMPTY cache makes every file classify DONE --
    # so `--next` answers {"status":"DONE","file":null} and the runner declares
    # the entire queue finished over work it simply could not see. Reproduced
    # 2026-08-06 (Codex review of the todo-metadata-layer section 17) and again
    # 2026-08-07 before this fix. A truncated cache is reachable in ordinary
    # operation: a concurrent build, a killed rebuild, a full disk.
    #
    # `cache_schema.load_and_validate` already rejects `[]` and malformed nodes,
    # and is what the other readers were routed through. Routing this one too
    # makes a corrupt cache an ERROR the run stops on, instead of a silent
    # completion verdict -- the difference between refusing and lying.
    #
    # Falls back to the raw parse ONLY if the validator cannot be imported, so a
    # missing scripts/ tree degrades to previous behaviour rather than wedging.
    try:
        sys.path.insert(0, os.path.join(root, "scripts", "todo-graph"))
        import cache_schema  # noqa: E402
        nodes, _info = cache_schema.load_and_validate(
            Path(path), Path(root) / "todo",
            check_stale=False, profile=cache_schema.PROFILE_SECTIONS)
        return nodes
    except ImportError:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
        if not isinstance(data, list):
            raise ValueError("unexpected cache shape: expected a top-level list")
        return data


def _scan_sections(text):
    """(scan, [(n_or_None, heading_line, body_start, body_end)]) -- shared walk.

    Mirrors `todo-reachability.py:_sections`, the projection the producer and
    gates already share: headings come from `leaf_views` (so `- ## 2. Nested`
    is a real section), fenced and blockquoted lines are never boundaries, and
    a body ends at the next `## ` of ANY kind -- closing matter such as
    `## Unit Tests` ends the last numbered section rather than inheriting its
    stamps. An over-long heading still delimits its section but has no usable
    number, so it yields None and nothing attributes to it.
    """
    fence = _load_fence()
    scan = fence.scan_text(text)
    mask, leaves = scan.mask, scan.leaf_views
    in_bq = scan.in_blockquote
    starts = [(i, h) for i, l in enumerate(leaves)
              if not mask[i] and not in_bq(i)
              and (h := fence.classify_heading(l)).kind != "none"]
    total = len(scan.lines)
    out = []
    for ln, head in starts:
        end = total
        for j in range(ln + 1, total):
            if not mask[j] and not in_bq(j) and fence.is_h2(leaves[j]):
                end = j
                break
        out.append((head.n if head.kind == "ok" else None, ln, ln + 1, end))
    return scan, out


def section_stamps(md_path):
    """Map section number -> set of stamp kinds present: 'V', 'Q', 'D'.

    V = `> **Verified:**`, Q = `> **Quality reviewed:**`, D = `> **Deferred:**`.
    Stamps are attributed through the SHARED projection (`_scan_sections`), so
    a nested heading owns the stamps under it and a fenced example never
    stamps. The stamp lines themselves stay PHYSICAL: every pattern anchors on
    `> ` and the projection strips that marker from a blockquote's
    continuation lines (see the measured failure in `todo-reachability.py`'s
    section walk), so matching projected lines would drop `Quality reviewed`
    from an ordinary two-line stamp block.
    """
    out = {}
    if not os.path.exists(md_path):
        return out
    with open(md_path, encoding="utf-8") as fh:
        text = fh.read()
    scan, sections = _scan_sections(text)
    mask, lines = scan.mask, scan.lines
    for num, _ln, start, end in sections:
        if num is None:
            continue
        kinds = out.setdefault(num, set())
        for j in range(start, end):
            if mask[j]:
                continue
            phys = lines[j]
            if phys[:1] != ">":
                continue
            if VERIFIED_RE.match(phys):
                kinds.add("V")
            elif QUALITY_RE.match(phys):
                kinds.add("Q")
            elif DEFERRED_RE.match(phys):
                kinds.add("D")
                if DEFERRED_RECOVERABLE_RE.search(phys):
                    kinds.add("DR")  # recoverable deferral (awaiting-*)
    return out


def file_lifecycle(md_path, root=None, rel_path=None):
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
        text = fh.read()
    scan, sections = _scan_sections(text)
    # lifecycle stamps live in the preamble only -- before the first section
    # heading of any kind (numbered or over-long), per the shared projection.
    bound = sections[0][1] if sections else len(scan.lines)
    for j in range(bound):
        if scan.mask[j]:
            continue
        line = scan.lines[j]
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
    if "DR" in kinds:
        return BLOCKED
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
    classes = {c for _, c in per}
    if classes == {DONE}:
        cls = DONE
    elif NEEDS_WORK in classes:
        cls = NEEDS_WORK
    else:
        cls = BLOCKED  # only DONE + recoverable-deferred sections remain
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
    """First NEEDS_WORK file in traversal order, else (None, BLOCKED) when
    only recoverable-deferred files remain, else (None, None) at true DONE."""
    saw_blocked = False
    for entry in traversal_order(cache):
        cls, _ = classify_file(entry, root)
        if cls == NEEDS_WORK:
            return entry, cls
        if cls == BLOCKED:
            saw_blocked = True
    return None, (BLOCKED if saw_blocked else None)


def collect_blockers(cache, root):
    """All recoverable-deferred sections: (file, section n, awaiting token)."""
    out = []
    for entry in traversal_order(cache):
        md_path = os.path.join(root, entry["file_path"])
        stamps = section_stamps(md_path)
        for s in entry.get("sections") or []:
            if classify_section(s, stamps) != BLOCKED:
                continue
            token = ""
            try:
                with open(md_path, encoding="utf-8") as fh:
                    text = fh.read()
                scan, sections = _scan_sections(text)
                for num, _ln, start, end in sections:
                    if num != s.get("n"):
                        continue
                    for j in range(start, end):
                        if scan.mask[j]:
                            continue
                        if DEFERRED_RE.match(scan.lines[j]):
                            mm = DEFERRED_RECOVERABLE_RE.search(scan.lines[j])
                            if mm:
                                token = mm.group(0).lower()
                            break
                    break
            except OSError:
                pass
            out.append({"file": entry["file_path"], "section": s.get("n"),
                        "awaiting": token})
    return out


def cmd_blockers(cache, root):
    print(json.dumps({"blockers": collect_blockers(cache, root)}))
    return 0


def cmd_next(cache, root):
    entry, cls = next_file(cache, root)
    if entry is None:
        if cls == BLOCKED:
            print(json.dumps({"status": BLOCKED, "file": None,
                              "blockers": collect_blockers(cache, root)}))
            return 0
        print(json.dumps({"status": "DONE", "file": None}))
        return 0
    lc = file_lifecycle(os.path.join(root, entry["file_path"]), root, entry["file_path"])
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
    lc = file_lifecycle(os.path.join(root, entry["file_path"]), root, entry["file_path"])
    print(json.dumps({
        "file": entry["file_path"],
        "file_class": cls,
        "lifecycle": lc,
        "stages_1_2_done": lc["validated"] and lc["gap_audited"],
        "sections": [{"n": n, "class": c} for n, c in per],
    }, indent=1))
    return 0


def cmd_summary(cache, root):
    # All THREE classes must be seeded: classify_file returns BLOCKED for a file
    # whose remaining sections are all DONE-or-recoverably-deferred, and the
    # two-key dict crashed with KeyError('BLOCKED') the moment one existed
    # (found 2026-07-27 -- the live tree has such files, so --summary was dead
    # for the operator while --next, a separate path, kept working).
    counts = {DONE: 0, NEEDS_WORK: 0, BLOCKED: 0}
    for entry in traversal_order(cache):
        cls, _ = classify_file(entry, root)
        counts[cls] = counts.get(cls, 0) + 1
        print(f"{cls:14s} {entry['file_path']}")
    print(f"\n-- {counts[DONE]} DONE | {counts[NEEDS_WORK]} NEEDS_WORK "
          f"| {counts[BLOCKED]} BLOCKED --")
    return 0


def cmd_selftest(cache, root):
    failures = []
    # 1. every entry classifies without raising and yields a known class.
    valid = {DONE, NEEDS_WORK, BLOCKED}
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
        # naturally-deferred section: TERMINAL Deferred stamp parks it, no V/Q needed
        ({"n": 4, "status": "/"}, {4: {"D"}}, DONE),
        ({"n": 4, "status": "x"}, {4: {"D"}}, DONE),
        # recoverable deferral (awaiting-*): BLOCKED, never DONE (3-state oracle)
        ({"n": 5, "status": "/"}, {5: {"D", "DR"}}, BLOCKED),
        ({"n": 5, "status": "x"}, {5: {"D", "DR"}}, BLOCKED),
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
    g.add_argument("--blockers", action="store_true")
    g.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    root = repo_root(args.repo_root)
    try:
        cache = load_cache(args.cache, root)
    except Exception as exc:
        # A cache this reader cannot trust must STOP the run, with a diagnosis.
        # The failure mode being guarded is not a crash -- it is the opposite: a
        # corrupt cache used to classify every file DONE, so `--next` answered
        # "the queue is finished" over work it could not see. Refusing loudly is
        # the whole point, so the message has to say what to do about it.
        name = type(exc).__name__
        sys.stderr.write(
            f"[sequencer_triage] REFUSING to classify: the todo-graph cache is "
            f"unusable ({name}: {str(exc)[:200]}).\n"
            f"  This is NOT 'the queue is done' -- it is 'the queue cannot be "
            f"read'. Treating it as DONE would silently complete unfinished work.\n"
            f"  Rebuild: bash scripts/todo-graph/build-and-validate.sh --keep-cache\n")
        return 2
    if args.next:
        return cmd_next(cache, root)
    if args.blockers:
        return cmd_blockers(cache, root)
    if args.classify:
        return cmd_classify(cache, root, args.classify)
    if args.summary:
        return cmd_summary(cache, root)
    if args.selftest:
        return cmd_selftest(cache, root)
    return 2


if __name__ == "__main__":
    sys.exit(main())
