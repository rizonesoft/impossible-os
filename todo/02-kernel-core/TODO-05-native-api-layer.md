# TODO-05 — Native API Layer (Nt/Zw)

> **Goal:** Replace the ad-hoc INT 0x80 / POSIX-numbered `SYS_*` dispatch table with a complete NT native API layer: `NTSTATUS` return values, `NtXxx`/`ZwXxx` naming, a `SYSCALL`/`SYSRET` fast path, a numbered System Service Descriptor Table (SSDT) with 200+ service entries, and the `NtCurrentTeb()` / `NtCurrentPeb()` inline contract. This is the exact interface that `ntdll.dll`, CSRSS, Win32k, and every driver framework use to talk to the kernel. This TODO is the **master registry** for all NT syscall endpoints — some are implemented here, others are implemented by domain-specific TODOs but get their SSDT slots reserved and documented here.

> [!IMPORTANT]
> **Current state:** `syscall.c` dispatches via `INT 0x80` with Linux-style `SYS_WRITE=1`, `SYS_READ=2`, … `SYS_MUNMAP=38`. Return value is a plain `int64_t`. No `NTSTATUS`, no `NtXxx`/`ZwXxx` entry points, no `SYSCALL`/`SYSRET` MSR setup, no SSDT. The existing 22 syscalls are the migration starting point; none are deleted here.

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — `task_exec`, ring-3 entry frame
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) — MSR_GS_BASE, per-CPU data
- [`include/kernel/msr.h`](../../include/kernel/msr.h) — MSR_IA32_STAR, MSR_IA32_LSTAR, MSR_IA32_FMASK already defined
- [`include/kernel/gdt.h`](../../include/kernel/gdt.h) — GDT_KERNEL_CODE, GDT_USER_CODE selectors
- → XREF: `TODO-03-object-manager.md` — Ob-routed NtXxx functions (NtClose, NtDuplicateObject, NtQueryObject, NtOpenDirectoryObject, NtQueryDirectoryObject already implemented); SSDT entries for file/process/sync depend on TODO-03 §3–§10
- → XREF: `TODO-04-peb-teb-user-abi.md §3–§4` — `swapgs` in syscall entry/exit uses the TEB GS contract; KERNEL_GS_BASE per-task
- → XREF: `TODO-01-kernel-init-sequencing.md` — syscall fast path init belongs in Phase 1 (after GDT/IDT, before scheduler)
- → XREF: `TODO-07-time-filetime-management.md §7` — NtQuerySystemTime, NtSetSystemTime, NtQueryPerformanceCounter, NtQueryTimerResolution; service numbers reserved in §4
- → XREF: `TODO-10-exception-dispatch-seh.md §4` — NtRaiseException and NtContinue; SSDT indices reserved in §4
- → XREF: `TODO-11-security-reference-monitor.md` — ZwXxx kernel-mode calls bypass SRM access checks; NtAccessCheck routed through SRM
- → XREF: `TODO-12-alpc-message-ports.md §8` — ALPC port syscalls (NtCreatePort, NtAlpcSendWaitReceivePort, etc.); SSDT indices reserved in §4
- → XREF: `TODO-13-registry-completion.md §4` — Registry syscalls (NtCreateKey, NtOpenKey, NtSetValueKey, etc.); SSDT indices reserved in §4
- → XREF: `TODO-15-power-management.md` — NtSetSystemPowerState, NtInitiatePowerAction; SSDT indices reserved in §4
- → XREF: `TODO-18-kernel-debugger-kd-protocol.md` — NtDebugActiveProcess, NtWaitForDebugEvent; SSDT indices reserved in §4

## Outcome

- All syscalls return `NTSTATUS`; `STATUS_SUCCESS = 0`, `STATUS_FAILURE` codes for errors.
- `NtXxx` entry points are the user-mode callable names; `ZwXxx` are the kernel-mode aliases.
- `SYSCALL`/`SYSRET` fast path replaces `INT 0x80`; `INT 0x2E` kept as compatibility fallback.
- A numbered SSDT table with 200+ entries maps service indices to kernel functions; `ntdll` stubs call by index.
- The existing 22 `SYS_*` calls are migrated to `Nt`-named equivalents at stable indices.
- `NtCurrentTeb()` (`mov rax, gs:[0x30]`) and `NtCurrentPeb()` (`mov rax, gs:[0x60]`) return correct values per TODO-04.
- All endpoint categories covered: file I/O, process/thread, memory, sync, registry, security/token, sections, timers, ALPC ports, debug, power, namespace, system info, atoms.

## Implementation Order

| ⭐  | Order | Deliverable                                                  | Depends On      | Status |
| --- | :---: | ------------------------------------------------------------ | --------------- | :----: |
| 💎  |   1   | NTSTATUS type and canonical status codes                     | —               |  [ ]   |
| 💎  |   2   | SYSCALL/SYSRET fast path (IA32_LSTAR)                        | TODO-04 §3–§4   |  [ ]   |
| 💎  |   3   | INT 0x2E compatibility path                                  | §2              |  [ ]   |
| 💎  |   4   | System Service Descriptor Table (SSDT) — 200+ entries        | §1              |  [ ]   |
| 💎  |   5   | Nt/Zw naming and existing syscall migration                  | §1, §4          |  [ ]   |
| 💎  |   6   | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile | §5, TODO-03 §3 |  [ ]   |
| 💎  |   7   | NtCreateProcess / NtCreateThread / process-thread lifecycle  | §5, TODO-03 §5  |  [ ]   |
| 💎  |   8   | Sync objects + NtWaitForMultipleObjects                      | §5, TODO-03 §6  |  [ ]   |
| 💎  |   9   | Virtual memory (alloc, free, protect, lock)                  | §5              |  [ ]   |
| 💎  |  10   | NtQuerySystemInformation / NtQueryInformationProcess         | §5              |  [ ]   |
| ⭐  |  11   | Extended error information (IOSB + TEB LastError)            | §5, TODO-04 §6  |  [ ]   |
| ⭐  |  12   | ZwXxx kernel-mode alias layer with privilege assertion       | §4, §5          |  [ ]   |
| 💎  |  13   | File metadata and device control                             | §6              |  [ ]   |
| 💎  |  14   | Registry syscalls                                            | §5, TODO-13 §4  |  [ ]   |
| 💎  |  15   | Token and access control syscalls                            | §5, TODO-11 §4  |  [ ]   |
| 💎  |  16   | Directory and symbolic link object syscalls                  | §5, TODO-03 §4  |  [ ]   |
| 💎  |  17   | Section and memory-mapped file syscalls                      | §5, TODO-03 §7  |  [ ]   |
| 💎  |  18   | Timer control syscalls                                       | §5, TODO-07 §7  |  [ ]   |
| 💎  |  19   | ALPC / LPC port syscalls                                     | §5, TODO-12 §8  |  [ ]   |
| 💎  |  20   | Exception and debug syscalls                                 | §5, TODO-10 §4  |  [ ]   |
| 💎  |  21   | Power and system control                                     | §5, TODO-15     |  [ ]   |
| 💎  |  22   | Atom, locale, and miscellaneous                              | §5              |  [ ]   |
| ⭐  |  23   | Syscall audit and tracing hook                               | §4              |  [ ]   |

> 💎 = parity — Windows NT and Linux both have equivalents for these categories.
> ⭐ = exclusive — the ZwXxx privilege layer, the audit hook, and the IOSB/LastError unified path go beyond what Linux offers.

---

## 1. NTSTATUS Type and Canonical Status Codes
Define the NT status type and the full set of codes needed across all 200+ syscall endpoints.

- [ ] Create `include/kernel/nt/ntstatus.h`:
  - `typedef uint32_t NTSTATUS`
  - Severity macros: `NT_SUCCESS(s)` = `((s) >> 30) == 0`, `NT_INFORMATION(s)` = `((s) >> 30) == 1`, `NT_WARNING(s)` = `((s) >> 30) == 2`, `NT_ERROR(s)` = `((s) >> 30) == 3`
  - **Success / informational:**
    - `STATUS_SUCCESS                    0x00000000`
    - `STATUS_PENDING                    0x00000103`
    - `STATUS_BUFFER_OVERFLOW            0x80000005` — data truncated; partial result returned
    - `STATUS_NO_MORE_FILES              0x80000006` — directory enumeration exhausted
    - `STATUS_NO_MORE_ENTRIES            0x8000001A` — registry/object enumeration exhausted
    - `STATUS_ALERTED                    0x00000101` — thread was alerted during wait
    - `STATUS_TIMEOUT                    0x00000102` — wait timed out (not an error)
  - **Error codes — object / handle:**
    - `STATUS_UNSUCCESSFUL               0xC0000001`
    - `STATUS_NOT_IMPLEMENTED            0xC0000002`
    - `STATUS_INVALID_INFO_CLASS         0xC0000003`
    - `STATUS_ACCESS_VIOLATION           0xC0000005` — user-buffer probe failed
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
  - **Error codes — sync:**
    - `STATUS_SEMAPHORE_LIMIT_EXCEEDED   0xC0000044`
    - `STATUS_MUTANT_NOT_OWNED           0xC0000046`
  - **Error codes — file I/O:**
    - `STATUS_END_OF_FILE                0xC0000011`
    - `STATUS_FILE_LOCK_CONFLICT         0xC0000054`
    - `STATUS_RANGE_NOT_LOCKED           0xC000007E`
    - `STATUS_DELETE_PENDING             0xC0000056`
    - `STATUS_CANNOT_DELETE              0xC0000121`
    - `STATUS_DIRECTORY_NOT_EMPTY        0xC0000101`
    - `STATUS_NOT_A_DIRECTORY            0xC0000103`
    - `STATUS_FILE_IS_A_DIRECTORY        0xC00000BA`
  - **Error codes — process / thread:**
    - `STATUS_PROCESS_IS_TERMINATING     0xC000010A`
    - `STATUS_THREAD_IS_TERMINATING      0xC000004B`
    - `STATUS_THREAD_NOT_IN_PROCESS      0xC000012A`
    - `STATUS_SUSPEND_COUNT_EXCEEDED     0xC000004A`
  - **Error codes — memory:**
    - `STATUS_CONFLICTING_ADDRESSES      0xC0000018`
    - `STATUS_SECTION_NOT_EXTENDED       0xC0000087`
    - `STATUS_INVALID_PAGE_PROTECTION    0xC0000045`
    - `STATUS_ALREADY_COMMITTED          0xC0000021`
    - `STATUS_MEMORY_NOT_ALLOCATED       0xC00000A0`
  - **Error codes — registry:**
    - `STATUS_KEY_DELETED                0xC000017C`
    - `STATUS_KEY_HAS_CHILDREN           0xC0000180`
    - `STATUS_CHILD_MUST_BE_VOLATILE     0xC0000181`
  - **Error codes — security:**
    - `STATUS_PRIVILEGE_NOT_HELD         0xC0000061`
    - `STATUS_BAD_IMPERSONATION_LEVEL    0xC00000A5`
  - **Error codes — debug:**
    - `STATUS_DEBUGGER_INACTIVE          0xC0000354`
    - `STATUS_PORT_NOT_SET               0xC0000353`
  - **Error codes — power / misc:**
    - `STATUS_NOT_SUPPORTED              0xC00000BB`
    - `STATUS_INVALID_DEVICE_REQUEST     0xC0000010`
    - `STATUS_DEVICE_NOT_READY           0xC00000A3`
