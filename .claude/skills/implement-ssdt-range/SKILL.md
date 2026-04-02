---
name: implement-ssdt-range
description: Implement and wire a range of SSDT entries (main or shadow table). Given an SSDT table range (e.g., "0x1020-0x1027 GDI Text"), implements each NtXxx function, registers it in the SSDT dispatcher, verifies it's callable via syscall, and marks Done [x] in the master table. Use when implementing a block of SSDT entries from TODO-05 or TODO-12.
---

# Implement SSDT Range

## Input

The user provides an SSDT range block — either pasted from TODO-05 (main SSDT, 0x0000+) or TODO-12 (shadow SSDT, 0x1000+). The block contains:
- Range header (e.g., `**0x1020–0x102F: GDI Text and Font**`)
- Table rows with: Index, Function name, Section reference, Owner, Done status

## Workflow

### 1. Parse the range
- Extract every function name and SSDT index from the table.
- Identify the owning TODO file (main SSDT = `todo/02-kernel-core/TODO-05-native-api-ssdt.md`, shadow SSDT = `todo/08-graphics-ui/TODO-12-win32k-shadow-ssdt.md`).
- Read the referenced section (§N) in the owning TODO for implementation details, signatures, and checklist items.

### 2. Check prerequisites
- Verify the SSDT infrastructure exists:
  - **Main SSDT:** `include/kernel/nt/ssdt.h` must define `SSDT_TABLE` and `SSDT_HANDLER`. `syscall_dispatch()` must exist. If not, implement TODO-05 §4 first.
  - **Shadow SSDT:** `include/kernel/nt/win32k_ssdt.h` must exist. `syscall_dispatch` must route `0x1000+` to Table 1. If not, implement TODO-12 §1 first.
- For each function in the range, grep the codebase for:
  - **Existing implementation** (e.g., `NtClose` in ob.c, `NtCreateEvent` in ob_event.c) — wrap it, don't rewrite.
  - **Underlying primitive** (e.g., `gfx_draw_line` for `NtGdiLineTo`, `vfs_read` for `NtReadFile`) — the NtXxx function wraps this.
  - **Missing prerequisites** — stop and report if a dependency is not yet implemented.

### 3. Implement each function
For each SSDT entry in the range:

a. **Create or locate the implementation file:**
   - Main SSDT NtXxx: `src/kernel/nt/nt_<category>.c` (e.g., `nt_file.c`, `nt_process.c`, `nt_memory.c`)
   - Shadow SSDT NtGdiXxx: `src/kernel/win32k/gdi_<category>.c` (e.g., `gdi_draw.c`, `gdi_text.c`)
   - Shadow SSDT NtUserXxx: `src/kernel/win32k/user_<category>.c` (e.g., `user_window.c`, `user_msg.c`)

b. **Write the function:**
   - Signature: `NTSTATUS NtXxxFunction(uint64_t a1, uint64_t a2, ...)` — args passed as raw uint64_t from SSDT dispatcher
   - Cast arguments to correct types inside the function
   - Validate user-mode pointers with `ProbeForRead`/`ProbeForWrite` if CPL=3 (→ XREF TODO-05 §12)
   - Call the underlying primitive (gfx_*, vfs_*, wm_*, ob_*, etc.)
   - Return proper `NTSTATUS` code (STATUS_SUCCESS, STATUS_INVALID_PARAMETER, STATUS_INVALID_HANDLE, etc.)
   - Follow freestanding kernel rules: no stdlib, use `kernel/types.h`, `kmalloc()`, etc.

c. **Register in the SSDT:**
   - Main SSDT: In `ssdt_init()` or `service_numbers.h`, set `ssdt_table[INDEX] = NtXxxFunction`
   - Shadow SSDT: In `win32k_init()`, set `win32k_table[INDEX - 0x1000] = NtXxxFunction`

d. **Add header declaration:**
   - Main SSDT: `include/kernel/nt/nt_<category>.h`
   - Shadow SSDT: `include/kernel/win32k/gdi.h` or `include/kernel/win32k/user.h`

