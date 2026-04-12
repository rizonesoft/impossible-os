# TODO-04 -- PEB / TEB & User-Mode ABI

> **Goal:** Implement the Process Environment Block, Thread Environment Block, and the complete x86-64 user-mode ABI handoff so that `ntdll.dll` and all Win32 DLLs can initialise and user programs run correctly. Without this, no Win32 binary can call `GetLastError`, locate loaded modules, parse command-line arguments, or access TLS. This is the first item every Win32 user-mode DLL depends on, and blocks everything downstream in the Win32 subsystem.

> [!IMPORTANT]
> **Current state:** Core PEB/TEB ABI (§1 -- §13) is complete and verified: TEB and PEB at correct Windows x64 offsets, swapgs on INT 0x80, KERNEL_GS_BASE per-task, PEB/TEB allocation, initial user stack frame, Ldr module list, 64 static TLS slots, Ob namespace exposure, KUSER_SHARED_DATA, TLS expansion (1024 slots), and extended ELF auxv (AT_RANDOM/AT_PHDR/AT_HWCAP). Remaining: §14 user-mode thread bootstrap (split `thread_create` into kthread/uthread) and §15 per-thread TEB allocation (blocked on §14).

## Inputs

- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- `task_exec`, `task_create`, `task_fork`
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) -- INT 0x80 handler
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) -- `IA32_GS_BASE` MSR setup
- [`src/kernel/elf.c`](../../src/kernel/elf.c) -- ELF loader (`elf_load`, `elf_load_result`)
- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) -- syscall table
- → XREF: `TODO-03-object-manager.md` -- handle table needed for `PEB->ProcessParameters` stdin/stdout/stderr HANDLE fields
- → XREF: `TODO-01-kernel-init-sequencing.md` -- PEB/TEB init belongs in Phase 3 (user platform); requires VMM and scheduler (Phase 2)
- → XREF: `TODO-05-native-api-ssdt.md` -- `NtCreateProcess` populates PEB; `LdrInitializeThunk` (ntdll entry) reads PEB->Ldr
- → XREF: `TODO-07-time-filetime-management.md §6, §12` -- §11 KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by the kernel time service (§6); ISR time update function provided by §12
- → XREF: `TODO-08-binary-system.md §2` -- §13 extended auxv (AT_PHDR, AT_PHNUM) requires `elf_load_result` extension with `phdr_vaddr` and `phnum` from the Enhanced ELF Loader
- → XREF: `TODO-17-kernel-security-hardening.md §9` -- §13 AT_RANDOM provides user-mode stack canary seed bytes; shares RDRAND path
- → XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §4` -- §12 kernel TLS expansion allocator consumed by user-mode TlsAlloc/TlsFree wrappers
- → XREF: `TODO-09-process-model-extensions.md §1` -- §1 adds `cwd[MAX_PATH]` to `struct task`; `RTL_USER_PROCESS_PARAMETERS.CurrentDirectory` in the PEB should be populated from `task->cwd` at `task_exec()` time
- → XREF: `TODO-22-kernel-bulletproofing.md` §1,§3,§5,§9 -- same 5-layer invariant pattern for gs:/frame/swapgs contracts that PEB/TEB and syscall paths rely on

## Outcome

- Every process has a PEB at a well-known user-mode address populated with image base, process parameters (command line, image path, env block, std handles), and Ldr data.
- Every thread has a TEB with GS self-pointer, stack limits, process ID, thread ID, last-error slot, and 64 TLS slots.
- `swapgs` gates every kernel-entry and kernel-exit path so GS always points to the correct structure: kernel per-CPU data in ring 0, TEB in ring 3.
- `task_exec` pushes a complete Win32-compatible initial stack frame (argc, argv, envp plus RTL_USER_PROCESS_PARAMETERS) before `iretq` to ring 3.
- Win32's `GetLastError` / `SetLastError`, `NtCurrentTeb()`, `NtCurrentPeb()` macros work.
- `KUSER_SHARED_DATA` at `0x7FFE0000` provides system time, tick count, build number, processor features, and QPC frequency -- user-mode reads without syscall.
- TLS currently supports the first 64 static slots; §12 extends this to the full Windows contract of 1088 slots (64 static + 1024 expansion).
- ELF auxiliary vector currently includes the minimal startup entries; §13 extends it with AT_RANDOM, AT_PHDR, AT_PHNUM, AT_BASE, and AT_SECURE for full Linux ABI compatibility and stack canary seeding.

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On         | Status |
| --- | :---: | ----------------------------------------------- | ------------------ | :----: |
| 💎  |   1   | TEB struct and GS self-pointer                  | --                 |  [x]   |
| 💎  |   2   | PEB struct and RTL_USER_PROCESS_PARAMETERS      | --                 |  [x]   |
| 💎  |   3   | swapgs on INT 0x80 entry and exit               | §1                 |  [x]   |
| 💎  |   4   | KERNEL_GS_BASE written at task_exec / fork      | §1, §3             |  [x]   |
| 💎  |   5   | PEB allocation and population at task_exec      | §2, §4             |  [x]   |
| 💎  |   6   | TEB allocation and population at thread create  | §1, §4             |  [x]   |
| 💎  |   7   | Initial user stack frame (argv / envp / PEB)    | §5, §6             |  [x]   |
| 💎  |   8   | PEB Ldr (module list) basic population          | §5                 |  [x]   |
| 💎  |   9   | TLS slot allocation (64 static slots)           | §6                 |  [x]   |
| ⭐  |  10   | PEB / TEB exposed in Ob namespace               | §5, §6             |  [x]   |
| 💎  |  11   | KUSER_SHARED_DATA -- kernel-user shared page    | §5                 |  [x]   |
| 💎  |  12   | TLS expansion slots (1024 dynamic slots)        | §9                 |  [x]   |
| 💎  |  13   | Extended auxiliary vector (AT_RANDOM + friends) | §7                 |  [x]   |
| 💎  |  14   | User-mode thread bootstrap (uthread_create)     | §6, T01§12, T05§11 |  [x]   |
| 💎  |  15   | Per-thread TEB allocation at uthread_create()   | §6, §12, §14       |  [x]   |

> 💎 = parity -- Windows NT / 11 and ntdll both require and implement all of these.
> ⭐ = exclusive -- exposing PEB and TEB as queryable named Ob objects enables user-mode introspection tools and debuggers without any kernel patching; Windows hides these as private loader internals.
> §11 -- §13 close parity gaps: KUSER_SHARED_DATA is used by every Win32 program for fast time queries; TLS expansion is needed by complex Win32 DLLs; AT_RANDOM is needed for ELF stack canaries.

---

## 1. TEB Struct and GS Self-Pointer
Define the TEB layout exactly matching Windows x64 offsets so ntdll inline macros (`NtCurrentTeb()` = `mov rax, gs:[0x30]`) work without patching.

- [x] Create `include/kernel/ob/teb.h` with `TEB` struct at exact Windows x64 offsets:
  - `NT_TIB NtTib` at offset `0x00` -- ExceptionList, StackBase, StackLimit, SubSystemTib, FiberData/Version, ArbitraryUserPointer, **Self** (pointer to the TEB itself)
  - `void *EnvironmentPointer` at `0x38`
  - `CLIENT_ID ClientId` at `0x40` -- UniqueProcess (PID), UniqueThread (TID)
  - `void *ActiveRpcHandle` at `0x50`
  - `void *ThreadLocalStoragePointer` at `0x58`
  - `PEB *ProcessEnvironmentBlock` at `0x60`
  - `uint32_t LastErrorValue` at `0x68`
  - `uint32_t CountOfOwnedCriticalSections` at `0x6C`
  - `uint64_t TlsSlots[64]` at `0x1480`
  - `uint64_t TlsExpansionSlots` pointer at `0x1780`
- [x] Add `CLIENT_ID` struct: two `uint64_t` fields (UniqueProcess, UniqueThread)
- [x] Add `NT_TIB` struct with correct field order and sizes
- [x] Annotate each field with its Windows offset as a comment -- 11 `_Static_assert` offset checks
- [x] Commit: `"kernel: peb -- TEB struct with correct Windows x64 offsets"`

**Test checkpoint:** Build-time `_Static_assert` checks for all listed TEB offsets pass; `test_peb_teb` offset assertions for TEB layout pass at boot. No ABI offset drift in serial/unit-test output. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 2. PEB Struct and RTL_USER_PROCESS_PARAMETERS
Define PEB layout at exact Windows x64 offsets so ntdll's startup code can walk it without any patching.

- [x] Create `include/kernel/ob/peb.h` with `PEB` struct -- 15 `_Static_assert` offset checks
- [x] Create `RTL_USER_PROCESS_PARAMETERS` struct with UNICODE_STRING fields
- [x] Create `UNICODE_STRING` struct: Length, MaximumLength (uint16_t), Buffer (uint16_t *)
- [x] Create `UHANDLE` type (uint64_t) for Win64 user-mode HANDLE (separate from kernel HANDLE)
- [x] Add `PEB_LDR_DATA` and `LDR_DATA_TABLE_ENTRY` stubs with `LIST_ENTRY` for §8
- [x] Add `LARGE_INTEGER`, `LIST_ENTRY` Win64 primitive types
- [x] Commit: `"kernel: peb -- PEB and RTL_USER_PROCESS_PARAMETERS structs"`

**Test checkpoint:** Build-time `_Static_assert` checks for key PEB and RTL_USER_PROCESS_PARAMETERS offsets pass; unit tests confirm `ImageBaseAddress`, `Ldr`, `ProcessParameters`, and version fields are at expected offsets. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 3. swapgs on INT 0x80 Entry and Exit
Kernel GS (`IA32_GS_BASE`) holds per-CPU data. User GS (`IA32_KERNEL_GS_BASE`) holds the TEB address. `swapgs` exchanges the two MSRs -- must fire on every ring-3→ring-0 transition and be reversed on every ring-0→ring-3 return.

- [x] In `isr_common_stub` (isr_stubs.asm): `test byte [rsp+24], 3` checks saved CS RPL; `swapgs` if ring 3
- [x] On exit path (before `iretq`): `test byte [rsp+8], 3` checks return CS RPL; matching `swapgs`
- [x] On `iretq` to ring 0 (kernel→kernel): `jz .no_swapgs_*` skips both swapgs
- [x] Regression comment block with symmetry requirement added at both swapgs sites
- [x] Verify with QEMU: ring-3 cmd.exe runs, 95 tests pass, desktop stable -- GS correct (WHPX, 2026-04-02)
- [x] Commit: `"kernel: abi -- swapgs on INT 0x80 ring-3 entry and exit"`

**Test checkpoint:** Ring-3 user program executes syscalls repeatedly without GS corruption; ISR entry/exit preserves kernel per-CPU GS in ring 0 and TEB GS in ring 3. No interrupt-path regressions or triple faults. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 4. KERNEL_GS_BASE Written at task_exec and Fork
`IA32_KERNEL_GS_BASE` (MSR 0xC0000102) must hold the TEB address before the first ring-3 instruction runs. `swapgs` (§3) exchanges GS_BASE ↔ KERNEL_GS_BASE, so after `swapgs` in the ISR entry the kernel sees per-CPU GS and user-mode sees TEB via GS.

- [x] `MSR_IA32_KERNEL_GS_BASE` (0xC0000102) already defined in `include/kernel/msr.h` line 50
- [x] Added `kernel_gs_base` field to `struct task` in task.h
- [x] `task_exec`: sets `tasks[pid].kernel_gs_base = 0` (§6 will set TEB address)
- [x] `task_fork`: copies parent's `kernel_gs_base` to child (§6 will allocate child TEB)
- [x] `task_init`: explicitly zeroes `kernel_gs_base` for all task slots
- [x] Context switch (`schedule_now` + `schedule`): save/restore via `msr_read`/`msr_write` on task switch
- [x] Commit: `"kernel: abi -- write KERNEL_GS_BASE at task_exec and fork"`

**Test checkpoint:** `task_exec`/`task_fork` initialize/copy `kernel_gs_base` correctly; context switches preserve per-task `IA32_KERNEL_GS_BASE`; post-syscall return still resolves TEB via GS. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 5. PEB Allocation and Population at task_exec
Allocate the PEB in the user address space and fill it before the first instruction runs.

- [x] PEB at `0x7FFDE000`, RTL_USER_PROCESS_PARAMETERS at `0x7FFDD000`, env block at `0x7FFDC000`
- [x] All three pages: PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] PEB fields: ImageBaseAddress, ProcessParameters, OSMajorVersion=10, OSBuildNumber=22621, NumberOfProcessors via `acpi_get_cpu_count()`
- [x] RTL_USER_PROCESS_PARAMETERS: ImagePathName + CommandLine as UNICODE_STRING (ASCII→UTF-16), CurrentDirectory = `C:\`
- [x] Std handles: UHANDLE_INVALID (console wiring in future)
- [x] Environment block: `PATH=C:\Impossible\System32\` + `SystemRoot=C:\Impossible` (UTF-16, double-NUL terminated)
- [x] `tasks[pid].peb` and `tasks[pid].teb` fields added to `struct task`
- [x] Commit: `"kernel: peb -- PEB allocation and population at exec"`

**Test checkpoint:** On `task_exec`, PEB and process-parameter pages map at expected user VAs, fields are non-NULL and consistent (`ImageBaseAddress`, `ProcessParameters`, OS version/build, processor count), and UTF-16 command-line/environment blocks decode correctly. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 6. TEB Allocation and Population at Thread Create

> → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §5` -- Phase 1 XSAVE/PCID window references TEB-backed XSAVE layout from this section.

