# Token Saver v22 -- Cost Findings (opened 2026-11-06)

> **CLOSED 2026-11-07. Successor: [v23](token-saver-v23.md).** 4 filed: 0 resolved, 1 rejected, 0 not reproduced, 3 carried. The combined split is in [improvements v22](../overnight-runner-improvements/overnight-runner-improvements-v22.md).

Cost and token findings from the run armed after the 2026-11-06 close-out of [v21](token-saver-v21.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

**Prices.** Opus 5.5 and Sonnet 5.5 both bill cache reads at $0.20/MTok; they differ on cache writes and output (Opus 2x). Moving work to a cheaper model saves only on writes and output, so context weight is usually the bigger lever.

---

## Carried forward from v21

- [ ] CONTEXT GROWTH is the dominant run cost: settle the two levers with an armed A/B, not arithmetic ([v20](token-saver-v20.md), [v21](token-saver-v21.md))
  - **VERDICT** CARRIED: still needs an armed cycle with a rotation threshold near 250K. New data point, not a peak: end-of-segment context over 20 segments read p50 231K, p90 378K, max 532K. v20's figures are per-turn peaks, so the two are not comparable. Settled by that A/B on cost per shipped section, which this cycle could not give because it shipped no sections.
  - BASELINE (v20, 23 sections of 20+ turns): context floor median 71K tokens, p50 312K, p90 395K, max 418K, resident share 24%; estimated main-loop spend ~$302 at list prices, 63% cache reads, 22% writes, 15% output.
  - Levers: (a) trim `CLAUDE.md` (~23K of the 71K floor; subagents already skip it where safe); (b) rotate earlier, trading p90 context against the re-orientation cost of a relaunch. Settled by one cycle with a rotation threshold near 250K, compared on cost per shipped section.
- [ ] `general-purpose` and `Explore` still inherit the session model; each needs its own replay task set before a Sonnet pin ([v20](token-saver-v20.md) model migration, [v21](token-saver-v21.md))
  - **VERDICT** CARRIED: each still needs a replay task builder before a pin. The run dispatched neither (3 agent dispatches in 21 logs), so the saving remains interactive only. Settled by building the two builders.
  - `agent-replay.py` has builders only for `kernel-quality-auditor` and `git-historian`. `Explore` needs locate-a-symbol tasks with a grep oracle; `general-purpose` needs tasks built from real interactive dispatch shapes. The run dispatched neither in v20 or v21, so the saving is interactive only.
- [-] Haiku 5.5 candidate: `todo-validation-mapper` only, on its own replay PASS and only if its job proves near mechanical ([v21](token-saver-v21.md))
  - **VERDICT** REJECTED: below the depth it warrants. 0 dispatches of `todo-validation-mapper` this cycle, and scripts already own its mechanical checks, so a replay builder would cost more than the move could save.
  - It needs a task builder with a deterministic oracle first, and scripts already do the purely mechanical checks, so it may not qualify. `gh-query-runner` moved at the v21 close-out (20/20 on both; $0.23 against $4.10). `git-historian` stays on Sonnet by operator decision: its replay passed (20/20; $0.23 against $4.30) but covers only commit lookup, not regression judgment. `xref-dependency-mapper` and `ssdt-auditor` were dropped: a miss there is not caught by verifying what they report. Haiku is for near-mechanical jobs only; a replay pass alone does not move an agent.
- [ ] Does medium effort hold quality? Writer and all 21 agents moved to medium by operator decision on 2026-11-06
  - **VERDICT** CARRIED: every segment ran at medium, but the comparison needs findings per shipped section, and 0 sections shipped. `kernel-quality-auditor` had 0 dispatches, so the watch item got no data. Settled by the first cycle that ships sections.
  - Compare findings per section and post-ship regressions against the v21 baseline. Metrics records carry the effective effort. The `kernel-quality-auditor` on Opus at medium is the one to watch first, since it is the sole SMP net among the agents.

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Subagent starting context.** BASELINE: every named agent started at ~43-46K tokens; a fresh `git-historian` with `omitClaudeMd: true` started at 4.5K. Measure: median first-call context per agent type from the subagent transcripts; the 14 lean agents should sit near 5K, the 7 doctrine agents near 44K.
- **Agent-result cache hit rate.** BASELINE (v21): 7 main-loop dispatches, 0 `cache-hit` lines (v20: 97 and 0). Dispatches fell because the v21 cycle used the section pack; the hit rate is still unproven. If still 0 across a mapper-heavy cycle, look at whether the mapper's canonical scope ever repeats.
- **Fork use.** BASELINE (v21 run): 0 forks, 0 `[FORK-CONTEXT]` refusals. Interactive forks from Opus contexts measured ~700K start, ~$8 each. Count refusals and forks that passed with `[fork-ok`.
- **Main-loop searching, by tool.** BASELINE (v21, commands in first position only, so a floor): 595 search calls in 2,202 main calls; Bash `sed` 235, `grep` 205, `cat` 56, `ls` 20, against 79 `Read` and 0 `Grep` (v20: 1,338 in 3,539). Measure whether Bash forms return more output per call before filing anything.
- **Refused calls, main loop.** BASELINE (v21): 58 of 2,202 (2.6%); `receiving-review-required` 23 against a target of < 20.
- **Codex outage refusals.** v21: 0 outages, so the gate was not exercised. Measure dispatches refused per outage (target 1-2, baseline 5).
- **Close-out transcript digest.** Largest v21 transcript `run-20260930-065129.log`: 510 calls (497 main), 7 refusals, top contributors `sed` 41, `grep` 38, `Skill` 25. The ungated-waste share is NOT measurable from these logs, which carry no such label, so the v19 ~3.9% has no successor figure. Define the measure before quoting it again.
- **Backgrounded ship push.** The loop's iterations are not logged, so per-ship polling is not measurable; v21 shows one backgrounded call per push. Replace this with a measure the logs can answer, or drop it.
- **Receipt route versus flake fixes.** Receipt-route pushes per cycle should approach 0 now that a baseline-only push no longer triggers the pack.

## Found live this cycle

<!-- The run files here. Nothing yet: v22 opened at close-out, before the next arm. -->
