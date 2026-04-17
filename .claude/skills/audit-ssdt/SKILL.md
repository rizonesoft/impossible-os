---
name: audit-ssdt
description: Audit SSDT status against actual codebase implementations. Finds all ssdt_register() calls, compares with TODO-05/TODO-12 master tables and TODO-05 checklist items, flags mismatches, inserts missing prerequisite/ownership TODO items, and updates docs. Use after implementing SSDT handlers, or periodically to keep status accurate. Auto-triggered by /verify-todo-section for SSDT-related sections.
---

# Audit SSDT

## Use This Skill When

- After implementing one or more SSDT handlers (`NtXxx` functions) and calling `ssdt_register()`.
- Periodically to catch drift between TODO-05/TODO-12 master tables and actual code.
- The `/verify-todo-section` pipeline auto-triggers this for SSDT-related sections.
- The user asks "which SSDT entries are wired?" or "audit the SSDT table."
- Before starting a new SSDT range implementation to get a clean baseline.

Reconcile SSDT status in TODO docs against the actual codebase. This includes:
- Master tables in TODO-05 (main, 0x0000+) and TODO-12 (shadow, 0x1000+)
- SSDT-related checklist items in the rest of TODO-05 (sections below the master table)
- PE import export tables in `src/kernel/pe.c` (kernel32/ntdll -> SSDT mapping)
- EIF import indices used in test binaries or referenced in TODO-23

The goal: every truly implemented and wired service has `[x]`, and no `[x]` exists for missing/stub handlers.

## Invocation

- **Standalone:** `/audit-ssdt` -- full audit of all SSDT tables
- **Auto-triggered:** from `/verify-todo-section` when the section references `ssdt_register()`, SSDT service numbers, `NtXxx` functions, or the TODO is TODO-05/TODO-12

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

### 3. Detect duplicates and gaps in service_numbers.h

```
Grep for: #define SSDT_Nt
File:     include/kernel/nt/service_numbers.h
```

- Extract all `#define SSDT_Nt* 0xNNNN` values.
- **Duplicate detection:** flag any two defines with the same numeric value. This would cause one handler to silently overwrite another during registration.
- **Gap detection:** flag numeric gaps in sequential ranges (e.g., 0x0010 defined, 0x0011 missing, 0x0012 defined). Gaps are acceptable if intentional (reserved for future use), but unexpected gaps should be flagged.
- Verify `SSDT_MAIN_COUNT` matches the actual count of defines.

### 4. Read the master tables

- **Main SSDT:** `todo/02-kernel-core/TODO-05-native-api-ssdt.md` section `### SSDT Master Table`
- **Shadow SSDT:** `todo/08-graphics-ui/TODO-15-win32k-shadow-ssdt.md` section `### Shadow SSDT Master Table`

Parse every row: Index, Function, Owner, Done status.

### 5. Read TODO-05 SSDT checklist items outside the master table

Scan TODO-05 for checklist bullets that describe SSDT work (especially lines containing `Nt*`, `SSDT`, or explicit indices like `0x003C`). Do not skip items just because they are in a section that looks complete.

For each checklist item, extract:
- Checkbox state (`[ ]` / `[x]`)
- Claimed status text (implemented, stub, alias, etc.)
- Function name(s) and index/indexes (if present)
- Section context (`## N.` heading)

### 6. Cross-check PE import export tables

Read `src/kernel/pe.c` for the kernel-side export tables (`s_kernel32_exports[]`, `s_ntdll_exports[]`, etc.). For each entry:
- Verify the SSDT index constant is valid (exists in `service_numbers.h`)
- Verify the SSDT index has a registered handler (from step 1)
- Flag any PE export mapping to an unregistered SSDT entry -- this means a Win32 API call will fail at runtime

### 7. Cross-check EIF import indices

Grep for EIF test binaries or TODO-23 references that use specific SSDT indices. Verify each referenced index has a registered handler.

### 8. Diff and report

Produce a structured report:

```
## SSDT Audit Report

### Implemented but not marked Done
| Index | Function | File:Line | Action |
|-------|----------|-----------|--------|

### Marked Done but not implemented
| Index | Function | Master Table Row | Action |
|-------|----------|-----------------|--------|

### Checklist mismatches (outside master table)
| Section | Item | Claimed | Reality | Action |
|---------|------|---------|---------|--------|

### PE export table mismatches
| DLL | Function | SSDT Index | Issue |
|-----|----------|------------|-------|

### service_numbers.h issues
| Issue | Details |
|-------|---------|

### Missing prerequisite/ownership items
| Section | Missing Item | Owner | XREF |
|---------|-------------|-------|------|

### Consistency summary
| Metric | Value |
|--------|-------|
| Defines in service_numbers.h | N |
| Rows in master table | N |
| ssdt_register() calls | N |
| Master table [x] count | N |
| Duplicate numeric values | N |
| PE exports -> unregistered SSDT | N |
```

### 9. Functional completeness check (not just wired)

For each handler that passes the "registered + not stub" check, verify it is **functionally complete**, not just partially implemented:

