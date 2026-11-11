#!/usr/bin/env python3
"""todo-plan: keep todo/implementation-plan.md in step with the TODO graph.

The plan lists every numbered section of every TODO file exactly once, in the
order it should be built: phases, and rows within a phase. Phase headings and
their paragraphs are written by hand. The rows are derived, never ticked by
hand: the box comes from the section's lifecycle class (the same classifier the
run obeys), the title from the section heading, Needs from the section's
`**Needs:**` line (`none` when it has no such line) and Items from a count of
its checklist items.

    python3 scripts/todo-plan.py --sync          rewrite boxes and titles, place new sections
    python3 scripts/todo-plan.py --check         exit 1 when the plan is stale or out of order
    python3 scripts/todo-plan.py --check --staged   the same, for what the git index holds
    python3 scripts/todo-plan.py --next [N]      the first N rows that can be built now
    python3 scripts/todo-plan.py --resolve REF   the file and heading a row names
    python3 scripts/todo-plan.py --cycles        open sections whose dependencies form a cycle
    python3 scripts/todo-plan.py --seed          write a first plan (refuses to overwrite one)

Boxes: [x] done (shipped, Verified and Quality reviewed, or parked by a
terminal deferral), [/] shipped or in progress and still owed work or review,
[ ] not started.

Order rule, enforced by --check: a section never sits before a section it
depends on while both are open (finished work constrains nothing). A
dependency cycle fails --check outright, because none of its members can ever
be built; --cycles lists the edges, and the fix is in the Depends On cells of
the Implementation Order tables.

Needs: what a section cannot finish without, from a closed list (NEEDS below).
It is a label and never holds a section back: the run builds every section,
proves what the emulators can prove, and parks only the one check that needs
the named thing. A value outside the list fails --check.

Exit: 0 ok, 1 the plan needs attention, 2 the graph could not be read.
"""

from __future__ import annotations

import argparse
import contextlib
import fcntl
import heapq
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PLAN_REL = "todo/implementation-plan.md"
SIGN = "§"

REF_PATTERN = r"D\d{2} T[0-9A-Za-z]+ " + SIGN + r"\d+"
# A row with or without the Needs and Items cells: a row pasted from an older
# plan still names its section. No cell holds a pipe (row_title replaces them).
ROW_RE = re.compile(r"^\|\s*\[(?P<box>[ x/])\]\s*\|\s*`(?P<ref>" + REF_PATTERN + r")`\s*\|(?P<title>[^|]*)\|"
                    r"(?:(?P<needs>[^|]*)\|(?P<items>[^|]*)\|)?\s*$")
PHASE_RE = re.compile(r"^### Phase (?P<n>\d+) -- (?P<title>\S.*?)\s*$")
PROGRESS_RE = re.compile(r"^> \*\*Progress:\*\* ")
FILE_RE = re.compile(r"^TODO-(?P<tok>[0-9A-Za-z]+)-")
TABLE_HEAD = ["| Done | Section | Deliverable | Needs | Items |", "| :-: | --- | --- | --- | --: |"]
HEAD_RE = re.compile(r"^\|\s*Done\s*\|\s*Section\s*\|\s*Deliverable\s*\|")
RULE_RE = re.compile(r"^\|(?:\s*:?-+:?\s*\|)+\s*$")
OLD_HEAD_RE = re.compile(r"^\|\s*Done\s*\|\s*Section\s*\|\s*Deliverable\s*\|\s*$")
OLD_RULE_RE = re.compile(r"^\|\s*:?-+:?\s*\|\s*:?-+:?\s*\|\s*:?-+:?\s*\|\s*$")
# What a section cannot finish without. A label, never a gate (see the module text).
NEEDS = ("bare-metal", "whpx-host", "display-session", "attended", "owner-decision", "third-party", "operator")
NEEDS_RE = re.compile(r"^\*\*Needs:\*\*[ \t]*(?P<value>.*?)\s*$")
# The lines git leaves in a file it could not merge. The rows are derived, so a
# conflicted plan is repaired by a sync: the markers go, a row both sides carry
# is kept once, and every cell is rewritten from the TODO files.
CONFLICT_RE = re.compile(r"^(?:<{7}|>{7}|\|{7})(?: .*)?$|^={7}$")
ITEM_RE = re.compile(r"^- \[[ x/]\] \S")
BUILD_TIMEOUT_S = 300
TITLE_MAX = 120
# What a heading carries beyond its name: an italic note such as
# `*(deferred -- ...)*`, and a trailing model tag such as `[Opus]` in backticks.
TITLE_NOTE_RE = re.compile(r"\s*\*\([^*]*\)\*")
TITLE_TAG_RE = re.compile(r"(?:\s*`\[[^`\]]*\]`)+\s*$")


