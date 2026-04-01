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
| 💎  |   1   | TEB struct and GS self-pointer                 | —          |  [ ]   |
| 💎  |   2   | PEB struct and RTL_USER_PROCESS_PARAMETERS     | —          |  [ ]   |
| 💎  |   3   | swapgs on INT 0x80 entry and exit              | 1          |  [ ]   |
| 💎  |   4   | KERNEL_GS_BASE written at task_exec / fork     | 1, 3       |  [ ]   |
| 💎  |   5   | PEB allocation and population at task_exec     | 2, 4       |  [ ]   |
| 💎  |   6   | TEB allocation and population at thread create | 1, 4       |  [ ]   |
| 💎  |   7   | Initial user stack frame (argv / envp / PEB)   | 5, 6       |  [ ]   |
| 💎  |   8   | PEB Ldr (module list) basic population         | 5          |  [ ]   |
| 💎  |   9   | TLS slot allocation (64 static slots)          | 6          |  [ ]   |
| ⭐  |  10   | PEB / TEB exposed in Ob namespace              | 5, 6, TODO-03 §4 | [ ] |

> 💎 = parity — Windows NT / 11 and ntdll both require and implement all of these.
> ⭐ = exclusive — exposing PEB and TEB as queryable named Ob objects enables user-mode introspection tools and debuggers without any kernel patching; Windows hides these as private loader internals.

---

## 1. TEB Struct and GS Self-Pointer `[Sonnet]`

Define the TEB layout exactly matching Windows x64 offsets so ntdll inline macros (`NtCurrentTeb()` = `mov rax, gs:[0x30]`) work without patching.

- [ ] Create `include/kernel/ob/teb.h` with `TEB` struct at exact Windows x64 offsets:
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
- [ ] Add `CLIENT_ID` struct: two `uint64_t` fields (UniqueProcess, UniqueThread)
- [ ] Add `NT_TIB` struct with correct field order and sizes
- [ ] Annotate each field with its Windows offset as a comment — future correctness check
- [ ] Commit: `"kernel: peb — TEB struct with correct Windows x64 offsets"`

## 2. PEB Struct and RTL_USER_PROCESS_PARAMETERS `[Sonnet]`

Define PEB layout at exact Windows x64 offsets so ntdll's startup code can walk it without any patching.

- [ ] Create `include/kernel/ob/peb.h` with `PEB` struct:
  - `uint8_t InheritedAddressSpace` at `0x00`
  - `uint8_t ReadImageFileExecOptions` at `0x01`
  - `uint8_t BeingDebugged` at `0x02`
  - `uint8_t BitField` at `0x03` — ImageUsesLargePages, NtGlobalFlag bits
  - `void *Mutant` at `0x08`
  - `void *ImageBaseAddress` at `0x10`
  - `PEB_LDR_DATA *Ldr` at `0x18`
  - `RTL_USER_PROCESS_PARAMETERS *ProcessParameters` at `0x20`
  - `uint32_t NtGlobalFlag` at `0xBC`
  - `LARGE_INTEGER CriticalSectionTimeout` at `0xC8`
  - `uint64_t HeapSegmentReserve` / `HeapSegmentCommit` at `0xD0`/`0xD8`
  - `uint32_t NumberOfProcessors` at `0xB8`
  - `uint32_t OSMajorVersion`, `OSMinorVersion`, `OSBuildNumber` at `0xA4`/`0xA8`/`0xAC`
  - `uint64_t TlsBitmap` / `TlsBitmapBits[2]` at `0x230`
- [ ] Create `RTL_USER_PROCESS_PARAMETERS` struct:
  - `uint32_t MaximumLength`, `Length`
  - `uint32_t Flags`
  - `uint32_t DebugFlags`
  - `void *ConsoleHandle`
  - `uint32_t ConsoleFlags`
  - `HANDLE StandardInput`, `StandardOutput`, `StandardError`
  - `UNICODE_STRING CurrentDirectory.DosPath`, `HANDLE CurrentDirectory.Handle`
  - `UNICODE_STRING DllPath`, `ImagePathName`, `CommandLine`
  - `void *Environment` — pointer to env block (null-terminated name=value pairs in UTF-16)
- [ ] Create minimal `UNICODE_STRING` struct: `Length`, `MaximumLength` (uint16_t), `Buffer` (wchar_t *)
- [ ] Add `PEB_LDR_DATA` stub for §8
- [ ] Commit: `"kernel: peb — PEB and RTL_USER_PROCESS_PARAMETERS structs"`

## 3. swapgs on INT 0x80 Entry and Exit `[Opus]`

Kernel GS (`IA32_GS_BASE`) holds per-CPU data. User GS (`IA32_KERNEL_GS_BASE`) holds the TEB address. `swapgs` exchanges the two MSRs — must fire on every ring-3→ring-0 transition and be reversed on every ring-0→ring-3 return.