- [ ] Create `include/kernel/nt/nt_types.h` with foundational NT types:
  - `HANDLE` — already `typedef int32_t HANDLE` in `include/kernel/ob/handle_table.h`; re-export, do not redefine
  - `IO_STATUS_BLOCK` — `NTSTATUS Status`, `uint64_t Information`
  - `UNICODE_STRING` — re-export from `include/kernel/ob/peb.h` (already defined per TODO-04)
  - `OBJECT_ATTRIBUTES` — `uint64_t Length`, `HANDLE RootDirectory`, `UNICODE_STRING *ObjectName`, `uint32_t Attributes` (`OBJ_CASE_INSENSITIVE = 0x40`, `OBJ_KERNEL_HANDLE = 0x200`, `OBJ_INHERIT = 0x02`, `OBJ_OPENIF = 0x80`)
  - `LARGE_INTEGER` — re-export from `include/kernel/ob/peb.h` (already defined per TODO-04)
  - `ACCESS_MASK` = `uint32_t`; `GENERIC_READ = 0x80000000`, `GENERIC_WRITE = 0x40000000`, `GENERIC_EXECUTE = 0x20000000`, `GENERIC_ALL = 0x10000000`
  - `CLIENT_ID` — `uint64_t UniqueProcess`, `uint64_t UniqueThread`
  - `CONTEXT` — forward-declare; full definition in TODO-10
- [ ] Annotate each `NTSTATUS` code with the condition that triggers it — serves as inline documentation
- [ ] Commit: `"kernel: nt — NTSTATUS type and canonical status codes"`

**Test checkpoint:** `bash scripts/build.sh` → `=== BUILD OK ===`. `NT_SUCCESS(0)` == true, `NT_ERROR(0xC0000001)` == true, `NT_WARNING(0x80000005)` == true, `NT_INFORMATION(0x00000101)` == true verified by unit test.

## 2. SYSCALL/SYSRET Fast Path

Replace `INT 0x80` with the x86-64 `SYSCALL`/`SYSRET` instruction pair. On `SYSCALL`: CPU saves RIP→RCX, RFLAGS→R11; jumps to `IA32_LSTAR`. On `SYSRET`: restores RIP from RCX, RFLAGS from R11; returns to ring 3.

- [ ] Use existing MSR constants from `include/kernel/msr.h`:
  - `MSR_IA32_STAR    0xC0000081` — CS/SS selectors for SYSCALL/SYSRET
  - `MSR_IA32_LSTAR   0xC0000082` — 64-bit SYSCALL entry point address
  - `MSR_IA32_FMASK   0xC0000084` — RFLAGS mask to clear on SYSCALL entry
- [ ] Write `syscall_entry` in `src/kernel/sched/syscall_entry.asm`:
  - `swapgs` (exchange user GS/TEB → kernel GS/per-CPU) — requires TODO-04 §3 to be done first
  - Save user RSP; load kernel RSP from `IA32_KERNEL_GS_BASE` or per-CPU TSS RSP0
  - Push minimal callee-save frame (do not push RCX/R11 — they are the saved user RIP/RFLAGS)
  - `call syscall_dispatch` — C handler taking `(syscall_number, arg1..arg5)`
  - Restore frame; `swapgs`; `sysretq`
- [ ] In Phase 1 init (after GDT/IDT, before scheduler): call `syscall_init_fast()`:
  - `wrmsr(MSR_IA32_STAR,  (GDT_KERNEL_CODE << 32) | ((GDT_USER_CODE - 16) << 48))` — ring-0 CS on SYSCALL, ring-3 CS on SYSRET (SYSRET loads CS from STAR[63:48]+16)
  - `wrmsr(MSR_IA32_LSTAR, (uint64_t)syscall_entry)` — entry point
  - `wrmsr(MSR_IA32_FMASK, 0x200)` — clear IF (interrupts off on entry)
  - Set `EFER_SCE` bit in `MSR_IA32_EFER` (`0xC0000080`) to enable `SYSCALL`/`SYSRET`
- [ ] `syscall_dispatch(uint64_t number, uint64_t a1..a5)` — calls into SSDT (§4)
- [ ] Keep `INT 0x80` registered in IDT for the transition period; remove after §3 INT 0x2E is up
- [ ] Commit: `"kernel: nt — SYSCALL/SYSRET fast path init"`

**Test checkpoint:** Serial log: `"syscall: fast path (SYSCALL/SYSRET) enabled"` during Phase 1. `POST16(0xD200)` entry, `POST16(0xD201)` exit. Ring-3 `syscall` instruction reaches `syscall_dispatch` without GPF. Verify on QEMU WHPX, TCG, VirtualBox, bare metal.

## 3. INT 0x2E Compatibility Path
Windows NT's original software-interrupt syscall vector. Required for early ntdll and any code that does not use `SYSCALL`.

- [ ] Register a new IDT handler for vector `0x2E` in `idt.c`:
  - Entry stub saves full register frame (same layout as INT 0x80 stub)
  - `swapgs` if coming from ring 3 (check CS & 3)
  - Calls `syscall_dispatch(rax, rdi, rsi, rdx, r10, r8, r9)` (NT calling convention for syscall args)
  - `swapgs` on return; `iretq`
- [ ] The INT 0x2E handler shares `syscall_dispatch`; no separate dispatch logic
- [ ] After INT 0x2E is verified working, keep INT 0x80 as a second alias until all existing user-mode test binaries are updated to use the new calling convention
- [ ] Commit: `"kernel: nt — INT 0x2E syscall compatibility path"`

**Test checkpoint:** Ring-3 `int 0x2E` with RAX=0x0015 reaches `NtClose` handler. Same register mapping as SYSCALL path. `POST16(0xD300)` entry, `POST16(0xD301)` exit. Verify on all 4 platforms.

## 4. System Service Descriptor Table (SSDT)

The SSDT is a flat array of function pointers indexed by the 12-bit service number in RAX. `ntdll` stubs do `mov rax, <service_number>; syscall`. This section defines the complete service number allocation for all 200+ NT API endpoints. Numbers are stable — changing them is an ABI break.

- [ ] Define `SSDT_ENTRY` and `SSDT_TABLE` in `include/kernel/nt/ssdt.h`:
  - `typedef NTSTATUS (*SSDT_HANDLER)(uint64_t a1, a2, a3, a4, a5, a6)`
  - `SSDT_TABLE` — array of `SSDT_HANDLER` + count + table name string
- [ ] Allocate Win32k shadow SSDT stub (table 1) as empty placeholder — filled by Win32k layer later
- [ ] Define complete service index assignments in `include/kernel/nt/service_numbers.h`:
- [ ] Implement `syscall_dispatch`: index RAX into active SSDT; call handler; return `NTSTATUS` in RAX
- [ ] Unimplemented slots return `STATUS_NOT_IMPLEMENTED` rather than crashing
- [ ] Commit: `"kernel: nt — SSDT and service number table"`

### SSDT Master Table

> Service numbers organized by functional range. Each range has headroom for future additions. Endpoints marked `→ TODO-XX` are implemented in that TODO and registered here.

**0x0000–0x000F: Core Object and Handle Operations**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0000 | NtClose                          | §6       |
| 0x0001 | NtDuplicateObject                | §6       |
| 0x0002 | NtQueryObject                    | §6       |
| 0x0003 | NtMakeTemporaryObject            | §16      |
| 0x0004 | NtMakePermanentObject            | §16      |
| 0x0005 | NtSetInformationObject           | §16      |
| 0x0006 | NtWaitForSingleObject            | §8       |
| 0x0007 | NtWaitForMultipleObjects         | §8       |
| 0x0008 | NtSignalAndWaitForSingleObject   | §8       |
| 0x0009 | NtCompareObjects                 | §16      |

**0x0010–0x002F: File I/O**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0010 | NtCreateFile                     | §6       |
| 0x0011 | NtOpenFile                       | §6       |
| 0x0012 | NtReadFile                       | §6       |
| 0x0013 | NtWriteFile                      | §6       |
| 0x0014 | NtDeleteFile                     | §13      |
| 0x0015 | NtQueryInformationFile           | §13      |
| 0x0016 | NtSetInformationFile             | §13      |
| 0x0017 | NtQueryDirectoryFile             | §13      |
| 0x0018 | NtFlushBuffersFile               | §13      |
| 0x0019 | NtDeviceIoControlFile            | §13      |
| 0x001A | NtFsControlFile                  | §13      |
| 0x001B | NtCreateNamedPipeFile            | §6       |
| 0x001C | NtCreateMailslotFile             | §13      |
| 0x001D | NtLockFile                       | §13      |
| 0x001E | NtUnlockFile                     | §13      |
| 0x001F | NtNotifyChangeDirectoryFile      | §13      |
| 0x0020 | NtQueryVolumeInformationFile     | §13      |
| 0x0021 | NtSetVolumeInformationFile       | §13      |
| 0x0022 | NtQueryEaFile                    | §13      |
| 0x0023 | NtSetEaFile                      | §13      |
| 0x0024 | NtReadFileScatter                | §13      |
| 0x0025 | NtWriteFileGather                | §13      |
| 0x0026 | NtCancelIoFile                   | §13      |
| 0x0027 | NtCancelIoFileEx                 | §13      |
| 0x0028 | NtQueryAttributesFile            | §13      |
| 0x0029 | NtQueryFullAttributesFile        | §13      |

**0x0030–0x004F: Process and Thread**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0030 | NtCreateProcess                  | §7       |
| 0x0031 | NtCreateProcessEx                | §7       |
| 0x0032 | NtOpenProcess                    | §7       |
| 0x0033 | NtTerminateProcess               | §7       |
| 0x0034 | NtQueryInformationProcess        | §10      |
| 0x0035 | NtSetInformationProcess          | §7       |
| 0x0036 | NtCreateThread                   | §7       |
| 0x0037 | NtCreateThreadEx                 | §7       |
| 0x0038 | NtOpenThread                     | §7       |
| 0x0039 | NtTerminateThread                | §7       |
| 0x003A | NtResumeThread                   | §7       |
| 0x003B | NtSuspendThread                  | §7       |
| 0x003C | NtGetContextThread               | §7       |
| 0x003D | NtSetContextThread               | §7       |
| 0x003E | NtQueryInformationThread         | §7       |
| 0x003F | NtSetInformationThread           | §7       |
| 0x0040 | NtAlertThread                    | §7       |
| 0x0041 | NtAlertResumeThread              | §7       |
| 0x0042 | NtImpersonateThread              | §15      |
| 0x0043 | NtQueueApcThread                 | §7       |
| 0x0044 | NtYieldExecution                 | §5       |
| 0x0045 | NtCreateUserProcess              | §7       |
| 0x0046 | NtTestAlert                      | §7       |
| 0x0047 | NtDelayExecution                 | §7       |

