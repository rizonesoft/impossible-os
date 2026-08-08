# Overnight Runner Improvements v12 -- Findings (opened 2026-08-08)

Runner-behavior findings from the run armed after the 2026-08-08 close-out of [v11](overnight-runner-improvements-v11.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v13.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v12.md`](../token-saver/token-saver-v12.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism.

**Why findings land here instead of being fixed.** The control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`) and the receipt surface are off-limits to the unattended run. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment.

**Quote the evidence a gate hands you.** v11's only NOT-REPRODUCED item failed on this: `build_offload_reminder.py` prints the SEGMENT it matched precisely so the claim can be checked, and the filing described the shape instead of quoting it. Eleven candidate shapes across two cycles have now been tested against that hook with zero false blocks. A gate that tells you what it matched -- quote it verbatim, or the finding cannot be actioned.

---

## Reasoning and autonomy findings -- file these too

Opened 2026-08-08. Until now this surface took MACHINERY findings ("a gate misfired", "a wedge") and reasoning lessons arrived only as preamble prose, if at all. They are first-class findings: file them as `- [ ]` items like any other. A separate capture file was considered and rejected -- the machinery/judgment line is too fuzzy to sort on (the v11 rebase defect was simultaneously a doctrine gap and a composition mistake), and a third surface would have to be threaded through `arm-sequencer.sh`, the doctrine card and the close-out ritual to gain nothing the heading below does not.

The nine shapes worth filing. Each earns its place from a real incident, not from principle:

- **"I should have thought about this differently."** The plain reasoning finding. A conclusion you reached and later found unsupported, an approach you committed to too early, an ambiguity you resolved silently that deserved a question. File the reasoning, not just the outcome -- the outcome is usually already visible in the diff.
- **Separate what you OBSERVED from what you INFERRED.** State the symptom and the mechanism as two claims, and mark which one you actually tested. v11's `build_offload_reminder` item did not: it reported a real-sounding block alongside a mechanism that turned out to be wrong, and the mechanism is what got investigated -- twice, across two cycles, eleven shapes. An observation with an explicitly UNVERIFIED mechanism is more useful than a confident wrong one, and cheaper to triage.
- **Verify the PROBE before trusting the result.** A measurement that cannot fail is not evidence. Three wrong measurements were made in the 2026-08-08 attended session alone: `suite | tail -6; echo rc=$?` read `tail`'s status and reported a failing suite as green; a hook fixture omitted a sibling module so the fallback path was measured instead of the hook; another ran a hook whose preconditions (`OVERNIGHT_SEQUENCER_RUN`, active SECTIONS state) were not met, so it was inert. Always include a CONTROL that must fire, and say in the filing that it did.
- **File the WORKAROUND, not just the block.** When you route around a gate, the route is the finding: it marks where the system fights work it should permit. v11's `bare_section_refs` item was filed this way -- the glyph was composed as `chr(0xA7)` to produce byte-identical output -- and that detail is what proved the gate was costing keystrokes without adding safety on that path. A workaround the gate cannot distinguish from compliance is a gate that is not doing its job.
- **File the shortcut you ALMOST took.** The near-miss carries more information than the action. On 2026-08-08 a red tooling suite made `SKIP_TOOLING_SUITE=1` the obvious next move; taking it would have pushed an untested tooling change on a suite that was red only because of environment inheritance. Record the tempting-but-wrong move and what made it tempting -- that is where a gate needs a better message, not a stronger block.
- **Name the counterfactual gate: what would have caught this earlier?** Turns a bug report into a gate proposal. If the answer is "nothing could have", say that explicitly -- it is the strongest possible argument for building something, and it stops the same class recurring silently.
- **When two explanations fit, say so, and name the test that would separate them.** Do not pick the likelier one and file it as fact. A filing that ends "either X or Y; running Z distinguishes them" is directly actionable; a filing that guesses costs a cycle when the guess is wrong.
- **Record the RECOVERY cost, in tool calls.** Not just that something failed, but what getting back on track cost. "Three refused commits plus about six run-side tool calls" is prioritisable; "this was annoying" is not. It is also the only way a papercut that fires every section ever outranks a dramatic one-off.
- **Report what you did NOT do, and why.** A deliberately skipped check, a section left unshipped, a fix judged too risky to make unattended. Silence reads as "not encountered", which is how a known gap becomes invisible -- the same black-hole shape this repo forbids for stamped sections.

Two standing cautions carried from v11, both still live:

- **A finding you cannot explain is still worth filing.** Two v10 items were reported BACKWARDS, as false positives blocking legitimate work; the false positives did not reproduce, and probing them found real BYPASSES in the opposite direction -- one of which ran the full test suite in the main context with nothing reporting it. Do not withhold an observation because the story does not close.
- **Prose is not a gate.** When filing "the run did X, which is forbidden", check whether anything actually FORBIDS it. The rule against the run editing its own control plane was stated in the skill, the doctrine file and every capture-file header, and no code enforced it -- the run edited `scripts/overnight/decision-registry.py` for ~90 lines with rc 0 from every guard.

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
