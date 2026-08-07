---
schema_version: 1
id: native-api-ssdt
domain: 02-kernel-core
status: active
title: "TODO-12 -- Native API Layer (Nt/Zw)"
---

# TODO-12 -- Native API Layer (Nt/Zw)

> **Validated:** 2026-06-28 | validate-todo-file clean (structure / IO table / XREF / test wiring); added 29 missing inter-section `---` separators
> **Gap-audited:** 2026-06-28 | parity-research-analyst (sonnet, todo-plan) 35-feature inventory + 13 gaps; codex-gap-audit red-team (needs-attention, 4 valid). Filed: G4 GUI-thread conversion (§4, XREF D08 T15/T16), G9 RtlNtStatusToDosError broad coverage (§11 false-completeness, 12->classes), G12 NtGetNextProcess/Thread 0x0202/0x0203 (§7, XREF T21 §4), G13 ProcessMitigationPolicy classes (§7, hands SystemCallDisablePolicy to §25). Already-owned (Branch C, not filed): G5 Win32k-lockdown->§25 filter, G6 KUSD-time->D02 T08 §12 / T11 §9, G8 KPTI->D02 T10 KPTI/TODO-33, G12/G13 cross-owned T21. Rejected: G2 Win11-internal SSDT byte-format minutiae.

> **Goal:** Replace the ad-hoc INT 0x80 / POSIX-numbered `SYS_*` dispatch table with a complete NT native API layer: `NTSTATUS` return values, `NtXxx`/`ZwXxx` naming, a `SYSCALL`/`SYSRET` fast path, a numbered System Service Descriptor Table (SSDT) with 475 service entries, and the `NtCurrentTeb()` / `NtCurrentPeb()` inline contract. This is the exact interface that `ntdll.dll`, CSRSS, Win32k, and every driver framework use to talk to the kernel. This TODO is the **master registry** for all NT syscall endpoints -- some are implemented here, others are implemented by domain-specific TODOs but get their SSDT slots reserved and documented here.

> [!IMPORTANT]
> **Current state:** `syscall.c` dispatches via `INT 0x80` with Linux-style `SYS_WRITE=1`, `SYS_READ=2`, … `SYS_MUNMAP=38`. Return value is a plain `int64_t`. No `NTSTATUS`, no `NtXxx`/`ZwXxx` entry points, no `SYSCALL`/`SYSRET` MSR setup, no SSDT. The existing 22 syscalls are the migration starting point; none are deleted here.

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- `task_exec`, ring-3 entry frame
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) -- MSR_GS_BASE, per-CPU data
- [`include/kernel/msr.h`](../../include/kernel/msr.h) -- MSR_IA32_STAR, MSR_IA32_LSTAR, MSR_IA32_FMASK already defined
- [`include/kernel/gdt.h`](../../include/kernel/gdt.h) -- GDT_KERNEL_CODE, GDT_USER_CODE selectors
- → XREF: `TODO-05-object-manager.md` -- Ob-routed NtXxx functions (NtClose, NtDuplicateObject, NtQueryObject, NtOpenDirectoryObject, NtQueryDirectoryObject already implemented); SSDT entries for file/process/sync depend on TODO-05 §2–§10
- → XREF: `TODO-11-peb-teb-user-abi.md §5–§8` -- `swapgs` in syscall entry/exit uses the TEB GS contract; KERNEL_GS_BASE per-task
- → XREF: `TODO-01-kernel-init-sequencing.md §5` -- syscall/SSDT fast path init runs in Phase 3 (`boot_desktop.c`, after scheduler, alongside `ssdt_init`); Phase-3 ownership accepted in TODO-01 §5 (consistent with §4 `ssdt_init` Phase-3)
- → XREF: `TODO-08-time-filetime-management.md §8–§9` -- NtSetTimerResolution/NtQueryTimerResolution (§8), NtQuerySystemTime/NtSetSystemTime/NtQueryPerformanceCounter (§9); service numbers reserved in §2
- → XREF: `TODO-23-exception-dispatch-seh.md §5` -- NtRaiseException and NtContinue; SSDT indices reserved in §4
- → XREF: `TODO-15-security-reference-monitor.md §8,§12` -- §8 SeAccessCheck bypasses for kernel-mode (ZwXxx) callers; §12 wires NtAccessCheck, NtOpenProcessToken, etc. to SSDT
- → XREF: `TODO-24-alpc-message-ports.md §8-§9` -- ALPC port syscalls (NtCreatePort, NtAlpcSendWaitReceivePort, etc.); SSDT indices reserved in §4
- → XREF: `TODO-14-registry-completion.md §5` -- Registry syscalls (NtCreateKey, NtOpenKey, NtSetValueKey, etc.); SSDT indices reserved in §5
- → XREF: `TODO-26-power-management.md §12` -- NtSetSystemPowerState, NtInitiatePowerAction; SSDT indices reserved in §4
- → XREF: `TODO-02-kernel-configuration-policy.md §8` -- `SystemKernelConfigInformation`, `NtQuerySystemConfiguration`, and `NtSetSystemConfiguration` extend the system-information/config ABI
- → XREF: `TODO-29-kernel-debugger-kd-protocol.md §13` -- NtDebugActiveProcess, NtWaitForDebugEvent; SSDT indices reserved in §4
- → XREF: `TODO-23-exception-dispatch-seh.md §13` -- ProbeForRead/ProbeForWrite safe probing used by §12 and all NtXxx handlers
- → XREF: `TODO-10-kernel-security-hardening.md` -- SSDT integrity protection (§27) complements KASLR and SMEP/SMAP
- → XREF: `08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §7` -- Win32k user-mode callback dispatch via §26 KeUserModeCallback; shadow SSDT stub registered in §5
- → XREF: `08-graphics-ui/TODO-A-Win32k-Shadow-SSDT-Master-Table.md` -- canonical 1300-row Table 1 slot map (`0x1000+`); keep `WIN32K_SSDT_*` constants aligned here when Win32k grows
- → XREF: `08-graphics-ui/TODO-16-win32k-shadow-native-api.md` -- Table 1 routing, bounds, and NTSTATUS contract (companion to this file for shadow SSDT only)
- → XREF: `TODO-07-irql-model-dpcs.md §11,§12` -- KAPC object type and APC delivery mechanism; NtQueueApcThread (SSDT 0x0043) and NtQueueApcThreadEx (SSDT 0x0380) consume KeInitializeApc/KeInsertQueueApc
- → XREF: `TODO-21-process-model-extensions.md §4,§5,§8,§10,§11,§13` -- §4 priority class, §5 scheduling policy, §8 accounting fields, §10 CPU affinity, §11 mitigation policy all flow through `NtSetInformationProcess`/`NtQueryInformationProcess` (§10 of this TODO); §13 wires Job Object SSDT entries 0x0160–0x0167; §12 pledge check integrates into the SSDT dispatcher alongside §25 syscall filter
- → XREF: `TODO-31-kernel-bulletproofing.md` §7 -- SSDT_MAIN_COUNT / last-index static asserts keep this TODO's syscall table in sync with `service_numbers.h`
- → XREF: `TODO-20-eif-full-implementation.md` §4-§7 -- EIF API version and import dispatch (including optional stubs) depend on SSDT layout and bounds enforced in this TODO
- → XREF: `00-infrastructure/TODO-04-usermode-test-framework.md §10` -- user-mode `test_win32.exe` tracks Win32 stubs delivered from this TODO §5

## Outcome

- All syscalls return `NTSTATUS`; `STATUS_SUCCESS = 0`, `STATUS_FAILURE` codes for errors.
- `NtXxx` entry points are the user-mode callable names; `ZwXxx` are the kernel-mode aliases.
- `SYSCALL`/`SYSRET` fast path replaces `INT 0x80`; `INT 0x2E` kept as compatibility fallback.
- A numbered SSDT table with 475 entries maps service indices to kernel functions; `ntdll` stubs call by index.
- The existing 22 `SYS_*` calls are migrated to `Nt`-named equivalents at stable indices.
- `NtCurrentTeb()` (`mov rax, gs:[0x30]`) and `NtCurrentPeb()` (`mov rax, gs:[0x60]`) return correct values per TODO-11.
- All endpoint categories covered: file I/O, process/thread, memory, sync, registry, security/token, sections, timers, ALPC ports, debug, power, namespace, system info, atoms.
- Per-process syscall filtering allows processes to restrict which syscalls their children can invoke -- parity with Win11 SystemCallDisablePolicy and Linux seccomp-bpf.
- `KeUserModeCallback()` enables the kernel to call user-mode functions (window procedures, hooks) and await their return via `NtCallbackReturn`.
- SSDT pages are hardware write-protected after init -- simpler and more secure than PatchGuard periodic checksums.

## Implementation Order

| ⭐  | Order | Deliverable                                                    | Depends On         | Status |
| --- | :---: | -------------------------------------------------------------- | ------------------ | :----: |
| 💎  |   1   | NTSTATUS type and canonical status codes                       | --                 |  [x]   |
| 💎  |   2   | SYSCALL/SYSRET fast path (IA32_LSTAR)                          | TODO-11 §5–§8      |  [x]   |
| 💎  |   3   | INT 0x2E compatibility path                                    | §2                 |  [/]   |
| 💎  |   4   | System Service Descriptor Table (SSDT) -- 475 entries          | §1                 |  [/]   |
| 💎  |   5   | Nt/Zw naming and existing syscall migration                    | §1, §4             |  [x]   |
| 💎  |   6   | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile | §5, TODO-05 §2     |  [/]   |
| 💎  |   7   | NtCreateProcess / NtCreateThread / process-thread lifecycle    | §5, TODO-05 §2     |  [/]   |
| 💎  |   8   | Sync objects + NtWaitForMultipleObjects                        | §5, TODO-05 §6     |  [x]   |
| 💎  |   9   | Virtual memory (alloc, free, protect, lock)                    | §5                 |  [/]   |
| 💎  |  10   | NtQuerySystemInformation / NtQueryInformationProcess           | §5                 |  [/]   |
| ⭐  |  11   | Extended error information (IOSB + TEB LastError)              | §5, TODO-11 §1     |  [x]   |
| ⭐  |  12   | ZwXxx kernel-mode alias layer with privilege assertion         | §4, §5             |  [x]   |
| 💎  |  13   | File metadata and device control                               | §6                 |  [x]   |
| 💎  |  14   | Registry syscalls (core CRUD)                                  | §5, TODO-14 §5     |  [/]   |
| 💎  |  15   | Registry syscalls (advanced: flush/notify/save/hive)           | §14, TODO-14 §5    |  [/]   |
| 💎  |  16   | Token open/query/adjust syscalls                               | §5, TODO-15 §7     |  [/]   |
| 💎  |  17   | Directory and symbolic link object syscalls                    | §5, TODO-05 §3     |  [/]   |
| 💎  |  18   | Section and memory-mapped file syscalls                        | §5, TODO-05 §7     |  [/]   |
| 💎  |  19   | Timer control syscalls                                         | §5, TODO-08 §8,§9  |  [/]   |
| 💎  |  20   | Legacy LPC port syscalls (stubs; engine TODO-09 §7)            | §5, TODO-09 §7     |  [/]   |
| 💎  |  21   | Exception and debug syscalls                                   | §5, TODO-23 §5     |  [/]   |
| 💎  |  22   | Power and system control                                       | §5, TODO-26 §20    |  [/]   |
| 💎  |  23   | Atom, locale, and miscellaneous                                | §5                 |  [x]   |
| ⭐  |  24   | Syscall audit and tracing hook                                 | §4                 |  [/]   |
| 💎  |  25   | Per-process syscall filtering (seccomp / SystemCallDisable)    | §4, §7             |  [x]   |
| 💎  |  26   | Kernel-to-user mode callback dispatch (KeUserModeCallback)     | §2, TODO-11 §9     |  [/]   |
| ⭐  |  27   | SSDT integrity protection (hardware write-protect)             | §4                 |  [/]   |
| 💎  |  28   | Extended directory enumeration classes                         | §6, §13            |  [x]   |
| 💎  |  29   | Token lifecycle + SRM access check syscalls                    | §16, TODO-15 §7,§8 |  [/]   |
| 💎  |  30   | Generic object management (make-temp/perm, set-info, compare)  | §17, TODO-05 §1,§9 |  [/]   |
| 💎  |  31   | Modern ALPC port syscalls                                      | §20, TODO-24 §8-§9 |  [x]   |
| 💎  |  32   | Post-ship follow-up backfill (2026-07-31 cohort)               | --                 |  [ ]   |

> 💎 = parity -- Windows NT and Linux both have equivalents for these categories.
> ⭐ = exclusive -- the ZwXxx privilege layer, the audit hook, SSDT integrity protection, and the IOSB/LastError unified path go beyond what Linux offers.

> [!IMPORTANT]
> **Self-contained execution model:** §1 through §5 (NTSTATUS, SYSCALL/SYSRET, INT 0x2E, SSDT, migration) are fully self-contained, no external blockers. §9 through §12, §24 through §28 are also unblocked. Sections §6 through §8, §13 through §23 wire domain-specific syscalls through the SSDT and depend on their respective domain TODOs (Object Manager, Registry, SRM, ALPC, etc.). This is by design: this TODO is the **master registry** for all NT syscall endpoints. Domain TODOs implement the logic; this TODO provides the SSDT wiring. The unblocked core (§1 through §5 + §9 through §12 + §24 through §28) delivers a fully functional SYSCALL/SYSRET fast path with 475 SSDT slots, NTSTATUS return values, and the audit/filter/integrity infrastructure. Domain-specific NtXxx wrappers activate as their domain TODOs complete.

---

## 1. NTSTATUS Type and Canonical Status Codes
Define the NT status type and the full set of codes needed across all 200+ syscall endpoints.

- [x] Create `include/kernel/nt/ntstatus.h`:
  - `typedef int32_t NTSTATUS` (Windows `LONG`; sign bit distinguishes success from error)
  - Severity macros: `NT_SUCCESS(s)` = `((NTSTATUS)(s)) >= 0` (canonical Win form), `NT_INFORMATION(s)` = `((uint32_t)(s) >> 30) == 1`, `NT_WARNING(s)` = `((uint32_t)(s) >> 30) == 2`, `NT_ERROR(s)` = `((uint32_t)(s) >> 30) == 3`
  - **Success / informational:**
    - `STATUS_SUCCESS                    0x00000000`
    - `STATUS_PENDING                    0x00000103`
    - `STATUS_BUFFER_OVERFLOW            0x80000005` -- data truncated; partial result returned
    - `STATUS_NO_MORE_FILES              0x80000006` -- directory enumeration exhausted
    - `STATUS_NO_MORE_ENTRIES            0x8000001A` -- registry/object enumeration exhausted
    - `STATUS_ALERTED                    0x00000101` -- thread was alerted during wait
    - `STATUS_TIMEOUT                    0x00000102` -- wait timed out (not an error)
  - **Error codes -- object / handle:**
    - `STATUS_UNSUCCESSFUL               0xC0000001`
    - `STATUS_NOT_IMPLEMENTED            0xC0000002`
    - `STATUS_INVALID_INFO_CLASS         0xC0000003`
    - `STATUS_ACCESS_VIOLATION           0xC0000005` -- user-buffer probe failed
    - `STATUS_INVALID_HANDLE             0xC0000008`
    - `STATUS_INVALID_PARAMETER          0xC000000D`
    - `STATUS_NO_MEMORY                  0xC0000017`
    - `STATUS_ACCESS_DENIED              0xC0000022`
    - `STATUS_BUFFER_TOO_SMALL           0xC0000023`
    - `STATUS_OBJECT_TYPE_MISMATCH       0xC0000024`
    - `STATUS_OBJECT_NAME_NOT_FOUND      0xC0000034`
    - `STATUS_OBJECT_NAME_COLLISION      0xC0000035`
    - `STATUS_PORT_DISCONNECTED          0xC0000037`
    - `STATUS_OBJECT_PATH_NOT_FOUND      0xC000003A`
    - `STATUS_PORT_CONNECTION_REFUSED    0xC0000041`
  - **Error codes -- sync:**
    - `STATUS_SEMAPHORE_LIMIT_EXCEEDED   0xC0000047`
    - `STATUS_MUTANT_NOT_OWNED           0xC0000046`
  - **Error codes -- file I/O:**
    - `STATUS_END_OF_FILE                0xC0000011`
    - `STATUS_FILE_LOCK_CONFLICT         0xC0000054`
    - `STATUS_RANGE_NOT_LOCKED           0xC000007E`
    - `STATUS_DELETE_PENDING             0xC0000056`
    - `STATUS_CANNOT_DELETE              0xC0000121`
    - `STATUS_DIRECTORY_NOT_EMPTY        0xC0000101`
    - `STATUS_NOT_A_DIRECTORY            0xC0000103`
    - `STATUS_FILE_IS_A_DIRECTORY        0xC00000BA`
  - **Error codes -- process / thread:**
    - `STATUS_PROCESS_IS_TERMINATING     0xC000010A`
    - `STATUS_THREAD_IS_TERMINATING      0xC000004B`
    - `STATUS_THREAD_NOT_IN_PROCESS      0xC000012A`
    - `STATUS_SUSPEND_COUNT_EXCEEDED     0xC000004A`
  - **Error codes -- memory:**
    - `STATUS_CONFLICTING_ADDRESSES      0xC0000018`
    - `STATUS_SECTION_NOT_EXTENDED       0xC0000087`
    - `STATUS_INVALID_PAGE_PROTECTION    0xC0000045`
    - `STATUS_ALREADY_COMMITTED          0xC0000021`
    - `STATUS_MEMORY_NOT_ALLOCATED       0xC00000A0`
  - **Error codes -- registry:**
    - `STATUS_KEY_DELETED                0xC000017C`
    - `STATUS_KEY_HAS_CHILDREN           0xC0000180`
    - `STATUS_CHILD_MUST_BE_VOLATILE     0xC0000181`
  - **Error codes -- security:**
    - `STATUS_PRIVILEGE_NOT_HELD         0xC0000061`
    - `STATUS_BAD_IMPERSONATION_LEVEL    0xC00000A5`
  - **Error codes -- debug:**
    - `STATUS_DEBUGGER_INACTIVE          0xC0000354`
    - `STATUS_PORT_NOT_SET               0xC0000353`
  - **Error codes -- power / misc:**
    - `STATUS_NOT_SUPPORTED              0xC00000BB`
    - `STATUS_INVALID_DEVICE_REQUEST     0xC0000010`
    - `STATUS_DEVICE_NOT_READY           0xC00000A3`
- [x] Create `include/kernel/nt/nt_types.h` with foundational NT types:
  - `HANDLE` -- already `typedef int32_t HANDLE` in `include/kernel/ob/handle_table.h`; re-export, do not redefine
  - `IO_STATUS_BLOCK` -- `NTSTATUS Status`, `uint64_t Information`
  - `UNICODE_STRING` -- re-export from `include/kernel/ob/peb.h` (already defined per TODO-11)
  - `OBJECT_ATTRIBUTES` -- `uint64_t Length`, `HANDLE RootDirectory`, `UNICODE_STRING *ObjectName`, `uint32_t Attributes` (`OBJ_CASE_INSENSITIVE = 0x40`, `OBJ_KERNEL_HANDLE = 0x200`, `OBJ_INHERIT = 0x02`, `OBJ_OPENIF = 0x80`)
  - `LARGE_INTEGER` -- re-export from `include/kernel/ob/peb.h` (already defined per TODO-11)
  - `ACCESS_MASK` = `uint32_t`; `GENERIC_READ = 0x80000000`, `GENERIC_WRITE = 0x40000000`, `GENERIC_EXECUTE = 0x20000000`, `GENERIC_ALL = 0x10000000`
  - `CLIENT_ID` -- `uint64_t UniqueProcess`, `uint64_t UniqueThread`
  - `CONTEXT` -- forward-declare; full definition in TODO-23
- [x] Annotate each `NTSTATUS` code with inline comment describing trigger condition
- [x] uefi_vars.h updated to forward-include from canonical `kernel/nt/ntstatus.h`
- [x] 6 unit tests: NT_SUCCESS, NT_ERROR, NT_WARNING, NT_INFORMATION, type sizes, OBJECT_ATTRIBUTES size
- [x] Commit: `"kernel: nt -- NTSTATUS type and canonical status codes"`

**Test checkpoint:** `bash scripts/build.sh` → `=== BUILD OK ===`. `NT_SUCCESS(0)` == true, `NT_ERROR(0xC0000001)` == true, `NT_WARNING(0x80000005)` == true, `NT_INFORMATION(0x00000101)` == true verified by unit test.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures

> **Notes:**
> - `NTSTATUS = int32_t` (Windows `LONG`); 66 canonical `STATUS_*` codes in `include/kernel/nt/ntstatus.h` with byte-for-byte Win32-ABI values.
> - Severity classified by `NT_SUCCESS` (sign-bit `>= 0`) + `NT_INFORMATION/WARNING/ERROR` (bits 31:30 via uint32 cast); the macros are header-inlined and hit on every syscall return.
> - Review fixed 2 ABI-value drifts (`STATUS_REPLY_MESSAGE_MISMATCH` 0xC000025E->0xC000021F, `STATUS_SEMAPHORE_LIMIT_EXCEEDED` 0xC0000044->0xC0000047) and moved `STATUS_DATATYPE_MISALIGNMENT` (0x80000002) into the warning band.
> - Consumed by every `NtXxx` handler + `RtlNtStatusToDosError` (§11); `test_nt_types.c` (`TEST_CAT_ABI`) asserts the four severity macros.
> - re-adversarial skipped: fixes were constant-value/placement corrections in a pure header (no locking/ISR/lifecycle, ~5 lines).

> **Verified:** 2026-06-28 | 1/1 items | build OK | tests 346+16 PASS
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 2H+1M fixed | scope: kernel-code-quality (constants/macro header)

---

## 2. SYSCALL/SYSRET Fast Path

> [!WARNING]
> **High-risk section.** Writing wrong values to IA32_STAR, IA32_LSTAR, IA32_FMASK, or EFER will triple-fault on the next `syscall` instruction with no diagnostic output. A bug in the assembly entry point (`swapgs` sequence, stack switch, callee-save frame) corrupts every subsequent syscall. **Rollback:** If this breaks, revert the MSR writes and keep the existing `INT 0x80` handler -- it remains registered until §3 completes. Test each MSR write individually before enabling EFER_SCE.

Replace `INT 0x80` with the x86-64 `SYSCALL`/`SYSRET` instruction pair. On `SYSCALL`: CPU saves RIP→RCX, RFLAGS→R11; jumps to `IA32_LSTAR`. On `SYSRET`: restores RIP from RCX, RFLAGS from R11; returns to ring 3.

- [x] MSR constants verified in `include/kernel/msr.h`: STAR, LSTAR, FMASK, EFER_SCE all present
- [x] `syscall_entry` written in `src/kernel/sched/syscall_entry.asm` -- swapgs, per-CPU scratch for user RSP, callee-save frame, calls `syscall_dispatch_fast()`
- [x] `syscall_init_fast()` in `src/kernel/sched/syscall_fast.c` -- detects GDT order incompatibility and safely defers activation
- [x] GDT reordered: UDATA=0x18, UCODE=0x20 (SYSRET-compatible). All code uses macros, no hardcoded selectors. MSRs written, EFER_SCE enabled.
- [x] `syscall_dispatch_fast()` -- C dispatcher routing to SSDT via `ssdt_dispatch()`
- [x] `per_cpu_data.syscall_rsp0` + `user_rsp_scratch` fields added; updated in scheduler context switch
- [x] INT 0x80 remains active as fallback (syscall_init() still called)
- [x] Commit: `"kernel: nt -- SYSCALL/SYSRET fast path init"` + GDT swap commit

**Test checkpoint:** Serial log: `"syscall: fast path (SYSCALL/SYSRET) enabled"` during Phase 3 (init runs in `boot_desktop.c`; see TODO-01 §5). `POST16(0xDA00)` entry, `POST16(0xDA01)` exit. Ring-3 `syscall` instruction reaches `syscall_dispatch` without GPF. Verify on QEMU WHPX, TCG, VirtualBox, bare metal. (Note: 0xD200/0xD201 are taken by `gdt.c`.)

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures; smoke boots to C:\>

> **Notes:**
> - SYSCALL/SYSRET via IA32_STAR/LSTAR/FMASK + EFER.SCE (`syscall_fast.c`); asm entry (`syscall_entry.asm`) does swapgs + per-CPU stack switch + 9-push callee-save + `cld`, calls `syscall_dispatch_fast` -> `ssdt_dispatch`.
> - GDT reordered UDATA=0x18/UCODE=0x20 (SYSRET-compatible, `_Static_assert`-pinned); STAR[63:48]=0x10 is correct (SYSRET forces RPL=3 -> CS=0x23/SS=0x1B).
> - MSR-readback canary disables EFER.SCE and falls back to INT 0x80 on any mismatch; INT 0x80 + INT 0x2E stay registered.
> - Review hardened FMASK 0x200 -> `SYSCALL_FMASK` 0x47700 (TF|IF|DF|IOPL|NT|AC) + defensive `cld`: blocks user TF single-stepping kernel entry + DF breaking the C ABI. FMASK masks only the in-kernel RFLAGS; SYSRET restores user RFLAGS from r11.
> - Per-CPU `syscall_rsp0`/`user_rsp_scratch` (gs:24/gs:32, `_Static_assert`-pinned), updated at both scheduler switch sites.

