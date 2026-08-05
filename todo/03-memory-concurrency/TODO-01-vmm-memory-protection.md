---
schema_version: 1
id: vmm-memory-protection
domain: 03-memory-concurrency
status: active
title: "TODO-01 -- VMM Memory Protection & Diagnostics"
---

# TODO-01 -- VMM Memory Protection & Diagnostics

> **Goal:** PMM, VMM, heap, swap, and mmap foundations exist. This TODO hardens and extends the memory model: Win32-compatible `mprotect` / `VirtualAlloc` / `VirtualFree`, demand paging (reserve vs. commit), W^X enforcement, `NtQueryVirtualMemory`, and a full allocator safety tier: build-time lint, canaries, leak detector, and PMM statistics. Pager, pagefile, and working-set policy are owned by `TODO-10`.

> [!IMPORTANT]
> **Memory rule:** `kmalloc` is for small kernel structs ≤ 4 KB only. Use `pmm_alloc_contiguous()` for every buffer that can exceed 4 KB (fonts, images, file data, DMA regions, network reassembly). Violating this crashes the 2 MiB heap silently. See CLAUDE.md "Freestanding Kernel -- No stdlib".

## Inputs

- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h)
- [`include/kernel/mm/pmm.h`](../../include/kernel/mm/pmm.h)
- [`include/kernel/mm/heap.h`](../../include/kernel/mm/heap.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c)
- [`src/kernel/mm/heap.c`](../../src/kernel/mm/heap.c)
- [`src/kernel/sched/syscall.c`](../../src/kernel/sched/syscall.c)
- [`scripts/build.sh`](../../scripts/build.sh)
- → XREF: `03-memory-concurrency/TODO-04-pager-reclaim-working-set.md §2,§4,§3` -- pagefile ownership, working-set trim policy, and background reclaim back the commit, lock, and fault behavior extended here
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §2` -- EFER.NXE and CR4 hardening must be active before §1–§4 can rely on NX bits
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §9` -- PAT MSR AP synchronization; vmm_map_mmio_uc() cache policy depends on consistent PAT MSR across all CPUs
- → XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md §1` -- TEB and stack bounds required for §6 guard page placement
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- syscall wiring for `NtProtectVirtualMemory`, `NtAllocateVirtualMemory`, `NtQueryVirtualMemory`, and the `VirtualAlloc` family
- → XREF: `01-boot-platform/TODO-02-uefi-hardening-secureboot.md §1` -- UEFI runtime handoff; firmware runtime page W^X policy is `01-boot-platform/TODO-27-uefi-advanced.md §3` (TODO-01 §1-§9)

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
- User stacks auto-grow on guard page fault up to a per-thread reserve limit; overflow past the limit delivers `EXCEPTION_STACK_OVERFLOW`.
- `NtLockVirtualMemory` / `mlock` pins committed pages in RAM with a per-process quota.
- Commit charge is tracked system-wide and enforced at allocation time -- allocations that would exceed physical RAM + swap are rejected deterministically (no OOM killer).
- Per-process memory counters (RSS, peak, faults) are surfaced via `GetProcessMemoryInfo` and `meminfo`.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎  |   1   | `mprotect` / `NtProtectVirtualMemory` + guard pages | 01-boot-platform/TODO-09-cpu-boot-sequencing.md §2, D02 T11 §7 |  [ ]   |
| 💎  |   2   | W^X enforcement in VMM                   | §1                                       |  [ ]   |
| 💎  |   3   | Demand paging -- MEM_RESERVE / MEM_COMMIT | §1                                       |  [ ]   |
| 💎  |   4   | `NtQueryVirtualMemory` -- `MEMORY_BASIC_INFORMATION` | §1, §2, §3                               |  [ ]   |
| 💎  |   5   | `VirtualAlloc` / `VirtualFree` / `VirtualProtect` wrappers | §3, §4                                   |  [ ]   |
| ⭐  |   6   | kmalloc size audit -- migrate oversized call-sites | --                                       |  [ ]   |
| ⭐  |   7   | Build-time kmalloc lint                  | §6                                       |  [ ]   |
| 💎  |   8   | PMM statistics -- `mm_stats_t` + `meminfo` | --                                       |  [ ]   |
| 💎  |   9   | Heap canaries + double-free detection    | --                                       |  [ ]   |
| 💎  |  10   | Kernel memory leak detector              | §9                                       |  [ ]   |
| 💎  |  11   | MMIO mapping with UC attributes + HPET validation | --                                       |  [ ]   |
| 💎  |  12   | Per-process user page mapping (`vmm_map_user_page`) | --                                       |  [/]   |
| 💎  |  13   | Auto-growing user stacks                 | §1                                       |  [ ]   |
| 💎  |  14   | NtLockVirtualMemory / mlock -- pin pages in RAM | §3                                       |  [ ]   |
| 💎  |  15   | Commit charge tracking + enforcement     | §3                                       |  [ ]   |
| 💎  |  16   | Process memory counters (`GetProcessMemoryInfo`) | §8                                       |  [ ]   |

> 💎 = parity -- Windows and Linux both implement these memory management features; Impossible OS must match.
> ⭐ = exclusive -- build-time allocator lint that fails the build on unannotated bare `kmalloc` calls is not present in Windows or Linux toolchains by default.

---

## 1. `mprotect` / `NtProtectVirtualMemory`

Change page permissions on an existing mapping -- essential for W^X policy, JIT compilers, and stack guards. The VMM updates PTE R/W and NX bits for the specified range without remapping pages.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §2` -- EFER.NXE must be set and `CR4.SMEP`/`CR4.SMAP` active before this section is implemented; NX-based protection has no effect without it.
> → XREF: `02-kernel-core/TODO-11-peb-teb-user-abi.md §1` -- TEB allocation is where the guard page below the stack is placed; coordinate guard page size and offset there.