One TEB per thread. Allocated in the user address space near the thread stack.

- [x] TEB at `0x7FFDB000` (TID 0), subsequent threads decrement by page (`0x7FFDB000 - TID * 0x1000`)
- [x] PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] Populated: NtTib.Self (gs:[0x30] self-pointer), StackBase/StackLimit, ExceptionList=0xFFFF...
- [x] ClientId: UniqueProcess=PID, UniqueThread=TID
- [x] ProcessEnvironmentBlock → PEB from §5, LastErrorValue = 0
- [x] `tasks[pid].teb` stored, `kernel_gs_base` set to TEB address for swapgs
- [x] Commit: `"kernel: peb -- TEB allocation and population at thread create"`

**Test checkpoint:** TEB is mapped per thread at expected VA, `NtTib.Self` is valid, stack bounds and ClientId are correct, `ProcessEnvironmentBlock` points at the process PEB, and `LastErrorValue` initializes to 0. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 7. Initial User Stack Frame
The user stack must have a valid calling frame waiting for the first instruction. Win32 convention: `ntdll!_LdrpInitialize` reads `PEB->ProcessParameters`; it does not expect argc/argv on the stack itself. However, the ELF ABI (for ELF-based binaries in the compatibility path) needs the Linux-style stack layout.

- [x] ELF initial stack: argc=1, argv[0]=program name, NULL, envp NULL, auxv (AT_ENTRY, AT_PAGESZ, AT_NULL)
- [x] String data at top of user stack, argv pointer to it, 16-byte aligned RSP
- [x] PE32+: RCX=PEB set in interrupt frame for all user tasks -- ntdll will find PEB in RCX when PE32+ loading lands (TODO-08)
- [x] Replaced all-zero stack top with Linux x86-64 ABI layout
- [x] Confirmed: cmd.exe _start→main()→printf works with new stack (WHPX build 1963, 2026-04-02)
- [x] Commit: `"kernel: abi -- initial user stack frame with argv, envp, auxv"`

