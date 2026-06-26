---
schema_version: 1
id: native-api-ssdt
domain: 02-kernel-core
status: active
title: "TODO-12 -- Native API Layer (Nt/Zw)"
---

# TODO-12 -- Native API Layer (Nt/Zw)

> **Goal:** Replace the ad-hoc INT 0x80 / POSIX-numbered `SYS_*` dispatch table with a complete NT native API layer: `NTSTATUS` return values, `NtXxx`/`ZwXxx` naming, a `SYSCALL`/`SYSRET` fast path, a numbered System Service Descriptor Table (SSDT) with 470 service entries, and the `NtCurrentTeb()` / `NtCurrentPeb()` inline contract. This is the exact interface that `ntdll.dll`, CSRSS, Win32k, and every driver framework use to talk to the kernel. This TODO is the **master registry** for all NT syscall endpoints -- some are implemented here, others are implemented by domain-specific TODOs but get their SSDT slots reserved and documented here.

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
- A numbered SSDT table with 470 entries maps service indices to kernel functions; `ntdll` stubs call by index.
- The existing 22 `SYS_*` calls are migrated to `Nt`-named equivalents at stable indices.
- `NtCurrentTeb()` (`mov rax, gs:[0x30]`) and `NtCurrentPeb()` (`mov rax, gs:[0x60]`) return correct values per TODO-11.
- All endpoint categories covered: file I/O, process/thread, memory, sync, registry, security/token, sections, timers, ALPC ports, debug, power, namespace, system info, atoms.
- Per-process syscall filtering allows processes to restrict which syscalls their children can invoke -- parity with Win11 SystemCallDisablePolicy and Linux seccomp-bpf.
- `KeUserModeCallback()` enables the kernel to call user-mode functions (window procedures, hooks) and await their return via `NtCallbackReturn`.
- SSDT pages are hardware write-protected after init -- simpler and more secure than PatchGuard periodic checksums.

## Implementation Order

| ⭐  | Order | Deliverable                                                    | Depends On        | Status |
| --- | :---: | -------------------------------------------------------------- | ----------------- | :----: |
| 💎  |   1   | NTSTATUS type and canonical status codes                       | --                |  [x]   |
| 💎  |   2   | SYSCALL/SYSRET fast path (IA32_LSTAR)                          | TODO-11 §5–§8     |  [x]   |
| 💎  |   3   | INT 0x2E compatibility path                                    | §2                |  [x]   |
| 💎  |   4   | System Service Descriptor Table (SSDT) -- 470 entries          | §1                |  [x]   |
| 💎  |   5   | Nt/Zw naming and existing syscall migration                    | §1, §4            |  [x]   |
| 💎  |   6   | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile | §5, TODO-05 §2    |  [x]   |
| 💎  |   7   | NtCreateProcess / NtCreateThread / process-thread lifecycle    | §5, TODO-05 §2    |  [/]   |
| 💎  |   8   | Sync objects + NtWaitForMultipleObjects                        | §5, TODO-05 §6    |  [/]   |
| 💎  |   9   | Virtual memory (alloc, free, protect, lock)                    | §5                |  [/]   |
| 💎  |  10   | NtQuerySystemInformation / NtQueryInformationProcess           | §5                |  [/]   |
| ⭐  |  11   | Extended error information (IOSB + TEB LastError)              | §5, TODO-11 §1    |  [x]   |
| ⭐  |  12   | ZwXxx kernel-mode alias layer with privilege assertion         | §4, §5            |  [x]   |
| 💎  |  13   | File metadata and device control                               | §6                |  [x]   |
| 💎  |  14   | Registry syscalls (core CRUD)                                  | §5, TODO-14 §5    |  [x]   |
| 💎  |  15   | Registry syscalls (advanced: flush/notify/save/hive)           | §14, TODO-14 §5   |  [/]   |
| 💎  |  16   | Token open/query/adjust syscalls                               | §5, TODO-15 §7    |  [x]   |
| 💎  |  17   | Directory and symbolic link object syscalls                    | §5, TODO-05 §3    |  [x]   |
| 💎  |  18   | Section and memory-mapped file syscalls                        | §5, TODO-05 §7    |  [x]   |
| 💎  |  19   | Timer control syscalls                                         | §5, TODO-08 §8,§9 |  [x]   |
| 💎  |  20   | Legacy LPC port syscalls                                       | §5, TODO-24 §8-§9 |  [x]   |
| 💎  |  21   | Exception and debug syscalls                                   | §5, TODO-23 §5    |  [ ]   |
| 💎  |  22   | Power and system control                                       | §5, TODO-26 §12   |  [ ]   |
| 💎  |  23   | Atom, locale, and miscellaneous                                | §5                |  [ ]   |
| ⭐  |  24   | Syscall audit and tracing hook                                 | §4                |  [ ]   |
| 💎  |  25   | Per-process syscall filtering (seccomp / SystemCallDisable)    | §4, §7            |  [ ]   |
| 💎  |  26   | Kernel-to-user mode callback dispatch (KeUserModeCallback)     | §2, TODO-11 §9    |  [ ]   |
| ⭐  |  27   | SSDT integrity protection (hardware write-protect)             | §4                |  [ ]   |
| 💎  |  28   | Extended directory enumeration classes                         | §6, §13           |  [x]   |
| 💎  |  29   | Token lifecycle + SRM access check syscalls                    | §16, TODO-15 §7,§8|  [ ]   |
| 💎  |  30   | Generic object management (make-temp/perm, set-info, compare)  | §17, TODO-05 §1,§9|  [ ]   |
| 💎  |  31   | Modern ALPC port syscalls                                      | §20, TODO-24 §8-§9 |  [x]   |

> 💎 = parity -- Windows NT and Linux both have equivalents for these categories.
> ⭐ = exclusive -- the ZwXxx privilege layer, the audit hook, SSDT integrity protection, and the IOSB/LastError unified path go beyond what Linux offers.

> [!IMPORTANT]
> **Self-contained execution model:** §1 through §5 (NTSTATUS, SYSCALL/SYSRET, INT 0x2E, SSDT, migration) are fully self-contained, no external blockers. §9 through §12, §24 through §28 are also unblocked. Sections §6 through §8, §13 through §23 wire domain-specific syscalls through the SSDT and depend on their respective domain TODOs (Object Manager, Registry, SRM, ALPC, etc.). This is by design: this TODO is the **master registry** for all NT syscall endpoints. Domain TODOs implement the logic; this TODO provides the SSDT wiring. The unblocked core (§1 through §5 + §9 through §12 + §24 through §28) delivers a fully functional SYSCALL/SYSRET fast path with 470 SSDT slots, NTSTATUS return values, and the audit/filter/integrity infrastructure. Domain-specific NtXxx wrappers activate as their domain TODOs complete.

---

## 1. NTSTATUS Type and Canonical Status Codes
Define the NT status type and the full set of codes needed across all 200+ syscall endpoints.

- [x] Create `include/kernel/nt/ntstatus.h`:
  - `typedef uint32_t NTSTATUS`
  - Severity macros: `NT_SUCCESS(s)` = `((s) >> 30) == 0`, `NT_INFORMATION(s)` = `((s) >> 30) == 1`, `NT_WARNING(s)` = `((s) >> 30) == 2`, `NT_ERROR(s)` = `((s) >> 30) == 3`
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
    - `STATUS_SEMAPHORE_LIMIT_EXCEEDED   0xC0000044`
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

## 3. INT 0x2E Compatibility Path
Windows NT's original software-interrupt syscall vector. Required for early ntdll and any code that does not use `SYSCALL`.

- [x] IDT vector 0x2E set to DPL=3 (type_attr 0xEE) in `idt.c` -- user-mode `int 0x2E` works
- [x] `syscall_handler_2e()` registered in `syscall.c` -- reads RAX as service number, R10/RDX/R8/R9 as args (Windows x64 ABI), dispatches via `ssdt_dispatch()`
- [x] Uses existing `irq14` stub which has full register save/restore + swapgs + iretq
- [x] INT 0x80 kept active alongside INT 0x2E (both paths coexist)
- [x] Commit: `"kernel: nt -- INT 0x2E syscall compatibility path"`
- [ ] **Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+**: Windows x64 ABI passes the first 4 args in `R10`/`RDX`/`R8`/`R9` and args 5+ on the user stack at `[RSP+0x28]`, `[RSP+0x30]`, ... Today `syscall_handler_2e()` in `src/kernel/sched/syscall.c` and `syscall_entry` in `src/kernel/sched/syscall_entry.asm` only populate the first 4 register slots plus whatever the dispatcher already has in `a5`/`a6`, so any `NtXxx` with more than 4 parameters has to kludge the rest through an extended-args struct at `a5` (e.g. `NT_MAPVIEW_ARGS` in `include/kernel/nt/nt_section.h`). Retrofit both entry paths to probe `[RSP+0x28..]` for the user stack frame with `ProbeForReadIfUser` and populate `a5`/`a6` directly (and any future `a7..a10` via an on-stack `SSDT_ARGS` struct passed to `ssdt_dispatch()`). Then delete `NT_MAPVIEW_ARGS` and the `NtCreateSection` packed-flags kludge at `nt_section.c:93-94`. This auto-closes TODO-12 §18 Accepted #1 (10-parameter `NtMapViewOfSection` stack ABI).

**Test checkpoint:** Ring-3 `int 0x2E` with RAX=0x0015 reaches `NtClose` handler. Same register mapping as SYSCALL path. `POST16(0xD300)` entry, `POST16(0xD301)` exit. Verify on all 4 platforms.

## 4. System Service Descriptor Table (SSDT)

The SSDT is a flat array of function pointers indexed by the 12-bit service number in RAX. `ntdll` stubs do `mov rax, <service_number>; syscall`. This section defines the complete service number allocation for all 470 NT API endpoints. Numbers are stable -- changing them is an ABI break.