**0x0050–0x006F: Memory Management**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0050 | NtAllocateVirtualMemory          | §9       |
| 0x0051 | NtFreeVirtualMemory              | §9       |
| 0x0052 | NtProtectVirtualMemory           | §9       |
| 0x0053 | NtQueryVirtualMemory             | §9       |
| 0x0054 | NtLockVirtualMemory              | §9       |
| 0x0055 | NtUnlockVirtualMemory            | §9       |
| 0x0056 | NtFlushVirtualMemory             | §9       |
| 0x0057 | NtReadVirtualMemory              | §9       |
| 0x0058 | NtWriteVirtualMemory             | §9       |
| 0x0059 | NtAllocateUserPhysicalPages      | §9       |
| 0x005A | NtFreeUserPhysicalPages          | §9       |
| 0x005B | NtMapUserPhysicalPages           | §9       |
| 0x005C | NtCreateSection                  | §17      |
| 0x005D | NtOpenSection                    | §17      |
| 0x005E | NtMapViewOfSection               | §17      |
| 0x005F | NtUnmapViewOfSection             | §17      |
| 0x0060 | NtExtendSection                  | §17      |
| 0x0061 | NtQuerySection                   | §17      |
| 0x0062 | NtAreMappedFilesTheSame          | §17      |

**0x0070–0x008F: Synchronization**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0070 | NtCreateEvent                    | §8       |
| 0x0071 | NtOpenEvent                      | §8       |
| 0x0072 | NtSetEvent                       | §8       |
| 0x0073 | NtResetEvent                     | §8       |
| 0x0074 | NtPulseEvent                     | §8       |
| 0x0075 | NtQueryEvent                     | §8       |
| 0x0076 | NtCreateMutant                   | §8       |
| 0x0077 | NtOpenMutant                     | §8       |
| 0x0078 | NtReleaseMutant                  | §8       |
| 0x0079 | NtQueryMutant                    | §8       |
| 0x007A | NtCreateSemaphore                | §8       |
| 0x007B | NtOpenSemaphore                  | §8       |
| 0x007C | NtReleaseSemaphore               | §8       |
| 0x007D | NtQuerySemaphore                 | §8       |
| 0x007E | NtCreateTimer                    | §18      |
| 0x007F | NtOpenTimer                      | §18      |
| 0x0080 | NtSetTimer                       | §18      |
| 0x0081 | NtCancelTimer                    | §18      |
| 0x0082 | NtQueryTimer                     | §18      |
| 0x0083 | NtSetTimerEx                     | §18      |
| 0x0084 | NtCreateKeyedEvent               | §8       |
| 0x0085 | NtOpenKeyedEvent                 | §8       |
| 0x0086 | NtWaitForKeyedEvent              | §8       |
| 0x0087 | NtReleaseKeyedEvent              | §8       |
| 0x0088 | NtCreateIoCompletion             | §13      |
| 0x0089 | NtSetIoCompletion                | §13      |
| 0x008A | NtRemoveIoCompletion             | §13      |
| 0x008B | NtQueryIoCompletion              | §13      |
| 0x008C | NtSetIoCompletionEx              | §13      |
| 0x008D | NtRemoveIoCompletionEx           | §13      |

**0x0090–0x00AF: Registry**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0090 | NtCreateKey                      | §14      |
| 0x0091 | NtCreateKeyTransacted            | §14      |
| 0x0092 | NtOpenKey                        | §14      |
| 0x0093 | NtOpenKeyTransacted              | §14      |
| 0x0094 | NtOpenKeyEx                      | §14      |
| 0x0095 | NtDeleteKey                      | §14      |
| 0x0096 | NtSetValueKey                    | §14      |
| 0x0097 | NtQueryValueKey                  | §14      |
| 0x0098 | NtDeleteValueKey                 | §14      |
| 0x0099 | NtEnumerateKey                   | §14      |
| 0x009A | NtEnumerateValueKey              | §14      |
| 0x009B | NtQueryKey                       | §14      |
| 0x009C | NtFlushKey                       | §14      |
| 0x009D | NtNotifyChangeKey                | §14      |
| 0x009E | NtNotifyChangeMultipleKeys       | §14      |
| 0x009F | NtRenameKey                      | §14      |
| 0x00A0 | NtSaveKey                        | §14      |
| 0x00A1 | NtSaveKeyEx                      | §14      |
| 0x00A2 | NtRestoreKey                     | §14      |
| 0x00A3 | NtLoadKey                        | §14      |
| 0x00A4 | NtLoadKeyEx                      | §14      |
| 0x00A5 | NtUnloadKey                      | §14      |
| 0x00A6 | NtUnloadKeyEx                    | §14      |
| 0x00A7 | NtQueryOpenSubKeys               | §14      |
| 0x00A8 | NtCompactKeys                    | §14      |
| 0x00A9 | NtCompressKey                    | §14      |
| 0x00AA | NtLockRegistryKey                | §14      |

**0x00B0–0x00CF: Security and Token**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x00B0 | NtOpenProcessToken               | §15      |
| 0x00B1 | NtOpenProcessTokenEx             | §15      |
| 0x00B2 | NtOpenThreadToken                | §15      |
| 0x00B3 | NtOpenThreadTokenEx              | §15      |
| 0x00B4 | NtQueryInformationToken          | §15      |
| 0x00B5 | NtSetInformationToken            | §15      |
| 0x00B6 | NtAdjustPrivilegesToken          | §15      |
| 0x00B7 | NtAdjustGroupsToken              | §15      |
| 0x00B8 | NtDuplicateToken                 | §15      |
| 0x00B9 | NtFilterToken                    | §15      |
| 0x00BA | NtCreateToken                    | §15      |
| 0x00BB | NtCompareTokens                  | §15      |
| 0x00BC | NtAccessCheck                    | §15      |
| 0x00BD | NtAccessCheckAndAuditAlarm       | §15      |
| 0x00BE | NtAccessCheckByType              | §15      |
| 0x00BF | NtPrivilegeCheck                 | §15      |
| 0x00C0 | NtPrivilegeObjectAuditAlarm      | §15      |
| 0x00C1 | NtSetSecurityObject              | §15      |
| 0x00C2 | NtQuerySecurityObject            | §15      |
| 0x00C3 | NtAllocateLocallyUniqueId        | §15      |
| 0x00C4 | NtCreateTokenEx                  | §15      |

**0x00D0–0x00EF: System Information and Control**

| Index  | Function                             | Section  |
|--------|--------------------------------------|----------|
| 0x00D0 | NtQuerySystemInformation             | §10      |
| 0x00D1 | NtSetSystemInformation               | §10      |
| 0x00D2 | NtQuerySystemEnvironmentValue        | §22      |
| 0x00D3 | NtSetSystemEnvironmentValue          | §22      |
| 0x00D4 | NtQuerySystemEnvironmentValueEx      | §22      |
| 0x00D5 | NtSetSystemEnvironmentValueEx        | §22      |
| 0x00D6 | NtEnumerateSystemEnvironmentValuesEx | §22      |
| 0x00D7 | NtShutdownSystem                     | §21      |
| 0x00D8 | NtDisplayString                      | §22      |
| 0x00D9 | NtRaiseHardError                     | §22      |
| 0x00DA | NtQueryDefaultLocale                 | §22      |
| 0x00DB | NtSetDefaultLocale                   | §22      |
| 0x00DC | NtQueryDefaultUILanguage             | §22      |
| 0x00DD | NtSetDefaultUILanguage               | §22      |
| 0x00DE | NtQueryInstallUILanguage             | §22      |
| 0x00DF | NtAddAtom                            | §22      |
| 0x00E0 | NtFindAtom                           | §22      |
| 0x00E1 | NtDeleteAtom                         | §22      |
| 0x00E2 | NtQueryInformationAtom               | §22      |

**0x00F0–0x00FF: Time and Timer (→ XREF TODO-07 §7)**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x00F0 | NtQuerySystemTime                | §18      |
| 0x00F1 | NtSetSystemTime                  | §18      |
| 0x00F2 | NtQueryPerformanceCounter        | §18      |
| 0x00F3 | NtQueryTimerResolution           | §18      |
| 0x00F4 | NtSetTimerResolution             | §18      |

**0x0100–0x011F: ALPC and LPC Ports (→ XREF TODO-12 §8)**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0100 | NtCreatePort                     | §19      |
| 0x0101 | NtCreateWaitablePort             | §19      |
| 0x0102 | NtConnectPort                    | §19      |
| 0x0103 | NtSecureConnectPort              | §19      |
| 0x0104 | NtAcceptConnectPort              | §19      |
| 0x0105 | NtCompleteConnectPort            | §19      |
| 0x0106 | NtListenPort                     | §19      |
| 0x0107 | NtReplyPort                      | §19      |
| 0x0108 | NtReplyWaitReceivePort           | §19      |
| 0x0109 | NtReplyWaitReceivePortEx         | §19      |
| 0x010A | NtRequestPort                    | §19      |
| 0x010B | NtRequestWaitReplyPort           | §19      |
| 0x010C | NtImpersonateClientOfPort        | §19      |
| 0x010D | NtReadRequestData                | §19      |
| 0x010E | NtWriteRequestData               | §19      |
| 0x010F | NtAlpcCreatePort                 | §19      |
| 0x0110 | NtAlpcConnectPort                | §19      |
| 0x0111 | NtAlpcConnectPortEx              | §19      |
| 0x0112 | NtAlpcAcceptConnectPort          | §19      |
| 0x0113 | NtAlpcSendWaitReceivePort        | §19      |
| 0x0114 | NtAlpcDisconnectPort             | §19      |
| 0x0115 | NtAlpcCancelMessage              | §19      |
| 0x0116 | NtAlpcCreatePortSection          | §19      |
| 0x0117 | NtAlpcDeletePortSection          | §19      |
| 0x0118 | NtAlpcCreateSectionView          | §19      |
| 0x0119 | NtAlpcDeleteSectionView          | §19      |
| 0x011A | NtAlpcCreateResourceReserve      | §19      |
| 0x011B | NtAlpcDeleteResourceReserve      | §19      |
| 0x011C | NtAlpcQueryInformation           | §19      |
| 0x011D | NtAlpcSetInformation             | §19      |
| 0x011E | NtAlpcQueryInformationMessage    | §19      |

**0x0120–0x012F: Namespace and Directory Objects**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0120 | NtCreateDirectoryObject          | §16      |
| 0x0121 | NtOpenDirectoryObject            | §16      |
| 0x0122 | NtQueryDirectoryObject           | §16      |
| 0x0123 | NtCreateSymbolicLinkObject       | §16      |
| 0x0124 | NtOpenSymbolicLinkObject         | §16      |
| 0x0125 | NtQuerySymbolicLinkObject        | §16      |

**0x0130–0x013F: Debug and Exception (→ XREF TODO-10 §4, TODO-18)**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0130 | NtRaiseException                 | §20      |
| 0x0131 | NtContinue                       | §20      |
| 0x0132 | NtDebugActiveProcess             | §20      |
| 0x0133 | NtDebugContinue                  | §20      |
| 0x0134 | NtRemoveProcessDebug             | §20      |
| 0x0135 | NtCreateDebugObject              | §20      |
| 0x0136 | NtWaitForDebugEvent              | §20      |
| 0x0137 | NtSetInformationDebugObject      | §20      |

