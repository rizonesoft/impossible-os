# Token Saver v20 -- Cost Findings (opened 2026-09-28)

Cost and token findings from the run armed after the 2026-09-28 close-out of [v19](token-saver-v19.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

---

## Carried forward from v19

Nothing: v19's single item was rejected below the 2% bar.

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Close-out transcript digest.** At every close-out, point `overnight-log-explorer` at the largest transcript of the cycle and record: total tool calls, ungated-waste share, and the top three contributors with line numbers. BASELINE (v19 close-out: ~3.9% ungated on the 970-call transcript; v18: 5.2%), and the v18 close-out reading follows): 387 calls, ~5.2% ungated, contributors = symbol re-derivation churn (12 calls), self-inflicted tool errors and retries (~5 calls: a malformed Read input, a Grep on a nonexistent path, a heredoc-edit assertion), duplicate reads (3). Previous: 602 calls, 2.0-2.3%. The number to beat is 20 calls; ALSO report the symbol-derivation-chain count asked for by the item above.
- **Does the `( cmd ) & echo started` false start recur?** v18 cycle: 0 in three transcripts (all pushes used the tracked shape). Keep at 0. The RELATED shape to count now is poll renewals per background job (baseline 3 for one tooling pack).
- **Did the v16 stamp-attribution fix remove real dispatch waste?** v18 cycle: yes as far as one section shows -- the TODO-12 s11 review ran 6 dispatch calls / 9 legs with every re-dispatch preceded by a `review_convergence.py should-redispatch` check and following a fix; 0 unchanged-leg re-dispatches. Corpus-wide: 77 legs over 11 sections in the cycle window. Keep measuring per section.
- **The pre-push tooling suite receipt.** v18 cycle: 2 pack-only refusals, both answered with the receipt route (~14-15 minutes each, 0 `SKIP_TOOLING_SUITE=1`). Measure: fraction of ship pushes hitting a valid receipt, and total wall-clock saved.
- **The suite LOCK's wait cost.** `test-tooling.sh` holds a per-worktree flock. Measure: contention frequency and seconds waiting.
- **J1 re-runs caused by attended commits.** v18 cycle: 1 refused rollover (`run-20260830-035742.log:633-635`, smoke receipt invalidated). Measure per attended session.
- **Backgrounded ship push.** v18 cycle: 3 pushes in the digested transcript, 1 poll each on two of them, 3 calls on the third (hook block). Measure poll iterations per ship; the `git log @{u}..HEAD` check stays.
- **Agent-result cache hit rate.** BASELINE: 21 dispatches in the v18 cycle window, 0 `cache-hit` lines. The offload doctrine IS being followed now (kernel-quality-auditor x2, boot-quality-auditor, concurrency-evidence-mapper x2, kernel-explorer x3, doc-sync-auditor x2, todo-hygiene-auditor, parity and spec researchers), so the cache finally has dispatches to hit; a repeat dispatch over unchanged content should show one. Measure: `cache-hit` entries versus total dispatches per segment.
- **Segment-start orientation cost.** BASELINE unchanged: ~1 minute and 17 calls from launch to first section edit on `run-20260826-160133`. v18 measurement (59 calls / 5m04s) was a FILE_CLOSE segment and is not comparable; measure on a segment that opens on a section pickup.
- **Cost of a re-verified finding versus a copied one.** v18 close-out: three digest counts re-grepped by the main session, all matched (387 / 12 / 4); one Bash call, no verdict changed. Keep measuring how often a re-check changes a filed verdict.
- **Unbounded per-event state walks.** `skill-progress.json` is append-only per event; any `for k,v in d.items()` probe over it grows without bound. Measure: whether any single probe's output exceeds a few hundred lines.
- **Receipt route versus flake fixes.** The v19 close-out removed the three timing flakes that forced most receipt-route pushes (~14-15 min each). Measure: receipt-route pushes per cycle should approach 0.

## Found live this cycle

<!-- The run files here. Nothing yet: v20 opened at close-out, before the next arm. -->