> **Verified:** 2026-06-28 | 7/7 items | build OK | smoke PASS (TCG 2.51s), tests 346+16 PASS
> **Deferred:** [H] syscall-path `transition_ring_record` forensic overhead (2x TSC+CR3+2 RDMSR per syscall) should be gated behind a runtime knob -> XREF: 02-kernel-core/TODO-12 §24 (item: "Gate syscall-path `transition_ring_record()` behind a runtime static-key/debug knob" at line 232)
> **Deferred:** [M] `ssdt_dispatch` bound uses hardcoded `SSDT_MAIN_MAX` for both tables; OOB if `SSDT_SHADOW_MAX` ever shrinks -> XREF: 02-kernel-core/TODO-12 §4 (item: "Bound `ssdt_dispatch` per-table, not by hardcoded `SSDT_MAIN_MAX`" at line 233)
> **Quality reviewed:** 2026-06-28 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1Crit fixed (FMASK), 2 deferred (1H+1M), 1Crit rejected (STAR false-positive) | scope: kernel-code-quality

---

## 3. INT 0x2E Compatibility Path
Windows NT's original software-interrupt syscall vector. Required for early ntdll and any code that does not use `SYSCALL`.

- [x] IDT vector 0x2E set to DPL=3 (type_attr 0xEE) in `idt.c` -- user-mode `int 0x2E` works
- [x] `syscall_handler_2e()` registered in `syscall.c` -- reads RAX as service number, R10/RDX/R8/R9 as args (Windows x64 ABI), dispatches via `ssdt_dispatch()`
- [x] Uses existing `irq14` stub which has full register save/restore + swapgs + iretq
- [x] INT 0x80 kept active alongside INT 0x2E (both paths coexist)
- [x] Commit: `"kernel: nt -- INT 0x2E syscall compatibility path"`
- [ ] **Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+**: Windows x64 ABI passes the first 4 args in `R10`/`RDX`/`R8`/`R9` and args 5+ on the user stack at `[RSP+0x28]`, `[RSP+0x30]`, ... Today `syscall_handler_2e()` in `src/kernel/sched/syscall.c` and `syscall_entry` in `src/kernel/sched/syscall_entry.asm` only populate the first 4 register slots plus whatever the dispatcher already has in `a5`/`a6`, so any `NtXxx` with more than 4 parameters has to kludge the rest through an extended-args struct at `a5` (e.g. `NT_MAPVIEW_ARGS` in `include/kernel/nt/nt_section.h`). Retrofit both entry paths to probe `[RSP+0x28..]` for the user stack frame with `ProbeForReadIfUser` and populate `a5`/`a6` directly (and any future `a7..a10` via an on-stack `SSDT_ARGS` struct passed to `ssdt_dispatch()`). Then delete `NT_MAPVIEW_ARGS` and the `NtCreateSection` packed-flags kludge at `nt_section.c:93-94`. This auto-closes TODO-12 §18 Accepted #1 (10-parameter `NtMapViewOfSection` stack ABI).

- [ ] [M] syscall IRQL wrap pays a redundant LAPIC TPR write per INT 0x2E/0x80: `syscall_lower_entry_irql`->`KeLowerIrql` reprograms TPR the IDT raise never set. Use a software-IRQL helper setting `pcpu->current_irql` without TPR. (§3)

**Test checkpoint:** Ring-3 `int 0x2E` with RAX=0x0015 reaches `NtClose` handler. Same register mapping as SYSCALL path. `POST16(0xD300)` entry, `POST16(0xD301)` exit. Verify on all 4 platforms.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures; smoke boots to C:\>

> **Notes:**
> - IDT `idt[0x2E].type_attr=0xEE` (DPL=3 gate, `idt.c:572`); `syscall_handler_2e` (`syscall.c:660`) reads RAX=service + R10/RDX/R8/R9 args (Windows x64 ABI) and dispatches via `ssdt_dispatch`, mirroring the SYSCALL fast path.
> - Reuses the standard ISR stub (register save/restore + swapgs + iretq); IRQL-wrapped to PASSIVE so NT paths don't run at DISPATCH; INT 0x80 + INT 0x2E coexist.
> - NTSTATUS zero-extension fix at `syscall.c:704` matches the SYSCALL path's `mov eax` behavior (fuzz-found int2e/SYSCALL divergence).
> - Open: >4-register-arg syscalls still kludge via extended-args struct (stack-arg retrofit deferred); legacy/compat path, cold relative to §2.
> - re-adversarial skipped: §3 review made no §3-scope code change (the [Critical] handler-probe gap is owned by §6; the perf TPR-write fix is deferred).

> **Verified:** 2026-06-28 | 4/6 items | build OK | tests 346+16 PASS (tree unchanged; §3 findings out-of-scope/deferred)
> **Accepted:** [Critical] file-I/O handlers deref raw user iosb/buf pointers with no ProbeFor*IfUser = ring-3 arbitrary kernel R/W; §3 entry path exposes it but §6 owns the handlers (design-reviewed into 4 items) -> XREF: 02-kernel-core/TODO-12 §6 (item: "Add `nt_write_iosb(iosb,status,info)` helper" at line 360)
> **Deferred:** [M] syscall IRQL entry wrap pays a redundant LAPIC TPR write per INT 0x2E/0x80 -> XREF: 02-kernel-core/TODO-12 §3 (item: "syscall IRQL wrap pays a redundant LAPIC TPR write" at line 247)
> **Deferred:** [L] INT 0x2E + SYSCALL entry read only 4 register args; >4-arg syscalls kludge via extended-args struct -> XREF: 02-kernel-core/TODO-12 §3 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+" at line 245)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 0 fixed, 1Crit accepted-XREF + 1M+1L deferred | scope: kernel-code-quality

---

## 4. System Service Descriptor Table (SSDT)

The SSDT is a flat array of function pointers indexed by the 12-bit service number in RAX. `ntdll` stubs do `mov rax, <service_number>; syscall`. This section defines the complete service number allocation for all 475 NT API endpoints. Numbers are stable -- changing them is an ABI break.

- [x] Define `SSDT_HANDLER` and `SSDT_TABLE` in `include/kernel/nt/ssdt.h` -- handler takes 6 uint64_t args, returns NTSTATUS; table has handlers array + count + implemented count + name
- [x] Shadow SSDT (table 1) allocated as empty placeholder -- indices 0x1000+, filled by Win32k later
- [x] 475 service index assignments in `include/kernel/nt/service_numbers.h` -- `SSDT_NtXxx` defines for every entry, `SSDT_MAIN_COUNT = 475`
- [x] `ssdt_dispatch()` -- selects table from bits 13:12, index from bits 11:0, calls handler
- [x] `ssdt_register()` -- replaces stub with real handler, tracks implemented count
- [x] Unimplemented slots return `STATUS_NOT_IMPLEMENTED` via `ssdt_stub_not_implemented()`
- [x] `ssdt_init()` called in Phase 3 before `syscall_init()`
- [x] 4 unit tests: unimplemented stub, invalid table, `table->count >= SSDT_MAIN_COUNT` (lower bound, grows as handlers register), register+dispatch
- [ ] GUI-thread conversion: when ssdt_dispatch selects Table 1 (shadow SSDT, RAX bits[13:12]=01) and the thread lacks GUI state, run a KiConvertGuiThread-equivalent init (expand kernel stack + wire GUI state) before the handler, after the audit/filter checks -> XREF: `D08 T15`, `D08 T16`
- [x] Bound `ssdt_dispatch`/`ssdt_register` per-table via new `SSDT_TABLE.max` (capacity) field instead of hardcoded `SSDT_MAIN_MAX`; drops the dead `count` term. Closes the §2-review OOB-if-shadow-shrinks finding. (§4 review)
- [x] Commit: `"kernel: nt -- SSDT and service number table"`

**Test checkpoint:** `ssdt_init` logs "SSDT initialized: N main slots"; ring-3 syscall with a valid service number reaches its handler; an out-of-range service number returns `STATUS_NOT_IMPLEMENTED`. 4 unit tests (stub, invalid table, main count, register+dispatch) pass.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures; smoke boots to C:\>

> **Notes:**
> - SSDT is a flat function-pointer array; `ssdt_dispatch` (`ssdt.c`) splits table-id (bits 13:12) + index (bits 11:0), bounds-checks, indirect-calls; `ssdt_init` stub-fills all slots before Phase-3 syscall enable.
> - Review hardened the dispatch + register bound to a per-table `SSDT_TABLE.max` capacity field instead of the hardcoded `SSDT_MAIN_MAX`, closing the §2-review OOB-if-shadow-shrinks finding (both tables 1024 today).
> - Consistency fixes: bound the `SSDT_MAIN_COUNT`/`SSDT_LAST_MAIN_INDEX` static-asserts to the real `SSDT_MAIN_MAX` in `ssdt.c`; documented count(extent)/max(capacity)/implemented(live) as distinct fields.
> - 475 main service numbers allocated in `service_numbers.h`; shadow (Win32k) table stays empty until filled by D08 T15.
> - re-adversarial skipped: the only behavioral change (the `table->max` bound) was adversarial+perf-approved in round 1; the consistency fixes are compile-time asserts + a doc comment.

> **Verified:** 2026-06-28 | 9/10 items | build OK | smoke PASS (TCG 2.50s), tests 346+16 PASS
> **Deferred:** [M] GUI-thread conversion (KiConvertGuiThread-equivalent) on the first Table-1 shadow-SSDT call -- blocked on Win32k shadow-SSDT fill + per-thread GUI state -> XREF: 02-kernel-core/TODO-12 §4 (item: "GUI-thread conversion: when ssdt_dispatch selects Table 1" at line 280; depends on D08 T15/T16)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 2M fixed | scope: kernel-code-quality

---

## 5. Nt/Zw Naming and Existing Syscall Migration
Rename / wrap the existing 22 `SYS_*` implementations to their `NtXxx` equivalents, change return types to `NTSTATUS`, and register them in the SSDT at the indices defined in §4.

- [x] Add `NtWriteFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_WRITE` logic → SSDT 0x0013
- [x] Add `NtReadFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_READ` logic → SSDT 0x0012
- [x] Add `NtTerminateProcess(HANDLE, NTSTATUS)` replacing `SYS_EXIT` → SSDT 0x0033
- [x] Add `NtYieldExecution()` replacing `SYS_YIELD` → SSDT 0x0044; returns `STATUS_SUCCESS`
- [x] Add `NtCreateProcess`/`NtCreateThread` wrapping task/thread create paths → SSDT 0x0030/0x0036 (§7)
- [x] Add `NtWaitForSingleObject(HANDLE, timeout)` replacing `SYS_WAITPID` → SSDT 0x0006
- [x] Add `NtQueryDirectoryFile` wrapping `SYS_READDIR` → SSDT 0x0017
- [x] Add `NtQuerySystemInformation(SystemProcessInformation)` wrapping `SYS_GETPROCS` → SSDT 0x00D0
- [x] Add `NtTerminateProcess` for kill → SSDT 0x0033
- [x] Add `NtQuerySystemInformation(SystemTimeOfDayInformation)` for uptime → SSDT 0x00D0
- [x] Add `NtShutdownSystem(ShutdownReboot / ShutdownPowerOff)` → SSDT 0x00D7
- [x] Add `NtCreateNamedPipeFile` / `NtReadFile` / `NtWriteFile` wrapping pipe → SSDT 0x001B
- [x] Add `NtCreateSection` / `NtMapViewOfSection` wrapping shmem → SSDT 0x005C/0x005E (→ XREF TODO-05 §7)
- [x] `SYS_NT_*` macros (in `syscall.h`/`abi_numbers.h`) are the compile-time aliases equal to the `SSDT_NtXxx` indices; legacy `SYS_*` keep their INT 0x80 numbers (separate number space -- not SSDT aliases) for backward compat
- [x] Change all `sys_*` implementations to return `NTSTATUS`; convert error paths to `STATUS_*` codes
- [x] Add `Nt_Close` SSDT wrapper at 0x0000 bridging to OB NtClose
- [x] 12 NtXxx handlers registered in SSDT via `nt_syscall_register_ssdt()` in `src/kernel/nt/nt_syscall.c`
- [x] 2 unit tests: SSDT registration verification (9 handler callability checks) + SYS_NT_* alias value validation (12 aliases)
- [x] Commit: `"kernel: nt -- migrate existing syscalls to NtXxx naming and NTSTATUS"`

**Test checkpoint:** All 22 existing syscalls still work via old `SYS_*` macros (backward compat). `NtWriteFile` returns `STATUS_SUCCESS` on valid write. Serial: existing boot/desktop tests pass without regression. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- this is the most dangerous migration; a return-type mismatch silently corrupts all user-mode callers.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures

> **Notes:**
> - Migrated the legacy SYS_* syscalls to NtXxx handlers returning NTSTATUS, registered in the SSDT via `nt_syscall_register_ssdt()` (`nt_syscall.c`); the INT 0x2E + SYSCALL paths dispatch them by service number.
> - `SYS_NT_*` macros equal the `SSDT_NtXxx` indices (compile-time aliases); legacy `SYS_*` keep their INT 0x80 number space for backward compat -- corrected the §5 contract wording (they are NOT SSDT aliases).
> - Review found the migrated handlers do not yet enforce the ring-3 trust boundary (privilege/handle-rights/user-pointer probes) -- a systemic dev-stage gap (no tokens, SMAP off, shared frames); filed 3 [Critical] §29 items + the §6 file-I/O probe item.
> - re-adversarial skipped: §5 review made no §5-scope code change (migration is correct; trust-boundary criticals Accepted to §29; the alias [M] is a TODO-contract fix).

> **Verified:** 2026-06-28 | 18/18 items | build OK | tests 346+16 PASS (tree unchanged; findings Accepted to §29)
> **Accepted:** [Critical] NtShutdownSystem (+ legacy SYS_SHUTDOWN) lets any ring-3 caller power off the machine with no SeShutdownPrivilege gate -> XREF: 02-kernel-core/TODO-12 §29 (item: "NtShutdownSystem + legacy SYS_SHUTDOWN: require SeShutdownPrivilege" at line 961)
> **Accepted:** [Critical] NtTerminateProcess kills any process by raw PID with no handle/PROCESS_TERMINATE check -> XREF: 02-kernel-core/TODO-12 §29 (item: "NtTerminateProcess: resolve through the handle table + require PROCESS_TERMINATE" at line 962)
> **Accepted:** [Critical] SystemProcessInformation writes through unprobed user pointers (kernel-write primitive) -> XREF: 02-kernel-core/TODO-12 §29 (item: "SystemProcessInformation: ProbeForWriteIfUser + copy_to_user" at line 963)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1M fixed, 3Crit accepted-XREF | scope: kernel-code-quality

---

## 6. NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile
Core file I/O entry points routed through the Object Manager (→ XREF TODO-05).

- [x] `NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength)`:
  - Parse `ObjectAttributes->ObjectName` via `oa_extract_path()` (strips `\??\` prefix)
  - Map `CreateDisposition` (`FILE_OPEN`, `FILE_CREATE`, `FILE_SUPERSEDE`, `FILE_OPEN_IF`, `FILE_OVERWRITE`, `FILE_OVERWRITE_IF`) to VFS flags via `disposition_to_vfs()`
  - Call `ob_create_file_handle()` → allocates FILE_OBJECT + HANDLE via ObpAllocateHandle
  - Populate `IoStatusBlock->Status` and `IoStatusBlock->Information` (`FILE_CREATED` / `FILE_OPENED` / `FILE_OVERWRITTEN` / `FILE_SUPERSEDED`)
  - SSDT 0x0010: a1=HANDLE* out, a2=ACCESS_MASK, a3=OBJECT_ATTRIBUTES*, a4=IO_STATUS_BLOCK*, a5=CreateDisposition, a6=ShareAccess|(CreateOptions<<16)
- [x] `NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions)`:
  - Delegates to NtCreateFile_handler with `CreateDisposition = FILE_OPEN`
  - SSDT 0x0011: a1=HANDLE* out, a2=ACCESS_MASK, a3=OBJECT_ATTRIBUTES*, a4=IO_STATUS_BLOCK*, a5=ShareAccess, a6=OpenOptions
- [x] `NtClose(Handle)`: already done in §5 -- calls `ObpFreeHandle` via `NtClose()` in ob.c
- [x] `NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key)`: upgraded from §5 -- now supports ByteOffset (a5), direct FILE_OBJECT lookup via ObpLookupHandle, STATUS_END_OF_FILE detection
- [x] `NtWriteFile(...)`: upgraded from §5 -- now supports ByteOffset (a5), direct FILE_OBJECT lookup, advances file offset after write
- [x] `NtCreateNamedPipeFile(...)`: already done in §5
- [x] `include/kernel/nt/nt_file.h` created: CreateDisposition (FILE_OPEN..FILE_OVERWRITE_IF), CreateOptions (FILE_DELETE_ON_CLOSE etc.), FileAttributes, IOSB Information values
- [x] 4 unit tests: NtCreateFile/NtOpenFile SSDT registration (2 checks), file I/O constant values (16 checks)
- [ ] [Critical] Add `nt_write_iosb(iosb,status,info)` helper (ProbeForWriteIfUser+copy_to_user every write-back, STATUS_ACCESS_VIOLATION on fail); replace all raw `iosb->` stores in the file-I/O handlers. (§6 design)
- [ ] [Critical] NtReadFile/NtQueryDirectoryFile output via a kmalloc'd kernel bounce buffer, then copy_to_user only the bytes produced -- ProbeForWriteIfUser alone is a range check, not a TOCTOU-safe guarded copy-out. (§6 design)
- [ ] [Critical] NtCreateFile: snapshot OBJECT_ATTRIBUTES + embedded UNICODE_STRING + bounded ObjectName->Buffer via copy_from_user into a kernel NUL-terminated path before `oa_extract_path`; write FileHandle/IOSB via copy_to_user. (§6 design)
- [ ] [H] NtReadFile/NtWriteFile ByteOffset (a5): ProbeForReadIfUser + copy_from_user into a local uint64_t and use the local -- never deref a5 directly. (§6 design)
- [ ] Length failures on Nt*Information{Process,Thread,JobObject} should return `STATUS_INFO_LENGTH_MISMATCH`, not `STATUS_BUFFER_TOO_SMALL` (NT convention); repo-wide sweep. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §8`
- [ ] [Critical] Probe + bounce the INFORMATION-class handlers too: `job_memcpy` and every raw `*ret_length` store in the Query*Information handlers are ring-3-addressed kernel writes. -> XREF: `TODO-25-kernel-resource-accounting-quotas.md §8`
- [x] Commit: `"kernel: nt -- NtCreateFile, NtOpenFile, NtClose, NtReadFile, NtWriteFile"`

