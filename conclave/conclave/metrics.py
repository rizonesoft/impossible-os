"""Per-run metrics + cumulative totals + outcome labels, project-namespaced.

Layout under data/projects/<project>/: metrics.jsonl (one rich line per run),
totals.json (cumulative ledger), outcomes.jsonl (the resolved/unresolved labels).
All writes atomic; all reads fail-open.
"""
from __future__ import annotations

import json
import os
import secrets
import time
from pathlib import Path

_TOTALS_BASE = {"runs": 0, "total_cost": 0.0, "panel_cost": 0.0, "judge_cost": 0.0,
                "total_tokens": 0, "resolved": 0, "unresolved": 0, "by_mode": {}}


def _ts() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S")


class MetricsStore:
    def __init__(self, root, project: str):
        self.base = Path(root) / "data" / "projects" / project

    def _p(self, name: str) -> Path:
        return self.base / name

    def _atomic(self, path: Path, text: str) -> None:
        self.base.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + f".{os.getpid()}.{secrets.token_hex(4)}.tmp")
        tmp.write_text(text, encoding="utf-8")
        tmp.replace(path)

    def append_run(self, record: dict) -> None:
        self.base.mkdir(parents=True, exist_ok=True)
        with self._p("metrics.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")

    def totals(self) -> dict:
        t = dict(_TOTALS_BASE)
        t["by_mode"] = {}
        try:
            t.update(json.loads(self._p("totals.json").read_text(encoding="utf-8")))
        except Exception:
            pass
        return t

    def _write_totals(self, t: dict) -> None:
        self._atomic(self._p("totals.json"), json.dumps(t, indent=2) + "\n")

    def add_run_to_totals(self, *, mode, run_cost, panel_cost, judge_cost, tokens) -> float:
        t = self.totals()
        t["runs"] += 1
        t["total_cost"] = round(t["total_cost"] + run_cost, 6)
        t["panel_cost"] = round(t["panel_cost"] + panel_cost, 6)
        t["judge_cost"] = round(t["judge_cost"] + judge_cost, 6)
        t["total_tokens"] += int(tokens)
        bm = t["by_mode"].setdefault(mode, {"runs": 0, "cost": 0.0})
        bm["runs"] += 1
        bm["cost"] = round(bm["cost"] + run_cost, 6)
        t["last_run"] = _ts()
        t["last_cost"] = round(run_cost, 6)
        self._write_totals(t)
        return t["total_cost"]

    def record_outcome(self, run_id: str, verdict: str) -> None:
        self.base.mkdir(parents=True, exist_ok=True)
        with self._p("outcomes.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps({"run_id": run_id, "verdict": verdict, "ts": _ts()}) + "\n")
        t = self.totals()
        key = "resolved" if verdict == "resolved" else "unresolved"
        t[key] = int(t.get(key, 0)) + 1
        self._write_totals(t)
        # back-fill the matching metrics line's intelligence.outcome
        p = self._p("metrics.jsonl")
        try:
            lines = p.read_text(encoding="utf-8").splitlines()
        except Exception:
            return
        changed = False
        for i, ln in enumerate(lines):
            try:
                rec = json.loads(ln)
            except Exception:
                continue
            if rec.get("run_id") == run_id:
                rec.setdefault("intelligence", {})["outcome"] = verdict
                lines[i] = json.dumps(rec)
                changed = True
        if changed:
            self._atomic(p, "\n".join(lines) + "\n")