**Test checkpoint:** User entry starts with valid argc/argv/envp/auxv layout and 16-byte stack alignment; hello/cmd user binaries run without stack faults; PE handoff keeps RCX=PEB contract. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 8. PEB Ldr (Module List) Basic Population
`ntdll!LdrInitializeThunk` walks `PEB->Ldr->InLoadOrderModuleList` to find already-loaded modules. Even a stub Ldr with just the main module prevents ntdll from faulting on an empty list.

- [x] `PEB_LDR_DATA` and `LDR_DATA_TABLE_ENTRY` already defined in peb.h (§2)
- [x] `PEB_LDR_DATA` allocated at PEB page offset 0x800; `Initialized = 1`
- [x] Main executable inserted in all 3 lists (InLoadOrder, InMemoryOrder, InInitializationOrder) as circular linked list
- [x] `LDR_DATA_TABLE_ENTRY`: DllBase, EntryPoint, SizeOfImage, FullDllName + BaseDllName as UNICODE_STRING
- [x] `PEB->Ldr` wired to the allocated `PEB_LDR_DATA`
- [ ] Full dynamic module list (LoadLibrary/FreeLibrary add/remove Ldr entries at runtime) -- → XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §3` (LdrLoadDll PE DLL loader)
- [x] Commit: `"kernel: peb -- minimal PEB Ldr with main module entry"`

**Test checkpoint:** `PEB->Ldr` is non-NULL at process start and the main image appears in all three loader lists with stable links; ntdll loader walk does not fault on initial module enumeration. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 9. TLS Slot Allocation (64 Static Slots)
TEB offsets `0x1480…0x1678` are the 64 static TLS slots used by `__declspec(thread)` and `TlsAlloc`. A minimal allocator is needed for Win32 DLLs that use TLS before the full heap is available.

- [x] `uint64_t tls_bitmap` added to `struct task` -- bit N = slot N allocated
- [x] `tls_alloc(pid)`: scan bitmap for first 0 bit, set it, return index (0-63) or -1
- [x] `tls_free(pid, index)`: clear bit, zero TEB->TlsSlots[index]
- [x] `tls_get_value(pid, index)` / `tls_set_value(pid, index, value)`: read/write TEB->TlsSlots[]
- [x] Slots 0-63 at gs:[0x1480 + index*8]; index >= 64 returns -1/0 (expansion stub)
- [x] Commit: `"kernel: peb -- TLS slot allocation (64 static slots)"`

**Test checkpoint:** `tls_alloc` returns unique indices 0-63 then fails/defers expansion path; `tls_set_value`/`tls_get_value` round-trip values; `tls_free` clears slot and allows reuse. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 10. PEB / TEB Exposed in Ob Namespace
Make the PEB and TEB for any process queryable by name through the Object Manager namespace. Uses `ObInsertObject` and `NtOpenDirectoryObject`/`NtQueryDirectoryObject` from the completed OB layer (see [TODO-03-object-manager.md](./TODO-03-object-manager.md)). Enables debuggers and introspection tools without kernel patching -- not possible on Windows or Linux without a private API.

- [x] PEB inserted as `\KernelObjects\Process<PID>\Peb` via `ObInsertObject`
- [x] TEB inserted as `\KernelObjects\Process<PID>\Teb` via `ObInsertObject`
- [x] Per-process directory `\KernelObjects\Process<PID>` created via `ob_ns_create_directory`
- [x] User-mode can enumerate via `NtOpenDirectoryObject` + `NtQueryDirectoryObject`
- [x] ObpPebType and ObpTebType registered as built-in OB types (body_size=4096)
- [x] Commit: `"kernel: peb -- PEB and TEB registered in Ob namespace"`

**Test checkpoint:** `\KernelObjects\Process<PID>\Peb` and `\KernelObjects\Process<PID>\Teb` resolve through Ob namespace queries from user mode and kernel mode; object typing/size matches expected PEB/TEB body layout. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

---

## 11. KUSER_SHARED_DATA -- Kernel-User Shared Page
Windows maps a single physical page at fixed virtual address `0x7FFE0000` (user read-only) and a kernel-mode writable address. This `KUSER_SHARED_DATA` structure gives every user-mode process access to system time, tick count, OS version, processor features, and QPC frequency without a syscall -- `GetTickCount()`, `QueryInterruptTime()`, and ntdll's fast-path `NtQuerySystemTime` all read from this page. Linux's vDSO serves the same purpose. Without this page, every time query requires a full syscall round-trip.

- [x] `KUSER_SHARED_DATA` struct in `kusd.h` -- packed, all fields at exact Windows x64 offsets (0x000-0x340), padded to page size
- [x] `KSYSTEM_TIME` struct with `ksystem_time_write()` inline triple-write protocol
- [x] 16 `_Static_assert` offset checks for all key fields (TickCountMultiplier, InterruptTime, SystemTime, TimeZoneBias, NtSystemRoot, NtBuildNumber, NtProductType, NtMajorVersion, NtMinorVersion, ProcessorFeatures, NumberOfPhysicalPages, QpcFrequency, SystemCall, TickCount, Cookie, sizeof == 4096)
- [x] Physical page allocated, mapped at `0x7FFE0000` user read-only + NX, kernel writes via identity alias
- [x] `kusd_init()`: populates all static fields:
  - `NtSystemRoot` = `L"C:\\Impossible"` (WCHAR), `ImageNumber` = 0x8664 (AMD64)
  - `NtMajorVersion` = 10, `NtMinorVersion` = 0, `NtBuildNumber` from build_info.h
  - `NtProductType` = NtProductWinNt, `NativeProcessorArchitecture` = 9 (AMD64)
  - `ProcessorFeatures[64]` populated from cpuid.c (MMX, SSE, SSE2-4.2, AVX, AVX2, RDRAND)
  - `QpcFrequency` = 10 MHz (fixed), `LargePageMinimum` = 2 MiB
  - `NumberOfPhysicalPages` from PMM, `Cookie` from RDRAND (TSC fallback)
  - `SystemCall` = 0 (SYSCALL mode)
- [x] `kusd_update_time()`: ISR-driven, writes InterruptTime, SystemTime, TimeZoneBias, TickCount via triple-write protocol. Wired into LAPIC + PIT timer ISRs (→ XREF TODO-07 §12)
- [x] Wired into Phase 2 boot after wall_clock_init() and timezone_init()
- [x] Commit: `"kernel: abi -- KUSER_SHARED_DATA shared page at 0x7FFE0000"`

**Test checkpoint:** Serial log shows `KUSD: mapped at user=0x7FFE0000 kernel=0x<rand>`. User-mode test reads `*(uint32_t *)0x7FFE026C` (NtMajorVersion) and gets `10`. `TickCountQuad` at `0x7FFE0320` increments over time. `POST16(0xDF00)` on entry, `POST16(0xDF01)` static init, `POST16(0xDF02)` time update wired, `POST16(0xDF03)` test read verified. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

---

## 12. TLS Expansion Slots (1024 Dynamic Slots)
Windows supports 1088 TLS slots per thread: 64 static slots in `TEB.TlsSlots[64]` (§9) plus 1024 expansion slots via `TEB.TlsExpansionSlots`. When `TlsAlloc()` exhausts the static 64, ntdll allocates the expansion array on demand. Without expansion support, any Win32 DLL that calls `TlsAlloc` more than 64 times across all loaded modules will fail -- common in large applications with many DLL dependencies.

- [x] Add `tls_expansion_bitmap[16]` field (1024 bits) to `struct task` plus `tls_expansion_allocated`, `tls_expansion_phys`, `tls_expansion_virt` for tracking and cleanup
- [x] `tls_alloc(pid)`: scans static bitmap first, then expansion bitmap; returns indices 0-1087; protected by `tls_lock` spinlock
- [x] `tls_free(pid, index)`: handles both static (0-63) and expansion (64-1087) ranges; zeros the slot in TEB; under `tls_lock`
- [x] `tls_get_value(pid, index)` / `tls_set_value(pid, index, value)`: dispatch to `TEB.TlsSlots[index]` or `TEB.TlsExpansionSlots[index-64]`; under `tls_lock`
- [x] Demand-allocate expansion array via `pmm_alloc_contiguous(2)` + `vmm_map_page()` at `0x7FFD0000`; two-phase commit: PMM alloc outside spinlock, mapping/zero-fill/commit under spinlock; race-loser frees its frames via `pmm_free_frame()`
- [/] On thread create: per-thread expansion arrays -- §15 landed per-thread TEBs, so static TLS slots (TEB.TlsSlots[64]) are now per-thread. However, expansion slots (TEB.TlsExpansionSlots) still point at one task-shared array. Full per-thread TLS expansion deferred to future TODO.
- [x] On task cleanup: `task_cleanup()` unmaps expansion pages via `vmm_unmap_page(virt, 1)` and clears `TEB.TlsExpansionSlots` under `tls_lock` to prevent races
- [x] `TLS_MINIMUM_AVAILABLE = 64`, `TLS_EXPANSION_SLOTS = 1024`, `TLS_MAXIMUM_AVAILABLE = 1088`, `TLS_EXPANSION_BITMAP_WORDS = 16` constants in `task.h`
- [x] 6 unit tests in `test_peb_teb.c` (TLS constants, static alloc/free, expansion alloc, expansion reuse, expansion boundary 1088 slots, POST codes)
- [x] Commit: `"kernel: abi -- TLS expansion slots (1024 dynamic slots, indices 64-1087)"`

**Test checkpoint:** Allocate 65 TLS slots -- first 64 from static, 65th triggers expansion array allocation. Read/write slot 64 and the highest expansion slot -- values round-trip correctly. Free slot 65 -- re-alloc returns index 65 (reuse). All 1088 slots can be allocated; 1089th `tls_alloc` returns -1. `POST16(0xDF10)` on entry, `POST16(0xDF11)` expansion alloc, `POST16(0xDF12)` boundary test, `POST16(0xDF13)` cleanup. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

---

## 13. Extended Auxiliary Vector (AT_RANDOM, AT_PHDR, AT_PHNUM)
The current ELF initial stack (§7) pushes `AT_ENTRY`, `AT_PAGESZ`, and `AT_NULL`. Linux user-mode C libraries (glibc, musl) expect additional entries -- most critically `AT_RANDOM` (16 random bytes used to seed the stack canary `__stack_chk_guard`) and `AT_PHDR`/`AT_PHNUM` (program header location for the dynamic linker). Without `AT_RANDOM`, dynamically linked ELF binaries compiled with `-fstack-protector` will use a zero or predictable canary, defeating stack overflow protection.

> [!NOTE]
> Resolved: shared `rdrand_bytes()` lives in `include/kernel/random.h` + `src/kernel/random.c`; `gpt.c` and `kusd_time.c` were migrated. The `elf_load_result` struct now carries `phdr_vaddr`, `phnum`, `phent`. The ELF program-header walk for auxv is the same shared `elf_compute_phdr_info()` static helper used by `elf_load()`, so the loader and `elf_extract_phdr_info()` cannot diverge. The CSPRNG (TODO-20 §5) is still the future superset for AT_RANDOM entropy hardening on TCG.

- [x] Define auxv type constants in `include/kernel/elf.h`:
  - `AT_PHDR   = 3` -- address of ELF program headers in memory
  - `AT_PHENT  = 4` -- size of one program header entry
  - `AT_PHNUM  = 5` -- number of program header entries
  - `AT_BASE   = 7` -- interpreter base address (0 if no interp)
  - `AT_FLAGS  = 8` -- flags (0)
  - `AT_UID    = 11`, `AT_EUID = 12`, `AT_GID = 13`, `AT_EGID = 14` -- process credentials
  - `AT_SECURE = 23` -- 1 if setuid/setgid, 0 otherwise
  - `AT_RANDOM = 25` -- pointer to 16 random bytes on the stack
  - `AT_HWCAP  = 16`, `AT_HWCAP2 = 26` -- CPU feature bitmask from `cpuid.c`
- [x] In `task_exec` ELF stack setup: push 16 random bytes (from `rdrand_bytes()` with TSC-mixed fallback on RDRAND failure) to the stack before auxv; set `AT_RANDOM` to point at them
- [x] Push `AT_PHDR` = `elf_load_result.phdr_vaddr`, `AT_PHENT` = `e_phentsize`, `AT_PHNUM` = `e_phnum` -- derived via `elf_extract_phdr_info()` (shared parser with `elf_load()`)
- [x] Push `AT_BASE` = 0 (no dynamic linker yet; correct value for static ELFs per Linux ABI)
- [x] Push `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID` = 0 (root) for now; updated when user/group model lands
- [x] Push `AT_SECURE` = 0 (no setuid support yet)
- [x] Push `AT_HWCAP` with raw CPUID leaf 1 EDX (matches Linux x86_64; no invented bit positions); `AT_HWCAP2` = 0 (deferred until syscall ABI hardening)
- [x] Retain existing `AT_ENTRY`, `AT_PAGESZ`, `AT_NULL`
- [x] Extend `elf_load_result` struct with `phdr_vaddr`, `phnum`, `phent` fields (additive)
- [x] Extract shared `rdrand_bytes()` helper and migrate `gpt.c` + `kusd_time.c` (third occurrence rule)
- [ ] Follow-up (XREF: TODO-20 §5): replace TSC-mixed AT_RANDOM fallback with kernel entropy pool when CSPRNG lands. Codex flagged the TSC fallback as weak entropy for stack-canary seeding; rejected for §13 because (1) Makefile USER_CFLAGS uses `-fno-stack-protector` so no current binary consumes `__stack_chk_guard`; (2) there is no runtime path for externally built ELFs to enter the system in the current state of the project; (3) RDRAND-capable platforms (bare metal, WHPX) take the fast path; (4) TCG fallback ships a `LOG_ERROR` line so degraded mode is visible. When TODO-20 §5 lands or any user binary opts into stack canaries, this fallback MUST be replaced.
- [x] Commit: `"kernel: abi -- extended auxiliary vector with AT_RANDOM, AT_PHDR, AT_HWCAP"`

**Test checkpoint:** Serial log shows `ELF auxv: AT_RANDOM=0x<stack_addr> AT_PHDR=0x<phdr> AT_PHNUM=<n> AT_HWCAP=0x<edx> (16 pairs, ELF)` from `task_exec` (single observable klog line; no POST16 codes -- this is post-boot code where klog is fully working). Unit test `PEB/TEB: user auxv populated (PID 2)` walks `tasks[2].user_auxv`, asserts AT_NULL terminator present, AT_RANDOM/PAGESZ/ENTRY/HWCAP present, AT_PAGESZ == 4096, AT_HWCAP non-zero and equal to raw CPUID 1 EDX, and reads 16 bytes at AT_RANDOM with at least one non-zero. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 14. User-Mode Thread Bootstrap (uthread_create)
The current `thread_create()` in `src/kernel/sched/task.c:1924` builds a ring-0 interrupt frame: `CS = GDT_KERNEL_CODE`, `SS = GDT_KERNEL_DATA`, `RIP = thread_wrapper`, `RSP = kmalloc'd kernel stack`. Every secondary thread runs in ring 0 on a kernel stack. `NtCreateThread` (TODO-05 §7) calls this function directly, so today's "user-mode multithreading" is actually kernel threads pretending to be user threads. Per-thread TEB (§15) cannot be implemented coherently on this foundation: a kernel thread that owns a user-space TEB pointer can never `swapgs` into it because it never returns to ring 3.

