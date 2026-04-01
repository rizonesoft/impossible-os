# TODO-10 — Exception Dispatch & SEH

> **Goal:** Replace the current "all CPU exceptions → `panic_screen()`" model with a proper Windows-style exception dispatch pipeline. That means: a captured `CONTEXT` record, an `EXCEPTION_RECORD` with fault address and exception code, a page-fault triage layer that separates recoverable faults from hard kills, a `KiUserExceptionDispatcher` path that delivers faults to user-mode SEH handlers via the TEB chain, x64 table-based unwind (`RtlVirtualUnwind`), Vectored Exception Handlers (VEH), an unhandled exception filter, kernel-mode safe probing (`ProbeForRead`/`ProbeForWrite`), and kernel-driver `__try`/`__except` support. Without this, every access violation — whether in a driver or a user app — crashes the whole OS rather than being caught and reported correctly.

> [!IMPORTANT]
> **Current state:** `vmm.c` has a `page_fault_handler` (vector 14) that chains to the swap and mmap handlers for kernel-mode recoverable faults, then falls through to `panic_screen()`. All other CPU exception vectors (0–13, 15–31) dispatch directly to `panic_screen()` via `idt.c`. No `EXCEPTION_RECORD` or `CONTEXT` is captured, no user-mode fault delivery path exists, and there is no kernel safe-probing API. The POSIX signal machinery (`signals` field in `struct task`) is wired but not exercised.

---

## Inputs

- `src/kernel/idt.c` — `isr_handler()`, `exception_names[]`, `idt_register_handler()`
- `src/kernel/irq.c` — vector registration infrastructure
- `src/kernel/mm/vmm.c` — existing `page_fault_handler` (ISR 14)
- `src/kernel/mm/mmap.c` — `mmap_handle_fault()`
- `src/kernel/mm/swap.c` — `swap_handle_fault()`
- `src/kernel/panic.c` — `panic_screen()` — remains the last resort
- `include/kernel/idt.h` — `interrupt_frame` struct (frame pointer, error code, rip, cs, rflags, rsp, ss)
- `src/kernel/sched/task.c` — `struct task`, `signals` field, `task_exec` ring-3 entry
- → XREF: `TODO-04-peb-teb-user-abi.md §1` (TEB `ExceptionList` pointer and struct layout)
- → XREF: `TODO-04-peb-teb-user-abi.md §7` (initial user stack frame — KiUserExceptionDispatcher target)
- → XREF: `TODO-05-native-api-layer.md §1` (NTSTATUS — `NtRaiseException`/`NtContinue` return values)
- → XREF: `TODO-06-irql-model-dpcs.md §3` (interrupt entry/exit IRQL — fault occurs at hardware IRQL)

## Outcome

