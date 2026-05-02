#!/usr/bin/env python3
# ============================================================================
# check_stub_behind_stamp.py -- lint Check 7 implementation.
#
# Walks build/todo-cache.json (TODO graph cache, per-item extension)
# and finds AI-slop stub-behind-stamp findings: a `[x]` checklist item
# names a function whose body is a <=3-line `return CONSTANT;` placeholder
# without an /* INTENTIONAL-STUB: <reason> */ marker on the body opener.
#
# Single Python entry point for lint Check 7: handles the cache existence
# probe, schema (`stamped_items` presence) check, mtime staleness check,
# AND the actual stub walk in one process. Single JSON parse; per-run
# file-content + symbol-resolution caches. Replaces 3 separate Python
# spawns + bash heredoc that the earlier shape used.
#
# Output: one finding per line in the format
#   <file>:<body_open_line>:stub-behind-stamp:<symbol> stamped in <todo>:<sec>:<item>
#
# Distinct exit codes (consumed by scripts/lint.sh Check 7):
#   0  walk completed (with or without findings -- caller counts findings)
#   2  cache missing (lint.sh treats as WARN -- contributor hasn't built)
#   3  cache lacks `stamped_items` field (lint.sh treats as WARN -- old cache)
#   4  cache stale relative to todo/**/*.md (lint.sh treats as ERROR)
#   5  cache unreadable / malformed JSON (lint.sh treats as ERROR)
#   6  internal helper failure (lint.sh treats as ERROR)
# ============================================================================

import json
import os
import sys
from functools import lru_cache
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "todo-graph"))

try:
    import resolve_symbol as _rs
except ImportError as exc:
    sys.stderr.write(f"[check_stub_behind_stamp] FATAL: cannot import resolve_symbol: {exc}\n")
    sys.exit(6)


# Per-run dedupe of resolve / stub work. The actual file-line cache lives
# in resolve_symbol.py at module scope (`_load_file_lines`) and is shared
# by both resolve_symbol() and is_stub_body(); these wrappers just memoize
# the parsed-result so the same (file, symbol) tuple does not re-walk the
# regex/brace logic when multiple stamped items name it.
@lru_cache(maxsize=2048)
def _resolve_cached(file_path: str, symbol: str):
    return _rs.resolve_symbol(file_path, symbol)


@lru_cache(maxsize=2048)
def _is_stub_cached(file_path: str, line_start: int, line_end: int):
    return _rs.is_stub_body(file_path, line_start, line_end)


def _check_staleness(cache_path: Path, todo_root: Path) -> bool:
    """True if cache is stale relative to any TODO-*.md mtime."""
    try:
        cache_m = cache_path.stat().st_mtime
    except OSError:
        return False
    newest = 0.0
    for dirpath, _, files in os.walk(todo_root):
        for f in files:
            if f.startswith("TODO-") and f.endswith(".md"):
                try:
                    m = os.stat(os.path.join(dirpath, f)).st_mtime
                    if m > newest:
                        newest = m
                except OSError:
                    pass
    return newest > cache_m


def _walk(nodes: list, repo_root: Path) -> int:
    repo_resolved = repo_root.resolve()
    findings = 0
    for node in nodes:
        items = node.get("stamped_items") or []
        for it in items:
            for ref in it.get("refs", []):
                if ref.get("kind") != "symbol":
                    continue
                file_rel = ref.get("file")
                symbol = ref.get("symbol")
                if not file_rel or not symbol:
                    continue
                if file_rel.startswith("/"):
                    continue
                try:
                    file_abs = (repo_root / file_rel).resolve()
                    file_abs.relative_to(repo_resolved)
                except (ValueError, OSError):
                    continue
                if not file_abs.is_file():
                    continue
                if file_abs.suffix not in (".c", ".h"):
                    continue
                resolved = _resolve_cached(str(file_abs), symbol)
                if resolved is None:
                    continue
                _, line_start, line_end = resolved
                stub = _is_stub_cached(str(file_abs), line_start, line_end)
                if stub is None:
                    continue
                ret_const, body_open_line = stub
                item_text = (it.get("item_text") or "").strip()
                if len(item_text) > 100:
                    item_text = item_text[:97] + "..."
                todo_path = node.get("file_path") or "?"
                section_n = it.get("section_n", "?")
                print(
                    f"{file_rel}:{body_open_line}:stub-behind-stamp:{symbol} "
                    f"returns {ret_const} (stamped [x] in {todo_path} "
                    f"section {section_n}: \"{item_text}\")"
                )
                findings += 1
    return findings


def main() -> int:
    cache_arg = os.environ.get("STUB_LINT_CACHE")
    repo_arg = os.environ.get("STUB_LINT_REPO_ROOT")
    cache_path = Path(cache_arg) if cache_arg else REPO_ROOT / "build" / "todo-cache.json"
    repo_root = Path(repo_arg).resolve() if repo_arg else REPO_ROOT
    todo_root = repo_root / "todo"

    if not cache_path.is_file():
        sys.stderr.write(f"[check_stub_behind_stamp] cache missing: {cache_path}\n")
        return 2

    try:
        nodes = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"[check_stub_behind_stamp] cache unreadable: {exc}\n")
        return 5
    if not isinstance(nodes, list):
        sys.stderr.write("[check_stub_behind_stamp] cache is not a JSON array\n")
        return 5

    if not any("stamped_items" in (n or {}) for n in nodes):
        sys.stderr.write("[check_stub_behind_stamp] cache lacks stamped_items field\n")
        return 3

    if todo_root.is_dir() and _check_staleness(cache_path, todo_root):
        sys.stderr.write("[check_stub_behind_stamp] cache stale relative to todo/**/*.md\n")
        return 4

    try:
        _walk(nodes, repo_root)
    except Exception as exc:  # pragma: no cover -- defensive
        sys.stderr.write(f"[check_stub_behind_stamp] internal failure: {exc}\n")
        return 6
    return 0


if __name__ == "__main__":
    sys.exit(main())
