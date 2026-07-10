---
schema_version: 1
id: exception-dispatch-seh
domain: 02-kernel-core
status: active
title: "TODO-23 -- Exception Dispatch & SEH"
---

# TODO-23 -- Exception Dispatch & SEH

> **Goal:** Replace the current "all CPU exceptions → `panic_screen()`" model with a proper Windows-style exception dispatch pipeline. That means: a captured `CONTEXT` record, an `EXCEPTION_RECORD` with fault address and exception code, a page-fault triage layer that separates recoverable faults from hard kills, debugger first-chance/second-chance notification, a `KiUserExceptionDispatcher` path that delivers faults to user-mode SEH handlers via the TEB chain, x64 table-based unwind (`RtlVirtualUnwind`), kernel-mode stack walking (`RtlCaptureStackBackTrace`), Vectored Exception Handlers (VEH), Vectored Continue Handlers (VCH), `__C_specific_handler` for SEH scope-table dispatch, an unhandled exception filter, kernel-mode safe probing (`ProbeForRead`/`ProbeForWrite`), kernel-driver `__try`/`__except` support, POSIX signal delivery for Linux-compat processes (including `sigaltstack`), and exception dispatch telemetry. Without this, every access violation -- whether in a driver or a user app -- crashes the whole OS rather than being caught and reported correctly.

> [!IMPORTANT]
> **Current state:** `vmm.c` has a `page_fault_handler` (vector 14) that chains to the swap and mmap handlers for kernel-mode recoverable faults, then falls through to `panic_screen()`. All other CPU exception vectors (0–13, 15–31) dispatch directly to `panic_screen()` via `idt.c`. No `EXCEPTION_RECORD` or `CONTEXT` is captured, no user-mode fault delivery path exists, and there is no kernel safe-probing API. The POSIX signal machinery (`signals` field in `struct task`) is wired but not exercised.

---

## Inputs

- `src/kernel/idt.c` -- `isr_handler()`, `exception_names[]`, `idt_register_handler()`
- `src/kernel/irq.c` -- vector registration infrastructure
- `src/kernel/mm/vmm.c` -- existing `page_fault_handler` (ISR 14)
- `src/kernel/mm/mmap.c` -- `mmap_handle_fault()`
- `src/kernel/mm/swap.c` -- `swap_handle_fault()`
- `src/kernel/panic.c` -- `panic_screen()` -- remains the last resort
- `include/kernel/idt.h` -- `interrupt_frame` struct (frame pointer, error code, rip, cs, rflags, rsp, ss)
- `src/kernel/sched/task.c` -- `struct task`, `signals` field, `task_exec` ring-3 entry
- → XREF: `TODO-11-peb-teb-user-abi.md §6` (TEB `ExceptionList` pointer and struct layout)
- → XREF: `TODO-14-registry-completion.md §4.2` -- NtXxx registry syscalls use `ProbeForRead` / `ProbeForWrite` (§13) for user-mode pointer validation
- → XREF: `TODO-11-peb-teb-user-abi.md §2` (initial user stack frame -- KiUserExceptionDispatcher target)
- → XREF: `TODO-12-native-api-ssdt.md §1` (NTSTATUS -- `NtRaiseException`/`NtContinue` return values)
- → XREF: `TODO-07-irql-model-dpcs.md §3` (interrupt entry/exit IRQL -- fault occurs at hardware IRQL)
- → XREF: `TODO-07-irql-model-dpcs.md §12` -- KiDeliverApc sets up user-mode trap frame for user APC delivery; KiUserApcDispatcher parallels KiUserExceptionDispatcher (§5) and should be implemented alongside it
- → XREF: `TODO-10-kernel-security-hardening.md §7` -- CET shadow stack; §3 of this TODO handles the resulting `#CP` exception (vector 21)
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md §4` -- #DB/#BP exception handlers; §3–4 of this TODO integrate with KD for first/second-chance notification
- → XREF: `TODO-04-system-logging.md §8` -- JSON structured event log; §16 of this TODO emits exception dispatch telemetry
- → XREF: `10-platform-services/TODO-10-linux-compat.md §9` -- `rt_sigaction` handler registration for Linux compat signal delivery (§15)

## Outcome

- `include/kernel/except.h` -- `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, NTSTATUS fault codes, `KI_EXCEPTION_REGISTRATION`, VEH/VCH list heads
- `src/kernel/except.c` -- `context_from_frame()`, `frame_from_context()`, `except_init()`, fault ISR handlers (vectors 0, 1, 3, 4, 6, 11, 12, 13, 21), `ki_dispatch_exception()` (master dispatcher with debugger first/second-chance), `ki_raise_kernel_exception()`, `except_log_dispatch()` (telemetry)
- `src/kernel/mm/vmm.c` -- `page_fault_handler()` triages #PF into user vs. kernel paths with EXCEPTION_RECORD capture
- `src/kernel/rtl/unwind.c` + `include/kernel/rtl/unwind.h` -- `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`, `RtlCaptureStackBackTrace`, `RtlWalkFrameChain`
- `src/kernel/rtl/seh.c` -- `RtlDispatchException`, `__C_specific_handler`, SEH SCOPE_TABLE walker, `UnhandledExceptionFilter`, `WerpReportFault()` stub
- `src/kernel/rtl/veh.c` -- `RtlAddVectoredExceptionHandler`, `RtlRemoveVectoredExceptionHandler`, `ki_call_veh_list`, `AddVectoredContinueHandler`, `RemoveVectoredContinueHandler`, `ki_call_vch_list`
- `src/kernel/probe.c` + `include/kernel/probe.h` -- `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`
- `src/kernel/compat/signal_compat.c` -- `ki_deliver_compat_signal()` (Linux compat, `CONFIG_LINUX_COMPAT` guarded), `sigaltstack` support
- `NtRaiseException` and `NtContinue` registered in SSDT (→ `TODO-12-native-api-ssdt.md §5`)
- `KiDebugRoutine` function pointer and `DbgkForwardException()` stub for debugger integration (→ `TODO-29`)

---

## Implementation Order