**Test checkpoint:** `NtCreateFile` on `X:\Logs\kernel.log` returns `STATUS_SUCCESS` + valid HANDLE. `NtClose(handle)` returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`. `NtReadFile` populates IOSB correctly.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures

> **Notes:**
> - Core file I/O is shipped + working: NtCreateFile/NtOpenFile (OB-backed FILE_OBJECT + handle), NtReadFile/NtWriteFile (ByteOffset, EOF), NtClose, NtCreateNamedPipeFile, routed through the Object Manager + VFS.
> - Trust boundary on these handlers NOT yet hardened: the 4 design-reviewed [Critical]/[H] items (nt_write_iosb, bounce buffers, OBJECT_ATTRIBUTES snapshot, ByteOffset capture) deferred to the NT trust-boundary campaign with §29.
> - Not live-exploitable in the current dev stage (no per-process isolation, SMAP off, identity-mapped shared frames) -- see the `project_nt_syscall_trust_boundary` analysis.

> **Deferred:** [Critical] §6 file-I/O user-pointer hardening (nt_write_iosb + kernel bounce buffers + OBJECT_ATTRIBUTES copy_from_user snapshot + ByteOffset capture) is a design-reviewed ~300-line security refactor; core file I/O ships, trust-boundary enforcement deferred -> XREF: 02-kernel-core/TODO-12 §6 (item: "Add `nt_write_iosb(iosb,status,info)` helper" at line 360, + 3 sibling items at lines 361-363)

---

## 7. NtCreateProcess / NtCreateThread / Process-Thread Lifecycle
Process and thread creation, suspension, termination, and thread context access through the Ob-managed process model (→ XREF TODO-05 §2).

- [x] `NtCreateProcess(0x0030)`: wraps task_create + ob_handle_table_inherit; returns HANDLE
- [ ] Wire `env_copy()` fail-closed into every constructor (`task_fork`, `task_create`=NtCreateProcess+boot/desktop, `task_create_user`) before `num_tasks++` publish (→ XREF `03-memory-concurrency/TODO-07-smp-phase2.md` §3)
- [ ] That env_copy pre-publish wiring also needs the atomic unpublished-slot reservation (concurrent creators must not claim the same `num_tasks` pid) -> XREF `03-memory-concurrency/TODO-06-scheduler-enhancement.md` §13
- [ ] Make job membership and `num_tasks++` publication atomic w.r.t. job TERMINATION, or revalidate the inherited job's
      terminated state under the job lock immediately before publication and unwind on failure. A concurrent terminate snapshots the member pid, fails to resolve it (`pid >= num_tasks`), skips it, and `task_fork` then publishes a live child into a terminated job. Pre-existing; `TODO-04` §21 widened the window by moving the child PML4 allocation into it. UNREACHABLE TODAY FOR ONE INCIDENTAL, UNDOCUMENTED REASON: `SYS_FORK` arrives through INT 0x80, installed as a 64-bit INTERRUPT gate (`0xEE`, `src/kernel/idt.c`), so IF is clear for the whole call and nothing in the window yields. It is NOT the parked APs that save it. Routing fork through the SYSCALL fast path (which does `sti`) or adding any sleeping call to this window makes it immediately reachable, so fix it before either. -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §21 (item: "`FAULT_SITE_FORK_CHILD_PML4` (id 4, pmm-owned)")
- [ ] The env_copy child snapshot must take cwd + environ TOGETHER under the parent `chdir_lock` (§12 made cwd + `=X:` one txn; a split snapshot or `task_create` cwd=root reset yields a child whose cwd + `=X:` never coexisted)
- [ ] When wired `env_copy()` gives an elevated child (IL > parent) the parent env, call `env_sanitize_for_elevation(child)` before publish so blocklisted vars aren't inherited -> XREF: 02-kernel-core/TODO-22 §16 ("env_sanitize_for_elevation")
- [x] `NtCreateProcessEx(0x0031)`: aliases NtCreateProcess (extended flags deferred to TODO-21)
- [ ] `NtCreateUserProcess(0x0045)`: stub returning STATUS_NOT_IMPLEMENTED (needs PE loader TODO-21)
- [x] `NtCreateThread(0x0036)`: wraps thread_create; supports CreateSuspended via suspend_count
- [x] `NtCreateThreadEx(0x0037)`: aliases NtCreateThread (extended flags deferred)
- [x] `NtOpenProcess(0x0032)`: lookup by CLIENT_ID.UniqueProcess; returns HANDLE
- [x] `NtOpenThread(0x0038)`: lookup by CLIENT_ID.UniqueProcess + UniqueThread
- [x] `NtTerminateProcess(0x0033)`: upgraded from §5 with proper handle lookup, ob_process_mark_dead
- [x] `NtTerminateThread(0x0039)`: set TASK_DEAD, ob_thread_mark_dead
- [x] `NtResumeThread(0x003A)`: decrement suspend_count; TASK_READY if 0
- [x] `NtSuspendThread(0x003B)`: increment suspend_count; TASK_BLOCKED; STATUS_SUSPEND_COUNT_EXCEEDED at 127
- [ ] `NtGetContextThread(0x003C)`: stub returning STATUS_NOT_IMPLEMENTED (needs CONTEXT from TODO-23)
- [ ] `NtSetContextThread(0x003D)`: stub returning STATUS_NOT_IMPLEMENTED (needs CONTEXT from TODO-23)
- [x] `NtQueryInformationThread(0x003E)`: ThreadBasicInformation (TEB, client ID, priority), ThreadPriority, ThreadBasePriority
- [x] `NtSetInformationThread(0x003F)`: ThreadPriority, ThreadBasePriority (via thread_set_priority), ThreadAffinityMask, ThreadIdealProcessor (accepted, enforcement deferred)
- [x] `NtQueryInformationProcess(0x0034)`: ProcessBasicInformation (PEB, affinity, parent PID), ProcessPriorityClass
- [x] `NtSetInformationProcess(0x0035)`: ProcessPriorityClass, ProcessDefaultHardErrorMode (accepted)
- [ ] `NtSetInformationProcess(0x0035)` ProcessHandleQuota: call `ob_handle_table_set_limit`; gate raise-above-default on `SeSinglePrivilegeCheck(SeIncreaseQuotaPrivilege)`, reject a 0 quota -> XREF: `02-kernel-core/TODO-05-object-manager.md §14`
- [x] `NtAlertThread(0x0040)`: returns SUCCESS (APC delivery deferred to T07 §12 KiDeliverApc)
- [x] `NtAlertResumeThread(0x0041)`: alert + NtResumeThread
- [x] `NtTestAlert(0x0046)`: returns SUCCESS (APC check deferred to T07 §12 KeTestAlertThread)
- [ ] `NtQueueApcThread(0x0043)`/`NtQueueApcThreadEx`/`NtQueueApcThreadEx2`: stub returning STATUS_NOT_IMPLEMENTED; Ex/Ex2 carry the special-user-APC flag -> XREF: T07 §11/§12 (KeInsertQueueApc + SpecialUserApcPending)
- [ ] `NtImpersonateThread(0x0042)`: stub returning STATUS_NOT_IMPLEMENTED (needs SRM from TODO-15)
- [x] `NtDelayExecution(0x0047)`: yield-loop sleep with uptime tick counter; negative 100-ns interval
- [x] Added `suspend_count` field to `struct thread` in task.h
- [x] New file: `include/kernel/nt/nt_process.h` with THREAD_BASIC_INFORMATION, PROCESS_BASIC_INFORMATION, info class constants
- [x] New file: `src/kernel/nt/nt_process.c` with 23 SSDT handlers
- [x] 2 unit tests: SSDT registration (7 handler checks), info class constants (7 value checks)
- [ ] NtGetNextProcess (0x0202) + NtGetNextThread (0x0203): modern enumeration -- restart from a previous handle with rights checks, no leaked references (slots reserved in `TODO-A-SSDT-Master-Table` / `service_numbers.h`; XREF `T21 §4`)
- [ ] NtSet/QueryInformationProcess mitigation carriers (ChildProcessPolicy + Signature + ImageLoad); SystemCallDisablePolicy -> §25. T21 §11 dispatch wired but both handlers DEFER (NOT_SUPPORTED) pending usercopy (TODO-23 §13)
- [ ] [Critical] NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects (`nt_process.c`) -- corrupts OB metadata, no type/access check. Use real OB objects + rights -> XREF `D02 T05 §2`. (§7)
- [ ] [Critical] NtCreateProcess leaves a runnable NULL-entry task (`nt_process.c:70` task_create entry=0) + no teardown on post-create alloc fail; create non-runnable until exec + add teardown. (§7)
- [ ] [H] Process-handle waits reap and require parentage: `wait_on_body` (nt_sync.c) uses parent-only reaping `task_waitpid`; NT waits are neither -- add a non-reaping process exit event for any handle holder. (§7)
- [x] Commit: `"kernel: nt -- NtCreateProcess, NtCreateThread, full process-thread lifecycle"`

**Test checkpoint:** `NtCreateProcess` returns valid HANDLE; PID visible in `\KernelObjects\`. `NtCreateThread` with `CreateSuspended=TRUE` doesn't run until `NtResumeThread`. `NtGetContextThread`/`NtSetContextThread` currently return `STATUS_NOT_IMPLEMENTED` until TODO-23 provides `CONTEXT`. `NtDelayExecution` sleeps for the correct interval.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 346 kernel + 16 user tests, 0 failures

> **Notes:**
> - 23 process/thread lifecycle handlers shipped in `nt_process.c` (create/open/terminate/suspend/resume, query/set info, delay), registered in the SSDT.
> - Adversarial review: handle model is a PID/TID-as-handle shortcut (not OB objects), NtCreateProcess leaves a runnable NULL-entry task, info syscalls unprobed -- 3 [Critical] filed (2 §7 -> D02 T05 §2, 1 -> §29).
> - Most other open items are blocked on external prerequisites (env-copy child-inheritance wiring gated on the D03 T07 §3 publication lock + D03 T06 §13 slot-reservation (the two §7 prereq items), NtCreateUserProcess D02 T21, Context D02 T23, ApcThread D02 T07, Impersonate D02 T15) and defer with their existing XREFs.

> **Deferred:** [Critical] §7 process/thread handle model (real OB PROCESS/THREAD objects + handle-rights + user-pointer probes + non-runnable NtCreateProcess + teardown) is gated on the OB object model + the NT trust-boundary campaign with §29 -> XREF: 02-kernel-core/TODO-12 §7 (item: "NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct" at line 413, + sibling at 414) and D02 T05 §2

---

## 8. Synchronisation Objects + NtWaitForMultipleObjects
Synchronisation objects through Ob-managed named types (→ XREF TODO-05 §6). Includes multi-wait and keyed event support.

- [x] `NtCreateEvent(0x0070)`: wraps NtCreateEvent in ob_event.c; SSDT handler parses OBJECT_ATTRIBUTES for named events
- [x] `NtOpenEvent(0x0071)`: ObLookupObjectByName + ObpAllocateHandle
- [x] `NtSetEvent(0x0072)`: event_set, returns PreviousState
- [x] `NtResetEvent(0x0073)`: event_reset, returns PreviousState
- [x] `NtPulseEvent(0x0074)`: set + immediate reset (wakes waiting threads)
- [x] `NtQueryEvent(0x0075)`: returns EVENT_BASIC_INFORMATION (EventType + EventState)
- [x] `NtWaitForSingleObject(0x0006)`: body pinned (`ObReferenceObjectSafe`) for the wait; timeout=0 is a consuming poll; finite timeouts honored on all types via `wait_on_body()`
- [x] `NtWaitForMultipleObjects(0x0007)`: two-phase -- resolve+pin all handles once, then poll-acquire; WaitAll is all-or-none with rollback and rejects duplicate bodies
- [x] `NtSignalAndWaitForSingleObject(0x0008)`: signal-then-wait; mutant signal enforces ownership and unwinds one recursion level
- [x] `NtCreateMutant(0x0076)`: wraps NtCreateMutex in ob_mutex.c; InitialOwner support
- [x] `NtOpenMutant(0x0077)`: ObLookupObjectByName for ObpMutexType
- [x] `NtReleaseMutant(0x0078)`: ownership check (STATUS_MUTANT_NOT_OWNED); recursive holds unwind before mutex_unlock; PreviousCount = 1 - depth
- [x] `NtQueryMutant(0x0079)`: MUTANT_BASIC_INFORMATION; CurrentCount reflects recursion depth (1 free, 0 held, negative recursive)
- [x] `NtCreateSemaphore(0x007A)`: wraps NtCreateSemaphore in ob_semaphore.c; validates initial <= max
- [x] `NtOpenSemaphore(0x007B)`: ObLookupObjectByName for ObpSemaphoreType
- [x] `NtReleaseSemaphore(0x007C)`: via `sem_release_checked()` -- 64-bit max-count guard, PreviousCount after validation, batched `sem_signal_n()` (O(waiters), not O(count))
- [x] `NtQuerySemaphore(0x007D)`: returns SEMAPHORE_BASIC_INFORMATION
- [/] `NtCreateKeyedEvent(0x0084)`: stub STATUS_NOT_IMPLEMENTED -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtCreateKeyedEvent" at line 252)
- [/] `NtOpenKeyedEvent(0x0085)`: stub -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtOpenKeyedEvent" at line 253)
- [/] `NtWaitForKeyedEvent(0x0086)`: stub -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtWaitForKeyedEvent" at line 254)
- [/] `NtReleaseKeyedEvent(0x0087)`: stub -> XREF: 03-memory-concurrency/TODO-08 §10 (item: "NtReleaseKeyedEvent" at line 255)
- [x] New file: `include/kernel/nt/nt_sync.h` with wait constants, info structs
- [x] New file: `src/kernel/nt/nt_sync.c` with 21 SSDT handlers
- [x] STATUS_WAIT_0, STATUS_ABANDONED, STATUS_MUTANT_LIMIT_EXCEEDED canonical in ntstatus.h (nt_sync.h includes it, no duplication)
- [x] Unit tests in `test_nt_sync.c` (TEST_CAT_ABI): registration, constants, semaphore overflow guard, WaitAll rollback, duplicate/alias rejection, WaitAny consume, mutant recursion + ceiling, SignalAndWait, absolute-deadline handling
- [x] [H] NtWaitForMultipleObjects WaitAll made all-or-none: two-phase resolve+pin, readiness prepass, poll-acquire with reverse rollback; failed/timed-out WaitAll consumes nothing
- [x] [H] NtReleaseSemaphore + NtSignalAndWait share `sem_release_checked()`: 64-bit MaximumCount guard, PreviousCount written only after validation
- [x] `event_try_consume()` (sched/event.c): read-before-CAS consuming poll; event_wait/event_wait_timeout also CAS-consume; timeout=0 paths consume per NT semantics
- [x] Mutant recursion: `MUTEX_OBJECT.recursion` + `MUTANT_MAX_RECURSION` cap (STATUS_MUTANT_LIMIT_EXCEEDED, no wrap); MUTANT_BASIC_INFORMATION matches Windows 8-byte ABI
- [x] `nt_timeout_to_ms` on `ke_delay_interval_to_ms`: absolute deadlines gated on `wall_clock_time_sourced()`, expired/unsourced resolve to poll (no infinite hang)
- [x] Commit: `"kernel: nt -- sync objects, NtWaitForMultipleObjects, keyed events"`

**Test checkpoint:** `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` round-trip succeeds. `NtWaitForMultipleObjects(WaitAny)` returns correct index and consumes the satisfying auto-reset signal; a partial `WaitAll` rolls back and consumes nothing. `NtCreateMutant` with `InitialOwner=TRUE` is owned by caller and recursively re-acquirable. Named objects visible in `\BaseNamedObjects\`. Keyed event APIs (0x0084-0x0087) return `STATUS_NOT_IMPLEMENTED` pending 03-memory-concurrency/TODO-08 §10.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 405 kernel + 16 user tests, 0 failures

> **Notes:**
> - **What shipped:** `nt_sync.c` wait-path rewrite: all-or-none WaitAll (prepass + rollback), consuming polls via `event_try_consume()`, per-wait pinning, shared `sem_release_checked()`, wrap-safe mutant recursion, source-gated timeouts.
> - **How it runs:** all 21 handlers dispatch through the SSDT unchanged; waits resolve the handle once, pin the body, and operate on the pinned body until return.
> - **Downstream effects:** WaitAny/WaitAll match Win32 `WaitForMultipleObjects`; MUTANT_BASIC_INFORMATION matches the Windows 8-byte ABI; Codex adoption evidence lives in the commit messages.
> - **Scope boundary:** keyed events owned by 03-memory-concurrency/TODO-08 §10; primitive wait-queue/auto-reset-handoff SMP atomicity by 03-memory-concurrency/TODO-08 §11; process-wait parent/timeout hardening by §7.
> **Verified:** 2026-07-02 | commit `31899c0d` | 27/31 items | build OK | abi 405/405 PASS
> **Accepted:** [Critical] handle lookup-then-ObReferenceObjectSafe pin has a freed-header window a concurrent NtClose can hit -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(...)` primitive" at line 148 -- retrofit list names nt_sync.c)
> **Accepted:** [H] auto-reset event_set publishes state before waking a queued waiter, so a timeout=0 poll on another CPU can steal the wake -> XREF: 03-memory-concurrency/TODO-08 §11 (item: "Semaphore + event atomicity" at line 281 -- names the direct-handoff-to-queued-waiter requirement)
> **Accepted:** [M] sem_release_checked check-then-signal + WaitAll rollback are not serialized against a concurrent NtReleaseSemaphore -> XREF: 03-memory-concurrency/TODO-08 §11 (item: "Semaphore + event atomicity" at line 281 -- serialize max-guard + rollback)
> **Accepted:** [M] multi-wait polls stay READY with no backoff (interim poll design; blocking waitable not built) -> XREF: 03-memory-concurrency/TODO-08 §8 (item: "`wait_any(waitable_t *handles[], count, timeout_ms)`" at line 220 -- the blocking waitable_t multi-wait that replaces the poll)
> **Accepted:** [M] sync SSDT handlers deref ring-3 handle-array/timeout/out pointers without probe/copy (systemic NT trust-boundary gap) -> XREF: 02-kernel-core/TODO-12 §29 (item: "Sync syscalls deref raw user pointers (`nt_sync.c`)" at line 1023)
> **Quality reviewed:** 2026-07-02 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 5H+2M fixed, 5 accepted-XREF | scope: kernel-code-quality

---

## 9. Virtual Memory (Alloc, Free, Protect, Lock, Cross-Process)
Virtual memory management entry points -- covers the full NT virtual memory API surface.

- [x] `NtAllocateVirtualMemory(0x0050)`: PMM contiguous allocation + VMM page mapping; PAGE_* to VMM flag conversion; zero-fills committed pages
- [x] `NtFreeVirtualMemory(0x0051)`: vmm_unmap_page + pmm_free_frame for each page in range
- [x] `NtProtectVirtualMemory(0x0052)`: re-maps pages with new VMM flags; returns OldProtect
- [x] `NtQueryVirtualMemory(0x0053)`: MemoryBasicInformation -- base, state (committed/free), protect, type
- [x] `NtLockVirtualMemory(0x0054)`: no-op STATUS_SUCCESS (no swap yet -- all pages pinned)
- [x] `NtUnlockVirtualMemory(0x0055)`: no-op STATUS_SUCCESS
- [x] `NtFlushVirtualMemory(0x0056)`: no-op STATUS_SUCCESS (no backing store yet)
- [x] `NtReadVirtualMemory(0x0057)`: identity-mapped memcpy from source address; cross-process via CR3 deferred
- [x] `NtWriteVirtualMemory(0x0058)`: identity-mapped memcpy to target address
- [x] Checked page-range guard `nt_vm_check_range()` on Alloc/Free/Protect (rejects zero/near-max RegionSize and base+size wrap; MEM_RELEASE size==0 rejects instead of silently leaking)
- [/] `NtAllocateUserPhysicalPages(0x0059)`: AWE stub STATUS_NOT_IMPLEMENTED -> XREF: 03-memory-concurrency/TODO-05-advanced-virtual-memory.md §11 (item: "NtAllocateUserPhysicalPages(0x0059)" at line 242)
- [/] `NtFreeUserPhysicalPages(0x005A)`: AWE stub -> XREF: 03-memory-concurrency/TODO-05-advanced-virtual-memory.md §11 (item: "NtFreeUserPhysicalPages(0x005A)" at line 243)
- [/] `NtMapUserPhysicalPages(0x005B)`: AWE stub -> XREF: 03-memory-concurrency/TODO-05-advanced-virtual-memory.md §11 (item: "NtMapUserPhysicalPages(0x005B)" at line 244)
- [x] New file: `include/kernel/nt/nt_memory.h` -- MEM_COMMIT/RESERVE/RELEASE, PAGE_* flags, MEMORY_BASIC_INFORMATION
- [x] New file: `src/kernel/nt/nt_memory.c` -- 12 SSDT handlers (9 implemented + 3 AWE stubs deferred to advanced-VM §11)
- [x] 2 unit tests: SSDT registration (5 handler checks), constant verification (12 checks) -- test_nt_types.c TEST_CAT_ABI
- [x] Commit: `"kernel: nt -- NtAllocate/Free/Protect/Lock/Read/WriteVirtualMemory"`

**Test checkpoint:** `NtAllocateVirtualMemory` with `MEM_COMMIT | PAGE_READWRITE` returns usable address; write+read round-trip. `NtProtectVirtualMemory` changes RW→RO; write attempt faults. `NtFreeVirtualMemory` with `MEM_RELEASE` returns `STATUS_SUCCESS`. `NtReadVirtualMemory` from kernel to user address space succeeds. AWE APIs (0x0059-0x005B) currently return `STATUS_NOT_IMPLEMENTED`.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 405 kernel + 16 user tests, 0 failures

> **Notes:**
> - **What shipped:** `nt_memory.c` -- 9 VM SSDT handlers (Alloc/Free/Protect/Query/Lock/Unlock/Flush/Read/Write) over VMM+PMM, PAGE_*<->VMM-flag conversion, MEMORY_BASIC_INFORMATION; 3 AWE handlers are STATUS_NOT_IMPLEMENTED stubs.
> - **How it runs:** all 12 handlers dispatch through the SSDT; alloc uses PMM contiguous + VMM mapping, protect re-maps with new flags, read/write are identity-mapped memcpy (cross-process via CR3 deferred).
> - **Downstream effects:** the 9 core VM syscalls back user-mode VirtualAlloc/VirtualProtect/VirtualFree; AWE deferral filed with a concrete owner.
> - **Scope boundary:** AWE (0x0059-0x005B, physical page windows) owned by 03-memory-concurrency/TODO-05-advanced-virtual-memory §11; cross-process read/write via CR3 + per-process isolation owned by the Win32 PE loader work.
> **Deferred:** [Critical] `NtProtectVirtualMemory` reports success but does not enforce protection -- base==0 pages are identity-mapped huge pages so vmm_get_physical is non-zero and the remap is skipped; needs per-process 4KiB PTEs (reason: infra) -> XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §1 (item: "Wire `NtProtectVirtualMemory(handle, &base, &size, new_protect, &old_protect)`" at line 89)
> **Deferred:** [Critical] `NtFreeVirtualMemory` MEM_RELEASE size==0 cannot free without per-region extent tracking; now returns STATUS_INVALID_PARAMETER instead of a silent leak-success (reason: infra) -> XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §3 (item: "Wire `NtFreeVirtualMemory(handle, &base, &size, MEM_RELEASE)`" at line 127)
> **Deferred:** [H] `NtAllocateVirtualMemory` MEM_COMMIT has no partial-alloc rollback (needs a PMM free-contiguous helper) (reason: infra) -> XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §3 (item: "Wire `NtAllocateVirtualMemory(handle, &base, zero_bits, &size, type, protect)`" at line 126)
> **Deferred:** [H] VM handlers mutate PMM bitmap + VMM page tables with no address-space lock or TLB shootdown (single-thread-assumed) (reason: infra) -> XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §3 (item: "Per-process PML4 spinlock" at line 129)

---

## 10. NtQuerySystemInformation / NtQueryInformationProcess
Provides OS version, process list, performance counters, and detailed process info to ntdll and user-mode tools.

- [/] `NtQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ReturnLength)`:
  - [x] `SystemBasicInformation (0)`: page size, CPU count, min/max user address, allocation granularity
  - [x] `SystemProcessorInformation (1)`: AMD64 architecture, level, max CPUs
  - [x] `SystemPerformanceInformation (2)`: PMM free/used/total pages
  - [x] `SystemTimeOfDayInformation (3)`: uptime seconds (from §5)
  - [x] `SystemProcessInformation (5)`: PID, state, name list (from §5)
  - [/] `SystemProcessorPerformanceInformation (8)`: deferred -- needs per-CPU time accounting (see Deferred Class Completion Checklist below)
  - [/] `SystemModuleInformation (11)`: deferred -- needs module loader (see Deferred Class Completion Checklist below)
  - [/] `SystemHandleInformation (16)`: deferred -- needs system-wide handle snapshot (see Deferred Class Completion Checklist below)
  - [/] `SystemObjectInformation (17)`: deferred -- OB type-stats marshalling (see Deferred Class Completion Checklist below)
  - [/] `SystemInterruptInformation (23)`: deferred -- needs per-CPU interrupt counters (see Deferred Class Completion Checklist below)
  - [/] `SystemExceptionInformation (33)`: deferred -- needs exception counters (see Deferred Class Completion Checklist below)
  - [/] `SystemRegistryQuotaInformation (37)`: deferred -- needs registry quota model (see Deferred Class Completion Checklist below)
  - [/] `SystemBootPerformanceInformation (custom)`: deferred (→ XREF: `TODO-01-kernel-init-sequencing.md §12`)
  - [x] Unimplemented classes return `STATUS_NOT_IMPLEMENTED` via default case
- [/] `NtSetSystemInformation(0x00D1)`: stub returning STATUS_NOT_IMPLEMENTED -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md (SeSystemtimePrivilege / SeSystemEnvironmentPrivilege enforcement)
- [ ] [H] Fixed-size query handlers probe the raw user `Length`, not the derived output size -- §13's full-range `ProbeForWrite` touch makes that an O(pages) DoS. Probe the selected struct size (e.g. NtQueryTimer) -> XREF: `TODO-23 §13`
- [x] `NtQueryInformationProcess` extended with 7 new info classes:
  - `ProcessBasicInformation (0)`: PEB, PID, parent PID, affinity (done in §7)
  - `ProcessTimes (4)`: creation, exit, kernel, user times (zeroed -- the per-task fields exist (TODO-21 §8) but the ring-3 output copy is gated on PTE-aware usercopy; wire here when that lands)
  - `ProcessDebugPort (7)`: 0 (not debugged)
  - `ProcessPriorityClass (18)`: base priority (done in §7)
  - `ProcessHandleCount (20)`: handle_table.count
  - `ProcessSessionInformation (24)`: session 0
  - `ProcessWow64Information (26)`: 0 (native 64-bit)
  - `ProcessImageFileName (27)`: task name string
- [x] 2 unit tests: SystemInfo classes (5 checks incl. PageSize==4096), ProcessInfo classes (4 checks incl. DebugPort==0)
- [x] Commit: `"kernel: nt -- NtQuerySystemInformation and NtQueryInformationProcess"`

**Test checkpoint:** `NtQuerySystemInformation(SystemBasicInformation)` returns correct page size (4096) and processor count. `NtQueryInformationProcess(ProcessBasicInformation)` returns valid PEB address (0x7FFDE000). `SystemProcessInformation` enumerates all running processes.

### Deferred Class Completion Checklist

- [/] `SystemProcessorPerformanceInformation (8)`: add real CPU time accounting first (scheduler tick attribution), then expose per-CPU idle/kernel/user times via `NtQuerySystemInformation`. The accounting groundwork is still pending in `todo/02-kernel-core/TODO-21-process-model-extensions.md` (line 188).
- [/] `SystemModuleInformation (11)`: implement module registry/loader list (`exec_register_module`, lookup/enumeration), then serialize that list in `NtQuerySystemInformation`. Module list work is still planned in `todo/02-kernel-core/TODO-17-binary-system.md` (line 165).
- [/] `SystemHandleInformation (16)`: add a safe system-wide handle table snapshot across all processes (PID + handle + access + object/type), then expose it through class 16. Current implementation has per-process tables only.
- [/] `SystemObjectInformation (17)`: wire `NtQuerySystemInformation` to OB type stats aggregation. Most raw stats already exist in `src/kernel/ob/ob.c` (line 363), but class 17 marshalling is not implemented.
- [/] `SystemInterruptInformation (23)`: add per-CPU interrupt counters and return them. You currently have per-vector global IRQ counts in `src/kernel/irq.c` (line 23), but no per-CPU increment path is wired.
- [/] `SystemExceptionInformation (33)`: add exception counters in the IDT exception path, then expose those totals. Exception handling exists, but exception stats tracking does not.
- [/] `SystemRegistryQuotaInformation (37)`: implement registry quota accounting/enforcement (limit, used, peak) in registry write paths, then query output. Current registry code has no quota model.
- [/] `SystemBootPerformanceInformation (custom)`: boot perf NVRAM read/write is already present in `src/kernel/boot_timing.c` (line 333), but you still need a kernel accessor/API and a class serializer in `NtQuerySystemInformation` for user-mode consumption.
- [/] For all deferred classes: add ABI structs + buffer-size handling + unit tests in `src/kernel/test/test_nt_types.c`, then update TODO-12 §10 checklist states.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 405 kernel + 16 user tests, 0 failures

> **Notes:**
> - **What shipped:** `NtQuerySystemInformation` (5 SystemInfo classes) + `NtQueryInformationProcess` (8 process info classes) through the SSDT; unimplemented classes return STATUS_NOT_IMPLEMENTED via the default case.
> - **How it runs:** each info class marshals kernel state (PMM counts, CPU count, PID/name list, PEB/handle-count) into the caller buffer with length/ReturnLength handling.
> - **Downstream effects:** backs user-mode GetSystemInfo / NtQuerySystemInformation for the implemented classes; the 8 deferred SystemInfo classes are each owner-tracked in the Deferred Class Completion Checklist above.
> - **Scope boundary:** deferred classes need CPU-time accounting (TODO-21), module loader (TODO-17), per-CPU/exception counters, registry quota, boot-perf serialization; NtSetSystemInformation needs SRM privilege enforcement (TODO-15).
> **Deferred:** [M] 8 SystemInformation classes (8/11/16/17/23/33/37/custom) return STATUS_NOT_IMPLEMENTED pending their data-source infrastructure (reason: infra) -> XREF: 02-kernel-core/TODO-21-process-model-extensions.md (CPU-time accounting at line 188) + 02-kernel-core/TODO-17-binary-system.md (module list at line 165)
> **Deferred:** [M] `NtSetSystemInformation(0x00D1)` stub pending privilege enforcement (reason: infra) -> XREF: 02-kernel-core/TODO-15-security-reference-monitor.md (SeSystemtimePrivilege / SeSystemEnvironmentPrivilege access check)

---

## 11. Extended Error Information (IOSB + LastError)
NT propagates detailed error info through two channels: `IO_STATUS_BLOCK` (async I/O) and `TEB->LastErrorValue` (Win32 `GetLastError`). Both must be populated correctly.

