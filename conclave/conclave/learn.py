"""Self-learning: tune panel/routing policies from outcome labels (no weight updates),
and infer outcome labels automatically from a project's build/test success signal.

Policies live in policies/<project>.json. Auditable and reversible.
"""
from __future__ import annotations

import json
import os
import secrets
from pathlib import Path

_RESOLVED_DELTA = 1.0
_UNRESOLVED_DELTA = -0.25


def _path(root: Path, project: str) -> Path:
    return Path(root) / "policies" / f"{project}.json"


def _load(root, project: str) -> dict:
    try:
        return json.loads(_path(root, project).read_text(encoding="utf-8"))
    except Exception:
        return {"panel_credit": {}, "route": {}}


def _save(root, project: str, pol: dict) -> None:
    p = _path(root, project)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(f".{os.getpid()}.{secrets.token_hex(4)}.tmp")
    tmp.write_text(json.dumps(pol, indent=2) + "\n", encoding="utf-8")
    tmp.replace(p)


def update_policies(root, project: str, run_record: dict, verdict: str) -> None:
    pol = _load(root, project)
    pol.setdefault("panel_credit", {})
    pol.setdefault("route", {})
    delta = _RESOLVED_DELTA if verdict == "resolved" else _UNRESOLVED_DELTA
    ok_models = [pm["model"] for pm in run_record.get("per_model", []) if pm.get("status") == "ok"]
    for model in ok_models:
        pol["panel_credit"][model] = round(pol["panel_credit"].get(model, 0.0) + delta, 4)
    sig = run_record.get("signature")
    if sig and verdict == "resolved" and ok_models:
        pol["route"][sig] = ok_models
    _save(root, project, pol)


def select_panel(root, project: str, signature: str, full_panel: list) -> list:
    return _load(root, project).get("route", {}).get(signature, full_panel)


def infer_outcome(build_ok):
    """Automatic label from the project's own success signal."""
    if build_ok is None:
        return None
    return "resolved" if build_ok else "unresolved"
