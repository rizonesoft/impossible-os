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

- [ ] Arming accepts a REVOKED OAuth token: the run then dies at launch and the circuit breaker disarms it, two minutes after the operator walked away
  - OBSERVED 2026-09-28 15:40-15:41, attended arm with `--force --hours 48`: both the watchdog launch (`run-20260928-154004`) and the main launch (`run-20260928-154104`) ended in 5 s with `Failed to authenticate. API Error: 401 OAuth access token is invalid.`, then `Circuit breaker tripped: consecutive unproductive runs -- STOPPING and disarming.`
  - MECHANISM, confirmed at source: `arm-sequencer.sh:199-217` checks that `~/.conclave/secrets/claude-oauth-token.env` exists and carries a `CLAUDE_CODE_OAUTH_TOKEN=` line; nothing checks the token authenticates. Its own `expires_at` was 2027-09-13, so the token was revoked, not expired, and a date check would not catch it either.
  - Fix shape: before writing any timer, run one minimal authenticated call with that env file (`claude -p` with a one-word prompt and one turn) and REFUSE to arm on a 401, naming the re-mint command. Costs one tiny request per arm.
  - SECOND, operator-side lesson: the attending session watched `reports/latest.log`, which still pointed at the 2026-09-05 log until the new run repointed it, so the first check read an old run as the new one. Watch `systemctl --user status` or the newest `run-<date>-*.log`, never the symlink, until the new log exists.