class PlanError(Exception):
    """The graph or the plan could not be read; no verdict was reached."""


def row_title(raw: str) -> str:
    """A section heading as one short table cell: its name, without appended notes."""
    t = " ".join(raw.replace("|", "/").split())
    t = TITLE_TAG_RE.sub("", TITLE_NOTE_RE.sub("", t)).strip()
    if len(t) > TITLE_MAX:
        t = t[:TITLE_MAX - 3].rsplit(" ", 1)[0].rstrip(" ,;:(-") + "..."
        if t.count("`") % 2:  # the cut landed inside a code span
            t = t[:-3].rsplit("`", 1)[0].rstrip(" ,;:(-") + "..."
    return t or "(untitled)"


class Section:
    __slots__ = ("ref", "path", "n", "title", "box", "deps", "wait", "unmet", "blocked", "domain", "sort", "label", "selfdep", "dead",
                 "needs", "items", "needs_fault", "needs_seen")

    def __init__(self, ref, path, n, title, box, domain, sort):
        self.ref, self.path, self.n, self.title, self.box = ref, path, n, title, box
        self.domain, self.sort = domain, sort
        self.deps = []      # refs this section depends on (resolved, in the corpus)
        self.wait = []      # the deps that constrain ORDER: both ends still open
        self.unmet = False  # an unsatisfied dependency of any kind, resolved or not
        self.blocked = False  # parked on a recoverable deferral: not buildable now
        self.label = None   # the section a leading "§N" in the Deliverable names, if any
        self.selfdep = False  # its own Depends On cell names it: never buildable
        self.dead = []      # dependency tokens no work can satisfy (no such section, not a TODO)
        self.needs = []     # the values of its **Needs:** line, in the order written
        self.items = 0      # its checklist items
        self.needs_seen = False
        self.needs_fault = None  # why its **Needs:** line cannot be read, if it cannot


# --- the graph ---------------------------------------------------------------

def load_sections(root: Path, nodes=None) -> dict:
    """ref -> Section for every numbered section, from a privately built cache.

    The cache is rebuilt into a temporary directory on every run: the shared
    build/todo-cache.json goes stale on any TODO edit and is deleted by several
    hooks, and a plan computed from a stale graph would be confidently wrong.
    A caller that already holds nodes certified fresh against the corpus (the
    triage oracle re-reads its cache under the strict freshness check) passes
    them instead, and no rebuild is paid. Never pass nodes loaded without that
    check: a stale graph would report unfinished prerequisites as met.
    """
    graph_dir = REPO / "scripts" / "todo-graph"
    if str(graph_dir) not in sys.path:
        sys.path.insert(0, str(graph_dir))
    try:
        import cache_schema as cs
        import query as q
    except ImportError as exc:
        raise PlanError(f"the TODO graph library could not be imported: {exc}") from None
    if nodes is not None:
        return _sections_from(nodes, root, q)
    with tempfile.TemporaryDirectory(prefix="todo-plan-") as tmp:
        cache = Path(tmp) / "todo-cache.json"
        try:
            built = subprocess.run(
                [sys.executable, str(graph_dir / "build.py"), "--quiet", "--root", str(root / "todo"),
                 "--repo-root", str(root), "--output", str(cache)],
                capture_output=True, text=True, timeout=BUILD_TIMEOUT_S, check=False)
        except subprocess.TimeoutExpired:
            raise PlanError(f"the graph build did not finish in {BUILD_TIMEOUT_S}s") from None
        if built.returncode != 0:
            tail = (built.stderr or built.stdout).strip().splitlines()[-5:]
            raise PlanError("the graph build failed:\n  " + "\n  ".join(tail))
        try:
            nodes, _ = cs.load_and_validate(cache, root / "todo", check_stale=False, profile=cs.PROFILE_QUERY)
        except cs.CacheSchemaError as exc:
            raise PlanError(f"the graph cache is not usable: {exc}") from None
    return _sections_from(nodes, root, q)


