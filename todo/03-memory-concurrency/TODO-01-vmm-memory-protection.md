# TODO-01 -- VMM Memory Protection & Diagnostics

> **Goal:** PMM, VMM, heap, swap, and mmap are complete. This TODO hardens and extends the memory model: Win32-compatible `mprotect` / `VirtualAlloc` / `VirtualFree`, demand paging (reserve vs. commit), W^X enforcement, `NtQueryVirtualMemory`, and a full allocator safety tier -- build-time lint, canaries, leak detector, and PMM statistics.

> [!IMPORTANT]
> **Memory rule:** `kmalloc` is for small kernel structs ≤ 4 KB only. Use `pmm_alloc_contiguous()` for every buffer that can exceed 4 KB (fonts, images, file data, DMA regions, network reassembly). Violating this crashes the 2 MiB heap silently. See [`freestanding-kernel-code.mdc`](../../.cursor/rules/freestanding-kernel-code.mdc).

## Inputs

- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h)
- [`include/kernel/mm/pmm.h`](../../include/kernel/mm/pmm.h)
- [`include/kernel/mm/heap.h`](../../include/kernel/mm/heap.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c)
- [`src/kernel/mm/heap.c`](../../src/kernel/mm/heap.c)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`scripts/build.sh`](../../scripts/build.sh)
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` -- EFER.NXE and CR4 hardening must be active before §1–§3 can rely on NX bits
- → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §8` -- PAT MSR AP synchronization; vmm_map_mmio_uc() cache policy depends on consistent PAT MSR across all CPUs
- → XREF: `02-kernel-core/TODO-04-peb-teb-user-abi.md §6` -- TEB and stack bounds required for §1 guard page placement
- → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md` -- syscall wiring for `NtProtectVirtualMemory`, `NtAllocateVirtualMemory`, `NtQueryVirtualMemory`, and the `VirtualAlloc` family
- → XREF: `01-boot-platform/TODO-01-uefi-hardening-secureboot.md §10` -- UEFI W^X (firmware runtime pages); §3 policy applies there too

## Outcome

- `mprotect()` / `NtProtectVirtualMemory` update PTE R/W and NX bits for arbitrary virtual ranges; PROT_NONE guard pages below stack bounds turn overflows into catchable page faults.
- MEM_RESERVE allocates virtual address space without backing frames; MEM_COMMIT zero-fills on first access; `NtAllocateVirtualMemory` accepts both flags per the Win32 contract.
- W^X is enforced in the VMM: no mapping may be simultaneously writable and executable; `mprotect(PROT_WRITE | PROT_EXEC)` is rejected at PTE update time with no bypass.
- `NtQueryVirtualMemory(MEMORY_BASIC_INFORMATION)` returns correct `State`, `Protect`, and `Type` for any virtual address range, enabling the Win32 `VirtualQuery` range-walk pattern.
- `VirtualAlloc` / `VirtualFree` / `VirtualProtect` / `VirtualQuery` Win32 wrappers route through the native NT memory API.
- The build fails on any bare `kmalloc` call without a `/* kmalloc OK: */` annotation; all oversized call-sites have been migrated to `pmm_alloc_contiguous`.
- Tail canaries on every `kmalloc` block are validated at `kfree`; double-free writes a poison pattern that panics on re-free.
- Debug builds accumulate an allocation table keyed by caller PC; `memleak` shell command dumps live un-freed entries.
- `meminfo` shell command and `mm_stats_t` surface PMM totals, free pages, and largest contiguous block.

## Implementation Order

| ⭐  | Order | Deliverable                                               | Depends On                    | Status |
| --- | :---: | --------------------------------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | `mprotect` / `NtProtectVirtualMemory` + guard pages       | TODO-04-cpu §2, TODO-04 §6    |  [ ]   |
| 💎  |   2   | W^X enforcement in VMM                                    | §1                            |  [ ]   |
| 💎  |   3   | Demand paging -- MEM_RESERVE / MEM_COMMIT                  | §1                            |  [ ]   |
| 💎  |   4   | `NtQueryVirtualMemory` -- `MEMORY_BASIC_INFORMATION`       | §1, §2, §3                    |  [ ]   |
| 💎  |   5   | `VirtualAlloc` / `VirtualFree` / `VirtualProtect` wrappers | §3, §4                       |  [ ]   |
| ⭐  |   6   | kmalloc size audit -- migrate oversized call-sites         | --                             |  [ ]   |
| ⭐  |   7   | Build-time kmalloc lint                                   | §6                            |  [ ]   |
| 💎  |   8   | PMM statistics -- `mm_stats_t` + `meminfo`                 | --                             |  [ ]   |
| 💎  |   9   | Heap canaries + double-free detection                     | --                             |  [ ]   |
| 💎  |  10   | Kernel memory leak detector                               | §9                            |  [ ]   |
| 💎  |  11   | MMIO mapping with UC attributes + HPET validation         | --                             |  [ ]   |
| 💎  |  12   | Per-process user page mapping (`vmm_map_user_page`)       | --                             |  [ ]   |

> 💎 = parity -- Windows and Linux both implement these memory management features; Impossible OS must match.
> ⭐ = exclusive -- build-time allocator lint that fails the build on unannotated bare `kmalloc` calls is not present in Windows or Linux toolchains by default.

---

## 1. `mprotect` / `NtProtectVirtualMemory` `[Opus]`

Change page permissions on an existing mapping -- essential for W^X policy, JIT compilers, and stack guards. The VMM updates PTE R/W and NX bits for the specified range without remapping pages.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-04-cpu-boot-sequencing.md §2` -- EFER.NXE must be set and `CR4.SMEP`/`CR4.SMAP` active before this section is implemented; NX-based protection has no effect without it.
> → XREF: `02-kernel-core/TODO-04-peb-teb-user-abi.md §6` -- TEB allocation is where the guard page below the stack is placed; coordinate guard page size and offset there.