- **Read the handler body.** Does it handle the documented parameters and code paths? A `NtCreateFile` that only handles `FILE_OPEN` but not `FILE_CREATE` is incomplete.
- **Check return paths.** Does the normal path return a meaningful `NTSTATUS`? Are error paths handled (not just `return STATUS_NOT_IMPLEMENTED` for edge cases)?
- **Check documented behavior.** Cross-reference against the TODO-05 checklist description for that handler. If the checklist says "implement X, Y, Z" and only X is done, the handler is `[/]` not `[x]`.
- **Classify:** `COMPLETE` (all documented behavior), `PARTIAL` (some paths work, others return NOT_IMPLEMENTED), `STUB` (registered but no real logic).
- Only `COMPLETE` handlers get `[x]`. `PARTIAL` gets `[/]` with a note listing what's missing. `STUB` gets `[ ]`.
- **Downgrade rule:** If a master table entry or checklist item is currently `[x]` but the handler is PARTIAL or STUB, **unconditionally revert to `[/]` or `[ ]`**. A previous `[x]` does not grandfather a handler -- every audit re-evaluates from scratch against current code. No exceptions.

### 10. Cross-reference and loose-end check

For each handler marked COMPLETE, verify the full chain:

- **Cross-TODO consumers:** Grep all TODO files for references to this `NtXxx` function. Are the consumers' checklist items updated? (e.g., if `NtCreateFile` is done, does TODO-08 §9's PE import table list it as resolved?)
- **PE export table sync:** Is the function listed in `s_kernel32_exports[]` or `s_ntdll_exports[]` in `pe.c`? If a Win32 wrapper exists (e.g., `CreateFileW` -> `NtCreateFile`), it should be in the export table.
- **EIF dispatch table:** If the SSDT index is used by EIF binaries, verify the dispatch table maps it correctly.
- **Unit test coverage:** Does `test_nt_types.c` or another test file assert on this handler? Flag handlers with zero test coverage.
- **Documentation sync:** Is the handler mentioned in the TODO-05 master table Owner column with the correct file reference?

Flag any broken chain links as loose ends in the report. These are NOT blockers for `[x]` on the handler itself, but they are action items that need follow-up.

Add a new report section:
```
### Loose ends (cross-reference gaps)
| Function | Issue | Action |
|----------|-------|--------|
| NtCreateFile | Not in s_kernel32_exports[] | Add CreateFileW mapping |
| NtWriteFile | No unit test | Add to test_nt_types.c |
```

### 11. Update the master tables

For each mismatch found in steps 8-10:
- Edit the Done column: `[ ]` -> `[x]` (COMPLETE only), `[x]` -> `[/]` (PARTIAL), or `[x]` -> `[ ]` (STUB/missing)
- Update the Owner column to reference the implementing source file and TODO section (e.g., `T02 §7 (etw.c)`)
- If a range header has all entries `[x]`, add `-- **N/N DONE** (file.c, T02 §N)` to the header

### 12. Update TODO-05 checklist items (outside master table)

For mismatches from steps 8-10:
- Update checkbox state to match reality.
- Rewrite stale wording when needed (for example, checked "stub" bullets that are now real implementations).
- Preserve index references and section links while updating status text.
- If a section lacks prerequisite/ownership clarity, add explicit unchecked checklist items with owners/XREFs in dependency order.
- Even if a section has many `[x]` items, insert newly discovered missing `[ ]` items where they logically belong.

### 13. Update the progress summary

At the bottom of the master table, update the implementation progress line:
```
> **Implementation progress: N/470 wired** (X.X%) -- [list of complete ranges]. Run `/audit-ssdt` to refresh.
```

### 14. Commit

If changes were made, commit with:
```
docs: audit SSDT master table -- N/470 wired (X.X%)
```

## Guardrails

- Do NOT count test registrations (test_nt_types.c uses 0x03FF for unit testing).
- Do NOT change service number definitions in `service_numbers.h` -- this skill only audits the Done column.
- Do NOT implement missing handlers -- just flag them. Use `/implement-ssdt-range` for that.
- The `ssdt_stub_not_implemented` default handler is NOT a real implementation.
- **3-tier classification** (from step 9):
  - `[x]` = COMPLETE: registered + all documented code paths implemented + error handling + not just STATUS_NOT_IMPLEMENTED stubs on edge cases.
  - `[/]` = PARTIAL: registered + some paths work but others return STATUS_NOT_IMPLEMENTED or are missing. Note what's missing.
  - `[ ]` = STUB/MISSING: not registered, or registered but handler is a stub/placeholder.
- Same strict rule for checklist items: only `[x]` for COMPLETE handlers.
- Always validate against live codebase evidence; TODO checkbox state is never source-of-truth.
- If evidence is incomplete/ambiguous, keep or revert to `[ ]` and add explicit missing prerequisite/ownership items.
- Before changing any checklist item to `[x]`, verify ALL of the following:
  1. Correct SSDT registration exists for the listed service/index.
  2. Handler symbol resolves to real code (not a stub symbol name and not a thin placeholder).
  3. Handler does not simply return `STATUS_NOT_IMPLEMENTED` in the normal path.
  4. Handler covers all documented parameters and code paths (not just the happy path).
  5. (When an index is listed) registration index matches the checklist claim.
  6. Cross-TODO consumers are aware of the implementation (loose-end check from step 10).
- Shadow SSDT entries (0x1000+) are in a separate table and TODO file.
- Owner references use compact notation: `T02 §7 (file.c)` -- not `TODO-02 Section 7`.