**0x0140–0x014F: Power and Shutdown (→ XREF TODO-15)**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0140 | NtSetSystemPowerState            | §21      |
| 0x0141 | NtInitiatePowerAction            | §21      |
| 0x0142 | NtPowerInformation               | §21      |
| 0x0143 | NtGetDevicePowerState            | §21      |
| 0x0144 | NtSetThreadExecutionState        | §21      |
| 0x0145 | NtRequestWakeupLatency           | §21      |

**0x0150–0x015F: Audit and Tracing (Impossible OS exclusive)**

| Index  | Function                         | Section  |
|--------|----------------------------------|----------|
| 0x0150 | NtRegisterSyscallAuditHook       | §23      |
| 0x0151 | NtUnregisterSyscallAuditHook     | §23      |
| 0x0152 | NtQuerySyscallAuditState         | §23      |

> **Total: 235 service entries** across 15 functional ranges. 48 reserved slots for future use. Shadow SSDT (Win32k) has a separate index space starting at 0x1000.

**Test checkpoint:** `syscall_dispatch(0xFFFF)` returns `STATUS_NOT_IMPLEMENTED`, not crash. `syscall_dispatch(valid_index)` calls correct handler. Serial: `"ssdt: registered N services"` during init.

## 5. Nt/Zw Naming and Existing Syscall Migration
Rename / wrap the existing 22 `SYS_*` implementations to their `NtXxx` equivalents, change return types to `NTSTATUS`, and register them in the SSDT at the indices defined in §4.

- [ ] Add `NtWriteFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_WRITE` logic → SSDT 0x0013
- [ ] Add `NtReadFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_READ` logic → SSDT 0x0012
- [ ] Add `NtTerminateProcess(HANDLE, NTSTATUS)` replacing `SYS_EXIT` → SSDT 0x0033
- [ ] Add `NtYieldExecution()` replacing `SYS_YIELD` → SSDT 0x0044; returns `STATUS_SUCCESS`
- [ ] Add `NtCreateProcess`/`NtCreateThread` wrapping fork/exec paths → SSDT 0x0030/0x0036 (§7)
- [ ] Add `NtWaitForSingleObject(HANDLE, timeout)` replacing `SYS_WAITPID` → SSDT 0x0006
- [ ] Add `NtQueryDirectoryFile` wrapping `SYS_READDIR` → SSDT 0x0017
- [ ] Add `NtQuerySystemInformation(SystemProcessInformation)` wrapping `SYS_GETPROCS` → SSDT 0x00D0
- [ ] Add `NtTerminateProcess` for kill → SSDT 0x0033
- [ ] Add `NtQuerySystemInformation(SystemTimeOfDayInformation)` for uptime → SSDT 0x00D0
- [ ] Add `NtShutdownSystem(ShutdownReboot / ShutdownPowerOff)` → SSDT 0x00D7
- [ ] Add `NtCreateNamedPipeFile` / `NtReadFile` / `NtWriteFile` wrapping pipe → SSDT 0x001B
- [ ] Add `NtCreateSection` / `NtMapViewOfSection` wrapping shmem → SSDT 0x005C/0x005E (→ XREF TODO-03 §7)
- [ ] Keep `SYS_*` macros as compile-time aliases pointing to the same SSDT indices for transition
- [ ] Change all `sys_*` implementations to return `NTSTATUS`; convert error paths to `STATUS_*` codes
- [ ] Commit: `"kernel: nt — migrate existing syscalls to NtXxx naming and NTSTATUS"`

**Test checkpoint:** All 22 existing syscalls still work via old `SYS_*` macros (backward compat). `NtWriteFile` returns `STATUS_SUCCESS` on valid write. Serial: existing boot/desktop tests pass without regression.

## 6. NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile
Core file I/O entry points routed through the Object Manager (→ XREF TODO-03).

- [ ] `NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength)`:
  - Parse `ObjectAttributes->ObjectName` via Ob namespace (TODO-03 §4)
  - Map `CreateDisposition` (`FILE_OPEN`, `FILE_CREATE`, `FILE_SUPERSEDE`, `FILE_OPEN_IF`, `FILE_OVERWRITE`, `FILE_OVERWRITE_IF`) to VFS flags
  - Call `vfs_open()` → wrap result as an Ob File object → allocate handle via `ObpAllocateHandle`
  - Populate `IoStatusBlock->Status` and `IoStatusBlock->Information` (`FILE_CREATED` / `FILE_OPENED` / `FILE_OVERWRITTEN` / `FILE_SUPERSEDED`)
- [ ] `NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions)`:
  - Subset of `NtCreateFile` with `CreateDisposition = FILE_OPEN`
- [ ] `NtClose(Handle)`: call `ObpFreeHandle` (TODO-03 §9); return `STATUS_SUCCESS` or `STATUS_INVALID_HANDLE`
- [ ] `NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key)`: look up File object via handle table; call `vfs_read`; fill IOSB
- [ ] `NtWriteFile(...)`: symmetric with NtReadFile
- [ ] `NtCreateNamedPipeFile(...)`: create a named pipe File object
- [ ] Commit: `"kernel: nt — NtCreateFile, NtOpenFile, NtClose, NtReadFile, NtWriteFile"`