- `include/kernel/except.h` — `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, NTSTATUS fault codes
- `src/kernel/except.c` — `context_from_frame()`, `frame_from_context()`, `except_init()`, fault ISR handlers (vectors 0, 4, 6, 11, 12, 13), `ki_raise_kernel_exception()`
- `src/kernel/mm/vmm.c` — `page_fault_handler()` triages #PF into user vs. kernel paths with EXCEPTION_RECORD capture
- `src/kernel/rtl/unwind.c` + `include/kernel/rtl/unwind.h` — `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`
- `src/kernel/rtl/seh.c` — `RtlDispatchException`, SEH SCOPE_TABLE walker, `UnhandledExceptionFilter`, `WerpReportFault()` stub
- `src/kernel/rtl/veh.c` — `RtlAddVectoredExceptionHandler`, `RtlRemoveVectoredExceptionHandler`, `ki_call_veh_list`
- `src/kernel/probe.c` + `include/kernel/probe.h` — `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`
- `src/kernel/compat/signal_compat.c` — `ki_deliver_compat_signal()` (Linux compat, `CONFIG_LINUX_COMPAT` guarded)
- `NtRaiseException` and `NtContinue` registered in SSDT (→ `TODO-05-native-api-layer.md §4`)

---

## Implementation Order

| # | Section | Type | Depends on | Overlap / Handoff |
|---|---------|------|------------|-------------------|
| 1 | EXCEPTION_RECORD, CONTEXT, EXCEPTION_POINTERS | 💎 | `interrupt_frame`, TODO-05 §1 | Types used by all sections below |
| 2 | #PF triage — user vs. kernel, COW, guard, stack growth | 💎 | §1, TODO-06 §3, vmm.c | Enables user fault delivery (§4) |
| 3 | General fault-to-exception mapping (#GP, #UD, #DE, #OF, #SS) | 💎 | §1 | Enables §4 |
| 4 | KiUserExceptionDispatcher — ring-3 exception delivery | 💎 | §1–3, TODO-04 §7 | Enables §6, §8 |
| 5 | x64 table-based unwind (.pdata, RtlVirtualUnwind) | 💎 | §1 | Enables §6, §7, §11 |
| 6 | SEH chain walk and `__try`/`__except` frame dispatch | 💎 | §4, §5, TODO-04 §1 | Enables §7 |
| 7 | RtlUnwindEx — unwind to target frame | 💎 | §5, §6 | Enables §11 |
| 8 | Vectored Exception Handlers (VEH) | 💎 | §4, TODO-03 §3 | User-mode ntdll registration |
| 9 | Unhandled exception filter and WER hook | 💎 | §6, §8 | Terminal fault handling |
| 10 | Kernel safe probing (ProbeForRead / ProbeForWrite) | ⭐ | §2 | Prerequisite for §11 |
| 11 | Kernel-mode `__try`/`__except` for drivers | 💎 | §5, §7, §10 | Driver exception safety |
| 12 | POSIX signal delivery from exceptions (Linux compat) | 💎 | §3, §4 | Linux compat layer handoff |

---

## 1. EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types `[Sonnet]`

**Prompt:** Define the three canonical Windows exception types in a new header `include/kernel/except.h`. `EXCEPTION_RECORD` holds the exception code (`EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_ILLEGAL_INSTRUCTION`, etc.), exception flags (`EXCEPTION_CONTINUABLE`), a nested-exception pointer, the fault address, and up to `EXCEPTION_MAXIMUM_PARAMETERS` (15) information parameters.
`CONTEXT` holds the full x86-64 register file (all GP registers, RIP, RFLAGS, CS/DS/ES/FS/GS/SS, XMM0–15, FPU control/status). `EXCEPTION_POINTERS` pairs the two. Add NTSTATUS codes for the common faults (`STATUS_ACCESS_VIOLATION 0xC0000005`, `STATUS_ILLEGAL_INSTRUCTION 0xC000001D`, `STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094`, etc.).
Add `context_from_frame(struct interrupt_frame *f, CONTEXT *ctx)` to populate a CONTEXT from the ISR frame.

> [!IMPORTANT]
> → XREF: `TODO-05 §1` — `NTSTATUS` type must be defined first.

- [ ] `include/kernel/except.h` — `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, exception codes
- [ ] `src/kernel/except.c` — `context_from_frame()`, `frame_from_context()`
- [ ] Add to `Makefile` and verify it compiles clean
- [ ] Commit: `"kernel: add EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS types"`

---

## 2. #PF Triage — User vs. Kernel, COW, Guard Pages, Stack Growth `[Opus]`

