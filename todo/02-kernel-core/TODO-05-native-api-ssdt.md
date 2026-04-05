# TODO-05 -- Native API Layer (Nt/Zw)

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
- → XREF: `TODO-03-object-manager.md` -- Ob-routed NtXxx functions (NtClose, NtDuplicateObject, NtQueryObject, NtOpenDirectoryObject, NtQueryDirectoryObject already implemented); SSDT entries for file/process/sync depend on TODO-03 §3–§10
- → XREF: `TODO-04-peb-teb-user-abi.md §3–§4` -- `swapgs` in syscall entry/exit uses the TEB GS contract; KERNEL_GS_BASE per-task
- → XREF: `TODO-01-kernel-init-sequencing.md §3` -- syscall fast path init belongs in Phase 1 (after GDT/IDT, before scheduler)
- → XREF: `TODO-07-time-filetime-management.md §8–§9` -- NtSetTimerResolution/NtQueryTimerResolution (§8), NtQuerySystemTime/NtSetSystemTime/NtQueryPerformanceCounter (§9); service numbers reserved in §4
- → XREF: `TODO-10-exception-dispatch-seh.md §5` -- NtRaiseException and NtContinue; SSDT indices reserved in §4
- → XREF: `TODO-11-security-reference-monitor.md §5,§12` -- §5 SeAccessCheck bypasses for kernel-mode (ZwXxx) callers; §12 wires NtAccessCheck, NtOpenProcessToken, etc. to SSDT
- → XREF: `TODO-12-alpc-message-ports.md §8` -- ALPC port syscalls (NtCreatePort, NtAlpcSendWaitReceivePort, etc.); SSDT indices reserved in §4
- → XREF: `TODO-13-registry-completion.md §4` -- Registry syscalls (NtCreateKey, NtOpenKey, NtSetValueKey, etc.); SSDT indices reserved in §4
- → XREF: `TODO-15-power-management.md §12` -- NtSetSystemPowerState, NtInitiatePowerAction; SSDT indices reserved in §4
- → XREF: `TODO-18-kernel-debugger-kd-protocol.md §13` -- NtDebugActiveProcess, NtWaitForDebugEvent; SSDT indices reserved in §4
- → XREF: `TODO-10-exception-dispatch-seh.md §13` -- ProbeForRead/ProbeForWrite safe probing used by §12 and all NtXxx handlers
- → XREF: `TODO-17-kernel-security-hardening.md` -- SSDT integrity protection (§26) complements KASLR and SMEP/SMAP
- → XREF: `08-graphics-ui/TODO-12-win32k-shadow-ssdt.md §8` -- Win32k user-mode callback dispatch via §25 KeUserModeCallback; shadow SSDT stub registered in §4
- → XREF: `TODO-06-irql-model-dpcs.md §11,§12` -- KAPC object type and APC delivery mechanism; NtQueueApcThread (SSDT 0x0043) and NtQueueApcThreadEx (SSDT 0x0380) consume KeInitializeApc/KeInsertQueueApc
- → XREF: `TODO-09-process-model-extensions.md §4,§5,§8,§10,§11,§13` -- §4 priority class, §5 scheduling policy, §8 accounting fields, §10 CPU affinity, §11 mitigation policy all flow through `NtSetInformationProcess`/`NtQueryInformationProcess` (§10 of this TODO); §13 wires Job Object SSDT entries 0x0160–0x0167; §12 pledge check integrates into the SSDT dispatcher alongside §24 syscall filter

## Outcome

- All syscalls return `NTSTATUS`; `STATUS_SUCCESS = 0`, `STATUS_FAILURE` codes for errors.
- `NtXxx` entry points are the user-mode callable names; `ZwXxx` are the kernel-mode aliases.
- `SYSCALL`/`SYSRET` fast path replaces `INT 0x80`; `INT 0x2E` kept as compatibility fallback.
- A numbered SSDT table with 470 entries maps service indices to kernel functions; `ntdll` stubs call by index.
- The existing 22 `SYS_*` calls are migrated to `Nt`-named equivalents at stable indices.
- `NtCurrentTeb()` (`mov rax, gs:[0x30]`) and `NtCurrentPeb()` (`mov rax, gs:[0x60]`) return correct values per TODO-04.
- All endpoint categories covered: file I/O, process/thread, memory, sync, registry, security/token, sections, timers, ALPC ports, debug, power, namespace, system info, atoms.
- Per-process syscall filtering allows processes to restrict which syscalls their children can invoke -- parity with Win11 SystemCallDisablePolicy and Linux seccomp-bpf.
- `KeUserModeCallback()` enables the kernel to call user-mode functions (window procedures, hooks) and await their return via `NtCallbackReturn`.
- SSDT pages are hardware write-protected after init -- simpler and more secure than PatchGuard periodic checksums.

## Implementation Order

| ⭐  | Order | Deliverable                                                    | Depends On        | Status |
| --- | :---: | -------------------------------------------------------------- | ----------------- | :----: |
| 💎  |   1   | NTSTATUS type and canonical status codes                       | --                |  [x]   |
| 💎  |   2   | SYSCALL/SYSRET fast path (IA32_LSTAR)                          | TODO-04 §3–§4     |  [x]   |
| 💎  |   3   | INT 0x2E compatibility path                                    | §2                |  [x]   |
| 💎  |   4   | System Service Descriptor Table (SSDT) -- 470 entries          | §1                |  [x]   |
| 💎  |   5   | Nt/Zw naming and existing syscall migration                    | §1, §4            |  [x]   |
| 💎  |   6   | NtCreateFile / NtOpenFile / NtClose / NtReadFile / NtWriteFile | §5, TODO-03 §3    |  [ ]   |
| 💎  |   7   | NtCreateProcess / NtCreateThread / process-thread lifecycle    | §5, TODO-03 §5    |  [ ]   |
| 💎  |   8   | Sync objects + NtWaitForMultipleObjects                        | §5, TODO-03 §6    |  [ ]   |
| 💎  |   9   | Virtual memory (alloc, free, protect, lock)                    | §5                |  [ ]   |
| 💎  |  10   | NtQuerySystemInformation / NtQueryInformationProcess           | §5                |  [ ]   |
| ⭐  |  11   | Extended error information (IOSB + TEB LastError)              | §5, TODO-04 §6    |  [ ]   |
| ⭐  |  12   | ZwXxx kernel-mode alias layer with privilege assertion         | §4, §5            |  [ ]   |
| 💎  |  13   | File metadata and device control                               | §6                |  [ ]   |
| 💎  |  14   | Registry syscalls                                              | §5, TODO-13 §4    |  [ ]   |
| 💎  |  15   | Token and access control syscalls                              | §5, TODO-11 §4    |  [ ]   |
| 💎  |  16   | Directory and symbolic link object syscalls                    | §5, TODO-03 §4    |  [ ]   |
| 💎  |  17   | Section and memory-mapped file syscalls                        | §5, TODO-03 §7    |  [ ]   |
| 💎  |  18   | Timer control syscalls                                         | §5, TODO-07 §8,§9 |  [ ]   |
| 💎  |  19   | ALPC / LPC port syscalls                                       | §5, TODO-12 §8    |  [ ]   |
| 💎  |  20   | Exception and debug syscalls                                   | §5, TODO-10 §5    |  [ ]   |
| 💎  |  21   | Power and system control                                       | §5, TODO-15 §12   |  [ ]   |
| 💎  |  22   | Atom, locale, and miscellaneous                                | §5                |  [ ]   |
| ⭐  |  23   | Syscall audit and tracing hook                                 | §4                |  [ ]   |
| 💎  |  24   | Per-process syscall filtering (seccomp / SystemCallDisable)    | §4, §7            |  [ ]   |
| 💎  |  25   | Kernel-to-user mode callback dispatch (KeUserModeCallback)     | §2, TODO-04 §5    |  [ ]   |
| ⭐  |  26   | SSDT integrity protection (hardware write-protect)             | §4                |  [ ]   |

> 💎 = parity -- Windows NT and Linux both have equivalents for these categories.
> ⭐ = exclusive -- the ZwXxx privilege layer, the audit hook, SSDT integrity protection, and the IOSB/LastError unified path go beyond what Linux offers.

> [!IMPORTANT]
> **Self-contained execution model:** §1–§5 (NTSTATUS, SYSCALL/SYSRET, INT 0x2E, SSDT, migration) are fully self-contained -- no external blockers. §9–§12, §22–§26 are also unblocked. Sections §6–§8, §13–§21 wire domain-specific syscalls through the SSDT and depend on their respective domain TODOs (Object Manager, Registry, SRM, ALPC, etc.). This is by design -- this TODO is the **master registry** for all NT syscall endpoints. Domain TODOs implement the logic; this TODO provides the SSDT wiring. The unblocked core (§1–§5 + §9–§12 + §22–§26) delivers a fully functional SYSCALL/SYSRET fast path with 470 SSDT slots, NTSTATUS return values, and the audit/filter/integrity infrastructure. Domain-specific NtXxx wrappers activate as their domain TODOs complete.

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
  - `UNICODE_STRING` -- re-export from `include/kernel/ob/peb.h` (already defined per TODO-04)
  - `OBJECT_ATTRIBUTES` -- `uint64_t Length`, `HANDLE RootDirectory`, `UNICODE_STRING *ObjectName`, `uint32_t Attributes` (`OBJ_CASE_INSENSITIVE = 0x40`, `OBJ_KERNEL_HANDLE = 0x200`, `OBJ_INHERIT = 0x02`, `OBJ_OPENIF = 0x80`)
  - `LARGE_INTEGER` -- re-export from `include/kernel/ob/peb.h` (already defined per TODO-04)
  - `ACCESS_MASK` = `uint32_t`; `GENERIC_READ = 0x80000000`, `GENERIC_WRITE = 0x40000000`, `GENERIC_EXECUTE = 0x20000000`, `GENERIC_ALL = 0x10000000`
  - `CLIENT_ID` -- `uint64_t UniqueProcess`, `uint64_t UniqueThread`
  - `CONTEXT` -- forward-declare; full definition in TODO-10
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

**Test checkpoint:** Serial log: `"syscall: fast path (SYSCALL/SYSRET) enabled"` during Phase 1. `POST16(0xDA00)` entry, `POST16(0xDA01)` exit. Ring-3 `syscall` instruction reaches `syscall_dispatch` without GPF. Verify on QEMU WHPX, TCG, VirtualBox, bare metal. (Note: 0xD200/0xD201 are taken by `gdt.c`.)

## 3. INT 0x2E Compatibility Path
Windows NT's original software-interrupt syscall vector. Required for early ntdll and any code that does not use `SYSCALL`.

- [x] IDT vector 0x2E set to DPL=3 (type_attr 0xEE) in `idt.c` -- user-mode `int 0x2E` works
- [x] `syscall_handler_2e()` registered in `syscall.c` -- reads RAX as service number, R10/RDX/R8/R9 as args (Windows x64 ABI), dispatches via `ssdt_dispatch()`
- [x] Uses existing `irq14` stub which has full register save/restore + swapgs + iretq
- [x] INT 0x80 kept active alongside INT 0x2E (both paths coexist)
- [x] Commit: `"kernel: nt -- INT 0x2E syscall compatibility path"`

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