- [ ] In the INT 0x80 ISR stub (`idt.asm` or equivalent): add `swapgs` as the **first** instruction before any register save when `CS & 3 == 3` (came from ring 3)
  - Check CPL using `testb $3, 8(%rsp)` (CS is 8 bytes past `rsp` on exception entry) — execute `swapgs` only if non-zero, to be safe on nested kernel faults
- [ ] On INT 0x80 exit path (before `iretq` back to ring 3): add matching `swapgs` to restore TEB address to GS before `iretq`
- [ ] On `iretq` to ring 0 (kernel→kernel): do NOT call `swapgs`
- [ ] Add regression comment block at the swapgs site explaining the symmetry requirement — a missing or double swapgs causes immediate GS corruption and is hard to debug
- [ ] Verify with QEMU: after ring-3 task runs, GS in ring 0 still points to per-CPU data
- [ ] Commit: `"kernel: abi — swapgs on INT 0x80 ring-3 entry and exit"`

## 4. KERNEL_GS_BASE Written at task_exec and Fork `[Opus]`

`IA32_KERNEL_GS_BASE` (MSR 0xC0000102) must hold the TEB address before the first ring-3 instruction runs. `swapgs` (§3) exchanges GS_BASE ↔ KERNEL_GS_BASE, so after `swapgs` in the ISR entry the kernel sees per-CPU GS and user-mode sees TEB via GS.

- [ ] Define `MSR_KERNEL_GS_BASE 0xC0000102` in `include/kernel/smp/msr.h` (or smp.c)
- [ ] In `task_exec`: after allocating TEB (§6), `wrmsr(MSR_KERNEL_GS_BASE, teb_addr)` before the scheduler runs the task
- [ ] In `task_fork`: duplicate TEB for the child; write child's TEB address to `IA32_KERNEL_GS_BASE` when the child is first scheduled
- [ ] In `task_create` for kernel tasks: write `0` to `IA32_KERNEL_GS_BASE` — kernel tasks never execute `swapgs` back to a TEB
- [ ] On context switch (`switch_context.asm`): save and restore `IA32_KERNEL_GS_BASE` per task if SMP or if multiple user tasks share the same CPU
- [ ] Commit: `"kernel: abi — write KERNEL_GS_BASE at task_exec and fork"`

## 5. PEB Allocation and Population at task_exec `[Sonnet]`

Allocate the PEB in the user address space and fill it before the first instruction runs.

- [ ] Reserve a fixed user-mode address for PEB: `0x7FFDE000` (matches Windows default for 64-bit processes on low addresses); map a VMM page there (writable, user-accessible)
- [ ] Zero the entire PEB page
- [ ] Populate mandatory fields:
  - `ImageBaseAddress` ← ELF load base from `elf_load_result`
  - `ProcessParameters` ← pointer to the `RTL_USER_PROCESS_PARAMETERS` block (§2)
  - `OSMajorVersion = 10`, `OSMinorVersion = 0`, `OSBuildNumber = 22621` — Win11 compatibility
  - `NumberOfProcessors` ← CPUID leaf 1 logical processor count
  - `BeingDebugged = 0`
- [ ] Build `RTL_USER_PROCESS_PARAMETERS` in user address space:
  - Convert `task->name` to a `UNICODE_STRING` ImagePathName (ASCII → UTF-16 inline)
  - Build `CommandLine` from task argv array
  - Set `StandardInput = INVALID_HANDLE_VALUE`, `StandardOutput`, `StandardError` ← handles from the process handle table once TODO-03 §3 is done; for now write sentinel
  - Build environment block: null-terminated UTF-16 `name=value\0name=value\0\0` pairs starting with `PATH=C:\Impossible\System32\`
- [ ] Store PEB pointer in `task_t` struct for kernel reference: `tasks[pid].peb`
- [ ] Commit: `"kernel: peb — PEB allocation and population at exec"`

## 6. TEB Allocation and Population at Thread Create `[Sonnet]`

One TEB per thread. Allocated in the user address space near the thread stack.

- [ ] Allocate one page for the TEB at user address `0x7FFDC000` for the initial thread; subsequent threads increment by page (or use VMM range allocation)
- [ ] Zero the TEB page
- [ ] Populate mandatory fields:
  - `NtTib.Self` ← TEB address (the GS self-pointer; `gs:[0x30]` must return the TEB address)
  - `NtTib.StackBase` ← `user_stack_base + USER_STACK_SIZE`
  - `NtTib.StackLimit` ← `user_stack_base`
  - `NtTib.ExceptionList` ← `0xFFFFFFFFFFFFFFFF` (no active SEH frame)
  - `ClientId.UniqueProcess` ← PID (as uint64_t)
  - `ClientId.UniqueThread` ← TID
  - `ProcessEnvironmentBlock` ← PEB address from §5
  - `LastErrorValue` ← 0
- [ ] Store TEB pointer in the task struct: `tasks[pid].teb`
- [ ] Commit: `"kernel: peb — TEB allocation and population at thread create"`

## 7. Initial User Stack Frame `[Opus]`

The user stack must have a valid calling frame waiting for the first instruction. Win32 convention: `ntdll!_LdrpInitialize` reads `PEB->ProcessParameters`; it does not expect argc/argv on the stack itself. However, the ELF ABI (for ELF-based binaries in the compatibility path) needs the Linux-style stack layout.

- [ ] For **ELF executables** (current path): push the Linux x86-64 initial stack layout:
  - `argc` (uint64_t), then `argv[0]…argv[argc-1]`, NULL, then `envp[0]…`, NULL, then `AT_NULL` auxv entry — RSP on entry to `_start` points to `argc`
  - `AT_ENTRY`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_FLAGS`, `AT_PAGESZ` auxv entries populated from ELF load result
  - `AT_NULL` terminator
