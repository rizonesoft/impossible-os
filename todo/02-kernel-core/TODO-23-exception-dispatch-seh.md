---
schema_version: 1
id: exception-dispatch-seh
domain: 02-kernel-core
status: active
title: "TODO-23 -- Exception Dispatch & SEH"
---

# TODO-23 -- Exception Dispatch & SEH

> **Validated:** 2026-07-15 | validate-todo-file clean (structure / IO table / XREF / test wiring)

> **Gap-audited:** 2026-07-15 | gap-audit + codex-gap-audit; 13 findings filed (ring-0/ring-3 boundary, PEB-filter myth, probe-API duplication, CONTEXT ABI, fastfail, CET unwind)

> **Goal:** Replace the current "all CPU exceptions → `panic_screen()`" model with a proper Windows-style exception dispatch pipeline. That means: a captured `CONTEXT` record, an `EXCEPTION_RECORD` with fault address and exception code, a page-fault triage layer that separates recoverable faults from hard kills, debugger first-chance/second-chance notification, a `KiUserExceptionDispatcher` path that delivers faults to user-mode SEH handlers via the TEB chain, x64 table-based unwind (`RtlVirtualUnwind`), kernel-mode stack walking (`RtlCaptureStackBackTrace`), Vectored Exception Handlers (VEH), Vectored Continue Handlers (VCH), `__C_specific_handler` for SEH scope-table dispatch, an unhandled exception filter, kernel-mode safe probing (`ProbeForRead`/`ProbeForWrite`), kernel-driver `__try`/`__except` support, POSIX signal delivery for Linux-compat processes (including `sigaltstack`), and exception dispatch telemetry. Without this, every access violation -- whether in a driver or a user app -- crashes the whole OS rather than being caught and reported correctly.

> [!NOTE]
> **WHOLE-FILE BLOCKER CLEARED 2026-07-17 -- this file is UNPARKED.** Every section here was gated on the kernel-BSS/`USER_BASE` ceiling: `.bss` ended at `0x7FEE55`, 4523 bytes under `USER_BASE` `0x800000`, so `scripts/build.sh`'s BSS-collision guard failed any section adding more than ~4 KiB of `.text`/`.rodata`. `02-kernel-core/TODO-33 §10` converted the large static pools to frame-backed storage and the BSS end moved to `0x6c2000` -- **~1272 KiB of headroom**, ~280x what this file's §1 needed. The 16 ceiling `Deferred` stamps were swept and the Implementation Order reset to `[ ]`; the file now runs on its own Implementation Order, §1 first -> XREF: `02-kernel-core/TODO-33 §11` (item: "`TODO-23 §1-§16` (exception/SEH)").
>
> **§1 is code-COMPLETE and parked in `git stash` `todo23-s1-wip` -- APPLY it, do not rewrite.** It was implemented and design-reviewed before the park; re-verify it against the moved HEAD (the tree advanced substantially) rather than trusting the old review, exactly as `TODO-24 §7` needed when its stash was re-applied.

> [!IMPORTANT]
> **Current state:** `vmm.c` has a `page_fault_handler` (vector 14) that chains to the swap and mmap handlers for kernel-mode recoverable faults, then falls through to `panic_screen()`. All other CPU exception vectors (0-13, 15-31) dispatch directly to `panic_screen()` via `idt.c`. No `EXCEPTION_RECORD` or `CONTEXT` is captured, no user-mode fault delivery path exists, and there is no kernel safe-probing API. The POSIX signal machinery (`signals` field in `struct task`) is wired but not exercised.

> [!IMPORTANT]
> **Address-space boundary (binding for §4-§12).** Windows dispatches user-mode exceptions in **ring 3**: the kernel captures the fault and delivers a trap frame to `KiUserExceptionDispatcher`; ntdll's `RtlDispatchException` then walks VEH -> SEH -> VCH -> top-level filter. The kernel NEVER calls user handlers. This TODO owns the **ring-0 half**: capture (§1-§3), debugger/debug-port mediation (§4), trap-frame delivery (§5), unwind + stack walking for KERNEL code (§6, §7, §9), `NtContinue`/`NtRaiseException` validation, kernel-mode SEH (§14), and terminal termination/bugcheck. The **ring-3 half** (VEH/VCH lists, `RtlDispatchException`, `__C_specific_handler`, the top-level filter and crash dialog) is owned by `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5`. Running application callbacks in kernel context would destroy the privilege boundary and turn a handler fault into a kernel fault.

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
- → XREF: `TODO-14-registry-completion.md §4` -- NtXxx registry syscalls use `ProbeForRead` / `ProbeForWrite` (§13) for user-mode pointer validation
- → XREF: `TODO-11-peb-teb-user-abi.md §7` (initial user stack frame -- KiUserExceptionDispatcher target)
- → XREF: `TODO-12-native-api-ssdt.md §1` (NTSTATUS -- `NtRaiseException`/`NtContinue` return values)
- → XREF: `TODO-07-irql-model-dpcs.md §3` (interrupt entry/exit IRQL -- fault occurs at hardware IRQL)
- → XREF: `TODO-07-irql-model-dpcs.md §12` -- KiDeliverApc sets up user-mode trap frame for user APC delivery; KiUserApcDispatcher parallels KiUserExceptionDispatcher (§5) and should be implemented alongside it
- → XREF: `TODO-10-kernel-security-hardening.md §9` -- CET shadow stack; §3 of this TODO handles the resulting `#CP` exception (vector 21)
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md §5` -- #DB/#BP exception routing to KD; §3-§4 of this TODO integrate with KD for first/second-chance notification
- → XREF: `TODO-04-system-logging.md §6` -- structured JSON log events; §16 of this TODO emits exception dispatch telemetry
- → XREF: `10-platform-services/TODO-10-linux-compat.md §8` -- `rt_sigaction` handler registration for Linux compat signal delivery (§15)

## Outcome

- `include/kernel/except.h` -- `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, NTSTATUS fault codes, `KI_EXCEPTION_REGISTRATION`, VEH/VCH list heads
- `src/kernel/except.c` -- `context_from_frame()`, `frame_from_context()`, `except_init()`, fault ISR handlers (vectors 0, 1, 3, 4, 6, 11, 12, 13, 21), `ki_dispatch_exception()` (master dispatcher with debugger first/second-chance), `ki_raise_kernel_exception()`, `except_log_dispatch()` (telemetry)
- `src/kernel/mm/vmm.c` -- `page_fault_handler()` triages #PF into user vs. kernel paths with EXCEPTION_RECORD capture
- `src/kernel/rtl/unwind.c` + `include/kernel/rtl/unwind.h` -- `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`, `RtlCaptureStackBackTrace`, `RtlWalkFrameChain`
- `src/kernel/rtl/seh.c` -- kernel-mode SEH SCOPE_TABLE walker (§14) + `WerpReportFault()` stub. The ring-3 `RtlDispatchException` / `__C_specific_handler` / top-level filter are ntdll-side -> `12-user-platform-sdk/TODO-04 §5`
- `include/kernel/except.h` -- `VECTORED_EXCEPTION_ENTRY` node + disposition ABI shared with ntdll (the VEH/VCH lists themselves are ring-3 state, not kernel state)
- `include/kernel/nt/zw.h` + `src/kernel/nt/ssdt.c` -- EXISTING `ProbeForRead`/`ProbeForWrite` extended in place (page-touch) plus new `try_copy_from_user` / `try_copy_to_user` fault-fixup copies. No new `probe.h`/`probe.c`
- `src/kernel/compat/signal_compat.c` -- `ki_deliver_compat_signal()` (Linux compat, `CONFIG_LINUX_COMPAT` guarded), `sigaltstack` support
- `NtRaiseException` and `NtContinue` registered in SSDT (→ `TODO-12-native-api-ssdt.md §5`)
- `KiDebugRoutine` function pointer and `DbgkForwardException()` stub for debugger integration (→ `TODO-29`)

