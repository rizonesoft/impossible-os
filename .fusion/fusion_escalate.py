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
    cost = (data.get("usage") or {}).get("cost") or 0
    return content, float(cost), time.time() - t0


def _remaining_credits(secret) -> float:
    d = _http_chat_get(CREDITS_URL, secret).get("data") or {}
    return float(d.get("total_credits", 0)) - float(d.get("total_usage", 0))


def _http_chat_get(url, secret, timeout=10):
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {secret}"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _probe(secret, model, sys_prompt, brief, timeout):
    try:
        content, cost, sec = _http_chat(
            secret, model,
            [{"role": "system", "content": sys_prompt}, {"role": "user", "content": brief}],
            timeout)
        return {"model": model, "status": "ok", "content": content,
                "cost": cost, "sec": round(sec, 1), "chars": len(content)}
    except Exception as e:
        return {"model": model, "status": "error", "content": "", "cost": 0,
                "sec": 0, "chars": 0, "err": str(e)[:160]}


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


def escalate(*, enabled, secret, cfg, mode, brief, root, prior=None) -> dict:
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
    try:
        panel = _run_panel(secret, cfg["panel"], sys_prompt, brief,
                           cfg.get("model_timeout_s", 480), cfg.get("workers", 0))
    except Exception:
        return {"status": "unavailable"}
    ok = [r for r in panel if r["status"] == "ok" and r["content"].strip()]
    metrics = [{k: r.get(k) for k in ("model", "status", "cost", "sec", "chars")} for r in panel]
    if not ok:
        return {"status": "unavailable", "panel": metrics}

    judged = None
    jcost = 0
    try:
        judged, jcost, _ = _judge(secret, cfg["judge"], brief, panel, prior,
                                  cfg.get("judge_timeout_s", 480))
    except Exception:
        judged = None

    _budget_increment(budget_path)  # paid panel calls happened
    total = sum(r["cost"] for r in panel) + (jcost or 0)
    _dataset_append(root, {
        "tier": "fusion", "mode": mode, "panel": cfg["panel"], "judge": cfg["judge"],
        "problem_brief": brief, "per_model": metrics, "output": judged or "",
        "cost": total, "judged": judged is not None, "outcome": "unknown",
    })
    if judged is not None:
        return {"status": "ok", "output": judged, "panel": metrics, "cost": total}
    # Judge failed -> hand the raw panel to the main thread (the final review layer).
    raw = "\n\n".join(f"### {r['model']}\n{r['content']}" for r in ok)
    return {"status": "panel_only", "output": raw, "panel": metrics, "cost": total}


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
