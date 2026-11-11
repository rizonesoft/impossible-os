# Three-Pass Review: Plan, Round, Completion

**Goal:** every section gets three GPT-6 Astra passes read from one settings file, instead of five to seven separate dispatches whose kinds and cap are spread across hooks and skills. Decided with the operator on 2026-11-11. The shape and the file format come from RasterFold's `.conclave/reviewers.toml`.

## What changes for a section

| Pass | When | Replaces | Asks |
|---|---|---|---|
| `plan` | before code | `design` | is the plan sound, complete and buildable |
| `round` | after the build and its gates pass | `test-coverage`, `adversarial`, `consistency`, `perf` and the post-ship repeat of the last three | every lens in `[[schedule.rounds]]`, each with its own verdict |
| `completion` | on the final candidate, after the round's findings are fixed and the gates rerun | nothing: this pass is new | did the section deliver its plan, and does the TODO record match what shipped |
| `reconciliation` | only after a completion that returns gaps | `re-adversarial` | are the named gaps closed, with nothing reopened |

A clean section costs three passes. A completion with gaps is repaired and reconciled until it passes or the section has spent `pass_cap` returned passes.

## The settings file

`.conclave/reviewers.toml` is the only place the schedule lives. `scripts/review_pin.py` loads and validates it, strictly: unknown keys, unknown lenses, more than one round, or a route with no reviewer are refused.

- `pass_cap = 45`: every returned pass counts (plan, round, completion, reconciliations). The old 42 counted adversarial re-dispatches only; 45 is that 42 plus the three fixed passes.
- `no_progress_cap = 3`: three passes in a row with no new finding end the loop, as today.
- `model` is checked against `~/.codex/config.toml` and never passed as a flag. There is no `effort` key: effort stays the operator's dial in the Codex config.
- A missing or invalid pin refuses the dispatch. It never falls back to the old shape.

## A decision this reverses, and how it is guarded

Until about 2026-04-24 this repository used one combined prompt. It was split because Codex deep-dived whichever angle had the most text and skimmed the rest (`review-todo-section/SKILL.md`, "Rationale for separate dispatches"). The combined round returns with two guards against that failure:

1. **One verdict per lens.** The dispatcher appends each scheduled lens with its question and requires a `LENS <name>: clean | findings` line for each. The recorder refuses a round that is missing a lens line, so a skimmed lens is an incomplete pass, not a clean one.
2. **Proof before the switch.** The combined round is replayed on three shipped sections whose separate-leg findings are on record. It ships only if it reports every high and critical finding the separate legs reported. If it misses one, the round is split into two (defects, then consistency and optimization) and replayed again. The result is recorded in this file.

## Where it is wired

| Place | Today | Change |
|---|---|---|
| `scripts/codex-dispatch.sh` and the broker wrapper | accept any kind | read the pin, refuse a kind the schedule does not name, append the lenses |
| `.claude/hooks/codex_review_completed.py:617`, `:861` | two hardcoded lists of 8 kinds; an unknown kind is dropped silently | take the kind list from the pin; an unknown kind is a loud refusal |
| `.claude/hooks/section_commit_gate.py:148` | a stamp commit needs `adversarial`, `consistency`, `perf` | needs `round` and `completion` bound to the candidate; `plan` is bound to the section, not to HEAD, because it precedes the code |
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
