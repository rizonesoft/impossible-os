#!/usr/bin/env python3
"""Resolve missing AI workflow obligations from shared evidence."""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from ai_workflow_import import common  # type: ignore
else:
    from . import common

try:
    if __package__ in (None, ""):
        import importlib.util

        _EV_PATH = Path(__file__).resolve().parent / "evidence.py"
        _spec = importlib.util.spec_from_file_location("ai_workflow_evidence", _EV_PATH)
        assert _spec and _spec.loader
        evidence = importlib.util.module_from_spec(_spec)
        _spec.loader.exec_module(evidence)
    else:
        from . import evidence
except Exception as exc:  # pragma: no cover - import failure is fatal
    raise SystemExit(f"failed to load evidence helper: {exc}")


RESOLVE_CANDIDATE_WINDOW_SIZE = 128
RESOLVE_TARGET_SCAN_RECORD_LIMIT = RESOLVE_CANDIDATE_WINDOW_SIZE * 4


def _expired(ev: dict[str, Any], now_ns: int) -> bool:
    try:
        expires = int(ev.get("expires_at_ns") or 0)
    except Exception:
        return False
    return bool(expires and expires < now_ns)


def _current(
    ev: dict[str, Any],
    root: Path,
    blob_cache: dict[str, str],
    exempt_path: str = "",
) -> bool:
    """Blob currency for an event's source bindings.

    `exempt_path` is the resolution's OWN TODO file: the workflow mutates it
    progressively (item flips, sequential stamps), so binding evidence to its
    blob would make each stamp invalidate the previous one and the reviews --
    the pilot hit exactly that self-invalidation. Source-file bindings (code,
    scripts) stay strict."""
    blobs = ev.get("source_blobs") or {}
    if not isinstance(blobs, dict):
        return True
    rel_paths = [
        rel for rel in (common.rel_path(path, root) for path in blobs)
        if rel != exempt_path
    ]
    missing = [rel for rel in rel_paths if rel not in blob_cache]
    if missing:
        blob_cache.update(common.git_blobs_or_digests(missing, root))
    for path, old_blob in blobs.items():
        rel = common.rel_path(path, root)
        if rel == exempt_path:
            continue
        if not common.source_binding_matches(old_blob, blob_cache[rel]):
            return False
    return True


def _event_ok(
    ev: dict[str, Any],
    *,
    kind: str,
    result: str | None = None,
    role_prefix: str | None = None,
    not_run_id: str | None = None,
    current_only: bool = True,
    allow_legacy: bool = True,
    require_source_blobs: bool = False,
    require_meta: dict[str, str] | None = None,
    require_meta_keys: tuple[str, ...] = (),
    root: Path,
    blob_cache: dict[str, str],
    check_current: bool = True,
    exempt_path: str = "",
) -> bool:
    if ev.get("kind") != kind:
        return False
    if result and ev.get("result") != result:
        return False
    if role_prefix and not str(ev.get("role", "")).startswith(role_prefix):
        return False
    if not_run_id:
        metadata = ev.get("metadata") or {}
        metadata_run = metadata.get("driver_run_id") if isinstance(metadata, dict) else ""
        if ev.get("run_id") == not_run_id or metadata_run == not_run_id:
            return False
    if not allow_legacy and ev.get("legacy_import"):
        return False
    blobs = ev.get("source_blobs")
    if require_source_blobs and (not isinstance(blobs, dict) or not blobs):
        return False
    if require_meta or require_meta_keys:
        # provenance fields (exit code, log path, final marker) must be present
        # in the event metadata -- a forged/partial record cannot satisfy
        md = ev.get("metadata") or {}
        if not isinstance(md, dict):
            return False
        for key, wanted in (require_meta or {}).items():
            if str(md.get(key)) != wanted:
                return False
        for key in require_meta_keys:
            if not md.get(key):
                return False
        # bind the claim to the artifact: the named log must exist NOW and its
        # tail must carry the recorded final marker. Cooperative trust boundary:
        # a caller could still fabricate the log; the full run-wrapper binding
        # is owned by the review-evidence-integrity work in the AI-workflow TODO.
        if "log_path" in require_meta_keys and "final_marker" in require_meta_keys:
            log_path = root / str(md.get("log_path"))
            try:
                with log_path.open("rb") as f:
                    f.seek(0, 2)
                    f.seek(max(0, f.tell() - 65536))
                    tail = f.read().decode("utf-8", errors="replace")
            except OSError:
                return False
            last_lines = [ln for ln in tail.splitlines() if ln.strip()]
            # the sentinel must be the FINAL state of the log, not merely
            # present somewhere in the tail (a stale OK followed by failure
            # output must not satisfy)
            if not last_lines or last_lines[-1].strip() != str(md.get("final_marker")).strip():
                return False
    if exempt_path and str(ev.get("kind", "")).startswith("stamp."):
        # stamp evidence is exempt from TODO blob currency (later stamps and
        # item flips legitimately mutate the file), but the stamp LINE itself
        # must be present in the STAGED content -- a stamp written to the
        # worktree and never staged is not commit-ready proof
        md2 = ev.get("metadata") or {}
        want = md2.get("stamp_text_sha256") if isinstance(md2, dict) else ""
        if want and not _staged_line_sha_present(root, exempt_path, str(want)):
            return False
    if current_only and check_current and not _current(ev, root, blob_cache, exempt_path):
        return False
    return True


