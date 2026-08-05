#!/usr/bin/env python3
# ============================================================================
# corpus_resolution_snapshot.py -- record what resolve_symbol() ACTUALLY
# resolves across the live TODO corpus, as (file, symbol) -> (path, start, end).
#
# Why this exists. Check 7's coverage gate (scripts/lint/stub-lint-baseline.json)
# records a COUNT, and a count is not a mapping. A change can hold the count at
# 55 while silently re-pointing an existing symbol at the wrong line range, or
# while a wrong new resolution offsets a lost one. That is exactly the blind spot
# that let TODO-06 section 10's first attempt pass 115/115 tests plus its own
# mutation-checked fixtures and still take coverage from 52/186 to 1/186
# (reverted, 5cff59cc): synthetic fixtures proved the NEW shapes parsed, and
# nothing checked that the shapes which already worked still did.
#
# So the contract this tool enforces is stronger than the count:
#   every (file, symbol) resolved BEFORE a change must resolve to the SAME
#   (path, line_start, line_end) AFTER it. New resolutions are additions only,
#   and are printed so a human ground-truths them rather than trusting the count.
#
# Usage:
#   corpus_resolution_snapshot.py write <out.json>       # snapshot current tree
#   corpus_resolution_snapshot.py compare <before.json>  # diff vs current tree
#
# `compare` exits 0 when no prior mapping was lost or moved, 1 otherwise.
# ============================================================================

import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from resolve_symbol import resolve_symbol  # noqa: E402


def _repo_root() -> Path:
    return Path(os.environ.get("STUB_LINT_REPO_ROOT", ".")).resolve()


def _cache_path() -> Path:
    return Path(os.environ.get("STUB_LINT_CACHE", "build/todo-cache.json"))


class CacheError(RuntimeError):
    """The cache cannot support a trustworthy snapshot."""