**Prompt:** Rewrite `page_fault_handler()` in `vmm.c` to triage before panicking.
Decode `CR2` (fault address) and the error code bits (P=present, W=write, U=user, I=fetch, PK=prot-key, SS=shadow-stack). Decision tree:
- **Kernel fault, not present** → existing swap/mmap chain (unchanged).
- **Kernel fault, protection violation** → kernel exception path — call `ki_raise_kernel_exception()` (§11) or `panic_screen()` if not inside a probed region (§10).
- **User fault, not present** → check if address falls in a guard page region (stack auto-grow) or an mmap region; map the page, retry, or deliver `STATUS_ACCESS_VIOLATION` via `KiUserExceptionDispatcher` (§4).
- **User fault, protection violation** → deliver `STATUS_ACCESS_VIOLATION` to user-mode.
- **User fetch fault** → deliver `STATUS_ACCESS_VIOLATION` with `ExceptionInformation[0]=8` (execute).
Extract the CONTEXT (§1) and fault address before deciding. Mark the old behavior (always panic) as the unreachable final clause.

> [!IMPORTANT]
> → XREF: `TODO-06 §3` — interrupt entry raises IRQL; fault handler runs at device IRQL.
> → XREF: `TODO-04 §5` — `task->peb` needed to find user-mode VMM range boundary.

- [ ] Decode error-code bits; distinguish user vs. kernel fault cleanly
- [ ] Guard page / stack-growth detection with configurable stack reserve (default 1 MiB)
- [ ] Build `EXCEPTION_RECORD` and `CONTEXT` on the kernel stack before dispatch
- [ ] Route to `ki_dispatch_user_exception()` stub (implemented fully in §4)
- [ ] Route to `ki_raise_kernel_exception()` stub (implemented fully in §11)
- [ ] Commit: `"mm: triage #PF into user/kernel paths; defer to exception dispatch"`

---

## 3. General Fault-to-Exception Mapping (#GP, #UD, #DE, #OF, #SS) `[Sonnet]`

**Prompt:** Add per-exception ISR handlers for the remaining CPU fault vectors that should not always panic. Register handlers via `idt_register_handler()` in a new `except_init()` called from `kernel_main`. Mapping:
- Vector 0 (`#DE`) → `STATUS_INTEGER_DIVIDE_BY_ZERO` if user-mode, panic if kernel.
- Vector 4 (`#OF`) → `STATUS_INTEGER_OVERFLOW` if user.
- Vector 6 (`#UD`) → `STATUS_ILLEGAL_INSTRUCTION` if user.
- Vector 11 (`#NP`) → `STATUS_ACCESS_VIOLATION` (segment not present) if user.
- Vector 12 (`#SS`) → `STATUS_STACK_OVERFLOW` if user.
- Vector 13 (`#GP`) → `STATUS_ACCESS_VIOLATION` if user; kernel = probe check first, then panic.
Kernel-mode faults for vectors 0, 4, 6 always call `panic_screen()` (no recovery).
Each handler builds an `EXCEPTION_RECORD` (§1) and routes to `ki_dispatch_user_exception()` (§4) or `ki_raise_kernel_exception()` (§11) based on CPL in the saved CS.

> [!IMPORTANT]
> → XREF: `TODO-06 §3` — ISR entry/exit must preserve IRQL contract.

- [ ] `src/kernel/except.c` — handlers for vectors 0, 4, 6, 11, 12, 13
- [ ] `except_init()` — register all handlers; call from kernel init phase 1 (→ XREF: `TODO-01 §3`)
- [ ] CPL check from saved `frame->cs & 3` to distinguish user vs. kernel origin
- [ ] Commit: `"kernel: map #GP/#UD/#DE/#OF/#SS to EXCEPTION_RECORD dispatch"`

---

## 4. KiUserExceptionDispatcher — Ring-3 Exception Delivery `[Opus]`