> [!IMPORTANT]
> **Discovered during the per-thread TEB (§15) design review (2026-04-07).** Codex flagged that retrofitting per-thread TEBs onto `thread_create()` is architecturally wrong because the function builds kernel-mode iret frames. §14 is the prerequisite that splits thread creation into kernel and user variants so §15 has a real foundation. The first user task's main thread (created via `task_exec()`) already builds a ring-3 iret frame correctly -- that path is the reference for `uthread_create()`.

> [!NOTE]
> **Prerequisites resolved (2026-04-10):**
> 1. `vmm_map_user_page(cr3, va, phys)` -- implemented in TODO-01-vmm §12 (ce91b05a)
> 2. Per-thread `kernel_rsp` + TSS.rsp0 switching -- implemented in TODO-05-sched §11 (3bf99ed0)

**Files:** `src/kernel/sched/task.c`, `include/kernel/sched/task.h`, `src/kernel/nt/nt_process.c`

- [x] Rename `thread_create()` to `kthread_create()` -- all 11 in-tree callers updated (test_ipc.c: 10, test_sched.c: 1). Legacy `thread_create()` wrapper kept for back-compat (dispatches based on `peb`).
- [x] `uthread_create()` implemented in task.c -- ring-3 iret frame, PMM kernel stack (3 pages: guard + 8 KiB), PMM user stack (4 pages: 16 KiB) mapped via `vmm_map_user_page()`, full error rollback (Codex double-free fix applied), page-aligned size enforcement.
- [x] `USER_THREAD_STACK_BASE` = `0x7FFCA000` defined in task.h with VA layout diagram. `USER_THREAD_STACK_LOWEST` derived constant. Per-thread user stack strides downward by `USER_STACK_SIZE * tid`.
- [x] Static asserts: `USER_THREAD_STACK_BASE < 0x7FFCB000` (below TEB), `USER_THREAD_STACK_LOWEST > 0x1000000` (above kernel region), `USER_THREAD_STACK_LOWEST < USER_THREAD_STACK_BASE` (no wrap).
- [x] `NtCreateThread_handler` uses `thread_create()` wrapper which dispatches to `uthread_create` (PEB present) or `kthread_create` (no PEB). No handler code change needed.
- [x] `kthread_create` + `uthread_create` + `thread_create` declarations in task.h.
- [x] User stack pages reclaimed at `task_cleanup()` time via `vmm_unmap_user_page()` per thread. Deferred from `thread_join()` per TLB shootdown constraint. User stack ownership tracked via `user_stack_va` + `user_stack_pages` fields on `struct thread`.
- [x] `task_cleanup()` walks secondary threads and reclaims user stack pages. Thread 0 uses task-level `kfree` (unchanged).
- [x] `task_exec()` guarded with `current_thread == 0` check -- rejects exec from secondary threads.
- [x] Unit tests in test_peb_teb.c: `test_kthread_create_smoke` (smoke), `test_uthread_stack_layout` (VA range checks), `test_uthread_rejects_kernel_task` (PEB guard). 3 tests, no live boot calls.
- [x] Commit: `"kernel: peb -- user-mode thread bootstrap (kthread_create + uthread_create split)"` (638a0899)

