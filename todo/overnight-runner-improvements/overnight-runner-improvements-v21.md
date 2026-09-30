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

- [ ] Reasoning: a hostile-input guard built as a DENYLIST cost eight review rounds before the switch to an ALLOWLIST ended the class (TODO-10 section 24, 2026-09-29)
  - Observed: the raw-HTML net of `scripts/site/build.py` (commits `f3a1a8b1d`, `f2255bd87`) gained one refused shape per round: srcset, then srcdoc, svg, CDATA, abrupt comments, noscript, unfinished tags. Each was a real, measured bypass, and each fix only named that shape.
  - Inferred, not tested: an attribute and tag allowlist at round 3 would have closed rounds 4-12. The corpus used no raw HTML attributes at all (surveyed live), so the allowlist cost nothing. The doctrine rule "three failures of one class -> take the structurally different alternative" was applied at round 3 for URL parsing (WHATWG via Node), but not for HTML, because each HTML finding looked like a different class.
  - Second, smaller lesson: a fix that makes output depend on checkout state (a `git for-each-ref` inventory for longest-ref resolution) was accepted from a review and then had to be reverted, because it broke byte-reproducibility. A review suggestion that trades an invariant for a hypothetical deserves the invariant check BEFORE implementation.
  - Cost: 58 Codex dispatches on one host-tooling section (20 adversarial, 18 consistency, 18 perf, design, test-coverage) and about 2h35m of wall-clock.
  - Obvious fix (not applied, control-plane text): in the review-loop guidance, treat "each round names a new SHAPE of the same boundary (input the checker cannot see)" as one class for the three-failures rule, and ask "denylist or allowlist?" at the first such finding.
- [ ] Reasoning: a client state machine grown one state at a time cost five review rounds, each finding a new stale-state path (TODO-10 section 25, 2026-09-30)
  - Observed: the docs search combobox (`gh-pages/docs-template.html`) went all-at-once load, then progressive shards (round 3), then selection keep by href, then by page (round 4), then a manifest refetch on retry (round 6), then retry on ANY failed shard and invalidating old options (round 7), then emptying the old list DOM (round 8). Each finding was real and reproduced in Node; round 9 approved all three legs.
  - Inferred, not tested: one written table of states (manifest pending/loaded/failed x shards loading/failed/done x popup open/dismissed) with the allowed user actions per row, drawn at round 3, would likely have surfaced rounds 4-8 together. It is the same shape as the section-24 lesson: several rounds on one boundary are one class.
  - What worked, for reuse: `scripts/site/tests/search_harness.js` drives the SHIPPED inline script against a DOM stub and fetch stub, so every finding became a regression test with a mutation control that fired. It also caught two bugs before any reviewer did: Escape during load reopened results, and a failed shard refresh retried in a loop.
  - Cost: 29 Codex dispatches (design, test-coverage, 9 adversarial, 9 consistency, 9 perf) and about 1h10m wall-clock.
  - Obvious fix (not applied, control-plane text): when a review finding lands in a UI or protocol state machine, require a state table in the fix before the next round, not only the patched transition.
- [ ] Reasoning: a correct reject was reversed on new evidence without asking whether the new case had a cheaper fix inside the existing design; it cost two review rounds and a revert (TODO-10 section 25, 2026-09-30)
  - Observed: the "keep a legacy search.json" finding was rejected twice, then accepted in the post-ship review because it named a new case (a new template reading a browser-cached legacy file). The accepted fix split the URL (`search-v2.json`), which broke the v2 readers already deployed at `search.json`; all three legs caught it, and the split was reverted.
  - Tested: the new case is fully covered by revalidating the manifest after a failed load (a harness `cached` test with a mutation control that fired). The URL split was never needed.
  - Inferred: when a rejected finding returns with a new case, first ask what the smallest change is that covers only that case, and whether the proposed remedy touches a contract that is already deployed (a URL, a schema, a file name). A published URL is an ABI.
  - Cost: two extra three-leg rounds (6 Codex dispatches) and about 20 minutes.
- [ ] Reasoning: a docs page written from a source-file HEADER inherited its false safety claim; header prose is a claim to verify, not a fact to copy (TODO-10 section 26, 2026-09-30)
  - Observed: `docs/host-tools/ixfs-fsck.md` repeated `ixfs_fsck.c:17` ("a partial directory walk disables every freeing pass"). Two review rounds then found three read-failure paths that header does not cover (pass-7 reconcile, dangling-dirent clear, snapshot skip), filed as `01-boot-platform/TODO-22` section 8.
  - Same session, same shape, caught by self-check before review: "the recovery flow calls `ixfs_fsck`" (nothing calls it) and "the SDK parser ignores inline data" (it reads it at `ixfs-core.c:221`).
  - Inferred, not tested: for a docs section, a grep for callers plus one read of each guard the page names would have caught all five before the first dispatch; each cost a finding round.
  - What worked: running the tools the pages describe (`sdk/build.sh`, `test_ixfs_core`, `bootimg.py inspect`, `read-blackbox.sh`, `fsck.fat -n`) found four real defects no prose read would have: the 92-vs-128-byte inode drift, the "OK" report for a skipped tool, and the BlackBox FAT32 corruption.
- [ ] `build_offload_reminder` blocks a HOST SDK tool's `make test`, which is not the kernel suite (2026-09-30, TODO-10 section 26)
  - Observed: `cd sdk/src/ixfs-mount && make test >/dev/null 2>&1; ./test_ixfs_core ...` was refused as a bare in-context test run. That target only compiles `test_ixfs_core` with the host gcc (`sdk/src/ixfs-mount/Makefile`, `test:` rule); it runs no suite and produces two lines of output.
  - Worked around by calling gcc directly, which is exactly what the block was meant to route through `run-artifact.sh`; the route is the finding.
  - Obvious fix (not applied, control plane): exempt a `make` whose `-C` or preceding `cd` target is under `sdk/`, or match `make test-*` and the repo-root `make test` only.