**Prompt:** Implement `ki_dispatch_user_exception(struct interrupt_frame *frame, EXCEPTION_RECORD *rec)`.
This function must:
1. Capture a full `CONTEXT` from `frame` (§1 `context_from_frame`).
2. Allocate space on the **user-mode stack** (`frame->rsp`) by decrementing it by `sizeof(CONTEXT) + sizeof(EXCEPTION_RECORD) + sizeof(EXCEPTION_POINTERS)`, then align to 16 bytes.
3. Copy `EXCEPTION_RECORD` and `CONTEXT` onto the user stack (kernel `memcpy`).
4. Patch `frame->rip` to `ntdll!KiUserExceptionDispatcher` (address stored in TEB or PEB).
5. Patch `frame->rsp` to the new user stack pointer.
6. Return from the ISR — the IRET will land in `KiUserExceptionDispatcher` with `EXCEPTION_POINTERS *` in RCX per the Microsoft x64 ABI.
Add `NtRaiseException(EXCEPTION_RECORD *, CONTEXT *, BOOLEAN)` and `NtContinue(CONTEXT *, BOOLEAN)` syscall stubs to `SSDT` (→ XREF: `TODO-05 §4`): `NtRaiseException` calls `ki_dispatch_user_exception` directly; `NtContinue` restores the `CONTEXT` onto the current thread frame and returns to user-mode.

> [!IMPORTANT]
> → XREF: `TODO-04 §7` — user stack frame layout must match for IRET to succeed.
> → XREF: `TODO-05 §4` — SSDT must be extended with NtRaiseException/NtContinue entries.

- [ ] `ki_dispatch_user_exception()` — push CONTEXT+EXCEPTION_RECORD on user stack, redirect IRET
- [ ] `NtRaiseException` SSDT entry
- [ ] `NtContinue` SSDT entry — restore CONTEXT, resume user-mode
- [ ] Handle misaligned or invalid user RSP gracefully (double-fault fallback to panic)
- [ ] Commit: `"kernel: implement KiUserExceptionDispatcher and NtRaiseException/NtContinue"`

---

## 5. x64 Table-Based Unwind (`.pdata`, `RtlVirtualUnwind`) `[Opus]`

**Prompt:** Implement the x64 PE32+ unwind machinery used by `RtlUnwindEx` and the SEH chain walker. Add `RtlLookupFunctionEntry(ULONGLONG pc, ULONGLONG *base, void *history)` — it scans the `.pdata` section of the module containing `pc` for the matching `RUNTIME_FUNCTION` entry. Implement `RtlVirtualUnwind(handler_type, image_base, pc, func_entry, ctx, handler_data, establisher_frame, ctx_ptrs)` — it applies the `UNWIND_INFO` opcodes (`UWOP_PUSH_NONVOL`, `UWOP_ALLOC_SMALL`, `UWOP_ALLOC_LARGE`, `UWOP_SET_FPREG`, `UWOP_SAVE_NONVOL`, `UWOP_SAVE_XMM128`) to a `CONTEXT`, returning the parent frame's PC and RBP. These two functions live in a new `src/kernel/rtl/unwind.c` / `include/kernel/rtl/unwind.h`.

> [!IMPORTANT]
> → XREF: `TODO-08 §5–6` — PE32+ loader must populate an in-memory `.pdata` section for loaded modules before unwind can work for user binaries.

- [ ] `include/kernel/rtl/unwind.h` — `RUNTIME_FUNCTION`, `UNWIND_INFO`, `UNWIND_CODE`, `SCOPE_TABLE`
- [ ] `src/kernel/rtl/unwind.c` — `RtlLookupFunctionEntry`, `RtlVirtualUnwind`
- [ ] Support all UWOP opcodes used by Clang/MSVC for x86-64
- [ ] Unit test: unwind a 3-frame kernel test stack and verify the recovered RIP chain
- [ ] Commit: `"rtl: implement RtlLookupFunctionEntry and RtlVirtualUnwind for x64 unwind"`

---

## 6. SEH Chain Walk and `__try`/`__except` Frame Dispatch `[Opus]`