**Test checkpoint:** `NtCreateFile` on `C:\Impossible\System\Logs\kernel.log` returns `STATUS_SUCCESS` + valid HANDLE. `NtClose(handle)` returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`. `NtReadFile` populates IOSB correctly.

## 7. NtCreateProcess / NtCreateThread / Process-Thread Lifecycle
Process and thread creation, suspension, termination, and thread context access through the Ob-managed process model (→ XREF TODO-03 §5).

- [ ] `NtCreateProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, InheritObjectTable, SectionHandle, DebugPort, ExceptionPort)`:
  - Allocate new `task_t` via `task_create` equivalent; register as `ObpProcessType` object
  - If `InheritObjectTable` = TRUE, copy inheritable handles (TODO-03 §10)
  - Return process HANDLE via `ObpAllocateHandle`
- [ ] `NtCreateProcessEx(...)`: extended version with additional flags (Job assignment, etc.)
- [ ] `NtCreateUserProcess(...)`: combined process + thread creation (Windows Vista+ path)
- [ ] `NtCreateThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, ClientId, ThreadContext, InitialTeb, CreateSuspended)`:
  - Allocate thread struct; populate TEB (TODO-04 §6); register as `ObpThreadType` object
  - If `CreateSuspended`, start thread in suspended state (THREAD_SUSPEND_COUNT = 1)
  - Return thread HANDLE
- [ ] `NtCreateThreadEx(...)`: extended version with create flags and attribute list
- [ ] `NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId)`:
  - Look up by PID in the Ob namespace `\KernelObjects\Process<PID>`
- [ ] `NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId)`: symmetric
- [ ] `NtTerminateProcess(ProcessHandle, ExitStatus)`: terminate process and all threads
- [ ] `NtTerminateThread(ThreadHandle, ExitStatus)`: terminate a single thread
- [ ] `NtResumeThread(ThreadHandle, PreviousSuspendCount)`: decrement suspend count; schedule if 0
- [ ] `NtSuspendThread(ThreadHandle, PreviousSuspendCount)`: increment suspend count; deschedule
- [ ] `NtGetContextThread(ThreadHandle, Context)`: read thread register state
- [ ] `NtSetContextThread(ThreadHandle, Context)`: write thread register state (→ XREF TODO-10 for CONTEXT struct)
- [ ] `NtQueryInformationThread(ThreadHandle, ThreadInformationClass, Buffer, Length)`:
  - `ThreadBasicInformation (0)`: TEB address, client ID, priority
  - `ThreadTimes (1)`: creation time, user time, kernel time
- [ ] `NtSetInformationThread(ThreadHandle, ThreadInformationClass, Buffer, Length)`:
  - `ThreadPriority (2)`: set scheduling priority
  - `ThreadBasePriority (3)`: set base priority
  - `ThreadAffinityMask (4)`: set CPU affinity
  - `ThreadIdealProcessor (13)`: set preferred CPU
- [ ] `NtSetInformationProcess(ProcessHandle, ProcessInformationClass, Buffer, Length)`:
  - `ProcessPriorityClass (18)`: set process priority class
  - `ProcessDefaultHardErrorMode (12)`: set hard error mode
- [ ] `NtAlertThread(ThreadHandle)`: alert a waiting thread
- [ ] `NtAlertResumeThread(ThreadHandle, PreviousSuspendCount)`: alert + resume
- [ ] `NtTestAlert()`: consume pending alert for current thread
- [ ] `NtQueueApcThread(ThreadHandle, ApcRoutine, ApcArgument1/2/3)`: queue user-mode APC
- [ ] `NtDelayExecution(Alertable, DelayInterval)`: thread sleep with optional alert wake
- [ ] Commit: `"kernel: nt — NtCreateProcess, NtCreateThread, full process-thread lifecycle"`

**Test checkpoint:** `NtCreateProcess` returns valid HANDLE; PID visible in `\KernelObjects\`. `NtCreateThread` with `CreateSuspended=TRUE` doesn't run until `NtResumeThread`. `NtGetContextThread` returns valid RIP for a suspended thread. `NtDelayExecution` sleeps for the correct interval.

## 8. Synchronisation Objects + NtWaitForMultipleObjects
Synchronisation objects through Ob-managed named types (→ XREF TODO-03 §6). Includes multi-wait and keyed event support.

- [ ] `NtCreateEvent(EventHandle, DesiredAccess, ObjectAttributes, EventType, InitialState)`:
  - `EventType`: `NotificationEvent (0)` = manual-reset, `SynchronizationEvent (1)` = auto-reset
  - Route through `ObpEventType`; name in `\BaseNamedObjects\` if `ObjectAttributes->ObjectName` set
- [ ] `NtOpenEvent(EventHandle, DesiredAccess, ObjectAttributes)`: open by name
- [ ] `NtSetEvent(EventHandle, PreviousState)`: signal the event; wake waiting threads
- [ ] `NtResetEvent(EventHandle, PreviousState)`: clear the event
- [ ] `NtPulseEvent(EventHandle, PreviousState)`: set + reset atomically (wakes one waiter)
- [ ] `NtQueryEvent(EventHandle, EventBasicInformation, Buffer, Length)`: get event type + state
- [ ] `NtWaitForSingleObject(Handle, Alertable, Timeout)`: block until object is signalled or timeout
- [ ] `NtWaitForMultipleObjects(Count, Handles, WaitType, Alertable, Timeout)`:
  - `WaitType`: `WaitAll (0)` = all objects must be signalled, `WaitAny (1)` = any one
  - Returns index of the satisfied object (for `WaitAny`) or `STATUS_WAIT_0`
  - Maximum 64 handles per call (`MAXIMUM_WAIT_OBJECTS = 64`)
- [ ] `NtSignalAndWaitForSingleObject(ObjectToSignal, WaitObject, Alertable, Timeout)`:
  - Atomic signal-then-wait; used by `SignalObjectAndWait()` Win32 API
- [ ] `NtCreateMutant(MutantHandle, DesiredAccess, ObjectAttributes, InitialOwner)`: Ob-wrapped mutex
- [ ] `NtOpenMutant(MutantHandle, DesiredAccess, ObjectAttributes)`: open by name
- [ ] `NtReleaseMutant(MutantHandle, PreviousCount)`: release mutex; return `STATUS_MUTANT_NOT_OWNED` if caller does not own it
- [ ] `NtQueryMutant(MutantHandle, MutantBasicInformation, Buffer, Length)`: get owner + count
- [ ] `NtCreateSemaphore(SemaphoreHandle, DesiredAccess, ObjectAttributes, InitialCount, MaximumCount)`
- [ ] `NtOpenSemaphore(SemaphoreHandle, DesiredAccess, ObjectAttributes)`: open by name
- [ ] `NtReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)`
- [ ] `NtQuerySemaphore(SemaphoreHandle, SemaphoreBasicInformation, Buffer, Length)`: get count + max
- [ ] `NtCreateKeyedEvent(KeyedEventHandle, DesiredAccess, ObjectAttributes, Flags)`:
  - Lightweight futex-style primitive; used internally by NTDLL for SRW locks and condition variables
- [ ] `NtOpenKeyedEvent(...)`: open by name
- [ ] `NtWaitForKeyedEvent(KeyedEventHandle, KeyValue, Alertable, Timeout)`: wait on a specific key
- [ ] `NtReleaseKeyedEvent(KeyedEventHandle, KeyValue, Alertable, Timeout)`: release a specific key
- [ ] Commit: `"kernel: nt — sync objects, NtWaitForMultipleObjects, keyed events"`

**Test checkpoint:** `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` round-trip succeeds. `NtWaitForMultipleObjects(WaitAny)` returns correct index. `NtCreateMutant` with `InitialOwner=TRUE` is owned by caller. Named objects visible in `\BaseNamedObjects\`. Keyed event wait/release pair succeeds between two threads.

## 9. Virtual Memory (Alloc, Free, Protect, Lock, Cross-Process)
Virtual memory management entry points — covers the full NT virtual memory API surface.

- [ ] `NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect)`:
  - `AllocationType`: `MEM_COMMIT = 0x1000`, `MEM_RESERVE = 0x2000`, `MEM_RESET = 0x80000`
  - `Protect`: `PAGE_NOACCESS = 0x01`, `PAGE_READONLY = 0x02`, `PAGE_READWRITE = 0x04`, `PAGE_EXECUTE = 0x10`, `PAGE_EXECUTE_READ = 0x20`, `PAGE_EXECUTE_READWRITE = 0x40`, `PAGE_GUARD = 0x100`
  - Route to VMM page allocator; return allocated address in `*BaseAddress`
- [ ] `NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType)`:
  - `FreeType`: `MEM_RELEASE = 0x8000`, `MEM_DECOMMIT = 0x4000`
- [ ] `NtProtectVirtualMemory(ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect)`:
  - Change page protection flags on committed pages; updates PTE bits
  - Critical for: JIT compilation (RW→RX), DEP enforcement, shadow stacks
- [ ] `NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass, Buffer, Length)`:
  - `MemoryBasicInformation (0)`: base, allocation base, protect flags, state, type, region size
  - `MemoryWorkingSetExInformation (4)`: working set pages with share/lock info
- [ ] `NtLockVirtualMemory(ProcessHandle, BaseAddress, RegionSize, MapType)`: pin pages in physical memory (prevent page-out)
- [ ] `NtUnlockVirtualMemory(ProcessHandle, BaseAddress, RegionSize, MapType)`: unpin pages
- [ ] `NtFlushVirtualMemory(ProcessHandle, BaseAddress, RegionSize, IoStatus)`: flush dirty pages to backing store
- [ ] `NtReadVirtualMemory(ProcessHandle, BaseAddress, Buffer, BufferSize, NumberOfBytesRead)`:
  - Read from another process's address space (debugger, tool support)
- [ ] `NtWriteVirtualMemory(ProcessHandle, BaseAddress, Buffer, BufferSize, NumberOfBytesWritten)`:
  - Write to another process's address space (code injection, debugger breakpoints)
- [ ] `NtAllocateUserPhysicalPages(ProcessHandle, NumberOfPages, UserPfnArray)`: AWE-style allocation
- [ ] `NtFreeUserPhysicalPages(ProcessHandle, NumberOfPages, UserPfnArray)`: AWE free
- [ ] `NtMapUserPhysicalPages(VirtualAddress, NumberOfPages, UserPfnArray)`: AWE map
- [ ] Commit: `"kernel: nt — NtAllocate/Free/Protect/Lock/Read/WriteVirtualMemory"`

**Test checkpoint:** `NtAllocateVirtualMemory` with `MEM_COMMIT | PAGE_READWRITE` returns usable address; write+read round-trip. `NtProtectVirtualMemory` changes RW→RO; write attempt faults. `NtFreeVirtualMemory` with `MEM_RELEASE` returns `STATUS_SUCCESS`. `NtReadVirtualMemory` from kernel to user address space succeeds.

## 10. NtQuerySystemInformation / NtQueryInformationProcess
Provides OS version, process list, performance counters, and detailed process info to ntdll and user-mode tools.

- [ ] `NtQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ReturnLength)`:
  - `SystemBasicInformation (0)`: number of processors, page size, min/max user address, allocation granularity
  - `SystemPerformanceInformation (2)`: available pages, commit total, commit limit (from PMM stats)
  - `SystemTimeOfDayInformation (3)`: boot time, current time, time zone bias
  - `SystemProcessInformation (5)`: linked list of `SYSTEM_PROCESS_INFORMATION` — PID, name, thread count, handle count, memory usage
  - `SystemProcessorInformation (1)`: processor architecture, level, revision
  - `SystemModuleInformation (11)`: loaded kernel modules
  - `SystemHandleInformation (16)`: system-wide handle table dump
  - `SystemObjectInformation (17)`: object type statistics
  - `SystemInterruptInformation (23)`: per-CPU interrupt counts
  - `SystemExceptionInformation (33)`: exception statistics
  - `SystemRegistryQuotaInformation (37)`: registry size limits
  - `SystemProcessorPerformanceInformation (8)`: per-CPU idle/kernel/user times
  - Unimplemented classes return `STATUS_NOT_IMPLEMENTED`
- [ ] `NtSetSystemInformation(SystemInformationClass, Buffer, Length)`:
  - `SystemTimeSlipNotification (46)`: register time slip callback
  - Privileged operation — requires `SeSystemtimePrivilege` for time-related classes
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, Buffer, Length)`:
  - `ProcessBasicInformation (0)`: PEB address, PID, parent PID, exit status, affinity mask
  - `ProcessImageFileName (27)`: full path of the executable
  - `ProcessDebugPort (7)`: debug port handle (0 if not being debugged)
  - `ProcessWow64Information (26)`: WoW64 PEB address (NULL for native 64-bit)
  - `ProcessHandleCount (20)`: number of open handles
  - `ProcessSessionInformation (24)`: session ID
  - `ProcessTimes (4)`: creation, exit, kernel, user times
- [ ] Commit: `"kernel: nt — NtQuerySystemInformation and NtQueryInformationProcess"`

**Test checkpoint:** `NtQuerySystemInformation(SystemBasicInformation)` returns correct page size (4096) and processor count. `NtQueryInformationProcess(ProcessBasicInformation)` returns valid PEB address (0x7FFDE000). `SystemProcessInformation` enumerates all running processes.

## 11. Extended Error Information (IOSB + LastError)
NT propagates detailed error info through two channels: `IO_STATUS_BLOCK` (async I/O) and `TEB->LastErrorValue` (Win32 `GetLastError`). Both must be populated correctly.

- [ ] All file I/O `NtXxx` functions write final `NTSTATUS` into `IoStatusBlock->Status` and byte count / disposition into `IoStatusBlock->Information`
- [ ] On every `NTSTATUS` return from a syscall: if `NT_ERROR(status)`, also write the Win32 error translation into `TEB->LastErrorValue` (at `gs:[0x68]`) using a compact `RtlNtStatusToDosError` table for common codes
- [ ] `RtlNtStatusToDosError` minimal table: `STATUS_ACCESS_DENIED→5`, `STATUS_NO_MEMORY→8`, `STATUS_INVALID_HANDLE→6`, `STATUS_OBJECT_NAME_NOT_FOUND→2`, `STATUS_NOT_IMPLEMENTED→50`, `STATUS_INVALID_PARAMETER→87`, `STATUS_BUFFER_TOO_SMALL→122`, `STATUS_ACCESS_VIOLATION→998`, `STATUS_PRIVILEGE_NOT_HELD→1314`
- [ ] This is the only place `TEB->LastErrorValue` is written by kernel code — usermode `SetLastError` writes it directly via GS offset without a syscall
- [ ] Commit: `"kernel: nt — IOSB and TEB LastErrorValue propagation"`

**Test checkpoint:** After a failing `NtOpenFile` (non-existent path), `gs:[0x68]` == Win32 error code (2 = FILE_NOT_FOUND). After `NtReadFile`, IOSB `Status == STATUS_SUCCESS`, `Information == bytes_read`.

## 12. ZwXxx Kernel-Mode Alias Layer

`ZwXxx` names are identical to `NtXxx` in user mode. In kernel mode (`CPL=0`), `ZwXxx` calls bypass the user-mode probe and use kernel-mode access rights directly. This is the convention all of Windows' own drivers and executive components use.

- [ ] Add a `ZwXxx` header `include/kernel/nt/zw.h` that declares each `ZwXxx` as an alias for the same SSDT entry point
- [ ] In the SSDT dispatcher: if caller is CPL=0, skip user-buffer probe and pointer validation; if CPL=3, validate all user-space buffer pointers before use (probe for read/write)
- [ ] Add `ProbeForRead(Address, Length, Alignment)` and `ProbeForWrite(Address, Length, Alignment)` helper functions — verify address range is user-mode accessible
- [ ] Add a `ASSERT_KERNEL_CALLER()` macro that fires `STATUS_PRIVILEGE_NOT_HELD` if a kernel-only API is called from user mode
- [ ] Document the convention: kernel components call `Zw` variants; user-mode calls `Nt` variants; both resolve to the same implementation, differentiated only by CPL check
- [ ] Commit: `"kernel: nt — ZwXxx kernel-mode alias layer with CPL probe bypass"`

**Test checkpoint:** `ZwClose` from CPL=0 succeeds without user-buffer probe. CPL=3 call with kernel-space pointer returns `STATUS_ACCESS_VIOLATION`. `ASSERT_KERNEL_CALLER()` fires `STATUS_PRIVILEGE_NOT_HELD` from ring 3.

## 13. File Metadata and Device Control
Extended file operations: metadata queries, attribute modification, device I/O control, file locking, I/O completion ports. These are the Win32 `GetFileAttributes`, `SetFileTime`, `DeviceIoControl`, `LockFile` foundation.

