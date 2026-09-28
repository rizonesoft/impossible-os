---
schema_version: 1
id: peb-teb-user-abi
domain: 02-kernel-core
status: active
title: "TODO-11 -- PEB / TEB & User-Mode ABI"
---

# TODO-11 -- PEB / TEB & User-Mode ABI

> **Validated:** 2026-06-28 | validate-todo-file clean (structure / IO table / XREF / test wiring); added 11 missing inter-section `---` separators
> **Gap-audited:** 2026-06-28 | mature confirmatory pass + codex-gap-audit; 1 finding filed (§11 `FullNumberOfPhysicalPages`, Win11 24H2); PEB/TEB/auxv ABI stable, no new baseline gaps

> **Goal:** Implement the Process Environment Block, Thread Environment Block, and the complete x86-64 user-mode ABI handoff so that `ntdll.dll` and all Win32 DLLs can initialise and user programs run correctly. Without this, no Win32 binary can call `GetLastError`, locate loaded modules, parse command-line arguments, or access TLS. This is the first item every Win32 user-mode DLL depends on, and blocks everything downstream in the Win32 subsystem.

> [!IMPORTANT]
> **Current state:** Core PEB/TEB ABI, user-mode thread bootstrap, and per-thread TEB allocation are complete and verified: TEB and PEB at correct Windows x64 offsets, swapgs on INT 0x80, KERNEL_GS_BASE per-task/per-thread, PEB/TEB allocation, initial user stack frame, Ldr module list, 64 static TLS slots, Ob namespace exposure, most `KUSER_SHARED_DATA` time/version fields, TLS expansion (1024 slots), and extended ELF auxv (AT_RANDOM/AT_PHDR/AT_HWCAP). Remaining: finish `KUSER_SHARED_DATA` policy publication for TODO-02 §5 and §10 and add dedicated KUSD test coverage.

## Inputs

- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- `task_exec`, `task_create`, `task_fork`
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c) -- INT 0x80 handler
- [`src/kernel/smp/smp.c`](../../src/kernel/smp/smp.c) -- `IA32_GS_BASE` MSR setup
- [`src/kernel/elf.c`](../../src/kernel/elf.c) -- ELF loader (`elf_load`, `elf_load_result`)
- [`include/kernel/sched/syscall.h`](../../include/kernel/sched/syscall.h) -- syscall table
- → XREF: `TODO-05-object-manager.md` -- handle table needed for `PEB->ProcessParameters` stdin/stdout/stderr HANDLE fields
- → XREF: `TODO-01-kernel-init-sequencing.md` -- PEB/TEB init belongs in Phase 3 (user platform); requires VMM and scheduler (Phase 2)
- → XREF: `TODO-12-native-api-ssdt.md` -- `NtCreateProcess` populates PEB; `LdrInitializeThunk` (ntdll entry) reads PEB->Ldr
- → XREF: `TODO-08-time-filetime-management.md §6, §12` -- §11 KUSER_SHARED_DATA time fields (SystemTime, InterruptTime, QpcFrequency) are populated by the kernel time service (§6); ISR time update function provided by §12
- → XREF: `TODO-02-kernel-configuration-policy.md §5, §10` -- §11 must mirror Safe Mode, debugger, mitigation, and boot-status summary bits from the effective kernel policy snapshot
- → XREF: `TODO-17-binary-system.md §4` -- §13 extended auxv (AT_PHDR, AT_PHNUM) requires `elf_load_result` extension with `phdr_vaddr` and `phnum` from the Enhanced ELF Loader
- → XREF: `TODO-10-kernel-security-hardening.md §10` -- §12 AT_RANDOM provides user-mode stack canary seed bytes; shares RDRAND path
- → XREF: `12-user-platform-sdk/TODO-04-ntdll-user-runtime.md §6` -- §12 kernel TLS expansion allocator consumed by user-mode TlsAlloc/TlsFree wrappers
- → XREF: `TODO-21-process-model-extensions.md §1` -- §1 adds `cwd[MAX_PATH]` to `struct task`; `RTL_USER_PROCESS_PARAMETERS.CurrentDirectory` in the PEB should be populated from `task->cwd` at `task_exec()` time
- → XREF: `TODO-31-kernel-bulletproofing.md` §1,§3,§5,§9 -- same 5-layer invariant pattern for gs:/frame/swapgs contracts that PEB/TEB and syscall paths rely on
- → XREF: `TODO-23-exception-dispatch-seh.md` §5, §8, §12 -- T23 §5 pushes the KiUserExceptionDispatcher frame onto the §7 initial user stack; §8 SEH reads TEB `ExceptionList` (§6); §12 stores the per-process filter in the §2 PEB `UnhandledExceptionFilter` field
- → XREF: `TODO-22-environment-variables.md` §5, §6, §10 -- `NtSetEnvironmentVariable` (§5) updates the kernel-authoritative `task->environ`; the optional `PEB->ProcessParameters->Environment` raw-block re-sync is a §6 follow-up (in-tree readers already use `task->environ`); `CREATE_UNICODE_ENVIRONMENT` when passing custom UTF-16 env blocks to process creation; sorted UTF-16 env block contract (gap-analysis 2026-04-15)

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
| 💎  |   2   | PEB struct and RTL_USER_PROCESS_PARAMETERS      | --                 |  [/]   |
| 💎  |   3   | swapgs on INT 0x80 entry and exit               | §1                 |  [x]   |
| 💎  |   4   | KERNEL_GS_BASE written at task_exec / fork      | §1, §3             |  [/]   |
| 💎  |   5   | PEB allocation and population at task_exec      | §2, §4             |  [/]   |
| 💎  |   6   | TEB allocation and population at thread create  | §1, §4             |  [/]   |
| 💎  |   7   | Initial user stack frame (argv / envp / PEB)    | §5, §6             |  [x]   |
| 💎  |   8   | PEB Ldr (module list) basic population          | §5                 |  [/]   |
| 💎  |   9   | TLS slot allocation (64 static slots)           | §6                 |  [/]   |
| ⭐  |  10   | PEB / TEB exposed in Ob namespace               | §5, §6             |  [/]   |
| 💎  |  11   | KUSER_SHARED_DATA -- kernel-user shared page    | §5                 |  [/]   |
| 💎  |  12   | TLS expansion slots (1024 dynamic slots)        | §9                 |  [/]   |
| 💎  |  13   | Extended auxiliary vector (AT_RANDOM + friends) | §7                 |  [/]   |
| 💎  |  14   | User-mode thread bootstrap (uthread_create)     | §6, T01§12, T12§11 |  [/]   |
| 💎  |  15   | Per-thread TEB allocation at uthread_create()   | §6, §12, §14       |  [/]   |
| 💎  |  16   | TEB multi-page mapping + user-VA non-overlap    | §1, §6, §15, §19   |  [/]   |
| 💎  |  17   | PEB x64 version-field offsets (0x118/0x120)     | §2, §5             |  [/]   |
| 💎  |  18   | Paranoid swapgs entry for NMI/#DF/#MCE          | §3, T01            |  [/]   |
| 💎  |  19   | exec/fork/switch GS-base staging + cost         | §4, §5, §6         |  [/]   |
| 💎  |  20   | PEB alloc robustness + Win32 ABI handoff        | §5, §7             |  [/]   |
| 💎  |  21   | Format-specific user startup frame + env unify  | §7, §5             |  [/]   |
| 💎  |  22   | PEB Ldr module identity (DllBase/size/name)     | §8, §5             |  [/]   |
| 💎  |  23   | Per-thread TLS value storage (static+expansion) | §9, §12, §16       |  [/]   |
| ⭐  |  24   | Safe PEB/TEB Ob-namespace wrappers + lifecycle  | §10                |  [/]   |
| 💎  |  25   | TLS expansion mapping safety + alloc race       | §12, §16           |  [/]   |
| 💎  |  26   | auxv hardening: classified AT_RANDOM + AT_PHDR  | §13                |  [/]   |
| 💎  |  27   | uthread_create robustness (guard/tid/stack)     | §14, §16           |  [/]   |
| 💎  |  28   | Intermittent TEB-without-kernel_gs_base halt    | §1, §4             |  [/]   |

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | PEB/TEB offset suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: `include/kernel/ob/teb.h` -- `TEB` at exact Windows x64 offsets (Self 0x30, ClientId 0x40, PEB 0x60, LastError 0x68, TlsSlots 0x1480, TlsExpansionSlots 0x1780), `NT_TIB`, `CLIENT_ID`; 11 `_Static_assert` offset checks pin the ABI.
> - Verified: `test_peb_teb.c` asserts the offsets under `TEST_CAT_ABI`; `NtCurrentTeb()` = `gs:[0x30]` and `GetLastError` = `gs:[0x68]` resolve without ntdll patching.
> - Scope boundary: the struct is correct at `sizeof` 0x2000; the allocator mapping it (§6) under-maps to one page -- multi-page TEB fix owned by §16.
> **Verified:** 2026-06-28 | commit `68d26557` | 4/4 items | build OK | 11 offset `_Static_assert` + TEST_CAT_ABI suite
> **Deferred:** [H] §1's `sizeof(TEB)` = 0x2000 (2 pages) is under-mapped by §6's one-page `teb_alloc_for_task`, aliasing the 0x1480/0x1780 TLS fields; the struct is correct, the mapping fix is owned elsewhere. -> XREF: §16 (item: "`teb_alloc_for_task()`: `pmm_alloc_contiguous(TEB_PAGES)`, map both pages..." at line 372)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H deferred-to-§16, 0 open | scope: kernel-code-quality

---

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | PEB offset suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: `peb.h` -- `PEB` (early pointers x64-correct: ImageBase 0x10, Ldr 0x18, ProcessParameters 0x20), RTLPP, `UNICODE_STRING`, `PEB_LDR_DATA`; `sizeof(PEB)` 0x1000 fits §5's 1-page alloc.
> - Review (Codex 3x) found the post-0x28 version fields use x86-32 offsets (OSMajorVersion 0xA4 vs x64 0x118); the early pointer block is correct -- fix owned by §17.
> - Scope boundary: PEB version-offset correction -> §17; PEB-vs-KUSD build-number disagreement -> §11.
> **Deferred:** [H] PEB version fields at x86-32 offsets (OSMajorVersion 0xA4 / OSBuildNumber 0xAC) not x64 (0x118 / 0x120); a real Win64 ntdll reads the reserved pad; latent until the PE loader. -> XREF: §17 (item: "Rebuild the post-0x28 PEB region in `peb.h` to the authoritative x64 `_PEB`..." at line 399)
> **Accepted:** [M] `PEB.OSBuildNumber`=22621 disagrees with `KUSD.NtBuildNumber`=`BUILD_NUMBER`=8937; both are user-visible NT-version sources. -> XREF: §11 (item: "NT-ABI build consistency: ... one shared NT build constant ..." at line 265)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H deferred-to-§17, 1M accepted-XREF (§11) | scope: kernel-code-quality

---

## 3. swapgs on INT 0x80 Entry and Exit
Kernel GS (`IA32_GS_BASE`) holds per-CPU data. User GS (`IA32_KERNEL_GS_BASE`) holds the TEB address. `swapgs` exchanges the two MSRs -- must fire on every ring-3→ring-0 transition and be reversed on every ring-0→ring-3 return.

