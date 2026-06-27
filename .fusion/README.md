# .fusion -- OpenRouter Fusion escalation (apex tier)

The last rung of the Claude -> Codex -> Fusion escalation ladder. A panel of cheap,
diverse models (GLM 5.2 + Gemini 3.5 Flash + Kimi K2.7) deliberates and a judge
(GLM 5.2) synthesizes, via OpenRouter's `openrouter/fusion`.

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
