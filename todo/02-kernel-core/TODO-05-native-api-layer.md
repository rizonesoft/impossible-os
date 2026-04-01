# TODO-05 — Native API Layer (Nt/Zw)

> **Goal:** Replace the ad-hoc INT 0x80 / POSIX-numbered `SYS_*` dispatch table with a proper NT native API layer: `NTSTATUS` return values, `NtXxx`/`ZwXxx` naming, a `SYSCALL`/`SYSRET` fast path, a numbered System Service Descriptor Table (SSDT), and the `NtCurrentTeb()` and `NtCurrentPeb()` inline contract. This is the exact interface that `ntdll.dll`, CSRSS, Win32k, and every driver framework use to talk to the kernel. Without it, the Win32 subsystem layer cannot be built correctly.

> [!IMPORTANT]
> **Current state:** `syscall.c` dispatches via `INT 0x80` with Linux-style `SYS_WRITE=1`, `SYS_READ=2`, … `SYS_MUNMAP=38`. Return value is a plain `int64_t`. No `NTSTATUS`, no `NtXxx`/`ZwXxx` entry points, no `SYSCALL`/`SYSRET` MSR setup, no SSDT. The existing 22 syscalls are the migration starting point; none are deleted here.

## Inputs

- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — `task_exec`, ring-3 entry frame
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) — MSR_GS_BASE, per-CPU data
- → XREF: `TODO-03-object-manager.md` — `NtCreateFile`, `NtOpenFile`, `NtClose` are Ob-routed; SSDT entries 0x0025–0x002C depend on TODO-03
- → XREF: `TODO-04-peb-teb-user-abi.md` — `swapgs` in syscall entry/exit uses the TEB GS contract from TODO-04 §3–§4
- → XREF: `TODO-01-kernel-init-sequencing.md` — syscall fast path init belongs in Phase 1 (after GDT/IDT, before scheduler)
- → XREF: `TODO-07-time-filetime-management.md §7` — `NtQuerySystemTime`, `NtSetSystemTime`, `NtQueryPerformanceCounter`, `NtQueryTimerResolution` are registered in this SSDT; service numbers must be reserved before TODO-07 §7 is implemented
- → XREF: `TODO-10-exception-dispatch-seh.md §4` — `NtRaiseException` and `NtContinue` are registered in this SSDT; SSDT indices must be reserved before TODO-10 §4 is implemented
- → XREF: `TODO-11-security-reference-monitor.md` — `ZwXxx` kernel-mode calls bypass SRM access checks; must be documented

## Outcome

- All syscalls return `NTSTATUS`; `STATUS_SUCCESS = 0`, `STATUS_FAILURE` codes for errors.
- `NtXxx` entry points are the user-mode callable names; `ZwXxx` are the kernel-mode aliases.
- `SYSCALL`/`SYSRET` fast path replaces `INT 0x80`; `INT 0x2E` kept as compatibility fallback.
- A numbered SSDT table maps service indices to kernel functions; `ntdll` stubs call by index.
- The existing 22 `SYS_*` calls are migrated to `Nt`-named equivalents at stable indices.
- `NtCurrentTeb()` (`mov rax, gs:[0x30]`) and `NtCurrentPeb()` (`mov rax, gs:[0x60]`) return correct values per TODO-04.

## Implementation Order

| ⭐  | Order | Deliverable                                                | Depends On     | Status |
| --- | :---: | ---------------------------------------------------------- | -------------- | :----: |
| 💎  |   1   | NTSTATUS type and canonical status codes                   | —              |  [ ]   |
| 💎  |   2   | SYSCALL/SYSRET fast path (IA32_LSTAR)                      | TODO-04 §3–§4  |  [ ]   |
| 💎  |   3   | INT 0x2E compatibility path                                | 2              |  [ ]   |
| 💎  |   4   | System Service Descriptor Table (SSDT)                     | 1              |  [ ]   |
| 💎  |   5   | Nt/Zw naming and existing syscall migration                | 1, 4           |  [ ]   |
| 💎  |   6   | NtCreateFile / NtOpenFile / NtClose                        | 5, TODO-03 §3  |  [ ]   |
| 💎  |   7   | NtCreateProcess / NtCreateThread                           | 5, TODO-03 §5  |  [ ]   |
| 💎  |   8   | NtCreateEvent / NtCreateMutex / NtCreateSemaphore          | 5, TODO-03 §6  |  [ ]   |
| 💎  |   9   | NtAllocateVirtualMemory / NtFreeVirtualMemory              | 5              |  [ ]   |
| 💎  |  10   | NtQuerySystemInformation (basic classes)                   | 5              |  [ ]   |
| ⭐  |  11   | Extended error information (NtCurrentTeb LastError + IOSB) | 5, TODO-04 §6  |  [ ]   |
| ⭐  |  12   | ZwXxx kernel-mode alias layer with privilege assertion     | 4, 5           |  [ ]   |