---

## Implementation Order

| ⭐   | Order | Deliverable                                                      | Depends On                 | Status |
| --- | :---: | ---------------------------------------------------------------- | -------------------------- | :----: |
| 💎   |   1   | EXCEPTION_RECORD, CONTEXT, EXCEPTION_POINTERS                    | TODO-12 §1, TODO-33 §7     |  [ ]   |
| 💎   |   2   | #PF triage -- user vs. kernel, COW, guard, stack growth          | §1, TODO-07 §3             |  [ ]   |
| 💎   |   3   | Fault-to-exception mapping (#DE/#DB/#BP/#OF/#UD/#NP/#SS/#GP/#CP) | §1, TODO-10 §9, TODO-29 §5 |  [ ]   |
| 💎   |   4   | Debugger first-chance / second-chance notification               | §1-§3, TODO-29 §5          |  [ ]   |
| 💎   |   5   | KiUserExceptionDispatcher -- ring-3 delivery                     | §4, TODO-11 §7             |  [ ]   |
| 💎   |   6   | x64 table-based unwind (.pdata, RtlVirtualUnwind)                | §1                         |  [ ]   |
| 💎   |   7   | Stack walking (RtlCaptureStackBackTrace)                         | §6, TODO-07 §3             |  [ ]   |
| 💎   |   8   | SEH chain walk + `__C_specific_handler`                          | §5, §6, TODO-11 §6         |  [ ]   |
| 💎   |   9   | RtlUnwindEx -- unwind to target frame                            | §6, §8                     |  [ ]   |
| 💎   |  10   | Vectored Exception Handlers (VEH)                                | §4, TODO-05 §3             |  [ ]   |
| 💎   |  11   | Vectored Continue Handlers (VCH)                                 | §4, §10                    |  [ ]   |
| 💎   |  12   | Unhandled exception filter + WER hook                            | §7, §8, §10                |  [ ]   |
| ⭐   |  13   | Kernel safe probing (ProbeForRead/Write)                         | §2                         |  [ ]   |
| 💎   |  14   | Kernel-mode `__try`/`__except` for drivers                       | §6, §9, §13                |  [ ]   |
| 💎   |  15   | POSIX signal delivery from exceptions (Linux compat)             | §3, §5, D10T10 §8          |  [ ]   |
| ⭐   |  16   | Exception dispatch telemetry                                     | §4, TODO-04 §6             |  [ ]   |

> 💎 = parity -- Windows implements this feature; Impossible OS must match.
> ⭐ = exclusive -- not present in either Windows or Linux at the kernel level.

---

## 1. EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types

**Prompt:** Define the three canonical Windows exception types in a new header `include/kernel/except.h`. `EXCEPTION_RECORD` holds the exception code (`EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_ILLEGAL_INSTRUCTION`, etc.), exception flags (`EXCEPTION_CONTINUABLE`), a nested-exception pointer, the fault address, and up to `EXCEPTION_MAXIMUM_PARAMETERS` (15) information parameters.
`CONTEXT` holds the full x86-64 register file (all GP registers, RIP, RFLAGS, CS/DS/ES/FS/GS/SS, XMM0-15, FPU control/status). `EXCEPTION_POINTERS` pairs the two. Add NTSTATUS codes for the common faults (`STATUS_ACCESS_VIOLATION 0xC0000005`, `STATUS_ILLEGAL_INSTRUCTION 0xC000001D`, `STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094`, etc.).
Add `context_from_frame(struct interrupt_frame *f, CONTEXT *ctx)` to populate a CONTEXT from the ISR frame.

> [!IMPORTANT]
> → XREF: `TODO-12 §1` -- SATISFIED: `include/kernel/nt/ntstatus.h` is the canonical `NTSTATUS` home (`typedef int32_t NTSTATUS` at `:21`). `except.h` includes it; exception status codes are added THERE, never re-declared here.
> → XREF: `TODO-33 §1-§7` -- BLOCKER: this section is implemented but cannot link (kernel BSS / `USER_BASE` ceiling). See the Deferred stamp below.

> [!NOTE]
> `CONTEXT` is NOT new: `struct _CONTEXT` (1232 bytes) already ships in `include/kernel/panic.h` for the crash-dump pipeline, forward-declared at `include/kernel/nt/nt_types.h:101`. This section MOVES it (plus `XMM_SAVE_AREA32`, `M128A`, the `CONTEXT_*` flags) into `except.h` and has `panic.h` include that -- a consolidation, not a second definition.

- [x] `include/kernel/except.h` -- includes `kernel/nt/ntstatus.h` (canonical NTSTATUS); no local typedef. TODO-12 §1 landed, so the old `uefi_vars.h` temp-typedef instruction was obsolete
- [x] `include/kernel/except.h` -- `EXCEPTION_RECORD`, `EXCEPTION_POINTERS`, exception codes + `CONTEXT`/`XMM_SAVE_AREA32`/`M128A` moved from `panic.h`; `EXCEPTION_*` alias the `STATUS_*` values added to `ntstatus.h`
- [x] `CONTEXT.ContextFlags` contract: exact-group `CONTEXT_HAS_GROUP((f & g) == g)` macro -- a bare `flags & CONTEXT_CONTROL` is TRUE for ANY group (all share `CONTEXT_AMD64`). §5/§6/§9 MUST use it, never a bare AND
- [x] `_Static_assert` the AMD64 `CONTEXT` ABI: `ContextFlags` 0x30, `XMM_SAVE_AREA32` 0x100, size **0x4D0 (1232)** -- the old 0x4E0 was wrong. `EXCEPTION_RECORD` pins every field offset + 152 size
- [x] FPU/SIMD: `context_from_frame` does NO FXSAVE -- lazy FPU means live regs hold another task state (leak). Unsupported groups zeroed + flag CLEARED; `FltSave` parks at init `FCW=0x037F`/`MXCSR=0x1F80` -> XREF: `D01 T09 §5` owns `XCR0`
- [x] `src/kernel/except.c` -- `context_from_frame()`, `frame_from_context()`, `context_init_fpu_state()`; frame-backed CONTROL+INTEGER only, returns the mask actually captured. `panic_build_context()` delegates its frame fill
- [x] `Makefile` auto-globs `src/kernel/**/*.c` -- no edit needed for `except.c`; `TEST_CAT_EXCEPT` did need the `test-except` target

**Test checkpoint:** `sizeof(EXCEPTION_RECORD)` matches Windows ABI (152 bytes on x64). `sizeof(CONTEXT)` == 1232 with all GP registers + XMM0-15. `context_from_frame` round-trips correctly: populate from a test `interrupt_frame`, convert back via `frame_from_context`, compare -- RIP/RSP/RFLAGS/CS/SS/GP all match. The `_Static_assert` block + the `except` suite are the ABI proof; the boot-log line is emitted by `except_init()` (§3), which owns kernel init registration. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 16 suites -- BLOCKED: cannot link until TODO-33 lands

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
> → XREF: `TODO-07 §3` -- interrupt entry raises IRQL; fault handler runs at device IRQL.
> → XREF: `TODO-11 §5` -- `task->peb` needed to find user-mode VMM range boundary.
> → XREF: `D03 T04 §4` -- lazy mapped-file and swap-backed page-in live in the pager TODO; this section routes eligible not-present faults there rather than re-implementing pager policy in the exception layer.

- [ ] Decode error-code bits; distinguish user vs. kernel fault cleanly
- [ ] Guard page / stack-growth detection with configurable stack reserve (default 1 MiB)
- [ ] Commit-vs-reserve boundary: track committed and reserved bounds separately; on a successful grow MOVE the guard one page down and re-arm it (one-shot), and reject a fault that jumped OVER the guard instead of growing into it
- [ ] Reserve exhausted = terminal: deliver `STATUS_STACK_OVERFLOW` (§3 maps #SS) WITHOUT touching the exhausted stack; never grow past the reserve into adjacent mappings
- [ ] Build `EXCEPTION_RECORD` and `CONTEXT` on the kernel stack before dispatch
- [ ] Route to `ki_dispatch_exception()` (§4) for user-mode faults
- [ ] Route to `ki_raise_kernel_exception()` stub (implemented fully in §14)

**Test checkpoint:** Trigger a user-mode NULL dereference -- serial log shows `"pf: user fault at 0x0, code=0x<ec>"` and routes to `ki_dispatch_exception()` (stub returns to `panic_screen()` until §4 lands). Trigger a kernel-mode swap fault -- existing swap/mmap chain still handles it correctly. Guard page fault at stack bottom auto-grows the stack. `POST16(0xDE20)` on #PF entry, `POST16(0xDE21)` after triage decision. If crash: check last POST -- 0xDE20 = never entered triage, 0xDE21 = triage completed but dispatch failed. Test on: QEMU WHPX + TCG. Verify on bare metal -- #PF error code bits may differ.

- [ ] Commit: `"mm: triage #PF into user/kernel paths; defer to exception dispatch"`


---

## 3. General Fault-to-Exception Mapping (#DE, #DB, #BP, #OF, #UD, #NP, #SS, #GP, #CP)

**Prompt:** Add per-exception ISR handlers for the CPU fault vectors that should not always panic. Register handlers via `idt_register_handler()` in a new `except_init()` called from `kernel_main`. Mapping:
- Vector 0 (`#DE`) → `STATUS_INTEGER_DIVIDE_BY_ZERO` if user-mode, panic if kernel.
- Vector 1 (`#DB`) → `STATUS_SINGLE_STEP`. Route through `ki_dispatch_exception()` (§4) which notifies the debugger first-chance; if no debugger or debugger declines, deliver to user-mode as `STATUS_SINGLE_STEP` via `KiUserExceptionDispatcher` (§5). Kernel-mode `#DB` routes to KD (→ XREF: `TODO-29 §5`).
- Vector 3 (`#BP`) → `STATUS_BREAKPOINT`. Adjust `frame->rip -= 1` (INT3 is 1 byte). Route through `ki_dispatch_exception()` (§4) for debugger first-chance; if unhandled, deliver to user-mode. Kernel-mode `#BP` routes to KD (→ XREF: `TODO-29 §5`).
- Vector 4 (`#OF`) → `STATUS_INTEGER_OVERFLOW` if user.
- Vector 6 (`#UD`) → `STATUS_ILLEGAL_INSTRUCTION` if user.
- Vector 11 (`#NP`) → `STATUS_ACCESS_VIOLATION` (segment not present) if user.
- Vector 12 (`#SS`) → `STATUS_STACK_OVERFLOW` if user.
- Vector 13 (`#GP`) → `STATUS_ACCESS_VIOLATION` if user; kernel = probe check first, then panic.
- Vector 21 (`#CP`) → `STATUS_CONTROL_STACK_VIOLATION` (CET shadow-stack mismatch). User-mode: deliver via `KiUserExceptionDispatcher` (§5). Kernel-mode: `KeBugCheckEx(KERNEL_CET_SHADOW_STACK_VIOLATION)`. Only register this handler if `cpu_has(CPU_FEATURE_CET_SS)` returns true (→ XREF: `TODO-10 §9`).
Kernel-mode faults for vectors 0, 4, 6 always call `panic_screen()` (no recovery).
Each handler builds an `EXCEPTION_RECORD` (§1) and routes through `ki_dispatch_exception()` (§4) which orchestrates debugger notification and handler dispatch based on CPL in the saved CS.

> [!IMPORTANT]
> → XREF: `TODO-07 §3` -- ISR entry/exit must preserve IRQL contract.
> → XREF: `TODO-29 §5` -- #DB/#BP handlers must coexist with KD. If KD is attached, `ki_dispatch_exception()` calls `KiDebugRoutine` first-chance. If KD is not present or declines, dispatch continues to VEH/SEH. TODO-29 §5 registers raw handlers; when TODO-23 §3 lands, those handlers must be adapted to call through `ki_dispatch_exception()` instead.
> → XREF: `TODO-10 §9` -- #CP (vector 21) is generated by CET shadow stack violations. Only register the handler if `cpu_has(CPU_FEATURE_CET_SS)` returns true.
> → XREF: `TODO-10 §12` + `TODO-27 §1` -- the KERNEL-side security-check failure already ships (`__stack_chk_fail` at `src/kernel/security/stack_canary.c:154` -> `KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE, ...)`, code 0x139 defined at `include/kernel/bugcheck.h:27`). This section adds ONLY the ring-3 `int 0x29` vector; do not add a second kernel bugcheck path.

- [ ] `src/kernel/except.c` -- handlers for vectors 0, 1, 3, 4, 6, 11, 12, 13
- [ ] `src/kernel/except.c` -- conditional handler for vector 21 (`#CP`) if CET is supported
- [ ] Vector 41 (`0x29`, `__fastfail`): ring-3 `int 0x29` MUST bypass VEH/SEH/VCH and the top-level filter; take the FAST_FAIL code from ECX and terminate with noncontinuable `STATUS_STACK_BUFFER_OVERRUN` (0xC0000409)
- [ ] `except_init()` -- register all handlers; call from kernel init phase 1 (→ XREF: `TODO-01 §3`)
- [ ] CPL check from saved `frame->cs & 3` to distinguish user vs. kernel origin

**Test checkpoint:** Trigger user-mode `ud2` -- serial log shows `"except: #UD at 0x<rip>, STATUS_ILLEGAL_INSTRUCTION"`. Trigger user-mode `int3` -- serial log shows `"except: #BP at 0x<rip>, STATUS_BREAKPOINT"`. Kernel-mode `div 0` → `panic_screen()` with `STATUS_INTEGER_DIVIDE_BY_ZERO`. If CET supported: `#CP` handler registered (check `except_init` log). `POST16(0xDE30)` before `except_init()`, `POST16(0xDE31)` after all handlers registered. If crash at 0xDE30: `except_init` never entered. Test on: QEMU WHPX + TCG. Verify on bare metal.

- [ ] Commit: `"kernel: map CPU exceptions to EXCEPTION_RECORD dispatch (#DE/#DB/#BP/#GP/#UD/#SS/#CP)"`


---

## 4. Debugger First-Chance / Second-Chance Notification

**Prompt:** Implement the master exception dispatcher `ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx, KPROCESSOR_MODE mode, BOOLEAN first_chance)` that orchestrates the full Windows NT exception dispatch sequence. On Windows, the dispatch order is:
1. **Debugger first-chance notification** -- if the process has a debug port (or kernel debugger is attached for kernel-mode exceptions), send the exception to the debugger. If the debugger handles it (continues execution), stop.
2. **Ring-3 handover** (user-mode exceptions) -- deliver the trap frame to `KiUserExceptionDispatcher` (§5). Steps 3-6 then run **in ring 3**, inside ntdll, NOT in this function: VEH (§10) -> SEH (§8) -> VCH (§11) -> top-level filter (§12). See the address-space boundary callout at the top of this file.
3. **Second-chance re-entry** -- when ring-3 dispatch declines everything, ntdll re-enters the kernel via `NtRaiseException(first_chance=FALSE)` (§5). Only then does the kernel notify the debugger a second time. This round-trip IS the second-chance mechanism; there is no kernel-side handler walk.
4. **Terminal** -- no handler anywhere: terminate the process with the exception code as exit status.

For kernel-mode there is no ring-3 leg: first-chance `KiDebugRoutine` -> kernel SEH (§14) -> second-chance `KiDebugRoutine` -> `KeBugCheckEx`.

> [!IMPORTANT]
> → XREF: `TODO-29 §5` -- KD is the kernel debugger; `KiDebugRoutine` is the function pointer that `ki_dispatch_exception` calls for kernel-mode first/second-chance. If KD is not attached, `KiDebugRoutine` is NULL and the notification is skipped.
> → XREF: `TODO-29 §14` -- User-mode debug port is `NtDebugActiveProcess`; `DbgkForwardException()` sends the exception to the debug port. Stub `DbgkForwardException` to return FALSE until TODO-29 §14 lands.

> [!WARNING]
> `KPROCESSOR_MODE` does not exist yet. Define locally in `include/kernel/except.h`: `typedef enum { KernelMode = 0, UserMode = 1 } KPROCESSOR_MODE;`. Canonical definition moves to a shared NT types header when TODO-12 matures.
> `KeBugCheckEx` is implemented in `src/kernel/panic.c` per `TODO-27-crash-dump-generation.md` §1. When `except.c` lands, include `panic.h` (or a forward declaration) and call the shared `KeBugCheckEx` entry point for terminal kernel-mode faults. Do not add a second implementation in `except.c`.

- [ ] `include/kernel/except.h` -- `typedef enum { KernelMode, UserMode } KPROCESSOR_MODE;` (local; moved to shared header later)
- [ ] `src/kernel/except.c`: include `panic.h`; call `KeBugCheckEx(...)` for terminal kernel-mode dispatch (no duplicate body)
- [ ] `src/kernel/except.c` -- `ki_dispatch_exception(rec, ctx, mode, first_chance)` -- master dispatcher
- [ ] `KiDebugRoutine` function pointer -- defaults to NULL (no debugger); set by KD attach (→ XREF: TODO-29 §5)
- [ ] `DbgkForwardException(rec, ctx, first_chance)` -- stub returning FALSE; sends exception to user-mode debug port when TODO-29 §14 lands
- [ ] For user-mode: first-chance debugger → hand off to `KiUserExceptionDispatcher` (§5); accept the `NtRaiseException(first_chance=FALSE)` re-entry → second-chance debugger → terminate. Do NOT call VEH/SEH/VCH from ring 0
- [ ] For kernel-mode: call `KiDebugRoutine` first-chance → kernel SEH (§14) → `KiDebugRoutine` second-chance → `KeBugCheckEx`
- [ ] Pick the right bugcheck (both already in `include/kernel/bugcheck.h`): `BUGCHECK_SYSTEM_SERVICE_EXCEPTION` (0x3B) when the fault happened inside a syscall/SSDT dispatch, else `BUGCHECK_KMODE_EXCEPTION_NOT_HANDLED` (0x1E) -> XREF: `TODO-27 §1`

**Test checkpoint:** With `KiDebugRoutine == NULL`: user-mode access violation routes through VEH → SEH → unhandled filter path (stubs return FALSE until §8-§12 land). With `KiDebugRoutine` set to a test function: first-chance notification fires before VEH. Kernel-mode unhandled exception calls `KeBugCheckEx`. Serial log shows `"except: dispatch user exception code=0x<code>, first_chance=1"`. `POST16(0xDE40)` on dispatcher entry, `POST16(0xDE41)` after dispatch decision. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: implement ki_dispatch_exception with debugger first/second-chance notification"`


---

## 5. KiUserExceptionDispatcher -- Ring-3 Exception Delivery

**Prompt:** Implement `ki_deliver_user_exception(struct interrupt_frame *frame, EXCEPTION_RECORD *rec)` -- called by `ki_dispatch_exception()` (§4) when a user-mode exception is ready for delivery.
This function must:
1. Capture a full `CONTEXT` from `frame` (§1 `context_from_frame`).
2. Allocate space on the **user-mode stack** (`frame->rsp`) by decrementing it by `sizeof(CONTEXT) + sizeof(EXCEPTION_RECORD) + sizeof(EXCEPTION_POINTERS)`, then align to 16 bytes.
3. Copy `EXCEPTION_RECORD` and `CONTEXT` onto the user stack via `try_copy_to_user` (§13) -- NOT a raw kernel `memcpy`. Until per-process page tables land, user stacks are `kmalloc`'d from the kernel heap and share 2 MiB pages with kernel data, so a merely range-valid RSP can alias kernel memory; an unchecked write here is kernel corruption, not a user fault.
4. Patch `frame->rip` to `ntdll!KiUserExceptionDispatcher` (address stored in TEB or PEB).
5. Patch `frame->rsp` to the new user stack pointer.
6. Return from the ISR -- the IRET will land in `KiUserExceptionDispatcher` with `EXCEPTION_POINTERS *` in RCX per the Microsoft x64 ABI.
Add `NtRaiseException(EXCEPTION_RECORD *, CONTEXT *, BOOLEAN)` and `NtContinue(CONTEXT *, BOOLEAN)` syscall stubs to `SSDT` (→ XREF: `TODO-12 §5`): `NtRaiseException` calls `ki_dispatch_exception` (§4) directly; `NtContinue` restores the `CONTEXT` onto the current thread frame and returns to user-mode.

> [!IMPORTANT]
> → XREF: `TODO-11 §7` -- user stack frame layout must match for IRET to succeed.
> → XREF: `TODO-12 §5` -- SSDT must be extended with NtRaiseException/NtContinue entries.

- [ ] `ki_deliver_user_exception()` -- push CONTEXT+EXCEPTION_RECORD on user stack, redirect IRET
- [ ] Reserve the user-stack block with a CHECKED subtraction (reject underflow), `ProbeForWrite` the whole reserved range, then write it ONLY via `try_copy_to_user` (§13) -- never a raw kernel `memcpy` through `frame->rsp`
- [ ] Delivery failure (bad/misaligned/unwritable RSP, or the copy faults) = terminate the process, never `panic_screen()` and never a second fault in exception context
- [ ] `NtRaiseException` SSDT entry -- calls `ki_dispatch_exception()` (§4); `first_chance=TRUE` for a software raise, `FALSE` for the ntdll second-chance re-entry (§4 step 3)
- [ ] `NtContinue` SSDT entry -- validate BEFORE restoring: previous mode, canonical RIP/RSP, user-mode CS/SS selectors, safe RFLAGS bits, honoured `ContextFlags` (§1), and reject a noncontinuable exception; then resume user-mode

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
- [ ] `RtlAddFunctionTable`/`RtlDeleteFunctionTable`/growable-table APIs + `RtlInstallFunctionTableCallback` (the variant JIT engines prefer) for dynamic code with no backing image 💎 -> XREF: TODO-18 §5.
- [ ] SMP lifetime for dynamic tables: sorted non-overlapping lookup, overlap rejection, executable-range validation, deferred reclamation so a delete cannot free a table another CPU is unwinding through
- [ ] `RtlPcToFileHeader(pc, base)` -- public "which module owns this PC" export wrapping `exec_find_module_by_pc()`; crash reporting (§12) and the debugger consume it
- [ ] Unit test: unwind a 3-frame kernel test stack and verify the recovered RIP chain

**Test checkpoint:** `RtlLookupFunctionEntry` for a known kernel function returns a valid `RUNTIME_FUNCTION` with correct `BeginAddress`/`EndAddress`. `RtlVirtualUnwind` on a 3-frame test call chain recovers the correct RIP for each parent frame. Unknown address returns NULL. Serial log: `"rtl: unwind init, <N> .pdata entries registered"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlLookupFunctionEntry and RtlVirtualUnwind for x64 unwind"`


---

## 7. Stack Walking (`RtlCaptureStackBackTrace`, `RtlWalkFrameChain`)

**Prompt:** Implement kernel-mode stack walking using the x64 unwind tables from §6. On Windows, `RtlCaptureStackBackTrace` is the primary API for capturing a stack trace -- it walks the call stack via `RtlVirtualUnwind` and records return addresses. Both user-mode (ntdll) and kernel-mode (ntoskrnl) expose this function. Linux has `stack_trace_save()`. Impossible OS needs the kernel-mode implementation here; the user-mode version is in `TODO-04-ntdll-user-runtime.md`.

- [ ] `include/kernel/rtl/unwind.h` -- declare `RtlCaptureStackBackTrace(skip, count, buffer, hash)`, `RtlWalkFrameChain(callers, count, flags)`
- [ ] `src/kernel/rtl/unwind.c` -- implement `RtlCaptureStackBackTrace`: call `RtlVirtualUnwind` (§6) in a loop, skip `skip` frames, record up to `count` return addresses into `buffer`, compute optional `hash`; max 0xFE frames
- [ ] `src/kernel/rtl/unwind.c` -- implement `RtlWalkFrameChain`: thin wrapper; `flags & 1` = user-mode stack walk (read user RSP/RBP via safe probe §13)
- [ ] IRQL requirement: callable at `IRQL <= DISPATCH_LEVEL` (→ XREF: `TODO-07 §3`)

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
- [ ] `RtlRestoreContext(ctx, rec)` -- the ONE terminal resume primitive shared by the §5 continue-execution path and the end of this unwind; it honours `ContextFlags` (§1) so the two paths cannot diverge
- [ ] `EXCEPTION_COLLIDED_UNWIND`: a `__finally` that itself raises while unwinding must set the flag and resume the ORIGINAL unwind from the collided frame, not restart it
- [ ] CET: when CET_SS is active, unwinding N frames must advance the shadow-stack pointer by N (INCSSP-equivalent) so the next `ret` does not raise a fresh #CP -> XREF: `TODO-10 §9`

**Test checkpoint:** `RtlUnwindEx` from a 3-frame stack to the target frame invokes `__finally` in each intermediate frame. `EXCEPTION_UNWINDING` flag is set on the `EXCEPTION_RECORD` during the walk and cleared after. Target frame receives `return_value` in RAX. Serial log: `"rtl: unwind to frame 0x<target>, <N> finally handlers invoked"`. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement RtlUnwindEx with termination handler invocation"`


---

## 10. Vectored Exception Handlers (VEH)

**Prompt:** Establish the kernel side of the Vectored Exception Handler contract. VEH is a per-process, ring-3 mechanism: ntdll anchors a doubly-linked list of `VECTORED_EXCEPTION_ENTRY` nodes at `TEB.VehListHead` and walks it from `RtlDispatchException` **before** the SEH scope-table walk (§8), after the kernel has delivered the fault via `KiUserExceptionDispatcher` (§5). The kernel's only job here is to publish the shared node/disposition ABI and reserve the anchor field so the two sides agree; the list itself is never read or walked from ring 0.

> [!WARNING]
> **Ownership: the VEH list is RING-3 state.** Windows keeps it in ntdll and walks it from `RtlDispatchException`; the kernel never calls a VEH handler. `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5` owns the list, the add/remove APIs, and the walk (item: "**VEH list**: doubly-linked list of `VECTORED_HANDLER_ENTRY` nodes anchored at `TEB.VehListHead`"). This section therefore does NOT put a VEH head in `struct task` or call handlers from `ki_dispatch_exception()`; it ships only what ring 3 needs from the kernel.

> [!IMPORTANT]
> → XREF: `TODO-05 §3` -- VEH handles are not Win32 kernel handles; use a simple opaque pointer. No overlap with the Object Manager handle table.

- [ ] `include/kernel/except.h` -- `VECTORED_EXCEPTION_ENTRY` node layout + `EXCEPTION_CONTINUE_EXECUTION`/`EXCEPTION_CONTINUE_SEARCH` disposition constants, as the shared kernel/ntdll ABI contract
- [ ] `_Static_assert` the node layout so ntdll's list (D12 T04 §5) and any kernel-side introspection agree byte-for-byte
- [ ] Reserve `TEB.VehListHead` (the ring-3 list anchor) -> XREF: `TODO-11 §6` owns the TEB field
- [ ] Scope boundary: `RtlAddVectoredExceptionHandler` / `RtlRemoveVectoredExceptionHandler` / the walk are implemented in ntdll -> XREF: `12-user-platform-sdk/TODO-04 §5`; NOT in `src/kernel/rtl/veh.c`

**Test checkpoint:** `_Static_assert`s pin `VECTORED_EXCEPTION_ENTRY` size/offsets and the disposition constants (`EXCEPTION_CONTINUE_EXECUTION=-1`, `EXCEPTION_CONTINUE_SEARCH=0`); a unit test asserts the kernel's view of the node matches the ntdll view byte-for-byte. `TEB.VehListHead` exists at its pinned offset and is zero-initialised at thread create. Grep proves NO kernel call site walks the VEH list (`ki_call_veh_list` must not exist). Behavioural VEH tests live with the ntdll implementation (D12 T04 §5). Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement Vectored Exception Handler (VEH) list"`


---

## 11. Vectored Continue Handlers (VCH)

**Prompt:** Implement the Vectored Continue Handler list -- a separate mechanism from VEH (§10). On Windows, VCH handlers are called **after** a frame-based (SEH) handler has been found and has decided to continue execution, but **before** execution actually resumes. This allows monitoring/logging handlers to observe that an exception was handled without interfering with the dispatch. The VCH list uses the same `VECTORED_EXCEPTION_ENTRY` node type as VEH but is stored in a separate list head.

Like VEH (§10), all three ring-3 pieces -- `AddVectoredContinueHandler`, `RemoveVectoredContinueHandler`, and the post-SEH walk -- are implemented in ntdll (D12 T04 §5). The kernel contributes only the second list anchor and the shared node ABI.

> [!NOTE]
> VCH is distinct from VEH. VEH runs BEFORE frame-based handlers; VCH runs AFTER. Both are per-process and both live in ring 3 (§10 ownership note applies verbatim). Full order: kernel debugger first-chance (§4) → [ring 3: VEH → SEH → VCH] → kernel second-chance via `NtRaiseException` re-entry (§4) → terminate.

- [ ] Reserve a SECOND ring-3 list anchor for VCH, distinct from `TEB.VehListHead`, reusing the §10 node type -> XREF: `TODO-11 §6` owns the TEB field
- [ ] Scope boundary: `AddVectoredContinueHandler` / `RemoveVectoredContinueHandler` and the post-SEH walk are ntdll-side -> XREF: `12-user-platform-sdk/TODO-04 §5`; no `ki_call_vch_list` in ring 0

**Test checkpoint:** The VCH anchor exists at its pinned TEB offset, is distinct from `TEB.VehListHead`, and is zero-initialised at thread create; a unit test asserts both anchors reuse the same `VECTORED_EXCEPTION_ENTRY` node ABI. Grep proves no `ki_call_vch_list` exists in ring 0. Behavioural VCH ordering ("fires only after SEH succeeds") is tested with the ntdll implementation (D12 T04 §5). Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement Vectored Continue Handler (VCH) list"`


---

## 12. Unhandled Exception Filter and WER Hook

**Prompt:** Implement the terminal path reached when `ki_dispatch_exception()` (§4) exhausts all handlers (VEH declined, SEH returned FALSE, debugger second-chance declined, VCH had no effect). The dispatch reaches this section only if no handler claimed the exception. Steps:
The top-level filter itself is **ring-3 state** and is owned by ntdll (D12 T04 §5). This section owns the kernel terminal: what happens when ring 3 declines everything (or cannot run at all), re-entering via `NtRaiseException(first_chance=FALSE)` (§4 step 3). Steps:
1. Second-chance debugger notification (§4), then terminate the process with the exception code as exit status.
2. Emit a structured log entry (→ XREF: `TODO-04 §6`) with the full `EXCEPTION_RECORD` and the first 8 frames via `RtlCaptureStackBackTrace` (§7).
Add a WER (Windows Error Reporting) stub: `WerpReportFault()` calls into a future `werfault.exe` process via a named pipe (leave as a no-op stub for now, log to serial).

> [!WARNING]
> **There is no `PEB.UnhandledExceptionFilter` field -- do not add one.** On Windows `SetUnhandledExceptionFilter` stores an `EncodePointer`-obfuscated pointer in the kernel32 global `BasepCurrentTopLevelFilter`, NOT in the PEB. Inventing a PEB field would also collide with `TODO-11 §17`, which rebuilds the post-0x28 PEB region to authoritative x64 `_PEB` offsets so a real ntdll can read it unpatched. The Impossible OS analogue is a user-runtime process-global in ntdll -> XREF: `12-user-platform-sdk/TODO-04 §5`.

- [ ] Kernel terminal path: on `NtRaiseException(first_chance=FALSE)` re-entry, second-chance debugger → terminate with the exception NTSTATUS as exit status
- [ ] Terminate cleanly when ring 3 CANNOT be reached at all (no ntdll mapped, or §5 delivery failed) -- this is the only path that must not depend on user state
- [ ] Structured crash log: exception code, fault address, top-8 frames via `RtlCaptureStackBackTrace` (§7)
- [ ] `WerpReportFault()` stub -- serial log only for now
- [ ] Scope boundary: `SetUnhandledExceptionFilter` / `UnhandledExceptionFilter` / the crash dialog are ntdll-side -> XREF: `12-user-platform-sdk/TODO-04 §5`

**Test checkpoint:** An access violation that ring 3 declines re-enters via `NtRaiseException(first_chance=FALSE)` and terminates: `WerpReportFault()` logs `"wer: fault report code=0xC0000005, addr=0x<addr>"` plus an 8-frame stack trace, and the process exit code is the exception NTSTATUS. A fault with §5 delivery deliberately failed (bad user RSP) still terminates the process without a kernel panic. Grep proves no `PEB.UnhandledExceptionFilter` field was added. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"rtl: implement unhandled exception filter, WER stub, and crash log"`


---

## 13. Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`)

**Prompt:** Repair and complete the EXISTING kernel probing API so a ring-3 pointer cannot crash or corrupt the kernel. `ProbeForRead`/`ProbeForWrite` already exist (declared `include/kernel/nt/zw.h:57-58`, implemented `src/kernel/nt/ssdt.c:60` and `:95`), and `ProbeForWrite` is literally `return ProbeForRead(...)` -- a range/alignment/overflow check with no page touch, so it cannot prove writability. Extend those symbols IN PLACE; do NOT create a second `probe.h`/`probe.c` pair with the same names. What is missing:
- `ProbeForWrite` page-touch: touch the first byte of each page to force a present+writable mapping and catch write-protected pages.
- `try_copy_from_user(dst, src, n)` / `try_copy_to_user(dst, src, n)` -- fault-recoverable copies (Linux `__ex_table` in spirit). A per-CPU `safe_return_rip` slot; the #PF handler (§2) checks it and redirects to the safe-return path when a fault lands inside a guarded copy. This closes the TOCTOU between probe and dereference that a range check alone cannot.

> [!IMPORTANT]
> → XREF: `TODO-07 §2` -- the per-CPU safe_return_rip slot is CPU-local data, co-located with the IRQL tracking fields.

> [!WARNING]
> **The range check is currently the ONLY user/kernel separation, and it is not sufficient on its own.** There are no per-process page tables yet: user stacks are `kmalloc`'d from the kernel heap and share 2 MiB pages with kernel data, and SMEP/SMAP stay off until that lands (`docs/infrastructure/bare-metal-gotchas.md`). So an address below `MM_USER_PROBE_ADDRESS` can still alias kernel memory, and a probe-passing write can corrupt the kernel rather than fault. Treat `try_copy_*` fixup as the real safety boundary; full isolation is owned by the per-process page-table work -> XREF: `TODO-10 §2`.

- [ ] Extend `include/kernel/nt/zw.h` + `src/kernel/nt/ssdt.c` in place: add `try_copy_from_user` / `try_copy_to_user` beside the existing probes (no duplicate `ProbeForRead`/`ProbeForWrite` symbols anywhere)
- [ ] `ProbeForWrite`: stop delegating to `ProbeForRead`; add the per-page write touch so it actually proves writability
- [ ] `safe_return_rip` slot in the CPU-local area; #PF triage (§2) checks it and redirects on a guarded-copy fault
- [ ] Migrate user-pointer dereferences behind `try_copy_*` (existing `copy_from_user` at `include/kernel/cpu_security.h:266` has no fixup) -> XREF: `TODO-21 §11` + `TODO-05 §11` already defer on exactly this gap

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
> → XREF: `TODO-07 §3` -- kernel `__try` must only be used at `PASSIVE_LEVEL` or `APC_LEVEL`; add `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` assertion at the start of `ki_raise_kernel_exception`.

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
Deliver via the existing `task->signals` mechanism. If the signal has a handler registered via `rt_sigaction` (→ XREF: `10-platform-services/TODO-10-linux-compat.md §8`), set up a signal frame on the user stack with a `siginfo_t` containing fault details and redirect execution to the handler. If the signal has no handler (default disposition), terminate the task with an appropriate `NTSTATUS` exit code.
If the process has set up a `sigaltstack`, deliver `SIGSEGV` on the alternate stack to handle stack overflow correctly.
This section is gated on the Linux compat layer existing -- stub it out with a compile-time flag `CONFIG_LINUX_COMPAT` for now.

> [!IMPORTANT]
> → XREF: `TODO-21 §6` (process capabilities) -- compat mode is a process flag.
> → XREF: `10-platform-services/TODO-10-linux-compat.md §8` -- `rt_sigaction` handler registration. This section handles the kernel-side fault-to-signal mapping and signal frame setup; the compat layer TODO handles the `rt_sigaction`/`rt_sigprocmask` API surface.
> Delivery path hooks into §12 (unhandled exception filter) as a pre-termination step.

- [ ] `include/kernel/compat.h` -- `CONFIG_LINUX_COMPAT` guard, fault-to-signal table
- [ ] `src/kernel/compat/signal_compat.c` -- `ki_deliver_compat_signal()` -- fault-to-signal translation
- [ ] `siginfo_t` population: `si_signo`, `si_code` (e.g., `SEGV_MAPERR`, `SEGV_ACCERR`), `si_addr` (fault address)
- [ ] `sigaltstack` support: if `task->sigaltstack` is set and signal is `SIGSEGV`, deliver on the alternate stack
- [ ] Return path: `rt_sigreturn` is ALREADY owned by `D10 T10 §8` (item: "User-mode signal frame: ... on `sigreturn(15)` → restore saved frame"); guarantee the frame pushed here is the shape that item restores, do not add a second sigreturn
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
- [ ] JSON format compatible with TODO-04 §6 structured events; event type `"exception_dispatch"`
- [ ] Rate limiting: max 100 exception dispatch logs per second per process to prevent log flooding from intentional exceptions (e.g., guard page probing)
- [ ] Compile-time `CONFIG_EXCEPT_TELEMETRY` guard (default: enabled in debug builds, disabled in release)

**Test checkpoint:** Trigger a user-mode access violation → serial log contains JSON event: `{"type":"exception_dispatch","code":"0xC0000005","handler":"veh","disposition":"CONTINUE_SEARCH"}` (or similar). Rate limiting: trigger 200 exceptions in rapid succession -- log shows ≤100 entries. `CONFIG_EXCEPT_TELEMETRY=0` build: no telemetry log entries emitted. Test on: QEMU WHPX + TCG.

- [ ] Commit: `"kernel: add exception dispatch telemetry to JSON structured log"`


---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11           | 🐧 Linux          | 🚀 Impossible OS       |
| --- | ----------------------------- | ----------------- | ---------------- | --------------------- |
| 💎   | EXCEPTION_RECORD/CONTEXT      | ✅ ntdll           | ❌                | ⬜ §1                  |
| 💎   | #PF user/kernel triage        | ✅                 | ✅                | ⚠️ §2 kernel-only     |
| 💎   | #DB/#BP debugger routing      | ✅                 | ✅ ptrace         | ⬜ §3                  |
| 💎   | #CP CET shadow-stack          | ✅ 24H2+           | ✅ 6.6+           | ⬜ §3 conditional      |
| 💎   | Debugger 1st/2nd-chance       | ✅ KiDebugRoutine  | ✅ ptrace         | ⬜ §4                  |
| 💎   | KiUserExceptionDispatcher     | ✅                 | ❌                | ⬜ §5                  |
| 💎   | x64 table-based unwind        | ✅ UNWIND_INFO     | ✅ .eh_frame      | ⬜ §6                  |
| 💎   | Kernel stack walking          | ✅ RtlCaptureStack | ✅ stack_trace    | ⬜ §7                  |
| 💎   | SEH + __C_specific_handler    | ✅                 | ❌                | ⬜ §8                  |
| 💎   | RtlUnwindEx + __finally       | ✅                 | ❌                | ⬜ §9                  |
| 💎   | VEH list                      | ✅ ntdll           | ❌                | ⬜ §10 ABI, D12T04 §5  |
| 💎   | VCH list                      | ✅ ntdll           | ❌                | ⬜ §11 ABI, D12T04 §5  |
| 💎   | Unhandled exception filter    | ✅ WER             | ✅ core dump      | ⬜ §12 kernel terminal |
| 💎   | `__fastfail` / INT 0x29       | ✅ 0xC0000409      | ❌                | ⬜ §3 vector 41        |
| 💎   | CONTEXT ContextFlags + FXSAVE | ✅ 0x4E0 ABI       | ✅ ucontext_t     | ⬜ §1                  |
| 💎   | Fault-recoverable usercopy    | ✅ kernel SEH      | ✅ `__ex_table`   | ⬜ §13 try_copy_*      |
| ⭐   | IRQL-aware safe probing       | ✅ ProbeForRead    | ✅ copy_from_user | ⬜ §13                 |
| 💎   | Kernel __try/__except         | ✅                 | ❌                | ⬜ §14                 |
| 💎   | POSIX signal from faults      | ❌                 | ✅                | ⬜ §15 compat          |
| 💎   | sigaltstack overflow          | ❌                 | ✅                | ⬜ §15 compat          |
| ⭐   | Dispatch telemetry            | ❌                 | ❌                | ⬜ §16 JSON log        |
| ⭐   | Exception budget / storm ctrl | ❌                 | ❌                | ⬜ §16 rate-limit ext  |

> **After parity items:** Impossible OS matches Windows on the full SEH/VEH/VCH pipeline and matches Linux on POSIX signal delivery. Exclusive differentiators: **dispatch telemetry** recording the full VEH → SEH → VCH handler chain into the JSON structured log (neither WER nor core dumps capture the decision sequence); **IRQL-aware safe probing** co-locating `safe_return_rip` with IRQL fields for zero-overhead probe checks in the #PF handler; and **exception storm control** rate-limiting per-process exceptions to prevent DoS from runaway JITs or intentional exception flooding.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_except()`.
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Add `TEST_CAT_EXCEPT` + its `"except"` short name to `include/kernel/test/test.h`; register every case below via `test_suite_register_cat(..., TEST_CAT_EXCEPT)`
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
  - `VECTORED_EXCEPTION_ENTRY` node size/offsets and disposition constants match the ntdll ABI (`_Static_assert` + runtime check); VEH and VCH anchors are distinct and zero-initialised
  - No ring-0 VEH/VCH walker exists: `ki_call_veh_list` / `ki_call_vch_list` are absent (registration/dispatch behaviour is tested with the ntdll implementation, D12 T04 §5)
  - `CONTEXT.ContextFlags` gates restoration: a CONTEXT with FLOATING_POINT clear leaves XMM state untouched on `NtContinue`
  - `NtContinue` rejects a non-canonical RIP, a kernel-mode CS selector, and a noncontinuable exception
  - `ProbeForWrite` on a read-only user page fails (proves the page touch, not just the range check)
  - `try_copy_from_user` from an unmapped page returns an error via the `safe_return_rip` fixup and the kernel keeps running
  - Kernel `__try`/`__except` around a guarded region: exception handler fires and kernel continues
- [ ] Register in `test_runner_init()`: `test_register_except()`
- [ ] Commit: `"test: add exception dispatch and SEH test suite"`

---

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

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | N suites, 0 failures
