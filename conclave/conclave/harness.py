"""The reasoning harness -- Conclave's core intelligence.

escalate(): gate -> retrieve (inject memory) -> structured prompt -> panel + judge
-> verify -> metrics. Model-agnostic; the panel/judge are config, not code. Off by
default; fail-open for the caller, fail-closed on spend.
"""
from __future__ import annotations

import json
import time
from pathlib import Path

from conclave import http, judge, panel

_NO_SEARCH = ("Do NOT web-search. Do NOT invent errata numbers, microcode revisions, or "
             "undocumented MSRs -- reason only from documented behavior and the context given.")

_SYS_PROMPTS = {
    "stuck": ("You are a senior systems engineer cracking a hard problem the primary agent "
              "and its reviewer could not solve. Reason rigorously from first principles. "
              + _NO_SEARCH),
    "review": ("You are a senior reviewer doing a high-stakes adversarial review of a change "
               "on a critical path (SMP, boot ABI, security). Find what the prior review missed. "
               + _NO_SEARCH),
}


def _budget_path(root, project) -> Path:
    return Path(root) / "data" / "projects" / project / "budget.json"


def _budget_used(p: Path) -> int:
    try:
        return int(json.loads(p.read_text()).get("calls_used", 0))
    except Exception:
        return 0


def _budget_increment(p: Path) -> None:
    try:
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps({"calls_used": _budget_used(p) + 1}))
    except Exception:
        pass


def _build_context(brief: str, retrieved: list) -> str:
    lessons = "\n".join(f"- {h['lesson']}" for h in retrieved if h.get("lesson"))
    if not lessons:
        return brief
    return f"RELEVANT PRIOR KNOWLEDGE:\n{lessons}\n\nPROBLEM:\n{brief}"


def escalate(*, cfg, secret, mode, brief, project, root, memory, metrics,
             prior=None, target=None, run_id=None) -> dict:
    if not cfg.enabled:
        return {"status": "disabled"}
    if not secret:
        return {"status": "no_key"}
    bpath = _budget_path(root, project)
    if _budget_used(bpath) >= cfg.max_calls:
        return {"status": "over_budget"}
    try:
        remaining = http.remaining_credits(secret)
    except Exception:
        remaining = None
    if remaining is None or remaining < cfg.min_credits:
        return {"status": "low_balance", "remaining": remaining}

    try:
        retrieved = memory.recall(brief, k=5)
    except Exception:
        retrieved = []
    ctx = _build_context(brief, retrieved)
    sys_prompt = _SYS_PROMPTS.get(mode, _SYS_PROMPTS["stuck"])

    wall0 = time.time()
    try:
        pr = panel.run(secret, cfg.panel, sys_prompt, ctx, cfg.model_timeout_s, cfg.workers)
    except Exception:
        return {"status": "unavailable"}
    ok = [r for r in pr if r["status"] == "ok" and r["content"].strip()]
    per_model = [{k: r.get(k) for k in ("model", "status", "cost", "sec", "chars",
                                        "prompt_tokens", "completion_tokens")} for r in pr]
    if not ok:
        return {"status": "unavailable", "panel": per_model}

    judged, jcost, jsec, jtoks = None, 0, 0.0, {"prompt": 0, "completion": 0}
    try:
        judged, jcost, jsec, jtoks = judge.judge(secret, cfg.judge, ctx, pr, prior, cfg.judge_timeout_s)
    except Exception:
        judged = None

    _budget_increment(bpath)
    panel_cost = sum(r["cost"] for r in pr)
    judge_cost = jcost or 0
    total = panel_cost + judge_cost
    wall = time.time() - wall0
    secs = [r.get("sec", 0) or 0 for r in pr]
    p_tok = sum(int(r.get("prompt_tokens") or 0) for r in pr) + int(jtoks.get("prompt") or 0)
    c_tok = sum(int(r.get("completion_tokens") or 0) for r in pr) + int(jtoks.get("completion") or 0)
    cumulative = metrics.add_run_to_totals(mode=mode, run_cost=total, panel_cost=panel_cost,
                                           judge_cost=judge_cost, tokens=p_tok + c_tok)
    metrics.append_run({
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "run_id": run_id, "mode": mode,
        "target": target or mode, "judge": cfg.judge, "panel_models": cfg.panel,
        "latency_s": {"panel_max": round(max(secs) if secs else 0, 1),
                      "panel_sum": round(sum(secs), 1), "judge": round(jsec, 1),
                      "wall": round(wall, 1)},
        "cost_usd": {"panel": round(panel_cost, 4), "judge": round(judge_cost, 4),
                     "run_total": round(total, 4), "cumulative_total": round(cumulative, 4)},
        "tokens": {"prompt": p_tok, "completion": c_tok, "total": p_tok + c_tok},
        "intelligence": {"panel_ok": len(ok), "panel_total": len(pr),
                         "judged": judged is not None,
                         "judge_chars": len(judged) if judged else 0, "outcome": "unknown"},
        "per_model": per_model})

    common = {"panel": per_model, "cost": total, "cumulative_cost": round(cumulative, 4),
              "tokens": p_tok + c_tok, "wall_s": round(wall, 1)}
    if judged is not None:
        return {"status": "ok", "output": judged, **common}
    raw = "\n\n".join(f"### {r['model']}\n{r['content']}" for r in ok)
    return {"status": "panel_only", "output": raw, **common}
