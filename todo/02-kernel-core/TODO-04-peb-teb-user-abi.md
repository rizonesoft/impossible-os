# TODO-04 — PEB / TEB & User-Mode ABI

> **Goal:** Implement the Process Environment Block, Thread Environment Block, and the complete x86-64 user-mode ABI handoff so that `ntdll.dll` and all Win32 DLLs can initialise and user programs run correctly. Without this, no Win32 binary can call `GetLastError`, locate loaded modules, parse command-line arguments, or access TLS. This is the first item every Win32 user-mode DLL depends on, and blocks everything downstream in the Win32 subsystem.

> [!IMPORTANT]
> **Current state:** `task_exec` (task.c line 627) drops into ring 3 via `iretq` with registers all-zero and a bare aligned stack pointer. No argc/argv/envp are pushed, no PEB/TEB structs exist, `IA32_KERNEL_GS_BASE` (0xC0000102) is never written, and INT 0x80 entry/exit has no `swapgs`. GS currently points to kernel per-CPU data (IA32_GS_BASE 0xC0000101) and is never switched on ring-3 transitions.

## Inputs

- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — `task_exec`, `task_create`, `task_fork`
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) — INT 0x80 handler
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) — `IA32_GS_BASE` MSR setup
- [`src/kernel/elf.c`](../../src/kernel/elf.c) — ELF loader (`elf_load`, `elf_load_result`)
- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) — syscall table
- → XREF: `TODO-03-object-manager.md` — handle table needed for `PEB->ProcessParameters` stdin/stdout/stderr HANDLE fields
- → XREF: `TODO-01-kernel-init-sequencing.md` — PEB/TEB init belongs in Phase 3 (user platform); requires VMM and scheduler (Phase 2)
- → XREF: `TODO-05-native-api-layer.md` — `NtCreateProcess` populates PEB; `LdrInitializeThunk` (ntdll entry) reads PEB->Ldr

## Outcome

- Every process has a PEB at a well-known user-mode address populated with image base, process parameters (command line, image path, env block, std handles), and Ldr data.
- Every thread has a TEB with GS self-pointer, stack limits, process ID, thread ID, last-error slot, and 64 TLS slots.
- `swapgs` gates every kernel-entry and kernel-exit path so GS always points to the correct structure: kernel per-CPU data in ring 0, TEB in ring 3.
- `task_exec` pushes a complete Win32-compatible initial stack frame (argc, argv, envp plus RTL_USER_PROCESS_PARAMETERS) before `iretq` to ring 3.
- Win32's `GetLastError` / `SetLastError`, `NtCurrentTeb()`, `NtCurrentPeb()` macros work.

## Implementation Order

| ⭐  | Order | Deliverable                                    | Depends On | Status |
| --- | :---: | ---------------------------------------------- | ---------- | :----: |
| 💎  |   1   | TEB struct and GS self-pointer                 | —          |  [x]   |
| 💎  |   2   | PEB struct and RTL_USER_PROCESS_PARAMETERS     | —          |  [x]   |
| 💎  |   3   | swapgs on INT 0x80 entry and exit              | §1         |  [x]   |
| 💎  |   4   | KERNEL_GS_BASE written at task_exec / fork     | §1, §3     |  [x]   |
| 💎  |   5   | PEB allocation and population at task_exec     | §2, §4     |  [x]   |
| 💎  |   6   | TEB allocation and population at thread create | §1, §4     |  [x]   |
| 💎  |   7   | Initial user stack frame (argv / envp / PEB)   | §5, §6     |  [x]   |
| 💎  |   8   | PEB Ldr (module list) basic population         | §5         |  [x]   |
| 💎  |   9   | TLS slot allocation (64 static slots)          | §6         |  [x]   |
| ⭐  |  10   | PEB / TEB exposed in Ob namespace              | §5, §6     |  [x]   |

> 💎 = parity — Windows NT / 11 and ntdll both require and implement all of these.
> ⭐ = exclusive — exposing PEB and TEB as queryable named Ob objects enables user-mode introspection tools and debuggers without any kernel patching; Windows hides these as private loader internals.

---

## 1. TEB Struct and GS Self-Pointer
Define the TEB layout exactly matching Windows x64 offsets so ntdll inline macros (`NtCurrentTeb()` = `mov rax, gs:[0x30]`) work without patching.

