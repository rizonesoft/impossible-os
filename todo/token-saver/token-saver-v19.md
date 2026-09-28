# Token Saver v19 -- Cost Findings (opened 2026-09-03)

> **CLOSED 2026-09-28.** Superseded by [v20](token-saver-v20.md). One item filed, REJECTED below the 2% bar (5 of 970 calls, 0.5%). The same transcript measured ~3.9% ungated waste against the v18 baseline of 5.2%.

Cost and token findings from the run armed after the 2026-09-03 close-out of [v18](token-saver-v18.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. The close-out that follows the next stop triages every item, verifies it against the tree as it is then, and records a verdict for every one.

**Why findings land here instead of being fixed.** Cost machinery is control plane (`.claude/hooks/**`, `scripts/overnight/**`) or receipt surface, both off-limits unattended. Record the finding in the same turn it is observed, then advance.

**What to write.** One item per finding: what was observed live (run id, segment, numbers), the mechanism confirmed at source, and the measured or bounded cost. **The >= 2% bar applies.** A projection is not a finding. Left below the bar, record it as excluded rather than as work.

---

## The close-out digest, second reading

The v18 close-out pointed `overnight-log-explorer` at `run-20260829-220355.log` (147 KB, 387 tool calls: Bash 248, Grep 75, Read 37, Skill 12, WebSearch 6, Agent 4, WebFetch 3, Glob 2, Edit 0, Write 0) and measured ungated waste at **~20 calls (5.2%)**, up from 2.0-2.3% on the v17 transcript. The composition changed: no tooling-shape incident this time, and no single mechanism -- the largest block was REASONING (a fact re-derived by symbol-guessing after it had just been read), then self-inflicted errors, then duplicate reads. The main session re-verified the load-bearing counts (387 tool lines; 12 calls at log:61-73; 4 Agent dispatches). Two structural notes from the same digest: every file mutation in the transcript went through a `python3 - <<'PY'` heredoc with an `assert s.count(old)==1` guard (Edit/Write = 0), so Edit-tool string-not-found waste is structurally absent and its analogue is a Python `AssertionError` (one seen, log:625-628); and the segment's first mutation came 59 calls / 5m04s in, but that segment opened on a FILE_CLOSE sweep, not a section pickup, so it is not comparable to the 17-call orientation baseline.

## Found at the 2026-09-03 close-out, not by a run

- [-] A fact already read from CLAUDE.md in the same segment was re-derived by symbol-name guessing: 12 calls, 3.1% of the transcript (`run-20260829-220355.log:61-73`).
  - **VERDICT (2026-09-28 close-out):** REJECTED, below the 2% bar: verified at `ob_file.h` (read at log:188, re-derived at log:217-221), 5 calls of 970, 0.5%. The whole run's ungated waste was ~3.9% against the v18 baseline of 5.2%, which is the number to keep watching.
  - The fact: `VMM_MAX_GUARD_PAGES = 640` at `include/kernel/mm/vmm.h:212`, stated verbatim in the CLAUDE.md Safety Gates bullet the segment read at log:49; the 12 calls were 9 Grep, 1 full-file Read, 2 Bash.
  - OBSERVED: eight Greps across `vmm.h`/`vmm.c`/`src/kernel` with shifting guesses (`guard_table[`, `GUARD_TABLE_SIZE`, `guard_entries[`, `install_guard_page`), a full Read of `vmm.c` (log:68), then two confirming Greps landing on the fact already in hand; 22:07:29 to 22:07:50.
  - This is a reasoning finding with no mechanical fix: the doctrine's routing ladder already says a fact surfaced by a read in the same turn is cited, not re-derived. Above the bar as a single incident; whether it is a CLASS is the question for the next run.
  - Settled by: the next close-out digest reporting the count of symbol-derivation chains longer than 3 calls whose answer was present in a file read earlier in the same segment. Two or more such chains promotes it to a sequencer-skill lesson; zero retires it.
- Excluded (below the bar, recorded so they are not re-filed as new): `section_review_required.py` blocking the push-POLL Bash call after a code-touching commit, worked around with 2 extra calls (`run-20260829-220355.log:414-419`, 0.5%); background-job poll RENEWALS when a job outlives one `timeout 560` window, 3 polls for one out-of-band tooling pack (`run-20260830-035742.log:615-617`) and 2 for its push (log:609,612), ~1.3% of that 300-call transcript; three duplicate or near-duplicate single-shot Greps/Reads (`run-20260829-220355.log:52-57, 84`, 0.8%).

## Standing measurement obligations

Carried with baselines. A measurement without one is an anecdote.

- **Close-out transcript digest.** At every close-out, point `overnight-log-explorer` at the largest transcript of the cycle and record: total tool calls, ungated-waste share, and the top three contributors with line numbers. BASELINE (v18 close-out): 387 calls, ~5.2% ungated, contributors = symbol re-derivation churn (12 calls), self-inflicted tool errors and retries (~5 calls: a malformed Read input, a Grep on a nonexistent path, a heredoc-edit assertion), duplicate reads (3). Previous: 602 calls, 2.0-2.3%. The number to beat is 20 calls; ALSO report the symbol-derivation-chain count asked for by the item above.
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

## Found live this cycle

<!-- The run files here. Nothing yet: v19 opened at close-out, before the next arm. -->