- [ ] Define `PROT_NONE`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` constants in `include/kernel/mm/vmm.h`
- [ ] Implement `vmm_protect(virt, size, prot)` -- walk PTEs for range; map `PROT_WRITE` → R/W=1, `PROT_EXEC` → NX=0, `PROT_NONE` → Present=0 (access fault on touch)
- [ ] `invlpg` on every modified PTE address to invalidate TLB; on SMP, IPI shootdown for other CPUs
- [ ] Wire `mprotect(addr, len, prot)` POSIX syscall → `vmm_protect()`
- [ ] Wire `NtProtectVirtualMemory(handle, &base, &size, new_protect, &old_protect)` → `vmm_protect()` (→ XREF `02-kernel-core/TODO-05-native-api-ssdt.md`)
- [ ] Allocate one PROT_NONE guard page below each thread's initial stack in `thread_create()`; page fault on guard page → deliver `EXCEPTION_STACK_OVERFLOW`
- [ ] Update `pmm_init()`, `vmm_init()`, `heap_init()` to return `boot_result_t` instead of `void` -- moved from TODO-01 §8
- [ ] Commit: `"mm: mprotect / NtProtectVirtualMemory + stack guard pages"`

## 2. W^X Enforcement `[Opus]`

Disallow write-and-execute simultaneously on any mapping. Enforced in the VMM so no user-mode or kernel path can bypass it -- a hard kernel-level security invariant.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

- [ ] Add W^X check in `vmm_protect()`: if `(prot & PROT_WRITE) && (prot & PROT_EXEC)` → return `STATUS_INVALID_PAGE_PROTECTION`; log `[VMM] W^X violation rejected: addr=0x%lx prot=0x%x`
- [ ] Apply same guard in `vmm_alloc_region()` and in the `NtAllocateVirtualMemory` protect parameter path
- [ ] Reject `PAGE_EXECUTE_READWRITE` at the Win32 wrapper layer (§5) before it reaches the NT layer
- [ ] Serial log at VMM init: `[VMM] W^X policy: active`
- [ ] Commit: `"mm: W^X enforcement -- hard reject write+exec on any mapping"`

## 3. Demand Paging -- MEM_RESERVE / MEM_COMMIT `[Opus]`

Reserve virtual address space without backing frames; commit pages on-demand with zero-fill on first access -- the core of the Win32 `VirtualAlloc` model and a prerequisite for large address space consumers.

> [!NOTE]
> **Current VMM state (2026-04-04):** `vmm_set_user_page()` now auto-splits 2 MiB huge pages on demand and propagates User bit at all 4 levels, so user-mode memory can be at ANY address. However, per-process PML4s currently share the kernel's physical frames (identity-mapped clones). True process isolation requires `NtAllocateVirtualMemory` to allocate unique physical pages per process and map them into the per-process PML4. This section must implement that physical isolation -- without it, all processes see each other's data at the same virtual addresses.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

- [ ] Add `VMM_STATE_RESERVED` region state: VMA entry exists in the region tree but no PTEs allocated; `State = MEM_RESERVE` in `NtQueryVirtualMemory` results
- [ ] Extend `vmm_alloc_region()` to accept `MEM_RESERVE` and `MEM_COMMIT` flags
- [ ] `MEM_COMMIT` path: mark region committed; zero-fill backing frames on first access (page fault handler checks if faulting address is in a committed region → allocate frame + zero + map PTE → retry)
- [ ] `MEM_RESERVE` path: record region; page faults in a reserved-but-not-committed range → `EXCEPTION_ACCESS_VIOLATION` (not a silent commit)
- [ ] Wire `NtAllocateVirtualMemory(handle, &base, zero_bits, &size, type, protect)` accepting `MEM_RESERVE`, `MEM_COMMIT`, and `MEM_RESERVE|MEM_COMMIT` (→ XREF `02-kernel-core/TODO-05-native-api-ssdt.md`)
- [ ] Wire `NtFreeVirtualMemory(handle, &base, &size, MEM_RELEASE)` → unmap committed pages and remove reservation
- [ ] **Per-process physical isolation:** `vmm_create_user_pml4()` must allocate unique physical pages per process instead of cloning the kernel's identity-mapped frames. Currently all processes share the same physical pages at the same virtual addresses -- process A can read/write process B's data. `MEM_COMMIT` must allocate fresh zero-filled frames from PMM and map them into the per-process PML4 only. Prerequisite for multi-process Win32 (→ XREF D10/T7§4, D10/T7§7)
- [ ] **Per-process PML4 spinlock:** when multi-threaded process creation or `VirtualAlloc` from user threads calls `vmm_set_user_page()` concurrently on the same PML4, add a per-process lock to serialize PD splitting and User bit propagation. Currently safe because PML4 manipulation is task-local and single-threaded (documented in vmm.c)
- [ ] Commit: `"mm: demand paging -- MEM_RESERVE / MEM_COMMIT / NtAllocateVirtualMemory"`

## 4. `NtQueryVirtualMemory` `[Sonnet]`

Expose the VMM region state to user-mode so Win32 apps can walk their own address space and query protection, commit state, and type for any virtual address range.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

- [ ] Define `MEMORY_BASIC_INFORMATION`: `BaseAddress`, `AllocationBase`, `AllocationProtect`, `RegionSize`, `State` (`MEM_FREE` / `MEM_RESERVE` / `MEM_COMMIT`), `Protect`, `Type` (`MEM_PRIVATE` / `MEM_MAPPED` / `MEM_IMAGE`)
- [ ] Implement `vmm_query_region(virt, out_mbi)` -- find VMA containing `virt`, populate `MEMORY_BASIC_INFORMATION`; for unmapped addresses return `State=MEM_FREE`, `RegionSize` = gap to next VMA
- [ ] Wire `NtQueryVirtualMemory(handle, base, MemoryBasicInformation, buf, buf_size, &ret_len)`; return `STATUS_NOT_IMPLEMENTED` for other info classes
- [ ] Support Win32 range-walk: successive calls with `base = prev.BaseAddress + prev.RegionSize` must cover the full user address space without gaps
- [ ] Commit: `"mm: NtQueryVirtualMemory -- MEMORY_BASIC_INFORMATION"`

## 5. `VirtualAlloc` / `VirtualFree` / `VirtualProtect` Win32 Wrappers `[Sonnet]`

Thin Win32 shim layer over the NT memory API so user-mode code can use the standard Windows memory management interface directly.

**Files:** `include/kernel/win32/memory.h`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-05-native-api-ssdt.md` -- Win32 memory API surface; confirm function signatures and `PAGE_*` constant values match Windows documentation before implementing.