- [ ] Define `PROT_NONE`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` constants in `include/kernel/mm/vmm.h`
- [ ] Implement `vmm_protect(virt, size, prot)` -- walk PTEs for range; map `PROT_WRITE` → R/W=1, `PROT_EXEC` → NX=0, `PROT_NONE` → Present=0 (access fault on touch)
- [ ] `invlpg` on every modified PTE address to invalidate TLB; on SMP, IPI shootdown for other CPUs
- [ ] Wire `mprotect(addr, len, prot)` POSIX syscall → `vmm_protect()`
- [ ] Wire `NtProtectVirtualMemory(handle, &base, &size, new_protect, &old_protect)` → `vmm_protect()` (→ XREF `02-kernel-core/TODO-12-native-api-ssdt.md`)
- [ ] Allocate one PROT_NONE guard page below each thread's initial stack in `thread_create()`; page fault on guard page → deliver `EXCEPTION_STACK_OVERFLOW`
- [ ] Update `pmm_init()`, `vmm_init()`, `heap_init()` to return `boot_result_t` instead of `void` -- moved from TODO-01 §8
- [ ] Bounded quarantine + retry owner for stack runs whose guard teardown was REFUSED, covering all three stack
      classes (task, thread, AP). Filed 2026-07-28 from TODO-21 §19.
      - **`vmm_uninstall_guard_page()` deliberately RETAINS the registration on failure** so a retry can restore and release it, and `task_free_kernel_stack()` returns a status so a caller can decline to clear its pointer.
      - **But no caller can hold that pointer durably**: `task.stack_pending_free` survives a refused drain only until the next successful exec republishes the slot, `task_cleanup` and the exec rollback paths clear their pointer regardless of status, and `thread_free_stacks` plus AP startup have no owner at all (retaining a raw pointer across thread-slot reuse risks a stale pointer or a double free).
      - **Needs a durable multi-entry list plus a retry driver**, and lifecycle fault injection that forces a refusal through re-exec, post-commit rollback and reap.
      - **Bounded meanwhile**: nothing unsafe is ever freed, the path needs a hijacked guard VA or an OOM in the restoring `vmm_map_page`, and each occurrence costs one run plus one of the 640 slots and logs `LOG_ERROR`.
      -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "A REFUSED guard teardown leaks its stack run")
- [ ] Commit: `"mm: mprotect / NtProtectVirtualMemory + stack guard pages"`

## 2. W^X Enforcement

Disallow write-and-execute simultaneously on any mapping. Enforced in the VMM so no user-mode or kernel path can bypass it -- a hard kernel-level security invariant.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

- [ ] Add W^X check in `vmm_protect()`: if `(prot & PROT_WRITE) && (prot & PROT_EXEC)` → return `STATUS_INVALID_PAGE_PROTECTION`; log `[VMM] W^X violation rejected: addr=0x%lx prot=0x%x`
- [ ] Apply same guard in `vmm_alloc_region()` and in the `NtAllocateVirtualMemory` protect parameter path
- [ ] Reject `PAGE_EXECUTE_READWRITE` at the Win32 wrapper layer (§5) before it reaches the NT layer
- [ ] Serial log at VMM init: `[VMM] W^X policy: active`
- [ ] Commit: `"mm: W^X enforcement -- hard reject write+exec on any mapping"`

## 3. Demand Paging -- MEM_RESERVE / MEM_COMMIT

Reserve virtual address space without backing frames; commit pages on-demand with zero-fill on first access -- the core of the Win32 `VirtualAlloc` model and a prerequisite for large address space consumers.

> [!NOTE]
> **Current VMM state (2026-04-04):** `vmm_set_user_page()` now auto-splits 2 MiB huge pages on demand and propagates User bit at all 4 levels, so user-mode memory can be at ANY address. However, per-process PML4s currently share the kernel's physical frames (identity-mapped clones). True process isolation requires `NtAllocateVirtualMemory` to allocate unique physical pages per process and map them into the per-process PML4. This section must implement that physical isolation -- without it, all processes see each other's data at the same virtual addresses.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

- [ ] Add `VMM_STATE_RESERVED` region state: VMA entry exists in the region tree but no PTEs allocated; `State = MEM_RESERVE` in `NtQueryVirtualMemory` results
- [ ] Extend `vmm_alloc_region()` to accept `MEM_RESERVE` and `MEM_COMMIT` flags
- [ ] `MEM_COMMIT` path: mark region committed; zero-fill backing frames on first access (page fault handler checks if faulting address is in a committed region → allocate frame + zero + map PTE → retry)
- [ ] `MEM_RESERVE` path: record region; page faults in a reserved-but-not-committed range → `EXCEPTION_ACCESS_VIOLATION` (not a silent commit)
- [ ] Wire `NtAllocateVirtualMemory(handle, &base, zero_bits, &size, type, protect)` accepting `MEM_RESERVE`, `MEM_COMMIT`, and `MEM_RESERVE|MEM_COMMIT` (→ XREF `02-kernel-core/TODO-12-native-api-ssdt.md`)
- [ ] Wire `NtFreeVirtualMemory(handle, &base, &size, MEM_RELEASE)` → unmap committed pages and remove reservation
- [ ] **Per-process physical isolation:** `vmm_create_user_pml4()` must allocate unique physical pages per process instead of cloning the kernel's identity-mapped frames. Currently all processes share the same physical pages at the same virtual addresses -- process A can read/write process B's data. `MEM_COMMIT` must allocate fresh zero-filled frames from PMM and map them into the per-process PML4 only. Prerequisite for multi-process Win32 and `02-kernel-core/TODO-20-eif-full-implementation.md §9` EIF dispatch-table isolation (→ XREF D10/T7§4, D10/T7§7)
- [ ] **Per-process PML4 spinlock:** when multi-threaded process creation or `VirtualAlloc` from user threads calls `vmm_set_user_page()` concurrently on the same PML4, add a per-process lock to serialize PD splitting and User bit propagation. Currently safe because PML4 manipulation is task-local and single-threaded (documented in vmm.c)
- [ ] Commit: `"mm: demand paging -- MEM_RESERVE / MEM_COMMIT / NtAllocateVirtualMemory"`

## 4. `NtQueryVirtualMemory`

Expose the VMM region state to user-mode so Win32 apps can walk their own address space and query protection, commit state, and type for any virtual address range.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/sched/syscall.c`

