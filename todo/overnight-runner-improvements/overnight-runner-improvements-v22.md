# Overnight Runner Improvements v22 -- Findings (opened 2026-11-06)

Opened at the 2026-11-06 close-out of [v21](overnight-runner-improvements-v21.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT file the run appends to; the close-out that follows the next stop triages every item and records a verdict for each.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v22.md`](../token-saver/token-saver-v22.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface; a bad gate edit with nobody watching is unrecoverable. Record the finding in the same turn it is observed, then advance.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Separate what you OBSERVED from what you INFER, and state the CONSEQUENCE as its own checkable sentence. Quote verbatim what a gate says it matched. **Verify a probe with a control whose effect you can predict, and check the control for SEMANTIC effect.**

## Reasoning and autonomy findings -- file these too

File reasoning lessons as `- [ ]` items like any other finding (the nine shapes are listed in [v12](overnight-runner-improvements-v12.md)). The v21 cycle added these, now written into the skills that act on them:

- **Several rounds on one boundary are one class** (`review-todo-section` step 13.5): by round 3, change the structure rather than patch the next shape.
- **A rejected finding that returns gets the smallest change that covers its case**, and a published URL, schema or file name is an ABI (same step).
- **A source comment is a claim to check** (`docs/contributing/docs-page-contract.md`).
- **A green failure-injection test proves nothing until its control has run** (`implement-todo-section` test wiring).
- **Kill a helper by its recorded pid, never by a command-line pattern** (`overnight-sequencer`).

---

## What shipped in the 2026-11-06 v21 close-out, and is therefore UNDER TEST

Every code change below carries a refusal-direction control, and each was mutation-checked.

- **`build_offload_reminder` exempts a host SDK `make test`.** Only `make -C <sdk dir> test` and a command opening `cd <sdk dir> && make test`. Control: 14 shapes that reach the repo root still block. Watch: `build-offload BLOCK` should name only real suite runs (v21: 1, this false positive).
- **`todo-staged-check` admits a section deferred whole on a named owner.** Needs a same-commit `> **Deferred:**` stamp and an `XREF:` to another TODO file on each added `- [/]`. Controls: no XREF, same-file XREF, no Deferred stamp, and an owner on a neighbouring item only all still refuse. Watch: `SKIP_TODO_STAGED_CHECK` should be 0 (v21: 2 mentions).
- **`review_convergence` scopes `gh-pages/`.** A template-only fix used to answer CONVERGED without a look. Control: a path outside every scope still leaves the fingerprint alone. Watch: redispatch counts on site-template sections.
- **`arm-sequencer.sh` warns on findings with no verdict** (`capture-open-findings.py`). Advisory, because a re-arm inside one cycle is legitimate. Watch: does the line ever fire on a same-cycle re-arm, and is it read on a skipped close-out. Promote to a refusal only with that evidence.
- **Writer pinned to medium.** `arm-sequencer.sh` now defaults to `--effort medium` rather than inheriting the saved CLI default; `--effort inherit` still works. Watch: the launcher's `model:` line must read `effort medium`, and metrics records must carry it.
- **Every agent at `effort: medium`, and `gh-query-runner` on Haiku.** Check 14 refuses a missing or different effort line. Control: a fixture agent without it and one at `high` both fail lint. Replay tasks for the agent are in `scripts/overnight/replay/gh-query-runner.jsonl`.
- **Host toolchain repair after the Ubuntu 26.04 upgrade.** GCC 15's C23 default broke a host tool (`HOST_CC` now pins `gnu11`) and binutils 2.46 removed `efi-app-x86_64` (the loader recipe now names `pei-x86-64` and `efi-app`). Both failed only on a clean build, so every smoke leg failed while incremental builds stayed green. `setup.sh --verify` also failed NASM 3.01's two-part version as unparseable. `/tmp` is now tmpfs, so an explicit `git clone --local` fixture could not hardlink across filesystems; it now lets git fall back to a copy. Controls: `test_host_cc_std.py`, `test_efi_image_format.py`, `test_setup_tool_versions.py`. Codex found no EFI or ABI regression, but neither binutils 2.42 nor real firmware was exercised: watch the first CI run, and boot the image on bare metal.
- **Commit identity.** A test fixture's name sat in `.git/config` from 2026-08-08, so 674 commits on `main` carry it. The `test_build.sh:38` guard stops the leak, the setting is removed, `.mailmap` maps the history for git (operator decision: no rewrite, GitHub's pages still show it), and `.githooks/pre-push` refuses a new fixture identity. Controls in `test_push_identity.py`. Watch: `git config --get-all user.name` must read one real name after every gated push.
- **Doctrine:** the review, implement, sequencer and docs-contract rules listed above. Also corrected: the review skill's description of convergence scopes and of receipt freshness (commit-bound, with a 30-minute window only for legacy entries), the stale reviewer model name in two docs, and `CLAUDE.md`'s claim that the arm catches a forgotten close-out.

## Carried forward from v21 -- open

v21 closed 10 resolved, 3 rejected, 0 not reproduced, 1 carried. The successor inherits the item, not the triage.

- [ ] `section_commit_gate` step-8 test-wiring binds tests to the source BASENAME, which 434 of 516 kernel sources cannot satisfy
  - History: [v19](overnight-runner-improvements-v19.md), [v20](overnight-runner-improvements-v20.md), [v21](overnight-runner-improvements-v21.md). Not exercised in the docs-only v20 and v21 cycles.
  - Settled by refusal data from a kernel cycle, then a same-commit test that registers a suite named for the staged basename, shipped WARN first with anti-`test_heap.c` refusal controls.

## Found at the v21 close-out -- open

- [ ] OPERATOR DECISION: should Medium findings keep a review loop going past round 8?
  - Measured on the last 500 triaged findings (all TODO-10, 2026-09-28 to 09-30, so one domain over three days): 429 Medium, 59 High, 12 Low. Across 831 stamped sections, Codex dispatches ran p50 5, p90 13, p99 32, and the three highest (58, 59, 62) are all documentation-site sections, above the kernel's exception-handling section at 42.
  - Proposal, not applied: from round 8, outside `src/kernel` and `src/boot`, only Critical and High continue the loop and valid Mediums are filed as named items in the owning section.
  - Lowering Astra's effort at round 8 was considered and advised against: late rounds still find real defects, and a shallower pass can look like convergence. It would also need a change to the Codex invocation policy. Settled by the operator.
- [ ] The finding ledger cannot measure the review chain: it keeps 500 entries (3 days at site-review rates) and records no section or round
  - `scripts/overnight/finding-ledger.py:44`. Consequence: yield per round, and whether the second adversarial pass after the ship finds anything, are not measurable, which is the evidence the decision above needs. Separately, 187 of 500 entries carry no `class`. Settled by a retained history plus `section` and `round` fields.
- [ ] `run-status.md` still read `Armed: True   Live: LIVE` five weeks after the stop, last written 2026-09-30 14:25, while `run-liveness.sh` says DEAD
  - Observed at this close-out; this answers the standing restart-survivability question with a yes. Inferred, not tested: only the live run writes the file, so nothing refreshes it after a stop. The mechanism is unconfirmed. Settled by finding the writer and having the stop path, or the reader, mark it stale.
- [ ] `quality-review-section` and `codex-review-todo` are never scheduled, so no review looks across sections of a file
  - Observed: neither name appears in `run_phase_guard.py` or the sequencer skill. Every automatic Codex review is section-scoped, plus the file-level gap audit before implementation. Whether that gap costs anything is unmeasured. Settled by running `codex-review-todo` once on a closed file and counting cross-section findings.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Refusals per class, main loop and subagents separately.** BASELINE (v21, 9 logs): main 58 of 2,202 calls (2.6%, v20 2.0%): `receiving-review-required` 23, broker bare-flag 16, genuine `Exit code` 10, other 4, `skill-step-block` 2, `SEQ-*` 2, `build-offload` 1, `READ-CACHED` 0. Subagents 2 of 203 (v20 108): ripgrep brace-glob errors 2, `AGENT-COVERED` 0 (v20 61, so that fix held). Targets: broker bare-flag repeats 0.
- **Opt-outs per segment, WITH reasons.** BASELINE (v21): 1 `SKIP_REVIEW_HOOK` (one docstring parenthetical after review), 1 `SKIP_CI_PARITY` (capture-file commit), 2 mentions of `SKIP_TODO_STAGED_CHECK` (whole-section deferral), 0 `SKIP_DISPATCH_GATE`, 0 `SKIP_SKILL_STEP_BLOCK`, 0 `SKIP_TOOLING_SUITE`.
- **CI verdict coverage on `main`.** BASELINE: 20 of 20 Build runs succeeded after the `build.yml` change, 0 cancelled (v20: 14 of 30 completed). Target held; keep it.
- **Arm-time lines.** Record the `token-probe:`, `breaker reset:` and new capture WARNING lines of every arm. They do not appear in the run logs (0 of 9), so read them from the arm output. Baseline: none yet.
- **Run state after a stop.** `sequencer-run.json` `active` after each stop. BASELINE: `false` after the v21 stop (v20: `true`, the defect). Fix held.
- **Runaway-arm firing rate.** BASELINE (v21): 0 in the run logs. Each firing must name a genuinely long or looping dispatch.
- **Pre-push tooling pack refusals.** Target: 0 flakes, and every refusal readable from the push log.
- **Build CPU rate.** BASELINE 118-153 ms/MiB on ~13.5 MB (2026-09-28). Record the Test 7 line from any pack run.
- **Kernel image headroom.** Re-establish from the first armed build; a kernel section reaching link time without reading the gate line is the regression to record.
- **Control-plane suite flake set.** Members unchanged. `test_query_bounds.sh` fails on a staled `build/todo-cache.json` every time; rebuild with `--keep-cache` before reading any of its failures as real.

## Found live this cycle

<!-- The run files here. Nothing yet: v22 opened at close-out, before the next arm. -->