- [ ] Define `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE`, `PAGE_EXECUTE_READ`, `PAGE_EXECUTE_READWRITE` constants; map each to `PROT_*` combinations
- [ ] `VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect)` → `NtAllocateVirtualMemory()`; translate `MEM_RESERVE` / `MEM_COMMIT` / `MEM_RESERVE|MEM_COMMIT` and `PAGE_*` protect flags
- [ ] `VirtualFree(lpAddress, dwSize, dwFreeType)` → `NtFreeVirtualMemory()`; `MEM_RELEASE` requires `dwSize=0`
- [ ] `VirtualProtect(lpAddress, dwSize, flNewProtect, &flOldProtect)` → `NtProtectVirtualMemory()`
- [ ] `VirtualQuery(lpAddress, &mbi, dwLength)` → `NtQueryVirtualMemory()`; copy `MEMORY_BASIC_INFORMATION` to caller
- [ ] Reject `PAGE_EXECUTE_READWRITE` at this layer before reaching the NT path (W^X policy enforcement in Win32 API surface)
- [ ] Commit: `"mm: VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery Win32 wrappers"`

## 6. kmalloc Size Audit `[Sonnet]`

Audit all `kmalloc` calls across the kernel and migrate any allocation that can exceed 4 KB at runtime to `pmm_alloc_contiguous()`. Annotate all remaining legitimate small-struct calls so the build-time lint (§7) can whitelist them.

