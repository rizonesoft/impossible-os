#!/usr/bin/env python3
"""Indexed decision registry -- settled questions get an ID, not re-research.

Deterministically extracts approved decisions from their three authoritative
homes and builds a searchable index, so agents and the main session cite a
decision ID instead of re-deriving (or worse, re-litigating) it:

  1. todo/answers.md               operator answers (`A: (operator, DATE)`)
                                   and proposed defaults (`A: (proposed ...)`)
  2. Accepted/Deferred stamp XREFs from build/todo-cache.json (ownership
                                   decisions: who owns what, what was parked)
  3. CLAUDE.md pinned decisions    `##`/`###` sections whose text contains a
                                   pin marker (pinned / permanently / policy /
                                   deliberate divergence / never / MUST)

  decision-registry.py build [PROJECT]         rebuild the index
  decision-registry.py search <terms...>       keyword search (AND), top 10
  decision-registry.py get <id>                full record

Index: .claude/state/decision-registry.jsonl. IDs are content hashes --
stable while the decision text is unchanged, new ID when it is amended
(an agent citing a dead ID is told to re-check the source).
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

INDEX_REL = ".claude/state/decision-registry.jsonl"
PIN_RE = re.compile(r"(?i)\b(pinned|permanently|policy|deliberate divergence"
                    r"|never|must not|hard rule)\b")


def _mk(kind: str, source: str, title: str, body: str) -> dict:
    body = re.sub(r"\s+", " ", body).strip()[:900]
    did = hashlib.sha256(f"{kind}\0{title}\0{body}".encode()).hexdigest()[:12]
    return {"id": did, "kind": kind, "source": source,
            "title": title.strip()[:160], "body": body}


def extract_answers(root: Path) -> list:
    p = root / "todo/answers.md"
    out = []
    try:
        text = p.read_text(encoding="utf-8")
    except OSError:
        return out
    # Q/A blocks: a `Q:` line followed by an `A: (...)` line.
    q = None
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith(("Q:", "**Q:")):
            q = s.lstrip("*").lstrip("Q:").strip()
        elif s.startswith(("A:", "**A:")) and q:
            out.append(_mk("operator-answer", "todo/answers.md", q,
                           s.lstrip("*").lstrip("A:").strip()))
            q = None
    return out


def extract_stamp_xrefs(root: Path) -> list:
    """Stamp XREFs from the todo-graph cache, read THROUGH the shared validator.

    WHY NOT A BARE `json.loads` (v13 carry, fixed 2026-08-10). This reader used
    to open `build/todo-cache.json` directly -- no `CACHE_FORMAT_VERSION` check,
    no digest-bound sidecar, no profile. Section 33 bumped that version 2 -> 3
    precisely because `stamps_xrefs[].target_path` KEPT ITS TYPE and CHANGED ITS
    MEANING, which is the one shape a defensive reader cannot detect: every key
    is present and every type is right, so the old-contract artifact parses
    cleanly and publishes decision records built on a stale meaning.

    This was the SECOND finding against this same reader in two versions -- v10
    records it reading `target`/`target_file`/`text`/`raw`, key names the cache
    never had, degrading silently to empty output. Same root shape both times:
    it consumed the cache without the contract that describes it.

    `cache_schema.load_and_validate` exists for exactly this and is what section
    17 of the metadata TODO shipped; adopting it here is that adoption, not a
    competing one-off. It fails CLOSED on LEGACY_FORMAT, so a stale-contract
    artifact yields NO decision records rather than wrong ones.
    """
    out = []
    tg = root / "scripts/todo-graph"
    if str(tg) not in sys.path:
        sys.path.insert(0, str(tg))
    try:
        import cache_schema as _cs
    except ImportError:
        # No validator reachable -> publish nothing. Falling back to a bare
        # parse here would reinstate exactly the hole this closes.
        return out
    try:
        entries, _info = _cs.load_and_validate(
            root / "build/todo-cache.json", root / "todo",
            check_stale=False, profile=_cs.PROFILE_STAMP_XREFS)
    except _cs.CacheSchemaError:
        # LEGACY_FORMAT, UNREADABLE, SHAPE -- every one means "this artifact
        # does not describe the contract this reader was written against".
        return out
    except (OSError, ValueError):
        return out
    for e in entries:
        for x in e.get("stamps_xrefs") or []:
            if not isinstance(x, dict):
                continue
            kind = (x.get("kind") or "").lower()
            if kind not in ("accepted", "deferred"):
                continue
            # READ THE KEYS THE PRODUCER EMITS. This asked for `target` /
            # `target_file` / `text` / `raw`, none of which the cache has ever
            # contained -- the real fields are `target_path`, `target_section`,
            # `item_name`, `severity`. So every stamp-xref record carried a
            # title ending in "-> " with nothing after it and a body that fell
            # through to a raw json.dumps. Invisible because the reader is
            # defensive at every step (`or ""`, `or json.dumps(x)`), so wrong
            # key names degrade to empty output instead of raising.
            tgt = x.get("target_path") or ""
            sec = x.get("target_section")
            if tgt and sec:
                tgt = f"{tgt} section {sec}"
            _item = x.get("item_name") or ""
            _sev = x.get("severity") or ""
            body = " | ".join(q for q in (
                f"severity: {_sev}" if _sev else "",
                f"item: {_item}" if _item else "",
                f"target: {tgt}" if tgt else "",
            ) if q) or json.dumps(x)
            out.append(_mk(f"stamp-{kind}", e.get("file_path", ""),
                           f"{kind}: {e.get('file_path', '')} -> {tgt}", body))
    return out


def extract_claude_md(root: Path) -> list:
    out = []
    try:
        text = (root / "CLAUDE.md").read_text(encoding="utf-8")
    except OSError:
        return out
    sections = re.split(r"^(#{2,3} .+)$", text, flags=re.M)
    for i in range(1, len(sections) - 1, 2):
        title = sections[i].lstrip("# ").strip()
        body = sections[i + 1]
        if PIN_RE.search(body[:1500]):
            out.append(_mk("claude-md-policy", "CLAUDE.md", title, body[:1200]))
    return out


def build(root: Path) -> int:
    records = (extract_answers(root) + extract_stamp_xrefs(root)
               + extract_claude_md(root))
    # de-dup by id
    seen, uniq = set(), []
    for r in records:
        if r["id"] not in seen:
            seen.add(r["id"])
            uniq.append(r)
    p = root / INDEX_REL
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(json.dumps(r) for r in uniq) + "\n",
                 encoding="utf-8")
    print(json.dumps({"built": len(uniq),
                      "by_kind": {k: sum(1 for r in uniq if r['kind'] == k)
                                  for k in {r['kind'] for r in uniq}}}))
    return 0


def load(root: Path) -> list:
    try:
        return [json.loads(ln) for ln in
                (root / INDEX_REL).read_text(encoding="utf-8").splitlines()
                if ln.strip()]
    except (OSError, ValueError):
        return []


def _auto_refresh_if_stale(root: Path) -> None:
    """I4: rebuild the index when build/todo-cache.json has advanced past it, so
    search/get never serve decisions staler than the current TODO state. The
    registry was previously refreshed only by an explicit `build`, so it drifted
    behind the TODO cache and a review re-triaged already-settled findings."""
    try:
        idx = root / INDEX_REL
        cache = root / "build" / "todo-cache.json"
        if not cache.exists():
            return
        if (not idx.exists()) or cache.stat().st_mtime > idx.stat().st_mtime:
            build(root)
    except Exception:
        pass


def main(argv) -> int:
    root = Path(".").resolve()
    if not argv or argv[0] == "build":
        if len(argv) > 1:
            root = Path(argv[1]).resolve()
        return build(root)
    _auto_refresh_if_stale(root)
    if argv[0] == "search":
        terms = [t.lower() for t in argv[1:]]
        if not terms:
            print("search needs terms", file=sys.stderr)
            return 2
        hits = [r for r in load(root)
                if all(t in (r["title"] + " " + r["body"]).lower()
                       for t in terms)]
        for r in hits[:10]:
            print(json.dumps({"id": r["id"], "kind": r["kind"],
                              "title": r["title"],
                              "body": r["body"][:200]}))
        if len(hits) > 10:
            print(json.dumps({"more_available": len(hits) - 10}))
        return 0 if hits else 1
    if argv[0] == "get" and len(argv) > 1:
        for r in load(root):
            if r["id"] == argv[1]:
                print(json.dumps(r, indent=1))
                return 0
        print(json.dumps({"error": "no such decision id (index stale? "
                                   "rebuild with `build`; amended decisions "
                                   "get NEW ids)"}))
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
