# Implementation Order, XREF, And Index Rules

## Naming And Placement

- Choose the domain from `todo/TODO-00-INDEX.md` and the domain `INDEX.md`.
- Use the domain's live local naming pattern, such as `TODO-03-short-name.md`.
- Keep the TODO in one canonical home and cross-link instead of cloning scope elsewhere.

## Implementation Order Table

Use this format:

```markdown
| ⭐  | Order | Deliverable             | Depends On | Status |
| --- | :---: | ----------------------- | ---------- | :----: |
| 💎  |   1   | Foundation / parity     | —          |  [ ]   |
| ⭐  |   2   | Exclusive differentiator | 1          |  [ ]   |
```

- `💎` = parity work — this is what Windows 11 and Linux already do; Impossible OS must match it.
- `⭐` = exclusive work — Impossible OS is superior or first; this is a differentiating feature.
- Every row must carry one of these two markers. Never leave the first column blank or use numbers there.
- `Depends On` stays compact. If it gets wordy, move detail into a short note block below the table.
- Do not create a later formatting-only cleanup step; the table must be correct at creation time.

## OS Comparison Table

Every TODO must include an `## OS Comparison` section. Use this format:

```markdown
## OS Comparison

| ⭐  | Feature          | 🪟 Windows 11 | 🐧 Linux | 🚀 Impossible OS                |
| --- | ---------------- | ------------- | -------- | ------------------------------- |
| 💎  | Core feature     | ✅            | ✅       | ⬜ Planned — §1.1               |
| ⭐  | Exclusive feature | ❌           | ❌       | ⬜ **Planned — world-first** 🚀 |
```

- `💎` rows = parity features; Impossible OS must reach the same level as Windows and Linux.
- `⭐` rows = exclusive features; Impossible OS goes further than either competitor.
- Status cells: ✅ = done/shipping, ⚠️ = partial/limited, ❌ = not available, ⬜ = planned (link the section).
- Add a short paragraph after the table summarising the competitive position at each milestone.

## XREF And Handoff Rules

- Add `→ XREF:` only when another TODO section is a real dependency or overlap.
- If two TODOs touch the same boundary, add a short overlap or handoff note near the affected section.
- Keep cross-file dependencies explicit enough that another agent can follow the order without guessing.

## Index Updates

- Update the domain `INDEX.md` whenever the new TODO changes active scope in that domain.
- Update `todo/TODO-00-INDEX.md` only when the new TODO changes root-level epic visibility.
- Do not leave numbering or index updates for a later sync pass.