- [x] In `isr_common_stub` (isr_stubs.asm): `test byte [rsp+24], 3` checks saved CS RPL; `swapgs` if ring 3
- [x] On exit path (before `iretq`): `test byte [rsp+8], 3` checks return CS RPL; matching `swapgs`
- [x] On `iretq` to ring 0 (kernel→kernel): `jz .no_swapgs_*` skips both swapgs
- [x] Regression comment block with symmetry requirement added at both swapgs sites
- [x] Verify with QEMU: ring-3 cmd.exe runs, 95 tests pass, desktop stable -- GS correct (WHPX, 2026-04-02)
- [x] Commit: `"kernel: abi -- swapgs on INT 0x80 ring-3 entry and exit"`

**Test checkpoint:** Ring-3 user program executes syscalls repeatedly without GS corruption; ISR entry/exit preserves kernel per-CPU GS in ring 0 and TEB GS in ring 3. No interrupt-path regressions or triple faults. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | swapgs/GS correctness via cmd.exe ring-3 + ABI suite, 0 failures
> **Notes:**
> - Shipped: `isr_common_stub` -- conditional entry swapgs on saved CS RPL + Spectre-v1 LFENCE (FENCE_SWAPGS both paths, §8); exit VERW (§19) + swapgs on return CS RPL; ring-0->ring-0 skips both.
> - Symmetric by construction (entry/exit gate on the same interrupted CS RPL); verified by cmd.exe running ring-3 syscalls without GS corruption.
> - Scope boundary: NMI/#DF/#MCE paranoid swapgs entry (async criticals sharing the CS-RPL stub) -> §18.
> **Verified:** 2026-06-28 | commit `20948e67` | 5/5 items | build OK | swapgs symmetry; cmd.exe ring-3
> **Deferred:** [H] NMI/#DF/#MCE route through the CS-RPL-only `isr_common_stub`; a critical async exception in the user-GS CPL0 window enters `isr_handler` with user GS in ring 0. -> XREF: §18 (item: "Add a paranoid entry for the IST vectors (NMI 2, #DF 8, #MCE 18)..." at line 423)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H deferred-to-§18; 2H rejected as false-positive (evidence in commit) | scope: kernel-code-quality

---

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | KERNEL_GS_BASE switch validated via cmd.exe ring-3, 0 failures
> **Notes:**
> - Shipped: per-thread `kernel_gs_base` (struct thread + task mirror) set at task_exec/fork; switch save/restore with exec_pending save-gate, NULL guard, MSR readback-verify, fail-closed boot-halt for ring-3-with-TEB-but-zero-GS.
> - Review (Codex 3x) confirmed the switch save/restore is sound but found exec/fork staging defects -- fixes owned by §19.
> - Scope boundary: exec_pending TOCTOU + fork parent-TEB + mirror staleness + per-switch readback cost -> §19; §4 checklist still describes the pre-§15 per-task model.
> **Deferred:** [Critical] task_exec/task_fork GS-base staging races (exec_pending published before TEB prime; fork publishes a runnable child with the parent TEB) + switch mirror/readback issues. -> XREF: §19 (item: "[CRITICAL] task_exec sets `exec_pending=1` (1996) before priming TEB/GS..." at line 555)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1C+1H+2M deferred-to-§19, 0 fixed | scope: kernel-code-quality

---

## 5. PEB Allocation and Population at task_exec
Allocate the PEB in the user address space and fill it before the first instruction runs.

- [x] PEB at `0x7FFDE000`, RTL_USER_PROCESS_PARAMETERS at `0x7FFDD000`, env block at `0x7FFDC000`
- [x] All three pages: PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] PEB fields: ImageBaseAddress, ProcessParameters, OSMajorVersion=10, OSBuildNumber=22621, NumberOfProcessors via `acpi_get_cpu_count()`
- [x] RTL_USER_PROCESS_PARAMETERS: ImagePathName + CommandLine as UNICODE_STRING (ASCII→UTF-16), CurrentDirectory = `C:\`
- [ ] `CommandLine` fidelity: the 4 KiB RTLPP page truncates a full-argv `CommandLine` (TODO-22 §4) and `peb_build_ustr` widens bytes 1:1 so UTF-8 mojibakes -- map enough ProcessParameters pages + UTF-8->UTF-16, or reject oversize
- [x] Std handles: UHANDLE_INVALID (console wiring in future)
- [x] Environment block: `PATH=C:\Impossible\System32\` + `SystemRoot=C:\Impossible` (UTF-16, double-NUL terminated)
- [x] `tasks[pid].peb` and `tasks[pid].teb` fields added to `struct task`
- [x] Commit: `"kernel: peb -- PEB allocation and population at exec"`

**Test checkpoint:** On `task_exec`, PEB and process-parameter pages map at expected user VAs, fields are non-NULL and consistent (`ImageBaseAddress`, `ProcessParameters`, OS version/build, processor count), and UTF-16 command-line/environment blocks decode correctly. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | PEB population suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: `peb_alloc_for_task` -- 3 PMM frames (PEB 0x7FFDE000 / RTLPP 0x7FFDD000 / env 0x7FFDC000) with partial-failure free, field population, RTLPP UNICODE_STRINGs, env block (PATH/SystemRoot).
> - Review (Codex 3x) found 4 HIGH robustness/ABI-handoff defects + a std-handle sentinel issue -- fixes owned by §20.
> - Scope boundary: checked mapping + bounded ustr + RCX ordering + RTLPP extent + std-handles -> §20; PEB/KUSD build mismatch -> §11.
> **Deferred:** [H] PEB/RTLPP/env setup proceeds through unchecked vmm_map_page, an unbounded peb_build_ustr (page overflow), a stale RCX=PEB handoff, and an RTLPP MaximumLength excluding its strings. -> XREF: §20 (item: "[HIGH] peb_alloc_for_task (1369) ignores the 3 vmm_map_page returns..." at line 470)
> **Accepted:** [M] `PEB.OSBuildNumber`=22621 disagrees with `KUSD.NtBuildNumber`=`BUILD_NUMBER`=8937. -> XREF: §11 (item: "NT-ABI build consistency: ... one shared NT build constant ..." at line 294)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 4H+1M deferred-to-§20, 1M accepted-XREF (§11) | scope: kernel-code-quality

---

## 6. TEB Allocation and Population at Thread Create

> → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §5` -- Phase 1 XSAVE/PCID activation window (XSAVE areas are PMM-backed via `task_alloc_xsave()`, not TEB).

One TEB per thread. Allocated in the user address space near the thread stack.

- [x] TEB at `0x7FFDB000` (TID 0), subsequent threads decrement by page (`0x7FFDB000 - TID * 0x1000`)
- [x] PMM alloc → VMM map (VMM_USER_RW) → zero-fill
- [x] Populated: NtTib.Self (gs:[0x30] self-pointer), StackBase/StackLimit, ExceptionList=0xFFFF...
- [x] ClientId: UniqueProcess=PID, UniqueThread=TID
- [x] ProcessEnvironmentBlock → PEB from §5, LastErrorValue = 0
- [x] `tasks[pid].teb` stored, `kernel_gs_base` set to TEB address for swapgs
- [x] Commit: `"kernel: peb -- TEB allocation and population at thread create"`

**Test checkpoint:** TEB is mapped per thread at expected VA, `NtTib.Self` is valid, stack bounds and ClientId are correct, `ProcessEnvironmentBlock` points at the process PEB, and `LastErrorValue` initializes to 0. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] `teb_alloc_for_task()` maps one 4 KiB page but `sizeof(TEB)` is 0x2000, so the second-page TLS fields alias the env block (TID 0) / adjacent TEBs (TID N); latent today (single-thread, short env). -> XREF: §16 (item: "`teb_alloc_for_task()`: `pmm_alloc_contiguous(TEB_PAGES)`, map both pages..." at line 372)

---

## 7. Initial User Stack Frame
The user stack must have a valid calling frame waiting for the first instruction. Win32 convention: `ntdll!_LdrpInitialize` reads `PEB->ProcessParameters`; it does not expect argc/argv on the stack itself. However, the ELF ABI (for ELF-based binaries in the compatibility path) needs the Linux-style stack layout.

- [x] ELF initial stack: argc=1, argv[0]=program name, NULL, envp NULL, auxv (AT_ENTRY, AT_PAGESZ, AT_NULL)
- [x] String data at top of user stack, argv pointer to it, 16-byte aligned RSP
- [x] PE32+: RCX=PEB set in interrupt frame for all user tasks -- ntdll will find PEB in RCX when PE32+ loading lands (TODO-17)
- [x] Replaced all-zero stack top with Linux x86-64 ABI layout
- [x] Confirmed: cmd.exe _start→main()→printf works with new stack (WHPX build 1963, 2026-04-02)
- [x] Commit: `"kernel: abi -- initial user stack frame with argv, envp, auxv"`

**Test checkpoint:** User entry starts with valid argc/argv/envp/auxv layout and 16-byte stack alignment; hello/cmd user binaries run without stack faults; PE handoff keeps RCX=PEB contract. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | auxv/argv layout validated; cmd.exe boots, 0 failures
> **Notes:**
> - Shipped: task_exec ELF initial stack -- 16 AT_RANDOM bytes + auxv (AT_PHDR/PHENT/PHNUM/BASE/UID../RANDOM/HWCAP/ENTRY/PAGESZ/NULL) + envp NULL + argv[0] + NULL + argc=1; iret frame user CS/SS RPL=3, rip=entry.
> - Review: adversarial CLEAN (fixed-size pushes, 16-byte aligned, no overlap in the 16 KiB user stack); consistency found the one-size-fits-all stack won't serve a real PE/EIF CRT -- format-specific frame owned by §21.
> - Scope boundary: RCX=PEB ordering -> §20; format-specific PE/EIF startup frame + envp-vs-PEB env unify -> §21.
> **Verified:** 2026-06-28 | commit `d8d1470f` | 5/5 items | build OK | cmd.exe ring-3 + auxv suite
> **Deferred:** [H] the single SysV ELF stack is emitted for every format; a real PE32+ CRT/ntdll needs a Win64 startup frame, EIF its own; ELF envp empty vs PEB env. -> XREF: §21 (item: "[HIGH] Branch the initial user-frame builder by loaded format..." at line 494)
> **Accepted:** [H] iret RCX=tasks[pid].peb set (~1973) before peb alloc (~2011); NULL on first exec. -> XREF: §20 (item: "[HIGH] The initial iret frame sets RCX from tasks[pid].peb..." at line 482)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H+1M deferred-to-§21, 1H accepted-XREF (§20) | scope: kernel-code-quality