> 💎 = parity — Windows NT and Linux (syscall fast path, typed return, SSDT equivalent) both have these.
> ⭐ = exclusive — the `ZwXxx` privilege assertion check and the IOSB/LastError unified path are more explicit than anything in the Linux syscall model, exposing correctness invariants at the API boundary.

---

## 1. NTSTATUS Type and Canonical Status Codes `[Sonnet]`

Define the NT status type and the minimum set of codes needed by syscall implementations.

- [ ] Create `include/kernel/nt/ntstatus.h`:
  - `typedef uint32_t NTSTATUS`
  - Severity bits: `NT_SUCCESS(s)` = `((s) >> 30) == 0`, `NT_INFORMATION(s)` = `((s) >> 30) == 1`, `NT_WARNING(s)` = `((s) >> 30) == 2`, `NT_ERROR(s)` = `((s) >> 30) == 3`
  - `STATUS_SUCCESS                    0x00000000`
  - `STATUS_PENDING                    0x00000103`
  - `STATUS_BUFFER_OVERFLOW            0x80000005`
  - `STATUS_NO_MORE_FILES              0x80000006`
  - `STATUS_UNSUCCESSFUL               0xC0000001`
  - `STATUS_NOT_IMPLEMENTED            0xC0000002`
  - `STATUS_INVALID_HANDLE             0xC0000008`
  - `STATUS_INVALID_PARAMETER          0xC000000D`
  - `STATUS_NO_MEMORY                  0xC0000017`
  - `STATUS_ACCESS_DENIED              0xC0000022`
  - `STATUS_OBJECT_NAME_NOT_FOUND      0xC0000034`
  - `STATUS_OBJECT_PATH_NOT_FOUND      0xC000003A`
  - `STATUS_BUFFER_TOO_SMALL           0xC0000023`
  - `STATUS_END_OF_FILE                0xC0000011`
  - `STATUS_OBJECT_TYPE_MISMATCH       0xC0000024`
  - `STATUS_PRIVILEGE_NOT_HELD         0xC0000061`
  - `STATUS_PROCESS_IS_TERMINATING     0xC000010A`
  - `STATUS_NOT_SUPPORTED              0xC00000BB`
- [ ] Create `include/kernel/nt/nt_types.h` with foundational NT types:
  - `HANDLE` = `void *` (opaque; index into handle table)
  - `IO_STATUS_BLOCK` — `NTSTATUS Status`, `uint64_t Information`
  - `UNICODE_STRING` — already in TODO-04; include or re-export here
  - `OBJECT_ATTRIBUTES` — `uint64_t Length`, `HANDLE RootDirectory`, `UNICODE_STRING *ObjectName`, `uint32_t Attributes` (`OBJ_CASE_INSENSITIVE = 0x40`, `OBJ_KERNEL_HANDLE = 0x200`)
  - `LARGE_INTEGER` = `int64_t`
  - `ACCESS_MASK` = `uint32_t`; `GENERIC_READ = 0x80000000`, `GENERIC_WRITE = 0x40000000`, `GENERIC_ALL = 0x10000000`
- [ ] Annotate each `NTSTATUS` code with the condition that triggers it — serves as inline documentation
- [ ] Commit: `"kernel: nt — NTSTATUS type and canonical status codes"`

## 2. SYSCALL/SYSRET Fast Path `[Opus]`

Replace `INT 0x80` with the x86-64 `SYSCALL`/`SYSRET` instruction pair. On `SYSCALL`: CPU saves RIP→RCX, RFLAGS→R11; jumps to `IA32_LSTAR`. On `SYSRET`: restores RIP from RCX, RFLAGS from R11; returns to ring 3.