- [ ] `NtQueryInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass)`:
  - `FileBasicInformation (4)`: creation/access/write/change times, attributes
  - `FileStandardInformation (5)`: allocation size, EOF, number of links, delete pending
  - `FileNameInformation (9)`: file name string
  - `FilePositionInformation (14)`: current byte offset
  - `FileAllInformation (18)`: combined basic + standard + name + position
  - `FileNetworkOpenInformation (34)`: all metadata for network redirector
- [ ] `NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass)`:
  - `FileBasicInformation (4)`: set timestamps and attributes
  - `FileDispositionInformation (13)`: mark for delete on close
  - `FileRenameInformation (10)`: rename file
  - `FilePositionInformation (14)`: set current offset
  - `FileEndOfFileInformation (20)`: truncate or extend file
  - `FileAllocationInformation (19)`: set allocated disk space
- [ ] `NtDeleteFile(ObjectAttributes)`: delete file by name without opening
- [ ] `NtQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan)`:
  - `FileDirectoryInformation (1)`: enumerate directory entries with full metadata
  - `FileBothDirectoryInformation (3)`: entries with 8.3 short name
  - `FileIdBothDirectoryInformation (37)`: entries with file ID
- [ ] `NtDeviceIoControlFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, IoControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength)`:
  - Routes I/O control requests to device drivers via IRP
  - `METHOD_BUFFERED`, `METHOD_IN_DIRECT`, `METHOD_OUT_DIRECT`, `METHOD_NEITHER`
- [ ] `NtFsControlFile(...)`: filesystem-specific control (defrag, compression, sparse file)
- [ ] `NtFlushBuffersFile(FileHandle, IoStatusBlock)`: flush file data to disk
- [ ] `NtLockFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, ByteOffset, Length, Key, FailImmediately, ExclusiveLock)`: byte-range lock
- [ ] `NtUnlockFile(FileHandle, IoStatusBlock, ByteOffset, Length, Key)`: release byte-range lock
- [ ] `NtNotifyChangeDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, BufferLength, CompletionFilter, WatchTree)`: file system change notification (ReadDirectoryChangesW foundation)
- [ ] `NtQueryVolumeInformationFile(FileHandle, IoStatusBlock, Buffer, Length, FsInformationClass)`:
  - `FileFsSizeInformation (3)`: total/available allocation units
  - `FileFsVolumeInformation (1)`: volume label, serial number
  - `FileFsAttributeInformation (5)`: file system name, max component length
- [ ] `NtQueryAttributesFile(ObjectAttributes, FileInformation)`: lightweight metadata query by name
- [ ] `NtCancelIoFile(FileHandle, IoStatusBlock)`: cancel pending I/O for calling thread
- [ ] `NtCancelIoFileEx(FileHandle, IoRequestToCancel, IoStatusBlock)`: cancel specific I/O request
- [ ] `NtCreateIoCompletion(IoCompletionHandle, DesiredAccess, ObjectAttributes, Count)`: I/O completion port
- [ ] `NtSetIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatus, IoStatusInformation)`: post completion
- [ ] `NtRemoveIoCompletion(IoCompletionHandle, KeyContext, ApcContext, IoStatusBlock, Timeout)`: dequeue completion
- [ ] `NtCreateMailslotFile(...)`: one-way IPC mailslot
- [ ] `NtReadFileScatter(...)` / `NtWriteFileGather(...)`: scatter/gather I/O
- [ ] Commit: `"kernel: nt — file metadata, device control, I/O completion ports"`

**Test checkpoint:** `NtQueryInformationFile(FileBasicInformation)` returns valid timestamps. `NtSetInformationFile(FileDispositionInformation)` marks file for delete; file removed after close. `NtDeviceIoControlFile` reaches driver dispatch. I/O completion port post + dequeue round-trip succeeds.

## 14. Registry Syscalls

> [!NOTE]
> Full registry engine implemented in TODO-13-registry-completion.md. This section wires the existing registry API into the SSDT with NT-compatible signatures and NTSTATUS return values.