def _sections_from(nodes, root: Path, q) -> dict:
    ctx = q.Ctx(nodes, root, True)
    try:
        deps = q.section_dep_index(ctx)
    except RuntimeError as exc:
        raise PlanError(str(exc)) from None

    sections, by_key, by_file = {}, {}, {}
    for node in sorted(nodes, key=lambda nd: nd["file_path"]):
        path = node["file_path"]
        rows = node.get("sections") or []
        if not rows:
            continue
        parts = Path(path).parts
        m_dom = re.match(r"^(\d{2})-", parts[1]) if len(parts) == 3 else None
        m_tok = FILE_RE.match(parts[-1])
        if not m_dom or not m_tok:
            raise PlanError(f"{path} has sections but no todo/NN-domain/TODO-XX-name.md address")
        domain, tok = int(m_dom.group(1)), m_tok.group("tok")
        headings = {h["n"]: h["title"] for h in node.get("section_headings") or []}
        for i, sec in enumerate(rows):
            n = sec.get("n")
            if n is None:
                raise PlanError(f"{path} has an Implementation Order row with no section number")
            ref = f"D{domain:02d} T{tok} {SIGN}{n}"
            if ref in sections:
                raise PlanError(f"{ref} names two sections ({sections[ref].path} and {path})")
            key = (path, n)
            cls = deps.section_class.get(key)
            if cls == deps.DONE:
                box = "x"
            elif sec.get("status") in ("x", "/") or cls == "BLOCKED":
                box = "/"
            else:
                box = " "
            title = row_title(headings.get(n) or sec.get("deliverable") or "")
            s = Section(ref, path, n, title, box, domain, (domain, tok.zfill(4), n, i))
            m_lab = LABEL_RE.match(sec.get("deliverable") or "")
            s.label = int(m_lab.group(1)) if m_lab else None
            s.blocked = cls == "BLOCKED"
            sections[ref] = s
            by_key[key] = s
            by_file.setdefault(path, []).append(s)

    for key, s in by_key.items():
        seen = set()
        for edge in (deps.selfdeps.get(key) or []) + (deps.cross.get(key) or []):
            if not edge.get("satisfied"):
                s.unmet = True
                if edge.get("reason") in DEAD_REASONS:
                    s.dead.append(edge.get("token") or "?")
            target = edge.get("target_path") or (s.path if edge in (deps.selfdeps.get(key) or []) else None)
            if target is None:
                continue
            m = edge.get("target_section")
            if target == s.path and m in (s.n, None):
                s.selfdep = True  # a bare own-file reference includes the section itself
            targets = by_file.get(target, []) if m is None else [by_key[(target, m)]] if (target, m) in by_key else []
            for t in targets:
                if t.ref != s.ref and t.ref not in seen:
                    seen.add(t.ref)
                    s.deps.append(t.ref)
    # Finished work constrains nothing: a done section keeps its place, and a
    # dependency that is done is no reason to sit later. Many done sections
    # still name prerequisites that were never formally closed.
    for s in sections.values():
        if s.box != "x":
            s.wait = [d for d in s.deps if sections[d].box != "x"]
    return sections


def read_bodies(sections: dict, root: Path) -> None:
    """Fill each section's Needs and Items from its body in the TODO file.

    The fence scan and the heading rule are the graph builder's own, so a
    `**Needs:**` line or a checklist item inside a code sample is not read."""
    if str(REPO / "scripts") not in sys.path:
        sys.path.insert(0, str(REPO / "scripts"))
    import todo_fence
    by_file = {}
    for s in sections.values():
        by_file.setdefault(s.path, {})[s.n] = s
    for path, by_n in by_file.items():
        try:
            scan = todo_fence.scan_text((root / path).read_text(encoding="utf-8"))
        except OSError as exc:
            raise PlanError(f"{path} could not be read: {exc}") from None
        cur = None
        for line, masked in zip(scan.lines, scan.mask):
            if masked:
                continue
            if todo_fence.is_h2(line):
                head = todo_fence.classify_heading(line)
                cur = by_n.get(head.n) if head.kind == "ok" else None
            elif cur is None:
                continue
            elif ITEM_RE.match(line):
                cur.items += 1
            else:
                m = NEEDS_RE.match(line)
                if m:
                    _read_needs(cur, m.group("value"))


def _read_needs(s: Section, value: str) -> None:
    if s.needs_seen:
        s.needs, s.needs_fault = [], "has more than one **Needs:** line"
        return
    s.needs_seen = True
    names = [v.strip() for v in value.split(",")]
    if names == ["none"]:
        return
    bad = [v or "(empty)" for v in names if v not in NEEDS]
    if bad:
        s.needs_fault = f"names {', '.join(bad)}"
    elif len(set(names)) != len(names):
        s.needs_fault = "names a value twice"
    else:
        s.needs = names


def needs_problems(sections: dict) -> list:
    """A **Needs:** line the plan cannot show: a value outside the list, a repeat, or a second line."""
    return [f"{s.ref} ({s.path}) {s.needs_fault} in **Needs:**; write `none` or a comma-separated list from: "
            + ", ".join(NEEDS)
            for s in sorted(sections.values(), key=lambda s: s.sort) if s.needs_fault]