### SSDT Master Table

> Service numbers organized by functional range. Each range has headroom for future additions. Endpoints marked `→ TODO-XX` are implemented in that TODO and registered here.

**0x0000–0x000F: Core Object and Handle Operations**

| Index  | Function                         | §   | Owner                | Done |
|--------|----------------------------------|-----|----------------------|------|
| 0x0000 | NtClose                          | §5  | T05 (§5 SSDT wrapper)| [x]  |
| 0x0001 | NtDuplicateObject                | §6  | T05 (ob.c exists)    | [ ]  |
| 0x0002 | NtQueryObject                    | §6  | T05 (ob.c exists)    | [ ]  |
| 0x0003 | NtMakeTemporaryObject            | §16 | T05                  | [ ]  |
| 0x0004 | NtMakePermanentObject            | §16 | T05                  | [ ]  |
| 0x0005 | NtSetInformationObject           | §16 | T05                  | [ ]  |
| 0x0006 | NtWaitForSingleObject            | §5  | T05 (§5 migration)   | [x]  |
| 0x0007 | NtWaitForMultipleObjects         | §8  | T05                  | [ ]  |
| 0x0008 | NtSignalAndWaitForSingleObject   | §8  | T05                  | [ ]  |
| 0x0009 | NtCompareObjects                 | §16 | T05                  | [ ]  |

**0x0010–0x002F: File I/O**

| Index  | Function                         | §   | Owner                     | Done |
|--------|----------------------------------|-----|---------------------------|------|
| 0x0010 | NtCreateFile                     | §6  | T05                       | [ ]  |
| 0x0011 | NtOpenFile                       | §6  | T05                       | [ ]  |
| 0x0012 | NtReadFile                       | §5  | T05 (§5 migration)         | [x]  |
| 0x0013 | NtWriteFile                      | §5  | T05 (§5 migration)         | [x]  |
| 0x0014 | NtDeleteFile                     | §13 | T05                       | [ ]  |
| 0x0015 | NtQueryInformationFile           | §13 | T05                       | [ ]  |
| 0x0016 | NtSetInformationFile             | §13 | T05                       | [ ]  |
| 0x0017 | NtQueryDirectoryFile             | §5  | T05 (§5 migration)         | [x]  |
| 0x0018 | NtFlushBuffersFile               | §13 | T05                       | [ ]  |
| 0x0019 | NtDeviceIoControlFile            | §13 | T05 (dispatch to drivers) | [ ]  |
| 0x001A | NtFsControlFile                  | §13 | T05                       | [ ]  |
| 0x001B | NtCreateNamedPipeFile            | §5  | T05 (§5 migration)         | [x]  |
| 0x001C | NtCreateMailslotFile             | §13 | T08-mem §3 (MSFS)         | [ ]  |
| 0x001D | NtLockFile                       | §13 | T05                       | [ ]  |
| 0x001E | NtUnlockFile                     | §13 | T05                       | [ ]  |
| 0x001F | NtNotifyChangeDirectoryFile      | §13 | T05                       | [ ]  |
| 0x0020 | NtQueryVolumeInformationFile     | §13 | T05                       | [ ]  |
| 0x0021 | NtSetVolumeInformationFile       | §13 | T05                       | [ ]  |
| 0x0022 | NtQueryEaFile                    | §13 | T05                       | [ ]  |
| 0x0023 | NtSetEaFile                      | §13 | T05                       | [ ]  |
| 0x0024 | NtReadFileScatter                | §13 | T05                       | [ ]  |
| 0x0025 | NtWriteFileGather                | §13 | T05                       | [ ]  |
| 0x0026 | NtCancelIoFile                   | §13 | T05                       | [ ]  |
| 0x0027 | NtCancelIoFileEx                 | §13 | T05                       | [ ]  |
| 0x0028 | NtQueryAttributesFile            | §13 | T05                       | [ ]  |
| 0x0029 | NtQueryFullAttributesFile        | §13 | T05                       | [ ]  |

**0x0030–0x004F: Process and Thread**

| Index  | Function                         | §   | Owner                  | Done |
|--------|----------------------------------|-----|------------------------|------|
| 0x0030 | NtCreateProcess                  | §7  | T05 (task.c exists)    | [ ]  |
| 0x0031 | NtCreateProcessEx                | §7  | T09 §4                 | [ ]  |
| 0x0032 | NtOpenProcess                    | §7  | T05                    | [ ]  |
| 0x0033 | NtTerminateProcess               | §5  | T05 (§5 migration)     | [x]  |
| 0x0034 | NtQueryInformationProcess        | §10 | T05                    | [ ]  |
| 0x0035 | NtSetInformationProcess          | §7  | T05                    | [ ]  |
| 0x0036 | NtCreateThread                   | §7  | T05                    | [ ]  |
| 0x0037 | NtCreateThreadEx                 | §7  | T09 §5                 | [ ]  |
| 0x0038 | NtOpenThread                     | §7  | T05                    | [ ]  |
| 0x0039 | NtTerminateThread                | §7  | T05                    | [ ]  |
| 0x003A | NtResumeThread                   | §7  | T05                    | [ ]  |
| 0x003B | NtSuspendThread                  | §7  | T05                    | [ ]  |
| 0x003C | NtGetContextThread               | §7  | T10 §4 (CONTEXT)       | [ ]  |
| 0x003D | NtSetContextThread               | §7  | T10 §4 (CONTEXT)       | [ ]  |
| 0x003E | NtQueryInformationThread         | §7  | T05                    | [ ]  |
| 0x003F | NtSetInformationThread           | §7  | T05                    | [ ]  |
| 0x0040 | NtAlertThread                    | §7  | T05                    | [ ]  |
| 0x0041 | NtAlertResumeThread              | §7  | T05                    | [ ]  |
| 0x0042 | NtImpersonateThread              | §15 | T11 (SRM)              | [ ]  |
| 0x0043 | NtQueueApcThread                 | §7  | T06 §11 (APC)          | [ ]  |
| 0x0044 | NtYieldExecution                 | §5  | T05 (§5 migration)     | [x]  |
| 0x0045 | NtCreateUserProcess              | §7  | T09 §4                 | [ ]  |
| 0x0046 | NtTestAlert                      | §7  | T05                    | [ ]  |
| 0x0047 | NtDelayExecution                 | §7  | T05                    | [ ]  |

**0x0050–0x006F: Memory Management**

| Index  | Function                         | §   | Owner                   | Done |
|--------|----------------------------------|-----|-------------------------|------|
| 0x0050 | NtAllocateVirtualMemory          | §9  | T05 (pmm exists)        | [ ]  |
| 0x0051 | NtFreeVirtualMemory              | §9  | T05                     | [ ]  |
| 0x0052 | NtProtectVirtualMemory           | §9  | T01-mem §1 (VMM prot)   | [ ]  |
| 0x0053 | NtQueryVirtualMemory             | §9  | T05                     | [ ]  |
| 0x0054 | NtLockVirtualMemory              | §9  | T04-mem §4 (adv VM)     | [ ]  |
| 0x0055 | NtUnlockVirtualMemory            | §9  | T04-mem §4              | [ ]  |
| 0x0056 | NtFlushVirtualMemory             | §9  | T05                     | [ ]  |
| 0x0057 | NtReadVirtualMemory              | §9  | T05                     | [ ]  |
| 0x0058 | NtWriteVirtualMemory             | §9  | T05                     | [ ]  |
| 0x0059 | NtAllocateUserPhysicalPages      | §9  | T04-mem §4 (AWE)        | [ ]  |
| 0x005A | NtFreeUserPhysicalPages          | §9  | T04-mem §4              | [ ]  |
| 0x005B | NtMapUserPhysicalPages           | §9  | T04-mem §4              | [ ]  |
| 0x005C | NtCreateSection                  | §5  | T05 (§5 migration)      | [x]  |
| 0x005D | NtOpenSection                    | §17 | T05                     | [ ]  |
| 0x005E | NtMapViewOfSection               | §5  | T05 (§5 migration)      | [x]  |
| 0x005F | NtUnmapViewOfSection             | §17 | T05 (ob_section exists) | [ ]  |
| 0x0060 | NtExtendSection                  | §17 | T05                     | [ ]  |
| 0x0061 | NtQuerySection                   | §17 | T05                     | [ ]  |
| 0x0062 | NtAreMappedFilesTheSame          | §17 | T05                     | [ ]  |

**0x0070–0x008F: Synchronization**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0070 | NtCreateEvent                    | §8  | T05 (ob_event exists)    | [ ]  |
| 0x0071 | NtOpenEvent                      | §8  | T05                      | [ ]  |
| 0x0072 | NtSetEvent                       | §8  | T05                      | [ ]  |
| 0x0073 | NtResetEvent                     | §8  | T05                      | [ ]  |
| 0x0074 | NtPulseEvent                     | §8  | T05                      | [ ]  |
| 0x0075 | NtQueryEvent                     | §8  | T05                      | [ ]  |
| 0x0076 | NtCreateMutant                   | §8  | T05 (ob_mutex exists)    | [ ]  |
| 0x0077 | NtOpenMutant                     | §8  | T05                      | [ ]  |
| 0x0078 | NtReleaseMutant                  | §8  | T05                      | [ ]  |
| 0x0079 | NtQueryMutant                    | §8  | T05                      | [ ]  |
| 0x007A | NtCreateSemaphore                | §8  | T05 (ob_sem exists)      | [ ]  |
| 0x007B | NtOpenSemaphore                  | §8  | T05                      | [ ]  |
| 0x007C | NtReleaseSemaphore               | §8  | T05                      | [ ]  |
| 0x007D | NtQuerySemaphore                 | §8  | T05                      | [ ]  |
| 0x007E | NtCreateTimer                    | §18 | T05 (ob_timer exists)    | [ ]  |
| 0x007F | NtOpenTimer                      | §18 | T05 §18                  | [ ]  |
| 0x0080 | NtSetTimer                       | §18 | T05 §18                  | [ ]  |
| 0x0081 | NtCancelTimer                    | §18 | T05 §18                  | [ ]  |
| 0x0082 | NtQueryTimer                     | §18 | T05 §18                  | [ ]  |
| 0x0083 | NtSetTimerEx                     | §18 | T05 §18                  | [ ]  |
| 0x0084 | NtCreateKeyedEvent               | §8  | T07-mem §4 (futex)       | [ ]  |
| 0x0085 | NtOpenKeyedEvent                 | §8  | T07-mem §4               | [ ]  |
| 0x0086 | NtWaitForKeyedEvent              | §8  | T07-mem §4               | [ ]  |
| 0x0087 | NtReleaseKeyedEvent              | §8  | T07-mem §4               | [ ]  |
| 0x0088 | NtCreateIoCompletion             | §13 | T08-mem §4 (IOCP)        | [ ]  |
| 0x0089 | NtSetIoCompletion                | §13 | T08-mem §4               | [ ]  |
| 0x008A | NtRemoveIoCompletion             | §13 | T08-mem §4               | [ ]  |
| 0x008B | NtQueryIoCompletion              | §13 | T08-mem §4               | [ ]  |
| 0x008C | NtSetIoCompletionEx              | §13 | T08-mem §4               | [ ]  |
| 0x008D | NtRemoveIoCompletionEx           | §13 | T08-mem §4               | [ ]  |

