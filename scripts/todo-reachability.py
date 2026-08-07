#!/usr/bin/env python3
"""Can a future pass still SEE this item? One question, one detector.

WHY THIS EXISTS RATHER THAN A FOURTH PARTIAL CHECK. Three checks already guard
pieces of this, each correct and each blind to the others:

  * `todo-orphan-check.py`  -- `- [ ]` in a section stamped BOTH Verified and
                               Quality-reviewed, exempting Deferred sections
  * `validate.py`           -- dangling sections / orphan IO rows
  * `sequencer_triage.py`   -- the classifier those checks are policing

Between them, three shapes of invisible work were found on 2026-08-02, all in
one morning, all real:

  1. A `## N.` body with NO Implementation Order row. PROVEN against the real
     completed `00-infrastructure/TODO-05`: adding a section body with an open
     item left the file classified DONE and the work unreachable; the SAME
     section with an IO row flipped it to NEEDS_WORK. The oracle classifies
     from the row and never reads the body.
  2. A bare `- [ ]` in a stamped, DONE section -- the shape orphan-check owns.
  3. Open `- [ ]` items inside DEFERRED-stamped sections. orphan-check exempts
     these BY DESIGN ("recorded blockers with a named owner"), and the exemption
     is right -- but 109 such items exist, and an item parked without naming an
     owner is indistinguishable from one that names one. It is work in the
     wrong SHAPE: the doctrine's repair is `- [/]` with the blocker named.

The question a reader actually has is not "which of three rules did this break"
but "will anyone ever come back to this?". That is what this answers.

REACHABLE means: some future pass will re-read this item. Concretely, the item
lives in a section the triage oracle does not classify DONE, OR in a Deferred
section whose parked item names an owner the owner-side sweep can act on.

Usage:
  todo-reachability.py [--json] [paths...]    exit 1 if unreachable work exists
  todo-reachability.py --selftest
Stdlib only; reads the todo-graph cache when present for the oracle's verdict.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_REPO_ROOT / "scripts" / "todo-graph"))
try:
    import cache_schema as _cs
except ImportError as _exc:                                  # pragma: no cover
    sys.stderr.write(f"todo-reachability: cannot import the shared "
                     f"cache-schema validator: {_exc}\n")
    raise

# EXIT CODES. 0 = clean, 1 = FINDINGS, 2 = infrastructure.
#
# `2` is NEW with the shared-cache-validator routing, and exists because 1 was
# already taken by a VERDICT. Before this, a cache that would not parse was
# swallowed by a bare `except Exception` and the audit silently fell back to
# per-file parsing, so an unusable cache produced a clean-looking finding count
# computed from a different source than the one this tool documents. Mapping
# the failure onto 1 would have been no better: an infrastructure refusal would
# then be indistinguishable from "there are unreachable items", which is the
# exact defect the shared validator exists to prevent (Codex design review of
# the cache-validation-reach work, [high]).
#
# A MISSING cache is deliberately NOT an error. Absence is not corruption: a
# fresh clone has no `build/todo-cache.json` until something builds it, and this
# audit is expected to work there via its per-file fallback. Every OTHER reason
# -- unreadable, wrong shape, empty, stale -- means a cache EXISTS and cannot be
# trusted, and silently preferring the fallback would hide that.
EXIT_INFRA = 2
_CACHE_FALLBACK_REASONS = frozenset((_cs.REASON_MISSING,))


class CacheUnusable(RuntimeError):
    """A cache exists but cannot be trusted. Carries the shared reason tag so
    the message names WHICH rule refused, and is caught in `main` and mapped to
    `EXIT_INFRA` -- never to the findings code."""

    def __init__(self, reason: str, message: str):
        super().__init__(message)
        self.reason = reason

SECTION_RE = re.compile(r"^## (\d+)\.")
OPEN_ITEM_RE = re.compile(r"^\s*- \[ \]")
PARKED_ITEM_RE = re.compile(r"^\s*- \[/\]")
# AUTHOR-TIME marker for a recurring/standing task -- work that is
# deliberately never "done" (an annual re-review, a periodic audit).
# Such an item is CORRECTLY a bare `- [ ]` and must not be parked, but
# the gate below would otherwise refuse fixpoint on it forever.
# NOTE this is not the inferred `parked-ownerless` rule that was removed
# (see the note at the end of audit()): the distinction is that a human
# WRITES this marker when authoring the item, rather than a detector
# guessing intent from shape. Unmarked items still flag -- fail-closed.
STANDING_RE = re.compile(r"\bstanding:", re.I)
VERIFIED_RE = re.compile(r"^> \*\*Verified:")
DEFERRED_RE = re.compile(r"^> \*\*Deferred:")
QUALITY_RE = re.compile(r"^> \*\*Quality reviewed:")
AWAITING_RE = re.compile(r"awaiting-[a-z]+")
IO_ROW_RE = re.compile(r"^\|[^|]*\|\s*(\d+)\s*\|")

# An owner is anything a later pass can act on: an XREF, a named TODO, an
# explicit awaiting-* token, or a file path. Deliberately generous -- the point
# is to separate "parked WITH a handle" from "parked into nothing", not to
# grade prose.
OWNER_RE = re.compile(
    r"XREF|awaiting-[a-z]+|TODO-\d+|todo/[0-9]{2}-[a-z-]+/|`[a-z_]+\.(c|h|py|sh)`",
    re.IGNORECASE)


ANY_H2_RE = re.compile(r"^## ")


def _sections(lines):
    """(number, line, body) with the body ending at the next `## ` of ANY kind.

    Ending only at the next NUMBERED section is wrong and was caught on
    2026-08-02 before it caused a bad edit: the LAST numbered section's body
    then runs to EOF, swallowing `## OS Comparison`, `## Unit Tests`,
    `## Verification` and `## History` -- all of which legitimately contain
    `- [ ]` items. That made every file's final section look like it held
    unreachable open work, and produced 11 confident findings that evaporated
    on inspection (each section actually had 0 open items of its own).
    """
    starts = [(i, int(m.group(1))) for i, l in enumerate(lines)
              if (m := SECTION_RE.match(l))]
    for idx, (ln, num) in enumerate(starts):
        end = len(lines)
        for j in range(ln + 1, len(lines)):
            if ANY_H2_RE.match(lines[j]):
                end = j
                break
        yield num, ln, lines[ln:end]


def _io_rows(lines):
    """Section numbers listed in the `## Implementation Order` table.

    SCOPED to that table, and tolerant of BOTH row shapes found in the corpus
    (2026-08-02) -- the number sits in column 2 behind a status glyph in most
    files (`| 💎 | 1 | Deliverable | ... |`) and in column 1 in others
    (`| 1 | Section | Tag | Dep |`). Scanning every table in the file instead
    produced 304 false "no IO row" findings, because an Inputs table's rows
    never carry a section number.
    """
    rows = set()
    inside = False
    for l in lines:
        if l.startswith("## "):
            inside = l[3:].strip().lower().startswith("implementation order")
            continue
        if not inside or not l.startswith("|"):
            continue
        cells = [c.strip() for c in l.strip().strip("|").split("|")]
        for cell in cells[:2]:
            if cell.isdigit():
                rows.add(int(cell))
                break
    return rows


_CACHE = {}
# The generation fingerprint taken when the cache was loaded, held so `main`
# can re-verify the corpus AFTER the audit walk. Empty when no cache was
# loaded (missing-cache fallback), in which case there is nothing to bind.
_CACHE_CORPUS = {}


def _load_cache(root):
    """Section status straight from the todo-graph cache: {path: {n: status}}.

    Spawning `sequencer_triage --classify` per file cost 232 subprocesses and
    made this unusable from lint (it silently timed out on 2026-08-02 and the
    check emitted nothing at all). The cache already carries what is needed --
    `sections[].status` is the Implementation Order marker the classifier reads
    -- so one JSON load replaces the whole fan-out.
    """
    if _CACHE:
        return _CACHE
    cache_path = Path(root) / "build" / "todo-cache.json"
    # ROUTED THROUGH THE SHARED CACHE-SCHEMA VALIDATOR, under the profile that
    # declares what this reader actually consumes: `sections[].n/.status`. The
    # default stamped-items profile would REFUSE a cache in which nothing has
    # shipped yet, which is a perfectly usable cache for a reachability audit.
    try:
        data, info = _cs.load_and_validate(
            cache_path, Path(root) / "todo", profile=_cs.PROFILE_SECTIONS)
        # RETAINED FOR POST-WALK RE-VERIFICATION, not decoration. The loader's
        # freshness scan bounds only the milliseconds inside itself; this tool
        # then reads every TODO body and stamp in the corpus, which is the
        # window that actually matters. A TODO edited during that walk would
        # pair OLD cached section statuses with NEW body items and either
        # suppress or fabricate a finding, at a normal exit code. `main` calls
        # `check_corpus_unchanged` with this fingerprint before publishing.
        _CACHE_CORPUS["corpus"] = info.corpus
        _CACHE_CORPUS["todo_root"] = Path(root) / "todo"
    except _cs.CacheSchemaError as exc:
        if exc.reason in _CACHE_FALLBACK_REASONS:
            _CACHE["__missing__"] = True
            return _CACHE
        # NOT a silent degrade. See EXIT_INFRA above: the cache exists and
        # cannot be trusted, so the audit refuses rather than quietly answering
        # from a different source.
        raise CacheUnusable(exc.reason, str(exc)) from exc
    for node in data:
        # `file_path` and the `sections[]` shape are now guaranteed by the
        # validator, so the defensive `if not fp` / `isinstance(sec, dict)`
        # skips this walk used to need are gone: skipping is exactly the silent
        # narrowing the routing exists to end.
        _CACHE[node["file_path"]] = {
            sec["n"]: (sec["status"] or "").strip()
            for sec in node["sections"]
        }
    return _CACHE


def _io_status(path, root):
    """{section: IO-table status} from the cache, or None when unavailable."""
    cache = _load_cache(root)
    if cache.get("__missing__"):
        return None
    return cache.get(str(path))


def _is_done(status, verified, quality, deferred, awaiting):
    """Mirror of sequencer_triage's DONE rule -- deliberately, not approximately.

    From its own docstring: DONE is a shipped `[x]`/`[/]` carrying BOTH
    Verified AND Quality-reviewed, OR an `[x]`/`[/]` carrying a TERMINAL
    Deferred stamp. A Deferred stamp bearing an `awaiting-*` token is BLOCKED,
    not DONE -- the run stays armed and fixpoint refuses -- so those sections
    ARE revisited and their items are reachable.

    Approximating this cost a wrong answer once already: keying on `[x]` alone
    reported 23 findings where the real rule gives a different set, and keying
    on the Verified stamp alone (the pre-cache fallback) gave 350.
    """
    if status not in ("x", "/"):
        return False
    if deferred:
        return not awaiting          # awaiting-* => BLOCKED => still revisited
    return verified and quality


def audit(path, root="."):
    """[(kind, section, detail)] for unreachable work in one file."""
    try:
        lines = Path(path).read_text(encoding="utf-8").split("\n")
    except OSError:
        return []
    rows = _io_rows(lines)
    status_map = _io_status(path, root)
    out = []
    for num, ln, body in _sections(lines):
        opens = [b.strip()[:90] for b in body if OPEN_ITEM_RE.match(b)]
        parked = [b.strip() for b in body if PARKED_ITEM_RE.match(b)]
        deferred = any(DEFERRED_RE.match(b) for b in body)
        stamped = any(VERIFIED_RE.match(b) for b in body)
        quality = any(QUALITY_RE.match(b) for b in body)
        awaiting = any(DEFERRED_RE.match(b) and AWAITING_RE.search(b) for b in body)

        # 1. body with no Implementation Order row -- invisible to the oracle
        if num not in rows and (opens or parked):
            out.append(("no-io-row", num,
                        f"section {num} has {len(opens)+len(parked)} item(s) but no "
                        f"Implementation Order row; the oracle classifies from the "
                        f"row and never reads the body"))
            continue

        status = (status_map or {}).get(num, "")
        is_done = (_is_done(status, stamped, quality, deferred, awaiting)
                   if status_map is not None else (stamped and quality))

        # 2. open items in a DONE section that is not a recorded deferral
        if is_done and opens and not deferred:
            out.append(("open-in-done", num,
                        f"{len(opens)} open item(s) in a DONE section: {opens[0]}"))

        # 3. open items parked in a Deferred section -- wrong shape.
        # A `standing:`-marked item is EXCLUDED: it is recurring work that is
        # correctly a bare `- [ ]` and must never be converted to a park. Without
        # this the doctrine and the gate contradicted each other -- the sequencer
        # skill instructs the run to LEAVE such an item alone, and this detector
        # then refused fixpoint on it, so a correctly-shaped corpus could never
        # complete. Found by the run itself, 2026-08-05, on an annual
        # trigger-review item.
        standing = [b for b in body
                    if OPEN_ITEM_RE.match(b) and STANDING_RE.search(b)]
        real_opens = [b for b in opens
                      if not STANDING_RE.search(b)]
        if is_done and real_opens and deferred:
            out.append(("open-in-deferred", num,
                        f"{len(real_opens)} open `- [ ]` item(s) inside a Deferred "
                        f"section (should be `- [/]` naming the blocker): "
                        f"{real_opens[0]}"))

        # NO "parked-ownerless" RULE. It was tried on 2026-08-02 and REMOVED:
        # `[/]` means IN PROGRESS in this repo, not "parked awaiting an owner"
        # (implement-todo-section: "`[x]` fully done or `[/]` in progress"), so
        # requiring every `[/]` to name a blocker misread 329 ordinary
        # progress markers -- e.g. `- [/] GetEnvironmentVariableA(...)` -- as
        # defects. A detector that cries wolf 329 times is worse than none.
    return out


def _targets(argv):
    paths = [a for a in argv if not a.startswith("-")]
    if paths:
        return paths
    return sorted(str(p) for p in Path("todo").rglob("TODO-*.md"))


def main(argv) -> int:
    as_json = "--json" in argv
    findings = {}
    try:
        # PRIME THE CACHE BEFORE ANY TODO BODY IS READ. `audit()` reads its
        # file and only THEN calls `_io_status`, which is what initialises the
        # fingerprint -- so on the first target the body was read BEFORE the
        # generation was pinned. A concurrent rebuild landing in that gap paired
        # old body lines with a new cache AND a new fingerprint, so the
        # post-walk check saw an unchanged corpus and published a
        # mixed-generation verdict: precisely the race this binding exists to
        # close (Codex adversarial round 2, [medium]).
        _load_cache(".")
        for path in _targets(argv):
            hits = audit(path)
            if hits:
                findings[path] = hits
        # CLOSE THE GENERATION WINDOW BEFORE PUBLISHING. Verdicts derived from
        # a corpus that moved under the walk are not verdicts.
        if _CACHE_CORPUS.get("corpus") is not None:
            _cs.check_corpus_unchanged(_CACHE_CORPUS["todo_root"],
                                       _CACHE_CORPUS["corpus"])
    except _cs.CacheSchemaError as exc:
        # `check_corpus_unchanged` raises the shared error directly. Same
        # destination as a load failure: infrastructure, never a verdict.
        sys.stderr.write(f"todo-reachability: corpus moved during the audit "
                         f"[{exc.reason}]: {exc}\n")
        return EXIT_INFRA
    except CacheUnusable as exc:
        # EXIT_INFRA, never 1. A caller distinguishing "unreachable items exist"
        # from "the audit could not run" depends on this separation.
        sys.stderr.write(f"todo-reachability: cache unusable "
                         f"[{exc.reason}]: {exc}\n")
        return EXIT_INFRA
    if as_json:
        print(json.dumps(findings, indent=2))
    else:
        total = sum(len(v) for v in findings.values())
        by_kind = {}
        for hits in findings.values():
            for kind, _, _ in hits:
                by_kind[kind] = by_kind.get(kind, 0) + 1
        for path, hits in sorted(findings.items()):
            print(f"\n{path}")
            for kind, num, detail in hits:
                print(f"  [{kind}] {detail}")
        print(f"\n{total} unreachable finding(s) across {len(findings)} file(s)")
        for k, v in sorted(by_kind.items(), key=lambda kv: -kv[1]):
            print(f"  {v:4d}  {k}")
    return 1 if findings else 0


def _selftest() -> int:
    import tempfile
    ok = True
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "TODO-99-x.md"
        p.write_text(
            "# X\n\n## Implementation Order\n\n| 💎 | 1 | a | -- | [x] |\n\n"
            "## 1. Stamped and done\n\n- [x] done\n- [ ] sneaked in\n"
            "> **Verified:** 2026-01-01 | commit `x`\n"
            # BOTH stamps are required for DONE -- a shipped-but-unreviewed
            # section is NEEDS_WORK, so a fixture with Verified alone is
            # correctly NOT flagged (this fixture originally omitted the
            # Quality-reviewed line and the selftest failed for the right
            # reason).
            "> **Quality reviewed:** 2026-01-01 | Codex\n\n"
            "## 2. No IO row at all\n\n- [ ] invisible work\n")
        hits = {k for k, _, _ in audit(str(p), root=td)}
        for want in ("open-in-done", "no-io-row"):
            if want not in hits:
                print(f"FAIL: {want} not detected ({hits})"); ok = False
    print("todo-reachability selftest", "OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(_selftest() if "--selftest" in sys.argv else main(sys.argv[1:]))
