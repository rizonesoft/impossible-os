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
Tier 2  Fusion assist        ensemble apex (GLM 5.2 + Kimi K2.7-code + DeepSeek V4
                             Pro, GLM judge; main thread = review layer) -- 1 call
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
3. **Two-layer spend cap.** (a) Per-run counter in
   `.claude/state/fusion-budget.json` (default `FUSION_MAX_CALLS=3`); over cap ->
   skip + log. (b) **Live balance floor:** before each Fusion call,
   `GET https://openrouter.ai/api/v1/credits` and skip if
   `total_credits - total_usage < FUSION_MIN_CREDITS` (default e.g. $2). The exact
   per-call cost comes from the completion's `usage` field (recorded in the dataset
   log, not estimated). **Fail-open for the runner, fail-CLOSED for spend:** if the
   balance check errors or returns unknown, the Fusion call is SKIPPED (never spend
   blind) while the runner continues its normal path.
4. **Bounded input.** Each Fusion/Codex call sends a focused problem brief + a
   capped slice of relevant snippets, never the whole codebase -- bounds input cost.
5. **Fail-open on EVERYTHING.** Missing key, network error, API 4xx/5xx, timeout
   (bounded by a HARD SIGALRM total-duration cap, default 600 s -- urllib's own
   `timeout` is per-read, not total; a 12+ min stall was observed before this cap
   was added), malformed response, budget exhausted -> the caller returns a
   clear "unavailable" result and the runner CONTINUES its normal path (keep
   debugging with Claude; the high-risk review already ran its normal passes --
   Fusion is always supplemental). It NEVER blocks the pipeline and NEVER crashes.
6. **Dataset-grade logging (for evals + future distillation).** Every escalation
   -- both Codex assist and Fusion -- appends a JSONL record to
   `.fusion/dataset.jsonl` (gitignored, local only, never committed):
   `{ts, tier, mode, models, problem_brief (input), output (synthesis),
   cost (exact, from the completion `usage` field), outcome}`. `outcome` is mode-specific: for `stuck`, whether the
   next build/test/smoke on that target passed after the assist was applied
   (`resolved`/`unresolved`/`unknown`); for `review`, the triage disposition of the
   findings (counts of Fix/Reject/Accept), or `unknown`. This seeds (a) evals to
   measure whether runner changes actually help, (b) later distillation of the
   cheap subagent/escalation tiers into smaller/local models, (c) later fine-tuning
   of the cheap panel models on this codebase. It is explicitly NOT for fine-tuning
   the main loop -- Claude is not fine-tunable.
7. **Data leaves the trust boundary (operator decision).** Enabling Fusion
   transmits the sent context to OpenRouter and the three external panel providers
   (Zhipu, Moonshot, DeepSeek) + OpenRouter's auto web-search (Google). The judge is
   GLM (Zhipu, already in the panel -- no extra provider); Anthropic sees only
   Fusion's OUTPUT, via the main-thread review, and already sees the code (Claude
   Code). The caller sends BOUNDED context only
   (`--context-file` capped at 60 KB; never whole files / the tree); the crossing is
   documented in `.fusion/README.md`. In interactive auto-mode a source-to-external
   crossing is correctly hard-blocked until approved; the headless run
   (bypassPermissions) is gated by `FUSION_ENABLED` + the spend caps instead.

## 4. Components

