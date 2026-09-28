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

## Found at the 2026-09-28 attended cost review

- [ ] CONTEXT GROWTH is the dominant run cost: cached re-reads are ~90% of model spend, and the fixed floor is only part of it
  - MEASURED sizes: project `CLAUDE.md` 88,667 bytes (~22k tokens), global `~/.claude/CLAUDE.md` 8,673, plus skill bodies that stay resident once invoked: `overnight-sequencer` 63,576, `implement-todo-section` 44,965, `review-todo-section` 39,594.
  - MEASURED shape of `CLAUDE.md`: Skills 19% (17 KB, mostly the agent table and offload doctrine that the skills repeat), Attended repair 10%, Git Hooks 9% (one 8 KB paragraph of August measurements), North Star 7%, Smoke Test 6%.
  - INFERRED cost: a ~965-call segment (`run-20260903-160406.log`) re-reads ~60k resident tokens per turn, ~58M cached-read tokens per segment, ~5.8M input-token equivalents at the 0.1 cache-read rate. Not yet verified against billed usage: the stream report carries no per-turn usage.
  - CORRECTION 2026-09-28: the first filing said the stream report carries no usage. Wrong: `scripts/overnight/stream-report.py` already writes per-section token totals to `.claude/overnight/metrics/run-*.jsonl`. Read them before estimating.
  - MEASURED from those records, last 30 sections with >= 20 turns: main-loop cache reads 1,981M tokens against 4.5M output; median context per turn ~300k, and TODO-03 section 23 averaged 497k over 509 turns. At the standard 0.1x cache-read and 5x output prices (an assumption, not a billing figure) cache reads are ~90% of the main loop's cost.
  - So the fixed floor (~60k tokens: `CLAUDE.md`, skills) is roughly 12-20% of a turn; the bigger lever is how far context grows before a segment rotates.
  - SHIPPED 2026-09-28 (attended), under test: every section record now carries `context_floor_tokens` (the session's first main turn), `context_min/p50/p90/max/mean_tokens` and `resident_share`; control `test_context_stats_floor_percentiles_and_sidechain_excluded` (a mutation that lets a subagent turn set the floor fails it).
  - NEXT, from that data rather than arithmetic: (a) trim `CLAUDE.md` and split the sequencer skill, judged by `context_floor_tokens` and `resident_share`; (b) rotate or compact earlier, judged by `context_p90_tokens` against the re-orientation cost of a relaunch (~17 calls, `token-saver` v18 baseline).
- [ ] A review wave built from `"$(cat file)"` prompts is paid for twice: the legs run and approve, then are re-dispatched because the hook saw no review kind
  - MEASURED `run-20260905-143453.log:358-376`: 5 dispatches + a 3m32s wait, all five `approve`, stamps still on section 22, then all 5 re-dispatched inline and waited again. 11 calls and one full Codex wave for work worth 6.
  - Same class as the carried attribution item in [overnight-runner-improvements-v20](../overnight-runner-improvements/overnight-runner-improvements-v20.md); the cheap half is a PreToolUse refusal BEFORE the round-trip when a broker/dispatch argument is not a literal that starts with `[review-kind:`. That needs no attribution redesign.
  - SHIPPED 2026-09-28 (attended), under test: `.claude/hooks/review_kind_literal_required.py` refuses exactly what the recorder cannot attribute (same extraction), counting dispatch segments so one bad leg beside a good one is caught; `scripts/overnight/tests/test_review_kind_literal.py`, 15 cases. Measure: refusals per run, and 0 re-dispatched waves.
- [ ] Refused tool calls are 6.3% of the largest transcript's calls, and several classes are predictable before the call is made
  - MEASURED `run-20260903-160406.log`: 61 `tool error:` of ~965 calls; `todo-item-line BLOCK` x8 (an over-cap lead drafted, refused, rewritten), `READ-CACHED` x7, `receiving-review-required` x5, `build-offload` x4 (plus 2 and 2 in the other two logs), `skill-step-block` x3.
  - Fix shape per class: draft TODO leads through `todo_item_line_length.py --check` before the Edit; the build-offload refusal message already names the wrapper, so the recurrence is a habit to put in the skill step that runs builds.
  - FULL BREAKDOWN 2026-09-28, all 21 run logs: 309 of 7,748 calls (4.0%). `receiving-review-required` 85, genuine build/test failures (`Exit code N`) 53, `build-offload` 32, post-ship `review-todo-section REQUIRED` 31, commit gates 18, `todo-item-line` 9, `READ-CACHED` 7, `broker-dispatch-required` 6, the rest small (path typos, blocked sleeps, oversized reads). No `build-offload` false positive reproduced: the truncated log lines each hid a real build later in the command.
  - SHIPPED 2026-09-28 (attended), under test: (a) `review-envelope.py` gains a `next` field and `wait-for-codex-verdict.sh` a `NEXT:` line naming the receive only when the wave is clean (receiving class); (b) `build_offload_reminder.py` REWRITES a plain bare chain via `updatedInput` instead of refusing, self-checked against its own matcher, unusual shapes still BLOCK (build-offload class); (c) `section_review_required.py` lets Bash through once the review is running, and the ship doctrine starts the review before polling the push (post-ship class: 12+ of the 31 were push polls the doctrine required, the rest the review's own work).
  - Measure next run: refusals per class against this baseline; target receiving < 20, build-offload ~0, post-ship ~0.
- [ ] A Codex backend outage costs a retry storm before the run defers
  - MEASURED `run-20260903-160406.log:423-436`: 5 dispatch+wait pairs (10 calls, ~4m20s) against a design review returning 404s. Fix shape: the broker counts transport failures (not findings) and tells the run to defer after 2.
  - SHIPPED 2026-09-28 (attended), under test: `scripts/overnight/codex-outage-check.py`, called by the broker, exits 75 when the last 2 completed legs in 20 minutes failed at the backend (HTTP status, stream disconnect, model at capacity; not app-server crashes, local ENOENT or review errors, classified over 3,422 legs). `scripts/overnight/tests/test_codex_outage_check.py`, 13 cases including the real 2026-09-03 artifacts. Measure: dispatches refused per outage (target 1-2, baseline 5).

- [ ] MODEL MIGRATION when Sonnet 5.5 and Haiku 5.5 ship: every move is gated by `scripts/overnight/agent-replay.py` (decided 2026-09-28)
  - MEASURED dispatch mix (`.claude/state/subagent-log.jsonl`): `general-purpose` 639 (613 in September, all interactive or workflow, none from the run; unpinned, so it inherits Opus), `kernel-quality-auditor` 251 (Opus), `overnight-log-explorer` 218, `kernel-explorer` 163, `concurrency-evidence-mapper` 132, `fork` 108 (inherits by design), `Explore` 106 (built-in).
  - Sonnet 5.5: every `model: sonnet` agent moves with the CLI alias, no edit. Replay `kernel-quality-auditor` Sonnet 5.5 against Opus on `scripts/overnight/replay/kernel-quality-auditor.jsonl` (105 tasks, 42 high or critical) and move it only on PASS. `general-purpose` and `Explore`: NOT blanket-defaulted. A `CLAUDE_CODE_SUBAGENT_MODEL=sonnet` setting was tried and reverted the same day at the operator's call: it would silently re-model every future agent without a `model` line. Each gets its own replay task set (general-purpose needs one built from real interactive dispatch shapes) and, on PASS, an explicit pin via a shadowing `.claude/agents/<name>.md`.
  - Haiku 5.5 candidates, each only on its own PASS: `git-historian` (task set built, 20 tasks), `gh-query-runner`, `todo-validation-mapper`, `xref-dependency-mapper`, `ssdt-auditor`. The last four still need task builders with deterministic oracles (`query.py`, `validate.py`, the SSDT master tables).
  - Harness verified 2026-09-28: one `git-historian` Sonnet task HIT in 21.7 s for $0.24; one auditor Sonnet task, $0.81 and 354 s, correctly scored a MISS after the scorer stopped crediting a 430-line region the auditor had cited as correct. Budget a full auditor pair at roughly 105 x ($0.80 + the Opus cost) before running it.

## Found live this cycle

<!-- The run files here. Nothing yet: v20 opened at close-out, before the next arm. -->
