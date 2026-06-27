#!/usr/bin/env python3
"""Fusion DIY-ensemble escalation -- the apex tier of the Claude->Codex->Fusion ladder.

Calls a lean panel of strong, diverse, NON-LADDER models SOLO and in parallel (NO
web-search), then a strong judge (Opus) synthesizes -- crediting unique-correct
insights and explicitly DISCARDING fabricated errata / microcode / MSRs. Returns the
synthesis PLUS per-model metrics (response / cost / time). Off-by-default; fail-open
for the runner, fail-CLOSED for spend. Stdlib only.

Why DIY, not the `openrouter/fusion` plugin: the plugin auto-web-searches (which
anchored panels onto hallucinated errata), reports a SUMMED cost (no per-model
metrics), and uses an opaque judge. Four rounds of testing (see .fusion/README.md)
showed DIY + no-search + a lean strong panel + an Opus discard-judge is the only
config that beat lone Opus -- at the lowest cost.

CLI: fusion_escalate.py --mode {stuck,review} [--brief T | --context-file P] [--root P]
Prints the judge synthesis on success; per-model metrics to stderr.
"""
from __future__ import annotations

import concurrent.futures
import json
import os
import sys
import time
import urllib.request
from pathlib import Path

try:
    import tomllib
except Exception:
    tomllib = None

CREDITS_URL = "https://openrouter.ai/api/v1/credits"
COMPLETIONS_URL = "https://openrouter.ai/api/v1/chat/completions"
MAX_CONTEXT_BYTES = 60000  # bound what leaves the trust boundary (see README)

# Final panel (four rounds of testing): strong, diverse, NON-ladder (Opus + Codex/
# GPT-5.5 are already in the ladder, so their analyses are fed to the judge, not
# re-run here). NO web-search -- it anchors models onto hallucinated specifics.
DEFAULT_CFG = {
    "panel": ["z-ai/glm-5.2", "deepseek/deepseek-v4-pro",
              "moonshotai/kimi-k2.7-code", "x-ai/grok-build-0.1",
              "minimax/minimax-m3", "qwen/qwen3-max"],  # probationary diversity
    "judge": "anthropic/claude-opus-4.8",
    "max_calls": 3,            # hard per-run call cap (FUSION_MAX_CALLS)
    "min_credits": 2.0,        # skip if OpenRouter balance is below this (USD)
    "model_timeout_s": 480,    # per-panel-model call bound (parallel -> ~= batch wall-clock)
    "judge_timeout_s": 480,    # judge call bound
    "workers": 6,              # parallel panel calls
}

_SYS_PROMPTS = {
    "stuck": ("You are an expert kernel/systems debugger. The primary agent is stuck "
              "after repeated failed attempts. Give the most likely root causes and "
              "concrete next steps to break the impasse. Be specific (file:line). Do "
              "NOT invent errata numbers, microcode revisions, or undocumented MSRs."),
    "review": ("You are an adversarial reviewer for a from-scratch OS kernel. Find "
               "correctness / SMP / lock-order / ABI / security defects the primary "
               "reviewers may have missed. Be concrete: file:line and why. Do NOT "
               "invent errata numbers or undocumented hardware behavior."),
}

# The discard-synthesis judge prompt -- the thing that made the ensemble beat Opus.
_JUDGE_SYS = (
    "You are the judge of an expert panel. You are given several independent "
    "analyses of the same hard problem (some may be prior analyses from the primary "
    "agent and its assistant). Synthesize: consensus, contradictions, any UNIQUE "
    "correct insight a single analysis had that the others missed, and blind spots. "
    "Explicitly call out and DISCARD any fabricated errata numbers, microcode "
    "revisions, or undocumented MSRs -- keep only verifiable facts. Then commit to "
    "the single most likely root cause and the concrete fix.")