def buildable(s: Section) -> bool:
    return s.box != "x" and not s.unmet and not s.blocked


def ready_rows(root: Path, nodes=None) -> list:
    """(file, section) for every row that can be built now, in plan order: the
    answer `--next 0` prints. Raises PlanError when there is no plan or graph."""
    sections = load_sections(root, nodes)
    path = root / PLAN_REL
    if not path.exists():
        raise PlanError(f"{PLAN_REL} does not exist; run --seed once")
    plan = Plan(path.read_text(encoding="utf-8"))
    return [(sections[r].path, sections[r].n) for r in plan.order() if r in sections and buildable(sections[r])]


def cyclic_refs(sections: dict) -> dict:
    """ref -> cycle id, for every ref on a dependency cycle (a multi-node SCC)."""
    index, low, on, stack, out, counter = {}, {}, set(), [], {}, [0]
    for start in sections:
        if start in index:
            continue
        work = [(start, iter(sections[start].wait))]
        index[start] = low[start] = counter[0]
        counter[0] += 1
        stack.append(start)
        on.add(start)
        while work:
            v, it = work[-1]
            pushed = False
            for w in it:
                if w not in index:
                    index[w] = low[w] = counter[0]
                    counter[0] += 1
                    stack.append(w)
                    on.add(w)
                    work.append((w, iter(sections[w].wait)))
                    pushed = True
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            if pushed:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                if len(comp) > 1:
                    out.update(dict.fromkeys(comp, index[v]))
    return out


def natural_phases(sections: dict) -> dict:
    """ref -> phase: its domain, or later when a dependency sits later."""
    phase = {r: s.domain for r, s in sections.items()}
    changed = True
    while changed:
        changed = False
        for r, s in sections.items():
            want = max([phase[r]] + [phase[d] for d in s.wait])
            if want != phase[r]:
                phase[r] = want
                changed = True
    return phase


def ordered(refs: list, sections: dict, cyc: dict) -> list:
    """Dependency order within one phase; file order breaks ties and cycles."""
    inside = set(refs)
    waiting = {r: {d for d in sections[r].wait if d in inside} for r in refs}
    users = {r: [] for r in refs}
    for r, ds in waiting.items():
        for d in ds:
            users[d].append(r)
    heap = [(sections[r].sort, r) for r in refs if not waiting[r]]
    heapq.heapify(heap)
    left = {r for r in refs if waiting[r]}
    out = []
    while heap or left:
        if not heap:  # stuck on a cycle: release its first member in file order
            # Prefer a cycle that waits on nothing outside itself, so an edge
            # from one cycle into another is still honoured.
            stuck = [x for x in left if x in cyc]
            free = [x for x in stuck if all(cyc.get(w) == cyc[x] for w in waiting[x])]
            r = min(free or stuck, key=lambda x: sections[x].sort)
            left.discard(r)
            waiting[r] = set()
            heapq.heappush(heap, (sections[r].sort, r))
        _, r = heapq.heappop(heap)
        out.append(r)
        for u in users[r]:
            if u in left:
                waiting[u].discard(r)
                if not waiting[u]:
                    left.discard(u)
                    heapq.heappush(heap, (sections[u].sort, u))
    return out


# --- the plan file -----------------------------------------------------------

class Plan:
    """The plan as lines, with each row's position and phase."""

    def __init__(self, text: str):
        self.lines = text.split("\n")
        self.rows = {}     # ref -> line index (first occurrence)
        self.dupes = []
        self.phase_of = {}  # ref -> phase number
        self.phases = {}   # phase number -> (heading line index, last row line index or None)
        current = None
        for i, line in enumerate(self.lines):
            mp = PHASE_RE.match(line)
            if mp:
                current = int(mp.group("n"))
                if current in self.phases:
                    raise PlanError(f"{PLAN_REL}: Phase {current} appears twice")
                self.phases[current] = [i, None]
                continue
            mr = ROW_RE.match(line)
            if not mr:
                continue
            if current is None:
                raise PlanError(f"{PLAN_REL}:{i + 1}: a section row sits before the first phase heading")
            ref = mr.group("ref")
            if ref in self.rows:
                self.dupes.append(ref)
                continue
            self.rows[ref] = i
            self.phase_of[ref] = current
            self.phases[current][1] = i

    def order(self) -> list:
        return sorted(self.rows, key=self.rows.get)


def row_line(s: Section) -> str:
    return f"| [{s.box}] | `{s.ref}` | {s.title} | {', '.join(s.needs) or 'none'} | {s.items} |"