- [ ] Define `MEMORY_BASIC_INFORMATION`: `BaseAddress`, `AllocationBase`, `AllocationProtect`, `RegionSize`, `State` (`MEM_FREE` / `MEM_RESERVE` / `MEM_COMMIT`), `Protect`, `Type` (`MEM_PRIVATE` / `MEM_MAPPED` / `MEM_IMAGE`)
- [ ] Implement `vmm_query_region(virt, out_mbi)` -- find VMA containing `virt`, populate `MEMORY_BASIC_INFORMATION`; for unmapped addresses return `State=MEM_FREE`, `RegionSize` = gap to next VMA
- [ ] Wire `NtQueryVirtualMemory(handle, base, MemoryBasicInformation, buf, buf_size, &ret_len)`; return `STATUS_NOT_IMPLEMENTED` for other info classes
- [ ] Support Win32 range-walk: successive calls with `base = prev.BaseAddress + prev.RegionSize` must cover the full user address space without gaps
- [ ] Bug: `vmm_get_physical` 2 MiB branch tests only `VMM_FLAG_HUGE`, not PRESENT (`vmm.c`) -- a non-present huge PDE returns a bogus phys; require `(PRESENT|HUGE)` like the 1 GiB branch + a non-present-PDE test (found via TODO-23 §7)
- [ ] Commit: `"mm: NtQueryVirtualMemory -- MEMORY_BASIC_INFORMATION"`

## 5. `VirtualAlloc` / `VirtualFree` / `VirtualProtect` Win32 Wrappers

