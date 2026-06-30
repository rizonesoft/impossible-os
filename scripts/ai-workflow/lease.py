#!/usr/bin/env python3
"""Driver lease manager for the AI workflow protocol."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from ai_workflow_import import common  # type: ignore
else:
    from . import common


DEFAULT_TTL_SEC = 60 * 60


def _paths(root: Path) -> tuple[Path, Path]:
    sd = common.ensure_state_dir(root)
    return sd / "active-lease.json", sd / "lease-history.jsonl"


def _task_id(todo: str, section: str) -> str:
    return f"{todo}#{section}"


def _load_active(root: Path) -> dict[str, Any] | None:
    active, _history = _paths(root)
    data = common.read_json(active, None)
    return data if isinstance(data, dict) else None


def _is_expired(lease: dict[str, Any], now: int) -> bool:
    expires = int(lease.get("expires_at_ns") or 0)
    return expires > 0 and expires < now


def _record(root: Path, action: str, lease: dict[str, Any], reason: str = "") -> None:
    _active, history = _paths(root)
    event = {
        "action": action,
        "at_ns": common.now_ns(),
        "lease": lease,
    }
    if reason:
        event["reason"] = reason
    common.append_jsonl(history, event)


def acquire(args: argparse.Namespace) -> int:
    root = common.repo_root()
    active_path, _history = _paths(root)
    now = common.now_ns()
    existing = _load_active(root)
    todo = common.rel_path(args.todo, root)
    section = str(args.section)
    task_id = _task_id(todo, section)
    run_id = args.run_id
    if existing and not _is_expired(existing, now):
        same_task = existing.get("task_id") == task_id
        same_run = existing.get("driver_run_id") == run_id
        if same_task and same_run:
            existing["expires_at_ns"] = now + int(args.ttl_sec * 1_000_000_000)
            existing["renewed_at_ns"] = now
            common.write_json_atomic(active_path, existing)
            _record(root, "renew", existing)
            print(json.dumps(existing, indent=2, sort_keys=True))
            return 0
        print(
            "lease conflict: active driver "
            f"{existing.get('driver_backend')}:{existing.get('driver_run_id')} "
            f"owns {existing.get('task_id')}",
            file=sys.stderr,
        )
        return 2

    if existing and _is_expired(existing, now):
        _record(root, "expire", existing)

    lease = {
        "task_id": task_id,
        "todo_path": todo,
        "section": section,
        "driver_backend": args.driver,
        "driver_run_id": run_id,
        "head_sha": common.head_sha(root),
        "started_at_ns": now,
        "expires_at_ns": now + int(args.ttl_sec * 1_000_000_000),
        "allowed_mutations": args.mutation or [],
    }
    common.write_json_atomic(active_path, lease)
    _record(root, "acquire", lease)
    print(json.dumps(lease, indent=2, sort_keys=True))
    return 0


def renew(args: argparse.Namespace) -> int:
    root = common.repo_root()
    active_path, _history = _paths(root)
    lease = _load_active(root)
    if not lease:
        print("no active lease", file=sys.stderr)
        return 1
    if args.run_id and lease.get("driver_run_id") != args.run_id:
        print("run-id does not own active lease", file=sys.stderr)
        return 2
    now = common.now_ns()
    lease["renewed_at_ns"] = now
    lease["expires_at_ns"] = now + int(args.ttl_sec * 1_000_000_000)
    common.write_json_atomic(active_path, lease)
    _record(root, "renew", lease)
    print(json.dumps(lease, indent=2, sort_keys=True))
    return 0


def release(args: argparse.Namespace) -> int:
    root = common.repo_root()
    active_path, _history = _paths(root)
    lease = _load_active(root)
    if not lease:
        print("no active lease")
        return 0
    if args.run_id and lease.get("driver_run_id") != args.run_id:
        print("run-id does not own active lease", file=sys.stderr)
        return 2
    _record(root, "release", lease)
    active_path.unlink(missing_ok=True)
    print("released")
    return 0


def complete(args: argparse.Namespace) -> int:
    root = common.repo_root()
    active_path, _history = _paths(root)
    lease = _load_active(root)
    if not lease:
        print("no active lease", file=sys.stderr)
        return 1
    if args.run_id and lease.get("driver_run_id") != args.run_id:
        print("run-id does not own active lease", file=sys.stderr)
        return 2
    completed = dict(lease)
    completed["completed_at_ns"] = common.now_ns()
    _record(root, "complete", completed)
    active_path.unlink(missing_ok=True)
    print(json.dumps(completed, indent=2, sort_keys=True))
    return 0


def force_release(args: argparse.Namespace) -> int:
    if not args.reason or len(args.reason.strip()) < 12:
        print("force-release requires --reason with at least 12 chars", file=sys.stderr)
        return 2
    root = common.repo_root()
    active_path, _history = _paths(root)
    lease = _load_active(root)
    if not lease:
        print("no active lease")
        return 0
    _record(root, "force-release", lease, args.reason)
    active_path.unlink(missing_ok=True)
    print("force-released")
    return 0


def status(_args: argparse.Namespace) -> int:
    root = common.repo_root()
    lease = _load_active(root)
    now = common.now_ns()
    if not lease:
        print(json.dumps({"active": False}, indent=2, sort_keys=True))
        return 0
    out = dict(lease)
    out["active"] = not _is_expired(lease, now)
    out["expired"] = _is_expired(lease, now)
    print(json.dumps(out, indent=2, sort_keys=True))
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("acquire")
    p.add_argument("--todo", required=True)
    p.add_argument("--section", required=True)
    p.add_argument("--driver", required=True)
    p.add_argument("--run-id", required=True)
    p.add_argument("--ttl-sec", type=int, default=DEFAULT_TTL_SEC)
    p.add_argument("--mutation", action="append")
    p.set_defaults(func=acquire)

    p = sub.add_parser("renew")
    p.add_argument("--run-id")
    p.add_argument("--ttl-sec", type=int, default=DEFAULT_TTL_SEC)
    p.set_defaults(func=renew)

    p = sub.add_parser("release")
    p.add_argument("--run-id")
    p.set_defaults(func=release)

    p = sub.add_parser("complete")
    p.add_argument("--run-id")
    p.set_defaults(func=complete)

    p = sub.add_parser("force-release")
    p.add_argument("--reason", required=True)
    p.set_defaults(func=force_release)

    p = sub.add_parser("status")
    p.set_defaults(func=status)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
