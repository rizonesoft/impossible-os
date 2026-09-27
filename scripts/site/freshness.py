#!/usr/bin/env python3
"""Documentation freshness: does a page still describe the code it names?

A docs page declares the code it describes with `sources=` in its directive; a
landing-page feature card does the same in gh-pages/features.json. A page (or
card) is STALE when those sources differ between the commit that last changed
the page's CONTENT and the tree being checked. Content, not history: comparing
trees with `git diff` handles reverts (a source edited and reverted is not a
change), merges (no history simplification to get wrong) and files deleted from
a source directory, in one call per page.

States:
  fresh    -- sources unchanged since the page content last changed
  stale    -- sources changed; the page may no longer be accurate
  editing  -- the page itself is being changed in the tree being checked, which
              is a review of it (not during a merge: merged-in content is not a
              review, so a merge falls through to the normal comparison)
  new      -- the page has no committed history yet
  unknown  -- the clone is shallow, so the baseline cannot be found

Warnings only, never errors: a code change is not always a docs change. The fix
for a stale page is to update it; if it is still accurate, bump its `reviewed=`
date, which is a content change and therefore a new baseline.
"""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FEATURES = "gh-pages/features.json"


@dataclass
class Record:
    kind: str                     # "page" | "card"
    name: str                     # docs path, or card title
    sources: list[str]
    state: str = "fresh"
    baseline: str | None = None   # commit that last changed the content
    baseline_date: str | None = None
    changed: list[str] = field(default_factory=list)


def _git(*args: str, ok=(0,)) -> subprocess.CompletedProcess:
    # --literal-pathspecs: a tracked file named ":x" or "*.c" is that file, never
    # pathspec magic (`--` stops option parsing, not pathspec interpretation).
    r = subprocess.run(["git", "--literal-pathspecs", *args], cwd=REPO, capture_output=True, text=True)
    if r.returncode not in ok:
        raise RuntimeError(f"git {' '.join(args[:3])} failed: {r.stderr.strip()[:200]}")
    return r


def is_shallow() -> bool:
    return _git("rev-parse", "--is-shallow-repository").stdout.strip() == "true"


def merging() -> bool:
    gitdir = _git("rev-parse", "--git-dir").stdout.strip()
    return (REPO / gitdir / "MERGE_HEAD").exists()


def tracked(source: str) -> tuple[set[str], set[str]]:
    """Tracked files (and their directories) in `source`. Untracked files never
    count as sources: they have no history, and CI would not see them."""
    if source in ("worktree", "index"):
        raw = _git("ls-files", "-z").stdout
    else:
        raw = _git("ls-tree", "-r", "-z", "--name-only", source).stdout
    files = {p for p in raw.split("\0") if p}
    dirs: set[str] = set()
    for f in files:
        parts = f.split("/")
        for i in range(1, len(parts)):
            dirs.add("/".join(parts[:i]))
    return files, dirs


def _base_rev(source: str) -> str:
    return "HEAD" if source in ("worktree", "index") else source


def content_baseline(path: str, rev: str) -> tuple[str, str] | None:
    """(commit, path-at-that-commit) of the last commit on `rev`'s first-parent
    line that changed `path`'s CONTENT, following renames and skipping pure
    (100% similar) renames, which move a page without reviewing it.

    `--first-parent -m` makes a merge that changed the page (a conflict
    resolution, or content arriving from the other side) show as a change
    against the main line; plain `log` prints no diff for merges and would pick
    an older commit. `-z` keeps unusual file names raw instead of C-quoted."""
    out = _git("log", "--first-parent", "-m", "-M", "--follow", "-z", "--name-status",
               "--format=%x01%H", rev, "--", path).stdout
    for chunk in out.split("\x01"):
        fields = chunk.split("\0")
        if len(fields) < 3:
            continue
        commit, status, paths = fields[0], fields[1].strip(), [f for f in fields[2:] if f]
        if not paths or status == "R100":
            continue
        return commit, paths[-1]
    return None


