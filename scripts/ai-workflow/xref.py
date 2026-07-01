#!/usr/bin/env python3
"""Canonical Accepted/Deferred XREF grammar -- ONE definition shared by every
consumer so the three-way drift (git hook / todo-graph / stamp writer) is gone.

Tiers (kept identical to the historical `.claude/hooks/accepted_xref_block.py`
so existing stamps are NOT retroactively re-classified into a BLOCK):

  bare      a clause citing a TODO with NO parenthetical            -> hooks BLOCK
  soft      a parenthetical present but no concrete marker          -> hooks WARN
  concrete  a concrete marker present (item:/at line/retrofit/helper;) -> OK

`canonical()` is the STRICT shape the stamp writer emits for NEW stamps:
`NN-domain/TODO-XX §N (item: "..." at line N)`.

Consumers:
  - `.claude/hooks/accepted_xref_block.py` (git hook)  -> bare_clauses()/soft_clauses() tiers
  - `scripts/ai-workflow/common.py` (stamp writer)     -> writer_bare_xrefs()/writer_has_concrete()

Deliberately NOT a consumer: `scripts/todo-graph/build.py`. Its `XREF_CLAUSE_RE`
serves a DIFFERENT purpose -- extracting dependency-graph EDGES, which requires the
canonical `-> XREF: <path> §N` shape and drops §-less clauses. `parse()` here is the
looser tier-classification match (section optional). The two are complementary, not
drifted: the WRITER (via writer_* above) emits the canonical XREFs that todo-graph's
edge parser consumes, so a stamp that passes the writer is one todo-graph can turn
into an edge. Routing todo-graph through parse() would change the graph (add §-less
edges, alter item capture) and is intentionally avoided. `parse()`/`canonical()` are
retained for callers that want structured access to a tier clause.
"""
from __future__ import annotations

import bisect
import re
from dataclasses import dataclass

# Clause SPEC: the historical git-hook regex. A clause runs from `XREF:` up to
# (but not including) the next `,`, `;`, or `]`, must cite a TODO before that
# terminator, and may span a later `XREF:` marker when no terminator intervenes
# (the lazy span coalesces them). NOT used for matching -- its lazy span rescans
# the tail per failed `XREF:` start, going quadratic on ownerless-storm lines in
# the per-commit hook. _clause_spans() below is the linear equivalent; the
# regression suite diffs the two on the tricky shapes.
_CLAUSE_RE = re.compile(r"XREF:\s*[^,;\]]*?TODO-\d+[^,;\]]*")

_XREF_MARK_RE = re.compile(r"XREF:")
_TODO_TOKEN_RE = re.compile(r"TODO-\d+")
_TERMINATOR_RE = re.compile(r"[,;\]]")


def _clause_spans(text: str) -> list[tuple[int, int]]:
    """Linear-time clause extraction, output-identical to _CLAUSE_RE.findall.

    Precomputes every `XREF:` marker, terminator, and TODO-token position once,
    then resolves each marker with two bisects: the clause segment ends at the
    first terminator after the marker; the marker matches iff a TODO token
    starts inside that segment; scanning resumes after a matched clause (marks
    inside it are consumed, reproducing the regex's non-overlap + coalescing).
    """
    spans: list[tuple[int, int]] = []
    marks = [m.start() for m in _XREF_MARK_RE.finditer(text)]
    if not marks:
        return spans
    terms = [m.start() for m in _TERMINATOR_RE.finditer(text)]
    todos = [m.start() for m in _TODO_TOKEN_RE.finditer(text)]
    pos = 0
    for s in marks:
        if s < pos:
            continue
        ti = bisect.bisect_right(terms, s)
        seg_end = terms[ti] if ti < len(terms) else len(text)
        di = bisect.bisect_right(todos, s)
        if di < len(todos) and todos[di] < seg_end:
            spans.append((s, seg_end))
            pos = seg_end
    return spans

# Concrete markers: identical to the git hook's has_concrete (marker ANYWHERE in the
# clause). Kept as-is so no committed stamp flips OK->BLOCK on rollout.
_CONCRETE_MARKERS = ("item:", "at line", "retrofit", "helper;")

# Extractors for todo-graph edges / writer canonical check.
_TARGET_RE = re.compile(r"(?P<path>(?:\d{2}-[a-z0-9-]+/)?TODO-\d+)")
_SECTION_RE = re.compile(r"§\s*(?P<sec>\d+)")
_ITEM_RE = re.compile(r'\b(?:item|new\s+item):\s*"(?P<item>[^"]+)"', re.IGNORECASE)
_AT_LINE_RE = re.compile(r"at line\s+\d+", re.IGNORECASE)

BARE = "bare"
SOFT = "soft"
CONCRETE = "concrete"


@dataclass(frozen=True)
class XrefClause:
    raw: str
    target_path: str          # "01-boot-platform/TODO-13" or "TODO-13"
    domain_qualified: bool     # path carries the NN-domain/ prefix
    section: str | None        # "7" (digits only), else None
    item_name: str | None      # quoted item name inside the parenthetical, else None
    tier: str                  # BARE / SOFT / CONCRETE


def _has_concrete(clause: str) -> bool:
    low = clause.lower()
    return any(m in low for m in _CONCRETE_MARKERS)


def classify(clause: str) -> str:
    """Tier of a single XREF clause -- identical semantics to the git hook."""
    if "(" not in clause:
        return BARE
    return CONCRETE if _has_concrete(clause) else SOFT


