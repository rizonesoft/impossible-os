#!/usr/bin/env python3
"""Tool-neutral evidence ledger for AI workflow gates."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Any

TARGET_QUERY_SCAN_RECORD_LIMIT = 2048

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from ai_workflow_import import common  # type: ignore
else:
    from . import common


def ledger_path(root: Path | None = None) -> Path:
    return common.ensure_state_dir(root or common.repo_root()) / "evidence.jsonl"


def index_marker_path(root: Path | None = None) -> Path:
    return common.ensure_state_dir(root or common.repo_root()) / "evidence-index" / "VERSION"


def target_index_path(root: Path, todo: str, section: str) -> Path:
    key = f"{common.rel_path(todo, root)}#{section}"
    name = hashlib.sha256(key.encode("utf-8")).hexdigest()[:24] + ".jsonl"
    return common.ensure_state_dir(root) / "evidence-index" / name


def _append_target_index_event(root: Path, payload: dict[str, Any]) -> bool:
    todo = str(payload.get("todo_path") or "")
    section = str(payload.get("section") or "")
    if not todo or not section:
        return False
    marker = index_marker_path(root)
    marker.parent.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        marker.write_text("1\n", encoding="utf-8")
    common.append_jsonl(target_index_path(root, todo, section), payload)
    return True


def load_events(root: Path | None = None) -> list[dict[str, Any]]:
    return common.iter_jsonl(ledger_path(root))


def _parse_metadata(items: list[str] | None) -> dict[str, str]:
    meta: dict[str, str] = {}
    for item in items or []:
        if "=" in item:
            k, v = item.split("=", 1)
            meta[k] = v
    return meta


def _parse_source_blobs(items: list[str] | None) -> dict[str, str]:
    blobs: dict[str, str] = {}
    root = common.repo_root()
    for item in items or []:
        if "=" not in item:
            continue
        path, blob = item.split("=", 1)
        rel = common.rel_path(path.strip(), root)
        blob = blob.strip()
        if rel and blob:
            blobs[rel] = blob
    return blobs


def append_event(payload: dict[str, Any], root: Path | None = None) -> dict[str, Any]:
    root = root or common.repo_root()
    payload = dict(payload)
    payload.setdefault("created_at_ns", common.now_ns())
    payload.setdefault("created_at", common.now_iso())
    payload.setdefault("expires_at_ns", 0)
    payload.setdefault("legacy_import", False)
    payload.setdefault("metadata", {})
    payload["event_id"] = common.event_id(payload)
    with common.workflow_lock(root):
        common.append_jsonl(ledger_path(root), payload)
        _append_target_index_event(root, payload)
    return payload


def _matches(ev: dict[str, Any], args: argparse.Namespace) -> bool:
    for attr in ("todo", "section", "role", "backend", "run_id", "kind", "result"):
        value = getattr(args, attr, None)
        if value is None:
            continue
        key = "todo_path" if attr == "todo" else attr
        if str(ev.get(key)) != str(value):
            return False
    if getattr(args, "not_run_id", None) and ev.get("run_id") == args.not_run_id:
        return False
    return True


def _expired(ev: dict[str, Any], now_ns: int) -> bool:
    try:
        expires = int(ev.get("expires_at_ns") or 0)
    except Exception:
        return False
    return bool(expires and expires < now_ns)


def _matching_events(args: argparse.Namespace, root: Path) -> list[dict[str, Any]]:
    now_ns = common.now_ns()
    if args.todo and args.section:
        path = target_index_path(root, args.todo, str(args.section))
        events = []
        scanned = 0
        for ev in common.iter_jsonl_reverse(path):
            if scanned >= TARGET_QUERY_SCAN_RECORD_LIMIT:
                break
            scanned += 1
            if _expired(ev, now_ns) or not _matches(ev, args):
                continue
            events.append(ev)
        events.reverse()
        return events
    return [ev for ev in load_events(root) if not _expired(ev, now_ns) and _matches(ev, args)]


def record_event(args: argparse.Namespace) -> int:
    root = common.repo_root()
    # Reviewer evidence is RECEIPT-ONLY: the codex_review_completed receipt
    # hook mirrors received reviews into the ledger itself. Recording a
    # codex-reviewer-* role through this public CLI would let a mutating
    # session manufacture shipping review proof without any Codex dispatch.
    # AI_WORKFLOW_ALLOW_REVIEWER_RECORD=1 is the fixture/repair escape and is
    # a deliberate, logged decision. Residual cooperative boundary: direct
    # JSONL edits remain possible by design (the ledger is local derived
    # state); this guard stops the accidental/drifting-session path.
    if (
        str(args.role or "").startswith("codex-reviewer-")
        and os.environ.get("AI_WORKFLOW_ALLOW_REVIEWER_RECORD", "") != "1"
    ):
        print(
            "reviewer evidence is receipt-only: the codex_review_completed "
            "receipt hook records received reviews. Set "
            "AI_WORKFLOW_ALLOW_REVIEWER_RECORD=1 only for fixtures/repair.",
            file=sys.stderr,
        )
        return 2
    todo = common.rel_path(args.todo, root) if args.todo else ""
    sources = args.source or []
    explicit_blobs = _parse_source_blobs(args.source_blob)
    blobs = common.source_blobs(sources, root)
    blobs.update(explicit_blobs)
    metadata = _parse_metadata(args.metadata)
    if args.command:
        metadata["command"] = args.command
    if args.exit_code is not None:
        metadata["exit_code"] = str(args.exit_code)
    if args.log_path:
        metadata["log_path"] = common.rel_path(args.log_path, root)
    if args.final_marker:
        metadata["final_marker"] = args.final_marker
    if args.stamp_text:
        metadata["stamp_text_sha256"] = hashlib.sha256(
            args.stamp_text.encode("utf-8")
        ).hexdigest()
    if args.source_evidence_id:
        metadata["source_evidence_ids"] = ",".join(args.source_evidence_id)
    if args.driver_run_id:
        metadata["driver_run_id"] = args.driver_run_id
    if args.review_run_id:
        metadata["review_run_id"] = args.review_run_id
    if args.review_kind:
        metadata["review_kind"] = args.review_kind
    payload: dict[str, Any] = {
        "task_id": args.task_id or (f"{todo}#{args.section}" if todo and args.section else ""),
        "todo_path": todo,
        "section": str(args.section or ""),
        "role": args.role,
        "backend": args.backend,
        "run_id": args.run_id,
        "kind": args.kind,
        "head_sha": args.head or common.head_sha(root),
        "source_blobs": blobs,
        "result": args.result,
        "summary_path": common.rel_path(args.summary_path, root) if args.summary_path else "",
        "created_at_ns": common.now_ns(),
        "created_at": common.now_iso(),
        "expires_at_ns": common.now_ns() + int(args.expires_in_sec * 1_000_000_000)
        if args.expires_in_sec
        else 0,
        "legacy_import": bool(args.legacy_import),
    }
    if metadata:
        payload["metadata"] = metadata
    payload = append_event(payload, root)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


def query(args: argparse.Namespace) -> int:
    root = common.repo_root()
    events = _matching_events(args, root)
    if args.source_current:
        filtered = []
        for ev in events:
            blobs = ev.get("source_blobs") or {}
            if not isinstance(blobs, dict):
                continue
            ok = True
            for path, old_blob in blobs.items():
                if not common.source_binding_matches(old_blob, common.git_blob_or_digest(path, root)):
                    ok = False
                    break
            if ok:
                filtered.append(ev)
        events = filtered
    if args.latest and events:
        events = [events[-1]]
    if args.format == "ids":
        for ev in events:
            print(ev.get("event_id", ""))
    else:
        print(json.dumps(events, indent=2, sort_keys=True))
    return 0 if events or not args.require else 1


def explain(args: argparse.Namespace) -> int:
    events = _matching_events(args, common.repo_root())
    if not events:
        print("No evidence found.")
        return 1 if args.require else 0
    for ev in events:
        print(
            f"{ev.get('event_id')} {ev.get('kind')} {ev.get('role')} "
            f"{ev.get('result')} run={ev.get('run_id')} head={str(ev.get('head_sha'))[:12]}"
        )
        blobs = ev.get("source_blobs") or {}
        if isinstance(blobs, dict) and blobs:
            print("  sources: " + ", ".join(sorted(blobs)))
    return 0


def _detect_kind_from_text(text: str) -> str:
    import re

    m = re.search(
        r"\[\s*review[-_ ]kind\s*:\s*"
        r"(design|adversarial-impl|adversarial|consistency|perf|"
        r"test-coverage|gap-audit|re-adversarial)\s*\]",
        text or "",
        re.IGNORECASE,
    )
    return m.group(1).lower() if m else ""


def _detect_todo_section_from_text(text: str) -> tuple[str, str]:
    import re

    tm = re.search(r"(?<![\w/-])(todo/[\w./-]+TODO-\d[\w./-]*\.md)", text or "")
    sm = re.search(r"(?:\u00a7\s*|section\s+)(\d+)", text or "", re.IGNORECASE)
    todo = tm.group(1).rstrip(").,;:") if tm else ""
    section = sm.group(1) if sm else ""
    return todo, section


def _import_legacy_history(root: Path) -> int:
    history = root / ".claude" / "state" / "codex-review-history.jsonl"
    count = 0
    for item in common.iter_jsonl(history):
        if item.get("received") is not True:
            continue
        trigger = str(item.get("trigger") or "")
        kind = str(item.get("review_kind") or "") or _detect_kind_from_text(trigger)
        todo = str(item.get("todo_path") or "")
        section = str(item.get("section") or "").lstrip("\u00a7")
        if not todo:
            todo, detected_section = _detect_todo_section_from_text(trigger)
            section = section or detected_section
        if kind not in common.REVIEW_KINDS or not todo:
            continue
        received_ts = item.get("received_timestamp_ns") or item.get("timestamp_ns") or 0
        trigger_blobs = item.get("trigger_blobs")
        has_source_binding = isinstance(trigger_blobs, dict) and bool(trigger_blobs)
        review_run_id = str(item.get("review_run_id") or "")
        result = "received" if has_source_binding and review_run_id else "telemetry"
        role = f"codex-reviewer-{kind}" if result == "received" else "legacy-review-history-importer"
        payload = {
            "task_id": f"{todo}#{section}",
            "todo_path": todo,
            "section": section,
            "role": role,
            "backend": "codex",
            "run_id": review_run_id or f"legacy-history-{kind}-{received_ts}",
            "kind": kind,
            "head_sha": item.get("head_sha", ""),
            "source_blobs": trigger_blobs if has_source_binding else {},
            "result": result,
            "summary_path": "",
            "created_at_ns": int(received_ts) if isinstance(received_ts, int) else 0,
            "created_at": "",
            "expires_at_ns": 0,
            "legacy_import": True,
            "metadata": {
                "source": "codex-review-history",
                "compatibility_result": result,
            },
        }
        append_event(payload, root)
        count += 1
    return count


def _import_legacy_todo_stamps(root: Path) -> int:
    import re

    stamp_re = re.compile(
        r"^\s*>\s+\*\*(Verified|Quality reviewed|Deferred|Accepted|Validated|Gap-audited):\*\*",
        re.IGNORECASE,
    )
    kind_by_label = {
        "verified": "stamp.verified",
        "quality reviewed": "stamp.quality-reviewed",
        "deferred": "stamp.deferred",
        "accepted": "stamp.accepted",
        "validated": "stamp.validated",
        "gap-audited": "stamp.gap-audited",
    }
    section_re = re.compile(r"^(##+)\s+(\d+)\.\s+")
    count = 0
    for path in (root / "todo").glob("**/*.md"):
        rel = common.rel_path(path, root)
        section = ""
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except Exception:
            continue
        for line in lines:
            sm = section_re.match(line)
            if sm:
                section = sm.group(2)
            mm = stamp_re.match(line)
            if not mm:
                continue
            label = mm.group(1).lower()
            kind = kind_by_label.get(label)
            if not kind:
                continue
            payload = {
                "task_id": f"{rel}#{section}",
                "todo_path": rel,
                "section": section,
                "role": "legacy-stamp-importer",
                "backend": "ai-workflow",
                "run_id": f"legacy-stamp-{common.event_id({'path': rel, 'section': section, 'line': line})}",
                "kind": kind,
                "head_sha": common.head_sha(root),
                "source_blobs": {rel: common.git_blob_or_digest(rel, root)},
                "result": "ok",
                "summary_path": "",
                "created_at_ns": 0,
                "created_at": "",
                "expires_at_ns": 0,
                "legacy_import": True,
                "metadata": {
                    "source": "todo-stamp",
                    "stamp_text_sha256": hashlib.sha256(line.encode("utf-8")).hexdigest(),
                },
            }
            append_event(payload, root)
            count += 1
    return count


def import_legacy(_args: argparse.Namespace) -> int:
    """Best-effort import of current Claude runtime state into compatibility events."""
    root = common.repo_root()
    state = root / ".claude" / "state"
    stamps = common.read_json(state / "last-review-stamps.json", {})
    count = 0
    if isinstance(stamps, dict):
        for todo, data in stamps.items():
            if not isinstance(data, dict):
                continue
            section = str(data.get("section") or "")
            for kind in common.REVIEW_KINDS:
                ts = data.get(kind)
                if not ts:
                    continue
                payload = {
                    "task_id": f"{todo}#{section}",
                    "todo_path": todo,
                    "section": section,
                    "role": "legacy-review-stamp-importer",
                    "backend": "codex",
                    "run_id": f"legacy-{kind}-{ts}",
                    "kind": kind,
                    "head_sha": data.get(f"{kind}_head", ""),
                    "source_blobs": {},
                    "result": "telemetry",
                    "summary_path": "",
                    "created_at_ns": int(ts),
                    "created_at": "",
                    "expires_at_ns": 0,
                    "legacy_import": True,
                    "metadata": {"source": "last-review-stamps"},
                }
                append_event(payload, root)
                count += 1
    count += _import_legacy_history(root)
    count += _import_legacy_todo_stamps(root)
    print(f"imported {count} legacy evidence event(s)")
    return 0


def gc(args: argparse.Namespace) -> int:
    root = common.repo_root()
    path = ledger_path(root)
    with common.workflow_lock(root):
        now = common.now_ns()
        kept = []
        removed = 0
        for ev in load_events(root):
            expires = int(ev.get("expires_at_ns") or 0)
            if expires and expires < now:
                removed += 1
            else:
                kept.append(ev)
        tmp = path.with_name(f"{path.name}.{os.getpid()}.{common.now_ns()}.tmp")
        tmp.write_text("".join(json.dumps(ev, sort_keys=True) + "\n" for ev in kept), encoding="utf-8")
        tmp.replace(path)
        _rebuild_target_indexes(root, kept)
    print(f"removed {removed} expired evidence event(s)")
    return 0


def _rebuild_target_indexes(root: Path, events: list[dict[str, Any]]) -> int:
    index_dir = common.ensure_state_dir(root) / "evidence-index"
    if index_dir.exists():
        for child in index_dir.iterdir():
            if child.is_file() and (child.name == "VERSION" or child.suffix == ".jsonl"):
                child.unlink()
    else:
        index_dir.mkdir(parents=True, exist_ok=True)
    indexed = 0
    for ev in events:
        if _append_target_index_event(root, ev):
            indexed += 1
    marker = index_marker_path(root)
    marker.parent.mkdir(parents=True, exist_ok=True)
    if not marker.exists():
        marker.write_text("1\n", encoding="utf-8")
    return indexed


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("record")
    p.add_argument("--task-id")
    p.add_argument("--todo")
    p.add_argument("--section")
    p.add_argument("--role", required=True)
    p.add_argument("--backend", required=True)
    p.add_argument("--run-id", required=True)
    p.add_argument("--kind", required=True)
    p.add_argument("--result", default="ok")
    p.add_argument("--source", action="append")
    p.add_argument("--source-blob", action="append",
                   help="Explicit source binding as path=blob_sha")
    p.add_argument("--summary-path")
    p.add_argument("--head")
    p.add_argument("--expires-in-sec", type=int, default=0)
    p.add_argument("--legacy-import", action="store_true")
    p.add_argument("--metadata", action="append")
    p.add_argument("--command")
    p.add_argument("--exit-code", type=int)
    p.add_argument("--log-path")
    p.add_argument("--final-marker")
    p.add_argument("--stamp-text")
    p.add_argument("--source-evidence-id", action="append")
    p.add_argument("--driver-run-id")
    p.add_argument("--review-run-id")
    p.add_argument("--review-kind")
    p.set_defaults(func=record_event)

    for name, func in (("query", query), ("explain", explain)):
        p = sub.add_parser(name)
        p.add_argument("--todo")
        p.add_argument("--section")
        p.add_argument("--role")
        p.add_argument("--backend")
        p.add_argument("--run-id")
        p.add_argument("--not-run-id")
        p.add_argument("--kind")
        p.add_argument("--result")
        p.add_argument("--latest", action="store_true")
        p.add_argument("--source-current", action="store_true")
        p.add_argument("--require", action="store_true")
        p.add_argument("--format", choices=("json", "ids"), default="json")
        p.set_defaults(func=func)

    p = sub.add_parser("import-legacy")
    p.set_defaults(func=import_legacy)

    p = sub.add_parser("gc")
    p.set_defaults(func=gc)
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = build_parser()
    args = ap.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