- [ ] For **PE32+ executables** (future): push a single null pointer as the return address and pass PEB address in RCX (Win64 calling convention first arg); ntdll will parse ProcessParameters for argv
- [ ] Replace the current all-zero stack top in `task_exec` with the correct layout above
- [ ] Confirm `_start` in `user/hello.c` still executes correctly with the new stack
- [ ] Commit: `"kernel: abi — initial user stack frame with argv, envp, auxv"`

## 8. PEB Ldr (Module List) Basic Population `[Sonnet]`

`ntdll!LdrInitializeThunk` walks `PEB->Ldr->InLoadOrderModuleList` to find already-loaded modules. Even a stub Ldr with just the main module prevents ntdll from faulting on an empty list.

- [ ] Define `PEB_LDR_DATA` struct:
  - `uint32_t Length`, `uint8_t Initialized`
  - `void *SsHandle`
  - `LIST_ENTRY InLoadOrderModuleList`
  - `LIST_ENTRY InMemoryOrderModuleList`
  - `LIST_ENTRY InInitializationOrderModuleList`
- [ ] Define `LDR_DATA_TABLE_ENTRY` with DllBase, EntryPoint, SizeOfImage, FullDllName (UNICODE_STRING), BaseDllName (UNICODE_STRING), Flags, LoadCount
- [ ] Allocate `PEB_LDR_DATA` in user space; set `Initialized = 1`
- [ ] Insert main executable as the first and only `LDR_DATA_TABLE_ENTRY` in all three lists
- [ ] Wire `PEB->Ldr` to the allocated `PEB_LDR_DATA`
- [ ] Full module list management (LoadLibrary / FreeLibrary) is out of scope here — see `TODO-08-binary-system.md` §7 (PE32+ import resolver) and §10 (ELF dynamic linker)
- [ ] Commit: `"kernel: peb — minimal PEB Ldr with main module entry"`

## 9. TLS Slot Allocation (64 Static Slots) `[Sonnet]`

TEB offsets `0x1480…0x1678` are the 64 static TLS slots used by `__declspec(thread)` and `TlsAlloc`. A minimal allocator is needed for Win32 DLLs that use TLS before the full heap is available.

- [ ] Add a per-process static TLS bitmap: 64-bit `uint64_t tls_bitmap` in `task_t` (one bit per slot; bit=0 means free)
- [ ] Implement `TlsAlloc()` kernel helper: scan bitmap for first free bit, set it, return slot index (0–63)
- [ ] Implement `TlsFree(index)`: clear the bit; zero the TLS slot in all threads of the process
- [ ] `TlsGetValue(index)` / `TlsSetValue(index, value)`: read/write `teb->TlsSlots[index]` directly (inline via GS offset for performance)
- [ ] Slots 0–63 map to `gs:[0x1480 + index * 8]`; expansion slots (index ≥ 64) go through `teb->TlsExpansionSlots` pointer — stub the expansion path (return error for index ≥ 64)
- [ ] Commit: `"kernel: peb — TLS slot allocation (64 static slots)"`

## 10. PEB / TEB Exposed in Ob Namespace `[Sonnet]`

Make the PEB and TEB for any process queryable by name through the Object Manager namespace (→ XREF: TODO-03 §4 and §11). Enables debuggers and introspection tools without kernel patching — not possible on Windows or Linux without a private API.

