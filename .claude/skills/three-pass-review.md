# Review Passes -- Plan, Round, Performance, Completion

How a section is reviewed under the reviewer pin, [`.conclave/reviewers.toml`](../../.conclave/reviewers.toml). Design: [docs/specs/2026-11-11-three-pass-review-design.md](../../docs/specs/2026-11-11-three-pass-review-design.md). The pin is the only place the schedule, the lenses and the cap live; this file says how to run it.

## The passes

| Pass | When | Asks | Dispatched at |
| ---- | ---- | ---- | ------------- |
| `plan` | before any code | is the plan sound, complete and buildable | the commit the section starts from |
| `round` | after the build and its gates pass | every lens in the pin, each with its own verdict | the built candidate |
| `performance` | with the round | the optimization lens alone, on the hot paths the body names | the built candidate |
| `completion` | after the findings of both are fixed and the gates rerun | did the section deliver its plan, and does the record match | the final candidate |
| `reconciliation` | only after a completion that returned gaps | are the named gaps closed, with nothing reopened | the repaired candidate |

A clean section costs four passes. Performance is its own pass because a round carrying an optimization lens found one of four known performance defects when replayed (the spec's Replay section). The round and the performance pass are dispatched again after their findings are fixed, until each comes back below a blocking level. Every returned pass counts toward `pass_cap` (45); `no_progress_cap` (3) passes in a row with nothing new end the loop. Bump the counter after each returned pass: `python3 .claude/hooks/review_round_guard.py --bump '<todo-path>#<section>' --progress <new|none>`.

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

Findings are tagged `P1`, `P2` or `P3` and the review ends with `HIGHEST:`. Only the levels in the pin's `blocking` list hold the stamp. A finding below them is filed as a tracked item and spends no further pass.

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
The completion pass at <completion-sha> returned these gaps: <the gaps, one a line>. The repair is <completion-sha>..HEAD. For each gap say whether it is closed, and report anything the repair reopened, each with file:line. End with RECONCILIATION: complete or RECONCILIATION: gaps.
```

## The stamp

The commit that adds a section's `**Verified:**` stamp is accepted when, for that section:

- a `plan` was dispatched on this history;
- the `round`, the `performance` pass and the last pass (the newer of `completion` and `reconciliation`) were each dispatched at HEAD, or at an ancestor with nothing changed since except the TODO record and generated files, and each ended below a blocking level;
- nothing a pass reads is staged with the stamp, and the commit stamps one section.

A fix made after a pass means that pass has not read the candidate: commit the fix, then dispatch the round and the performance pass again, and a reconciliation.

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
