#!/usr/bin/env python3
"""Coverage report and 1:1 ownership check for the Windows 11 inventory.

Owner: todo/08-graphics-ui/TODO-17-win11-parity-inventory.md section 7.

Writes docs/design/inventory/COVERAGE.md: records per surface and kind,
dispositions per surface, each baseline's matched, absent, pending and
unmatched rows, and how many owned records are implemented.

Ownership. A roadmap section owns a record when its body carries an
`**Inventory:**` line naming the record's id, or a prefix of it (the id's
leading dotted parts). The longest prefix that names a record wins; two
sections naming it at that same length are two owners. A `build` or
`equivalent` record needs exactly one owner. A `replaced` record needs none
(its `mergedInto` target does) and is counted apart; an `excluded` record and
a catalogue record (shortcut, metric, state and the like) need none. A `flow`
or `home` entry of consistency.json is owned by `**Inventory:** ledger:<id>`.
An `**Inventory:**` line with no such token (prose about the files a capture
section writes) owns nothing.

Implemented. An owned record counts as implemented when its owning section is
done in the implementation plan (todo-plan.py's classification, from a graph
built fresh for this run) AND carries both the Verified and the Quality
reviewed stamp (sequencer_triage.section_stamps): a section closed by a
Deferred stamp is done but implemented nothing.

  inventory-report.py                    write COVERAGE.md
  inventory-report.py --check            exit 1 when COVERAGE.md is stale
  inventory-report.py --owners --check   exit 1 on every ownerless or doubly owned record
  inventory-report.py --campaign --check exit 1 on every capture-matrix row without evidence
  --index                                judge the staged todo/ and inventory, not the working tree

The capture matrix is docs/design/inventory/expected.json (section 19), a JSON
array of rows {surface, state, profile, needs, fixture?, source?, reason?}.
`needs` lists the evidence a row requires: "records" (a canonical record of the
surface), "shot" (a PNG in shots.json for that surface, state and profile) and
"recording" (a video entry there). A row whose `source` is "absent" is a
documented absence: it needs a reason and a `sibling` (surface/state of a
captured row it is designed from) and no evidence. A row whose `source` is
"stand-in" names in `standin` an entry of stand-ins.json (section 21) whose
variant exists in standin/variants.json and whose proof screenshot is hashed. The campaign check also
fails on every baseline row still pending or unmatched and on every element a
capture left without a hand label (build/inventory/<surface>-unnamed.json
whose record still carries the automatic fallback label).
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import re
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[1]
INV_REL = pathlib.PurePosixPath("docs/design/inventory")
COVERAGE = "COVERAGE.md"
AUTHORING = "authoring.json"
EXPECTED = "expected.json"
STAND_INS = "stand-ins.json"
STANDIN_VARIANTS_REL = pathlib.PurePosixPath("scripts/inventory/standin/variants.json")
FIXTURES_REL = pathlib.PurePosixPath("scripts/inventory/fixtures")
GIT_TIMEOUT = 120  # deadline: the staged todo/ and inventory are a few thousand small files
OWNER_LINE = re.compile(r"^\*\*Inventory:\*\*(?P<rest>.*)$")
TOKEN = re.compile(r"(?<![\w.:-])(win11(?:\.[a-z0-9-]+)+|ledger:[a-z0-9-]+)(?![\w.-])")
NEED_OWNER = ("build", "equivalent")
LEDGER_OWNED = ("flow", "home")
NEEDS = ("records", "shot", "recording")
SOURCES = ("stand-in", "host", "absent")
VIDEO = (".mp4", ".webm")


def _module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _capture_core():
    """capture_core.py, which holds the fixture spec check capture.py uses."""
    return _module("capture_core", HERE / "capture_core.py")


def _inventory_checker():
    return _module("check_inventory", HERE / "check-inventory.py")


class ReportError(Exception):
    pass


# --- what the inventory holds ------------------------------------------------

def load_records(inv, checker):
    """id -> record for every canonical record (observations are measurements
    of an existing record, not records of their own). A file the checker would
    refuse is an error here too: a report over a broken tree is not a count."""
    records = {}
    for path, n, kind, _profile, _mine, obj, error in checker.load(inv, None):
        if kind != "record":
            continue
        if error or not isinstance(obj, dict) or not isinstance(obj.get("id"), str):
            raise ReportError(f"{path.relative_to(inv)}:{n}: not a record ({error or 'no id'}); run check-inventory.py")
        records.setdefault(obj["id"], obj)
    return records


def baselines(inv, checker):
    """The baseline summary over the records, without validating them again:
    lint Check 40 judges the same tree, and check-inventory.py stays the one
    place the matching rule lives."""
    found = {}
    for path, n, kind, _profile, _mine, obj, error in checker.load(inv, None):
        if kind == "record" and not error and isinstance(obj, dict) and isinstance(obj.get("id"), str):
            found.setdefault(obj["id"], (path, n, obj, True))
    return checker.baseline_summary(inv, None, found, lambda *a: None)


def load_ledger(inv):
    path = inv / "consistency.json"
    if not path.is_file():
        return {}
    entries = json.loads(path.read_text(encoding="utf-8"))
    return {e["id"]: e for e in entries if isinstance(e, dict) and "id" in e}


def load_json(path, default):
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else default


# --- who owns what -------------------------------------------------------------

def owner_lines(todo_root, triage):
    """[(token, (relative path, section number))] from every **Inventory:**
    line in a numbered section body. Sections come from the projection the
    graph and the gates share (sequencer_triage._scan_sections), so a nested
    `- ## 2.` heading is a section and fenced or blockquoted lines are not
    read."""
    out = []
    for md in sorted(todo_root.rglob("*.md")):
        rel = md.relative_to(todo_root.parent).as_posix()
        scan, sections = triage._scan_sections(md.read_text(encoding="utf-8"))
        for n, _head, body_start, body_end in sections:
            if n is None:
                continue
            for i in range(body_start, body_end):
                if scan.mask[i] or scan.in_blockquote(i):
                    continue
                m = OWNER_LINE.match(scan.lines[i])
                if m:
                    out.extend((tok, (rel, n)) for tok in TOKEN.findall(m.group("rest")))
    return out


def resolve_owners(records, ledger, lines):
    """(owners, problems): owners maps a record id or ledger:<id> to its one
    owning section; problems are the lines --owners --check prints."""
    by_token = {}
    for tok, sec in lines:
        by_token.setdefault(tok, set()).add(sec)
    owners, problems, used, doubled = {}, [], set(), set()
    for rid in sorted(records):
        parts = rid.split(".")
        for length in range(len(parts), 1, -1):     # longest prefix first; "win11" alone names nothing
            secs = by_token.get(".".join(parts[:length]))
            if secs:
                used.add(".".join(parts[:length]))
                if len(secs) > 1:
                    problems.append(f"two owners: {rid}")
                    doubled.add(rid)
                else:
                    owners[rid] = next(iter(secs))
                break
    for lid in sorted(ledger):
        tok = f"ledger:{lid}"
        secs = by_token.get(tok)
        if secs:
            used.add(tok)
            if len(secs) > 1:
                problems.append(f"two owners: {tok}")
                doubled.add(tok)
            else:
                owners[tok] = next(iter(secs))
    for rid in sorted(records):
        if records[rid].get("disposition") in NEED_OWNER and rid not in owners and rid not in doubled:
            problems.append(f"no owner: {rid}")
    for lid in sorted(ledger):
        tok = f"ledger:{lid}"
        if ledger[lid].get("kind") in LEDGER_OWNED and tok not in owners and tok not in doubled:
            problems.append(f"no owner: {tok}")
    for tok in sorted(set(by_token) - used):
        for path, n in sorted(by_token[tok]):
            problems.append(f"owns nothing: {tok} ({path} section {n})")
    # One line per subject, in the subject's order: a reader scans by id.
    problems.sort(key=lambda p: (p.split(": ", 1)[1], p))
    return owners, problems


# --- what is implemented ----------------------------------------------------------

def _triage():
    return _module("sequencer_triage", REPO / ".claude" / "hooks" / "sequencer_triage.py")


def implemented_sections(root):
    """{(path, n)} of sections done in the plan AND stamped Verified and
    Quality reviewed. The graph is built fresh for this run (todo-plan.py)."""
    plan = _module("todo_plan", REPO / "scripts" / "todo-plan.py")
    triage = _triage()
    try:
        sections = plan.load_sections(root)
    except plan.PlanError as exc:
        raise ReportError(f"the implementation plan could not be read: {exc}") from None
    done, stamps = set(), {}
    for s in sections.values():
        if s.box != "x":
            continue
        if s.path not in stamps:
            stamps[s.path] = triage.section_stamps(root / s.path)
        if {"V", "Q"} <= stamps[s.path].get(s.n, set()):
            done.add((s.path, s.n))
    return done


# --- the report ---------------------------------------------------------------------

def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "| " + " | ".join("---" for _ in head) + " |"]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return out


def render(records, ledger, owners, problems, summary, implemented, authoring):
    surfaces = sorted({r["surface"] for r in records.values()})
    lines = ["# Windows 11 Inventory Coverage", "",
             "Generated by `scripts/inventory/inventory-report.py` (the Windows 11 parity inventory roadmap, coverage report section); never edit it by hand.", ""]

    lines += ["## How many records does each surface hold?", ""]
    kinds = sorted({(r["surface"], r["kind"]) for r in records.values()})
    lines += table(["Surface", "Kind", "Records"],
                   [(s, k, sum(1 for r in records.values() if (r["surface"], r["kind"]) == (s, k))) for s, k in kinds])
    if not kinds:
        lines.append("| (none) | | 0 |")

    lines += ["", "## How is each surface's design decided?", ""]
    cols = ("build", "equivalent", "replaced", "excluded")
    rows = []
    for s in surfaces:
        mine = [r for r in records.values() if r["surface"] == s]
        rows.append([s] + [sum(1 for r in mine if r.get("disposition") == c) for c in cols]
                    + [sum(1 for r in mine if "disposition" not in r)])
    lines += table(["Surface", *cols, "catalogue or undecided"], rows)

    lines += ["", "## How complete is each baseline?", ""]
    lines += table(["Baseline", "Owner", "Rows", "Matched", "Absent", "Pending", "Unmatched"],
                   [(n, c["owner"], c["rows"], c["matched"], c["absent"], c["pending"], c["unmatched"])
                    for n, c in sorted(summary.items())])

    lines += ["", "## Who builds each surface, and how much is done?", ""]
    rows = []
    for s in surfaces:
        need = [rid for rid, r in records.items() if r["surface"] == s and r.get("disposition") in NEED_OWNER]
        owned = [rid for rid in need if rid in owners]
        done = [rid for rid in owned if owners[rid] in implemented]
        replaced = sum(1 for r in records.values() if r["surface"] == s and r.get("disposition") == "replaced")
        rows.append((s, len(need), len(owned), len(done), replaced))
    lines += table(["Surface", "Build or equivalent", "Owned", "Implemented", "Replaced"], rows)

    lines += ["", "## Who owns each consistency decision?", ""]
    lrows = []
    for lid, e in sorted(ledger.items()):
        if e.get("kind") not in LEDGER_OWNED:
            continue
        sec = owners.get(f"ledger:{lid}")
        lrows.append((lid, e["kind"], f"{sec[0]} section {sec[1]}" if sec else "none",
                      "yes" if sec in implemented else "no"))
    lines += table(["Entry", "Kind", "Owner", "Implemented"], lrows)

    pending = [s for s in surfaces if s not in authoring]
    lines += ["", "## Which surfaces still wait for their roadmap sections?", ""]
    lines += [f"- {s}" for s in pending] or ["- none"]

    lines += ["", "## What does the ownership check report?", ""]
    lines += [f"- {p}" for p in problems] or ["- nothing: every record that needs an owner has exactly one"]
    return "\n".join(lines) + "\n"


# --- the capture campaign -----------------------------------------------------------

def standin_problem(sid, stand_ins, variants):
    """Why the stand-in `sid` does not prove its row, or None when it does."""
    if not isinstance(sid, str) or not sid:
        return "the row names no standin"
    entry = stand_ins.get(sid) if isinstance(stand_ins, dict) else None
    if not isinstance(entry, dict):
        return f"{sid} is not in {INV_REL / STAND_INS}"
    if entry.get("variant") not in variants:
        return f"{sid}: variant {entry.get('variant')!r} is not in {STANDIN_VARIANTS_REL}"
    proof = entry.get("proof")
    if not isinstance(proof, dict) or not proof.get("file") or not re.fullmatch(r"[0-9a-f]{64}", str(proof.get("sha256", ""))):
        return f"{sid} has no proof screenshot (standin/prove.py records one)"
    return None


def campaign_problems(root, inv, records, summary):
    problems = []
    path = inv / EXPECTED
    if not path.is_file():
        return [f"no capture matrix: {INV_REL / EXPECTED} does not exist (section 19 writes it)"]
    rows = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(rows, list) or not rows:
        return [f"no capture matrix: {INV_REL / EXPECTED} holds no rows"]
    shots = load_json(inv / "shots.json", [])
    profiles = load_json(inv / "profiles.json", {})
    stand_ins = load_json(inv / STAND_INS, {})
    standin_variants = load_json(root / STANDIN_VARIANTS_REL, {})
    # A sibling is a row someone captures: present in the matrix and not absent itself.
    captured = {f"{r.get('surface')}/{r.get('state')}" for r in rows
                if isinstance(r, dict) and r.get("source") != "absent"}
    have = {}
    for sh in shots:
        key = (sh.get("surface"), sh.get("state"), sh.get("profile"))
        have.setdefault(key, set()).add("recording" if str(sh.get("file", "")).endswith(VIDEO) else "shot")
    surfaces = {r["surface"] for r in records.values()}
    for i, row in enumerate(rows):
        key = (row.get("surface"), row.get("state"), row.get("profile"))
        name = "/".join(str(k) for k in key)
        needs = row.get("needs")
        if not all(isinstance(k, str) and k for k in key) or not isinstance(needs, list) or not set(needs) <= set(NEEDS):
            problems.append(f"bad row {i}: needs surface, state, profile and needs from {', '.join(NEEDS)}")
            continue
        if row["profile"] not in profiles:
            problems.append(f"unknown profile: {name} (not in {INV_REL / 'profiles.json'})")
            continue
        source = row.get("source")
        if source is not None and source not in SOURCES:
            problems.append(f"bad row {i}: source is one of {', '.join(SOURCES)}")
            continue
        fixture = row.get("fixture")
        if fixture is not None:
            # Rows stay open until their fixture exists, like rows without evidence.
            problem = _capture_core().fixture_problem(root / FIXTURES_REL, fixture)
            if problem:
                problems.append(f"no fixture: {name} ({problem})")
        if source == "absent":
            reason = row.get("reason")
            if not isinstance(reason, str) or not reason.strip():
                problems.append(f"absent without a reason: {name}")
            elif row.get("sibling") not in captured:
                problems.append(f"absent without a sibling: {name} (sibling names the surface/state of a captured row)")
            continue
        if source == "stand-in":
            problem = standin_problem(row.get("standin"), stand_ins, standin_variants)
            if problem:
                problems.append(f"unproven stand-in: {name} ({problem})")
        if "records" in needs and row["surface"] not in surfaces:
            problems.append(f"no records: {name}")
        for kind in ("shot", "recording"):
            if kind in needs and kind not in have.get(key, set()):
                problems.append(f"no {kind}: {name}")
    for n, c in sorted(summary.items()):
        if c["pending"] or c["unmatched"]:
            problems.append(f"baseline {n}: {c['pending']} pending and {c['unmatched']} unmatched row(s)")
    for side in sorted((root / "build" / "inventory").glob("*-unnamed.json")):
        for el in load_json(side, []):
            rec = records.get(el.get("id"))
            fallback = el.get("uiaId") or el.get("className") or el.get("controlType") or "element"
            if rec is None or rec.get("label") == fallback:
                problems.append(f"no label: {el.get('id')}")
    return problems


# --- entry -------------------------------------------------------------------------------

# What the staged report reads: its own code and every module it loads, so a
# commit is judged by the report it records, not by a working-tree edit.
SNAPSHOT = ("todo", str(INV_REL), "scripts", ".claude/hooks/sequencer_triage.py")


def staged_root(repo):
    """The paths in SNAPSHOT, as the index holds them."""
    tmp = tempfile.TemporaryDirectory(prefix="inventory-report-index-")
    try:
        names = subprocess.run(["git", "-C", str(repo), "ls-files", "-z", "--", *SNAPSHOT],
                               capture_output=True, timeout=GIT_TIMEOUT, check=True).stdout
        subprocess.run(["git", "-C", str(repo), "checkout-index", "-z", "--stdin", f"--prefix={tmp.name}/"],
                       input=names, capture_output=True, timeout=GIT_TIMEOUT, check=True)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as exc:
        tmp.cleanup()
        raise ReportError(f"the git index could not be read: {exc}") from None
    return tmp


def run(root, args):
    inv = root / INV_REL
    todo = root / "todo"
    checker = _inventory_checker()
    records = load_records(inv, checker)
    ledger = load_ledger(inv)
    owners, problems = resolve_owners(records, ledger, owner_lines(todo, _triage()) if todo.is_dir() else [])
    if args.owners:
        for p in problems:
            print(p)
        return 1 if args.check and problems else 0
    summary = baselines(inv, checker)
    if args.campaign:
        found = campaign_problems(root, inv, records, summary)
        for p in found:
            print(p)
        return 1 if args.check and found else 0
    text = render(records, ledger, owners, problems, summary, implemented_sections(root),
                  load_json(inv / AUTHORING, {}))
    target = inv / COVERAGE
    if args.check:
        current = target.read_text(encoding="utf-8") if target.is_file() else None
        if current != text:
            print(f"{INV_REL / COVERAGE} is stale: run python3 scripts/inventory/inventory-report.py and stage it")
            return 1
        return 0
    if args.index:
        print("inventory-report: --index judges the staged tree and writes nothing; add --check", file=sys.stderr)
        return 2
    target.write_text(text, encoding="utf-8")
    print(f"inventory-report: wrote {INV_REL / COVERAGE} ({len(records)} records, {len(problems)} ownership line(s))")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="report instead of writing; exit 1 on a finding")
    ap.add_argument("--owners", action="store_true", help="the ownership check")
    ap.add_argument("--campaign", action="store_true", help="reconcile the capture matrix against the evidence")
    ap.add_argument("--index", action="store_true", help="judge the staged tree")
    ap.add_argument("--root", default=str(REPO), help=argparse.SUPPRESS)
    args = ap.parse_args(argv)
    if args.owners and args.campaign:
        ap.error("--owners and --campaign are separate checks")
    try:
        if args.index:
            with staged_root(args.root) as tmp:
                staged = pathlib.Path(tmp) / "scripts" / "inventory" / "inventory-report.py"
                if not staged.is_file():
                    return 0        # the commit removes the report: nothing to judge
                rest = [f"--{k}" for k in ("check", "owners", "campaign") if getattr(args, k)]
                # The staged copy, run on the staged tree: REPO resolves to the snapshot.
                return subprocess.run([sys.executable, str(staged), "--root", tmp, *rest],
                                      timeout=GIT_TIMEOUT * 3, check=False).returncode
        return run(pathlib.Path(args.root), args)
    except (ReportError, json.JSONDecodeError, OSError) as exc:
        print(f"inventory-report: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