- [x] All file I/O `NtXxx` functions write final `NTSTATUS` into `IoStatusBlock->Status` and byte count / disposition into `IoStatusBlock->Information` (already implemented in §6: NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile, NtQueryDirectoryFile all populate IOSB)
- [x] On every `NTSTATUS` return from a syscall: if `NT_ERROR(status)`, write the Win32 error translation into `TEB->LastErrorValue` (at `gs:[0x68]`) via `RtlNtStatusToDosError`. Wired in both SYSCALL (`syscall_fast.c`) and INT 0x2E (`syscall.c`) return paths.
- [x] `RtlNtStatusToDosError` 12-entry table in `ssdt.c`: SUCCESS->0, ACCESS_DENIED->5, NO_MEMORY->8, INVALID_HANDLE->6, NAME_NOT_FOUND->2, NOT_IMPLEMENTED->50, INVALID_PARAMETER->87, BUFFER_TOO_SMALL->122, ACCESS_VIOLATION->998, PRIVILEGE_NOT_HELD->1314, UNSUCCESSFUL->1, NAME_COLLISION->183. Unknown->317.
- [x] Kernel writes `TEB->LastErrorValue` only in the syscall return path; user-mode `SetLastError` writes `gs:[0x68]` directly without a syscall.
- [x] RtlNtStatusToDosError broad coverage: `s_nt_to_dos` expanded to ~40 rows across the common NTSTATUS classes + success-severity codes, with lookup before the NT_SUCCESS fallback; broad-coverage tests added
- [x] Commit: `"kernel: nt -- IOSB and TEB LastErrorValue propagation"` (607eb38b)

**Test checkpoint:** After a failing `NtOpenFile` (non-existent path), `gs:[0x68]` == Win32 error code (2 = FILE_NOT_FOUND). After `NtReadFile`, IOSB `Status == STATUS_SUCCESS`, `Information == bytes_read`.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 419 kernel + 16 user tests, 0 failures

> **Notes:**
> - **What shipped:** IOSB propagation on all file NtXxx, TEB->LastErrorValue written on NT_ERROR syscall returns, and a ~40-row NTSTATUS->Win32 error table (`s_nt_to_dos` in ssdt.c) over the common status classes + success-severity codes.
> - **How it runs:** SYSCALL + INT 0x2E return paths call RtlNtStatusToDosError; the table lookup runs before the NT_SUCCESS fallback so mapped success codes (TIMEOUT/PENDING/ABANDONED) are not masked to ERROR_SUCCESS.
> - **Downstream effects:** user-mode GetLastError returns Windows-correct codes for the common classes; mappings verified against Microsoft's system-error-code tables.
> - **Scope boundary:** the table covers common classes, not the full ntdll table; per-CPU previous-mode SMP hardening is §12; async IOSB pending completion is owned by the IRP/async I/O work.
> **Verified:** 2026-07-02 | commit `a25b7dbb` | 6/6 items | build OK | abi 419/419 PASS
> **Accepted:** [H] user-mode GetLastError/SetLastError not implemented, so the kernel's TEB->LastErrorValue write is not yet observable through the Win32 API -> XREF: 10-platform-services/TODO-08-win32-api-surface.md §8 (item: "`GetLastError()` -> read `TEB.LastErrorValue`" at line 225)
> **Accepted:** [M] NT_WARNING statuses (e.g. STATUS_BUFFER_OVERFLOW) do not update TEB->LastErrorValue (the syscall return path is NT_ERROR-only); the Win32 error API defines the warning-code policy -> XREF: 10-platform-services/TODO-08-win32-api-surface.md §8 (item: "`SetLastError(code)` -> write `TEB.LastErrorValue`" at line 226)
> **Quality reviewed:** 2026-07-02 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed, 1H+1M accepted-XREF | scope: kernel-code-quality

---

## 12. ZwXxx Kernel-Mode Alias Layer

`ZwXxx` names are identical to `NtXxx` in user mode. In kernel mode (`CPL=0`), `ZwXxx` calls bypass the user-mode probe and use kernel-mode access rights directly. This is the convention all of Windows' own drivers and executive components use.

- [x] Add a `ZwXxx` header `include/kernel/nt/zw.h` that declares each `ZwXxx` as a static inline calling `ssdt_dispatch()` directly (15 aliases: ZwClose, ZwCreateFile, ZwOpenFile, ZwReadFile, ZwWriteFile, ZwQueryInformationFile, ZwQueryDirectoryFile, ZwCreateEvent, ZwCreateMutant, ZwCreateSemaphore, ZwQuerySystemInformation, ZwYieldExecution, ZwDuplicateObject, ZwQueryObject, ZwWaitForSingleObject)
- [x] NOTE: 13 of 15 aliases resolve to registered handlers; `ZwDuplicateObject`/`ZwQueryObject` target SSDT 0x0001/0x0002, unregistered (fail closed) until the NT-ABI wrappers land in §30.
- [x] Previous-mode tracking via `ssdt_set_previous_mode()`/`ssdt_previous_mode()`: syscall entry paths (SYSCALL + INT 0x2E) set UserMode before dispatch, restore KernelMode after. ZwXxx callers leave mode at KernelMode. Handlers use `ProbeForReadIfUser()`/`ProbeForWriteIfUser()` convenience wrappers.
- [x] Add `ProbeForRead(Address, Length, Alignment)` and `ProbeForWrite(Address, Length, Alignment)` in `ssdt.c`: validate NULL, overflow, range below `MM_USER_PROBE_ADDRESS` (0x7FFF0000), alignment (power of 2). Returns `STATUS_ACCESS_VIOLATION` or `STATUS_DATATYPE_MISALIGNMENT`.
- [x] Add `ASSERT_KERNEL_CALLER()` macro in `zw.h`: returns `STATUS_PRIVILEGE_NOT_HELD` if previous mode is UserMode.
- [x] Convention documented in `zw.h` header comment: kernel calls Zw (no probe), user calls Nt via SYSCALL (probed). Both resolve to the same SSDT handler.
- [/] Previous-mode moved to per-thread (`struct thread.previous_mode` via `thread_current()`); cross-CPU closure needs per-CPU `thread_current()` -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor")
- [x] Commit: `"kernel: nt -- ZwXxx kernel-mode alias layer with CPL probe bypass"` (638568e8)

**Test checkpoint:** `ZwClose` from CPL=0 succeeds without user-buffer probe. CPL=3 call with kernel-space pointer returns `STATUS_ACCESS_VIOLATION`. `ASSERT_KERNEL_CALLER()` fires `STATUS_PRIVILEGE_NOT_HELD` from ring 3.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 423 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: ZwXxx CPL-bypass alias layer (15 static-inline aliases), ProbeForRead/Write user-buffer validation, ASSERT_KERNEL_CALLER, per-thread NT previous-mode.
> - Integrates: syscall entry (SYSCALL + INT 0x2E) sets UserMode before dispatch; handlers gate probes via ProbeFor*IfUser() on ssdt_previous_mode().
> - Downstream: previous_mode moved from a global to struct thread; reset to KernelMode on every slot-reuse and reap path (before the THREAD_FREE publish).
> - Scope boundary: 13/15 aliases live; ZwDuplicateObject/ZwQueryObject slots + full cross-CPU cursor deferred.
> - Canonical: zw.h header comment + ssdt.c SMP-closure note.
> **Verified:** 2026-07-02 | commit `777ca1de` | 6/7 items | build OK | abi 423/423 + sched 296 PASS
> **Accepted:** [H] `thread_reap_kernel_slot` publishes THREAD_FREE before APC rundown + field cleanup finish (pre-existing SMP reap race; a lockless kthread_create scan can claim the slot mid-reap) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "`thread_reap_kernel_slot` publishes `THREAD_FREE`" at line 129)
> **Accepted:** [M] cross-CPU probe-gating: `ssdt_previous_mode()` resolves via `thread_current()`, which reads the global current-thread cursor, not per-CPU state -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor" at line 120)
> **Deferred:** [M] `ZwDuplicateObject`/`ZwQueryObject` dispatch to unregistered SSDT 0x0001/0x0002 (fail closed) -> XREF: 02-kernel-core/TODO-12 §30 (item: "Register `NtDuplicateObject` → SSDT 0x0001" at line 1084; item: "Expose `NtQueryObject` at SSDT 0x0002" at line 1089)
> **Quality reviewed:** 2026-07-02 | Codex 8x (adversarial x2, consistency x2, perf x2, re-adversarial x2) | 2H+4M+1L fixed, 1H+2M deferred/accepted-XREF | scope: kernel-code-quality

---

## 13. File Metadata and Device Control
Extended file operations: metadata queries, attribute modification, device I/O control, file locking, I/O completion ports. These are the Win32 `GetFileAttributes`, `SetFileTime`, `DeviceIoControl`, `LockFile` foundation.

- [x] `NtQueryInformationFile` (SSDT 0x0015): FileBasicInformation (4), FileStandardInformation (5), FileNameInformation (9), FilePositionInformation (14), FileNetworkOpenInformation (34). Uses VFS stat + FILE_OBJECT offset. FileAllInformation (18) deferred (compound query).
- [x] `NtSetInformationFile` (SSDT 0x0016): FileBasicInformation (set times/attrs), FileDispositionInformation (delete-on-close), FileRenameInformation (UTF-16 rename), FilePositionInformation (seek), FileEndOfFileInformation (truncate), FileAllocationInformation (pre-allocate).
- [x] `NtDeleteFile` (SSDT 0x0014): delete by path via OBJECT_ATTRIBUTES -> vfs_unlink.
- [x] `NtQueryDirectoryFile`: extended info classes (FileDirectoryInformation, FileBothDirectoryInformation, FileIdBothDirectoryInformation) implemented in §28 with per-entry vfs_stat metadata, 8-byte-aligned packing, ReturnSingleEntry/RestartScan, and FILE_OBJECT cursor state.
- [x] `NtDeviceIoControlFile` (SSDT 0x0019): registered, returns STATUS_INVALID_DEVICE_REQUEST until IRP framework (driver model prerequisite). METHOD_* constants defined.
- [x] `NtFsControlFile` (SSDT 0x001A): registered, returns STATUS_INVALID_DEVICE_REQUEST (requires FS-specific control codes).
- [x] `NtFlushBuffersFile` (SSDT 0x0018): calls VFS flush op on the file's vfs_node.
- [x] `NtLockFile` (SSDT 0x001D): calls vfs_lock_file() with exclusive lock.
- [x] `NtUnlockFile` (SSDT 0x001E): calls vfs_unlock_file().
- [x] `NtNotifyChangeDirectoryFile` (SSDT 0x001F): registered, returns STATUS_INVALID_DEVICE_REQUEST (requires async I/O / IRP pending infrastructure).
- [x] `NtQueryVolumeInformationFile` (SSDT 0x0020): FileFsSizeInformation, FileFsVolumeInformation ("Impossible", serial 0x494D5053), FileFsAttributeInformation ("IXFS", case-sensitive).
- [x] `NtQueryAttributesFile` (SSDT 0x0028): lightweight stat by path via OBJECT_ATTRIBUTES -> vfs_stat -> FILE_BASIC_INFORMATION.
- [x] `NtCancelIoFile` (SSDT 0x0026) / `NtCancelIoFileEx` (SSDT 0x0027): no-op success (all I/O is synchronous; nothing to cancel).
- [x] `NtCreateIoCompletion` (SSDT 0x0088) / `NtSetIoCompletion` (SSDT 0x0089) / `NtRemoveIoCompletion` (SSDT 0x008A): 16-port pool, 64-entry ring buffer per port, post/dequeue round-trip functional.
- [x] `NtCreateMailslotFile` (SSDT 0x001C): registered, returns STATUS_INVALID_DEVICE_REQUEST (one-way IPC deferred).
- [x] `NtReadFileScatter` (SSDT 0x0024) / `NtWriteFileGather` (SSDT 0x0025): registered, returns STATUS_INVALID_DEVICE_REQUEST (requires page-aligned buffer segments).
- [x] Commit: `"kernel: nt -- file metadata, device control, I/O completion ports"` (928051e4)

**Test checkpoint:** `NtQueryInformationFile(FileBasicInformation)` returns valid timestamps. `NtSetInformationFile(FileDispositionInformation)` marks file for delete; file removed after close. `NtDeviceIoControlFile` reaches driver dispatch. I/O completion port post + dequeue round-trip succeeds.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 18 NtXxx file-metadata/device-control/IO-completion handlers in nt_file.c; IRP-dependent ops are honest STATUS_INVALID_DEVICE_REQUEST stubs.
> - Integrates: SSDT 0x0014-0x0028 + 0x0088-0x008A; ALPC posts completion packets into the same IOCP ports via io_completion_post.
> - Hardened this review: rename length validation, lazy stat (no query-loop DoS), FileNameInformation overflow contract + bounded copy, volume-info offset-18 ABI (details in commit).
> - Scope boundary: user-buffer probes, IOCP OB-isolation, IXFS long-name rejection deferred to owners; & 0x7F narrowing + size-hint omission are known Lows.
> - Test gap: security-path negative tests need a live VFS file-handle fixture; registration checks cover all handlers.
> **Verified:** 2026-07-02 | commit `24c110fb` | 16/16 items | build OK | abi 426/426 PASS
> **Accepted:** [Critical] IOCP `idx+0x10000` pseudo-handles are globally guessable (cross-task inject/drain), no per-process OB isolation -> XREF: 02-kernel-core/TODO-05 §9 (item: "Migrate IO completion ports to OB handles" at line 332)
> **Accepted:** [M] IXFS silently truncates names > 252 bytes on rename/create, so a 252-259 char op resolves to a different entry -> XREF: 05-storage-filesystems/TODO-06 §1 (item: "Reject over-length names" at line 79)
> **Quality reviewed:** 2026-07-02 | Codex 8x (adversarial x2, consistency x2, perf x2, re-adversarial x2) | 4H+4M fixed, 1Crit+1M accepted-XREF | scope: kernel-code-quality

---

## 14. Registry Syscalls (Core CRUD)

> [!NOTE]
> Full registry engine implemented in TODO-14-registry-completion.md. This section wires the core registry CRUD operations into the SSDT with NT-compatible signatures and NTSTATUS return values. Advanced operations (flush, notify, save/restore, hive load/unload) are in §15.

- [x] `NtCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition)` → SSDT 0x0090
- [x] `NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0092
- [x] `NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions)` → SSDT 0x0094
- [x] `NtDeleteKey(KeyHandle)` → SSDT 0x0095
- [x] `NtSetValueKey(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize)` → SSDT 0x0096
- [x] `NtQueryValueKey(KeyHandle, ValueName, KeyValueInformationClass, KeyValueInformation, Length, ResultLength)` → SSDT 0x0097
- [x] `NtDeleteValueKey(KeyHandle, ValueName)` → SSDT 0x0098
- [x] `NtEnumerateKey(KeyHandle, Index, KeyInformationClass, KeyInformation, Length, ResultLength)` → SSDT 0x0099
- [x] `NtEnumerateValueKey(KeyHandle, Index, KeyValueInformationClass, KeyValueInformation, Length, ResultLength)` → SSDT 0x009A
- [x] `NtQueryKey(KeyHandle, KeyInformationClass, KeyInformation, Length, ResultLength)` → SSDT 0x009B
- [ ] Registry syscalls with 5+ args (NtSetValueKey, NtQueryValueKey, NtEnumerate*, NtQueryKey, NtCreateKey Disposition) drop a5/a6 on real ring-3 calls until the entry paths load user-stack args. XREF: TODO-12 §3 line 245.
- [ ] `NtEnumerateKey(KeyFullInformation)` opens a transient child HKEY per entry (128-slot pool, fails under handle pressure). Add a `RegQueryInfoKeyDirect(reg_key_t*)` metadata helper to fill KEY_FULL_INFORMATION without a handle.
- [x] Commit: `"kernel: nt -- core registry syscalls wired to SSDT (create/open/query/set/delete/enumerate)"` (6d6535d5)

**Test checkpoint:** `NtCreateKey` under `\Registry\Machine\Software\Test` returns `STATUS_SUCCESS`. `NtSetValueKey` + `NtQueryValueKey` round-trip succeeds. `NtDeleteKey` removes the key. `NtEnumerateKey` iterates subkeys correctly.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 10 NtXxx core-CRUD registry handlers in nt_registry.c wired to SSDT 0x0090-0x009B via registry.c Win32 helpers.
> - Integrates: paths resolve via nt_reg_resolve_path (\Registry\Machine|User|CurrentConfig); handles are HKEY pseudo-handles, not OB table.
> - Hardened this review: root-name exact-match boundary (no MachineXYZ->HKLM aliasing), sizeof-1->__builtin_offsetof header math across 12 sites (details in commit).
> - Scope boundary: 5+arg syscalls need §3 stack-arg support for real ring-3; HKEY-not-OB (TODO-14 §5), registry SMP locking (TODO-31 §14), UTF-16 decode (TODO-14 §5) stay deferred.
> - Test gap: STATUS_KEY_HAS_CHILDREN + ring-3 5+arg paths untested (direct-dispatch tests cover handler logic).
> **Verified:** 2026-07-02 | commit `1d898565` | 10/13 items | build OK | abi 426/426 PASS
> **Deferred:** [H] registry syscalls needing args 5/6 (Data/DataSize, Length/ResultLength) drop them on real ring-3 calls until the entry paths load user-stack args -> XREF: 02-kernel-core/TODO-12 §14 (item: "Registry syscalls with 5+ args" at line 687)
> **Deferred:** [M] NtEnumerateKey(KeyFullInformation) opens a transient child HKEY per entry (128-slot pool, fails under handle pressure) -> XREF: 02-kernel-core/TODO-12 §14 (item: "`NtEnumerateKey`(KeyFullInformation) opens a transient child HKEY" at line 711)
> **Accepted:** [M] exact-root match + resolver treat UNICODE_STRING as NUL-terminated ASCII (counted contract) -> XREF: 02-kernel-core/TODO-14 §5 (item: "UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)" at line 291)
> **Quality reviewed:** 2026-07-02 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1H+1M fixed, 1H+2M deferred/accepted-XREF | scope: kernel-code-quality


---

## 15. Registry Syscalls (Advanced)

> [!NOTE]
> Persistence, notification, and hive management operations. Depends on §14 (core CRUD) and TODO-14 hive infrastructure.

- [x] `NtFlushKey(KeyHandle)` → SSDT 0x009C: delegates to `registry_flush()`
- [/] `NtNotifyChangeKey(KeyHandle, Event, ..., CompletionFilter, WatchTree, Asynchronous)` → SSDT 0x009D (STATUS_NOT_IMPLEMENTED; §3 shipped the watcher engine, syscall wiring pending -> XREF: 02-kernel-core/TODO-14 §4)
- [x] `NtRenameKey(KeyHandle, NewName)` → SSDT 0x009F (uses `RegRenameKey` helper)
- [x] `NtSaveKey(KeyHandle, FileHandle)` / `NtSaveKeyEx(...)` → SSDT 0x00A0/0x00A1 (resolves FileHandle via `vfs_get_path_from_node`, then `hive_save`)
- [x] `NtRestoreKey(KeyHandle, FileHandle, Flags)` → SSDT 0x00A2 (same path resolution, then `hive_load`)
- [x] `NtLoadKey(ObjectAttributes, ObjectAttributes)` / `NtLoadKeyEx(...)` → SSDT 0x00A3/0x00A4 (creates registry mountpoint, loads hive file by path)
- [x] `NtUnloadKey(ObjectAttributes)` / `NtUnloadKeyEx(...)` → SSDT 0x00A5/0x00A6 (uses `RegUnloadHive` helper)
- [ ] **NtUnloadKey provenance guard**: NtUnloadKey unloads ANY resolved subkey (no loaded-by-NtLoadKey check), so a caller can destroy \Registry\Machine\SYSTEM. Record loaded-hive roots in NtLoadKey; NtUnloadKey must match before RegUnloadHive.
- [x] Commit: `"kernel: nt -- advanced registry syscalls (flush/notify/save/restore/hive load)"` (dd22e696)

**Test checkpoint:** `NtFlushKey` completes without error. `NtNotifyChangeKey` fires callback after `NtSetValueKey` on watched key. `NtSaveKey` + `NtRestoreKey` round-trip succeeds.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 9 advanced registry handlers (flush/rename/save/restore/load/unload hive) in nt_registry.c wired to SSDT 0x009C-0x00A6 via registry.c helpers; NtNotifyChangeKey is a tracked STATUS_NOT_IMPLEMENTED stub.
> - Integrates: NtSaveKey/Restore resolve FileHandle via resolve_file_handle_path (ObpFileType check) -> hive_save/hive_load; NtLoadKey creates a mountpoint + rolls back a newly-created key on hive_load failure.
> - Reviewed this pass: handler wiring correct; hive-parse robustness bugs are registry.c engine scope (TODO-14 §8), and NtUnloadKey lacks a loaded-hive provenance guard (filed Critical).
> - Scope boundary: hive_load bounds/transactionality + journal validation -> TODO-14 §8; change notifications -> TODO-14 §3; registry SMP locking -> TODO-31 §14; UTF-16 decode -> TODO-14 §5.
> - Test gap: NtSaveKey/RestoreKey/LoadKey functional paths + NtUnloadKey provenance rejection are untested (registration-only).
> **Verified:** 2026-07-02 | commit `422113c1` | 6/8 items | build OK | abi 426/426 PASS
> **Deferred:** [Critical] NtUnloadKey unloads any resolved subkey with no loaded-hive provenance check (can destroy \Registry\Machine\SYSTEM) -> XREF: 02-kernel-core/TODO-12 §15 (item: "NtUnloadKey provenance guard" at line 721)
> **Deferred:** [M] NtNotifyChangeKey returns STATUS_NOT_IMPLEMENTED (§3 shipped the watcher engine; the syscall wiring is §4) -> XREF: 02-kernel-core/TODO-14 §4 (item: "NtNotifyChangeKey" at line 297)
> **Accepted:** [H] hive_load apply pass mutates the live tree with no rollback (validate-before-apply added by §8, but partial mutations still possible on apply-fail/race) -> XREF: 02-kernel-core/TODO-14 §14 (item: "reload transactionality" at the §14 checklist)
> **Quality reviewed:** 2026-07-02 | Codex 3x (adversarial, consistency, perf) | 0 fixed, 1Crit+1M deferred, 2H+1M accepted-XREF | scope: kernel-code-quality


---

## 16. Token Open, Query, and Adjust Syscalls

> [!NOTE]
> Token implementation exists in `src/kernel/security/token.c` (→ XREF TODO-15 §7). This section wires the "operate on an existing token" surface (open, query, set, adjust) plus `NtAllocateLocallyUniqueId` into the SSDT. Token creation/derivation and SRM access check are in §29.

- [x] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` → SSDT 0x00B0 (resolves process handle, returns task->token via `NtOpenProcessToken` library)
- [x] `NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle)` → SSDT 0x00B1 (HandleAttributes currently ignored; inherit/audit flags tracked in TODO-05 §2)
- [x] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` → SSDT 0x00B2 (falls back to owning task's primary token; thread impersonation slot pending → XREF: 02-kernel-core/TODO-15 §2)
- [x] `NtOpenThreadTokenEx(ThreadHandle, DesiredAccess, OpenAsSelf, HandleAttributes, TokenHandle)` → SSDT 0x00B3
- [x] `NtQueryInformationToken(TokenHandle, Class, Buffer, Length, ReturnLength)` → SSDT 0x00B4 (13 info classes via library; TokenSource is enum-defined but not yet wired -- falls to STATUS_INVALID_INFO_CLASS)
- [x] `NtSetInformationToken(TokenHandle, Class, Buffer, Length)` → SSDT 0x00B5 (all classes currently return STATUS_INVALID_INFO_CLASS; TokenIntegrityLevel + ownership-transfer setters deferred → XREF: 02-kernel-core/TODO-15 §7)
- [x] `NtAdjustPrivilegesToken(TokenHandle, DisableAllPrivileges, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B6 (unpacks TOKEN_PRIVILEGES, delegates to library)
- [x] `NtAdjustGroupsToken(TokenHandle, ResetToDefault, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B7 (unpacks TOKEN_GROUPS, delegates to library)
- [x] `NtAllocateLocallyUniqueId(Luid)` → SSDT 0x00C3 (SSDT wrapper around `NtAllocateLocallyUniqueId` in luid.c)
- [ ] NtAdjustPrivileges/GroupsToken mutate the token before proving PreviousState holds the required old-state entries, so an undersized buffer loses rollback state. Pre-scan the required count and return STATUS_BUFFER_TOO_SMALL without mutating.
- [x] Commit: `"kernel: nt -- token open/query/adjust syscalls"` (d692c058)

**Test checkpoint:** `NtOpenProcessToken` returns valid token handle. `NtQueryInformationToken(TokenUser)` returns correct SID. `NtAdjustPrivilegesToken` enables/disables a privilege. `NtAllocateLocallyUniqueId` returns monotonically increasing LUIDs.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security) | 1010 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 9 token syscall handlers in nt_token.c wired to SSDT 0x00B0-0x00B7 + 0x00C3; thin wrappers over the token.c library + luid.c.
> - Hardened this review: fixed a Critical PreviousState capacity over-count + the count-field overwrite (offsetof sizing), added a NULL-output guard in NtQueryInformationToken, corrected stale claims (13 classes; NtSetInformationToken rejects all).
> - Scope boundary: query marshalling -> TODO-15 §4; deep-copy setters/per-token lock -> TODO-15; PID/TID handles -> TODO-05 §9; granted_access -> TODO-15 §8; HandleAttributes -> TODO-05 §2; probes -> §6/§29.
> - Test gap: token-adjust success paths + PreviousState size edge cases (0/header-only/one-entry) + valid-class query positive paths are untested.
> **Verified:** 2026-07-02 | commit `7183b1bd` | 9/10 items | build OK | security 1010/1010 PASS
> **Deferred:** [H] NtAdjustPrivileges/GroupsToken mutate the token before proving PreviousState can hold the old-state entries, so an undersized buffer loses rollback state -> XREF: 02-kernel-core/TODO-12 §16 (item: "NtAdjustPrivileges/GroupsToken mutate the token before proving PreviousState" at line 758)
> **Accepted:** [H] token queries return kernel-owned SID pointers + TokenGroups uses the wrong header offset (unusable ABI / kernel-layout leak) -> XREF: 02-kernel-core/TODO-15 §4 (item: "TokenXxx query marshalling" at line 331)
> **Quality reviewed:** 2026-07-02 | Codex 4x (adversarial, consistency, perf, re-adversarial) | 1Crit+2H fixed, 1H accepted-XREF, 1H deferred | scope: kernel-code-quality


---

## 17. Directory and Symbolic Link Object Syscalls
Namespace manipulation -- create, open, and query Ob directory objects and symbolic links from user mode.

- [x] `NtCreateDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0120 (nt_namespace.c -- ob_ns_create_directory + ObInsertObject + ObpAllocateHandle; rejects duplicate via ObLookupObjectByName)
- [x] `NtOpenDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0121 (nt_namespace.c -- thin wrapper around library NtOpenDirectoryObject in ob.c)
- [x] `NtQueryDirectoryObject(DirHandle, Buffer, Length, SingleEntry, Restart, Context, ReturnLength)` → SSDT 0x0122 (nt_namespace.c -- a4 packs Restart<<8|SingleEntry; a5=Context*, a6=ReturnLength*)
- [x] `NtCreateSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes, LinkTarget)` → SSDT 0x0123 (nt_namespace.c -- ob_ns_create_symlink + ObInsertObject; bounded target length)
- [x] `NtOpenSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0124 (nt_namespace.c -- ObLookupObjectByName with ObpSymlinkType to stop at the link itself)
- [x] `NtQuerySymbolicLinkObject(LinkHandle, LinkTarget, ReturnedLength)` → SSDT 0x0125 (nt_namespace.c -- ObpLookupHandle + type-check against ObpSymlinkType + bounded copy of target string)
- [ ] `NtQueryDirectoryObject` returns the legacy 96-byte ASCII OBJECT_DIRECTORY_INFORMATION row, not the native UNICODE_STRING ABI; ntdll callers get bogus data. Define the native struct + byte ReturnLength. XREF: TODO-14 §5.
- [ ] `oa_name` (nt_namespace.c) ignores OBJECT_ATTRIBUTES.RootDirectory, so relative object names resolve as absolute/wrong namespace. Honor RootDirectory: reference the dir handle + resolve ObjectName relative to it.
- [x] Commit: `"kernel: nt -- directory and symbolic link object syscalls"` (6052c7aa)

