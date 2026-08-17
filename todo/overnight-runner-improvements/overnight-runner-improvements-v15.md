# Overnight Runner Improvements v15 -- Findings (opened 2026-08-16)

Opened at the 2026-08-16 close-out of [v14](overnight-runner-improvements-v14.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v16.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v15.md`](../token-saver/token-saver-v15.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment. **But before filing, ask where the FIX lands, not where the SYMPTOM appeared** (carried from v14): if every path the fix touches is outside the forbidden set, fix it in the owning TODO and file only the residue; when an item spans both, SPLIT it -- file the control-plane half, fix the rest.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFERRED, and say which you tested. Lead <= 250 chars; sub-bullet bodies <= 1,000, one idea per sub-bullet.

---

## What shipped in the 2026-08-16 v14 close-out, and is therefore UNDER TEST

Every change below carries a refusal-direction control (a case that must still be BLOCKED), because a canary exercises the happy path at scale and structurally cannot exercise the refusal path.

- **`sequencer_triage` now uses the shared `## N.` projection.** `section_stamps`/`file_lifecycle`/`collect_blockers` walk `_scan_sections` (fence + blockquote aware, nested-heading correct), and `todo-orphan-check.py`/`stranded_deferrals.py` adopted `classify_heading`. Private grammar deleted; fence-test residual set shrank. Watch for: a triage misclassification on a nested/fenced heading the 232-file differential (0 class diffs) did not cover.
- **`ci-check` reports a completed-run drought** (`completed_drought: true` at 5 consecutive newest-cancelled; NOTE-level, never sets `ours_red`). Watch for: firing on a legitimately-quiet branch (a single green head clears it), or NOT firing during a real timeout-cancel run.
- **Check-7 accepts a known non-owner that EXISTS** (capture files no longer wedge a path-limited commit). Refusal control: a NONEXISTENT path still gets rc 7.
- **`receiving_review_required` gates file-mutating Bash** (program-aware: interpreter bodies scanned, read-only tool args not, named mutators gated). Watch for: a false block on read-only Bash during reception (F3 class), or a mutating shape the deny-list misses (the commit-time gate is the real guarantee).
- **`_review_pipeline_passthrough` refuses `((` arithmetic and a leading command substitution.** Refusal control: `( git push ... )` ship shape and nested `( ( cmd ) )` still pass.
- **`skill_step_block` credits a receipt minted during the invocation** (bundled-dispatch no longer loses step 8). Refusal control: a prior section's receipt (before `started_ts`) never credits.
- **`review-envelope.py --section`** excludes another section's and unattributed legs. Watch for: a legitimately-attributed leg excluded (confirm the manifest carries `"section": N`).
- **`codex_review_completed` lock loop is monotonic.** Watch for: `four_dispatch_gate: D` still failing under load -- if so the residual (test measures whole-hook, not the lock section) is the cause.
- **Doctrine shipped:** convergence `record` at VERDICT time; envelope `--section`; `run-artifact.sh` backgrounded with the flag not `&`; WHPX absolute-path fallback; kernel-code-quality Gate 2 (count is never a slot bound); implement-unit-tests DOC-SYNC (4096 cap, leak column, harness primitives, coverage.md pointer).

## Found live this cycle

- [ ] Split predictor's `abi_impact` gate fires on PROSE and contradicts the file's own calibration note
      - OBSERVED 2026-08-17 on `todo/01-boot-platform/TODO-13 §15`: verdict `SPLIT-RECOMMENDED (6 work items; ABI impact + 6 work items (>=6 ABI gate))`, which lowers the split threshold from the default to 6 items. The section has no ABI change: it READS `boot_path`/`boot_reason`/`boot_source_flags`, which have existed in `struct boot_info` since v8 (`include/kernel/boot_info.h:1681-1683`), with no `BOOT_INFO_VERSION` bump and no mirror edit.
      - MECHANISM CONFIRMED AT SOURCE, not inferred: `scripts/overnight/section-manifest.py:431-433` sets `abi_impact` from `re.search(r"(?i)\b(ABI|NTSTATUS|SSDT|boot_info|...)\b", block)` over the section BODY TEXT. §15's body cites `boot_info.h` four times as evidence for where the recovery signals already live, so the flag fires on a citation. Any section that merely NAMES the header trips it.
      - The same file already records the arm as non-predictive: the calibration comment at `section-manifest.py:441-443` says `open_items` is the only feature with predictive power (r = +0.50) and that `abi_impact` scores **r = -0.22** against turns, i.e. sections mentioning ABI ran SHORTER. Line 491 nonetheless uses it to TIGHTEN the threshold, so the one arm measured to point the wrong way is wired to make splits more likely.
      - Cost this occurrence: one waiver authored, validated and carried (`waiver-check` rc 0), plus the reads to disprove the ABI premise. Cheap once; it recurs on every section whose body cites a header the regex names.
      - Do NOT apply -- `scripts/overnight/**` is control plane. Settled by either dropping the `abi_impact` arm from the threshold (its measured sign says it should never tighten) or deriving it from the section's actual likely_files diff surface rather than prose. Ship with a control that a section genuinely bumping `BOOT_INFO_VERSION` still flags.

- [ ] `test-tooling.sh` mutates the LIVE review-state file, so a pre-push run during an active session both fails spuriously and destroys that session's review receipts
      - OBSERVED 2026-08-17 on the TODO-13 §17 ship push: `FAIL 1/1358 tooling tests failed -- receiving_review_gate: PreToolUse allows Edit when no state file (want 0, got 2)`, blocking the push. Three Codex reviews were in flight at the time.
      - MECHANISM CONFIRMED AT SOURCE: `scripts/test-tooling.sh:1683` sets `STATE_FILE="$REPO_ROOT/.claude/state/last-codex-review.json"` -- the REAL file, not a temp copy. Sub-test 9 (`:1798-1800`) does `rm -f "$STATE_FILE"` then probes `receiving_review_required.py` expecting rc 0. A concurrent dispatch's PostToolUse hook re-creates that file between the `rm` and the probe, the gate correctly blocks, and the test reads rc 2.
      - TWO costs, and the second is the worse one. The visible cost is a blocked push on a green tree. The invisible cost is that the suite DELETES and rewrites the session's own `last-codex-review.json`, so an in-flight review's receipt can be destroyed by a test run -- the receiving-review gate and the section-commit gate both read that file.
      - Not a flake in the usual sense: it is deterministic given overlap, and the overlap is normal (a ship push runs the pre-push suite while the session may still be waiting on reviews). It is the same class as the already-recorded `flock` fix for concurrent suite runs, but for STATE rather than for the tree.
      - Do NOT apply -- `scripts/test-tooling.sh` is control plane. The fix is to point `STATE_FILE` at a temp dir for the duration of that sub-test (the hook already honors an override path in other tests), or to make the sub-test skip when a live review record exists.

## Carried forward from v14 -- open

- [ ] `subagent_audit` duration arm is dead and needs a dispatch-time-stamp companion
      - `duration_ms` is None in 1893/1893 SubagentStop payloads; the false comment claiming it is always present was fixed in v14, but the arm has never fired.
      - The real fix is a PreToolUse-on-Agent hook stamping a start time keyed by agent_id, with elapsed computed at Stop. Re-baselining `RUNAWAY_TOOL_USES` alone is explicitly NOT the fix -- it quiets the count noise while leaving the long-wall-clock-few-calls failure invisible.
- [ ] `four_dispatch_gate: D` measures whole-hook wall-clock, not the lock section
      - The hook lock loop is now monotonic (v14), so the clock-step mechanism is closed; but under genuine load the pre-lock subprocess calls can burn the holder's window and fail the test.
      - Settled by timing the lock section only (hook emits lock-wait duration; test reads it) or splitting `could not acquire` from a generously-bounded elapsed sanity check.
- [ ] `section_review_required` mid-review block is instrumented, awaiting a live reproduction
      - It now returns a reason string naming which of five conditions failed; the v14-filed mechanism ("reception displaced it") did NOT survive source verification, so the true cause is unknown.
      - Settled by one live block carrying the reason string.
- [ ] Codex-waiter against a mistyped log path -- NOT REPRODUCED as filed
      - The filed "refuse if the log is missing at start" fix is unsafe: the broker creates the log ASYNC in the detached process, so a missing log is legitimately a still-starting review (an existence check broke `test_missing_log_returns_still_running` and was reverted).
      - The underlying waste (a genuine typo waits the full bound) is real; settled by a grace-period-then-refuse refinement, not an immediate refusal.
- [ ] Filing discipline: ask where the FIX lands, not where the SYMPTOM appeared
      - Partially shipped as the "Why findings land here" preamble wording above; measure whether the run actually splits an item spanning forbidden+fixable rather than carrying the whole bundle.
- [ ] Section-commit content-binding turns a 3-dispatch review into 8
      - Every fix stales the reviews that prompted it. Exempt comment-only deltas from the staleness computation (a comment cannot change an adversarial/consistency/perf finding). Control-plane refinement; ship with a control that a real source edit still stales.
- [ ] Two review legs co-SIGTERMed within 6s -- unexplained
      - Filed unexplained on purpose (a guessed cause is worse). Recovery already works (`needs_redispatch` named both). Settled by a reproduction that identifies the shared reaper.
- [ ] `alias-staleness` promotion-class has no automation and no owner-side filing
      - 4 fixed findings past the promotion threshold, belonging to the todo-metadata-layer TODO the run is not the cursor for. Settled by a judgment on whether alias staleness is producer-assertable, filed into that TODO with an IO-table row.
- [ ] Reasoning lessons carried as standing guidance (promote to doctrine when the owning skill is next edited)
      - Root-fix-reflex: two consecutive rounds each fixing a defect the previous introduced = stop and simplify.
      - Reject-on-wrong-premise: state the premise as a separate checkable sentence, not folded into the conclusion.
      - Real-mechanism-wrong-consequence: when a finding says "X can then do Y", verify Y at its own file:line.
      - Grep the CONCEPT (every field/string carrying a semantic), not only the symbol whose definition moved.
      - Never suppress stderr on `git add` in the ship sequence; use `git show <rev>:<path>` for read-only history questions.
      - Diff top-level definition sets after any scripted multi-line source deletion; the build alone will not catch a rarely-compiled path.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Control-plane suite flake -- the snapshot-the-corpus decision is now DUE.** v14 recorded the THIRD test joining the corpus-under-walk flake set (`section_commit_gate: old review with matching content binding`, alongside `test_reachability_gate` and `test_stub_lint_coverage`). Per the baseline's own rule, three is the threshold: snapshot the corpus / isolate fixtures so a suite run cannot fail on the corpus moving under it. Measure: does a fourth arrive; does the snapshot land.
- **Section-hygiene branching factor.** Baseline: under 1 (v14: 0.86 over 24h). Watch it stays under 1; continuation waivers demanded (0 to date).
- **`completed_drought` firing rate.** New this cycle. Baseline: 0 at close-out. Measure how often it fires and whether any firing was a false alarm on a quiet branch.
- **Does the fixpoint rebuild ever return non-zero in practice?** Still unknown; rc 3 never observed live.