- [ ] Define MSR constants in `include/kernel/smp/msr.h` (or alongside `MSR_GS_BASE` in `smp.c`):
  - `MSR_STAR    0xC0000081` — CS/SS selectors for SYSCALL/SYSRET
  - `MSR_LSTAR   0xC0000082` — 64-bit SYSCALL entry point address
  - `MSR_FMASK   0xC0000084` — RFLAGS mask to clear on SYSCALL entry
- [ ] Write `syscall_entry` in `src/kernel/sched/syscall_entry.asm`:
  - `swapgs` (exchange user GS/TEB → kernel GS/per-CPU) — requires TODO-04 §3 to be done first
  - Save user RSP; load kernel RSP from `IA32_KERNEL_GS_BASE` or per-CPU TSS RSP0
  - Push minimal callee-save frame (do not push RCX/R11 — they are the saved user RIP/RFLAGS)
  - `call syscall_dispatch` — C handler taking `(syscall_number, arg1..arg5)`
  - Restore frame; `swapgs`; `sysretq`
- [ ] In Phase 1 init (after GDT/IDT, before scheduler): call `syscall_init_fast()`:
  - `wrmsr(MSR_STAR,  (KERNEL_CS << 32) | (USER_CS32 << 48))` — ring-0 CS on SYSCALL, ring-3 CS on SYSRET
  - `wrmsr(MSR_LSTAR, (uint64_t)syscall_entry)` — entry point
  - `wrmsr(MSR_FMASK, 0x200)` — clear IF (interrupts off on entry)
  - Set `SCE` bit in `IA32_EFER` MSR (`0xC0000080`) to enable `SYSCALL`/`SYSRET`
- [ ] `syscall_dispatch(uint64_t number, uint64_t a1..a5)` — calls into SSDT (§4)
- [ ] Keep `INT 0x80` registered in IDT for the transition period; remove after §3 INT 0x2E is up
- [ ] Commit: `"kernel: nt — SYSCALL/SYSRET fast path init"`

## 3. INT 0x2E Compatibility Path `[Sonnet]`

Windows NT's original software-interrupt syscall vector. Required for early ntdll and any code that does not use `SYSCALL`.

- [ ] Register a new IDT handler for vector `0x2E` in `idt.c`:
  - Entry stub saves full register frame (same layout as INT 0x80 stub)
  - `swapgs` if coming from ring 3 (check CS & 3)
  - Calls `syscall_dispatch(rax, rdi, rsi, rdx, r10, r8, r9)` (NT calling convention for syscall args)
  - `swapgs` on return; `iretq`
- [ ] The INT 0x2E handler shares `syscall_dispatch`; no separate dispatch logic
- [ ] After INT 0x2E is verified working, keep INT 0x80 as a second alias until all existing user-mode test binaries are updated to use the new calling convention
- [ ] Commit: `"kernel: nt — INT 0x2E syscall compatibility path"`

## 4. System Service Descriptor Table (SSDT) `[Opus]`

The SSDT is a flat array of function pointers indexed by the 12-bit service number in RAX. `ntdll` stubs `mov rax, <service_number>` then `syscall`.

- [ ] Define `SSDT_ENTRY` and `SSDT_TABLE` in `include/kernel/nt/ssdt.h`:
  - `typedef NTSTATUS (*SSDT_HANDLER)(uint64_t a1, a2, a3, a4, a5, a6)`
  - `SSDT_TABLE` — array of `SSDT_HANDLER` + count + table name string
- [ ] Allocate the Win32k shadow SSDT stub (table 1) as an empty placeholder — filled by Win32k layer later
- [ ] Define the first-pass service index assignments in `include/kernel/nt/service_numbers.h`:
  - Keep numbers stable — ntdll stubs are compiled against these; changing them is an ABI break
  - Start from Windows NT 6.x service numbers for the core set (file, process, memory, sync) so real ntdll binaries work without patching
  - Initial assignments (illustrative, align with actual NT numbers):
    - `0x0012` `NtWriteFile`, `0x0010` `NtReadFile`, `0x0015` `NtClose`
    - `0x0029` `NtCreateProcess`, `0x004B` `NtCreateThread`
    - `0x0055` `NtCreateEvent`, `0x0090` `NtCreateMutant`, `0x00C0` `NtCreateSemaphore`
    - `0x0018` `NtCreateFile`, `0x0030` `NtOpenFile`
    - `0x0040` `NtDuplicateObject`
    - `0x0004` `NtAllocateVirtualMemory`, `0x001B` `NtFreeVirtualMemory`
    - `0x0036` `NtQuerySystemInformation`
