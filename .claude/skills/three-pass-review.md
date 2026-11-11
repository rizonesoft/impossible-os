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
- A pass is dispatched through `scripts/codex-dispatch.sh` only. The broker refuses a pass kind, and the recorder credits a pass only against the wrapper's own receipt.
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

## Sections that started under the older kinds

A section whose stamp entry holds only `adversarial`, `consistency` and `perf` finishes on those rules. One pass stamp moves a section onto the three-pass rules; the two shapes are not mixed.