**Prompt:** Implement the SEH dispatcher that `KiUserExceptionDispatcher` (§4) calls.
For x64, SEH is table-based (not frame-linked) — the unwind tables (§5) encode `__try` scope ranges and handler RVAs in a `SCOPE_TABLE`. The dispatcher:
1. Calls `RtlLookupFunctionEntry` to find the enclosing function.
2. Walks `SCOPE_TABLE` entries (each entry: `BeginAddress`, `EndAddress`, `HandlerAddress`, `JumpTarget`).
3. For a matching scope: calls the filter expression. If `EXCEPTION_EXECUTE_HANDLER`, calls `RtlUnwindEx` (§7) to unwind the stack to the handler frame, then jumps to `JumpTarget`.
4. If no handler found in the current frame, walks to the parent frame via `RtlVirtualUnwind`.
5. If no handler found anywhere: falls through to the unhandled exception path (§9).
Expose `RtlDispatchException(EXCEPTION_RECORD *, CONTEXT *)` — returns TRUE if handled.

> [!IMPORTANT]
> → XREF: `TODO-04 §1` — TEB `ExceptionList` is the base for legacy x86 chain; x64 uses `.pdata` tables but TEB still needed for `NtCurrentTeb()` in `__try` lowering.

- [ ] `src/kernel/rtl/seh.c` — `RtlDispatchException`, scope-table walker
- [ ] Filter expression invocation with correct calling convention
- [ ] Nested exception handling (`EXCEPTION_NESTED_CALL` flag)
- [ ] Commit: `"rtl: implement RtlDispatchException and SEH scope-table walker"`

---

## 7. RtlUnwindEx — Unwind to a Target Frame `[Opus]`

**Prompt:** Implement `RtlUnwindEx(target_frame, target_ip, exception_record, return_value, ctx, history)`.
It iterates from the current RSP upward via `RtlVirtualUnwind` (§5), calling each frame's `__finally` block (via the termination handler in the SCOPE_TABLE), until it reaches `target_frame`. At that point it restores `ctx` (with `return_value` in RAX and `target_ip` in RIP) and jumps to the target. It must correctly handle:
- `__finally` termination handlers (UWOP_SCOPE with `HandlerAddress == TERMINATION`).
- Continuation frames (frames with no handler — just unwind and continue).
- The global unwind flag (`EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND`) set on the exception record during this walk.

- [ ] `src/kernel/rtl/unwind.c` — `RtlUnwindEx`
- [ ] Termination handler invocation during unwind walk
- [ ] Correctly set/clear `EXCEPTION_UNWINDING` flag
- [ ] Commit: `"rtl: implement RtlUnwindEx with termination handler invocation"`

---

## 8. Vectored Exception Handlers (VEH) `[Sonnet]`

**Prompt:** Implement the Vectored Exception Handler list, called by `KiUserExceptionDispatcher` **before** the SEH chain walk (§6). The VEH list is per-process — a doubly-linked list of `VECTORED_EXCEPTION_ENTRY` nodes, protected by an `rwlock`. Add:
- `RtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)` — prepend (first=1) or append (first=0) to the list; return an opaque handle.
- `RtlRemoveVectoredExceptionHandler(PVOID handle)` — unlink by handle.
- `ki_call_veh_list(EXCEPTION_POINTERS *ptrs)` — walk the list; return `TRUE` if any handler returned `EXCEPTION_CONTINUE_EXECUTION`.
Store the VEH list head in `struct task` (process-level, not thread-level).

> [!IMPORTANT]
> → XREF: `TODO-03 §3` — VEH handles are not Win32 kernel handles; use a simple opaque pointer. No overlap with the Object Manager handle table.

- [ ] `include/kernel/except.h` — `VECTORED_EXCEPTION_ENTRY`, VEH list head in `struct task`
- [ ] `src/kernel/rtl/veh.c` — `RtlAddVectoredExceptionHandler`, `RtlRemoveVectoredExceptionHandler`, `ki_call_veh_list`
- [ ] Thread-safe list manipulation with `rwlock`
- [ ] Call `ki_call_veh_list` first inside `KiUserExceptionDispatcher` before SEH walk
- [ ] Commit: `"rtl: implement Vectored Exception Handler (VEH) list"`

---

## 9. Unhandled Exception Filter and WER Hook `[Sonnet]`

