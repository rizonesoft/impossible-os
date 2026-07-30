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
> **WHOLE-FILE BLOCKER CLEARED 2026-07-17 -- this file is UNPARKED.** Every section here was gated on the kernel-BSS/`USER_BASE` ceiling: `.bss` ended at `0x7FEE55`, 4523 bytes under `USER_BASE` `0x800000`, so `scripts/build.sh`'s BSS-collision guard failed any section adding more than ~4 KiB of `.text`/`.rodata`. `02-kernel-core/TODO-33 §10` converted the large static pools to frame-backed storage and the BSS end moved to `0x6c2000` -- **~1272 KiB of headroom**, ~280x what this file's §1 needed. The 16 ceiling `Deferred` stamps were swept and 15 of 16 rows reset to `[ ]`; the file now runs on its own Implementation Order, §1 first. **§15 stays `[/]`** -- the ceiling was not its only blocker (its `D10T10 §8` Linux-compat signal prerequisite is open), and §3/§4 additionally depend on the open `TODO-29 §5`, so resolve each row's "Depends On" column before starting it -> XREF: `02-kernel-core/TODO-33 §11` (item: "`TODO-23 §1-§16` (exception/SEH)").
>
> **§1 SHIPPED 2026-07-18.** The parked `git stash` `todo23-s1-wip` was applied, re-verified against the moved HEAD, re-reviewed adversarially (alignment ABI fix adopted; cross-task panic-FPU leak filed to `TODO-27 §2`), built `-Werror`, and committed. `except.h` / `except.c` / `test_except.c` are in tree; §2 onward can build on the CONTEXT converters.

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

- `include/kernel/except.h` -- `EXCEPTION_RECORD`, `CONTEXT`, `EXCEPTION_POINTERS`, NTSTATUS fault codes, `KI_EXCEPTION_REGISTRATION`, `VECTORED_HANDLER_ENTRY` node ABI (the VEH/VCH list heads themselves are ntdll process-global state, not kernel fields)
- `src/kernel/except.c` -- `context_from_frame()`, `frame_from_context()`, `except_init()`, fault ISR handlers (vectors 0, 1, 3, 4, 6, 11, 12, 13, 21), `ki_dispatch_exception()` (master dispatcher with debugger first/second-chance), `ki_raise_kernel_exception()`, `except_log_dispatch()` (telemetry)
- `src/kernel/mm/vmm.c` -- `page_fault_handler()` triages #PF into user vs. kernel paths with EXCEPTION_RECORD capture
- `src/kernel/rtl/unwind.c` + `include/kernel/rtl/unwind.h` -- `RtlLookupFunctionEntry`, `RtlVirtualUnwind`, `RtlUnwindEx`, `RtlCaptureStackBackTrace`, `RtlWalkFrameChain`
- `src/kernel/rtl/seh.c` -- kernel-mode SEH SCOPE_TABLE walker (§14) + `WerpReportFault()` stub. The ring-3 `RtlDispatchException` / `__C_specific_handler` / top-level filter are ntdll-side -> `12-user-platform-sdk/TODO-04 §5`
- `include/kernel/except.h` -- `VECTORED_HANDLER_ENTRY` node + `PVECTORED_EXCEPTION_HANDLER` + disposition ABI shared with ntdll (the VEH/VCH lists themselves are ring-3 process-global state, not kernel state)
- `include/kernel/nt/zw.h` + `src/kernel/nt/ssdt.c` -- EXISTING `ProbeForRead`/`ProbeForWrite` extended in place (page-touch) plus new `try_copy_from_user` / `try_copy_to_user` fault-fixup copies. No new `probe.h`/`probe.c`
- `src/kernel/compat/signal_compat.c` -- `ki_deliver_compat_signal()` (Linux compat, `CONFIG_LINUX_COMPAT` guarded), `sigaltstack` support
- `NtRaiseException` and `NtContinue` registered in SSDT (→ `TODO-12-native-api-ssdt.md §5`)
- `KiDebugRoutine` function pointer and `DbgkForwardException()` stub for debugger integration (→ `TODO-29`)

---

## Implementation Order

