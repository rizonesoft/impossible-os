# Overnight Runner Improvements v16 -- Findings (opened 2026-08-17)

Opened at the 2026-08-17 close-out of [v15](overnight-runner-improvements-v15.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v17.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v16.md`](../token-saver/token-saver-v16.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment. **But before filing, ask where the FIX lands, not where the SYMPTOM appeared:** if every path the fix touches is outside the forbidden set, fix it in the owning TODO and file only the residue; when an item spans both, SPLIT it -- file the control-plane half, fix the rest.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFERRED, and say which you tested. Lead <= 250 chars; sub-bullet bodies <= 1,000, one idea per sub-bullet.

**And state the CONSEQUENCE as its own checkable sentence.** The v15 close-out overturned a finding whose mechanism was correct and whose consequence was never checked: the arm it named could not reach the outcome it was blamed for, and one line of threshold arithmetic showed it. When writing "X causes Y", verify Y at its own file:line, separately from X.

---

## What shipped in the 2026-08-17 v15 close-out, and is therefore UNDER TEST

Every change below carries a refusal-direction control (a case that must still be BLOCKED), because a canary exercises the happy path at scale and structurally cannot exercise the refusal path.

- **The split predictor's `abi_impact` arm is GONE from the verdict.** `section-manifest.py` no longer emits an ABI-weighted split reason; `abi_impact` is still computed and still reported in `complexity`. Removal rather than re-tuning, because the arm was dominated by the general item gate (fired at `>= 6` loose against a general gate at `>= 5`) and because the 2026-07-28 calibration measured it at r = -0.22. Refusal controls, BOTH mutation-proved to fire: `test_abi_arm_cannot_change_a_verdict` fails if any reintroduced arm actually tightens (caught a `>= 3` variant at n=3 with ABI-free reason text), and `test_abi_8_item_section_now_splits` fails on ANY verdict string containing "ABI". Watch for: a genuinely oversized ABI section that the item gate alone now under-calls.
- **`test-tooling.sh`'s receiving_review_gate block runs in a SANDBOX.** It builds a temp git repo, copies the four hooks in, and pushes into it so the writer hook's `git rev-parse` root and the reader hook's `__file__` state path both land inside. The backup/restore pair is deleted with the live-path dependency. Refusal controls: the negative sub-tests still expect rc 2, and a new assertion hashes the live `last-codex-review.json` before and after and fails if the suite touched it. Watch for: a sub-test that silently depended on real repo content and now passes vacuously in the sandbox.
- **`subagent_audit` records the union of SubagentStop payload KEY NAMES** to `.claude/state/subagent-payload-keys.json` (names only, never values; written only when a new key appears). This is an ENABLER, not a fix -- it settles whether a correlator exists for the dead duration arm. Watch for: read this file after one segment and act on it (see the carried item below).
- **`TODO-06 §49` files the `alias-staleness` promotion class** with an Implementation Order row, so the triage oracle can see it. Watch for: it being treated as the class's ANSWER rather than its owner -- the section's whole point is that the producer-assertable judgment has not been made.

## Found live this cycle

<!-- The run files here. Nothing yet: v16 opened at close-out, before the next arm. -->

## Carried forward from v15 -- open

- [ ] `subagent_audit` duration arm is dead; the enabler is shipped and its ANSWER is now the next step
      - `duration_ms` non-null in 0 of 1912 SubagentStop payloads (recounted 2026-08-17, was 0/1893 at v14).
      - THE PRECONDITION, identified at the v15 close-out after two cycles of restating the fix: the proposed repair keys a PreToolUse stamp to an agent id, and it was never established that the Stop payload carries a correlatable id at all.
      - **Concrete next step, cheap and deterministic:** after one segment, read `.claude/state/subagent-payload-keys.json`. If it names a correlator (a session or agent id), build the PreToolUse-on-Agent stamp. If it does not, DELETE the arm rather than repair it -- a guard that cannot fire is worse than no guard, because it reads as coverage.
- [ ] `four_dispatch_gate: D` measures whole-hook wall-clock, not the lock section
      - Did not fire in any of the 6 segments of the 2026-08-16/17 run. The lock loop is monotonic (v14), so the clock-step mechanism is closed; under genuine load the pre-lock subprocess calls can still burn the holder's window.
      - Not fixed deliberately: both remedies edit a currently-green assertion with no failure in hand, which is the churn this corpus recorded three times against `ROTATE_HINT_TURNS`. Settled by ONE live D failure carrying its elapsed number.
- [ ] `section_review_required` mid-review block is instrumented, still awaiting a live reproduction
      - Two full cycles with no firing: swept all 6 segment logs of the 2026-08-16/17 run and found 12 mentions of the hook name, none of them a block carrying the instrumented reason string.
      - The v14 guessed mechanism ("reception displaced it") already failed source verification, so guessing again is worse than waiting. Settled by one live block carrying the reason string.
- [ ] Codex-waiter against a mistyped log path -- the immediate-refusal fix is REJECTED; the grace-period form needs a measurement
      - The broker creates the log ASYNC in the detached process, so a missing log is legitimately a still-starting review. An existence check broke `test_missing_log_returns_still_running` and was reverted.
      - The waste is real (a genuine typo waits the full bound) but the grace period needs ONE measured number: the broker's actual log-creation latency. Choosing it by reasoning is the failure mode this corpus keeps paying for.
      - Settled by: instrument the broker to record log-creation latency for one run, set the period above the observed maximum, and ship a control that a still-starting review is never refused.
- [ ] Section-commit content-binding turns a 3-dispatch review into 8
      - The largest single cost number in this corpus, carried on DIFFICULTY rather than doubt. Every fix stales the reviews that prompted it.
      - The design problem: "comment-only" is a claim about meaning that a gate must decide mechanically across `.c`, `.h`, `.asm`, `.py`, `.sh` and `.md`, with string literals containing comment markers and vice versa. The failure is asymmetric -- a miss costs dispatches, a false positive silently accepts a review that no longer matches the committed source, which is the gate's entire guarantee.
      - Settled by: a conservative-by-construction classifier spec (unparseable or mixed hunk means NOT comment-only), plus two controls -- a real source edit still stales, and a hunk mixing a comment with a code change still stales.
- [ ] Two review legs co-SIGTERMed within 6s -- unexplained, still no reproduction
      - Swept all 6 segment logs of the 2026-08-16/17 run: one SIGTERM appears and it is a different event (a ship push reporting its own, commit still local, re-pushed fine).
      - Staying unexplained on purpose. Recovery works (`needs_redispatch` named both), so carrying it is bounded; guessing a reaper and "fixing" it is not. Settled by a reproduction that identifies the shared reaper.
- [ ] Reasoning lessons carried as standing guidance (promote to doctrine when the owning skill is next edited)
      - Real-mechanism-wrong-consequence: when a finding says "X can then do Y", verify Y at its own file:line. This one earned its keep at the v15 close-out -- it is what overturned the `abi_impact` finding.
      - Verify the probe before trusting the count: re-measure, never copy a number forward. Caught nothing wrong this cycle but re-confirmed both the 0/1912 duration count and the ledger class counts.
      - Root-fix-reflex: two consecutive rounds each fixing a defect the previous introduced = stop and simplify.
      - Reject-on-wrong-premise: state the premise as a separate checkable sentence, not folded into the conclusion.
      - Grep the CONCEPT (every field/string carrying a semantic), not only the symbol whose definition moved.
      - Never suppress stderr on `git add` in the ship sequence; use `git show <rev>:<path>` for read-only history questions.
      - Diff top-level definition sets after any scripted multi-line source deletion; the build alone will not catch a rarely-compiled path.
      - Settled by: promotion into `review-todo-section` or `superpowers:receiving-code-review` the next time either is edited for another reason, so it costs nothing extra.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Filing discipline: does the run SPLIT an item spanning forbidden+fixable?** Baseline 2026-08-17: 2 of 2 filings compliant, but 0 qualifying cases (both items were control-plane-only, so there was no half to split). Still unmeasured under a genuinely mixed item.
- **Control-plane suite flake -- the snapshot-the-corpus decision is OVERDUE, and a fourth symptom arrived.** Three tests already join the corpus-under-walk flake set (`section_commit_gate: old review with matching content binding`, `test_reachability_gate`, `test_stub_lint_coverage`). New 2026-08-17: `test_decision_registry_contract.py` fails both its cases when `build/todo-cache.json` vanishes mid-run, which is exactly what a concurrent `build-and-validate.sh` without `--keep-cache` does. Two consecutive runs of the same unchanged tree scored `77 passed, 2 failed` then `78 passed, 1 failed`. Measure: does the snapshot land, and does the pass count stop moving on an unchanged tree.
- **Section-hygiene branching factor.** Baseline: under 1 (v14: 0.86 over 24h). Watch it stays under 1; continuation waivers demanded (0 to date).
- **`completed_drought` firing rate.** Baseline: 0 at the v14 close-out, still 0 observed. Measure how often it fires and whether any firing was a false alarm on a quiet branch.
- **Does the fixpoint rebuild ever return non-zero in practice?** Still unknown; rc 3 never observed live.
- **Restart survivability.** New 2026-08-17: a WSL restart killed the run cleanly (tree clean, all pushed, nothing lost) but the systemd units are TRANSIENT, so they vanished with `/run` and the run did not come back on its own. Measure: whether any future host restart leaves work in a worse state than clean-and-pushed, since that is the property that made this one cheap.
