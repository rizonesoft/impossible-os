#!/usr/bin/env python3
"""OpenRouter Fusion escalation caller -- the apex tier of the Claude->Codex->Fusion
ladder. Off-by-default; fail-open for the runner, fail-CLOSED for spend. Stdlib only.

CLI: fusion_escalate.py --mode {stuck,review} [--brief TEXT] [--root PATH]
Reads the brief from --brief or stdin; prints the synthesis on success.
"""
from __future__ import annotations

import json
import os
import sys
import urllib.request
from pathlib import Path

try:
    import tomllib
except Exception:
    tomllib = None

CREDITS_URL = "https://openrouter.ai/api/v1/credits"
COMPLETIONS_URL = "https://openrouter.ai/api/v1/chat/completions"

DEFAULT_CFG = {
    "panel": ["z-ai/glm-5.2", "google/gemini-3.5-flash", "moonshotai/kimi-k2.7"],
    "judge": "z-ai/glm-5.2",
    "max_calls": 3,
    "min_credits": 2.0,
    "timeout_s": 240,
}

_SYS_PROMPTS = {
    "stuck": ("You are an expert kernel/systems debugger. The primary agent is stuck "
              "after repeated failed attempts. Give the most likely root causes and "
              "concrete next steps to break the impasse. Be specific (file:line)."),
    "review": ("You are an adversarial reviewer for a from-scratch OS kernel. Find "
               "correctness / SMP / lock-order / ABI / security defects the primary "
               "reviewers may have missed. Be concrete: file:line and why."),
}


def _http_json(url, headers, data=None, timeout=10):
    req = urllib.request.Request(
        url, data=data, headers=headers,
        method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _remaining_credits(secret) -> float:
    d = _http_json(CREDITS_URL, {"Authorization": f"Bearer {secret}"}).get("data") or {}
    return float(d.get("total_credits", 0)) - float(d.get("total_usage", 0))


def _fusion_call(secret, cfg, mode, brief):
    headers = {"Authorization": f"Bearer {secret}", "Content-Type": "application/json"}
    payload = {
        "model": "openrouter/fusion",
        "plugins": [{"id": "fusion", "analysis_models": cfg["panel"], "model": cfg["judge"]}],
        "messages": [
            {"role": "system", "content": _SYS_PROMPTS.get(mode, _SYS_PROMPTS["stuck"])},
            {"role": "user", "content": brief},
        ],
    }
    resp = _http_json(COMPLETIONS_URL, headers,
                      data=json.dumps(payload).encode("utf-8"),
                      timeout=cfg.get("timeout_s", 240))
    content = resp["choices"][0]["message"]["content"]
    cost = (resp.get("usage") or {}).get("cost")
    return content, cost


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


def escalate(*, enabled, secret, cfg, mode, brief, root) -> dict:
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
    # The paid call -- fail OPEN for the runner.
    try:
        content, cost = _fusion_call(secret, cfg, mode, brief)
    except Exception:
        return {"status": "unavailable"}
    _budget_increment(budget_path)
    _dataset_append(root, {
        "tier": "fusion", "mode": mode, "models": cfg["panel"], "judge": cfg["judge"],
        "problem_brief": brief, "output": content, "cost": cost, "outcome": "unknown",
    })
    return {"status": "ok", "output": content, "cost": cost}


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


MAX_CONTEXT_BYTES = 60000  # bound what leaves the trust boundary (see README)


def _read_context(path, max_bytes=MAX_CONTEXT_BYTES) -> str:
    """Read a context file, capped -- never send the whole tree off-machine."""
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
    if res["status"] == "ok":
        print(res["output"])
    else:
        print(f"[fusion] {res['status']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
