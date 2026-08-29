# Overnight Runner Improvements v18 -- Findings (opened 2026-08-29)

Opened at the 2026-08-29 close-out of [v17](overnight-runner-improvements-v17.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT file the run appends to; the close-out that follows the next stop triages every item here, verifies each claim against the tree as it is then, and records a verdict for every one.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v18.md`](../token-saver/token-saver-v18.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface; a bad gate edit with nobody watching is unrecoverable. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFER, and state the CONSEQUENCE as its own checkable sentence. Quote verbatim what a gate says it matched.

---

## What shipped in the 2026-08-29 v17 close-out, and is therefore UNDER TEST

Every change below carries a refusal-direction control (a case that must still be BLOCKED or still FAIL), because a canary exercises the happy path at scale and structurally cannot exercise the refusal path. All 85 control-plane cases green at close-out (`scripts/overnight/tests/run-all.sh`), including 6 new test files.

- **`[SEQ-WORKTREE]` adjudicates the INVOCATION, not the text.** `run_phase_guard.py::_is_worktree_mutation` strips heredoc bodies, segments on control operators, walks env-prefix and wrappers, descends `bash -c`, and treats data consumers (`echo`, `sed`, `python3`, `grep`, a quoted `git commit -m` message) as never a mutation; an unknown wrapper keeps the old anywhere-scan. Refusal controls: 18 must-block shapes (`test_worktree_guard.py`) including `env`, `timeout`, `xargs`, `exec`, `sh -ec`, `git -C`, `a&&git worktree add`, a heredoc opener on the same line, and a /tmp scratch repo (still blocked by design). Watch: any block whose command was text-only, and any mutation that slipped through a shape the 18 do not cover.
- **The pre-push pack prints the nested suite's own `[FAIL]` lines and keeps the full output** (`_tt_nested_fail_detail`, all five nested suites, `.claude/overnight/artifacts/nested-<suite>-<stamp>.log`). Emitted only on the failing path; the verdict is unchanged. Watch: the first pack-only refusal after this ships is the first one that can be attributed from the push log alone -- record what it named.
- **`.githooks/pre-push` runs the identity gate through `bash` regardless of mode, refuses loudly when the gate script is missing, and passes `--head <pushed oid>`.** Contract pinned on the hook text (`test_prepush_identity_caller.py`); the interrupted-rebase case is reasoned from source, not staged end to end.
- **The review broker attributes a section from a real UTF-8 sign, anchored to the text after the TODO path** (`--section-of` for probing). `scripts/codex-dispatch.sh` now tees its transcript under `.claude/overnight/reviews/` and appends the same manifest entry as the broker (`route: codex-dispatch.sh`); both routes record `head`, `index_tree`, `worktree_sha256` at dispatch time. Watch: `review-envelope.py --section N` must now see direct-wrapper legs; any `missing`/`needs_redispatch` on a leg that completed on disk is a regression.
- **`wait-for-codex-verdict.sh` reports a nonexistent path as `MISSING` (exit 5), never as hung.** A stale real log beside it still exits 4. Watch: any exit 5 in a run means a wrong path was polled -- record where the path came from.
- **`advance-work.py` emits `cursor_disagreement`** when an explicit cursor on the same file names a different section than the oracle. Watch: does it fire, and did the worker act on the oracle.
- **`skill_step_observer.py` records step 19 only for a commit into THIS project** (`_step19_commit_is_ours`; scratch repos under /tmp no longer count). Watch: a real section commit that was NOT observed would be the regression.
- **`subagent_audit.py` derives `duration_ms` from the leaf transcript's timestamps.** The 10-minute runaway arm can now fire for the first time in three cycles. Watch: its firing rate, and whether every firing names a genuinely long dispatch.
- **`section-manifest.py` attaches `bss_headroom` for kernel-touching sections and one gate line when tight.** Watch: a section that added kernel static data and still discovered the ceiling late would mean the advisory was not read.
- **Sequencer skill doctrine:** no edits to tooling-pack inputs between `git push` and its `rc=` line; the ship sequence ends with a `git log @{u}..HEAD` emptiness check (`PUSH LANDED`); a pack-only refusal is answered with the receipt route, never `SKIP_TOOLING_SUITE=1`; never pipe a review dispatch through `head`/`tail`; poll the broker's `logFile` verbatim; an rc-3 INFRASTRUCTURE identity-gate failure is re-runnable first; and a new `## Working-discipline lessons` section carrying the v16/v17 reasoning findings.

## Found at the 2026-08-29 close-out, not by a run

- [ ] The run died to a host restart MID-GATE and left work in a WORSE state than clean-and-pushed, which is exactly what the restart-survivability obligation asked.
  - OBSERVED: `run-20260828-082416.log` ends at 09:51:52 on 2026-08-28 inside TODO-10 section 32's final `test.sh` + 4-leg boot matrix; no later line, no NEEDS-OPERATOR entry (the file still carries the 2026-08-24 deadline stop), `systemctl --user list-timers` shows 0 overnight timers, `run-liveness.sh` says DEAD.
  - LEFT BEHIND: 24 uncommitted files (the whole section-32 implementation, its tests, `TODO-10` flipped to `[x]` with the body written), `.claude/state/sequencer-run.json` still `"active": true`, the `sequencer-armed` marker still present, and `run-status.md` reporting `Armed: True` for a run that no longer exists.
  - MECHANISM, confirmed at source: `scripts/overnight/overnight-arm.sh:99-106` creates the main and watchdog units with `systemd-run --on-calendar`, i.e. TRANSIENT units that live under `/run` and vanish with the user manager. A WSL restart (or `wsl --shutdown`) removes both; nothing re-creates them, so the run cannot come back, and nothing reconciles the on-disk state that says it is live.
  - CONSEQUENCE, checkable on its own: the operator must (a) notice from a stale status that nothing is running, (b) finish or discard a half-shipped section by hand, and (c) re-arm. At this close-out (a) cost the first ten minutes of the session, (b) is being done by re-running the section's gates and shipping it attended, and until (c) happens the queue is stopped with `TODO-10` mid-file.
  - FIX SHAPE, not applied (design decision with an operator half): persistent unit files under `~/.config/systemd/user/` enabled with `WantedBy=timers.target`, plus `loginctl enable-linger` so the user manager starts at boot, OR a documented "the run does not survive a host restart" contract with `run-status.py` reporting liveness (`run-liveness.sh`) instead of the marker. Either way `run-status.py` should say DEAD when liveness says DEAD; today it reads a marker file that a dead run cannot remove.
  - Settled by: one of the two designs chosen and shipped with a control (a killed user manager either relaunches the run or leaves every status surface saying it is dead), and a restart survivability measurement that names the state left behind.

- [ ] `SKIP_REVIEW_HOOK=1` is still being blanket-prefixed onto non-commit commands, which is the opt-out-leak shape the doctrine forbids.
  - MEASURED at close-out across the 14 run logs since 2026-08-24: 21 occurrences of `SKIP_REVIEW_HOOK=1`; at least 6 were prefixed onto python edits and test/smoke runs (`SKIP_REVIEW_HOOK=1 python3 - <<'PY' ...`, `SKIP_REVIEW_HOOK=1 bash scripts/overnight/run-artifact.sh ...`), 3 onto honest stamp-only commits with a `SKIP_REVIEW_HOOK_REASON`, the rest unattributable from the truncated log line. `RECEIVING_REVIEW_OVERRIDE` appeared 4 times, `SKIP_TOOLING_SUITE=1` 6 times (all on the pack-only refusals now instrumented).
  - The v17 obligation ("opt-outs taken per segment, target zero") is therefore answered: not zero, and the dominant shape is the leak, not the stamp-only case the v16 fixes targeted.
  - NOT ROOT-CAUSED which gate was being dodged on each edit; the log line is cut at ~160 chars. Candidate: the post-ship Edit/Write block (`section_review_required`) that fires until a `Verified:` stamp lands, which makes every review-fix edit between ship and stamp an opt-out. Stated as a hypothesis.
  - Settled by: a per-segment count of opt-outs with their REASON strings (make `SKIP_REVIEW_HOOK_REASON` mandatory in the hook so a bare `=1` is itself refused), and the count reaching zero on edits.

## Carried forward from v17 -- open

Each carries its verdict and what would settle it. v17 closed with 22 resolved against 14 carried, so the carried set shrank for the first time in three cycles; keep it shrinking.

- [ ] Whether an EXPLICIT section cursor should survive a segment boundary at all ([v17](overnight-runner-improvements-v17.md) residue)
  - The disagreement is now a packet field (`cursor_disagreement`); the design question is not root-caused.
  - Settled by: reading `run_phase_guard.py cursor` semantics and either clearing `section_source` to `derived` at rollover or documenting why it persists.
- [ ] `test_build.sh` and `test_todo_fence.py` pack-only refusals ([v17](overnight-runner-improvements-v17.md))
  - No reproduction this stop; the nested-suite detail now names the assertion and keeps the log.
  - Settled by: one in-pack refusal with its kept `nested-*.log`, which either names a concurrent edit (the v17 hypothesis) or a suite-internal interference.
- [ ] Seventh flake member `query: reader buffer is unbounded on a newline-free stream` rc 134 ([v17](overnight-runner-improvements-v17.md))
  - One red against two green; `peak: 0` is the thread to pull from the kept log when it recurs.
- [ ] `review_convergence.py record` fingerprints at CALL time ([v16](overnight-runner-improvements-v16.md), half done in v17)
  - Dispatch-time digests (`head`, `index_tree`, `worktree_sha256`) are now in `manifest.jsonl`; `record` still fingerprints the tree when called.
  - Settled by: `record` preferring the newest manifest entry's digests for the slice, with a control that a post-dispatch edit still redispatches.
- [ ] Section-commit content-binding turns a 3-dispatch review into 8 ([v16](overnight-runner-improvements-v16.md))
  - Carried on difficulty; conservative-by-construction classifier plus two controls, as recorded.
- [ ] `todo-reflow.py` cannot see prose wrapped below 78 columns ([v16](overnight-runner-improvements-v16.md))
  - Needs a floor parameter before it can be measured; ship a floor only with the count-and-sample numbers recorded.
- [ ] `boot_info` loader self-measurement has no kernel carriage ([v16](overnight-runner-improvements-v16.md))
  - Slot: immediately after TODO-10 section 32 ships (it edits the same four files), attended.
  - Two `F()` rows, an ownership-matrix row, `BOOT_INFO_VERSION` bump in BOTH headers, one assignment from `g_self_measure`, rebuild both images.
- [ ] Identity gate out-of-era base rejection at resolution time ([v16](overnight-runner-improvements-v16.md); doc half closed in v17)
  - Control needed: a legitimately old in-era base is still accepted.
- [ ] `defer-preserve` phase verb for a hard-blocker revert ([v16](overnight-runner-improvements-v16.md))
  - Design; `git diff` omits untracked files, so the naive form is proven wrong.
- [ ] `Accepted:`/`Deferred:` XREF naming the wrong SECTION with a paraphrased item ([v16](overnight-runner-improvements-v16.md))
  - Measure the false-positive rate against every existing stamp, ship WARN, repair, promote.
- [ ] `four_dispatch_gate: stamp commit + all 3 dispatches recent allows` flake ([v16](overnight-runner-improvements-v16.md))
  - Zero live occurrences since 2026-08-24; settled only by a reproduction under load or an order-dependence result.
- [ ] `four_dispatch_gate: D` measures whole-hook wall-clock ([v16](overnight-runner-improvements-v16.md))
  - Zero live D failures; settled by one carrying its elapsed number.
- [ ] `section_review_required` mid-review block, instrumented, never observed ([v16](overnight-runner-improvements-v16.md))
  - The v18 opt-out-leak item above names this gate as the candidate being dodged; a live block with its reason string would settle both.
- [ ] Codex-waiter grace period needs the log-creation-latency measurement ([v16](overnight-runner-improvements-v16.md))
  - The wrong-path case is now `MISSING`, exit 5; the immediate-refusal form stays rejected.
- [ ] Two review legs co-SIGTERMed within 6s, unexplained ([v16](overnight-runner-improvements-v16.md))
  - No new occurrence; recovery works; still not naming a reaper without evidence.
- [ ] `review-envelope.py` has no HEAD-binding refusal of its own ([v17](overnight-runner-improvements-v17.md), hardening candidate)
  - The two-route manifest fix removes the observed failure; a binding check is belt and braces.
  - Settled by: refuse to serve a wave older than the newest dispatch for the section, with a control that the newest wave is still served.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Opt-outs per segment, WITH reasons.** BASELINE at the v17 close-out: 21 `SKIP_REVIEW_HOOK=1`, 4 `RECEIVING_REVIEW_OVERRIDE`, 6 `SKIP_TOOLING_SUITE=1` across 14 logs; at least 6 of the first were leaks onto non-commit commands. Target: zero leaks; every remaining opt-out carries a `_REASON`.
- **Pack-only refusal attribution.** BASELINE: 0 of the v17 cycle's ~7 pack-only refusals were attributable from the push log. Target: every one names its assertion via the nested detail; record what it named.
- **Does the worktree guard ever block a text-only command again, and does any mutation slip past a shape the 18 controls do not cover?** Baseline 0 and 0.
- **Duration-arm firing rate.** New: `SUBAGENT-RUNAWAY ... duration_ms=` lines in `acknowledged-but-skipped.log` per run; each must name a genuinely long dispatch.
- **`cursor_disagreement` firing rate**, and whether the worker acted on the oracle when it fired. Baseline: one live disagreement in the v17 cycle (60 vs 59).
- **`MISSING` (exit 5) waiter results.** Baseline 0; each one means a wrong path was polled, record its origin.
- **Restart survivability.** ANSWERED WORSE this cycle: a host restart mid-gate left 24 uncommitted files, `active: true`, an armed marker and a status page saying Armed. Measure again after the design above ships.
- **Does the bare-`--flag` guard ever refuse a prompt that had no bare token?** v17 cycle: 6 refusals, all naming a real token (`--is-ancestor`, `--detach`, `--stat`, `--foreground` twice, `--norc`); the one retry "with flags backticked" was refused again for `--foreground`, but the log line is truncated so whether a bare copy remained cannot be verified. Baseline stays 0 confirmed guard defects.
- **Mid-token reflow guard refusals outstanding.** v17 cycle: 0. Keep at 0.
- **Filing discipline: does the run SPLIT an item spanning forbidden+fixable?** Still unmeasured under a genuinely mixed item.
- **Control-plane suite flake set.** Members unchanged (six plus the rc-134 suspect); pass count on an unchanged tree still to be observed with the corpus snapshotted. `test_query_bounds.sh` fails on a staled `build/todo-cache.json` every time -- rebuild with `--keep-cache` before reading any of its failures as real.
- **Section-hygiene branching factor.** Baseline under 1 (v14: 0.86 over 24h).
- **`completed_drought` firing rate** on a genuinely quiet branch. Unchanged.
- **Does the fixpoint rebuild ever return non-zero in practice?** rc 3 never observed live.
- **The pre-push tooling suite receipt.** Fraction of ship pushes hitting a valid receipt; v17 cycle added the receipt route as the documented answer to a pack-only refusal.
- **J1 re-runs caused by attended commits.** Measured 2026-08-10: 3 operator commits forced `j1a -> j1b -> j1c`. Measure per attended session.
- **BSS headroom.** BASELINE: 8192 bytes at the v16 close-out; `build/kernel.map` absent at the v17 close-out (clean build dir). The first armed build re-establishes the number; a section that adds kernel static data must show the advisory was read.

## Found live this cycle

- [ ] The harness system prompt's "Do not call the AgentTool unless the user requested it" DIRECTLY contradicts `review-todo-section` step 7, which makes the auditor dispatch the primary path for every `src/kernel/` and `src/boot/` section.
  - Observed 2026-08-29 21:0x on the TODO-10 section-32 review. The section touches both `src/kernel/` and `src/boot/`, so step 7 calls for `kernel-quality-auditor` (Opus) + `concurrency-evidence-mapper` + `boot-quality-auditor`. The session was carrying an explicit instruction not to use the Agent tool unless the user asked, and the user's prompt asked only for the sequencer.
  - THE ROUTE IS THE FINDING, stated per the capture file's own rule: the review ran step 7 through the skill's SANCTIONED fallback ("The main-session full walk is REQUIRED only when the auditor was not dispatched"), reading `kernel-code-quality/SKILL.md` and `boot-code-quality/SKILL.md` and walking the gates inline. No gate was skipped and the walk found two real items (a missing POST16 bracket on a Phase 0 step that can `boot_halt`; an undeclared SMP discipline on the module singleton), so the outcome was sound.
  - What it COST, which is the part worth fixing: the inline walk pulled ~14 KB of skill text plus the gate greps into the main context instead of a sidechain, on a review that also ran 5 Codex legs. That is precisely the offload the fleet exists to avoid, and it will recur on EVERY kernel/boot section for as long as both instructions are live.
  - NOT fixed here: the conflict is in the harness prompt and the skill, both outside what an unattended run may edit. The decision an operator has to make is which one wins for a sequencer-invoked review; a one-line carve-out in either place settles it.
- [ ] `.githooks/pre-push` refused the section-32 ship at 2/1367 while the identical bytes passed 1367/1367 out of band, and the failing sub-test was NOT the one the standing gotcha names.
  - Measured 2026-08-29: pre-push reported `scripts/todo-graph/tests/test_build.sh FAIL (843/844)` plus `inline_churn_warn expected churn warning at op 25, got: <empty>`; the out-of-band `run-artifact.sh pack-oob` on the same tree reported `PASS 1367/1367`, envelope `state: complete exit: 0`.
  - The named failure was `identity gate: the success-path reap mutation did not leak, so 22ed proves nothing` (nested log `.claude/overnight/artifacts/nested-test_build-20260829-213018.log:480`) -- a mutation-SENSITIVITY self-check about process reaping, not an assertion about shipped behaviour. Both runs also logged `test-tooling.sh: line 20955/21042: Killed sleep 45|60`, so the killed-subprocess shape is present in the GREEN run too and is not the discriminator.
  - The standing gotcha (expires 2026-10-15) records this as `827/828` and does not name a sub-test, so a reader cannot tell whether a new refusal is the same phenomenon. It is worth recording that the failing sub-test ROTATES between occurrences: two different mutation/timing self-checks now, which points at the pre-push ENVIRONMENT (process budget, concurrent reaping) rather than at either test.
  - The receipt route worked exactly as documented and cost ~15 minutes of wall clock: pack out of band -> `tooling-receipt.py write` -> push, which then reported `receipt: a green tooling suite already ran on these exact bytes -- not re-running`. No opt-out was used.