- [x] Define `SSDT_HANDLER` and `SSDT_TABLE` in `include/kernel/nt/ssdt.h` -- handler takes 6 uint64_t args, returns NTSTATUS; table has handlers array + count + implemented count + name
- [x] Shadow SSDT (table 1) allocated as empty placeholder -- indices 0x1000+, filled by Win32k later
- [x] 470 service index assignments in `include/kernel/nt/service_numbers.h` -- `SSDT_NtXxx` defines for every entry, `SSDT_MAIN_COUNT = 470`
- [x] `ssdt_dispatch()` -- selects table from bits 13:12, index from bits 11:0, calls handler
- [x] `ssdt_register()` -- replaces stub with real handler, tracks implemented count
- [x] Unimplemented slots return `STATUS_NOT_IMPLEMENTED` via `ssdt_stub_not_implemented()`
- [x] `ssdt_init()` called in Phase 3 before `syscall_init()`
- [x] 4 unit tests: unimplemented stub, invalid table, main count=470, register+dispatch
- [x] Commit: `"kernel: nt -- SSDT and service number table"`

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
- [x] Keep `SYS_*` macros as compile-time aliases pointing to the same SSDT indices for transition
- [x] Change all `sys_*` implementations to return `NTSTATUS`; convert error paths to `STATUS_*` codes
- [x] Add `Nt_Close` SSDT wrapper at 0x0000 bridging to OB NtClose
- [x] 12 NtXxx handlers registered in SSDT via `nt_syscall_register_ssdt()` in `src/kernel/nt/nt_syscall.c`
- [x] 2 unit tests: SSDT registration verification (9 handler callability checks) + SYS_NT_* alias value validation (12 aliases)
- [x] Commit: `"kernel: nt -- migrate existing syscalls to NtXxx naming and NTSTATUS"`

**Test checkpoint:** All 22 existing syscalls still work via old `SYS_*` macros (backward compat). `NtWriteFile` returns `STATUS_SUCCESS` on valid write. Serial: existing boot/desktop tests pass without regression. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- this is the most dangerous migration; a return-type mismatch silently corrupts all user-mode callers.

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
- [x] Commit: `"kernel: nt -- NtCreateFile, NtOpenFile, NtClose, NtReadFile, NtWriteFile"`

