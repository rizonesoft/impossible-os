# Review Passes -- Plan, Round, Performance, Completion

How a section is reviewed under the reviewer pin, [`.conclave/reviewers.toml`](../../.conclave/reviewers.toml). Design: [docs/specs/2026-11-11-three-pass-review-design.md](../../docs/specs/2026-11-11-three-pass-review-design.md). The pin is the only place the schedule, the lenses and the cap live; this file says how to run it.

## The passes

| Pass | When | Asks | Dispatched at |
| ---- | ---- | ---- | ------------- |
| `plan` | before any code | is the plan sound, complete and buildable | the commit the section starts from |
| `round` | after the build and its gates pass | every lens in the pin, each with its own verdict | the built candidate |
| `performance` | with the round | the optimization lens alone, on the hot paths the body names | the built candidate |
| `completion` | after the findings of both are fixed and the gates rerun | did the section deliver its plan, and does the record match | the final candidate |
| `reconciliation` | only after a completion that returned gaps | are the named gaps closed, and how does the newest repair itself fail or break a caller | the repaired candidate |

A clean section costs four passes. Performance is its own pass because a round carrying an optimization lens found one of four known performance defects when replayed (the spec's Replay section). The round and the performance pass are dispatched again after their findings are fixed, until each comes back below a blocking level. Every returned pass counts toward `pass_cap` (55 since 2026-11-21), the only cap on a loop: since 2026-11-20 (owner decision) the pin names no `no_progress_cap`, so a chain ends when its passes come back below a blocking level or at `pass_cap`, never because its worst finding stopped falling (the 2026-11-19 stall rule ended desktop, shell, apps, user and site repair chains after three equal-severity passes). At `pass_cap` the stamp no longer needs a clean round, but the performance pass and the last pass must still read the final candidate and end below a blocking level; file what the last round found as a follow-up item. Bump the counter after each returned pass: `python3 .claude/hooks/review_round_guard.py --bump '<todo-path>#<section>' --highest <the reply's HIGHEST> --progress <new|none> --base <REVIEW_BASE>`.

## Dispatch

One pass, one call, the dispatch alone in the call:

```bash
REVIEW_BASE=<sha the section started from> bash scripts/codex-dispatch.sh '[review-kind: round] <todo-path> section <N>
<body>'
```

- The marker is written once, as the first thing on the first line. Any other spelling is refused.
- `REVIEW_BASE` decides the lenses that depend on the changed files (`concurrency` and `bare-metal` for kernel and boot paths, `design` for drawn UI). Without it the base is `HEAD~1`, which is wrong for a section of more than one commit.
- The wrapper appends the lenses and the verdict line each one owes. Do not write them into the body.
- In the background, send the same prompt through `scripts/overnight/review-broker-codex-dispatch.sh` (with `REVIEW_BASE` set the same way): its detached job runs `scripts/codex-dispatch.sh`, so the pass is checked and receipted exactly as a foreground one, and its log ends `Turn completed (rc=N)` with the wrapper's exit status.
- The first line names the TODO and the section (`todo/<path>.md section <N>`). The commit gate finds a pass by them.
- The commit gate judges a pass from the receipt the wrapper appends to `.claude/overnight/reviews/manifest.jsonl` once the pass has been checked: the candidate it identified before the reviewer ran, whether every lens gave a verdict, and the review's `HIGHEST:` level. A direct companion call writes no receipt and counts for nothing.
- Each pass is a fresh Codex session. Never resume the previous pass.

## Reading the result

| Exit | Meaning | Next |
| ---- | ------- | ---- |
| 0 | every lens gave a verdict | receive the findings (`superpowers:receiving-code-review`), fix what is real |
| 3 | a lens gave no verdict line | the pass is incomplete and earns no credit; dispatch it again |
| 2 | refused before dispatch (the pin, the marker, the base, the reviews directory) | fix what the message names |

Findings are tagged `P1`, `P2` or `P3` and the review ends with `HIGHEST:`. Only the levels in the pin's `blocking` list hold the stamp; since 2026-11-21 (operator) that is all three, so every finding is fixed, the smallest included. A finding below the list, if a later pin narrows it, is filed as a tracked item and spends no further pass. The levels are graded by consequence in normal use, and every pass's prompt carries their meanings (`SEVERITY_TEXTS` and `SEVERITY_RULE` in `scripts/review_pin.py`, since 2026-11-21): a P1 or P2 names its reachable trigger and consequence (legal CPU and interrupt orderings, recovery paths and hostile input at a trust boundary count however rare), and a finding with no reachable trigger is P3. Grading stays the reviewer's: never lower a level in triage. When a finding is out of scope by fact (the guest is restored for every run, two runs at once are not supported), state that fact in the next prompt so the reviewer grades with it.

## Bodies

Keep a body short: scope, the candidate, and the specific risks of this section. Do not summarise what you built and do not suggest a verdict.

**plan**

```
[review-kind: plan] <todo-path> section <N>
Review the plan for this section before any code is written: the section text in the TODO and the design notes below. Report what makes it unsound, incomplete or unbuildable, each with the line of the TODO it concerns.
<design notes: the approach, the files to touch, the invariants, the tests>
```

**round**

```
[review-kind: round] <todo-path> section <N>
Review this section as implemented by the commits <base>..HEAD (git diff <base> HEAD). Read the section text, the code and tests the diff changes, and the callers it affects. Report defects only, each with file:line, most severe first.
<the two or three risks specific to this section>
```

**performance**

```
[review-kind: performance] <todo-path> section <N>
Performance review of this section as implemented by <base>..HEAD.
Hot path: <the call chain from each entry point to its leaf, and what bounds it: a tick, a hook deadline, a boot budget>
Report defects only, each with file:line, most severe first.
```

**completion**

```
[review-kind: completion] <todo-path> section <N>
The candidate is <base>..HEAD. Compare it with the section text and its checklist: name every promised item that is missing, stubbed or narrower than written, and every place the TODO record (items, counts, stamps, cross-references) disagrees with what the candidate contains. End with COMPLETION: complete or COMPLETION: gaps.
```

**reconciliation**

```
[review-kind: reconciliation] <todo-path> section <N>
The completion pass at <completion-sha> returned these gaps: <the gaps, one a line>. For each gap say whether it is closed, and report anything the repair reopened, each with file:line. End with RECONCILIATION: complete or RECONCILIATION: gaps.
```

The wrapper names the newest repair itself: it runs from the last reviewed candidate (the head of the section's newest checked completion or reconciliation pass in the reviews manifest) to HEAD with the working tree, and a reconciliation with no such pass on record is refused. Every reconciliation, in every part of the tree, carries `repair-adversarial` and `repair-integration` beside `plan-delivery` and `record` (owner decision 2026-11-20): both start at that repair and then cover the whole section, whose range the wrapper names too (operator, 2026-11-21: every pass reviews the whole section). That range starts at the section base the same pass recorded, so leave `REVIEW_BASE` unset or give that commit; another is refused. Kernel and boot repairs also get the full `adversarial`, `integration`, `concurrency` and `bare-metal` lenses.

## The stamp

The commit that adds a section's `**Verified:**` stamp is accepted when, for that section:

- a `plan` was dispatched on this history;
- the `round`, the `performance` pass and the last pass (the newer of `completion` and `reconciliation`) were each dispatched at HEAD, or at an ancestor with nothing changed since except the TODO record and generated files, and each ended below a blocking level;
- nothing a pass reads is staged with the stamp, and the commit stamps one section.

A fix made after a pass means that pass has not read the candidate: commit the fix, then dispatch the round and the performance pass again, and a reconciliation.

## When the reviewer is down

`scripts/overnight/review-broker-codex-dispatch.sh` exits 75 while the reviewer's backend is down (`scripts/overnight/codex-outage-check.py`). The section is then built unreviewed as a recorded debt, and reviewed in full when the reviewer answers:

- `python3 scripts/overnight/review_owed.py add --todo <todo-path> --section <N>` before the first edit. It records the commit the section starts from.
- Build and commit. Nothing is pushed while a debt is open; the push gate refuses.
- When `python3 scripts/overnight/review_owed.py probe` exits 0: dispatch the `plan` pass late, then the wave with no `REVIEW_BASE`. `scripts/codex-dispatch.sh` takes the recorded base for every pass after the plan, tells the reviewer the range and writes it on the receipt; a base that names the late plan pass's commit, or HEAD, is refused, and the stamp gate refuses a pass that was shown less.
- After the stamp commit: `python3 scripts/overnight/review_owed.py clear --todo <todo-path> --section <N>`, then push.

The steps for an unattended run are in [`overnight-sequencer`](overnight-sequencer/SKILL.md).

## Which rules a section is on

```bash
python3 scripts/review_pin.py --section <todo-path> <N>
```

| It prints | Meaning | Do |
| --------- | ------- | -- |
| `new` | nothing has been dispatched for the section | start it with a `plan` pass |
| `passes REVIEW_BASE=<sha>` | a pass was dispatched for it | continue with the passes; `<sha>` is the commit before the section's code |
| `older` | a review of the older kinds was recorded for it and no pass | finish it on the older kinds, as the skill steps are written |

## In the skills

The steps of `implement-todo-section` and `review-todo-section` name the older kinds. For a `new` or `passes` section, dispatch the pass in the right column where a step names the kind in the left one. Everything else in those steps applies unchanged: the fix discipline, the self-diff gate before a re-dispatch, one reception per wave, the round counter, how a round closes the loop.

| The step dispatches | Dispatch instead |
| ------------------- | ---------------- |
| `design` (implement step 4) | `plan`. It is no longer optional: the stamp needs one. |
| `test-coverage` (implement step 9) | nothing. `test-coverage` is a lens of the round. |
| `adversarial` (implement step 13) | `round`, on the working tree, before the commit. It finds defects early; it does not count for the stamp, because the candidate changes when you commit. |
| `adversarial`, then `consistency` and `perf` (review steps 5 and 8) | one wave of three on the committed candidate: `round`, `performance`, `completion` |
| `re-adversarial` (review step 13.5) | the wave again, with `reconciliation` in place of `completion` once a completion has returned gaps |

Three things differ from the older kinds, because the commit gate requires each of the three passes to have read the candidate being stamped and to have ended below a blocking level:

- **Any fix after a wave means the wave again.** There is no lighter path for a test-only or cosmetic fix: a pass that ran before the fix has not read it. Batch the fixes of one wave into one commit, then dispatch the next wave.
- **Do not skip a pass on a CONVERGED verdict** from `review_convergence.py`. A pass that is skipped has no receipt on the final candidate.
- **The stamp commit carries nothing a pass reads.** Commit code, hooks, scripts and docs first; the stamp commit holds the TODO, the plan file and generated files only, and stamps one section.

## The review loop

```bash
# 1. One wave: three calls in one message, in the background through the broker.
REVIEW_BASE=<sha> bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: round] <todo-path> section <N>
<body>'
REVIEW_BASE=<sha> bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: performance] <todo-path> section <N>
<body>'
REVIEW_BASE=<sha> bash scripts/overnight/review-broker-codex-dispatch.sh '[review-kind: completion] <todo-path> section <N>
<body>'
# 2. Wait on the three logFiles the broker printed, in one long call.
bash scripts/overnight/wait-for-codex-verdict.sh --max 540 <log1> <log2> <log3>
# 3. Read the wave.
python3 scripts/overnight/review-envelope.py . --kinds round,performance,completion --todo <todo-path> --section <N>
```

- `needs_redispatch` names a pass to dispatch again as it is: it crashed, or it is under `incomplete` because its review skipped a lens or its `HIGHEST:` line.
- When nothing needs a re-dispatch, receive the wave once (`superpowers:receiving-code-review`), then bump the round counter.
- `blocking` names each pass that ended at a blocking level. Fix what is real, commit, and dispatch the whole wave again on the new candidate, with `reconciliation` in place of `completion` if the completion returned gaps. Use `--kinds round,performance,reconciliation` for that wave.
- When `blocking` is empty and nothing changed after the wave, add the stamp.
- A finding you reject with code evidence still leaves the pass at the level it declared. Name it in the next wave's body as `rejected: <finding> because <evidence>` so the reviewer does not raise it again.

## Sections that started under the older kinds

A section whose stamp entry holds only `adversarial`, `consistency` and `perf` finishes on those rules. One pass stamp moves a section onto the three-pass rules; the two shapes are not mixed.

## Review discipline (operator, 2026-11-21)

Every section, every path, permanently:

- **A fix repairs the finding and nothing more.** A fix that would need a new hook, entry point or feature files that work as an item in its owning section and fixes the finding at the narrowest point. New surface added inside a fix round is what kept TODO-08 section 48's review open for 18 rounds.
- **Every prompt carries its scope facts:** the threat model, the decided design not to be re-litigated, and what is out of scope by decision. Only recorded decisions go in it, and a P1 is always reported.
- **The candidate being stamped gets a full-scope wave:** round, performance and completion, every section. The performance pass is never skipped, and every section is pushed on its own and watched to green.

Every pass reviews the whole section, on every path and in every round; no pass is scoped to the repair (operator, 2026-11-21, which ended the trial of scoped re-dispatch on TODO-08 sections 48 to 59 before it started: a scoped pass is one fewer full look). A re-dispatched pass may name the repair as where to look first: [Re-dispatched passes name the repair](../../todo/00-infrastructure/TODO-08-automation-hardening.md#64-re-dispatched-passes-name-the-repair).
