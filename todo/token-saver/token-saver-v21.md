# Token Saver v21 -- Cost Findings (opened 2026-09-29)
> **CLOSED 2026-11-06.** Superseded by [v22](token-saver-v22.md). Of the **4** items here, **0 resolved, 1 rejected, 0 not reproduced, 3 carried**. Carried exceeds resolved plus rejected in this file, and all three need a paid replay or an armed cycle rather than a code change.


Cost and token findings from the run armed after the 2026-09-29 close-out of [v20](token-saver-v20.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

**Prices changed.** Opus 5.5 and Sonnet 5.5 both bill cache reads at $0.20/MTok; they differ on cache writes and output (Opus 2x). Moving work to a cheaper model now saves only on writes and output, so context weight is usually the bigger lever.

---

## Carried forward from v20

- [ ] CONTEXT GROWTH is the dominant run cost: settle the two levers with an armed A/B, not arithmetic ([v20](token-saver-v20.md))
  - BASELINE (v20, 23 sections of 20+ turns): context floor median 71K tokens, p50 312K, p90 395K, max 418K, resident share 24%; estimated main-loop spend ~$302 at list prices, 63% cache reads, 22% writes, 15% output.
  - Levers: (a) trim `CLAUDE.md` (~23K of the 71K floor; subagents already skip it where safe); (b) rotate earlier, trading p90 context against the re-orientation cost of a relaunch. Settled by one cycle with a rotation threshold near 250K, compared on cost per shipped section.
  - **VERDICT (2026-11-06 close-out):** CARRIED: needs an armed cycle at a rotation threshold near 250K, which this stop cannot supply. No A/B ran in v21.
- [ ] `general-purpose` and `Explore` still inherit the session model (Opus from an Opus session); each needs its own replay task set before a Sonnet pin ([v20](token-saver-v20.md) model migration)
  - `agent-replay.py` has builders only for `kernel-quality-auditor` and `git-historian`. `Explore` needs locate-a-symbol tasks with a grep oracle; `general-purpose` needs tasks built from real interactive dispatch shapes (the retained transcripts show TODO-versus-design audits). The run itself dispatched neither in v20.
  - **VERDICT (2026-11-06 close-out):** CARRIED: 0 dispatches of either in the 9 v21 logs, so the gain is interactive only and no task builders exist. Settled by building the two replay task sets.
- [ ] Haiku 5.5 candidates, each only on its own replay PASS, once that model exists ([v20](token-saver-v20.md) model migration)
  - The current Haiku is 4.5. Candidates: `git-historian` (20 tasks built), `gh-query-runner`, `todo-validation-mapper`, `xref-dependency-mapper`, `ssdt-auditor`; the last four still need task builders with deterministic oracles.
  - **VERDICT (2026-11-06 close-out):** CARRIED for the other four. The blocker cleared: the `haiku` alias now resolves to `claude-haiku-5-5` (checked with one call). `git-historian` ran its 20 tasks on both models at this close-out: Sonnet 20/20, $4.30, 305s; Haiku 20/20, $0.23, 221s; `agent-replay.py compare` PASS. NOT pinned, by operator decision: Haiku is for near-mechanical work and must not downgrade quality, and these tasks test only "which commit touched this file", not the "when did this break" judgment the agent is also dispatched for. `gh-query-runner` is near mechanical, gained a builder from finished CI runs (8 failure digests marked high, 12 commit lookups), and passed: Sonnet 20/20, $4.10; Haiku 20/20, $0.23. It IS pinned to `haiku` at the close-out commit, the only agent moved, by operator decision. `todo-validation-mapper` stays open pending a builder; `xref-dependency-mapper` and `ssdt-auditor` were dropped as candidates because their misses or status judgments are not caught by verification.

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Subagent starting context.** BASELINE: every named agent started at ~43-46K tokens; a fresh `git-historian` with `omitClaudeMd: true` started at 4.5K. Measure: median first-call context per agent type from the subagent transcripts; the 14 lean agents should sit near 5K, the 7 doctrine agents near 44K.
- **Agent-result cache hit rate.** BASELINE (v20): 97 main-loop dispatches, 94 of them `section-context-mapper`, 0 `cache-hit` lines. Measure `cache-hit` against dispatches per segment; if still 0 across a cycle of mapper-heavy docs sections, look at whether the mapper's canonical scope ever repeats.
- **Fork use.** BASELINE (v20 run): 0 forks. Interactive forks from Opus contexts measured ~700K start, ~$8 each; `fork_context_guard.py` now refuses them above 150K. Count `[FORK-CONTEXT]` refusals and forks that passed with `[fork-ok`.
- **Main-loop searching, by tool.** BASELINE (v20): 1,338 search calls, 3 runs of 10+; by tool, Bash `sed` 568, `grep` 416, `cat` 110, `ls` 71 against 68 `Read` and 1 `Grep`. The doctrine prefers the Grep and Read tools; measure whether Bash forms return more output per call before filing anything.
- **Refused calls, main loop.** BASELINE (v20): 72 of 3,539 (2.0%); `receiving-review-required` 23 against a target of < 20.
- **Codex outage refusals.** v20: 0 outages, so the gate was not exercised. Measure dispatches refused per outage (target 1-2, baseline 5).
- **Close-out transcript digest.** At every close-out, record total tool calls, ungated-waste share and the top three contributors for the largest transcript of the cycle. BASELINE: v19 ~3.9% ungated on 970 calls.
- **Backgrounded ship push.** Poll iterations per ship; the `git log @{u}..HEAD` check stays.
- **Receipt route versus flake fixes.** Receipt-route pushes per cycle should approach 0 now that a baseline-only push no longer triggers the pack.

## Found live this cycle

<!-- The run files here. Nothing yet: v21 opened at close-out, before the next arm. -->
- [-] A confirming three-leg Codex wave is required after every staged byte change, so a one-line audit tweak late in review costs three dispatches and about eight minutes (TODO-10 section 31, 2026-09-30)
  - Observed: the post-ship review of section 31 ran four confirming waves (12 dispatches) after the main review approved; three were for single-function changes in `scripts/site/a11y/audit.mjs` (a dev-only audit script, not shipped code) that the prior wave's own findings prompted.
  - Mechanism: the section-commit gate binds each kind's receipt to the staged content, so any edit re-stales all three legs; `review_convergence.py` treats `scripts/` as outside every kind's scope and cannot say CONVERGED.
  - Measured cost: 12 of 59 dispatches (20%) for the section, about 32 minutes wall-clock.
  - Idea, not applied (control plane): let a leg that APPROVED keep its receipt across a later change confined to files it was not asked about, e.g. perf and consistency across an audit-script-only edit.
  - **VERDICT (2026-11-06 close-out):** REJECTED. Half the mechanism does not reproduce: `review_convergence.py` has scoped `scripts/` for every kind since 2026-08-10 (`test_tooling_sections_are_visible_to_every_kind`), so it declines to say CONVERGED because the bytes changed, not because it cannot see them. The idea of keeping an approved leg's receipt across a later edit weakens content-bound receipts, which took months to stabilise, and a consistency leg's scope is exactly the cross-file effect it would be assuming away. One section's evidence does not justify that. Re-file with a second measurement; a Medium-loop rule is the cheaper lever (see the v22 operator decision).
