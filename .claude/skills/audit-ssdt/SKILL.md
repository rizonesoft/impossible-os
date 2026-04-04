---
name: audit-ssdt
description: Audit the SSDT Master Table against actual codebase implementations. Finds all ssdt_register() calls, compares with the Done column in TODO-05 and TODO-12, flags mismatches, and updates the tables. Use after implementing SSDT handlers, or periodically to ensure the master tables reflect reality.
---

# Audit SSDT

Reconcile the SSDT Master Tables in TODO-05 (main, 0x0000+) and TODO-12 (shadow, 0x1000+) against the actual codebase. The goal: every `ssdt_register()` call in production code has a matching `[x]` in the master table, and no `[x]` exists without a real handler.

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

### 4. Diff and report

Produce a report with three sections:

**Implemented but not marked Done:**
Entries where `ssdt_register()` exists in code but the master table shows `[ ]`.
- Action: mark `[x]` and update Owner to show the implementing file.

**Marked Done but not implemented:**
Entries where the master table shows `[x]` but no `ssdt_register()` call exists.
- Action: revert to `[ ]` with a note.

**Consistency check:**
- Total defined in `service_numbers.h` (SSDT_MAIN_COUNT) vs total rows in master table.
- Total `[x]` in table vs total `ssdt_register()` calls.
- Any `ssdt_register()` using a raw number instead of a `SSDT_NtXxx` constant.

### 5. Update the master tables

For each mismatch found in step 4:
- Edit the Done column: `[ ]` -> `[x]` or `[x]` -> `[ ]`
- Update the Owner column to reference the implementing source file and TODO section (e.g., `T02 S7 (etw.c)`)
- If a range header has all entries `[x]`, add `-- **N/N DONE** (file.c, TXXSYY)` to the header

### 6. Update the progress summary

At the bottom of the master table, update the implementation progress line:
```
> **Implementation progress: N/470 wired** (X.X%) -- [list of complete ranges]. Run `/audit-ssdt` to refresh.
```

### 7. Commit

If changes were made, commit with:
```
docs: audit SSDT master table -- N/470 wired (X.X%)
```

## Guardrails

- Do NOT count test registrations (test_nt_types.c uses 0x03FF for unit testing).
- Do NOT change service number definitions in `service_numbers.h` -- this skill only audits the Done column.
- Do NOT implement missing handlers -- just flag them. Use `/implement-ssdt-range` for that.
- The `ssdt_stub_not_implemented` default handler is NOT a real implementation.
- Shadow SSDT entries (0x1000+) are in a separate table and TODO file.