- [ ] Implement `syscall_dispatch`: index RAX into the active SSDT; call handler; return `NTSTATUS` in RAX
- [ ] Unimplemented slots return `STATUS_NOT_IMPLEMENTED` rather than crashing
- [ ] Commit: `"kernel: nt — SSDT and service number table"`

## 5. Nt/Zw Naming and Existing Syscall Migration `[Sonnet]`

Rename / wrap the existing 22 `SYS_*` implementations to their `NtXxx` equivalents, change return types to `NTSTATUS`, and register them in the SSDT.

- [ ] Add `NtWriteFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_WRITE` logic
- [ ] Add `NtReadFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_READ` logic
- [ ] Add `NtTerminateProcess(HANDLE, NTSTATUS)` replacing `SYS_EXIT`
- [ ] Add `NtYieldExecution()` replacing `SYS_YIELD`; returns `STATUS_SUCCESS`
- [ ] Add `NtCreateProcess`/`NtCreateThread` wrapping fork/exec paths (§7)
- [ ] Add `NtWaitForSingleObject(HANDLE, timeout)` replacing `SYS_WAITPID`
- [ ] Add `NtQueryDirectoryFile` wrapping `SYS_READDIR`
- [ ] Add `NtQuerySystemInformation(SystemProcessInformation)` wrapping `SYS_GETPROCS`
- [ ] Add `NtTerminateProcess` for kill
- [ ] Add `NtQuerySystemInformation(SystemTimeOfDayInformation)` for uptime
- [ ] Add `NtShutdownSystem(ShutdownReboot / ShutdownPowerOff)` for reboot/shutdown
- [ ] Add `NtCreateNamedPipeFile` / `NtReadFile` / `NtWriteFile` wrapping pipe
- [ ] Add `NtCreateSection` / `NtMapViewOfSection` wrapping shmem (→ XREF TODO-03 §7)
- [ ] Keep `SYS_*` macros as compile-time aliases pointing to the same SSDT indices for the transition period — remove after all user-mode test binaries are ported
- [ ] Change all `sys_*` implementations to return `NTSTATUS`; convert error paths to `STATUS_*` codes rather than `-1`
- [ ] Commit: `"kernel: nt — migrate existing syscalls to NtXxx naming and NTSTATUS"`

## 6. NtCreateFile / NtOpenFile / NtClose `[Sonnet]`

Core file I/O entry points routed through the Object Manager (→ XREF TODO-03).

- [ ] `NtCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength)`:
  - Parse `ObjectAttributes->ObjectName` via Ob namespace (TODO-03 §4)
  - Map `CreateDisposition` (`FILE_OPEN`, `FILE_CREATE`, `FILE_SUPERSEDE`, etc.) to VFS flags
  - Call `vfs_open()` → wrap result as an Ob File object → allocate handle via `ObpAllocateHandle`
  - Populate `IoStatusBlock->Status` and `IoStatusBlock->Information` (FILE_CREATED / FILE_OPENED)
- [ ] `NtOpenFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, ShareAccess, OpenOptions)`:
  - Subset of `NtCreateFile` with `CreateDisposition = FILE_OPEN`
- [ ] `NtClose(Handle)`: call `ObpFreeHandle` (TODO-03 §9); return `STATUS_SUCCESS` or `STATUS_INVALID_HANDLE`
- [ ] `NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key)`: look up File object via handle table; call `vfs_read`; fill IOSB
- [ ] `NtWriteFile(...)`: symmetric with NtReadFile
- [ ] Commit: `"kernel: nt — NtCreateFile, NtOpenFile, NtClose, NtReadFile, NtWriteFile"`

## 7. NtCreateProcess / NtCreateThread `[Sonnet]`

Process and thread creation through the Ob-managed process model (→ XREF TODO-03 §5).

- [ ] `NtCreateProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ParentProcess, InheritObjectTable, SectionHandle, DebugPort, ExceptionPort)`:
  - Allocate new `task_t` via `task_create` equivalent; register as `ObpProcessType` object
  - If `InheritObjectTable` = TRUE, copy inheritable handles (TODO-03 §10)
  - Return process HANDLE via `ObpAllocateHandle`