**Prompt:** Implement the terminal path reached when `RtlDispatchException` (§6) returns FALSE (no handler claimed the exception). The dispatch order is:
1. VEH continue-search list (§8 — Vectored Continue Handlers).
2. Per-process unhandled exception filter set by `SetUnhandledExceptionFilter` (stored in PEB).
3. Default filter — terminate the process with the exception code as exit status and emit a structured log entry (→ XREF: `TODO-02 §6`) with the full `EXCEPTION_RECORD` and first 8 frames of the stack trace.
Add `RtlSetUnhandledExceptionFilter(handler)` — stores handler in `PEB.UnhandledExceptionFilter`.
Add `UnhandledExceptionFilter(EXCEPTION_POINTERS *)` — calls the per-process filter or default.
Add a WER (Windows Error Reporting) stub: `WerpReportFault()` calls into a future `werfault.exe` process via a named pipe (leave as a no-op stub for now, log to serial).

> [!IMPORTANT]
> → XREF: `TODO-04 §2` — `PEB.UnhandledExceptionFilter` field must be reserved in the PEB struct.

- [ ] `src/kernel/rtl/seh.c` — `UnhandledExceptionFilter`, default fatal handler
- [ ] `PEB.UnhandledExceptionFilter` field (→ XREF: `TODO-04 §2`)
- [ ] Structured crash log: exception code, fault address, top-8 frames via RtlCaptureStackBackTrace
- [ ] `WerpReportFault()` stub — serial log only for now
- [ ] Process termination with exception code as exit status
- [ ] Commit: `"rtl: implement unhandled exception filter, WER stub, and crash log"`

---

## 10. Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`) `[Sonnet]`

**Prompt:** Add kernel-mode safe pointer validation to prevent ring-3 pointers from crashing the kernel when accessed by syscall handlers. Implement:
- `ProbeForRead(addr, length, alignment)` — verify `[addr, addr+length)` is in user address space (below `USER_SPACE_LIMIT`) and aligned. Raise `STATUS_ACCESS_VIOLATION` (via `ki_raise_kernel_exception`) if not.
- `ProbeForWrite(addr, length, alignment)` — same check, plus touch the first byte of each page to force a present+writable mapping, catching write-protected pages.
- `try_copy_from_user(dst, src, n)` / `try_copy_to_user(dst, src, n)` — equivalent of Linux `copy_from_user`/`copy_to_user`. Uses a per-CPU `safe_return_rip` slot in CPU-local storage; the #PF handler checks it (§2) and redirects to the safe-return path if a fault occurs inside a guarded copy.

> [!IMPORTANT]
> → XREF: `TODO-06 §2` — the per-CPU safe_return_rip slot is CPU-local data, co-located with the IRQL tracking fields.

- [ ] `include/kernel/probe.h` — `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`
- [ ] `src/kernel/probe.c` — implementation; `safe_return_rip` slot in CPU-local area
- [ ] Update #PF triage (§2) to check `safe_return_rip` and redirect on kernel probe faults
- [ ] Apply `ProbeForRead`/`ProbeForWrite` to all syscall handlers that dereference user pointers
- [ ] Commit: `"kernel: add ProbeForRead/Write and try_copy_{from,to}_user safe probing"`

---

## 11. Kernel-Mode `__try`/`__except` for Drivers `[Opus]`

**Prompt:** Enable kernel-mode structured exception handling so drivers can wrap dangerous operations (MMIO access, DMA buffer reads) in `__try`/`__except`. The mechanism differs from user-mode: there is no user stack to push onto. Instead:
1. Add `KI_EXCEPTION_REGISTRATION` — a per-thread (kernel stack) record pushed by `__try` lowering code at the head of the thread's kernel stack frame.
2. `ki_raise_kernel_exception(EXCEPTION_RECORD *, CONTEXT *)` — walks the kernel-mode exception chain (stored in the per-CPU `current_thread->kernel_exception_list`), calls filter expressions, and invokes `RtlUnwindEx` (§7) for matching handlers.
3. Patch the kernel's `.pdata` section to include `UNWIND_INFO` for critical paths (requires linker script changes to emit `.pdata` for `clang-19`).
4. Guard against re-entrancy: if a kernel exception occurs inside a kernel exception handler, escalate directly to `KeBugCheckEx` (panic with structured code).

