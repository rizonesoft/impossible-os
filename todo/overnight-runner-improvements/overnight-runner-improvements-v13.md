# Overnight Runner Improvements v13 -- Findings (opened 2026-08-08)

Runner-behavior findings from the run armed after the 2026-08-08 close-out of [v12](overnight-runner-improvements-v12.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v14.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v13.md`](../token-saver/token-saver-v13.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism. v12's close-out settled that this is the COMMON case, not the exception: every cost number that run produced arrived attached to a gate and was filed here correctly.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Lead <= 250 chars; sub-bullet bodies <= 1,000, one idea per sub-bullet.

**Quote the evidence a gate hands you.** `build_offload_reminder.py` prints the SEGMENT it matched precisely so the claim can be checked. v12's three filings against it all quoted the segment, and that is why all three were diagnosable in one pass -- the shared mechanism (the exemption anchor never learned about `(`) was visible only because the quoted text showed the wrapper INSIDE the matched segment.

---

## Reasoning and autonomy findings -- file these too

Carried from v12 and still first-class: file reasoning lessons as `- [ ]` items like any other finding. The nine shapes worth filing (each earned from a real incident) are listed in [v12](overnight-runner-improvements-v12.md) and are not repeated here; three of them were paid for again during the v12 close-out and are restated because they keep recurring:

- **Verify the PROBE before trusting the result.** A measurement that cannot fail is not evidence. Include a CONTROL that MUST fire, and say in the filing that it did. This is now written into `implement-unit-tests/SKILL.md` for fixtures, with the four ways a fixture can be inert; the same applies to the throwaway measurements taken while debugging one.
- **Separate what you OBSERVED from what you INFERRED.** v12's `bare_section_refs` item named a wrong mechanism ("the hook has no such case") beside a real symptom. Investigating the SYMPTOM found a bigger defect than the one filed; investigating the stated mechanism alone would have closed it as not-reproduced. Mark the untested half.
- **A destructive probe runs on a copy.** `git clone --local` into /tmp costs seconds. v12 recorded a hypothesis-test that rewrote HEAD, the index and the reflog of the live checkout; no gate can catch this class, so it is doctrine or nothing.

---

## Carried forward from v12

- [ ] `build_offload_reminder.py` blocks a heredoc that WRITES a wrapped suite command, when the heredoc body contains a `#` comment
      The residual shape from v12's three-shape filing; the two subshell shapes are fixed and pinned. Mechanism CONFIRMED at source during the close-out, not inferred.
      - `_codex_dispatch._has_unmodelled_grammar` runs against the RAW command, before `_strip_heredoc_bodies`, so a `#` anywhere -- including inside a heredoc body that is pure data -- declares the command unsplittable. `_blocking_match` then fails closed on the whole command and finds the wrapped suite inside the heredoc text.
      - Reproduced both ways: the same heredoc WITHOUT a comment line is not blocked. Cost when it fires is low (author the file with the Write tool instead, 1 extra call), which is why it was not repaired under time.
      - It is a DESIGN decision, not a repair: fixing it means either relaxing a deliberately fail-closed rule (`_UNMODELLED` exists because a wrong split is worse than no split) or resolving a circularity -- to know which `#` is inside a body you must strip bodies first, and the stripper is itself confused by a `#`-commented `<<WORD`.
- [ ] Nothing checks that a new fixture FAILS against unmodified code
      v12's counterfactual gate, still unbuilt. Four inert fixtures shipped green in one section review; three of the four would have been caught by such a check.
      - Two candidate shapes: an `--assert-fails-without` harness convention, or a lint requiring every `t_pass` claiming a mutation to name the production needle it patched.
      - Blocked on a design decision about where fixture registration lives, which is why the v12 close-out shipped the authoring rule (`implement-unit-tests/SKILL.md`) instead and left this open.
- [ ] The control-plane suite's unnamed intermittent has a NAME: two tests fail whenever `build/todo-cache.json` is absent
      v12's standing obligation asked for the name of the test behind `68 passed, 1 failed` in five runs. Caught during the close-out, with the mechanism: `test_reachability_gate.py` and `test_stub_lint_coverage.py` both fail when the derived todo-graph cache is missing, and pass the moment it is rebuilt.
      - Two routine actions remove it, neither of them wrong: `bash scripts/todo-graph/build-and-validate.sh` WITHOUT `--keep-cache` deletes the cache it just validated (documented, easy to forget), and a clean build removes `build/` wholesale.
      - Observed here as `67 passed, 2 failed` at pre-commit, minutes after the same suite returned 69/69 -- the only thing between the two runs was a lint rebuild without `--keep-cache` and a clean build from the smoke matrix.
      - `test_reachability_gate` fails in a way that reads like a REGRESSION rather than a missing input: it expects rc 2 (refusal) for an out-of-tree file and gets rc 1 with a plausible-looking finding, so the failure text names a fixture, not the cache.
      - The repair is a decision, not a patch: either the two tests declare the cache a precondition and SKIP (or build it) when absent, or the suite builds it once up front. Failing on an absent derived artifact makes the suite report a regression that nobody introduced.
- [ ] Attended repair cannot commit while the run edits ANY `todo/` file -- needs an operator decision, carried since v11
      `lint.sh` Check 7, Check 17 and the `todo-reachability.py` audit scan the whole `todo/` tree at pre-commit rather than the staged set.
      - So an attended commit that deliberately EXCLUDES the run's file is still blocked by it, and re-blocked on every cache rebuild because the run keeps editing.
      - Cost three refused commits and about six run-side tool calls on 2026-08-07. Original filing: [v11](overnight-runner-improvements-v11.md).
      - The decision: scope those gates to the STAGED set (keeping the repo-wide scan for a bare `lint.sh` and for CI), or signal that an attended repair is in flight.
- [ ] `run-all.sh` and `lint.sh` were never audited for opt-out env inheritance
      `test-tooling.sh` was proven to inherit it and is fixed; these two read the same ambient family. Neither is proven to leak, neither is proven not to.
      - Settle it the way it was settled for the tooling suite: run each under a deliberately poisoned environment and compare against a clean baseline.

## What shipped in the 2026-08-08 close-out, and is therefore under test

New machinery, all of it control plane. If something in this list misbehaves, that is a REGRESSION and the highest-value thing this run can report.

- **`build_offload_reminder.py` exemption anchor steps over `(` and `{`.** The sanctioned wrapper is now exempt inside a subshell or brace group, which is the rc-capture idiom the ship sequence prescribes. Watch for: a BARE suite run escaping inside a group (the bypass controls are pinned, but this is the dangerous direction).
- **`build_offload_reminder.py` treats `{` as a transparent prefix.** Found by writing the control for the fix above: `{ bash scripts/test-tooling.sh; }` previously matched NOTHING and hid a bare suite run from the gate entirely. Watch for: a false block on a legitimate brace group.
- **`_content_lint._norm` derives the repo root instead of hardcoding it.** Every path exemption (todo-graph, ntfs spec-code, test-ai-system, stb/src-libs for citations) was inert for absolute paths and now applies. Watch for: an edit that SHOULD be blocked landing silently on one of those paths.
- **`scripts/test-tooling.sh` holds a per-worktree `flock`.** It waits, then refuses after `TT_LOCK_TIMEOUT` (900s default); `TT_NO_LOCK=1` overrides. Watch for: a spurious refusal from a stale lock holder, or a pre-push gate waiting behind a manual run and blowing the tool wall.
- **`run-artifact.sh` writes an in-flight record.** `.claude/state/last-artifact.json` is `state: running, exit: null` while the command runs and `state: complete` after. Watch for: a consumer that reads `exit` without checking `state` and now sees null.
- **`section-pack.py` search scope follows the section** (top-level dir of each manifest `likely_file`, reported as `search_paths`), and ranks Python/shell definitions. Watch for: a slower pack on a section whose likely files span many top-level dirs, or a wrong `def` picked from a newly-searched tree.
- **`scripts/lint.sh` Check 27 (oscomp-cell-cap) actually runs now.** It shipped INERT in `34af61ea6`: `... ) || true"` put the `|| true` inside the captured string, so the count parsed as `0 || true`, the `-gt` test errored with "integer expression expected" (printed on every lint run) and the branch was therefore always false. Verified both ways -- with the bug restored, a deliberately 120-column cell produces no error; with the fix, it names the file, line and width. Watch for: a false positive on a legitimately wide claim cell now that the check can speak.
- **Doctrine:** the harness exit code is not the verdict for a backgrounded launch (sequencer ship sequence); from round 4 the re-adversarial prompt points back at production behaviour (`review-todo-section` step 13.5); four named fixture-inertness checks (`implement-unit-tests`).

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Control-plane suite flake.** `scripts/overnight/tests/run-all.sh` returned `68 passed, 1 failed` once in five consecutive runs on 2026-08-08, then 69/69 four times running; the failing test's NAME was not captured. If it recurs, capture the name before re-running. Baseline: 69 tests, 1 failure in 5 runs.
- **Ship-push wall-clock**, now that the push is backgrounded. Baseline 2026-08-08: a `scripts/todo-graph/` push took over 10 minutes inside pre-push and was killed at the wall; the same content pushed cleanly once backgrounded. Record how long the poll actually waits, per push.
- **Tooling-suite cost, and now its LOCK.** Baseline: 1293 tests, ~6 minutes, fired at pre-push on four path prefixes. New question: how often does the lock actually contend, and how long does a waiter wait? A contended pre-push now serialises behind a manual run instead of poisoning it -- correct, but it is also new wall-clock nobody has measured.
- **Does the fixpoint rebuild ever return non-zero in practice?** It is fail-closed. Baseline: unknown -- rc 3 (corpus moved mid-read) has never been observed live. A fixpoint refused on it is the first real sighting and worth its own entry.
- **Does `section-pack.py` now resolve symbols for non-kernel sections?** Baseline 2026-08-08 on TODO-06 section 23: 0 of 24 symbols defined before the scope fix, 16 of 24 after; a kernel section is unchanged at 11 defined. If a tooling section still reports mostly-null defs, the manifest's `likely_files` is the next suspect, not the resolver.