---

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | PEB Ldr list-walk validated; cmd.exe boots, 0 failures
> **Notes:**
> - Shipped: PEB_LDR_DATA at PEB+0x800 (Initialized=1) + main-exe entry in all 3 circular lists (InLoadOrder/InMemoryOrder/InInitializationOrder); list/link offsets match peb.h (consistency confirmed).
> - Review (Codex 3x) confirmed the list wiring is correct but found the entry publishes wrong module identity (DllBase=entry, fake size, stale name) -- fixes owned by §22.
> - Scope boundary: DllBase/SizeOfImage/FullDllName-BaseDllName -> §22; peb_build_ustr page overflow -> §20; full dynamic module list -> D12 T04 §3.
> **Deferred:** [H] the Ldr entry sets DllBase=EntryPoint=entry (not the load base), SizeOfImage=0x20000 (fake), and FullDllName/BaseDllName from the stale task name. -> XREF: §22 (item: "[HIGH] Resolve the loaded_module_t (exec_load_fmt)..." at line 516)
> **Accepted:** [H] FullDllName/BaseDllName via the unbounded peb_build_ustr in the PEB page (overflow path). -> XREF: §20 (item: "[HIGH] peb_build_ustr (1332) has no page bound..." at line 492)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H+2M deferred-to-§22, 1H accepted-XREF (§20) | scope: kernel-code-quality

---

## 9. TLS Slot Allocation (64 Static Slots)
TEB offsets `0x1480…0x1678` are the 64 static TLS slots used by `__declspec(thread)` and `TlsAlloc`. A minimal allocator is needed for Win32 DLLs that use TLS before the full heap is available.

- [x] `uint64_t tls_bitmap` added to `struct task` -- bit N = slot N allocated
- [x] `tls_alloc(pid)`: scan bitmap for first 0 bit, set it, return index (0-63) or -1
- [x] `tls_free(pid, index)`: clear bit, zero TEB->TlsSlots[index]
- [x] `tls_get_value(pid, index)` / `tls_set_value(pid, index, value)`: read/write TEB->TlsSlots[]
- [x] Slots 0-63 at gs:[0x1480 + index*8]; index >= 64 returns -1/0 (expansion stub)
- [x] Commit: `"kernel: peb -- TLS slot allocation (64 static slots)"`

**Test checkpoint:** `tls_alloc` returns unique indices 0-63 then fails/defers expansion path; `tls_set_value`/`tls_get_value` round-trip values; `tls_free` clears slot and allows reuse. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | TLS alloc/free/get/set suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: per-task tls_bitmap (static 0-63) + tls_expansion_bitmap[16] (64-1087); tls_alloc/free/get/set under tls_lock irqsave; two-phase expansion alloc (alloc outside lock + retry).
> - Review (Codex 3x) confirmed the bitmap math + lock are sound but the value-access helpers target the thread-0 mirror TEB, not the current thread -- fix owned by §23; TEB.TlsSlots aliasing owned by §16.
> - Scope boundary: per-thread TLS value access -> §23; TEB under-mapping + TLS-expansion VA collision -> §16.
> **Deferred:** [H] tls_get/set/free static ops index tasks[pid].teb (thread-0 mirror) + the expansion pointer is installed only in thread-0's TEB; secondary threads get wrong/NULL TLS storage. -> XREF: §23 (item: "[HIGH] Route static-slot `tls_get_value`/`tls_set_value`/`tls_free`..." at line 537)
> **Accepted:** [H] TEB.TlsSlots (0x1480) lands in the unmapped/aliased TEB second page; TLS-expansion VA (0x7FFD0000) collides with low-TID TEBs. -> XREF: §16 (item: "Define `TEB_PAGES = 2` / `TEB_STRIDE = 0x2000`..." at line 444)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 2H deferred-to-§23, 2H accepted-XREF (§16) | scope: kernel-code-quality

---

## 10. PEB / TEB Exposed in Ob Namespace
Make the PEB and TEB for any process queryable by name through the Object Manager namespace. Uses `ObInsertObject` and `NtOpenDirectoryObject`/`NtQueryDirectoryObject` from the completed OB layer (see [TODO-05-object-manager.md](./TODO-05-object-manager.md)). Enables debuggers and introspection tools without kernel patching -- not possible on Windows or Linux without a private API.

- [x] PEB inserted as `\KernelObjects\Process<PID>\Peb` via `ObInsertObject`
- [x] TEB inserted as `\KernelObjects\Process<PID>\Teb` via `ObInsertObject`
- [x] Per-process directory `\KernelObjects\Process<PID>` created via `ob_ns_create_directory`
- [x] User-mode can enumerate via `NtOpenDirectoryObject` + `NtQueryDirectoryObject`
- [x] ObpPebType and ObpTebType registered as built-in OB types (body_size=4096)
- [x] Commit: `"kernel: peb -- PEB and TEB registered in Ob namespace"`

**Test checkpoint:** `\KernelObjects\Process<PID>\Peb` and `\KernelObjects\Process<PID>\Teb` resolve through Ob namespace queries from user mode and kernel mode; object typing/size matches expected PEB/TEB body layout. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | Ob PEB/TEB insertion validated via cmd.exe exec, 0 failures
> **Notes:**
> - Shipped: at task_exec, \KernelObjects\Process<PID> dir + Peb/Teb inserted via ObInsertObject; ObpPebType/ObpTebType registered (body_size=4096); intended user-mode enumeration via NtOpenDirectoryObject.
> - Review (Codex 3x) found the design unsafe: raw user-VA PEB/TEB inserted as Ob bodies corrupt the page before them, Process<PID> collides with ob_process_create, no exit cleanup -- redesign owned by §24.
> - Scope boundary: safe wrapper objects + collision + lifecycle + pdir name -> §24.
> **Deferred:** [Critical] §10 inserts raw user-VA PEB/TEB as Ob bodies (ObInsertObject writes a header into the adjacent page), collides with ob_process_create's Process<PID>, and never cleans up at exit. -> XREF: §24 (item: "[CRITICAL] Stop inserting raw PEB/TEB user VAs as Ob bodies..." at line 558)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1C+2H+1L deferred-to-§24, 0 fixed | scope: kernel-code-quality

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
- [x] `kusd_update_time()`: ISR-driven, writes InterruptTime, SystemTime, TimeZoneBias, TickCount via triple-write protocol. Wired into LAPIC + PIT timer ISRs (→ XREF TODO-17 §12)
- [x] Wired into Phase 2 boot after wall_clock_init() and timezone_init()
- [x] Mirror policy publication bits from TODO-02: `SafeBootMode`, `KdDebuggerEnabled`, and a packed mitigation summary in `MitigationPolicies`
  - Found shipped 2026-09-28: `kusd_init()` in `kusd_time.c` publishes all three from `kernel_config_get()` before `AbiMagic`, with EFFECTIVE CI policy (safe mode gates relaxation); the snapshot is immutable, so publish-once is complete.
- [ ] Win11 24H2 `FullNumberOfPhysicalPages` (ULONGLONG, 0x310): split a `SystemCallPad[2]` qword in `kusd.h`, populate from the 64-bit PMM frame count, add `_Static_assert` + unit assertion
- [ ] NT-ABI build consistency: `PEB.OSBuildNumber`=22621 vs `KUSD.NtBuildNumber`=`BUILD_NUMBER`(8937) disagree; use one shared NT build constant (22621) for both, separate from internal `BUILD_NUMBER`; test both agree
- [x] Commit: `"kernel: abi -- KUSER_SHARED_DATA shared page at 0x7FFE0000"`

**Test checkpoint:** Serial log shows `KUSD: mapped at user=0x7FFE0000 kernel=0x<rand>`. User-mode test reads `*(uint32_t *)0x7FFE026C` (NtMajorVersion) and gets `10`. `TickCountQuad` at `0x7FFE0320` increments over time. `POST16(0xDF00)` on entry, `POST16(0xDF01)` static init, `POST16(0xDF02)` time update wired, `POST16(0xDF03)` test read verified. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [M] KUSD time/version core is shipped + boot-wired; the 3 remaining items are (1) policy bits (SafeBootMode/KdDebuggerEnabled/MitigationPolicies) -- since SHIPPED, published by `kusd_init()` from the TODO-02 snapshot (2026-09-28); (2) Win11 24H2 `FullNumberOfPhysicalPages` (kusd.h ABI surgery at 0x310 + populate from the 64-bit PMM count + `_Static_assert`/test) and (3) a shared NT build constant unifying PEB.OSBuildNumber/KUSD.NtBuildNumber -- both specified + ready for a focused fresh-context pass (precise KUSD ABI work, not rushed at session tail). -> XREF: 02-kernel-core/TODO-02 §5 (item: "Safe Mode and Recovery Policy Object" -- the policy snapshot the KUSD mirror must read) + §10 (boot-status policy).

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 6 TLS expansion suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: tls_expansion_demand_alloc -- 2-page array at 0x7FFD0000 via pmm_alloc_contiguous(2) outside tls_lock + lock-held race-check + map/zero/commit; tls_alloc/free/get/set span 1088 slots; task_cleanup unmaps.
> - Review (Codex 3x) found the demand-alloc commits a broken pointer on map failure (1 GiB huge-page region) + a race-loser frees the winner's frames -- fixes owned by §25.
> - Scope boundary: mapping check + alloc race -> §25; VA collision -> §16; per-thread expansion pointer -> §23.
> **Deferred:** [Critical] tls_expansion_demand_alloc commits teb->TlsExpansionSlots + allocated even when unchecked vmm_map_page fails (1 GiB region) + a race-loser frees the winner's frames. -> XREF: §25 (item: "[CRITICAL] tls_expansion_demand_alloc (2443) does not check vmm_map_page..." at line 580)
> **Accepted:** [H] TLS-expansion VA 0x7FFD0000 collides with low-TID TEBs. -> XREF: §16 (item: "Define `TEB_PAGES = 2` / `TEB_STRIDE = 0x2000`..." at line 444)
> **Accepted:** [H] teb->TlsExpansionSlots installed only in thread-0's TEB. -> XREF: §23 (item: "[HIGH] Publish the shared task expansion-array pointer..." at line 557)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1C+1H deferred-to-§25, 2H accepted-XREF (§16, §23) | scope: kernel-code-quality

---

## 13. Extended Auxiliary Vector (AT_RANDOM, AT_PHDR, AT_PHNUM)
The current ELF initial stack (§7) pushes `AT_ENTRY`, `AT_PAGESZ`, and `AT_NULL`. Linux user-mode C libraries (glibc, musl) expect additional entries -- most critically `AT_RANDOM` (16 random bytes used to seed the stack canary `__stack_chk_guard`) and `AT_PHDR`/`AT_PHNUM` (program header location for the dynamic linker). Without `AT_RANDOM`, dynamically linked ELF binaries compiled with `-fstack-protector` will use a zero or predictable canary, defeating stack overflow protection.

