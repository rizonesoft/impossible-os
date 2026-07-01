# Fusion Ladder Plan 1 -- the .fusion/ caller foundation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the off-by-default, fail-open OpenRouter Fusion caller in `.fusion/` -- the apex escalation tier -- with the two-layer spend cap, dataset logging, and full offline (mocked) test coverage, shipping nothing that fires until enabled.

**Architecture:** `.fusion/fusion_escalate.py` exposes a pure `escalate(...)` core (gating + spend cap + balance floor + dataset write) that calls two thin HTTP helpers (`_remaining_credits`, `_fusion_call`); tests monkeypatch those helpers so the whole surface is provable with no key and no network. `config.toml` carries the panel/judge/budgets; the secret and the collected dataset are gitignored.

**Tech Stack:** Python 3.11+ stdlib only (`urllib`, `json`, `tomllib`); confirmed `python3` is 3.12.3. No pip dependency.

## Global Constraints

- Python stdlib only; ASCII only; no section-sign+digit in code.
- OFF by default: the Fusion call happens only when `FUSION_ENABLED=1` AND a secret is present (`.fusion/secret` or `OPENROUTER_API_KEY` env). Absent either -> a status result, no network call.
- Secret never in git: `.fusion/secret` and `.fusion/dataset.jsonl` are gitignored; everything else in `.fusion/` is tracked.
- Two-layer spend cap: per-run counter `FUSION_MAX_CALLS` (default 3) in `.claude/state/fusion-budget.json` + a live balance floor (`GET /api/v1/credits`, skip if `total_credits - total_usage < FUSION_MIN_CREDITS`, default 2.0).
- Fail-open for the runner, fail-CLOSED for spend: any error -> a status result + the runner continues; an unknown/low balance -> skip the paid call (never spend blind).
- Dataset-grade log: each successful call appends a JSONL record (brief + output + exact `usage` cost + outcome) to `.fusion/dataset.jsonl`.
- Verified wire shape: `POST https://openrouter.ai/api/v1/chat/completions`, body `{"model":"openrouter/fusion","plugins":[{"id":"fusion","analysis_models":[...],"model":"<judge>"}],"messages":[...]}`, `Authorization: Bearer <secret>`; synthesis at `choices[0].message.content`.
- Module file is `fusion_escalate.py` (underscore, so tests can `import` it); the spec's hyphen name is adjusted for Python importability.
- Plan 2 (ladder controller + stuck-detect hook + skill wiring) is NOT this plan.

---

### Task 1: fusion_escalate.py caller + offline mocked tests

**Files:**
- Create: `.fusion/fusion_escalate.py`
- Test: `scripts/overnight/tests/test_fusion_escalate.py` (create)

**Interfaces:**
- Produces:
  - `escalate(*, enabled: bool, secret: str, cfg: dict, mode: str, brief: str, root: Path) -> dict` -- returns `{"status": ...}` where status is one of `disabled / no_key / over_budget / low_balance / unavailable / ok`; `ok` adds `output` + `cost`.
  - `_remaining_credits(secret) -> float` and `_fusion_call(secret, cfg, mode, brief) -> (content, cost)` -- the two helpers tests monkeypatch.
  - cfg keys: `panel: list[str]`, `judge: str`, `max_calls: int`, `min_credits: float`, `timeout_s: int`.

- [ ] **Step 1: Write the failing test**

Create `scripts/overnight/tests/test_fusion_escalate.py`:

```python
#!/usr/bin/env python3
import json, sys, tempfile, pathlib

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / ".fusion"))
import fusion_escalate as fe  # noqa: E402

CFG = {"panel": ["a", "b", "c"], "judge": "a",
       "max_calls": 3, "min_credits": 2.0, "timeout_s": 5}


def _root(d, budget_used=None):
    root = pathlib.Path(d)
    if budget_used is not None:
        sp = root / ".claude" / "state"
        sp.mkdir(parents=True, exist_ok=True)
        (sp / "fusion-budget.json").write_text(json.dumps({"fusion_calls_used": budget_used}))
    return root


def test_disabled():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=False, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "disabled"


def test_no_key():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=True, secret="", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "no_key"


def test_over_budget():
    with tempfile.TemporaryDirectory() as d:
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d, budget_used=3))
        assert r["status"] == "over_budget"


def test_low_balance():
    with tempfile.TemporaryDirectory() as d:
        fe._remaining_credits = lambda s: 1.0
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "low_balance"


def test_balance_check_error_fails_closed():
    with tempfile.TemporaryDirectory() as d:
        def boom(s):
            raise RuntimeError("network down")
        fe._remaining_credits = boom
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=_root(d))
        assert r["status"] == "low_balance"  # unknown balance -> skip (no blind spend)


def test_ok_writes_dataset_and_increments_budget():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        fe._fusion_call = lambda s, c, m, b: ("SYNTHESIS", 0.12)
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="why does it crash", root=root)
        assert r["status"] == "ok" and r["output"] == "SYNTHESIS"
        ds = (root / ".fusion" / "dataset.jsonl").read_text()
        assert "SYNTHESIS" in ds and "why does it crash" in ds
        used = json.loads((root / ".claude" / "state" / "fusion-budget.json").read_text())
        assert used["fusion_calls_used"] == 1


def test_post_error_is_fail_open_no_charge():
    with tempfile.TemporaryDirectory() as d:
        root = _root(d)
        fe._remaining_credits = lambda s: 100.0
        def boom(*a):
            raise RuntimeError("api 500")
        fe._fusion_call = boom
        r = fe.escalate(enabled=True, secret="x", cfg=CFG, mode="stuck",
                        brief="b", root=root)
        assert r["status"] == "unavailable"
        assert not (root / ".claude" / "state" / "fusion-budget.json").exists()


if __name__ == "__main__":
    test_disabled()
    test_no_key()
    test_over_budget()
    test_low_balance()
    test_balance_check_error_fails_closed()
    test_ok_writes_dataset_and_increments_budget()
    test_post_error_is_fail_open_no_charge()
    print("PASS: fusion_escalate")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 scripts/overnight/tests/test_fusion_escalate.py`
Expected: FAIL (`ModuleNotFoundError: fusion_escalate`).

- [ ] **Step 3: Write `.fusion/fusion_escalate.py`**

```python
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


def main(argv) -> int:
    mode = argv[argv.index("--mode") + 1] if "--mode" in argv else "stuck"
    root = Path(argv[argv.index("--root") + 1]) if "--root" in argv else Path.cwd()
    brief = argv[argv.index("--brief") + 1] if "--brief" in argv else sys.stdin.read()
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
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 scripts/overnight/tests/test_fusion_escalate.py`
Expected: `PASS: fusion_escalate`

- [ ] **Step 5: Smoke the CLI disabled path (no network, no key)**

Run: `echo "why does AP boot hang" | python3 .fusion/fusion_escalate.py --mode stuck --root .; echo "rc=$?"`
Expected: stderr `[fusion] disabled` (FUSION_ENABLED unset), rc=0, no network call.

- [ ] **Step 6: Commit**

```bash
git add .fusion/fusion_escalate.py scripts/overnight/tests/test_fusion_escalate.py
git commit -m "fusion: Plan1 -- escalate caller (off-by-default, two-layer cap, fail-open/closed)"
```

---

### Task 2: config.toml + gitignore + README scaffolding

**Files:**
- Create: `.fusion/config.toml`
- Create: `.fusion/README.md`
- Modify: `.gitignore`

**Interfaces:** `config.toml` is consumed by `_load_cfg` (Task 1) with keys `panel/judge/max_calls/min_credits/timeout_s`.

- [ ] **Step 1: Create `.fusion/config.toml`**

```toml
# Fusion escalation config -- the apex tier of the Claude -> Codex -> Fusion ladder.
# See .fusion/README.md for how to enable. Off unless FUSION_ENABLED=1 + a secret.

# Panel (analysis_models): three independent labs for diverse blind spots.
panel = ["z-ai/glm-5.2", "google/gemini-3.5-flash", "moonshotai/kimi-k2.7"]
# Judge (Fusion's built-in synthesizer model).
judge = "z-ai/glm-5.2"

# Spend caps.
max_calls = 3        # hard per-run Fusion call cap (FUSION_MAX_CALLS)
min_credits = 2.0    # skip if OpenRouter remaining balance is below this (USD)
timeout_s = 240      # per-call wall-clock bound
```