- [ ] Insert each process PEB as a named object in `\KernelObjects\Process<PID>\Peb` using `ObInsertObject` (TODO-03 §4)
- [ ] Insert each thread TEB as `\KernelObjects\Process<PID>\Thread<TID>\Teb`
- [ ] Implement `ObpPebType` and `ObpTebType` — on_delete frees the user-space pages
- [ ] User-mode can call `NtOpenDirectoryObject` + `NtQueryDirectoryObject` (TODO-03 §11) to enumerate all live processes and inspect their PEB/TEB fields
- [ ] Document this as a public Impossible OS introspection API
- [ ] Commit: `"kernel: peb — PEB and TEB registered in Ob namespace"`

---

## OS Comparison


| ⭐ | Feature                               | Win11                                               | Linux                                        | Impossible OS                    |
|----|---------------------------------------|-----------------------------------------------------|----------------------------------------------|----------------------------------|
| 💎 | Per-process environment block         | ✅ PEB at `gs:[0x60]`, fully documented             | ❌ No equivalent (argv/envp on stack         | ⬜ §2 — , §5                     |
| 💎 | Per-thread block with GS self-pointer | ✅ TEB at `gs:[0x30]` self-ref                      | ⚠️ `fs:[0x00]` for glibc `pthread` TLS       | ⬜ §1 — , §6                     |
| 💎 | swapgs on kernel-entry / exit         | ✅ SWAPGS in KiSystemCall64 and KiExceptionDispatch | ✅ `swapgs` in entry.S / iret                | ⬜ §3 — , §4                     |
| 💎 | LastError per-thread slot             | ✅ `TEB->LastErrorValue` at `gs:[0x68]`             | ⚠️ Per-thread `errno` via `__errno_location` | ⬜ §6                            |
| 💎 | TLS static slots                      | ✅ `TEB->TlsSlots[64]` at `gs:[0x1480]`             | ✅ pthread key table + FS-base               | ⬜ §9                            |
| 💎 | RTL_USER_PROCESS_PARAMETERS           | ✅ Command line, env, std handles,                  | ❌ Passed on stack / `/proc/self/cmdline`    | ⬜ §2 — , §5                     |
| 💎 | PEB Ldr module list                   | ✅ `PEB->Ldr->InLoadOrderModuleList`                | ❌ `ld-linux` ELF link map (separate)        | ⬜ §8                            |
| 💎 | Initial user stack frame              | ✅ Win64: RCX=PEB on first call                     | ✅ Linux ELF ABI stack layout                | ⬜ §7                            |
| ⭐ | PEB/TEB as named Ob namespace objects | ❌ Private, not in Ob namespace                     | ❌ Not exposed                               | ⬜ §10 — public introspection    |
| ⭐ | Win11 OS version fields in PEB        | ✅ Internal only                                    | ❌ N/A                                       | ⬜ §5 — full Win11 compat fields |

> **After §1–§9:** Impossible OS matches Windows NT exactly on the user-mode ABI contract. `NtCurrentTeb()`, `GetLastError()`, TLS slots, and PEB->ProcessParameters all work at correct GS offsets — ntdll and Win32 DLLs can initialise without patching.
> **§10** goes beyond both Windows and Linux by making PEB and TEB first-class named objects in the Ob namespace, enabling any user-mode tool to introspect any process without a private API or kernel debugger.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_peb_teb()` (XREF: `00-infrastructure/TODO-03 §1`).
> Boot tests run with `debug=1` or `test=1` in boot.conf.

- [ ] Create `src/kernel/test/test_peb_teb.c` with:
  - TEB `NtTib.Self` at offset `0x30` equals the TEB base address (GS self-pointer contract)
  - TEB `ClientId.UniqueProcess` at offset `0x40` matches the task PID
  - TEB `ClientId.UniqueThread` at offset `0x48` matches the thread TID
  - TEB `ProcessEnvironmentBlock` at offset `0x60` points to a valid PEB address
  - TEB `LastErrorValue` at offset `0x68` is initialized to 0
  - PEB `ImageBaseAddress` at offset `0x10` matches the ELF load base from exec
  - PEB `ProcessParameters` at offset `0x20` is non-NULL and points to valid `RTL_USER_PROCESS_PARAMETERS`
  - PEB `OSMajorVersion == 10`, `OSMinorVersion == 0`, `OSBuildNumber == 22621`
  - PEB `NumberOfProcessors` matches CPUID-reported logical processor count
  - `RTL_USER_PROCESS_PARAMETERS.CommandLine` contains the executable name
  - `RTL_USER_PROCESS_PARAMETERS.ImagePathName` is non-empty
  - TLS slot 0 is initially free; `TlsAlloc()` returns 0; `TlsFree(0)` succeeds
  - `TlsSetValue(0, 0xDEAD)` followed by `TlsGetValue(0)` returns `0xDEAD`
  - `TlsAlloc()` for index >= 64 returns error (expansion not yet supported)
- [ ] Register in `test_runner_init()`: `test_register_peb_teb()`
- [ ] Commit: `"test: add PEB/TEB user-mode ABI test suite"`

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
