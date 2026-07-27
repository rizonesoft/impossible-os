# Stamp field rules -- the canonical reference

Read on demand from `review-todo-section` step 16. The step text in SKILL.md carries the four stamp lines you copy;
this file is the field-by-field spec. **This is the single source of truth for stamp grammar across the repo** --
`implement-todo-section`, `verify-todo-section`, and `quality-review-section` all defer here.

## The stamp block

```
> **Verified:** YYYY-MM-DD | commit `<hash>` | N/M items | build OK[ | <evidence token>]
> **Accepted:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX <section-ref> (item: "..." at line N)
> **Deferred:** [<sev>] <one-line finding> [(reason: <short>)] -> XREF: NN-domain/TODO-XX <section-ref> (item: "..." at line N)
> **Quality reviewed:** YYYY-MM-DD | Codex Nx (<kinds>) | <H>H+<M>M+<L>L fixed, <D> open | scope: <skill or "N/A (reason)">
```

Compact pipe-separated fields, one line per stamp, no blank lines between.

## Field rules

- `N/M items` -- `[x]` count vs total `[ ]+[/]+[x]` in the section (exclude the `Commit:` line).
- `build OK` | `build FAIL` -- single token. Do NOT re-paste `=== BUILD OK ===`.
- **Evidence token vocabulary (pick one; keep scannable):** `smoke PASS (<platform> <time>)` for boot-path work;
  `tests N/M PASS` when a dedicated suite ran; `<N> sentinels` / `<N> fields` / `<N> rows` for structural counts;
  `manual` when validation is a manual walkthrough. Free-form is still allowed; prefer the vocabulary. Skip if nothing
  surprising.
- **Codex line:** `Nx` is dispatch count (minimum 3 per current rules: adversarial + consistency + perf; bumps to 4
  when step 13.5 re-adversarial fires). `<kinds>` names dispatches (`adversarial`, `consistency`, `perf`; the legacy
  combined `quality` kind is no longer accepted -- split it into explicit entries; the legacy `dead-code` kind was
  retired 2026-04-25 and MUST NOT appear in new stamps). Add `re-adversarial` when step 13.5 fires; add `design` for
  upstream design dispatches that landed in the same effort. Findings as
  `<H>H+<M>M+<L>L fixed[, <D> open][, <N>M accepted-XREF]`; drop zero terms.
  **Keep the count terse:** `6H+5M+7L fixed, 2M accepted-XREF` is the right shape;
  `6H+5M+7L fixed (4H+3M+3L design adopted pre-code, 1H+1M+1L implement adversarial, ...)` is OVER-DETAILED --
  per-dispatch breakdowns belong in the commit message, not the stamp. `scope:` names the domain code-quality skill
  applied OR `N/A (<reason>)` like `N/A (docs-only)`. Do NOT write the full "No domain code-quality skill applies
  (...)" sentence.

## `Accepted:` vs `Deferred:` -- pick the right label

The semantic split matters for triage. Both are hook-enforced identically.

- **`Accepted:`** -- finding valid but **out-of-scope for this section**; ownership is elsewhere. XREF points to a
  concrete item in ANOTHER TODO section or file. Grep `Accepted:` when auditing ownership-transfer risk.
- **`Deferred:`** -- finding valid, **in-scope** for this TODO, but bigger than this commit. XREF points to a later
  item in THIS section or TODO file. Grep `Deferred:` when auditing "we promised to come back to this."

Further rules for both:

- **Severity tag** `[Critical]` / `[H]` / `[M]` / `[L]` is REQUIRED on every Accepted and Deferred line. Matches Codex
  finding severity. Enables `grep -r "Accepted:\s*\[H\]" todo/` for fast scope-risk triage.
- **Optional `(reason: <short>)`** -- include when the defer-rationale is not obvious from the finding. Common reasons:
  `scope` (belongs to another owner), `infra` (needs a bigger work unit), `not-functional-today` (HW/deployment
  guarantees mean it is not racing a real bug). Keep under ~10 words. Omit when the finding text alone explains it.
- Gets its OWN blockquote line between Verified and Quality reviewed. One XREF per concern; repeat the line for
  multiple. Domain-qualified (e.g. `02-kernel-core/TODO-17`, never bare `TODO-17`). The
  `(item: "<name>" at line N)` parenthetical is MANDATORY (hook-enforced, see the step 15 grammar).
- If there are NO accepted/deferred items, OMIT both lines entirely -- do NOT write `Accepted: none`.

## Escape hatch (rare)

If per-finding prose is genuinely load-bearing (cross-review context the commit message cannot carry), append a
`<details>` block after the stamps:

```
> <details><summary>Finding detail</summary>
>
> - H1 <one line> -> fixed at file:line
> - M1 <one line> -> deferred (Deferred above)
> </details>
```

Default is NO `<details>` block. Commit messages and `[x]` marks carry most audit weight; counts + XREFs carry the rest.

## Region hygiene checked at stamp time

- **Do NOT re-emit a `> **Test runner:**` line** -- the pre-stamp block written by `implement-todo-section` step 8 is
  the single source of truth. If the test count or bat file changed during review, edit the pre-stamp block in place
  instead of adding a second line.
- **Verify the `> **Notes:**` block is present and canonical-shape** (MANDATORY for sections marked `[x]` or `[/]`).
  Canonical order of the pre-stamp + stamp region: Test checkpoint paragraph -> blank -> `> **Test runner:**` -> blank
  -> `> **Notes:**` -> blank -> `> **Verified:**` -> `> **Accepted:**` (if any) -> `> **Deferred:**` (if any) ->
  `> **Quality reviewed:**`.
- If the Notes block is missing, ADD it now using the grammar in
  [implement-todo-section references/todo-bookkeeping.md](../../implement-todo-section/references/todo-bookkeeping.md)
  (3-6 bullets: what shipped, how it runs/integrates, downstream effects, canonical doc pointer, scope boundary -- ONE
  bullet each, ONE line each, NO sub-bullets, NO per-finding adoption sub-blocks).
- **If the Notes block is over-stuffed** (more than 6 bullets, or any sub-bullets, or per-finding "Design review
  adoptions" / "Implementation adversarial adoptions" / "Latent bug fixed" sub-blocks): **trim it back to canonical
  shape during this review pass.** Adoption details belong in commit messages, not Notes. `notes_bloat_check.py`
  BLOCKs edits that introduce > 6 bullets or sub-bullets; if the hook fires during the review, that is the signal to
  delete content, not to opt out via `SKIP_REVIEW_HOOK`.

The stamps are the machine-readable audit trail; commit messages are the per-finding evidence trail; Notes is the
30-second human scan summary.