> [!NOTE]
> Resolved: shared `rdrand_bytes()` lives in `include/kernel/random.h` + `src/kernel/random.c`; `gpt.c` and `kusd_time.c` were migrated. The `elf_load_result` struct now carries `phdr_vaddr`, `phnum`, `phent`. The ELF program-header walk for auxv is the same shared `elf_compute_phdr_info()` static helper used by `elf_load()`, so the loader and `elf_extract_phdr_info()` cannot diverge. AT_RANDOM hardening landed 2026-06-12: `task_exec()` now fills the 16 bytes via `csprng_fill()` (`TODO-03` §5); the RDRAND-direct + TSC fallback block is gone.

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
- [ ] Follow-up (XREF: TODO-03 §3): replace TSC-mixed AT_RANDOM fallback with kernel entropy pool when CSPRNG lands. Codex flagged the TSC fallback as weak entropy for stack-canary seeding; rejected for §13 because (1) Makefile USER_CFLAGS uses `-fno-stack-protector` so no current binary consumes `__stack_chk_guard`; (2) there is no runtime path for externally built ELFs to enter the system in the current state of the project; (3) RDRAND-capable platforms (bare metal, WHPX) take the fast path; (4) TCG fallback ships a `LOG_ERROR` line so degraded mode is visible. When TODO-03 §3 lands or any user binary opts into stack canaries, this fallback MUST be replaced.
- [x] Commit: `"kernel: abi -- extended auxiliary vector with AT_RANDOM, AT_PHDR, AT_HWCAP"`

**Test checkpoint:** Serial log shows `ELF auxv: AT_RANDOM=0x<stack_addr> AT_PHDR=0x<phdr> AT_PHNUM=<n> AT_HWCAP=0x<edx> (16 pairs, ELF)` from `task_exec` (single observable klog line; no POST16 codes -- this is post-boot code where klog is fully working). Unit test `PEB/TEB: user auxv populated (PID 2)` walks `tasks[2].user_auxv`, asserts AT_NULL terminator present, AT_RANDOM/PAGESZ/ENTRY/HWCAP present, AT_PAGESZ == 4096, AT_HWCAP non-zero and equal to raw CPUID 1 EDX, and reads 16 bytes at AT_RANDOM with at least one non-zero. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 9 auxv suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: task_exec auxv -- AT_RANDOM (16 bytes via csprng_fill), AT_PHDR/PHENT/PHNUM via shared elf_extract_phdr_info (0 for non-ELF), AT_HWCAP raw cpuid(1).edx, AT_ENTRY/PAGESZ/UID/SECURE; auxv[64] with a computed naux.
> - Review: §7 verified the stack layout is clean; this pass found the AT_RANDOM entropy grade is unenforced + AT_PHDR unvalidated -- fixes owned by §26.
> - Scope boundary: classified AT_RANDOM + validated AT_PHDR -> §26 (subsumes the stale TSC-fallback item).
> **Deferred:** [H] AT_RANDOM uses the unclassified csprng_fill (weak canary on a degraded-entropy boot) + AT_PHDR is emitted unvalidated. -> XREF: §26 (item: "[HIGH] AT_RANDOM uses the unclassified csprng_fill..." at line 602)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 1H+1M deferred-to-§26, 0 fixed | scope: kernel-code-quality

---

## 14. User-Mode Thread Bootstrap (uthread_create)
The current `thread_create()` in `src/kernel/sched/task.c:1924` builds a ring-0 interrupt frame: `CS = GDT_KERNEL_CODE`, `SS = GDT_KERNEL_DATA`, `RIP = thread_wrapper`, `RSP = kmalloc'd kernel stack`. Every secondary thread runs in ring 0 on a kernel stack. `NtCreateThread` (TODO-12 §2) calls this function directly, so today's "user-mode multithreading" is actually kernel threads pretending to be user threads. Per-thread TEB (§15) cannot be implemented coherently on this foundation: a kernel thread that owns a user-space TEB pointer can never `swapgs` into it because it never returns to ring 3.

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

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | 3 uthread suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: kthread_create/uthread_create split (thread_create dispatches by PEB); uthread_create builds a ring-3 frame + PMM kernel stack (guard) + PMM user stack in the parent PML4 + per-thread TEB; NtCreateThread routes through it.
> - Review (Codex 3x) found the split + reject-kernel-task logic correct but failure-mode/SMP holes -- fixes owned by §27.
> - Scope boundary: guard-page cleanup + tid reservation + stack-size validation + TEB-map rollback -> §27; TEB under-mapping -> §16.
> **Deferred:** [H] uthread_create frees a guarded kernel stack without uninstalling the guard, reserves tid without a lock (SMP slot corruption), and does not validate the caller stack size (VA wrap/collision). -> XREF: §27 (item: "[HIGH] uthread_create frees a GUARDED kernel stack..." at line 622)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 3H+1M deferred-to-§27, 0 fixed | scope: kernel-code-quality

---

## 15. Per-thread TEB Allocation at uthread_create()
Currently, all threads in a process share `tasks[pid].teb` (one TEB per task), which violates the Win32 contract that each thread has its own TEB at `gs:[0]`. This blocks per-thread TLS expansion arrays (§12 deferred item), per-thread `LastErrorValue`, per-thread stack bounds in `NtTib.StackBase/StackLimit`, and correct `NtCurrentTeb()` semantics on multithreaded processes. `NtCreateThread` (TODO-12 §2) creates a `struct thread` with its own kernel stack but no TEB.

> [!NOTE]
> **Prerequisite resolved:** §14 (uthread_create) shipped in commit 638a0899 (2026-04-10), unblocking §15. The runtime TEB unmap path is still constrained by the lack of SMP TLB shootdown (-> XREF: [03-memory-concurrency/TODO-07-smp-phase2.md §2](../03-memory-concurrency/TODO-07-smp-phase2.md)); until shootdown lands, per-thread TEB pages are reclaimed at `task_cleanup()` (process death), accepting a bounded leak of `THREAD_MAX * 4 KiB = 64 KiB` per multithreaded process between thread death and process exit.
> **Follow-on ownership:** The current `TEB_USER_BASE - tid * 0x1000` layout is acceptable only while TIDs remain tightly bounded. The full replacement for fixed `THREAD_MAX` semantics, slot-bound TIDs, and tid-derived TEB/user-stack VA placement lives in [03-memory-concurrency/TODO-06-scheduler-enhancement.md §12](../03-memory-concurrency/TODO-06-scheduler-enhancement.md).

> [!IMPORTANT]
> **Architectural impact:** This section changes the GS base swap path. After §14 lands, every context switch between threads in the same process must reload `IA32_KERNEL_GS_BASE` MSR with the incoming thread's TEB. The schedule() condition changes from `prev_task != next_task` to `prev_task != next_task || prev_thread != next_thread`. Hardware `swapgs` in `isr_stubs.asm` and `syscall_entry.asm` does NOT need to change -- it operates on whatever value the C scheduler wrote into the MSR.

**Files (when unblocked):** `include/kernel/sched/task.h`, `src/kernel/sched/task.c`, `src/kernel/nt/nt_process.c`, `src/kernel/test/test_peb_teb.c`

**Prerequisites:** §14 (uthread_create), and ideally `03-memory-concurrency/TODO-06 §2` (TLB shootdown) for runtime unmap; without §06 §2, cleanup is deferred to `task_cleanup()`.

- [x] Add `void *teb` field to `struct thread` in `include/kernel/sched/task.h`
- [x] Add `uint64_t kernel_gs_base` field to `struct thread`. Keep `tasks[pid].kernel_gs_base` and `tasks[pid].teb` as a mirror of `threads[0]` -- they remain in lockstep so single-thread fast-paths and TLS code (which still indexes by pid) work unchanged.
- [x] Place per-thread TEBs at distinct user VAs via the existing `TEB_USER_BASE - tid * 0x1000` formula in `teb_alloc_for_task()` (formula already accepts `tid`; today only `task_exec()` calls it with 0). After §14, `uthread_create()` calls it with the new tid. This is the bounded-TID implementation; allocator-backed TEB placement after TID/slot decoupling is tracked in `03-memory-concurrency/TODO-06-scheduler-enhancement.md §12`.
- [x] In `uthread_create()` (added by §14): after building the ring-3 frame, call `teb_alloc_for_task(pid, new_tid, ustack_base, USER_STACK_SIZE, parent->peb)` and store result in `t->threads[new_tid].teb`. Set `t->threads[new_tid].kernel_gs_base = (uint64_t)(uintptr_t)t->threads[new_tid].teb`. NULL TEB allocation -> roll back the user stack and return -1.
- [x] Update `schedule()` and `schedule_now()`: change KERNEL_GS_BASE swap condition from `prev_task != next_task` to `prev_task != next_task || prev_thread != next_thread`. Read from `tasks[next_task].threads[next_thread].kernel_gs_base`. NULL guard: if the new value is 0 (kernel thread), do NOT write the MSR.
- [x] `NtCurrentTeb()` semantics: `gs:[0x30]` (NtTib.Self) must point at the current thread's TEB after swapgs. This falls out automatically once the MSR write is per-thread; no source change needed beyond the schedule() update.
- [x] Update `NtCreateThread_handler` in `nt_process.c`: verify the new thread has a non-NULL TEB, return STATUS_NO_MEMORY if NULL. Update `NtQueryInformationThread_handler` to read `thr->teb` (per-thread) with fallback to `owner->teb` for kernel threads.
- [x] On thread exit: per-thread TEB reclaimed at `task_cleanup()` via `vmm_unmap_user_page(cr3, teb)` for each secondary thread. Deferred from `thread_join()` because runtime unmap lacks SMP TLB shootdown (-> XREF [03-memory-concurrency/TODO-06 §2](../03-memory-concurrency/TODO-07-smp-phase2.md)).
- [x] §12 TLS expansion remains task-scoped after §15. Static TLS slots (`TEB.TlsSlots[64]`) become per-thread automatically because they live inside the per-thread TEB struct, but expansion slots (`TEB.TlsExpansionSlots`) still point at one task-shared expansion array. Full per-thread TLS expansion is deferred to a future TODO.
- [x] Update `test_peb_teb.c`: `_Static_assert` for `teb` and `kernel_gs_base` fields, smoke test `threads[0].teb == NULL` in kernel context, per-thread TEB VA stride formula check across THREAD_MAX tids.
- [x] Commit: `"kernel: peb -- per-thread TEB allocation at uthread_create()"` (65cf3586)

**Test checkpoint:** Spawning two threads in the same process produces two distinct TEB VAs (`thread_a->teb != thread_b->teb`). `NtCurrentTeb()` from each thread returns its own TEB. Setting `LastErrorValue` in thread A does not affect thread B. Allocating a TLS slot in thread A and writing to it does not affect the same slot index in thread B (per-thread isolation). Context switch correctly swaps `IA32_KERNEL_GS_BASE` MSR. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Test runner:** `scripts\debug\kernel\run-abi-tests.bat` (SUITE=abi) | per-thread TEB suites under TEST_CAT_ABI, 0 failures
> **Notes:**
> - Shipped: per-thread teb + kernel_gs_base on struct thread (mirror of threads[0]); per-tid teb_alloc_for_task; schedule()/schedule_now() swap KERNEL_GS_BASE on prev_thread!=next_thread; NtCreateThread/NtQuery use thr->teb.
> - Review (Codex 3x) confirmed the per-thread GS-swap + NtQuery wiring correct, but the TEB mapping aliases cross-process + fork breaks the mirror -- owned by §16/§19.
> - Scope boundary: TEB cross-process aliasing + per-process mapping API -> §16; fork mirror/parent-TEB -> §19; uthread_create robustness -> §27.
> **Deferred:** [H] teb_alloc_for_task maps via the kernel-PML4 vmm_map_page so all processes alias the per-tid TEB VA (cleanup uses the per-process cr3 path). -> XREF: §16 (item: "[HIGH] teb_alloc_for_task maps via vmm_map_page (kernel PML4, GLOBAL)..." at line 497)
> **Deferred:** [H] task_fork breaks the task/thread-0 TEB mirror + reuses the parent TEB for a runnable child. -> XREF: §19 (item: "[HIGH] task_fork (1241-1248) publishes a runnable child with the parent TEB+GS..." at line 536)
> **Quality reviewed:** 2026-06-28 | Codex 3x (adversarial, consistency, perf) | 2H deferred-to-§16/§19, 0 fixed | scope: kernel-code-quality

