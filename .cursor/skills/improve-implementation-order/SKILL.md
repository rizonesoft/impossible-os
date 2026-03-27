---
name: improve-implementation-order
description: Audit and rewrite the Implementation Order table in a TODO file — verify execution order is correct and gap-free, compress verbose cross-domain references into compact notation (D=domain, T=TODO, §=section), and ensure every body section maps to a table row. Use when asked to improve, fix, audit, or review an Implementation Order table in a TODO file.
---

# Improve Implementation Order Table

## Compact Notation Rules

All dependency cells use compact notation only. Never write full file paths in the table.

| Dependency type | Format | Example |
|-----------------|--------|---------|
| No dependency | `—` | `—` |
| Internal row reference | `§N` | `§2` |
| Multiple internal refs | `§N, §M` | `§1, §3` |
| Boot phase | `P0` / `P1` | `P0` |
| Same-domain TODO | `TNN §N` | `T17 §3` |
| Cross-domain TODO | `DNN TNN §N` | `D02 T19 §1` |
| Multiple TODO refs (same row) | `TNN §N & TNN §N` | `T17 §1–3 & T19 §3` |
| Section range | `§N–N` | `§1–7` |

Domain numbers are the two-digit prefix of the folder (e.g. `02` for `02-kernel-core`, `04` for `04-drivers-hardware`). Omit the domain prefix when the referenced TODO is in the **same domain** as the file being edited.

Use `&` to join tightly-coupled deps that must both be satisfied at the same call site. Use `,` to separate independent prerequisites.

## Workflow

1. **Read** the full TODO file.
2. **Map body sections to table rows.**
   - List every `## N.` section heading found in the body.
   - List every row number in the Implementation Order table.
   - Flag any section that has no table row (missing row).
   - Flag any table row that has no corresponding body section (orphan row).
3. **Verify execution order.**
   - For each row N, confirm every `§M` dep has M < N.
   - Flag any row whose internal dep references a later row number — ordering error.
4. **Convert verbose references to compact notation.**
   - Replace full paths with `DNN TNN §N` compact form.
   - Same-domain refs → `TNN §N`.
5. **Check status column accuracy.**
   - Do not change status without code evidence — flag inconsistencies for `verify-todo-section`.
6. **Rewrite the table** with corrected order, compact notation, and aligned columns.

## Table Format

```
| ⭐  | Order | Deliverable                  | Depends On              | Status |
| --- | :---: | ---------------------------- | ----------------------- | :----: |
| 💎  |   1   | Short deliverable name       | —                       |  [ ]   |
| 💎  |   2   | Short deliverable name       | §1                      |  [ ]   |
| ⭐  |   3   | Exclusive deliverable name   | §1, D02 T19 §11         |  [ ]   |
```

## Guardrails

- Do not change the content or scope of deliverables — only order, notation, and formatting.
- Do not mark rows complete based on code inspection; flag for `verify-todo-section` instead.
- Do not reorder rows if doing so would change the meaning of the plan.
- If a dep chain is circular, flag it explicitly rather than silently reordering.