- [ ] `NtCreateThread(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, ClientId, ThreadContext, InitialTeb, CreateSuspended)`:
  - Allocate thread struct; populate TEB (TODO-04 §6); register as `ObpThreadType` object
  - If `CreateSuspended`, start thread in suspended state (THREAD_SUSPEND_COUNT = 1)
  - Return thread HANDLE
- [ ] `NtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId)`:
  - Look up by PID in the Ob namespace `\KernelObjects\Process<PID>`
- [ ] `NtOpenThread(ThreadHandle, DesiredAccess, ObjectAttributes, ClientId)`: symmetric
- [ ] `NtResumeThread(ThreadHandle, PreviousSuspendCount)`: decrement suspend count; schedule if 0
- [ ] `NtSuspendThread(ThreadHandle, PreviousSuspendCount)`: increment suspend count; deschedule
- [ ] Commit: `"kernel: nt — NtCreateProcess, NtCreateThread, NtOpenProcess, NtOpenThread"`

## 8. NtCreateEvent / NtCreateMutex / NtCreateSemaphore `[Sonnet]`

Synchronisation objects through Ob-managed named types (→ XREF TODO-03 §6).

- [ ] `NtCreateEvent(EventHandle, DesiredAccess, ObjectAttributes, EventType, InitialState)`:
  - `EventType`: `NotificationEvent (0)` = manual-reset, `SynchronizationEvent (1)` = auto-reset
  - Route through `ObpEventType`; name in `\BaseNamedObjects\` if `ObjectAttributes->ObjectName` set
- [ ] `NtOpenEvent(EventHandle, DesiredAccess, ObjectAttributes)`: open by name
- [ ] `NtSetEvent(EventHandle, PreviousState)`: signal the event; wake waiting threads
- [ ] `NtResetEvent(EventHandle, PreviousState)`: clear the event
- [ ] `NtWaitForSingleObject(Handle, Alertable, Timeout)`: block until object is signalled or timeout
- [ ] `NtCreateMutant(MutantHandle, DesiredAccess, ObjectAttributes, InitialOwner)`: Ob-wrapped mutex
- [ ] `NtReleaseMutant(MutantHandle, PreviousCount)`: release mutex; return `STATUS_MUTANT_NOT_OWNED` if caller does not own it
- [ ] `NtCreateSemaphore(SemaphoreHandle, DesiredAccess, ObjectAttributes, InitialCount, MaximumCount)`
- [ ] `NtReleaseSemaphore(SemaphoreHandle, ReleaseCount, PreviousCount)`
- [ ] Commit: `"kernel: nt — NtCreateEvent, NtCreateMutant, NtCreateSemaphore and wait"`

## 9. NtAllocateVirtualMemory / NtFreeVirtualMemory `[Sonnet]`

Virtual memory management entry points.

- [ ] `NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect)`:
  - `AllocationType`: `MEM_COMMIT = 0x1000`, `MEM_RESERVE = 0x2000`
  - `Protect`: `PAGE_READWRITE = 0x04`, `PAGE_EXECUTE_READ = 0x20`, etc.
  - Route to VMM page allocator; return allocated address in `*BaseAddress`
- [ ] `NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType)`:
  - `FreeType`: `MEM_RELEASE = 0x8000`, `MEM_DECOMMIT = 0x4000`
- [ ] `NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass, Buffer, Length)`:
  - `MemoryBasicInformation`: base, allocation base, protect flags, state, type
- [ ] Commit: `"kernel: nt — NtAllocateVirtualMemory, NtFreeVirtualMemory, NtQueryVirtualMemory"`

## 10. NtQuerySystemInformation (Basic Classes) `[Sonnet]`

Provides OS version, process list, and hardware info to ntdll and user-mode tools.

- [ ] `NtQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ReturnLength)`:
  - `SystemBasicInformation (0)`: number of processors, page size, min/max user address, allocation granularity
  - `SystemProcessInformation (5)`: linked list of `SYSTEM_PROCESS_INFORMATION` — PID, name, thread count, handle count (wraps `SYS_GETPROCS` data)
  - `SystemTimeOfDayInformation (3)`: boot time, current time, time zone bias (wraps uptime)
  - `SystemPerformanceInformation (2)`: available pages, commit total, commit limit (from PMM stats)
  - Unimplemented classes return `STATUS_NOT_IMPLEMENTED`
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, Buffer, Length)`:
  - `ProcessBasicInformation (0)`: PEB address, PID, parent PID, exit status
  - `ProcessImageFileName (27)`: full path of the executable
