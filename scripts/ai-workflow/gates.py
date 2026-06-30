#!/usr/bin/env python3
"""Shared gates for Claude hooks, git hooks, and Codex driver adapters."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from ai_workflow_import import common  # type: ignore
else:
    from . import common

try:
    if __package__ in (None, ""):
        import importlib.util

        _OB_PATH = Path(__file__).resolve().parent / "obligations.py"
        _spec = importlib.util.spec_from_file_location("ai_workflow_obligations", _OB_PATH)
        assert _spec and _spec.loader
        obligations = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(obligations)
    else:
        from . import obligations
except Exception as exc:  # pragma: no cover
    raise SystemExit(f"failed to load obligations helper: {exc}")


LEASE_HISTORY_SCAN_LIMIT = 512


def _resolve(todo: str, section: str, workflow: str, driver_run_id: str | None) -> dict:
    class _Args:
        pass

    a = _Args()
    a.todo = todo
    a.section = section
    a.workflow = workflow
    a.driver_run_id = driver_run_id
    return obligations.resolve(a)  # type: ignore[arg-type]


def _active_or_completed_lease(todo: str, section: str, root: Path) -> tuple[bool, list[str]]:
    todo = common.rel_path(todo, root)
    section = str(section)
    active = common.read_json(common.state_dir(root) / "active-lease.json", {})
    if isinstance(active, dict) and active:
        if active.get("todo_path") == todo and str(active.get("section")) == section:
            if int(active.get("expires_at_ns") or 0) >= common.now_ns():
                return True, [f"active:{active.get('driver_run_id') or ''}"]
    for idx, item in enumerate(common.iter_jsonl_reverse(common.state_dir(root) / "lease-history.jsonl")):
        if idx >= LEASE_HISTORY_SCAN_LIMIT:
            break
        if item.get("action") != "complete":
            continue
        lease = item.get("lease") or {}
        if not isinstance(lease, dict):
            continue
        if lease.get("todo_path") == todo and str(lease.get("section")) == section:
            return True, [f"complete:{lease.get('driver_run_id') or ''}"]
    return False, []


def _staged_todo_files(root: Path) -> tuple[list[str], str]:
    try:
        out = subprocess.check_output(
            ["git", "diff", "--cached", "--name-only", "-z"],
            cwd=str(root), text=True, timeout=5, stderr=subprocess.DEVNULL,
        )
    except Exception as exc:
        return ([], f"staged todo inspection failed: {type(exc).__name__}")
    return (
        [p for p in out.split("\x00") if p.startswith("todo/") and p.endswith(".md")],
        "",
    )


_IO_NEW_DONE_RE = re.compile(r"^\+\s*\|.+\|\s*\[x\]\s*\|\s*$")
_STAMP_ADDED_RE = re.compile(
    r"^\+\s{0,3}(?:>\s*)+\*\*(Verified|Quality reviewed):\*\*",
)
_HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")


def _staged_file_lines(root: Path, path: str) -> list[str]:
    try:
        out = subprocess.check_output(
            ["git", "show", f":{path}"],
            cwd=str(root),
            text=True,
            stderr=subprocess.DEVNULL,
            timeout=5,
        )
    except Exception:
        return []
    return out.splitlines()


def _section_for_added_line(root: Path, todo: str, added_lineno: int) -> str:
    section = ""
    lines = _staged_file_lines(root, todo)
    if not lines:
        return section
    for idx in range(min(added_lineno, len(lines)) - 1, -1, -1):
        m = re.match(r"^##+\s+(\d+)\.\s+", lines[idx])
        if m:
            section = m.group(1)
            break
    return section


def _table_cells(line: str) -> list[str]:
    text = line.strip()
    if not text.startswith("|") or not text.endswith("|"):
        return []
    return [cell.strip() for cell in text.strip("|").split("|")]


def _table_labels(cells: list[str]) -> list[str]:
    return [re.sub(r"\s+", " ", cell.strip().lower()) for cell in cells]


def _section_value(raw: object) -> str:
    text = str(raw or "").strip()
    m = re.fullmatch(r"§\s*(\d+)", text)
    if m:
        return m.group(1)
    m = re.fullmatch(r"(?:[sS]|section\s+)?(\d+)", text, re.IGNORECASE)
    if m:
        return m.group(1)
    return ""


def _io_header_for_added_line(root: Path, todo: str, added_lineno: int) -> list[str]:
    lines = _staged_file_lines(root, todo)
    if not lines:
        return []
    start = min(max(added_lineno - 1, 0), len(lines) - 1)
    for idx in range(start, -1, -1):
        if re.match(r"^##+\s+", lines[idx]):
            break
        cells = _table_cells(lines[idx])
        labels = _table_labels(cells)
        if "status" in labels and ("order" in labels or "section" in labels):
            return cells
    return []


def _section_from_done_io_row(root: Path, todo: str, added_lineno: int, diff_line: str) -> str:
    """Return the target section for an added [x] Implementation Order row.

    The target is header-bound so dependency cells cannot satisfy a lease for
    the wrong section.
    """
    if not _IO_NEW_DONE_RE.match(diff_line):
        return ""
    cells = _table_cells(diff_line[1:])
    if not cells or cells[-1] != "[x]":
        return ""
    header = _io_header_for_added_line(root, todo, added_lineno)
    labels = _table_labels(header)
    for wanted in ("section", "order"):
        if wanted in labels:
            idx = labels.index(wanted)
            if idx < len(cells):
                return _section_value(cells[idx])
            return ""
    numeric_cells = [cell for cell in cells if re.fullmatch(r"\d+", cell)]
    if numeric_cells:
        return numeric_cells[0]
    return ""


def _staged_targets(root: Path) -> tuple[list[dict[str, str]], list[str]]:
    targets: list[dict[str, str]] = []
    errors: list[str] = []
    seen: set[tuple[str, str, str]] = set()
    todos, todo_err = _staged_todo_files(root)
    if todo_err:
        return (targets, [todo_err])
    for todo in todos:
        try:
            diff = subprocess.check_output(
                ["git", "diff", "--cached", "-U0", "--", todo],
                cwd=str(root), text=True, timeout=5,
                stderr=subprocess.DEVNULL,
            )
        except Exception as exc:
            errors.append(f"staged diff inspection failed for {todo}: {type(exc).__name__}")
            continue
        new_lineno = 0
        for line in diff.splitlines():
            hm = _HUNK_RE.match(line)
            if hm:
                new_lineno = int(hm.group(1)) - 1
                continue
            if line.startswith("+++") or line.startswith("---"):
                continue
            if line.startswith("+"):
                new_lineno += 1
                section = _section_from_done_io_row(root, todo, new_lineno, line)
                if section:
                    item = (todo, section, "implementation-order")
                    if item not in seen:
                        seen.add(item)
                        targets.append({"todo": todo, "section": section, "reason": item[2]})
                    continue
                if _STAMP_ADDED_RE.match(line):
                    section = _section_for_added_line(root, todo, new_lineno)
                    if section:
                        item = (todo, section, "stamp")
                        if item not in seen:
                            seen.add(item)
                            targets.append({"todo": todo, "section": section, "reason": item[2]})
            elif line.startswith("-"):
                continue
            else:
                if new_lineno:
                    new_lineno += 1
    return (targets, errors)


def staged_commit(args: argparse.Namespace) -> int:
    root = common.repo_root()
    targets, errors = _staged_targets(root)
    missing: list[dict[str, str]] = []
    satisfied: list[dict[str, object]] = []
    for target in targets:
        ok, ids = _active_or_completed_lease(target["todo"], target["section"], root)
        if ok:
            done = dict(target)
            done["lease_ids"] = ids
            satisfied.append(done)
        else:
            missing.append(target)
    verdict = "ALLOW" if not missing and not errors else "BLOCK"
    result = {
        "gate": "staged-commit",
        "verdict": verdict,
        "targets": targets,
        "satisfied": satisfied,
        "missing": missing,
        "errors": errors,
    }
    if args.format == "json":
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"{verdict} staged-commit: {len(targets)} target(s)")
        for err in errors:
            print(f"  inspection error: {err}")
        for item in missing:
            print(
                "  missing lease: "
                f"{item['todo']} section {item['section']} ({item['reason']})"
            )
    return 0 if not missing and not errors else 2


def check(args: argparse.Namespace) -> int:
    if args.gate == "staged-commit":
        return staged_commit(args)
    if not args.todo or not args.section:
        print("--todo and --section are required for this gate", file=sys.stderr)
        return 2
    out = _resolve(args.todo, args.section, args.workflow, args.driver_run_id)
    missing = [item["name"] for item in out["required"] if item["status"] == "missing"]
    if args.gate == "pre-edit":
        missing = [m for m in missing if m == "driver-lease"]
    elif args.gate == "stamp":
        missing = [m for m in missing if m != "verified-stamp"]
    elif args.gate == "commit":
        pass
    verdict = "ALLOW" if not missing else "BLOCK"
    result = {
        "gate": args.gate,
        "verdict": verdict,
        "missing": missing,
        "obligations": out,
    }
    if args.format == "json":
        print(json.dumps(result, indent=2, sort_keys=True))
    else:
        print(f"{verdict} {args.gate}: {args.todo} section {args.section}")
        for item in missing:
            print(f"  missing: {item}")
    return 0 if not missing else 2


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gate", choices=("pre-edit", "stamp", "commit", "staged-commit"))
    ap.add_argument("--todo")
    ap.add_argument("--section")
    ap.add_argument("--workflow", choices=("implement", "review", "verify", "complete-file"), default="implement")
    ap.add_argument("--driver-run-id")
    ap.add_argument("--mode", default="direct")
    ap.add_argument("--format", choices=("text", "json"), default="text")
    return check(ap.parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