> [!IMPORTANT]
> → XREF: `TODO-06 §3` — kernel `__try` must only be used at `PASSIVE_LEVEL` or `APC_LEVEL`; add `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` assertion at the start of `ki_raise_kernel_exception`.

- [ ] `include/kernel/except.h` — `KI_EXCEPTION_REGISTRATION`, kernel exception chain head in `struct task`
- [ ] `src/kernel/except.c` — `ki_raise_kernel_exception()`
- [ ] Linker script: ensure kernel code sections emit `.pdata` with `-fexceptions` (or manual stubs)
- [ ] Re-entrancy guard: nested kernel exception → `KeBugCheckEx`
- [ ] Wrap one existing dangerous driver operation (e.g., AHCI MMIO read) as a smoke test
- [ ] Commit: `"kernel: implement kernel-mode __try/__except via KI_EXCEPTION_REGISTRATION"`

---

## 12. POSIX Signal Delivery from Exceptions (Linux Compat) `[Sonnet]`

**Prompt:** Map hardware faults to POSIX signals for processes running under the Linux compatibility layer. After the VEH list and SEH dispatch both decline the exception (neither handled it), check the current task's `compat_mode` flag. If set:
- `STATUS_ACCESS_VIOLATION` → `SIGSEGV`
- `STATUS_ILLEGAL_INSTRUCTION` → `SIGILL`
- `STATUS_INTEGER_DIVIDE_BY_ZERO` → `SIGFPE`
- `STATUS_STACK_OVERFLOW` → `SIGSEGV` (with `si_code = SEGV_ACCERR`)
- `STATUS_BREAKPOINT` → `SIGTRAP`
Deliver via the existing `task->signals` mechanism. If the signal has no handler (default disposition), terminate the task with an appropriate `NTSTATUS` exit code.
This section is gated on the Linux compat layer existing — stub it out with a compile-time flag `CONFIG_LINUX_COMPAT` for now.

> [!IMPORTANT]
> → XREF: `TODO-09 §6` (process capabilities) — compat mode is a process flag.
> Delivery path hooks into §9 (unhandled exception filter) as a pre-termination step.

- [ ] `include/kernel/compat.h` — `CONFIG_LINUX_COMPAT` guard, fault-to-signal table
- [ ] `src/kernel/compat/signal_compat.c` — `ki_deliver_compat_signal()` — fault-to-signal translation
- [ ] Hook into §9 (unhandled exception filter) before process termination
- [ ] `compat_mode` flag in `struct task` (or reuse `capabilities` field bit 63 as compat bit)
- [ ] Commit: `"kernel: map hardware faults to POSIX signals for Linux compat processes"`

---

## OS Comparison


| ⭐ | Feature                                      | Win11                 | Linux              | Impossible OS          |
|----|----------------------------------------------|-----------------------|--------------------|------------------------|
| 💎 | EXCEPTION_RECORD / CONTEXT types             | ✅ ntdll              | ❌                 | ⬜                     |
| 💎 | #PF user/kernel triage                       | ✅                    | ✅                 | ⚠️ §2 — Kernel-only () |
| 💎 | KiUserExceptionDispatcher                    | ✅                    | ❌                 | ⬜                     |
| 💎 | x64 table-based unwind                       | ✅ UNWIND_INFO        | ✅ .eh_frame/DWARF | ⬜                     |
| 💎 | SEH chain walk                               | ✅                    | ❌                 | ⬜                     |
| 💎 | RtlUnwindEx with `__finally` dispatch        | ✅                    | ❌                 | ⬜                     |
| 💎 | Vectored Exception Handlers                  | ✅                    | ❌                 | ⬜                     |
| 💎 | Unhandled exception filter                   | ✅ WER                | ✅ core dump       | ⬜                     |
| ⭐ | Kernel safe probing with per-CPU safe-return | ✅ ProbeForRead/Write | ✅ copy_from_user  | ⬜ (IRQL-aware)        |
| 💎 | Kernel `__try`/`__except` for drivers        | ✅                    | ❌                 | ⬜                     |
| 💎 | POSIX signal delivery from faults            | ❌                    | ✅                 | ⬜ (compat)            |

