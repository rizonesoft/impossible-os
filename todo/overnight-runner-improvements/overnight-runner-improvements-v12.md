# Overnight Runner Improvements v12 -- Findings (opened 2026-08-08)

Runner-behavior findings from the run armed after the 2026-08-08 close-out of [v11](overnight-runner-improvements-v11.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v13.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v12.md`](../token-saver/token-saver-v12.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`) and the receipt surface are off-limits to the unattended run. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment.

**Quote the evidence a gate hands you.** v11's only NOT-REPRODUCED item failed on this: `build_offload_reminder.py` prints the SEGMENT it matched precisely so the claim can be checked, and the filing described the shape instead of quoting it. Eleven candidate shapes across two cycles have now been tested against that hook with zero false blocks. A gate that tells you what it matched -- quote it verbatim, or the finding cannot be actioned.

---

## What shipped in the 2026-08-08 stop, and is therefore under test

New machinery, all of it control plane. If something in this list misbehaves, that is a REGRESSION and the highest-value thing this run can report.

- **Ship sequence reshaped** (`overnight-sequencer/SKILL.md`, `b1583c3f6`). Never pipe the rebase (a pipeline's status is its LAST command, so `| tail -N && git push` gated the push on `tail` and let a failed rebase through). And BACKGROUND the ship push, polling it like the J1 chain: `.githooks/pre-push` runs `test-tooling.sh` (~6 min) on any `scripts/lint/`, `scripts/todo-graph/`, `scripts/test-tooling.sh` or `.claude/hooks/` change, which exceeds the 10-minute tool wall. Watch for: a ship push that still races the wall, or a poll loop that reports success on an unfinished push.
- **`section_review_required.py` no longer reads re-padding as a status flip** (`b1583c3f6`). It compares the row normalized for whitespace against the minus side. Watch for: a REAL `[ ]`->`[x]` ship that fails to gate (the dangerous direction).
- **`test-tooling.sh` clears inherited opt-out env** (`3d92831ad`) -- `SKIP_*`, `ATTENDED_REPAIR_OVERRIDE`, `CODEX_FLAG_OVERRIDE` -- and prints what it cleared. Watch for: a test that NEEDED an ambient var and now fails.
- **`run_phase_guard.py fixpoint` fails closed on the rebuild** (this close-out). It refuses fixpoint unless `build-and-validate.sh` returns exactly 0, so a stale cache can no longer certify DONE. Watch for: a fixpoint refused on a transient rc that should have been retried rather than refused.
- **`bare_section_refs.py` is scoped to the repo** (this close-out). Paths outside the repo root exit 0; `..` traversal still cannot smuggle a tracked file out of scope. Watch for: an in-repo file that stops being judged.

## Carried forward from v11

- **Attended repair cannot commit while the run edits ANY `todo/` file.** `lint.sh` Check 7, Check 17 and the `todo-reachability.py` audit scan the whole `todo/` tree at pre-commit rather than the staged set, so an attended commit that deliberately EXCLUDES the run's file is still blocked by it -- and re-blocked on every cache rebuild, because the run keeps editing. Cost three refused commits and about six run-side tool calls on 2026-08-07. Needs an operator decision: scope those gates to the STAGED set (keeping the repo-wide scan for a bare `lint.sh` and for CI), or signal that an attended repair is in flight. [v11 item](overnight-runner-improvements-v11.md)
- **`run-all.sh` and `lint.sh` were never audited for opt-out env inheritance.** `test-tooling.sh` was proven to inherit it and is now fixed; these two read the same ambient family. Neither is proven to leak, neither is proven not to. Settle it the same way it was settled for the tooling suite: run each under a deliberately poisoned environment and compare against a clean baseline.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Control-plane suite flake.** `scripts/overnight/tests/run-all.sh` returned `68 passed, 1 failed` once in five consecutive runs on 2026-08-08, then 69/69 four times running; the failing test's NAME was not captured. If it recurs, capture the name before re-running -- an unnamed intermittent cannot be fixed. Baseline: 69 tests, 1 failure in 5 runs.
- **Ship-push wall-clock**, now that the push is backgrounded. Baseline 2026-08-08: a `scripts/todo-graph/` push took over 10 minutes inside pre-push and was killed at the wall; the same content pushed cleanly once backgrounded. Record how long the poll actually waits, per push.
- **Tooling-suite cost.** Baseline: `test-tooling.sh` is 1293 tests and its own comment prices it at ~6 minutes; it fires at pre-push on four path prefixes. If most ship pushes touch those prefixes, the ~6 minutes is a per-section tax worth reporting to [`token-saver-v12.md`](../token-saver/token-saver-v12.md).
- **Does the fixpoint rebuild ever return non-zero in practice?** It is now fail-closed. Baseline: unknown -- rc 3 (corpus moved mid-read) has never been observed live. If a fixpoint is refused on it, that is the first real sighting and worth its own entry.
