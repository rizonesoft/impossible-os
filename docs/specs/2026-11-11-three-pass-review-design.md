# Review Passes: Plan, Round, Performance, Completion

**Goal:** every section gets four GPT-6 Astra passes read from one settings file, instead of five to seven separate dispatches whose kinds and cap are spread across hooks and skills. Decided with the operator on 2026-11-11. The shape and the file format come from RasterFold's `.conclave/reviewers.toml`.

## What changes for a section

| Pass | When | Replaces | Asks |
|---|---|---|---|
| `plan` | before code | `design` | is the plan sound, complete and buildable |
| `round` | after the build and its gates pass | `test-coverage`, `adversarial`, `consistency` and the post-ship repeat of the last two | every defect lens in `[[schedule.rounds]]`, each with its own verdict |
| `performance` | with the round | `perf` | the `optimization` lens alone, with the section's hot paths named in the prompt |
| `completion` | on the final candidate | nothing: this pass is new | did the section deliver its plan, and does the TODO record match what shipped |
| `reconciliation` | only after a completion that returned gaps | `re-adversarial` | are the named gaps closed, with nothing reopened |

A clean section costs four passes, against five to seven today. The round and the performance pass are dispatched again after their findings are fixed. A section is stamped when the round, the performance pass and the last pass (completion, or the reconciliation after it) each read the candidate being stamped and ended below a blocking level, or the section has spent `pass_cap` returned passes.

## The settings file

`.conclave/reviewers.toml` is the only place the schedule lives. `scripts/review_pin.py` loads and validates it, strictly: unknown keys, unknown lenses, more than one round, or a route with no reviewer are refused.

- `pass_cap = 45`: every returned pass counts (plan, round, performance, completion, reconciliations). The old 42 counted adversarial re-dispatches only. Raised to 55 by the operator later on 2026-11-11.
- `no_progress_cap = 3`: three passes in a row with no new finding end the loop, as today. Raised to 5 by the operator later on 2026-11-11.
- `model` is checked against `~/.codex/config.toml` and never passed as a flag. There is no `effort` key: effort stays the operator's dial in the Codex config.
- A missing or invalid pin refuses the dispatch. It never falls back to the old shape.

## Replay: why performance is its own pass

Until about 2026-04-24 this repository used one combined prompt. It was split because Codex deep-dived whichever angle had the most text and skimmed the rest (`review-todo-section/SKILL.md`, "Rationale for separate dispatches"). Before combining again, the combined round was replayed on 2026-11-11 against shipped candidates whose separate reviews had each found a high defect. Of 3,840 recorded review legs, 43 ran on a clean committed tree and found a high or critical defect, so they repeat exactly. Each candidate was checked out and reviewed by one `round` with a neutral prompt.

| Candidate | Known high defect, found by | Combined round |
|---|---|---|
| Boot entry store, section 23 (`04404fee5`) | restore aborts on Windows directory flush: adversarial, consistency | found, same lines |
| Power management, section 8 (`9d40a3ba6`), held out | retry loses the D0 obligation: consistency | found, same lines |
| Power management, section 8, held out | D0 polling times out early: adversarial | a different defect in the same lines |
| Power management, section 1 (`44e3f3731`), held out | DSDT length, missing S5: adversarial, consistency | one related finding; six findings in all, three P1 |
| Developer tooling, section 18 (`acb831b3c`) | planner deadline exceeds the Stop hook budget: perf | missed; found once the lens question was sharpened with it in view |
| UEFI hardening, section 16 (`b2df3a0e6`) | unconditional payload hashing at PMM start: perf | missed twice |
| Power management, section 8, held out | 70 ms busy-spin in interrupt context: perf | missed |
| Power management, section 1, held out | unbounded logging before EOI: perf | missed |

- **Performance defects: one of four**, and that one only after the question was tuned to it. A round carrying only consistency, quality and optimization found one of the two it was tried on. The dedicated performance review found all four originally, and found the tooling one again when rerun.
- **Other defects:** the round is productive but samples differently from the older reviews, so it is repeated until it comes back clean, as the adversarial review is today.
- **Severity labels differ.** The round rated as P2 what the older reviews rated high. With `blocking = ["P1", "P2"]` those still hold the stamp.