Impossible OS distinguishes itself with: IRQL-aware kernel safe probing (the per-CPU `safe_return_rip` slot is co-located with the IRQL tracking fields so the #PF handler checks probe state in a single conditional with no extra memory load), structured crash telemetry feeding the JSON log system (TODO-02), and SEH delivery that validates the user stack pointer against the TEB stack bounds before touching it — preventing an overwritten RSP from escalating a user crash into a kernel panic.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_except()` (XREF: `docs/infrastructure/kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_except.c` with:
  - `context_from_frame` populates all GP registers from an `interrupt_frame` correctly
  - `frame_from_context` restores registers back; round-trip preserves RIP, RSP, RFLAGS
  - `EXCEPTION_RECORD` for `STATUS_ACCESS_VIOLATION` has correct code and fault address in `ExceptionInformation[1]`
  - `EXCEPTION_RECORD` for `STATUS_INTEGER_DIVIDE_BY_ZERO` has correct code and `EXCEPTION_CONTINUABLE` flag
  - `ProbeForRead` on user-space address (below `USER_SPACE_LIMIT`) with correct alignment succeeds
  - `ProbeForRead` on kernel address raises `STATUS_ACCESS_VIOLATION` (does not panic)
  - `ProbeForWrite` on kernel address raises `STATUS_ACCESS_VIOLATION`
  - `ProbeForRead` with misaligned address and alignment > 1 raises `STATUS_DATATYPE_MISALIGNMENT`
  - `try_copy_from_user` from valid mapped user page succeeds; data matches
  - `try_copy_from_user` from unmapped address returns error (does not panic)
  - `try_copy_to_user` to valid mapped user page succeeds; data readable back
  - `RtlVirtualUnwind` on a known 3-frame kernel stack recovers correct RIP chain
  - `RtlLookupFunctionEntry` returns non-NULL for a known function in `.pdata`; returns NULL for address outside any module
  - VEH registration: `RtlAddVectoredExceptionHandler` returns non-NULL handle
  - VEH removal: `RtlRemoveVectoredExceptionHandler` with valid handle succeeds
  - Kernel `__try`/`__except` around a guarded region: exception handler fires and kernel continues
- [ ] Register in `test_runner_init()`: `test_register_except()`
- [ ] Commit: `"test: add exception dispatch and SEH test suite"`

## Verification

- [ ] Trigger a deliberate user-mode `NULL` dereference; verify the process terminates with `STATUS_ACCESS_VIOLATION` and a log entry — not a kernel panic.
- [ ] Trigger a user-mode divide-by-zero; verify `STATUS_INTEGER_DIVIDE_BY_ZERO`.
- [ ] Register a VEH that continues execution after patching EIP past the fault; verify the process survives.
- [ ] Register a `__try`/`__except` block around a `NULL` dereference in a test driver; verify the handler fires and the kernel keeps running.
- [ ] Call `ProbeForRead` with a kernel address from a syscall handler; verify it raises `STATUS_ACCESS_VIOLATION` without panicking.
- [ ] Verify `RtlVirtualUnwind` correctly unwinds a 4-frame kernel test stack to the expected RIP values.
- [ ] Commit: `"kernel/rtl: exception dispatch, SEH, VEH, safe probing, and kernel __try/__except complete"`
