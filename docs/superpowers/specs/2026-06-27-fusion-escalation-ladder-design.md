# Fusion Escalation Ladder (Claude -> Codex -> Fusion)

> Design spec. Status: approved 2026-06-27. Operator: Derick Payne. Scope: when the
> overnight main loop is stuck on a hard problem, or completing a highest-stakes
> section, escalate through a graduated ladder whose apex is an OpenRouter Fusion
> ensemble -- cheapest-capable-tier first, each rung gated on the previous rung's
> exhaustion, off-by-default and fail-open throughout.

## 1. Problem and goal

A single model (Opus main loop) can loop on a hard problem -- a bare-metal crash
whose root cause resists hypotheses, a design impasse, contradictory signals --
burning expensive turns without converging. Likewise, the highest-stakes decisions
(SMP/lock-order, ABI, security) can pass the normal reviewers and still be wrong,
at a cost (a hardware crash) that justifies extra scrutiny.

The goal: a graduated escalation ladder that brings progressively more (and more
diverse) compute to bear ONLY as a problem proves genuinely hard, so the expensive
ensemble is rare and justified -- and, because the upper tiers run on models far
cheaper than Opus, escalating is often *cheaper* than letting Opus keep looping.

## 2. The escalation ladder

```
Tier 0  Claude (main loop)   works the problem
          |  stuck = 3 consecutive failures on the SAME target (build/test/smoke)
          v
Tier 1  Codex assist         single strong second opinion (GPT-5.5) -- up to 2 rounds
          |  2 rounds exhausted, still stuck
          v
Tier 2  Fusion assist        ensemble apex (GLM 5.2 + Gemini 3.5 Flash + Kimi K2.7,
                             judge GLM 5.2) -- 1 call, last resort
```

The numbers are per-tier budgets, all configurable: **3** = Claude stuck threshold,
**2** = Codex assist rounds, **1** = Fusion call per impasse. **Each tier fires
only if the cheaper tier failed.** Fusion (priciest) is reached only after a strong
single model (Codex) has already failed, which makes it genuinely rare.

**Both roles use this ladder; Fusion is always the top rung:**
- **Stuck-solver:** Tier 0 Claude -> Tier 1 Codex rescue -> Tier 2 Fusion (mode
  `stuck`, a hypothesis-generation prompt).
- **High-risk reviewer:** Claude self-review -> Codex review (already in the
  pipeline) -> Fusion review (mode `review`, apex, flagged paths only).

**Tier 1 reuses existing infra.** The repo already ships the `codex:rescue` /
codex-rescue subagent ("delegate investigation / fix-request / rescue work to
Codex"). The new build is the **ladder controller** (count 3 -> invoke rescue ->
count 2 rounds -> escalate) and the **Fusion apex tier**, not a fresh Codex
integration.

## 3. Hard invariants (cost + safety -- load-bearing)

1. **OFF by default.** The Fusion tier fires only when `FUSION_ENABLED=1` AND a key
   is present at `.fusion/secret` (or `OPENROUTER_API_KEY` in env). Absent either ->
   the ladder simply stops at the Codex tier; the runner is unaffected.
2. **Secret never in git.** `.fusion/secret` is gitignored; everything else in
   `.fusion/` is tracked. No key, token, or credential is ever committed.
3. **Hard per-run cap.** A budget counter in `.claude/state/fusion-budget.json`
   (default `FUSION_MAX_CALLS=3` per run); over cap -> Fusion tier is skipped +
   logged, never called.
4. **Bounded input.** Each Fusion/Codex call sends a focused problem brief + a
   capped slice of relevant snippets, never the whole codebase -- bounds input cost.
5. **Fail-open on EVERYTHING.** Missing key, network error, API 4xx/5xx, timeout
   (bounded ~240 s), malformed response, budget exhausted -> the caller returns a
   clear "unavailable" result and the runner CONTINUES its normal path (keep
   debugging with Claude; the high-risk review already ran its normal passes --
   Fusion is always supplemental). It NEVER blocks the pipeline and NEVER crashes.
6. **Every call logged** to `.fusion/` (or `.claude/state/`) with tier, mode,
   models, and a cost estimate.

## 4. Components

```
.fusion/                         (self-contained Fusion module)
  fusion-escalate.py   caller -- Python stdlib (urllib+json); env+budget gated; fail-open
  ladder.py            Claude->Codex->Fusion controller: tier state + per-tier budgets
  config.toml          panel/judge model ids + budgets (3/2/1) + caps -- tracked
  secret               OPENROUTER_API_KEY -- GITIGNORED, never committed
  README.md            opt-in setup (how to enable, where the key goes) -- tracked
.claude/hooks/
  fusion_stuck_detect.py   PostToolUse on Bash: counts consecutive same-target
                           build/test/smoke failures; at threshold emits a
                           systemMessage pointing the loop at the ladder. Reads
                           .fusion/config.toml. (Hooks must live here for settings.json.)
.claude/state/
  fusion-stuck.json    {last_target, consecutive_failures}
  fusion-budget.json   {run_id, fusion_calls_used}
```