Decisions that follow: performance is a pass of its own and `optimization` is not a lens of the round; the round and the performance pass repeat until clean; and three guards keep a skimmed pass from counting:

1. **One verdict per lens.** The dispatcher appends each scheduled lens with its question and requires a `LENS <name>: clean | findings` line for each, read from the reviewer's final response only. A pass missing one is incomplete and earns no credit.
2. **The worst level is on the receipt.** The dispatcher records the review's `HIGHEST:` line. A pass that ended at a blocking level is not the last word, so a completion that returned gaps cannot carry a stamp.
3. **The gate judges a pass from the dispatcher's receipt alone.** Only `scripts/codex-dispatch.sh` writes one, after it has checked the pass, and it names the candidate identified before the reviewer ran. The recorder's stamp file is not used for a pass: it is written when a dispatch starts, which for the run's background broker is before there is anything to judge, and it keeps one entry per TODO across sections. The broker runs a pass through the same wrapper inside its detached job.

Limits of the replay: eight known defects on five candidates; one prompt wording changed between the first and third runs ("report every defect"); and the rerun of the dedicated performance review used a checklist adapted to tooling, one item of which points at the class of defect it then found.

## Where it is wired

| Place | Today | Change |
|---|---|---|
| `scripts/codex-dispatch.sh` and the broker wrapper | accept any kind | read the pin, refuse a kind the schedule does not name, append the lenses |
| `.claude/hooks/codex_review_completed.py:617`, `:861` | two hardcoded lists of 8 kinds; an unknown kind is dropped silently | take the kind list from the pin; an unknown kind is a loud refusal |
| `.claude/hooks/section_commit_gate.py` | a stamp commit needs `adversarial`, `consistency`, `perf` | needs `round`, `performance` and the last pass each bound to the candidate, the section and a clean receipt; `plan` is bound to the history and the section, because it precedes the code |
| `section_commit_gate.py` re-adversarial trigger and step-9 warning | separate rules | removed: the round carries test coverage, and completion plus reconciliation cover post-fix review |
| `.claude/hooks/review_round_guard.py` | counts `--bump` calls, ceiling 42 from an environment default | counts returned passes, caps from the pin |
| `.claude/hooks/skill_step_map.py`, `skill_step_observer.py`, `review_convergence.py` | bind steps and scopes to the old kinds | rebound to the four kinds |
| `scripts/overnight/review-envelope.py` | defaults to three legs | one leg per pass |
| Skills (`implement-todo-section`, `review-todo-section`, `quality-review-section`, `overnight-sequencer`, the `codex-*` set) | dispatch the old kinds | dispatch `plan`, `round`, `completion`, `reconciliation`; `codex-consistency-audit`, `codex-perf-review` and `codex-test-coverage` stay as on-demand tools outside the section pipeline |
| New prompt templates | none for completion | `docs/infrastructure/review-prompts/{plan,round,completion,reconciliation}.md` |
| `gap-audit` | file-level kind | unchanged: the loader names it as the one kind outside the section schedule |

## Sections already under review

A section whose stamps file already holds an old-kind entry finishes under the old rules: the commit gate accepts the old three-kind evidence for that section only. A section that starts after this lands uses the pin. The old path is deleted once no open section carries old-kind evidence.

## Order of work

1. The pin loader and its tests.
2. The recorder and dispatcher read the pin (the silent-drop lists go first, because they fail without an error).
3. The commit gate and round guard.
4. The step map, observer, convergence scopes and envelope.
5. The four prompt templates, then the skills and `CLAUDE.md`.
6. The replay, recorded here.
7. One GPT-6 Astra review of the whole change, then push.

## Limits

- The replay proves recall on three sections, not on every kind of section.
- RasterFold keeps its cap of 33 in its script; moving it into its pin is a separate change in that repository.
- Reversing this costs one revert while no section has started under the new shape; after that, in-flight sections need the same finish-on-own-rules handling in the other direction.