**0x0090–0x00AF: Registry**

| Index  | Function                         | §   | Owner                | Done |
|--------|----------------------------------|-----|----------------------|------|
| 0x0090 | NtCreateKey                      | §14 | T13 §4               | [ ]  |
| 0x0091 | NtCreateKeyTransacted            | §14 | T13 §4               | [ ]  |
| 0x0092 | NtOpenKey                        | §14 | T13 §4               | [ ]  |
| 0x0093 | NtOpenKeyTransacted              | §14 | T13 §4               | [ ]  |
| 0x0094 | NtOpenKeyEx                      | §14 | T13 §4               | [ ]  |
| 0x0095 | NtDeleteKey                      | §14 | T13 §4               | [ ]  |
| 0x0096 | NtSetValueKey                    | §14 | T13 §4               | [ ]  |
| 0x0097 | NtQueryValueKey                  | §14 | T13 §4               | [ ]  |
| 0x0098 | NtDeleteValueKey                 | §14 | T13 §4               | [ ]  |
| 0x0099 | NtEnumerateKey                   | §14 | T13 §4               | [ ]  |
| 0x009A | NtEnumerateValueKey              | §14 | T13 §4               | [ ]  |
| 0x009B | NtQueryKey                       | §14 | T13 §4               | [ ]  |
| 0x009C | NtFlushKey                       | §14 | T13 §4               | [ ]  |
| 0x009D | NtNotifyChangeKey                | §14 | T13 §4               | [ ]  |
| 0x009E | NtNotifyChangeMultipleKeys       | §14 | T13 §4               | [ ]  |
| 0x009F | NtRenameKey                      | §14 | T13 §4               | [ ]  |
| 0x00A0 | NtSaveKey                        | §14 | T13 §4               | [ ]  |
| 0x00A1 | NtSaveKeyEx                      | §14 | T13 §4               | [ ]  |
| 0x00A2 | NtRestoreKey                     | §14 | T13 §4               | [ ]  |
| 0x00A3 | NtLoadKey                        | §14 | T13 §4               | [ ]  |
| 0x00A4 | NtLoadKeyEx                      | §14 | T13 §4               | [ ]  |
| 0x00A5 | NtUnloadKey                      | §14 | T13 §4               | [ ]  |
| 0x00A6 | NtUnloadKeyEx                    | §14 | T13 §4               | [ ]  |
| 0x00A7 | NtQueryOpenSubKeys               | §14 | T13 §4               | [ ]  |
| 0x00A8 | NtCompactKeys                    | §14 | T13 §4               | [ ]  |
| 0x00A9 | NtCompressKey                    | §14 | T13 §4               | [ ]  |
| 0x00AA | NtLockRegistryKey                | §14 | T13 §4               | [ ]  |

**0x00B0–0x00CF: Security and Token**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x00B0 | NtOpenProcessToken               | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B1 | NtOpenProcessTokenEx             | §15 | T11                      | [ ]  |
| 0x00B2 | NtOpenThreadToken                | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B3 | NtOpenThreadTokenEx              | §15 | T11                      | [ ]  |
| 0x00B4 | NtQueryInformationToken          | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B5 | NtSetInformationToken            | §15 | T11                      | [ ]  |
| 0x00B6 | NtAdjustPrivilegesToken          | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B7 | NtAdjustGroupsToken              | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B8 | NtDuplicateToken                 | §15 | T11 (token.c exists)     | [ ]  |
| 0x00B9 | NtFilterToken                    | §15 | T11                      | [ ]  |
| 0x00BA | NtCreateToken                    | §15 | T11                      | [ ]  |
| 0x00BB | NtCompareTokens                  | §15 | T11                      | [ ]  |
| 0x00BC | NtAccessCheck                    | §15 | T11 §4 (SeAccessCheck)   | [ ]  |
| 0x00BD | NtAccessCheckAndAuditAlarm       | §15 | T11                      | [ ]  |
| 0x00BE | NtAccessCheckByType              | §15 | T11                      | [ ]  |
| 0x00BF | NtPrivilegeCheck                 | §15 | T11                      | [ ]  |
| 0x00C0 | NtPrivilegeObjectAuditAlarm      | §15 | T11                      | [ ]  |
| 0x00C1 | NtSetSecurityObject              | §15 | T11                      | [ ]  |
| 0x00C2 | NtQuerySecurityObject            | §15 | T11                      | [ ]  |
| 0x00C3 | NtAllocateLocallyUniqueId        | §15 | T11 (luid.c exists)      | [ ]  |
| 0x00C4 | NtCreateTokenEx                  | §15 | T11                      | [ ]  |

**0x00D0–0x00EF: System Information and Control**

| Index  | Function                             | §   | Owner                  | Done |
|--------|--------------------------------------|-----|------------------------|------|
| 0x00D0 | NtQuerySystemInformation             | §5  | T05 (§5 migration)     | [x]  |
| 0x00D1 | NtSetSystemInformation               | §10 | T05                    | [ ]  |
| 0x00D2 | NtQuerySystemEnvironmentValue        | §22 | T14 §5 (env vars)      | [ ]  |
| 0x00D3 | NtSetSystemEnvironmentValue          | §22 | T14 §5                 | [ ]  |
| 0x00D4 | NtQuerySystemEnvironmentValueEx      | §22 | T14 §5                 | [ ]  |
| 0x00D5 | NtSetSystemEnvironmentValueEx        | §22 | T14 §5                 | [ ]  |
| 0x00D6 | NtEnumerateSystemEnvironmentValuesEx | §22 | T14 §5                 | [ ]  |
| 0x00D7 | NtShutdownSystem                     | §5  | T05 (§5 migration)     | [x]  |
| 0x00D8 | NtDisplayString                      | §22 | T05                    | [ ]  |
| 0x00D9 | NtRaiseHardError                     | §22 | T05                    | [ ]  |
| 0x00DA | NtQueryDefaultLocale                 | §22 | T05                    | [ ]  |
| 0x00DB | NtSetDefaultLocale                   | §22 | T05                    | [ ]  |
| 0x00DC | NtQueryDefaultUILanguage             | §22 | T05                    | [ ]  |
| 0x00DD | NtSetDefaultUILanguage               | §22 | T05                    | [ ]  |
| 0x00DE | NtQueryInstallUILanguage             | §22 | T05                    | [ ]  |
| 0x00DF | NtAddAtom                            | §22 | T05                    | [ ]  |
| 0x00E0 | NtFindAtom                           | §22 | T05                    | [ ]  |
| 0x00E1 | NtDeleteAtom                         | §22 | T05                    | [ ]  |
| 0x00E2 | NtQueryInformationAtom               | §22 | T05                    | [ ]  |

**0x00F0–0x00FF: Time and Timer (→ XREF TODO-07 §8,§9)**

| Index  | Function                         | §   | Owner                | Done |
|--------|----------------------------------|-----|----------------------|------|
| 0x00F0 | NtQuerySystemTime                | §18 | T07 §9               | [ ]  |
| 0x00F1 | NtSetSystemTime                  | §18 | T07 §9               | [ ]  |
| 0x00F2 | NtQueryPerformanceCounter        | §18 | T07 §9               | [ ]  |
| 0x00F3 | NtQueryTimerResolution           | §18 | T07 §8               | [ ]  |
| 0x00F4 | NtSetTimerResolution             | §18 | T07 §8               | [ ]  |

**0x0100–0x011F: ALPC and LPC Ports (→ XREF TODO-12 §8)**

| Index  | Function                         | §   | Owner                | Done |
|--------|----------------------------------|-----|----------------------|------|
| 0x0100 | NtCreatePort                     | §19 | T12 §8               | [ ]  |
| 0x0101 | NtCreateWaitablePort             | §19 | T12 §8               | [ ]  |
| 0x0102 | NtConnectPort                    | §19 | T12 §8               | [ ]  |
| 0x0103 | NtSecureConnectPort              | §19 | T12 §8               | [ ]  |
| 0x0104 | NtAcceptConnectPort              | §19 | T12 §8               | [ ]  |
| 0x0105 | NtCompleteConnectPort            | §19 | T12 §8               | [ ]  |
| 0x0106 | NtListenPort                     | §19 | T12 §8               | [ ]  |
| 0x0107 | NtReplyPort                      | §19 | T12 §8               | [ ]  |
| 0x0108 | NtReplyWaitReceivePort           | §19 | T12 §8               | [ ]  |
| 0x0109 | NtReplyWaitReceivePortEx         | §19 | T12 §8               | [ ]  |
| 0x010A | NtRequestPort                    | §19 | T12 §8               | [ ]  |
| 0x010B | NtRequestWaitReplyPort           | §19 | T12 §8               | [ ]  |
| 0x010C | NtImpersonateClientOfPort        | §19 | T12 §8               | [ ]  |
| 0x010D | NtReadRequestData                | §19 | T12 §8               | [ ]  |
| 0x010E | NtWriteRequestData               | §19 | T12 §8               | [ ]  |
| 0x010F | NtAlpcCreatePort                 | §19 | T12 §8               | [ ]  |
| 0x0110 | NtAlpcConnectPort                | §19 | T12 §8               | [ ]  |
| 0x0111 | NtAlpcConnectPortEx              | §19 | T12 §8               | [ ]  |
| 0x0112 | NtAlpcAcceptConnectPort          | §19 | T12 §8               | [ ]  |
| 0x0113 | NtAlpcSendWaitReceivePort        | §19 | T12 §8               | [ ]  |
| 0x0114 | NtAlpcDisconnectPort             | §19 | T12 §8               | [ ]  |
| 0x0115 | NtAlpcCancelMessage              | §19 | T12 §8               | [ ]  |
| 0x0116 | NtAlpcCreatePortSection          | §19 | T12 §8               | [ ]  |
| 0x0117 | NtAlpcDeletePortSection          | §19 | T12 §8               | [ ]  |
| 0x0118 | NtAlpcCreateSectionView          | §19 | T12 §8               | [ ]  |
| 0x0119 | NtAlpcDeleteSectionView          | §19 | T12 §8               | [ ]  |
| 0x011A | NtAlpcCreateResourceReserve      | §19 | T12 §8               | [ ]  |
| 0x011B | NtAlpcDeleteResourceReserve      | §19 | T12 §8               | [ ]  |
| 0x011C | NtAlpcQueryInformation           | §19 | T12 §8               | [ ]  |
| 0x011D | NtAlpcSetInformation             | §19 | T12 §8               | [ ]  |
| 0x011E | NtAlpcQueryInformationMessage    | §19 | T12 §8               | [ ]  |