- [ ] `NtCreateKey(KeyHandle, DesiredAccess, ObjectAttributes, TitleIndex, Class, CreateOptions, Disposition)` → SSDT 0x0090
- [ ] `NtOpenKey(KeyHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0092
- [ ] `NtOpenKeyEx(KeyHandle, DesiredAccess, ObjectAttributes, OpenOptions)` → SSDT 0x0094
- [ ] `NtDeleteKey(KeyHandle)` → SSDT 0x0095
- [ ] `NtSetValueKey(KeyHandle, ValueName, TitleIndex, Type, Data, DataSize)` → SSDT 0x0096
- [ ] `NtQueryValueKey(KeyHandle, ValueName, KeyValueInformationClass, KeyValueInformation, Length, ResultLength)` → SSDT 0x0097
- [ ] `NtDeleteValueKey(KeyHandle, ValueName)` → SSDT 0x0098
- [ ] `NtEnumerateKey(KeyHandle, Index, KeyInformationClass, KeyInformation, Length, ResultLength)` → SSDT 0x0099
- [ ] `NtEnumerateValueKey(KeyHandle, Index, KeyValueInformationClass, KeyValueInformation, Length, ResultLength)` → SSDT 0x009A
- [ ] `NtQueryKey(KeyHandle, KeyInformationClass, KeyInformation, Length, ResultLength)` → SSDT 0x009B
- [ ] `NtFlushKey(KeyHandle)` → SSDT 0x009C: flush to backing hive
- [ ] `NtNotifyChangeKey(KeyHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, CompletionFilter, WatchTree, Buffer, BufferSize, Asynchronous)` → SSDT 0x009D
- [ ] `NtRenameKey(KeyHandle, NewName)` → SSDT 0x009F
- [ ] `NtSaveKey(KeyHandle, FileHandle)` / `NtSaveKeyEx(...)` → SSDT 0x00A0/0x00A1
- [ ] `NtRestoreKey(KeyHandle, FileHandle, Flags)` → SSDT 0x00A2
- [ ] `NtLoadKey(ObjectAttributes, ObjectAttributes)` / `NtLoadKeyEx(...)` → SSDT 0x00A3/0x00A4
- [ ] `NtUnloadKey(ObjectAttributes)` / `NtUnloadKeyEx(...)` → SSDT 0x00A5/0x00A6
- [ ] Commit: `"kernel: nt — registry syscalls wired to SSDT"`

**Test checkpoint:** `NtCreateKey` under `\Registry\Machine\Software\Test` returns `STATUS_SUCCESS`. `NtSetValueKey` + `NtQueryValueKey` round-trip succeeds. `NtDeleteKey` removes the key. `NtEnumerateKey` iterates subkeys correctly.

## 15. Token and Access Control Syscalls

> [!NOTE]
> Token implementation exists in `src/kernel/security/token.c` (→ XREF TODO-11). NtAccessCheck routes through the Security Reference Monitor. This section wires these into the SSDT.

- [ ] `NtOpenProcessToken(ProcessHandle, DesiredAccess, TokenHandle)` → SSDT 0x00B0
- [ ] `NtOpenProcessTokenEx(ProcessHandle, DesiredAccess, HandleAttributes, TokenHandle)` → SSDT 0x00B1
- [ ] `NtOpenThreadToken(ThreadHandle, DesiredAccess, OpenAsSelf, TokenHandle)` → SSDT 0x00B2
- [ ] `NtOpenThreadTokenEx(ThreadHandle, DesiredAccess, OpenAsSelf, HandleAttributes, TokenHandle)` → SSDT 0x00B3
- [ ] `NtQueryInformationToken(TokenHandle, TokenInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x00B4:
  - `TokenUser (1)`, `TokenGroups (2)`, `TokenPrivileges (3)`, `TokenOwner (4)`, `TokenPrimaryGroup (5)`, `TokenDefaultDacl (6)`, `TokenSource (7)`, `TokenType (8)`, `TokenStatistics (10)`, `TokenSessionId (12)`
- [ ] `NtSetInformationToken(TokenHandle, TokenInformationClass, Buffer, Length)` → SSDT 0x00B5
- [ ] `NtAdjustPrivilegesToken(TokenHandle, DisableAllPrivileges, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B6
- [ ] `NtAdjustGroupsToken(TokenHandle, ResetToDefault, NewState, BufferLength, PreviousState, ReturnLength)` → SSDT 0x00B7
- [ ] `NtDuplicateToken(ExistingTokenHandle, DesiredAccess, ObjectAttributes, EffectiveOnly, TokenType, NewTokenHandle)` → SSDT 0x00B8
- [ ] `NtFilterToken(ExistingTokenHandle, Flags, SidsToDisable, PrivilegesToDelete, RestrictedSids, NewTokenHandle)` → SSDT 0x00B9
- [ ] `NtCreateToken(TokenHandle, DesiredAccess, ObjectAttributes, TokenType, AuthenticationId, ExpirationTime, User, Groups, Privileges, Owner, PrimaryGroup, DefaultDacl, Source)` → SSDT 0x00BA
- [ ] `NtAccessCheck(SecurityDescriptor, ClientToken, DesiredAccess, GenericMapping, PrivilegeSet, PrivilegeSetLength, GrantedAccess, AccessStatus)` → SSDT 0x00BC:
  - Core SRM decision point; routes to `SeAccessCheck()` in the Security Reference Monitor
- [ ] `NtPrivilegeCheck(ClientToken, RequiredPrivileges, Result)` → SSDT 0x00BF
- [ ] `NtSetSecurityObject(Handle, SecurityInformation, SecurityDescriptor)` → SSDT 0x00C1
- [ ] `NtQuerySecurityObject(Handle, SecurityInformation, SecurityDescriptor, Length, LengthNeeded)` → SSDT 0x00C2
- [ ] `NtAllocateLocallyUniqueId(Luid)` → SSDT 0x00C3
- [ ] Commit: `"kernel: nt — token and access control syscalls"`

**Test checkpoint:** `NtOpenProcessToken` returns valid token handle. `NtQueryInformationToken(TokenUser)` returns correct SID. `NtAccessCheck` against an object with DACL returns correct granted access. `NtAdjustPrivilegesToken` enables/disables a privilege.

## 16. Directory and Symbolic Link Object Syscalls
Namespace manipulation — create, open, and query Ob directory objects and symbolic links from user mode.

- [ ] `NtCreateDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0120
- [ ] `NtOpenDirectoryObject(DirectoryHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0121 (already implemented in ob.c)
- [ ] `NtQueryDirectoryObject(DirectoryHandle, Buffer, Length, ReturnSingleEntry, RestartScan, Context, ReturnLength)` → SSDT 0x0122 (already implemented in ob.c)
- [ ] `NtCreateSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes, LinkTarget)` → SSDT 0x0123
- [ ] `NtOpenSymbolicLinkObject(LinkHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x0124
- [ ] `NtQuerySymbolicLinkObject(LinkHandle, LinkTarget, ReturnedLength)` → SSDT 0x0125
- [ ] `NtMakeTemporaryObject(Handle)` → SSDT 0x0003: clear OB_FLAG_PERMANENT
- [ ] `NtMakePermanentObject(Handle)` → SSDT 0x0004: set OB_FLAG_PERMANENT (kernel-only)
- [ ] `NtSetInformationObject(Handle, ObjectInformationClass, Buffer, Length)` → SSDT 0x0005
- [ ] `NtCompareObjects(FirstObjectHandle, SecondObjectHandle)` → SSDT 0x0009: test if two handles refer to the same object
- [ ] Commit: `"kernel: nt — directory, symbolic link, and object management syscalls"`

**Test checkpoint:** `NtCreateDirectoryObject` creates `\Test`; `NtOpenDirectoryObject` opens it. `NtCreateSymbolicLinkObject` creates `\TestLink → \Test`; `NtQuerySymbolicLinkObject` returns `\Test`. `NtCompareObjects` returns `STATUS_SUCCESS` for two handles to the same object.

## 17. Section and Memory-Mapped File Syscalls

> [!NOTE]
> Section (shared memory) object type implemented in TODO-03 §7 (`ob_section.c`). This section wires it into the SSDT with full NT-compatible signatures.

- [ ] `NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle)` → SSDT 0x005C:
  - `AllocationAttributes`: `SEC_COMMIT = 0x8000000`, `SEC_RESERVE = 0x4000000`, `SEC_IMAGE = 0x1000000`, `SEC_NOCACHE = 0x10000000`
  - If `FileHandle` non-NULL: file-backed section; otherwise page-file-backed
- [ ] `NtOpenSection(SectionHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x005D
- [ ] `NtMapViewOfSection(SectionHandle, ProcessHandle, BaseAddress, ZeroBits, CommitSize, SectionOffset, ViewSize, InheritDisposition, AllocationType, Win32Protect)` → SSDT 0x005E
- [ ] `NtUnmapViewOfSection(ProcessHandle, BaseAddress)` → SSDT 0x005F
- [ ] `NtExtendSection(SectionHandle, NewMaximumSize)` → SSDT 0x0060
- [ ] `NtQuerySection(SectionHandle, SectionInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0061:
  - `SectionBasicInformation (0)`: base address, size, attributes
  - `SectionImageInformation (1)`: entry point, image base, stack info (for PE sections)
- [ ] `NtAreMappedFilesTheSame(File1MappedAsAnImage, File2MappedAsFile)` → SSDT 0x0062
- [ ] Commit: `"kernel: nt — section and memory-mapped file syscalls"`

**Test checkpoint:** `NtCreateSection` with `SEC_COMMIT` creates pagefile-backed section. `NtMapViewOfSection` maps into current process; write/read round-trip. `NtUnmapViewOfSection` unmaps. File-backed section maps file contents correctly.

## 18. Timer Control Syscalls

> [!NOTE]
> Timer object type implemented in TODO-03 (`ob_timer.c`). Time source APIs (NtQuerySystemTime, etc.) implemented in TODO-07 §7. This section provides the full timer control surface and SSDT wiring.

- [ ] `NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType)` → SSDT 0x007E:
  - `TimerType`: `NotificationTimer (0)` = manual-reset, `SynchronizationTimer (1)` = auto-reset
- [ ] `NtOpenTimer(TimerHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x007F
- [ ] `NtSetTimer(TimerHandle, DueTime, TimerApcRoutine, TimerContext, ResumeTimer, Period, PreviousState)` → SSDT 0x0080:
  - `DueTime`: negative = relative (100ns units), positive = absolute FILETIME
  - `Period`: 0 = one-shot, >0 = periodic (milliseconds)
- [ ] `NtCancelTimer(TimerHandle, CurrentState)` → SSDT 0x0081
- [ ] `NtQueryTimer(TimerHandle, TimerInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0082
- [ ] `NtSetTimerEx(TimerHandle, TimerSetInformationClass, Buffer, Length)` → SSDT 0x0083
- [ ] `NtQuerySystemTime(SystemTime)` → SSDT 0x00F0 (→ XREF TODO-07 §7)
- [ ] `NtSetSystemTime(SystemTime, PreviousTime)` → SSDT 0x00F1 (→ XREF TODO-07 §7)
- [ ] `NtQueryPerformanceCounter(PerformanceCounter, PerformanceFrequency)` → SSDT 0x00F2 (→ XREF TODO-07 §7)
- [ ] `NtQueryTimerResolution(MaximumTime, MinimumTime, CurrentTime)` → SSDT 0x00F3 (→ XREF TODO-07 §7)
- [ ] `NtSetTimerResolution(DesiredTime, SetResolution, ActualTime)` → SSDT 0x00F4
- [ ] Commit: `"kernel: nt — timer control and time query syscalls"`

**Test checkpoint:** `NtCreateTimer` + `NtSetTimer` with relative 100ms due time fires. `NtCancelTimer` cancels before fire returns `STATUS_SUCCESS`. `NtQueryPerformanceCounter` returns monotonically increasing value. `NtQueryTimerResolution` reports correct LAPIC timer resolution.

## 19. ALPC / LPC Port Syscalls

> [!NOTE]
> Full ALPC implementation in TODO-12-alpc-message-ports.md §8. This section reserves SSDT indices and provides the NT-compatible syscall signatures for both legacy LPC and modern ALPC.

**Legacy LPC (NT 3.x–5.x compatibility):**
- [ ] `NtCreatePort(PortHandle, ObjectAttributes, MaxConnectionInfoLength, MaxMessageLength, MaxPoolUsage)` → SSDT 0x0100
- [ ] `NtCreateWaitablePort(...)` → SSDT 0x0101
- [ ] `NtConnectPort(PortHandle, PortName, SecurityQos, ClientView, ServerView, MaxMessageLength, ConnectionInformation, ConnectionInformationLength)` → SSDT 0x0102
- [ ] `NtSecureConnectPort(...)` → SSDT 0x0103: with SID validation
- [ ] `NtAcceptConnectPort(PortHandle, PortContext, ConnectionRequest, AcceptConnection, ServerView, ClientView)` → SSDT 0x0104
- [ ] `NtCompleteConnectPort(PortHandle)` → SSDT 0x0105
- [ ] `NtListenPort(PortHandle, ConnectionRequest)` → SSDT 0x0106
- [ ] `NtReplyPort(PortHandle, ReplyMessage)` → SSDT 0x0107
- [ ] `NtReplyWaitReceivePort(PortHandle, PortContext, ReplyMessage, ReceiveMessage)` → SSDT 0x0108
- [ ] `NtReplyWaitReceivePortEx(PortHandle, PortContext, ReplyMessage, ReceiveMessage, Timeout)` → SSDT 0x0109
- [ ] `NtRequestPort(PortHandle, RequestMessage)` → SSDT 0x010A
- [ ] `NtRequestWaitReplyPort(PortHandle, RequestMessage, ReplyMessage)` → SSDT 0x010B
- [ ] `NtImpersonateClientOfPort(PortHandle, Message)` → SSDT 0x010C

**Modern ALPC (Vista+):**
- [ ] `NtAlpcCreatePort(PortHandle, ObjectAttributes, PortAttributes)` → SSDT 0x010F
- [ ] `NtAlpcConnectPort(PortHandle, PortName, ObjectAttributes, PortAttributes, Flags, RequiredServerSid, ConnectionMessage, BufferLength, OutMessageAttributes, InMessageAttributes, Timeout)` → SSDT 0x0110
- [ ] `NtAlpcConnectPortEx(...)` → SSDT 0x0111
- [ ] `NtAlpcAcceptConnectPort(PortHandle, ConnectionPortHandle, Flags, ObjectAttributes, PortAttributes, PortContext, ConnectionRequest, ConnectionMessageAttributes, AcceptConnection)` → SSDT 0x0112
- [ ] `NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMessage, SendMessageAttributes, ReceiveMessage, BufferLength, ReceiveMessageAttributes, Timeout)` → SSDT 0x0113
- [ ] `NtAlpcDisconnectPort(PortHandle, Flags)` → SSDT 0x0114
- [ ] `NtAlpcCancelMessage(PortHandle, Flags, MessageContext)` → SSDT 0x0115
- [ ] `NtAlpcCreatePortSection(PortHandle, Flags, SectionHandle, SectionSize, AlpcSectionHandle, ActualSectionSize)` → SSDT 0x0116
- [ ] `NtAlpcDeletePortSection(PortHandle, Flags, SectionHandle)` → SSDT 0x0117
- [ ] `NtAlpcCreateSectionView(PortHandle, Flags, ViewAttributes)` → SSDT 0x0118
- [ ] `NtAlpcDeleteSectionView(PortHandle, Flags, ViewBase)` → SSDT 0x0119
- [ ] `NtAlpcCreateResourceReserve(PortHandle, Flags, MessageSize, ResourceId)` → SSDT 0x011A
- [ ] `NtAlpcDeleteResourceReserve(PortHandle, Flags, ResourceId)` → SSDT 0x011B
- [ ] `NtAlpcQueryInformation(PortHandle, PortInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x011C
- [ ] `NtAlpcSetInformation(PortHandle, PortInformationClass, Buffer, Length)` → SSDT 0x011D
- [ ] `NtAlpcQueryInformationMessage(PortHandle, PortMessage, MessageInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x011E
- [ ] Commit: `"kernel: nt — ALPC and LPC port syscalls wired to SSDT"`

**Test checkpoint:** LPC `NtCreatePort` + `NtConnectPort` + `NtRequestWaitReplyPort` message round-trip. ALPC `NtAlpcCreatePort` + `NtAlpcConnectPort` + `NtAlpcSendWaitReceivePort` round-trip. Port visible in `\RPC Control\` namespace.

## 20. Exception and Debug Syscalls

> [!NOTE]
> Exception dispatch implemented in TODO-10-exception-dispatch-seh.md §4. Debug infrastructure in TODO-18-kernel-debugger-kd-protocol.md. This section reserves SSDT indices and defines the NT-compatible signatures.

- [ ] `NtRaiseException(ExceptionRecord, ContextRecord, FirstChance)` → SSDT 0x0130 (→ XREF TODO-10 §4):
  - Delivers exception to the structured exception handler chain
- [ ] `NtContinue(ContextRecord, RaiseAlert)` → SSDT 0x0131 (→ XREF TODO-10 §4):
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
- [ ] Commit: `"kernel: nt — exception and debug syscalls"`

**Test checkpoint:** `NtRaiseException` with `STATUS_BREAKPOINT` reaches SEH handler. `NtCreateDebugObject` + `NtDebugActiveProcess` on a child process captures breakpoint events via `NtWaitForDebugEvent`. `NtDebugContinue(DBG_CONTINUE)` resumes the debuggee.

## 21. Power and System Control

> [!NOTE]
> Power management implementation in TODO-15-power-management.md. This section provides the SSDT wiring for power and system control syscalls.

- [ ] `NtShutdownSystem(Action)` → SSDT 0x00D7:
  - `ShutdownNoReboot (0)`, `ShutdownReboot (1)`, `ShutdownPowerOff (2)`
  - Requires `SeShutdownPrivilege`
- [ ] `NtSetSystemPowerState(SystemAction, LightestSystemState, Flags)` → SSDT 0x0140:
  - `SystemAction`: `PowerActionSleep`, `PowerActionHibernate`, `PowerActionShutdown`
  - Routes through ACPI power management (→ XREF TODO-15)
- [ ] `NtInitiatePowerAction(SystemAction, LightestSystemState, Flags, Asynchronous)` → SSDT 0x0141
- [ ] `NtPowerInformation(InformationLevel, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength)` → SSDT 0x0142:
  - `SystemBatteryState (5)`: battery level, AC power
  - `ProcessorInformation (11)`: frequency, max frequency
  - `SystemPowerCapabilities (4)`: S-state support, lid switch
- [ ] `NtGetDevicePowerState(Device, State)` → SSDT 0x0143
- [ ] `NtSetThreadExecutionState(NewFlags, PreviousFlags)` → SSDT 0x0144:
  - `ES_SYSTEM_REQUIRED`, `ES_DISPLAY_REQUIRED`, `ES_CONTINUOUS`
  - Prevents sleep/screen-off while active
- [ ] Commit: `"kernel: nt — power and system control syscalls"`

**Test checkpoint:** `NtShutdownSystem(ShutdownReboot)` triggers ACPI reset. `NtPowerInformation(SystemPowerCapabilities)` returns valid S-state support mask. `NtSetThreadExecutionState` prevents idle sleep during long operation.

## 22. Atom, Locale, and Miscellaneous Syscalls
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
- [ ] `NtQuerySystemEnvironmentValue(VariableName, VariableValue, ValueLength, ReturnLength)` → SSDT 0x00D2: UEFI runtime variable access
- [ ] `NtSetSystemEnvironmentValue(VariableName, VariableValue)` → SSDT 0x00D3
- [ ] `NtDisplayString(String)` → SSDT 0x00D8: blue-screen-style text output during boot
- [ ] `NtRaiseHardError(ErrorStatus, NumberOfParameters, UnicodeStringParameterMask, Parameters, ValidResponseOptions, Response)` → SSDT 0x00D9: system-modal error dialog
- [ ] Commit: `"kernel: nt — atom table, locale, environment, misc syscalls"`

**Test checkpoint:** `NtAddAtom("TestAtom")` returns atom ID > 0. `NtFindAtom("TestAtom")` returns same ID. `NtDeleteAtom` removes it; subsequent `NtFindAtom` returns `STATUS_OBJECT_NAME_NOT_FOUND`. `NtQueryDefaultLocale` returns valid LCID.

## 23. Syscall Audit and Tracing Hook

> [!IMPORTANT]
> **Impossible OS exclusive feature.** Windows uses ETW (heavyweight, complex configuration). Linux uses seccomp-bpf (complex BPF programs) or strace (ptrace overhead). Impossible OS provides a first-class kernel API for syscall-level auditing with minimal overhead.

- [ ] `NtRegisterSyscallAuditHook(HookHandle, AuditRoutine, Context, Flags)` → SSDT 0x0150:
  - `Flags`: `AUDIT_PRE_CALL = 1` (before handler), `AUDIT_POST_CALL = 2` (after handler), `AUDIT_BOTH = 3`
  - `AuditRoutine(ServiceNumber, Args, Context, Phase)` — called in kernel context
  - Hook can inspect arguments, log, or deny (return `STATUS_ACCESS_DENIED` from pre-call to block)
- [ ] `NtUnregisterSyscallAuditHook(HookHandle)` → SSDT 0x0151
- [ ] `NtQuerySyscallAuditState(Buffer, Length, ReturnLength)` → SSDT 0x0152:
  - Returns list of active hooks, their coverage (pre/post/both), and overhead metrics
- [ ] SSDT dispatcher integration: before/after each `syscall_dispatch` call, check hook list and invoke if registered
  - Fast path: single atomic read of hook pointer; NULL = no hooks, no overhead
  - Hook list is RCU-protected for lock-free read in the hot path
- [ ] Requires `SeAuditPrivilege` to register hooks
- [ ] Commit: `"kernel: nt — syscall audit and tracing hook (SSDT pre/post)"`

**Test checkpoint:** Register pre-call audit hook; every syscall logs service number to ring buffer. Register post-call hook; verify NTSTATUS is captured. Pre-call hook returning `STATUS_ACCESS_DENIED` blocks the syscall. Unregister hook; verify zero overhead (no measurable latency increase).

---

## OS Comparison

| ⭐ | Feature                    | Win11                      | Linux                      | Impossible OS              |
|----|----------------------------|----------------------------|----------------------------|----------------------------|
| 💎 | SYSCALL/SYSRET fast path   | ✅ KiSystemCall64+LSTAR    | ✅ entry_SYSCALL_64        | ⬜ §2                      |
| 💎 | Typed failure return       | ✅ NTSTATUS on all NtXxx   | ✅ -ERRNO signed           | ⬜ §1                      |
| 💎 | Service descriptor table   | ✅ SSDT + shadow SSDT      | ✅ sys_call_table[]        | ⬜ §4 — 235 entries        |
| 💎 | SW-interrupt compat path   | ✅ INT 0x2E (legacy)       | ✅ INT 0x80 (32-bit)       | ⬜ §3                      |
| 💎 | IO_STATUS_BLOCK async I/O  | ✅ IOSB on all file Nt     | ⚠️ io_uring only           | ⬜ §11                     |
| 💎 | File metadata syscalls     | ✅ NtQuery/SetInfoFile     | ✅ stat/fstat/utimensat    | ⬜ §13                     |
| 💎 | Device I/O control         | ✅ NtDeviceIoControlFile   | ✅ ioctl()                 | ⬜ §13                     |
| 💎 | I/O completion ports       | ✅ NtCreateIoCompletion    | ✅ epoll/io_uring          | ⬜ §13                     |
| 💎 | Process/thread create API  | ✅ NtCreate{Process,Thread}| ✅ clone/execve            | ⬜ §7                      |
| 💎 | Thread context get/set     | ✅ NtGet/SetContextThread  | ✅ ptrace GETREGS          | ⬜ §7                      |
| 💎 | Named sync objects         | ✅ NtCreate{Event,Mutant}  | ✅ POSIX sem + futex       | ⬜ §8                      |
| 💎 | Multi-object wait          | ✅ NtWaitForMultipleObj    | ⚠️ No direct equivalent   | ⬜ §8                      |
| 💎 | Keyed events (futex)       | ✅ NtWaitForKeyedEvent     | ✅ futex()                 | ⬜ §8                      |
| 💎 | Virtual memory syscalls    | ✅ NtAllocate/Free/Protect | ✅ mmap/mprotect/munmap    | ⬜ §9                      |
| 💎 | Cross-process memory       | ✅ NtRead/WriteVirtualMem  | ✅ process_vm_readv        | ⬜ §9                      |
| 💎 | OS info query syscall      | ✅ NtQuerySystemInfo       | ✅ sysinfo + /proc         | ⬜ §10                     |
| 💎 | LastError per-thread       | ✅ TEB→LastErrorValue      | ✅ errno via TLS           | ⬜ §11 + TODO-04 §6       |
| 💎 | Registry syscalls          | ✅ NtCreate/Open/QueryKey  | ❌ No equivalent           | ⬜ §14 + TODO-13           |
| 💎 | Token/access control       | ✅ NtAccessCheck + tokens  | ✅ capabilities + DAC/MAC  | ⬜ §15 + TODO-11           |
| 💎 | Namespace dir/symlink      | ✅ NtCreateDirectoryObj    | ❌ No kernel namespace     | ⬜ §16                     |
| 💎 | Memory-mapped sections     | ✅ NtCreateSection/MapView | ✅ mmap with MAP_SHARED    | ⬜ §17 + TODO-03 §7       |
| 💎 | Timer objects              | ✅ NtSetTimer periodic     | ✅ timerfd_create          | ⬜ §18 + TODO-07 §7       |
| 💎 | ALPC message ports         | ✅ NtAlpcSendWaitReceive   | ❌ No equivalent           | ⬜ §19 + TODO-12           |
| 💎 | Debug API                  | ✅ NtDebugActiveProcess    | ✅ ptrace                  | ⬜ §20 + TODO-18           |
| 💎 | Power management           | ✅ NtSetSystemPowerState   | ✅ sys_reboot + ACPI       | ⬜ §21 + TODO-15           |
| 💎 | Atom table                 | ✅ NtAddAtom/FindAtom      | ❌ No equivalent           | ⬜ §22                     |
| ⭐ | ZwXxx CPL-gated aliases    | ✅ Internal, undocumented  | ❌ No equivalent           | ⬜ §12 — explicit, public  |
| ⭐ | Stable native API contract | ⚠️ Undocumented            | ❌ No stable native API    | ⬜ §4+§12 — numbered+public|
| ⭐ | Syscall audit hook         | ⚠️ ETW, heavyweight        | ⚠️ seccomp-bpf, complex   | ⬜ §23 — first-class API   |

> **After §1–§22:** Impossible OS has complete NT native API coverage — 235 syscall endpoints across file I/O, process/thread, memory, sync, registry, security, sections, timers, ALPC, debug, power, namespace, atoms, and system info. Real `ntdll.dll` stubs can call into the kernel.
> **§12** makes the `ZwXxx` layer an explicit, documented public contract — Windows keeps it internal/undocumented and Linux has no equivalent.
> **§23** provides first-class syscall auditing — no ETW complexity, no BPF programs, just a kernel callback with near-zero idle overhead.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nt_api()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_nt_api.c` with:
  - **NTSTATUS macros (§1):**
    - `NT_SUCCESS(STATUS_SUCCESS)` returns true; `NT_ERROR(STATUS_INVALID_HANDLE)` returns true
    - `NT_SUCCESS(STATUS_ACCESS_DENIED)` returns false; `NT_WARNING(STATUS_BUFFER_OVERFLOW)` returns true
    - `NT_INFORMATION(STATUS_ALERTED)` returns true; `NT_ERROR(STATUS_MUTANT_NOT_OWNED)` returns true
  - **SSDT dispatch (§4):**
    - Valid index calls handler; invalid index returns `STATUS_NOT_IMPLEMENTED`
    - Index beyond table size returns `STATUS_NOT_IMPLEMENTED`, not a crash
    - SSDT has ≥ 235 registered entries
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
    - `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` round-trip
    - `NtWaitForMultipleObjects(WaitAny)` returns correct index when one object signalled
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
  - **Token (§15):**
    - `NtOpenProcessToken` on current process returns valid handle
  - **Audit hook (§23):**
    - Register pre-call hook; verify it fires on `NtClose`
    - Unregister hook; verify no more callbacks
- [ ] Register in `test_runner_init()`: `test_register_nt_api()`
- [ ] Commit: `"test: add native API layer test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: Phase 1 shows `"syscall: fast path (SYSCALL/SYSRET) enabled"`
- [ ] Serial log: `"ssdt: registered 235 services"` (or more)
- [ ] `mov rax, 0x0000; syscall` from ring 3 reaches `NtClose` handler without a GPF
- [ ] INT 0x2E from ring 3 reaches the same `syscall_dispatch` with correct register mapping
- [ ] `NtCreateFile` on `C:\Impossible\System\Logs\kernel.log` returns `STATUS_SUCCESS` and a valid HANDLE
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
- [ ] All 4 platforms: QEMU WHPX, QEMU TCG, VirtualBox, bare metal
- [ ] Commit: `"kernel: nt — native API layer complete"`
