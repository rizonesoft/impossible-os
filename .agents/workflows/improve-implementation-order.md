---
description: Audit and rewrite the Implementation Order table in a TODO file — verify execution order is correct and gap-free, compress verbose cross-domain references into compact notation, and ensure every body section maps to a table row.
---

> **Cursor skill (authoritative):** [`.cursor/skills/improve-implementation-order/SKILL.md`](../../.cursor/skills/improve-implementation-order/SKILL.md)
> **Canonical workflow:** [`.impossible/workflows/improve-implementation-order.md`](../../.impossible/workflows/improve-implementation-order.md)

## Compact Notation

| Dependency type | Format | Example |
|-----------------|--------|---------|
| No dependency | `—` | `—` |
| Internal row ref | `§N` | `§2` |
| Same-domain TODO | `TNN §N` | `T17 §3` |
| Cross-domain TODO | `DNN TNN §N` | `D02 T19 §1` |
| Section range | `§N–N` | `§1–7` |

## Workflow

1. Read the full TODO file
2. Map body `## N.` sections to table rows — flag missing rows or orphan rows
3. Verify execution order: every `§M` dep must have M < N (flag violations)
4. Convert verbose dep refs to compact notation (`DNN TNN §N`)
5. Check status column accuracy — flag inconsistencies, do not change without evidence
6. Rewrite table with corrected order, compact notation, aligned columns

## Guardrails

- Do not change the content or scope of deliverables — only order, notation, formatting
- Do not mark rows complete based on code inspection alone
- Do not reorder if doing so changes the meaning of the plan
- Flag circular dep chains explicitly rather than silently reordering
