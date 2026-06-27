# .fusion -- OpenRouter Fusion escalation (apex tier)

The last rung of the Claude -> Codex -> Fusion escalation ladder. A panel of cheap,
diverse models (GLM 5.2 + Kimi K2.7-code + DeepSeek V4 Pro) deliberates and a cheap
judge (GLM 5.2) synthesizes, via OpenRouter's `openrouter/fusion`. The main Claude
thread is the real review layer -- it reads + validates Fusion's output before
acting, so we do not pay OpenRouter for a duplicate Opus judge.

## Off by default

Nothing here calls the network unless BOTH:
1. `FUSION_ENABLED=1` is set in the environment, and
2. a key exists at `.fusion/secret` (one line) or `OPENROUTER_API_KEY` is set.

## Setup (opt-in)

```bash
cp .fusion/secret.example .fusion/secret   # then replace the placeholder with your key
export FUSION_ENABLED=1
```

`.fusion/secret.example` is the tracked one-line template; `.fusion/secret` (your
real key) is gitignored.

## Spend safety

- Per-run cap `max_calls` (config.toml) + a live balance floor: each call first
  checks `GET /api/v1/credits` and skips if remaining < `min_credits`.
- Fail-open for the runner (an error never blocks the pipeline), fail-CLOSED for
  spend (an unknown/low balance -> skip, never spend blind).

## Data leaves the trust boundary

Enabling Fusion **transmits the context you send** (failing code, diffs, logs) to
OpenRouter, which fans it out to the external panel providers (Zhipu/GLM,
Moonshot/Kimi, DeepSeek) plus OpenRouter's auto web-search (Google); the GLM judge
is Zhipu, already in the panel. Your kernel source leaves your
machine to those services. Only enable Fusion if that is acceptable for this code.
The caller sends BOUNDED context (`--context-file` is capped at 60 KB), never the
whole tree. In interactive auto-mode, a source-to-external crossing is correctly
blocked until you approve it; the headless overnight run uses bypassPermissions and
is gated instead by `FUSION_ENABLED` + the spend caps.

## Files

- `fusion_escalate.py` -- the caller (`--mode stuck|review`, brief on stdin).
- `config.toml` -- panel / judge / caps.
- `secret` -- your OpenRouter key (GITIGNORED, never commit).
- `dataset.jsonl` -- collected escalation in/out + outcome for evals/distillation
  (GITIGNORED, local only).
