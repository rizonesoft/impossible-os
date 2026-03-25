# Implementation Order, XREF, And Index Rules

## Naming And Placement

- Choose the domain from `todo/TODO-00-INDEX.md` and the domain `INDEX.md`.
- Use the domain's live local naming pattern, such as `TODO-03-short-name.md`.
- Keep the TODO in one canonical home and cross-link instead of cloning scope elsewhere.

## Implementation Order Table

Use this format:

```markdown
| ⭐ | Order | Deliverable     | Depends On | Status |
| -- | :---: | --------------- | ---------- | :----: |
| 💎 |   1   | Foundation work | —          |  [ ]   |
| ⭐ |   2   | Exclusive work  | 1          |  [ ]   |
```

- `💎` = parity or standard work.
- `⭐` = exclusive or differentiating work.
- `Depends On` stays compact. If it gets wordy, move detail into a short note block below the table.
- Do not create a later formatting-only cleanup step; the table must be correct at creation time.

## XREF And Handoff Rules

- Add `→ XREF:` only when another TODO section is a real dependency or overlap.
- If two TODOs touch the same boundary, add a short overlap or handoff note near the affected section.
- Keep cross-file dependencies explicit enough that another agent can follow the order without guessing.

## Index Updates

- Update the domain `INDEX.md` whenever the new TODO changes active scope in that domain.
- Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-level epic visibility.
- Do not leave numbering or index updates for a later sync pass.