```
.fusion/                         (self-contained Fusion module)
  fusion-escalate.py   caller -- Python stdlib (urllib+json); env+budget gated; fail-open
  ladder.py            Claude->Codex->Fusion controller: tier state + per-tier budgets
  config.toml          panel/judge model ids + budgets (3/2/1) + caps -- tracked
  secret               OPENROUTER_API_KEY -- GITIGNORED, never committed
  dataset.jsonl        escalation in/out + resolution outcome -- GITIGNORED (eval/distill seed)
  jobs/<id>.{request,result,meta}   async Fusion job queue (Plan 2) -- GITIGNORED runtime state
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

> **IMPLEMENTATION NOTE (2026-06-27, supersedes the plugin design below).** Four
> rounds of live testing (see `.fusion/README.md`) proved the `openrouter/fusion`
> plugin is the WRONG tool: its auto web-search anchors the panel onto hallucinated
> errata, it reports only a summed cost (no per-model metrics), and its judge is
> opaque. Fusion is therefore built as a **DIY ensemble**: each panel model called
> SOLO + in parallel, NO web-search, then an Opus discard-judge synthesizes (credits
> unique-correct insights, throws out fabricated errata/microcode/MSRs), fed the
> Opus (Tier 0) + Codex (Tier 1) analyses via `escalate(prior=...)`. Shipped panel:
> GLM 5.2 + DeepSeek V4 Pro + Kimi K2.7-code + Grok-build (+ MiniMax-M3 + Qwen3-Max
> on probation). The plugin wire-shape below is retained only as historical record.

- `analysis_models` (panel): `z-ai/glm-5.2`, `moonshotai/kimi-k2.7-code`,
  `deepseek/deepseek-v4-pro` -- three strong, architecturally-diverse coders, none
  of them a ladder model (Claude/GPT). Diversity from what already failed is the
  point. IDs verified against OpenRouter 2026-06-27: an invalid id (the earlier
  `kimi-k2.7`) is SILENTLY dropped, shrinking the panel -- the first smoke test ran
  a 2-model panel before this fix.
- `model` (Fusion's built-in judge): `z-ai/glm-5.2` -- a CHEAP in-Fusion
  synthesizer, NOT Opus. We already pay for Opus in the main Claude thread, which
  reads Fusion's output and validates it before acting (the WS8 trust contract);
  that main-thread review IS the Opus-grade judge, for free. Paying OpenRouter for a
  duplicate Opus judge was paying twice (~43% of the measured call cost).
### Verified OpenRouter Fusion wire shape (docs, 2026-06-27)

- **Endpoint:** `POST https://openrouter.ai/api/v1/chat/completions`.
- **Auth:** `Authorization: Bearer <contents of .fusion/secret>`.
- **Body:** `{"model": "openrouter/fusion", "plugins": [{"id": "fusion",
  "analysis_models": ["z-ai/glm-5.2", "moonshotai/kimi-k2.7-code",
  "deepseek/deepseek-v4-pro"], "model": "z-ai/glm-5.2"}], "messages": [...]}`.
  (Docs examples prefix model ids with `~`; the exact id-syntax variant is confirmed
  against a live id list at impl time -- a one-line config value, not a code change.)
- **Web search/fetch** is auto-enabled for the panel; no extra config.
- **Response:** the judge's synthesized answer is in `choices[0].message.content`
  (embedded text, not a separate field); `usage` reports the summed cost.
- **Pricing:** roughly 4-5x a single completion (N panel calls + 1 judge). On this
  panel that is still ~$0.50-0.70 per call (measured: a real apex call with the
  auto web-search and a ~50KB synthesis cost $0.61 on 2026-06-27).

## 5b. Async job-queue model (the runner's Fusion path) -- Plan 2

A hard total-duration cap stops the runner hanging, but a genuinely hard question
(heavy web-search + long deliberation) can legitimately need 10-30 min -- exactly
when Fusion is most valuable, and exactly when a *synchronous* cap would fail-open
and give nothing. The fix is NOT a bigger synchronous cap; it is **async dispatch
reusing the existing deferral machinery**. The reframe that makes this work: the
overnight runner is a QUEUE / fixpoint loop, not a single-problem solver, so being
stuck on one section almost never means there is nothing else to do.

### Async Fusion IS a special deferral

The runner already has machinery for "cannot finish this now": mark the section
`[/]` + a Deferred stamp + an XREF, advance to the next section/file, re-examine on
the next fixpoint pass. Async Fusion reuses exactly that:

1. The ladder exhausts Codex (2 rounds) -> dispatches Fusion as a DETACHED OS
   process writing to `.fusion/jobs/<id>.result`, and defers the section: `[/]` +
   "awaiting Fusion job `<id>`".
2. The runner ADVANCES -- works other sections / other TODO files (the queue).
3. On a later pass, BEFORE re-working a fusion-deferred section, the controller
   checks the result file: ready -> un-defer and the MAIN THREAD reviews + applies
   it (the WS8 review layer); not ready -> leave it deferred, keep going.

So there is NO new wait-loop -- the fixpoint loop's normal next-pass cadence is the
polling. This decouples *how long Fusion may THINK* from *how long the runner WAITS
in line*.

### "What if Opus AND Codex are stuck and they NEED the Fusion result to carry on?"