- [ ] **Step 2: Add gitignore rules for the secret + dataset**

Append to `.gitignore`:
```
# .fusion/ -- Fusion escalation (apex tier). Secret + collected dataset never committed.
.fusion/secret
.fusion/dataset.jsonl
```

- [ ] **Step 3: Create `.fusion/README.md`**

```markdown
# .fusion -- OpenRouter Fusion escalation (apex tier)

The last rung of the Claude -> Codex -> Fusion escalation ladder. A panel of
cheap, diverse models (GLM 5.2 + Gemini 3.5 Flash + Kimi K2.7) deliberates and a
judge (GLM 5.2) synthesizes, via OpenRouter's `openrouter/fusion`.

## Off by default

Nothing here calls the network unless BOTH:
1. `FUSION_ENABLED=1` is set in the environment, and
2. a key exists at `.fusion/secret` (one line) or `OPENROUTER_API_KEY` is set.

## Setup (opt-in)

```bash
printf '%s' 'sk-or-...your-openrouter-key...' > .fusion/secret   # gitignored
export FUSION_ENABLED=1
```

## Spend safety

- Per-run cap `max_calls` (config.toml) + a live balance floor: each call first
  checks `GET /api/v1/credits` and skips if remaining < `min_credits`.
- Fail-open for the runner (an error never blocks the pipeline), fail-CLOSED for
  spend (an unknown/low balance -> skip, never spend blind).

## Files

- `fusion_escalate.py` -- the caller (`--mode stuck|review`, brief on stdin).
- `config.toml` -- panel / judge / caps.
- `secret` -- your OpenRouter key (GITIGNORED, never commit).
- `dataset.jsonl` -- collected escalation in/out + outcome for evals/distillation
  (GITIGNORED, local only).
```

- [ ] **Step 4: Verify config loads, gitignore works, tracked files are tracked**

Run:
```bash
python3 -c "import sys,pathlib; sys.path.insert(0,'.fusion'); import fusion_escalate as fe; print(fe._load_cfg(pathlib.Path('.'))['panel'])"
printf 'sk-or-test' > .fusion/secret
git check-ignore .fusion/secret && echo "secret IGNORED (good)"
git check-ignore .fusion/dataset.jsonl && echo "dataset path IGNORED (good)"
git check-ignore .fusion/config.toml >/dev/null && echo "config WRONGLY ignored" || echo "config tracked (good)"
rm -f .fusion/secret
```
Expected: panel prints the three model ids; `secret`/`dataset.jsonl` IGNORED; `config.toml` tracked; the test secret is removed.

- [ ] **Step 5: Commit**

```bash
git add .fusion/config.toml .fusion/README.md .gitignore
git commit -m "fusion: Plan1 -- config.toml + README + gitignore (secret + dataset never committed)"
```

---

## Self-Review

**Spec coverage (Plan 1 slice):** off-by-default gating (escalate `disabled`/`no_key`), two-layer spend cap (`over_budget` + `low_balance` via `_remaining_credits`), fail-open-runner / fail-closed-spend (the two try/except blocks + the balance-unknown skip), dataset-grade logging (`_dataset_append` with brief+output+cost+outcome), verified wire shape (`_fusion_call` payload), `.fusion/secret`+`dataset.jsonl` gitignored, config in `.fusion/config.toml`. Ladder controller, stuck-detect hook, and skill wiring are Plan 2 (out of scope, stated).

**Placeholder scan:** every code step is complete; commands have expected output. No TBD/"handle errors"/vague steps.

**Type consistency:** `escalate(enabled, secret, cfg, mode, brief, root)` and the cfg keys (`panel/judge/max_calls/min_credits/timeout_s`) are identical in the test (Task 1), the implementation (Task 1), and `config.toml` (Task 2). `_remaining_credits(secret)` / `_fusion_call(secret, cfg, mode, brief)` signatures match the monkeypatches in the test.

## Follow-on (Plan 2)

`.fusion/ladder.py` controller (3 -> Codex 2 -> Fusion 1, reusing `codex-rescue`); `.claude/hooks/fusion_stuck_detect.py` PostToolUse repeated-failure tracker; skill wiring (review-todo-section high-risk supplement, debug-session/stuck, sequencer note); resolution-outcome backfill into `dataset.jsonl`.