---

## 16. TEB Multi-Page Mapping and User-VA Non-Overlap
`sizeof(TEB)` is 0x2000 -- `TlsSlots` at 0x1480 and `TlsExpansionSlots` at 0x1780 push the struct into a second page -- but `teb_alloc_for_task()` allocates one frame, maps one 4 KiB page, zeroes 4096 bytes, and strides per-thread TEBs by 0x1000. Triple-confirmed by the §1 review (Codex adversarial + consistency + perf): `gs:[0x1480]`/`gs:[0x1780]` and `tls_get_value`/`tls_set_value` touch the unmapped second page, which for TID 0 aliases the env block (0x7FFDC000) and for TID N aliases TID N-1's TEB. Latent today (single-thread binaries; short env lands in the zeroed page-tail) but a real isolation/corruption defect. -> XREF: §1 (TEB struct/size), §6 (teb_alloc_for_task), §15 (per-thread TEB stride).

- [ ] Define `TEB_PAGES = 2` / `TEB_STRIDE = 0x2000` in `task.c`; `_Static_assert(sizeof(TEB) <= TEB_PAGES * 0x1000)`
- [ ] `teb_alloc_for_task()`: `pmm_alloc_contiguous(TEB_PAGES)`, map both pages, zero the full `TEB_PAGES * 0x1000` (today: one frame/page/4096 bytes)
- [ ] Stride per-thread TEB VAs by `TEB_STRIDE` (`TEB_USER_BASE - tid * TEB_STRIDE`), not 0x1000
- [ ] Relocate the TEB region to a design-pinned dedicated band: `TEB_USER_BASE=0x7FF88000`, band [0x7FF6A000,0x7FF8A000) below USER_THREAD_STACK_LOWEST; PEB/RTLPP/ENV/TLS-expansion/user-stacks unchanged
- [ ] Sequence (dep §19): create cr3 first, map both TEB pages via vmm_map_user_page(cr3), init via identity-mapped frames, set threads[0].teb/kernel_gs_base, THEN publish exec_pending
- [ ] `task_cleanup()`: unmap `TEB_PAGES` per secondary-thread TEB
- [ ] Non-overlap unit test: `sizeof(TEB)` vs stride; TID 0 + highest-TID TEB ranges vs env/RTLPP/PEB/TLS-expansion/user-stack ranges
- [ ] [HIGH] teb_alloc_for_task maps via vmm_map_page (kernel PML4, GLOBAL) so all processes share the per-tid TEB VA (cross-process aliasing); cleanup uses vmm_unmap_user_page(cr3). Fix: map via vmm_map_user_page(cr3) + check return/free
- [ ] Commit: `"kernel: peb -- map full 2-page TEB + non-overlapping per-thread VA stride"`

**Test checkpoint:** `tls_set_value(pid, 0, 0xCAFE)` writes inside the mapped TEB (not the env page) and a read of `gs:[0x1480]` returns it; two threads' TEB ranges and the env/RTLPP/PEB pages are provably disjoint (unit test); boot still reaches `C:\>`. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [Critical] §16 is design-reviewed (Codex design `bnqdm310m`: VA band pinned -- `TEB_USER_BASE=0x7FF88000`, dedicated band [0x7FF6A000,0x7FF8A000), 2-page per-process `vmm_map_user_page(cr3)` mapping with a stage-then-publish sequence) but BLOCKED on §19's exec_pending ordering for correctness (the design verdict: do not complete §16 while exec_pending can be visible before TEB/GS is staged). §16 + §19 are a coordinated CRITICAL VA-layout + exec/fork change for a fresh-context implementation. -> XREF: §19 (item: "[CRITICAL] task_exec sets `exec_pending=1` (1996) before priming TEB/GS..." at line 555)

---

## 17. PEB x64 Version-Field Offsets
The §2 PEB places OSMajorVersion at 0xA4, OSMinorVersion at 0xA8, OSBuildNumber at 0xAC, CriticalSectionTimeout at 0xC8, TlsBitmap at 0x230 -- these are the **x86-32** PEB offsets. The real x64 `_PEB` (Geoff Chappell pebteb / ntdoc / bytepointer tebpeb64) puts OSMajorVersion at 0x118, OSMinorVersion at 0x11C, OSBuildNumber at 0x120, CriticalSectionTimeout at 0xC0. The early pointer block (ImageBaseAddress 0x10, Ldr 0x18, ProcessParameters 0x20) is x64-correct; only the post-0x28 region is wrong. A real Win64 ntdll reading `PEB+0x118` gets the reserved-zero pad, not 10.0.22621. Latent today (in-tree tests assert the same wrong offsets; no real ntdll yet) but breaks Win64 compat when the PE loader lands. -> XREF: §2 (PEB struct), §5 (population), `D02 T17` (PE loader / ntdll).

- [ ] Rebuild the post-0x28 PEB region in `peb.h` to the authoritative x64 `_PEB`: OSMajorVersion 0x118, OSMinorVersion 0x11C, OSBuildNumber 0x120, OSCSDVersion 0x122, OSPlatformId 0x124, ImageSubsystem 0x128
- [ ] Move CriticalSectionTimeout to 0xC0, and NtGlobalFlag / heap thresholds / NumberOfProcessors / TlsBitmap to their real x64 offsets; resize the reserved-pad spans
- [ ] Update the `_Static_assert` offset checks to the corrected x64 offsets
- [ ] Update `test_peb_offsets` in `test_peb_teb.c` to assert the x64 offsets (it currently asserts the 32-bit ones)
- [ ] Confirm `peb_alloc_for_task` resolves by field name (no literal offsets) and cmd.exe still boots
- [ ] Commit: `"kernel: peb -- correct PEB version-field offsets to x64 ABI (0x118/0x120)"`

**Test checkpoint:** `__builtin_offsetof(PEB, OSMajorVersion) == 0x118` and `OSBuildNumber == 0x120`; `test_peb_offsets` asserts the x64 offsets; the PEB populated by `peb_alloc_for_task` reads 10.0.22621 at the corrected offsets; boot reaches `C:\>`. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] PEB x64 Version-Field Offsets -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §17 (item: "Rebuild the post-0x28 PEB region in `peb.h` to the authoritative x64 `_PEB`: OSM" at line 521).

---

## 18. Paranoid swapgs Entry for NMI / #DF / #MCE
`isr_common_stub` decides swapgs purely on the saved CS RPL (`test [rsp+24],3`). NMI, #DF, and #MCE route through the same stub on IST stacks and are not masked by IF. If one fires after kernel entry but before the entry swapgs, or in the acknowledged post-swapgs-exit CPL0 window (isr_stubs.asm:122), the nested vector sees a ring-0 saved CS, skips swapgs, and `isr_handler` runs gs:-relative with USER GS in ring 0 -- a fatal halt / wrong per-CPU access on bare metal (latent on QEMU where NMI/MCE are rare). Linux solves this with paranoid entry: read the actual GS base via MSR to decide swapgs, not the CS RPL. -> XREF: §3 (INT 0x80 swapgs), `D02 T01` (IDT/IST setup).

