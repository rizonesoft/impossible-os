# Codex Prompt Shape -- Canonical Review-Kind Markers

Every `node ...codex-companion.mjs adversarial-review "<prompt>"` dispatch MUST start its prompt with a `[review-kind: <kind>]` marker on the first non-blank line, followed by the TODO path the dispatch targets. This is what the observer + the section-commit four-dispatch gate use to attribute the dispatch.

## The 7 canonical kinds

| Marker                   | Skill / context                       | Step binding (implement-todo-section / review-todo-section)        |
| ------------------------ | ------------------------------------- | ------------------------------------------------------------------ |
| `design`                 | `codex-design-review`                 | implement step 4                                                   |
| `adversarial-impl`       | implementation-time adversarial pass  | implement step 13 (alternative to `adversarial`)                   |
| `adversarial`            | `codex-adversarial-review-section`    | implement step 13 / review step 5                                  |
| `test-coverage`          | `codex-test-coverage`                 | implement step 9 / review step 8                                   |
| `consistency`            | `codex-consistency-audit`             | implement step 20 (inline) / review step 8                         |
| `perf`                   | `codex-perf-review`                   | implement step 20 (inline) / review step 8                         |
| `re-adversarial`         | `codex-fix-review` retrigger          | implement step 20 (inline) / review step 13                        |

Step numbers are recorded by `.claude/hooks/skill_step_observer.py` via the map in `.claude/hooks/skill_step_map.py`. Coverage of all 7 kinds is locked in by 8 sub-tests in the `[skill_step_observer]` block of `scripts/test-tooling.sh`.

## Required prompt opening

```
[review-kind: <kind>] <todo-path> <section-ref> -- <one-line scope>

<rest of the prompt body>
```

Concrete shape:

```
[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md §25 -- skill_step_observer regex coverage.

Diff under review: ...
```

The `<todo-path>` token is what the codex_review_completed.py PostToolUse hook scans for to attribute the dispatch in `.claude/state/last-review-stamps.json`. Without it the per-kind stamp is recorded against `(unknown)` and the four-dispatch gate cannot match.

## What un-marked dispatches do

If the prompt's first non-blank line carries no `[review-kind: ...]`, the observer's classifier returns empty and the dispatch is NOT bound to any step. `skill_step_observer.py` emits a stderr WARN naming the active skill and pointing at this doc; the dispatch lands in the conversation with no step credit. The four-dispatch gate independently still recognizes the unmarked dispatch as a generic Codex trigger via `codex_review_completed.py`'s _classify, but cannot assign a kind so the per-kind stamp is missing.

Effect: an unmarked dispatch wastes a Codex round AND blocks the next section-commit because the gate sees no `consistency` / `perf` / etc. stamp. Always marker your dispatches.

## Skill-author checklist

Every codex-* skill that documents how to construct a prompt must:

1. Show the leading `[review-kind: <kind>]` marker in its example.
2. Match the kind to the table above (e.g. the `codex-perf-review` skill must use `perf`, not `performance`).
3. Include the targeted TODO path on the marker line.
4. Avoid first-person preambles ("I implemented...", "I'm reviewing...") -- the codex_review_completed.py PostToolUse hook emits a CONSENSAGENT-bias WARN if the leading line opens with a self-summary, since unbiased reviewer prompts produce more findings.

## Source of truth

The matcher: `.claude/hooks/skill_step_map.py` `_IMPLEMENT_TODO_SECTION` and `_REVIEW_TODO_SECTION` step rules. The classifier: `.claude/hooks/_review_kind.py detect_review_kind_from_cmd`. The tests: `scripts/test-tooling.sh [skill_step_observer]` block. Adding a new canonical kind = updating all three plus this doc.
