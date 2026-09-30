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
  unknown  -- the baseline cannot be found: the clone is shallow, or git
              failed (then `error` says why and a warning is printed)

Warnings only, never errors: a code change is not always a docs change. The fix
for a stale page is to update it; if it is still accurate, bump its `reviewed=`
date, which is a content change and therefore a new baseline.
"""
from __future__ import annotations

import json
import subprocess
import threading
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
    error: str = ""               # why the state is "unknown" when it is not a shallow clone


GIT_TIMEOUT = 300   # every call is local and sub-second; a hung git fails the check instead of holding it


def _git(*args: str, ok=(0,)) -> subprocess.CompletedProcess:
    # --literal-pathspecs: a tracked file named ":x" or "*.c" is that file, never
    # pathspec magic (`--` stops option parsing, not pathspec interpretation).
    try:
        r = subprocess.run(["git", "--literal-pathspecs", *args], cwd=REPO, capture_output=True, text=True,
                           timeout=GIT_TIMEOUT)
    except subprocess.TimeoutExpired:
        # A RuntimeError, like every other git failure here, so the page or card reads
        # "unknown" (a warning) instead of the timeout aborting the whole build or lint.
        raise RuntimeError(f"git {' '.join(args[:3])} did not finish within {GIT_TIMEOUT} s") from None
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
    # BOUNDED: ask for one record first; almost every page's latest record IS
    # its baseline. Widen only when every record returned was a pure rename.
    # A git failure raises (via _git) rather than reading as "no history".
    for n in (1, 16, 0):
        args = ["log", "--first-parent", "-m", "-M", "--follow", "-z", "--name-status", "--format=%x01%H%x02%cs"]
        out = _git(*(args + ([f"-n{n}"] if n else []) + [rev, "--", path])).stdout
        records = [r for r in out.split("\x01") if r]
        for rec in records:
            found = _baseline_record(rec)
            if found:
                return found
        if not n or len(records) < n:
            return None
    return None


def _baseline_record(raw: str) -> tuple[str, str, str] | None:
    """(commit, path, date) if this log record changed the page's content."""
    fields = raw.split("\0")
    if len(fields) < 3 or "\x02" not in fields[0]:
        return None
    commit, date = fields[0].split("\x02", 1)
    status, paths = fields[1].strip(), [f for f in fields[2:] if f]
    if not paths or status == "R100":
        return None
    return commit, paths[-1], date


def _blob_in(source: str, path: str) -> str | None:
    if source == "worktree":
        p = REPO / path
        # fail-direction: an absent path has no blob: it differs from HEAD (editing) and matches no deleted file
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
    try:
        return _check_page(path, sources, source, shallow, in_merge)
    except RuntimeError as e:          # git could not answer: unknown, and said so, never "new"
        return Record("page", path, sources, state="unknown", error=str(e))


def _check_page(path: str, sources: list[str], source: str, shallow: bool, in_merge: bool) -> Record:
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
        rec.baseline, rec.baseline_date = base[0], base[2]
        rec.changed = sources_changed(rec.baseline, sources, source)
        rec.state = "stale" if rec.changed else "fresh"
        return rec
    rec.baseline, _then_path, rec.baseline_date = base
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
                return {}   # fail-direction: no card file at REV has no cards; build.py's card check refuses that tree
            text = r.stdout
        cards = json.loads(text).get("cards", [])
    except (OSError, ValueError, AttributeError):
        return {}   # fail-direction: build.py's card check fails the same build on an unreadable or invalid file
    return {c.get("title"): _card_key(c) for c in cards}


def _card_key(card: dict) -> dict:
    # `reviewed` is part of the card: bumping it is how a still-accurate card is
    # re-reviewed, so it must move the card's baseline like any other edit.
    key = {k: card.get(k) for k in ("title", "text", "owners", "sources", "reviewed")}
    if isinstance(key["sources"], list):     # same normalisation as page directives and card validation
        key["sources"] = [x.rstrip("/") if isinstance(x, str) else x for x in key["sources"]]
    return key