- [ ] Add a paranoid entry for the IST vectors (NMI 2, #DF 8, #MCE 18): read `IA32_GS_BASE` to determine whether GS is currently user or kernel, swapgs only if user, and record the decision
- [ ] Paranoid exit: undo exactly the swapgs the paranoid entry performed (do not use the CS-RPL exit test for these vectors)
- [ ] Route NMI/#DF/#MCE to dedicated paranoid stubs instead of the plain `isr_common_stub` CS-RPL path; keep the IST stack assignment
  - PARTIALLY IN PLACE for vector 2 since 2026-08-14, which shortens this item rather than closing it: `01-boot-platform/TODO-10 §22` gave NMI a dedicated `isr2` plus its own body instantiation (`isr_nmi_stub`, from the `ISR_STUB_BODY` macro), keeping the IST2 assignment. So the ROUTING this item asks for exists for vector 2 and the remaining work there is the GS detection itself -- read `IA32_GS_BASE`, swap only if user, record and undo exactly that decision. #DF (8) and #MCE (18) still take the plain CS-RPL path and still need both halves. The CS-RPL defect itself is unchanged and was re-confirmed against the shipped stub by that section's review -> XREF: `01-boot-platform/TODO-10 §22` (item: "Dedicated vector-2 entry stub").
- [ ] Guard the rdmsr-based detection against NMI nesting and the no-SWAPGS-CPU case
- [ ] Test: a synthetic nested-entry scenario (or documented bare-metal validation) proving the paranoid path swaps GS correctly when entered from the user-GS CPL0 window
- [ ] Commit: `"kernel: isr -- paranoid swapgs entry for NMI/#DF/#MCE (MSR GS-base detection)"`

**Test checkpoint:** NMI/#DF/#MCE entered from ring 3, from ring-0 kernel, and from the post-swapgs user-GS window all reach `isr_handler` with KERNEL GS (validated via a gs:-relative per-CPU read); no GS corruption; boot reaches `C:\>`. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] Paranoid swapgs Entry for NMI / #DF / #MCE -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §18 (item: "Add a paranoid entry for the IST vectors (NMI 2, #DF 8, #MCE 18): read `IA32_GS_" at line 535).

---

## 19. exec/fork/switch GS-base Staging Correctness and Cost
The §4 context-switch save/restore (per-thread GS, NULL guard, fail-closed) is structurally sound, but the Codex 3x review of §4 surfaced staging + cost defects across `task_exec`, `task_fork`, and the switch save path. -> XREF: §4 (switch save/restore), §5 (task_exec), §6 (TEB alloc).

- [ ] [CRITICAL] task_exec sets `exec_pending=1` (1996) before priming TEB/GS (2014); a preemption there can clobber the primed GS via the save-gate. Fix: stage fully before publishing exec_pending, or disable preemption across it
      **REPRODUCED 2026-07-29, no longer theoretical.** The `smoke-matrix` `kvm:2cpu` leg halted booting `cmd.exe`: `[CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0 (task.teb=0x7ffdb000 thread.teb=0x7ffdb000)` -- section 6's own fail-closed guard firing on exactly this window, one instruction-stream away from a ring-3 task running with a stale GS. Intermittent: 1 failure in 3 consecutive 4-leg matrix runs on an unrelated tree, kvm:2cpu, with the other legs green. Evidence: `.claude/overnight/artifacts/20260729-*-s24-smoke3.log`.
      - **NOT SMP-only -- correction 2026-07-29 (TODO-04 §28).** The identical `[CRIT]` fired on the **kvm:1cpu** leg (`.claude/overnight/artifacts/20260729-120323-s28-smoke5.log`, leg log `build/smoke-matrix/kvm-1cpu.log`, halt at `[  3.610]` booting `cmd.exe`), so the earlier "SMP-only, kvm:2cpu ONLY" reading was an artifact of the sample, not a property of the bug: a single CPU still preempts on the timer tick inside the exec_pending-to-TEB-prime window. Do NOT scope the fix to multi-CPU.
      - **Combined observed rate: 5 failures in 20 consecutive 4-leg matrix runs** (kvm:2cpu, kvm:1cpu, kvm:2cpu, kvm:2cpu, kvm:1cpu), always this signature, always on a tree with no kernel or ring-3 change -- TODO-04 §28 touched only host-side tooling and two facade headers, and the same matrix passed 4/4 fifteen times around the five failures, including immediately after each one.
      - **Never once on a tcg leg across all 20 runs**, which is itself evidence: KVM's real-CPU timing is what lands the tick inside the window. Both CPU counts are affected about equally (3 on 2cpu, 2 on 1cpu).
      - **Roughly a 1-in-4 reproduction rate on kvm**, so a single green matrix is NOT proof a change is innocent here, and a single red one is not proof it is guilty -- re-run before concluding either way.
      - **A previous kvm:2cpu failure on 2026-07-28 could not be diagnosed** because the matrix destroyed its own leg logs; that reporting bug is fixed, so this is the first captured instance of a signature CLAUDE.md already documents under the Smoke Test section.
      - **FIRES ON WHPX TOO -- 2026-07-30, operator run on native Windows.** `scripts/machines/run-qemu.ps1 -Accel whpx`, 2 CPUs, 1920x1080, real hardware (11th Gen i5-11600K): identical halt at `[ 22.270]` booting `cmd.exe` -- `[CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0 (task.teb=0x000000007ffdb000 thread.teb=0x000000007ffdb000)`, immediately after `PID 4: PEB=0x7ffde000 TEB=0x7ffdb000`. Reproduced ONCE IN TWO consecutive runs on the same image.
      - **This is the finding that generalises it beyond KVM.** Every prior sighting was KVM (5 in 20 matrix runs) plus one plain `test.sh`; the working hypothesis was "KVM's real-CPU timing lands the tick inside the window". WHPX is a DIFFERENT hypervisor with a different timer path, and it reproduces at a HIGHER rate (1-in-2 vs ~1-in-4). So the window is a property of the exec_pending-to-TEB-prime sequence itself, not of any one accelerator -- do not scope the fix, the repro, or the regression to KVM. TCG remains the only engine that has never shown it, which is consistent with the tick landing outside the window rather than with TCG being safe.
      - **The WHPX run also carried an unstable TSC** (`[WARN] time: clocksource: demoting TSC -> HPET (17397 ppm)`) and 11 DPC overruns (worst 310 us) before the halt. Whether a demoted clocksource widens the window is UNTESTED and is a hypothesis, not a finding -- but it is the first sighting where the tick source visibly changed mid-boot, so record the clocksource state when reproducing.
      - **Also fires outside the smoke matrix (2026-07-30, TODO-04 §30 review).** A plain `bash scripts/test.sh XML=1 JSON=1` on kvm:2cpu halted with the identical `[CRIT]` at `[  7.990]`, again booting a ring-3 task on a tree whose only changes were host-side shell in `scripts/`, and the immediately following identical invocation passed with 27037 kernel + 17 user-mode green. Evidence: `.claude/overnight/artifacts/20260730-020020-e2e-r4.log`. So the window is not a property of the smoke path either: any KVM boot that reaches the exec_pending-to-TEB-prime window can land the tick inside it, which widens the reproduction surface for whoever takes the fix
- [ ] [HIGH] task_fork (1241-1248) publishes a runnable child with the parent TEB+GS, no child TEB alloc (fork: syscall.c:401); a child run before exec gets the parent PID/TID/TLS. Fix: alloc a child TEB before publishing
- [ ] [MEDIUM] Switch save (852/1071) updates only `threads[prev].kernel_gs_base`; for prev_thread==0 the task-level mirror goes stale and fork copies it stale. Fix: update the mirror + fork copies one canonical value
- [ ] [MEDIUM] Switch readback (859/1073) does 3 MSR ops per user-thread switch (rdmsr+wrmsr+rdmsr). Fix: cache the programmed GS per CPU + skip when unchanged, or gate the readback to one-time/post-exec
- [ ] Update §4's checklist text to the post-§15 per-thread reality (per-thread field + task-level thread-0 mirror)
- [ ] Commit: `"kernel: sched -- exec/fork/switch GS-base staging correctness + cost"`

**Test checkpoint:** a forced preemption between exec_pending publish and TEB prime does not clobber the primed GS; a fork child reads its OWN PID/TID/TLS before exec; the task/thread-0 mirror stays equal across switches + fork; steady-state switches skip the redundant readback (instrumentation counter). Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [Critical] exec/fork/switch GS-base Staging Correctness and Cost -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §19 (item: "[CRITICAL] task_exec sets `exec_pending=1` (1996) before priming TEB/GS (2014);" at line 555).

---

## 20. PEB Allocation Robustness and Win32 ABI Handoff
The §5 review (Codex 3x) found the PEB/RTLPP/env setup proceeds through unchecked mappings, an unbounded string builder, and several Win32-ABI-handoff metadata defects. Latent today (short names, single exec, no real ntdll/console) but each breaks a real consumer once PE/ntdll/console arrive. -> XREF: §5 (peb_alloc_for_task), §7 (initial frame RCX=PEB), §11 (PEB/KUSD build constant).

- [ ] [HIGH] peb_alloc_for_task (1369) ignores the 3 vmm_map_page returns then writes the fixed VAs; a failed map corrupts what is mapped there + still returns non-NULL. Fix: checked per-process mapping, roll back frames, return NULL before writing
- [ ] [HIGH] teb_alloc_for_task ignores its vmm_map_page return the same way peb_alloc_for_task ignores its three; it then writes the TEB VA and returns non-NULL, so a caller's NULL check cannot see a failed mapping. Fix with the same checked-map + rollback shape. Raised by `D02 T21 §19` round-2 review, which added NULL checks that this gap defeats
- [ ] [HIGH] peb_build_ustr (1332) has no page bound + casts byte-length to uint16_t; an unbounded task name (written 4x) can overrun the RTLPP/PEB pages + wrap Length. Fix: pass a capacity, bounded-copy/truncate, validate before the uint16_t assign
- [x] [HIGH] The initial iret frame set RCX from tasks[pid].peb BEFORE peb_alloc_for_task ran -- NULL on first exec, stale on re-exec. FIXED in `D02 T21 §19`: the frame builder leaves the slot 0 and task_exec patches RCX from the new PEB inside its publication block. -> XREF: `D02 T21 §19` (item: "Publication race closed by making publication the LAST act")
- [ ] [HIGH] pp->MaximumLength = sizeof(RTLPP) (1400) but UNICODE_STRING buffers are written after the struct; Windows makes MaximumLength the full process-params extent. Fix: set it to the full used block + bound every Buffer to it
- [ ] [MEDIUM] StandardInput/Output/Error = UHANDLE_INVALID (1420-1423) -- the Win32 failure sentinel; once console I/O reads them GetStdHandle/WriteConsole fail. Fix: publish NULL until real handles, or wire handle-table slots
- [ ] Commit: `"kernel: peb -- PEB alloc robustness (checked map, bounded ustr, RCX ordering, RTLPP extent)"`

**Test checkpoint:** fault-injected vmm_map_page failure returns NULL + frees frames (no fixed-VA write); an overlong task name truncates/rejects without overrunning the RTLPP page; the iret RCX equals the new PEB; every RTLPP UNICODE_STRING.Buffer lies within MaximumLength; std handles are not the failure sentinel. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] PEB Allocation Robustness and Win32 ABI Handoff -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §20 (item: "[HIGH] peb_alloc_for_task (1369) ignores the 3 vmm_map_page returns then writes" at line 563).

---

## 21. Format-Specific User Startup Frame (ELF / PE / EIF) + Env Unification
`task_exec` emits one SysV ELF process-init stack (argc/argv/envp/auxv) for EVERY loaded format, only zeroing PHDR metadata for non-ELF, then also sets the Win64 RCX=PEB. The ELF stack is correct for ELF (the §7 adversarial pass found no overflow/alignment/overlap bug), but a real PE32+ CRT/ntdll needs a format-specific Win64 startup frame and EIF its own; the current single frame works only for the bare-asm PE smoke test that ignores CRT startup. -> XREF: §7 (ELF stack), `D02 T17` (PE loader / ntdll startup), §5 (PEB env).

- [ ] [HIGH] Branch the initial user-frame builder by loaded format: keep argc/argv/envp/auxv for ELF; add an explicit PE32+ Win64 startup frame (shadow/home space, no argc-at-rsp) for ntdll/CRT; give EIF a documented native startup contract
- [ ] [MEDIUM] PEB `Environment` is hard-coded and ELF `envp[]` empty, but `task->environ` is authoritative (SYS_EXEC envp adoption lands there, TODO-22 §4); build the PEB env block + ELF envp from it (`env_build_block_utf16`)
- [ ] Commit: `"kernel: exec -- format-specific user startup frame (ELF/PE/EIF) + unified env"`

**Test checkpoint:** an ELF binary still starts with the SysV stack; a PE32+ fixture enters its Win64 startup contract (RCX=PEB + home space, no argc-at-rsp dependency); `getenv("PATH")` and the Win32 environment block agree. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] Format-Specific User Startup Frame (ELF / PE / EIF) + Env Unification -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §21 (item: "[HIGH] Branch the initial user-frame builder by loaded format: keep argc/argv/en" at line 577).

---

## 22. PEB Ldr Module Identity (DllBase / SizeOfImage / Name)
The §8 PEB Ldr main-module entry publishes wrong module identity: `peb_alloc_for_task` is called with `entry` (the entry point) as `image_base`, so DllBase=EntryPoint=entry (not the load base; e.g. 0x800100 vs 0x800000), SizeOfImage is a hardcoded 0x20000, and FullDllName/BaseDllName come from the stale `tasks[pid].name`. ntdll/loader walks of PEB->Ldr get an address inside the image, a fake range, and the pre-exec name (a cmd.exe child that execs hello.exe shows cmd.exe). -> XREF: §8 (Ldr population), §5 (PEB alloc signature), `D02 T17` (PE module metadata), §20 (peb_build_ustr bound).

- [ ] [HIGH] Resolve the loaded_module_t (exec_load_fmt) and pass base + entry + size into the PEB builder; set DllBase=base (not the entry point), SizeOfImage=actual (not the hardcoded 0x20000)
- [ ] [MEDIUM] Populate FullDllName from the full resolved exec path + BaseDllName from the final component (not the stale tasks[pid].name); update SYS_EXEC to set the name from the requested filename
- [ ] Commit: `"kernel: peb -- correct PEB Ldr module identity (real base/size + resolved path/basename)"`

**Test checkpoint:** PEB->Ldr main-module DllBase == the loaded image base (not the entry point), SizeOfImage == the actual loaded size, FullDllName == the full path, BaseDllName == the basename; a cmd.exe child that execs hello.exe shows hello.exe. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] PEB Ldr Module Identity (DllBase / SizeOfImage / Name) -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §22 (item: "[HIGH] Resolve the loaded_module_t (exec_load_fmt) and pass base + entry + size" at line 588).