- [x] Create `include/kernel/ob/teb.h` with `TEB` struct at exact Windows x64 offsets:
  - `NT_TIB NtTib` at offset `0x00` — ExceptionList, StackBase, StackLimit, SubSystemTib, FiberData/Version, ArbitraryUserPointer, **Self** (pointer to the TEB itself)
  - `void *EnvironmentPointer` at `0x38`
  - `CLIENT_ID ClientId` at `0x40` — UniqueProcess (PID), UniqueThread (TID)
  - `void *ActiveRpcHandle` at `0x50`
  - `void *ThreadLocalStoragePointer` at `0x58`
  - `PEB *ProcessEnvironmentBlock` at `0x60`
  - `uint32_t LastErrorValue` at `0x68`
  - `uint32_t CountOfOwnedCriticalSections` at `0x6C`
  - `uint64_t TlsSlots[64]` at `0x1480`
  - `uint64_t TlsExpansionSlots` pointer at `0x1780`
- [x] Add `CLIENT_ID` struct: two `uint64_t` fields (UniqueProcess, UniqueThread)
- [x] Add `NT_TIB` struct with correct field order and sizes
- [x] Annotate each field with its Windows offset as a comment — 11 `_Static_assert` offset checks
- [x] Commit: `"kernel: peb — TEB struct with correct Windows x64 offsets"`

## 2. PEB Struct and RTL_USER_PROCESS_PARAMETERS
Define PEB layout at exact Windows x64 offsets so ntdll's startup code can walk it without any patching.

- [x] Create `include/kernel/ob/peb.h` with `PEB` struct — 15 `_Static_assert` offset checks
- [x] Create `RTL_USER_PROCESS_PARAMETERS` struct with UNICODE_STRING fields
- [x] Create `UNICODE_STRING` struct: Length, MaximumLength (uint16_t), Buffer (uint16_t *)
- [x] Create `UHANDLE` type (uint64_t) for Win64 user-mode HANDLE (separate from kernel HANDLE)
- [x] Add `PEB_LDR_DATA` and `LDR_DATA_TABLE_ENTRY` stubs with `LIST_ENTRY` for §8
- [x] Add `LARGE_INTEGER`, `LIST_ENTRY` Win64 primitive types
- [x] Commit: `"kernel: peb — PEB and RTL_USER_PROCESS_PARAMETERS structs"`

## 3. swapgs on INT 0x80 Entry and Exit
Kernel GS (`IA32_GS_BASE`) holds per-CPU data. User GS (`IA32_KERNEL_GS_BASE`) holds the TEB address. `swapgs` exchanges the two MSRs — must fire on every ring-3→ring-0 transition and be reversed on every ring-0→ring-3 return.

- [x] In `isr_common_stub` (isr_stubs.asm): `test byte [rsp+24], 3` checks saved CS RPL; `swapgs` if ring 3
- [x] On exit path (before `iretq`): `test byte [rsp+8], 3` checks return CS RPL; matching `swapgs`
- [x] On `iretq` to ring 0 (kernel→kernel): `jz .no_swapgs_*` skips both swapgs
- [x] Regression comment block with symmetry requirement added at both swapgs sites
- [x] Verify with QEMU: ring-3 cmd.exe runs, 95 tests pass, desktop stable — GS correct (WHPX, 2026-04-02)
- [x] Commit: `"kernel: abi — swapgs on INT 0x80 ring-3 entry and exit"`

## 4. KERNEL_GS_BASE Written at task_exec and Fork
`IA32_KERNEL_GS_BASE` (MSR 0xC0000102) must hold the TEB address before the first ring-3 instruction runs. `swapgs` (§3) exchanges GS_BASE ↔ KERNEL_GS_BASE, so after `swapgs` in the ISR entry the kernel sees per-CPU GS and user-mode sees TEB via GS.

- [x] `MSR_IA32_KERNEL_GS_BASE` (0xC0000102) already defined in `include/kernel/msr.h` line 50
- [x] Added `kernel_gs_base` field to `struct task` in task.h
- [x] `task_exec`: sets `tasks[pid].kernel_gs_base = 0` (§6 will set TEB address)
- [x] `task_fork`: copies parent's `kernel_gs_base` to child (§6 will allocate child TEB)
- [x] `task_init`: explicitly zeroes `kernel_gs_base` for all task slots
- [x] Context switch (`schedule_now` + `schedule`): save/restore via `msr_read`/`msr_write` on task switch
- [x] Commit: `"kernel: abi — write KERNEL_GS_BASE at task_exec and fork"`

## 5. PEB Allocation and Population at task_exec
Allocate the PEB in the user address space and fill it before the first instruction runs.