def _load_nodes() -> list:
    """Parse AND VALIDATE the cache.

    A gate that fails OPEN is worse than no gate. `collect()` originally trusted
    any JSON: a cache of `[]` or `[{}]` yielded zero refs, `write` exited 0, and
    the resulting empty baseline made `compare` report "OK, 54 added" -- a
    vacuous pass in which a resolver regression on any ref absent from that
    baseline is never checked. Semantic emptiness and staleness are the COMMON
    corruption paths (a syntax error already failed loudly), so both are refused
    here rather than silently producing a passing snapshot.
    """
    p = _cache_path()
    try:
        nodes = json.loads(p.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise CacheError(f"cache not found: {p} (run build-and-validate.sh)")
    except (OSError, json.JSONDecodeError) as exc:
        raise CacheError(f"cache unreadable: {p}: {exc}")
    if not isinstance(nodes, list) or not nodes:
        raise CacheError(f"cache is not a non-empty node array: {p}")
    if not any(isinstance(n, dict) and n.get("stamped_items") for n in nodes):
        raise CacheError(
            f"cache carries no stamped_items -- it predates the section 9 "
            f"extension, or came from a different generator: {p}")
    # Staleness, mirroring check_stub_behind_stamp.py: a cache older than the
    # newest TODO describes a tree that no longer exists, and a baseline taken
    # from it silently omits live refs.
    try:
        cache_m = p.stat().st_mtime
        newest = 0.0
        for dirpath, _, files in os.walk("todo"):
            for f in files:
                if f.startswith("TODO-") and f.endswith(".md"):
                    newest = max(newest,
                                 os.stat(os.path.join(dirpath, f)).st_mtime)
        if newest > cache_m:
            raise CacheError(
                f"cache is STALE (a TODO is newer than {p}); rebuild via "
                f"scripts/todo-graph/build-and-validate.sh --keep-cache")
    except OSError:
        pass  # cannot stat -- do not manufacture a staleness verdict
    return nodes


def collect() -> dict:
    """Return {"<todo>#<sec>.<item>r<n> <file>::<symbol>": [path, start, end]
    | None} for every symbol ref carrying a paired file in the cache's
    stamped_items -- the exact ref population lint Check 7 walks.

    KEYED PER OCCURRENCE, not per (file, symbol). Deduping looked harmless --
    the same pair always resolves the same way -- but it quietly weakened the
    contract. Seven pairs in the live corpus appear twice; if one occurrence
    disappears or changes away from `kind=symbol`, the surviving occurrence
    holds the key and the tuple, so `compare` reports OK while a stamped
    reference really did regress.
    """
    root = _repo_root()
    nodes = _load_nodes()
    out = {}
    memo = {}  # (abs, sym) -> tuple|None; resolution is deterministic per pair
    for node in nodes:
        todo_path = node.get("file_path") or "?"
        for it in node.get("stamped_items") or []:
            sec = it.get("section_n", "?")
            idx = it.get("item_idx", "?")
            for ref_i, ref in enumerate(it.get("refs") or []):
                if ref.get("kind") != "symbol":
                    continue
                rel, sym = ref.get("file"), ref.get("symbol")
                if not rel or not sym:
                    continue
                # MIRROR check_stub_behind_stamp._walk's ref filters exactly
                # (:101-111). A snapshot over a wider population than Check 7
                # actually walks is not a gate on Check 7 -- it would report
                # "unresolved" for .md/.py refs the check never attempts, and
                # the before/after numbers would not be comparable to the
                # coverage baseline they exist to protect.
                if rel.startswith("/"):
                    continue
                try:
                    abs_p = (root / rel).resolve()
                    abs_p.relative_to(root)
                except (ValueError, OSError):
                    continue
                if not abs_p.is_file() or abs_p.suffix not in (".c", ".h"):
                    continue
                key = f"{todo_path}#{sec}.{idx}r{ref_i} {rel}::{sym}"
                memo_key = (str(abs_p), sym)
                if memo_key not in memo:
                    res = resolve_symbol(str(abs_p), sym)
                    if res is None:
                        memo[memo_key] = None
                    else:
                        # ALWAYS repo-relative. An absolute fallback here would
                        # differ between checkouts and read as a false MOVE, so
                        # a path that will not relativise is a hard error, not
                        # a quietly-different value.
                        rp = Path(res[0]).resolve()
                        try:
                            rel_out = str(rp.relative_to(root))
                        except ValueError:
                            raise CacheError(
                                f"resolved outside repo root: {rp} (root {root})")
                        memo[memo_key] = [rel_out, res[1], res[2]]
                out[key] = memo[memo_key]
    return out


def main(argv) -> int:
    if len(argv) != 2 or argv[0] not in ("write", "compare"):
        sys.stderr.write(
            "usage: corpus_resolution_snapshot.py write <out.json>\n"
            "       corpus_resolution_snapshot.py compare <before.json>\n")
        return 2

    try:
        now = collect()
    except CacheError as exc:
        # Exit 3 == INFRASTRUCTURE, deliberately distinct from 0 (pass) and
        # 1 (regression). A caller must never read "the gate could not run" as
        # "the gate passed".
        sys.stderr.write(f"[corpus_resolution_snapshot] {exc}\n")
        return 3
    resolved_now = {k: v for k, v in now.items() if v is not None}
    if not now:
        sys.stderr.write("[corpus_resolution_snapshot] no eligible refs -- "
                         "refusing to treat an empty population as a result\n")
        return 3

    if argv[0] == "write":
        Path(argv[1]).write_text(
            json.dumps(now, indent=1, sort_keys=True), encoding="utf-8")
        print(f"snapshot: {len(resolved_now)} resolved of {len(now)} refs "
              f"-> {argv[1]}")
        return 0

    before = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    resolved_before = {k: v for k, v in before.items() if v is not None}

    lost, moved = [], []
    for k, v in resolved_before.items():
        nv = now.get(k)
        if nv is None:
            lost.append((k, v))
        elif nv != v:
            moved.append((k, v, nv))
    added = sorted(k for k in resolved_now if k not in resolved_before)

    print(f"before: {len(resolved_before)} resolved of {len(before)} refs")
    print(f"after:  {len(resolved_now)} resolved of {len(now)} refs")
    for k, v in lost:
        print(f"  LOST   {k} was {v}")
    for k, v, nv in moved:
        print(f"  MOVED  {k} {v} -> {nv}")
    for k in added:
        print(f"  ADDED  {k} -> {resolved_now[k]}  (GROUND-TRUTH BY HAND)")

    if lost or moved:
        print(f"\nFAIL: {len(lost)} lost, {len(moved)} moved. A prior mapping "
              f"changed -- the resolved COUNT alone would not have shown this.")
        return 1
    print(f"\nOK: no prior mapping lost or moved; {len(added)} added.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