Thin Win32 shim layer over the NT memory API so user-mode code can use the standard Windows memory management interface directly.

**Files:** `include/kernel/win32/memory.h`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- Win32 memory API surface; confirm function signatures and `PAGE_*` constant values match Windows documentation before implementing.

- [ ] Define `PAGE_NOACCESS`, `PAGE_READONLY`, `PAGE_READWRITE`, `PAGE_EXECUTE`, `PAGE_EXECUTE_READ`, `PAGE_EXECUTE_READWRITE` constants; map each to `PROT_*` combinations
- [ ] `VirtualAlloc(lpAddress, dwSize, flAllocationType, flProtect)` → `NtAllocateVirtualMemory()`; translate `MEM_RESERVE` / `MEM_COMMIT` / `MEM_RESERVE|MEM_COMMIT` and `PAGE_*` protect flags
- [ ] `VirtualFree(lpAddress, dwSize, dwFreeType)` → `NtFreeVirtualMemory()`; `MEM_RELEASE` requires `dwSize=0`
- [ ] `VirtualProtect(lpAddress, dwSize, flNewProtect, &flOldProtect)` → `NtProtectVirtualMemory()`
- [ ] `VirtualQuery(lpAddress, &mbi, dwLength)` → `NtQueryVirtualMemory()`; copy `MEMORY_BASIC_INFORMATION` to caller
- [ ] Reject `PAGE_EXECUTE_READWRITE` at this layer before reaching the NT path (W^X policy enforcement in Win32 API surface)
- [ ] Commit: `"mm: VirtualAlloc / VirtualFree / VirtualProtect / VirtualQuery Win32 wrappers"`

## 6. kmalloc Size Audit

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

## 7. Build-Time kmalloc Lint

Fail the build on any `kmalloc` call that lacks a `/* kmalloc OK: */` whitelist annotation, preventing future regressions from ever reaching the repo.

**Files:** `scripts/lint-alloc.sh` (new), `scripts/build.sh`

- [ ] Create `scripts/lint-alloc.sh`: `rg 'kmalloc' src/ --include='*.c'`; skip lines matching `/* kmalloc OK:`; print `file:line` for each violation; exit non-zero on any hit
- [ ] Integrate `bash scripts/lint-alloc.sh` into `scripts/build.sh` before the compilation stage
- [ ] Verify: add a bare `kmalloc` call → build fails with clear message; add `/* kmalloc OK: test */` → build passes
- [ ] Commit: `"build: kmalloc lint -- build fails on unannotated kmalloc call"`

## 8. PMM Statistics

Surface physical memory utilization so `meminfo`, Task Manager, and diagnostics tools can report total/free/used without digging through WinDbg or `/proc`.

**Files:** `include/kernel/mm/pmm.h`, `src/kernel/mm/pmm.c`, `src/kernel/sched/syscall.c`

- [ ] Define `mm_stats_t`: `total_frames`, `free_frames`, `used_frames`, `largest_free_block_frames`, `alloc_count`
- [ ] Implement `pmm_stats(mm_stats_t *out)` -- populate from PMM internal state without side effects
- [ ] `meminfo` shell command: print human-readable table (physical totals, heap usage, PMM region count)
- [ ] Expose via `SYS_MMSTATS` syscall for the Task Manager memory tab
- [ ] Verify boot serial log already shows `[PMM] X MiB free of Y MiB`; align format with `mm_stats_t` output
- [ ] Commit: `"mm: PMM statistics -- mm_stats_t + meminfo shell command"`

## 9. Heap Canaries + Double-Free Detection

Catch heap buffer overruns at `kfree` time and detect double-free without any instrumentation overhead in release builds.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] Write `HEAP_CANARY` (`0xDEADC0DEDEADC0DE`) after the last byte of every `kmalloc` block; `kfree` verifies canary before freeing and panics `"heap corruption at 0x%p"` on mismatch
- [ ] Double-free detection: write `FREE_MAGIC` (`0xBAADF00DBAADF00D`) to the first 8 bytes of the block on free; `kfree` checks for this pattern first and panics `"double free at 0x%p"`
- [ ] Canary storage is transparent to callers (heap allocates `size + sizeof(canary)` internally)
- [ ] Commit: `"mm: heap tail canaries + double-free detection"`

## 10. Kernel Memory Leak Detector