- [ ] Commit: `"kernel: nt — NtQuerySystemInformation and NtQueryInformationProcess"`

## 11. Extended Error Information (IOSB + LastError) `[Sonnet]`

NT propagates detailed error info through two channels: `IO_STATUS_BLOCK` (async I/O) and `TEB->LastErrorValue` (Win32 `GetLastError`). Both must be populated correctly.

- [ ] All file I/O `NtXxx` functions write final `NTSTATUS` into `IoStatusBlock->Status` and byte count / disposition into `IoStatusBlock->Information`
- [ ] On every `NTSTATUS` return from a syscall: if `NT_ERROR(status)`, also write the Win32 error translation into `TEB->LastErrorValue` (at `gs:[0x68]`) using a compact `RtlNtStatusToDosError` table for common codes
- [ ] `RtlNtStatusToDosError` minimal table: `STATUS_ACCESS_DENIED→5`, `STATUS_NO_MEMORY→8`, `STATUS_INVALID_HANDLE→6`, `STATUS_OBJECT_NAME_NOT_FOUND→2`, `STATUS_NOT_IMPLEMENTED→50`
- [ ] This is the only place `TEB->LastErrorValue` is written by kernel code — usermode `SetLastError` writes it directly via GS offset without a syscall
- [ ] Commit: `"kernel: nt — IOSB and TEB LastErrorValue propagation"`

## 12. ZwXxx Kernel-Mode Alias Layer `[Opus]`

`ZwXxx` names are identical to `NtXxx` in user mode. In kernel mode (`CPL=0`), `ZwXxx` calls bypass the user-mode probe and use kernel-mode access rights directly. This is the convention all of Windows' own drivers and executive components use.

- [ ] Add a `ZwXxx` header `include/kernel/nt/zw.h` that declares each `ZwXxx` as an alias for the same SSDT entry point
- [ ] In the SSDT dispatcher: if caller is CPL=0, skip user-buffer probe and pointer validation; if CPL=3, validate all user-space buffer pointers before use (probe for read/write)
- [ ] Add a `ASSERT_KERNEL_CALLER()` macro that fires `STATUS_PRIVILEGE_NOT_HELD` if a kernel-only API is called from user mode
- [ ] Document the convention: kernel components call `Zw` variants; user-mode calls `Nt` variants; both resolve to the same implementation, differentiated only by CPL check
- [ ] Commit: `"kernel: nt — ZwXxx kernel-mode alias layer with CPL probe bypass"`

---

## OS Comparison


| ⭐ | Feature                               | Win11                                                 | Linux                                          | Impossible OS                           |
|----|---------------------------------------|-------------------------------------------------------|------------------------------------------------|-----------------------------------------|
| 💎 | SYSCALL/SYSRET fast path              | ✅ KiSystemCall64 via IA32_LSTAR                      | ✅ `syscall` entry via `entry_SYSCALL_64`      | ⬜ §2                                   |
| 💎 | Typed failure return                  | ✅ Every NtXxx returns NTSTATUS                       | ✅ `-ERRNO` signed return                      | ⬜ §1                                   |
| 💎 | Numbered service descriptor table     | ✅ SSDT (ntoskrnl) + shadow SSDT                      | ✅ syscall table `sys_call_table[]`            | ⬜ §4                                   |
| 💎 | Software-interrupt compatibility path | ✅ INT 0x2E (legacy NT)                               | ✅ INT 0x80 (32-bit compat)                    | ⬜ §3                                   |
| 💎 | IO_STATUS_BLOCK for async I/O         | ✅ Every file NtXxx populates IOSB                    | ⚠️ `io_uring` result ring; no IOSB             | ⬜ §11                                  |
| 💎 | Process/thread creation via typed API | ✅ NtCreateProcess / NtCreateThread                   | ✅ `clone()` / `execve()`                      | ⬜ §7                                   |
| 💎 | Named sync objects via syscall        | ✅ NtCreateEvent / NtCreateMutant / NtCreateSemaphore | ✅ POSIX named semaphores + futex              | ⬜ §8                                   |
| 💎 | Virtual memory management syscalls    | ✅ NtAllocateVirtualMemory / NtFreeVirtualMemory      | ✅ `mmap()` / `munmap()`                       | ⬜ §9                                   |
| 💎 | OS info query syscall                 | ✅ NtQuerySystemInformation                           | ✅ `sysinfo()` / `/proc/`                      | ⬜ §10                                  |
| 💎 | LastError per-thread slot             | ✅ TEB->LastErrorValue set by kernel on               | ✅ `errno` via thread-local `__errno_location` | ⬜ §11 — (+ TODO-04 §6)                 |
| ⭐ | ZwXxx CPL-gated kernel alias layer    | ✅ Internal convention; not documented/public         | ❌ No equivalent; drivers use same             | ⬜ §12 — explicit, documented           |
| ⭐ | Unified Nt/Zw contract as public API  | ⚠️ NT native API is undocumented                      | ❌ No stable native API; syscall               | ⬜ §4 — /§12 — stable, numbered, public |