Skill wiring: `review-todo-section` (Fusion review supplement on flagged paths),
`debug-session` / the stuck path (ladder invocation), and an overnight-sequencer
note. The network call lives only in `fusion-escalate.py` (a script the loop runs)
-- never in a hook.

**Language decision:** Python stdlib, not C++. The task is I/O-bound (HTTP POST +
wait on 3 models + a judge); C++ gives zero perf benefit and would require adding
libcurl + a JSON lib to the host bootstrap (the host has `clang++` but no
networking/JSON C++ stack). Python `urllib`+`json` is zero-dependency, zero-build,
fail-open-trivial, and matches every other host tool in the repo.

## 5. Panel / judge configuration (config.toml)

- `analysis_models` (panel): `z-ai/glm-5.2`, `google/gemini-3.5-flash`,
  `moonshotai/kimi-k2.7` (or `kimi-k2.7-code`).
- `model` (Fusion's built-in judge): `z-ai/glm-5.2` -- strong coder, cheap, keeps
  the whole deliberation off Opus.
### Verified OpenRouter Fusion wire shape (docs, 2026-06-27)

- **Endpoint:** `POST https://openrouter.ai/api/v1/chat/completions`.
- **Auth:** `Authorization: Bearer <contents of .fusion/secret>`.
- **Body:** `{"model": "openrouter/fusion", "plugins": [{"id": "fusion",
  "analysis_models": ["z-ai/glm-5.2", "google/gemini-3.5-flash",
  "moonshotai/kimi-k2.7"], "model": "z-ai/glm-5.2"}], "messages": [...]}`.
  (Docs examples prefix model ids with `~`; the exact id-syntax variant is confirmed
  against a live id list at impl time -- a one-line config value, not a code change.)
- **Web search/fetch** is auto-enabled for the panel; no extra config.
- **Response:** the judge's synthesized answer is in `choices[0].message.content`
  (embedded text, not a separate field); `usage` reports the summed cost.
- **Pricing:** roughly 4-5x a single completion (N panel calls + 1 judge). On this
  cheap panel that is still ~$0.10-0.15 per call.

## 6. Cost model (why this is affordable)

OpenRouter prices (June 2026, $/M in/out): GLM 5.2 0.95/3.00, Gemini 3.5 Flash
1.50/9.00, Kimi K2.6 0.66/3.41 -- all ~10-25x cheaper on output than Opus-class
(~75/M). A Fusion call (~20K context x 3 panel + judge, ~2K out each) lands around
$0.10-0.15. Reached only after Claude (3) and Codex (2) failed, capped at
`FUSION_MAX_CALLS` per run, it is a rare spend that frequently *saves* money by
ending an Opus loop that would have cost more.

## 7. Triggers (auto-detect both)

- **Stuck-solver:** `fusion_stuck_detect.py` (PostToolUse on Bash) increments
  `consecutive_failures` when a build/test/smoke command exits nonzero on the same
  target; resets on success or target change. At `FUSION_STUCK_THRESHOLD` (3) it
  emits a systemMessage telling the loop to invoke `ladder.py`, which runs Codex
  (2) then, if still stuck, Fusion (1).
- **High-risk reviewer:** `review-todo-section`, for sections touching flagged
  paths (SMP/lock-order, boot ABI, security), runs the Fusion review tier as a
  supplement after the normal Codex review -- budget-gated, fail-open.

## 8. Out of scope / deferred

- Auto-tuning the budgets from observed hit-rates (manual config for now).
- A Fusion tier for non-kernel/low-stakes work (kept to stuck + high-risk only).
- Streaming/partial Fusion output (one synthesized result is enough).

## 9. Success criteria

- With Fusion disabled (default): the ladder stops at Codex; zero behavior change;
  no network calls; all existing tests green.
- `fusion-escalate.py` fails open on a missing key / mocked API error -> returns
  "unavailable", exit 0, runner continues (covered by offline unit tests that mock
  the HTTP layer).
- Budget cap enforced: the (N+1)th Fusion call in a run is skipped + logged.
- `.fusion/secret` is gitignored and never appears in `git status`/a commit.
- Stuck-detect increments on repeated same-target failure and resets on success;
  the threshold systemMessage fires exactly at 3.
- One end-to-end dry-run (mocked Fusion response) shows Claude->Codex->Fusion tier
  progression with each rung gated on the previous.
