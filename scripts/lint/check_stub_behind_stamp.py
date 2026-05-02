#!/usr/bin/env python3
# ============================================================================
# check_stub_behind_stamp.py -- lint Check 7 implementation.
#
# Walks build/todo-cache.json (TODO graph cache, per-item extension)
# and finds AI-slop stub-behind-stamp findings: a `[x]` checklist item
# names a function whose body is a <=3-line `return CONSTANT;` placeholder
# without an /* INTENTIONAL-STUB: <reason> */ marker on the body opener.
#
# Output: one finding per line in the format
#   <file>:<body_open_line>:stub-behind-stamp:<symbol> stamped in <todo>:<sec>:<item>
#
# Caller (scripts/lint.sh) routes each line through error()/warn() based
# on its own legacy-allowlist policy. Exit code 0 on clean walk; nonzero
# only on internal failure (malformed cache, missing dep). Findings do
# NOT change exit code -- the bash caller handles that.
# ============================================================================

import json
import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "todo-graph"))

try:
    from resolve_symbol import resolve_symbol, is_stub_body
except ImportError as exc:
    sys.stderr.write(f"[check_stub_behind_stamp] FATAL: cannot import resolve_symbol: {exc}\n")
    sys.exit(2)


def _walk(cache_path: Path, repo_root: Path) -> int:
    try:
        nodes = json.loads(cache_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"[check_stub_behind_stamp] FATAL: cache unreadable: {exc}\n")
        return 2
    if not isinstance(nodes, list):
        sys.stderr.write("[check_stub_behind_stamp] FATAL: cache is not a JSON array\n")
        return 2

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
                    # Symbol with no resolved file: per the per-item cache
                    # extension contract, the build.py parser pairs
                    # symbol refs with the first file ref in the same
                    # item. Missing pairing means the item named a
                    # symbol but no file -- silently skip rather than
                    # guess across the tree.
                    continue
                # Repo-escape guard (Codex F3): cache refs originate from
                # markdown links in TODO files; a path like `../../etc/foo.c`
                # could resolve outside the repo. Reject absolute paths and
                # any resolved path not under repo_root.
                if file_rel.startswith("/"):
                    continue
                try:
                    file_abs = (repo_root / file_rel).resolve()
                    file_abs.relative_to(repo_root.resolve())
                except (ValueError, OSError):
                    continue
                if not file_abs.is_file():
                    continue
                # Only inspect C source/headers. Skip Python / shell / md
                # so symbol-named items in a docs `[x]` line don't
                # mis-trigger.
                if file_abs.suffix not in (".c", ".h"):
                    continue
                resolved = resolve_symbol(str(file_abs), symbol)
                if resolved is None:
                    continue
                _, line_start, line_end = resolved
                stub = is_stub_body(str(file_abs), line_start, line_end)
                if stub is None:
                    continue
                ret_const, body_open_line = stub
                # Trim item text to a sane lint-line length.
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
    return 0 if findings >= 0 else 2


def main() -> int:
    cache_arg = os.environ.get("STUB_LINT_CACHE")
    repo_arg = os.environ.get("STUB_LINT_REPO_ROOT")
    if cache_arg:
        cache_path = Path(cache_arg)
    else:
        cache_path = REPO_ROOT / "build" / "todo-cache.json"
    if repo_arg:
        repo_root = Path(repo_arg).resolve()
    else:
        repo_root = REPO_ROOT
    if not cache_path.is_file():
        sys.stderr.write(f"[check_stub_behind_stamp] cache missing: {cache_path}\n")
        return 2
    return _walk(cache_path, repo_root)


if __name__ == "__main__":
    sys.exit(main())
