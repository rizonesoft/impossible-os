# Overnight Runner Improvements v14 -- Findings (opened 2026-08-10)

Runner-behavior findings from the run armed after the 2026-08-10 close-out of [v13](overnight-runner-improvements-v13.md). CAPTURE surface, not a work queue: it sits outside the sequencer's traversal, so nothing here is implemented by the run. It is the CURRENT capture file -- the sequencer files to the NEWEST `overnight-runner-improvements-vNN.md` in this directory, which is this one until an operator opens v15.

**Scope:** flow, gates, wedges, and machinery correctness. Cost findings go to [`token-saver-v14.md`](../token-saver/token-saver-v14.md) -- but a MISFIRING GATE is both, and belongs here with its mechanism. Three cycles have now confirmed that is the common case, not the exception.

**Why findings land here instead of being fixed.** The run may not edit its own control plane (`.claude/hooks/**`, `.claude/skills/**`, `scripts/overnight/**`, `.githooks/**`, `.claude/settings.json`) or the receipt surface. Record the finding in the same turn it is observed, then advance; a finding carried in-context to "report later" dies with the segment.

**What to write.** What was observed live (run id, segment, the exact refusal text or behavior), the mechanism confirmed at source with file:line, and what it cost or would cost. Lead <= 250 chars; sub-bullet bodies <= 1,000, one idea per sub-bullet.

**Separate what you OBSERVED from what you INFERRED, and say which you tested.** v13 filed 38 items and the two most useful ones did this explicitly; the two hardest to action did not. An observation with an admittedly-untested mechanism is worth more than a confident wrong one and costs less to triage.

---

## Carried forward from v13 -- 29 items, and why that number is a problem

v13 closed 38 filed / 6 resolved / 3 rejected / 29 carried. **More carried than resolved-plus-rejected is the shape of a backlog turning into an archive**, and it is stated here rather than buried so the next close-out starts from an honest position.

The carried items are NOT re-listed individually -- they live in [v13](overnight-runner-improvements-v13.md) with a verdict and a "what would settle it" on each, and re-copying them here would fork the record. They cluster as:

- **Eleven reasoning/doctrine items** wanting ONE grouped edit to the code-quality and unit-test skills, not eleven separate ones. The strongest are: a measurement whose coverage does not match the checker's is not evidence; verification-before-completion applies to NUMBERS, not just pass/fail; a control that passes both ways is a pin, not a guard.
- **The convergence-gate scope decision** (`KIND_SCOPES` maps every review kind to `src`, so a tooling section converges permanently after one verdict). Measured cost: five valid findings that only appeared because the run dispatched AGAINST the gate's advice. Needs a decision between following the reviewed diff and failing open on unknown surfaces.
- **The commit-message backtick cluster** (three items). A commit message is unreviewed input to a shell, the repo lints source but never messages, and it has already created a stray file in the repo root. Needs a decision about adding an always-on commit-msg gate.
- **Two review-binding questions** -- a newly untracked file cannot bind to a review, and dispatching legs before invoking the skill costs a whole wave. Both turn on whether the binding attaches to the dispatch or the skill invocation; settle them together.
- **Four items with an owner elsewhere**: the fourth XREF grammar and the bare `json.loads` consumer both belong to consolidations already open in the metadata-layer TODO.

## What shipped in the 2026-08-10 close-out, and is therefore under test

- **`_review_pipeline_passthrough` sees through grouping.** `is_review_pipeline_passthrough` steps over leading `(` / `{` before applying its first-token allowlist, so the sequencer's own prescribed ship shape -- `( git push ... ; echo "rc=$?" ) &` -- is no longer refused by the gate its own doctrine has to satisfy. Closed five v13 items with one fix. Watch for: any shell form that reaches a program the allowlist would refuse.
- **Loop headers are deliberately still blocked**, and that is the security line: a `for i in $(...)` header runs a substitution before the body, so stepping into the body would allowlist a call on the strength of a program that is not the one that runs first. Pinned as a control. Watch for: pressure to widen it -- the sanctioned long wait already has allowed spellings.
- **`section_review_required` is scoped to whoever shipped.** It exempts a session when a run is ACTIVE and this session is not it, using the same `OVERNIGHT_SEQUENCER_RUN` discriminator `build_offload_reminder` was given for the identical class in July. Watch for: a real section ship going unreviewed -- the run itself is never exempt, and with no run active nothing changed.

**NOT VERIFIED, and the next session should close this:** the batched adversarial review over this close-out's diff was dispatched and hung on the known long-prompt failure, then not retried for context. The diff widens a gate's allowlist, which is exactly the class that most wants a second pair of eyes. Re-dispatch it TIGHT (short prompt) against `_review_pipeline_passthrough.py` and `section_review_required.py` before trusting them under an unattended run.

## Standing measurement obligations

Carry the baselines forward. A measurement without one is an anecdote.

- **Does the section-hygiene machinery hold?** Baseline 2026-08-10: the metadata-layer TODO went 9 -> 39 sections in eight days, then the gates landed. Branching over the last 24h of that run was 6 created / 7 shipped = **0.86, under 1 for the first time**. Watch: does it stay under 1, do continuation waivers ever get demanded (**0 ever, to date**), and does `user_impact` start appearing as "Nothing today" on sections that then do not get built?
- **Control-plane suite flake.** Two tests fail whenever the corpus moves under them. Baseline: `test_reachability_gate` and `test_stub_lint_coverage`, both passing on a rebuilt cache within seconds of failing. If a THIRD test joins them, the design decision (snapshot the corpus) stops being optional.
- **Ship-push wall-clock with the receipt.** Baseline: 44s on a valid receipt, 95-99s when stale, against a 10-minute wall kill before it existed.
- **Does the fixpoint rebuild ever return non-zero in practice?** Still unknown; rc 3 has never been observed live.
