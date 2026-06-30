# Codex Prompt Shape -- Canonical Review-Kind Markers

Every Codex dispatch MUST go through the canonical wrapper, with the prompt as a single-quoted argv:

```bash
bash scripts/codex-dispatch.sh '[review-kind: <kind>] <todo-path> <body>'
```

The wrapper enforces `argc == 1` (multi-argv misuse fails loud) and `exec`s into `node ".../codex-companion.mjs" adversarial-review "$1"`. The prompt's first non-blank line MUST start with the `[review-kind: <kind>]` marker followed by the TODO path the dispatch targets. This is what the observer + the section-commit four-dispatch gate use to attribute the dispatch.

> **Why single quotes are mandatory.** Bash double-quotes evaluate `$(...)`, `${...}`, and backslash escapes BEFORE the wrapper sees argv. By the time the wrapper validates, any unintended substitution has already happened. The escaping discipline is enforced at the source-text layer by `scripts/lint.sh` Check 12 -- single-quote your prompts so the lint never has reason to fire.

The legacy direct-node form (`node ".../codex-companion.mjs" adversarial-review "<prompt>"`) is still recognized by the hooks for backward compat with existing logs and ad-hoc invocations, but new dispatches use the wrapper. The bare CLI form `codex review "<prompt>"` is also recognized; the current classifier also accepts `codex e "<prompt>"`, but TODO-10 classifies that as stale because `codex e` is the `codex exec` driver alias, not trusted reviewer proof.

**Multi-line dispatches are supported.** A `node ".../codex-companion.mjs" \<NL>    adversarial-review '<prompt>'` shape (typed verbatim or pulled from a docs example with backslash line-continuation) classifies identically to its single-line equivalent; the helper preprocesses `\<newline>[ \t]*` to whitespace before tokenizing.

## The 8 canonical kinds

| Marker                   | Skill / context                       | Step binding (implement-todo-section / review-todo-section / gap-audit-todo) |
| ------------------------ | ------------------------------------- | ------------------------------------------------------------------ |
| `design`                 | `codex-design-review`                 | implement step 4                                                   |
| `adversarial-impl`       | implementation-time adversarial pass  | implement step 13 (alternative to `adversarial`)                   |
| `adversarial`            | `codex-adversarial-review-section`    | implement step 13 / review step 5                                  |
| `test-coverage`          | `codex-test-coverage`                 | implement step 9 / review step 8                                   |
| `consistency`            | `codex-consistency-audit`             | implement step 20 (inline) / review step 8                         |
| `perf`                   | `codex-perf-review`                   | implement step 20 (inline) / review step 8                         |
| `re-adversarial`         | `codex-fix-review` retrigger          | implement step 20 (inline) / review step 13                        |
| `gap-audit`              | `codex-gap-audit`                     | gap-audit-todo step 14.5 (mandatory)                               |

Step numbers are recorded by `.claude/hooks/skill_step_observer.py` via the map in `.claude/hooks/skill_step_map.py`. Coverage of all 8 kinds is locked in by 8 sub-tests in the `[skill_step_observer]` block of `scripts/test-tooling.sh`.

## Required prompt opening

```
[review-kind: <kind>] <todo-path> <section-ref> -- <one-line scope>

<rest of the prompt body>
```

Concrete shape (canonical wrapper form):

```bash
bash scripts/codex-dispatch.sh '[review-kind: adversarial] todo/00-infrastructure/TODO-08-automation-hardening.md §25 -- skill_step_observer regex coverage.

Diff under review: ...'
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