def _blob_in(source: str, path: str) -> str | None:
    if source == "worktree":
        p = REPO / path
        return _git("hash-object", str(p)).stdout.strip() if p.is_file() else None
    spec = f":{path}" if source == "index" else f"{source}:{path}"
    r = _git("rev-parse", "--verify", "--quiet", spec, ok=(0, 1, 128))
    return r.stdout.strip() or None


def sources_changed(baseline: str, sources: list[str], source: str) -> list[str]:
    """Paths under `sources` that differ between `baseline` and `source`."""
    if source == "worktree":
        args = ["diff", "--name-only", baseline, "--"]
    elif source == "index":
        args = ["diff", "--name-only", "--cached", baseline, "--"]
    else:
        args = ["diff", "--name-only", baseline, source, "--"]
    return [l for l in _git(*args, *sources).stdout.splitlines() if l]


def uncommitted_rename(path: str, source: str) -> tuple[str, bool] | None:
    """(old path, pure?) when `path` is the new name of an UNCOMMITTED rename
    against HEAD. A pure rename (100% similar) keeps the old path's baseline; a
    rename with edits is an edit of the page."""
    if source not in ("worktree", "index"):
        return None
    args = ["diff", "-M", "-z", "--name-status"] + (["--cached"] if source == "index" else []) + ["HEAD"]
    fields = _git(*args).stdout.split("\0")
    i = 0
    while i < len(fields):
        status = fields[i]
        if status.startswith(("R", "C")) and i + 2 < len(fields):
            old, new = fields[i + 1], fields[i + 2]
            if new == path and status.startswith("R"):
                return old, status == "R100"
            i += 3
        else:
            i += 2
    if source == "worktree" and (REPO / path).is_file():
        # A plain `mv` leaves the destination UNTRACKED, which no diff against
        # HEAD reports; match its bytes against the tracked paths deleted since.
        blob = _blob_in("worktree", path)
        gone = _git("diff", "-z", "--name-only", "--diff-filter=D", "HEAD").stdout.split("\0")
        for old in (g for g in gone if g):
            if blob and _blob_in("HEAD", old) == blob:
                return old, True
    return None


def _date(commit: str) -> str:
    return _git("log", "-1", "--format=%cs", commit).stdout.strip()


def check_page(path: str, sources: list[str], source: str, shallow: bool, in_merge: bool) -> Record:
    rec = Record("page", path, sources)
    if shallow:
        rec.state = "unknown"
        return rec
    base = content_baseline(path, _base_rev(source))
    if base is None:
        moved = uncommitted_rename(path, source)
        if moved is None:
            rec.state = "new"
            return rec
        old, pure = moved
        if not pure and not in_merge:
            rec.state = "editing"
            return rec
        base = content_baseline(old, "HEAD")      # a pure move keeps the old page's age
        if base is None:
            rec.state = "new"
            return rec
        rec.baseline, rec.baseline_date = base[0], _date(base[0])
        rec.changed = sources_changed(rec.baseline, sources, source)
        rec.state = "stale" if rec.changed else "fresh"
        return rec
    rec.baseline, _then_path = base
    rec.baseline_date = _date(rec.baseline)
    # EDITING is an UNCOMMITTED change to the page (worktree or index against
    # HEAD). A committed revision is never "editing"; nor is content brought in
    # by a merge in progress, which nobody has reviewed.
    if source in ("worktree", "index") and not in_merge and _blob_in(source, path) != _blob_in("HEAD", path):
        rec.state = "editing"
        return rec
    rec.changed = sources_changed(rec.baseline, sources, source)
    rec.state = "stale" if rec.changed else "fresh"
    return rec


