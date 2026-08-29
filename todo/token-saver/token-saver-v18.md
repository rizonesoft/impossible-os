# Token Saver v18 -- Cost Findings (opened 2026-08-29)

Cost and token findings from the run armed after the 2026-08-29 close-out of [v17](token-saver-v17.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

---

## The open question is retired; the close-out digest replaces it

v11 through v17 asked whether a gate-free cost finding exists. v17 answered it by measurement: pointing `overnight-log-explorer` at one full transcript found ungated waste at **2.0-2.3% (12-14 of 602 calls)** on `run-20260826-160133`, i.e. real, borderline, and concentrated in three self-diagnosed tooling-shape incidents rather than in re-reads or orientation churn. Two of the three were fixed at the same close-out. So the class exists at the bar, a run cannot see it, and a close-out can, for one Sonnet dispatch. That is now a standing close-out step (below), not a question to restate.

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Close-out transcript digest.** At every close-out, point `overnight-log-explorer` at the largest transcript of the cycle and record: total tool calls, ungated-waste share, and the top three contributors with line numbers. BASELINE: 602 calls, 2.0-2.3% ungated, contributors = background-subshell false start (6 calls), jobId-derived poll path (2 incidents, one ~18 min re-dispatch), one worktree-guard false positive. Two of three fixed; the number to beat is 6 calls and zero re-dispatches.
- **Does the `( cmd ) & echo started` false start recur?** The sequencer skill already documents the tracked-background shape; the run re-learned it mid-session after paying 6 calls. Measure per segment: polls of an empty log before the run switches shape. Target 0.
- **Did the v16 stamp-attribution fix remove real dispatch waste?** BASELINE: one v16 section paid ~20 minutes and 7 extra Codex dispatches re-running reviews already performed. v17 cycle: not measured per section; the opt-out counts in `overnight-runner-improvements-v18` (21 `SKIP_REVIEW_HOOK=1`) suggest the stamp path is still being routed around. Measure: extra dispatches per section attributable to a stale-stamp refusal.
- **The pre-push tooling suite receipt.** Measured 2026-08-09: 44s on a valid receipt, 95-99s stale, 10-minute wall kill without. Measure: fraction of ship pushes hitting a valid receipt, and total wall-clock saved. v17 cycle: 6 `SKIP_TOOLING_SUITE=1` opt-outs on pack-only refusals, each one a pack (~800s) that produced no receipt.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Measure: contention frequency and seconds waiting.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10: 3 operator commits forced `j1a -> j1b -> j1c`, ~17 minutes paid twice. Measure per attended session.
- **Backgrounded ship push.** Measure poll iterations and tokens per ship against the previous inline cost; the ship sequence now ends with a `git log @{u}..HEAD` check, so a push that never landed is no longer invisible.
- **Agent-result cache hit rate.** Baseline still needed: `cache-hit` entries versus total dispatches per segment. The v17 transcript had ZERO agent dispatches in 16 hours, so the cache had nothing to hit; whether the offload doctrine is being followed at all is the prior question.
- **Segment-start orientation cost.** BASELINE from the v17 digest: ~1 minute and 17 calls from launch to first section edit on `run-20260826-160133`. Measure per relaunch.
- **Cost of a re-verified finding versus a copied one.** The v17 close-out re-verified every count it recorded (the digest's 602/499/41/29 were re-grepped by the main session and matched exactly); the habit cost one Bash call and changed no verdict this time. Keep measuring how often a re-check changes a filed verdict.
- **Unbounded per-event state walks.** `skill-progress.json` is append-only per event; any `for k,v in d.items()` probe over it grows without bound. Measure: whether any single probe's output exceeds a few hundred lines.

## Found live this cycle

<!-- The run files here. Nothing yet: v18 opened at close-out, before the next arm. -->