def _staged_line_sha_present(root: Path, todo: str, want_sha: str) -> bool:
    try:
        text = subprocess.check_output(
            ["git", "show", f":{todo}"],
            cwd=str(root), text=True, stderr=subprocess.DEVNULL, timeout=5,
        )
    except Exception:
        try:
            text = (root / todo).read_text(encoding="utf-8", errors="replace")
        except OSError:
            return False
    return any(
        hashlib.sha256(line.encode("utf-8")).hexdigest() == want_sha
        for line in text.splitlines()
    )


def _resolve_events(
    todo: str,
    section: str,
    specs: list[dict[str, Any]],
    root: Path,
    diagnostics: list[str] | None = None,
) -> dict[str, list[str]]:
    found: dict[str, list[str]] = {}
    remaining = {str(spec["name"]) for spec in specs}
    candidates: list[tuple[dict[str, Any], set[str]]] = []
    candidate_names: set[str] = set()
    defer_coverage_names: set[str] = set()
    blob_cache: dict[str, str] = {}

    def resolve_candidate_window() -> set[str]:
        nonlocal candidates, candidate_names
        if not candidates or not remaining:
            candidates = []
            candidate_names = set()
            return set()
        before_remaining = set(remaining)
        covered_names = set(candidate_names)
        source_paths: set[str] = set()
        for ev, names in candidates:
            for spec in specs:
                name = str(spec["name"])
                if name not in remaining or name not in names:
                    continue
                if spec["match"].get("current_only", True):
                    blobs = ev.get("source_blobs") or {}
                    if isinstance(blobs, dict):
                        source_paths.update(
                            rel for rel in (common.rel_path(path, root) for path in blobs)
                            if rel != todo
                        )
        missing_source_paths = sorted(path for path in source_paths if path not in blob_cache)
        if missing_source_paths:
            blob_cache.update(common.git_blobs_or_digests(missing_source_paths, root))
        for ev, names in candidates:
            if not remaining:
                break
            eid = str(ev.get("event_id") or "")
            for spec in specs:
                name = str(spec["name"])
                if name not in remaining or name not in names:
                    continue
                if _event_ok(ev, root=root, blob_cache=blob_cache,
                             exempt_path=todo, **spec["match"]):
                    found[name] = [eid]
                    remaining.remove(name)
        candidates = []
        candidate_names = set()
        return before_remaining & covered_names & remaining

    index_path = evidence.target_index_path(root, todo, section)
    if not index_path.exists():
        return found
    now_ns = common.now_ns()
    events = iter(common.iter_jsonl_reverse(index_path))
    scanned = 0
    while True:
        if scanned >= RESOLVE_TARGET_SCAN_RECORD_LIMIT:
            if diagnostics is not None and remaining:
                diagnostics.append(
                    "target-index scan capped at "
                    f"{RESOLVE_TARGET_SCAN_RECORD_LIMIT} records for {todo}#{section}; "
                    "rerun missing or stale evidence"
                )
            break
        try:
            ev = next(events)
        except StopIteration:
            break
        scanned += 1
        if not remaining:
            break
        if ev.get("todo_path") != todo or str(ev.get("section")) != str(section):
            continue
        if _expired(ev, now_ns):
            continue
        eid = str(ev.get("event_id") or "")
        if not eid:
            continue
        names: set[str] = set()
        for spec in specs:
            name = str(spec["name"])
            if name not in remaining:
                continue
            if not _event_ok(
                ev,
                root=root,
                blob_cache={},
                check_current=False,
                **spec["match"],
            ):
                continue
            names.add(name)
        if names:
            candidates.append((ev, names))
            candidate_names.update(names)
            covered = remaining <= candidate_names
            if (
                len(candidates) >= RESOLVE_CANDIDATE_WINDOW_SIZE
                or (covered and not remaining <= defer_coverage_names)
            ):
                stale_names = resolve_candidate_window()
                defer_coverage_names = (defer_coverage_names | stale_names) & remaining
    if remaining:
        resolve_candidate_window()
    return found


def _active_lease(todo: str, section: str, driver_run_id: str | None, root: Path) -> tuple[bool, list[str]]:
    lease_path = common.state_dir(root) / "active-lease.json"
    lease = common.read_json(lease_path, {})
    if not isinstance(lease, dict) or not lease:
        return False, []
    if lease.get("todo_path") != todo or str(lease.get("section")) != str(section):
        return False, []
    if driver_run_id and lease.get("driver_run_id") != driver_run_id:
        return False, []
    if int(lease.get("expires_at_ns") or 0) < common.now_ns():
        return False, []
    return True, [str(lease.get("driver_run_id") or "active-lease")]