Debug-mode allocation tracker with zero overhead in release builds -- enabled by a boot param or compile flag so developers can catch leaks without a full debug recompile.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] Define `KMALLOC_DEBUG` compile-time flag; wrap `kmalloc`/`kfree` with macros capturing `__FILE__`, `__LINE__`, `__builtin_return_address(0)`, size -- compiled out entirely when flag is absent
- [ ] Store allocation records in a fixed-size static table (separate debug pool, not `kmalloc` itself)
- [ ] Implement `kmalloc_dump_leaks()` -- called at shutdown; prints all un-freed table entries with caller context
- [ ] `memleak` shell command → `kmalloc_dump_leaks()`
- [ ] Implement `kmalloc_stats(mm_stats_t *out)` -- current used bytes, peak used bytes, live allocation count (feeds §8 `SYS_MMSTATS`)
- [ ] Boot param `kmalloc_debug=1` enables tracking at runtime without recompile (→ XREF `01-boot-platform/TODO-14-boot-diagnostics.md §2` -- boot param API)
- [ ] Commit: `"mm: kmalloc leak detector (debug build) + kmalloc_stats"`

## 11. MMIO Mapping with UC Attributes + HPET Validation

Implement `vmm_map_mmio()` / `MmMapIoSpace()` to create uncacheable (UC) mappings for device MMIO regions. The boot identity map uses write-back (WB) caching on all 2 MiB pages -- directly accessing MMIO through WB pages causes stale reads, data corruption, or machine check exceptions (MCE) on real hardware. Every MMIO access in the kernel (HPET, ECAM, NVMe BARs, future GPU BARs) must go through this function.

**Files:** `include/kernel/mm/vmm.h`, `src/kernel/mm/vmm.c`, `src/kernel/drivers/lapic.c`, `src/kernel/drivers/hpet.c` (new)

> [!IMPORTANT]
> → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §4` -- HPET timer driver consumes `vmm_map_mmio()` for register access.
> → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §1` -- PCIe ECAM needs `vmm_map_mmio()` with UC for config space.
> → XREF: `04-drivers-hardware/TODO-02-apic-interrupt-routing.md` -- LAPIC/IOAPIC MMIO should use UC mappings (currently works via MTRR override).
> → XREF: `02-kernel-core/TODO-09-x86-64-architecture.md §7` -- PAT configuration for WC (framebuffer) and UC (MMIO) page types.

> **Current state (2026-03-28):** The bootloader maps all 4 GiB with `0x87` (Present+Writable+User+PS) -- no PCD/PWT bits, so all pages are WB cached. LAPIC/IOAPIC work because MTRRs override those specific ranges to UC. HPET, ECAM, and other MMIO devices have no MTRR entries and crash on bare metal when accessed through WB pages. The HPET calibration path in `lapic.c` currently has a probe guard but should use a proper UC mapping instead.

- [ ] Implement `vmm_map_mmio(phys_base, size)` -- 4 KiB PTEs, `PCD=1`+`PWT=1` (UC), return VA via a central kernel VA allocator with reserved non-overlapping ranges (identity/mmap/PE/MMIO), replacing the minimal hand-picked windows (D01 T10 §1).
- [ ] Implement `vmm_unmap_mmio(virt, size)` -- unmap PTEs AND reclaim the VA range (minimal version leaks the VA span on unmap/failure).
- [ ] VMM-wide kernel page-table-creation lock: `get_or_create_table()` is unlocked, so two CPUs mapping a fresh `kernel_pml4` subtree race. Serialize it or pre-create the MMIO hierarchy pre-SMP (bites D01 T10 §1).
- [ ] PE loader rejects reserved kernel VA ranges: `pe_load()` accepts any file-controlled `ImageBase`, which can map over the MMIO/mmap windows. Reject overlap once the central VA allocator owns the ranges.
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
> **Prerequisite for D02 T11 §14 (`uthread_create`).** Discovered during Codex design review 2026-04-10: `pmm_alloc_contiguous()` returns a physical frame, but there is no VMM API to install that frame at an arbitrary user VA in a per-process PML4. Without this, user thread stacks would silently alias the identity-mapped physical page at the target VA, corrupting arbitrary memory.

→ XREF: [`02-kernel-core/TODO-11-peb-teb-user-abi.md §14`](../02-kernel-core/TODO-11-peb-teb-user-abi.md) -- consumer (uthread_create per-thread user stack mapping)