| ⭐   | Order | Deliverable                                                        | Depends On                 | Status |
| --- | :---: | ------------------------------------------------------------------ | -------------------------- | :----: |
| 💎   |   1   | EXCEPTION_RECORD, CONTEXT, EXCEPTION_POINTERS                      | TODO-12 §1, TODO-33 §10    |  [x]   |
| 💎   |   2   | #PF triage -- user/kernel decode, EXCEPTION_RECORD build, routing  | §1, TODO-07 §3             |  [/]   |
| 💎   |   3   | Fault-to-exception mapping (#DE/#DB/#BP/#OF/#UD/#NP/#SS/#GP/#CP)   | §1, TODO-10 §9, TODO-29 §5 |  [/]   |
| 💎   |   4   | Debugger first-chance / second-chance notification                 | §1-§3, TODO-29 §5          |  [/]   |
| 💎   |   5   | KiUserExceptionDispatcher -- ring-3 delivery                       | §4, §13, TODO-11 §7        |  [/]   |
| 💎   |   6   | x64 table-based unwind (.pdata, RtlVirtualUnwind)                  | §1                         |  [x]   |
| 💎   |   7   | Stack walking (RtlCaptureStackBackTrace)                           | §6, TODO-07 §3             |  [/]   |
| 💎   |   8   | SEH chain walk + `__C_specific_handler` (re-owned ring-3 → T04 §5) | §5, §6, TODO-11 §6         |  [/]   |
| 💎   |   9   | RtlUnwindEx -- unwind to target frame                              | §6, §8                     |  [/]   |
| 💎   |  10   | Vectored Exception Handlers (VEH)                                  | §4, TODO-05 §3             |  [x]   |
| 💎   |  11   | Vectored Continue Handlers (VCH)                                   | §4, §10                    |  [x]   |
| 💎   |  12   | Unhandled exception filter + WER hook                              | §7, §8, §10                |  [/]   |
| ⭐   |  13   | Kernel safe probing (ProbeForRead/Write)                           | §2                         |  [x]   |
| 💎   |  14   | Kernel-mode `__try`/`__except` for drivers                         | §6, §9, §13                |  [/]   |
| 💎   |  15   | POSIX signal delivery from exceptions (Linux compat)               | §3, §5, D10T10 §8          |  [/]   |
| ⭐   |  16   | Exception dispatch telemetry                                       | §4, TODO-04 §6             |  [/]   |
| 💎   |  17   | Guard-page stack auto-grow (split from §2; land right after §2)    | §5, TODO-07 §3, TODO-01 §3 |  [/]   |

> 💎 = parity -- Windows implements this feature; Impossible OS must match.
> ⭐ = exclusive -- not present in either Windows or Linux at the kernel level.

---

## 1. EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS Types

**Prompt:** Define the three canonical Windows exception types in a new header `include/kernel/except.h`. `EXCEPTION_RECORD` holds the exception code (`EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_ILLEGAL_INSTRUCTION`, etc.), exception flags (`EXCEPTION_CONTINUABLE`), a nested-exception pointer, the fault address, and up to `EXCEPTION_MAXIMUM_PARAMETERS` (15) information parameters.
`CONTEXT` holds the full x86-64 register file (all GP registers, RIP, RFLAGS, CS/DS/ES/FS/GS/SS, XMM0-15, FPU control/status). `EXCEPTION_POINTERS` pairs the two. Add NTSTATUS codes for the common faults (`STATUS_ACCESS_VIOLATION 0xC0000005`, `STATUS_ILLEGAL_INSTRUCTION 0xC000001D`, `STATUS_INTEGER_DIVIDE_BY_ZERO 0xC0000094`, etc.).
Add `context_from_frame(struct interrupt_frame *f, CONTEXT *ctx)` to populate a CONTEXT from the ISR frame.

> [!IMPORTANT]
> → XREF: `TODO-12 §1` -- SATISFIED: `include/kernel/nt/ntstatus.h` is the canonical `NTSTATUS` home (`typedef int32_t NTSTATUS` at `:21`). `except.h` includes it; exception status codes are added THERE, never re-declared here.
> → XREF: `TODO-33 §10` -- CLEARED: the kernel BSS / `USER_BASE` ceiling that blocked linking was resolved (frame-backed pools moved BSS end to `0x6c2000`); §1 now builds and links.

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

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 17 suites, 0 failures

- [x] Commit: `"kernel: add EXCEPTION_RECORD, CONTEXT, and EXCEPTION_POINTERS types"`

> **Notes:**
> - **What shipped** -- `include/kernel/except.h` (EXCEPTION_RECORD/CONTEXT/EXCEPTION_POINTERS + `CONTEXT_HAS_GROUP` macro, offsets `_Static_assert`-pinned, `aligned(16)`), `src/kernel/except.c` converters, `test_except.c` (16 suites).
> - **How it integrates** -- frame-backed CONTROL+INTEGER only, returning the mask actually captured; `panic.h` now includes `except.h` and `panic_build_context()` delegates its frame fill to `context_from_frame`.
> - **Downstream effects** -- satisfies `TODO-27`'s T23-§1 CONTEXT reconciliation (single source of truth); adversarial review adopted a 16-byte alignment fix and filed the cross-task panic-FPU dump leak to `TODO-27 §2`.
> - **Canonical doc** -- the `include/kernel/except.h` header block (the exception-ABI owner).
> - **Scope boundary** -- §1 owns the ABI types + frame<->CONTEXT converters; per-thread FPU/segment/debug capture is `§5` (NtGetContextThread); the ring-3 dispatch half is `TODO-04 §5`.
>
> **Verified:** 2026-07-18 | ship `b8d48470` + review fixes | 7/7 items | build OK | tests 17/17 PASS; smoke PASS (KVM 3.25s)
> **Accepted:** [H] panic FPU capture can record a prior task's SIMD state under lazy FPU, and lacks an FCW/MXCSR fallback if a caller skips the capture -> XREF: 02-kernel-core/TODO-27 §2 (item: "Cross-task FPU dump leak: sample CR0.TS ..." at line 116)
> **Quality reviewed:** 2026-07-18 | Codex 3x (adversarial, consistency, perf) + kernel-quality-auditor + parity-analyst | 2M fixed, 1H accepted-XREF | scope: kernel-code-quality


---

## 2. #PF Triage -- User vs. Kernel Decode, EXCEPTION_RECORD/CONTEXT Build, Dispatch Routing

> **Split note (2026-07-18):** Guard-page / stack auto-grow (commit-vs-reserve tracking, guard re-arm, terminal `STATUS_STACK_OVERFLOW`) was split out to **§17** so this section stays one worker context (SPLIT-RECOMMENDED: ABI impact + 8 items). This section owns error-code decode, the user/kernel decision tree, building `EXCEPTION_RECORD`/`CONTEXT`, and routing to the dispatch stubs; §17 hooks its growable-stack case into the not-present-user branch below.

**Prompt:** Rewrite `page_fault_handler()` in `vmm.c` to triage before panicking.
Decode `CR2` (fault address) and the error code bits (P=present, W=write, U=user, I=fetch, PK=prot-key, SS=shadow-stack). Decision tree:
- **Kernel fault, not present** → existing swap/mmap chain (unchanged).
- **Kernel fault, protection violation** → route through `ki_dispatch_exception(..., KernelMode, ...)` -- the master dispatcher owns the `ki_raise_kernel_exception()` (§14) call and terminal bugcheck/panic; falls back to `panic_screen()` if not inside a probed region (§13).
- **User fault, not present** → existing swap/mmap chain; else hand a growable-stack guard hit to `vmm_try_grow_stack()` (§17); else deliver `STATUS_ACCESS_VIOLATION` via `KiUserExceptionDispatcher` (§5). Until §17 lands, a guard hit keeps the current labeled panic.
- **User fault, protection violation** → deliver `STATUS_ACCESS_VIOLATION` to user-mode.
- **User fetch fault** → deliver `STATUS_ACCESS_VIOLATION` with `ExceptionInformation[0]=8` (execute).
Extract the CONTEXT (§1) and fault address before deciding. Mark the old behavior (always panic) as the unreachable final clause.

> [!IMPORTANT]
> → XREF: `TODO-07 §3` -- interrupt entry raises IRQL; fault handler runs at device IRQL.
> → XREF: `TODO-11 §5` -- `task->peb` needed to find user-mode VMM range boundary.
> → XREF: `D03 T04 §4` -- lazy mapped-file and swap-backed page-in live in the pager TODO; this section routes eligible not-present faults there rather than re-implementing pager policy in the exception layer.
> → XREF: `§17` -- guard-page stack auto-grow (`vmm_try_grow_stack()`) is the split-off owner of the growable-stack case in the not-present-user branch.

- [x] Decode error-code bits (named `PF_EC_*` per Intel SDM) and distinguish user vs. kernel fault via the U bit (`page_fault_handler`, `vmm.c`)
- [x] Build `EXCEPTION_RECORD` + a CONTROL/INTEGER `CONTEXT` in cache-line-aligned per-CPU scratch (`pf_exc_scratch[MAX_CPUS]`, `in_use` guard) not on the #PF stack; pure `pf_build_access_violation()`
- [x] Route BOTH modes through `ki_dispatch_exception(rec, ctx, frame, mode, first_chance)` (mode = U bit); resume via IRET on `KI_EXCEPTION_HANDLED`, else terminal panic
- [x] `ki_raise_kernel_exception(rec, ctx, frame)` stub declared for §14 (called BY the dispatcher, not the #PF handler); `klog` only on the user terminal path (kernel may hold `s_klog_lock`)
- [x] BUG (found in §3, fixed in §13): `#PF` re-registered in `boot_phase1` after `except_init` -- `idt_init` zeroed the phase-0 `handlers[14]` (live #PF hit the generic panic); §13's fixup needs the reachable handler

**Test checkpoint:** Trigger a user-mode NULL dereference -- serial log shows `"pf: user fault at 0x0 code=0x<ec>"` and routes to `ki_dispatch_exception()` (stub returns UNHANDLED, handler panics until §5 ring-3 delivery lands). Trigger a kernel-mode swap fault -- existing guard/swap/mmap chain still handles it correctly (COW is a present-bit write -- pager chain runs unchanged, not gated on not-present). A guard-page hit keeps the current labeled panic (§17 replaces it with auto-grow). No POST16 on this post-Phase-3 runtime fault path (Codex design review: POST16 is boot-path telemetry, unobservable after desktop start); user-path diagnostics use `klog`, kernel-path uses the fault-safe `panic_screen`. Test on: QEMU WHPX + TCG. Verify on bare metal -- #PF error code bits may differ.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 5 new suites (pf write / read+fetch decode, CONTEXT zeroing, stub disposition, kernel-CPL capture), 0 failures

- [x] Commit: `"mm: triage #PF into user/kernel paths; defer to exception dispatch"`

> **Notes:**
> - **What shipped** -- `page_fault_handler` triage rewrite (`vmm.c`): decode `PF_EC_*`, unchanged guard/swap/mmap chain, then build an access-violation record in per-CPU scratch and route by U bit; pure `pf_build_access_violation()` + 5 unit tests.
> - **How it integrates** -- routes BOTH modes through the `except.h` dispatch ABI (`ki_dispatch_exception` returns `KI_EXCEPTION_DISPOSITION`, live frame passed); ships UNHANDLED stubs, so unresolved faults stay terminal.
> - **Downstream effects** -- lands `KPROCESSOR_MODE` + the dispatch decls §4/§5/§14 build on; §17 hooks stack auto-grow into the not-present-user branch. Codex design + review adoptions in the commit messages.
> - **Canonical doc** -- the `include/kernel/except.h` dispatch-ABI block (the exception-dispatch contract owner).
> - **Scope boundary** -- §2 owns triage + record build + dispatch routing; ring-3 delivery is §5, kernel SEH is §14, stack auto-grow is §17, safe probing is §13.
>
> **Verified:** 2026-07-18 | ship `eda0aea7` + review fixes | 4/4 items | build OK | smoke PASS (KVM 2.64s)
> **Accepted:** [H] §14 `ki_raise_kernel_exception` IRQL check must be fault-safe (non-`klog`) -- a #PF at elevated IRQL inside klog would deadlock on `s_klog_lock` -> XREF: 02-kernel-core/TODO-23 §14 (item: "replace the §2 UNHANDLED stub with the real `ki_raise_kernel_exception(rec, ctx, frame)` body; fault-safe (non-`klog`) IRQL check" at line 173)
> **Quality reviewed:** 2026-07-18 | Codex 7x (adversarial, consistency, perf, re-adversarial) | 2H+4M fixed, 1H accepted-XREF, 1H rejected (long-mode always pushes SS:RSP) | scope: kernel-code-quality


---

## 3. General Fault-to-Exception Mapping (#DE, #DB, #BP, #OF, #UD, #NP, #SS, #GP, #CP)

**Prompt:** Add per-exception ISR handlers for the CPU fault vectors that should not always panic. Register handlers via `idt_register_handler()` in a new `except_init()` called from `kernel_main`. Mapping:
- Vector 0 (`#DE`) → `STATUS_INTEGER_DIVIDE_BY_ZERO` if user-mode, panic if kernel.
- Vector 1 (`#DB`) → `STATUS_SINGLE_STEP`. Route through `ki_dispatch_exception()` (§4) which notifies the debugger first-chance; if no debugger or debugger declines, deliver to user-mode as `STATUS_SINGLE_STEP` via `KiUserExceptionDispatcher` (§5). Kernel-mode `#DB` routes to KD (→ XREF: `TODO-29 §5`).
- Vector 3 (`#BP`) → `STATUS_BREAKPOINT`. Adjust `frame->rip -= 1` (INT3 is 1 byte). Route through `ki_dispatch_exception()` (§4) for debugger first-chance; if unhandled, deliver to user-mode. Kernel-mode `#BP` routes to KD (→ XREF: `TODO-29 §5`).
- Vector 4 (`#OF`) → `STATUS_INTEGER_OVERFLOW` if user.
- Vector 6 (`#UD`) → `STATUS_ILLEGAL_INSTRUCTION` if user.
- Vector 11 (`#NP`) → `STATUS_ACCESS_VIOLATION` (segment not present) if user.
- Vector 12 (`#SS`) → `STATUS_STACK_OVERFLOW` if user (raw stack-segment fault; the guard-page stack-overflow path via #PF is §17, the primary `STATUS_STACK_OVERFLOW` producer).
- Vector 13 (`#GP`) → `STATUS_ACCESS_VIOLATION` if user (well-formed 2-param AV record; cause-aware refinement below); kernel = probe check first, then panic.
- Vector 21 (`#CP`) → noncontinuable `STATUS_STACK_BUFFER_OVERRUN` + fast-fail subcode 0x39 (`FAST_FAIL_CONTROL_INVALID_RETURN_ADDRESS`), matching real Windows shadow-stack delivery (NOT a dedicated `STATUS_CONTROL_STACK_VIOLATION`). User-mode: via `KiUserExceptionDispatcher` (§5). Kernel-mode: `KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE, 0x39, ...)`. Only register if `cpu_has(CPU_FEATURE_CET_SS)` or `cpu_has(CPU_FEATURE_CET_IBT)` -- both raise #CP (→ XREF: `TODO-10 §9`, `§10`).
Kernel-mode faults for vectors 0, 4, 6 always call `panic_screen()` (no recovery).
Each handler builds an `EXCEPTION_RECORD` (§1) and, for the recoverable paths, routes through `ki_dispatch_exception()` (§4) based on CPL in the saved CS. The always-terminal cases bypass the dispatcher: kernel-mode #DE/#OF/#UD `panic_screen()` immediately and kernel-mode #CP calls `KeBugCheckEx()` directly (user-mode #CP still routes through the dispatcher).

> [!IMPORTANT]
> → XREF: `TODO-07 §3` -- ISR entry/exit must preserve IRQL contract.
> → XREF: `TODO-29 §5` -- #DB/#BP handlers must coexist with KD. If KD is attached, `ki_dispatch_exception()` calls `KiDebugRoutine` first-chance. If KD is not present or declines, dispatch continues to VEH/SEH. TODO-29 §5 registers raw handlers; when TODO-23 §3 lands, those handlers must be adapted to call through `ki_dispatch_exception()` instead.
> → XREF: `TODO-10 §9`, `§10` -- #CP (vector 21) is generated by CET shadow-stack RET mismatches AND CET IBT missing-`ENDBR64` violations. Register the handler if `cpu_has(CPU_FEATURE_CET_SS)` OR `cpu_has(CPU_FEATURE_CET_IBT)` returns true.
> → XREF: `TODO-10 §12` + `TODO-27 §1` -- the KERNEL-side security-check failure already ships (`__stack_chk_fail` at `src/kernel/security/stack_canary.c:154` -> `KeBugCheckEx(BUGCHECK_KERNEL_SECURITY_CHECK_FAILURE, ...)`, code 0x139 defined at `include/kernel/bugcheck.h:27`). This section adds ONLY the ring-3 `int 0x29` vector; do not add a second kernel bugcheck path.

- [x] `except.c` `except_common_handler` maps 0/#DE 1/#DB 3/#BP 4/#OF 6/#UD 11/#NP 12/#SS 13/#GP via `fault_maps[]`->NTSTATUS; kernel #DE/#OF/#UD panic, rest route `ki_dispatch_exception`
- [x] `except.c` conditional `#CP` (v21), registered if CET_SS or CET_IBT (both raise #CP): noncontinuable `STATUS_STACK_BUFFER_OVERRUN` + subcode 0x39; kernel `KeBugCheckEx(0x139, 0x39)` (real-Windows ABI, not `STATUS_CONTROL_STACK_VIOLATION`)
- [ ] `#CP` error-code decode: distinguish SHSTK RET-mismatch vs IBT missing-`ENDBR64` (vector-21 error-code bits) and deliver the IBT-specific fast-fail subcode instead of the blanket SHSTK 0x39 (Codex consistency; base registration covers both)
- [x] Vector 41 (`0x29` `__fastfail`): `except_fastfail_handler` bypasses dispatch; FAST_FAIL code from ECX, noncontinuable `STATUS_STACK_BUFFER_OVERRUN`; gate DPL=3 for ring-3 `int 0x29`
- [x] `except_init()` registers handlers + DPL=3 gates (3/4/0x29); from `boot_phase1()` after `idt_init()` (phase-0 reg erased by the handlers[] clear) (→ XREF: `TODO-01 §3`)
- [x] CPL check via `(frame->cs & 3) ? UserMode : KernelMode`
- [ ] Cause-aware `#GP`: decode faulting instruction (`try_copy_from_user`, §13), deliver `STATUS_PRIVILEGED_INSTRUCTION` (ring-3 privileged opcode) or `STATUS_INVALID_LOCK_SEQUENCE` (bad LOCK prefix) vs the blanket `STATUS_ACCESS_VIOLATION`
- [ ] FP-exception delivery: register #MF (v16, x87) + #XM (v19, SIMD) handlers mapping to the `STATUS_FLOAT_*` family (`STATUS_FLOAT_MULTIPLE_TRAPS` for #XM); currently ownerless -> generic panic (Codex parity, alongside the lazy-FPU/#NM work)

> **Interim (until ring-3 delivery, §5):** a user-mode unhandled fault -- including a ring-3 `int3`/`into`/`int 0x29` now that those gates are DPL=3 -- terminates via `panic_screen()`, the same interim section 2 shipped for user #PF. Per-process termination (never a whole-system panic) is owned by §5. No new DoS class: ring-3 could already panic via `ud2`/#UD or a NULL dereference.

**Test checkpoint:** Boot log shows `except: fault-to-exception handlers registered (... #CP <on|off>)` -- `#CP on` where CPUID reports CET_SS or CET_IBT (both raise #CP), `#CP off` otherwise (QEMU TCG: off). Registration is feature-gated on those CPUID bits, independent of whether CET enforcement is enabled. `POST16(0xDE30)` before `except_init()`, `POST16(0xDE31)` after -- if crash at 0xDE30, `except_init` never entered. Pure record/mapping helpers (`except_vector_to_status`, `except_vector_kernel_fatal`, `except_build_record`) unit-tested (fault-context-safe, no live-fault trigger under WSL TCG). Live `ud2`/`int3`/`div 0` delivery lands with ring-3 delivery; verify on bare metal then. Test on: QEMU WHPX + TCG. Verify on bare metal.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 28 suites, 0 failures

> **Notes:**
> - **What shipped** -- `except.c`: `fault_maps[]`, `except_common_handler`, `except_fastfail_handler`, `except_init`, per-CPU `except_scratch`, pure `except_vector_to_status`/`_kernel_fatal`/`except_build_record`, `idt_set_user_callable`.
> - **How it runs** -- `except_init()` from `boot_phase1()` after `idt_init()` (POST16 0xDE30/0xDE31); dispatch via `idt.c` `handlers[frame->int_no]`, fault-context (lock/alloc-free), record in per-CPU scratch. #CP registration is feature-gated on CPUID CET_SS or CET_IBT (off on TCG), independent of CET enforcement enable.
> - **Downstream effects** -- recoverable faults route through the `ki_dispatch_exception` contract (kernel #DE/#OF/#UD panic, kernel #CP + `__fastfail` bypass it); the stub declines, so live delivery/termination lands with §5 + kernel SEH (§8/§14). Codex adoptions in the commit message.
> - **ABI decision** -- #CP uses `STATUS_STACK_BUFFER_OVERRUN` + subcode 0x39, not `STATUS_CONTROL_STACK_VIOLATION` (a different path); matches real Windows. `STATUS_STACK_BUFFER_OVERRUN` now canonical in `ntstatus.h` (deduped from `stack_canary.c`).
> - **Scope boundary** -- general fault vectors only; #PF stays with the VMM triage (section 2), #NM with lazy-FPU, NMI/#DF/#MC keep dedicated handlers. Ring-3 delivery is §5; kernel SEH §8/§14; three refinements stay open above (#CP SHSTK/IBT decode, cause-aware #GP, #MF/#XM).
> **Verified:** 2026-07-18 | commit `bfdfe36e` + review fixes | 5/8 items | build OK | smoke PASS (TCG 2.54s)
> **Accepted:** [M] user-mode unhandled fault -> `panic_screen()` is interim (no per-process termination yet) -> XREF: 02-kernel-core/TODO-23 §5 (item: "Delivery failure ... terminate the process, never `panic_screen()`" at line 222)
> **Deferred:** [M] #CP delivers the SHSTK subcode for IBT violations too (dormant until CET enables) -> XREF: 02-kernel-core/TODO-23 §3 (item: "`#CP` error-code decode" at line 204)
> **Deferred:** [M] cause-aware #GP decode (privileged-instruction / invalid-LOCK sub-cases) -> XREF: 02-kernel-core/TODO-23 §3 (item: "Cause-aware `#GP`" at line 208)
> **Deferred:** [M] #MF/#XM FP-exception delivery is ownerless -> XREF: 02-kernel-core/TODO-23 §3 (item: "FP-exception delivery" at line 209)
> **Quality reviewed:** 2026-07-18 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 15M+2L fixed, 3M deferred-XREF, 1M accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"kernel: map CPU exceptions to EXCEPTION_RECORD dispatch (#DE/#DB/#BP/#GP/#UD/#SS/#CP)"`


---

## 4. Debugger First-Chance / Second-Chance Notification

**Prompt:** Implement the master exception dispatcher (declaration + UNHANDLED stub landed by §2: `KI_EXCEPTION_DISPOSITION ki_dispatch_exception(EXCEPTION_RECORD *rec, CONTEXT *ctx, struct interrupt_frame *frame, KPROCESSOR_MODE mode, int first_chance)` -- the live `frame` is passed so ring-3 delivery can rewrite it, and a `KI_EXCEPTION_HANDLED` return resumes via IRET) that orchestrates the full Windows NT exception dispatch sequence. On Windows, the dispatch order is:
1. **Debugger first-chance notification** -- if the process has a debug port (or kernel debugger is attached for kernel-mode exceptions), send the exception to the debugger. If the debugger handles it (continues execution), stop.
2. **Ring-3 handover** (user-mode exceptions) -- deliver the trap frame to `KiUserExceptionDispatcher` (§5). Steps 3-6 then run **in ring 3**, inside ntdll, NOT in this function: VEH (§10) -> SEH (§8) -> VCH (§11) -> top-level filter (§12). See the address-space boundary callout at the top of this file.
3. **Second-chance re-entry** -- when ring-3 dispatch declines everything, ntdll re-enters the kernel via `NtRaiseException(first_chance=FALSE)` (§5). Only then does the kernel notify the debugger a second time. This round-trip IS the second-chance mechanism; there is no kernel-side handler walk.
4. **Terminal** -- no handler anywhere: terminate the process with the exception code as exit status.

For kernel-mode there is no ring-3 leg: first-chance `KiDebugRoutine` -> kernel SEH (§14) -> second-chance `KiDebugRoutine` -> `KeBugCheckEx`.

> [!IMPORTANT]
> → XREF: `TODO-29 §5` -- KD is the kernel debugger; `KiDebugRoutine` is the function pointer that `ki_dispatch_exception` calls for kernel-mode first/second-chance. If KD is not attached, `KiDebugRoutine` is NULL and the notification is skipped.
> → XREF: `TODO-29 §14` -- User-mode debug port is `NtDebugActiveProcess`; `DbgkForwardException()` sends the exception to the debug port. Stub `DbgkForwardException` to return FALSE until TODO-29 §14 lands.

> [!NOTE]
> `KPROCESSOR_MODE` (`typedef enum { KernelMode = 0, UserMode = 1 }`), the `KI_EXCEPTION_DISPOSITION` enum, and the `ki_dispatch_exception` / `ki_raise_kernel_exception` declarations were landed by §2 in `include/kernel/except.h`; §4/§14 replace the UNHANDLED stubs with the real bodies. Canonical `KPROCESSOR_MODE` moves to a shared NT types header when TODO-12 matures.
> **The §2 #PF handler routes BOTH user and kernel faults through `ki_dispatch_exception` (mode arg distinguishes)**, so the KernelMode leg here MUST call `ki_raise_kernel_exception` internally (do not expect the #PF handler to call it directly). **HANDLED contract:** a `KI_EXCEPTION_HANDLED` return MUST leave the live `frame` carrying the final resume/delivery state (the caller IRETs the frame and never re-applies `ctx`) -- a dispatcher working in `ctx` must `frame_from_context()` it back first, or IRET refaults in a loop.
> `KeBugCheckEx` is implemented in `src/kernel/panic.c` per `TODO-27-crash-dump-generation.md` §1. When `except.c` lands, include `panic.h` (or a forward declaration) and call the shared `KeBugCheckEx` entry point for terminal kernel-mode faults. Do not add a second implementation in `except.c`.

- [x] `include/kernel/except.h` -- `typedef enum { KernelMode, UserMode } KPROCESSOR_MODE;` (landed by §2, local; moved to shared header later)
- [x] `except.c` kernel-mode terminal calls `KeBugCheckExFrame` -- frame-aware fault-safe sibling of `KeBugCheckEx` in `panic.c` (shared core, keeps trap-frame evidence, skips fault-unsafe registry write); no duplicate body
- [x] `src/kernel/except.c` `ki_dispatch_exception(rec, ctx, frame, mode, first_chance)` master dispatcher replaces the §2 UNHANDLED stub; orchestrates debugger notify -> kernel SEH stub -> terminal
- [x] `KiDebugRoutine` callback (static in `except.c`, default NULL) published atomically via `ki_set_debug_routine()`, read acquire in the fault path → XREF: TODO-29 KD kernel-debugger attach
- [x] `DbgkForwardException(rec, ctx, first_chance)` stub returns FALSE (no debug port) → XREF: TODO-29 user-mode debug port (`NtDebugActiveProcess`)
- [/] User-mode: debugger first/second-chance via `DbgkForwardException`, declines to the caller terminal when undebugged; ring-0 does NOT call VEH/SEH/VCH. Ring-3 handover to `KiUserExceptionDispatcher` deferred to §5
- [x] Kernel-mode: `KiDebugRoutine` first-chance → `ki_raise_kernel_exception` (kernel SEH, §14 stub) → `KiDebugRoutine` second-chance → `KeBugCheckExFrame` terminal; sequence wired here, SEH body owned by §14
- [x] `ki_kernel_bugcheck_code`/`_params` select STOP 0x3B (params code/instr/CONTEXT-addr/0) when a user syscall is on the stack (per-thread `in_system_service` flag, untouched by Zw) else 0x1E -> XREF: `TODO-27 §1`

**Test checkpoint:** With `KiDebugRoutine == NULL`: an undebugged user access violation returns `KI_EXCEPTION_UNHANDLED` so the caller performs the WER + user-safe panic terminal (VEH/SEH/VCH are ring-3, §8-§12). With `KiDebugRoutine` set to a test function: a kernel-mode first-chance notification fires (mode=KernelMode, first_chance=1) and a HANDLED return resumes without a bugcheck. Kernel-mode unhandled exception calls `KeBugCheckExFrame` (STOP 0x1E, or 0x3B inside a system service). Serial log shows `"except: dispatch user exception code=0x<code>, first_chance=1"` (klog, not POST16 -- fault-path code is post-Phase-3). Validated by the `except` unit suite; on-hardware check QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 156 kernel suites, 0 failures

> **Notes:**
> - **What shipped** -- `ki_dispatch_exception` dispatcher (debugger notify, `KiDebugRoutine`, `DbgkForwardException` stub, bugcheck selector) in `except.c`; `KeBugCheckExFrame` frame-safe terminal in `panic.c`; `in_system_service` flag.
> - **How it runs** -- both `#PF` (§2) and general-fault (§3) handlers route through the dispatcher; kernel faults bugcheck internally (noreturn), so the callers' kernel `else` panic branches are now defense-in-depth fallbacks.
> - **Downstream effects** -- unblocks §5 ring-3 delivery and §14 kernel SEH. Codex design + adversarial adoptions (frame-safe STOP identity, Zw-proof service flag, per-code 0x1E/0x3B param layout, slot-reuse reset) in the commit message.
> - **Canonical doc** -- [`TODO-23-exception-dispatch-seh.md`](TODO-23-exception-dispatch-seh.md) §4.
> - **Scope boundary** -- §4 owns debugger notify + terminal selection; ring-3 delivery §5, kernel SEH §14, VEH/VCH §10-§11, KD attach + user debug port TODO-29; `in_system_service` SMP closure rides with `previous_mode`'s deferred work.

> **Verified:** 2026-07-18 | commit `7a10bb24` | 7/8 items | build OK | smoke PASS (TCG 2.72s)
> **Accepted:** [M] framed terminal still runs `panic_screen_impl` FPU-capture POST16 + restart-registry I/O unconditionally (pre-existing, panic-path-wide; §4 skips only the persist-write + entry POST16) -> XREF: 02-kernel-core/TODO-27 §1 (item: "Fault-context gating in `panic_screen_impl`" at line 101)
> **Accepted:** [M] 0x3B vs 0x1E classification reads `thread_current()` global cursor (pre-existing, shared with `previous_mode`; forensic-only) -> XREF: 03-memory-concurrency/TODO-07 (item: "Per-CPU current-thread cursor" at line 120)
> **Quality reviewed:** 2026-07-18 | Codex 10x (design + adversarial + consistency + perf + re-adversarial) | 2H+9M+2L fixed, 2M accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"kernel: implement ki_dispatch_exception with debugger first/second-chance notification"`


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

> [!NOTE]
> **DEFERRED (2026-07-18) -- blocked on missing infrastructure; SPLIT at implementation time.** A pre-implementation Codex design review (6 High) established that §5 depends on four prerequisites not yet built:
> 1. a per-CPU current-thread cursor + per-CPU idle task/frame (`03-memory-concurrency/TODO-07-smp-phase2.md` lines 118-119) -- without it the trap-frame plumbing corrupts frames across CPUs and fault-context termination re-faults the sole runnable task;
> 2. a real ntdll mapping that publishes the `KiUserExceptionDispatcher` VA per process (`TODO-11 §7`, `10-platform-services/TODO-07 §7`);
> 3. a `KI_EXCEPTION_TERMINATE` disposition + a guaranteed-idle-frame termination primitive (today `schedule_now` returns the same frame for the sole runnable task, and the per-CPU exception scratch guard would strand on a process-only exit);
> 4. a full-frame path for `NtContinue`/`NtRaiseException` (the SYSCALL fast path is SYSRET-only and cannot restore an arbitrary CONTEXT; INT 0x2E carries the live frame).
> At implementation time SPLIT into (a) `ki_deliver_user_exception` delivery + termination infrastructure and (b) an INT 0x2E-only `NtRaiseException`/`NtContinue` section, per the design review.

- [ ] `ki_deliver_user_exception(frame,rec)`: capture `CONTEXT` (§1) + build a pinned `KI_USER_EXCEPTION_FRAME` (Win64 ABI: 32B home + terminal return slot + `EXCEPTION_POINTERS`/record/context) via CHECKED `rsp` subtraction, reject underflow
- [ ] Write the block ONLY via `ProbeForWrite` + `try_copy_to_user` (§13), never a raw `memcpy` through `frame->rsp`; point `EXCEPTION_POINTERS` at the user-stack copies; patch `frame->rip`=dispatcher, `frame->rsp`, `frame->rcx`=`EXCEPTION_POINTERS*`
- [ ] Dispatcher address is PER-PROCESS readiness (a `task`-level VA set only after ntdll maps + its export resolves), NOT a global; `0` = hard delivery failure, never IRET to a placeholder -> XREF: `TODO-11 §7`, `10-platform-services/TODO-07 §7`
- [ ] Delivery failure (unset dispatcher, bad RSP, copy fault) returns a new `KI_EXCEPTION_TERMINATE` disposition: clear the per-CPU scratch slot THEN hand to a guaranteed idle-frame terminate primitive -- never `panic_screen()`, never a 2nd fault
- [ ] Termination primitive requires the per-CPU idle task/frame + per-CPU current-thread cursor (absent today: `schedule_now` returns the same frame for the sole runnable task) -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md` (lines 118-119)
- [ ] `NtRaiseException` SSDT entry (0x0130), INT 0x2E only: calls `ki_dispatch_exception()` (§4) `first_chance=TRUE` for a raise, `FALSE` for the ntdll second-chance re-entry; fail STATUS on the frameless SYSCALL fast path
- [ ] `NtContinue` SSDT entry (0x0131): validate untrusted IRET frame -- EXACT user CS/SS, user-range RIP/RSP, known `ContextFlags`, RFLAGS whitelist (force bit1+IF, clear IOPL/NT/VM/reserved), reject noncontinuable; atomic validated-copy restore
- [ ] Per-thread `cur_trap_frame` + `trap_frame_replaced` plumbing in `syscall_handler_2e` -- BLOCKED on the per-CPU current-thread cursor (else cross-CPU frame corruption) -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md` (line 119)

**Test checkpoint:** User-mode fault delivery: after `ki_deliver_user_exception()`, `frame->rip` points to `KiUserExceptionDispatcher` address (or stub entry), `frame->rsp` is 16-byte aligned and below the original RSP. `EXCEPTION_RECORD` and `CONTEXT` are readable on the user stack. Invalid user RSP (e.g., 0xDEAD) → process terminated, not kernel panic. `POST16(0xDE50)` before user stack manipulation, `POST16(0xDE51)` after IRET redirect. Test on: QEMU WHPX + TCG.

> **Deferred:** [H] ring-3 exception delivery + `NtContinue`/`NtRaiseException` blocked on the per-CPU current-thread cursor + per-CPU idle task/frame (trap-frame ownership across CPUs; fault-context termination without a re-fault) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md (item: "Per-CPU current-thread cursor" at line 119; item: "Per-CPU idle task" at line 118)

- [ ] Commit: `"kernel: implement KiUserExceptionDispatcher and NtRaiseException/NtContinue"`


---

## 6. x64 Table-Based Unwind (`.pdata`, `RtlVirtualUnwind`)

**Prompt:** Implement the x64 PE32+ unwind machinery used by `RtlUnwindEx` and the SEH chain walker. Add `RtlLookupFunctionEntry(ULONGLONG pc, ULONGLONG *base, void *history)` -- it scans the `.pdata` section of the module containing `pc` for the matching `RUNTIME_FUNCTION` entry. Implement `RtlVirtualUnwind(handler_type, image_base, pc, func_entry, ctx, handler_data, establisher_frame, ctx_ptrs)` -- it applies the `UNWIND_INFO` opcodes (`UWOP_PUSH_NONVOL`, `UWOP_ALLOC_SMALL`, `UWOP_ALLOC_LARGE`, `UWOP_SET_FPREG`, `UWOP_SAVE_NONVOL`, `UWOP_SAVE_XMM128`) to a `CONTEXT`, returning the parent frame's PC and RBP. These two functions live in a new `src/kernel/rtl/unwind.c` / `include/kernel/rtl/unwind.h`.

> [!IMPORTANT]
> → XREF: `TODO-17 §6` -- PE32+ section loader registers `.pdata` with the module list; `exec_find_module_by_pc()` (§7) provides module lookup for `RtlLookupFunctionEntry`.

- [x] `include/kernel/rtl/unwind.h` -- `RUNTIME_FUNCTION`, `UNWIND_INFO`, `UNWIND_CODE`, `SCOPE_TABLE`, `KNONVOLATILE_CONTEXT_POINTERS` (ABI structs `_Static_assert`-pinned; mask accessors, no bitfield-packing dependence)
- [x] `src/kernel/rtl/unwind.c` -- `RtlLookupFunctionEntry` (module `.pdata` binary-search, then dynamic registry) + `RtlVirtualUnwind` (prolog interpreter + iterative chaining/single terminal pop + epilog detection)
- [x] Support all UWOP opcodes: `PUSH_NONVOL`, `ALLOC_SMALL`, `ALLOC_LARGE`, `SET_FPREG`, `SAVE_NONVOL(_FAR)`, `SAVE_XMM128(_FAR)`, `PUSH_MACHFRAME`; `EPILOG`/`SPARE_CODE` skipped structurally
- [x] `RtlAddFunctionTable`/`RtlDeleteFunctionTable` + `RtlInstallFunctionTableCallback` (JIT variant) 💎; registry stores a private table copy. Growable variant deferred -> XREF: TODO-18 §5 (item: "Dynamic/JIT unwind ... growable table").
- [x] SMP lifetime: spinlock sorted list, overlap/order validation at add; delete unlinks-under-lock then frees. Callback lookups snapshot cb+ctx under the lock, so self-unregister never stalls. -> XREF: TODO-18 §5 (item: "Validate unwind ranges").
- [x] `RtlPcToFileHeader(pc, base)` -- public "which module owns this PC" wrapping `exec_find_module_by_pc()`; crash reporting (§12) and the debugger consume it
- [x] Unit test: synthetic table via `RtlAddFunctionTable`; 3-frame recovered-RIP chain + prolog/epilog/frame-register/callback/malformed-fail-safe (`test_unwind.c`, 10 suites)
- [ ] Kernel-mode unwind metadata provider (ELF kernel has no `.pdata`, kernel PCs return NULL); needed before §7 table-walks kernel stacks -> XREF: TODO-18 §5 (item: "Register PE `.pdata` ... Translate `.eh_frame`").
- [ ] SMP-epoch dynamic-table lookup: close the cross-call window where a concurrent `RtlDeleteFunctionTable` can free a table an unwinder still holds an entry into (needs SMP RCU/epoch) -> XREF: `rcu.h` SMP-future quiescent-state tracking.
- [ ] UWOP_EPILOG (unwind-info v2) epilog metadata: closes the residual where a PC exactly at an indirect tail-call jmp (frame torn down) is not detected by forward scanning -> XREF: this section (`unwind_try_epilog` indirect note).
- [ ] Indirect / noncontiguous-fragment RUNTIME_FUNCTION support (UnwindInfoAddress bit 0): pair the fragment range with the shared parent unwind info; currently returns NULL (fail-safe) -> XREF: this section (`RtlLookupFunctionEntry` indirect note).
- [ ] Exact winnt.h ABI types on the public unwind prototypes (PUNWIND_HISTORY_TABLE, PEXCEPTION_ROUTINE, BOOLEAN, PCWSTR OutOfProcessCallbackDll) when user-mode ntdll exports land -> XREF: this section (kernel-internal today).
- [ ] Combined lookup+unwind path: a stack walk does two `exec_find_module_by_pc` 368B snapshots per loaded-module frame (RtlLookupFunctionEntry + unwind_module_extent); pass the extent from lookup into unwind -> XREF: §7 (stack walker).

**Test checkpoint:** The kernel is ELF (no `.pdata`), so kernel PCs have no `RUNTIME_FUNCTION` and `RtlLookupFunctionEntry` correctly returns NULL for them; the engine is proven against a SYNTHETIC table registered via `RtlAddFunctionTable`. Lookup returns the registered entry for an in-range PC and NULL out-of-range. `RtlVirtualUnwind` on a 3-frame chain recovers the correct RIP per parent frame; prolog, epilog, and frame-register (`SET_FPREG`) forms all recover RIP/RSP; malformed metadata fails safe (leaf pop, no crash). Serial log: `"rtl: unwind engine ready (dynamic tables: 0, kernel .pdata: ELF none)"`. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 22 unwind suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/rtl/unwind.{c,h}` (~1010 LOC + 22 test suites): the x64 table-based unwind engine -- `RtlLookupFunctionEntry`, `RtlVirtualUnwind` (prolog interpreter + epilog simulation + chaining + transactional malformed fail-safe), the dynamic function-table registry, `RtlPcToFileHeader`.
> - Wired into Phase 3 via `rtl_unwind_init()` from `exec_init()` (klog banner; not boot-path, no POST16). Consumes the `.pdata` the PE loader records into `loaded_module_t`.
> - Owns the unwind ENGINE only; T18 §5 owns metadata lifetime/normalized storage + load-time validation, T17 owns section parsing. Kernel-mode unwind needs the T18 provider (filed item above) before §7 table-walks kernel stacks.
> - Canonical doc: `include/kernel/rtl/unwind.h` header block (ownership split + SMP/lifetime + bounds-safety contract).
> - Scope boundary: engine-internal (consumed by §7 stack walk, §12 crash reporting, the debugger); NOT yet exported to user-mode ntdll (`pe.c` export tables unchanged).
> **Verified:** 2026-07-18 | ship `a63dd05b` + review fixes | 7/13 items | build OK | tests 21887+16 PASS; smoke PASS (TCG 2.57s)
> **Accepted:** [H] engine trusts the establisher frame; fault-safe stack reads + stack-range validation for UNTRUSTED frames belong to consumers -> XREF: 02-kernel-core/TODO-23 §7 (item: "Fault-safe frame reads" at line 368)
> **Accepted:** [H] loader-side `.pdata`/`.xdata` mapped-section validation + production kernel-mode unwind metadata (ELF `.eh_frame`) -> XREF: 02-kernel-core/TODO-18 §5 (item: "Register PE `.pdata` ... Translate `.eh_frame`" at line 132)
> **Deferred:** [M] SMP-epoch lookup, UWOP_EPILOG v2 metadata, indirect-fragment support, exact winnt.h ABI types, combined lookup+unwind path -> XREF: 02-kernel-core/TODO-23 §6 (item: "SMP-epoch dynamic-table lookup" at line 339)
> **Quality reviewed:** 2026-07-18 | Codex 42x (design, adversarial, re-adversarial, consistency, perf) + kernel-quality-auditor + concurrency-mapper | 40H+11M+3L fixed, 0 open, 8 accepted/deferred-XREF, 2 rejected | scope: kernel-code-quality

- [x] Commit: `"rtl: implement RtlLookupFunctionEntry and RtlVirtualUnwind for x64 unwind"`


---

## 7. Stack Walking (`RtlCaptureStackBackTrace`, `RtlWalkFrameChain`)

**Prompt:** Implement kernel-mode stack walking using the x64 unwind tables from §6. On Windows, `RtlCaptureStackBackTrace` is the primary API for capturing a stack trace -- it walks the call stack via `RtlVirtualUnwind` and records return addresses. Both user-mode (ntdll) and kernel-mode (ntoskrnl) expose this function. Linux has `stack_trace_save()`. Impossible OS needs the kernel-mode implementation here; the user-mode version is in `TODO-04-ntdll-user-runtime.md`.

- [x] `include/kernel/rtl/unwind.h` -- declare `RtlCaptureStackBackTrace`, `RtlWalkFrameChain`, `rtl_capture_stack_from_context(CONTEXT*)` (crash-path entry), `RTL_MAX_STACK_FRAMES` (0xFE)
- [x] `src/kernel/rtl/unwind.c` -- `RtlCaptureStackBackTrace` + `rtl_capture_stack_from_context`: bounds-checked RBP walk, skip/count/hash, max 0xFE. No `.pdata`; metadata-accurate walk deferred -> XREF: `TODO-18 §5` (`.eh_frame` registry)
- [x] Frame-pointer retention: `-fno-omit-frame-pointer` kernel-wide (Makefile CFLAGS) so RBP is a valid frame chain (also repairs `panic.c`'s best-effort RBP walk)
- [/] `src/kernel/rtl/unwind.c` -- `RtlWalkFrameChain`: `flags==0` kernel walk shipped; `flags & 1` user walk returns 0 (needs a saved current-user-`CONTEXT` accessor) -> XREF: `TODO-07 §3` (saved user trap-frame accessor)
- [x] Fault-safe frame reads: `[rbp]`/`[rbp+8]` via `__kstack_read_u64` (RIP-keyed #PF fixup, `cpu_security.c`+`vmm.c` redirect before the pager); RBP RSP-anchored, aligned, monotonic, bounded; `ret` in kernel text -> XREF: TODO-23 §6
- [x] IRQL requirement: callable at `IRQL <= DISPATCH_LEVEL` -- no lock, no block, fault-safe reads only (→ XREF: `TODO-07 §3`)

**Test checkpoint:** `RtlCaptureStackBackTrace(0, 8, buf, &hash)` from a 4-deep chain returns ≥4 kernel-code frames with a non-zero hash; `skip=1` drops the immediate caller (first frame == skip=0 second frame); `rtl_capture_stack_from_context` on a synthetic RBP chain returns the exact frames with a deterministic hash; a non-monotonic RBP terminates the walk; `__kstack_read_u64` recovers a #PF on an unmapped kernel VA (returns -1, no bugcheck). Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 8 new StackWalk suites, 0 failures (21923 kernel + 16 user PASS)

> **Notes:**
> - **What shipped** -- kernel stack walking in `unwind.c`: `RtlCaptureStackBackTrace`, `RtlWalkFrameChain`, `rtl_capture_stack_from_context`; a bounds-checked RBP frame-chain walk (no `RtlVirtualUnwind` -- the ELF kernel has no `.pdata`).
> - **Fault boundary** -- new `__kstack_read_u64` (`cpu_security.c`) is a RIP-keyed fault-recoverable kernel read; `page_fault_handler` redirects its #PF to the fixup BEFORE the pager, so a corrupt RBP stops the walk instead of bugchecking.
> - **How it integrates** -- `-fno-omit-frame-pointer` kernel-wide makes RBP a valid frame chain (also repairs `panic.c`); capture starts at `__builtin_frame_address(0)` so `skip==0` yields the caller; crash consumers pass a `context_from_frame` CONTEXT.
> - **Downstream effects** -- 4 Codex design rounds hardened the fault boundary (see commit msg); `RtlWalkFrameChain` user walk `[/]` deferred to `TODO-07 §3`; metadata-accurate walk deferred to `TODO-18 §5`.
> - **Canonical doc** -- the stack-walk block in `include/kernel/rtl/unwind.h`.
> - **Scope boundary** -- §7 owns the kernel walker + fault-safe read; `.eh_frame`/`.pdata` registry is `TODO-18 §5`; user-mode `RtlCaptureStackBackTrace` (ntdll) is `TODO-04`.
> **Verified:** 2026-07-18 | ship `1d3e1ab5` + review fixes | 5/6 items | build OK | 21923 kernel + 16 user PASS | smoke PASS (KVM 2.75s)
> **Deferred:** [M] loaded-module (PE driver) return frames stop the walk -- kernel-text-only PC validation stays lock-free to avoid the `s_module_lock` crash-path deadlock (the deadlock itself is fixed); no PE drivers load yet -> XREF: `02-kernel-core/TODO-18 §6` (item: "Lock-free crash-safe module-range lookup" at line 151)
> **Quality reviewed:** 2026-07-18 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+6M+1L fixed, 1M deferred | scope: kernel-code-quality

- [x] Commit: `"rtl: implement RtlCaptureStackBackTrace and RtlWalkFrameChain for kernel-mode stack walking"`


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

> [!NOTE]
> **RE-OWNED to ring-3 ntdll (2026-07-18) -- NOT implemented in kernel `seh.c`.** The 2026-07-15 ring-0/ring-3 boundary correction (the file-top "Address-space boundary" IMPORTANT note, binding for §4-§12) and this TODO's Outcome (line: "`src/kernel/rtl/seh.c` -- kernel-mode SEH SCOPE_TABLE walker (§14) ... The ring-3 `RtlDispatchException` / `__C_specific_handler` ... are ntdll-side") both place `RtlDispatchException`, `__C_specific_handler`, and the SCOPE_TABLE **search-pass** walker in ntdll ring-3 code (`src/user/ntdll/ntdll_except.c`), because the kernel never runs user handlers. This section's original body predates that correction and is stale. What stays in TODO-23: the **kernel-mode** SEH SCOPE_TABLE walker for drivers is §14; the shared table-based search/unwind engine (`RtlLookupFunctionEntry` / `RtlVirtualUnwind`, §6) is already shipped `[x]`; `RtlUnwindEx` (§9) is the kernel-side unwind primitive both walkers call. The shared dispatch ABI is already published kernel-side: the `EXCEPTION_DISPOSITION` enum, `DISPATCHER_CONTEXT`, and the funclet (ms_abi) calling convention live in `include/kernel/rtl/unwind.h` (§6/§9), and the `EXCEPTION_EXECUTE_HANDLER`/`_CONTINUE_SEARCH`/`_CONTINUE_EXECUTION` filter constants are shipped in `include/kernel/except.h` (§10). What travels with the walker to its owner is the dispatch LOGIC -- the search-pass scope-table walk + filter/`__finally` invocation -- not those ABI definitions (ntdll §5 for the user path; §14 for the kernel-driver path). -> XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5` (item: "`KiUserExceptionDispatcher`" at line 277 -- walks SEH via table-based x64 dispatch invoking each frame's `__C_specific_handler`).

- [ ] `src/kernel/rtl/seh.c` -- `RtlDispatchException`, scope-table walker
- [ ] `src/kernel/rtl/seh.c` -- `__C_specific_handler` -- the language-specific exception handler for C `__try`/`__except`; referenced by `UNWIND_INFO.ExceptionHandler` in compiled PE binaries
- [ ] Filter expression invocation with correct calling convention
- [ ] Nested exception handling (`EXCEPTION_NESTED_CALL` flag)

**Test checkpoint:** `RtlDispatchException` with a `SCOPE_TABLE` containing one matching `__try` scope calls the filter expression and returns TRUE. Filter returning `EXCEPTION_CONTINUE_SEARCH` → walks to parent frame. No matching scope in any frame → returns FALSE. `__C_specific_handler` is invoked for functions whose `UNWIND_INFO` references it. Serial log: `"seh: scope match at RVA 0x<rva>, filter=EXECUTE_HANDLER"`. Test on: QEMU WHPX + TCG. (Now owned ring-3; the acceptance criteria above are validated in ntdll -- see the RE-OWNED note.)

> **Deferred:** [H] re-owned out of TODO-23: ring-3 SEH search-pass dispatch (`RtlDispatchException`, `__C_specific_handler`, SCOPE_TABLE walker, filter/`__finally` funclet invocation, `EXCEPTION_NESTED_CALL` guard) is ntdll-side, not kernel `seh.c` -- the kernel never calls user handlers (ring-0/ring-3 boundary, file-top note + Outcome). Shared dispatch ABI (`EXCEPTION_DISPOSITION`, filter-result constants, `DISPATCHER_CONTEXT`, funclet calling convention) travels with the walker to its owner -> XREF: 12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5 (item: "`KiUserExceptionDispatcher`" at line 277)

- [ ] Commit: `"rtl: implement RtlDispatchException, __C_specific_handler, and SEH scope-table walker"`


---

## 9. RtlUnwindEx -- Unwind to a Target Frame

**Prompt:** Implement `RtlUnwindEx(target_frame, target_ip, exception_record, return_value, ctx, history)`.
It iterates from the current RSP upward via `RtlVirtualUnwind` (§6), calling each frame's `__finally` block (via the termination handler in the SCOPE_TABLE), until it reaches `target_frame`. At that point it restores `ctx` (with `return_value` in RAX and `target_ip` in RIP) and jumps to the target. It must correctly handle:
- `__finally` termination handlers (UWOP_SCOPE with `HandlerAddress == TERMINATION`).
- Continuation frames (frames with no handler -- just unwind and continue).
- The global unwind flag (`EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND`) set on the exception record during this walk.

- [x] `src/kernel/rtl/unwind.c` -- `RtlUnwindEx` (no-return) + testable `rtl_unwind_to_target` (two-pass: fault-free PREFLIGHT validates reachability, then the __finally EXECUTE pass); shared dispatch ABI in `unwind.h`
- [x] Termination handler invocation -- each EXITED frame `__finally` runs with `EXCEPTION_UNWINDING`; the TARGET frame handler runs with `EXCEPTION_TARGET_UNWIND`; resume is the target frame own context (PREFLIGHT invariants re-checked in EXECUTE)
- [x] Set/clear the unwind flags (`EXCEPTION_UNWINDING`, `TARGET_UNWIND`) on the record, cleared once the target is reached; a whole-stack EXIT unwind (NULL target_frame) and a NULL target_ip are rejected (`STATUS_INVALID_PARAMETER`)
- [x] `RtlRestoreContext(ctx, rec)` -- SAME-CPL kernel terminal resume (NASM `unwind_asm.asm`): restores CONTROL+INTEGER via a `cli`-guarded same-CPL `iretq` so RIP+RFLAGS+RSP take effect atomically; NOT the ring-3 delivery path
- [/] `EXCEPTION_COLLIDED_UNWIND`: in-place single-level adopt (canonical RIP + forward-span RSP validated); a distinct-`ContextRecord` nested collision fails safe (`STATUS_BAD_STACK`), full adoption -> XREF: `TODO-23 §14`
- [ ] Leaf-convention step for a metadata-free frame + whole-stack EXIT unwind -- today a NULL lookup fails safe and EXIT unwind is rejected; both land with the kernel-unwind metadata provider -> XREF: `02-kernel-core/TODO-18 §5`
- [ ] CET: advance the shadow-stack pointer by N unwound frames (INCSSP) -- BLOCKED: kernel CET shadow stacks are detected-but-disabled (enable trampoline owned elsewhere), no live shadow stack to advance -> XREF: `TODO-10 §9`
- [ ] Stack-safety hardening (full): guard-page-aware bounds + an asm entry check before the C prologue (or emergency unwind stack) + a near-guard test -- best-effort check + compact snapshot shipped -> XREF: this section

**Test checkpoint:** `rtl_unwind_to_target` over a synthetic 3-frame `RtlAddFunctionTable` chain runs the ms_abi handler for both EXITED frames (`EXCEPTION_UNWINDING`) plus the target frame (`EXCEPTION_TARGET_UNWIND`, once); resume Rip=`target_ip`, Rax=`return_value`, Rsp=the target frame; the flag is cleared after. An unreachable target fails preflight with `STATUS_BAD_STACK` and runs NO `__finally`; an invalid handler disposition returns `STATUS_INVALID_DISPOSITION`; a collided disposition sets `EXCEPTION_COLLIDED_UNWIND` and still resolves; a collision IN the target frame and a wild-RSP collided redirect fail safe; a NULL target_frame/target_ip returns `STATUS_INVALID_PARAMETER` with flags cleared. A failure AFTER a `__finally` already ran reports `finally_count > 0` so the public `RtlUnwindEx` goes terminal (`KeBugCheckEx`) instead of returning to a half-cleaned caller. `RtlRestoreContext` restores rbx/r12/rsp and transfers to Rip (asm round-trip harness); `DISPATCHER_CONTEXT` ABI offsets pinned. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 13 new UnwindEx suites, 0 failures (377 kernel + 16 user PASS)

> **Notes:**
> - **What shipped** -- `RtlUnwindEx` + testable `rtl_unwind_to_target`, NASM `RtlRestoreContext` + `rtl_restore_selftest` (new `unwind_asm.asm`), the shared dispatch ABI (`DISPATCHER_CONTEXT`, ms_abi `PEXCEPTION_ROUTINE`) in `unwind.h`; 6 tests.
> - **How it runs** -- two-pass: a fault-free PREFLIGHT validates reachability, then EXECUTE runs each exited frame `__finally` via the UHANDLER; `RtlRestoreContext` is the same-CPL terminal resume. Synthetic tables only.
> - **Downstream effects** -- defines the dispatch ABI the kernel-driver SEH walker + ntdll build on; single-level `EXCEPTION_COLLIDED_UNWIND` handled, full nested protocol filed to §14; CET shadow-stack unwind filed to TODO-10 §9.
> - **Canonical doc** -- the RtlUnwindEx / dispatch-ABI block in `include/kernel/rtl/unwind.h`.
> - **Scope boundary** -- §9 owns the kernel unwind-to-target + terminal resume ENGINE; ring-3 `RtlDispatchException` is ntdll (TODO-04); the kernel-driver walker + full collided recovery are §14; NtContinue IRET restore is §5.
> **Verified:** 2026-07-19 | ship `377c693f` + review fixes | 4/8 items | build OK | tests 377+16 PASS; smoke PASS (2.71s)
> **Accepted:** [H] full stack-safety hardening (guard-page-aware bounds + asm entry-trampoline check before the C prologue + emergency stack + near-guard test); best-effort entry headroom check + compact snapshot + noinline preflight shipped, no ring-0 caller yet -> XREF: 02-kernel-core/TODO-23 §9 (item: "Stack-safety hardening (full)" at line 443)
> **Accepted:** [H] leaf convention for metadata-free frames + whole-stack EXIT unwind (both fail safe today) -> XREF: 02-kernel-core/TODO-23 §9 (item: "Leaf-convention step for a metadata-free frame" at line 441)
> **Accepted:** [M] RtlUnwindEx exact ms_abi/winnt.h ABI types (kernel-internal SysV today, like the §6 Rtl* engine) -> XREF: 02-kernel-core/TODO-23 §6 (item: "Exact winnt.h ABI types on the public unwind prototypes" at line 342)
> **Deferred:** [H] full nested/multi-scope collided-unwind recovery + fault-safe untrusted-context reads + precise finally-funclet tracking -> XREF: 02-kernel-core/TODO-23 §14 (item: "Full `EXCEPTION_COLLIDED_UNWIND` protocol" at line 459)
> **Deferred:** [M] CET shadow-stack INCSSP during unwind (kernel CET disabled) -> XREF: 02-kernel-core/TODO-23 §9 (item: "CET: advance the shadow-stack pointer" at line 442)
> **Quality reviewed:** 2026-07-19 | Codex 18x (design, adversarial, consistency, perf, re-adversarial) | 18H+7M+3L fixed, 0 open, 5 accepted/deferred-XREF | scope: kernel-code-quality

- [x] Commit: `"rtl: implement RtlUnwindEx with termination handler invocation"`


---

## 10. Vectored Exception Handlers (VEH)

**Prompt:** Establish the kernel side of the Vectored Exception Handler contract. VEH is a per-process, ring-3 mechanism: ntdll keeps a lock-guarded doubly-linked list of `VECTORED_HANDLER_ENTRY` nodes as **process-global** state (a `LdrpVectorHandlerList`-analog) and walks it from `RtlDispatchException` **before** the SEH scope-table walk (§8), after the kernel has delivered the fault via `KiUserExceptionDispatcher` (§5). The kernel's only job here is to publish the shared node/disposition ABI so the two sides agree byte-for-byte; the list itself is never anchored, read, or walked from ring 0.

> [!WARNING]
> **Ownership: the VEH list is RING-3 state, and its anchor is ntdll process-global -- NOT a kernel/TEB/PEB field.** Windows keeps it in ntdll and walks it from `RtlDispatchException`; the kernel never calls a VEH handler. `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §5` owns the list head, the add/remove APIs, and the walk. This section therefore does NOT put a VEH head in `struct task`, the TEB, or the PEB, and does NOT call handlers from `ki_dispatch_exception()`; it ships only the shared node/disposition ABI.

> [!IMPORTANT]
> **No per-thread anchor.** VEH is per-PROCESS: a `TEB.VehListHead` (per-thread) anchor would give each thread its own list and break `AddVectoredExceptionHandler` semantics (a handler registered on one thread would not fire for another thread's fault). This mirrors §12's rule that there is no `PEB.UnhandledExceptionFilter` field -- process-wide ring-3 state lives in ntdll. The original draft's `TEB.VehListHead` reservation was dropped by design review (2026-07-19); the ntdll anchor also sidesteps the two-page TEB mapping dependency in `TODO-11 §16`.

> [!IMPORTANT]
> → XREF: `TODO-05 §3` -- VEH handles are not Win32 kernel handles; use a simple opaque pointer. No overlap with the Object Manager handle table.

- [x] `include/kernel/except.h` -- `VECTORED_HANDLER_ENTRY` node (24B: `LIST_ENTRY`@0x00, handler@0x10), `PVECTORED_EXCEPTION_HANDLER` (ms_abi, int32_t return), and the winnt.h disposition triad (-1/0/1), as the shared kernel/ntdll ABI
- [x] `_Static_assert` node size/offsets/alignment for byte-for-byte ntdll agreement; a behavioural test invokes a synthetic ms_abi handler through the typedef to prove convention + 32-bit return width
- [x] No kernel/TEB/PEB anchor field: the list head is ntdll process-global state (design review dropped the per-thread `TEB.VehListHead`) -> XREF: `12-user-platform-sdk/TODO-04 §5` owns the list head + lock
- [x] Scope boundary: `AddVectoredExceptionHandler` / `RemoveVectoredExceptionHandler` / the walk are implemented in ntdll -> XREF: `12-user-platform-sdk/TODO-04 §5`; NOT in `src/kernel/rtl/veh.c`

**Test checkpoint:** `_Static_assert`s pin `VECTORED_HANDLER_ENTRY` size(24)/offsets(0x00,0x10) and the disposition constants (`EXCEPTION_CONTINUE_EXECUTION=-1`, `EXCEPTION_CONTINUE_SEARCH=0`, `EXCEPTION_EXECUTE_HANDLER=1`); a unit test asserts the same at runtime plus invokes a synthetic ms_abi handler through `PVECTORED_EXCEPTION_HANDLER` and confirms each disposition survives the ms_abi + 32-bit-return path. Grep proves NO kernel call site anchors or walks the VEH list (`ki_call_veh_list` / `VehListHead` must not exist in ring 0). Behavioural VEH tests live with the ntdll implementation (D12 T04 §5). Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 387 suites, 0 failures
> **Notes:**
> - **What shipped** -- `include/kernel/except.h` VEH shared ABI: `VECTORED_HANDLER_ENTRY` (24B), `PVECTORED_EXCEPTION_HANDLER` (ms_abi, int32_t return), and the winnt.h disposition triad, all Layer-1 `_Static_assert`ed.
> - **How it integrates** -- kernel publishes the ABI only; ntdll (D12 T04 §5) owns the process-global lock-guarded list. Kernel never anchors/reads/walks it. `test_except.c` adds a node-layout + behavioural-ms_abi test (`TEST_CAT_EXCEPT`).
> - **Downstream effects** -- unblocks the ntdll VEH list against the current node ABI (24B, PROVISIONAL: re-entrancy design at TODO-04 §5 may extend it); §11 (VCH) reuses `VECTORED_HANDLER_ENTRY`. Design review dropped the per-thread `TEB.VehListHead`; adversarial caught the false Windows byte-for-byte claim + re-entrancy premise (adoptions in commit).
> - **Canonical doc** -- `include/kernel/except.h` VEH shared ABI block.
> - **Scope boundary** -- §10 owns the shared node/disposition ABI; TODO-04 §5 owns the list head, lock, add/remove APIs, and the walk; TODO-11 owns the TEB layout (no VEH field added).
> **Verified:** 2026-07-19 | commit `6bf93677` | 4/4 items | build OK | tests 387/387 PASS
> **Accepted:** [H] VEH re-entrancy/lifetime -- a handler may call `RemoveVectoredExceptionHandler` or fault into nested dispatch, so the 24-byte node is provisional; the re-entrant-dispatch protocol is ntdll's -> XREF: `12-user-platform-sdk/TODO-04 §5` (item: "Re-entrant dispatch + removal safety" at line 290)
> **Quality reviewed:** 2026-07-19 | Codex 6x (design, adversarial, re-adversarial, consistency, perf) | 4H+3M fixed, 1H accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"rtl: implement Vectored Exception Handler (VEH) list"`


---

## 11. Vectored Continue Handlers (VCH)

**Prompt:** Confirm the kernel-side boundary for the Vectored Continue Handler (VCH) list -- a separate mechanism from VEH (§10). This section ships NO kernel VCH code; the VCH list, its APIs, and its walk are ntdll-side. On Windows, VCH handlers run when exception dispatch **continues execution** -- after a VEH handler returns `EXCEPTION_CONTINUE_EXECUTION` OR a frame-based (SEH) handler decides to continue -- but **before** execution actually resumes. This lets monitoring/logging handlers observe that an exception was handled without interfering with the dispatch. The VCH list reuses the same `VECTORED_HANDLER_ENTRY` node type as VEH (§10) but is a separate ntdll process-global list head.

Like VEH (§10), all three ring-3 pieces -- `AddVectoredContinueHandler`, `RemoveVectoredContinueHandler`, and the continuation walk -- are implemented in ntdll (D12 T04 §5). The kernel contributes only the shared node ABI (already published by §10); the second list head is ntdll process-global state, NOT a kernel/TEB/PEB field.

> [!NOTE]
> VCH is distinct from VEH. VEH runs BEFORE frame-based handlers; VCH runs when dispatch CONTINUES execution -- on EITHER the VEH-continue or the SEH-continue path -- before resume. Both are per-process and both live in ring 3 (§10 ownership note applies verbatim: the list head is ntdll process-global, never a TEB/PEB anchor). Full order: kernel debugger first-chance (§4) → ring 3 [walk VEH; if VEH continues → VCH → resume; else walk SEH; if a frame handler continues → VCH → resume; if none continue → top-level filter] → kernel second-chance via `NtRaiseException` re-entry (§4) → terminate.

- [x] Confirmed: VCH reuses §10's `VECTORED_HANDLER_ENTRY` node (no new kernel type); `except.h` comment now pins ONE node / TWO ntdll heads, no kernel anchor -> XREF: `12-user-platform-sdk/TODO-04 §5` (item: "VCH list") owns both heads + locks
- [x] Scope boundary: `AddVectoredContinueHandler` / `RemoveVectoredContinueHandler` + the continuation walk are ntdll-side -> XREF: `12-user-platform-sdk/TODO-04 §5` (item: "VCH walk"); grep proves no `ki_call_vch_list` in ring 0

**Test checkpoint:** The kernel publishes NO second anchor (VCH reuses §10's `VECTORED_HANDLER_ENTRY` and the ntdll process-global model); grep proves no `ki_call_vch_list` and no VCH TEB/PEB field exist in ring 0. Behavioural VCH ordering (fires on EITHER continue path -- VEH-continue or SEH-continue -- before resume) is tested with the ntdll implementation (D12 T04 §5). Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 387 suites, 0 failures
> **Notes:**
> - **What shipped** -- `include/kernel/except.h` node comment now states the ONE-node / TWO-ntdll-heads / no-kernel-anchor boundary for VEH+VCH; no new kernel type, function, anchor, or test was added for VCH.
> - **How it integrates** -- VCH reuses §10's `VECTORED_HANDLER_ENTRY` verbatim; the relabelled layout test ("VEH/VCH shared node ABI layout") proves the shared node TYPE's ABI, not runtime lists (D12T04 §5); a grep confirms no ring-0 VCH anchor.
> - **Downstream effects** -- `12-user-platform-sdk/TODO-04 §5` gained concrete VCH items (list head, both APIs, VCH walk, return semantics, ordering/re-entrancy tests) + a both-continue-paths dispatch recipe; design review caught the empty-XREF + tautological-test traps.
> - **Canonical doc** -- `include/kernel/except.h` VEH/VCH shared node ABI block.
> - **Scope boundary** -- §11 owns only the kernel boundary confirmation; TODO-04 §5 owns the second list head, both VCH APIs, and the VCH continuation walk.
> **Verified:** 2026-07-19 | commit `066c44d2` | 2/2 items | build OK | 387 except suites
> **Quality reviewed:** 2026-07-19 | Codex 6x (design, adversarial, consistency, perf, re-adversarial) | 3H+4M fixed | scope: kernel-code-quality

- [x] Commit: `"rtl: implement Vectored Continue Handler (VCH) list"`


---

## 12. Unhandled Exception Filter and WER Hook

**Prompt:** Implement the terminal path reached when `ki_dispatch_exception()` (§4) exhausts all handlers (VEH declined, SEH returned FALSE, debugger second-chance declined, VCH had no effect). The dispatch reaches this section only if no handler claimed the exception. Steps:
The top-level filter itself is **ring-3 state** and is owned by ntdll (D12 T04 §5). This section owns the kernel terminal: what happens when ring 3 declines everything (or cannot run at all), re-entering via `NtRaiseException(first_chance=FALSE)` (§4 step 3). Steps:
1. Second-chance debugger notification (§4), then terminate the process with the exception code as exit status.
2. Emit a structured log entry (→ XREF: `TODO-04 §6`) with the full `EXCEPTION_RECORD` and the first 8 frames via `RtlCaptureStackBackTrace` (§7).
Add a WER (Windows Error Reporting) stub: `WerpReportFault()` calls into a future `werfault.exe` process via a named pipe (leave as a no-op stub for now, log to serial).

> [!WARNING]
> **There is no `PEB.UnhandledExceptionFilter` field -- do not add one.** On Windows `SetUnhandledExceptionFilter` stores an `EncodePointer`-obfuscated pointer in the kernel32 global `BasepCurrentTopLevelFilter`, NOT in the PEB. Inventing a PEB field would also collide with `TODO-11 §17`, which rebuilds the post-0x28 PEB region to authoritative x64 `_PEB` offsets so a real ntdll can read it unpatched. The Impossible OS analogue is a user-runtime process-global in ntdll -> XREF: `12-user-platform-sdk/TODO-04 §5`.

- [/] Kernel terminal on `NtRaiseException(first_chance=FALSE)` second-chance -> terminate with the exception NTSTATUS as exit status -- DEFERRED: needs the §5 terminate primitive (see Deferred stamp)
- [/] Terminate cleanly when ring 3 CANNOT be reached at all (no ntdll / §5 delivery failed) -- DEFERRED: same §5 terminate primitive; `task_exit` uses the global `current_task` cursor, unsafe from a trap frame (see Deferred stamp)
- [x] Structured crash log: `wer_write_crash_report` adds `status` (NTSTATUS), `fault_addr`, `stack` (frame 0 = faulting RIP + up to 7 kernel frames via `RtlCaptureStackBackTrace` §7); `user_trace_available:false` (user walk deferred)
- [x] `WerpReportFault(code, addr)` serial-only stub via pure `wer_format_fault_line` (`wer: fault report code=0x.., addr=0x..`); fault-safe; werfault.exe reporter ntdll-side -> XREF: `12-user-platform-sdk/TODO-04 §5`
- [x] Scope boundary: `SetUnhandledExceptionFilter` / `UnhandledExceptionFilter` / the crash dialog are ntdll-side (no kernel PEB field added) -> XREF: `12-user-platform-sdk/TODO-04 §5`

**Test checkpoint (shipped subset):** Every user-fault terminal calls `WerpReportFault()`, whose logical line is `"wer: fault report code=0x<ntstatus>, addr=0x<addr>\n"` (verified byte-exact by `test_wer_format_fault_line`; `serial_write` converts the LF to CRLF on the UART). The enriched JSON report (`status`/`fault_addr`/`stack`) is emitted ONLY by `except.c`'s general-fault terminal (#GP/#NP/#SS/etc.); the `vmm.c` #PF terminal (the common access-violation path) emits the serial `WerpReportFault` line ONLY -- no JSON (a safe VFS-in-#PF path is deferred), and the `idt.c` unhandled-vector fallback is `panic_screen`-only. The terminal ACTION stays `panic_screen` (per-process termination is the deferred §5 work); grep proves no `PEB.UnhandledExceptionFilter` field was added. The full ring-3-decline -> `NtRaiseException(first_chance=FALSE)` -> terminate round-trip awaits §5. Test on: QEMU WHPX + TCG.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | WER line format asserted byte-exact; 0 failures

> **Notes:**
> - **What shipped** -- `WerpReportFault()` + pure `wer_format_fault_line` serial stub in `src/kernel/wer.c`; `wer_write_crash_report` enriched with `status`/`fault_addr`/`stack` (up to `WER_CRASH_MAX_FRAMES`=8, frame 0 = faulting RIP).
> - **How it runs** -- the `except.c` and `vmm.c` #PF user terminals call `WerpReportFault` (serial-only via `serial_write`, fault-safe) before `panic_screen`; the VFS `wer_write_crash_report` runs only on `except.c`'s user-fault terminal (abort/kernel paths are `panic_screen`-only).
> - **Downstream effects** -- items 1-2 (terminate-instead-of-panic) deferred to §5's `KI_EXCEPTION_TERMINATE` primitive; Codex design-review adoptions (4 findings) in the commit message.
> - **Canonical doc** -- `include/kernel/wer.h` (WER hook + crash-report ABI).
> - **Scope boundary** -- §12 owns the WER hook + crash log; per-process TERMINATION is §5 (needs TODO-07 per-CPU cursor); the top-level filter + crash dialog are ntdll (TODO-04 §5).

> **Verified:** 2026-07-19 | commit `eaf3b4c1` | 3/5 items | build OK | except 393+16 PASS, smoke PASS (KVM 2.66s)
> **Accepted:** [H] `panic_screen`'s `serial_write` can self-deadlock on `g_serial_lock` in a #DF/#MC/NMI abort context (pre-existing; §12 removed its own idt.c abort-path serial) -> XREF: 01-boot-platform/TODO-10 §2 (item: "Abort-safe serial for the panic path" at line 155)
> **Accepted:** [M] serial-first WER ordering: `serial_write` (no UART timeout) runs before the VFS report, so a stuck UART could stall it; serial-first still protects the likelier VFS-hang, and a bounded serial primitive removes the risk -> XREF: 01-boot-platform/TODO-10 §2 (item: "Abort-safe serial for the panic path" at line 155)
> **Accepted:** [M] unregistered ring-3 #MF/#XM reaching the idt.c panic fallback get no persistent WER report (the fallback is `panic_screen`-only: IF=0 interrupt-gate + abort-context make serial/VFS unsafe there); they gain WER once mapped into the recoverable terminal -> XREF: 02-kernel-core/TODO-23 §3 (item: "FP-exception delivery" at line 207)
> **Deferred:** [H] terminal PROCESS-TERMINATION (items 1-2) needs the §5 `KI_EXCEPTION_TERMINATE` primitive (per-CPU current-thread cursor + per-CPU idle frame + caller-owned scratch release); the terminal action stays `panic_screen` until then -> XREF: TODO-23 §5 (item: "Delivery failure ... returns a new `KI_EXCEPTION_TERMINATE` disposition"); `03-memory-concurrency/TODO-07-smp-phase2.md` (item: "Per-CPU current-thread cursor" at line 119)
> **Quality reviewed:** 2026-07-19 | Codex 16x (design, adversarial, consistency, perf, re-adversarial) | 3H+9M+1L fixed, 1H+2M accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"rtl: WER fault hook + enriched crash log (unhandled-filter terminate deferred to s5)"`


---

## 13. Kernel Safe Probing (`ProbeForRead`, `ProbeForWrite`)

**Prompt:** Repair and complete the EXISTING kernel probing API so a ring-3 pointer cannot crash or corrupt the kernel. `ProbeForRead`/`ProbeForWrite` already exist (declared `include/kernel/nt/zw.h:57-58`, implemented `src/kernel/nt/ssdt.c:60` and `:95`), and `ProbeForWrite` is literally `return ProbeForRead(...)` -- a range/alignment/overflow check with no page touch, so it cannot prove writability. Extend those symbols IN PLACE; do NOT create a second `probe.h`/`probe.c` pair with the same names. What is missing:
- `ProbeForWrite` page-touch: touch the first byte of each page to force a present+writable mapping and catch write-protected pages.
- `try_copy_from_user(dst, src, n)` / `try_copy_to_user(dst, src, n)` -- fault-recoverable copies (Linux `__ex_table` in spirit). A static exception table keyed by the exact faulting instruction RIP: the #PF handler (§2), after the guard/swap/mmap chain, redirects a user-range fault taken at a guarded copy to its fixup. This closes the TOCTOU between probe and dereference that a range check alone cannot.

> [!IMPORTANT]
> The recovery mechanism is a **static, RIP-keyed exception table** (`__uaccess_*_fault`/`_fixup` global labels), NOT a per-CPU `safe_return_rip` slot: the faulting RIP alone identifies the guarded instruction, so there is no per-CPU mutable state and no `cli` -- inherently SMP/preempt-safe, and un-misredirectable by a nested NMI/MCE #PF.

> [!WARNING]
> **The range check + fault fixup is fault RECOVERY, NOT user/kernel isolation.** There are no per-process page tables yet: user stacks are `kmalloc`'d from the kernel heap and share 2 MiB pages with kernel data, and SMEP/SMAP stay off until that lands (`docs/infrastructure/bare-metal-gotchas.md`). An in-range pointer that aliases mapped kernel data resolves WITHOUT faulting, so `try_copy_*` cannot stop a mapped-alias write -- it only turns an unmapped/read-only/misaligned user pointer into a graceful error instead of a bugcheck. True isolation is owned by the per-process page-table work -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §2` (item: "Per-process physical isolation: `vmm_create_user_pml4()` must allocate unique physical pages per process" at line 121).

- [x] `try_copy_from_user` / `try_copy_to_user` (`ssdt.c`, decl `zw.h`): probe range then copy via guarded `__uaccess_copy`; a #PF on the user operand returns `STATUS_ACCESS_VIOLATION`, not a bugcheck. Extended in place, no `probe.h`
- [x] Static RIP-keyed exception table (`__uaccess_*_fault`/`_fixup` labels, `cpu_security.c`); `page_fault_handler` redirects a `CR2 < MM_USER_END` guarded-instruction fault to its fixup, after guard/swap/mmap -- NO per-CPU slot, NO cli
- [x] `copy_from_user`/`copy_to_user` (`cpu_security.c`) delegate to `__uaccess_copy` -- all 16 callers gain fault recovery; int/-1 ABI + SMAP + test injection kept. Per-call `try_copy_*` migration owned by `TODO-21 §11`/`TODO-05 §11`
- [x] Re-register `#PF` (ISR 14) in `boot_phase1` after `except_init` (`vmm_register_page_fault_handler`) -- closes the latent §2 bug where `idt_init` zeroed `handlers[14]`

**Test checkpoint:** `ProbeForRead(user_addr, 8, 4)` on a valid mapped user page succeeds; `ProbeForRead(kernel_addr, 8, 4)` and `ProbeForWrite(kernel_addr, ...)` raise `STATUS_ACCESS_VIOLATION` -- do NOT panic. `ProbeForWrite` touches each page and proves writability. `try_copy_from_user` from an unmapped user page returns `STATUS_ACCESS_VIOLATION` via the RIP-keyed exception-table redirect and the kernel keeps running. No POST16 (post-Phase-3 runtime fault path; klog/serial is the diagnostic surface). Test on: QEMU WHPX + TCG. Verify on bare metal -- TLB behavior differs.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 6 new suites (ProbeForWrite kernel/NULL/zero, __uaccess_copy success, __uaccess_touch_w, try_copy_{from,to}_user probe gate), 0 failures. Live unmapped-page fault-recovery is serial-validated (Verification).

> **Notes:**
> - **What shipped** -- fault-recoverable user access: direction-split guarded `__uaccess_copy_from`/`_to` + `__uaccess_touch_w` (`cpu_security.c`), `try_copy_{from,to}_user` + full-range page-touch `ProbeForWrite` (`ssdt.c`), and the `page_fault_handler` redirect (`vmm.c`); 8 `except` suites.
> - **How it integrates** -- `copy_from_user`/`copy_to_user` delegate to `__uaccess_copy_{from,to}` (all 16 callers gain recovery unchanged); a #PF at a `__uaccess_*_fault` label matching the fault DIRECTION on a `CR2 < MM_USER_END` address redirects to its fixup after the pager chain.
> - **Downstream effects** -- unblocks §5 (`try_copy_to_user` for KiUserExceptionDispatcher) and the deferred user-buffer handlers in `TODO-21 §11` / `TODO-05 §11`; also closes the latent §2 `handlers[14]`-NULL bug.
> - **Canonical doc** -- the fault-recoverable user-access block in `src/kernel/cpu_security.c` (mechanism owner).
> - **Scope boundary** -- §13 owns fault RECOVERY (unmapped/RO/misaligned -> error); it is NOT an isolation boundary (mapped-alias isolation owned by `03-memory-concurrency/TODO-01 §2`). Ring-3 delivery is §5, kernel SEH is §14.
> **Verified:** 2026-07-18 | ship `c2d25f63` + review fixes | 4/4 items | build OK | smoke PASS (KVM 2.66s) | 21775 kernel + 16 user PASS
> **Accepted:** [H] ALPC recv raw-`memcpy` concurrent-unmap DoS (pre-existing; §13 provides the fix primitive, does not regress it) -> XREF: `02-kernel-core/TODO-24 §7` (item: "Recv-buffer TOCTOU DoS: `alpc_receive_only` + `alpc_sync_request`" at line 313)
> **Accepted:** [H] `KERNEL_ACCESS_USER_BEGIN/END` BSP-global `cpu_has` SMAP gate #UDs on a feature-skewed AP (shared macro; `copy_*_user` affected identically) -> XREF: `02-kernel-core/TODO-10 §2` (item: "`KERNEL_ACCESS_USER_BEGIN/END` gate on BSP-global `cpu_has`" at line 143)
> **Accepted:** [H] Fixed-size query handlers probe the raw user `Length`, so §13's full-range `ProbeForWrite` touch is an O(pages) DoS (caller-side; NtQueryTimer exemplar) -> XREF: `02-kernel-core/TODO-12 §10` (item: "Fixed-size query handlers probe the raw user `Length`" at line 548)
> **Quality reviewed:** 2026-07-18 | Codex 10x (design, adversarial x4, re-adversarial, consistency x2, perf x2) | 1C+3H+4M fixed, 3H accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"kernel: add try_copy_{from,to}_user + ProbeForWrite page-touch; re-register #PF"`


---

## 14. Kernel-Mode `__try`/`__except` for Drivers

**Prompt:** Enable kernel-mode structured exception handling so drivers can wrap dangerous operations (MMIO access, DMA buffer reads) in `__try`/`__except`. The mechanism differs from user-mode: there is no user stack to push onto. Instead:
1. Add `KI_EXCEPTION_REGISTRATION` -- a per-thread (kernel stack) record pushed by `__try` lowering code at the head of the thread's kernel stack frame.
2. `ki_raise_kernel_exception(EXCEPTION_RECORD *, CONTEXT *, struct interrupt_frame *)` (declaration + UNHANDLED stub landed by §2; returns `KI_EXCEPTION_DISPOSITION`) -- walks the kernel-mode exception chain (stored in the per-CPU `current_thread->kernel_exception_list`), calls filter expressions, and invokes `RtlUnwindEx` (§9) for matching handlers.
3. Patch the kernel's `.pdata` section to include `UNWIND_INFO` for critical paths (requires linker script changes to emit `.pdata` for `clang-19`).
4. Guard against re-entrancy: if a kernel exception occurs inside a kernel exception handler, escalate directly to `KeBugCheckEx` (panic with structured code).

> [!IMPORTANT]
> → XREF: `TODO-07 §3` -- kernel `__try` must only be used at `PASSIVE_LEVEL` or `APC_LEVEL`; add `IRQL_REQUIRE_AT_MOST(APC_LEVEL)` assertion at the start of `ki_raise_kernel_exception`.
> **Fault-safe entry (from §2 review):** the IRQL check MUST be non-logging in fault context. A `#PF` preserves the faulting IRQL, so a fault at elevated IRQL -- possibly already inside `klog` holding `s_klog_lock` -- must NOT route through a `klog`-logging `IRQL_REQUIRE_AT_MOST`. Use a fault-safe (non-`klog`) check and bypass kernel SEH at elevated IRQL, matching §2's no-`klog`-on-kernel-fault rule.

- [x] `except.h` -- `KI_JMP_BUF`/`KI_EXCEPTION_REGISTRATION`/`KI_EXCEPTION_FILTER` types + `KI_TRY`/`KI_EXCEPT`/`KI_END_TRY`/`KI_EXCEPTION_FRAME` macros; `kernel_exception_list` head in `struct thread` (registration-list + setjmp scheme).
- [x] `except.c` -- real `ki_raise_kernel_exception` (fault-safe IRQL/IF gate, one `thread_current()` snapshot + kstack-bounds validation, filters, unlink-through-target, trap-frame rewrite) + chain helpers + `except_seh.asm` `ki_seh_setjmp`.
- [x] `.pdata`/`UNWIND_INFO` -- N/A: the registration scheme resumes by trap-frame rewrite, independent of table unwind, so no linker `.pdata` emission is needed (design-review-accepted v1 scope).
- [x] Re-entrancy guard: per-CPU `ki_seh_dispatch[]` flag -- a fault re-entering the walk/filter (incl. cross-vector) escalates to `KeBugCheckExFrame(0x1E KMODE_EXCEPTION_NOT_HANDLED)`.
- [ ] Full `EXCEPTION_COLLIDED_UNWIND` for driver `__try`/`__finally`: nested-unwind detection, `ScopeIndex`, two-pass termination-handler unwind -- larger than the `__try`/`__except` v1 here (design review accepted deferring). -> XREF: `TODO-23 §9`
- [ ] Boot/main-thread `KI_TRY`: declines on task 0 (boot stack not tracked in `stack_base`, which doubles as the `kfree` target). kthread drivers work; decouple the SEH stack-bounds source from the `kfree` target for main-thread probing.
- [x] Live fault-recovery test wraps a dangerous op (guarded write to an unmapped VA) proving "caught, not panic" -- `test_ki_try_recovers_live_kernel_fault` (the AHCI-MMIO-read analog).

**Test checkpoint:** `KI_TRY { *(volatile uint32_t*)unmapped = 0; } KI_EXCEPT(reg) { } KI_END_TRY;` -- handler fires, kernel continues (validated live: a real #PF caught and resumed). A fault re-entering the walk/filter → `KeBugCheckEx(0x1E)`. The walk is fault-safe (klog-free), so no serial line on the hot path; not boot-path, so no `POST16`. Tested on QEMU TCG (WSL). Verify on bare metal + WHPX.

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 425 kernel + 16 user tests, 0 failures

> **Notes:**
> - **What shipped** -- `KI_TRY`/`KI_EXCEPT` kernel SEH: `except.h` types+macros, `except.c` `ki_raise_kernel_exception` + chain helpers, `except_seh.asm` `ki_seh_setjmp`, `kernel_exception_list` in `struct thread`; 6 new tests incl. a live #PF recovery.
> - **How it runs** -- a kernel `#PF`/`#GP` reaches `ki_raise_kernel_exception`, which walks the thread's chain and rewrites the trap frame so `IRETQ` resumes in `KI_EXCEPT`; fault-safe, declines above `APC_LEVEL`/IF-clear/stack-mismatch.
> - **Downstream effects** -- drivers can now guard MMIO/DMA reads on kthreads; design-review adoptions (SMP cursor, re-entrancy, node lifetime, returns_twice/RFLAGS) in the commit message.
> - **Canonical doc** -- `include/kernel/except.h` (KI_TRY/KI_EXCEPT contract + SMP/lifetime safety block).
> - **Scope boundary** -- §14 owns `__try`/`__except` v1; `__try`/`__finally` collided-unwind and boot/main-thread `KI_TRY` are tracked open items here; the per-CPU current-thread cursor is TODO-07.

> **Verified:** 2026-07-19 | commit `95a4e592` | 5/7 items | build OK | tests 434/434 PASS
> **Accepted:** [H] pre-existing SMP quiescence gap: a joined/reaped thread's kernel stack can be freed while a KI_TRY victim still runs on it (stack UAF predates SEH; the chain-clears + walk bounds checks contain the SEH surface) (reason: scope) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md (item: "thread_join / thread_free_stacks off-CPU barrier" at line 126)
> **Deferred:** [M] `__try`/`__finally` EXCEPTION_COLLIDED_UNWIND two-pass unwind -- larger than the `__try`/`__except` v1 shipped (reason: infra) -> XREF: 02-kernel-core/TODO-23 §14 (item: "Full `EXCEPTION_COLLIDED_UNWIND` for driver `__try`/`__finally`" at line 627)
> **Deferred:** [M] boot/main-thread `KI_TRY` declines -- boot stack is not tracked in `stack_base` (the `kfree` target) (reason: infra) -> XREF: 02-kernel-core/TODO-23 §14 (item: "Boot/main-thread `KI_TRY`" at line 628)
> **Quality reviewed:** 2026-07-19 | Codex 8x (design, adversarial, consistency, perf, re-adversarial) | 4H+3M fixed, 1H accepted-XREF | scope: kernel-code-quality

- [x] Commit: `"kernel: implement kernel-mode __try/__except via KI_EXCEPTION_REGISTRATION"`


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

> **Deferred:** [H] the ceiling cleared, but it was never this section's only blocker: the Implementation Order declares `D10T10 §8` (Linux-compat signal delivery) as a hard prerequisite and that work is entirely open, so the Test checkpoint here -- working `rt_sigaction` / `sigaltstack` / `sigreturn` behavior -- cannot pass. `CONFIG_LINUX_COMPAT` stubbing would compile but not satisfy it (reason: external prerequisite) -> XREF: `10-platform-services/TODO-10 §8`


---

## 16. Exception Dispatch Telemetry

**Prompt:** Neither Windows nor Linux logs exception dispatch decisions for post-mortem analysis. Impossible OS can record the full exception dispatch trace -- which handler was tried, what it returned, how many frames were unwound -- into the JSON structured log system (TODO-04). This enables a developer to see exactly why an exception was or wasn't caught, without attaching a debugger.

> [!TIP]
> **Competitive edge:** Windows WER only captures the final crash state. Linux core dumps only capture memory. Neither records the dispatch decision chain: "VEH handler at 0x1234 returned CONTINUE_SEARCH, SEH filter at 0x5678 returned EXCEPTION_EXECUTE_HANDLER, unwound 3 frames." This telemetry turns exception handling from a black box into a fully observable pipeline.

- [x] `except.c` `except_log_dispatch(rec, handler, disposition)` emits a JSON event per kernel-observed dispatch decision; wired into the klog-safe ring-3 legs of `ki_dispatch_exception`, not the lock-free kernel-SEH walk
- [x] Log entries: exception `code`, fault `addr`, `handler` (unhandled/debugger at the kernel boundary), `disposition`, `pid`/`tid`; `frames_unwound` is `null` in ring 0 (no in-kernel unwind). Pure `except_format_dispatch_json` builds + escapes it
- [x] Event is a flat JSON object emitted via `klog_unrated` (type `"exception_dispatch"`); shows directly on serial, in `events.jsonl` as the `msg` field. Native top-level structured fields → XREF: `02-kernel-core/TODO-32 §8`
- [x] Rate limiting: per-process 100/1s + system-wide 256/1s aggregate (both packed-atomic CAS via `except_telem_rate_gate`); `klog_unrated` skips the subsystem cap. Sink-occupancy async lane → XREF: `02-kernel-core/TODO-32`
- [x] Compile-time `CONFIG_EXCEPT_TELEMETRY` (`#if`, authoritative 1/0 from `Makefile EXCEPT_TELEMETRY=on/off` + flavor stamp); default on. Release-off default awaits a release build flavor (none exists yet)
- [ ] Ring-3 per-handler telemetry (VEH/SEH/VCH handler type + address + unwound frame count) → XREF: `12-user-platform-sdk/TODO-04 §5` `KiUserExceptionDispatcher` (ntdll owns the ring-3 chain; kernel has no VEH/VCH walker)

**Test checkpoint:** Unit tests (`SUITE=except`) validate `except_format_dispatch_json` (shape / disposition map / escaping / truncation-drop) and `except_telem_rate_gate` (window + cap + NULL fail-open). Live: a user-mode access violation emits `{"type":"exception_dispatch","code":"0xc0000005","handler":"unhandled","disposition":"continue_search","frames_unwound":null,...}` on serial (the kernel-boundary handler is `unhandled`/`debugger`; ring-3 `veh`/`seh`/`vch` names come from TODO-04 §5). Rate: one process exceeding 100 events/1s window is dropped. `EXCEPT_TELEMETRY=off` build: no `exception_dispatch` string in `kernel.exe` (verified via `strings`). Test on: QEMU WHPX + TCG.

- [x] Commit: `"kernel: add exception dispatch telemetry to JSON structured log"`

> **Test runner:** `scripts\debug\kernel\run-except-tests.bat` (SUITE=except) | 452 suites, 0 failures

> **Notes:**
> - **What shipped** -- `except_log_dispatch` + pure `except_format_dispatch_json`/`except_telem_rate_gate` (except.c ~150 LOC); `klog_unrated` rate-bypass; `task.except_telem_rate`; `EXCEPT_TELEMETRY` Makefile flag + flip stamp.
> - **How it integrates** -- klog-safe ring-3 legs of `ki_dispatch_exception` (not the lock-free kernel-SEH walk); per-process packed-atomic 100/1s gate; `#if CONFIG_EXCEPT_TELEMETRY` (default on) compiles the hook + event string out when off.
> - **Downstream effects** -- ring-3 per-handler telemetry owned by TODO-04 §5; per-process attribution becomes exact when TODO-07 §3 per-CPU cursor lands. Codex design-review adoptions in the commit message.
> - **Scope boundary** -- §16 owns the kernel-boundary emitter + shared `exception_dispatch` JSON schema; the ring-3 VEH→SEH→VCH chain telemetry is TODO-04 §5 (the kernel has no ring-0 VEH/VCH walker).
> **Verified:** 2026-07-19 | commit `ddd82c57` | 5/6 items | build OK | tests 455/455, smoke PASS
> **Accepted:** [M] telemetry emission adds a synchronous `klog_disk` flush+alloc on the ring-3 fault leg (pre-existing same-leg `klog` behavior; kernel-quality-auditor rated LOW) -> XREF: `02-kernel-core/TODO-32 §2` (item: "`klog_v2` ... atomic enqueue, returns" at line 88)
> **Quality reviewed:** 2026-07-19 | Codex 12x (design, adversarial, re-adversarial, consistency, perf) | 1H+8M fixed, 1H+3M accepted-XREF | scope: kernel-code-quality


---

## 17. Guard-Page Stack Auto-Grow -- Commit/Reserve Tracking, Guard Re-Arm, Terminal Overflow

> **Split from §2 (2026-07-18).** §2 owns #PF triage and dispatch routing; this section owns the growable-stack case of the not-present-user branch. Windows commits a thread stack lazily: a small committed region backed by real pages, a larger reserved region, and a one-page guard just below the committed low-water mark. Touching the guard commits one more page and MOVES the guard down; running out of reserve raises `STATUS_STACK_OVERFLOW`. This section implements that for user (and kernel-task) stacks.

**Prompt:** Add per-stack `{reserve_low, commit_low, guard_page}` tracking and a `vmm_try_grow_stack(fault_addr, is_user)` hook called from §2's not-present-user (and kernel-task-stack) branch. On a guard hit inside `[reserve_low, commit_low)`: commit one page (map + zero), move the guard one page down, re-arm it (one-shot), and retry the instruction. A fault BELOW `reserve_low` (jumped over the guard, or reserve exhausted) is terminal: deliver `STATUS_STACK_OVERFLOW` WITHOUT touching the exhausted stack (dispatch on a safe path / the current labeled panic until §5 lands), and never grow past the reserve into an adjacent mapping.

> [!IMPORTANT]
> → XREF: `§2` -- the #PF triage that calls `vmm_try_grow_stack()`; ships first.
> → XREF: `§3` -- `#SS` maps to `STATUS_STACK_OVERFLOW`; the terminal path reuses that mapping.
> → XREF: `TODO-11 §5` -- `task->peb`/thread stack bounds for the user-mode range check.

> [!NOTE]
> **DEFERRED (2026-07-19) -- blocked on four unbuilt prerequisites.** A pre-implementation Codex design review (3 High + 1 Medium), a kernel-explorer integration map, and a concurrency-evidence inventory converged on a no-ship: a correct, SMP-safe, address-space-correct auto-grow is not buildable until the following land. Verified at file:line:
> 1. **Per-CPU current-thread cursor** -- the #PF handler must know the FAULTING task's `cr3` to look up its growable stack and commit via `vmm_map_user_page(cr3,...)`. A global VA-keyed registry is wrong: secondary user-stack VAs stride from `USER_THREAD_STACK_BASE` and are REUSED across per-process PML4s (`task.c:3795-3810`). The cursor is absent (`schedule_now` has no per-CPU current-thread) -- the same blocker that deferred §5/§16.
> 2. **Reserved-VA lazy-commit layout** -- every stack today is a contiguous identity-mapped physical alloc with an adjacent allocation directly below (`task.c:676`, `smp.c:378`, `gdt.c:88`); there is NO reserve headroom to grow into. Growable stacks need a reserved user-VA window backed on demand (demand paging + a per-process frame-map primitive).
> 3. **Safe terminal delivery/termination** -- reserve-exhausted overflow must deliver `STATUS_STACK_OVERFLOW` WITHOUT touching the exhausted stack; ring-3 delivery + process termination is itself deferred (§5), so the terminal path can only `panic_screen` today, not terminate the process.
> 4. **PMM + page-table SMP locking** -- the COMMIT path (`pmm_alloc_frame` + `vmm_map_page`) is unlocked and cannot run inside a short spinlock critical section; per-process PML4 + kernel PT-creation locks are unbuilt.
> When these land: implement the mechanism (per-`cr3` growable-stack registry; GROWING-state commit performed OUTSIDE the registry lock with rollback; guard move + one-shot re-arm; a distinct TERMINAL result routed through the existing #PF per-CPU scratch path) and convert user-thread stacks to the reserved/lazy-commit layout. Kernel-task stacks are NOT growable on the current non-IST #PF path (the handler runs on the exhausted stack -> #DF); that requires separate safe-entry-stack/IST work.

- [ ] Guard-page / stack-growth detection with a configurable stack reserve (default 1 MiB); track per-stack committed and reserved bounds separately
- [ ] Successful grow: commit one page, MOVE the guard one page down and re-arm it (one-shot); reject a fault that jumped OVER the guard instead of growing into it
- [ ] Reserve exhausted = terminal: deliver `STATUS_STACK_OVERFLOW` WITHOUT touching the exhausted stack; never grow past the reserve into adjacent mappings
- [ ] SMP-safe: serialize concurrent grows on the same stack; the guard-table update and PTE map must not race a second CPU faulting the same guard

**Test checkpoint:** A recursive user thread that walks its stack down past the committed low-water mark auto-grows one page per guard hit and continues; serial shows the commit_low moving down. A runaway recursion that exhausts the 1 MiB reserve delivers `STATUS_STACK_OVERFLOW` (labeled panic until §5) without a double-fault. A fault that jumps over the guard (large `alloca`) is rejected as terminal, not grown. Test on: QEMU WHPX + TCG. Verify on bare metal.

- [ ] Commit: `"mm: guard-page stack auto-grow with commit/reserve tracking"`

> **Deferred:** [H] auto-grow needs the FAULTING task's `cr3` at #PF time (a global VA-keyed registry is wrong: per-process PML4s reuse secondary-stack VAs at `task.c:3795-3810`) -> XREF: `03-memory-concurrency/TODO-07-smp-phase2.md` (item: "Per-CPU current-thread cursor" at line 120)
> **Deferred:** [H] growable stacks need a reserved-VA window backed on demand + a per-process frame-map primitive (stacks are contiguous identity-mapped today, no headroom) -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` (item: "`MEM_COMMIT` path: mark region committed; zero-fill backing frames on first access" at line 124)
> **Deferred:** [M] reserve-exhausted terminal must deliver `STATUS_STACK_OVERFLOW` without touching the exhausted stack; ring-3 delivery/termination is itself deferred -> XREF: `02-kernel-core/TODO-23 §5` (item: "clear the per-CPU scratch slot THEN hand to a guaranteed idle-frame terminate primitive" at line 309)
> **Deferred:** [M] COMMIT (`pmm_alloc_frame` + `vmm_map_page`) is unlocked and cannot run under a short spinlock; needs PMM + per-process PML4 locking -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` (item: "Per-process PML4 spinlock" at line 129)


---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11               | 🐧 Linux            | 🚀 Impossible OS                                             |
| --- | ----------------------------- | --------------------- | ------------------ | ----------------------------------------------------------- |
| 💎   | EXCEPTION_RECORD/CONTEXT      | ✅ ntdll               | ❌                  | ✅ §1 `except.h`                                             |
| 💎   | #PF user/kernel triage        | ✅                     | ✅                  | ✅ §2 triage+dispatch                                        |
| 💎   | Fault->NTSTATUS mapping       | ✅                     | ✅ signals          | ✅ §3 map (deliver §5)                                       |
| 💎   | Lazy stack commit/auto-grow   | ✅ guard commit        | ✅ expand_stack     | ⬜ §17 reserve/commit                                        |
| 💎   | #DB/#BP debugger routing      | ✅                     | ✅ ptrace           | ◐ §3 map / §4 KD                                            |
| 💎   | #CP CET shadow-stack          | ✅ 24H2+               | ✅ 6.6+             | ◐ §3 handler (CET-gated; delivery §5)                       |
| 💎   | Debugger 1st/2nd-chance       | ✅ KiDebugRoutine      | ✅ ptrace           | ◐ §4 KiDebugRoutine (deliver §5)                            |
| 💎   | Kernel-mode bugcheck terminal | ✅ KeBugCheckEx        | ✅ oops/panic       | ◐ §4 KeBugCheckExFrame 0x1E/0x3B                            |
| 💎   | KiUserExceptionDispatcher     | ✅                     | ❌                  | ⬜ §5                                                        |
| 💎   | x64 table-based unwind        | ✅ UNWIND_INFO         | ✅ .eh_frame        | ✅ §6 engine (kernel meta: T18 §5)                           |
| 💎   | Dynamic/JIT function tables   | ✅ RtlAddFunctionTable | ✅ __register_frame | ✅ §6 registry + callback                                    |
| 💎   | Kernel stack walking          | ✅ RtlCaptureStack     | ✅ stack_trace      | ✅ §7 RBP walk + fault-safe read                             |
| 💎   | SEH + __C_specific_handler    | ✅                     | ❌                  | ⬜ ring-3 → T04 §5 (§8 re-owned)                             |
| 💎   | RtlUnwindEx + __finally       | ✅                     | ❌                  | ✅ §9 unwind-to-target + RtlRestoreContext                   |
| 💎   | VEH list                      | ✅ ntdll               | ❌                  | ✅ §10 node ABI (list D12T04 §5)                             |
| 💎   | VCH list                      | ✅ ntdll               | ❌                  | ✅ §11 kernel boundary (list D12T04 §5)                      |
| 💎   | Unhandled exception filter    | ✅ WER                 | ✅ core dump        | 🟡 WER hook + crash log (§12); terminate → §5                |
| 💎   | `__fastfail` / INT 0x29       | ✅ 0xC0000409          | ❌                  | ◐ §3 handler (DPL=3; per-proc term §5)                      |
| 💎   | CONTEXT ContextFlags + FXSAVE | ✅ 0x4D0 ABI           | ✅ ucontext_t       | ✅ §1 layout                                                 |
| 💎   | Fault-recoverable usercopy    | ✅ kernel SEH          | ✅ `__ex_table`     | ✅ §13 try_copy_* / RIP-keyed table                          |
| 💎   | ProbeForRead/Write page-touch | ✅ ProbeForWrite       | ✅ copy_from_user   | ✅ §13 page-touch write probe                                |
| 💎   | Kernel __try/__except         | ✅                     | ❌                  | 🟡 §14 KI_TRY/KI_EXCEPT v1 (regn+setjmp; __finally deferred) |
| 💎   | POSIX signal from faults      | ❌                     | ✅                  | ⬜ §15 compat                                                |
| 💎   | sigaltstack overflow          | ❌                     | ✅                  | ⬜ §15 compat                                                |
| ⭐   | Dispatch telemetry            | ❌                     | ❌                  | 🟡 §16 kernel-boundary JSON; ring-3 chain → T04 §5           |
| ⭐   | Exception budget / storm ctrl | ❌                     | ❌                  | ✅ §16 per-proc 100/1s + 256/1s aggregate gate               |

> **After parity items:** Impossible OS matches Windows on the full SEH/VEH/VCH pipeline and matches Linux on POSIX signal delivery. Exclusive differentiators: **dispatch telemetry** recording the full VEH → SEH → VCH handler chain into the JSON structured log (neither WER nor core dumps capture the decision sequence); and **exception storm control** rate-limiting per-process exceptions to prevent DoS from runaway JITs or intentional exception flooding. (Safe probing itself is parity: §13 ships a standard RIP-keyed usercopy fixup, matching Windows kernel SEH and Linux `__ex_table`.)

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
  - `EXCEPTION_RECORD` for `#CP` (if CET supported) has code `STATUS_STACK_BUFFER_OVERRUN` + fast-fail subcode 0x39 in `ExceptionInformation[0]`
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
  - `VECTORED_HANDLER_ENTRY` node size/offsets and disposition constants (-1/0/1) match the ntdll ABI (`_Static_assert` + runtime check); a behavioural test invokes a synthetic ms_abi handler through the typedef and confirms each disposition survives the ms_abi + 32-bit-return path
  - No ring-0 VEH/VCH walker exists: `ki_call_veh_list` / `ki_call_vch_list` are absent (registration/dispatch behaviour is tested with the ntdll implementation, D12 T04 §5)
  - `CONTEXT.ContextFlags` gates restoration: a CONTEXT with FLOATING_POINT clear leaves XMM state untouched on `NtContinue`
  - `NtContinue` rejects a non-canonical RIP, a kernel-mode CS selector, and a noncontinuable exception
  - `ProbeForWrite` on a read-only user page fails (proves the page touch, not just the range check)
  - `try_copy_from_user` from an unmapped page returns an error via the RIP-keyed exception-table redirect (`__uaccess_copy_from_fault` -> `_fixup`) and the kernel keeps running
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