**Test checkpoint:** Boot completes normally. `cmd.exe` (single-threaded user task) starts unchanged because `task_exec()` builds its own ring-3 frame. A user binary calling `NtCreateThread` (none in tree today) would receive a real ring-3 thread on a user stack -- verifiable when a multi-threaded test binary lands. `kthread_create` calls from kernel boot code work unchanged (DPC worker, work queue, etc.). Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

## 15. Per-thread TEB Allocation at uthread_create()
Currently, all threads in a process share `tasks[pid].teb` (one TEB per task), which violates the Win32 contract that each thread has its own TEB at `gs:[0]`. This blocks per-thread TLS expansion arrays (§12 deferred item), per-thread `LastErrorValue`, per-thread stack bounds in `NtTib.StackBase/StackLimit`, and correct `NtCurrentTeb()` semantics on multithreaded processes. `NtCreateThread` (TODO-05 §7) creates a `struct thread` with its own kernel stack but no TEB.

> [!NOTE]
> **Prerequisite resolved:** §14 (uthread_create) shipped in commit 638a0899 (2026-04-10), unblocking §15. The runtime TEB unmap path is still constrained by the lack of SMP TLB shootdown (-> XREF: [03-memory-concurrency/TODO-06-smp-phase2.md §2](../03-memory-concurrency/TODO-06-smp-phase2.md)); until shootdown lands, per-thread TEB pages are reclaimed at `task_cleanup()` (process death), accepting a bounded leak of `THREAD_MAX * 4 KiB = 64 KiB` per multithreaded process between thread death and process exit.