**Test checkpoint:** `NtCreateDirectoryObject` creates `\Test`; `NtOpenDirectoryObject` opens it. `NtCreateSymbolicLinkObject` creates `\TestLink → \Test`; `NtQuerySymbolicLinkObject` returns `\Test`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 6 namespace handlers (create/open/query directory + symbolic link) in nt_namespace.c wired to SSDT 0x0120-0x0125 via the OB namespace library.
> - Integrates: atomic create-rollback (handle-alloc-before-insert, OB_FLAG_PERMANENT clear); query handlers ObpLookupHandle + type-check; symlink depth guard OB_SYMLINK_DEPTH=8.
> - Reviewed this pass: handler wiring correct; the trust-boundary + UAF + native-ABI gaps are systemic/infrastructure, filed to their owners; corrected the stale a6-packing doc.
> - Scope boundary: missing probes -> §29 probe list; lookup-then-ref UAF -> TODO-05 §3; native directory ABI + RootDirectory -> §17; UTF-16 decode -> TODO-14 §5.
> - Test gap: ReturnSingleEntry/RestartScan packed-flag paths + relative-name (RootDirectory) create/open are untested.
> **Verified:** 2026-07-02 | commit `517cb921` | 6/8 items | build OK | ob 426/426 PASS
> **Accepted:** [Critical] handlers deref raw user pointers with no ProbeFor*IfUser (systemic NT trust boundary) -> XREF: 02-kernel-core/TODO-12 §29 (item: "Namespace + token syscalls deref raw user pointers" at line 1109)
> **Accepted:** [Critical] NtQuerySymbolicLinkObject reads an unpinned ObpLookupHandle object (concurrent NtClose UAF) -> XREF: 02-kernel-core/TODO-05 §3 (item: "Add `ObpReferenceObjectByHandle(table, handle, required_type, required_access, out_body, out_granted)` primitive" at line 148)
> **Accepted:** [H] UNICODE_STRING.Buffer cast to const char* (ASCII assumption) -> XREF: 02-kernel-core/TODO-14 §5 (item: "UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)" at line 291)
> **Deferred:** [H] NtQueryDirectoryObject returns the legacy 96-byte ASCII row, not the native UNICODE_STRING ABI -> XREF: 02-kernel-core/TODO-12 §17 (item: "`NtQueryDirectoryObject` returns the legacy 96-byte ASCII" at line 786)
> **Deferred:** [H] oa_name ignores OBJECT_ATTRIBUTES.RootDirectory (relative names resolve as absolute) -> XREF: 02-kernel-core/TODO-12 §17 (item: "`oa_name` (nt_namespace.c) ignores OBJECT_ATTRIBUTES.RootDirectory" at line 787)
> **Quality reviewed:** 2026-07-02 | Codex 3x (adversarial, consistency, perf) | 0 fixed, 2Crit+1H accepted-XREF, 2H deferred | scope: kernel-code-quality


---

## 18. Section and Memory-Mapped File Syscalls

> [!NOTE]
> Section (shared memory) object type implemented in TODO-05 §7 (`ob_section.c`). SSDT wiring lives in `src/kernel/nt/nt_section.c` (registers 0x005C-0x0062 after `nt_memory_register_ssdt()`). `NtCreateSection` packs `SectionPageProtection` (low 32) and `AllocationAttributes` (high 32) in SSDT arg `a5`; optional `NT_MAPVIEW_ARGS` at `a5` extends `NtMapViewOfSection` beyond the first four register slots.

- [x] `NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle)` → SSDT 0x005C:
  - `AllocationAttributes`: `SEC_COMMIT = 0x8000000`, `SEC_RESERVE = 0x4000000`, `SEC_IMAGE = 0x1000000`, `SEC_NOCACHE = 0x10000000`
  - If `FileHandle` non-NULL: file-backed section; otherwise page-file-backed
- [x] `NtOpenSection(SectionHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x005D
- [x] `NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect)` → SSDT 0x005E
- [x] `NtUnmapViewOfSection(ProcessHandle, BaseAddress)` → SSDT 0x005F
- [x] `NtExtendSection(SectionHandle, NewMaximumSize)` → SSDT 0x0060
- [x] `NtQuerySection(SectionHandle, SectionInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0061:
  - `SectionBasicInformation (0)`: base address, size, attributes
  - `SectionImageInformation (1)`: entry point, image base, stack info (for PE sections)
- [x] `NtAreMappedFilesTheSame(File1MappedAsAnImage, File2MappedAsFile)` → SSDT 0x0062
- [ ] `ObUnmapViewOfSectionByBase` scans only live section handles, so closing the section handle before `NtUnmapViewOfSection` leaks the view/pages. Add a handle-independent per-task view index ((pid,base) -> referenced SECTION_OBJECT).
- [ ] `NtMapViewOfSection` parses `Win32Protect` but `map_user_page_impl` (vmm.c) hardcodes RW+User PTE flags, so PAGE_READONLY sections map writable (no W^X). Pass section protection into `vmm_share_user_page`.
- [ ] `ObMapViewOfSectionFull` holds the irqsave section lock across the per-page `vmm_share_user_page` loop (interrupts off, O(pages)). Snapshot view state under the lock, drop it for VMM work, reacquire to publish/rollback.
- [x] Commit: `"kernel: nt -- section and memory-mapped file syscalls"`

**Test checkpoint:** `NtCreateSection` with `SEC_COMMIT` creates pagefile-backed section. `NtMapViewOfSection` maps into current process; write/read round-trip. `NtUnmapViewOfSection` unmaps. File-backed section maps file contents correctly.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 7 section/mmap handlers (create/open/map/unmap/extend/query/same) in nt_section.c wired to SSDT 0x005C-0x0062; anon + file-backed sections, per-PID view tracking, 11 ProbeFor sites.
> - Reviewed this pass: probing/sizing solid; mmap lifetime + protection gaps need per-process page tables + a handle-independent view index (infrastructure); OS Comparison row downgraded to in-progress.
> - Scope boundary: Win32Protect + section-lock + unmap-after-close -> new §18 items; the 7 pre-existing accepts (10-param ABI, cross-proc, SEC_RESERVE, SectionImageInfo, ObpLookupHandle UAF, granted_access, O(n) scans) stay valid below.
> - Test gap: Win32Protect enforcement + unmap-after-close + NtAreMappedFilesTheSame untested.

> **Codex (2026-04-14):** Fixed page-count overflow (64-bit math + `section_page_count_from_size`), per-section `spinlock_t` for views/size/phys, `oa_probe_ascii_name` for user `OBJECT_ATTRIBUTES`, NULL buffer guard in `ObQuerySectionObject`, file-backed create uses single validated `FILE_OBJECT` for `vfs_read` (no second lookup), `ObExtendSection` double-checked backing swap under lock.
> **Verified:** 2026-07-02 | commit `b82c51de` | 7/10 items | build OK | ob 426/426 PASS
> **Deferred:** [H] Win32Protect parsed but never applied; map_user_page_impl hardcodes RW+User so PAGE_READONLY sections map writable (no W^X) -> XREF: 02-kernel-core/TODO-12 §18 (item: "`NtMapViewOfSection` parses `Win32Protect`" at line 827)
> **Deferred:** [H] closing the section handle makes its view un-unmappable (ObUnmapViewOfSectionByBase scans only live handles), leaking view/pages/VA -> XREF: 02-kernel-core/TODO-12 §18 (item: "`ObUnmapViewOfSectionByBase` scans only live section handles" at line 826)
> **Deferred:** [H] ObMapViewOfSectionFull holds the irqsave section lock across the per-page VMM loop (interrupts off) -> XREF: 02-kernel-core/TODO-12 §18 (item: "`ObMapViewOfSectionFull` holds the irqsave section lock" at line 828)
> **Quality reviewed:** 2026-07-02 | Codex 3x (adversarial, consistency, perf) | 0 fixed (OS Comparison row reconciled), 3H deferred, 7+1 accepted-XREF | scope: kernel-code-quality
> **Accepted:** (1) `NtMapViewOfSection` full 10-parameter Windows stack ABI not wired on INT 0x2E / fast syscall (only 4-5 GPR slots today); extended fields use optional `NT_MAPVIEW_ARGS*` at `a5` when non-NULL -> XREF: 02-kernel-core/TODO-12 §4 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+", names `syscall_handler_2e`/`syscall_entry.asm` to retrofit, deletes `NT_MAPVIEW_ARGS` + `NtCreateSection` packed-flags kludge at `nt_section.c:93-94`). (2) Cross-process map / other-process `NtUnmapViewOfSection` -> XREF: 03-memory-concurrency/TODO-04 §5 (item: "Retrofit `src/kernel/nt/nt_section.c` once per-process page tables exist", names both guards at `nt_section.c:182`/`235` to remove). (3) `SEC_RESERVE`-only sparse pagefile sections without physical pages -> XREF: 03-memory-concurrency/TODO-04 §5 (item: "Implement `SEC_RESERVE` sparse pagefile-backed sections", names `ObCreateSectionEx` pre-alloc behavior to convert to zero-page PTEs + demand fault). (4) `SectionImageInformation` full Windows layout + PE parse (entry point, stack) -> XREF: 02-kernel-core/TODO-08 §8 (item: `NtQuerySection(..., SectionImageInformation)` PE persist at line 210 -- names backing `SECTION_OBJECT`/`ob_section.c`/`nt_section.c` retrofit list). (5) Initial `ObpLookupHandle` UAF race in `nt_section.c` (body could be freed by concurrent `NtClose` on another thread of the same task between lookup and use) -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add `ObpReferenceObjectByHandle(...)` primitive", explicitly names retrofit list including all of `nt_section.c` and `ObUnmapViewOfSectionByBase` at `ob_section.c:414`). (6) Section handle `granted_access` not enforced per Windows rights model (`SECTION_MAP_READ/WRITE/EXECUTE`, `SECTION_EXTEND_SIZE`, `SECTION_QUERY`) -> XREF: 02-kernel-core/TODO-15 §8 (item: "Enforce per-handle `granted_access` on section syscalls", names required masks per handler and the `test_ob.c` negative test to add). (7) `ObUnmapViewOfSectionByBase` and `ObAreMappedFilesTheSame` O(handle_capacity × SECTION_MAX_VIEWS) scans on every call -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add per-task view-base index", names both functions and `(task_pid, base_addr)` key to add to `struct task`).


---

## 19. Timer Control Syscalls

> [!NOTE]
> Timer object type implemented in TODO-05 (`ob_timer.c`). Time source APIs (NtQuerySystemTime, etc.) implemented in TODO-08 §9; timer resolution APIs in TODO-08 §8. This section provides the full timer control surface and SSDT wiring. SSDT handlers live in `src/kernel/nt/nt_timer.c`. The armed-timer queue is a singly-linked list of `TIMER_OBJECT` bodies; `nt_timer_tick()` walks it from both `lapic_timer_handler` (LAPIC path) and `pit_tick_increment` (PIT/TCG path) on every tick under an irqsave spinlock, calling `event_set()` on any timer whose `due_ns` has been reached. Periodic timers self-rearm. `wait_on_handle` in `src/kernel/nt/nt_sync.c` dispatches `ObpTimerType` through the same event so `NtWaitForSingleObject(timer)` works. `NtSetTimer` takes 7 params; `Period` (low 32) and `ResumeTimer` (high 32) are packed into `a5` via `NT_SETTIMER_PACK()` until TODO-12 §4 extends the entry to read stack args.

- [x] `NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType)` → SSDT 0x007E:
  - `TimerType`: `NotificationTimer (0)` = manual-reset, `SynchronizationTimer (1)` = auto-reset
- [x] `NtOpenTimer(TimerHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x007F
- [x] `NtSetTimer(TimerHandle, DueTime, TimerApcRoutine, TimerContext, ResumeTimer, Period, PreviousState)` → SSDT 0x0080:
  - `DueTime`: negative = relative (100ns units), positive = absolute FILETIME
  - `Period`: 0 = one-shot, >0 = periodic (milliseconds)
- [x] `NtCancelTimer(TimerHandle, CurrentState)` → SSDT 0x0081
- [x] `NtQueryTimer(TimerHandle, TimerInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0082
- [x] `NtSetTimerEx(TimerHandle, TimerSetInformationClass, Buffer, Length)` → SSDT 0x0083
- [x] `NtQuerySystemTime(SystemTime)` → SSDT 0x00F0 (→ XREF TODO-08 §9)
- [x] `NtSetSystemTime(SystemTime, PreviousTime)` → SSDT 0x00F1 (→ XREF TODO-08 §9)
- [x] `NtQueryPerformanceCounter(PerformanceCounter, PerformanceFrequency)` → SSDT 0x00F2 (→ XREF TODO-08 §9)
- [x] `NtQueryTimerResolution(MaximumTime, MinimumTime, CurrentTime)` → SSDT 0x00F3 (→ XREF TODO-08 §8)
- [x] `NtSetTimerResolution(DesiredTime, SetResolution, ActualTime)` → SSDT 0x00F4 (→ XREF TODO-08 §8)
- [ ] `nt_timer_tick` drops `s_armed_lock` before `event_set`, so a concurrent `NtSetTimer` re-arm in the gap lets the stale one-shot signal fire the new arm early. Add a per-arm generation captured before the signal-chain move.
- [ ] `compute_due_ns` treats positive absolute-FILETIME `DueTime` as fire-now; convert against `wall_clock_time_sourced`/KeQuerySystemTime with overflow-safe interval math so future deadlines delay.
- [ ] `NtSetTimerEx(TimerSetCoalescableTimer)` returns SUCCESS without reading DueTime/Period or calling `nt_timer_arm`, so the timer never fires. Parse the info struct + arm (ignore only the coalescing tolerance).
- [ ] Timer PreviousState/CurrentState/TimerState derive from `active` (armed) not the event signal state, so a fired one-shot / consumed sync timer reports wrong state. Base state on the event signal state.
- [ ] `NtQueryTimer` ReturnLength is `uint64_t*` (probes 8 bytes) but the Win ABI is `PULONG` (uint32_t); a 4-byte-ULONG caller can fault or overwrite the adjacent stack slot. Change to `uint32_t*` + 4-byte probe/write.
- [ ] `nt_timer_tick` walks EVERY armed timer per tick under `s_armed_lock` irqsave (O(total armed), not O(expired)) and signals periodic timers under the lock. Add a timing wheel/heap + move periodic signalling to the unlocked phase.
- [ ] `nt_timer_tick` Phase B calls `ObDereferenceObject` from the tick ISR; the final one-shot deref can run `timer_on_delete` -> `kfree` from ISR context (IRQL violation). Queue the final deref to DPC/thread context.
- [x] Commit: `"kernel: nt -- timer control and time query syscalls"`

**Test checkpoint:** `NtCreateTimer` + `NtSetTimer` with relative 100ms due time fires. `NtCancelTimer` cancels before fire returns `STATUS_SUCCESS`. `NtQueryPerformanceCounter` returns monotonically increasing value. `NtQueryTimerResolution` reports correct LAPIC timer resolution.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 6 timer handlers (create/open/set/cancel/query/setEx) in nt_timer.c wired to SSDT 0x007E-0x0083; two-phase tick, 11 ProbeFor sites; 5 time-query syscalls are TODO-08-owned.
> - Reviewed this pass: probing solid; the timer state machine is under-hardened -- 6 new blockers (rearm race, absolute-FILETIME, SetTimerEx no-op, state-from-signal, ReturnLength ABI, ISR-free IRQL) filed as §18-style items.
> - Scope boundary: 6 new blockers -> §19 items 870-876; flat-list timing-wheel -> TODO-05 §6; ObpLookupHandle UAF -> TODO-05 §2; oa NUL-term -> TODO-31 §14; args 5+ ring-3 -> §3.
> - Test gap: no deterministic fire-via-tick test; NtSetTimerEx + absolute-time + rearm-race + state-of-fired-timer untested.

> **Codex adversarial (2026-04-14):** Fixed `compute_due_ns` integer overflow with saturating 100-ns->ns conversion + saturated `now + rel_ns` add (cap `0x7FFFFFFFFFFFFFFF`); fixed `nt_timer_arm` missed-fire race by moving `event_reset()` BEFORE the armed-list insert (inside `s_armed_lock`) so any post-arm `event_set()` survives until consumed.
> **Verified:** 2026-07-02 | commit `91ce552c` | 11/18 items | build OK | ob 426/426 PASS
> **Deferred:** [H] nt_timer_tick drops s_armed_lock before event_set, so a concurrent NtSetTimer re-arm lets the stale one-shot signal fire the new arm early -> XREF: 02-kernel-core/TODO-12 §19 (item: "`nt_timer_tick` drops `s_armed_lock` before `event_set`" at line 870)
> **Deferred:** [H] compute_due_ns treats positive absolute-FILETIME DueTime as fire-now (immediate signal for future deadlines) -> XREF: 02-kernel-core/TODO-12 §19 (item: "`compute_due_ns` treats positive absolute-FILETIME" at line 871)
> **Deferred:** [H] NtSetTimerEx(TimerSetCoalescableTimer) returns SUCCESS without arming the timer, so it never fires -> XREF: 02-kernel-core/TODO-12 §19 (item: "`NtSetTimerEx(TimerSetCoalescableTimer)` returns SUCCESS" at line 872)
> **Deferred:** [H] nt_timer_tick Phase B calls ObDereferenceObject from the tick ISR; the final one-shot deref can run kfree from ISR context (IRQL violation) -> XREF: 02-kernel-core/TODO-12 §19 (item: "`nt_timer_tick` Phase B calls `ObDereferenceObject`" at line 876)
> **Quality reviewed:** 2026-07-02 | Codex 3x (adversarial, consistency, perf) | 0 fixed (OS Comparison row reconciled), 6H+1M deferred, 3 accepted-XREF | scope: kernel-code-quality
> **Accepted:** (1) `UNICODE_STRING.Buffer` in `oa_probe_ascii_name` is returned as a raw pointer and consumed via `snprintf("%s", name)` in `ObCreateTimerEx`/`ObOpenTimer`, risking an overread if the buffer has no NUL within probed bytes (same codebase-wide pattern as `nt_section.c`/`nt_namespace.c`) -> XREF: 02-kernel-core/TODO-31 §14 (item: "UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)" at line 246 -- retrofit list explicitly names `nt_timer.c::oa_probe_ascii_name` and the ASCII path construction in `ObCreateTimerEx`/`ObOpenTimer`). (2) Initial `ObpLookupHandle` UAF race in `nt_timer.c::resolve_timer_handle` (body could be freed by concurrent `NtClose` on another thread of the same task between lookup and use) -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add `ObpReferenceObjectByHandle(...)` primitive" at line 112 -- retrofit list now explicitly names `nt_timer.c` and enumerates the four consumer handlers). (3) Flat armed-list linear scan per tick (tens of timers OK today, O(N * ticks) at scale) -> XREF: 02-kernel-core/TODO-05 §6 (item: "Replace the flat NT timer armed list with an ordered structure (min-heap or timing wheel)" -- names `nt_timer.c::s_armed_head`, suggests min-heap or timing wheel, requires 1024-timer stress test).

---

## 20. Legacy LPC Port Syscalls

> [!NOTE]
> Legacy LPC (NT 3.x-5.x) syscall surface. Reserves SSDT indices 0x0100-0x010E (15 slots) and wires NT-compatible signatures so user-mode callers get `STATUS_NOT_IMPLEMENTED` (rather than an empty SSDT slot) until the LPC engine lands. All 15 handlers are deliberate stubs (`LPC_STUB_BODY`); the functional retrofit is TODO-09 §7 (item "Retrofit the 15 LPC SSDT stubs"). Full port infrastructure -- `ALPC_PORT` object, message queue, connection state machine, send/receive engine -- is implemented in `02-kernel-core/TODO-24-alpc-message-ports.md`. Modern ALPC (Vista+) syscalls split to §31. LPC and ALPC share the same underlying kernel object type (`ObpAlpcPortType`); LPC is a thin compatibility adapter over ALPC in Windows and will be the same here.

- [x] `NtCreatePort(PortHandle, ObjectAttributes, MaxConnectionInfoLength, MaxMessageLength, MaxPoolUsage)` → SSDT 0x0100
- [x] `NtCreateWaitablePort(...)` → SSDT 0x0101
- [x] `NtConnectPort(PortHandle, PortName, SecurityQos, ClientView, ServerView, MaxMessageLength, ConnectionInformation, ConnectionInformationLength)` → SSDT 0x0102
- [x] `NtSecureConnectPort(...)` → SSDT 0x0103: with SID validation
- [x] `NtAcceptConnectPort(PortHandle, PortContext, ConnectionRequest, AcceptConnection, ServerView, ClientView)` → SSDT 0x0104
- [x] `NtCompleteConnectPort(PortHandle)` → SSDT 0x0105
- [x] `NtListenPort(PortHandle, ConnectionRequest)` → SSDT 0x0106
- [x] `NtReplyPort(PortHandle, ReplyMessage)` → SSDT 0x0107
- [x] `NtReplyWaitReceivePort(PortHandle, PortContext, ReplyMessage, ReceiveMessage)` → SSDT 0x0108
- [x] `NtReplyWaitReceivePortEx(PortHandle, PortContext, ReplyMessage, ReceiveMessage, Timeout)` → SSDT 0x0109
- [x] `NtRequestPort(PortHandle, RequestMessage)` → SSDT 0x010A
- [x] `NtRequestWaitReplyPort(PortHandle, RequestMessage, ReplyMessage)` → SSDT 0x010B
- [x] `NtImpersonateClientOfPort(PortHandle, Message)` → SSDT 0x010C
- [x] `NtReadRequestData(PortHandle, Message, DataEntryIndex, Buffer, BufferSize, BytesRead)` → SSDT 0x010D
- [x] `NtWriteRequestData(PortHandle, Message, DataEntryIndex, Buffer, BufferSize, BytesWritten)` → SSDT 0x010E
- [x] Commit: `"kernel: nt -- legacy LPC port syscalls wired to SSDT"`

**Test checkpoint:** All 15 LPC SSDT slots resolve to the registered handler (not the default `ssdt_stub_not_implemented`). Each handler returns `STATUS_NOT_IMPLEMENTED` until TODO-24 §8 LPC engine lands; functional round-trip (`NtCreatePort` + `NtConnectPort` + `NtRequestWaitReplyPort`) is exercised by TODO-24 §8 tests and TODO-24 §8-§9 tests.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob) | 426 kernel + 16 user tests, 0 failures
> **Notes:**
> - Shipped: 15 legacy LPC SSDT stubs (0x0100-0x010E) in nt_lpc.c via LPC_STUB_BODY (returns STATUS_NOT_IMPLEMENTED); registration is bounds-checked + boot-fatal.
> - Reviewed this pass: all 15 are honest deferred stubs (adversarial found no exploitable surface, perf approved); the engine retrofit is a concrete owner item enumerating every slot.
> - Scope boundary: the whole LPC engine (port objects, connection state machine, request/reply, SeAccessCheck, impersonation, Read/WriteRequestData) -> TODO-09 §7; IO + Master Table rows are [/] (stub-wired, not functional).
> - Test gap: none for stubs -- test_nt_lpc_pending_features (TEST_PENDING) asserts all 15 return NOT_IMPLEMENTED. Metadata follow-up: TODO-A owner cells + service_numbers next-avail still name stale T17.