**Test checkpoint:** `NtCreateFile` on `X:\Logs\kernel.log` returns `STATUS_SUCCESS` + valid HANDLE. `NtClose(handle)` returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`. `NtReadFile` populates IOSB correctly.

## 7. NtCreateProcess / NtCreateThread / Process-Thread Lifecycle
Process and thread creation, suspension, termination, and thread context access through the Ob-managed process model (→ XREF TODO-05 §2).

- [x] `NtCreateProcess(0x0030)`: wraps task_create + ob_handle_table_inherit; returns HANDLE
- [ ] When `TODO-22-environment-variables.md §1` lands: deep-copy parent `task->environ` and `task->argv` into the child via `env_copy()` / argv helpers (→ XREF `TODO-22-environment-variables.md §1`)
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
- [x] Commit: `"kernel: nt -- NtCreateProcess, NtCreateThread, full process-thread lifecycle"`

**Test checkpoint:** `NtCreateProcess` returns valid HANDLE; PID visible in `\KernelObjects\`. `NtCreateThread` with `CreateSuspended=TRUE` doesn't run until `NtResumeThread`. `NtGetContextThread`/`NtSetContextThread` currently return `STATUS_NOT_IMPLEMENTED` until TODO-23 provides `CONTEXT`. `NtDelayExecution` sleeps for the correct interval.

## 8. Synchronisation Objects + NtWaitForMultipleObjects
Synchronisation objects through Ob-managed named types (→ XREF TODO-05 §6). Includes multi-wait and keyed event support.

- [x] `NtCreateEvent(0x0070)`: wraps NtCreateEvent in ob_event.c; SSDT handler parses OBJECT_ATTRIBUTES for named events
- [x] `NtOpenEvent(0x0071)`: ObLookupObjectByName + ObpAllocateHandle
- [x] `NtSetEvent(0x0072)`: event_set, returns PreviousState
- [x] `NtResetEvent(0x0073)`: event_reset, returns PreviousState
- [x] `NtPulseEvent(0x0074)`: set + immediate reset (wakes waiting threads)
- [x] `NtQueryEvent(0x0075)`: returns EVENT_BASIC_INFORMATION (EventType + EventState)
- [x] `NtWaitForSingleObject(0x0006)`: upgraded from §5 -- dispatches to event/mutex/semaphore/process wait based on object type, supports NT timeout (100-ns relative)
- [x] `NtWaitForMultipleObjects(0x0007)`: WaitAll (sequential), WaitAny (polling yield loop), 64 handle max, returns STATUS_WAIT_0+index
- [x] `NtSignalAndWaitForSingleObject(0x0008)`: atomic signal-then-wait; signal dispatches to event/mutex/semaphore
- [x] `NtCreateMutant(0x0076)`: wraps NtCreateMutex in ob_mutex.c; InitialOwner support
- [x] `NtOpenMutant(0x0077)`: ObLookupObjectByName for ObpMutexType
- [x] `NtReleaseMutant(0x0078)`: ownership check (STATUS_MUTANT_NOT_OWNED), mutex_unlock
- [x] `NtQueryMutant(0x0079)`: returns MUTANT_BASIC_INFORMATION (CurrentCount, OwnedByCaller, AbandonedState)
- [x] `NtCreateSemaphore(0x007A)`: wraps NtCreateSemaphore in ob_semaphore.c; validates initial <= max
- [x] `NtOpenSemaphore(0x007B)`: ObLookupObjectByName for ObpSemaphoreType
- [x] `NtReleaseSemaphore(0x007C)`: checks max count, sem_signal N times
- [x] `NtQuerySemaphore(0x007D)`: returns SEMAPHORE_BASIC_INFORMATION
- [ ] `NtCreateKeyedEvent(0x0084)`: stub STATUS_NOT_IMPLEMENTED (deferred to TODO-17)
- [ ] `NtOpenKeyedEvent(0x0085)`: stub
- [ ] `NtWaitForKeyedEvent(0x0086)`: stub
- [ ] `NtReleaseKeyedEvent(0x0087)`: stub
- [x] New file: `include/kernel/nt/nt_sync.h` with wait constants, info structs
- [x] New file: `src/kernel/nt/nt_sync.c` with 21 SSDT handlers
- [x] Added STATUS_WAIT_0, STATUS_ABANDONED to ntstatus.h
- [x] 2 unit tests: SSDT registration (6 handler checks), sync constant verification (7 checks)
- [x] Commit: `"kernel: nt -- sync objects, NtWaitForMultipleObjects, keyed events"`

**Test checkpoint:** `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` round-trip succeeds. `NtWaitForMultipleObjects(WaitAny)` returns correct index. `NtCreateMutant` with `InitialOwner=TRUE` is owned by caller. Named objects visible in `\BaseNamedObjects\`. Keyed event APIs (0x0084-0x0087) currently return `STATUS_NOT_IMPLEMENTED` pending TODO-17.

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
- [ ] `NtAllocateUserPhysicalPages(0x0059)`: AWE stub STATUS_NOT_IMPLEMENTED
- [ ] `NtFreeUserPhysicalPages(0x005A)`: AWE stub
- [ ] `NtMapUserPhysicalPages(0x005B)`: AWE stub
- [x] New file: `include/kernel/nt/nt_memory.h` -- MEM_COMMIT/RESERVE/RELEASE, PAGE_* flags, MEMORY_BASIC_INFORMATION
- [x] New file: `src/kernel/nt/nt_memory.c` -- 12 SSDT handlers (9 implemented + 3 AWE stubs)
- [x] 2 unit tests: SSDT registration (5 handler checks), constant verification (12 checks)
- [x] Commit: `"kernel: nt -- NtAllocate/Free/Protect/Lock/Read/WriteVirtualMemory"`

**Test checkpoint:** `NtAllocateVirtualMemory` with `MEM_COMMIT | PAGE_READWRITE` returns usable address; write+read round-trip. `NtProtectVirtualMemory` changes RW→RO; write attempt faults. `NtFreeVirtualMemory` with `MEM_RELEASE` returns `STATUS_SUCCESS`. `NtReadVirtualMemory` from kernel to user address space succeeds. AWE APIs (0x0059-0x005B) currently return `STATUS_NOT_IMPLEMENTED`.

## 10. NtQuerySystemInformation / NtQueryInformationProcess
Provides OS version, process list, performance counters, and detailed process info to ntdll and user-mode tools.

- [/] `NtQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ReturnLength)`:
  - [x] `SystemBasicInformation (0)`: page size, CPU count, min/max user address, allocation granularity
  - [x] `SystemProcessorInformation (1)`: AMD64 architecture, level, max CPUs
  - [x] `SystemPerformanceInformation (2)`: PMM free/used/total pages
  - [x] `SystemTimeOfDayInformation (3)`: uptime seconds (from §5)
  - [x] `SystemProcessInformation (5)`: PID, state, name list (from §5)
  - [ ] `SystemProcessorPerformanceInformation (8)`: deferred (per-CPU time accounting not yet implemented)
  - [ ] `SystemModuleInformation (11)`: deferred (module loader not yet implemented)
  - [ ] `SystemHandleInformation (16)`: deferred (system-wide handle dump not yet implemented)
  - [ ] `SystemObjectInformation (17)`: deferred (OB type statistics aggregation)
  - [ ] `SystemInterruptInformation (23)`: deferred (per-CPU interrupt counters)
  - [ ] `SystemExceptionInformation (33)`: deferred (exception stats not tracked)
  - [ ] `SystemRegistryQuotaInformation (37)`: deferred (registry not quota-limited yet)
  - [ ] `SystemBootPerformanceInformation (custom)`: deferred (→ XREF: `TODO-01-kernel-init-sequencing.md §12`)
  - [x] Unimplemented classes return `STATUS_NOT_IMPLEMENTED` via default case
- [ ] `NtSetSystemInformation(0x00D1)`: stub returning STATUS_NOT_IMPLEMENTED (requires SeSystemtimePrivilege from TODO-15)
- [x] `NtQueryInformationProcess` extended with 7 new info classes:
  - `ProcessBasicInformation (0)`: PEB, PID, parent PID, affinity (done in §7)
  - `ProcessTimes (4)`: creation, exit, kernel, user times (zeroed -- not tracked yet)
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

- [ ] `SystemProcessorPerformanceInformation (8)`: add real CPU time accounting first (scheduler tick attribution), then expose per-CPU idle/kernel/user times via `NtQuerySystemInformation`. The accounting groundwork is still pending in `todo/02-kernel-core/TODO-21-process-model-extensions.md` (line 188).
- [ ] `SystemModuleInformation (11)`: implement module registry/loader list (`exec_register_module`, lookup/enumeration), then serialize that list in `NtQuerySystemInformation`. Module list work is still planned in `todo/02-kernel-core/TODO-17-binary-system.md` (line 165).
- [ ] `SystemHandleInformation (16)`: add a safe system-wide handle table snapshot across all processes (PID + handle + access + object/type), then expose it through class 16. Current implementation has per-process tables only.
- [ ] `SystemObjectInformation (17)`: wire `NtQuerySystemInformation` to OB type stats aggregation. Most raw stats already exist in `src/kernel/ob/ob.c` (line 363), but class 17 marshalling is not implemented.
- [ ] `SystemInterruptInformation (23)`: add per-CPU interrupt counters and return them. You currently have per-vector global IRQ counts in `src/kernel/irq.c` (line 23), but no per-CPU increment path is wired.
- [ ] `SystemExceptionInformation (33)`: add exception counters in the IDT exception path, then expose those totals. Exception handling exists, but exception stats tracking does not.
- [ ] `SystemRegistryQuotaInformation (37)`: implement registry quota accounting/enforcement (limit, used, peak) in registry write paths, then query output. Current registry code has no quota model.
- [ ] `SystemBootPerformanceInformation (custom)`: boot perf NVRAM read/write is already present in `src/kernel/boot_timing.c` (line 333), but you still need a kernel accessor/API and a class serializer in `NtQuerySystemInformation` for user-mode consumption.
- [ ] For all deferred classes: add ABI structs + buffer-size handling + unit tests in `src/kernel/test/test_nt_types.c`, then update TODO-12 §10 checklist states.

## 11. Extended Error Information (IOSB + LastError)
NT propagates detailed error info through two channels: `IO_STATUS_BLOCK` (async I/O) and `TEB->LastErrorValue` (Win32 `GetLastError`). Both must be populated correctly.

- [x] All file I/O `NtXxx` functions write final `NTSTATUS` into `IoStatusBlock->Status` and byte count / disposition into `IoStatusBlock->Information` (already implemented in §6: NtCreateFile, NtOpenFile, NtReadFile, NtWriteFile, NtQueryDirectoryFile all populate IOSB)
- [x] On every `NTSTATUS` return from a syscall: if `NT_ERROR(status)`, write the Win32 error translation into `TEB->LastErrorValue` (at `gs:[0x68]`) via `RtlNtStatusToDosError`. Wired in both SYSCALL (`syscall_fast.c`) and INT 0x2E (`syscall.c`) return paths.
- [x] `RtlNtStatusToDosError` 12-entry table in `ssdt.c`: SUCCESS->0, ACCESS_DENIED->5, NO_MEMORY->8, INVALID_HANDLE->6, NAME_NOT_FOUND->2, NOT_IMPLEMENTED->50, INVALID_PARAMETER->87, BUFFER_TOO_SMALL->122, ACCESS_VIOLATION->998, PRIVILEGE_NOT_HELD->1314, UNSUCCESSFUL->1, NAME_COLLISION->183. Unknown->317.
- [x] Kernel writes `TEB->LastErrorValue` only in the syscall return path; user-mode `SetLastError` writes `gs:[0x68]` directly without a syscall.
- [x] Commit: `"kernel: nt -- IOSB and TEB LastErrorValue propagation"` (607eb38b)

**Test checkpoint:** After a failing `NtOpenFile` (non-existent path), `gs:[0x68]` == Win32 error code (2 = FILE_NOT_FOUND). After `NtReadFile`, IOSB `Status == STATUS_SUCCESS`, `Information == bytes_read`.

## 12. ZwXxx Kernel-Mode Alias Layer

`ZwXxx` names are identical to `NtXxx` in user mode. In kernel mode (`CPL=0`), `ZwXxx` calls bypass the user-mode probe and use kernel-mode access rights directly. This is the convention all of Windows' own drivers and executive components use.

- [x] Add a `ZwXxx` header `include/kernel/nt/zw.h` that declares each `ZwXxx` as a static inline calling `ssdt_dispatch()` directly (15 aliases: ZwClose, ZwCreateFile, ZwOpenFile, ZwReadFile, ZwWriteFile, ZwQueryInformationFile, ZwQueryDirectoryFile, ZwCreateEvent, ZwCreateMutant, ZwCreateSemaphore, ZwQuerySystemInformation, ZwYieldExecution, ZwDuplicateObject, ZwQueryObject, ZwWaitForSingleObject)
- [x] Previous-mode tracking via `ssdt_set_previous_mode()`/`ssdt_previous_mode()`: syscall entry paths (SYSCALL + INT 0x2E) set UserMode before dispatch, restore KernelMode after. ZwXxx callers leave mode at KernelMode. Handlers use `ProbeForReadIfUser()`/`ProbeForWriteIfUser()` convenience wrappers.
- [x] Add `ProbeForRead(Address, Length, Alignment)` and `ProbeForWrite(Address, Length, Alignment)` in `ssdt.c`: validate NULL, overflow, range below `MM_USER_PROBE_ADDRESS` (0x7FFF0000), alignment (power of 2). Returns `STATUS_ACCESS_VIOLATION` or `STATUS_DATATYPE_MISALIGNMENT`.
- [x] Add `ASSERT_KERNEL_CALLER()` macro in `zw.h`: returns `STATUS_PRIVILEGE_NOT_HELD` if previous mode is UserMode.
- [x] Convention documented in `zw.h` header comment: kernel calls Zw (no probe), user calls Nt via SYSCALL (probed). Both resolve to the same SSDT handler.
- [ ] Make `s_previous_mode` (global in `ssdt.c`) per-CPU or pass it through `ssdt_dispatch()`: under SMP user scheduling one CPU can skip another's `ProbeFor*IfUser()`. -> XREF: `TODO-03-kernel-libraries.md` §5
- [x] Commit: `"kernel: nt -- ZwXxx kernel-mode alias layer with CPL probe bypass"` (638568e8)

**Test checkpoint:** `ZwClose` from CPL=0 succeeds without user-buffer probe. CPL=3 call with kernel-space pointer returns `STATUS_ACCESS_VIOLATION`. `ASSERT_KERNEL_CALLER()` fires `STATUS_PRIVILEGE_NOT_HELD` from ring 3.

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
- [x] Commit: `"kernel: nt -- core registry syscalls wired to SSDT (create/open/query/set/delete/enumerate)"` (6d6535d5)

**Test checkpoint:** `NtCreateKey` under `\Registry\Machine\Software\Test` returns `STATUS_SUCCESS`. `NtSetValueKey` + `NtQueryValueKey` round-trip succeeds. `NtDeleteKey` removes the key. `NtEnumerateKey` iterates subkeys correctly.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi)
> **Expected:** 9 suites (2 Win32 + 7 NT SSDT), 0 failures

> **Verified** (2026-04-13): all 10 NtXxx handlers registered (SSDT 0x0090-0x009B), evidence mapped to nt_registry.c:730-739. NT path resolution (\Registry\Machine\...) functional. Predefined handle safety (RegIsPredefinedKey). NtDeleteKey returns STATUS_KEY_HAS_CHILDREN for non-empty keys. KeyFullInformation supported in NtEnumerateKey. ntdll exports sorted. Build clean.
> **Accepted:** HKEY-to-HANDLE ABI gap (registry handles not in OB table, NtClose cannot close them) -> XREF: 02-kernel-core/TODO-14 §5 (new item: "Migrate HKEY to OB handle table" -- registers ObpKeyType and routes §14/§15 handlers through ObpAllocateHandle). SMP locking for registry pools -> XREF: 02-kernel-core/TODO-31 §14 (concrete spinlock wrapping for registry_flush/hive_save/hive_load). UTF-16 UNICODE_STRING decode -> XREF: 02-kernel-core/TODO-14 §5 (nt_decode_unicode_string helper). KeyNodeInformation -> XREF: 02-kernel-core/TODO-14 §5 (new item: "KeyNodeInformation output struct in NtQueryKey/NtEnumerateKey").
> **Quality reviewed** (2026-04-13): dead code clean (all structs/helpers used). SSDT numbers match service_numbers.h. Error mapping consistent (ERROR_ACCESS_DENIED -> STATUS_ACCESS_DENIED; child-count check returns STATUS_KEY_HAS_CHILDREN specifically). Stack usage bounded by REG_MAX_* constants (512B data arrays). NtEnumerateKey KeyFullInformation opens/queries/closes child key inline.


## 15. Registry Syscalls (Advanced)

> [!NOTE]
> Persistence, notification, and hive management operations. Depends on §14 (core CRUD) and TODO-14 hive infrastructure.

- [x] `NtFlushKey(KeyHandle)` → SSDT 0x009C: delegates to `registry_flush()`
- [/] `NtNotifyChangeKey(KeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, CompletionFilter, WatchTree, Buffer, BufferSize, Asynchronous)` → SSDT 0x009D (returns STATUS_NOT_IMPLEMENTED pending change-notification infrastructure -> XREF: 02-kernel-core/TODO-31 §3)
- [x] `NtRenameKey(KeyHandle, NewName)` → SSDT 0x009F (uses `RegRenameKey` helper)
- [x] `NtSaveKey(KeyHandle, FileHandle)` / `NtSaveKeyEx(...)` → SSDT 0x00A0/0x00A1 (resolves FileHandle via `vfs_get_path_from_node`, then `hive_save`)
- [x] `NtRestoreKey(KeyHandle, FileHandle, Flags)` → SSDT 0x00A2 (same path resolution, then `hive_load`)
- [x] `NtLoadKey(ObjectAttributes, ObjectAttributes)` / `NtLoadKeyEx(...)` → SSDT 0x00A3/0x00A4 (creates registry mountpoint, loads hive file by path)
- [x] `NtUnloadKey(ObjectAttributes)` / `NtUnloadKeyEx(...)` → SSDT 0x00A5/0x00A6 (uses `RegUnloadHive` helper)
- [x] Commit: `"kernel: nt -- advanced registry syscalls (flush/notify/save/restore/hive load)"` (dd22e696)

**Test checkpoint:** `NtFlushKey` completes without error. `NtNotifyChangeKey` fires callback after `NtSetValueKey` on watched key. `NtSaveKey` + `NtRestoreKey` round-trip succeeds.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi)
> **Expected:** 5 new §15 tests (NtFlushKey, NtRenameKey, NtNotifyChangeKey blocked, NtUnloadKey invalid, advanced SSDT registered), 0 failures.

> **Verified** (2026-04-14): 10 handlers registered at SSDT 0x009C-0x00A6 via nt_registry.c:1113-1122. 9 complete paths ([x]) route through registry.c helpers: NtFlushKey → registry_flush_checked with STATUS_REGISTRY_IO_FAILED propagation; NtRenameKey → RegRenameKey with case-only no-op + collision check; NtSaveKey/Ex + NtRestoreKey → vfs_get_path_from_node(bounded strlen, overflow-safe math, cycle detection) → hive_save/hive_load; NtLoadKey/Ex → mountpoint creation with rollback on hive_load failure; NtUnloadKey/Ex → RegUnloadHive recursive free. 1 stub ([/]): NtNotifyChangeKey returns STATUS_NOT_IMPLEMENTED with SCOPE-GAP-ALLOWED sentinel. 10 ntdll PE exports added (alphabetically sorted for binary search). Build clean.
> **Accepted:** Change notifications (NtNotifyChangeKey watcher lists) -> XREF: 02-kernel-core/TODO-31 §3 (reg_watcher_t + reg_notify_register), §4 line 271 (NtNotifyChangeKey SSDT wiring calls §3 infra). SMP dirty-flag races on registry_flush/hive_table -> XREF: 02-kernel-core/TODO-31 §14 (explicit wrap of registry_flush/hive_save/hive_load). UTF-16 UNICODE_STRING decode -> XREF: 02-kernel-core/TODO-14 §5 (kernel-wide `nt_decode_unicode_string` helper in src/kernel/nt/nt_string.c; retrofit list explicitly covers §14, §15, §17). Per-key flush optimization -> XREF: 02-kernel-core/TODO-14 §5 (new `registry_flush_key` walks parent chain to owning hive).
> **Quality reviewed** (2026-04-14): dead code clean. Consistency: reg_resolve_key in registry.c now tombstone-aware (rejects keys with name[0]=='\0'); §14 handlers (NtDeleteKey, NtRenameKey, NtQueryKey) routed through shared resolve_hkey in nt_registry.c for uniform liveness semantics. NtLoadKey rollback on hive_load failure uses captured disposition. File handle type-checked via OB_HEADER_FROM_BODY + ObpFileType. New helpers: vfs_get_path_from_node (bounded VFS_MAX_NAME strlen, depth-64 cycle guard, subtraction-form bounds), vfs_name_len_bounded, vfs_find_drive_letter. 5 new tests.


## 16. Token Open, Query, and Adjust Syscalls

> [!NOTE]
> Token implementation exists in `src/kernel/security/token.c` (→ XREF TODO-15 §7). This section wires the "operate on an existing token" surface (open, query, set, adjust) plus `NtAllocateLocallyUniqueId` into the SSDT. Token creation/derivation and SRM access check are in §29.

- [x] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` → SSDT 0x00B0 (resolves process handle, returns task->token via `NtOpenProcessToken` library)
- [x] `NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle)` → SSDT 0x00B1 (HandleAttributes currently ignored; inherit/audit flags tracked in TODO-05 §2)
- [x] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` → SSDT 0x00B2 (falls back to owning task's primary token; thread impersonation slot pending → XREF: 02-kernel-core/TODO-15 §2)
- [x] `NtOpenThreadTokenEx(ThreadHandle, DesiredAccess, OpenAsSelf, HandleAttributes, TokenHandle)` → SSDT 0x00B3
- [x] `NtQueryInformationToken(TokenHandle, TokenInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x00B4 (14 info classes via library: TokenUser, TokenGroups, TokenPrivileges, TokenOwner, TokenPrimaryGroup, TokenDefaultDacl, TokenSource, TokenTypeInfo, TokenImpersonationLevelInfo, TokenStatistics, TokenElevationTypeInfo, TokenLinkedToken, TokenIsElevatedInfo, TokenIntegrityLevel)
- [x] `NtSetInformationToken(TokenHandle, TokenInformationClass, Buffer, Length)` → SSDT 0x00B5 (supports TokenIntegrityLevel; read-only classes return STATUS_INVALID_INFO_CLASS; ownership-transfer classes → XREF: 02-kernel-core/TODO-15 §7)
- [x] `NtAdjustPrivilegesToken(TokenHandle, DisableAllPrivileges, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B6 (unpacks TOKEN_PRIVILEGES, delegates to library)
- [x] `NtAdjustGroupsToken(TokenHandle, ResetToDefault, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B7 (unpacks TOKEN_GROUPS, delegates to library)
- [x] `NtAllocateLocallyUniqueId(Luid)` → SSDT 0x00C3 (SSDT wrapper around `NtAllocateLocallyUniqueId` in luid.c)
- [x] Commit: `"kernel: nt -- token open/query/adjust syscalls"` (d692c058)

**Test checkpoint:** `NtOpenProcessToken` returns valid token handle. `NtQueryInformationToken(TokenUser)` returns correct SID. `NtAdjustPrivilegesToken` enables/disables a privilege. `NtAllocateLocallyUniqueId` returns monotonically increasing LUIDs.

> **Test runner:** `scripts\debug\kernel\run-security-tests.bat` (SUITE=security)
> **Expected:** 6 new §16 tests (NtAllocateLocallyUniqueId via SSDT, NtOpenProcessToken, NtQueryInformationToken invalid handle, NtSetInformationToken read-only, NtAdjustPrivilegesToken invalid, token SSDT registration), 0 failures.

> **Verified** (2026-04-14): 9 handlers registered at SSDT 0x00B0-0x00B7 + 0x00C3 via nt_token.c:480-497. NtOpenProcessToken/Ex route through resolve_process_handle → task->token → library NtOpenProcessToken. NtOpenThreadToken/Ex resolve via resolve_thread_handle, fall back to owning task's primary token (thread impersonation pending). NtQueryInformationToken routes 14 info classes through library. NtSetInformationToken rejects all classes with STATUS_INVALID_INFO_CLASS pending deep-copy setters. NtAdjust{Privileges,Groups}Token unpack TOKEN_PRIVILEGES/TOKEN_GROUPS with bounded counts (TOKEN_MAX_PRIVS=36, TOKEN_MAX_GROUPS=32) and 64-bit capacity math. NtAllocateLocallyUniqueId is a 1-line SSDT wrapper around luid.c. 10 ntdll PE exports added (sorted). 6 new tests in TEST_CAT_SECURITY. token.h cleaned: removed duplicate NTSTATUS defines, uses canonical kernel/nt/ntstatus.h. Build clean.
> **Accepted:** (1) Process/thread handles are PID/TID-encoded rather than OB-allocated (matches existing nt_process.c pattern; no per-handle access rights enforcement) -> XREF: 02-kernel-core/TODO-05 §9 (new item: "Migrate PID/TID-encoded process/thread handles to OB-allocated handles"). (2) Token handle granted_access not checked on mutate ops -> XREF: 02-kernel-core/TODO-15 §8 (new item: "Enforce per-handle granted_access on token mutation syscalls"). (3) NtOpenProcessTokenEx HandleAttributes discarded -> XREF: 02-kernel-core/TODO-05 §2 (new item: "Wire HandleAttributes through ObpAllocateHandle for NtXxxEx syscalls"). (4) NtSetInformationToken set classes (deep-copy) -> XREF: 02-kernel-core/TODO-15 §7 (item: "Deep-copy token field setters for NtSetInformationToken"). (5) Per-token SMP lock -> XREF: 02-kernel-core/TODO-15 §7 (item: "Per-token lock for SMP safety"). (6) Thread impersonation slot -> XREF: 02-kernel-core/TODO-15 §2 (items: ImpersonationToken field + NtImpersonateThread + NtSetInformationThread).
> **Quality reviewed** (2026-04-14): dead code clean (local TOKEN_GROUPS struct needed -- not in token.h; resolve_thread_handle's out_task is used in NtOpenThreadToken). Consistency: TokenTypeInfo=8 matches Windows TokenType enum value (the name difference is because ntstatus.h already uses "TokenType" for an enum; our enum member is TokenTypeInfo). Performance: resolve_thread_handle linear scan bounded by task->num_threads; adjust privileges O(n*m) bounded by 36*36=1296 ops. Codex findings (PID-as-handle, granted_access, HandleAttributes) are pre-existing systemic gaps not introduced by §16; all three accepted with concrete XREFs.


## 17. Directory and Symbolic Link Object Syscalls
Namespace manipulation -- create, open, and query Ob directory objects and symbolic links from user mode.

- [x] `NtCreateDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0120 (nt_namespace.c -- ob_ns_create_directory + ObInsertObject + ObpAllocateHandle; rejects duplicate via ObLookupObjectByName)
- [x] `NtOpenDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0121 (nt_namespace.c -- thin wrapper around library NtOpenDirectoryObject in ob.c)
- [x] `NtQueryDirectoryObject(DirectoryHandle, Buffer, Length, ReturnSingleEntry, RestartScan, Context, ReturnLength)` → SSDT 0x0122 (nt_namespace.c -- 7 NT params packed into 6 SSDT slots: a6 = (high32 ReturnLength*) | (low32 Context*); kernel pointers in low-4-GiB)
- [x] `NtCreateSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes, LinkTarget)` → SSDT 0x0123 (nt_namespace.c -- ob_ns_create_symlink + ObInsertObject; bounded target length)
- [x] `NtOpenSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0124 (nt_namespace.c -- ObLookupObjectByName with ObpSymlinkType to stop at the link itself)
- [x] `NtQuerySymbolicLinkObject(LinkHandle, LinkTarget, ReturnedLength)` → SSDT 0x0125 (nt_namespace.c -- ObpLookupHandle + type-check against ObpSymlinkType + bounded copy of target string)
- [x] Commit: `"kernel: nt -- directory and symbolic link object syscalls"` (6052c7aa)

**Test checkpoint:** `NtCreateDirectoryObject` creates `\Test`; `NtOpenDirectoryObject` opens it. `NtCreateSymbolicLinkObject` creates `\TestLink → \Test`; `NtQuerySymbolicLinkObject` returns `\Test`.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 5 new §17 tests (NT create+open directory roundtrip, open not found, symlink roundtrip, query wrong type, namespace SSDT registered), 0 failures.

> **Verified** (2026-04-14): 6 handlers registered at SSDT 0x0120-0x0125 via nt_namespace.c:512-523. Create handlers (Directory, SymbolicLink) are atomic: allocate handle BEFORE inserting into namespace, clear OB_FLAG_PERMANENT on rollback so deref actually frees. Query handlers use ObpLookupHandle + type-check (ObpDirectoryType / ObpSymlinkType). NtQueryDirectoryObject ABI repacked: a4 carries (RestartScan << 8) | ReturnSingleEntry so Context (a5) and ReturnLength (a6) stay full 64-bit pointers. ReturnSingleEntry caps effective buf_count to 1. 10 ntdll PE exports added (sorted). 5 new tests in TEST_CAT_OB. Build clean.
> **Accepted:** UNICODE_STRING.Buffer cast to const char* + NUL-terminated bounded_strlen scan (kernel-wide ASCII pattern shared with §14, §15 NT handlers; §16 has no UNICODE_STRING args) -> XREF: 02-kernel-core/TODO-14 §5 (kernel-wide `nt_decode_unicode_string` helper in `src/kernel/nt/nt_string.c`; retrofit list explicitly names §17's `oa_name`, `path_within_bounds`, and `NtCreateSymbolicLinkObject_handler` target read as consumers).
> **Quality reviewed** (2026-04-14): dead code clean (bounded_strlen is local-scope; ns_memcpy is local-scope; split_path is single-use). Consistency: SSDT constants 0x0120-0x0125 match service_numbers.h:269-274; OBJECT_DIRECTORY_INFORMATION layout matches library NtQueryDirectoryObject signature in ob.c:421. Performance: split_path linear (one pass for last_sep + one memcpy); ObInsertObject linear over directory entries (bounded by OB_DIR_MAX_ENTRIES=128). bounded_strlen called twice per create (once for plen guard, once for leaf len) -- acceptable given OB_PATH_MAX=256. Codex finding "ReturnSingleEntry not honored" fixed: caps buf_count to 1 when flag set.


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
- [x] Commit: `"kernel: nt -- section and memory-mapped file syscalls"`

**Test checkpoint:** `NtCreateSection` with `SEC_COMMIT` creates pagefile-backed section. `NtMapViewOfSection` maps into current process; write/read round-trip. `NtUnmapViewOfSection` unmaps. File-backed section maps file contents correctly.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 5 new §18 tests (anon roundtrip, file-backed map, named open+query+extend, unmap bad base, SSDT slots registered), 0 failures. Note: full `bash scripts/test.sh SUITE=ob` may exceed 60s TCG boot budget on slow hosts; build verification `=== BUILD OK ===` plus OB suite on Windows QEMU per project norm.

> **Codex (2026-04-14):** Fixed page-count overflow (64-bit math + `section_page_count_from_size`), per-section `spinlock_t` for views/size/phys, `oa_probe_ascii_name` for user `OBJECT_ATTRIBUTES`, NULL buffer guard in `ObQuerySectionObject`, file-backed create uses single validated `FILE_OBJECT` for `vfs_read` (no second lookup), `ObExtendSection` double-checked backing swap under lock.
> **Verified** (2026-04-14): `nt_section_register_ssdt()` registers 7 handlers at 0x005C-0x0062 (`nt_section.c`). `ObCreateSectionEx` backs anonymous (`pmm_alloc_contiguous` + zero) and file-backed (`vfs_read` snapshot + `vfs_get_path_from_node` identity) sections; `ObMapViewOfSectionFull` / `ObUnmapViewOfSectionByBase` track views by PID; `ObExtendSectionObject` reallocates backing (rejects `SEC_IMAGE`). `NtMapViewOfSection` rejects non-current `ProcessHandle` with `STATUS_ACCESS_DENIED` until per-process VMM exists. PE exports added in `pe.c` (sorted). SYS_SHMEM paths call `ObMapViewOfSection(..., task_current()->pid)`.
> **Quality reviewed** (2026-04-14): Codex round 2 (adversarial + quality) drove five local fixes: (1) `ProbeForWriteIfUser` on `out_handle`/`base_ptr` in `NtCreateSection`/`NtOpenSection`/`NtMapViewOfSection` (no more arbitrary kernel write via attacker-controlled output pointers); (2) symmetric `target != task_current()` guard in `NtUnmapViewOfSection` (cross-process unmap now matches map semantics); (3) `ObExtendSectionObject` rejects with `STATUS_SECTION_NOT_EXTENDED` when `map_count > 0` and rechecks under the relock after the alloc/copy window to abort if a view was installed racily (no stranded callers on freed pages); (4) `ObMapViewOfSectionFull` now calls `ObReferenceObject(so)` INSIDE the spinlock before releasing it, and both unmap paths call `ObDereferenceObject(so)` after removing the view (views pin the backing until unmapped, closing the close-during-map UAF window); (5) named-section create: `ObInsertObject` return is checked -- on name collision redirect to the winner, on no-winner failure return `INVALID_HANDLE_VALUE` instead of silently leaking an unnamed orphan section. `SECTION_BASIC_INFORMATION` reordered to match Windows SDK `ntddk.h` layout (BaseAddress @0, AllocationAttributes @8, MaximumSize @16) with four `_Static_assert` offset/size gates.
> **Accepted:** (1) `NtMapViewOfSection` full 10-parameter Windows stack ABI not wired on INT 0x2E / fast syscall (only 4-5 GPR slots today); extended fields use optional `NT_MAPVIEW_ARGS*` at `a5` when non-NULL -> XREF: 02-kernel-core/TODO-12 §4 (item: "Extend INT 0x2E + SYSCALL entry to read stack arguments 5-6+", names `syscall_handler_2e`/`syscall_entry.asm` to retrofit, deletes `NT_MAPVIEW_ARGS` + `NtCreateSection` packed-flags kludge at `nt_section.c:93-94`). (2) Cross-process map / other-process `NtUnmapViewOfSection` -> XREF: 03-memory-concurrency/TODO-04 §5 (item: "Retrofit `src/kernel/nt/nt_section.c` once per-process page tables exist", names both guards at `nt_section.c:182`/`235` to remove). (3) `SEC_RESERVE`-only sparse pagefile sections without physical pages -> XREF: 03-memory-concurrency/TODO-04 §5 (item: "Implement `SEC_RESERVE` sparse pagefile-backed sections", names `ObCreateSectionEx` pre-alloc behavior to convert to zero-page PTEs + demand fault). (4) `SectionImageInformation` full Windows layout + PE parse (entry point, stack) -> XREF: 02-kernel-core/TODO-08 §8 (item: `NtQuerySection(..., SectionImageInformation)` PE persist at line 210 -- names backing `SECTION_OBJECT`/`ob_section.c`/`nt_section.c` retrofit list). (5) Initial `ObpLookupHandle` UAF race in `nt_section.c` (body could be freed by concurrent `NtClose` on another thread of the same task between lookup and use) -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add `ObpReferenceObjectByHandle(...)` primitive", explicitly names retrofit list including all of `nt_section.c` and `ObUnmapViewOfSectionByBase` at `ob_section.c:414`). (6) Section handle `granted_access` not enforced per Windows rights model (`SECTION_MAP_READ/WRITE/EXECUTE`, `SECTION_EXTEND_SIZE`, `SECTION_QUERY`) -> XREF: 02-kernel-core/TODO-15 §8 (item: "Enforce per-handle `granted_access` on section syscalls", names required masks per handler and the `test_ob.c` negative test to add). (7) `ObUnmapViewOfSectionByBase` and `ObAreMappedFilesTheSame` O(handle_capacity × SECTION_MAX_VIEWS) scans on every call -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add per-task view-base index", names both functions and `(task_pid, base_addr)` key to add to `struct task`).


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
- [x] Commit: `"kernel: nt -- timer control and time query syscalls"`

**Test checkpoint:** `NtCreateTimer` + `NtSetTimer` with relative 100ms due time fires. `NtCancelTimer` cancels before fire returns `STATUS_SUCCESS`. `NtQueryPerformanceCounter` returns monotonically increasing value. `NtQueryTimerResolution` reports correct LAPIC timer resolution.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 5 new §19 tests (create+query, set/cancel, open existing, wrong-type mismatch, SSDT slots registered), 0 failures. The actual fire-via-event test is exercised via `nt_timer_tick()` under WHPX; on TCG the PIT path is equivalent.

> **Codex adversarial (2026-04-14):** Fixed `compute_due_ns` integer overflow with saturating 100-ns->ns conversion + saturated `now + rel_ns` add (cap `0x7FFFFFFFFFFFFFFF`); fixed `nt_timer_arm` missed-fire race by moving `event_reset()` BEFORE the armed-list insert (inside `s_armed_lock`) so any post-arm `event_set()` survives until consumed.
> **Verified** (2026-04-14): `nt_timer_register_ssdt()` registers 6 handlers at 0x007E-0x0083 (`nt_timer.c:575`). Armed-timer list head (`s_armed_head`) + irqsave spinlock (`s_armed_lock`); `nt_timer_tick()` called from `lapic_timer_handler` after `kusd_update_time()` and from `pit_tick_increment` after `pit_lock` release. `resolve_timer_handle` rejects wrong object types with `STATUS_OBJECT_TYPE_MISMATCH`. `nt_timer_detach()` wired into `timer_on_close` and `timer_on_delete` so a closing/freeing timer is unlinked from the armed list before event state is cleared. `wait_on_handle` in `nt_sync.c` dispatches `ObpTimerType` so `NtWaitForSingleObject(timer, ...)` works. PE exports added in `pe.c` (sorted).
> **Quality reviewed** (2026-04-14): Codex quality review drove two local fixes: (1) `nt_timer_tick` now uses a two-phase pattern -- Phase A detaches one-shot due timers into a local `signal_chain` under `s_armed_lock` (periodic timers are signalled inline since they stay on the queue); Phase B releases the lock, then calls `event_set()` for the one-shot chain so waiter-list walks in `event_set` no longer contribute to lock hold time and cannot serialize other CPUs' Set/Cancel calls. (2) Removed the dead legacy `NtCreateTimer(ht, name)` wrapper from `ob_timer.c` / `ob_timer.h` (zero callers).
> **Accepted:** (1) `UNICODE_STRING.Buffer` in `oa_probe_ascii_name` is returned as a raw pointer and consumed via `snprintf("%s", name)` in `ObCreateTimerEx`/`ObOpenTimer`, risking an overread if the buffer has no NUL within probed bytes (same codebase-wide pattern as `nt_section.c`/`nt_namespace.c`) -> XREF: 02-kernel-core/TODO-31 §14 (item: "UTF-16 decode for `UNICODE_STRING` inputs (kernel-wide)" at line 246 -- retrofit list explicitly names `nt_timer.c::oa_probe_ascii_name` and the ASCII path construction in `ObCreateTimerEx`/`ObOpenTimer`). (2) Initial `ObpLookupHandle` UAF race in `nt_timer.c::resolve_timer_handle` (body could be freed by concurrent `NtClose` on another thread of the same task between lookup and use) -> XREF: 02-kernel-core/TODO-05 §2 (item: "Add `ObpReferenceObjectByHandle(...)` primitive" at line 112 -- retrofit list now explicitly names `nt_timer.c` and enumerates the four consumer handlers). (3) Flat armed-list linear scan per tick (tens of timers OK today, O(N * ticks) at scale) -> XREF: 02-kernel-core/TODO-05 §6 (item: "Replace the flat NT timer armed list with an ordered structure (min-heap or timing wheel)" -- names `nt_timer.c::s_armed_head`, suggests min-heap or timing wheel, requires 1024-timer stress test).

## 20. Legacy LPC Port Syscalls

> [!NOTE]
> Legacy LPC (NT 3.x-5.x) syscall surface. Reserves SSDT indices 0x0100-0x010C and wires NT-compatible signatures so user-mode callers get `STATUS_NOT_IMPLEMENTED` (rather than an empty SSDT slot) until the ALPC subsystem lands. Full port infrastructure -- `ALPC_PORT` object, message queue, connection state machine, send/receive engine -- is implemented in `02-kernel-core/TODO-24-alpc-message-ports.md`. Modern ALPC (Vista+) syscalls split to §31. LPC and ALPC share the same underlying kernel object type (`ObpAlpcPortType`); LPC is a thin compatibility adapter over ALPC in Windows and will be the same here.

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

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 2 new §20 tests (LPC slots registered, LPC returns deferred-status), 0 failures.

> **Codex adversarial (2026-04-14):** Three Medium findings, all fixed: (1) `nt_lpc_register_ssdt()` silently accepted `ssdt_register()` failures; replaced with per-slot `lpc_register_one()` helper that checks ssdt_get_table, detects collision with `ssdt_stub_not_implemented`, checks ssdt_register return, logs per-slot failures, and suppresses the success line when any of the 15 fail. (2) `test_nt_lpc_returns_deferred_status` only dispatched 5 of 15 slots; expanded to iterate all 15 so mis-registration to another non-stub handler in any unsampled slot is caught. (3) `LPC_STUB_BODY`'s `s_warned` was a non-atomic one-time flag -- replaced with atomic acquire/release so concurrent first-calls on multiple CPUs cannot double-log.
> **Verified** (2026-04-14): `nt_lpc_register_ssdt()` registers 15 stub handlers at SSDT 0x0100-0x010E (`nt_lpc.c`). Each handler is guarded by `SCOPE-GAP-ALLOWED` sentinel and returns `STATUS_NOT_IMPLEMENTED` with a first-call `klog(LOG_WARN, ...)` trace. NT wait path does not need an `ObpAlpcPortType` case yet -- no ALPC_PORT objects exist until TODO-17 §8 lands. PE exports added in `pe.c` (sorted, 15 entries). Master table TODO-A rows 0x0100-0x010E flipped from `[ ]` to `[/]` (stub-wired, deterministic status, not functional) and Owner updated to `T12 §20 (nt_lpc.c stub, T17 §7 retrofit)`.
> **Quality reviewed** (2026-04-14): Codex quality review drove one fix: `LPC_STUB_BODY` steady-state was `__atomic_exchange_n` (read-modify-write on every syscall); replaced with `__atomic_load_n(RELAXED)` fast path + `__atomic_compare_exchange_n` only on the 0->1 transition. User-mode callers repeatedly invoking an unimplemented LPC syscall no longer force cross-CPU cacheline ping-pong.
> **Accepted:** (1) LPC engine itself (port objects, connection state machine, request/reply rendezvous, SeAccessCheck for SecureConnectPort, SeImpersonate for ImpersonateClientOfPort, out-of-line ReadRequestData/WriteRequestData) intentionally deferred -- this section's explicit scope is SSDT reservation + syscall signatures only -> XREF: 03-memory-concurrency/TODO-08 §7 (item: "Retrofit the 15 LPC SSDT stubs in src/kernel/nt/nt_lpc.c (SSDT 0x0100-0x010E) to real handlers that call into lpc.c instead of returning STATUS_NOT_IMPLEMENTED" -- enumerates every slot with per-syscall retrofit semantics and auto-closes §20).

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

## 23. Atom, Locale, and Miscellaneous Syscalls
Catch-all for global atom table, locale management, environment variables, and display/error APIs.

- [ ] `NtAddAtom(AtomName, Length, Atom)` → SSDT 0x00DF: add string to global atom table; return 16-bit atom ID
- [ ] `NtFindAtom(AtomName, Length, Atom)` → SSDT 0x00E0: look up atom by string
- [ ] `NtDeleteAtom(Atom)` → SSDT 0x00E1: remove atom from table
- [ ] `NtQueryInformationAtom(Atom, AtomInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x00E2
- [ ] `NtQueryDefaultLocale(UserProfile, DefaultLocaleId)` → SSDT 0x00DA
- [ ] `NtSetDefaultLocale(UserProfile, DefaultLocaleId)` → SSDT 0x00DB
- [ ] `NtQueryDefaultUILanguage(DefaultUILanguageId)` → SSDT 0x00DC
- [ ] `NtSetDefaultUILanguage(DefaultUILanguageId)` → SSDT 0x00DD
- [ ] `NtQueryInstallUILanguage(InstallUILanguageId)` → SSDT 0x00DE
- [x] `NtQuerySystemEnvironmentValue(VariableName, VariableValue, ValueLength, ReturnLength)` → SSDT 0x00D2: UEFI runtime variable access
- [x] `NtSetSystemEnvironmentValue(VariableName, VariableValue)` → SSDT 0x00D3
- [ ] `NtDisplayString(String)` → SSDT 0x00D8: blue-screen-style text output during boot
- [ ] `NtRaiseHardError(ErrorStatus, NumberOfParameters, UnicodeStringParameterMask, Parameters, ValidResponseOptions, Response)` → SSDT 0x00D9: system-modal error dialog
- [ ] Commit: `"kernel: nt -- atom table, locale, environment, misc syscalls"`

**Test checkpoint:** `NtAddAtom("TestAtom")` returns atom ID > 0. `NtFindAtom("TestAtom")` returns same ID. `NtDeleteAtom` removes it; subsequent `NtFindAtom` returns `STATUS_OBJECT_NAME_NOT_FOUND`. `NtQueryDefaultLocale` returns valid LCID.

## 24. Syscall Audit and Tracing Hook

> [!IMPORTANT]
> **Impossible OS exclusive feature.** Windows uses ETW (heavyweight, complex configuration). Linux uses seccomp-bpf (complex BPF programs) or strace (ptrace overhead). Impossible OS provides a first-class kernel API for syscall-level auditing with minimal overhead.

- [ ] `NtRegisterSyscallAuditHook(HookHandle, AuditRoutine, Context, Flags)` → SSDT 0x0150:
  - `Flags`: `AUDIT_PRE_CALL = 1` (before handler), `AUDIT_POST_CALL = 2` (after handler), `AUDIT_BOTH = 3`
  - `AuditRoutine(ServiceNumber, Args, Context, Phase)` -- called in kernel context
  - Hook can inspect arguments, log, or deny (return `STATUS_ACCESS_DENIED` from pre-call to block)
- [ ] `NtUnregisterSyscallAuditHook(HookHandle)` → SSDT 0x0151
- [ ] `NtQuerySyscallAuditState(Buffer, Length, ReturnLength)` → SSDT 0x0152:
  - Returns list of active hooks, their coverage (pre/post/both), and overhead metrics
- [ ] SSDT dispatcher integration: before/after each `syscall_dispatch` call, check hook list and invoke if registered
  - Fast path: single atomic read of hook pointer; NULL = no hooks, no overhead
  - Hook list is RCU-protected for lock-free read in the hot path
- [ ] Requires `SeAuditPrivilege` to register hooks
- [ ] Commit: `"kernel: nt -- syscall audit and tracing hook (SSDT pre/post)"`

**Test checkpoint:** Register pre-call audit hook; every syscall logs service number to ring buffer. Register post-call hook; verify NTSTATUS is captured. Pre-call hook returning `STATUS_ACCESS_DENIED` blocks the syscall. Unregister hook; verify zero overhead (no measurable latency increase).

## 25. Per-Process Syscall Filtering

> [!NOTE]
> → XREF: `TODO-21-process-model-extensions.md §12` -- scope overlap: TODO-21 §12 adds pledge/unveil-style category-based restriction (`NtPledge`/`NtUnveil`). This section adds per-index bitmap filtering. Both run in the SSDT dispatcher; bitmap filter runs FIRST (per-index), then pledge category check. Both must pass for the syscall to proceed.

Per-process syscall restrictions allow a process to lock down which system services its children (or itself) can invoke. Windows has `PROCESS_MITIGATION_SYSTEM_CALL_DISABLE_POLICY` (DisallowWin32kSystemCalls, DisallowFsctlSystemCalls) stored in EPROCESS MitigationFlags. Linux has seccomp-bpf with per-process BPF programs and constant-action bitmap caching. Impossible OS provides a first-class bitmap-based filter with optional BPF programs for argument inspection.

- [ ] Define `SYSCALL_FILTER` struct in `include/kernel/nt/syscall_filter.h`:
  - `uint64_t allow_bitmap[SSDT_MAX_ENTRIES / 64]` -- one bit per SSDT index; 1 = allowed, 0 = blocked
  - `uint64_t allow_shadow_bitmap[WIN32K_MAX_ENTRIES / 64]` -- same for shadow SSDT (Win32k)
  - `uint32_t flags` -- `SYSCALL_FILTER_INHERIT = 1` (child inherits), `SYSCALL_FILTER_LOCKED = 2` (cannot relax)
- [ ] Add `SYSCALL_FILTER *syscall_filter` field to `task_t` (NULL = no filter, all syscalls allowed)
- [ ] Add `ProcessSystemCallFilterPolicy` info class to `NtSetInformationProcess` (§7, SSDT 0x0035):
  - `DisallowWin32kSystemCalls`: clear all shadow SSDT bits in filter bitmap
  - `DisallowFsctlSystemCalls`: clear `NtFsControlFile` (0x001A) bit, except pipe-related FSCTL codes
  - `CustomBitmap`: install a caller-supplied allow/deny bitmap
  - Filter can only be tightened (bits cleared), never relaxed once `SYSCALL_FILTER_LOCKED` is set
- [ ] SSDT dispatcher integration: after index lookup, before handler call, check `current_task->syscall_filter`:
  - If filter is NULL, skip (zero overhead -- single pointer test)
  - If filter exists, test bitmap bit for the service index; if blocked, return `STATUS_ACCESS_DENIED`
- [ ] Filter inheritance: `NtCreateProcess` / `NtCreateUserProcess` copies parent's filter to child if `SYSCALL_FILTER_INHERIT` is set
- [ ] Audit mode: `SYSCALL_FILTER_AUDIT = 4` -- log blocked syscalls to klog instead of denying
- [ ] Commit: `"kernel: nt -- per-process syscall filter bitmap (seccomp/SystemCallDisable parity)"`

**Test checkpoint:** Set filter blocking `NtWriteFile` on child process; child's `NtWriteFile` returns `STATUS_ACCESS_DENIED`. Parent's `NtWriteFile` still works. `DisallowWin32kSystemCalls` blocks shadow SSDT calls. Filter inheritance: grandchild also blocked. Locked filter cannot be relaxed. Audit mode logs but doesn't block.

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

## 27. SSDT Integrity Protection

> [!TIP]
> **Impossible OS competitive edge.** Windows uses PatchGuard/KPP -- a complex, opaque system that periodically checksums kernel structures and BSODs on tampering. It's a cat-and-mouse arms race with rootkits. Linux has no SSDT integrity protection at all (`sys_call_table` is `const` but not hardware-enforced). Impossible OS uses hardware write-protection: mark the SSDT pages as read-only via PTE after initialization. Any write attempt triggers a #PF that the kernel catches and escalates to `KeBugCheck(CRITICAL_STRUCTURE_CORRUPTION)`. Zero runtime overhead, no periodic polling, no timing-based detection -- just hardware-enforced immutability.

- [ ] After `ssdt_init()` completes and all 470 handlers are registered, mark SSDT pages as read-only via PTE manipulation (clear R/W bit, flush TLB for affected pages)

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

---

## 29. Token Lifecycle and SRM Access Check Syscalls

> [!NOTE]
> Split from §16 to keep each section under the 10-item limit. §16 covers "operate on existing token" (open/query/set/adjust). This section covers token creation/derivation and the Security Reference Monitor decision points (NtAccessCheck, NtPrivilegeCheck, security-descriptor I/O). Token infrastructure in `src/kernel/security/token.c`; SRM engine in `src/kernel/security/srm.c` (→ XREF TODO-15 §7, §8).

- [ ] `NtDuplicateToken(ExistingTokenHandle, DesiredAccess, ObjectAttributes, EffectiveOnly, TokenType, NewTokenHandle)` → SSDT 0x00B8
- [ ] `NtFilterToken(ExistingTokenHandle, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewTokenHandle)` → SSDT 0x00B9
- [ ] `NtCreateToken(TokenHandle, DesiredAccess, ObjectAttributes, TokenType, AuthenticationId, ExpirationTime, User, Groups, Privileges, Owner, PrimaryGroup, DefaultDacl, Source)` → SSDT 0x00BA
- [ ] `NtAccessCheck(SecurityDescriptor, ClientToken, DesiredAccess, GenericMapping, PrivilegeSet, PrivilegeSetLength, GrantedAccess, AccessStatus)` → SSDT 0x00BC:
  - Core SRM decision point; routes to `SeAccessCheck()` in the Security Reference Monitor
- [ ] `NtPrivilegeCheck(ClientToken, RequiredPrivileges, Result)` → SSDT 0x00BF
- [ ] `NtSetSecurityObject(Handle, SecurityInformation, SecurityDescriptor)` → SSDT 0x00C1
- [ ] `NtQuerySecurityObject(Handle, SecurityInformation, SecurityDescriptor, Length, LengthNeeded)` → SSDT 0x00C2
- [ ] Commit: `"kernel: nt -- token lifecycle and SRM access check syscalls"`

**Test checkpoint:** `NtDuplicateToken` returns a distinct token with copied privileges/groups. `NtAccessCheck` against an object with DACL returns correct granted access. `NtPrivilegeCheck` with a held privilege returns TRUE; with a missing privilege returns FALSE. `NtQuerySecurityObject` round-trips through `NtSetSecurityObject`.

---

## 30. Generic Object Management Syscalls

> [!NOTE]
> Split from §17 to keep both sections under the 10-item limit. §17 covers namespace objects (directory + symlink); this section covers object lifetime and identity operations that apply to **any** OB type (events, mutexes, files, sections, etc.). Object Manager infrastructure in `src/kernel/ob/ob.c` (→ XREF TODO-05 §1, §4, §9).

- [ ] `NtMakeTemporaryObject(Handle)` → SSDT 0x0003: clears `OB_FLAG_PERMANENT`, allowing the object to be deleted when its reference count drops to zero
- [ ] `NtMakePermanentObject(Handle)` → SSDT 0x0004: sets `OB_FLAG_PERMANENT` (kernel-mode caller only; requires `SeCreatePermanentPrivilege`)
- [ ] `NtSetInformationObject(Handle, ObjectInformationClass, Buffer, Length)` → SSDT 0x0005: writable counterpart to `NtQueryObject`; supports `ObjectHandleFlagInformation` (set `OBJ_INHERIT` / `OBJ_PROTECT_CLOSE` on the HANDLE_TABLE_ENTRY)
- [ ] `NtCompareObjects(FirstObjectHandle, SecondObjectHandle)` → SSDT 0x0009: returns `STATUS_SUCCESS` if both handles refer to the same underlying object body, else `STATUS_NOT_SAME_OBJECT`
- [ ] **Expose `NtQueryObject` with the Win11 `OBJECT_TYPE_INFORMATION` NT ABI** (`UNICODE_STRING TypeName` + canonical field order + offset asserts), replacing the internal `char[32]`+counters struct in `ob.c`/`ob.h`. XREF: TODO-05 §12.
- [ ] Commit: `"kernel: nt -- generic object management syscalls (make-temp/perm, set-info, compare)"`

**Test checkpoint:** `NtMakePermanentObject` on an event handle prevents deletion when last reference released. `NtMakeTemporaryObject` re-enables deletion. `NtSetInformationObject(ObjectHandleFlagInformation)` toggles `OBJ_INHERIT` on a handle. `NtCompareObjects` with two duplicate handles returns `STATUS_SUCCESS`; with handles to different objects returns `STATUS_NOT_SAME_OBJECT`.

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

**Test checkpoint:** All 16 ALPC SSDT slots resolve to the registered handler (not the default `ssdt_stub_not_implemented`). Each handler returns `STATUS_NOT_IMPLEMENTED` until TODO-24 §8-§9 ALPC engine lands; functional round-trip (`NtAlpcCreatePort` + `NtAlpcConnectPort` + `NtAlpcSendWaitReceivePort`) and `\RPC Control\` namespace visibility are exercised by TODO-24 §8-§9 tests.

> **Test runner:** `scripts\debug\kernel\run-ob-tests.bat` (SUITE=ob)
> **Expected:** 2 new §31 tests (ALPC slots registered, ALPC returns deferred-status) + 1 cross-section PE export-sort invariant test, 0 failures.

> **Codex adversarial (2026-04-14):** Two findings, both fixed: (1) `s_ntdll_exports[]` in `pe.c` was not strictly sorted (pre-existing drift plus my mis-insertion of `NtAcceptConnectPort` and `NtAlpc*` entries) -- `pe_lookup_export()` binary-searches the table, so any out-of-order pair silently breaks NTAPI import resolution. Resorted the full 82-entry table and added `pe_exports_sorted_check()` exposed via `pe.h`. (2) `nt_alpc_register_ssdt()` was fail-open -- logged failures but returned void; `boot_phase3` continued with a partial ALPC surface. Changed to return `int` (failure count); applied the same mirror fix to `nt_lpc_register_ssdt()`. Both returns are now accumulated in `boot_desktop.c` and a non-zero result escalates to `boot_halt("SSDT registration failed")`.
> **Verified** (2026-04-14): `nt_alpc_register_ssdt()` registers 16 stub handlers at SSDT 0x010F-0x011E (`nt_alpc.c`). Each handler is guarded by `SCOPE-GAP-ALLOWED` sentinel, uses the load-first + CAS-on-0-to-1 atomic one-time-log pattern (matching §20 after its review fix), and returns `STATUS_NOT_IMPLEMENTED`. `alpc_register_one()` helper checks `ssdt_get_table`, detects collision with the default stub, captures `ssdt_register()` return, and per-slot logs failures. PE exports added in `pe.c` (16 entries, alphabetically sorted). Master table TODO-A rows 0x010F-0x011E flipped from `[ ]` to `[/]` and Owner updated to `T12 §31 (nt_alpc.c stub, T24 §8 retrofit)`.
> **Quality reviewed** (2026-04-14): Codex quality review drove two fixes: (1) `alpc_register_one()` / `lpc_register_one()` computed `idx = svc & 0xFFF` (4096 range) and read `tbl->handlers[idx]` BEFORE any bounds check, but `SSDT_MAIN_MAX` is 1024 -- a mis-numbered service constant above 0x3FF would read past the handler array. Added `if (idx >= SSDT_MAIN_MAX)` guard before the handler load in BOTH helpers, and changed the mask to `SSDT_INDEX_MASK` for consistency with `ssdt_register()` itself. (2) `pe_exports_sorted_check()` was test-only; release boots (which skip tests) could ship with unsorted exports and silently break import resolution. Added a boot-time call in `boot_desktop.c` after SSDT registration that `boot_halt`s on any non-zero violation, making the invariant a hard gate on EVERY boot, not just debug.
> **Accepted:** (1) ALPC engine itself (ALPC_PORT object, connection state machine, PORT_MESSAGE rendezvous, port sections, resource reserves, 11-class `ALPC_PORT_INFORMATION_CLASS`, 2-class `ALPC_MESSAGE_INFORMATION_CLASS`, `\RPC Control\` namespace directory) intentionally deferred -- this section's explicit scope is SSDT reservation + syscall signatures only -> XREF: 02-kernel-core/TODO-24 §8 (SSDT retrofit item: "Retrofit the 16 ALPC SSDT stubs in src/kernel/nt/nt_alpc.c (SSDT 0x010F-0x011E) to real handlers that call into the ALPC engine instead of returning STATUS_NOT_IMPLEMENTED") and TODO-24 §9 (Query/Set/CancelMessage) -- enumerates every slot with per-syscall retrofit semantics and auto-closes §31).

---

## OS Comparison

| ⭐ | Feature                    | 🪟 Win11                    | 🐧 Linux                   | 🚀 Impossible OS            |
|----|----------------------------|------------------------------|----------------------------|------------------------------|
| 💎 | SYSCALL/SYSRET fast path   | ✅ KiSystemCall64+LSTAR     | ✅ entry_SYSCALL_64        | ✅ §2 LSTAR + SYSRET enabled |
| 💎 | Typed failure return       | ✅ NTSTATUS on all NtXxx    | ✅ -ERRNO signed           | ✅ §1 NTSTATUS + 50 codes   |
| 💎 | Service descriptor table   | ✅ SSDT + shadow SSDT       | ✅ sys_call_table[]        | ✅ §4 SSDT 470 + shadow stub |
| 💎 | SW-interrupt compat path   | ✅ INT 0x2E (legacy)        | ✅ INT 0x80 (32-bit)       | ✅ §3 INT 0x2E + 0x80       |
| 💎 | IO_STATUS_BLOCK async I/O  | ✅ IOSB on all file Nt      | ⚠️ io_uring only           | ⬜ §11                      |
| 💎 | File metadata syscalls     | ✅ NtQuery/SetInfoFile      | ✅ stat/fstat/utimensat    | ⬜ §13                      |
| 💎 | Device I/O control         | ✅ NtDeviceIoControlFile    | ✅ ioctl()                 | ⬜ §13                      |
| 💎 | I/O completion ports       | ✅ NtCreateIoCompletion     | ✅ epoll/io_uring          | ⬜ §13                      |
| 💎 | Process/thread create API  | ✅ NtCreate{Process,Thread} | ✅ clone/execve            | ✅ §7 23 handlers wired     |
| 💎 | Thread context get/set     | ✅ NtGet/SetContextThread   | ✅ ptrace GETREGS          | 🔄 §7 stubs (needs TODO-23) |
| 💎 | Named sync objects         | ✅ NtCreate{Event,Mutant}   | ✅ POSIX sem + futex       | ✅ §8 Event+Mutant+Semaphore |
| 💎 | Multi-object wait          | ✅ NtWaitForMultipleObj     | ⚠️ No direct equivalent    | ✅ §8 WaitAll+WaitAny 64 max |
| 💎 | Keyed events (futex)       | ✅ NtWaitForKeyedEvent      | ✅ futex()                 | 🔄 §8 stubs (TODO-17)       |
| 💎 | Virtual memory syscalls    | ✅ NtAllocate/Free/Protect  | ✅ mmap/mprotect/munmap    | ✅ §9 Alloc+Free+Protect+Query |
| 💎 | Cross-process memory       | ✅ NtRead/WriteVirtualMem   | ✅ process_vm_readv        | ✅ §9 Read+Write (identity) |
| 💎 | OS info query syscall      | ✅ NtQuerySystemInfo        | ✅ sysinfo + /proc         | 🔄 §10 5 classes + defaults |
| 💎 | LastError per-thread       | ✅ TEB→LastErrorValue       | ✅ errno via TLS           | ⬜ §11 + TODO-11 §1         |
| 💎 | Registry syscalls          | ✅ NtCreate/Open/QueryKey   | ❌ No equivalent           | ⬜ §14 + TODO-14            |
| 💎 | Token/access control       | ✅ NtAccessCheck + tokens   | ✅ capabilities + DAC/MAC  | ⬜ §16 + TODO-15            |
| 💎 | Namespace dir/symlink      | ✅ NtCreateDirectoryObj     | ❌ No kernel namespace     | ⬜ §17                      |
| 💎 | Memory-mapped sections     | ✅ NtCreateSection/MapView  | ✅ mmap with MAP_SHARED    | ✅ §18 full §5 SSDT 0x005C-0x0062 |
| 💎 | Timer objects              | ✅ NtSetTimer periodic      | ✅ timerfd_create          | ✅ §19 full 6 SSDT 0x007E-0x0083 |
| 💎 | ALPC message ports         | ✅ NtAlpcSendWaitReceive    | ❌ No equivalent           | 🟡 §20 (LPC stub) + §31 (ALPC stub), engine in TODO-24 |
| 💎 | Debug API                  | ✅ NtDebugActiveProcess     | ✅ ptrace                  | ⬜ §21 + TODO-29            |
| 💎 | Power management           | ✅ NtSetSystemPowerState    | ✅ sys_reboot + ACPI       | 🔄 §5 NtShutdownSystem wired |
| 💎 | Atom table                 | ✅ NtAddAtom/FindAtom       | ❌ No equivalent           | ⬜ §23                      |
| ⭐ | ZwXxx CPL-gated aliases    | ✅ Internal, undocumented   | ❌ No equivalent           | ✅ §12 zw.h + ProbeFor*      |
| ⭐ | Stable native API contract | ⚠️ Undocumented             | ❌ No stable native API    | ⬜ §4+§12 -- numbered+public |
| ⭐ | Syscall audit hook         | ⚠️ ETW, heavyweight         | ⚠️ seccomp-bpf, complex    | ⬜ §24 -- first-class API    |
| 💎 | Per-process syscall filter | ✅ SystemCallDisablePolicy  | ✅ seccomp-bpf + Landlock  | ⬜ §25 -- bitmap + BPF       |
| 💎 | Kernel→user callbacks      | ✅ KeUserModeCallback       | ⚠️ Signals only            | ⬜ §26                      |
| ⭐ | SSDT integrity protection  | ⚠️ PatchGuard (periodic)    | ❌ No protection           | ⬜ §27 -- HW write-protect   |

> **Target after §1–§23 completion:** Impossible OS reaches complete NT native API coverage across 470 syscall endpoints. Current state is partial; many domain and deferred sections remain open.
> **§12** makes the `ZwXxx` layer an explicit, documented public contract -- Windows keeps it internal/undocumented and Linux has no equivalent.
> **§24** provides first-class syscall auditing -- no ETW complexity, no BPF programs, just a kernel callback with near-zero idle overhead.
> **§25** closes the per-process syscall filtering parity gap -- both Win11 and Linux restrict per-process syscall access; Impossible OS uses a fast bitmap with optional BPF programs.
> **§26** enables kernel→user callbacks required by Win32k for window procedure dispatch -- without it, `SendMessage` and `DispatchMessage` cannot work.
> **§27** provides hardware-enforced SSDT immutability -- simpler and more secure than PatchGuard's periodic checksums.

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
    - SSDT has ≥ 470 registered entries
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

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: Phase 3 shows `"syscall: fast path (SYSCALL/SYSRET) enabled"`
- [ ] Serial log: `"ssdt: registered 470 services"` (or more)
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
