# Overnight Runner Improvements v21 -- Findings (opened 2026-09-29)

Opened at the 2026-09-29 close-out of [v20](overnight-runner-improvements-v20.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT file the run appends to; the close-out that follows the next stop triages every item and records a verdict for each.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v21.md`](../token-saver/token-saver-v21.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface; a bad gate edit with nobody watching is unrecoverable. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFER, and state the CONSEQUENCE as its own checkable sentence. Quote verbatim what a gate says it matched. **Verify a probe with a control whose effect you can predict, and check the control for SEMANTIC effect.**

## Reasoning and autonomy findings -- file these too

File reasoning lessons as `- [ ]` items like any other finding (the nine shapes are listed in [v12](overnight-runner-improvements-v12.md)). The v20 cycle added four, now written into the skills that act on them:

- **A broker call is never a probe.** Test a prompt against the guard's `case` pattern locally; a "probe" dispatch is a real review leg that cannot be cancelled.
- **Apply the review scope rule at the FIRST out-of-surface finding** (`review-todo-section` step 13), and never answer a reviewer's scenario by weakening what a guard exists to prevent.
- **Name the reference parser at design time** (`codex-design-review`): a section parsing a standard format asks which implementation to delegate to, not which edge cases its own parser misses.
- **A gate that passes locally can still fail in CI on the toolchain build.** The v20 red `main` came from Ubuntu's apt Node serializing URLs differently from the official build; reproduce CI-only failures in an `ubuntu:24.04` container before guessing.

---

## What shipped in the 2026-09-29 v20 close-out, and is therefore UNDER TEST

Every change below carries a refusal-direction control.

- **`token-probe.sh` at arm time.** One Haiku call with the token file's token; a 401/invalid/revoked answer refuses to arm. Control: a rejected token refuses, and an echoed token never appears in the output. Watch: the `token-probe:` line on every arm; an `inconclusive` line means the probe could not judge.
- **`reset-breaker.sh` at arm time, before the pre-arm gate.** Clears backoff, unproductive streak, no-ship streak and `NEEDS-OPERATOR.md`, printing what it cleared. Controls: skipped while a timer is active; the launcher never calls it. Watch: a `breaker reset:` line naming a value means the previous run ended on a trip.
- **Every launcher stop clears the run state, and `attended_repair_guard` requires the armed marker.** Controls: `test_deadline_abort.py` fails if a stop path loses the clear; active state plus marker still blocks. Watch: `sequencer-run.json` must read `active: false` after every stop.
- **`agent_coverage_gate` recognises subagents by payload markers.** Control: a main-session whole-file re-read still blocks. Watch: `sub tool error: [AGENT-COVERED` should be 0 (v20 baseline 61).
- **Broker bare-flag refusal says the backtick must touch the flag.** Control: a spaced code span is still refused. Watch: a second refusal on the same flag within one dispatch attempt (v20: 19 refusals, 11 on `--cached`).
- **`t_fail` keeps its detail under `--quiet`; `.githooks/pre-push` exempts a baseline-only `stub-lint-baseline.json` push; the sequencer skill's J1 block uses `run-artifact.sh`.** Controls in `test_tooling_fail_detail.py`, `test_prepush_tooling_trigger.py`, `test_build_offload_reminder.py`.
- **`build.yml` builds every push to `main`** (grouped by SHA, never cancelled); pull requests still cancel superseded runs. Watch: every main push has a completed Build verdict.
- **Agents (attended, same day):** model pins are aliases only (Check 14), `kernel-quality-auditor` stays on Opus, 14 evidence agents set `omitClaudeMd: true`, and `fork_context_guard.py` refuses a fork from a context above 150K.

## Carried forward from v20 -- open

v20 closed 13 resolved, 4 rejected, 0 not reproduced, 1 carried. The successor inherits the item, not the triage.

- [ ] `section_commit_gate` step-8 test-wiring binds tests to the source BASENAME, which 434 of 516 kernel sources cannot satisfy ([v19](overnight-runner-improvements-v19.md), [v20](overnight-runner-improvements-v20.md))
  - Not exercised in the v20 docs-only cycle (0 commit-gate refusals in 21 logs). Settled by refusal data from a kernel cycle, then a same-commit test that registers a suite named for the staged basename, shipped WARN first with anti-`test_heap.c` refusal controls.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Refusals per class, main loop and subagents separately.** BASELINE (v20, 21 logs, first labelled cycle): main 72 of 3,539 calls (2.0%): `receiving-review-required` 23, broker bare-flag 19, genuine `Exit code` 14, other 7, `skill-step-block` 5, `SEQ-*` 2, `READ-CACHED` 1, `build-offload` 1. Subagents 108: `AGENT-COVERED` 61, oversized or missing file 22, ripgrep pattern errors (look-around, brace globs) 14, other 11. Targets: `AGENT-COVERED` 0, broker bare-flag repeats 0.
- **Opt-outs per segment, WITH reasons.** BASELINE (v20): 2 `SKIP_DISPATCH_GATE` (one stamp repair), 0 `SKIP_REVIEW_HOOK`, 0 `SKIP_SKILL_STEP_BLOCK` (v18: 11), 0 `SKIP_TOOLING_SUITE`, 0 `SKIP_CI_PARITY`.
- **CI verdict coverage on `main`.** BASELINE: 14 of the last 30 Build runs completed (16 cancelled) before the `build.yml` change. Target: every main push completes.
- **Arm-time lines.** Record the `token-probe:` and `breaker reset:` lines of every arm. Baseline: none yet.
- **Run state after a stop.** `sequencer-run.json` `active` after each deadline, fixpoint or disarm stop. Baseline: `true` after the v20 deadline stop (the defect).
- **Restart survivability.** Does `run-status.md` ever say `Armed: True` without `STALE MARKER` while liveness says DEAD.
- **Runaway-arm firing rate.** BASELINE: count arm 0 of ~21 at 60 expected; duration arm 0. Each firing must name a genuinely long or looping dispatch.
- **Pre-push tooling pack refusals.** v20: 1 refusal on the pushes measured, a real assertion (the lint fixture that lacked a `model:` line, attended). Target: 0 flakes, and every refusal readable from the push log now that `t_fail` keeps its detail.
- **Build CPU rate.** BASELINE 118-153 ms/MiB on ~13.5 MB (2026-09-28). Record the Test 7 line from any pack run.
- **Kernel image headroom.** Re-establish from the first armed build; a kernel section reaching link time without reading the gate line is the regression to record.
- **Control-plane suite flake set.** Members unchanged. `test_query_bounds.sh` fails on a staled `build/todo-cache.json` every time; rebuild with `--keep-cache` before reading any of its failures as real.

## Found live this cycle

<!-- The run files here. Nothing yet: v21 opened at close-out, before the next arm. -->
