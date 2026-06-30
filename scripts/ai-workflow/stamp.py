#!/usr/bin/env python3
"""Generate TODO stamps from ledger evidence."""
from __future__ import annotations

import argparse
import hashlib
import re
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
        _EV_PATH = Path(__file__).resolve().parent / "evidence.py"
        _ev_spec = importlib.util.spec_from_file_location("ai_workflow_evidence", _EV_PATH)
        assert _ev_spec and _ev_spec.loader
        evidence = importlib.util.module_from_spec(_ev_spec)
        _ev_spec.loader.exec_module(evidence)
    else:
        from . import evidence, obligations
except Exception as exc:  # pragma: no cover
    raise SystemExit(f"failed to load workflow helpers: {exc}")


STAMP_KIND = {
    "verified": "Verified",
    "quality-reviewed": "Quality reviewed",
    "deferred": "Deferred",
    "accepted": "Accepted",
    "validated": "Validated",
    "gap-audited": "Gap-audited",
}


def _section_heading(section: str) -> re.Pattern[str]:
    return re.compile(rf"^(##+)\s+{re.escape(str(section))}\.\s+")


def _insert_stamp(text: str, section: str, stamp_line: str) -> str:
    lines = text.splitlines()
    pat = _section_heading(section)
    for idx, line in enumerate(lines):
        if pat.match(line):
            insert_at = idx + 1
            while insert_at < len(lines) and lines[insert_at].startswith("> **"):
                insert_at += 1
            if insert_at < len(lines) and lines[insert_at].strip():
                lines.insert(insert_at, "")
            lines.insert(insert_at, stamp_line)
            return "\n".join(lines) + "\n"
    raise ValueError(f"section heading not found: {section}")


def _required_evidence(todo: str, section: str, driver_run_id: str | None) -> tuple[bool, list[str], list[str]]:
    class _Args:
        pass

    a = _Args()
    a.todo = todo
    a.section = section
    a.workflow = "implement"
    a.driver_run_id = driver_run_id
    out = obligations.resolve(a)  # type: ignore[arg-type]
    missing = [item["name"] for item in out["required"] if item["status"] == "missing" and item["name"] != "verified-stamp"]
    ids: list[str] = []
    for item in out["required"]:
        if item["status"] == "satisfied":
            ids.extend(item["evidence_ids"])
    return (not missing, ids, missing)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("stamp_kind", choices=sorted(STAMP_KIND))
    ap.add_argument("todo")
    ap.add_argument("--section", required=True)
    ap.add_argument("--driver-run-id")
    ap.add_argument("--summary", default="")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--explain-missing", action="store_true")
    ap.add_argument("--allow-missing", action="store_true")
    args = ap.parse_args(argv)

    root = common.repo_root()
    todo = common.rel_path(args.todo, root)
    ok, evidence_ids, missing = _required_evidence(todo, args.section, args.driver_run_id)
    if args.explain_missing:
        if missing:
            print("missing evidence: " + ", ".join(missing))
        else:
            print("missing evidence: none")
        if not args.write and not args.dry_run:
            return 0
    if args.stamp_kind in ("verified", "quality-reviewed") and missing and not args.allow_missing:
        print("missing evidence: " + ", ".join(missing), file=sys.stderr)
        return 2

    label = STAMP_KIND[args.stamp_kind]
    evidence_head = ",".join(evidence_ids[:4]) if evidence_ids else "manual"
    summary = f" | {args.summary}" if args.summary else ""
    stamp_line = (
        f"> **{label}:** {common.today_iso()} | ai-workflow evidence "
        f"{evidence_head}{summary}"
    )

    path = root / todo
    if args.dry_run or not args.write:
        print(stamp_line)
        return 0

    text = common.load_text(path)
    new_text = _insert_stamp(text, args.section, stamp_line)
    path.write_text(new_text, encoding="utf-8")

    stamp_hash = hashlib.sha256(stamp_line.encode("utf-8")).hexdigest()
    post_write_blob = common.worktree_blob_or_digest(todo, root)
    source_blob = f"{todo}={post_write_blob}" if post_write_blob else ""
    generated_args = [
        "record",
        "--todo",
        todo,
        "--section",
        str(args.section),
        "--role",
        "stamp-writer",
        "--backend",
        "ai-workflow",
        "--run-id",
        args.driver_run_id or f"stamp-{common.now_ns()}",
        "--kind",
        "stamp.generated",
        "--result",
        "ok",
        "--stamp-text",
        stamp_line,
        "--metadata",
        f"stamp_kind={args.stamp_kind}",
        "--metadata",
        f"stamp_label={label}",
        "--metadata",
        f"stamp_text_sha256={stamp_hash}",
    ]
    if source_blob:
        generated_args.extend(["--source-blob", source_blob])
    for eid in evidence_ids:
        generated_args.extend(["--source-evidence-id", eid])
    first_rc = evidence.main(generated_args)

    payload_args = [
        "record",
        "--todo",
        todo,
        "--section",
        str(args.section),
        "--role",
        "stamp-writer",
        "--backend",
        "ai-workflow",
        "--run-id",
        args.driver_run_id or f"stamp-{common.now_ns()}",
        "--kind",
        f"stamp.{args.stamp_kind}",
        "--result",
        "ok",
        "--stamp-text",
        stamp_line,
        "--metadata",
        f"stamp_label={label}",
        "--metadata",
        f"stamp_text_sha256={stamp_hash}",
    ]
    if source_blob:
        payload_args.extend(["--source-blob", source_blob])
    for eid in evidence_ids:
        payload_args.extend(["--source-evidence-id", eid])
    second_rc = evidence.main(payload_args)
    return first_rc or second_rc


if __name__ == "__main__":
    raise SystemExit(main())
