#!/usr/bin/env python3
"""Generate TODO stamps from ledger evidence."""
from __future__ import annotations

import argparse
import hashlib
import os
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


_STAMP_LABEL_RE = re.compile(
    r"^>\s*\*\*(Verified|Accepted|Deferred|Quality reviewed|Validated|Gap-audited):\*\*"
)

# Any bold blockquote label, e.g. `> **Notes:**`, `> **Re-reviewed:**`. Used to keep
# the stamp-group sorter from absorbing an INDEPENDENT blockquote as a stamp's
# continuation text (which would move it during the re-sort).
_BQ_LABEL_RE = re.compile(r"^>\s*\*\*[^*]+:\*\*")

# Canonical section-stamp order (review-todo-section step 16). A new stamp joins
# the group at its ranked position so `Quality reviewed` can never precede
# `Verified`, regardless of the order callers write them.
_STAMP_ORDER = {"Verified": 0, "Accepted": 1, "Deferred": 2, "Quality reviewed": 3}


def _stamp_rank(line: str) -> int:
    m = _STAMP_LABEL_RE.match(line)
    if not m:
        return 99
    return _STAMP_ORDER.get(m.group(1), 50)


def _section_bounds(lines: list[str], section: str) -> tuple[int, int]:
    pat = _section_heading(section)
    start = next((i for i, line in enumerate(lines) if pat.match(line)), None)
    if start is None:
        raise ValueError(f"section heading not found: {section}")
    end = len(lines)
    for j in range(start + 1, len(lines)):
        # A numbered section ends at the next H2 heading (numbered or named, e.g.
        # `## OS Comparison`) or a thematic break -- NOT at an H3/H4 subsection.
        if re.match(r"^##\s", lines[j]) or lines[j].strip() == "---":
            end = j
            break
    return start, end


def _insert_stamp(text: str, section: str, stamp_line: str) -> str:
    """Insert a section-local stamp in the canonical bottom stamp block.

    The roadmap style keeps stamps at the section's tail: after the Test
    checkpoint / Test runner / Notes, with Verified / Accepted / Deferred /
    Quality reviewed grouped contiguously. Join an existing stamp group when
    present; otherwise place the stamp after the trailing Notes/Test-runner
    blockquote (with the `>` quoted-blank separator the style uses); otherwise
    append after the section body. Never abut the heading.
    """
    lines = text.splitlines()
    start, end = _section_bounds(lines, section)

    stamp_idx = [i for i in range(start + 1, end) if _STAMP_LABEL_RE.match(lines[i])]
    if stamp_idx:
        # Split the group into per-stamp units (label line + its continuation `>`
        # lines) and stable-sort the whole group plus the new stamp by canonical
        # rank. Sorting (not insert-before-first-greater) guarantees the output is
        # ordered even when the existing group was already out of order.
        units: list[list[str]] = []
        last_end = stamp_idx[0]
        for pos, i in enumerate(stamp_idx):
            nxt = stamp_idx[pos + 1] if pos + 1 < len(stamp_idx) else end
            j = i + 1
            while j < nxt and lines[j].startswith(">") and not _BQ_LABEL_RE.match(lines[j]):
                j += 1
            units.append(lines[i:j])
            last_end = j
        block_lo = stamp_idx[0]
        if sum(len(u) for u in units) == last_end - block_lo:
            # Contiguous stamp block (no gaps): stable-sort keeps equal-rank stamps
            # in place while forcing canonical rank order over the whole group.
            ordered = sorted(units + [[stamp_line]], key=lambda blk: _stamp_rank(blk[0]))
            lines[block_lo:last_end] = [ln for blk in ordered for ln in blk]
            return "\n".join(lines) + "\n"
        # Interleaved/non-contiguous stamps: fall back to a ranked single insert.
        after = next(
            (i for i in stamp_idx if _stamp_rank(lines[i]) > _stamp_rank(stamp_line)), None
        )
        lines.insert(after if after is not None else last_end, stamp_line)
        return "\n".join(lines) + "\n"

    k = end
    while k > start + 1 and not lines[k - 1].strip():
        k -= 1
    if k > start + 1 and lines[k - 1].startswith(">"):
        lines.insert(k, ">")
        lines.insert(k + 1, stamp_line)
    else:
        if k > start + 1 and lines[k - 1].strip():
            lines.insert(k, "")
            k += 1
        lines.insert(k, stamp_line)
    return "\n".join(lines) + "\n"