def _http_chat(secret, model, messages, timeout):
    payload = {"model": model, "messages": messages}
    req = urllib.request.Request(
        COMPLETIONS_URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Authorization": f"Bearer {secret}", "Content-Type": "application/json"},
        method="POST")
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    content = data["choices"][0]["message"]["content"]
    usage = data.get("usage") or {}
    cost = usage.get("cost") or 0
    toks = {"prompt": int(usage.get("prompt_tokens") or 0),
            "completion": int(usage.get("completion_tokens") or 0)}
    return content, float(cost), time.time() - t0, toks


def _remaining_credits(secret) -> float:
    d = _http_chat_get(CREDITS_URL, secret).get("data") or {}
    return float(d.get("total_credits", 0)) - float(d.get("total_usage", 0))


def _http_chat_get(url, secret, timeout=10):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {secret}"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _probe(secret, model, sys_prompt, brief, timeout):
    try:
        content, cost, sec, toks = _http_chat(
            secret, model,
            [{"role": "system", "content": sys_prompt}, {"role": "user", "content": brief}],
            timeout)
        return {"model": model, "status": "ok", "content": content,
                "cost": cost, "sec": round(sec, 1), "chars": len(content),
                "prompt_tokens": toks["prompt"], "completion_tokens": toks["completion"]}
    except Exception as e:
        return {"model": model, "status": "error", "content": "", "cost": 0,
                "sec": 0, "chars": 0, "prompt_tokens": 0, "completion_tokens": 0,
                "err": str(e)[:160]}


def _run_panel(secret, panel, sys_prompt, brief, timeout, workers):
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers or len(panel)) as ex:
        return list(ex.map(lambda m: _probe(secret, m, sys_prompt, brief, timeout), panel))


def _judge(secret, judge_model, brief, panel_results, prior, timeout):
    parts = []
    for p in (prior or []):
        parts.append(f"### {p.get('source', 'prior')} (prior ladder analysis)\n{p.get('content', '')}")
    for r in panel_results:
        if r["status"] == "ok" and r["content"].strip():
            parts.append(f"### {r['model']}\n{r['content']}")
    blob = "\n\n".join(parts)
    msgs = [{"role": "system", "content": _JUDGE_SYS},
            {"role": "user", "content": f"PROBLEM:\n{brief}\n\nANALYSES:\n{blob}"}]
    return _http_chat(secret, judge_model, msgs, timeout)


def _budget_used(path: Path) -> int:
    try:
        return int(json.loads(path.read_text()).get("fusion_calls_used", 0))
    except Exception:
        return 0


def _budget_increment(path: Path) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps({"fusion_calls_used": _budget_used(path) + 1}))
    except Exception:
        pass


def _dataset_append(root: Path, record: dict) -> None:
    try:
        p = root / ".fusion" / "dataset.jsonl"
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")
    except Exception:
        pass


def _metrics_append(root: Path, record: dict) -> None:
    """One rich per-run metrics line (performance / cost / tokens / intelligence).
    Lean: no full brief/output. Fail-open."""
    try:
        p = root / ".fusion" / "metrics.jsonl"
        p.parent.mkdir(parents=True, exist_ok=True)
        with p.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")
    except Exception:
        pass


def _read_totals(root: Path) -> dict:
    base = {"runs": 0, "total_cost": 0.0, "panel_cost": 0.0, "judge_cost": 0.0,
            "total_tokens": 0, "resolved": 0, "unresolved": 0, "by_mode": {}}
    try:
        base.update(json.loads((root / ".fusion" / "totals.json").read_text()))
    except Exception:
        pass
    return base


def _write_totals(root: Path, totals: dict) -> None:
    try:
        p = root / ".fusion" / "totals.json"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(totals, indent=2) + "\n", encoding="utf-8")
    except Exception:
        pass


