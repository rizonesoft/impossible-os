---
name: audit-ssdt
description: Audit SSDT status against actual codebase implementations. Finds all ssdt_register() calls, compares with TODO-05/TODO-12 master tables and TODO-05 checklist items, flags mismatches, inserts missing prerequisite/ownership TODO items, and updates docs. Use after implementing SSDT handlers, or periodically to keep status accurate.
---

# Audit SSDT

Reconcile SSDT status in TODO docs against the actual codebase. This includes:
- Master tables in TODO-05 (main, 0x0000+) and TODO-12 (shadow, 0x1000+)
- SSDT-related checklist items in the rest of TODO-05 (sections below the master table)

The goal: every truly implemented and wired service has `[x]`, and no `[x]` exists for missing/stub handlers.

## Workflow

### 1. Scan the codebase for all registered handlers

```
Grep for: ssdt_register(
Files:    src/kernel/**/*.c (exclude src/kernel/test/)
```

For each match, extract:
- The service number constant (e.g., `SSDT_NtTraceEvent`)
- The handler function name
- The source file and line number

Also resolve each constant to its numeric value by reading `include/kernel/nt/service_numbers.h`.

### 2. Scan for SSDT handler implementations

```
Grep for: ^NTSTATUS Nt
Files:    src/kernel/**/*.c (exclude src/kernel/test/)
```

Cross-reference: every function passed to `ssdt_register()` must have a matching implementation. Flag orphaned registrations (register without implementation) or orphaned implementations (function exists but not registered).

### 3. Read the master tables

- **Main SSDT:** `todo/02-kernel-core/TODO-05-native-api-ssdt.md` section `### SSDT Master Table`
- **Shadow SSDT:** `todo/08-graphics-ui/TODO-12-win32k-shadow-ssdt.md` section `### Shadow SSDT Master Table` (if it exists)

Parse every row: Index, Function, Owner, Done status.

### 4. Read TODO-05 SSDT checklist items outside the master table

Scan TODO-05 for checklist bullets that describe SSDT work (especially lines containing `Nt*`, `SSDT`, or explicit indices like `0x003C`). Do not skip items just because they are in a section that looks complete.

For each checklist item, extract:
- Checkbox state (`[ ]` / `[x]`)
- Claimed status text (implemented, stub, alias, etc.)
- Function name(s) and index/indexes (if present)
- Section context (`## N.` heading)

### 5. Diff and report

Produce a report with four sections:

**Implemented but not marked Done:**
Entries where `ssdt_register()` exists in code, the handler is real (not stub / not `STATUS_NOT_IMPLEMENTED` placeholder), and the master table shows `[ ]`.
- Action: mark `[x]` and update Owner to show the implementing file.

**Marked Done but not implemented:**
Entries where the master table shows `[x]` but no `ssdt_register()` call exists.
- Action: revert to `[ ]` with a note.

**Checklist mismatches (outside master table):**
- `[ ]` item but code is fully implemented and wired.
  - Action: change to `[x]` only after strict proof (see guardrails).
- `[x]` item but code is stub / not wired / `STATUS_NOT_IMPLEMENTED`.
  - Action: change to `[ ]` or rewrite as explicit stub status if intentionally deferred.
- `[x]` item text says "stub" but implementation is now real.
  - Action: keep `[x]` and rewrite text to the real implemented behavior.

**Missing prerequisite/ownership items:**
- Section marked complete but evidence shows missing prerequisite implementation/wiring work.
  - Action: add new unchecked checklist items in that section in logical prerequisite-first order.
- Every newly added unchecked item must include ownership + reference:
  - owner path/function or owning TODO section
  - `→ XREF: TODO-XX §N` for external dependency
  - placement before dependent checklist items

**Consistency check:**
- Total defined in `service_numbers.h` (SSDT_MAIN_COUNT) vs total rows in master table.
- Total `[x]` in table vs total `ssdt_register()` calls.
- Any `ssdt_register()` using a raw number instead of a `SSDT_NtXxx` constant.

### 6. Update the master tables

For each mismatch found in step 5:
- Edit the Done column: `[ ]` -> `[x]` or `[x]` -> `[ ]`
- Update the Owner column to reference the implementing source file and TODO section (e.g., `T02 S7 (etw.c)`)
- If a range header has all entries `[x]`, add `-- **N/N DONE** (file.c, TXXSYY)` to the header

### 7. Update TODO-05 checklist items (outside master table)

For mismatches from step 5:
- Update checkbox state to match reality.
- Rewrite stale wording when needed (for example, checked "stub" bullets that are now real implementations).
- Preserve index references and section links while updating status text.
- If a section lacks prerequisite/ownership clarity, add explicit unchecked checklist items with owners/XREFs in dependency order.
- Even if a section has many `[x]` items, insert newly discovered missing `[ ]` items where they logically belong.

### 8. Update the progress summary

At the bottom of the master table, update the implementation progress line:
```
> **Implementation progress: N/470 wired** (X.X%) -- [list of complete ranges]. Run `/audit-ssdt` to refresh.
```

### 9. Commit

If changes were made, commit with:
```
docs: audit SSDT master table -- N/470 wired (X.X%)
```

## Guardrails

- Do NOT count test registrations (test_nt_types.c uses 0x03FF for unit testing).
- Do NOT change service number definitions in `service_numbers.h` -- this skill only audits the Done column.
- Do NOT implement missing handlers -- just flag them. Use `/implement-ssdt-range` for that.
- The `ssdt_stub_not_implemented` default handler is NOT a real implementation.
- `Done=[x]` requires BOTH: wired via `ssdt_register()` and functionally implemented. Do not mark stubs (e.g., `*_stub`) or handlers that just return `STATUS_NOT_IMPLEMENTED`.
- Same strict rule for checklist items: only mark complete when both implemented and wired.
- Always validate against live codebase evidence; TODO checkbox state is never source-of-truth.
- If evidence is incomplete/ambiguous, keep or revert to `[ ]` and add explicit missing prerequisite/ownership items.
- Before changing any checklist item to `[x]`, verify all of the following:
  1. Correct SSDT registration exists for the listed service/index.
  2. Handler symbol resolves to real code (not a stub symbol name and not a thin placeholder).
  3. Handler does not simply return `STATUS_NOT_IMPLEMENTED` in the normal path.
  4. (When an index is listed) registration index matches the checklist claim.
- Shadow SSDT entries (0x1000+) are in a separate table and TODO file.