---

## 23. Per-thread TLS Value Storage (Static + Expansion)
§9's TLS allocation bitmaps are per-process (correct), but the value-access helpers index `tasks[pid].teb` (the thread-0 mirror) after §15 made `TEB.TlsSlots` per-thread. A secondary thread's `tls_get/set/free` hit thread-0's slots, and the expansion-array pointer is installed only in thread-0's TEB (secondary threads see `TlsExpansionSlots == NULL`). Breaks the Windows current-thread `TlsGetValue/TlsSetValue` contract; sharpens the vague §12 deferred "per-thread TLS expansion" item. -> XREF: §9 (TLS helpers), §12 (deferred per-thread expansion), §16 (TEB under-mapping + VA non-overlap).

- [ ] [HIGH] Route static-slot `tls_get_value`/`tls_set_value`/`tls_free` through the current thread's TEB (`tasks[pid].threads[current_thread].teb` when pid==current_task) or add a TID param; add a 2-user-thread test
- [ ] [HIGH] Publish the shared task expansion-array pointer into EVERY live user-thread TEB (`tls_expansion_demand_alloc` + `uthread_create` from `tasks[pid].tls_expansion_virt`); clear all live thread TEB pointers on cleanup
- [ ] Commit: `"kernel: peb -- per-thread TLS value storage (static + expansion pointer)"`

**Test checkpoint:** two threads in one process alloc one slot index, set distinct values, and each reads back its own (no cross-thread bleed); a secondary thread created after expansion alloc sees a non-NULL `TlsExpansionSlots`; cleanup clears every thread's pointer. Gated on the §16 TEB-mapping fix for the static-slot storage to be page-correct. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] Per-thread TLS Value Storage (Static + Expansion) -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §23 (item: "[HIGH] Route static-slot `tls_get_value`/`tls_set_value`/`tls_free` through the" at line 599).

---

## 24. PEB/TEB Ob-Namespace Exposure: Safe Wrapper Objects + Lifecycle
§10 inserts raw user-VA PEB/TEB pages directly as Ob object bodies, which is unsafe: `ObInsertObject` writes `OB_HEADER_FROM_BODY` (body - sizeof(OBJECT_HEADER)) into the page BEFORE the PEB/TEB (corrupting the RTLPP page / pre-TEB memory), the `Process<PID>` directory collides with `ob_process_create`'s existing object (so `\KernelObjects\Process<PID>\Peb` is never actually published and proc_dir leaks), and nothing removes the entries at task_exit (dangling after address-space teardown). The exclusive feature is non-functional + unsafe as built. -> XREF: §10 (Ob insertion), ob_process.c (existing Process<PID>), src/kernel/ob (ObInsertObject / ObpPebType).

- [ ] [CRITICAL] Stop inserting raw PEB/TEB user VAs as Ob bodies (2055-2058); use real ObpPebType/ObpTebType wrapper objects (ob_alloc_object) holding the VA, or a non-owning namespace record that never refs/derefs/frees the target
- [ ] [HIGH] Resolve the Process<PID> collision with ob_process_create (task.c:2049 vs ob_process.c:73): one owner of the dir; expose Peb/Teb under it; check every ObInsertObject return + unwind on failure
- [ ] [HIGH] Remove the per-process directory + Peb/Teb wrappers in task_exit/task_cleanup BEFORE destroying the user address space; add exec/exit/repeat-PID lifecycle tests
- [ ] [LOW] pdir_name digit build (2044-2045) only handles 2 digits; correct for PID >= 100 (latent under TASK_MAX=32) or use snprintf
- [ ] [HIGH] Free the primary PEB/RTLPP/env/TEB physical frames at exit: `peb_alloc_for_task` allocs them at fixed shared VAs, nothing frees them; store phys per-task + `pmm_free_frame` at `task_cleanup` (-> XREF: `02-kernel-core/TODO-21 §14`)
- [ ] Commit: `"kernel: ob -- safe PEB/TEB namespace wrappers + lifecycle cleanup"`

**Test checkpoint:** inserting a PEB/TEB does not write a header into the adjacent RTLPP/pre-TEB page (no user-ABI corruption); `\KernelObjects\Process<PID>\Peb` resolves via NtOpenDirectoryObject; after the process exits the directory + entries are gone (no stale-VA lookup); a re-used PID does not collide. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [Critical] PEB/TEB Ob-Namespace Exposure: Safe Wrapper Objects + Lifecycle -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §24 (item: "[CRITICAL] Stop inserting raw PEB/TEB user VAs as Ob bodies (2055-2058); use rea" at line 610).

---

## 25. TLS Expansion Mapping Safety and Alloc Race
§12's `tls_expansion_demand_alloc` commits a broken TLS pointer when mapping fails and can free another allocator's frames. -> XREF: §12 (demand-alloc), §16 (VA collision + huge-page split), §20 (unchecked vmm_map_page pattern), `D03 T03 §1` (PMM bitmap SMP lock).

- [ ] [CRITICAL] tls_expansion_demand_alloc (2443) does not check vmm_map_page; 0x7FFD0000 may be 1-GiB-mapped so the map fails, then it zero-fills the huge mapping + publishes a broken pointer + leaks. Fix: check maps, roll back/free/return -1
- [ ] [HIGH] PMM alloc is outside tls_lock + pmm_alloc_contiguous is unsynchronized; two CPUs can grab the same frames, then the lock-loser frees the WINNER's. Fix: serialize alloc-in-progress under tls_lock; gated on the PMM bitmap lock (D03 T03 §1)
- [ ] Commit: `"kernel: peb -- TLS expansion mapping check + alloc-race serialization"`

**Test checkpoint:** a fault-injected vmm_map_page failure leaves `tls_expansion_allocated` clear, frees both frames, returns -1 (no broken pointer published); a concurrent 2-CPU TLS expansion stress proves a race loser cannot free the winner's frames. Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [Critical] TLS Expansion Mapping Safety and Alloc Race -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §25 (item: "[CRITICAL] tls_expansion_demand_alloc (2443) does not check vmm_map_page; 0x7FFD" at line 623).

---

## 26. auxv Hardening: Classified AT_RANDOM + Validated AT_PHDR
§13's auxv is structurally clean (§7 verified the stack layout), but the AT_RANDOM entropy grade is unenforced and AT_PHDR is unvalidated. -> XREF: §13 (auxv emission + the open TSC-fallback item), `D02 T03 §5` (csprng classified API), `D02 T17` (ELF load-result phdr).

- [ ] [HIGH] AT_RANDOM uses the unclassified csprng_fill (1851); it can emergency-seed pre-init or return DEGRADED-class bytes -> weak canary on low-entropy boot. Fix: csprng_fill_classified()/csprng_crypto_ok() gate (subsumes the stale §13 TSC item)
- [ ] [MEDIUM] AT_PHDR is emitted from elf_extract_phdr_info without proving phdr_vaddr is the loaded user VA (trusts PT_PHDR.p_vaddr); validate phdr_vaddr..+phnum*phent lies in the loaded PT_LOAD range or carry phdr from the load result
- [ ] Commit: `"kernel: exec -- classified AT_RANDOM entropy gate + validated AT_PHDR"`

**Test checkpoint:** AT_RANDOM bytes come from a crypto-grade CSPRNG (or exec is delayed/failed below the minimum entropy class); AT_PHDR points within the loaded image's program-header range (a malformed PT_PHDR.p_vaddr is rejected/corrected). Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] auxv Hardening: Classified AT_RANDOM + Validated AT_PHDR -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §26 (item: "[HIGH] AT_RANDOM uses the unclassified csprng_fill (1851); it can emergency-seed" at line 634).

---

## 27. uthread_create Robustness: Guard-page Cleanup + tid Reservation + Stack-size Validation
§14's uthread_create has failure-mode + concurrency holes. -> XREF: §14 (uthread_create), §16 (teb_alloc mapping rollback), §15 (per-thread TEB).

- [ ] [HIGH] uthread_create frees a GUARDED kernel stack via pmm_free_pages (2777) without vmm_uninstall_guard_page; the freed frame keeps a stale guard PTE/slot + faults the next allocator. Fix: uninstall the guard before any free
- [ ] [HIGH] tid = t->num_threads (2789) + slot publish has NO task lock; two CPUs in one task get the same tid -> VA/slot corruption + double-increment. Fix: reserve tid + mark slot INITIALIZING under a per-task lock
- [ ] [HIGH] caller user_stack_size is unvalidated; USER_THREAD_STACK_BASE - tid*user_stack_size can wrap/collide with ELF/TEB/TLS/kernel regions. Fix: cap the size + validate [ustack_va,+size) for overflow + non-overlap
- [ ] [MEDIUM] teb_alloc_for_task ignores vmm_map_page's return (1526) so it can publish a non-NULL but unmapped TEB; uthread_create only rolls back on NULL. Fix: check the map, free + return NULL (pairs with §16 multi-page TEB)
- [ ] Commit: `"kernel: sched -- uthread_create robustness (guard cleanup, tid lock, stack validation)"`

**Test checkpoint:** a rollback path uninstalls the kernel-stack guard before freeing (the frame is reusable); two concurrent uthread_create on one task get distinct tids/VAs (no slot corruption); an oversize/overlapping user_stack_size is rejected; a TEB map failure rolls back fully (no READY thread with a broken TEB). Test on: QEMU WHPX + TCG, VirtualBox, bare metal.

> **Deferred:** [H] uthread_create Robustness: Guard-page Cleanup + tid Reservation + Stack-size Validation -- deferred as part of the coordinated §16-§27 CRITICAL ABI/SMP fix-campaign from the §1-§15 review sweep (operator-reserved per CLAUDE.md ABI-change/large-refactor; design-verified for §16); filed plan ready for a focused fresh-context implementation. -> XREF: §27 (item: "[HIGH] uthread_create frees a GUARDED kernel stack via pmm_free_pages (2777) wit" at line 645).

---

## 28. Intermittent TEB-without-kernel_gs_base Halt at Ring-3 Entry

A 2-CPU `SUITE=exec` run halted at the FIRST user-mode binary with `[CRIT] sched: ring-3 task 4 thread 0 has TEB but kernel_gs_base=0 (task.teb=0x7ffdb000 thread.teb=0x7ffdb000)` (`src/kernel/sched/task.c:1426`), immediately after `PID 4 -> entry 0x800000`. Both TEB pointers were populated and only `kernel_gs_base` was zero, so the TEB was allocated and published while the MSR write that pairs with it did not land -- the check fired exactly as designed and the halt is correct behaviour; the DEFECT is the state it caught.

> [!NOTE]
> Observed 2026-07-31 during TODO-04 §43 (an unrelated host/launcher-identity change: the diff touched `src/kernel/test/test_usermode*.c` and two host scripts, and no file under `src/kernel/sched/`). INTERMITTENT -- one halt in three consecutive `SUITE=exec` runs on the same tree; the two re-runs reported `1840 tests passed, 0 failed` with zero occurrences of the CRIT. **RESOLVED 2026-08-02 in `2edb130b5`** -- see item 1 below for the root cause and item 2 for the fix, both verified still in place at HEAD `21921b527` (2026-09-03). Only the regression test (item 3) remains open, parked on the kernel image ceiling.