**0x0120–0x012F: Namespace and Directory Objects**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0120 | NtCreateDirectoryObject          | §16 | T05                      | [ ]  |
| 0x0121 | NtOpenDirectoryObject            | §16 | T05 (ob.c exists)        | [ ]  |
| 0x0122 | NtQueryDirectoryObject           | §16 | T05 (ob.c exists)        | [ ]  |
| 0x0123 | NtCreateSymbolicLinkObject       | §16 | T05                      | [ ]  |
| 0x0124 | NtOpenSymbolicLinkObject         | §16 | T05                      | [ ]  |
| 0x0125 | NtQuerySymbolicLinkObject        | §16 | T05                      | [ ]  |

**0x0130–0x013F: Debug and Exception (→ XREF TODO-10 §5, TODO-18)**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0130 | NtRaiseException                 | §20 | T10 §4                   | [ ]  |
| 0x0131 | NtContinue                       | §20 | T10 §4                   | [ ]  |
| 0x0132 | NtDebugActiveProcess             | §20 | T18 §13                  | [ ]  |
| 0x0133 | NtDebugContinue                  | §20 | T18 §13                  | [ ]  |
| 0x0134 | NtRemoveProcessDebug             | §20 | T18 §13                  | [ ]  |
| 0x0135 | NtCreateDebugObject              | §20 | T18 §13                  | [ ]  |
| 0x0136 | NtWaitForDebugEvent              | §20 | T18 §13                  | [ ]  |
| 0x0137 | NtSetInformationDebugObject      | §20 | T18 §13                  | [ ]  |

**0x0140–0x014F: Power and Shutdown (→ XREF TODO-15)**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0140 | NtSetSystemPowerState            | §21 | T15 §12                  | [ ]  |
| 0x0141 | NtInitiatePowerAction            | §21 | T15 §12                  | [ ]  |
| 0x0142 | NtPowerInformation               | §21 | T15 §12                  | [ ]  |
| 0x0143 | NtGetDevicePowerState            | §21 | T15 §12                  | [ ]  |
| 0x0144 | NtSetThreadExecutionState        | §21 | T15 §12                  | [ ]  |
| 0x0145 | NtRequestWakeupLatency           | §21 | T15 §12                  | [ ]  |

**0x0150–0x015F: Audit and Tracing (Impossible OS exclusive)**

| Index  | Function                         | §   | Owner                | Done |
|--------|----------------------------------|-----|----------------------|------|
| 0x0150 | NtRegisterSyscallAuditHook       | §23 | T05                  | [ ]  |
| 0x0151 | NtUnregisterSyscallAuditHook     | §23 | T05                  | [ ]  |
| 0x0152 | NtQuerySyscallAuditState         | §23 | T05                  | [ ]  |

**0x0160–0x017F: Job Objects**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0160 | NtCreateJobObject                | §7  | T09 §13                  | [ ]  |
| 0x0161 | NtOpenJobObject                  | §7  | T09 §13                  | [ ]  |
| 0x0162 | NtAssignProcessToJobObject       | §7  | T09 §13                  | [ ]  |
| 0x0163 | NtTerminateJobObject             | §7  | T09 §13                  | [ ]  |
| 0x0164 | NtQueryInformationJobObject      | §7  | T09 §13                  | [ ]  |
| 0x0165 | NtSetInformationJobObject        | §7  | T09 §13                  | [ ]  |
| 0x0166 | NtIsProcessInJob                 | §7  | T09 §13                  | [ ]  |
| 0x0167 | NtCreateJobSet                   | §7  | T09 §13                  | [ ]  |

**0x0180–0x019F: Worker Factory (Thread Pool)**

| Index  | Function                           | §   | Owner                   | Done |
|--------|------------------------------------|-----|-------------------------|------|
| 0x0180 | NtCreateWorkerFactory              | §7  | T05-mem §5 (IOCP pool)  | [ ]  |
| 0x0181 | NtWorkerFactoryWorkerReady         | §7  | T05-mem §5              | [ ]  |
| 0x0182 | NtReleaseWorkerFactoryWorker       | §7  | T05-mem §5              | [ ]  |
| 0x0183 | NtShutdownWorkerFactory            | §7  | T05-mem §5              | [ ]  |
| 0x0184 | NtQueryInformationWorkerFactory    | §7  | T05-mem §5              | [ ]  |
| 0x0185 | NtSetInformationWorkerFactory      | §7  | T05-mem §5              | [ ]  |
| 0x0186 | NtWaitForWorkViaWorkerFactory      | §7  | T05-mem §5              | [ ]  |

**0x01A0–0x01CF: Kernel Transaction Manager (KTM)**

| Index  | Function                           | §   | Owner                  | Done |
|--------|------------------------------------|-----|------------------------|------|
| 0x01A0 | NtCreateTransactionManager         | §22 | T13 (registry txn)     | [ ]  |
| 0x01A1 | NtOpenTransactionManager           | §22 | T13                    | [ ]  |
| 0x01A2 | NtCreateTransaction                | §22 | T13                    | [ ]  |
| 0x01A3 | NtOpenTransaction                  | §22 | T13                    | [ ]  |
| 0x01A4 | NtCommitTransaction                | §22 | T13                    | [ ]  |
| 0x01A5 | NtRollbackTransaction              | §22 | T13                    | [ ]  |
| 0x01A6 | NtQueryInformationTransaction      | §22 | T13                    | [ ]  |
| 0x01A7 | NtSetInformationTransaction        | §22 | T13                    | [ ]  |
| 0x01A8 | NtCreateResourceManager            | §22 | T13                    | [ ]  |
| 0x01A9 | NtOpenResourceManager              | §22 | T13                    | [ ]  |
| 0x01AA | NtQueryInformationResourceManager  | §22 | T13                    | [ ]  |
| 0x01AB | NtSetInformationResourceManager    | §22 | T13                    | [ ]  |
| 0x01AC | NtCreateEnlistment                 | §22 | T13                    | [ ]  |
| 0x01AD | NtOpenEnlistment                   | §22 | T13                    | [ ]  |
| 0x01AE | NtQueryInformationEnlistment       | §22 | T13                    | [ ]  |
| 0x01AF | NtSetInformationEnlistment         | §22 | T13                    | [ ]  |
| 0x01B0 | NtPrepareEnlistment                | §22 | T13                    | [ ]  |
| 0x01B1 | NtPrePrepareEnlistment             | §22 | T13                    | [ ]  |
| 0x01B2 | NtCommitEnlistment                 | §22 | T13                    | [ ]  |
| 0x01B3 | NtRollbackEnlistment               | §22 | T13                    | [ ]  |
| 0x01B4 | NtRecoverTransactionManager        | §22 | T13                    | [ ]  |
| 0x01B5 | NtRecoverResourceManager           | §22 | T13                    | [ ]  |
| 0x01B6 | NtRecoverEnlistment                | §22 | T13                    | [ ]  |
| 0x01B7 | NtPropagationComplete              | §22 | T13                    | [ ]  |
| 0x01B8 | NtPropagationFailed                | §22 | T13                    | [ ]  |
| 0x01B9 | NtFreezeTransactions               | §22 | T13                    | [ ]  |
| 0x01BA | NtThawTransactions                 | §22 | T13                    | [ ]  |
| 0x01BB | NtCreateRegistryTransaction        | §14 | T13 §4                 | [ ]  |
| 0x01BC | NtOpenRegistryTransaction          | §14 | T13 §4                 | [ ]  |
| 0x01BD | NtCommitRegistryTransaction        | §14 | T13 §4                 | [ ]  |
| 0x01BE | NtRollbackRegistryTransaction      | §14 | T13 §4                 | [ ]  |

**0x01D0–0x01DF: ETW (Event Tracing for Windows)** -- **7/7 DONE** (etw.c, T02 §7)

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x01D0 | NtTraceEvent                     | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D1 | NtTraceControl                   | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D2 | NtCreateTrace                    | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D3 | NtQueryTrace                     | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D4 | NtUpdateTrace                    | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D5 | NtStopTrace                      | §22 | T02 §7 (etw.c)          | [x]  |
| 0x01D6 | NtFlushTrace                     | §22 | T02 §7 (etw.c)          | [x]  |

**0x01E0–0x01EF: WNF (Windows Notification Facility)**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x01E0 | NtCreateWnfStateName             | §22 | T05                      | [ ]  |
| 0x01E1 | NtDeleteWnfStateName             | §22 | T05                      | [ ]  |
| 0x01E2 | NtQueryWnfStateData              | §22 | T05                      | [ ]  |
| 0x01E3 | NtUpdateWnfStateData             | §22 | T05                      | [ ]  |
| 0x01E4 | NtSubscribeWnfStateChange        | §22 | T05                      | [ ]  |
| 0x01E5 | NtUnsubscribeWnfStateChange      | §22 | T05                      | [ ]  |
| 0x01E6 | NtQueryWnfStateNameInformation   | §22 | T05                      | [ ]  |

**0x01F0–0x01FF: Enclave (VBS / SGX)**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x01F0 | NtCreateEnclave                  | §22 | T17 (security hardening) | [ ]  |
| 0x01F1 | NtLoadEnclaveData                | §22 | T17                      | [ ]  |
| 0x01F2 | NtInitializeEnclave              | §22 | T17                      | [ ]  |
| 0x01F3 | NtTerminateEnclave               | §22 | T17                      | [ ]  |
| 0x01F4 | NtCallEnclave                    | §22 | T17                      | [ ]  |

**0x0200–0x021F: Process and Thread Extensions**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0200 | NtSuspendProcess                 | §7  | T09 §4                   | [ ]  |
| 0x0201 | NtResumeProcess                  | §7  | T09 §4                   | [ ]  |
| 0x0202 | NtGetNextProcess                 | §7  | T09 §4                   | [ ]  |
| 0x0203 | NtGetNextThread                  | §7  | T09 §4                   | [ ]  |
| 0x0204 | NtCreateProcessStateChange       | §7  | T09                      | [ ]  |
| 0x0205 | NtChangeProcessState             | §7  | T09                      | [ ]  |
| 0x0206 | NtCreateThreadStateChange        | §7  | T09                      | [ ]  |
| 0x0207 | NtChangeThreadState              | §7  | T09                      | [ ]  |
| 0x0208 | NtGetCurrentProcessorNumber      | §10 | T05                      | [ ]  |
| 0x0209 | NtGetCurrentProcessorNumberEx    | §10 | T05                      | [ ]  |
| 0x020A | NtFlushProcessWriteBuffers       | §9  | T05                      | [ ]  |
| 0x020B | NtQueryPortInformationProcess    | §10 | T05                      | [ ]  |

**0x0220–0x023F: Memory Extensions**

| Index  | Function                           | §   | Owner                  | Done |
|--------|--------------------------------------|-----|----------------------|------|
| 0x0220 | NtAllocateVirtualMemoryEx          | §9  | T04-mem §4             | [ ]  |
| 0x0221 | NtCreateSectionEx                  | §17 | T05                    | [ ]  |
| 0x0222 | NtMapViewOfSectionEx               | §17 | T05                    | [ ]  |
| 0x0223 | NtSetInformationVirtualMemory      | §9  | T04-mem §4             | [ ]  |
| 0x0224 | NtGetWriteWatch                    | §9  | T04-mem §4             | [ ]  |
| 0x0225 | NtResetWriteWatch                  | §9  | T04-mem §4             | [ ]  |
| 0x0226 | NtCreatePagingFile                 | §9  | T05                    | [ ]  |