def check_cards(source: str, shallow: bool, in_merge: bool) -> list[Record]:
    """One record per feature card. Each card's baseline is the commit that last
    changed THAT card (its title, text, owners or sources), so editing one card
    does not mark the others as reviewed."""
    try:
        now = {"worktree": lambda: _cards_at(None), "index": _cards_from_index}.get(source, lambda: _cards_at(source))()
    except RuntimeError as e:                 # the card file itself unreadable: one unknown record, said so
        return [Record("card", FEATURES, [], state="unknown", error=str(e))]
    cache: dict[str, dict] = {}
    history: list[str] = []
    reader = None

    def version(i: int) -> dict:                                # fetched LAZILY, newest first,
        if history[i] not in cache:                             # through ONE cat-file process
            cache[history[i]] = _parse_cards(reader.read(f"{history[i]}:{FEATURES}"))
        return cache[history[i]]

    try:
        try:
            # Inside the handler: a history query that fails or times out makes every
            # card "unknown", never an aborted build.
            history = [] if shallow else _git("log", "--first-parent", "--format=%H", _base_rev(source), "--",
                                              FEATURES).stdout.split()
            reader = _BlobReader() if history else None
            head_cards = version(0) if history else {}
            recs = _judge_cards(now, history, version, head_cards, source, shallow, in_merge)
        finally:
            if reader:
                reader.close()
    except RuntimeError as e:                 # history unreadable: unknown, and said so
        return [Record("card", t, list(c.get("sources") or []), state="unknown", error=str(e))
                for t, c in now.items()]
    return recs


def _judge_cards(now, history, version, head_cards, source, shallow, in_merge) -> list[Record]:
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
        for i, commit in enumerate(history):                    # walk back while the card is unchanged;
            if target is None or version(i).get(title) != target:   # stops at its boundary
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


