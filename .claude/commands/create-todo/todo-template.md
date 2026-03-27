# TODO Template

Use this as the default starting point for a new leaf TODO.

```markdown
# TODO-03-short-name

> **Goal:** One short paragraph describing the current state, the change to make, and the end state.

## Inputs

- [Relevant source or doc](../../path/to/input.md)

## Outcome

- Clear result 1.
- Clear result 2.

## Implementation Order

| ⭐  | Order | Deliverable             | Depends On | Status |
| --- | :---: | ----------------------- | ---------- | :----: |
| 💎  |   1   | Foundation / parity     | —          |  [ ]   |
| 💎  |   2   | More parity work        | §1         |  [ ]   |
| ⭐  |   3   | Exclusive differentiator | §1, §2    |  [ ]   |

> 💎 = parity work — matches what Windows 11 and Linux already do.
> ⭐ = exclusive work — Impossible OS is superior or first.

## 1. Workstream `[Sonnet]`

### 1.1 First Section

- [ ] Implement the scoped change.
- [ ] Verify the scoped change.
- [ ] Commit: `"scope: short description"`

## OS Comparison

| ⭐  | Feature          | 🪟 Windows 11 | 🐧 Linux | 🚀 Impossible OS                  |
| --- | ---------------- | ------------- | -------- | --------------------------------- |
| 💎  | Core feature     | ✅            | ✅       | ⬜ Planned — §1.1                 |
| ⭐  | Exclusive feature | ❌           | ❌       | ⬜ **Planned — world-first** 🚀   |

> After completing parity items Impossible OS matches Windows and Linux.
> Exclusive items make Impossible OS superior.

## Verification

- [ ] Build and test evidence is defined.
- [ ] Remaining limits or follow-ups are called out explicitly.
```

## Notes

- Default to a leaf TODO.
- Add child-TODO sections only when the topic truly needs multiple files or shared verification.
- Keep tables compact. The `💎`/`⭐` column must always be present in both the Implementation Order table and the OS Comparison table.
- The OS Comparison table is **mandatory** in every TODO — it is what makes Impossible OS's competitive position visible.
- Use ✅ = done, ⚠️ = partial, ⬜ = planned, ❌ = not available/not applicable.