FILE_LEVEL_KINDS = {"validated", "gap-audited"}
FILE_LEVEL_REQUIRED = {"validated": "todo-graph-validate", "gap-audited": "gap-audit"}


def _file_evidence_ids(root: Path, todo: str, kind: str) -> list[str]:
    """Event IDs of non-legacy ledger events of `kind` recorded for `todo`.

    A `validated`/`gap-audited` file-level stamp must be ledger-backed (the
    section Outcome), so it needs a real `todo-graph-validate` / `gap-audit`
    event. Blob-currency is intentionally NOT required here: the stamp write
    mutates the TODO it validates, so a blob-match rule would self-invalidate.
    Evidence is NOT filtered on section: the real file-level producers record
    with an empty section (the gap-audit receipt via codex_review_completed.py)
    or an explicit `file` (the validate workflow), so a section filter would
    drop valid receipts. File-scoping the producers plus index-backed lookup
    (instead of this full ledger scan) is tracked by the shared-gate-library
    evidence-integrity follow-up in TODO-10 (shared gates across hooks).
    """
    ledger = common.state_dir(root) / "evidence.jsonl"
    ids: list[str] = []
    for ev in common.iter_jsonl(ledger):
        if ev.get("todo_path") != todo or ev.get("kind") != kind or ev.get("legacy_import"):
            continue
        if ev.get("result") not in ("ok", "received", "", None):
            continue
        if ev.get("event_id"):
            ids.append(ev["event_id"])
    return ids


def _first_section_index(lines: list[str]) -> int:
    sec = re.compile(r"^##+\s+\d+\.\s+")
    for idx, line in enumerate(lines):
        if sec.match(line):
            return idx
    return len(lines)


