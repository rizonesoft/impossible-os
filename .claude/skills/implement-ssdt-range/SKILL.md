---
name: implement-ssdt-range
description: Implement and wire a range of SSDT entries (main or shadow table). Given an SSDT table range (e.g., "0x1020-0x1027 GDI Text"), implements each NtXxx function, registers it in the SSDT dispatcher, verifies it's callable via syscall, and marks Done [x] in the master table. Use when implementing a block of SSDT entries from TODO-05 or TODO-12.
---

# Implement SSDT Range

> **External-Reviewer Contract:** This skill dispatches Codex as part of its workflow. Every finding goes through `superpowers:receiving-code-review` (verify at file:line, Fix / Reject / Accept, never blind-implement). Canonical contract: [docs/infrastructure/ai-system.md#external-reviewer-contract-codex](../../../docs/infrastructure/ai-system.md#external-reviewer-contract-codex).

## Use This Skill When

- The user pastes or references an SSDT range block (e.g., "implement 0x1020-0x1027 GDI Text").
- Batch-implementing a group of `NtXxx`/`NtGdi`/`NtUser` syscall stubs from TODO-05 or TODO-12.
- The user asks to "wire up SSDT entries" or "implement the next SSDT block."
- Do NOT use for single ad-hoc `NtXxx` functions -- just implement inline.

## Input

The user provides an SSDT range block -- either pasted from TODO-05 (main SSDT, 0x0000+) or TODO-12 (shadow SSDT, 0x1000+). The block contains:
- Range header (e.g., `**0x1020-0x102F: GDI Text and Font**`)
- Table rows with: Index, Function name, Section reference, Owner, Done status

## Workflow

### 1. Parse the range
- Extract every function name and SSDT index from the table.
- Identify the owning TODO file (main SSDT = `todo/02-kernel-core/TODO-05-native-api-ssdt.md`, shadow SSDT = `todo/08-graphics-ui/TODO-15-win32k-shadow-ssdt.md`).
- Read the referenced section (§N) in the owning TODO for implementation details, signatures, and checklist items.

### 2. Check prerequisites
- Verify the SSDT infrastructure exists:
  - **Main SSDT:** `include/kernel/nt/ssdt.h` must define `SSDT_TABLE` and `SSDT_HANDLER`. `syscall_dispatch()` must exist. If not, implement TODO-05 §4 first.
  - **Shadow SSDT:** `include/kernel/nt/win32k_ssdt.h` must exist. `syscall_dispatch` must route `0x1000+` to Table 1. If not, implement TODO-12 §1 first.
- For each function in the range, grep the codebase for:
  - **Existing implementation** (e.g., `NtClose` in ob.c, `NtCreateEvent` in ob_event.c) -- wrap it, don't rewrite.
  - **Underlying primitive** (e.g., `gfx_draw_line` for `NtGdiLineTo`, `vfs_read` for `NtReadFile`) -- the NtXxx function wraps this.
  - **Missing prerequisites** -- stop and report if a dependency is not yet implemented.

### 3. Run `kernel-code-quality` skill
SSDT handlers are security-critical -- user-mode code calls directly into these functions via SYSCALL. Walk the quality gates BEFORE writing code:
- **Gate 2 (SMP):** SSDT handlers run on any CPU. Any shared state they touch needs locks or atomics.
- **Gate 5 (Error handling):** Every handler MUST validate all parameters and return proper NTSTATUS. No silent failures.
- **Gate 6 (Bare metal):** If the handler touches MMIO or CPUID-gated features, check bare-metal correctness.
- **Gate 9 (Production):** No stubs, no STATUS_NOT_IMPLEMENTED on the normal path. Real working code.

### 4. Implement each function
For each SSDT entry in the range:

a. **Create or locate the implementation file:**
   - Main SSDT NtXxx: `src/kernel/nt/nt_<category>.c` (e.g., `nt_file.c`, `nt_process.c`, `nt_memory.c`)
   - Shadow SSDT NtGdiXxx: `src/kernel/win32k/gdi_<category>.c` (e.g., `gdi_draw.c`, `gdi_text.c`)
   - Shadow SSDT NtUserXxx: `src/kernel/win32k/user_<category>.c` (e.g., `user_window.c`, `user_msg.c`)

b. **Write the function:**
   - Signature: `NTSTATUS NtXxxFunction(uint64_t a1, uint64_t a2, ...)` -- args passed as raw uint64_t from SSDT dispatcher
   - Cast arguments to correct types inside the function
   - Validate user-mode pointers with `ProbeForRead`/`ProbeForWrite` if CPL=3 (-> XREF TODO-05 §12)
   - Call the underlying primitive (gfx_*, vfs_*, wm_*, ob_*, etc.)
   - Return proper `NTSTATUS` code (STATUS_SUCCESS, STATUS_INVALID_PARAMETER, STATUS_INVALID_HANDLE, etc.)
   - Follow freestanding kernel rules: no stdlib, use `kernel/types.h`, `kmalloc()`, etc.

c. **Register in the SSDT:**
   - Main SSDT: call `ssdt_register(SSDT_NtXxx, NtXxxFunction)` in the appropriate init function
   - Shadow SSDT: call `win32k_register(SSDT_NtXxx, NtXxxFunction)` in `win32k_init()`

d. **Add header declaration:**
   - Main SSDT: `include/kernel/nt/nt_<category>.h`
   - Shadow SSDT: `include/kernel/win32k/gdi.h` or `include/kernel/win32k/user.h`

e. **Update PE export tables:**
   - If the NtXxx function has a Win32 wrapper name (e.g., `NtCreateFile` -> `CreateFileW`), add the mapping to the appropriate export table in `src/kernel/pe.c`:
     - `s_kernel32_exports[]` for kernel32.dll functions
     - `s_ntdll_exports[]` for ntdll.dll functions
   - Keep the export array sorted by name (binary search requirement).

### 5. Build and verify
```bash
bash scripts/build.sh
tail -1 build/build.log  # must show === BUILD OK ===
```

### 6. Wire unit tests
- Check which `TEST_CAT_*` category fits the range (e.g., `TEST_CAT_ABI` for Nt syscalls, `TEST_CAT_OB` for object operations).
- Add test assertions in the appropriate `test_*.c` file (e.g., `test_nt_types.c` for Nt syscalls).
- For each function in the range:
  ```c
  TEST_ASSERT(NtXxxFunction(valid_args) == STATUS_SUCCESS, "NtXxx succeeds with valid args");
  TEST_ASSERT(NtXxxFunction(NULL_arg) == STATUS_INVALID_PARAMETER, "NtXxx rejects NULL");
  ```
- Confirm build passes after adding tests.

### 7. Codex adversarial review (MANDATORY)
SSDT handlers are the kernel's attack surface -- every user-mode process can call them. Adversarial review is non-negotiable.

Dispatch to Codex plugin:
```bash
node "$HOME/.claude/plugins/marketplaces/openai-codex/plugins/codex/scripts/codex-companion.mjs" adversarial-review "<focus prompt>"
```

Focus prompt must cover:
- **Security:** Can crafted SYSCALL args corrupt kernel state, escalate privilege, or DoS?
- **Pointer validation:** Are all user-mode pointers ProbeForRead/Write'd before dereference?
- **NTSTATUS correctness:** Does every path return the correct status code?
- **SMP safety:** Any shared state accessed without synchronization?
- **Error paths:** Does every failure path clean up partial state?

### 8. Fix loop (max 3 rounds)
Apply `superpowers:receiving-code-review` discipline: do NOT blindly fix every Codex finding.
- **Verify technically first.** Read the flagged code. Is the finding correct?
- **If valid:** fix the root cause, rebuild.
- **If wrong:** reject with concrete technical evidence (not "I disagree").
- **If valid but out of scope:** accept with justification, note as follow-up.
Fix all valid Critical and High. Fix valid Medium unless explicitly accepted. Re-review via Codex focusing on previous findings.

### 9. Mark Done in the master table
For each successfully implemented, wired, and reviewed entry:
- In the master SSDT table (TODO-05 or TODO-12), change `| [ ] |` to `| [x] |` in the Done column.
- **3-tier classification (from audit-ssdt):**
  - `[x]` = COMPLETE: registered + ALL documented code paths implemented + error handling + not just STATUS_NOT_IMPLEMENTED on edge cases
  - `[/]` = PARTIAL: registered + some paths work but others return STATUS_NOT_IMPLEMENTED
  - `[ ]` = STUB/MISSING: not registered, or registered but no real logic
- **Only `[x]` for COMPLETE handlers.** A handler that returns STATUS_SUCCESS without doing real work is STUB, not COMPLETE.
- Update the Owner column: `T02 §N (file.c)` or `T12 §N (file.c)`.

### 10. Update the TODO section + cross-TODO loose ends
- In the referenced section (§N), mark each `- [ ]` checklist item `[x]` for the functions just implemented.
- If ALL functions in the section are done, update the Implementation Order table row to `[x]`.
- Update the OS Comparison table if applicable.
- **Cross-TODO sync:** check if other TODOs reference the newly-wired SSDT entries:
  - TODO-08 §9 PE import resolver (`s_kernel32_exports[]`, `s_ntdll_exports[]`) -- are the new functions listed?
  - TODO-23 EIF imports -- do any EIF test binaries reference these SSDT indices?
  - Other TODO sections that were blocked on these entries -- update their status.
- **Recommend running `/audit-ssdt`** after the range is complete to verify full consistency.

### 11. Commit and push
- Commit message format: `"kernel: nt -- wire N SSDT entries 0xNNNN-0xNNNN (<category name>)"`
  - Example: `"kernel: win32k -- wire 8 shadow SSDT entries 0x1020-0x1027 (GDI text and font)"`
- Stage: implementation files, headers, SSDT registration, PE export table updates, updated TODO file, test changes.
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
- [ ] Each function is functionally COMPLETE (not just STATUS_SUCCESS stub)
- [ ] PE export tables updated for functions with Win32 wrapper names
- [ ] Build passes: `=== BUILD OK ===`
- [ ] Codex adversarial review completed with all Critical/High fixed
- [ ] Done column in master table is `[x]` for COMPLETE entries only
- [ ] Section checklist items are `[x]` in the owning TODO
- [ ] Unit tests added/updated for the range
- [ ] Cross-TODO loose ends checked (PE exports, EIF imports, dependent TODOs)

## Guardrails

- Only implement functions listed in the provided range -- do not expand scope.
- If a function requires a prerequisite that doesn't exist (e.g., SSDT dispatcher, a specific Ob type), stop and report -- do not stub it with `return STATUS_NOT_IMPLEMENTED`.
- The goal is **real working code**, not stubs. Each function must do something meaningful.
- If an underlying primitive doesn't exist yet (e.g., `gfx_ellipse` for `NtGdiEllipse`), implement a minimal version or report the gap -- do not skip the entry.
- Do not mark Done `[x]` unless the function is COMPLETE (all documented paths, not just happy path). PARTIAL gets `[/]`.
- Follow freestanding kernel rules from CLAUDE.md: no stdlib, NASM assembly, Win32 native API surface.
- The Codex adversarial review (step 7) is MANDATORY. SSDT handlers are the kernel's attack surface -- no exceptions.
- Apply `receiving-code-review` discipline to Codex findings -- verify technically before fixing.