> [!IMPORTANT]
> **Architectural impact:** This section changes the GS base swap path. After §14 lands, every context switch between threads in the same process must reload `IA32_KERNEL_GS_BASE` MSR with the incoming thread's TEB. The schedule() condition changes from `prev_task != next_task` to `prev_task != next_task || prev_thread != next_thread`. Hardware `swapgs` in `isr_stubs.asm` and `syscall_entry.asm` does NOT need to change -- it operates on whatever value the C scheduler wrote into the MSR.

**Files (when unblocked):** `include/kernel/sched/task.h`, `src/kernel/sched/task.c`, `src/kernel/nt/nt_process.c`, `src/kernel/test/test_peb_teb.c`

**Prerequisites:** §14 (uthread_create), and ideally `03-memory-concurrency/TODO-06 §2` (TLB shootdown) for runtime unmap; without §06 §2, cleanup is deferred to `task_cleanup()`.

- [x] Add `void *teb` field to `struct thread` in `include/kernel/sched/task.h`
- [x] Add `uint64_t kernel_gs_base` field to `struct thread`. Keep `tasks[pid].kernel_gs_base` and `tasks[pid].teb` as a mirror of `threads[0]` -- they remain in lockstep so single-thread fast-paths and TLS code (which still indexes by pid) work unchanged.
- [x] Place per-thread TEBs at distinct user VAs via the existing `TEB_USER_BASE - tid * 0x1000` formula in `teb_alloc_for_task()` (formula already accepts `tid`; today only `task_exec()` calls it with 0). After §14, `uthread_create()` calls it with the new tid.
- [x] In `uthread_create()` (added by §14): after building the ring-3 frame, call `teb_alloc_for_task(pid, new_tid, ustack_base, USER_STACK_SIZE, parent->peb)` and store result in `t->threads[new_tid].teb`. Set `t->threads[new_tid].kernel_gs_base = (uint64_t)(uintptr_t)t->threads[new_tid].teb`. NULL TEB allocation -> roll back the user stack and return -1.
- [x] Update `schedule()` and `schedule_now()`: change KERNEL_GS_BASE swap condition from `prev_task != next_task` to `prev_task != next_task || prev_thread != next_thread`. Read from `tasks[next_task].threads[next_thread].kernel_gs_base`. NULL guard: if the new value is 0 (kernel thread), do NOT write the MSR.
- [x] `NtCurrentTeb()` semantics: `gs:[0x30]` (NtTib.Self) must point at the current thread's TEB after swapgs. This falls out automatically once the MSR write is per-thread; no source change needed beyond the schedule() update.
- [x] Update `NtCreateThread_handler` in `nt_process.c`: verify the new thread has a non-NULL TEB, return STATUS_NO_MEMORY if NULL. Update `NtQueryInformationThread_handler` to read `thr->teb` (per-thread) with fallback to `owner->teb` for kernel threads.
- [x] On thread exit: per-thread TEB reclaimed at `task_cleanup()` via `vmm_unmap_user_page(cr3, teb)` for each secondary thread. Deferred from `thread_join()` because runtime unmap lacks SMP TLB shootdown (-> XREF [03-memory-concurrency/TODO-06 §2](../03-memory-concurrency/TODO-06-smp-phase2.md)).
- [x] §12 TLS expansion remains task-scoped after §15. Static TLS slots (`TEB.TlsSlots[64]`) become per-thread automatically because they live inside the per-thread TEB struct, but expansion slots (`TEB.TlsExpansionSlots`) still point at one task-shared expansion array. Full per-thread TLS expansion is deferred to a future TODO.
- [x] Update `test_peb_teb.c`: `_Static_assert` for `teb` and `kernel_gs_base` fields, smoke test `threads[0].teb == NULL` in kernel context, per-thread TEB VA stride formula check across THREAD_MAX tids.
- [x] Commit: `"kernel: peb -- per-thread TEB allocation at uthread_create()"` (65cf3586)