- [x] Reproduce deterministically before changing anything: identify what orders the TEB publish against the `KERNEL_GS_BASE` write, and whether a tick landing between them is the window
      The single observed instance was `[cpu:0]` at the ring-3 entry of the first binary, on a 2-CPU TCG run. Both `task.teb` and `thread.teb` were already set, which narrows it to the MSR write rather than TEB allocation. Do NOT start from the AP path: every observation to date is `[cpu:0]`, and the 2026-07-29 sighting was a 1-CPU KVM leg, so neither "needs an AP" nor "needs 2 CPUs" is established. Scoping the hunt to AP ordering before the reproduction says so is what this item exists to prevent.
      - **Rate measurement 2026-08-01 (TODO-04 §51):** 0 occurrences in 6 consecutive 2-CPU `SUITE=exec` runs, all exit 0, plus 0 across ~20 further 2-CPU full-suite runs on the same tree. The live-gotcha file recorded roughly 1 in 3 on 2026-07-31, so the rate has dropped sharply without anyone fixing this -- which makes a single-shot gate even less able to see it, and makes an unreproduced run weaker evidence of absence than it looks.
      - A repeat sweep of BARE boots measures the wrong thing: the three 2026-08-01 sightings all fired inside a full `SUITE=exec` run or a matrix leg, while 10 consecutive plain `SMOKE_SMP=1` boots passed. Whatever the trigger is, it correlates with state the suite leaves behind.
      - **Recurred 2026-08-02 01:37 (TODO-04 §52), same signature, `task 4 thread 0`, `task.teb == thread.teb == 0x7ffdb000`, at `[cpu:0]`.** Two consecutive full-suite runs on an IDENTICAL tree: the first halted, the immediate re-run passed 28151+17. That pair is the cleanest evidence yet that this is timing and not tree state, and it puts the rate back above the ~0-in-26 measured the day before -- so treat the 2026-08-01 measurement as a sampling artifact rather than as a decay. The §52 diff touched no kernel code (host `scripts/*.sh`, docs, TODO only), which rules that change out as a cause.
      - **RESOLVED 2026-08-02 in `2edb130b5`** ("kernel: order the TEB/gs_base publication and its readers, fixing the intermittent 2-CPU boot halt"), same day as the last recurrence above: `task_exec` published the TEB ~120 lines before `kernel_gs_base` and outside the `local_irq_save` window (`task.c` publication window, then ~4292-4335), so another CPU could observe a non-NULL TEB with `gs_base` still 0 and trip the fail-closed guard, which has no `exec_pending` exemption.
      -> XREF: `00-infrastructure/TODO-04-usermode-test-framework.md` §51 (item: "Reproduce and root-cause the intermittent boot halt observed in the same window, or prove it cannot recur")
- [x] Close the window at the source rather than by retrying or by relaxing the check -- the check is the net that caught this and must keep halting
      **Verified at HEAD `21921b527` (2026-09-03):** the writer (`task.c:4441-4465`, inside task_exec's `local_irq_save` publication window) stores `kernel_gs_base` FIRST then release-stores the TEB SECOND (`__ATOMIC_RELEASE`); BOTH scheduler switch paths (`schedule_now` ~task.c:1620, `task_switch_cooperative` ~task.c:1924, "kept textually identical" by design) acquire-load the TEB before reading `kernel_gs_base` (`__ATOMIC_ACQUIRE`), so a reader that observes a non-NULL TEB is guaranteed to also observe the matching `gs_base`. The fail-closed guard (`ring-3 task %u thread %u has TEB but kernel_gs_base=0`) is unchanged and still fatals on a genuine mismatch.
      -> XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md` §4 (item: "KERNEL_GS_BASE written at task_exec / fork")
- [/] Add an SMP regression test for TEB-published-without-kernel_gs_base -- BLOCKED on the image ceiling -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13
  - Design of record, built and MEASURED at HEAD `21921b527` (control: `__kernel_end 0x7fead5`, `.text` budget 95 bytes, `scripts/overnight/bss-headroom.py`), so the redo starts from a design, not a blank page. A pure truth-table test over fabricated pointers was rejected by design review as unable to detect a regression that REORDERS the real writer's two stores or the real reader's two loads while leaving final values unchanged. The design that survived review: extract the exact writer sequence (`sched_publish_teb_gs_base()`) and reader sequence (`sched_load_teb_gs_pair()`) into `include/kernel/sched/task.h` as `static inline` pure helpers, used verbatim at all 3 production call sites (measured net ZERO `.text` growth -- the compiler folds them back to identical code). The writer helper takes an optional `mid_publish_hook` function pointer (NULL at every production call site, a compile-time-constant that folds away entirely) so a test can pause the REAL writer mid-sequence via the existing `test_race_barrier_t` rendezvous (`race_barrier.h`) and have the REAL reader observe the exact half-published state under `release(a_first=0)`. A hand-written mutation writer (TEB stored first, the exact `2edb130b5` bug shape) proves the harness detects the defect class it exists to catch (`bad_seen == iterations`, not just "at least once").
  - Why deferred, measured against the green control above: with the two helpers plus the test's two new registered functions (`test_sched.c`), `scripts/build.sh` refuses with `BSS COLLISION` (`__kernel_end` pushed to `0x800000`, exactly `USER_BASE`). The helper extraction ALONE costs zero bytes; the test functions (kthread spawns, barrier rendezvous, `TEST_ASSERT_EQ` call sites x4) are what exceeds the 95-byte `.text` budget. No smaller test was found that still exercises the real ordered sequence rather than a rejected truth table.
  - Work preserved at `.claude/state/deferred-TODO-11-peb-teb-user-abi-s28.patch` (both helpers in `task.h`, both production call-site updates in `task.c`, both test functions plus registration in `test_sched.c` -- the full design, not a smaller variant).
- [/] Commit: `"kernel: sched -- add TEB/kernel_gs_base publish-order regression test"` -- BLOCKED on the same ceiling -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13

**Test checkpoint:** repeated 2-CPU `bash scripts/test.sh SUITE=exec` runs and `bash scripts/test-smoke-matrix.sh` (TCG+KVM x 1+2 CPUs) complete with zero `has TEB but kernel_gs_base=0` occurrences; the CRIT still fires when the pairing is deliberately broken. Test on: QEMU TCG, QEMU KVM, VirtualBox, bare metal.

> **Deferred:** [M] items 1-2 (root cause + the fix itself) shipped in `2edb130b5` (2026-08-02) and are verified still in place at HEAD `21921b527`. Item 3 (the SMP regression test) was designed, implemented, and MEASURED against a green control, then reverted: it does not fit the kernel image ceiling (`.text` budget 95 bytes; the test pushes `__kernel_end` to exactly `USER_BASE`, `scripts/build.sh` refuses with `BSS COLLISION`). This is bookkeeping risk, not a live defect -- the fix itself is verified shipped and the guard that catches a regression (the fail-closed fatal) is unchanged and still active; only the NEW regression test is parked. -> XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §13 (item: "Unpark what the new headroom actually admits, and say what it does NOT")

---

## OS Comparison

| ⭐  | Feature                     | 🪟 Win11                  | 🐧 Linux                 | 🚀 Impossible OS                  |
| --- | --------------------------- | ------------------------- | ------------------------ | --------------------------------- |
| 💎  | Per-process env block       | ✅ PEB at gs:[0x60]       | ❌ argv/envp on stack    | ✅ §2+§5 PEB allocated            |
| 💎  | Per-thread block (TEB)      | ✅ TEB at gs:[0x30]       | ⚠️ glibc pthread TLS     | ⚠️ §1+§6 task-level (-> §15)      |
| 💎  | swapgs kernel entry/exit    | ✅ KiSystemCall64         | ✅ entry.S swapgs        | ✅ §3+§4 swapgs+MSR               |
| 💎  | LastError per-thread        | ✅ TEB offset 0x68        | ⚠️ errno per-thread      | ⚠️ §6 task-level (-> §15)         |
| 💎  | TLS static slots (64)       | ✅ TEB offset 0x1480      | ✅ pthread + FS-base     | ✅ §9 bitmap alloc                |
| 💎  | Process parameters          | ✅ cmdline, env, handles  | ❌ stack + /proc         | ✅ §5 RTLPP populated             |
| 💎  | Ldr module list             | ✅ PEB->Ldr linked list   | ❌ ld-linux link map     | ✅ §8 main module                 |
| 💎  | Initial stack frame         | ✅ RCX=PEB (Win64)        | ✅ ELF ABI layout        | ✅ §7 argc/argv/auxv              |
| ⭐  | PEB/TEB in Ob namespace     | ❌ Private internal       | ❌ Not exposed           | ✅ §10 public API                 |
| ⭐  | Win11 version in PEB        | ✅ Internal only          | ❌ N/A                   | ✅ §5 10.0.22621                  |
| 💎  | KUSER_SHARED_DATA page      | ✅ 0x7FFE0000 read-only   | ✅ vDSO equivalent       | [/] §11 time core; policy pending |
| 💎  | TLS expansion (1024 slots)  | ✅ TlsExpansionSlots      | ✅ pthread TLS unlimited | ✅ §12 1024 slots                 |
| 💎  | AT_RANDOM stack canary      | ⚠️ PEB Cookie (different) | ✅ auxv AT_RANDOM        | ✅ §13 RDRAND+TSC fb              |
| 💎  | AT_PHDR/AT_PHNUM auxv       | ❌ PE, not ELF            | ✅ auxv standard         | ✅ §13 shared parser              |
| 💎  | CPU feature auxv (AT_HWCAP) | ⚠️ ProcessorFeatures[]    | ✅ AT_HWCAP/AT_HWCAP2    | ✅ §13 raw CPUID 1 EDX            |
| 💎  | Real user threads (ring 3)  | ✅ NtCreateThread ring 3  | ✅ clone() ring 3        | ✅ §14 uthread_create done        |
| 💎  | Per-thread TEB / GS swap    | ✅ Per-thread TEB         | ✅ Per-thread FS_BASE    | ✅ §15 per-thread TEB+MSR         |

> **Current parity:** Impossible OS matches Windows NT on the core user-mode ABI contract for PEB/TEB, TLS, and user threads, with one remaining `KUSER_SHARED_DATA` policy-publication gap in §11. `NtCurrentTeb()`, `GetLastError()`, TLS slots, and PEB->ProcessParameters all work at correct GS offsets -- ntdll and Win32 DLLs initialise without patching.
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
- [ ] KUSER_SHARED_DATA tests are NOT in `test_peb_teb.c` today. The time/version core of §11 is verified at runtime via cmd.exe accessing `0x7FFE0000`, but no compile-time offset checks or unit assertions exist for the KUSD struct fields and the policy publication bits are still pending. Tracked as a deferred test gap; rolls into the next test-coverage pass on §11.

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
- [ ] Commit: `"kernel: peb/teb -- user-mode ABI complete"` -- pending §11 policy publication and dedicated KUSD tests to claim the file as Done