> **After §1–§11:** Impossible OS matches Windows NT exactly on the native API calling convention, service numbers, IOSB semantics, NTSTATUS codes, and LastError propagation. Real `ntdll.dll` stubs can call into the kernel without patching.
> **§12** makes the `ZwXxx` layer an explicit, documented public contract — Windows keeps it internal/undocumented and Linux has no equivalent at all.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_nt_api()` (XREF: `00-infrastructure/TODO-03 §1`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_nt_api.c` with:
  - `NT_SUCCESS(STATUS_SUCCESS)` returns true; `NT_ERROR(STATUS_INVALID_HANDLE)` returns true
  - `NT_SUCCESS(STATUS_ACCESS_DENIED)` returns false; `NT_WARNING(STATUS_BUFFER_OVERFLOW)` returns true
  - SSDT dispatch: valid index calls handler; invalid index returns `STATUS_NOT_IMPLEMENTED`
  - SSDT dispatch: index beyond table size returns `STATUS_NOT_IMPLEMENTED`, not a crash
  - `NtClose(INVALID_HANDLE_VALUE)` returns `STATUS_INVALID_HANDLE`
  - `NtCreateFile` on existing file returns `STATUS_SUCCESS` and a non-NULL HANDLE
  - `NtClose` on a valid HANDLE returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`
  - `NtReadFile` populates `IO_STATUS_BLOCK.Status = STATUS_SUCCESS` and `Information = bytes_read`
  - `NtWriteFile` populates `IO_STATUS_BLOCK` correctly after write
  - `NtDuplicateObject` from kernel-mode produces valid second handle to same object
  - `NtQuerySystemInformation(SystemBasicInformation)` returns correct page size and processor count
  - `NtAllocateVirtualMemory` with `MEM_COMMIT` returns a usable address; write/read round-trip succeeds
  - `NtFreeVirtualMemory` on allocated region returns `STATUS_SUCCESS`
  - `RtlNtStatusToDosError(STATUS_ACCESS_DENIED)` returns `5`; `STATUS_NO_MEMORY` returns `8`
  - ZwXxx kernel-mode call: `ZwClose` from CPL=0 skips user-buffer probe (no fault)
- [ ] Register in `test_runner_init()`: `test_register_nt_api()`
- [ ] Commit: `"test: add native API layer test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: Phase 1 shows `"syscall: fast path (SYSCALL/SYSRET) enabled"`
- [ ] `mov rax, 0x0015; syscall` from ring 3 reaches `NtClose` handler without a GPF
- [ ] INT 0x2E from ring 3 reaches the same `syscall_dispatch` with correct register mapping
- [ ] `NtCreateFile` on `C:\Impossible\System\Logs\kernel.log` returns `STATUS_SUCCESS` and a valid HANDLE
- [ ] `NtClose(handle)` returns `STATUS_SUCCESS`; calling it again returns `STATUS_INVALID_HANDLE`
- [ ] SSDT slot for unimplemented index returns `STATUS_NOT_IMPLEMENTED`, not a crash
- [ ] `TEB->LastErrorValue` at `gs:[0x68]` is updated after a failing `NtOpenFile`
- [ ] IOSB populated correctly: `Status = STATUS_SUCCESS`, `Information = bytes_read` after `NtReadFile`
- [ ] CPL=0 `ZwXxx` call skips user-buffer probe; CPL=3 call through same index validates pointer
- [ ] Commit: `"kernel: nt — native API layer complete"`