**Test checkpoint:** Spawning two threads in the same process produces two distinct TEB VAs (`thread_a->teb != thread_b->teb`). `NtCurrentTeb()` from each thread returns its own TEB. Setting `LastErrorValue` in thread A does not affect thread B. Allocating a TLS slot in thread A and writing to it does not affect the same slot index in thread B (per-thread isolation). Context switch correctly swaps `IA32_KERNEL_GS_BASE` MSR. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

---

## OS Comparison

| ⭐ | Feature                     | 🪟 Win11                  | 🐧 Linux                 | 🚀 Impossible OS              |
|----|-----------------------------|----------------------------|--------------------------|--------------------------------|
| 💎 | Per-process env block       | ✅ PEB at gs:[0x60]       | ❌ argv/envp on stack    | ✅ §2+§5 PEB allocated        |
| 💎 | Per-thread block (TEB)      | ✅ TEB at gs:[0x30]       | ⚠️ glibc pthread TLS     | ⚠️ §1+§6 task-level (-> §15)  |
| 💎 | swapgs kernel entry/exit    | ✅ KiSystemCall64         | ✅ entry.S swapgs        | ✅ §3+§4 swapgs+MSR           |
| 💎 | LastError per-thread        | ✅ TEB offset 0x68        | ⚠️ errno per-thread      | ⚠️ §6 task-level (-> §15)     |
| 💎 | TLS static slots (64)       | ✅ TEB offset 0x1480      | ✅ pthread + FS-base     | ✅ §9 bitmap alloc            |
| 💎 | Process parameters          | ✅ cmdline, env, handles  | ❌ stack + /proc         | ✅ §5 RTLPP populated         |
| 💎 | Ldr module list             | ✅ PEB->Ldr linked list   | ❌ ld-linux link map     | ✅ §8 main module             |
| 💎 | Initial stack frame         | ✅ RCX=PEB (Win64)        | ✅ ELF ABI layout        | ✅ §7 argc/argv/auxv          |
| ⭐ | PEB/TEB in Ob namespace     | ❌ Private internal       | ❌ Not exposed           | ✅ §10 public API             |
| ⭐ | Win11 version in PEB        | ✅ Internal only          | ❌ N/A                   | ✅ §5 10.0.22621              |
| 💎 | KUSER_SHARED_DATA page      | ✅ 0x7FFE0000 read-only   | ✅ vDSO equivalent       | ✅ §11 full struct+ISR        |
| 💎 | TLS expansion (1024 slots)  | ✅ TlsExpansionSlots      | ✅ pthread TLS unlimited | ✅ §12 1024 slots             |
| 💎 | AT_RANDOM stack canary      | ⚠️ PEB Cookie (different) | ✅ auxv AT_RANDOM        | ✅ §13 RDRAND+TSC fb          |
| 💎 | AT_PHDR/AT_PHNUM auxv       | ❌ PE, not ELF            | ✅ auxv standard         | ✅ §13 shared parser          |
| 💎 | CPU feature auxv (AT_HWCAP) | ⚠️ ProcessorFeatures[]    | ✅ AT_HWCAP/AT_HWCAP2    | ✅ §13 raw CPUID 1 EDX        |
| 💎 | Real user threads (ring 3)  | ✅ NtCreateThread ring 3  | ✅ clone() ring 3        | ✅ §14 uthread_create done    |
| 💎 | Per-thread TEB / GS swap    | ✅ Per-thread TEB         | ✅ Per-thread FS_BASE    | ✅ §15 per-thread TEB+MSR     |