**0x0240–0x024F: Event Pair**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0240 | NtCreateEventPair                | §8  | T05                      | [ ]  |
| 0x0241 | NtOpenEventPair                  | §8  | T05                      | [ ]  |
| 0x0242 | NtSetHighEventPair               | §8  | T05                      | [ ]  |
| 0x0243 | NtSetLowEventPair                | §8  | T05                      | [ ]  |
| 0x0244 | NtWaitHighEventPair              | §8  | T05                      | [ ]  |
| 0x0245 | NtWaitLowEventPair               | §8  | T05                      | [ ]  |

**0x0250–0x025F: Profile and Performance Counters**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0250 | NtCreateProfile                  | §10 | T05                      | [ ]  |
| 0x0251 | NtCreateProfileEx                | §10 | T05                      | [ ]  |
| 0x0252 | NtStartProfile                   | §10 | T05                      | [ ]  |
| 0x0253 | NtStopProfile                    | §10 | T05                      | [ ]  |
| 0x0254 | NtSetIntervalProfile             | §10 | T05                      | [ ]  |
| 0x0255 | NtQueryIntervalProfile           | §10 | T05                      | [ ]  |

**0x0260–0x026F: Session and Licensing**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0260 | NtOpenSession                    | §22 | T05                      | [ ]  |
| 0x0261 | NtNotifyChangeSession            | §22 | T05                      | [ ]  |
| 0x0262 | NtQueryLicenseValue              | §22 | T05                      | [ ]  |
| 0x0263 | NtGetMUIRegistryInfo             | §22 | T05                      | [ ]  |
| 0x0264 | NtIsUILanguageComitted           | §22 | T05                      | [ ]  |
| 0x0265 | NtFlushInstallUILanguage         | §22 | T05                      | [ ]  |

**0x0270–0x027F: Plug and Play**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0270 | NtPlugPlayControl                | §22 | T01-drv §1 (device mgr)  | [ ]  |
| 0x0271 | NtGetPlugPlayEvent               | §22 | T01-drv §1               | [ ]  |
| 0x0272 | NtSerializeBoot                  | §22 | T05                      | [ ]  |

**0x0280–0x029F: I/O Ring (Fast Async I/O -- Win11+)**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x0280 | NtCreateIoRing                   | §13 | T05                      | [ ]  |
| 0x0281 | NtSubmitIoRing                   | §13 | T05                      | [ ]  |
| 0x0282 | NtQueryIoRingCapabilities        | §13 | T05                      | [ ]  |
| 0x0283 | NtSetInformationIoRing           | §13 | T05                      | [ ]  |
| 0x0284 | NtCloseIoRing                    | §13 | T05                      | [ ]  |

**0x02A0–0x02BF: Security Extensions (AppContainer, Signing)**

| Index  | Function                                   | §   | Owner                  | Done |
|--------|--------------------------------------------|-----|------------------------|------|
| 0x02A0 | NtCreateLowBoxToken                        | §15 | T11                    | [ ]  |
| 0x02A1 | NtQuerySecurityPolicy                      | §15 | T11                    | [ ]  |
| 0x02A2 | NtSetCachedSigningLevel                    | §15 | T17 (security harden)  | [ ]  |
| 0x02A3 | NtGetCachedSigningLevel                    | §15 | T17                    | [ ]  |
| 0x02A4 | NtCompareSigningLevels                     | §15 | T17                    | [ ]  |
| 0x02A5 | NtSetInformationSymbolicLink               | §16 | T05                    | [ ]  |
| 0x02A6 | NtQuerySecurityAttributesToken             | §15 | T11                    | [ ]  |
| 0x02A7 | NtAccessCheckByTypeAndAuditAlarm           | §15 | T11                    | [ ]  |
| 0x02A8 | NtAccessCheckByTypeResultListAndAuditAlarm | §15 | T11                    | [ ]  |

**0x02C0–0x02DF: Object and Namespace Extensions**

| Index  | Function                           | §   | Owner                  | Done |
|--------|------------------------------------|-----|------------------------|------|
| 0x02C0 | NtCreateDirectoryObjectEx          | §16 | T05                    | [ ]  |
| 0x02C1 | NtQueryDirectoryFileEx             | §13 | T05                    | [ ]  |
| 0x02C2 | NtCreatePrivateNamespace           | §16 | T05                    | [ ]  |
| 0x02C3 | NtOpenPrivateNamespace             | §16 | T05                    | [ ]  |
| 0x02C4 | NtDeletePrivateNamespace           | §16 | T05                    | [ ]  |

**0x02E0–0x02FF: Debug and Filter Extensions**

| Index  | Function                         | §   | Owner                    | Done |
|--------|----------------------------------|-----|--------------------------|------|
| 0x02E0 | NtSystemDebugControl             | §20 | T18 §13                  | [ ]  |
| 0x02E1 | NtQueryDebugFilterState          | §20 | T18 §13                  | [ ]  |
| 0x02E2 | NtSetDebugFilterState            | §20 | T18 §13                  | [ ]  |

**0x0300–0x034F: Miscellaneous / Extended APIs**

| Index  | Function                                              | §   | Owner            | Done |
|--------|-------------------------------------------------------|-----|------------------|------|
| 0x0300 | NtCallbackReturn                                      | §22 | T05              | [ ]  |
| 0x0301 | NtSetLdtEntries                                       | §22 | T05 (x86 compat) | [ ]  |
| 0x0302 | NtQueryOpenSubKeysEx                                  | §14 | T13 §4           | [ ]  |
| 0x0303 | NtMapCMFModule                                        | §22 | T05              | [ ]  |
| 0x0304 | NtCancelSynchronousIoFile                             | §13 | T05              | [ ]  |
| 0x0305 | NtSetTimer2                                           | §18 | T05 §18          | [ ]  |
| 0x0306 | NtCancelTimer2                                        | §18 | T05 §18          | [ ]  |
| 0x0307 | NtCreateResourceManager                               | §22 | T13              | [ ]  |
| 0x0308 | NtApphelpCacheControl                                 | §22 | T05              | [ ]  |
| 0x0309 | NtRaiseStatus                                         | §22 | T05              | [ ]  |
| 0x030A | NtFlushKey                                            | §14 | T13 §4           | [ ]  |
| 0x030B | NtWaitForAlertByThreadId                              | §8  | T07-mem §4       | [ ]  |
| 0x030C | NtAlertThreadByThreadId                               | §8  | T07-mem §4       | [ ]  |
| 0x030D | NtQueryAuxiliaryCounterFrequency                      | §10 | T05              | [ ]  |
| 0x030E | NtConvertBetweenAuxiliaryCounterAndPerformanceCounter | §10 | T05              | [ ]  |
| 0x030F | NtManagePartition                                     | §22 | T05              | [ ]  |
| 0x0310 | NtCreatePartition                                     | §22 | T05              | [ ]  |
| 0x0311 | NtOpenPartition                                       | §22 | T05              | [ ]  |
| 0x0312 | NtManageHotPatch                                      | §22 | T05              | [ ]  |
| 0x0313 | NtQuerySystemInformationEx                            | §10 | T05              | [ ]  |
| 0x0314 | NtCreateTokenEx                                       | §15 | T11              | [ ]  |
| 0x0315 | NtCompareObjects                                      | §16 | T05              | [ ]  |
| 0x0316 | NtQueryInformationByName                              | §13 | T05              | [ ]  |
| 0x0317 | NtCancelWaitCompletionPacket                          | §13 | T05              | [ ]  |
| 0x0318 | NtAssociateWaitCompletionPacket                       | §13 | T05              | [ ]  |
| 0x0319 | NtCreateWaitCompletionPacket                          | §13 | T05              | [ ]  |
| 0x031A | NtDirectGraphicsCall                                  | §22 | T08-gfx (GPU)    | [ ]  |
| 0x031B | NtSetWnfProcessNotificationEvent                      | §22 | T05              | [ ]  |
| 0x031C | NtCopyFileChunk                                       | §13 | T05              | [ ]  |
| 0x031D | NtCreateCrossVmEvent                                  | §8  | T05              | [ ]  |
| 0x031E | NtCreateCrossVmMutant                                 | §8  | T05              | [ ]  |
| 0x031F | NtAcquireCrossVmMutant                                | §8  | T05              | [ ]  |
| 0x0320 | NtQueryInformationEnlistment                          | §22 | T13              | [ ]  |
| 0x0321 | NtSetInformationEnlistment                            | §22 | T13              | [ ]  |
| 0x0322 | NtQueryInformationResourceManager                     | §22 | T13              | [ ]  |
| 0x0323 | NtSetInformationResourceManager                       | §22 | T13              | [ ]  |
| 0x0324 | NtQueryInformationTransactionManager                  | §22 | T13              | [ ]  |
| 0x0325 | NtSetInformationTransactionManager                    | §22 | T13              | [ ]  |

**0x0340–0x037F: Extended File and Volume Operations**

| Index  | Function                             | §   | Owner                | Done |
|--------|--------------------------------------|-----|----------------------|------|
| 0x0340 | NtQueryQuotaInformationFile          | §13 | T05                  | [ ]  |
| 0x0341 | NtSetQuotaInformationFile            | §13 | T05                  | [ ]  |
| 0x0342 | NtQueryOleDirectoryFile              | §13 | T05                  | [ ]  |
| 0x0343 | NtCancelIoFileEx                     | §13 | T05                  | [ ]  |
| 0x0344 | NtSetVolumeInformationFile           | §13 | T05                  | [ ]  |
| 0x0345 | NtSetEaFile                          | §13 | T05                  | [ ]  |
| 0x0346 | NtQueryEaFile                        | §13 | T05                  | [ ]  |
| 0x0347 | NtCreateToken                        | §15 | T11                  | [ ]  |
| 0x0348 | NtFilterToken                        | §15 | T11                  | [ ]  |
| 0x0349 | NtCompareTokens                      | §15 | T11                  | [ ]  |
| 0x034A | NtAccessCheckByTypeResultList        | §15 | T11                  | [ ]  |
| 0x034B | NtOpenObjectAuditAlarm               | §15 | T11                  | [ ]  |
| 0x034C | NtCloseObjectAuditAlarm              | §15 | T11                  | [ ]  |
| 0x034D | NtDeleteObjectAuditAlarm             | §15 | T11                  | [ ]  |
| 0x034E | NtPrivilegedServiceAuditAlarm        | §15 | T11                  | [ ]  |
| 0x034F | NtSetContextChannel                  | §22 | T05                  | [ ]  |

**0x0380–0x03BF: Extended Thread, Memory, and Misc**