def _totals_add_run(root: Path, *, mode, run_cost, panel_cost, judge_cost, tokens) -> float:
    """Roll one run into the cumulative ledger. Returns the new cumulative total_cost.
    Fail-open: on any error returns this run's cost as the best-effort cumulative."""
    try:
        t = _read_totals(root)
        t["runs"] += 1
        t["total_cost"] = round(t["total_cost"] + run_cost, 6)
        t["panel_cost"] = round(t["panel_cost"] + panel_cost, 6)
        t["judge_cost"] = round(t["judge_cost"] + judge_cost, 6)
        t["total_tokens"] += int(tokens)
        bm = t["by_mode"].setdefault(mode, {"runs": 0, "cost": 0.0})
        bm["runs"] += 1
        bm["cost"] = round(bm["cost"] + run_cost, 6)
        t["last_run"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        t["last_cost"] = round(run_cost, 6)
        _write_totals(root, t)
        return t["total_cost"]
    except Exception:
        return round(run_cost, 6)


def record_outcome(root: Path, run_id: str, verdict: str) -> None:
    """Back-fill a run's quality verdict: bump totals resolved/unresolved and stamp
    intelligence.outcome on the matching metrics line (by run_id). Fail-open."""
    try:
        t = _read_totals(root)
        if verdict == "resolved":
            t["resolved"] = int(t.get("resolved", 0)) + 1
        else:
            t["unresolved"] = int(t.get("unresolved", 0)) + 1
        _write_totals(root, t)
    except Exception:
        pass
    if not run_id:
        return
    try:
        p = root / ".fusion" / "metrics.jsonl"
        lines = p.read_text(encoding="utf-8").splitlines()
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
            p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    except Exception:
        pass


def escalate(*, enabled, secret, cfg, mode, brief, root, prior=None,
             target=None, run_id=None) -> dict:
    if not enabled:
        return {"status": "disabled"}
    if not secret:
        return {"status": "no_key"}
    budget_path = root / ".claude" / "state" / "fusion-budget.json"
    if _budget_used(budget_path) >= cfg["max_calls"]:
        return {"status": "over_budget"}
    # Balance floor -- fail CLOSED on spend (unknown balance -> skip).
    try:
        remaining = _remaining_credits(secret)
    except Exception:
        remaining = None
    if remaining is None or remaining < cfg["min_credits"]:
        return {"status": "low_balance", "remaining": remaining}

    sys_prompt = _SYS_PROMPTS.get(mode, _SYS_PROMPTS["stuck"])
    wall0 = time.time()
    try:
        panel = _run_panel(secret, cfg["panel"], sys_prompt, brief,
                           cfg.get("model_timeout_s", 480), cfg.get("workers", 0))
    except Exception:
        return {"status": "unavailable"}
    ok = [r for r in panel if r["status"] == "ok" and r["content"].strip()]
    metrics = [{k: r.get(k) for k in ("model", "status", "cost", "sec", "chars",
                                      "prompt_tokens", "completion_tokens")} for r in panel]
    if not ok:
        return {"status": "unavailable", "panel": metrics}

    judged = None
    jcost = 0
    jsec = 0.0
    jtoks = {"prompt": 0, "completion": 0}
    try:
        judged, jcost, jsec, jtoks = _judge(secret, cfg["judge"], brief, panel, prior,
                                            cfg.get("judge_timeout_s", 480))
    except Exception:
        judged = None

    _budget_increment(budget_path)  # paid panel calls happened
    panel_cost = sum(r["cost"] for r in panel)
    judge_cost = jcost or 0
    total = panel_cost + judge_cost
    wall = time.time() - wall0

    secs = [r.get("sec", 0) or 0 for r in panel]
    p_tok = sum(int(r.get("prompt_tokens") or 0) for r in panel) + int(jtoks.get("prompt") or 0)
    c_tok = sum(int(r.get("completion_tokens") or 0) for r in panel) + int(jtoks.get("completion") or 0)
    cumulative = _totals_add_run(root, mode=mode, run_cost=total, panel_cost=panel_cost,
                                 judge_cost=judge_cost, tokens=p_tok + c_tok)
    intelligence = {"panel_ok": len(ok), "panel_total": len(panel),
                    "judged": judged is not None,
                    "judge_chars": len(judged) if judged else 0, "outcome": "unknown"}
    _metrics_append(root, {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"), "run_id": run_id,
        "mode": mode, "target": target or mode, "judge": cfg["judge"],
        "panel_models": cfg["panel"],
        "latency_s": {"panel_max": round(max(secs) if secs else 0, 1),
                      "panel_sum": round(sum(secs), 1), "judge": round(jsec, 1),
                      "wall": round(wall, 1)},
        "cost_usd": {"panel": round(panel_cost, 4), "judge": round(judge_cost, 4),
                     "run_total": round(total, 4), "cumulative_total": round(cumulative, 4)},
        "tokens": {"prompt": p_tok, "completion": c_tok, "total": p_tok + c_tok},
        "intelligence": intelligence, "per_model": metrics,
    })
    _dataset_append(root, {
        "tier": "fusion", "mode": mode, "panel": cfg["panel"], "judge": cfg["judge"],
        "problem_brief": brief, "per_model": metrics, "output": judged or "",
        "cost": total, "judged": judged is not None, "outcome": "unknown",
    })
    common = {"panel": metrics, "cost": total, "cumulative_cost": round(cumulative, 4),
              "tokens": p_tok + c_tok, "wall_s": round(wall, 1)}
    if judged is not None:
        return {"status": "ok", "output": judged, **common}
    # Judge failed -> hand the raw panel to the main thread (the final review layer).
    raw = "\n\n".join(f"### {r['model']}\n{r['content']}" for r in ok)
    return {"status": "panel_only", "output": raw, **common}


def _load_cfg(root: Path) -> dict:
    cfg = dict(DEFAULT_CFG)
    p = root / ".fusion" / "config.toml"
    if tomllib is not None and p.exists():
        try:
            data = tomllib.loads(p.read_text(encoding="utf-8"))
            cfg.update({k: data[k] for k in DEFAULT_CFG if k in data})
        except Exception:
            pass
    return cfg


def _read_secret(root: Path) -> str:
    env = os.environ.get("OPENROUTER_API_KEY")
    if env:
        return env.strip()
    try:
        return (root / ".fusion" / "secret").read_text(encoding="utf-8").strip()
    except Exception:
        return ""


def _read_context(path, max_bytes=MAX_CONTEXT_BYTES) -> str:
    try:
        data = Path(path).read_text(encoding="utf-8", errors="replace")
    except Exception:
        return ""
    if len(data) > max_bytes:
        data = data[:max_bytes] + "\n...[truncated by fusion context cap]..."
    return data


def main(argv) -> int:
    mode = argv[argv.index("--mode") + 1] if "--mode" in argv else "stuck"
    root = Path(argv[argv.index("--root") + 1]) if "--root" in argv else Path.cwd()
    instruction = argv[argv.index("--brief") + 1] if "--brief" in argv else ""
    if "--context-file" in argv:
        ctx = _read_context(argv[argv.index("--context-file") + 1])
        brief = (instruction + "\n\n" + ctx) if instruction else ctx
    elif instruction:
        brief = instruction
    else:
        brief = sys.stdin.read()
    res = escalate(enabled=os.environ.get("FUSION_ENABLED") == "1",
                   secret=_read_secret(root), cfg=_load_cfg(root),
                   mode=mode, brief=brief, root=root)
    for r in res.get("panel", []) or []:
        c = f"${r['cost']:.4f}" if r.get("cost") else "-"
        print(f"[fusion] {r['model']:36} {r['status']:8} {c:>9} {r.get('sec', 0):>6}s",
              file=sys.stderr)
    if res["status"] in ("ok", "panel_only"):
        if res["status"] == "panel_only":
            print("[fusion] judge unavailable -- raw panel below (synthesize yourself)",
                  file=sys.stderr)
        print(res["output"])
    else:
        print(f"[fusion] {res['status']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