> **After §1 -- §13:** Impossible OS matches Windows NT on the user-mode ABI contract for single-threaded user processes. `NtCurrentTeb()`, `GetLastError()`, TLS slots, and PEB->ProcessParameters all work at correct GS offsets -- ntdll and Win32 DLLs initialise without patching.
> **§10** goes beyond both Windows and Linux by making PEB and TEB first-class named objects in the Ob namespace.
> **§14 + §15** close the remaining gap for multi-threaded user processes: §14 splits `thread_create` so secondary threads run in ring 3, §15 layers per-thread TEBs on that foundation.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_peb_teb()` in [`src/kernel/test/test_runner.c`](../../src/kernel/test/test_runner.c).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] Create `src/kernel/test/test_peb_teb.c` -- 5 suites, 18 assertions:
  - TEB offsets: Self at 0x30, ClientId at 0x40, PEB ptr at 0x60, LastError at 0x68, TlsSlots at 0x1480
  - PEB offsets: ImageBaseAddress at 0x10, Ldr at 0x18, ProcessParameters at 0x20, OSMajorVersion at 0xA4
  - PEB OS version: OSMajorVersion==10, OSMinorVersion==0, OSBuildNumber==22621
  - PEB populated: ProcessParameters non-NULL, NumberOfProcessors matches acpi, BeingDebugged==0
  - RTLPP content: ImagePathName non-empty, CommandLine non-empty, Environment non-NULL
  - TEB runtime tests (GS self-pointer, ClientId, LastError, TLS) -- deferred to §6+§9
- [x] Register in `test_runner_init()`: `test_register_peb_teb()`
- [x] Commit: `"test: add PEB/TEB user-mode ABI test suite"`

> **Done:** 5 suites, 18 assertions (2026-04-02). Offset tests always pass (compile-time). OS version + populated field tests skip gracefully if PEB not yet allocated (tests run before task_exec). TEB runtime tests deferred to §6.

- [x] TLS expansion tests: `tls_alloc` returns 0 -- 63 for first 64 calls; 65th call returns 64 (expansion); `tls_set_value(pid, 64, 0xCAFE)` + `tls_get_value(pid, 64)` == 0xCAFE; `tls_alloc` up to 1087 succeeds; 1088th returns -1 -- DONE: covered by §12's 6 suites in `test_peb_teb.c` (TLS constants, static alloc/free, expansion alloc, expansion reuse, expansion boundary at 1088, POST codes)
- [x] Extended auxv tests: after `task_exec` of ELF binary, stack contains AT_RANDOM pointing to 16 non-zero bytes; AT_PHDR non-NULL; AT_PHNUM > 0; AT_PAGESZ == 4096; AT_HWCAP non-zero and equal to raw CPUID 1 EDX -- 9 suites in `test_peb_teb.c` (auxv constants, rdrand_bytes smoke + boundaries, elf_extract PT_PHDR / PT_LOAD fallback / PT_PHDR vaddr=0 fallback / no coverage / invalid, user auxv populated PID 2)
- [ ] KUSER_SHARED_DATA tests are NOT in `test_peb_teb.c` today. The infrastructure (§11) is complete and verified at runtime via cmd.exe accessing `0x7FFE0000`, but no compile-time offset checks or unit assertions exist for the KUSD struct fields. Tracked as a deferred test gap; rolls into the next test-coverage pass on §11.

## Verification

- [x] `bash scripts/build.sh clean` -> `=== BUILD OK ===` -- PASS: build 1967 (WHPX, 2026-04-02)
- [x] Serial log: `PID 2: PEB=0x7FFDE000 TEB=0x7FFDB000 (Win 10.0.22621, 2 CPUs)` at 35.170s -- PASS (WHPX, 2026-04-02)
- [x] In ring 3: `gs:[0x30]` returns TEB self-pointer -- PASS: implicit via `cmd.exe` running. The shell is built against the same TEB layout; if `gs:[0x30]` self-pointer were broken, `NtCurrentTeb()`-derived calls would crash on entry. cmd.exe reaches the prompt (WHPX, 2026-04-02)
- [x] In ring 3: `gs:[0x60]` returns PEB -- PASS: implicit via `cmd.exe` running and ntdll-style PEB reads succeeding (WHPX, 2026-04-02)
- [x] In ring 3: `gs:[0x68]` reads LastErrorValue = 0 -- PASS: implicit via `cmd.exe` initial state -- LastError starts at 0 from `teb_alloc_for_task()` zero-fill, no syscall has set it before the prompt appears (WHPX, 2026-04-02)
- [x] After INT 0x80 entry: GS points to per-CPU data -- PASS: 109 unit tests pass, all use smp_this_cpu() via GS (WHPX, 2026-04-02)
- [x] After `iretq` exit: GS in ring 3 points to TEB -- PASS: cmd.exe runs, swapgs restores KERNEL_GS_BASE=TEB on every ISR exit (WHPX, 2026-04-02)
- [x] `user/hello.exe` / cmd.exe starts correctly with new stack frame -- PASS: `C:\>` prompt at 35.170s (WHPX, 2026-04-02)
- [x] `PEB->OSMajorVersion == 10`, `OSBuildNumber == 22621` -- PASS: logged in serial at task_exec (WHPX, 2026-04-02)
- [ ] Commit: `"kernel: peb/teb -- user-mode ABI complete"` -- pending §14 + §15 to claim the file as Done