**Files:** all `*.c` under `src/`, `include/kernel/mm/heap.h`

- [ ] Run: `rg 'kmalloc' src/ --include='*.c' -l` and classify each call site
- [ ] `src/kernel/gfx/`: font data, glyph cache, pixel buffers -- migrate oversized to `pmm_alloc_contiguous()`
- [ ] `src/desktop/`: icon bitmaps, wallpaper scratch buffers -- migrate oversized
- [ ] `src/kernel/net/`: packet reassembly buffers that can exceed 4 KB -- migrate
- [ ] `src/kernel/fs/`: directory read buffers, inode caches -- migrate if dynamically large
- [ ] `src/kernel/drivers/`: all DMA staging buffers must use PMM (require contiguous physical pages)
- [ ] Add `/* kmalloc OK: <reason> */` comment to every remaining legitimate call-site (e.g., `/* kmalloc OK: task_t is ~256 B */`)
- [ ] Commit: `"mm: kmalloc size audit -- migrate oversized allocations to pmm_alloc_contiguous"`

## 7. Build-Time kmalloc Lint `[Sonnet]`

Fail the build on any `kmalloc` call that lacks a `/* kmalloc OK: */` whitelist annotation, preventing future regressions from ever reaching the repo.

**Files:** `scripts/lint-alloc.sh` (new), `scripts/build.sh`

- [ ] Create `scripts/lint-alloc.sh`: `rg 'kmalloc' src/ --include='*.c'`; skip lines matching `/* kmalloc OK:`; print `file:line` for each violation; exit non-zero on any hit
- [ ] Integrate `bash scripts/lint-alloc.sh` into `scripts/build.sh` before the compilation stage
- [ ] Verify: add a bare `kmalloc` call → build fails with clear message; add `/* kmalloc OK: test */` → build passes
- [ ] Commit: `"build: kmalloc lint -- build fails on unannotated kmalloc call"`

## 8. PMM Statistics `[Sonnet]`

Surface physical memory utilization so `meminfo`, Task Manager, and diagnostics tools can report total/free/used without digging through WinDbg or `/proc`.

**Files:** `include/kernel/mm/pmm.h`, `src/kernel/mm/pmm.c`, `src/kernel/sched/syscall.c`

- [ ] Define `mm_stats_t`: `total_frames`, `free_frames`, `used_frames`, `largest_free_block_frames`, `alloc_count`
- [ ] Implement `pmm_stats(mm_stats_t *out)` -- populate from PMM internal state without side effects
- [ ] `meminfo` shell command: print human-readable table (physical totals, heap usage, PMM region count)
- [ ] Expose via `SYS_MMSTATS` syscall for the Task Manager memory tab
- [ ] Verify boot serial log already shows `[PMM] X MiB free of Y MiB`; align format with `mm_stats_t` output
- [ ] Commit: `"mm: PMM statistics -- mm_stats_t + meminfo shell command"`

## 9. Heap Canaries + Double-Free Detection `[Sonnet]`

Catch heap buffer overruns at `kfree` time and detect double-free without any instrumentation overhead in release builds.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] Write `HEAP_CANARY` (`0xDEADC0DEDEADC0DE`) after the last byte of every `kmalloc` block; `kfree` verifies canary before freeing and panics `"heap corruption at 0x%p"` on mismatch
- [ ] Double-free detection: write `FREE_MAGIC` (`0xBAADF00DBAADF00D`) to the first 8 bytes of the block on free; `kfree` checks for this pattern first and panics `"double free at 0x%p"`
- [ ] Canary storage is transparent to callers (heap allocates `size + sizeof(canary)` internally)
- [ ] Commit: `"mm: heap tail canaries + double-free detection"`