- [x] Implement `int vmm_map_user_page(uintptr_t cr3, uintptr_t virt, uintptr_t phys)` in `src/kernel/mm/vmm.c` -- walks per-process PML4, creates intermediate tables with User+Writable flags, auto-splits 2 MiB huge pages, installs final PTE with Present+Writable+User+NX, invlpg after. Returns 0/-1.
- [x] Implement `void vmm_unmap_user_page(uintptr_t cr3, uintptr_t virt)` -- walks PML4, extracts frame from PTE, clears PTE, pmm_free_frame(), invlpg. Does not free intermediate tables.
- [x] Add declarations to `include/kernel/mm/vmm.h`
- [x] Zero-fill the physical frame BEFORE mapping via `zero_page(phys)` (identity-mapped, so phys == kernel VA). Prevents kernel data leaking to user mode.
- [x] Unit test in `test_vmm.c`: `test_vmm_map_user_page_roundtrip` -- allocates frame, maps at 0x200000000 via kernel PML4, writes pattern through identity map, reads back through mapped VA, asserts match + vmm_get_physical resolve, unmaps, verifies PTE cleared. 5 assertions, no live boot calls.
- [x] Commit: `"mm: vmm_map_user_page -- map arbitrary phys frame into per-process PML4 at user VA"` (ce91b05a)
- [ ] Kernel-root page-table mutations do not reach existing process roots: this clone copies the kernel PD BY VALUE, so a later guard install or split is invisible under an older CR3 (consumer: `02-kernel-core/TODO-21 §19`)
      Filed 2026-07-28 from TODO-21 §19's adversarial review. A runtime `vmm_install_guard_page()` splits the kernel PD entry and clears a PTE in the new page table; a process root cloned earlier still holds the original 2 MiB huge value, so the guard is not present under that CR3 and a stack overflow crosses it silently. Needs either a live-root registry that replays kernel-PD mutations, or sharing the kernel PD by reference -- which conflicts with the per-entry User clearing this clone performs.
- [ ] Privatize cloned kernel PDs before splitting a huge page outside PD[4]: `vmm_create_user_pml4` shares PDPT[1..511] by raw pointer, so `uthread_create` PDPT[1] stacks mutate the global kernel PD. Filed from D01 T10 §8; consumer D02 T11 §14.
- [ ] Make `task_exec` fork+exec private-frame remap atomic under OOM: on `pmm_alloc_frame` failure it continues into `exec_load` and corrupts the parent image; preallocate-or-rollback and fail exec. Filed from D01 T10 §8; see TODO-05 §1 COW fork.
- [ ] In the `task_exec` private-frame remap loop, zero via `zero_page` not a scalar qword loop, and batch one TLB flush after all PTEs install instead of per-page INVLPG. Filed from D01 T10 §8 perf review.

**Test checkpoint:** Unit test passes (pattern write through identity map, read through mapped user VA). `task_exec()` path still works (it uses `vmm_set_user_page()` for the ELF range, which is unchanged). Boot completes normally on QEMU WHPX, TCG, VirtualBox, bare metal.

---

## 13. Auto-Growing User Stacks

When a thread's stack guard page is hit, expand the stack instead of killing the thread. Windows grows stacks automatically up to the PE header's `SizeOfStackReserve` (default 1 MiB reserve, 4 KiB initial commit). Linux uses `RLIMIT_STACK` (default 8 MiB) and grows on fault. Currently Impossible OS allocates a fixed 16 KiB user stack -- any overflow past the guard page is fatal.

- [ ] In the page fault handler: if faulting address is the guard page below a user stack, allocate a new frame via `vmm_map_user_page()`, install it below the current stack, move the guard page down by one page
- [ ] Track per-thread stack limit (`stack_reserve`) and current committed extent in `struct thread`
- [ ] Reject growth past `stack_reserve` -- deliver `EXCEPTION_STACK_OVERFLOW`
- [ ] Default `stack_reserve = 64 KiB` (configurable per-thread via `NtCreateThread` parameter)
- [ ] klog on each growth: `[sched] PID %u TID %u stack grown to %u KiB`
- [ ] Commit: `"mm: auto-growing user stacks -- expand on guard page fault"`

**Test checkpoint:** User binary with deep recursion grows stack beyond 16 KiB without crashing. Growth stops at `stack_reserve` and delivers `EXCEPTION_STACK_OVERFLOW`. klog shows growth events. Verify on QEMU WHPX, TCG, VBox, bare metal.

---

## 14. NtLockVirtualMemory / mlock -- Pin Pages in RAM

Pin committed pages so they cannot be paged out. Required for DMA buffers, crypto key storage, and real-time tasks. Windows provides `VirtualLock` / `NtLockVirtualMemory`; Linux provides `mlock(2)`.