def _row_cells(s: Section) -> tuple:
    return (s.box, s.title, ", ".join(s.needs) or "none", str(s.items))


def progress_line(sections: dict) -> str:
    total = len(sections)
    done = sum(1 for s in sections.values() if s.box == "x")
    part = sum(1 for s in sections.values() if s.box == "/")
    pct = (done * 100 // total) if total else 0
    return (f"> **Progress:** **{done} of {total} sections complete ({pct}%).** {part} more are shipped or in "
            f"progress and still owed work or review. Derived by `python3 scripts/todo-plan.py --sync`, never edited by hand.")


def aligned(text: str) -> str:
    """The text with its tables aligned by the repository's own formatter."""
    with tempfile.TemporaryDirectory(prefix="todo-plan-") as tmp:
        probe = Path(tmp) / "plan.md"
        probe.write_text(text, encoding="utf-8")
        done = subprocess.run([sys.executable, str(REPO / "scripts" / "format-md-tables.py"), str(probe)],
                              capture_output=True, text=True, timeout=BUILD_TIMEOUT_S, check=False)
        if done.returncode != 0:
            raise PlanError("the table formatter failed: " + (done.stderr or done.stdout).strip()[-300:])
        return probe.read_text(encoding="utf-8")


def write_atomic(path: Path, text: str, expect) -> None:
    """Replace the plan, unless it changed since `expect` was read (None: it must not exist).

    The temporary file is this process's own, so two syncs at once cannot write
    through each other; the loser of the race is refused and reruns.
    """
    fd, name = tempfile.mkstemp(prefix="." + path.name + ".", suffix=".tmp", dir=path.parent)
    tmp = Path(name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        now = path.read_text(encoding="utf-8") if path.exists() else None
        if now != expect:
            raise PlanError(f"{PLAN_REL} changed while it was being synced; nothing was written, run the sync again")
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def unconflicted(text: str) -> tuple:
    """A plan git could not merge, without its conflict markers, and whether it held any.

    Both sides' lines are in the file, so the copies a plan may hold only once
    go here, before the plan is parsed: a second progress line, a phase heading
    already seen, and a table header or rule in the middle of a table. A row
    both sides carry is dropped by the sync as a duplicate, and a line the
    second side repeats word for word is dropped here. Hand-written prose the
    two sides wrote differently stays, both copies, for a person to settle.
    """
    lines = text.split("\n")
    if not any(CONFLICT_RE.match(line) for line in lines):
        return text, False
    out, phases, progress, prev = [], set(), False, ""
    side, ours = "", set()  # which part of a conflict the line is in, and the lines its first side held
    for line in lines:
        if CONFLICT_RE.match(line):
            side = {"<": "ours", "|": "base", "=": "theirs", ">": ""}[line[0]]
            if side in ("ours", ""):
                ours = set()
            continue
        if side == "base":
            continue  # the common ancestor, shown by diff3-style conflicts
        kind = ("head" if HEAD_RE.match(line) else "rule" if RULE_RE.match(line) else
                "row" if ROW_RE.match(line) else "")
        mp = PHASE_RE.match(line)
        if side == "ours":
            ours.add(line)
        if ((kind == "head" and prev) or (kind == "rule" and prev in ("rule", "row"))
                or (PROGRESS_RE.match(line) and progress) or (mp and mp.group("n") in phases)
                or (side == "theirs" and kind != "row" and line in ours)):
            continue
        if mp:
            phases.add(mp.group("n"))
        progress = progress or bool(PROGRESS_RE.match(line))
        if line.strip():
            prev = kind
        out.append(line)
    return "\n".join(out), True


def synced_text(text: str, sections: dict) -> tuple:
    """The plan text brought up to date, and what changed (as messages)."""
    text, repaired = unconflicted(text)
    plan = Plan(text)
    notes = ["removed merge conflict markers"] if repaired else []
    lines = plan.lines
    drop = set()
    for i, line in enumerate(lines):
        mr = ROW_RE.match(line)
        if not mr:
            if PROGRESS_RE.match(line):
                lines[i] = progress_line(sections)
            elif OLD_HEAD_RE.match(line) and i + 1 < len(lines) and OLD_RULE_RE.match(lines[i + 1]):
                lines[i], lines[i + 1] = TABLE_HEAD  # a table from before the Needs and Items columns
            continue
        ref = mr.group("ref")
        if ref not in sections:
            drop.add(i)
            notes.append(f"removed {ref}: no such section")
        elif plan.rows[ref] != i:
            drop.add(i)
            notes.append(f"removed a duplicate row for {ref}")
        elif tuple((mr.group(k) or "").strip() for k in ("box", "title", "needs", "items")) != _row_cells(sections[ref]):
            # Compared on meaning, so alignment padding is left as it was.
            lines[i] = row_line(sections[ref])
    missing = sorted((r for r in sections if r not in plan.rows), key=lambda r: sections[r].sort)
    inserts = {}  # line index -> rows to add after it
    if missing:
        if not plan.phases:
            raise PlanError(f"{PLAN_REL} has no phase to place {len(missing)} new section(s) in; run --seed")
        phase_of = dict(plan.phase_of)
        last_row = {p: v[1] for p, v in plan.phases.items()}
        file_last = {}
        for ref, i in plan.rows.items():
            k = (sections[ref].path, plan.phase_of[ref]) if ref in sections else None
            if k and i > file_last.get(k, -1):
                file_last[k] = i
        known = sorted(plan.phases)
        for ref in ordered(missing, sections, cyclic_refs(sections)):
            s = sections[ref]
            home = [phase_of[r] for r in phase_of if r in sections and sections[r].path == s.path]
            want = max([max(home) if home else s.domain] + [phase_of[d] for d in s.wait if d in phase_of])
            target = next((p for p in known if p >= want), known[-1])
            anchor = file_last.get((s.path, target))
            if anchor is None:
                anchor = last_row[target]
            if anchor is None:  # an empty phase: start its table under the heading paragraph
                anchor = plan.phases[target][0]
                nxt = min([v[0] for p, v in plan.phases.items() if v[0] > anchor] + [len(lines)])
                while anchor + 1 < nxt and not lines[anchor + 1].startswith("#"):
                    anchor += 1
                while anchor > plan.phases[target][0] and not lines[anchor].strip():
                    anchor -= 1
                inserts.setdefault(anchor, []).extend(["", *TABLE_HEAD])
            inserts.setdefault(anchor, []).append(row_line(s))
            phase_of[ref] = target
            notes.append(f"placed {ref} in Phase {target}")
    out = []
    for i, line in enumerate(lines):
        if i not in drop:
            out.append(line)
        out.extend(inserts.get(i, []))
    return "\n".join(out), notes


def order_problems(plan: Plan, sections: dict) -> list:
    cyc = cyclic_refs(sections)
    out = []
    for ref, i in plan.rows.items():
        s = sections.get(ref)
        if s is None:
            continue
        for d in s.wait:
            # Only an edge inside ONE cycle is unorderable; an edge between two
            # cycles, or into one, can be respected and is held.
            if d in plan.rows and plan.rows[d] > i and not (ref in cyc and cyc.get(d) == cyc[ref]):
                out.append(f"{ref} (Phase {plan.phase_of[ref]}) sits before its dependency {d} "
                           f"(Phase {plan.phase_of[d]})")
    return out


LABEL_RE = re.compile(SIGN + r"\s?(\d+)\b")


def label_problems(sections: dict) -> list:
    """A row whose Deliverable opens with another section's "§N".

    The graph takes a row's section number from its Section column, else from
    Order. A table that keeps Order as a build position and writes the real
    number into the Deliverable is read with every section shifted, so its
    Depends On cells resolve to the wrong sections. Ten files had that shape
    until 2026-11-11, and three of the plan's cycles were nothing else. The
    label can be the wrong half too (IXFS advanced, the same day), so the
    message names both headings and the reader decides which the row is."""
    heading = {(s.path, s.n): s.title for s in sections.values()}
    return [f"{s.path} Order {s.n} is labelled {SIGN}{s.label}: the graph reads it as {SIGN}{s.n} "
            f"(\"{heading.get((s.path, s.n), '?')}\"), not {SIGN}{s.label} (\"{heading.get((s.path, s.label), '?')}\"). "
            f"If the row describes {SIGN}{s.label}, add a Section column; if not, the label is wrong and goes"
            for s in sorted(sections.values(), key=lambda s: s.sort) if s.label is not None and s.label != s.n]


# Edge verdicts that no amount of work changes: the target section does not
# exist, the target is a file the graph does not hold (a domain INDEX or a
# reference table), or the resolver could not place the token at all.
DEAD_REASONS = ("dangling", "non-node", "unresolved")


def dead_problems(sections: dict) -> list:
    """An open section with a dependency no work can satisfy.

    The run starts a section only when its dependencies are done, so such an
    edge parks it until nothing else is left."""
    return [f"{s.ref} depends on {', '.join(sorted(set(s.dead)))}, which names no section that work can finish: "
            "point the Depends On cell at the owning section, or write it as prose"
            for s in sorted(sections.values(), key=lambda s: s.sort) if s.dead and s.box != "x"]


def self_problems(sections: dict) -> list:
    """An open section whose Depends On cell names the section itself.

    The edge can never be satisfied, so the section is never buildable, and the
    cycle check cannot see it (a self-edge is not kept in the graph). It is the
    usual trace of a cell written as a build position: a cell naming position
    three, left on the row that is now section three."""
    return [f"{s.ref} depends on itself: its Depends On cell names {SIGN}{s.n} "
            "(a build position written where a section number belongs?)"
            for s in sorted(sections.values(), key=lambda s: s.sort) if s.selfdep and s.box != "x"]


def cycle_problems(sections: dict) -> list:
    """One problem per dependency cycle: its members can never be built, so --check fails."""
    groups = {}
    for ref, cid in cyclic_refs(sections).items():
        groups.setdefault(cid, []).append(ref)
    return [f"dependency cycle: {', '.join(sorted(refs, key=lambda r: sections[r].sort))} "
            "(correct their Depends On cells; python3 scripts/todo-plan.py --cycles lists the edges)"
            for refs in sorted(groups.values(), key=lambda g: min(sections[r].sort for r in g))]


def cycle_report(sections: dict) -> list:
    """One line per open section on a dependency cycle, with the edges that close it."""
    cyc = cyclic_refs(sections)
    return [f"{r}\t{sections[r].path}\twaits on: " + ", ".join(d for d in sections[r].wait if cyc.get(d) == cyc[r])
            for r in sorted(cyc, key=lambda r: sections[r].sort)]


def seed_text(sections: dict, root: Path) -> str:
    phase = natural_phases(sections)
    cyc = cyclic_refs(sections)
    names = {}
    for d in sorted((root / "todo").iterdir()):
        m = re.match(r"^(\d{2})-(.+)$", d.name)
        if d.is_dir() and m:
            names[int(m.group(1))] = m.group(2).replace("-", " ").capitalize()
    head = [
        "# Impossible OS -- Implementation Plan",
        "",
        "The order to build every roadmap section in, from today to the release.",
        "",
        progress_line(sections),
        "",
        "## The phases",
    ]
    for p in sorted(set(phase.values())):
        refs = ordered([r for r in sections if phase[r] == p], sections, cyc)
        head += ["", f"### Phase {p} -- {names.get(p, 'Phase ' + str(p))}", "",
                 f"Seeded from the `{p:02d}` domain and from the sections of earlier domains that depend on it.",
                 "", *TABLE_HEAD]
        head += [row_line(sections[r]) for r in refs]
    return "\n".join(head) + "\n"


# --- commands ----------------------------------------------------------------

class WriterLock:
    """One sync at a time, from reading the plan to replacing it.

    Without it two syncs can both find the plan unchanged and replace each
    other's result. The lock file lives under build/, which git ignores. A
    hand edit made while a sync runs does not take the lock; write_atomic's
    comparison refuses that case.
    """

    def __init__(self, root: Path):
        self.path = root / "build" / ".todo-plan.lock"
        self.fh = None

    def __enter__(self):
        wait = float(os.environ.get("TODO_PLAN_LOCK_WAIT_S", "120"))
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.fh = open(self.path, "w")  # noqa: SIM115 -- held for the lifetime of the lock
        deadline = time.monotonic() + wait
        while True:
            try:
                fcntl.flock(self.fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.monotonic() >= deadline:
                    self.fh.close()
                    raise PlanError(f"another sync holds {self.path} (waited {wait:g}s); run the sync again") from None
                time.sleep(0.1)

    def __exit__(self, *exc):
        fcntl.flock(self.fh, fcntl.LOCK_UN)
        self.fh.close()


def staged_tree(root: Path):
    """todo/ as the git index holds it, written to a temporary directory.

    A commit carries the index, not the working tree: a plan synced but not
    staged, or a TODO edit that is not part of the commit, must not decide
    whether the commit's own plan is current.
    """
    tmp = tempfile.TemporaryDirectory(prefix="todo-plan-index-")
    try:
        names = subprocess.run(["git", "-C", str(root), "ls-files", "-z", "--", "todo"],
                               capture_output=True, timeout=BUILD_TIMEOUT_S, check=True).stdout
        subprocess.run(["git", "-C", str(root), "checkout-index", "-z", "--stdin", "--prefix=" + tmp.name + "/"],
                       input=names, capture_output=True, timeout=BUILD_TIMEOUT_S, check=True)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired, OSError) as exc:
        tmp.cleanup()
        raise PlanError(f"the git index could not be read: {exc}") from None
    return tmp


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sync", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--next", nargs="?", type=int, const=10, metavar="N")
    mode.add_argument("--resolve", metavar="REF")
    mode.add_argument("--cycles", action="store_true")
    mode.add_argument("--seed", action="store_true")
    ap.add_argument("--json", action="store_true", help="JSON output for --next and --resolve")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--staged", action="store_true",
                    help="with --check: judge the plan and TODO files in the git index, which is what a commit carries")
    ap.add_argument("--repo-root", type=Path, default=REPO)
    args = ap.parse_args(argv)
    root = args.repo_root.resolve()
    staged_dir = None
    if args.staged:
        if not args.check:
            ap.error("--staged goes with --check")
        try:
            staged_dir = staged_tree(root)
        except PlanError as exc:
            print(f"todo-plan: {exc}", file=sys.stderr)
            return 2
        root = Path(staged_dir.name)
        # The export is not a repository. A hook in a linked worktree exports GIT_DIR, and the graph build
        # would then read the real repository's history from the export's directory and refuse the build as
        # "the corpus git history moved" (2026-11-11). The index has been read; the build gets a plain shell.
        for name in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR", "GIT_PREFIX"):
            os.environ.pop(name, None)
    path = root / PLAN_REL
    lock = WriterLock(root) if (args.sync or args.seed) else contextlib.nullcontext()

    def say(msg):
        if not args.quiet:
            print(msg)

    try:
        lock.__enter__()
    except PlanError as exc:
        if staged_dir is not None:
            staged_dir.cleanup()
        print(f"todo-plan: {exc}", file=sys.stderr)
        return 2
    try:
        sections = load_sections(root)
        read_bodies(sections, root)
        if args.cycles:
            for line in cycle_report(sections):
                print(line)
            return 0
        if args.seed:
            if path.exists():
                raise PlanError(f"{PLAN_REL} exists; --seed never overwrites a plan (use --sync)")
            write_atomic(path, aligned(seed_text(sections, root)), None)
            say(f"todo-plan: wrote {PLAN_REL} with {len(sections)} sections")
            return 0
        if not path.exists():
            if args.staged:
                return 0  # the commit carries no plan: nothing to hold it to
            raise PlanError(f"{PLAN_REL} does not exist; run --seed once")
        text = path.read_text(encoding="utf-8")

        if args.sync or args.check:
            new, notes = synced_text(text, sections)
            new = aligned(new)
            problems = needs_problems(sections) + label_problems(sections) + self_problems(sections) + dead_problems(sections) + cycle_problems(sections) + order_problems(Plan(new), sections)
            if args.sync:
                if new != text:
                    write_atomic(path, new, text)
                for n in notes:
                    say(f"todo-plan: {n}")
                say(f"todo-plan: {PLAN_REL} {'updated' if new != text else 'already current'}")
            elif new != text:
                where = " as staged (sync, then git add it)" if args.staged else ""
                problems.insert(0, f"{PLAN_REL}{where} is stale ({len(notes)} row(s) to place or remove, or a box, a "
                                   "title, a Needs or Items cell or the progress line differs): python3 scripts/todo-plan.py --sync")
            for p in problems:
                print(f"todo-plan: {p}", file=sys.stderr)
            return 1 if problems else 0

        plan = Plan(text)
        if args.resolve is not None:
            m = re.search(REF_PATTERN, " ".join(args.resolve.replace("`", " ").split()))
            s = sections.get(m.group(0)) if m else None
            if s is None:
                print(f"todo-plan: no section matches {args.resolve!r}", file=sys.stderr)
                return 1
            rec = {"ref": s.ref, "file": s.path, "section": s.n, "title": s.title, "box": s.box,
                   "phase": plan.phase_of.get(s.ref), "depends_on": s.deps, "ready": buildable(s),
                   "needs": s.needs, "items": s.items}
            print(json.dumps(rec, indent=1) if args.json else f"{s.path}\t{s.n}\t{s.title}")
            return 0

        ready = [sections[r] for r in plan.order() if r in sections and buildable(sections[r])]
        ready = ready[: args.next] if args.next else ready
        if args.json:
            print(json.dumps([{"ref": s.ref, "file": s.path, "section": s.n, "title": s.title, "box": s.box,
                               "phase": plan.phase_of[s.ref], "needs": s.needs, "items": s.items}
                              for s in ready], indent=1))
        else:
            for s in ready:
                print(f"{row_line(s)}\tPhase {plan.phase_of[s.ref]}\t{s.path}")
        return 0
    except PlanError as exc:
        print(f"todo-plan: {exc}", file=sys.stderr)
        return 2
    finally:
        lock.__exit__(None, None, None)
        if staged_dir is not None:
            staged_dir.cleanup()


if __name__ == "__main__":
    sys.exit(main())