## 10. Kernel Memory Leak Detector `[Sonnet]`

Debug-mode allocation tracker with zero overhead in release builds -- enabled by a boot param or compile flag so developers can catch leaks without a full debug recompile.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] Define `KMALLOC_DEBUG` compile-time flag; wrap `kmalloc`/`kfree` with macros capturing `__FILE__`, `__LINE__`, `__builtin_return_address(0)`, size -- compiled out entirely when flag is absent
- [ ] Store allocation records in a fixed-size static table (separate debug pool, not `kmalloc` itself)
- [ ] Implement `kmalloc_dump_leaks()` -- called at shutdown; prints all un-freed table entries with caller context
- [ ] `memleak` shell command → `kmalloc_dump_leaks()`
- [ ] Implement `kmalloc_stats(mm_stats_t *out)` -- current used bytes, peak used bytes, live allocation count (feeds §8 `SYS_MMSTATS`)
- [ ] Boot param `kmalloc_debug=1` enables tracking at runtime without recompile (→ XREF `TODO-02-boot-diagnostics.md §1` -- boot param API)
- [ ] Commit: `"mm: kmalloc leak detector (debug build) + kmalloc_stats"`

## 11. MMIO Mapping with UC Attributes + HPET Validation `[Opus]`

Implement `vmm_map_mmio()` / `MmMapIoSpace()` to create uncacheable (UC) mappings for device MMIO regions. The boot identity map uses write-back (WB) caching on all 2 MiB pages -- directly accessing MMIO through WB pages causes stale reads, data corruption, or machine check exceptions (MCE) on real hardware. Every MMIO access in the kernel (HPET, ECAM, NVMe BARs, future GPU BARs) must go through this function.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/hpet.c` (new)

> [!IMPORTANT]
> → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §2` -- HPET timer driver consumes `vmm_map_mmio()` for register access.
> → XREF: `04-drivers-hardware/TODO-02-core-driver-enhancements.md §3` -- PCIe ECAM needs `vmm_map_mmio()` with UC for config space.
> → XREF: `04-drivers-hardware/TODO-03-apic-interrupt-routing.md` -- LAPIC/IOAPIC MMIO should use UC mappings (currently works via MTRR override).
> → XREF: `02-kernel-core/TODO-19-x86-64-architecture.md §5` -- PAT configuration for WC (framebuffer) and UC (MMIO) page types.

> **Current state (2026-03-28):** The bootloader maps all 4 GiB with `0x87` (Present+Writable+User+PS) -- no PCD/PWT bits, so all pages are WB cached. LAPIC/IOAPIC work because MTRRs override those specific ranges to UC. HPET, ECAM, and other MMIO devices have no MTRR entries and crash on bare metal when accessed through WB pages. The HPET calibration path in `lapic.c` currently has a probe guard but should use a proper UC mapping instead.

- [ ] Implement `vmm_map_mmio(phys_base, size)` -- allocate 4 KiB PTEs, set `PCD=1` + `PWT=1` (UC memory type), return virtual address. Use a dedicated kernel VA region above the identity map to avoid conflicts.
- [ ] Implement `vmm_unmap_mmio(virt, size)` -- unmap and free PTEs.
- [ ] Implement `MmMapIoSpace(phys, size, cache_type)` Win32 wrapper -- routes to `vmm_map_mmio()` with cache type translation (`MmNonCached` → UC, `MmWriteCombined` → WC via PAT).
- [ ] HPET validation: read General Capabilities register via UC mapping; reject if `REV_ID == 0`, `COUNTER_CLK_PERIOD == 0`, or `COUNTER_CLK_PERIOD > 100000000` (>100ns/tick).
- [ ] HPET quirk table: static table of `{ vendor_id, device_id, quirk_flags }` for known-broken HPET implementations (AMD SB700/SB800 HPET counter freeze, Intel ICH9 64-bit read errata). Check against HPET's `VENDOR_ID` field in the capabilities register.
- [ ] Migrate HPET calibration in `lapic.c` from raw identity-mapped MMIO to `vmm_map_mmio()`.
- [ ] Migrate AHCI ABAR access to `vmm_map_mmio()` (currently identity-mapped).
- [ ] Audit all `volatile uint32_t *reg = (volatile uint32_t *)(uintptr_t)phys_addr` patterns in drivers -- each is a candidate for `vmm_map_mmio()`.
- [ ] Boot log: `[VMM] MMIO: mapped 0x%lx (%u bytes) as UC at 0x%lx`
- [ ] Commit: `"mm: vmm_map_mmio / MmMapIoSpace -- UC MMIO mappings + HPET validation"`

