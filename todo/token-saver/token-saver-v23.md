# Token Saver v23 -- Cost Findings (opened 2026-11-07)

Cost and token findings from the run armed after the 2026-11-07 close-out of [v22](token-saver-v22.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

**Prices.** Opus 5.5 and Sonnet 5.5 both bill cache reads at $0.20/MTok; they differ on cache writes and output (Opus 2x). Moving work to a cheaper model saves only on writes and output, so context weight is usually the bigger lever.

---

## Carried forward from v22

- [ ] CONTEXT GROWTH is the dominant run cost: settle the two levers with an armed A/B, not arithmetic ([v20](token-saver-v20.md), [v22](token-saver-v22.md))
  - BASELINE (v20, 23 sections of 20+ turns): context floor median 71K tokens, p50 312K, p90 395K, max 418K, per-turn peaks. v22 logged only end-of-segment: p50 231K, p90 378K, max 532K over 20 segments, not comparable.
  - Levers: (a) trim `CLAUDE.md`; (b) rotate earlier. Settled by one cycle with a rotation threshold near 250K, compared on cost per shipped section.
- [ ] `general-purpose` and `Explore` still inherit the session model; each needs its own replay task set before a Sonnet pin ([v22](token-saver-v22.md))
  - `agent-replay.py` has no builder for either. The run dispatched neither, so the saving is interactive only.
- [ ] Does medium effort hold quality? Writer and all 21 agents moved to medium by operator decision on 2026-11-06 ([v22](token-saver-v22.md))
  - v22 shipped no sections and dispatched no `kernel-quality-auditor`, so there is no comparison yet. Compare findings per section and post-ship regressions against the v21 baseline once sections ship.

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Cost per shipped section.** BASELINE (v22): undefined, 0 sections shipped. Per segment the run cost $9.99 to $95.23.
- **Subagent starting context.** BASELINE: every named agent started at ~43-46K tokens; a fresh `git-historian` with `omitClaudeMd: true` started at 4.5K. The 14 lean agents should sit near 5K, the 7 doctrine agents near 44K.
- **Agent-result cache hit rate.** BASELINE (v22): 3 main-loop dispatches, 0 hits, 2 stores (v21: 7 and 0). Still unproven.
- **Fork use.** BASELINE (v22): 0 forks, 0 `[FORK-CONTEXT]` refusals.
- **Main-loop searching, by tool.** BASELINE (v22, first position, approximate): 569 of 1,560 calls (36.5%, v21 27%): Bash forms 462, `Read` 107, `Grep` 0. Measure whether Bash forms return more output per call before filing anything.
- **Refused calls, main loop.** BASELINE (v22): 27 of 1,560 (1.7%); `receiving-review-required` 10.
- **Codex outage refusals.** v22: 0 outages, so the gate was not exercised. Measure dispatches refused per outage (target 1-2, baseline 5).
- **Receipt route versus flake fixes.** Receipt-route pushes per cycle should approach 0 now that a baseline-only push no longer triggers the pack.

## Found live this cycle

