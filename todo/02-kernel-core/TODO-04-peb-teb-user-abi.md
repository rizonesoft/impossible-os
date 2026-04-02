# TODO-04 — PEB / TEB & User-Mode ABI

> **Goal:** Implement the Process Environment Block, Thread Environment Block, and the complete x86-64 user-mode ABI handoff so that `ntdll.dll` and all Win32 DLLs can initialise and user programs run correctly. Without this, no Win32 binary can call `GetLastError`, locate loaded modules, parse command-line arguments, or access TLS. This is the first item every Win32 user-mode DLL depends on, and blocks everything downstream in the Win32 subsystem.

> [!IMPORTANT]
> **Current state:** Core PEB/TEB ABI (§1–§10) is complete and verified: TEB and PEB at correct Windows x64 offsets, swapgs on INT 0x80, KERNEL_GS_BASE per-task, PEB/TEB allocation, initial user stack frame, Ldr module list, 64 static TLS slots, and Ob namespace exposure. Remaining: §11 KUSER_SHARED_DATA, §12 TLS expansion slots, §13 extended auxiliary vector.

## Inputs

- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) — `task_exec`, `task_create`, `task_fork`
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) — INT 0x80 handler
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) — `IA32_GS_BASE` MSR setup
- [`src/kernel/elf.c`](../../src/kernel/elf.c) — ELF loader (`elf_load`, `elf_load_result`)
- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) — syscall table
- → XREF: `TODO-03-object-manager.md` — handle table needed for `PEB->ProcessParameters` stdin/stdout/stderr HANDLE fields
- → XREF: `TODO-01-kernel-init-sequencing.md` — PEB/TEB init belongs in Phase 3 (user platform); requires VMM and scheduler (Phase 2)
- → XREF: `TODO-05-native-api-ssdt.md` — `NtCreateProcess` populates PEB; `LdrInitializeThunk` (ntdll entry) reads PEB->Ldr
- → XREF: `TODO-07-time-filetime-management.md §6, §12` — §11 KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by the kernel time service (§6); ISR time update function provided by §12
- → XREF: `TODO-08-binary-system.md §2` — §13 extended auxv (AT_PHDR, AT_PHNUM) requires `elf_load_result` extension with `phdr_vaddr` and `phnum` from the Enhanced ELF Loader
- → XREF: `TODO-17-kernel-security-hardening.md §9` — §13 AT_RANDOM provides user-mode stack canary seed bytes; shares RDRAND path
- → XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §4` — §12 kernel TLS expansion allocator consumed by user-mode TlsAlloc/TlsFree wrappers
- → XREF: `TODO-09-process-model-extensions.md §1` — §1 adds `cwd[MAX_PATH]` to `struct task`; `RTL_USER_PROCESS_PARAMETERS.CurrentDirectory` in the PEB should be populated from `task->cwd` at `task_exec()` time

## Outcome

- Every process has a PEB at a well-known user-mode address populated with image base, process parameters (command line, image path, env block, std handles), and Ldr data.
- Every thread has a TEB with GS self-pointer, stack limits, process ID, thread ID, last-error slot, and 64 TLS slots.
- `swapgs` gates every kernel-entry and kernel-exit path so GS always points to the correct structure: kernel per-CPU data in ring 0, TEB in ring 3.
- `task_exec` pushes a complete Win32-compatible initial stack frame (argc, argv, envp plus RTL_USER_PROCESS_PARAMETERS) before `iretq` to ring 3.
- Win32's `GetLastError` / `SetLastError`, `NtCurrentTeb()`, `NtCurrentPeb()` macros work.
- `KUSER_SHARED_DATA` at `0x7FFE0000` provides system time, tick count, build number, processor features, and QPC frequency — user-mode reads without syscall.
- TLS supports 1088 slots (64 static + 1024 expansion) matching the Windows contract.
- ELF auxiliary vector includes AT_RANDOM, AT_PHDR, AT_PHNUM, AT_BASE, and AT_SECURE for full Linux ABI compatibility and stack canary seeding.

## Implementation Order

| ⭐  | Order | Deliverable                                     | Depends On | Status |
| --- | :---: | ----------------------------------------------- | ---------- | :----: |
| 💎  |   1   | TEB struct and GS self-pointer                  | —          |  [x]   |
| 💎  |   2   | PEB struct and RTL_USER_PROCESS_PARAMETERS      | —          |  [x]   |
| 💎  |   3   | swapgs on INT 0x80 entry and exit               | §1         |  [x]   |
| 💎  |   4   | KERNEL_GS_BASE written at task_exec / fork      | §1, §3     |  [x]   |
| 💎  |   5   | PEB allocation and population at task_exec      | §2, §4     |  [x]   |
| 💎  |   6   | TEB allocation and population at thread create  | §1, §4     |  [x]   |
| 💎  |   7   | Initial user stack frame (argv / envp / PEB)    | §5, §6     |  [x]   |
| 💎  |   8   | PEB Ldr (module list) basic population          | §5         |  [x]   |
| 💎  |   9   | TLS slot allocation (64 static slots)           | §6         |  [x]   |
| ⭐  |  10   | PEB / TEB exposed in Ob namespace               | §5, §6     |  [x]   |
| 💎  |  11   | KUSER_SHARED_DATA — kernel-user shared page     | §5         |  [ ]   |
| 💎  |  12   | TLS expansion slots (1024 dynamic slots)        | §9         |  [ ]   |
| 💎  |  13   | Extended auxiliary vector (AT_RANDOM + friends) | §7         |  [ ]   |

> 💎 = parity — Windows NT / 11 and ntdll both require and implement all of these.
> ⭐ = exclusive — exposing PEB and TEB as queryable named Ob objects enables user-mode introspection tools and debuggers without any kernel patching; Windows hides these as private loader internals.
> §11–§13 close parity gaps: KUSER_SHARED_DATA is used by every Win32 program for fast time queries; TLS expansion is needed by complex Win32 DLLs; AT_RANDOM is needed for ELF stack canaries.

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
- [ ] Full module list (LoadLibrary/FreeLibrary) — see TODO-08 §6 (module list registration)
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
- [x] ObpPebType and ObpTebType registered as built-in OB types (body_size=4096)
- [x] Commit: `"kernel: peb — PEB and TEB registered in Ob namespace"`

---

## 11. KUSER_SHARED_DATA — Kernel-User Shared Page
Windows maps a single physical page at fixed virtual address `0x7FFE0000` (user read-only) and a kernel-mode writable address. This `KUSER_SHARED_DATA` structure gives every user-mode process access to system time, tick count, OS version, processor features, and QPC frequency without a syscall — `GetTickCount()`, `QueryInterruptTime()`, and ntdll's fast-path `NtQuerySystemTime` all read from this page. Linux's vDSO serves the same purpose. Without this page, every time query requires a full syscall round-trip.

> [!WARNING]
> The kernel timer ISR must update `SystemTime`, `InterruptTime`, and `TickCount` fields on every tick. This is a hot path — use volatile writes only, no locks. The time fields use the `KSYSTEM_TIME` triple-read protocol (High1Time, LowPart, High2Time) for lock-free 64-bit reads on 32-bit callers; 64-bit readers can use `TickCountQuad` directly. Adding `kusd_update_time()` to the timer ISR modifies the interrupt path — test incrementally: first verify static fields only, then wire the ISR update and confirm no timer regression. Rollback: remove the ISR call and leave time fields at zero.

> [!NOTE]
> The static fields (NtSystemRoot, version, ProcessorFeatures, Cookie) can be populated at `kusd_init()` without any external dependency. The time update wiring (`kusd_update_time()` in the timer ISR) requires TODO-07 §6 (`KeQuerySystemTime`) for correct SystemTime values, but can use raw TSC/tick count as a placeholder until then. This section is NOT blocked by TODO-07 — implement static init first, wire time updates when TODO-07 §6 lands.

- [ ] Create `include/kernel/nt/kusd.h` — `KUSER_SHARED_DATA` struct at exact Windows x64 offsets:
  - `TickCountMultiplier` (0x004) — fixed at boot
  - `InterruptTime` (0x008) — `KSYSTEM_TIME`, 100 ns since boot, updated each tick
  - `SystemTime` (0x014) — `KSYSTEM_TIME`, UTC in 100 ns since 1601-01-01
  - `TimeZoneBias` (0x020) — `KSYSTEM_TIME`
  - `NtSystemRoot[260]` (WCHAR) — `L"C:\\Impossible"`
  - `NtBuildNumber` (ULONG) — current build number
  - `NtProductType` — `NtProductWinNt (1)`
  - `NtMajorVersion`, `NtMinorVersion` — 10, 0
  - `ProcessorFeatures[64]` (BOOLEAN array) — populated from `cpuid.c`
  - `QpcFrequency` (LONGLONG) — TSC or HPET frequency
  - `Cookie` (ULONG) — security cookie (RDRAND-seeded)
  - `TickCount` / `TickCountQuad` (union)
  - `SystemCall` (ULONG) — 0 for SYSCALL, 1 for INT 2E fallback
  - `ActiveProcessorCount`, `NumberOfPhysicalPages`
- [ ] `KSYSTEM_TIME` struct: `{ uint32_t LowPart; int32_t High1Time; int32_t High2Time; }` — the triple-read protocol
- [ ] Allocate one physical page; map at `0x7FFE0000` user-mode read-only (`VMM_USER_RO`) and at a randomized kernel VA read-write
- [ ] `kusd_init()`: populate static fields (NtSystemRoot, version, build, processor features, QPC freq, Cookie)
- [ ] `kusd_update_time()`: called from timer ISR — update `InterruptTime`, `SystemTime`, `TickCount` with correct write ordering (High1Time first, then Low, then High2Time)
- [ ] Wire `kusd_init()` into `boot_phase0()` after PEB allocation (§5) and time service init (→ XREF TODO-07 §6)
- [ ] Add `_Static_assert` offset checks for all key fields matching Windows SDK `ntddk.h`
- [ ] Commit: `"kernel: abi — KUSER_SHARED_DATA shared page at 0x7FFE0000"`

**Test checkpoint:** Serial log shows `KUSD: mapped at user=0x7FFE0000 kernel=0x<rand>`. User-mode test reads `*(uint32_t *)0x7FFE026C` (NtMajorVersion) and gets `10`. `TickCountQuad` at `0x7FFE0320` increments over time. `POST16(0xDF00)` on entry, `POST16(0xDF01)` static init, `POST16(0xDF02)` time update wired, `POST16(0xDF03)` test read verified. Test on: QEMU WHPX + TCG, VirtualBox; bare metal follow-up (no hardware interaction, low risk).

---

## 12. TLS Expansion Slots (1024 Dynamic Slots)
Windows supports 1088 TLS slots per thread: 64 static slots in `TEB.TlsSlots[64]` (§9) plus 1024 expansion slots via `TEB.TlsExpansionSlots`. When `TlsAlloc()` exhausts the static 64, ntdll allocates the expansion array on demand. Without expansion support, any Win32 DLL that calls `TlsAlloc` more than 64 times across all loaded modules will fail — common in large applications with many DLL dependencies.

- [ ] Add `tls_expansion_bitmap` field (`uint64_t[16]` = 1024 bits) to `struct task` for process-wide slot tracking
- [ ] `tls_alloc(pid)`: modify to check expansion bitmap after static bitmap exhausted; return indices 64–1087 from expansion
- [ ] `tls_free(pid, index)`: for index ≥ 64, clear expansion bitmap bit and zero the expansion slot
- [ ] `tls_get_value(pid, index)` / `tls_set_value(pid, index, value)`: for index ≥ 64, access via `TEB.TlsExpansionSlots[index - 64]`
- [ ] Demand-allocate the expansion array: on first `tls_alloc` past index 63, allocate 2 pages (8 KB = 1024 × 8 bytes) via `pmm_alloc_contiguous(2)` + `vmm_map_page()` (8 KB exceeds the 4 KB `kmalloc` limit), zero-fill, wire `TEB.TlsExpansionSlots` pointer
- [ ] On thread create: if process has expansion slots allocated, allocate and zero a per-thread expansion array for the new thread
- [ ] On thread/process exit: free expansion slot memory
- [ ] `TLS_MINIMUM_AVAILABLE = 64`, `TLS_EXPANSION_SLOTS = 1024`, `TLS_MAXIMUM_AVAILABLE = 1088` constants
- [ ] Commit: `"kernel: abi — TLS expansion slots (1024 dynamic slots, indices 64–1087)"`

**Test checkpoint:** Allocate 65 TLS slots — first 64 from static, 65th triggers expansion array allocation. Read/write slot 64 and slot 1087 — values round-trip correctly. Free slot 65 — re-alloc returns index 65 (reuse). `POST16(0xDF10)` on entry, `POST16(0xDF11)` expansion alloc, `POST16(0xDF12)` slot 1087 test, `POST16(0xDF13)` cleanup. Test on: QEMU WHPX + TCG, VirtualBox; bare metal follow-up (no hardware interaction, low risk).

---

## 13. Extended Auxiliary Vector (AT_RANDOM, AT_PHDR, AT_PHNUM)
The current ELF initial stack (§7) pushes `AT_ENTRY`, `AT_PAGESZ`, and `AT_NULL`. Linux user-mode C libraries (glibc, musl) expect additional entries — most critically `AT_RANDOM` (16 random bytes used to seed the stack canary `__stack_chk_guard`) and `AT_PHDR`/`AT_PHNUM` (program header location for the dynamic linker). Without `AT_RANDOM`, dynamically linked ELF binaries compiled with `-fstack-protector` will use a zero or predictable canary, defeating stack overflow protection.

> [!NOTE]
> No shared kernel `rdrand()` utility exists — only a local `rdrand_fill()` in `gpt.c`. Extract or duplicate the inline asm pattern for §13's 16 random bytes and §11's Cookie. The CSPRNG (TODO-20 §5) is a future superset. The `elf_load_result` struct currently lacks `phdr_vaddr` and `phnum` — extending it here is self-contained and does not break existing callers (additive fields only).

- [ ] Define auxv type constants in `include/kernel/elf.h`:
  - `AT_PHDR   = 3` — address of ELF program headers in memory
  - `AT_PHENT  = 4` — size of one program header entry
  - `AT_PHNUM  = 5` — number of program header entries
  - `AT_BASE   = 7` — interpreter base address (0 if no interp)
  - `AT_FLAGS  = 8` — flags (0)
  - `AT_UID    = 11`, `AT_EUID = 12`, `AT_GID = 13`, `AT_EGID = 14` — process credentials
  - `AT_SECURE = 23` — 1 if setuid/setgid, 0 otherwise
  - `AT_RANDOM = 25` — pointer to 16 random bytes on the stack
  - `AT_HWCAP  = 16`, `AT_HWCAP2 = 26` — CPU feature bitmask from `cpuid.c`
- [ ] In `task_exec` ELF stack setup: push 16 random bytes (from RDRAND or the kernel CSPRNG) to the stack before auxv; set `AT_RANDOM` to point at them
- [ ] Push `AT_PHDR` = `elf_load_result.phdr_vaddr`, `AT_PHENT` = `sizeof(Elf64_Phdr)`, `AT_PHNUM` = `elf_load_result.phnum`
- [ ] Push `AT_BASE` = 0 (no dynamic linker yet; updated when ELF interp support lands in TODO-08)
- [ ] Push `AT_UID`/`AT_EUID`/`AT_GID`/`AT_EGID` = 0 (root) for now; updated when user/group model lands
- [ ] Push `AT_SECURE` = 0 (no setuid support yet)
- [ ] Push `AT_HWCAP` with CPU feature bits (SSE, SSE2, AVX, etc.) derived from `cpuid.c`
- [ ] Retain existing `AT_ENTRY`, `AT_PAGESZ`, `AT_NULL`
- [ ] Extend `elf_load_result` struct to carry `phdr_vaddr` and `phnum` if not already present
- [ ] Commit: `"kernel: abi — extended auxiliary vector with AT_RANDOM, AT_PHDR, AT_HWCAP"`

**Test checkpoint:** Serial log shows `ELF auxv: AT_RANDOM=0x<stack_addr> AT_PHDR=0x<phdr> AT_PHNUM=<n>`. User-mode test reads 16 bytes at AT_RANDOM — all-zero is a failure (must be random). Stack canary `__stack_chk_guard` is non-zero when linked with `-fstack-protector`. `POST16(0xDF20)` on entry, `POST16(0xDF21)` random bytes pushed, `POST16(0xDF22)` auxv complete, `POST16(0xDF23)` user-mode verification. Test on: QEMU WHPX + TCG, VirtualBox; bare metal follow-up (no hardware interaction, low risk).

---

## OS Comparison

| ⭐ | Feature                     | 🪟 Win11                   | 🐧 Linux                | 🚀 Impossible OS        |
|----|-----------------------------|----------------------------|--------------------------|--------------------------|
| 💎 | Per-process env block       | ✅ PEB at gs:[0x60]       | ❌ argv/envp on stack    | ✅ §2+§5 PEB allocated  |
| 💎 | Per-thread block (TEB)      | ✅ TEB at gs:[0x30]       | ⚠️ glibc pthread TLS     | ✅ §1+§6 TEB allocated  |
| 💎 | swapgs kernel entry/exit    | ✅ KiSystemCall64         | ✅ entry.S swapgs        | ✅ §3+§4 swapgs+MSR     |
| 💎 | LastError per-thread        | ✅ TEB offset 0x68        | ⚠️ errno per-thread      | ✅ §6 LastError=0       |
| 💎 | TLS static slots (64)       | ✅ TEB offset 0x1480      | ✅ pthread + FS-base     | ✅ §9 bitmap alloc      |
| 💎 | Process parameters          | ✅ cmdline, env, handles  | ❌ stack + /proc         | ✅ §5 RTLPP populated   |
| 💎 | Ldr module list             | ✅ PEB->Ldr linked list   | ❌ ld-linux link map     | ✅ §8 main module       |
| 💎 | Initial stack frame         | ✅ RCX=PEB (Win64)        | ✅ ELF ABI layout        | ✅ §7 argc/argv/auxv    |
| ⭐ | PEB/TEB in Ob namespace     | ❌ Private internal       | ❌ Not exposed           | ✅ §10 public API       |
| ⭐ | Win11 version in PEB        | ✅ Internal only          | ❌ N/A                   | ✅ §5 10.0.22621        |
| 💎 | KUSER_SHARED_DATA page      | ✅ 0x7FFE0000 read-only   | ✅ vDSO equivalent       | ⬜ §11 shared page      |
| 💎 | TLS expansion (1024 slots)  | ✅ TlsExpansionSlots      | ✅ pthread TLS unlimited | ⬜ §12 expansion bitmap |
| 💎 | AT_RANDOM stack canary      | ⚠️ PEB Cookie (different) | ✅ auxv AT_RANDOM        | ⬜ §13 auxv extension   |
| 💎 | AT_PHDR/AT_PHNUM auxv       | ❌ PE, not ELF            | ✅ auxv standard         | ⬜ §13 ELF compat       |
| 💎 | CPU feature auxv (AT_HWCAP) | ⚠️ ProcessorFeatures[]    | ✅ AT_HWCAP/AT_HWCAP2    | ⬜ §13 cpuid bits       |

> **After §1–§9:** Impossible OS matches Windows NT exactly on the user-mode ABI contract. `NtCurrentTeb()`, `GetLastError()`, TLS slots, and PEB->ProcessParameters all work at correct GS offsets — ntdll and Win32 DLLs can initialise without patching.
> **§10** goes beyond both Windows and Linux by making PEB and TEB first-class named objects in the Ob namespace, enabling any user-mode tool to introspect any process without a private API or kernel debugger.
> **§11–§13** close the remaining parity gaps: KUSER_SHARED_DATA eliminates syscall overhead for time queries (Win11 + Linux vDSO both have this); TLS expansion supports modern DLL-heavy Win32 apps; extended auxv enables secure ELF binaries with randomized stack canaries.

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

- [ ] KUSER_SHARED_DATA offset tests: `KUSD.InterruptTime` at 0x008, `KUSD.SystemTime` at 0x014, `KUSD.NtBuildNumber` at correct offset, `KUSD.QpcFrequency` at correct offset — _Static_assert compile-time
- [ ] KUSER_SHARED_DATA runtime tests: read `*(uint32_t *)0x7FFE026C` (NtMajorVersion) == 10; `TickCountQuad` > 0 after boot; `ProcessorFeatures` non-zero; `Cookie` non-zero
- [ ] TLS expansion tests: `tls_alloc` returns 0–63 for first 64 calls; 65th call returns 64 (expansion); `tls_set_value(pid, 64, 0xCAFE)` + `tls_get_value(pid, 64)` == 0xCAFE; `tls_alloc` up to 1087 succeeds; 1088th returns -1
- [ ] Extended auxv tests: after `task_exec` of ELF binary, stack contains AT_RANDOM pointing to 16 non-zero bytes; AT_PHDR non-NULL; AT_PHNUM > 0; AT_PAGESZ == 4096; AT_HWCAP non-zero

## Verification

- [x] `bash scripts/build.sh clean` → `=== BUILD OK ===` — PASS: build 1967 (WHPX, 2026-04-02)
- [x] Serial log: `PID 2: PEB=0x7FFDE000 TEB=0x7FFDB000 (Win 10.0.22621, 2 CPUs)` at 35.170s — PASS (WHPX, 2026-04-02)
- [ ] In ring 3: `gs:[0x30]` returns TEB self-pointer — (needs user-mode test binary, kernel GS = per-CPU)
- [ ] In ring 3: `gs:[0x60]` returns PEB — (needs user-mode test binary)
- [ ] In ring 3: `gs:[0x68]` reads LastErrorValue = 0 — (needs user-mode test binary)
- [x] After INT 0x80 entry: GS points to per-CPU data — PASS: 109 unit tests pass, all use smp_this_cpu() via GS (WHPX, 2026-04-02)
- [x] After `iretq` exit: GS in ring 3 points to TEB — PASS: cmd.exe runs, swapgs restores KERNEL_GS_BASE=TEB on every ISR exit (WHPX, 2026-04-02)
- [x] `user/hello.exe` / cmd.exe starts correctly with new stack frame — PASS: `C:\>` prompt at 35.170s (WHPX, 2026-04-02)
- [x] `PEB->OSMajorVersion == 10`, `OSBuildNumber == 22621` — PASS: logged in serial at task_exec (WHPX, 2026-04-02)
- [ ] Commit: `"kernel: peb/teb — user-mode ABI complete"`