> **Verified:** 2026-07-02 | commit `645afa47` | 15/15 items | build OK | ob 426/426 PASS (stub-wired)
> **Deferred:** [M] all 15 LPC syscalls are deliberate stubs (STATUS_NOT_IMPLEMENTED); the whole port/message engine + SeAccessCheck/impersonation is deferred -> XREF: 03-memory-concurrency/TODO-09 §7 (item: "Retrofit the 15 LPC SSDT stubs in `src/kernel/nt/nt_lpc.c`" at line 185)
> **Quality reviewed:** 2026-07-02 | Codex 3x (adversarial, consistency, perf) | 0 fixed (note range + IO/status + owner XREF reconciled), 1M deferred | scope: kernel-code-quality

---

## 21. Exception and Debug Syscalls

> [!NOTE]
> Exception dispatch implemented in TODO-23-exception-dispatch-seh.md §5. Debug infrastructure in TODO-29-kernel-debugger-kd-protocol.md §13. This section reserves SSDT indices and defines the NT-compatible signatures.

- [ ] `NtRaiseException(ExceptionRecord, ContextRecord, FirstChance)` → SSDT 0x0130 (→ XREF TODO-23 §5):
  - Delivers exception to the structured exception handler chain
- [ ] `NtContinue(ContextRecord, RaiseAlert)` → SSDT 0x0131 (→ XREF TODO-23 §5):
  - Resume execution from exception with modified context
- [ ] `NtCreateDebugObject(DebugObjectHandle, DesiredAccess, ObjectAttributes, Flags)` → SSDT 0x0135
- [ ] `NtDebugActiveProcess(ProcessHandle, DebugObjectHandle)` → SSDT 0x0132:
  - Attach debug object to process; all exceptions route to debugger first
- [ ] `NtRemoveProcessDebug(ProcessHandle, DebugObjectHandle)` → SSDT 0x0134:
  - Detach debugger from process
- [ ] `NtWaitForDebugEvent(DebugObjectHandle, Alertable, Timeout, WaitStateChange)` → SSDT 0x0136:
  - Wait for next debug event (breakpoint, exception, thread create/exit, process exit, module load)
- [ ] `NtDebugContinue(DebugObjectHandle, ClientId, ContinueStatus)` → SSDT 0x0133:
  - Continue after debug event with `DBG_CONTINUE` or `DBG_EXCEPTION_NOT_HANDLED`
- [ ] `NtSetInformationDebugObject(DebugObjectHandle, DebugObjectInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0137
- [ ] Commit: `"kernel: nt -- exception and debug syscalls"`

**Test checkpoint:** `NtRaiseException` with `STATUS_BREAKPOINT` reaches SEH handler. `NtCreateDebugObject` + `NtDebugActiveProcess` on a child process captures breakpoint events via `NtWaitForDebugEvent`. `NtDebugContinue(DBG_CONTINUE)` resumes the debuggee.

> **Test runner:** N/A (reservation-only section -- no handler code) | validation: owner sections TODO-23 §5 + TODO-29 §13 carry the functional tests

> **Notes:**
> - Reservation-only section: reserves SSDT indices 0x0130-0x0137 and pins the NT-compatible signatures; no handler code lands here by design (see section NOTE callout).
> - Indices reserved in `include/kernel/nt/service_numbers.h:279-286` (`SSDT_NtRaiseException` through `SSDT_NtSetInformationDebugObject`).
> - Exception delivery (`NtRaiseException`/`NtContinue`) is owned by TODO-23 §5; the six debug-object syscalls are owned by TODO-29 §13.
> - Scope boundary: functional handlers, registration, and unit tests belong to the two owner sections, not here.
> **Deferred:** [M] `NtRaiseException`/`NtContinue` handlers unimplemented; §21 reserves the SSDT slots + signatures only -> XREF: 02-kernel-core/TODO-23-exception-dispatch-seh.md §5 (items: "`NtRaiseException` SSDT entry -- calls `ki_dispatch_exception()`" at line 213, "`NtContinue` SSDT entry -- restore CONTEXT, resume user-mode" at line 214)
> **Deferred:** [M] Six debug-object syscalls unimplemented (NtCreateDebugObject/NtDebugActiveProcess/NtRemoveProcessDebug/NtWaitForDebugEvent/NtDebugContinue/NtSetInformationDebugObject) -> XREF: 02-kernel-core/TODO-29-kernel-debugger-kd-protocol.md §13 (items: "`NtCreateDebugObject` ... register as ObpDebugType" at line 473 through "`NtSetInformationDebugObject`" at line 478)

---

## 22. Power and System Control

> [!NOTE]
> Power management implementation in TODO-26-power-management.md. This section provides the SSDT wiring for power and system control syscalls.

- [ ] `NtShutdownSystem(Action)` → SSDT 0x00D7:
  - `ShutdownNoReboot (0)`, `ShutdownReboot (1)`, `ShutdownPowerOff (2)`
  - Requires `SeShutdownPrivilege`
- [ ] `NtSetSystemPowerState(SystemAction, LightestSystemState, Flags)` → SSDT 0x0140:
  - `SystemAction`: `PowerActionSleep`, `PowerActionHibernate`, `PowerActionShutdown`
  - Routes through ACPI power management (→ XREF TODO-26)
- [ ] `NtInitiatePowerAction(SystemAction, LightestSystemState, Flags, Asynchronous)` → SSDT 0x0141
- [ ] `NtPowerInformation(InformationLevel, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength)` → SSDT 0x0142:
  - `SystemBatteryState (5)`: battery level, AC power
  - `ProcessorInformation (11)`: frequency, max frequency
  - `SystemPowerCapabilities (4)`: S-state support, lid switch
- [ ] `NtGetDevicePowerState(Device, State)` → SSDT 0x0143
- [ ] `NtSetThreadExecutionState(NewFlags, PreviousFlags)` → SSDT 0x0144:
  - `ES_SYSTEM_REQUIRED`, `ES_DISPLAY_REQUIRED`, `ES_CONTINUOUS`
  - Prevents sleep/screen-off while active
- [ ] Commit: `"kernel: nt -- power and system control syscalls"`

**Test checkpoint:** `NtShutdownSystem(ShutdownReboot)` triggers ACPI reset. `NtPowerInformation(SystemPowerCapabilities)` returns valid S-state support mask. `NtSetThreadExecutionState` prevents idle sleep during long operation.

> **Test runner:** N/A (SSDT-wiring section gated on TODO-26) | validation: `NtShutdownSystem` reboot path verified via serial log; remaining surface tested by TODO-26 §20

> **Notes:**
> - SSDT-wiring section: the ACPI power subsystem (S-state transitions, battery/thermal state, device D-states, idle governor) is owned by TODO-26; this section only wires the NtXxx entry points once that infrastructure lands.
> - Self-contained piece already shipped: `NtShutdownSystem` (SSDT 0x00D7) is implemented + registered in `src/kernel/nt/nt_syscall.c:1188` (reboot/power-off via SYS_REBOOT/SYS_SHUTDOWN).
> - Remaining 6 syscalls (NtSetSystemPowerState/NtInitiatePowerAction/NtPowerInformation/NtGetDevicePowerState/NtSetThreadExecutionState/NtRequestWakeupLatency) all route through `pm_*` helpers + ACPI S-state code that does not exist yet.
> - Scope boundary: the power handlers, their `pm_*` backends, and unit tests belong to TODO-26 §20, not here.
> **Deferred:** [M] Six power-management syscalls unimplemented; the ACPI S-state/battery/device-power backend they call is owned elsewhere (only `NtShutdownSystem` is self-contained and already wired) -> XREF: 02-kernel-core/TODO-26-power-management.md §20 (item: "`NtSetSystemPowerState` ... route through ACPI S-state transition" at line 773 through "`NtRequestWakeupLatency`" at line 778)

---

## 23. Atom, Locale, and Miscellaneous Syscalls
Catch-all for global atom table, locale management, environment variables, and display/error APIs.

- [x] `NtAddAtom` → SSDT 0x00DF: `nt_atom_add` in `nt_misc.c`; refcounted global string atom, ID from 0xC000; integer atoms (name ptr < 0xC000) pass through
- [x] `NtFindAtom` → SSDT 0x00E0: `nt_atom_find`; case-insensitive lookup, no refcount change; `STATUS_OBJECT_NAME_NOT_FOUND` when absent
- [x] `NtDeleteAtom` → SSDT 0x00E1: `nt_atom_delete`; decrements refcount, frees slot at zero; no-op success for integer atoms
- [x] `NtQueryInformationAtom` → SSDT 0x00E2: `AtomBasicInformation` fills `ATOM_BASIC_INFORMATION` (usage/flags/name) with `ReturnLength` + `STATUS_BUFFER_TOO_SMALL`
- [x] `NtQueryDefaultLocale(UserProfile, DefaultLocaleId)` → SSDT 0x00DA: returns global LCID (`UserProfile` ignored -- no per-user hive yet)
- [x] `NtSetDefaultLocale(UserProfile, DefaultLocaleId)` → SSDT 0x00DB: fails closed with `STATUS_PRIVILEGE_NOT_HELD` (machine-wide locale write is privileged; per-user hive owned by TODO-13 §6)
- [x] `NtQueryDefaultUILanguage(DefaultUILanguageId)` → SSDT 0x00DC: returns global UI LANGID
- [x] `NtSetDefaultUILanguage(DefaultUILanguageId)` → SSDT 0x00DD: fails closed with `STATUS_PRIVILEGE_NOT_HELD` (same privilege gate)
- [x] `NtQueryInstallUILanguage(InstallUILanguageId)` → SSDT 0x00DE: returns immutable install LANGID (0x0409)
- [x] `NtQuerySystemEnvironmentValue(VariableName, VariableValue, ValueLength, ReturnLength)` → SSDT 0x00D2: UEFI runtime variable access
- [x] `NtSetSystemEnvironmentValue(VariableName, VariableValue)` → SSDT 0x00D3
- [x] `NtDisplayString(String)` → SSDT 0x00D8: TCB-gated (`ASSERT_KERNEL_CALLER`); kernel callers marshal PUNICODE_STRING, UTF-16→ASCII to `klog`; user-mode → `STATUS_PRIVILEGE_NOT_HELD`
- [x] `NtRaiseHardError(...)` → SSDT 0x00D9: `OptionShutdownSystem` → `STATUS_PRIVILEGE_NOT_HELD`; kernel-mode callers log, user-mode returns `ResponseNotHandled` without touching the shared log
- [x] Commit: `"kernel: nt -- atom table, locale, environment, misc syscalls"`

**Test checkpoint:** `NtAddAtom("TestAtom")` returns atom ID > 0. `NtFindAtom("TestAtom")` returns same ID. `NtDeleteAtom` removes it; subsequent `NtFindAtom` returns `STATUS_OBJECT_NAME_NOT_FOUND`. `NtQueryDefaultLocale` returns valid LCID.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 9 suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/nt/nt_misc.c` + `include/kernel/nt/nt_misc.h`: global atom table (refcounted, 512-slot, IDs 0xC000+) plus locale/UI-language storage, `NtDisplayString`, and `NtRaiseHardError`; 11 SSDT handlers at 0x00D8-0x00E2.
> - Registered via `nt_misc_register_ssdt()` in `boot_desktop.c`; all user pointers go through `ProbeFor{Read,Write}IfUser` + `copy_{from,to}_user`; integer atoms (name ptr < 0xC000) never dereferenced.
> - Atom/locale logic is exposed as pure `nt_atom_*`/`nt_locale_*` helpers so `test_nt_misc.c` (TEST_CAT_ABI, 9 suites) exercises it without the syscall path.
> - Setters/privileged ops fail closed: locale setters + NtDisplayString + NtRaiseHardError shutdown return `STATUS_PRIVILEGE_NOT_HELD` for unprivileged callers.
> - Scope boundary: full Unicode case-folding, per-user locale hives, the production hash-indexed atom table, copy_*_user fault-recovery, and >4-arg (a5/a6) stack marshalling are owned elsewhere (see Accepted stamps).
> **Verified:** 2026-07-03 | commit `ce9d7387` | 14/14 items | build OK | tests 974 kernel + 16 user PASS
> **Accepted:** [M] copy_*_user is not fault-recoverable (ProbeFor* validates range not page-presence; an in-range-unmapped user page faults in kernel) -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Audit all syscall handlers: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()`" at line 126 -- nt_misc.c added to retrofit list)
> **Accepted:** [M] NtRaiseHardError Response/OptionShutdownSystem + NtQueryInformationAtom ReturnLength need >4-arg stack marshalling (entry path drops a5/a6) -> XREF: 02-kernel-core/TODO-12 §3 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+" at line 245)
> **Accepted:** [M] atom lookup holds the irqsave `s_atom_lock` across an O(512x255) name scan (unprivileged full-table miss -> ~130us IRQ-disabled) -> XREF: 02-kernel-core/TODO-13-atom-nls-locale-subsystem.md §3 (item: "Bound the locked atom lookup ... add a per-slot case-folded hash / bucket index or a thread-context mutex")
> **Quality reviewed:** 2026-07-03 | Codex 16x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 3H+10M fixed, 3M accepted-XREF | scope: kernel-code-quality

---

## 24. Syscall Audit and Tracing Hook

> [!IMPORTANT]
> **Impossible OS exclusive feature.** Windows uses ETW (heavyweight, complex configuration). Linux uses seccomp-bpf (complex BPF programs) or strace (ptrace overhead). Impossible OS provides a first-class kernel API for syscall-level auditing with minimal overhead.

- [/] `NtRegisterSyscallAuditHook` → SSDT 0x0150: shipped as kernel C API `nt_audit_register` (`AUDIT_PRE`/`POST`/`BOTH`); raw-function-pointer SSDT syscall NOT exposed (unsafe pre-SMP-closed previous-mode) -> deferred to §29
  - `AuditRoutine(ServiceNumber, Args, Context, Phase, Status)` -- kernel context; a PRE routine returning non-`STATUS_SUCCESS` blocks the syscall
- [/] `NtUnregisterSyscallAuditHook` → SSDT 0x0151: shipped as kernel C API `nt_audit_unregister` (non-blocking try-semantics: `STATUS_UNSUCCESSFUL` while a dispatch is in flight, else clears the slot); SSDT exposure deferred to §29
- [x] `NtQuerySyscallAuditState(Buffer, Length, ReturnLength)` → SSDT 0x0152: `SYSCALL_AUDIT_STATE` (active count, per-hook handle/flags/invocations, denied); probe+copy + `ReturnLength`/`BUFFER_TOO_SMALL`; kernel-mode only (`ASSERT_KERNEL_CALLER`)
- [x] SSDT dispatcher integration: `ssdt_dispatch` runs one `nt_audit_begin`/`nt_audit_end` session (PRE before + POST after the handler); a non-`STATUS_SUCCESS` PRE blocks the handler; audited path out-of-line so the no-hook path is zero-cost
  - Fast path: relaxed atomic `nt_audit_hook_count()==0` -> zero overhead; hooks snapshot under `s_audit_lock` + `in_flight` refcount, invoked outside the lock; per-thread `in_audit` guard breaks recursion
- [x] Gate the syscall-path `transition_ring_record()` behind `g_transition_ring_active`: `cmp/jz` in `syscall_entry.asm` (both sites), default off; IDT/fault records stay on (panic forensics); `transition_ring_set_enabled()` toggles it. (§2 perf)
- [x] Registration is kernel-mode only (raw function-pointer `AuditRoutine` is untrusted from ring-3); ring-3-safe `SeAuditPrivilege` gating owned by §29 (no `SeSinglePrivilegeCheck` yet)
- [ ] Perf: hoist the `g_transition_ring_active` `cmp/jz` in `syscall_entry.asm` before the register save blocks so the off path skips the 4 push/pop pairs, not just the record.
- [ ] Perf: make the active audit dispatch read-mostly (RCU/seqlock snapshot + per-CPU padded `in_flight`) so an always-on hook does not serialize every syscall on the global `s_audit_lock`.
- [x] Commit: `"kernel: nt -- syscall audit and tracing hook (SSDT pre/post)"`

**Test checkpoint:** Register pre-call audit hook; every syscall logs service number to ring buffer. Register post-call hook; verify NTSTATUS is captured. Pre-call hook returning `STATUS_ACCESS_DENIED` blocks the syscall. Unregister hook; verify zero overhead (no measurable latency increase).

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 8 suites, 0 failures

> **Notes:**
> - Shipped `src/kernel/nt/nt_audit.c` + `include/kernel/nt/nt_audit.h`: 8-slot hook pool + begin/end session dispatch. Only `NtQuerySyscallAuditState` (0x0152, kernel-only) is SSDT-exposed; register/unregister are kernel C APIs.
> - Hot path: `ssdt_dispatch` reads `nt_audit_hook_count()` (relaxed atomic), zero cost when none registered; hooks are snapshotted under `s_audit_lock` (with a per-hook `in_flight` refcount) and invoked OUTSIDE it (so a hook may yield/block); a per-thread `thread->in_audit` guard breaks recursion; `nt_audit_unregister` refuses (`STATUS_UNSUCCESSFUL`) while `in_flight != 0`, else frees the slot -- context safe with no wait/wake primitive needed.
> - Syscall-path `transition_ring_record()` gated behind `g_transition_ring_active` (`cmp/jz` in `syscall_entry.asm`, default off); IDT/fault transition records stay on for panic forensics.
> - Security: registration takes a raw kernel function pointer, so it is NOT a ring-3 syscall (would be kernel-code execution if the non-SMP-closed previous-mode gate were raced); the kernel C API is the only registration path.
> - Scope boundary: covers the SSDT surface (SYSCALL + INT 0x2E), NOT legacy INT 0x80; ring-3-safe registration via a validated descriptor + `SeAuditPrivilege` is owned by §29.
> **Verified:** 2026-07-03 | commit `8537a1b8` | 4/8 items | build OK | tests 1008 kernel + 16 user PASS
> **Accepted:** [M] the `NtQuerySyscallAuditState` copy-out is not fault-recoverable (in-range unmapped user page faults in kernel) -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Audit all syscall handlers: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()`" at line 132)
> **Deferred:** [M] perf: syscall-entry off-path still does push/pop pairs, and an always-on hook serializes syscalls on the global `s_audit_lock` -> XREF: 02-kernel-core/TODO-12 §24 (items: "Perf: hoist the `g_transition_ring_active`..." + "Perf: make the active audit dispatch read-mostly...")
> **Quality reviewed:** 2026-07-03 | Codex 24x (design, adversarial, re-adversarial, consistency, perf, test-coverage) | 1Crit+5H+9M fixed, 1M accepted-XREF, 2M deferred | scope: kernel-code-quality

---

## 25. Per-Process Syscall Filtering

> [!NOTE]
> → XREF: `TODO-21-process-model-extensions.md §12` -- scope overlap: TODO-21 §12 adds pledge/unveil-style category-based restriction (`NtPledge`/`NtUnveil`). This section adds per-index bitmap filtering. Both run in the SSDT dispatcher; bitmap filter runs FIRST (per-index), then pledge category check. Both must pass for the syscall to proceed.

Per-process syscall restrictions allow a process to lock down which system services its children (or itself) can invoke. Windows has `PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY` (DisallowWin32kSystemCalls, DisallowFsctlSystemCalls) stored in EPROCESS MitigationFlags. Linux has seccomp-bpf with per-process BPF programs and constant-action bitmap caching. Impossible OS provides a first-class bitmap-based filter with optional BPF programs for argument inspection.

- [x] `SYSCALL_FILTER` in `include/kernel/nt/syscall_filter.h`: `allow_main[16]` + `allow_shadow[16]` allow bitmaps (1=allowed; sized on `SSDT_MAIN_MAX`/`SSDT_SHADOW_MAX`), `flags` INHERIT=1/LOCKED=2/AUDIT=4, `retired_prev` retire chain.
- [x] `struct task` gains `syscall_filter` (NULL=allow-all) + `syscall_filter_counted`; published/read via `__atomic` release/acquire, explicit NULL-reset at every create site (reused slot must not inherit a stale ptr).
- [x] `ProcessSystemCallFilterPolicy` (=41) on `NtSetInformationProcess`, restricted to self (probe+`copy_from_user`): `DisallowWin32kSystemCalls` / `DisallowFsctlSystemCalls` (0x001A) / `CustomBitmap`; LOCKED = subset-only tighten + frozen flags.
- [x] `ssdt_dispatch` filter check runs before the audit session + handler, gated by `syscall_filter_active()` (zero cost when no filter exists); blocked index returns `STATUS_ACCESS_DENIED`; KernelMode (Zw) callers bypass.
- [x] Inheritance: `NtCreateProcess` fail-closed pre-clone when `SYSCALL_FILTER_INHERIT` set; `task_fork` inherits unconditionally (seccomp) and fails closed on clone alloc-failure. (`NtCreateUserProcess` still a stub.)
- [x] Audit mode `SYSCALL_FILTER_AUDIT=4`: `syscall_filter_check` klogs the would-block and allows instead of denying.
- [x] Lifetime: immutable snapshots freed only at `task_cleanup` (reap barrier); count contribution dropped at every `TASK_DEAD` site via `syscall_filter_task_dead`; `syscall_filter_counted` single-decrement; generation cap 64.
- [x] Commit: `"kernel: nt -- per-process syscall filter bitmap (seccomp/SystemCallDisable parity)"`

**Test checkpoint:** A process filters itself (`CustomBitmap` clearing a service index); its own blocked syscall returns `STATUS_ACCESS_DENIED`, other syscalls still work. `DisallowWin32kSystemCalls` blocks shadow SSDT calls. Inheritance: a child created with `SYSCALL_FILTER_INHERIT` (or forked) inherits the filter; grandchild also blocked. Locked filter cannot be relaxed (`STATUS_ACCESS_DENIED`). Audit mode logs but does not block. A filter-policy install targeting a non-self process returns `STATUS_ACCESS_DENIED`.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 8 sfilter suites, 0 failures