| Index  | Function                             | §   | Owner                | Done |
|--------|--------------------------------------|-----|----------------------|------|
| 0x0380 | NtQueueApcThreadEx                   | §7  | T06 §11 (APC)        | [ ]  |
| 0x0381 | NtQueueApcThreadEx2                  | §7  | T06 §11              | [ ]  |
| 0x0382 | NtSetIoCompletionEx                  | §13 | T08-mem §4           | [ ]  |
| 0x0383 | NtRemoveIoCompletionEx               | §13 | T08-mem §4           | [ ]  |
| 0x0384 | NtAlertThreadByThreadIdEx            | §8  | T07-mem §4           | [ ]  |
| 0x0385 | NtWaitForAlertByThreadIdEx           | §8  | T07-mem §4           | [ ]  |
| 0x0386 | NtMapViewOfSection3                  | §17 | T05                  | [ ]  |
| 0x0387 | NtUnmapViewOfSection2                | §17 | T05                  | [ ]  |
| 0x0388 | NtCreateSemaphoreEx                  | §8  | T05                  | [ ]  |
| 0x0389 | NtCreateMutantEx                     | §8  | T05                  | [ ]  |
| 0x038A | NtCreateEventEx                      | §8  | T05                  | [ ]  |
| 0x038B | NtOpenKeyedEvent2                    | §8  | T07-mem §4           | [ ]  |
| 0x038C | NtCreateTimerEx                      | §18 | T05 §18              | [ ]  |
| 0x038D | NtQueryTimerEx                       | §18 | T05 §18              | [ ]  |
| 0x038E | NtSetTimer2                          | §18 | T05 §18              | [ ]  |
| 0x038F | NtCancelTimer2                       | §18 | T05 §18              | [ ]  |
| 0x0390 | NtOpenProcessEx                      | §7  | T09 §4               | [ ]  |
| 0x0391 | NtOpenThreadEx                       | §7  | T09 §4               | [ ]  |
| 0x0392 | NtQueryInformationJobObject          | §7  | T09 §13              | [ ]  |
| 0x0393 | NtSetInformationJobObject            | §7  | T09 §13              | [ ]  |
| 0x0394 | NtQueryDirectoryObjectEx             | §16 | T05                  | [ ]  |
| 0x0395 | NtQuerySymbolicLinkObjectEx          | §16 | T05                  | [ ]  |
| 0x0396 | NtSetSecurityObjectEx                | §15 | T11                  | [ ]  |
| 0x0397 | NtQuerySecurityObjectEx              | §15 | T11                  | [ ]  |
| 0x0398 | NtCreateNamedPipeFileEx              | §6  | T08-mem §1           | [ ]  |
| 0x0399 | NtCreateMailslotFileEx               | §13 | T08-mem §3           | [ ]  |
| 0x039A | NtNotifyChangeDirectoryFileEx        | §13 | T05                  | [ ]  |
| 0x039B | NtSetInformationProcessEx            | §7  | T09 §4               | [ ]  |
| 0x039C | NtQueryInformationProcessEx          | §10 | T05                  | [ ]  |
| 0x039D | NtQueryInformationThreadEx           | §7  | T05                  | [ ]  |

**0x03C0–0x03DF: Impossible OS Exclusive Extensions**

| Index  | Function                             | §   | Owner                  | Done |
|--------|--------------------------------------|-----|------------------------|------|
| 0x03C0 | NtQueryKernelModuleInfo              | §10 | T05                    | [ ]  |
| 0x03C1 | NtQueryBootConfiguration             | §10 | T05                    | [ ]  |
| 0x03C2 | NtQueryPmmStatistics                 | §10 | T05                    | [ ]  |
| 0x03C3 | NtQueryHeapStatistics                | §10 | T05                    | [ ]  |
| 0x03C4 | NtQuerySchedulerStatistics           | §10 | T05 (sched stats)      | [ ]  |
| 0x03C5 | NtQueryInterruptStatistics           | §10 | T05                    | [ ]  |
| 0x03C6 | NtQueryPciDeviceList                 | §10 | T05                    | [ ]  |
| 0x03C7 | NtQueryUsbDeviceList                 | §10 | T05                    | [ ]  |
| 0x03C8 | NtQueryNvmeNamespaceList             | §10 | T05                    | [ ]  |
| 0x03C9 | NtQueryNetworkInterfaceList          | §10 | T05                    | [ ]  |
| 0x03CA | NtQueryPostCodeHistory               | §10 | T05 (boot diagnostics) | [ ]  |
| 0x03CB | NtQueryObNamespaceTree               | §16 | T05                    | [ ]  |
| 0x03CC | NtQueryRegistryStatistics            | §14 | T13                    | [ ]  |
| 0x03CD | NtQueryVfsStatistics                 | §13 | T05                    | [ ]  |
| 0x03CE | NtQuerySmpCpuInfo                    | §10 | T05 (per-CPU info)     | [ ]  |
| 0x03CF | NtQueryKlogRingBuffer                | §10 | T05 (serial log)       | [ ]  |
| 0x03D0 | NtSetKlogLevel                       | §10 | T05                    | [ ]  |
| 0x03D1 | NtQueryCompositorStatistics          | §10 | T05 (desktop stats)    | [ ]  |
| 0x03D2 | NtQueryTimerCalibration              | §18 | T07 §8                 | [ ]  |
| 0x03D3 | NtQueryAcpiTables                    | §10 | T05                    | [ ]  |
| 0x03D4 | NtCreateHardLink                     | §13 | T05                    | [ ]  |
| 0x03D5 | NtQueryHardLinks                     | §13 | T05                    | [ ]  |
| 0x03D6 | NtQueryDriverList                    | §10 | T05 (loaded drivers)   | [ ]  |
| 0x03D7 | NtQueryTaskList                      | §10 | T05 (sched tasks)      | [ ]  |

> **Total: 470 service entries** across 30 functional ranges -- full Windows 11 parity plus Impossible OS exclusive extensions. Shadow SSDT (Win32k) has a separate index space starting at 0x1000.
>
> **Implementation progress: 7/470 wired** (1.5%) -- ETW range complete. Run `/audit-ssdt` to refresh.

**Test checkpoint:** `syscall_dispatch(0xFFFF)` returns `STATUS_NOT_IMPLEMENTED`, not crash. `syscall_dispatch(valid_index)` calls correct handler. Serial: `"ssdt: registered 470 services"` during init.

## 5. Nt/Zw Naming and Existing Syscall Migration
Rename / wrap the existing 22 `SYS_*` implementations to their `NtXxx` equivalents, change return types to `NTSTATUS`, and register them in the SSDT at the indices defined in §4.

- [x] Add `NtWriteFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_WRITE` logic → SSDT 0x0013
- [x] Add `NtReadFile(HANDLE, PIOSB, buf, len)` wrapping `SYS_READ` logic → SSDT 0x0012
- [x] Add `NtTerminateProcess(HANDLE, NTSTATUS)` replacing `SYS_EXIT` → SSDT 0x0033
- [x] Add `NtYieldExecution()` replacing `SYS_YIELD` → SSDT 0x0044; returns `STATUS_SUCCESS`
- [ ] Add `NtCreateProcess`/`NtCreateThread` wrapping fork/exec paths → SSDT 0x0030/0x0036 (§7)
- [x] Add `NtWaitForSingleObject(HANDLE, timeout)` replacing `SYS_WAITPID` → SSDT 0x0006
- [x] Add `NtQueryDirectoryFile` wrapping `SYS_READDIR` → SSDT 0x0017
- [x] Add `NtQuerySystemInformation(SystemProcessInformation)` wrapping `SYS_GETPROCS` → SSDT 0x00D0
- [x] Add `NtTerminateProcess` for kill → SSDT 0x0033
- [x] Add `NtQuerySystemInformation(SystemTimeOfDayInformation)` for uptime → SSDT 0x00D0
- [x] Add `NtShutdownSystem(ShutdownReboot / ShutdownPowerOff)` → SSDT 0x00D7
- [x] Add `NtCreateNamedPipeFile` / `NtReadFile` / `NtWriteFile` wrapping pipe → SSDT 0x001B
- [x] Add `NtCreateSection` / `NtMapViewOfSection` wrapping shmem → SSDT 0x005C/0x005E (→ XREF TODO-03 §7)
- [x] Keep `SYS_*` macros as compile-time aliases pointing to the same SSDT indices for transition
- [x] Change all `sys_*` implementations to return `NTSTATUS`; convert error paths to `STATUS_*` codes
- [x] Add `Nt_Close` SSDT wrapper at 0x0000 bridging to OB NtClose
- [x] 12 NtXxx handlers registered in SSDT via `nt_syscall_register_ssdt()` in `src/kernel/nt/nt_syscall.c`
- [x] 2 unit tests: SSDT registration verification (9 handler callability checks) + SYS_NT_* alias value validation (12 aliases)
- [ ] Commit: `"kernel: nt -- migrate existing syscalls to NtXxx naming and NTSTATUS"`

**Test checkpoint:** All 22 existing syscalls still work via old `SYS_*` macros (backward compat). `NtWriteFile` returns `STATUS_SUCCESS` on valid write. Serial: existing boot/desktop tests pass without regression. Verify on QEMU WHPX, TCG, VirtualBox, bare metal -- this is the most dangerous migration; a return-type mismatch silently corrupts all user-mode callers.

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
- [ ] Commit: `"kernel: nt -- NtCreateFile, NtOpenFile, NtClose, NtReadFile, NtWriteFile"`

**Test checkpoint:** `NtCreateFile` on `X:\Logs\kernel.log` returns `STATUS_SUCCESS` + valid HANDLE. `NtClose(handle)` returns `STATUS_SUCCESS`; second `NtClose` returns `STATUS_INVALID_HANDLE`. `NtReadFile` populates IOSB correctly.

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
- [ ] Commit: `"kernel: nt -- NtCreateProcess, NtCreateThread, full process-thread lifecycle"`

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
- [ ] Commit: `"kernel: nt -- sync objects, NtWaitForMultipleObjects, keyed events"`