def resolve(args: argparse.Namespace) -> dict[str, Any]:
    root = common.repo_root()
    todo = common.rel_path(args.todo, root)
    section = str(args.section)
    required: list[dict[str, Any]] = []
    driver_run = args.driver_run_id
    event_specs: list[dict[str, Any]] = []
    diagnostics: list[str] = []

    def add(name: str, ok: bool, evidence_ids: list[str], next_action: str) -> None:
        required.append(
            {
                "name": name,
                "status": "satisfied" if ok else "missing",
                "evidence_ids": evidence_ids,
                "next_action": "" if ok else next_action,
            }
        )

    # implement/verify resolution REQUIRES a session identity: without one the
    # lease check degrades to presence-only and same-run reviewer evidence
    # cannot be excluded, so both fail closed instead (consistency finding)
    identity_missing = args.workflow in ("implement", "verify") and not driver_run
    if identity_missing:
        diagnostics.append(
            "no --driver-run-id supplied: driver-lease and codex-review "
            "obligations fail closed for implement/verify workflows"
        )

    ok, ids = _active_lease(todo, section, driver_run, root)
    if identity_missing:
        ok, ids = False, []
    add(
        "driver-lease",
        ok,
        ids,
        "python3 scripts/ai-workflow/lease.py acquire --todo <todo> --section <n> --driver <backend> --run-id <run> (and pass --driver-run-id here)",
    )

    if args.workflow in ("implement", "verify"):
        event_specs.append(
            {
                "name": "build",
                "match": {
                    "kind": "build",
                    "result": "ok",
                    "allow_legacy": False,
                    "require_source_blobs": True,
                    # full provenance with the sentinel PINNED: comparing the
                    # log against a caller-supplied marker would let a failed
                    # log satisfy the gate by recording its own failure line
                    "require_meta": {
                        "exit_code": "0",
                        "final_marker": "=== BUILD OK ===",
                    },
                    "require_meta_keys": ("log_path", "final_marker"),
                },
                "next_action": (
                    "bash scripts/build.sh && record build evidence with "
                    "--exit-code 0 --log-path build/build.log "
                    "--final-marker '=== BUILD OK ==='"
                ),
            }
        )

    for kind in common.REQUIRED_SHIP_REVIEW_KINDS:
        event_specs.append(
            {
                "name": f"codex-review-{kind}",
                "match": {
                    "kind": kind,
                    "result": "received",
                    "role_prefix": f"codex-reviewer-{kind}",
                    "not_run_id": driver_run,
                    "allow_legacy": False,
                    "require_source_blobs": True,
                },
                "next_action": f"bash scripts/codex-dispatch.sh '[review-kind: {kind}] {todo} section {section} ...'",
            }
        )

    if args.workflow in ("implement", "verify"):
        event_specs.append(
            {
                "name": "verified-stamp",
                "match": {
                    "kind": "stamp.verified",
                    "result": "ok",
                    "allow_legacy": False,
                    "require_source_blobs": True,
                },
                "next_action": "python3 scripts/ai-workflow/stamp.py verified <todo> --section <n> --write",
            }
        )

    event_specs.append(
        {
            "name": "todo-graph-validate",
            "match": {
                "kind": "todo-graph-validate",
                "result": "ok",
                "current_only": False,
                "allow_legacy": False,
            },
            "next_action": "bash scripts/todo-graph/build-and-validate.sh --keep-cache && record validation evidence",
        }
    )

    found = _resolve_events(todo, section, event_specs, root, diagnostics)
    if identity_missing:
        # reviewer independence cannot be established without the session
        # identity -- review evidence never satisfies an anonymous resolution
        for name in list(found):
            if name.startswith("codex-review-"):
                found.pop(name)
    for spec in event_specs:
        name = str(spec["name"])
        ids = found.get(name, [])
        add(name, bool(ids), ids, str(spec["next_action"]))

    missing = [item for item in required if item["status"] == "missing"]
    return {
        "todo_path": todo,
        "section": section,
        "workflow": args.workflow,
        "driver_run_id": driver_run or "",
        "status": "ready" if not missing else "missing",
        "required": required,
        "missing_count": len(missing),
        "diagnostics": diagnostics,
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("todo")
    ap.add_argument("--section", required=True)
    ap.add_argument("--workflow", choices=("implement", "review", "verify", "complete-file"), default="implement")
    ap.add_argument("--driver-run-id")
    ap.add_argument("--format", choices=("json", "text"), default="text")
    ap.add_argument("--require-clean", action="store_true")
    args = ap.parse_args(argv)
    out = resolve(args)
    if args.format == "json":
        print(json.dumps(out, indent=2, sort_keys=True))
    else:
        print(f"{out['todo_path']} section {out['section']} workflow={out['workflow']}: {out['status']}")
        for item in out["required"]:
            mark = "OK" if item["status"] == "satisfied" else "MISS"
            print(f"  {mark} {item['name']}")
            if item["next_action"]:
                print(f"       next: {item['next_action']}")
        for item in out.get("diagnostics", []):
            print(f"  NOTE {item}")
    return 1 if args.require_clean and out["missing_count"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