---

## 12. Per-Process User Page Mapping (`vmm_map_user_page`)

Map an arbitrary PMM-allocated physical frame into a specific process PML4 at a chosen user-mode virtual address. The current `vmm_set_user_page()` only sets the User bit on an existing identity-mapped page -- it cannot map a DIFFERENT physical frame at a given VA. This blocks `uthread_create()` (per-thread user stacks) and future `VirtualAlloc(MEM_COMMIT)` which both need to place specific physical pages at process-chosen VAs.

> [!IMPORTANT]
> **Prerequisite for TODO-04 §14 (uthread_create).** Discovered during Codex design review 2026-04-10: `pmm_alloc_contiguous()` returns a physical frame, but there is no VMM API to install that frame at an arbitrary user VA in a per-process PML4. Without this, user thread stacks would silently alias the identity-mapped physical page at the target VA, corrupting arbitrary memory.

→ XREF: [`02-kernel-core/TODO-04-peb-teb-user-abi.md §14`](../02-kernel-core/TODO-04-peb-teb-user-abi.md) -- consumer (uthread_create per-thread user stack mapping)

- [ ] Implement `int vmm_map_user_page(uintptr_t cr3, uintptr_t virt, uintptr_t phys)` in `src/kernel/mm/vmm.c`:
  - Walk the 4-level page table rooted at `cr3` (not the kernel PML4 -- the process PML4)
  - Create intermediate tables (PDPT, PD, PT) as needed via `pmm_alloc_frame()`, zero-fill, set Present+Writable+User on each level
  - Install the final PTE: `phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX`
  - If a huge page (2 MiB) is encountered at the PD level, split it first via `vmm_split_huge_page()` before inserting the 4 KiB PTE
  - Return 0 on success, -1 on allocation failure
  - `invlpg` the target VA after PTE installation (local CPU only -- no SMP shootdown per TODO-06 §2 constraint)
- [ ] Implement `void vmm_unmap_user_page(uintptr_t cr3, uintptr_t virt)`:
  - Walk the process PML4, clear the PTE, free the physical frame via `pmm_free_frame()`
  - `invlpg` the target VA
  - Do NOT free intermediate page tables (they may hold other mappings)
- [ ] Add declaration to `include/kernel/mm/vmm.h`
- [ ] Zero-fill the physical frame BEFORE mapping it as User -- prevent kernel data leaking to user mode. Use a temporary kernel mapping or the identity map (the frame IS identity-mapped before the user PTE is installed)
- [ ] Unit test: allocate a frame via `pmm_alloc_frame()`, map it at a test VA via `vmm_map_user_page(kernel_pml4, test_va, frame)`, write a pattern through the identity map, read back through `test_va`, assert match. Unmap and verify the frame is freed. (No live boot calls -- uses kernel PML4 as the test target, which is safe in single-threaded test context.)
- [ ] Commit: `"mm: vmm_map_user_page -- map arbitrary phys frame into per-process PML4 at user VA"`

**Test checkpoint:** Unit test passes (pattern write through identity map, read through mapped user VA). `task_exec()` path still works (it uses `vmm_set_user_page()` for the ELF range, which is unchanged). Boot completes normally on QEMU WHPX, TCG, VirtualBox, bare metal.

---

## OS Comparison


