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
from resolve_symbol import (  # noqa: E402
    ResolverInputError, follow_declaration, resolve_symbol)


def _repo_root() -> Path:
    return Path(os.environ.get("STUB_LINT_REPO_ROOT", ".")).resolve()


def _cache_path() -> Path:
    return Path(os.environ.get("STUB_LINT_CACHE", "build/todo-cache.json"))


SNAPSHOT_SCHEMA = 1


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
    # Walk the SELECTED repository's todo/, not the process CWD's. Using a bare
    # relative "todo" meant the staleness check silently measured nothing
    # whenever the tool ran from anywhere but the repo root -- exactly the
    # fail-open the rest of this function exists to close.
    #
    # os.walk SWALLOWS traversal errors unless given an onerror callback -- a
    # missing root simply yields nothing and an unreadable subtree is skipped.
    # Wrapping the walk in try/except therefore caught nothing: an unreadable
    # TODO subtree would leave `newest` low and an old cache would look fresh,
    # which is the exact fail-open this check exists to close.
    def _walk_err(exc):
        raise CacheError(f"cannot traverse todo tree: {exc}")

    todo_root = _repo_root() / "todo"
    if not todo_root.is_dir():
        raise CacheError(f"todo root is not a readable directory: {todo_root}")
    try:
        cache_m = p.stat().st_mtime
        newest = 0.0
        for dirpath, _, files in os.walk(todo_root, onerror=_walk_err):
            for f in files:
                if f.startswith("TODO-") and f.endswith(".md"):
                    newest = max(newest,
                                 os.stat(os.path.join(dirpath, f)).st_mtime)
    except OSError as exc:
        # A stat failure is an INFRASTRUCTURE error, not "fresh enough".
        raise CacheError(f"cannot determine cache freshness: {exc}")
    if newest == 0.0:
        raise CacheError(f"no TODO files found under {todo_root} -- refusing to "
                         f"call a cache fresh against an empty corpus")
    if newest > cache_m:
        raise CacheError(
            f"cache is STALE (a TODO is newer than {p}); rebuild via "
            f"scripts/todo-graph/build-and-validate.sh --keep-cache")
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
                        # MIRROR check_stub_behind_stamp._walk's fallback
                        # exactly: a header-only ref may still resolve via
                        # its repo-convention sibling .c. Without this, the
                        # snapshot silently omitted every follow_declaration
                        # mapping -- Check 7's floor rose 57 -> 111 while this
                        # tool's own "0 lost / 0 moved" proof covered only
                        # the resolve_symbol()-direct subset (Codex
                        # adversarial: the identity proof did not cover what
                        # it claimed to cover).
                        res = follow_declaration(str(abs_p), sym, str(root))
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
    except ResolverInputError as exc:
        # `collect()` calls resolve_symbol() per ref, and resolve_symbol() now
        # raises this for an input it refuses to answer about (oversized file,
        # or one that changed on disk mid-run) rather than silently returning
        # None. Left uncaught, this surfaced as an unhandled traceback and a
        # bare process exit 1 -- colliding with exit 1's DOCUMENTED meaning
        # here (a prior mapping was lost or moved), which a caller reading
        # only the exit code would misread as a real regression instead of
        # "the walk could not complete". Same INFRASTRUCTURE code as
        # CacheError, for the same reason: not a result, not a pass.
        sys.stderr.write(f"[corpus_resolution_snapshot] resolver input "
                         f"refused: {exc}\n")
        return 3
    resolved_now = {k: v for k, v in now.items() if v is not None}
    if not now:
        sys.stderr.write("[corpus_resolution_snapshot] no eligible refs -- "
                         "refusing to treat an empty population as a result\n")
        return 3

    if argv[0] == "write":
        # SELF-DESCRIBING. The bare mapping-only format could not distinguish a
        # complete baseline from a truncated one: a single-entry file compared
        # clean, reporting the other 56 resolved mappings as ADDED and exiting 0.
        # Recording the population lets `compare` detect truncation instead of
        # trusting the file's own size.
        Path(argv[1]).write_text(json.dumps({
            "schema": SNAPSHOT_SCHEMA,
            "refs": len(now),
            "resolved": len(resolved_now),
            "mappings": now,
        }, indent=1, sort_keys=True), encoding="utf-8")
        print(f"snapshot: {len(resolved_now)} resolved of {len(now)} refs "
              f"-> {argv[1]}")
        return 0

    # VALIDATE THE BASELINE TOO. Validating only the live cache left the other
    # input fail-open: a `{}` baseline, or one whose mappings are all null,
    # yields an empty `resolved_before`, so every current mapping classifies as
    # ADDED, `lost` and `moved` stay empty, and compare exits 0 having checked
    # nothing. That is the same vacuous pass the cache validation above exists
    # to prevent, on the input the gate is actually comparing against.
    try:
        before = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline unreadable: "
                         f"{argv[1]}: {exc}\n")
        return 3
    if not isinstance(before, dict) or not before:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline is not a "
                         f"non-empty object: {argv[1]}\n")
        return 3
    if before.get("schema") != SNAPSHOT_SCHEMA:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline schema is "
                         f"{before.get('schema')!r}, expected {SNAPSHOT_SCHEMA}"
                         f" -- regenerate it with `write`\n")
        return 3
    mappings = before.get("mappings")
    if not isinstance(mappings, dict) or not mappings:
        sys.stderr.write("[corpus_resolution_snapshot] baseline has no "
                         "mappings object\n")
        return 3
    # TRUNCATION: the recorded population must match what the file actually
    # holds. Without this a one-entry baseline passed cleanly.
    if before.get("refs") != len(mappings):
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline is TRUNCATED: "
                         f"declares {before.get('refs')} refs, holds "
                         f"{len(mappings)}\n")
        return 3
    # ELEMENT TYPES, not just arity. `[path, 411.0, 442.0]` satisfied a
    # length-3 check and compared EQUAL to the integer tuple under Python, so
    # an arity-only check let a malformed baseline through.
    bad = []
    for k, v in mappings.items():
        if v is None:
            continue
        if not (isinstance(v, list) and len(v) == 3):
            bad.append(k)
            continue
        pth, a, b = v
        if not isinstance(pth, str) or not pth:
            bad.append(k)
        elif isinstance(a, bool) or isinstance(b, bool):
            bad.append(k)
        elif not isinstance(a, int) or not isinstance(b, int):
            bad.append(k)
        elif a < 1 or b < a:
            bad.append(k)
    if bad:
        sys.stderr.write(f"[corpus_resolution_snapshot] baseline has "
                         f"{len(bad)} malformed mapping(s), e.g. {bad[0]!r}\n")
        return 3
    before = mappings
    resolved_before = {k: v for k, v in before.items() if v is not None}
    if not resolved_before:
        sys.stderr.write("[corpus_resolution_snapshot] baseline records ZERO "
                         "resolved mappings -- there is nothing to protect, so "
                         "a pass would be vacuous\n")
        return 3

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