| ⭐   | Order | Deliverable                                              | Depends On                 | Status |
| --- | :---: | -------------------------------------------------------- | -------------------------- | :----: |
| 💎   |   1   | EXCEPTION_RECORD, CONTEXT, EXCEPTION_POINTERS            | TODO-12 §1                 |  [ ]   |
| 💎   |   2   | #PF triage -- user vs. kernel, COW, guard, stack growth  | §1, TODO-17 §3             |  [ ]   |
| 💎   |   3   | Fault-to-exception mapping (#DE/#DB/#BP/#UD/#GP/#SS/#CP) | §1, TODO-23 §9, TODO-29 §4 |  [ ]   |
| 💎   |   4   | Debugger first-chance / second-chance notification       | §1–§3, TODO-29 §4          |  [ ]   |
| 💎   |   5   | KiUserExceptionDispatcher -- ring-3 delivery             | §4, TODO-11 §2             |  [ ]   |
| 💎   |   6   | x64 table-based unwind (.pdata, RtlVirtualUnwind)        | §1                         |  [ ]   |
| 💎   |   7   | Stack walking (RtlCaptureStackBackTrace)                 | §6, TODO-17 §3             |  [ ]   |
| 💎   |   8   | SEH chain walk + `__C_specific_handler`                  | §5, §6, TODO-11 §6         |  [ ]   |
| 💎   |   9   | RtlUnwindEx -- unwind to target frame                    | §6, §8                     |  [ ]   |
| 💎   |  10   | Vectored Exception Handlers (VEH)                        | §4, TODO-05 §2             |  [ ]   |
| 💎   |  11   | Vectored Continue Handlers (VCH)                         | §4, §10                    |  [ ]   |
| 💎   |  12   | Unhandled exception filter + WER hook                    | §7, §8, §10                |  [ ]   |
| ⭐   |  13   | Kernel safe probing (ProbeForRead/Write)                 | §2                         |  [ ]   |
| 💎   |  14   | Kernel-mode `__try`/`__except` for drivers               | §6, §9, §13                |  [ ]   |
| 💎   |  15   | POSIX signal delivery from exceptions (Linux compat)     | §3, §5, D10T10 §8          |  [ ]   |
| ⭐   |  16   | Exception dispatch telemetry                             | §4, TODO-11 §1             |  [ ]   |

> 💎 = parity -- Windows implements this feature; Impossible OS must match.
> ⭐ = exclusive -- not present in either Windows or Linux at the kernel level.

---

## 1. EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types

**Prompt:** Define the three canonical Windows exception types in a new header `include/kernel/except.h`. `EXCEPTION_RECORD` holds the exception code (`EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_ILLEGAL_INSTRUCTION`, etc.), exception flags (`EXCEPTION_CONTINUABLE`), a nested-exception pointer, the fault address, and up to `EXCEPTION_MAXIMUM_PARAMETERS` (15) information parameters.
`CONTEXT` holds the full x86-64 register file (all GP registers, RIP, RFLAGS, CS/DS/ES/FS/GS/SS, XMM0–15, FPU control/status). `EXCEPTION_POINTERS` pairs the two. Add NTSTATUS codes for the common faults (`STATUS_ACCESS_VIOLATION 0xC0000005`, `STATUS_ILLEGAL_INSTRUCTION 0xC000001D`, `STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094`, etc.).
Add `context_from_frame(struct interrupt_frame *f, CONTEXT *ctx)` to populate a CONTEXT from the ISR frame.

> [!IMPORTANT]
> → XREF: `TODO-12 §1` -- `NTSTATUS` type must be defined first.

> [!WARNING]
> A temporary `typedef int32_t NTSTATUS;` already exists in `include/kernel/uefi_vars.h`. Reuse that typedef in `include/kernel/except.h` (or move it to a shared `include/kernel/ntstatus.h` if the include dependency is awkward). Add the exception-specific status codes (`STATUS_ACCESS_VIOLATION 0xC0000005`, etc.) alongside. When TODO-12 §1 lands with the canonical NTSTATUS and full code table, consolidate into a single header.

- [ ] `include/kernel/except.h` -- `typedef int32_t NTSTATUS;` (local minimal; removed when TODO-12 §1 lands)
- [ ] `include/kernel/except.h` -- `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, exception codes
- [ ] `src/kernel/except.c` -- `context_from_frame()`, `frame_from_context()`
- [ ] Add to `Makefile` and verify it compiles clean

**Test checkpoint:** `sizeof(EXCEPTION_RECORD)` matches Windows ABI (152 bytes on x64). `sizeof(CONTEXT)` includes all GP registers + XMM0–15. `context_from_frame` round-trips correctly: populate from a test `interrupt_frame`, convert back via `frame_from_context`, compare -- all fields match. Serial log: `"except: types compiled, CONTEXT size=<N>"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: add EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS types"`

---

## 2. #PF Triage -- User vs. Kernel, COW, Guard Pages, Stack Growth

**Prompt:** Rewrite `page_fault_handler()` in `vmm.c` to triage before panicking.
Decode `CR2` (fault address) and the error code bits (P=present, W=write, U=user, I=fetch, PK=prot-key, SS=shadow-stack). Decision tree:
- **Kernel fault, not present** → existing swap/mmap chain (unchanged).
- **Kernel fault, protection violation** → kernel exception path -- call `ki_raise_kernel_exception()` (§14) or `panic_screen()` if not inside a probed region (§13).
- **User fault, not present** → check if address falls in a guard page region (stack auto-grow) or an mmap region; map the page, retry, or deliver `STATUS_ACCESS_VIOLATION` via `KiUserExceptionDispatcher` (§5).
- **User fault, protection violation** → deliver `STATUS_ACCESS_VIOLATION` to user-mode.
- **User fetch fault** → deliver `STATUS_ACCESS_VIOLATION` with `ExceptionInformation[0]=8` (execute).
Extract the CONTEXT (§1) and fault address before deciding. Mark the old behavior (always panic) as the unreachable final clause.

> [!IMPORTANT]
> → XREF: `TODO-17 §3` -- interrupt entry raises IRQL; fault handler runs at device IRQL.
> → XREF: `TODO-11 §9` -- `task->peb` needed to find user-mode VMM range boundary.
> → XREF: `D03 T04 §4` -- lazy mapped-file and swap-backed page-in live in the pager TODO; this section routes eligible not-present faults there rather than re-implementing pager policy in the exception layer.

- [ ] Decode error-code bits; distinguish user vs. kernel fault cleanly
- [ ] Guard page / stack-growth detection with configurable stack reserve (default 1 MiB)
- [ ] Build `EXCEPTION_RECORD` and `CONTEXT` on the kernel stack before dispatch
- [ ] Route to `ki_dispatch_exception()` (§4) for user-mode faults
- [ ] Route to `ki_raise_kernel_exception()` stub (implemented fully in §14)

**Test checkpoint:** Trigger a user-mode NULL dereference -- serial log shows `"pf: user fault at 0x0, code=0x<ec>"` and routes to `ki_dispatch_exception()` (stub returns to `panic_screen()` until §4 lands). Trigger a kernel-mode swap fault -- existing swap/mmap chain still handles it correctly. Guard page fault at stack bottom auto-grows the stack. `POST16(0xDE20)` on #PF entry, `POST16(0xDE21)` after triage decision. If crash: check last POST -- 0xDE20 = never entered triage, 0xDE21 = triage completed but dispatch failed. Test on: QEMU WHPX + TCG. Verify on bare metal -- #PF error code bits may differ.

- [ ] Commit: `"mm: triage #PF into user/kernel paths; defer to exception dispatch"`

---

## 3. General Fault-to-Exception Mapping (#DE, #DB, #BP, #OF, #UD, #NP, #SS, #GP, #CP)

**Prompt:** Add per-exception ISR handlers for the CPU fault vectors that should not always panic. Register handlers via `idt_register_handler()` in a new `except_init()` called from `kernel_main`. Mapping:
- Vector 0 (`#DE`) → `STATUS_INTEGER_DIVIDE_BY_ZERO` if user-mode, panic if kernel.
- Vector 1 (`#DB`) → `STATUS_SINGLE_STEP`. Route through `ki_dispatch_exception()` (§4) which notifies the debugger first-chance; if no debugger or debugger declines, deliver to user-mode as `STATUS_SINGLE_STEP` via `KiUserExceptionDispatcher` (§5). Kernel-mode `#DB` routes to KD (→ XREF: `TODO-29 §4`).
- Vector 3 (`#BP`) → `STATUS_BREAKPOINT`. Adjust `frame->rip -= 1` (INT3 is 1 byte). Route through `ki_dispatch_exception()` (§4) for debugger first-chance; if unhandled, deliver to user-mode. Kernel-mode `#BP` routes to KD (→ XREF: `TODO-29 §4`).
- Vector 4 (`#OF`) → `STATUS_INTEGER_OVERFLOW` if user.
- Vector 6 (`#UD`) → `STATUS_ILLEGAL_INSTRUCTION` if user.
- Vector 11 (`#NP`) → `STATUS_ACCESS_VIOLATION` (segment not present) if user.
- Vector 12 (`#SS`) → `STATUS_STACK_OVERFLOW` if user.
- Vector 13 (`#GP`) → `STATUS_ACCESS_VIOLATION` if user; kernel = probe check first, then panic.
- Vector 21 (`#CP`) → `STATUS_CONTROL_STACK_VIOLATION` (CET shadow-stack mismatch). User-mode: deliver via `KiUserExceptionDispatcher` (§5). Kernel-mode: `KeBugCheckEx(KERNEL_CET_SHADOW_STACK_VIOLATION)`. Only register this handler if `cpu_has(CPU_FEATURE_CET_SS)` returns true (→ XREF: `TODO-23 §9`).
Kernel-mode faults for vectors 0, 4, 6 always call `panic_screen()` (no recovery).
Each handler builds an `EXCEPTION_RECORD` (§1) and routes through `ki_dispatch_exception()` (§4) which orchestrates debugger notification and handler dispatch based on CPL in the saved CS.

> [!IMPORTANT]
> → XREF: `TODO-17 §3` -- ISR entry/exit must preserve IRQL contract.
> → XREF: `TODO-29 §4` -- #DB/#BP handlers must coexist with KD. If KD is attached, `ki_dispatch_exception()` calls `KiDebugRoutine` first-chance. If KD is not present or declines, dispatch continues to VEH/SEH. TODO-29 §4 registers raw handlers; when TODO-23 §3 lands, those handlers must be adapted to call through `ki_dispatch_exception()` instead.
> → XREF: `TODO-23 §9` -- #CP (vector 21) is generated by CET shadow stack violations. Only register the handler if `cpu_has(CPU_FEATURE_CET_SS)` returns true.

- [ ] `src/kernel/except.c` -- handlers for vectors 0, 1, 3, 4, 6, 11, 12, 13
- [ ] `src/kernel/except.c` -- conditional handler for vector 21 (`#CP`) if CET is supported
- [ ] `except_init()` -- register all handlers; call from kernel init phase 1 (→ XREF: `TODO-01 §3`)
- [ ] CPL check from saved `frame->cs & 3` to distinguish user vs. kernel origin

**Test checkpoint:** Trigger user-mode `ud2` -- serial log shows `"except: #UD at 0x<rip>, STATUS_ILLEGAL_INSTRUCTION"`. Trigger user-mode `int3` -- serial log shows `"except: #BP at 0x<rip>, STATUS_BREAKPOINT"`. Kernel-mode `div 0` → `panic_screen()` with `STATUS_INTEGER_DIVIDE_BY_ZERO`. If CET supported: `#CP` handler registered (check `except_init` log). `POST16(0xDE30)` before `except_init()`, `POST16(0xDE31)` after all handlers registered. If crash at 0xDE30: `except_init` never entered. Test on: QEMU WHPX + TCG. Verify on bare metal.

- [ ] Commit: `"kernel: map CPU exceptions to EXCEPTION_RECORD dispatch (#DE/#DB/#BP/#GP/#UD/#SS/#CP)"`

---

## 4. Debugger First-Chance / Second-Chance Notification

**Prompt:** Implement the master exception dispatcher `ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx, KPROCESSOR_MODE mode, BOOLEAN first_chance)` that orchestrates the full Windows NT exception dispatch sequence. On Windows, the dispatch order is:
1. **Debugger first-chance notification** -- if the process has a debug port (or kernel debugger is attached for kernel-mode exceptions), send the exception to the debugger. If the debugger handles it (continues execution), stop.
2. **VEH list** (§10, user-mode only) -- walk `RtlAddVectoredExceptionHandler` list.
3. **SEH/frame-based dispatch** (§8, user-mode; §14, kernel-mode).
4. **Debugger second-chance notification** -- if no handler claimed the exception, notify the debugger again. This is the debugger's last chance to handle it.
5. **VCH list** (§11, user-mode only) -- walk `AddVectoredContinueHandler` list (only if exception was handled by SEH).
6. **Unhandled exception filter** (§12) -- terminal handling.

For kernel-mode: if no handler → `KeBugCheckEx` with the exception code.

> [!IMPORTANT]
> → XREF: `TODO-29 §4` -- KD is the kernel debugger; `KiDebugRoutine` is the function pointer that `ki_dispatch_exception` calls for kernel-mode first/second-chance. If KD is not attached, `KiDebugRoutine` is NULL and the notification is skipped.
> → XREF: `TODO-29 §13` -- User-mode debug port is `NtDebugActiveProcess`; `DbgkForwardException()` sends the exception to the debug port. Stub `DbgkForwardException` to return FALSE until TODO-29 §13 lands.

> [!WARNING]
> `KPROCESSOR_MODE` does not exist yet. Define locally in `include/kernel/except.h`: `typedef enum { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;`. Canonical definition moves to a shared NT types header when TODO-12 matures.
> `KeBugCheckEx` is implemented in `src/kernel/panic.c` per `TODO-27-crash-dump-generation.md` §1. When `except.c` lands, include `panic.h` (or a forward declaration) and call the shared `KeBugCheckEx` entry point for terminal kernel-mode faults. Do not add a second implementation in `except.c`.

- [ ] `include/kernel/except.h` -- `typedef enum { KernelMode, UserMode } KPROCESSOR_MODE;` (local; moved to shared header later)
- [ ] `src/kernel/except.c`: include `panic.h`; call `KeBugCheckEx(...)` for terminal kernel-mode dispatch (no duplicate body)
- [ ] `src/kernel/except.c` -- `ki_dispatch_exception(rec, ctx, mode, first_chance)` -- master dispatcher
- [ ] `KiDebugRoutine` function pointer -- defaults to NULL (no debugger); set by KD attach (→ XREF: TODO-29 §4)
- [ ] `DbgkForwardException(rec, ctx, first_chance)` -- stub returning FALSE; sends exception to user-mode debug port when TODO-29 §13 lands
- [ ] For user-mode: orchestrate VEH → SEH → second-chance → VCH → unhandled filter
- [ ] For kernel-mode: call `KiDebugRoutine` first-chance → kernel SEH (§14) → `KiDebugRoutine` second-chance → `KeBugCheckEx`

**Test checkpoint:** With `KiDebugRoutine == NULL`: user-mode access violation routes through VEH → SEH → unhandled filter path (stubs return FALSE until §8–§12 land). With `KiDebugRoutine` set to a test function: first-chance notification fires before VEH. Kernel-mode unhandled exception calls `KeBugCheckEx`. Serial log shows `"except: dispatch user exception code=0x<code>, first_chance=1"`. `POST16(0xDE40)` on dispatcher entry, `POST16(0xDE41)` after dispatch decision. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: implement ki_dispatch_exception with debugger first/second-chance notification"`

---

## 5. KiUserExceptionDispatcher -- Ring-3 Exception Delivery

**Prompt:** Implement `ki_deliver_user_exception(struct interrupt_frame *frame, EXCEPTION_RECORD *rec)` -- called by `ki_dispatch_exception()` (§4) when a user-mode exception is ready for delivery.
This function must:
1. Capture a full `CONTEXT` from `frame` (§1 `context_from_frame`).
2. Allocate space on the **user-mode stack** (`frame->rsp`) by decrementing it by `sizeof(CONTEXT) + sizeof(EXCEPTION_RECORD) + sizeof(EXCEPTION_POINTERS)`, then align to 16 bytes.
3. Copy `EXCEPTION_RECORD` and `CONTEXT` onto the user stack (kernel `memcpy`).
4. Patch `frame->rip` to `ntdll!KiUserExceptionDispatcher` (address stored in TEB or PEB).
5. Patch `frame->rsp` to the new user stack pointer.
6. Return from the ISR -- the IRET will land in `KiUserExceptionDispatcher` with `EXCEPTION_POINTERS *` in RCX per the Microsoft x64 ABI.
Add `NtRaiseException(EXCEPTION_RECORD *, CONTEXT *, BOOLEAN)` and `NtContinue(CONTEXT *, BOOLEAN)` syscall stubs to `SSDT` (→ XREF: `TODO-12 §5`): `NtRaiseException` calls `ki_dispatch_exception` (§5) directly; `NtContinue` restores the `CONTEXT` onto the current thread frame and returns to user-mode.

> [!IMPORTANT]
> → XREF: `TODO-11 §2` -- user stack frame layout must match for IRET to succeed.
> → XREF: `TODO-12 §5` -- SSDT must be extended with NtRaiseException/NtContinue entries.

- [ ] `ki_deliver_user_exception()` -- push CONTEXT+EXCEPTION_RECORD on user stack, redirect IRET
- [ ] `NtRaiseException` SSDT entry -- calls `ki_dispatch_exception()` (§4) with `first_chance=TRUE`
- [ ] `NtContinue` SSDT entry -- restore CONTEXT, resume user-mode
- [ ] Handle misaligned or invalid user RSP gracefully (double-fault fallback to panic)

**Test checkpoint:** User-mode fault delivery: after `ki_deliver_user_exception()`, `frame->rip` points to `KiUserExceptionDispatcher` address (or stub entry), `frame->rsp` is 16-byte aligned and below the original RSP. `EXCEPTION_RECORD` and `CONTEXT` are readable on the user stack. Invalid user RSP (e.g., 0xDEAD) → process terminated, not kernel panic. `POST16(0xDE50)` before user stack manipulation, `POST16(0xDE51)` after IRET redirect. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: implement KiUserExceptionDispatcher and NtRaiseException/NtContinue"`

---

## 6. x64 Table-Based Unwind (`.pdata`, `RtlVirtualUnwind`)

**Prompt:** Implement the x64 PE32+ unwind machinery used by `RtlUnwindEx` and the SEH chain walker. Add `RtlLookupFunctionEntry(ULONGLONG pc, ULONGLONG *base, void *history)` -- it scans the `.pdata` section of the module containing `pc` for the matching `RUNTIME_FUNCTION` entry. Implement `RtlVirtualUnwind(handler_type, image_base, pc, func_entry, ctx, handler_data, establisher_frame, ctx_ptrs)` -- it applies the `UNWIND_INFO` opcodes (`UWOP_PUSH_NONVOL`, `UWOP_ALLOC_SMALL`, `UWOP_ALLOC_LARGE`, `UWOP_SET_FPREG`, `UWOP_SAVE_NONVOL`, `UWOP_SAVE_XMM128`) to a `CONTEXT`, returning the parent frame's PC and RBP. These two functions live in a new `src/kernel/rtl/unwind.c` / `include/kernel/rtl/unwind.h`.

> [!IMPORTANT]
> → XREF: `TODO-17 §6` -- PE32+ section loader registers `.pdata` with the module list; `exec_find_module_by_pc()` (§7) provides module lookup for `RtlLookupFunctionEntry`.

- [ ] `include/kernel/rtl/unwind.h` -- `RUNTIME_FUNCTION`, `UNWIND_INFO`, `UNWIND_CODE`, `SCOPE_TABLE`
- [ ] `src/kernel/rtl/unwind.c` -- `RtlLookupFunctionEntry`, `RtlVirtualUnwind`
- [ ] Support all UWOP opcodes used by Clang/MSVC for x86-64
- [ ] `RtlAddFunctionTable`/`RtlDeleteFunctionTable`/growable-table APIs for dynamic/JIT code with no backing image (Windows-parity 💎); the growable table storage is owned by the image registry. -> XREF: TODO-18 §5 (unwind metadata registry).
- [ ] Unit test: unwind a 3-frame kernel test stack and verify the recovered RIP chain

**Test checkpoint:** `RtlLookupFunctionEntry` for a known kernel function returns a valid `RUNTIME_FUNCTION` with correct `BeginAddress`/`EndAddress`. `RtlVirtualUnwind` on a 3-frame test call chain recovers the correct RIP for each parent frame. Unknown address returns NULL. Serial log: `"rtl: unwind init, <N> .pdata entries registered"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlLookupFunctionEntry and RtlVirtualUnwind for x64 unwind"`

---

## 7. Stack Walking (`RtlCaptureStackBackTrace`, `RtlWalkFrameChain`)

**Prompt:** Implement kernel-mode stack walking using the x64 unwind tables from §6. On Windows, `RtlCaptureStackBackTrace` is the primary API for capturing a stack trace -- it walks the call stack via `RtlVirtualUnwind` and records return addresses. Both user-mode (ntdll) and kernel-mode (ntoskrnl) expose this function. Linux has `stack_trace_save()`. Impossible OS needs the kernel-mode implementation here; the user-mode version is in `TODO-04-ntdll-user-runtime.md`.

- [ ] `include/kernel/rtl/unwind.h` -- declare `RtlCaptureStackBackTrace(skip, count, buffer, hash)`, `RtlWalkFrameChain(callers, count, flags)`
- [ ] `src/kernel/rtl/unwind.c` -- implement `RtlCaptureStackBackTrace`: call `RtlVirtualUnwind` (§6) in a loop, skip `skip` frames, record up to `count` return addresses into `buffer`, compute optional `hash`; max 0xFE frames
- [ ] `src/kernel/rtl/unwind.c` -- implement `RtlWalkFrameChain`: thin wrapper; `flags & 1` = user-mode stack walk (read user RSP/RBP via safe probe §13)
- [ ] IRQL requirement: callable at `IRQL <= DISPATCH_LEVEL` (→ XREF: `TODO-17 §3`)

**Test checkpoint:** `RtlCaptureStackBackTrace(0, 10, buf, NULL)` called from a 4-deep call chain returns ≥4 frames. `RtlCaptureStackBackTrace(2, 10, buf, NULL)` skips 2 frames -- first captured RIP differs from skip=0 case. Hash output is non-zero and deterministic for the same call site. Serial log: `"rtl: captured <N> stack frames"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlCaptureStackBackTrace and RtlWalkFrameChain for kernel-mode stack walking"`

---

## 8. SEH Chain Walk and `__try`/`__except` Frame Dispatch

**Prompt:** Implement the SEH dispatcher that `KiUserExceptionDispatcher` (§5) calls.
For x64, SEH is table-based (not frame-linked) -- the unwind tables (§6) encode `__try` scope ranges and handler RVAs in a `SCOPE_TABLE`. The language-specific handler `__C_specific_handler` (registered in each function's `UNWIND_INFO`) is invoked by the dispatcher:
1. Calls `RtlLookupFunctionEntry` to find the enclosing function.
2. Invokes the language-specific handler (`__C_specific_handler` for C SEH), which walks `SCOPE_TABLE` entries (each entry: `BeginAddress`, `EndAddress`, `HandlerAddress`, `JumpTarget`).
3. For a matching scope: calls the filter expression. If `EXCEPTION_EXECUTE_HANDLER`, calls `RtlUnwindEx` (§9) to unwind the stack to the handler frame, then jumps to `JumpTarget`.
4. If no handler found in the current frame, walks to the parent frame via `RtlVirtualUnwind`.
5. If no handler found anywhere: falls through to the unhandled exception path (§12).
Expose `RtlDispatchException(EXCEPTION_RECORD *, CONTEXT *)` -- returns TRUE if handled.

> [!IMPORTANT]
> → XREF: `TODO-11 §6` -- TEB `ExceptionList` is the base for legacy x86 chain; x64 uses `.pdata` tables but TEB still needed for `NtCurrentTeb()` in `__try` lowering.

- [ ] `src/kernel/rtl/seh.c` -- `RtlDispatchException`, scope-table walker
- [ ] `src/kernel/rtl/seh.c` -- `__C_specific_handler` -- the language-specific exception handler for C `__try`/`__except`; referenced by `UNWIND_INFO.ExceptionHandler` in compiled PE binaries
- [ ] Filter expression invocation with correct calling convention
- [ ] Nested exception handling (`EXCEPTION_NESTED_CALL` flag)

**Test checkpoint:** `RtlDispatchException` with a `SCOPE_TABLE` containing one matching `__try` scope calls the filter expression and returns TRUE. Filter returning `EXCEPTION_CONTINUE_SEARCH` → walks to parent frame. No matching scope in any frame → returns FALSE. `__C_specific_handler` is invoked for functions whose `UNWIND_INFO` references it. Serial log: `"seh: scope match at RVA 0x<rva>, filter=EXECUTE_HANDLER"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlDispatchException, __C_specific_handler, and SEH scope-table walker"`

---

## 9. RtlUnwindEx -- Unwind to a Target Frame

**Prompt:** Implement `RtlUnwindEx(target_frame, target_ip, exception_record, return_value, ctx, history)`.
It iterates from the current RSP upward via `RtlVirtualUnwind` (§6), calling each frame's `__finally` block (via the termination handler in the SCOPE_TABLE), until it reaches `target_frame`. At that point it restores `ctx` (with `return_value` in RAX and `target_ip` in RIP) and jumps to the target. It must correctly handle:
- `__finally` termination handlers (UWOP_SCOPE with `HandlerAddress == TERMINATION`).
- Continuation frames (frames with no handler -- just unwind and continue).
- The global unwind flag (`EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND`) set on the exception record during this walk.

- [ ] `src/kernel/rtl/unwind.c` -- `RtlUnwindEx`
- [ ] Termination handler invocation during unwind walk
- [ ] Correctly set/clear `EXCEPTION_UNWINDING` flag

**Test checkpoint:** `RtlUnwindEx` from a 3-frame stack to the target frame invokes `__finally` in each intermediate frame. `EXCEPTION_UNWINDING` flag is set on the `EXCEPTION_RECORD` during the walk and cleared after. Target frame receives `return_value` in RAX. Serial log: `"rtl: unwind to frame 0x<target>, <N> finally handlers invoked"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlUnwindEx with termination handler invocation"`

---

## 10. Vectored Exception Handlers (VEH)

**Prompt:** Implement the Vectored Exception Handler list, called by `ki_dispatch_exception()` (§4) **before** the SEH chain walk (§8). The VEH list is per-process -- a doubly-linked list of `VECTORED_EXCEPTION_ENTRY` nodes, protected by an `rwlock`. Add:
- `RtlAddVectoredExceptionHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)` -- prepend (first=1) or append (first=0) to the list; return an opaque handle.
- `RtlRemoveVectoredExceptionHandler(PVOID handle)` -- unlink by handle.
- `ki_call_veh_list(EXCEPTION_POINTERS *ptrs)` -- walk the list; return `TRUE` if any handler returned `EXCEPTION_CONTINUE_EXECUTION`.
Store the VEH list head in `struct task` (process-level, not thread-level).

> [!IMPORTANT]
> → XREF: `TODO-05 §2` -- VEH handles are not Win32 kernel handles; use a simple opaque pointer. No overlap with the Object Manager handle table.

- [ ] `include/kernel/except.h` -- `VECTORED_EXCEPTION_ENTRY`, VEH list head in `struct task`
- [ ] `src/kernel/rtl/veh.c` -- `RtlAddVectoredExceptionHandler`, `RtlRemoveVectoredExceptionHandler`, `ki_call_veh_list`
- [ ] Thread-safe list manipulation with `rwlock`
- [ ] `ki_dispatch_exception()` (§4) calls `ki_call_veh_list` before SEH dispatch

**Test checkpoint:** `RtlAddVectoredExceptionHandler(1, handler)` returns non-NULL handle. Second registration with `first=0` appends -- first handler called first. `RtlRemoveVectoredExceptionHandler(handle)` succeeds; removed handler is not called on next exception. `ki_call_veh_list` with handler returning `EXCEPTION_CONTINUE_EXECUTION` returns TRUE. Thread-safe: two threads registering concurrently don't corrupt the list. Serial log: `"veh: registered handler at 0x<addr>"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement Vectored Exception Handler (VEH) list"`

---

## 11. Vectored Continue Handlers (VCH)

**Prompt:** Implement the Vectored Continue Handler list -- a separate mechanism from VEH (§10). On Windows, VCH handlers are called **after** a frame-based (SEH) handler has been found and has decided to continue execution, but **before** execution actually resumes. This allows monitoring/logging handlers to observe that an exception was handled without interfering with the dispatch. The VCH list uses the same `VECTORED_EXCEPTION_ENTRY` node type as VEH but is stored in a separate list head.

- `AddVectoredContinueHandler(ULONG first, PVECTORED_EXCEPTION_HANDLER handler)` -- prepend (first=1) or append (first=0) to the VCH list.
- `RemoveVectoredContinueHandler(PVOID handle)` -- unlink by handle.
- `ki_call_vch_list(EXCEPTION_POINTERS *ptrs)` -- walk the VCH list after SEH dispatch succeeds.

> [!NOTE]
> VCH is distinct from VEH. VEH runs BEFORE frame-based handlers; VCH runs AFTER. Both are per-process. The dispatch order in `ki_dispatch_exception()` (§4) is: debugger first-chance → VEH → SEH → debugger second-chance → VCH → unhandled filter.

- [ ] `include/kernel/except.h` -- VCH list head in `struct task` (separate from VEH list head)
- [ ] `src/kernel/rtl/veh.c` -- `AddVectoredContinueHandler`, `RemoveVectoredContinueHandler`, `ki_call_vch_list`
- [ ] `ki_dispatch_exception()` (§4) calls `ki_call_vch_list` after SEH handler returns `EXCEPTION_CONTINUE_EXECUTION`

**Test checkpoint:** `AddVectoredContinueHandler` returns non-NULL. After SEH handles an exception, VCH handler fires -- serial log shows `"vch: continue handler called, code=0x<code>"`. VCH handler NOT called when exception is unhandled (only when SEH succeeds). `RemoveVectoredContinueHandler` removes correctly. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement Vectored Continue Handler (VCH) list"`

---

## 12. Unhandled Exception Filter and WER Hook

**Prompt:** Implement the terminal path reached when `ki_dispatch_exception()` (§4) exhausts all handlers (VEH declined, SEH returned FALSE, debugger second-chance declined, VCH had no effect). The dispatch reaches this section only if no handler claimed the exception. Steps:
1. Per-process unhandled exception filter set by `SetUnhandledExceptionFilter` (stored in PEB).
2. Default filter -- terminate the process with the exception code as exit status and emit a structured log entry (→ XREF: `TODO-11 §1`) with the full `EXCEPTION_RECORD` and first 8 frames of the stack trace via `RtlCaptureStackBackTrace` (§2).
Add `RtlSetUnhandledExceptionFilter(handler)` -- stores handler in `PEB.UnhandledExceptionFilter`.
Add `UnhandledExceptionFilter(EXCEPTION_POINTERS *)` -- calls the per-process filter or default.
Add a WER (Windows Error Reporting) stub: `WerpReportFault()` calls into a future `werfault.exe` process via a named pipe (leave as a no-op stub for now, log to serial).

> [!IMPORTANT]
> → XREF: `TODO-11 §7` -- `PEB.UnhandledExceptionFilter` field must be reserved in the PEB struct.

- [ ] `src/kernel/rtl/seh.c` -- `UnhandledExceptionFilter`, default fatal handler
- [ ] `PEB.UnhandledExceptionFilter` field (→ XREF: `TODO-11 §7`)
- [ ] Structured crash log: exception code, fault address, top-8 frames via `RtlCaptureStackBackTrace` (§7)
- [ ] `WerpReportFault()` stub -- serial log only for now
- [ ] Process termination with exception code as exit status

**Test checkpoint:** Unhandled user-mode access violation: `UnhandledExceptionFilter` calls `WerpReportFault()` -- serial log shows `"wer: fault report code=0xC0000005, addr=0x<addr>"` followed by 8-frame stack trace. Process exit code is the exception NTSTATUS. `SetUnhandledExceptionFilter(custom_handler)` -- custom handler called instead of default. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement unhandled exception filter, WER stub, and crash log"`

---

## 13. Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`)

**Prompt:** Add kernel-mode safe pointer validation to prevent ring-3 pointers from crashing the kernel when accessed by syscall handlers. Implement:
- `ProbeForRead(addr, length, alignment)` -- verify `[addr, addr+length)` is in user address space (below `USER_SPACE_LIMIT`) and aligned. Raise `STATUS_ACCESS_VIOLATION` (via `ki_raise_kernel_exception`) if not.
- `ProbeForWrite(addr, length, alignment)` -- same check, plus touch the first byte of each page to force a present+writable mapping, catching write-protected pages.
- `try_copy_from_user(dst, src, n)` / `try_copy_to_user(dst, src, n)` -- equivalent of Linux `copy_from_user`/`copy_to_user`. Uses a per-CPU `safe_return_rip` slot in CPU-local storage; the #PF handler checks it (§2) and redirects to the safe-return path if a fault occurs inside a guarded copy.

> [!IMPORTANT]
> → XREF: `TODO-17 §4` -- the per-CPU safe_return_rip slot is CPU-local data, co-located with the IRQL tracking fields.

- [ ] `include/kernel/probe.h` -- `ProbeForRead`, `ProbeForWrite`, `try_copy_from_user`, `try_copy_to_user`
- [ ] `src/kernel/probe.c` -- implementation; `safe_return_rip` slot in CPU-local area
- [ ] Update #PF triage (§2) to check `safe_return_rip` and redirect on kernel probe faults
- [ ] Apply `ProbeForRead`/`ProbeForWrite` to all syscall handlers that dereference user pointers

**Test checkpoint:** `ProbeForRead(user_addr, 8, 4)` on a valid mapped user page succeeds (no exception). `ProbeForRead(kernel_addr, 8, 4)` raises `STATUS_ACCESS_VIOLATION` -- does NOT panic. `ProbeForWrite(user_addr, 4096, 1)` touches each page -- succeeds for mapped writable pages. `try_copy_from_user` from unmapped address returns error code, not crash. `POST16(0xDED0)` before `safe_return_rip` slot setup, `POST16(0xDED1)` after #PF safe-return path tested. If crash at 0xDED0: safe_return_rip slot not initialized. Test on: QEMU WHPX + TCG. Verify on bare metal -- TLB behavior differs.

- [ ] Commit: `"kernel: add ProbeForRead/Write and try_copy_{from,to}_user safe probing"`

---

## 14. Kernel-Mode `__try`/`__except` for Drivers

**Prompt:** Enable kernel-mode structured exception handling so drivers can wrap dangerous operations (MMIO access, DMA buffer reads) in `__try`/`__except`. The mechanism differs from user-mode: there is no user stack to push onto. Instead:
1. Add `KI_EXCEPTION_REGISTRATION` -- a per-thread (kernel stack) record pushed by `__try` lowering code at the head of the thread's kernel stack frame.
2. `ki_raise_kernel_exception(EXCEPTION_RECORD *, CONTEXT *)` -- walks the kernel-mode exception chain (stored in the per-CPU `current_thread->kernel_exception_list`), calls filter expressions, and invokes `RtlUnwindEx` (§9) for matching handlers.
3. Patch the kernel's `.pdata` section to include `UNWIND_INFO` for critical paths (requires linker script changes to emit `.pdata` for `clang-19`).
4. Guard against re-entrancy: if a kernel exception occurs inside a kernel exception handler, escalate directly to `KeBugCheckEx` (panic with structured code).

> [!IMPORTANT]
> → XREF: `TODO-17 §3` -- kernel `__try` must only be used at `PASSIVE_LEVEL` or `APC_LEVEL`; add `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` assertion at the start of `ki_raise_kernel_exception`.

- [ ] `include/kernel/except.h` -- `KI_EXCEPTION_REGISTRATION`, kernel exception chain head in `struct task`
- [ ] `src/kernel/except.c` -- `ki_raise_kernel_exception()`
- [ ] Linker script: ensure kernel code sections emit `.pdata` with `-fexceptions` (or manual stubs)
- [ ] Re-entrancy guard: nested kernel exception → `KeBugCheckEx`
- [ ] Wrap one existing dangerous driver operation (e.g., AHCI MMIO read) as a smoke test

**Test checkpoint:** Kernel `__try { *(volatile int*)0 = 0; } __except(EXCEPTION_EXECUTE_HANDLER) { /* handled */ }` -- handler fires, kernel continues executing. Serial log: `"except: kernel exception handled at 0x<rip>"`. Nested kernel exception (exception inside handler) → `KeBugCheckEx` with code `KERNEL_EXCEPTION_NOT_HANDLED`. AHCI MMIO read smoke test: guarded read of unmapped BAR address → exception caught, not panic. `POST16(0xDEE0)` before kernel exception chain walk, `POST16(0xDEE1)` after handler invocation. Test on: QEMU WHPX + TCG. Verify on bare metal.

- [ ] Commit: `"kernel: implement kernel-mode __try/__except via KI_EXCEPTION_REGISTRATION"`

---

## 15. POSIX Signal Delivery from Exceptions (Linux Compat)

**Prompt:** Map hardware faults to POSIX signals for processes running under the Linux compatibility layer. After the VEH list and SEH dispatch both decline the exception (neither handled it), check the current task's `compat_mode` flag. If set:
- `STATUS_ACCESS_VIOLATION` → `SIGSEGV`
- `STATUS_ILLEGAL_INSTRUCTION` → `SIGILL`
- `STATUS_INTEGER_DIVIDE_BY_ZERO` → `SIGFPE`
- `STATUS_STACK_OVERFLOW` → `SIGSEGV` (with `si_code = SEGV_ACCERR`)
- `STATUS_BREAKPOINT` → `SIGTRAP`
Deliver via the existing `task->signals` mechanism. If the signal has a handler registered via `rt_sigaction` (→ XREF: `10-platform-services/TODO-10-linux-compat.md §9`), set up a signal frame on the user stack with a `siginfo_t` containing fault details and redirect execution to the handler. If the signal has no handler (default disposition), terminate the task with an appropriate `NTSTATUS` exit code.
If the process has set up a `sigaltstack`, deliver `SIGSEGV` on the alternate stack to handle stack overflow correctly.
This section is gated on the Linux compat layer existing -- stub it out with a compile-time flag `CONFIG_LINUX_COMPAT` for now.

> [!IMPORTANT]
> → XREF: `TODO-21 §6` (process capabilities) -- compat mode is a process flag.
> → XREF: `10-platform-services/TODO-10-linux-compat.md §9` -- `rt_sigaction` handler registration. This section handles the kernel-side fault-to-signal mapping and signal frame setup; the compat layer TODO handles the `rt_sigaction`/`rt_sigprocmask` API surface.
> Delivery path hooks into §12 (unhandled exception filter) as a pre-termination step.

- [ ] `include/kernel/compat.h` -- `CONFIG_LINUX_COMPAT` guard, fault-to-signal table
- [ ] `src/kernel/compat/signal_compat.c` -- `ki_deliver_compat_signal()` -- fault-to-signal translation
- [ ] `siginfo_t` population: `si_signo`, `si_code` (e.g., `SEGV_MAPERR`, `SEGV_ACCERR`), `si_addr` (fault address)
- [ ] `sigaltstack` support: if `task->sigaltstack` is set and signal is `SIGSEGV`, deliver on the alternate stack
- [ ] Hook into §12 (unhandled exception filter) before process termination
- [ ] `compat_mode` flag in `struct task` (or reuse `capabilities` field bit 63 as compat bit)

**Test checkpoint:** Linux-compat process with `SIGSEGV` handler: NULL dereference delivers `SIGSEGV` with `si_code=SEGV_MAPERR`, `si_addr=0x0` -- handler fires, process continues. Linux-compat process without handler: NULL dereference terminates with `STATUS_ACCESS_VIOLATION`. `sigaltstack` set: stack overflow delivers `SIGSEGV` on alternate stack, not the overflowed stack. Compile guard: `#ifndef CONFIG_LINUX_COMPAT` → all compat code is excluded. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: map hardware faults to POSIX signals for Linux compat processes"`

---

## 16. Exception Dispatch Telemetry

**Prompt:** Neither Windows nor Linux logs exception dispatch decisions for post-mortem analysis. Impossible OS can record the full exception dispatch trace -- which handler was tried, what it returned, how many frames were unwound -- into the JSON structured log system (TODO-04). This enables a developer to see exactly why an exception was or wasn't caught, without attaching a debugger.

> [!TIP]
> **Competitive edge:** Windows WER only captures the final crash state. Linux core dumps only capture memory. Neither records the dispatch decision chain: "VEH handler at 0x1234 returned CONTINUE_SEARCH, SEH filter at 0x5678 returned EXCEPTION_EXECUTE_HANDLER, unwound 3 frames." This telemetry turns exception handling from a black box into a fully observable pipeline.

- [ ] `src/kernel/except.c` -- `except_log_dispatch(EXCEPTION_RECORD *rec, const char *handler_name, int disposition)` -- log each handler invocation with disposition
- [ ] Log entries: exception code, fault address, handler type (VEH/SEH/VCH/filter/unhandled), handler address, disposition returned, frame count unwound
- [ ] JSON format compatible with TODO-11 §1 structured events; event type `"exception_dispatch"`
- [ ] Rate limiting: max 100 exception dispatch logs per second per process to prevent log flooding from intentional exceptions (e.g., guard page probing)
- [ ] Compile-time `CONFIG_EXCEPT_TELEMETRY` guard (default: enabled in debug builds, disabled in release)

**Test checkpoint:** Trigger a user-mode access violation → serial log contains JSON event: `{"type":"exception_dispatch","code":"0xC0000005","handler":"veh","disposition":"CONTINUE_SEARCH"}` (or similar). Rate limiting: trigger 200 exceptions in rapid succession -- log shows ≤100 entries. `CONFIG_EXCEPT_TELEMETRY=0` build: no telemetry log entries emitted. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: add exception dispatch telemetry to JSON structured log"`

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11           | 🐧 Linux          | 🚀 Impossible OS      |
| --- | ----------------------------- | ----------------- | ---------------- | -------------------- |
| 💎   | EXCEPTION_RECORD/CONTEXT      | ✅ ntdll           | ❌                | ⬜ §1                 |
| 💎   | #PF user/kernel triage        | ✅                 | ✅                | ⚠️ §2 kernel-only    |
| 💎   | #DB/#BP debugger routing      | ✅                 | ✅ ptrace         | ⬜ §3                 |
| 💎   | #CP CET shadow-stack          | ✅ 24H2+           | ✅ 6.6+           | ⬜ §3 conditional     |
| 💎   | Debugger 1st/2nd-chance       | ✅ KiDebugRoutine  | ✅ ptrace         | ⬜ §4                 |
| 💎   | KiUserExceptionDispatcher     | ✅                 | ❌                | ⬜ §5                 |
| 💎   | x64 table-based unwind        | ✅ UNWIND_INFO     | ✅ .eh_frame      | ⬜ §6                 |
| 💎   | Kernel stack walking          | ✅ RtlCaptureStack | ✅ stack_trace    | ⬜ §7                 |
| 💎   | SEH + __C_specific_handler    | ✅                 | ❌                | ⬜ §8                 |
| 💎   | RtlUnwindEx + __finally       | ✅                 | ❌                | ⬜ §9                 |
| 💎   | VEH list                      | ✅                 | ❌                | ⬜ §10                |
| 💎   | VCH list                      | ✅                 | ❌                | ⬜ §11                |
| 💎   | Unhandled exception filter    | ✅ WER             | ✅ core dump      | ⬜ §12                |
| ⭐   | IRQL-aware safe probing       | ✅ ProbeForRead    | ✅ copy_from_user | ⬜ §13                |
| 💎   | Kernel __try/__except         | ✅                 | ❌                | ⬜ §14                |
| 💎   | POSIX signal from faults      | ❌                 | ✅                | ⬜ §15 compat         |
| 💎   | sigaltstack overflow          | ❌                 | ✅                | ⬜ §15 compat         |
| ⭐   | Dispatch telemetry            | ❌                 | ❌                | ⬜ §16 JSON log       |
| ⭐   | Exception budget / storm ctrl | ❌                 | ❌                | ⬜ §16 rate-limit ext |

> **After parity items:** Impossible OS matches Windows on the full SEH/VEH/VCH pipeline and matches Linux on POSIX signal delivery. Exclusive differentiators: **dispatch telemetry** recording the full VEH → SEH → VCH handler chain into the JSON structured log (neither WER nor core dumps capture the decision sequence); **IRQL-aware safe probing** co-locating `safe_return_rip` with IRQL fields for zero-overhead probe checks in the #PF handler; and **exception storm control** rate-limiting per-process exceptions to prevent DoS from runaway JITs or intentional exception flooding.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_except()`.
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_except.c` with:
  - `context_from_frame` populates all GP registers from an `interrupt_frame` correctly
  - `frame_from_context` restores registers back; round-trip preserves RIP, RSP, RFLAGS
  - `EXCEPTION_RECORD` for `STATUS_ACCESS_VIOLATION` has correct code and fault address in `ExceptionInformation[1]`
  - `EXCEPTION_RECORD` for `STATUS_INTEGER_DIVIDE_BY_ZERO` has correct code and `EXCEPTION_CONTINUABLE` flag
  - `EXCEPTION_RECORD` for `STATUS_BREAKPOINT` has `ExceptionAddress` adjusted by -1 (past INT3 byte)
  - `EXCEPTION_RECORD` for `STATUS_CONTROL_STACK_VIOLATION` (if CET supported) has correct code
  - `ki_dispatch_exception` with no debugger and no handlers returns to unhandled filter path
  - `ki_dispatch_exception` with KiDebugRoutine set calls debugger first-chance before VEH
  - `ProbeForRead` on user-space address (below `USER_SPACE_LIMIT`) with correct alignment succeeds
  - `ProbeForRead` on kernel address raises `STATUS_ACCESS_VIOLATION` (does not panic)
  - `ProbeForWrite` on kernel address raises `STATUS_ACCESS_VIOLATION`
  - `ProbeForRead` with misaligned address and alignment > 1 raises `STATUS_DATATYPE_MISALIGNMENT`
  - `try_copy_from_user` from valid mapped user page succeeds; data matches
  - `try_copy_from_user` from unmapped address returns error (does not panic)
  - `try_copy_to_user` to valid mapped user page succeeds; data readable back
  - `RtlVirtualUnwind` on a known 3-frame kernel stack recovers correct RIP chain
  - `RtlLookupFunctionEntry` returns non-NULL for a known function in `.pdata`; returns NULL for address outside any module
  - `RtlCaptureStackBackTrace` with skip=0 and count=5 returns ≥3 frames (test frame + caller + caller's caller)
  - `RtlCaptureStackBackTrace` with skip=1 skips the immediate caller correctly
  - VEH registration: `RtlAddVectoredExceptionHandler` returns non-NULL handle
  - VEH removal: `RtlRemoveVectoredExceptionHandler` with valid handle succeeds
  - VCH registration: `AddVectoredContinueHandler` returns non-NULL handle
  - VCH removal: `RemoveVectoredContinueHandler` with valid handle succeeds
  - `__C_specific_handler` correctly invokes filter expression for a matching scope-table entry
  - Kernel `__try`/`__except` around a guarded region: exception handler fires and kernel continues
- [ ] Register in `test_runner_init()`: `test_register_except()`
- [ ] Commit: `"test: add exception dispatch and SEH test suite"`

## Verification

- [ ] Trigger a deliberate user-mode `NULL` dereference; verify the process terminates with `STATUS_ACCESS_VIOLATION` and a log entry -- not a kernel panic.
- [ ] Trigger a user-mode divide-by-zero; verify `STATUS_INTEGER_DIVIDE_BY_ZERO`.
- [ ] Trigger a user-mode INT3; verify `STATUS_BREAKPOINT` is delivered. With debugger attached: verify first-chance notification fires.
- [ ] Register a VEH that continues execution after patching RIP past the fault; verify the process survives.
- [ ] Register a VCH; trigger an exception handled by SEH; verify the VCH fires after the SEH handler.
- [ ] Register a `__try`/`__except` block around a `NULL` dereference in a test driver; verify the handler fires and the kernel keeps running.
- [ ] Call `ProbeForRead` with a kernel address from a syscall handler; verify it raises `STATUS_ACCESS_VIOLATION` without panicking.
- [ ] Verify `RtlVirtualUnwind` correctly unwinds a 4-frame kernel test stack to the expected RIP values.
- [ ] Verify `RtlCaptureStackBackTrace(0, 10, buf, NULL)` returns ≥4 frames from a known call depth.
- [ ] Verify exception dispatch telemetry: trigger a fault, check serial log for JSON `exception_dispatch` event with handler chain.
- [ ] Commit: `"kernel/rtl: exception dispatch, SEH, VEH, VCH, safe probing, debugger notification, and kernel __try/__except complete"`