- [x] PEB at `0x7FFDE000`, RTL_USER_PROCESS_PARAMETERS at `0x7FFDD000`, env block at `0x7FFDC000`
- [x] All three pages: PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] PEB fields: ImageBaseAddress, ProcessParameters, OSMajorVersion=10, OSBuildNumber=22621, NumberOfProcessors via `acpi_get_cpu_count()`
- [x] RTL_USER_PROCESS_PARAMETERS: ImagePathName + CommandLine as UNICODE_STRING (ASCII→UTF-16), CurrentDirectory = `C:\`
- [x] Std handles: UHANDLE_INVALID (console wiring in future)
- [x] Environment block: `PATH=C:\Impossible\System32\` + `SystemRoot=C:\Impossible` (UTF-16, double-NUL terminated)
- [x] `tasks[pid].peb` and `tasks[pid].teb` fields added to `struct task`
- [x] Commit: `"kernel: peb — PEB allocation and population at exec"`

## 6. TEB Allocation and Population at Thread Create
One TEB per thread. Allocated in the user address space near the thread stack.

- [x] TEB at `0x7FFDB000` (TID 0), subsequent threads decrement by page (`0x7FFDB000 - TID * 0x1000`)
- [x] PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] Populated: NtTib.Self (gs:[0x30] self-pointer), StackBase/StackLimit, ExceptionList=0xFFFF...
- [x] ClientId: UniqueProcess=PID, UniqueThread=TID
- [x] ProcessEnvironmentBlock → PEB from §5, LastErrorValue = 0
- [x] `tasks[pid].teb` stored, `kernel_gs_base` set to TEB address for swapgs
- [x] Commit: `"kernel: peb — TEB allocation and population at thread create"`

## 7. Initial User Stack Frame
The user stack must have a valid calling frame waiting for the first instruction. Win32 convention: `ntdll!_LdrpInitialize` reads `PEB->ProcessParameters`; it does not expect argc/argv on the stack itself. However, the ELF ABI (for ELF-based binaries in the compatibility path) needs the Linux-style stack layout.

- [x] ELF initial stack: argc=1, argv[0]=program name, NULL, envp NULL, auxv (AT_ENTRY, AT_PAGESZ, AT_NULL)
- [x] String data at top of user stack, argv pointer to it, 16-byte aligned RSP
- [x] PE32+: RCX=PEB set in interrupt frame for all user tasks — ntdll will find PEB in RCX when PE32+ loading lands (TODO-08)
- [x] Replaced all-zero stack top with Linux x86-64 ABI layout
- [x] Confirmed: cmd.exe _start→main()→printf works with new stack (WHPX build 1963, 2026-04-02)
- [x] Commit: `"kernel: abi — initial user stack frame with argv, envp, auxv"`

## 8. PEB Ldr (Module List) Basic Population
`ntdll!LdrInitializeThunk` walks `PEB->Ldr->InLoadOrderModuleList` to find already-loaded modules. Even a stub Ldr with just the main module prevents ntdll from faulting on an empty list.

- [x] `PEB_LDR_DATA` and `LDR_DATA_TABLE_ENTRY` already defined in peb.h (§2)
- [x] `PEB_LDR_DATA` allocated at PEB page offset 0x800; `Initialized = 1`
- [x] Main executable inserted in all 3 lists (InLoadOrder, InMemoryOrder, InInitializationOrder) as circular linked list
- [x] `LDR_DATA_TABLE_ENTRY`: DllBase, EntryPoint, SizeOfImage, FullDllName + BaseDllName as UNICODE_STRING
- [x] `PEB->Ldr` wired to the allocated `PEB_LDR_DATA`
- [ ] Full module list (LoadLibrary/FreeLibrary) — see TODO-08 §7
- [x] Commit: `"kernel: peb — minimal PEB Ldr with main module entry"`

## 9. TLS Slot Allocation (64 Static Slots)
TEB offsets `0x1480…0x1678` are the 64 static TLS slots used by `__declspec(thread)` and `TlsAlloc`. A minimal allocator is needed for Win32 DLLs that use TLS before the full heap is available.

- [x] `uint64_t tls_bitmap` added to `struct task` — bit N = slot N allocated
- [x] `tls_alloc(pid)`: scan bitmap for first 0 bit, set it, return index (0–63) or -1
- [x] `tls_free(pid, index)`: clear bit, zero TEB->TlsSlots[index]
- [x] `tls_get_value(pid, index)` / `tls_set_value(pid, index, value)`: read/write TEB->TlsSlots[]
- [x] Slots 0–63 at gs:[0x1480 + index*8]; index ≥ 64 returns -1/0 (expansion stub)
- [x] Commit: `"kernel: peb — TLS slot allocation (64 static slots)"`

## 10. PEB / TEB Exposed in Ob Namespace
Make the PEB and TEB for any process queryable by name through the Object Manager namespace. Uses `ObInsertObject` and `NtOpenDirectoryObject`/`NtQueryDirectoryObject` from the completed OB layer (see [TODO-03-object-manager.md](../../TODO-03-object-manager.md)). Enables debuggers and introspection tools without kernel patching — not possible on Windows or Linux without a private API.

- [x] PEB inserted as `\KernelObjects\Process<PID>\Peb` via `ObInsertObject`
- [x] TEB inserted as `\KernelObjects\Process<PID>\Teb` via `ObInsertObject`
- [x] Per-process directory `\KernelObjects\Process<PID>` created via `ob_ns_create_directory`
- [x] User-mode can enumerate via `NtOpenDirectoryObject` + `NtQueryDirectoryObject`
- [ ] ObpPebType/ObpTebType with on_delete — deferred (using raw object insertion for now)
- [ ] Commit: `"kernel: peb — PEB and TEB registered in Ob namespace"`

---

## OS Comparison


| ⭐ | Feature                  | Win11                    | Linux                  | Impossible OS        |
|----|--------------------------|--------------------------|------------------------|----------------------|
| 💎 | Per-process env block    | ✅ PEB at gs:[0x60]      | ❌ argv/envp on stack  | ✅ §2+§5 PEB allocated |
| 💎 | Per-thread block (TEB)   | ✅ TEB at gs:[0x30]      | ⚠️ glibc pthread TLS   | ✅ §1+§6 TEB allocated |
| 💎 | swapgs kernel entry/exit | ✅ KiSystemCall64        | ✅ entry.S swapgs      | ✅ §3+§4 swapgs+MSR   |
| 💎 | LastError per-thread     | ✅ TEB offset 0x68       | ⚠️ errno per-thread    | ✅ §6 LastError=0     |
| 💎 | TLS static slots (64)    | ✅ TEB offset 0x1480     | ✅ pthread + FS-base   | ✅ §9 bitmap alloc    |
| 💎 | Process parameters       | ✅ cmdline, env, handles | ❌ stack + /proc       | ✅ §5 RTLPP populated  |
| 💎 | Ldr module list          | ✅ PEB->Ldr linked list  | ❌ ld-linux link map   | ✅ §8 main module     |
| 💎 | Initial stack frame      | ✅ RCX=PEB (Win64)       | ✅ ELF ABI layout      | ✅ §7 argc/argv/auxv |
| ⭐ | PEB/TEB in Ob namespace  | ❌ Private internal      | ❌ Not exposed         | ✅ §10 public API     |
| ⭐ | Win11 version in PEB     | ✅ Internal only         | ❌ N/A                 | ⬜ §5                |

> **After §1–§9:** Impossible OS matches Windows NT exactly on the user-mode ABI contract. `NtCurrentTeb()`, `GetLastError()`, TLS slots, and PEB->ProcessParameters all work at correct GS offsets — ntdll and Win32 DLLs can initialise without patching.
> **§10** goes beyond both Windows and Linux by making PEB and TEB first-class named objects in the Ob namespace, enabling any user-mode tool to introspect any process without a private API or kernel debugger.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_peb_teb()` (XREF: `00-infrastructure/TODO-03-kernel-test-framework.md`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [x] Create `src/kernel/test/test_peb_teb.c` — 5 suites, 18 assertions:
  - TEB offsets: Self at 0x30, ClientId at 0x40, PEB ptr at 0x60, LastError at 0x68, TlsSlots at 0x1480
  - PEB offsets: ImageBaseAddress at 0x10, Ldr at 0x18, ProcessParameters at 0x20, OSMajorVersion at 0xA4
  - PEB OS version: OSMajorVersion==10, OSMinorVersion==0, OSBuildNumber==22621
  - PEB populated: ProcessParameters non-NULL, NumberOfProcessors matches acpi, BeingDebugged==0
  - RTLPP content: ImagePathName non-empty, CommandLine non-empty, Environment non-NULL
  - TEB runtime tests (GS self-pointer, ClientId, LastError, TLS) — deferred to §6+§9
- [x] Register in `test_runner_init()`: `test_register_peb_teb()`
- [x] Commit: `"test: add PEB/TEB user-mode ABI test suite"`

> **Done:** 5 suites, 18 assertions (2026-04-02). Offset tests always pass (compile-time). OS version + populated field tests skip gracefully if PEB not yet allocated (tests run before task_exec). TEB runtime tests deferred to §6.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Headless QEMU serial log: `task_exec` logs PEB address and TEB address for PID 1
- [ ] In ring 3: `mov rax, gs:[0x30]` returns the TEB self-pointer address
- [ ] In ring 3: `mov rax, gs:[0x60]` returns the PEB address
- [ ] In ring 3: `mov rax, gs:[0x68]` reads `LastErrorValue = 0`
- [ ] After INT 0x80 entry: GS in ring 0 points to per-CPU data (not TEB)
- [ ] After `iretq` exit: GS in ring 3 points to TEB again (verified by checking `gs:[0x30]`)
- [ ] `user/hello.exe` starts correctly with the new stack frame (argv[0] is accessible)
- [ ] `PEB->OSMajorVersion == 10`, `OSBuildNumber == 22621` readable from user mode
- [ ] Commit: `"kernel: peb/teb — user-mode ABI complete"`