| ⭐ | Feature                                     | 🪟 Win11                                                  | 🐧 Linux                                           | 🚀 Impossible OS                                      |
|----|---------------------------------------------|--------------------------------------------------------|-------------------------------------------------|----------------------------------------------------|
| 💎 | `VirtualProtect` / `mprotect` + guard pages | ✅ `VirtualProtect`; guard page per thread             | ✅ `mprotect(2)`; guard via `sigaltstack` +     | ⬜ §1 -- `vmm_protect()` + PROT_NONE page per       |
| 💎 | W^X enforcement on all mappings             | ⚠️ DEP (NX) enforced; `PAGE_EXECUTE_READWRITE` allowed | ✅ NX enforced; `READ_IMPLIES_EXEC` deprecated  | ⬜ §2 -- hard reject at PTE update,                 |
| 💎 | MEM_RESERVE / MEM_COMMIT demand paging      | ✅ Core Win32 contract; `VirtualAlloc(MEM_RESERVE)`    | ✅ Overcommit + anonymous zero-fill on          | ⬜ §3 -- VMM region states + zero-fill              |
| 💎 | `NtQueryVirtualMemory` / `/proc/maps`       | ✅ `VirtualQuery` → `MEMORY_BASIC_INFORMATION`         | ✅ `/proc/self/maps` text dump of VMAs          | ⬜ §4 -- `NtQueryVirtualMemory` structured query    |
| 💎 | `VirtualAlloc` / `VirtualFree` Win32 API    | ✅ Native Win32 memory management surface              | ✅ `mmap(2)` / `munmap(2)` POSIX equivalent     | ⬜ §5 -- Win32 shim → `NtAllocateVirtualMemory`     |
| ⭐ | Build-time allocator lint                   | ❌ Driver Verifier is runtime only                     | ❌ `sparse`/`smatch` external; no build-fail on | ⬜ §7 -- build fails on unannotated `kmalloc`       |
| 💎 | PMM / physical memory statistics            | ✅ `!poolused` (WinDbg), Task Manager                  | ✅ `/proc/meminfo`, `free(1)`                   | ⬜ §8 -- `mm_stats_t` + `meminfo` shell command     |
| 💎 | Heap canaries + double-free detection       | ✅ Debug heap (user-mode); kernel via                  | ✅ SLUB debug allocator (`CONFIG_SLUB_DEBUG`)   | ⬜ §9 -- tail canary on every `kmalloc`             |
| 💎 | Kernel memory leak detector                 | ✅ Driver Verifier LEAK tracking                       | ✅ `kmemleak` kernel debug option               | ⬜ §10 -- allocation table, `memleak` shell command |
| 💎 | UC MMIO mapping                             | ✅ `MmMapIoSpace` with cache type                      | ✅ `ioremap()` / `ioremap_uc()` for device      | ⬜ §11 -- `vmm_map_mmio()` + HPET quirk table       |

> **After parity items:** Impossible OS matches Windows and Linux on VirtualProtect/mprotect, demand paging, VirtualAlloc, NtQueryVirtualMemory, PMM stats, heap canaries, and leak detection. The W^X enforcement is stronger than Windows -- `PAGE_EXECUTE_READWRITE` is a hard kernel reject with no bypass path. The build-time kmalloc lint enforces correct allocator discipline at compile time rather than catching violations at runtime.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `mprotect(PROT_NONE)` on a mapped page → access faults; `mprotect(PROT_READ)` → read succeeds, write faults
- [ ] `mprotect(PROT_WRITE | PROT_EXEC)` → returns error (W^X rejection); no page marked W+X
- [ ] Stack overflow test: write past guard page → `EXCEPTION_STACK_OVERFLOW` (not silent corruption)
- [ ] `VirtualAlloc(MEM_RESERVE)` → `VirtualQuery` shows `State=MEM_RESERVE`; access faults; `VirtualAlloc(MEM_COMMIT)` → access succeeds with zero content
- [ ] `VirtualQuery` range-walk covers full user address space without gaps or infinite loop
- [ ] Add bare `kmalloc` to a source file → `build.sh` fails with lint error; add `/* kmalloc OK: test */` → build passes
- [ ] `meminfo` shell command prints physical total, free, and heap usage matching boot log figures
- [ ] Heap corruption test: overwrite past `kmalloc` allocation end → `kfree` panics with `"heap corruption"`
- [ ] Double-free test: `kfree` same pointer twice → second `kfree` panics with `"double free"`
- [ ] `memleak` command shows no un-freed entries after a clean boot (debug build)
- [ ] `vmm_map_mmio()` returns a UC-mapped virtual address; HPET register reads return valid capabilities (not 0xFFFFFFFF)
- [ ] Bare-metal boot: HPET calibration succeeds via UC mapping (no MCE); LAPIC timer runs at correct frequency
- [ ] HPET quirk table: known-broken HPET vendor IDs are skipped with a log message
- [ ] Commit: `"mm: VMM memory protection, W^X, demand paging, allocator safety tier"`