- [ ] Implement `vmm_lock_pages(virt, size)` -- mark pages non-evictable in the VMM region tracker
- [ ] Implement `vmm_unlock_pages(virt, size)` -- allow eviction again
- [ ] Wire `NtLockVirtualMemory(handle, &base, &size, MAP_PROCESS)` via SSDT
- [ ] Wire `NtUnlockVirtualMemory(handle, &base, &size, MAP_PROCESS)` via SSDT
- [ ] Per-process lock limit (default 256 KiB) -- return `STATUS_WORKING_SET_QUOTA` if exceeded
- [ ] Commit: `"mm: NtLockVirtualMemory / mlock -- pin pages in RAM"`

**Test checkpoint:** Lock a page, verify it survives a working-set trim (once swap exists). Without swap: verify the lock metadata is tracked and the syscall returns STATUS_SUCCESS. Verify on QEMU WHPX, TCG, VBox, bare metal.

---

## 15. Commit Charge Tracking + Enforcement

Track system-wide committed virtual memory (sum of all MEM_COMMIT pages across all processes) against the commit limit (physical RAM + pagefile size). Prevents silent overcommit that would cause OOM crashes. Windows enforces this via the commit charge; Linux uses `vm.overcommit_memory` (default: heuristic overcommit, no hard limit).

> [!TIP]
> **Competitive edge:** Unlike Linux (which overcommits by default and relies on the OOM killer), Impossible OS tracks commit charge from day one and rejects allocations that would exceed the limit. This provides deterministic failure at allocation time rather than random process death under pressure.

- [ ] Add `g_commit_charge` (atomic uint64) and `g_commit_limit` (physical RAM + swap size) in `vmm.c`
- [ ] In `vmm_alloc_region(MEM_COMMIT)`: `atomic_add(&g_commit_charge, pages)`. If result exceeds `g_commit_limit`, undo and return `STATUS_COMMITMENT_LIMIT`
- [ ] In `vmm_free_region()`: `atomic_sub(&g_commit_charge, pages)`
- [ ] Expose via `NtQuerySystemInformation(SystemPerformanceInformation)` -- `CommittedPages`, `CommitLimit`
- [ ] Surface in `meminfo` shell command: `Commit: X / Y MiB (Z%)`
- [ ] Commit: `"mm: commit charge tracking -- deterministic alloc rejection at commit limit"`

**Test checkpoint:** Allocate until commit limit reached -- next `VirtualAlloc(MEM_COMMIT)` returns NULL / `STATUS_COMMITMENT_LIMIT`. `meminfo` shows commit charge increasing with allocations. Verify on QEMU WHPX, TCG, VBox, bare metal.

---

## 16. Process Memory Counters (`GetProcessMemoryInfo`)

Expose per-process memory statistics: working set size, peak working set, page fault count (minor/major split), private bytes. Required for Task Manager, performance monitoring, and process diagnostics. Windows provides `GetProcessMemoryInfo` / `NtQueryInformationProcess(ProcessVmCounters)`; Linux provides `/proc/PID/status` (VmRSS, VmPeak) plus `getrusage` `ru_minflt`/`ru_majflt`.

> [!NOTE]
> → XREF: `02-kernel-core/TODO-21-process-model-extensions.md §8` (item: "`getrusage(RUSAGE_SELF)` helper") owns the times + I/O + context-switch accounting fields. This section is the SOLE owner of the VM/fault fields: page-fault counts, minor/major classification, and residency-based working set. §8's `getrusage` helper leaves `ru_minflt`/`ru_majflt`/RSS zero until this section populates them.

- [ ] Add counters to `struct task`: `page_fault_count`, `minor_faults`, `major_faults`, `working_set_pages`, `peak_working_set_pages`, `private_pages` (all `_Atomic uint64_t`)
- [ ] In the `#PF` handler: increment `page_fault_count`, and classify `minor_faults` (backing frame already resident) vs `major_faults` (frame had to be sourced) -- feeds `ru_minflt`/`ru_majflt`
- [ ] Track `working_set_pages` by actual residency transitions (page mapped-in / evicted), not commit/decommit; track `peak_working_set_pages` as the high-water max
- [ ] Wire `NtQueryInformationProcess(ProcessVmCounters)` -- return `VM_COUNTERS` struct
- [ ] Wire `GetProcessMemoryInfo()` Win32 wrapper (psapi.h style)
- [ ] Populate the VM fields of §8's `getrusage(RUSAGE_SELF)` (`ru_minflt`/`ru_majflt`/`ru_maxrss`) from these counters (→ XREF `02-kernel-core/TODO-21-process-model-extensions.md §8`)
- [ ] Surface in `meminfo` shell command per-process: `PID N: RSS X KiB, Peak Y KiB, min/maj F/G`
- [ ] Commit: `"mm: process memory counters -- GetProcessMemoryInfo + VM_COUNTERS"`

