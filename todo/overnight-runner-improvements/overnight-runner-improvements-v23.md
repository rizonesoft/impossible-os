# Overnight Runner Improvements v23 -- Findings (opened 2026-11-07)

Opened at the 2026-11-07 close-out of [v22](overnight-runner-improvements-v22.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT file the run appends to; the close-out that follows the next stop triages every item and records a verdict for each.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v23.md`](../token-saver/token-saver-v23.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface; a bad gate edit with nobody watching is unrecoverable. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFER, and state the CONSEQUENCE as its own checkable sentence. Quote verbatim what a gate says it matched. **Verify a probe with a control whose effect you can predict, and check the control for SEMANTIC effect.**

File reasoning lessons as `- [ ]` items like any other finding (the nine shapes are listed in [v12](overnight-runner-improvements-v12.md)).

---

## What shipped in the 2026-11-07 v22 close-out, and is therefore UNDER TEST

**Sections can ship again, and that is the first thing to watch.** The operator removed `Skill(review *)` from `~/.claude/settings.json`; a fresh-session probe confirmed the skill loads, with a control that the old rule refuses it. No section has completed a ship and post-ship review since 2026-09-30, so the path itself is unproven end to end.

- **`skill_denies.py`:** the arm refuses, and the breaker trips at segment end, when a deny matches a pipeline skill. Controls in `test_skill_denies.py`. Watch: any `skill-denied` outcome.
- **`record-rollover` reads the J1 verdicts.** Watch: `NOT recorded` refusals. A refusal on a green chain means the verdict sink has a hole; the likely shape is a build input moving between legs.
- **Gate parsing:** `section_commit_gate` separates commands and reads clustered flags, and only on scripts with no substitutions, nested quoting or `#`. `build_offload_reminder` walks `env` and `timeout` and drops a leading comment block only. Each is narrow on purpose: five review rounds broke the broader versions, so do not widen them on a false refusal alone. Watch: `build-offload` naming only real suite runs (v22: 6); a mid-script comment above a wrapped suite still refuses. `env A=1 bash scripts/test.sh` now refuses where it used to pass. `SEQ-TEARDOWN` is unchanged and still refuses prose saying `clear` beside a guard call (v22: 2 false).
- **`collect-questions.py`** reads the real stamps and `todo/answers.md`: 58 open at this close-out against 0 before. Watch: does the file get read at each lifecycle exit.
- **`stale_parks.py`** is advisory, at arm and in ADVANCE. Watch: precision over a cycle. Today 2 hits, 1 true. Promote to a fixpoint refusal only with that evidence.
- **Round-8 rule:** outside `src/kernel` and `src/boot`, only Critical and High continue from round 8. Doctrine, not a gate.
- **CI has three jobs now.** `Build and test the OS`, `Tooling regression pack`, and `Build Impossible OS`, which keeps the name the ruleset requires and passes only when both pass. Watch: the required check goes red when only the pack fails, and the OS verdict arrives about 20 minutes sooner. Not exercised until the first push.
- **`CI_CONTAINER=1`**, **pre-push host gates** (`SKIP_HOST_GATES` should be 0), **`run-status.md`** at every segment end, **`-z defs`** on the loader link (not exercised on real firmware), **ledger** `--section` and `--round` plus an archive.
- **All 30 questions in `todo/answers.md` were answered on 2026-11-07, and nothing has applied them yet.** 36 `awaiting-answer` stamps still read BLOCKED, and the launcher's heal probe should retry them because the file changed. Watch: does the run apply each answer, drop the token and reopen the section. Three answers mean stay as you are (Q14, Q17, Q24 report-only), so those stamps need a plain Deferred reason instead of the token, or they read BLOCKED forever. `collect-questions.py` now drops a stamp once every question it names is answered, so a stuck one shows in `sequencer_triage.py --blockers` and not on the operator list.
- **Review debt from v22's run:** item commits landed without a section review while the refusal held, including the FAT32 hardening, DPC watchdog and remote IPI, IRQL work, and `event_t`. Their Codex legs ran; the post-ship reviews did not. They get reviewed when their sections close. `00-infrastructure/TODO-10` s33 is reopened with its resume note.

## Carried forward from v22 -- open

v22 closed (both files) 24 resolved, 6 rejected, 1 not reproduced, 6 carried. The successor inherits the item, not the triage.

- [ ] `section_commit_gate` step-8 test-wiring binds tests to the source BASENAME, which 434 of 516 kernel sources cannot satisfy
  - History: [v19](overnight-runner-improvements-v19.md) through [v22](overnight-runner-improvements-v22.md). 0 refusals in v22, because no section reached a ship commit.
  - Settled by refusal data from a cycle that ships kernel sections, then a same-commit test that registers a suite named for the staged basename, shipped WARN first with anti-`test_heap.c` refusal controls.
- [ ] `quality-review-section` and `codex-review-todo` are never scheduled, so no review looks across sections of a file ([v22](overnight-runner-improvements-v22.md))
  - Settled by running `codex-review-todo` once on a closed file, after the first sections ship, and counting cross-section findings.
- [ ] Anything that depends on state from a PREVIOUS boot is invisible to the suite and the smoke matrix ([v22](overnight-runner-improvements-v22.md))
  - A second-boot leg means restructuring `test-smoke.sh`, the primary boot gate, so it needs a design pass. The first concrete need is the open item at `01-boot-platform/TODO-29:649`. Settled when that item is reached.

## Found at the v22 close-out -- open

- [ ] `SEQ-TEARDOWN` is a text match, so the guard CLI's `clear` has no enforcement of its own
  - OBSERVED, review round 3: `python3 .claude/hooks/run_phase_guard.py $(printf cl; printf ear)` is not refused at HEAD, because no ` clear` appears in the text, and the CLI's handler then saves `{"active": false}`. Nothing suggests a run has done this; it takes deliberate obfuscation.
  - The owning layer is the CLI verb, not a command-line scan. It cannot simply refuse under `OVERNIGHT_SEQUENCER_RUN=1`, because `overnight-launch.sh:137` and `:158` call `clear` from that same environment on the deadline and fixpoint stops. Settled by a design for how the launcher proves it is the launcher.
  - The same shape probably applies to `phase FIXPOINT`, not tested. Do not narrow the text match in the meantime: three rounds broke every narrowing.

- [ ] `section_commit_gate` reads a script shlex cannot tokenize as no commit at all
  - OBSERVED at this close-out: `git commit -m $'a\';b' -n` returns `(False, False)`, here and at HEAD, because ANSI-C quoting leaves shlex an unbalanced quote. Bash runs it as a real commit with `-n`. The close-out stopped one unparseable LINE from hiding other lines, but a line that is itself the commit still escapes. Settled by reading an untokenizable line naming `git commit` as a commit, measured for false refusals first.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote. v22 figures are from the 21 logs `run-20261107-*`, one of them unfinished when read.

- **Refusals per class, main loop and subagents separately.** BASELINE (v22): main 27 of 1,560 calls (1.7%, v21 2.6%): `receiving-review-required` 10, `build-offload` 6, genuine `Exit code` 4, `SEQ-TEARDOWN` 2, permission-rule 2, `SEQ-PHASE` 1, `section_commit_gate` 1, other 1, broker bare-flag 0, `skill-step-block` 0, `READ-CACHED` 0. Subagents 0 of 126. The logs truncate hook messages, so class comes from the visible prefix. Targets: `SEQ-TEARDOWN` 0, permission-rule 0.
- **Sections shipped with a post-ship review.** BASELINE (v22): 0. Target: every section ship, starting with TODO-10 s33.
- **Opt-outs per segment, WITH reasons.** BASELINE (v22): 1 `SKIP_CODEX_REVIEW` (one new test function, no production code); 0 of `SKIP_REVIEW_HOOK`, `SKIP_CI_PARITY`, `SKIP_TODO_STAGED_CHECK`, `SKIP_DISPATCH_GATE`, `SKIP_SKILL_STEP_BLOCK`, `SKIP_TOOLING_SUITE`. New: `SKIP_HOST_GATES`.
- **CI verdict coverage on `main`.** BASELINE (v22): last 40 `Build` runs 37 success, 3 failure, 0 cancelled. Keep it, and record it per job now that there are two.
- **Segment outcome.** BASELINE (v22): 20 of 20 finished segments reported `checkpoint`, including those wedged on the refusal. Any `skill-denied` must name a real deny.
- **Arm-time lines.** Record the `token-probe:`, `breaker reset:`, capture WARNING and stale-parks lines of every arm. They do not appear in the run logs, so read them from the arm output. Baseline: none yet.
- **Run state after a stop.** `sequencer-run.json` `active` after each stop. BASELINE (v21): `false`. Fix held.
- **Runaway-arm firing rate.** BASELINE (v22): 0. Each firing must name a genuinely long or looping dispatch.
- **Pre-push tooling pack refusals.** Target: 0 flakes, and every refusal readable from the push log.
- **Build CPU rate.** BASELINE 118-153 ms/MiB on ~13.5 MB (2026-09-28). Record the Test 7 line from any pack run.
- **Kernel image headroom.** BASELINE (2026-11-07): 66,500 bytes before the ceiling; per-section budgets `.text` 68,815, `.rodata` 67,714, `.data` 66,500, `.bss` 69,523. `.text` read 153,647 at the start of the run, so about 85 KB went in one day; watch this.
- **Control-plane suite flake set.** Members unchanged. `test_query_bounds.sh` fails on a staled `build/todo-cache.json` every time; rebuild with `--keep-cache` before reading any of its failures as real.

## Found live this cycle

