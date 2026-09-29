# Overnight Runner Improvements v20 -- Findings (opened 2026-09-28)

Opened at the 2026-09-28 close-out of [v19](overnight-runner-improvements-v19.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT file the run appends to; the close-out that follows the next stop triages every item and records a verdict for each.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v19.md`](../token-saver/token-saver-v19.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface; a bad gate edit with nobody watching is unrecoverable. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFER, and state the CONSEQUENCE as its own checkable sentence. Quote verbatim what a gate says it matched. **Verify a probe with a control whose effect you can predict, and check the control for SEMANTIC effect**: a mutation the compiler folds away, or a tool that writes nothing to the pipe you hash, both read as a clean pass (v18, paid twice in one turn).

## Reasoning and autonomy findings -- file these too

File reasoning lessons as `- [ ]` items like any other finding (the nine shapes are listed in [v12](overnight-runner-improvements-v12.md)). The v19 cycle paid again for two of them:

- **Verify the PROBE before trusting the result.** v19 had three instances (a digit-spelling grep reported as a clean sweep, a `pgrep` matching its own command line, a hand-computed image budget 270x off the sensor), and the close-out added a fourth: a `ps | grep -c` that matched itself.
- **A probe must not write state.** At the close-out, a synthetic dispatch JSON piped through EVERY hook made `codex_review_completed.py` record a fake Codex review and armed the receiving-review gate. Probe the ONE hook under test, never the directory.

---

## What shipped in the 2026-09-28 v19 close-out, and is therefore UNDER TEST

Every change below carries a refusal-direction control.

- **`test_build.sh` closes inherited fds 3-8 at start.** Root cause of the `22ed` pre-push flake: the identity gate marks its probe by opening fd 3 (`scripts/todo-graph/identity-gate.sh:3185`), and git hands the pre-push hook extra descriptors. Control: with fd 3 and 4 held open the suite passes `22ed`; with the close disabled it must fail (result recorded on the v19 item). Watch: any pre-push `22ed` failure is now a real regression, not a flake.
- **`s48` (`test_todo_fence.py`) scales on USER CPU time, 2 and 8 MiB interleaved, best of 5.** Control: an injected quadratic tail copy fails the 8x bar in 7 of 8 shapes. Standalone 3.8-4.3x, under heavy synthetic load 2.5-4.7x. Watch: an `s48` failure in the pack should now be real.
- **`test_build.sh` Test 7 is a CPU-time RATE: 250 ms per MiB of `todo/`, floor 2 s.** Measured 118-153 ms/MiB at ~13.5 MB. Control: a 60 ms/MiB rate with no floor FAILS. Watch: the reported ms/MiB as the corpus grows; a rising RATE is a build.py regression.
- **`test-tooling.sh` quotes the named `test_todo_fence` FAIL lines** instead of `tail -3`.
- **`skill_step_block.py` exempts a Bash commit whose whole tree is under `todo/` or `docs/`.** Control: `scripts/overnight/tests/test_step_block_docs_only.py` (7 cases, 3 refusal). Watch: `SKIP_SKILL_STEP_BLOCK` count per segment should fall (v18 baseline 11).
- **Check 24 and `todo-reachability.py` label a cache-absent count PARTIAL.** Watch: any Check 24 number quoted without the label while the cache was absent.
- **`test-smoke-matrix.sh` holds a per-worktree flock** (waits 1200 s, then refuses rc 3). Watch: the wait line in a run log means two matrices overlapped.
- **`test-coverage.sh` counts assert CALLS outside comments** (15570 -> 15535 at close-out; the drop is the correction, not lost tests).
- **Doctrine:** `boot-code-quality` Gate 15 (`.bss` poison); `arm-sequencer.sh` prints a `DEADLINE:` line and CLAUDE.md says arming starts a countdown; the ship push is issued with `run_in_background: true`; the adversarial template enumerates every value of a new predicate; `review-todo-section` 8a re-reads the whole section on a stale doc claim.

## Carried forward from v19 -- open

v19 closed 17 resolved, 8 rejected, 1 not reproduced, 4 carried. Each carried item keeps its verdict in v19; the successor inherits the item, not the triage.

- [ ] `review_convergence.py record` fingerprints at CALL time, not dispatch time ([v19](overnight-runner-improvements-v19.md), now with an observed failure).
  - Settled by `record --verdict-log <path>` using that dispatch's manifest row, plus a control that a post-dispatch edit still redispatches.
- [ ] `build.yml` `cancel-in-progress` leaves rapid pushes with no CI verdict ([v19](overnight-runner-improvements-v19.md); 5 of 6 runs cancelled on 2026-09-27/28).
  - OPERATOR decision: `cancel-in-progress: false` versus `paths-ignore` for docs/todo-only pushes.
- [ ] `section_commit_gate` step-8 test-wiring binds tests to the source BASENAME, which 434 of 516 kernel sources cannot satisfy ([v19](overnight-runner-improvements-v19.md)).
  - Settled by accepting a same-commit test that registers a suite named for the staged basename, shipped WARN first, with anti-`test_heap.c` refusal controls.
- [ ] A Codex dispatch whose prompt arrives via command substitution or a loop records no kind and no section ([v19](overnight-runner-improvements-v19.md)).
  - Settled by the broker re-emitting the parsed triple for the hook, or the gate naming each stamp's section in its refusal.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Opt-outs per segment, WITH reasons.** BASELINE at the v18 close-out, 9 logs: 3 `SKIP_REVIEW_HOOK=1` (all with a reason, all on commits), 0 `RECEIVING_REVIEW_OVERRIDE`, 0 `SKIP_TOOLING_SUITE=1`, 0 `SKIP_CI_PARITY=1`, 11 `SKIP_SKILL_STEP_BLOCK=1` (10 with a visible reason). Target: zero leaks (met); every opt-out carries a `_REASON`; the `SKIP_SKILL_STEP_BLOCK` reasons named `bookkeeping`, `deferral`, `sweep-only`, `advance` -- if one reason dominates, that gate has a shape it should allow.
- **Pack-only refusal attribution.** BASELINE: 2 of 2 in the v18 cycle attributed from the nested detail (both `test_build.sh` 843/844). Target stays: every one names its assertion; record it on the carried pre-push item.
- **Does the worktree guard ever block a text-only command again, and does any mutation slip past a shape the 18 controls do not cover?** v18 cycle: 0 and 0.
- **Runaway-arm firing rate at the new threshold.** BASELINE: count arm 8 of 21 at 30; expected 0 of ~21 at 60 on the same distribution (max observed 53). Duration arm: 0 (max 377 s). Each firing must name a genuinely long or looping dispatch; record the `subagent-log.jsonl` row.
- **`cursor_disagreement` firing rate.** v18 cycle: 0 in 9 segments.
- **`MISSING` (exit 5) waiter results.** v18 cycle: 0.
- **Restart survivability.** v18 cycle: the run stopped CLEANLY on 2026-08-30 13:04 (rollover checkpoint, `active: false`, marker cleared); the status page now reports liveness. Measure: does `run-status.md` ever say `Armed: True` without `STALE MARKER` while liveness says DEAD.
- **Does the bare-`--flag` guard ever refuse a prompt that had no bare token?** v18 cycle: 0 refusals. v19: 1 refusal on 2026-09-03, and it was a TRUE positive -- a perf prompt quoted C's `--timeout` pre-decrement from a UART poll loop, which is a real bare token. Backticking it, as the refusal text says, was the correct and complete fix. Baseline stays 0 confirmed guard defects.
- **Mid-token reflow guard refusals outstanding.** v18 cycle: 0.
- **Filing discipline: does the run SPLIT an item spanning forbidden+fixable?** Still unmeasured under a genuinely mixed item.
- **Control-plane suite flake set.** Members unchanged (six plus the rc-134 suspect, silent this cycle). `test_query_bounds.sh` fails on a staled `build/todo-cache.json` every time -- rebuild with `--keep-cache` before reading any of its failures as real.
- **`completed_drought` firing rate.** v18 cycle: 3 CI packets, all `completed_drought: false`.
- **Does the fixpoint rebuild ever return non-zero in practice?** rc 3 never observed live.
- **The pre-push tooling suite receipt.** v18 cycle: 2 ship pushes took the receipt route after a pack-only refusal, ~14-15 minutes each; 0 `SKIP_TOOLING_SUITE=1`.
- **J1 re-runs caused by attended commits.** v18 cycle: 1 refused rollover (`run-20260830-035742.log`, smoke receipt invalidated) forcing a full extra J1 chain.
- **Kernel image headroom.** BASELINE at the v18 close-out (HEAD `7cd799909`): exact `.text 79, .rodata 1906, .data 980, .bss 1323` bytes, page-rounded 4096. A kernel section that reaches link time without having read the gate line is the regression to record; the number after the first armed build re-establishes the baseline.
- **Pre-push tooling pack refusals after the v19 fixes.** BASELINE: v19 recorded 8+ pack-only refusals, all on `22ed`, `s48`, the 2 s budget or the plugin MANIFEST row. Target: 0 flakes; any refusal now names a real assertion. Record every one with its nested log line.
- **Build CPU rate.** BASELINE 118-153 ms/MiB on ~13.5 MB (2026-09-28). Record the Test 7 line from any pack run.

## Found live this cycle

<!-- The run files here. Nothing yet: v20 opened at close-out, before the next arm. -->

- [ ] create-todo's docs-page step still allows the provisional minimal page "until the docs page contract lands"; the contract has landed, so the step should now point at it and its template
  - OBSERVED 2026-09-28 implementing TODO-10 section 3: the item "Add a docs-page step to `.claude/skills/create-todo/SKILL.md`" is control-plane (`.claude/skills/**`), which the unattended run may not edit, so it is parked `[/]` operator-gated in that section.
  - MECHANISM, confirmed at source: `.claude/skills/create-todo/SKILL.md:87` already requires a page with `covers=` (Check 30 enforces it via `check_baseline()` in `scripts/site/build.py`), but its fallback allows a short Overview-only page, which is now below the `docs/contributing/docs-page-contract.md` bar.
  - Fix shape: UPGRADE the existing step rather than adding a second one: copy `docs/contributing/_template.md` into the folder the contract's folder map names, set `covers=`, state nothing is implemented yet, run `build.py --check`; delete the "until the docs page contract lands" fallback.

- [ ] Arming accepts a REVOKED OAuth token: the run then dies at launch and the circuit breaker disarms it, two minutes after the operator walked away
  - OBSERVED 2026-09-28 15:40-15:41, attended arm with `--force --hours 48`: both the watchdog launch (`run-20260928-154004`) and the main launch (`run-20260928-154104`) ended in 5 s with `Failed to authenticate. API Error: 401 OAuth access token is invalid.`, then `Circuit breaker tripped: consecutive unproductive runs -- STOPPING and disarming.`
  - MECHANISM, confirmed at source: `arm-sequencer.sh:199-217` checks that `~/.conclave/secrets/claude-oauth-token.env` exists and carries a `CLAUDE_CODE_OAUTH_TOKEN=` line; nothing checks the token authenticates. Its own `expires_at` was 2027-09-13, so the token was revoked, not expired, and a date check would not catch it either.
  - Fix shape: before writing any timer, run one minimal authenticated call with that env file (`claude -p` with a one-word prompt and one turn) and REFUSE to arm on a 401, naming the re-mint command. Costs one tiny request per arm.
  - SECOND, operator-side lesson: the attending session watched `reports/latest.log`, which still pointed at the 2026-09-05 log until the new run repointed it, so the first check read an old run as the new one. Watch `systemctl --user status` or the newest `run-<date>-*.log`, never the symlink, until the new log exists.
- [ ] A fresh ARM inherits the previous run's circuit-breaker state, so a fixed failure keeps stopping the new run
  - OBSERVED 2026-09-28: after the revoked-token stop (the item above), the re-arm at 16:05 was refused by its pre-arm check (`watchdog backoff active until 16:41`), and once that and `state/unproductive-streak` (count 3) were cleared by hand, the 16:07 launch stopped at once: `deadline-check: STOP -- 2 consecutive segments shipped no section (limit 2)`. The two "segments" were the two 5-second 401 launches of the PREVIOUS arm.
  - MECHANISM, confirmed at source: breaker state lives in `.claude/overnight/watchdog-backoff-until` (`overnight-launch.sh:64`), `.claude/overnight/state/unproductive-streak`, `.claude/overnight/noship-streak` (`deadline-check.sh:32`, limit 2) and the `NEEDS-OPERATOR.md` marker; `arm-sequencer.sh` resets none of them. The backoff is cleared only when real work runs (`overnight-launch.sh:176`), which a stopped run never reaches.
  - Operator question behind it: should closing a canary or starting a new run clear this automatically? Yes for an explicit ARM: arming is the operator saying "start now", and state from a run that has ended describes that run, not this one. NOT for watchdog relaunches inside a live run, where the breaker is doing its job.
  - Fix shape: `arm-sequencer.sh` removes those four files after its pre-arm health check passes, and prints what it cleared (a `reset: noship-streak=2 ...` line), so a recurring cause stays visible. The pre-arm auth probe from the item above stops a still-broken token from being re-armed into the same loop. Refusal control: a watchdog relaunch (not an arm) must still honour all four.
- [ ] `agent_coverage_gate` blocks SUBAGENTS from reading the files they were sent to examine: 56 of 77 subagent refusals in the 2026-09-28 canary
  - OBSERVED, attended, run segments 16:18-20:13: 77 `sub tool error:` lines (visible only since the `sub ` label shipped the same day), 56 of them `[AGENT-COVERED BLOCK]`. Example `run-20260928-172015.log` 17:24:45: a subagent's whole-file Read of `docs/contributing/docs-page-contract.md` refused because an EARLIER agent had mapped it.
  - MECHANISM, confirmed at source: `.claude/hooks/agent_coverage_gate.py` exists to make an agent dispatch REPLACE the MAIN session's read (its header: "the main session then did the same work again in the expensive context"), but the gate path does not check the caller, so a subagent, which starts with an empty context, is refused the read its task needs.
  - COST: 56 wasted subagent calls in ~4 hours, and the worse cost, agents reporting on files they were not allowed to read. The main-session protection is untouched by the fix.
  - Fix shape: exempt sidechain callers, using the same caller test `websearch_offload_gate` and `runner_bash_guard` use (the agent transcript path). Refusal control: a MAIN-session whole-file re-read of a covered path must still block on its third occurrence.
- [ ] A shipped section was rolled over with its review fixes committed but its Verified + Quality-reviewed stamps never written, so the next worker found it still NEEDS_WORK
  - OBSERVED 2026-09-28 17:10, relaunch after rollover at HEAD `63b5c6df1`: TODO-10 section 3 classified NEEDS_WORK. Review commit `3ec5b3c03` says "...; stamps" in its subject, yet it touched only `CONTRIBUTING.md`, `COUNT.md`, `README.md` and `docs/contributing/docs-page-contract.md`; the TODO carried no stamp lines for section 3.
  - MECHANISM (inferred, not reproduced): `rollover` VERIFIED checks tree cleanliness, push state and receipts but not that the section it closes is oracle-DONE, so a review whose stamp step was skipped still rotates cleanly. The stamp was repaired this segment under `SKIP_DISPATCH_GATE` (stamp-repair), because the fresh session had no review-class Skill invocation.
  - Fix shape: make `rollover` refuse when the section just shipped (the one whose IO row flipped since `last_rollover_epoch`) still classifies NEEDS_WORK in `sequencer_triage.py --classify`. Control: the prior segment's state at `63b5c6df1` must be refused.
- [ ] A mistaken broker dispatch cannot be cancelled: the teardown guard refuses stopping a `codex-rev-*` unit, so a wrong-prompt leg runs to completion
  - OBSERVED 2026-09-28 17:34, TODO-10 section 4: the broker's flag guard (`review-broker-codex-dispatch.sh:69`) refused my prompt twice, correctly: `git diff --cached` inside backticks still tokenises as `--cached` because the backtick span held spaces. I then sent a short prompt to test whether the guard was the problem; that "probe" was a real dispatch (`20260928-173435-adversarial.out`).
  - Stopping its systemd unit was refused by `run_phase_guard.py pretool` as `SEQ-TEARDOWN`; so was a Bash heredoc that merely QUOTED the stop command while filing this item (the guard matches command text, including data). The full-scope leg (`20260928-173447-adversarial.out`) ran beside the short one at the same HEAD. The short leg found three valid findings the full leg missed, so the cost was one leg, not a wasted one.
  - REASONING finding: a broker call is never a probe; test a prompt against the guard's `case` pattern locally instead.
  - Fix shape: (a) the refusal text should say the backticks must enclose only the flag, not a phrase containing it; (b) the teardown guard could allow stopping a `codex-rev-*` unit, since killing a review is not disarming the run. Control for (b): stopping the sequencer's own unit must still be refused.
- [ ] REASONING: a docs section's review loop drifted into designing an unbuilt component, and one "fix" weakened a safety guard; apply the scope rule at the FIRST out-of-surface finding
  - OBSERVED 2026-09-29, TODO-10 section 15 (networking docs, `5f71cff17` + `994606991`): 30 Codex dispatches. From round 3 on, most findings were about the design of the unimplemented NTP client in `07-networking/TODO-06` section 2, which the section had only edited to correct drift. Each fix drew a deeper RFC 5905 finding (bootstrap, nonce, LI=3, zero origin, root distance, clock generation).
  - The round-3 fix answered "define handling for offsets over ten years" with the permissive path (force `KeSetSystemTime()`), which bypassed the hostile-reply guard at `wall_clock.c:589-591`; the post-ship perf leg caught it as a [high]. INFERRED cause: the fix aimed at making the reviewer's scenario work rather than at what the guard exists to prevent.
  - What worked in the end: filing the remaining design items in the owning open section (the review skill's "branch test is SCOPE" rule) closed the loop in one step. Applying it at the first NTP-design finding would have saved roughly 8 dispatches.
  - Second, smaller: a repo scan printed the offending `TODO-06-widget-dialogs.md:14` hit but cut the line before the match, and I read past it; the next consistency round found it. A scan whose output is truncated needs `grep -o` context before being called clean.
- [ ] The review broker's bare-flag BLOCK advises "wrap the flag in backticks", but a multi-word code span such as `` `git diff --cached` `` is still blocked, correctly; say the backtick must touch the flag
  - OBSERVED 2026-09-29, TODO-10 section 16: the first dispatch was blocked on `--cached`; the retry wrapped the whole phrase in backticks and was blocked again with the same message; the third rewrote it as prose.
  - Mechanism confirmed at `scripts/overnight/review-broker-codex-dispatch.sh:69-82`: the guard matches `*[[:space:]]--[a-zA-Z]*`, and the companion tokenizer splits on whitespace, so `--cached` inside a spaced code span becomes its own token (it begins `--`); only a backtick glued to the flag protects it. The guard is right; the fix text is incomplete.
  - Cost: one wasted dispatch turn per occurrence. Suggested fix (not applied, control plane): append "the backtick must immediately precede the flag; a code span with a space before the flag does not protect it".
- [ ] A top-level tooling assertion that fails under `--quiet` reports only its NAME: `t_fail` drops its detail argument, so a pre-push refusal cannot be triaged from the push log
  - OBSERVED 2026-09-29 03:02, pushing `5bedc6135`: the refusal read `FAIL 1/1375` and `- test.sh: utest_reap_qemu force-ends a SIGTERM-resistant VM on a deadline`, with no `rc=`/`signals=` detail and no `full nested output:` path.
  - Mechanism confirmed at `scripts/test-tooling.sh:262-267`: `t_fail` appends only `$1` to `FAILURES` and prints `$2` only when `QUIET=0`. The v17 nested-detail fix (`_tt_nested_fail_detail`, `:763`) covers nested suites only, so inline assertions (like this one at `:21250`) keep the old blind spot.
  - Cost: the case was green 3/3 standalone, so the only safe route was a full out-of-band pack (~23 min) plus a receipt; the detail string would have shown at once whether the deadline, the signal target or the stall net fired.
  - Fix shape (not applied: editing the pack mid-section costs another full pack at push): keep `$1 :: $2` in `FAILURES`, or print the detail in the summary block even under `--quiet`. Control: a forced `t_fail name detail` under `--quiet` must print `detail`.
- [ ] A docs-only section's stamp commit can move lint Check 7's `total` and so pull the whole ~20-minute pre-push tooling pack in behind a two-line baseline bump
  - OBSERVED 2026-09-29 05:10, TODO-10 section 18 review stamp `b53b23fb5`: marking one already-shipped item `[x]` in `09-desktop-shell/TODO-07` added two backticked symbol refs behind a stamp, so the commit was refused with `POPULATION GREW: 1723 vs 1721`; the sanctioned repair edits `scripts/lint/stub-lint-baseline.json`.
  - Mechanism confirmed: `scripts/lint/check_stub_behind_stamp.py` refuses any denominator move (rc 7), and `scripts/lint/` is a pre-push tooling trigger, so the bump made the push run `scripts/test-tooling.sh` in full (push started ~05:10, `rc=0` at ~05:33) for a change no tooling code could observe.
  - Cost: ~23 minutes of wall-clock per occurrence and one refused commit; model cost was small (bounded polls). Every docs section that ticks a satisfied item in another roadmap can hit it.
  - Fix shape (not applied, control plane): either scope the pre-push trigger so a baseline-only diff to `stub-lint-baseline.json` runs just the Check 7 fixture, or let the receipt route accept a baseline-only change. Inferred, not tested: the pack cannot be affected by a `total`/`by_owner` edit, since only lint reads that file.
- [ ] The overnight-sequencer SKILL's J1 block prescribes a bare `build.sh && test.sh && test-smoke.sh` chain that `build_offload_reminder` BLOCKs in SECTIONS
  - OBSERVED 2026-09-29 06:17, after the TODO-10 section 19 ship: issuing the chain exactly as `.claude/skills/overnight-sequencer/SKILL.md:436` shows was refused with `[build-offload BLOCK -- reroute]`; re-running each step through `scripts/overnight/run-artifact.sh` (j1-build, j1-test, j1-smoke) then `receipts.py record-rollover` worked first time.
  - MECHANISM, confirmed at source: the SKILL block and the rollover guard's refusal text both name the bare commands, while the hook requires the wrapper for every build or test run in the main context. Cost: one refused call and one extra turn per section boundary; small, but it recurs every rollover and teaches the run that the doctrine block is not runnable as written.
  - Fix shape (control-plane, attended only): rewrite the J1 block as three `run-artifact.sh` calls (build, test QUIET=1, smoke last) followed by `record-rollover`, and make the rollover guard's refusal text print the same shape. Control: the rewritten block must pass the hook unchanged.