> **Notes:**
> - Shipped `include/kernel/nt/syscall_filter.h` + `src/kernel/nt/syscall_filter.c`: per-process SSDT allow-bitmap (main+shadow), immutable snapshots published via `__atomic` release/acquire, gated system-wide by `g_syscall_filter_count`.
> - Dispatch: `ssdt_dispatch` runs the bitmap check before the audit session; no-filter is one ACQUIRE load + branch; KernelMode (Zw) callers bypass; blocked -> ACCESS_DENIED (audit mode logs + allows).
> - Install is self-only via `NtSetInformationProcess(ProcessSystemCallFilterPolicy=41)` (probe+copy); LOCKED = subset-only tighten + frozen flags; inheritance fail-closed pre-clone (NtCreateProcess opt-in, fork unconditional).
> - Lifetime: retire chain freed only at the `task_cleanup` reap barrier; count contribution dropped at every TASK_DEAD site (`syscall_filter_task_dead`); `syscall_filter_counted` single-decrement; generation cap 64.
> - Scope boundary: enforcement is not yet SMP-closed (KernelMode bypass + snapshot free rely on the global current-cursor); full SMP hardening + cross-process install are accepted-XREF below.
> **Verified:** 2026-07-03 | commit `269eccd9` | 8/8 items | build OK | tests 13686 kernel + 16 user PASS
> **Accepted:** [H] KernelMode bypass + filtered-task resolution read the global current-thread cursor (not SMP-closed); on real SMP a filtered user syscall could be misclassified as KernelMode -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor: `thread_current()` resolves from `g_rq[this_cpu()]` ... closes the cross-CPU probe-gating half of 02-kernel-core/TODO-12 §12 (`ssdt_previous_mode`)" at line 116)
> **Accepted:** [H] snapshot free at the reap barrier has no cross-CPU reader grace period; safe on the single-cursor scheduler, a sibling reader on another CPU could race the free once per-CPU run queues land -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §6 (item: "`call_rcu(cb)` defers callbacks to a per-CPU list drained after each quiescent state" at line 171)
> **Accepted:** [M] cross-process filter install (beyond self) needs real OB process objects + granted-access rights; restricted to self meanwhile -> XREF: 02-kernel-core/TODO-12 §7 (item: "[Critical] NtCreateProcess/NtOpenProcess/NtOpenThread return raw PID/TID/task-struct, not OB PROCESS/THREAD objects ... Use real OB objects + rights" at line 1112)
> **Accepted:** [H] `NtCreateProcess` leaks the unstarted child task if `ObpAllocateHandle` fails after `task_create` (the inherited filter clone is freed, but the child TCB is not); the child is inert (no returned handle, never scheduled/exec'd) so it is not an unfiltered-runnable escape -> XREF: 02-kernel-core/TODO-05-object-manager.md (item: "Atomic CreateProcess teardown on failure ... add a `task_destroy(pid)` for unstarted tasks, then make inheritance all-or-fail with teardown" at line 359)
> **Accepted:** [M] the active-count drop at `TASK_DEAD` precedes a proven all-threads-off-CPU quiescence; correct on the single-cursor scheduler (a DEAD task's threads never run), but on real SMP a still-running sibling could see the count reach zero -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (item: "Per-CPU current-thread cursor ... proven-off-CPU-on-all-CPUs reap barrier" at line 116)
> **Accepted:** [M] the audit-mode log throttle bumps one global `s_audit_log_seq` atomic on the blocked-audit branch; uncontended on the single-cursor scheduler but a shared cacheline under real per-CPU run queues (a multi-core audited loop would bounce it) -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (per-CPU state; replace the global audit sample counter with a per-CPU cacheline-isolated one at line 116)
> **Accepted:** [H] the `ProcessSystemCallFilterPolicy` copy of the policy buffer via `copy_from_user` is not fault-recoverable: an unmapped but in-range, aligned user pointer faults in the kernel instead of returning `STATUS_ACCESS_VIOLATION` (systemic usercopy gap, identical to the §24 audit copy-out; affects every NT handler that copies user memory) -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Audit all syscall handlers: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()`" at line 132)
> **Accepted:** [H] `NtCreateProcess` attaches the inherited filter after `task_create` publishes the child (TASK_READY + num_tasks++); non-exploitable today (the child has no entry point and cannot issue a syscall until the parent exec's it, which is after the attach, and NtCreateProcess is atomic under IF=0 on the single-cursor scheduler), but attach-before-publish needs a create-suspended/unpublished task path -> XREF: 02-kernel-core/TODO-05-object-manager.md (item: "Atomic CreateProcess teardown on failure ... a create-suspended/unpublished task path" at line 359)
> **Accepted:** [M] the count gate (`g_syscall_filter_count`) and the filter pointer are two separate atomics, so a lockless reader could observe count==0 while the pointer is already published (miss a just-installed filter for one in-flight syscall); serialized on the single-cursor scheduler, an SMP window -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (per-CPU current-thread cursor / read-side ordering at line 116)
> **Accepted:** [M] once any task is filtered, every SSDT dispatch (even for an unfiltered current task) pays `task_current()` + acquire-load + the out-of-line check; bounded and only while a filter is alive, but not proportional to filtered tasks -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §3 (cache the current task/filter in per-CPU syscall-entry state at line 116)
> **Quality reviewed:** 2026-07-03 | Codex 17x (design, adversarial x4, consistency x4, perf x4, re-adversarial x4) + kernel-quality-auditor | 3H+3M+2L fixed, 10 accepted-XREF | scope: kernel-code-quality

---

## 26. Kernel-to-User Mode Callback Dispatch
Windows NT allows the kernel to call user-mode functions (window procedures, clipboard callbacks, hooks) via `KeUserModeCallback`. The kernel pushes a callback frame, returns to user mode at `KiUserCallbackDispatcher` in ntdll, which indexes the `PEB.KernelCallbackTable` array and calls the registered function. The user-mode function then calls `NtCallbackReturn` (SSDT 0x0300) to return the result to the kernel. This mechanism is critical for Win32k -- every `DispatchMessage` / `SendMessage` uses it.

- [ ] Add `PEB.KernelCallbackTable` field at offset 0x058 (Windows x64 layout) -- pointer to an array of callback function pointers, populated by ntdll/user32 init
- [ ] Implement `KeUserModeCallback(ApiNumber, InputBuffer, InputLength, OutputBuffer, OutputLength)` in `src/kernel/nt/callback.c`:
  - Save current kernel stack frame (RSP, RBP, return address) in a per-thread callback stack
  - Build a user-mode trap frame pointing to `KiUserCallbackDispatcher` in ntdll
  - Pass `ApiNumber`, `InputBuffer`, `InputLength` on the user stack
  - Return to user mode via `sysret` or `iretq`
  - Block until user mode calls `NtCallbackReturn`
- [ ] Wire `NtCallbackReturn` (SSDT 0x0300, currently in §23 catch-all):
  - Copy `OutputBuffer` / `OutputLength` / `Status` back to the kernel-side `KeUserModeCallback` caller
  - Restore kernel stack frame from the callback stack
  - Resume kernel execution at the point after `KeUserModeCallback` returned
- [ ] Per-thread callback depth counter: limit to `CALLBACK_MAX_DEPTH = 64` to prevent stack exhaustion
- [ ] Re-entrant syscalls: user-mode callback code can itself call syscalls; the SSDT dispatcher must handle nested kernel entry correctly
- [ ] Add `KiUserCallbackDispatcher` export address to ntdll (→ XREF: 12-user-platform-sdk/TODO-04-ntdll-user-runtime.md)
- [ ] Commit: `"kernel: nt -- KeUserModeCallback and kernel-to-user callback dispatch"`

> [!NOTE]
> This mechanism is consumed by Win32k (→ XREF: 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §7) for `NtUserDispatchMessage` and `NtUserSendMessage`. Until this section is implemented, Win32k cannot call user-mode window procedures.

**Test checkpoint:** Kernel calls `KeUserModeCallback(0, ...)` → user-mode callback fires → `NtCallbackReturn` returns result to kernel. Nested callback (callback calls syscall which calls another callback) succeeds up to depth 64. Depth 65 returns `STATUS_STACK_OVERFLOW`. Callback on terminated thread returns `STATUS_THREAD_IS_TERMINATING`.

> **Deferred:** [blocker] KeUserModeCallback is blocked on missing infrastructure and has no in-tree caller: no ntdll `KiUserCallbackDispatcher` to return into (ntdll runtime unbuilt), no `PEB.KernelCallbackTable` field (+0x058), no kernel `KiCallUserMode` stack-splice/continuation primitive (needs new asm -- SYSRET/iretq today only return to the syscall caller or the scheduler switch-in, not into a suspended kernel C frame), `NtCallbackReturn` (0x0300) is an unregistered NOT_IMPLEMENTED stub, no per-thread callback-stack/depth fields, and no consumer (Win32k unbuilt) -- the test checkpoint is unsatisfiable, so implementing now would ship untested forward-infrastructure. -> XREF: 12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §1 (item: "Kernel-callback user side ... `KiUserCallbackDispatcher` in `ntdll_except.c`") + 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §7 (USER window management -- the `NtUserDispatchMessage`/`NtUserSendMessage` consumer that calls KeUserModeCallback)

---

## 27. SSDT Integrity Protection

> [!TIP]
> **Impossible OS competitive edge.** Windows uses PatchGuard/KPP -- a complex, opaque system that periodically checksums kernel structures and BSODs on tampering. It's a cat-and-mouse arms race with rootkits. Linux has no SSDT integrity protection at all (`sys_call_table` is `const` but not hardware-enforced). Impossible OS uses hardware write-protection: mark the SSDT pages as read-only via PTE after initialization. Any write attempt triggers a #PF that the kernel catches and escalates to `KeBugCheck(CRITICAL_STRUCTURE_CORRUPTION)`. Zero runtime overhead, no periodic polling, no timing-based detection -- just hardware-enforced immutability.

- [ ] After `ssdt_init()` completes and all 475 handlers are registered, mark SSDT pages as read-only via PTE manipulation (clear R/W bit, flush TLB for affected pages)

> [!NOTE]
> `vmm_protect()` is planned in `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` but does not yet exist. Until it lands, use direct PTE writes: `pte &= ~PTE_WRITE; invlpg(addr)`. This is self-contained -- no external dependency blocks §27.
- [ ] Same for shadow SSDT pages after `win32k_init()` (→ XREF: 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §1)
- [ ] In the #PF handler: if faulting address is within SSDT page range AND fault was a write, call `KeBugCheck(0x00000109)` -- `CRITICAL_STRUCTURE_CORRUPTION`
- [ ] Provide `ssdt_register_late(index, handler)` for drivers that need to register handlers after init:
  - Temporarily mark SSDT page writable, write the handler, re-mark read-only
  - Requires `SeLoadDriverPrivilege`; logs to klog
- [ ] Compile-time: declare SSDT arrays as `const` where possible; the runtime write-protect is the enforcement layer
- [ ] Commit: `"kernel: nt -- SSDT hardware write-protection (integrity enforcement)"`

**Test checkpoint:** After init, writing to SSDT address triggers #PF → BugCheck. `ssdt_register_late` succeeds with correct privilege. `ssdt_register_late` without privilege returns `STATUS_PRIVILEGE_NOT_HELD`. SSDT dispatch still works normally after write-protect (read-only doesn't block reads).

> **Deferred:** [blocker] The write-protect is a hardware-enforced INTEGRITY feature, but `vmm_set_ro` (the RO-transition primitive) is local-invlpg-only and the SSDT is registered in Phase 3 (`boot_desktop.c`), AFTER AP launch in Phase 2 -- so a BSP-local RO transition leaves APs holding a stale writable (huge-page) TLB entry over the SSDT, an SMP write bypass that defeats the feature's purpose. No cross-CPU TLB shootdown exists (only `vmm_flush_tlb_all`, a local CR3 reload). Shipping BSP-only RO for a "hardware-enforced immutability" feature is substandard; the correct fix is an SMP-safe TLB shootdown wired into the RO path, which is SMP-phase-2 infrastructure. The self-contained parts (array `aligned(4096)`, `#PF`->`KeBugCheckEx(0x109)` hook, `ssdt_register_late` WP-clear window gated on `ASSERT_KERNEL_CALLER`) are all ready to land once the shootdown exists. -> XREF: 03-memory-concurrency/TODO-07-smp-phase2.md §2 (items: "`tlb_shootdown(cpu_mask, vaddr, len)`" + "Hook `tlb_shootdown()` into ... `vmm_set_ro()`/`vmm_protect_range()` (W^X + SSDT write-protect TODO-12 §27)"). Shadow-SSDT sub-item additionally blocked on `win32k_init` -> XREF: 08-graphics-ui/TODO-15-win32k-shadow-ssdt.md §1.

---

## 28. Extended Directory Enumeration Classes

NtQueryDirectoryFile (§6) currently returns `FileNamesInformation` only (name + inode). Win32 `FindFirstFileW`/`FindNextFileW` require richer info classes that include per-entry timestamps, size, and attributes. This section extends the directory enumeration path with three additional info classes.

**Prerequisites:** §6 (NtQueryDirectoryFile basic), §13 (FILE_BASIC_INFORMATION / FILE_STANDARD_INFORMATION structs)

- [x] Define `FILE_DIRECTORY_INFORMATION` struct (FileInformationClass 1) in `nt_file.h`: NextEntryOffset, FileIndex, CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize, FileAttributes, FileNameLength, FileName[1]. Fixed-size macro `FILE_DIR_INFO_FIXED_SIZE`.
- [x] Define `FILE_BOTH_DIR_INFORMATION` struct (FileInformationClass 3) in `nt_file.h`: adds ShortNameLength + ShortName[12] (8.3 alias, uppercase truncated). Fixed-size macro `FILE_BOTH_DIR_INFO_FIXED_SIZE`.
- [x] Define `FILE_ID_BOTH_DIR_INFORMATION` struct (FileInformationClass 37) in `nt_file.h`: adds FileId (uint64_t from vfs_dirent.inode). Fixed-size macro `FILE_ID_BOTH_DIR_INFO_FIXED_SIZE`.
- [x] Extended NtQueryDirectoryFile handler dispatches on FileInformationClass: class 0 = legacy ASCII (S6 compat), class 1 = FILE_DIRECTORY_INFORMATION, class 3 = FILE_BOTH_DIR_INFORMATION, class 37 = FILE_ID_BOTH_DIR_INFORMATION. Invalid class returns STATUS_INVALID_INFO_CLASS.
- [x] Per-entry metadata via `vfs_finddir(dir, name)` -> `child->ops->stat(child, &st)` for each entry. Falls back to `child->size` if no stat op.
- [x] Entries packed sequentially in output buffer with 8-byte aligned NextEntryOffset. Last entry has NextEntryOffset = 0. Stops when buffer is full.
- [x] ReturnSingleEntry flag (a6 bit 0): returns exactly 1 entry per call. Enumeration cursor stored in `FILE_OBJECT.dir_enum_index`.
- [x] RestartScan flag (a6 bit 1): resets `FILE_OBJECT.dir_enum_index` to 0 before enumeration.
- [x] Commit: `"kernel: nt -- extended directory enumeration (FileDirectoryInfo, FileBothDir, FileIdBothDir)"` (7338e0a4)

**Test checkpoint:** `NtQueryDirectoryFile(FileBothDirectoryInformation)` on `C:\` returns entries with valid timestamps and sizes. Entry names match VFS readdir output. 8.3 ShortName is populated (uppercase truncated). ReturnSingleEntry returns exactly 1 entry per call. RestartScan re-reads from the beginning.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | dir-enum struct-size + dispatch suites, 0 failures

> **Notes:**
> - Shipped 3 Win32 dir info classes (FILE_DIRECTORY/BOTH_DIR/ID_BOTH_DIR_INFORMATION) in `nt_file.h` + the `NtQueryDirectoryFile` class packer in `nt_syscall.c` (NextEntryOffset chain, ReturnSingleEntry/RestartScan, `dir_enum_index` cursor).
> - Review hardened it: `FILE_*_FIXED_SIZE` -> `__builtin_offsetof(FileName)` + 6 ABI `_Static_assert`s; info_class checked before the loop; bad handle -> `STATUS_INVALID_HANDLE`; up-front `ProbeForWriteIfUser`; error exits set iosb.
> - Consumers: Win32 `FindFirstFileW`/`FindNextFileW` (TODO-05 §6) via `WIN32_FIND_DATAW`.
> - Canonical doc: the struct definitions in `include/kernel/nt/nt_file.h`.
> - Scope boundary: fault-recoverable `copy_to_user`, the O(1) cursor, and cursor SMP-serialization are accepted-XREF below; ShortName 8.3 collisions + non-ASCII UTF-16 decode stay naive.
> **Verified:** 2026-07-03 | commit `7338e0a4` (impl) + review fixes | 9/9 items | build OK | tests 1051 kernel + 16 user PASS
> **Accepted:** [Critical] the output buffer + IO_STATUS_BLOCK are `ProbeForWriteIfUser`'d then written entry-by-entry through the raw user pointer (not a fault-recoverable `copy_to_user` bounce) -- an in-range unmapped page faults in the kernel; systemic usercopy gap -> XREF: 03-memory-concurrency/TODO-02-memory-security.md §4 (item: "Audit all syscall handlers: replace raw user-pointer dereference with `copy_from_user()` / `copy_to_user()` ... every `src/kernel/nt/nt_*.c` ... any user-pointer-typed syscall arg" at line 126)
> **Accepted:** [H] `FILE_OBJECT.dir_enum_index` cursor read-modify-write across the enumeration loop is unlocked; concurrent enumeration on a duplicated handle races (skip/dupe); single-cursor-safe today -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "VFS dir-enum cursor ... serialize vs concurrent shared-handle enum" at line 170)
> **Accepted:** [H] enumeration is O(n^2) -- `vfs_readdir(index)` re-walks from head and `vfs_finddir`+`stat` re-scan per entry (VFS iterator API is index-based, no metadata in `vfs_dirent`) -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "VFS dir-enum cursor ... O(1) `FILE_OBJECT` cursor + metadata in `vfs_dirent`" at line 170)
> **Accepted:** [H] `NtQueryDirectoryFile(fh==0, ...)` still enumerates `C:\` (the original §6 no-handle dev shim) instead of `STATUS_INVALID_HANDLE` for Windows parity; the fh==-1 and all other-handle paths are now validated, only fh==0 keeps the shim (the ABI unit test relies on it, and requiring a valid handle needs the `CreateFile(LIST_DIR)` handle-open path) -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "Retire the `NtQueryDirectoryFile` fh==0 C:\ shim ... require a valid handle" at line 171)
> **Accepted:** [H] the read gate checks object-wide `FILE_OBJECT.access`, not the per-handle `granted_access`, so a duplicate handle with read stripped still enumerates; systemic handle-rights gap shared by NtReadFile/NtWriteFile -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "Per-handle granted-access enum gate" at line 176)
> **Accepted:** [M] `disposition_to_vfs` maps only GENERIC_READ/WRITE, so a directory opened list+`GENERIC_WRITE` is stored write-only and the `VFS_O_READ` gate false-denies it; systemic mapping limitation affecting every read gate -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "`disposition_to_vfs` FILE_LIST_DIRECTORY mapping" at line 177)
> **Accepted:** [M] the `IO_STATUS_BLOCK` is not published on the null/zero-buffer and buffer-probe-failure early returns (they precede the iosb probe), so a caller reusing the IOSB can observe a stale completion -> XREF: 05-storage-filesystems/TODO-05-win32-file-io-api.md §6 (item: "`NtQueryDirectoryFile` IOSB on early exits" at line 178)
> **Quality reviewed:** 2026-07-03 | Codex 15x (adversarial x4, consistency x4, perf x4, re-adversarial x3) + kernel-quality-auditor | 1Crit+6H+3M+2L fixed (incl. handle-access VFS_O_READ gate + last-entry unpadded packing), 7 accepted-XREF | scope: kernel-code-quality

---

## 29. Token Lifecycle and SRM Access Check Syscalls

> [!NOTE]
> Split from §16 to keep each section under the 10-item limit. §16 covers "operate on existing token" (open/query/set/adjust). This section covers token creation/derivation and the Security Reference Monitor decision points (NtAccessCheck, NtPrivilegeCheck, security-descriptor I/O). Token infrastructure in `src/kernel/security/token.c`; SRM engine in `src/kernel/security/srm.c` (→ XREF TODO-15 §7, §8).

- [ ] Ring-3-safe audit-hook registration (SSDT 0x0150/0x0151, stubs today): expose §24 `nt_audit.c` register/unregister via a validated descriptor (id, not raw pointer) gated by `SeSinglePrivilegeCheck(SeAuditPrivilege)`.
- [ ] `NtDuplicateToken(ExistingTokenHandle, DesiredAccess, ObjectAttributes, EffectiveOnly, TokenType, NewTokenHandle)` → SSDT 0x00B8
- [ ] `NtFilterToken(ExistingTokenHandle, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewTokenHandle)` → SSDT 0x00B9
- [ ] `NtCreateToken(TokenHandle, DesiredAccess, ObjectAttributes, TokenType, AuthenticationId, ExpirationTime, User, Groups, Privileges, Owner, PrimaryGroup, DefaultDacl, Source)` → SSDT 0x00BA
- [ ] `NtAccessCheck(SecurityDescriptor, ClientToken, DesiredAccess, GenericMapping, PrivilegeSet, PrivilegeSetLength, GrantedAccess, AccessStatus)` → SSDT 0x00BC:
  - Core SRM decision point; routes to `SeAccessCheck()` in the Security Reference Monitor
- [x] `NtPrivilegeCheck(ClientToken, RequiredPrivileges, Result)` → SSDT 0x00BF -- shipped in TODO-15 §12 (nt_token.c NtPrivilegeCheck_handler + token-explicit SePrivilegeCheckToken)
- [ ] `NtSetSecurityObject(Handle, SecurityInformation, SecurityDescriptor)` → SSDT 0x00C1
- [ ] `NtQuerySecurityObject(Handle, SecurityInformation, SecurityDescriptor, Length, LengthNeeded)` → SSDT 0x00C2
- [ ] [Critical] NtShutdownSystem + legacy SYS_SHUTDOWN: require SeShutdownPrivilege before acpi_reboot/shutdown -- any ring-3 caller can power off the machine today (`nt_syscall.c:1195`). (§5 review)
- [ ] [Critical] NtTerminateProcess: resolve through the handle table + require PROCESS_TERMINATE, not a raw PID -- `nt_process.c:227` lets any ring-3 caller kill any process by PID. (§5 review)
- [ ] [Critical] SystemProcessInformation: ProbeForWriteIfUser + copy_to_user + length-mismatch status -- unprobed user-pointer write at `nt_syscall.c:923`. (§5 review)
- [ ] [Critical] NtQuery/SetInformationProcess + NtQuery/SetInformationThread + NtDelayExecution interval: input/output user pointers deref'd raw (`nt_process.c:460+`) -- probe + copy_from_user/copy_to_user via kernel bounce buffers. (§7 review)
- [ ] [Critical] Sync syscalls deref raw user pointers (`nt_sync.c`): NtWaitForMultipleObjects handle array + timeout, Create* name/OA/out-HANDLE, prev-state/count outputs -- probe + copy-in/out via kernel buffers. (§8)
- [ ] [Critical] Namespace + token syscalls deref raw user pointers (`nt_namespace.c`, `nt_token.c`): out-HANDLE, Context/ReturnLength, OBJECT_ATTRIBUTES, symlink target, token buffers -- probe + copy-in/out via kernel buffers. (§16/§17)
- [ ] **Foundation: per-token SMP lock** (`token.h`/`token.c`): add a spinlock to `ACCESS_TOKEN` taken by every read/mutate path so a privilege decision cannot observe a torn `Privileges[]` mid-adjust. (§29 design)
- [ ] **Foundation: owned restricted-SID storage** (`token.c`): replace the single `RestrictedSids`+count with owned per-entry storage (bounded usercopy, deep-dup, `token_on_delete` free) before `NtFilterToken`. (§29 design)
- [ ] **Foundation: lifetime-pinned + access-mask token resolution** (`nt_token.c`): `resolve_token_handle` must `ObReferenceObjectByHandle`-pin the token (`NtClose` UAF today) AND enforce per-op `TOKEN_*` bits; retrofit all handlers. (§29)
- [ ] **Foundation: default-token privilege policy** (`SeCreateUserToken`): do NOT enable `SeShutdownPrivilege` for standard users, else the shutdown gate is meaningless; require elevation + negative test. (§29 design)
- [ ] **Foundation: shared privilege-gated shutdown helper** (`nt_syscall.c`, `sched/syscall.c`): route `NtShutdownSystem` AND legacy `SYS_REBOOT`/`SYS_SHUTDOWN` through one `SeShutdownPrivilege` check. (§29 design)
- [ ] `NtAccessCheck`/`SeAccessCheck` DACL engine + `GENERIC_MAPPING` type are external prerequisites -> XREF: `TODO-15-security-reference-monitor.md §5,§8`.
- [ ] Commit: `"kernel: nt -- token lifecycle and SRM access check syscalls"`

**Test checkpoint:** `NtDuplicateToken` returns a distinct token with copied privileges/groups. `NtAccessCheck` against an object with DACL returns correct granted access. `NtPrivilegeCheck` with a held privilege returns TRUE; with a missing privilege returns FALSE. `NtQuerySecurityObject` round-trips through `NtSetSecurityObject`.

> **Deferred:** 2026-07-03 | reason: correct token-lifecycle + SRM syscalls require foundational token-security infrastructure not present today -- a per-token SMP lock (privilege reads race in-place `NtAdjustPrivilegesToken` mutation), owned restricted-SID storage for `NtFilterToken`, access-mask-aware `resolve_token_handle`, and a default-token privilege-policy fix (standard tokens hold `SeShutdownPrivilege` enabled, which nullifies the shutdown gate). §29 Codex design review returned needs-attention (2 Critical + 2 High + 1 Medium) proving a thin wiring ships bypassable/racy/ineffective enforcement. The SRM decision engine (`SeAccessCheck` + `GENERIC_MAPPING`) is also absent. Security-sensitive; deferred rather than guessed in an unattended pass. Foundations tracked as `[ ]` items above -> XREF: `TODO-15-security-reference-monitor.md §5,§8` (SeAccessCheck DACL engine + SeSinglePrivilegeCheck/SePrivilegeCheck SRM API).

---

## 30. Generic Object Management Syscalls

> [!NOTE]
> Split from §17 to keep both sections under the 10-item limit. §17 covers namespace objects (directory + symlink); this section covers object lifetime and identity operations that apply to **any** OB type (events, mutexes, files, sections, etc.). Object Manager infrastructure in `src/kernel/ob/ob.c` (→ XREF TODO-05 §1, §4, §9).

- [ ] Register `NtDuplicateObject` → SSDT 0x0001 (process-handle NT ABI); wrap `ob.c` `NtDuplicateObject` after resolving process handles to handle tables. Unblocks the `ZwDuplicateObject` alias in `zw.h`. XREF: TODO-05 §9.
- [ ] `NtMakeTemporaryObject(Handle)` → SSDT 0x0003: clears `OB_FLAG_PERMANENT`, allowing the object to be deleted when its reference count drops to zero
- [ ] `NtMakePermanentObject(Handle)` → SSDT 0x0004: sets `OB_FLAG_PERMANENT` (kernel-mode caller only; requires `SeCreatePermanentPrivilege`)
- [ ] `NtSetInformationObject(Handle, ObjectInformationClass, Buffer, Length)` → SSDT 0x0005: writable counterpart to `NtQueryObject`; supports `ObjectHandleFlagInformation` (set `OBJ_INHERIT` / `OBJ_PROTECT_CLOSE` on the HANDLE_TABLE_ENTRY)
- [ ] `NtCompareObjects(FirstObjectHandle, SecondObjectHandle)` → SSDT 0x0009: returns `STATUS_SUCCESS` if both handles refer to the same underlying object body, else `STATUS_NOT_SAME_OBJECT`
- [ ] **Expose `NtQueryObject` at SSDT 0x0002 with the Win11 `OBJECT_TYPE_INFORMATION` NT ABI** (`UNICODE_STRING TypeName`), replacing the internal `char[32]`+counters struct in `ob.c`; unblocks the `ZwQueryObject` alias. XREF: TODO-05 §12.
- [ ] **Foundation: pinned handle resolution** -- handlers must resolve+ref via `ObpReferenceObjectByHandle` + `ObDereferenceObject`, not raw `ObpLookupHandle` (unref'd, UAF vs close) -> XREF: `TODO-05-win32-file-io-api.md §3`. (§30 design)
- [ ] **Foundation: SMP-safe make-permanent gate** -- `NtMakePermanentObject`'s `ssdt_previous_mode()` gate is unreliable until previous-mode is per-CPU (cursor misclassifies on SMP) -> XREF: `TODO-07-smp-phase2.md §3`. (§30 design)
- [ ] **Foundation: DELETE-access gate on make-temporary** -- `NtMakeTemporaryObject` must require DELETE via per-handle `granted_access`, else any handle alters object lifetime. (§30 design)
- [ ] Commit: `"kernel: nt -- generic object management syscalls (make-temp/perm, set-info, compare)"`

**Test checkpoint:** `NtMakePermanentObject` on an event handle prevents deletion when last reference released. `NtMakeTemporaryObject` re-enables deletion. `NtSetInformationObject(ObjectHandleFlagInformation)` toggles `OBJ_INHERIT` on a handle. `NtCompareObjects` with two duplicate handles returns `STATUS_SUCCESS`; with handles to different objects returns `STATUS_NOT_SAME_OBJECT`.

> **Deferred:** 2026-07-03 | reason: the object-lifetime + identity syscalls require the pinned handle-lookup primitive that does not exist yet. `ObpLookupHandle` returns a raw unreferenced `HANDLE_TABLE_ENTRY*` (`handle_table.c:343-367`), so `NtMakeTemporary`/`NtMakePermanent`/`NtSetInformationObject`/`NtCompareObjects` built on it are use-after-free surfaces against a concurrent `NtClose` -- §30 Codex design review returned needs-attention (1 Critical + 2 High). The pinned `ObpReferenceObjectByHandle` primitive is owned by TODO-05 §3; the SMP-safe kernel-mode gate needs per-CPU previous-mode (TODO-07 §3); the make-temporary DELETE-access gate needs per-handle granted-access enforcement. Security/lifetime-sensitive; deferred rather than ship a UAF regression in an unattended pass. Foundations tracked as `[ ]` items above -> XREF: `TODO-05-win32-file-io-api.md §3` (`ObReferenceObjectByHandle` at line 115) + `TODO-07-smp-phase2.md §3`.

---

## 31. Modern ALPC Port Syscalls

> [!NOTE]
> Split from §20 to keep both sections under the 10-item limit. §20 covers legacy LPC (NT 3.x-5.x) compatibility syscalls; this section covers the modern ALPC (Vista+) surface. Reserves SSDT indices 0x010F-0x011E and wires NT-compatible signatures so user-mode callers get `STATUS_NOT_IMPLEMENTED` (rather than an empty SSDT slot) until the ALPC subsystem lands. Full port infrastructure -- `ALPC_PORT` object, message queue, connection state machine, send/receive engine, section views, resource reserves, information classes -- is implemented in `02-kernel-core/TODO-24-alpc-message-ports.md`. `\RPC Control\` namespace directory is added by TODO-24 §2 (`ObpAlpcPortType` registration).

- [x] `NtAlpcCreatePort(PortHandle, ObjectAttributes, PortAttributes)` → SSDT 0x010F
- [x] `NtAlpcConnectPort(PortHandle, PortName, ObjectAttributes, PortAttributes, Flags, RequiredServerSid, ConnectionMessage, BufferLength, OutMessageAttributes, InMessageAttributes, Timeout)` → SSDT 0x0110
- [x] `NtAlpcConnectPortEx(...)` → SSDT 0x0111
- [x] `NtAlpcAcceptConnectPort(PortHandle, ConnectionPortHandle, Flags, ObjectAttributes, PortAttributes, PortContext, ConnectionRequest, ConnectionMessageAttributes, AcceptConnection)` → SSDT 0x0112
- [x] `NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMessage, SendMessageAttributes, ReceiveMessage, BufferLength, ReceiveMessageAttributes, Timeout)` → SSDT 0x0113
- [x] `NtAlpcDisconnectPort(PortHandle, Flags)` → SSDT 0x0114
- [x] `NtAlpcCancelMessage(PortHandle, Flags, MessageContext)` → SSDT 0x0115
- [x] `NtAlpcCreatePortSection(PortHandle, Flags, SectionHandle, SectionSize, AlpcSectionHandle, ActualSectionSize)` → SSDT 0x0116
- [x] `NtAlpcDeletePortSection(PortHandle, Flags, SectionHandle)` → SSDT 0x0117
- [x] `NtAlpcCreateSectionView(PortHandle, Flags, ViewAttributes)` → SSDT 0x0118
- [x] `NtAlpcDeleteSectionView(PortHandle, Flags, ViewBase)` → SSDT 0x0119
- [x] `NtAlpcCreateResourceReserve(PortHandle, Flags, MessageSize, ResourceId)` → SSDT 0x011A
- [x] `NtAlpcDeleteResourceReserve(PortHandle, Flags, ResourceId)` → SSDT 0x011B
- [x] `NtAlpcQueryInformation(PortHandle, PortInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x011C
- [x] `NtAlpcSetInformation(PortHandle, PortInformationClass, Buffer, Length)` → SSDT 0x011D
- [x] `NtAlpcQueryInformationMessage(PortHandle, PortMessage, MessageInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x011E
- [x] Commit: `"kernel: nt -- modern ALPC port syscalls wired to SSDT"`

**Test checkpoint:** All 16 ALPC SSDT slots resolve to the registered handler (not the default `ssdt_stub_not_implemented`). At §31 ship each returned `STATUS_NOT_IMPLEMENTED` (SCOPE-GAP-ALLOWED); TODO-24 §3-§6 has since retrofitted several (CreatePort/Connect/Accept/Disconnect/SendWaitReceive/SetInformation) to real handlers, the rest remain deferred stubs. Functional round-trip and `\RPC Control\` namespace visibility are exercised by TODO-24 tests.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 2 new §31 tests (ALPC slots registered, ALPC returns deferred-status) + 1 cross-section PE export-sort invariant test, 0 failures.

> **Notes:**
> - **What shipped:** `nt_alpc.c` -- 16 `NtAlpc*` SSDT stub handlers (0x010F-0x011E) via `nt_alpc_register_ssdt()` (failure-count return) + 16 sorted PE exports in `pe.c`.
> - **How it integrates:** registered from `boot_desktop.c`; `alpc_register_one()` bounds-checks `idx` vs `SSDT_MAIN_MAX` before the load, a non-zero failure total escalates to `boot_halt`; `pe_exports_sorted_check()` gates export order every boot.
> - **Downstream effects:** TODO-24 §3-§6 retrofitted the connect/send/receive/set handlers to the real ALPC engine; remaining slots stay deferred stubs until their retrofit.
> - **Canonical doc:** [`TODO-24-alpc-message-ports.md`](TODO-24-alpc-message-ports.md) owns the ALPC engine + real handler ABIs.
> - **Scope boundary:** §31 owns SSDT slot reservation + stub wiring + PE exports; TODO-24 owns the ALPC engine and the Windows-exact handler arg ABIs.
> **Verified:** 2026-07-03 | commit `e89ed22e` | 16/16 items | build OK | 13681 kernel + 16 user tests PASS
> **Accepted:** [H] ALPC engine (ALPC_PORT object, connection state machine, PORT_MESSAGE rendezvous, port sections, resource reserves, info classes, `\RPC Control\` directory) deferred -- §31 scope is SSDT reservation + signatures only -> XREF: 02-kernel-core/TODO-24 §8 (item: "Retrofit the ALPC SSDT stubs in `nt_alpc.c` (0x010F-0x011E) to engine-backed handlers." at line 365)
> **Accepted:** [H] the TODO-24-retrofitted live ALPC handlers (`NtAlpcConnectPort`, `NtAlpcAcceptConnectPort`, `NtAlpcSendWaitReceivePort`, `NtAlpcSetInformation`) use compacted <=6-arg ABIs (SSDT dispatch passes only a1-a6), dropping Windows args (message attributes, ObjectAttributes, RequiredServerSid, LARGE_INTEGER timeout ptr); Windows-exact ABI needs a >6-arg SSDT dispatch, owned by TODO-24 -> XREF: 02-kernel-core/TODO-24 §6 (item: "6.3 Message attributes dispatcher: ALPC_MESSAGE_ATTRIBUTES struct passed to send/receive" at line 273)
> **Accepted:** [H] the TODO-24-retrofitted live ALPC handlers probe user buffers once then pass the original caller pointers into the ALPC engine (re-read/written after a wait) -- a TOCTOU/unprobed-usercopy path; the systemic copy_to_user/copy_from_user retrofit owns this -> XREF: 03-memory-concurrency/TODO-02-memory-security.md (item: "Audit all syscall handlers ... every `src/kernel/nt/nt_*.c` ... `nt_alpc.c`" at line 126)
> **Accepted:** [H] pre-existing PE-loader OOB read (not §31; §31 only added export entries): `pe_resolve_dll_imports` (`pe.c`) validates untrusted import-name RVAs with 32-bit `name_rva+2`/`hint_rva+3` that wrap near `UINT32_MAX` -> bound passes, OOB read on a malformed PE -> XREF: 10-platform-services/TODO-07-win32-pe-loader.md §6 (item: "Harden `pe_resolve_dll_imports` RVA bounds")
> **Quality reviewed:** 2026-07-03 | Codex 9x (adversarial x3, consistency x3, perf x3) | 1M fixed (Port/ALPC next-available hint), 4H accepted-XREF (ALPC engine, compacted-ABI class, usercopy TOCTOU, PE-import RVA wrap) | scope: kernel-code-quality

---

## 32. Post-Ship Follow-Up Backfill (orphan cohort 2026-07-31)

Items moved here VERBATIM from their original, already-stamped sections, where they were unreachable: the triage oracle classifies a stamped section DONE without reading its body, so an item appended after the stamp is invisible to every later pass. Source section noted per group. Cohort context: `todo/overnight-runner-improvements/overnight-runner-improvements-v05.md` item 3.

From the stamped section 13:
- [ ] `FILE_OBJECT` canonical-path sync across ALIASED handles on rename: a node-shared current path so setinfo unveil re-checks use the current name for EVERY open handle, not just the renaming one (-> XREF: TODO-21 s12 unveil).
- [ ] Serialize same-handle path-mutating `NtSetInformationFile` (FILE_OBJECT/node lock across unveil auth + rename/truncate + `fo->path` commit) so a racing rename cannot let a sibling thread authorize a stale path (-> XREF: TODO-21 s12).
- [ ] Tail-pack `FILE_OBJECT.path` into the object-manager allocation (like the tail-packed SD) so a file open needs one heap node, not a second `kmalloc` on the open hot path (-> XREF: TODO-21 s12).
- [ ] Variable-length `unveil_entry` (store the folded path inline-sized, not a fixed 512 B) so cloning a maximal 128-entry unveil set at fork copies far less than ~66 KiB through the heap (-> XREF: TODO-21 s12).
- [ ] `NtSetInformationFile` NT ACCESS_MASK enforcement: track the granted mask on `FILE_OBJECT`, require DELETE for dispose/rename and FILE_WRITE_* for truncate/alloc/attrs (interim gate: any write access) (-> XREF: TODO-21 s12).

**Test checkpoint:** per moved item; each carries its original acceptance text.

---

## OS Comparison

| ⭐  | Feature                    | 🪟 Win11                    | 🐧 Linux                  | 🚀 Impossible OS                                                                          |
| --- | -------------------------- | --------------------------- | ------------------------- | ----------------------------------------------------------------------------------------- |
| 💎  | SYSCALL/SYSRET fast path   | ✅ KiSystemCall64+LSTAR     | ✅ entry_SYSCALL_64       | ✅ §2 LSTAR + SYSRET enabled                                                              |
| 💎  | Typed failure return       | ✅ NTSTATUS on all NtXxx    | ✅ -ERRNO signed          | ✅ §1 NTSTATUS + 66 codes                                                                 |
| 💎  | Service descriptor table   | ✅ SSDT + shadow SSDT       | ✅ sys_call_table[]       | ✅ §4 SSDT 475 + shadow stub                                                              |
| 💎  | SW-interrupt compat path   | ✅ INT 0x2E (legacy)        | ✅ INT 0x80 (32-bit)      | ✅ §3 INT 0x2E + 0x80                                                                     |
| 💎  | IO_STATUS_BLOCK async I/O  | ✅ IOSB on all file Nt      | ⚠️ io_uring only          | ⬜ §11                                                                                    |
| 💎  | File metadata syscalls     | ✅ NtQuery/SetInfoFile      | ✅ stat/fstat/utimensat   | ⬜ §13                                                                                    |
| 💎  | Device I/O control         | ✅ NtDeviceIoControlFile    | ✅ ioctl()                | ⬜ §13                                                                                    |
| 💎  | I/O completion ports       | ✅ NtCreateIoCompletion     | ✅ epoll/io_uring         | ⬜ §13                                                                                    |
| 💎  | Process/thread create API  | ✅ NtCreate{Process,Thread} | ✅ clone/execve           | ✅ §7 23 handlers wired                                                                   |
| 💎  | Thread context get/set     | ✅ NtGet/SetContextThread   | ✅ ptrace GETREGS         | 🔄 §7 stubs (needs TODO-23)                                                               |
| 💎  | Named sync objects         | ✅ NtCreate{Event,Mutant}   | ✅ POSIX sem + futex      | ✅ §8 Event+Mutant+Semaphore                                                              |
| 💎  | Multi-object wait          | ✅ NtWaitForMultipleObj     | ⚠️ No direct equivalent   | ✅ §8 all-or-none WaitAll, 64 max                                                         |
| 💎  | Keyed events (futex)       | ✅ NtWaitForKeyedEvent      | ✅ futex()                | 🔄 §8 stubs (T08-mem §10)                                                                 |
| 💎  | Virtual memory syscalls    | ✅ NtAllocate/Free/Protect  | ✅ mmap/mprotect/munmap   | ✅ §9 Alloc+Free+Protect+Query                                                            |
| 💎  | Cross-process memory       | ✅ NtRead/WriteVirtualMem   | ✅ process_vm_readv       | ✅ §9 Read+Write (identity)                                                               |
| 💎  | OS info query syscall      | ✅ NtQuerySystemInfo        | ✅ sysinfo + /proc        | 🔄 §10 5 classes + defaults                                                               |
| 💎  | LastError per-thread       | ✅ TEB→LastErrorValue       | ✅ errno via TLS          | ⬜ §11 + TODO-11 §1                                                                       |
| 💎  | Registry syscalls          | ✅ NtCreate/Open/QueryKey   | ❌ No equivalent          | ⬜ §14 + TODO-14                                                                          |
| 💎  | Token/access control       | ✅ NtAccessCheck + tokens   | ✅ capabilities + DAC/MAC | ⬜ §16 + TODO-15                                                                          |
| 💎  | Namespace dir/symlink      | ✅ NtCreateDirectoryObj     | ❌ No kernel namespace    | ⬜ §17                                                                                    |
| 💎  | Memory-mapped sections     | ✅ NtCreateSection/MapView  | ✅ mmap with MAP_SHARED   | 🟡 §18 SSDT 0x005C-0x0062; unmap-after-close + protection deferred                        |
| 💎  | Timer objects              | ✅ NtSetTimer periodic      | ✅ timerfd_create         | 🟡 §19 6 SSDT 0x007E-0x0083; absolute-time + SetTimerEx arm + tick-ISR hardening deferred |
| 💎  | ALPC message ports         | ✅ NtAlpcSendWaitReceive    | ❌ No equivalent          | 🟡 §20 (LPC stub) + §31 (ALPC stub), engine in TODO-24                                    |
| 💎  | Debug API                  | ✅ NtDebugActiveProcess     | ✅ ptrace                 | ⬜ §21 + TODO-29                                                                          |
| 💎  | Power management           | ✅ NtSetSystemPowerState    | ✅ sys_reboot + ACPI      | 🔄 §5 NtShutdownSystem wired                                                              |
| 💎  | Atom table                 | ✅ NtAddAtom/FindAtom       | ❌ No equivalent          | ✅ §23 refcounted global atoms                                                            |
| ⭐  | Syscall audit/tracing hook | ⚠️ ETW (heavyweight)        | ⚠️ seccomp-bpf / ptrace   | ✅ §24 pre/post SSDT hook, deny + zero-overhead-when-off                                  |
| 💎  | Default locale / UI lang   | ✅ NtQuery/SetDefaultLocale | ✅ setlocale + LANG       | 🟡 §23 query returns en-US; set fails-closed (priv, TODO-13 §6)                           |
| ⭐  | ZwXxx CPL-gated aliases    | ✅ Internal, undocumented   | ❌ No equivalent          | ✅ §12 zw.h + ProbeFor*                                                                   |
| ⭐  | Stable native API contract | ⚠️ Undocumented             | ❌ No stable native API   | ⬜ §4+§12 -- numbered+public                                                              |
| ⭐  | Syscall audit hook         | ⚠️ ETW, heavyweight         | ⚠️ seccomp-bpf, complex   | ⬜ §24 -- first-class API                                                                 |
| 💎  | Per-process syscall filter | ✅ SystemCallDisablePolicy  | ✅ seccomp-bpf + Landlock | ⬜ §25 -- bitmap + BPF                                                                    |
| 💎  | Kernel→user callbacks      | ✅ KeUserModeCallback       | ⚠️ Signals only           | ⬜ §26                                                                                    |
| ⭐  | SSDT integrity protection  | ⚠️ PatchGuard (periodic)    | ❌ No protection          | ⬜ §27 -- HW write-protect                                                                |

> **Target after §1–§23 completion:** Impossible OS reaches complete NT native API coverage across 475 syscall endpoints. Current state is partial; many domain and deferred sections remain open.
> **§12** makes the `ZwXxx` layer an explicit, documented public contract -- Windows keeps it internal/undocumented and Linux has no equivalent.
> **§24** provides first-class syscall auditing -- no ETW complexity, no BPF programs, just a kernel callback with near-zero idle overhead.
> **§25** closes the per-process syscall filtering parity gap -- both Win11 and Linux restrict per-process syscall access; Impossible OS uses a fast bitmap with optional BPF programs.
> **§26** enables kernel→user callbacks required by Win32k for window procedure dispatch -- without it, `SendMessage` and `DispatchMessage` cannot work.
> **§27** provides hardware-enforced SSDT immutability -- simpler and more secure than PatchGuard's periodic checksums.

---

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nt_api()` (XREF: `00-infrastructure/TODO-03-kernel-test-harness.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_nt_api.c` with:
  - **NTSTATUS macros (§1):**
    - `NT_SUCCESS(STATUS_SUCCESS)` returns true; `NT_ERROR(STATUS_INVALID_HANDLE)` returns true
    - `NT_SUCCESS(STATUS_ACCESS_DENIED)` returns false; `NT_WARNING(STATUS_BUFFER_OVERFLOW)` returns true
    - `NT_INFORMATION(STATUS_ALERTED)` returns true; `NT_ERROR(STATUS_MUTANT_NOT_OWNED)` returns true
  - **SSDT dispatch (§4):**
    - Valid index calls handler; invalid index returns `STATUS_NOT_IMPLEMENTED`
    - Index beyond table size returns `STATUS_NOT_IMPLEMENTED`, not a crash
    - SSDT has ≥ 475 registered entries
  - **Core handle ops (§6):**
    - `NtClose(INVALID_HANDLE_VALUE)` returns `STATUS_INVALID_HANDLE`
    - `NtCreateFile` on existing file returns `STATUS_SUCCESS` and a valid HANDLE
    - `NtClose` on a valid HANDLE returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`
    - `NtDuplicateObject` from kernel-mode produces valid second handle to same object
  - **File I/O (§6, §13):**
    - `NtReadFile` populates `IO_STATUS_BLOCK.Status = STATUS_SUCCESS` and `Information = bytes_read`
    - `NtWriteFile` populates `IO_STATUS_BLOCK` correctly after write
    - `NtQueryInformationFile(FileBasicInformation)` returns non-zero creation time
    - `NtQueryAttributesFile` returns file attributes without opening
  - **Sync (§8):**
    - `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` returns `STATUS_WAIT_0`
    - `NtWaitForMultipleObjects(WaitAny)` returns `STATUS_WAIT_0 + 1` when second object signalled
  - **Memory (§9):**
    - `NtAllocateVirtualMemory` with `MEM_COMMIT` returns a usable address; write/read round-trip succeeds
    - `NtProtectVirtualMemory` changes RW→RO; old protect returned correctly
    - `NtFreeVirtualMemory` on allocated region returns `STATUS_SUCCESS`
  - **System info (§10):**
    - `NtQuerySystemInformation(SystemBasicInformation)` returns correct page size and processor count
  - **Error propagation (§11):**
    - `RtlNtStatusToDosError(STATUS_ACCESS_DENIED)` returns `5`; `STATUS_NO_MEMORY` returns `8`
  - **ZwXxx (§12):**
    - ZwXxx kernel-mode call: `ZwClose` from CPL=0 skips user-buffer probe (no fault)
  - **Registry (§14):**
    - `NtCreateKey` + `NtSetValueKey` + `NtQueryValueKey` round-trip
  - **Token (§16):**
    - `NtOpenProcessToken` on current process returns valid handle
  - **Audit hook (§24):**
    - Register pre-call hook; verify it fires on `NtClose`
    - Unregister hook; verify no more callbacks
  - **Syscall filter (§25):**
    - Set filter blocking `NtWriteFile` on child; child's `NtWriteFile` returns `STATUS_ACCESS_DENIED`
    - Parent's `NtWriteFile` still works (filter is per-process)
    - `DisallowWin32kSystemCalls` blocks shadow SSDT calls
    - Locked filter (`SYSCALL_FILTER_LOCKED`) cannot be relaxed
  - **Kernel→user callback (§26):**
    - `KeUserModeCallback(0, ...)` fires user-mode callback; `NtCallbackReturn` returns result
    - Nested callback (depth 2) succeeds; depth > 64 returns `STATUS_STACK_OVERFLOW`
  - **SSDT integrity (§27):**
    - Write to SSDT page after init triggers BugCheck
    - `ssdt_register_late` with correct privilege succeeds
    - `ssdt_register_late` without privilege returns `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Register in `test_runner_init()`: `test_register_nt_api()`
- [ ] Commit: `"test: add native API layer test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: Phase 3 shows `"syscall: fast path (SYSCALL/SYSRET) enabled"`
- [ ] Serial log at init: `"SSDT initialized: %u main slots (last=0x%03X), shadow stub ready"`
- [ ] `mov rax, 0x0000; syscall` from ring 3 reaches `NtClose` handler without a GPF
- [ ] INT 0x2E from ring 3 reaches the same `syscall_dispatch` with correct register mapping
- [ ] `NtCreateFile` on `X:\Logs\kernel.log` returns `STATUS_SUCCESS` and a valid HANDLE
- [ ] `NtClose(handle)` returns `STATUS_SUCCESS`; calling it again returns `STATUS_INVALID_HANDLE`
- [ ] SSDT slot for unimplemented index returns `STATUS_NOT_IMPLEMENTED`, not a crash
- [ ] `NtWaitForMultipleObjects` with 2 events, one signalled, returns correct index
- [ ] `NtProtectVirtualMemory` changes page protection; read-after-RO-change faults correctly
- [ ] `NtQueryInformationFile(FileBasicInformation)` returns valid timestamps
- [ ] `NtDeviceIoControlFile` routes to device driver dispatch
- [ ] `NtCreateKey` + `NtSetValueKey` + `NtQueryValueKey` round-trip succeeds
- [ ] `NtOpenProcessToken` + `NtQueryInformationToken(TokenUser)` returns correct SID
- [ ] `NtCreateDirectoryObject` + `NtCreateSymbolicLinkObject` + `NtQuerySymbolicLinkObject` round-trip
- [ ] `NtCreateSection(SEC_COMMIT)` + `NtMapViewOfSection` + write/read round-trip
- [ ] `NtSetTimer` fires after specified delay; `NtCancelTimer` cancels pending timer
- [ ] `TEB->LastErrorValue` at `gs:[0x68]` is updated after a failing `NtOpenFile`
- [ ] IOSB populated correctly: `Status = STATUS_SUCCESS`, `Information = bytes_read` after `NtReadFile`
- [ ] CPL=0 `ZwXxx` call skips user-buffer probe; CPL=3 call through same index validates pointer
- [ ] Syscall audit hook captures service numbers for all calls during test run
- [ ] Per-process syscall filter blocks `NtWriteFile` for child process; parent unaffected
- [ ] `KeUserModeCallback` → user-mode callback → `NtCallbackReturn` round-trip succeeds
- [ ] SSDT page write after init triggers `CRITICAL_STRUCTURE_CORRUPTION` BugCheck
- [ ] All 4 platforms: QEMU WHPX, QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: nt -- native API layer complete"`