**Test checkpoint:** After booting, `meminfo` shows per-process RSS and minor/major fault counts. Page fault count increases on demand-paged access; a first-touch fault counts as major, a re-map of a resident frame as minor. Peak working set is >= current. Verify on QEMU WHPX, TCG, VBox, bare metal.

---

## OS Comparison


| ⭐  | Feature                 | 🪟 Win11              | 🐧 Linux             | 🚀 Impossible OS         |
| --- | ----------------------- | --------------------- | -------------------- | ------------------------ |
| 💎  | Page protection + guard | ✅ VirtualProtect     | ✅ mprotect(2)       | ⬜ §1 vmm_protect        |
| 💎  | W^X enforcement         | ⚠️ DEP; RWX allowed    | ✅ NX enforced       | ⬜ §2 hard reject        |
| 💎  | Demand paging           | ✅ RESERVE/COMMIT     | ✅ overcommit+zero   | ⬜ §3 region states      |
| 💎  | Query virtual memory    | ✅ VirtualQuery       | ✅ /proc/maps        | ⬜ §4 NtQueryVirtualMem  |
| 💎  | VirtualAlloc API        | ✅ native Win32       | ✅ mmap/munmap       | ⬜ §5 Win32 shim         |
| ⭐  | Build-time alloc lint   | ❌ runtime only       | ❌ external tools    | ⬜ §7 build-fail lint    |
| 💎  | PMM statistics          | ✅ Task Manager       | ✅ /proc/meminfo     | ⬜ §8 mm_stats_t         |
| 💎  | Heap canaries           | ✅ debug heap         | ✅ SLUB debug        | ⬜ §9 tail canary        |
| 💎  | Memory leak detector    | ✅ Driver Verifier    | ✅ kmemleak          | ⬜ §10 memleak cmd       |
| 💎  | UC MMIO mapping         | ✅ MmMapIoSpace       | ✅ ioremap_uc        | ⬜ §11 vmm_map_mmio      |
| 💎  | Per-process page map    | ✅ ZwMapViewOfSection | ✅ do_mmap PTE walk  | ✅ §12 done              |
| 💎  | Auto-growing stacks     | ✅ PE StackReserve    | ✅ RLIMIT_STACK      | ⬜ §13 guard page expand |
| 💎  | Pin pages (mlock)       | ✅ VirtualLock        | ✅ mlock(2)          | ⬜ §14 NtLockVirtualMem  |
| ⭐  | Commit charge tracking  | ✅ hard commit limit  | ⚠️ overcommit default | ⬜ §15 deterministic     |
| 💎  | Process memory counters | ✅ GetProcessMemInfo  | ✅ /proc/PID/status  | ⬜ §16 VM_COUNTERS       |

> Parity: matches Win11+Linux on page protection, demand paging, VirtualAlloc, query, PMM stats, heap safety, leak detection, MMIO, auto-growing stacks, page pinning, process counters. W^X is stronger than Windows (hard reject RWX). Build-time allocator lint and deterministic commit charge tracking (vs Linux overcommit) are exclusive.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_vmm()` (see `src/kernel/test/test_runner.c`).
> Tests run with `debug=1` or `test=1` in boot.conf under `TEST_CAT_MM`.

- [x] §12: `test_vmm_map_user_page_roundtrip` -- PMM frame alloc, map at 0x200000000, identity-map write, mapped-VA read-back, vmm_get_physical resolve, unmap verify. 5 assertions. (ce91b05a)
- [ ] §1: `vmm_protect()` changes page permissions; access fault on PROT_NONE page
- [ ] §2: W^X reject on PROT_WRITE|PROT_EXEC
- [ ] §9: heap canary overflow detection at kfree
- [ ] §9: double-free detection at kfree
- [ ] §15: commit charge atomic increment/decrement round-trip
- [ ] §16: `page_fault_count` increments on access to demand-paged region

> **Note:** Most §1-§11 tests require runtime page fault verification which cannot run in WSL (no QEMU). The test checkpoint blocks in each section define the serial-log verification criteria for native Windows / bare metal testing.

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

**Test runner:** `scripts\debug\kernel\run-mm-tests.bat` (SUITE=mm)

## History

| Date | Action | Summary |
|------|--------|---------|
| 2026-04-10 | validate | 12 sections checked; stripped 11 model tags, fixed broken .cursor anchor + UEFI §10 XREF, added Unit Tests skeleton, added §12 OS Comparison row |
| 2026-04-10 | gap-analysis | 7 web searches, 15+ sources; added §13-§16 (auto-grow stacks, mlock, commit charge, process counters); 4 new IO/OS rows; 8 features deferred to other TODOs; no conflicts |