**Test checkpoint:** `NtCreateEvent` + `NtSetEvent` + `NtWaitForSingleObject` round-trip succeeds. `NtWaitForMultipleObjects(WaitAny)` returns correct index. `NtCreateMutant` with `InitialOwner=TRUE` is owned by caller. Named objects visible in `\BaseNamedObjects\`. Keyed event wait/release pair succeeds between two threads.

## 9. Virtual Memory (Alloc, Free, Protect, Lock, Cross-Process)
Virtual memory management entry points -- covers the full NT virtual memory API surface.

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
- [ ] Commit: `"kernel: nt -- NtAllocate/Free/Protect/Lock/Read/WriteVirtualMemory"`

**Test checkpoint:** `NtAllocateVirtualMemory` with `MEM_COMMIT | PAGE_READWRITE` returns usable address; write+read round-trip. `NtProtectVirtualMemory` changes RW→RO; write attempt faults. `NtFreeVirtualMemory` with `MEM_RELEASE` returns `STATUS_SUCCESS`. `NtReadVirtualMemory` from kernel to user address space succeeds.

## 10. NtQuerySystemInformation / NtQueryInformationProcess
Provides OS version, process list, performance counters, and detailed process info to ntdll and user-mode tools.

- [ ] `NtQuerySystemInformation(SystemInformationClass, SystemInformation, Length, ReturnLength)`:
  - `SystemBasicInformation (0)`: number of processors, page size, min/max user address, allocation granularity
  - `SystemPerformanceInformation (2)`: available pages, commit total, commit limit (from PMM stats)
  - `SystemTimeOfDayInformation (3)`: boot time, current time, time zone bias
  - `SystemProcessInformation (5)`: linked list of `SYSTEM_PROCESS_INFORMATION` -- PID, name, thread count, handle count, memory usage
  - `SystemProcessorInformation (1)`: processor architecture, level, revision
  - `SystemModuleInformation (11)`: loaded kernel modules
  - `SystemHandleInformation (16)`: system-wide handle table dump
  - `SystemObjectInformation (17)`: object type statistics
  - `SystemInterruptInformation (23)`: per-CPU interrupt counts
  - `SystemExceptionInformation (33)`: exception statistics
  - `SystemRegistryQuotaInformation (37)`: registry size limits
  - `SystemProcessorPerformanceInformation (8)`: per-CPU idle/kernel/user times
  - `SystemBootPerformanceInformation (custom)`: read `ImpossibleBootPerf` NVRAM data -- enables `bootperf` shell command (→ XREF: `TODO-01-kernel-init-sequencing.md §12`)
  - Unimplemented classes return `STATUS_NOT_IMPLEMENTED`
- [ ] `NtSetSystemInformation(SystemInformationClass, Buffer, Length)`:
  - `SystemTimeSlipNotification (46)`: register time slip callback
  - Privileged operation -- requires `SeSystemtimePrivilege` for time-related classes
- [ ] `NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, Buffer, Length)`:
  - `ProcessBasicInformation (0)`: PEB address, PID, parent PID, exit status, affinity mask
  - `ProcessImageFileName (27)`: full path of the executable
  - `ProcessDebugPort (7)`: debug port handle (0 if not being debugged)
  - `ProcessWow64Information (26)`: WoW64 PEB address (NULL for native 64-bit)
  - `ProcessHandleCount (20)`: number of open handles
  - `ProcessSessionInformation (24)`: session ID
  - `ProcessTimes (4)`: creation, exit, kernel, user times
- [ ] Commit: `"kernel: nt -- NtQuerySystemInformation and NtQueryInformationProcess"`

**Test checkpoint:** `NtQuerySystemInformation(SystemBasicInformation)` returns correct page size (4096) and processor count. `NtQueryInformationProcess(ProcessBasicInformation)` returns valid PEB address (0x7FFDE000). `SystemProcessInformation` enumerates all running processes.

## 11. Extended Error Information (IOSB + LastError)
NT propagates detailed error info through two channels: `IO_STATUS_BLOCK` (async I/O) and `TEB->LastErrorValue` (Win32 `GetLastError`). Both must be populated correctly.

- [ ] All file I/O `NtXxx` functions write final `NTSTATUS` into `IoStatusBlock->Status` and byte count / disposition into `IoStatusBlock->Information`
- [ ] On every `NTSTATUS` return from a syscall: if `NT_ERROR(status)`, also write the Win32 error translation into `TEB->LastErrorValue` (at `gs:[0x68]`) using a compact `RtlNtStatusToDosError` table for common codes
- [ ] `RtlNtStatusToDosError` minimal table: `STATUS_ACCESS_DENIED→5`, `STATUS_NO_MEMORY→8`, `STATUS_INVALID_HANDLE→6`, `STATUS_OBJECT_NAME_NOT_FOUND→2`, `STATUS_NOT_IMPLEMENTED→50`, `STATUS_INVALID_PARAMETER→87`, `STATUS_BUFFER_TOO_SMALL→122`, `STATUS_ACCESS_VIOLATION→998`, `STATUS_PRIVILEGE_NOT_HELD→1314`
- [ ] This is the only place `TEB->LastErrorValue` is written by kernel code -- usermode `SetLastError` writes it directly via GS offset without a syscall
- [ ] Commit: `"kernel: nt -- IOSB and TEB LastErrorValue propagation"`

**Test checkpoint:** After a failing `NtOpenFile` (non-existent path), `gs:[0x68]` == Win32 error code (2 = FILE_NOT_FOUND). After `NtReadFile`, IOSB `Status == STATUS_SUCCESS`, `Information == bytes_read`.

## 12. ZwXxx Kernel-Mode Alias Layer

`ZwXxx` names are identical to `NtXxx` in user mode. In kernel mode (`CPL=0`), `ZwXxx` calls bypass the user-mode probe and use kernel-mode access rights directly. This is the convention all of Windows' own drivers and executive components use.

- [ ] Add a `ZwXxx` header `include/kernel/nt/zw.h` that declares each `ZwXxx` as an alias for the same SSDT entry point
- [ ] In the SSDT dispatcher: if caller is CPL=0, skip user-buffer probe and pointer validation; if CPL=3, validate all user-space buffer pointers before use (probe for read/write)
- [ ] Add `ProbeForRead(Address, Length, Alignment)` and `ProbeForWrite(Address, Length, Alignment)` helper functions -- verify address range is user-mode accessible
- [ ] Add a `ASSERT_KERNEL_CALLER()` macro that fires `STATUS_PRIVILEGE_NOT_HELD` if a kernel-only API is called from user mode
- [ ] Document the convention: kernel components call `Zw` variants; user-mode calls `Nt` variants; both resolve to the same implementation, differentiated only by CPL check
- [ ] Commit: `"kernel: nt -- ZwXxx kernel-mode alias layer with CPL probe bypass"`

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
- [ ] Commit: `"kernel: nt -- file metadata, device control, I/O completion ports"`

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
- [ ] Commit: `"kernel: nt -- registry syscalls wired to SSDT"`

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
- [ ] Commit: `"kernel: nt -- token and access control syscalls"`

**Test checkpoint:** `NtOpenProcessToken` returns valid token handle. `NtQueryInformationToken(TokenUser)` returns correct SID. `NtAccessCheck` against an object with DACL returns correct granted access. `NtAdjustPrivilegesToken` enables/disables a privilege.

## 16. Directory and Symbolic Link Object Syscalls
Namespace manipulation -- create, open, and query Ob directory objects and symbolic links from user mode.

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
- [ ] Commit: `"kernel: nt -- directory, symbolic link, and object management syscalls"`

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
- [ ] Commit: `"kernel: nt -- section and memory-mapped file syscalls"`

**Test checkpoint:** `NtCreateSection` with `SEC_COMMIT` creates pagefile-backed section. `NtMapViewOfSection` maps into current process; write/read round-trip. `NtUnmapViewOfSection` unmaps. File-backed section maps file contents correctly.

## 18. Timer Control Syscalls

> [!NOTE]
> Timer object type implemented in TODO-03 (`ob_timer.c`). Time source APIs (NtQuerySystemTime, etc.) implemented in TODO-07 §9; timer resolution APIs in TODO-07 §8. This section provides the full timer control surface and SSDT wiring.

- [ ] `NtCreateTimer(TimerHandle, DesiredAccess, ObjectAttributes, TimerType)` → SSDT 0x007E:
  - `TimerType`: `NotificationTimer (0)` = manual-reset, `SynchronizationTimer (1)` = auto-reset
- [ ] `NtOpenTimer(TimerHandle, DesiredAccess, ObjectAttributes)` → SSDT 0x007F
- [ ] `NtSetTimer(TimerHandle, DueTime, TimerApcRoutine, TimerContext, ResumeTimer, Period, PreviousState)` → SSDT 0x0080:
  - `DueTime`: negative = relative (100ns units), positive = absolute FILETIME
  - `Period`: 0 = one-shot, >0 = periodic (milliseconds)
- [ ] `NtCancelTimer(TimerHandle, CurrentState)` → SSDT 0x0081
- [ ] `NtQueryTimer(TimerHandle, TimerInformationClass, Buffer, Length, ReturnLength)` → SSDT 0x0082
- [ ] `NtSetTimerEx(TimerHandle, TimerSetInformationClass, Buffer, Length)` → SSDT 0x0083
- [ ] `NtQuerySystemTime(SystemTime)` → SSDT 0x00F0 (→ XREF TODO-07 §9)
- [ ] `NtSetSystemTime(SystemTime, PreviousTime)` → SSDT 0x00F1 (→ XREF TODO-07 §9)
- [ ] `NtQueryPerformanceCounter(PerformanceCounter, PerformanceFrequency)` → SSDT 0x00F2 (→ XREF TODO-07 §9)
- [ ] `NtQueryTimerResolution(MaximumTime, MinimumTime, CurrentTime)` → SSDT 0x00F3 (→ XREF TODO-07 §8)
- [ ] `NtSetTimerResolution(DesiredTime, SetResolution, ActualTime)` → SSDT 0x00F4 (→ XREF TODO-07 §8)
- [ ] Commit: `"kernel: nt -- timer control and time query syscalls"`

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
- [ ] Commit: `"kernel: nt -- ALPC and LPC port syscalls wired to SSDT"`

**Test checkpoint:** LPC `NtCreatePort` + `NtConnectPort` + `NtRequestWaitReplyPort` message round-trip. ALPC `NtAlpcCreatePort` + `NtAlpcConnectPort` + `NtAlpcSendWaitReceivePort` round-trip. Port visible in `\RPC Control\` namespace.

## 20. Exception and Debug Syscalls

> [!NOTE]
> Exception dispatch implemented in TODO-10-exception-dispatch-seh.md §5. Debug infrastructure in TODO-18-kernel-debugger-kd-protocol.md. This section reserves SSDT indices and defines the NT-compatible signatures.

- [ ] `NtRaiseException(ExceptionRecord, ContextRecord, FirstChance)` → SSDT 0x0130 (→ XREF TODO-10 §5):
  - Delivers exception to the structured exception handler chain
- [ ] `NtContinue(ContextRecord, RaiseAlert)` → SSDT 0x0131 (→ XREF TODO-10 §5):
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
- [ ] Commit: `"kernel: nt -- power and system control syscalls"`

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
- [ ] Commit: `"kernel: nt -- atom table, locale, environment, misc syscalls"`

**Test checkpoint:** `NtAddAtom("TestAtom")` returns atom ID > 0. `NtFindAtom("TestAtom")` returns same ID. `NtDeleteAtom` removes it; subsequent `NtFindAtom` returns `STATUS_OBJECT_NAME_NOT_FOUND`. `NtQueryDefaultLocale` returns valid LCID.

## 23. Syscall Audit and Tracing Hook

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

## 24. Per-Process Syscall Filtering

> [!NOTE]
> → XREF: `TODO-09-process-model-extensions.md §12` -- scope overlap: TODO-09 §12 adds pledge/unveil-style category-based restriction (`NtPledge`/`NtUnveil`). This section adds per-index bitmap filtering. Both run in the SSDT dispatcher; bitmap filter runs FIRST (per-index), then pledge category check. Both must pass for the syscall to proceed.

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

## 25. Kernel-to-User Mode Callback Dispatch
Windows NT allows the kernel to call user-mode functions (window procedures, clipboard callbacks, hooks) via `KeUserModeCallback`. The kernel pushes a callback frame, returns to user mode at `KiUserCallbackDispatcher` in ntdll, which indexes the `PEB.KernelCallbackTable` array and calls the registered function. The user-mode function then calls `NtCallbackReturn` (SSDT 0x0300) to return the result to the kernel. This mechanism is critical for Win32k -- every `DispatchMessage` / `SendMessage` uses it.

- [ ] Add `PEB.KernelCallbackTable` field at offset 0x058 (Windows x64 layout) -- pointer to an array of callback function pointers, populated by ntdll/user32 init
- [ ] Implement `KeUserModeCallback(ApiNumber, InputBuffer, InputLength, OutputBuffer, OutputLength)` in `src/kernel/nt/callback.c`:
  - Save current kernel stack frame (RSP, RBP, return address) in a per-thread callback stack
  - Build a user-mode trap frame pointing to `KiUserCallbackDispatcher` in ntdll
  - Pass `ApiNumber`, `InputBuffer`, `InputLength` on the user stack
  - Return to user mode via `sysret` or `iretq`
  - Block until user mode calls `NtCallbackReturn`
- [ ] Wire `NtCallbackReturn` (SSDT 0x0300, currently in §22 catch-all):
  - Copy `OutputBuffer` / `OutputLength` / `Status` back to the kernel-side `KeUserModeCallback` caller
  - Restore kernel stack frame from the callback stack
  - Resume kernel execution at the point after `KeUserModeCallback` returned
- [ ] Per-thread callback depth counter: limit to `CALLBACK_MAX_DEPTH = 64` to prevent stack exhaustion
- [ ] Re-entrant syscalls: user-mode callback code can itself call syscalls; the SSDT dispatcher must handle nested kernel entry correctly
- [ ] Add `KiUserCallbackDispatcher` export address to ntdll (→ XREF: 12-user-platform-sdk/TODO-04-ntdll-user-runtime.md)
- [ ] Commit: `"kernel: nt -- KeUserModeCallback and kernel-to-user callback dispatch"`

> [!NOTE]
> This mechanism is consumed by Win32k (→ XREF: 08-graphics-ui/TODO-12-win32k-shadow-ssdt.md §8) for `NtUserDispatchMessage` and `NtUserSendMessage`. Until this section is implemented, Win32k cannot call user-mode window procedures.

**Test checkpoint:** Kernel calls `KeUserModeCallback(0, ...)` → user-mode callback fires → `NtCallbackReturn` returns result to kernel. Nested callback (callback calls syscall which calls another callback) succeeds up to depth 64. Depth 65 returns `STATUS_STACK_OVERFLOW`. Callback on terminated thread returns `STATUS_THREAD_IS_TERMINATING`.

## 26. SSDT Integrity Protection

> [!TIP]
> **Impossible OS competitive edge.** Windows uses PatchGuard/KPP -- a complex, opaque system that periodically checksums kernel structures and BSODs on tampering. It's a cat-and-mouse arms race with rootkits. Linux has no SSDT integrity protection at all (`sys_call_table` is `const` but not hardware-enforced). Impossible OS uses hardware write-protection: mark the SSDT pages as read-only via PTE after initialization. Any write attempt triggers a #PF that the kernel catches and escalates to `KeBugCheck(CRITICAL_STRUCTURE_CORRUPTION)`. Zero runtime overhead, no periodic polling, no timing-based detection -- just hardware-enforced immutability.

- [ ] After `ssdt_init()` completes and all 470 handlers are registered, mark SSDT pages as read-only via PTE manipulation (clear R/W bit, flush TLB for affected pages)

> [!NOTE]
> `vmm_protect()` is planned in `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` but does not yet exist. Until it lands, use direct PTE writes: `pte &= ~PTE_WRITE; invlpg(addr)`. This is self-contained -- no external dependency blocks §26.
- [ ] Same for shadow SSDT pages after `win32k_init()` (→ XREF: 08-graphics-ui/TODO-12-win32k-shadow-ssdt.md §1)
- [ ] In the #PF handler: if faulting address is within SSDT page range AND fault was a write, call `KeBugCheck(0x00000109)` -- `CRITICAL_STRUCTURE_CORRUPTION`
- [ ] Provide `ssdt_register_late(index, handler)` for drivers that need to register handlers after init:
  - Temporarily mark SSDT page writable, write the handler, re-mark read-only
  - Requires `SeLoadDriverPrivilege`; logs to klog
- [ ] Compile-time: declare SSDT arrays as `const` where possible; the runtime write-protect is the enforcement layer
- [ ] Commit: `"kernel: nt -- SSDT hardware write-protection (integrity enforcement)"`

**Test checkpoint:** After init, writing to SSDT address triggers #PF → BugCheck. `ssdt_register_late` succeeds with correct privilege. `ssdt_register_late` without privilege returns `STATUS_PRIVILEGE_NOT_HELD`. SSDT dispatch still works normally after write-protect (read-only doesn't block reads).

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
| 💎 | Process/thread create API  | ✅ NtCreate{Process,Thread} | ✅ clone/execve            | ⬜ §7                       |
| 💎 | Thread context get/set     | ✅ NtGet/SetContextThread   | ✅ ptrace GETREGS          | ⬜ §7                       |
| 💎 | Named sync objects         | ✅ NtCreate{Event,Mutant}   | ✅ POSIX sem + futex       | ⬜ §8                       |
| 💎 | Multi-object wait          | ✅ NtWaitForMultipleObj     | ⚠️ No direct equivalent    | ⬜ §8                       |
| 💎 | Keyed events (futex)       | ✅ NtWaitForKeyedEvent      | ✅ futex()                 | ⬜ §8                       |
| 💎 | Virtual memory syscalls    | ✅ NtAllocate/Free/Protect  | ✅ mmap/mprotect/munmap    | ⬜ §9                       |
| 💎 | Cross-process memory       | ✅ NtRead/WriteVirtualMem   | ✅ process_vm_readv        | ⬜ §9                       |
| 💎 | OS info query syscall      | ✅ NtQuerySystemInfo        | ✅ sysinfo + /proc         | ✅ §5 NtQuerySystemInfo 2 classes |
| 💎 | LastError per-thread       | ✅ TEB→LastErrorValue       | ✅ errno via TLS           | ⬜ §11 + TODO-04 §6         |
| 💎 | Registry syscalls          | ✅ NtCreate/Open/QueryKey   | ❌ No equivalent           | ⬜ §14 + TODO-13            |
| 💎 | Token/access control       | ✅ NtAccessCheck + tokens   | ✅ capabilities + DAC/MAC  | ⬜ §15 + TODO-11            |
| 💎 | Namespace dir/symlink      | ✅ NtCreateDirectoryObj     | ❌ No kernel namespace     | ⬜ §16                      |
| 💎 | Memory-mapped sections     | ✅ NtCreateSection/MapView  | ✅ mmap with MAP_SHARED    | 🔄 §5 Create+Map wired      |
| 💎 | Timer objects              | ✅ NtSetTimer periodic      | ✅ timerfd_create          | ⬜ §18 + TODO-07 §8,§9      |
| 💎 | ALPC message ports         | ✅ NtAlpcSendWaitReceive    | ❌ No equivalent           | ⬜ §19 + TODO-12            |
| 💎 | Debug API                  | ✅ NtDebugActiveProcess     | ✅ ptrace                  | ⬜ §20 + TODO-18            |
| 💎 | Power management           | ✅ NtSetSystemPowerState    | ✅ sys_reboot + ACPI       | 🔄 §5 NtShutdownSystem wired |
| 💎 | Atom table                 | ✅ NtAddAtom/FindAtom       | ❌ No equivalent           | ⬜ §22                      |
| ⭐ | ZwXxx CPL-gated aliases    | ✅ Internal, undocumented   | ❌ No equivalent           | ⬜ §12 -- explicit, public   |
| ⭐ | Stable native API contract | ⚠️ Undocumented             | ❌ No stable native API    | ⬜ §4+§12 -- numbered+public |
| ⭐ | Syscall audit hook         | ⚠️ ETW, heavyweight         | ⚠️ seccomp-bpf, complex    | ⬜ §23 -- first-class API    |
| 💎 | Per-process syscall filter | ✅ SystemCallDisablePolicy  | ✅ seccomp-bpf + Landlock  | ⬜ §24 -- bitmap + BPF       |
| 💎 | Kernel→user callbacks      | ✅ KeUserModeCallback       | ⚠️ Signals only            | ⬜ §25                      |
| ⭐ | SSDT integrity protection  | ⚠️ PatchGuard (periodic)    | ❌ No protection           | ⬜ §26 -- HW write-protect   |

> **After §1–§22:** Impossible OS has complete NT native API coverage -- 470 syscall endpoints across file I/O, process/thread, memory, sync, registry, security, sections, timers, ALPC, debug, power, namespace, atoms, and system info. Real `ntdll.dll` stubs can call into the kernel.
> **§12** makes the `ZwXxx` layer an explicit, documented public contract -- Windows keeps it internal/undocumented and Linux has no equivalent.
> **§23** provides first-class syscall auditing -- no ETW complexity, no BPF programs, just a kernel callback with near-zero idle overhead.
> **§24** closes the per-process syscall filtering parity gap -- both Win11 and Linux restrict per-process syscall access; Impossible OS uses a fast bitmap with optional BPF programs.
> **§25** enables kernel→user callbacks required by Win32k for window procedure dispatch -- without it, `SendMessage` and `DispatchMessage` cannot work.
> **§26** provides hardware-enforced SSDT immutability -- simpler and more secure than PatchGuard's periodic checksums.

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
  - **Token (§15):**
    - `NtOpenProcessToken` on current process returns valid handle
  - **Audit hook (§23):**
    - Register pre-call hook; verify it fires on `NtClose`
    - Unregister hook; verify no more callbacks
  - **Syscall filter (§24):**
    - Set filter blocking `NtWriteFile` on child; child's `NtWriteFile` returns `STATUS_ACCESS_DENIED`
    - Parent's `NtWriteFile` still works (filter is per-process)
    - `DisallowWin32kSystemCalls` blocks shadow SSDT calls
    - Locked filter (`SYSCALL_FILTER_LOCKED`) cannot be relaxed
  - **Kernel→user callback (§25):**
    - `KeUserModeCallback(0, ...)` fires user-mode callback; `NtCallbackReturn` returns result
    - Nested callback (depth 2) succeeds; depth > 64 returns `STATUS_STACK_OVERFLOW`
  - **SSDT integrity (§26):**
    - Write to SSDT page after init triggers BugCheck
    - `ssdt_register_late` with correct privilege succeeds
    - `ssdt_register_late` without privilege returns `STATUS_PRIVILEGE_NOT_HELD`
- [ ] Register in `test_runner_init()`: `test_register_nt_api()`
- [ ] Commit: `"test: add native API layer test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: Phase 1 shows `"syscall: fast path (SYSCALL/SYSRET) enabled"`
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