class _BlobReader:
    """A persistent `git cat-file --batch`: one process for every historical
    version read, instead of one `git show` per version."""

    def __init__(self, limit: float = GIT_TIMEOUT):
        # deadline: the timer below kills the reader `limit` seconds after it starts, ending any read in EOF
        self.proc = subprocess.Popen(["git", "cat-file", "--batch"], cwd=REPO,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        # Every caller (a local lint run included, which has no supervisor) gets the
        # same bound: a stalled read cannot hold the check, the card reads "unknown".
        self.limit, self.expired = limit, False
        self.timer = threading.Timer(limit, self._expire)
        self.timer.daemon = True
        self.timer.start()

    def _expire(self) -> None:
        self.expired = True
        self.proc.kill()

    def read(self, spec: str) -> str | None:
        """The object's text, None ONLY for git's explicit "<spec> missing"; any
        other shape (EOF, a malformed header, a short payload) is a failure and
        raises, so a dead reader can never read as "this card is new"."""
        try:
            self.proc.stdin.write(spec.encode("utf-8", "surrogateescape") + b"\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as e:
            raise RuntimeError(f"git cat-file --batch is not running: {e}") from e
        line = self.proc.stdout.readline()
        if not line and self.expired:
            raise RuntimeError(f"git cat-file --batch did not answer within {self.limit:.0f} s")
        parts = line.split()
        if len(parts) == 2 and parts[1] == b"missing":
            return None
        if len(parts) != 3 or not parts[2].isdigit():
            raise RuntimeError(f"git cat-file --batch gave an unexpected reply: {line[:80]!r}")
        size = int(parts[2])
        data = self.proc.stdout.read(size + 1)                   # content + trailing LF
        if len(data) != size + 1 or not data.endswith(b"\n"):
            raise RuntimeError(f"git cat-file --batch returned {len(data)} of {size + 1} bytes")
        return data[:-1].decode("utf-8", "replace")

    def close(self):
        self.timer.cancel()
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            rc = self.proc.wait(timeout=GIT_TIMEOUT)   # stdin is closed, so a healthy cat-file exits at once
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise RuntimeError(f"git cat-file --batch did not exit {GIT_TIMEOUT} s after its input closed") from None
        if rc not in (0, None):
            raise RuntimeError(f"git cat-file --batch exited with status {rc}")


def _parse_cards(text: str | None) -> dict[str, dict]:
    """The cards of one historical version. None is git's explicit "missing": the
    file did not exist then, so no card did. A version that does not parse, or
    holds anything but a list of card objects, RAISES, so the card reads as
    "unknown" rather than dated by a later commit (fresher than it is)."""
    if text is None:
        return {}   # git said the file is missing at that commit, so no card existed there
    try:
        data = json.loads(text)
    except ValueError as e:
        raise RuntimeError(f"{FEATURES} does not parse at a commit in its history: {e}") from None
    cards = data.get("cards") if isinstance(data, dict) else None
    if not isinstance(cards, list) or not all(isinstance(c, dict) for c in cards):
        raise RuntimeError(f"{FEATURES} at a commit in its history is not a list of card objects")
    return {c.get("title"): _card_key(c) for c in cards}


def _cards_from_index() -> dict[str, dict]:
    r = _git("show", f":{FEATURES}", ok=(0, 128))
    if r.returncode:
        return {}   # fail-direction: no staged card file has no cards; build.py's card check refuses that index
    try:
        cards = json.loads(r.stdout).get("cards", [])
    except ValueError:
        return {}   # fail-direction: build.py's card check fails the same build on an invalid staged file
    return {c.get("title"): _card_key(c) for c in cards}


def has_head() -> bool:
    return _git("rev-parse", "--verify", "--quiet", "HEAD^{commit}", ok=(0, 1, 128)).returncode == 0


def head_text(path: str) -> str | None:
    r = _git("show", f"HEAD:{path}", ok=(0, 128))
    return r.stdout if r.returncode == 0 else None   # fail-direction: HEAD lacks the page, so it adds no sources


def with_head_sources(page_sources: dict[str, list[str]], pages: list[str], parse,
                      source: str = "index") -> dict[str, list[str]]:
    """During a merge, add each page's HEAD `sources=` to what the merged tree
    declares: a merged-in directive that drops a changed source (or all of them)
    is not a review, so it must not clear the page. A page the merge renamed is
    looked up under its HEAD name."""
    out = dict(page_sources)
    for path in pages:
        text = head_text(path)
        if text is None:
            moved = uncommitted_rename(path, source)
            text = head_text(moved[0]) if moved else None
        head = parse(text or "")
        if head:
            out[path] = list(dict.fromkeys(head + out.get(path, [])))
    return out


def check_all(pages: dict[str, list[str]], source: str) -> list[Record]:
    """`pages` maps a repo-relative docs path to its declared sources."""
    from concurrent.futures import ThreadPoolExecutor
    # The shared probes are NOT caught: if git cannot answer them (or times out), the
    # check fails with the reason, which is bounded and fails closed. Only one page's
    # or one card's history reads "unknown".
    if source in ("worktree", "index") and not has_head():
        # Before the first commit there is no history to compare against.
        return [Record("page", p, s, state="new") for p, s in sorted(pages.items())]
    shallow, in_merge = is_shallow(), merging()
    with ThreadPoolExecutor(max_workers=8) as pool:   # git calls are I/O-bound subprocesses
        recs = list(pool.map(lambda kv: check_page(kv[0], kv[1], source, shallow, in_merge), sorted(pages.items())))
    return recs + check_cards(source, shallow, in_merge)


def warning(rec: Record) -> str | None:
    if rec.state == "unknown" and rec.error:
        return f"{rec.name}: freshness could not be checked ({rec.error})"
    if rec.state != "stale":
        return None
    shown = ", ".join(rec.changed[:3]) + (f" and {len(rec.changed) - 3} more" if len(rec.changed) > 3 else "")
    what = rec.name if rec.kind == "page" else f"{FEATURES} card {rec.name!r}"
    return (f"{what}: its sources changed since it was last updated on {rec.baseline_date} ({shown}); "
            f"update it, or bump its reviewed= date if it is still accurate")