def _insert_preamble_stamp(text: str, stamp_line: str, label: str) -> str:
    """Insert a file-level lifecycle stamp into the preamble.

    validated / gap-audited stamps are file-level: `sequencer_triage.file_lifecycle`
    only recognizes them BEFORE the first `## N.` heading. Replace-in-place when a
    same-label preamble stamp exists (idempotent re-stamp); otherwise insert after
    the last existing lifecycle stamp, else just above `> **Goal:**`, else right
    after the H1. Hard-fail when no H1 exists.
    """
    lines = text.splitlines()
    h1 = next((i for i, ln in enumerate(lines) if re.match(r"^#\s+\S", ln)), None)
    if h1 is None:
        raise ValueError("no H1 heading found for file-level lifecycle stamp")
    end = _first_section_index(lines)
    label_re = re.compile(rf"^>\s*\*\*{re.escape(label)}:\*\*")
    for i in range(h1 + 1, end):
        if label_re.match(lines[i]):
            lines[i] = stamp_line
            return "\n".join(lines) + "\n"
    lifecycle_re = re.compile(r"^>\s*\*\*(Validated|Gap-audited|Re-scoped):\*\*")
    goal_re = re.compile(r"^>\s*\*\*Goal:\*\*")
    last_life = None
    goal_idx = None
    for i in range(h1 + 1, end):
        if lifecycle_re.match(lines[i]):
            last_life = i
        elif goal_idx is None and goal_re.match(lines[i]):
            goal_idx = i
    if last_life is not None:
        block, anchor = ["", stamp_line], last_life + 1
    elif goal_idx is not None:
        block, anchor = [stamp_line, ""], goal_idx
    else:
        block, anchor = ["", stamp_line], h1 + 1
    for off, content in enumerate(block):
        lines.insert(anchor + off, content)
    return "\n".join(lines) + "\n"


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
    ap.add_argument("--section")
    ap.add_argument("--driver-run-id")
    ap.add_argument("--summary", default="")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--explain-missing", action="store_true")
    ap.add_argument("--allow-missing", action="store_true")
    args = ap.parse_args(argv)

    root = common.repo_root()
    todo = common.rel_path(args.todo, root)
    is_file_level = args.stamp_kind in FILE_LEVEL_KINDS

    if not is_file_level and not args.section:
        return common.die(f"--section is required for {args.stamp_kind} stamps", 2)

    if args.stamp_kind in ("accepted", "deferred"):
        bare = common.bare_xrefs(args.summary)
        if bare:
            return common.die(
                "non-canonical XREF (writer requires the graph-consumable clause "
                '`-> XREF: NN-domain/TODO-XX §N (item: "..." at line N)` -- arrow '
                "prefix, section immediately after the target, item parenthetical "
                "before any , ; ]): " + " | ".join(bare),
                2,
            )
        if not common.has_concrete_todo_xref(args.summary):
            return common.die(
                f"{args.stamp_kind} stamp requires a canonical XREF naming a TODO owner "
                '(`... -> XREF: NN-domain/TODO-XX §N (item: "..." at line N)`, section '
                "immediately after the target)",
                2,
            )

    if is_file_level:
        req_kind = FILE_LEVEL_REQUIRED[args.stamp_kind]
        evidence_ids = _file_evidence_ids(root, todo, req_kind)
        missing: list[str] = []
        section_evidence = "file"
        if not evidence_ids and not args.allow_missing:
            return common.die(
                f"missing {req_kind} evidence for the {args.stamp_kind} stamp; run the "
                "workflow that records it, or pass --allow-missing for a manual stamp",
                2,
            )
    else:
        _ok, evidence_ids, missing = _required_evidence(todo, args.section, args.driver_run_id)
        section_evidence = str(args.section)

    if args.explain_missing:
        print("missing evidence: " + (", ".join(missing) if missing else "none"))
        if not args.write and not args.dry_run:
            return 0
    if args.stamp_kind in ("verified", "quality-reviewed") and missing and not args.allow_missing:
        return common.die("missing evidence: " + ", ".join(missing), 2)

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
    try:
        if is_file_level:
            new_text = _insert_preamble_stamp(text, stamp_line, label)
        else:
            new_text = _insert_stamp(text, args.section, stamp_line)
    except ValueError as exc:
        return common.die(str(exc), 2)

    stamp_hash = hashlib.sha256(stamp_line.encode("utf-8")).hexdigest()
    run_id = args.driver_run_id or f"stamp-{common.now_ns()}"

    def _record_args(kind: str, extra_meta: list[str]) -> list[str]:
        rec = [
            "record",
            "--todo", todo,
            "--section", section_evidence,
            "--role", "stamp-writer",
            "--backend", "ai-workflow",
            "--run-id", run_id,
            "--kind", kind,
            "--result", "ok",
            "--stamp-text", stamp_line,
        ]
        for meta in extra_meta + [f"stamp_label={label}", f"stamp_text_sha256={stamp_hash}"]:
            rec.extend(["--metadata", meta])
        post_write_blob = common.worktree_blob_or_digest(todo, root)
        if post_write_blob:
            rec.extend(["--source-blob", f"{todo}={post_write_blob}"])
        for eid in evidence_ids:
            rec.extend(["--source-evidence-id", eid])
        return rec

    def _atomic_write(content: str) -> None:
        tmp = path.with_name(f"{path.name}.{os.getpid()}.stamp.tmp")
        tmp.write_text(content, encoding="utf-8")
        os.replace(tmp, path)

    def _rolled_back(reason: str) -> int:
        # rollback is atomic too (tmp + os.replace): the TODO is either restored
        # or untouched-with-stamp, never truncated mid-restore
        try:
            _atomic_write(text)
        except OSError as exc:
            return common.die(
                f"{reason}; ROLLBACK FAILED ({exc}) -- {todo} may retain a stamp "
                f"with no backing stamp.generated evidence",
                2,
            )
        return common.die(f"{reason}; stamp write rolled back", 2)

    # Atomicity: the TODO write and both ledger appends run in ONE workflow_lock
    # critical section (the nested evidence.append_event lock re-enters via the
    # process-local depth counter). If either append fails -- broken ledger, lock
    # contention resolved against us, any exception -- the TODO text is rolled back,
    # so a stamp can never exist without its backing stamp.generated evidence.
    try:
        with common.workflow_lock(root):
            try:
                _atomic_write(new_text)
            except OSError as exc:
                return common.die(f"failed to write stamp to {todo} ({exc})", 2)
            try:
                first_rc = evidence.main(
                    _record_args("stamp.generated", [f"stamp_kind={args.stamp_kind}"])
                )
                second_rc = (
                    evidence.main(_record_args(f"stamp.{args.stamp_kind}", []))
                    if first_rc == 0
                    else first_rc
                )
            except BaseException as exc:
                return _rolled_back(f"stamp evidence recording failed ({exc})")
            if first_rc or second_rc:
                return _rolled_back("stamp evidence recording failed")
    except (RuntimeError, OSError) as exc:
        return common.die(
            f"workflow lock unavailable ({exc}); refusing to write a stamp that would "
            f"lack backing stamp.generated evidence",
            2,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