### 4. Build and verify
```bash
bash scripts/build.sh
tail -1 build/build.log  # must show === BUILD OK ===
```

For runtime verification (if SSDT dispatcher is wired):
```bash
bash scripts/build.sh run
# Check serial output for the range's functions being called
```

### 5. Test each entry
For each function in the range, verify it's callable:
- **From kernel mode:** Direct C call to `NtXxxFunction(...)` returns `STATUS_SUCCESS` for valid input, correct error code for invalid input.
- **From SSDT dispatch:** `syscall_dispatch(INDEX, args...)` reaches the correct handler.
- **Edge cases:** NULL pointer → `STATUS_INVALID_PARAMETER`, invalid handle → `STATUS_INVALID_HANDLE`, unimplemented → `STATUS_NOT_IMPLEMENTED` (not crash).

If the TODO has a `## Unit Tests` section, add test assertions for the range:
```c
TEST_ASSERT(NtXxxFunction(valid_args) == STATUS_SUCCESS, "NtXxx succeeds with valid args");
TEST_ASSERT(NtXxxFunction(NULL) == STATUS_INVALID_PARAMETER, "NtXxx rejects NULL");
```

### 6. Mark Done in the master table
For each successfully implemented and wired entry:
- In the master SSDT table (TODO-05 or TODO-12), change `| [ ]  |` to `| [x]  |` in the Done column.
- **Only mark `[x]` if ALL of these are true:**
  1. The function implementation exists in a `.c` file
  2. The function is registered in the SSDT dispatch table (callable via `syscall`)
  3. The function returns correct NTSTATUS for valid and invalid inputs
  4. The build passes

### 7. Update the TODO section
- In the referenced section (§N), mark each `- [ ]` checklist item `[x]` for the functions just implemented.
- If ALL functions in the section are done, update the Implementation Order table row to `[x]`.
- Update the OS Comparison table if applicable.

### 8. Commit and push
- Commit message format: `"kernel: nt — wire SSDT 0xNNNN–0xNNNN (<category name>)"`
  - Example: `"kernel: win32k — wire shadow SSDT 0x1020–0x1027 (GDI text and font)"`
- Stage: implementation files, headers, SSDT registration, updated TODO file, test changes.
- Push to `origin/main` immediately.
- Mark the section's `- [ ] Commit: "..."` checklist item `[x]`.

## Validation Checklist

After completing the range, verify:

- [ ] Every function in the range has an implementation (`.c` file)
- [ ] Every function is registered in the SSDT dispatch table
- [ ] `syscall_dispatch(INDEX)` for each INDEX in range reaches the correct handler
- [ ] Invalid INDEX beyond table size returns `STATUS_NOT_IMPLEMENTED` (not crash)
- [ ] Each function handles NULL/invalid args gracefully (no kernel panic)
- [ ] Each function returns correct `NTSTATUS` codes
- [ ] Build passes: `=== BUILD OK ===`
- [ ] Done column in master table is `[x]` for every entry in the range
- [ ] Section checklist items are `[x]` in the owning TODO
- [ ] Unit tests added/updated for the range

## Guardrails

- Only implement functions listed in the provided range — do not expand scope.
- If a function requires a prerequisite that doesn't exist (e.g., SSDT dispatcher, a specific Ob type), stop and report — do not stub it with `return STATUS_NOT_IMPLEMENTED`.
- The goal is **real working code**, not stubs. Each function must do something meaningful.
- If an underlying primitive doesn't exist yet (e.g., `gfx_ellipse` for `NtGdiEllipse`), implement a minimal version or report the gap — do not skip the entry.
- Do not mark Done `[x]` unless the function is both implemented AND wired. Code existing in a `.c` file but not registered in the SSDT is NOT done.
- Follow freestanding kernel rules from CLAUDE.md: no stdlib, NASM assembly, Win32 native API surface.