def _cards_at(rev: str | None) -> dict[str, dict]:
    """Cards keyed by title at `rev` (None = the working tree)."""
    try:
        if rev is None:
            text = (REPO / FEATURES).read_text(encoding="utf-8")
        else:
            r = _git("show", f"{rev}:{FEATURES}", ok=(0, 128))
            if r.returncode:
                return {}
            text = r.stdout
        cards = json.loads(text).get("cards", [])
    except (OSError, ValueError, AttributeError):
        return {}
    return {c.get("title"): _card_key(c) for c in cards}


def _card_key(card: dict) -> dict:
    # `reviewed` is part of the card: bumping it is how a still-accurate card is
    # re-reviewed, so it must move the card's baseline like any other edit.
    return {k: card.get(k) for k in ("title", "text", "owners", "sources", "reviewed")}


def check_cards(source: str, shallow: bool, in_merge: bool) -> list[Record]:
    """One record per feature card. Each card's baseline is the commit that last
    changed THAT card (its title, text, owners or sources), so editing one card
    does not mark the others as reviewed."""
    now = {"worktree": lambda: _cards_at(None), "index": _cards_from_index}.get(source, lambda: _cards_at(source))()
    history = [] if shallow else _git("log", "--first-parent", "--format=%H", _base_rev(source), "--",
                                      FEATURES).stdout.split()
    versions = [(c, _cards_at(c)) for c in history]            # newest first
    head_cards = versions[0][1] if versions else {}
    out = []
    for title, card in now.items():
        rec = Record("card", title, list(card.get("sources") or []))
        if shallow:
            rec.state = "unknown"
            out.append(rec)
            continue
        # Which version's age to measure. An uncommitted edit is a review. During a
        # merge the merged-in text is not, so the card is judged by the version
        # HEAD carries (a card only the other side has is new).
        uncommitted = source in ("worktree", "index") and head_cards.get(title) != card
        if uncommitted and not in_merge:
            rec.state = "editing" if title in head_cards else "new"
            out.append(rec)
            continue
        target = head_cards.get(title) if uncommitted else card
        if uncommitted and target is not None:
            # Merged-in sources are not reviewed either: check HEAD's list and any
            # the merge adds, so dropping a stale source cannot clear the warning.
            rec.sources = list(dict.fromkeys(list(target.get("sources") or []) + rec.sources))
        base = None
        for commit, cards in versions:                          # walk back while the card is unchanged
            if target is None or cards.get(title) != target:
                break
            base = commit
        if base is None:
            rec.state = "new"
            out.append(rec)
            continue
        rec.baseline, rec.baseline_date = base, _date(base)
        rec.changed = sources_changed(base, rec.sources, source) if rec.sources else []
        rec.state = "stale" if rec.changed else "fresh"
        out.append(rec)
    return out


def _cards_from_index() -> dict[str, dict]:
    r = _git("show", f":{FEATURES}", ok=(0, 128))
    if r.returncode:
        return {}
    try:
        cards = json.loads(r.stdout).get("cards", [])
    except ValueError:
        return {}
    return {c.get("title"): _card_key(c) for c in cards}


def check_all(pages: dict[str, list[str]], source: str) -> list[Record]:
    """`pages` maps a repo-relative docs path to its declared sources."""
    from concurrent.futures import ThreadPoolExecutor
    shallow, in_merge = is_shallow(), merging()
    with ThreadPoolExecutor(max_workers=8) as pool:   # git calls are I/O-bound subprocesses
        recs = list(pool.map(lambda kv: check_page(kv[0], kv[1], source, shallow, in_merge), sorted(pages.items())))
    return recs + check_cards(source, shallow, in_merge)


def warning(rec: Record) -> str | None:
    if rec.state != "stale":
        return None
    shown = ", ".join(rec.changed[:3]) + (f" and {len(rec.changed) - 3} more" if len(rec.changed) > 3 else "")
    what = rec.name if rec.kind == "page" else f"{FEATURES} card {rec.name!r}"
    return (f"{what}: its sources changed since it was last updated on {rec.baseline_date} ({shown}); "
            f"update it, or bump its reviewed= date if it is still accurate")