- **Case A -- other work exists (common):** park the stuck section, do other
  sections/files; Fusion's answer is usually ready within a pass or two. The runner
  stays productive while Fusion deliberates for 20 min. No one waits.
- **Case B -- everything is blocked on it (rare, e.g. a broken build no one can
  fix):** there genuinely is no other productive work, so waiting is FREE -- the
  defer + next-pass (or a bounded poll) holds up nothing, because nothing else could
  have progressed anyway.

The principle: async does NOT mean "never wait"; it means "never wait IN-LINE
holding up other work." When there is no other work, the wait is costless; when
there is, you do not pay it.

### Durability (why async is *better*, not just cheaper)

Because the Fusion job is a DETACHED OS process writing a result file, it SURVIVES
the main loop's own lifecycle: if the Claude run compacts or the watchdog relaunches
it mid-deliberation, the Fusion process keeps running and the `.result` persists. On
relaunch the runner reads its state ("section X awaiting job `<id>`"), finds the
result, and applies it. A SYNCHRONOUS 20-min call would be lost the instant the main
loop hit a usage limit or compaction; the async job cannot be. This is the real
reason async is correct for an unattended runner.

### Bounds (a hung job cannot wedge the runner)

- The detached job has its OWN ceiling (async cap, e.g. 1800 s); on timeout it
  writes a `failed`/`timeout` result. The controller treats that section as
  genuinely blocked (defer with the captured diagnostic = a human punch-list item).
- If Fusion returns but does NOT break the impasse across N passes, the fixpoint's
  existing "identical failure across 3 passes -> Run Log punch-list" rule takes
  over. So "Opus + Codex + Fusion all failed" has a defined terminal: park, log for
  the human, move on. The run never wedges.

### State + situational-awareness tie-in

- Job dir: `.fusion/jobs/<id>.request` (the brief), `.fusion/jobs/<id>.result`
  (written on completion), `.fusion/jobs/<id>.meta` (status + which section it is
  for). Gitignored (local runtime state, like `dataset.jsonl`).
- The situational brief (`runner-status`) gains a line -- "N Fusion jobs pending" --
  so the loop KNOWS it has outstanding async work to collect each pass. The
  ladder controller and the awareness layer meet here.

### Synchronous path is retained for interactive use

The 600 s SIGALRM cap bounds the SYNCHRONOUS inline path (a human-watched
interactive / `debug-session` / quick check, where blocking briefly is fine). The
async job-queue path is specifically for the unattended runner.

## 6. Cost model (why this is affordable)

OpenRouter prices (June 2026, $/M in/out): GLM 5.2 0.95/3.00, Kimi K2.7-code
~0.95/4.00, DeepSeek V4 Pro 0.435/0.87. Measured breakdown of the 2026-06-27 smoke
test ($0.61 total, run with an Opus judge): Opus judge $0.248 (43%), auto web-search
$0.174 (30%, billed as Gemini Flash), GLM $0.151 (26%), DeepSeek $0.008 (1%).
Swapping the judge to GLM removes the 43% Opus line -- and the main Claude thread,
which we already pay for, is the real review layer (it reads + validates Fusion's
output before acting anyway, per the WS8 trust contract), so Opus-grade judgment
stays free. Expected new cost ~$0.30-0.40. The auto web-search (~30%) is the
next-biggest line and is not currently disableable. Because Fusion is the rare apex
tier (after Claude (3) + Codex (2) failed, capped at `FUSION_MAX_CALLS`), it still
frequently *saves* money by ending an Opus loop that would have cost more.

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
- Balance floor enforced: with a mocked `/credits` response below
  `FUSION_MIN_CREDITS`, the Fusion call is skipped; with the balance check mocked to
  error, the call is also skipped (fail-closed on spend) while the runner continues.
- `.fusion/secret` AND `.fusion/dataset.jsonl` are gitignored and never appear in
  `git status` / a commit.
- Each escalation appends one `dataset.jsonl` record carrying the problem brief,
  the synthesis output, and a resolution outcome (resolved/unresolved/triage
  counts/unknown) -- verifiable on a mocked dry-run.
- Stuck-detect increments on repeated same-target failure and resets on success;
  the threshold systemMessage fires exactly at 3.
- One end-to-end dry-run (mocked Fusion response) shows Claude->Codex->Fusion tier
  progression with each rung gated on the previous.