def parse(summary: str) -> list[XrefClause]:
    """Every TODO-citing `XREF:` clause in a stamp summary, in order.

    A clause that carries the `XREF:` marker but names no TODO target is NOT
    returned by _CLAUSE_RE (it requires TODO-\\d+); callers that must reject an
    ownerless `XREF: no-owner` should use has_ownerless_xref().
    """
    out: list[XrefClause] = []
    if not summary or "XREF:" not in summary:
        return out
    for lo, hi in _clause_spans(summary):
        clause = summary[lo:hi]
        tm = _TARGET_RE.search(clause)
        path = tm.group("path") if tm else ""
        sm = _SECTION_RE.search(clause)
        im = _ITEM_RE.search(clause)
        out.append(XrefClause(
            raw=clause.strip(),
            target_path=path,
            domain_qualified=bool(path) and "/" in path,
            section=sm.group("sec") if sm else None,
            item_name=im.group("item") if im else None,
            tier=classify(clause),
        ))
    return out


def bare_clauses(summary: str) -> list[str]:
    """Clauses that BLOCK (bare -- no parenthetical). Empty means none."""
    return [c.raw[:100] for c in parse(summary) if c.tier == BARE]


def soft_clauses(summary: str) -> list[str]:
    """Clauses that WARN (parenthetical present but no concrete marker)."""
    return [c.raw[:100] for c in parse(summary) if c.tier == SOFT]


def has_ownerless_xref(summary: str) -> bool:
    """True when the summary carries an `XREF:` marker that names NO TODO target
    (e.g. `XREF: no-owner`). The writer rejects these; the loose clause regex
    silently skips them, so this is a separate, explicit check.
    """
    if not summary or "XREF:" not in summary:
        return False
    for chunk in re.split(r"XREF:", summary)[1:]:
        # bound the ownerless probe at the next XREF / clause terminator
        head = re.split(r"[,;\]]|XREF:", chunk, 1)[0]
        if not _TARGET_RE.search(head):
            return True
    return False


def is_concrete(summary: str) -> bool:
    """True iff the summary has >=1 concrete TODO clause and NO bare/ownerless one.
    This is the stamp WRITER's minimum bar for a new accepted/deferred stamp.
    """
    clauses = parse(summary)
    if not clauses:
        return False
    if has_ownerless_xref(summary):
        return False
    if any(c.tier == BARE for c in clauses):
        return False
    return any(c.tier == CONCRETE for c in clauses)


# ---------------------------------------------------------------------------
# Writer-side predicates (stricter than the gate tiers on purpose). The gate must
# stay lenient so existing stamps survive; the WRITER refuses to emit a non-owning
# stamp in the first place. Both live here so the two strictness levels cannot drift
# across files. These reproduce common.py's historical bare_xrefs/has_concrete rules:
#   - marker must sit INSIDE a parenthetical (RA-H1: a marker in loose prose parks
#     work with no greppable owner -- todo-graph only reads owners from the paren);
#   - markers are item:/retrofit/helper; (RA2-M1: standalone "at line" dropped; the
#     canonical (item: "..." at line N) form still qualifies via item:);
#   - clauses split on the case-sensitive `XREF:` marker every sibling parser uses.
_WRITER_MARKERS = ("item:", "retrofit", "helper;")
_PAREN_RE = re.compile(r"\(([^()]*)\)")


def _writer_clause_head(clause: str) -> str:
    """The clause text the writer validates: quoted spans blanked FIRST (a quoted
    item name may contain parens or clause terminators -- `item: "Enforce lease
    OWNERSHIP (not presence) ..."`), then bounded at the first `,`/`;`/`]`. The
    bound mirrors the git hook's clause regex, so a parenthetical sitting past a
    terminator (which the hook would drop, classifying the clause bare and
    BLOCKING the commit) can never satisfy the writer."""
    return re.split(r"[,;\]]", re.sub(r'"[^"]*"', '""', clause), 1)[0]


def _writer_clause_ok(clause: str) -> bool:
    """True when the clause head names a TODO owner AND carries a concrete
    parenthetical marker inside that same head (writer-strict subset of the
    git hook: everything the writer accepts, the hook also accepts)."""
    head = _writer_clause_head(clause)
    if not re.search(r"TODO-\d+", head):
        return False
    for inner in _PAREN_RE.findall(head):
        low = inner.lower()
        if any(m in low for m in _WRITER_MARKERS):
            return True
    return False


def _writer_clauses(summary: str) -> list[str]:
    if "XREF:" not in (summary or ""):
        return []
    return summary.split("XREF:")[1:]


def writer_bare_xrefs(summary: str) -> list[str]:
    """Non-owning XREF clauses the WRITER must refuse (each clause validated
    independently so a bare clause is not masked by a later concrete one).

    A chunk whose clause head names NO TODO target is ownerless and equally
    refused -- skipping it would let an ownerless clause ride on a concrete
    sibling's writer_has_concrete pass."""
    bad: list[str] = []
    for clause in _writer_clauses(summary):
        if not _writer_clause_ok(clause):
            bad.append(("XREF:" + clause).strip()[:100])
    return bad


def writer_has_concrete(summary: str) -> bool:
    """True when the summary carries >=1 concrete TODO XREF clause (writer bar)."""
    return any(_writer_clause_ok(c) for c in _writer_clauses(summary))


def canonical(clause: str) -> bool:
    """Strict canonical form for a single clause the WRITER should emit:
    a domain-qualified TODO path + `§N` + a concrete `(item: "..." at line N)`.
    """
    tm = _TARGET_RE.search(clause)
    if not tm or "/" not in tm.group("path"):
        return False
    if not _SECTION_RE.search(clause):
        return False
    # the item marker must sit inside a parenthetical alongside an `at line N`
    for inner in re.findall(r"\(([^()]*)\)", clause):
        if _ITEM_RE.search(inner) and _AT_LINE_RE.search(inner):
            return True
    return False
