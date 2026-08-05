---
schema_version: 1
id: advanced-virtual-memory
domain: 03-memory-concurrency
status: active
title: "TODO-05 -- Advanced Virtual Memory"
---

# TODO-05 -- Advanced Virtual Memory

> **Goal:** Complete the upper tier of the virtual memory model: COW `fork()`, 2 MiB and 1 GiB huge pages, `madvise` / `MEM_RESET` hints, Win32 Section Object multi-view mappings, per-process memory limits via Job Objects, a zero-copy DMA buffer pool for storage and network drivers, compressed memory (zRAM-style), NUMA-aware PMM, and a transparent huge pages collapser.

## Inputs

- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h)
- [`include/kernel/mm/pmm.h`](../../include/kernel/mm/pmm.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c)
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c)
- [`src/kernel/drivers/ahci.c`](../../src/kernel/drivers/ahci.c)
- → XREF: `03-memory-concurrency/TODO-04-pager-reclaim-working-set.md §2,§3,§6,§5` -- pagefile ownership, reclaim, lazy mapped-file faults, and replacement policy are foundations for `madvise`, sections, and compressed-memory handoff
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- `mprotect` must exist before §1 COW fork (write-protection of shared pages)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` -- demand paging (MEM_COMMIT); Section Objects (§5) build directly on top of it
- → XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §5` -- `vmalloc` backing needed for §10 compressed memory pool
- → XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §8` -- memory pressure notifications trigger §10 compression
- → XREF: `01-boot-platform/TODO-09-cpu-boot-sequencing.md §1` -- CPUID capability capture; huge page support (§2, §4) and RDRAND (§10) require it
- → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- `NtCreateSection` / `NtMapViewOfSection` / `NtCreateJobObject` syscall wiring for §6 and §8
- → XREF: `04-drivers-hardware` domain -- AHCI and VirtIO drivers consume the zero-copy DMA pool in §7

## Outcome

- `fork()` is O(page-table-copy) -- shared pages are write-protected in both parent and child; a write fault copies the page, decrements the old frame's refcount, and re-marks it writable.
- User-mode `mmap` and kernel drivers can use 2 MiB pages for large buffers; kernel MMIO mappings may use 1 GiB pages.
- `madvise` / `MEM_RESET` / `MEM_RESET_UNDO` hints let callers communicate access patterns and reclaim committed pages without unmapping.
- `NtCreateSection` / `NtMapViewOfSection` / `NtUnmapViewOfSection` implement Windows shared-memory and file-backed section semantics, with COW splitting on write.
- Job Objects enforce per-process committed-page limits; exceeding the limit returns `STATUS_COMMITMENT_LIMIT` instead of silent OOM.
- AHCI, VirtIO, and RTL8139 drivers use `dma_alloc_coherent` / `dma_map_sg` for zero-copy physical-contiguous DMA buffers.
- Pages evicted under `MM_PRESSURE_HIGH` are LZ4-compressed into a vmalloc pool before going to the pagefile, reducing I/O by 2–5×.
- PMM allocates from the closest NUMA node to the requesting CPU; topology read from ACPI SRAT.
- A background thread collapses 512 contiguous committed 4 KiB pages into a single 2 MiB page for `MADV_HUGEPAGE` regions.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                       | Status |
| --- | :---: | ---------------------------------------- | -------------------------------- | :----: |
| ⭐   |   1   | §1 COW `fork()`                          | TODO-01 §1 (`mprotect`)          |  [ ]   |
| 💎   |   2   | §2 2 MiB huge pages                      | PMM                              |  [ ]   |
| 💎   |   3   | §3 1 GiB pages (kernel MMIO)             | §2                               |  [ ]   |
| 💎   |   4   | §4 `madvise` / `MEM_RESET` hints         | §2 (for `MADV_HUGEPAGE`)         |  [ ]   |
| 💎   |   5   | §5 Section Object multi-view mappings    | §1, TODO-01 §3                   |  [ ]   |
| 💎   |   6   | §6 Per-process memory limits (Job Objects) | TODO-01 §3 (commit tracking)     |  [ ]   |
| 💎   |   7   | §7 Zero-copy DMA buffer pool             | §2, PMM contiguous               |  [ ]   |
| 💎   |   8   | §8 Compressed memory (zRAM-style)        | TODO-03 §5 (vmalloc), TODO-03 §8 |  [ ]   |
| 💎   |   9   | §9 NUMA-aware PMM                        | ACPI SRAT, CPUID                 |  [ ]   |
| ⭐   |  10   | §10 Transparent huge pages collapser     | §2, §4, scheduler tick           |  [ ]   |
| 💎   |  11   | §11 Address Windowing Extensions (AWE)   | per-process page tables, §1      |  [ ]   |

> ⭐ = exclusive -- COW `fork()` is a POSIX capability Windows does not have; the THP collapser is a Linux-specific optimization Impossible OS adds on top of the huge-page foundation.
> 💎 = parity -- Windows and Linux both implement huge pages, madvise, Section Objects, DMA pools, compressed memory, NUMA, and AWE-style physical-page windowing (`AllocateUserPhysicalPages` / `MAP_HUGETLB` reservation).

---

## 1. COW `fork()` `[Opus]`

`fork()` creates a child process sharing the parent's physical pages until either writes -- at which point the written page is copied (copy-on-write). After `fork()` both address spaces are identical; COW makes it nearly instant by sharing frames and deferring copies to the first write.

**Files:** `src/kernel/mm/vmm.c`, `src/kernel/mm/pmm.c`, `src/kernel/sched/task.c`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §1` -- `vmm_protect()` must exist; COW uses write-protection to trigger faults on shared pages.
> Per-frame refcounts must be incremented atomically; a race between two concurrent writes to the same COW page can cause a use-after-free if the frame is freed while still mapped in the parent.

- [ ] Add `ref_count` field to each PMM frame descriptor; increment to 2 for every page shared by `fork()`
- [ ] `fork()`: deep-copy the page tables (PML4→PDP→PD→PT structure), but mark all user-writable pages read-only (`R-` PTE) in both parent and child; do not copy the physical frames
- [ ] On write page fault (`PFEC.W=1`) to a read-only page with `ref_count > 1`: allocate new frame, copy contents, install in faulting process, decrement old frame refcount; if `ref_count` reaches 1 re-mark old frame writable in the surviving process
- [ ] If `ref_count == 1` at write fault time (no other sharer): simply re-mark writable -- no copy needed
- [ ] Add `SYS_FORK` syscall alias (POSIX number 57); kernel returns child PID to parent, 0 to child
- [ ] `exit()` / `task_destroy()`: walk page tables, decrement refcounts, free frames only when `ref_count == 0`
- [ ] Boot test: `fork()` a process, parent and child write different pages -- verify no data mixing and no double-free
- [ ] Commit: `"mm: COW fork() -- page-table duplication + write-fault copy, SYS_FORK"`

## 2. Huge Pages (2 MiB) `[Opus]`

Map large contiguous regions with 2 MiB PD entries (`PS=1`) instead of 512 × 4 KiB PTEs. Reduces TLB pressure dramatically for large buffers -- framebuffer back-buffer (3.6 MiB), file mmaps, DMA staging.

**Files:** `src/kernel/mm/vmm.c`, `src/kernel/mm/pmm.c`, `include/kernel/mm/vmm.h`

> [!IMPORTANT]
> A 2 MiB page must be 2 MiB-aligned both physically and virtually. `pmm_alloc_huge()` must scan the PMM bitmap for 512 contiguous page-aligned frames. If alignment fails, it is better to fall back to 4 KiB pages than to corrupt the page tables with a misaligned PD entry.

- [ ] Implement `pmm_alloc_huge()` -- allocate 512 physically contiguous and 2 MiB-aligned frames; return null if unavailable (caller falls back to 4 KiB)
- [ ] Implement `pmm_free_huge(phys)` -- return the 512 frames to PMM
- [ ] Add `MAP_HUGE` flag constant; `vmm_alloc_region()` with `MAP_HUGE` installs a 2 MiB PD entry (`PS=1`) instead of a PT hierarchy
- [ ] Page fault handler: distinguish 2 MiB PD entry (PS bit set at PD level) from a 4 KiB PT entry; handle faults correctly for huge pages
- [ ] `munmap` for huge-page regions: clear PD entry, call `pmm_free_huge()`
- [ ] Boot log: `[VMM] 2 MiB huge page support: available` (check `CPUID.01H:EDX.PSE` bit 3)
- [ ] Commit: `"mm: 2 MiB huge pages -- pmm_alloc_huge, MAP_HUGE, PD PS-bit entry"`

## 3. 1 GiB Pages (Kernel MMIO) `[Opus]`

Map large MMIO regions and reserved physical ranges with 1 GiB PDPTE entries (`PS=1`) to eliminate PT walks for device memory. Kernel-only -- user processes always use 4 KiB or 2 MiB pages.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

- [ ] Check `CPUID.80000001H:EDX` bit 26 (`PDPE1GB`) for 1 GiB page support; store in `cpu_features.pdpe1gb`
- [ ] Implement `vmm_map_1g(virt, phys, flags)` -- install a 1 GiB PDPTE entry (`PS=1`); assert 1 GiB alignment on both `virt` and `phys`; kernel-mode only
- [ ] Page fault handler: recognize 1 GiB PDPTE entries (PS bit at PDPTE level)
- [ ] Use for: identity-mapping large MMIO BARs in PCI device init, large reserved physical ranges from ACPI SRAT
- [ ] Boot log: `[VMM] 1 GiB page support: available` / `not supported (old CPU)`
- [ ] Commit: `"mm: 1 GiB pages -- vmm_map_1g for kernel MMIO, PDPTE PS-bit entry"`

## 4. `madvise` / `MEM_RESET` Hints `[Sonnet]`

Allow callers to communicate access patterns and reclaim committed pages without unmapping -- mirrors POSIX `madvise` and Win32 `VirtualAlloc(MEM_RESET)`.

**Files:** `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`, `src/kernel/sched/syscall.c`

- [ ] Define advice constants: `MADV_DONTNEED`, `MADV_SEQUENTIAL`, `MADV_RANDOM`, `MADV_HUGEPAGE`, `MADV_NOHUGEPAGE`
- [ ] `MADV_DONTNEED`: mark pages as not needed -- kernel may reclaim them; access restores zero-filled pages on demand
- [ ] `MADV_SEQUENTIAL`: hint to prefetcher; no current page-table effect; reserved for future demand-paging prefetch
- [ ] `MADV_HUGEPAGE` / `MADV_NOHUGEPAGE`: set / clear `VMR_HUGEPAGE` flag on the VMA region for the THP collapser (§10)
- [ ] Wire POSIX `madvise(addr, len, advice)` syscall → `vmm_advise()`
- [ ] Wire Win32 `VirtualAlloc(base, size, MEM_RESET)` → `MADV_DONTNEED` path (decommit backing frames without releasing the reservation)
- [ ] Wire `VirtualAlloc(base, size, MEM_RESET_UNDO)` → re-commit with zero-fill on next access
- [ ] Commit: `"mm: madvise / MEM_RESET -- page reclaim hints and MADV_HUGEPAGE region flag"`

## 5. Section Object Multi-View Mappings `[Opus]`

`NtCreateSection` / `NtMapViewOfSection` implement Windows shared-memory and file-backed section semantics: the same physical pages mapped simultaneously at different virtual addresses in different processes, with COW splitting on write.

**Files:** `include/kernel/mm/section.h` (new), `src/kernel/mm/section.c` (new), `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` -- demand paging infrastructure is the foundation; `NtMapViewOfSection` maps committed pages into a new address space.
> → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- `NtCreateSection` and `NtMapViewOfSection` syscall numbers and parameter structures.
> A section handle must outlive all views. Track refcounts on the section object; the last `NtUnmapViewOfSection` or handle close frees the backing pages.

- [ ] Define `section_t`: backing type (anonymous / file-backed), size, PMM frame list or VFS file reference, view list, refcount, protect flags
- [ ] Implement `NtCreateSection(handle_out, access, obj_attrs, max_size, protect, attrs, file_handle)` -- create section from anonymous PMM or open file; return kernel handle
- [ ] Implement `NtMapViewOfSection(section, process, &base, zero_bits, commit, &offset, &view_size, inherit, alloc_type, protect)` -- map section pages into `process`'s address space at `base`; install PTEs pointing to section frames
- [ ] Implement `NtUnmapViewOfSection(process, base)` -- remove PTE mappings for the view; decrement section refcount; free frames when refcount reaches 0
- [ ] COW view: if section is opened `PAGE_WRITECOPY`, mark pages read-only; write fault → copy frame, install in faulting process, remove from shared frame list
- [ ] Win32 wrappers: `CreateFileMapping` → `NtCreateSection`; `MapViewOfFile` → `NtMapViewOfSection`; `UnmapViewOfFile` → `NtUnmapViewOfSection`
- [ ] **Retrofit `src/kernel/nt/nt_section.c` once per-process page tables exist**: today `NtMapViewOfSection_handler` returns `STATUS_ACCESS_DENIED` for non-current `ProcessHandle` (`nt_section.c:182`) and `NtUnmapViewOfSection_handler` has a symmetric guard (`nt_section.c:235`). When §5 lands (install PTEs pointing to section frames into the target process's VMM), remove both guards and route the map/unmap through `target->vmm`. This auto-closes TODO-06 §18 Accepted #2 (cross-process map / other-process NtUnmapViewOfSection).
- [ ] **Implement `SEC_RESERVE` sparse pagefile-backed sections**: today `src/kernel/ob/ob_section.c::ObCreateSectionEx` always calls `pmm_alloc_contiguous(page_count)` + `memset(0)` for pagefile-backed sections even when `AllocationAttributes & SEC_RESERVE` is set. Spec-correct behavior: reserve the address range in the section (track `page_count` as reserved but allocate no physical frames); install zero-page PTEs on `NtMapViewOfSection`; on first write fault, allocate a frame from PMM and install writable PTE. Requires demand paging from TODO-01 §3 (`→ XREF: 03-memory-concurrency/TODO-01-vmm-memory-protection.md §3`). This auto-closes TODO-06 §18 Accepted #3 (SEC_RESERVE-only sparse pagefile sections).
- [ ] Commit: `"mm: Section Object -- NtCreateSection / NtMapViewOfSection / NtUnmapViewOfSection"`

## 6. Per-Process Memory Limits (Job Objects) `[Sonnet]`

Enforce committed-page limits per process or group of processes via the Win32 Job Object API, returning `STATUS_COMMITMENT_LIMIT` instead of silently failing or OOM-killing.

**Files:** `include/kernel/job.h` (new), `src/kernel/job.c` (new), `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-12-native-api-ssdt.md` -- `NtCreateJobObject` / `NtSetInformationJobObject` syscall wiring.
> Committed-page count tracking must be updated atomically at every `MEM_COMMIT` in `vmm_alloc_region()` and every `MEM_RELEASE` in `NtFreeVirtualMemory`. A missed decrement causes phantom limits.

- [ ] Define `job_t`: `ProcessMemoryLimit` (committed pages), current committed count, process list, refcount
- [ ] Implement `NtCreateJobObject(handle_out, obj_attrs)` -- allocate job; return kernel handle
- [ ] Implement `NtSetInformationJobObject(job, JobObjectBasicLimitInformation, &info, size)` -- accept `ProcessMemoryLimit` field in bytes; convert to pages; store in job
- [ ] Implement `NtAssignProcessToJobObject(job, process)` -- link process to job; add committed page count
- [ ] In `vmm_alloc_region()` `MEM_COMMIT` path: check `process->job->committed + new_pages <= job->page_limit`; if over limit return `STATUS_COMMITMENT_LIMIT`
- [ ] Increment / decrement `job->committed` atomically at each commit / release
- [ ] Commit: `"mm: Job Object ProcessMemoryLimit -- committed-page enforcement, STATUS_COMMITMENT_LIMIT"`

## 7. Zero-Copy DMA Buffer Pool `[Opus]`

A kernel DMA buffer API that allocates physically contiguous, cache-coherent memory for device drivers -- replacing ad-hoc `pmm_alloc_contiguous()` calls in AHCI, VirtIO, and RTL8139 with a unified scatter-gather interface.

**Files:** `include/kernel/mm/dma.h` (new), `src/kernel/mm/dma.c` (new)

> [!IMPORTANT]
> DMA buffers must remain resident (NonPagedPool, never swapped) and must not be remapped by KASLR. Mark them with the `DMA_ALLOC` tag and exclude them from the SLAB shrinker and compressed-memory paths.

- [ ] Define `dma_addr_t` (physical bus address) and `dma_sg_entry_t { phys, len }` for scatter-gather
- [ ] Implement `dma_alloc_coherent(size, &phys_out)` -- `pmm_alloc_contiguous()` for physically contiguous, DMA-safe frames; map into kernel virtual space; return virtual pointer and `phys_out`
- [ ] Implement `dma_free_coherent(virt, phys, size)` -- unmap and return frames to PMM
- [ ] Implement `dma_map_sg(sg_list, count)` -- walk a scatter-gather list, pin each page, populate `sg_entry.phys` with physical addresses for the DMA controller
- [ ] Implement `dma_unmap_sg(sg_list, count)` -- unpin pages
- [ ] Migrate AHCI PRD table allocation to `dma_alloc_coherent`
- [ ] Migrate VirtIO virtqueue descriptor ring allocation to `dma_alloc_coherent`
- [ ] Boot log: `[DMA] pool ready`
- [ ] Commit: `"mm: zero-copy DMA buffer pool -- dma_alloc_coherent, dma_map_sg, migrate AHCI/VirtIO"`

## 8. Compressed Memory (zRAM-style) `[Opus]`

LZ4-compress pages that would otherwise be written to the pagefile under `MM_PRESSURE_HIGH` -- a vmalloc-backed in-kernel swap cache that reduces pagefile I/O by 2–5× for compressible data.

**Files:** `src/kernel/mm/zram.c` (new), `include/kernel/mm/zram.h` (new)

> [!IMPORTANT]
> → XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §5` -- `vmalloc` is the backing store for compressed page data; it must be ready before this section is implemented.
> → XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §8` -- compressed memory is triggered by the `MM_PRESSURE_HIGH` callback.
> → XREF: `03-memory-concurrency/TODO-04-pager-reclaim-working-set.md §3,§5` -- reclaim decides when anonymous pages are offered to compressed backing and when they fall through to the pagefile; keep victim policy authoritative there.
> LZ4 is already vendored in the kernel (`src/kernel/lz4`). If it is not, add it before this section.

- [ ] Reserve a `vmalloc` pool of configurable size (default: 25% of RAM, read from Registry `HKLM\SYSTEM\Memory\ZramSizeMiB`)
- [ ] Register a `MM_PRESSURE_HIGH` callback: on high pressure, intercept page eviction before pagefile write → attempt LZ4-compress the page
- [ ] If compressed size < 4 KB: store in vmalloc pool at a `zram_slot_t { compressed_data, comp_size }`; mark PTE as `ZRAM` (Present=0, special swap-like encoding)
- [ ] If compressed size ≥ 4 KB (incompressible): fall through to pagefile as normal
- [ ] Page fault on `ZRAM` PTE: allocate new frame, LZ4-decompress from pool slot, install frame, mark slot free
- [ ] On `MM_PRESSURE_CRITICAL`: flush entire zRAM pool to pagefile to free vmalloc space
- [ ] Boot log: `[ZRAM] pool: %u MiB reserved`; `[ZRAM] %u pages compressed (%u MiB saved)` at 1-minute intervals
- [ ] Commit: `"mm: compressed memory (zRAM) -- LZ4 in-kernel swap cache, vmalloc pool"`

## 9. NUMA-Aware PMM `[Sonnet]`

Parse the ACPI SRAT table at boot to identify NUMA nodes and their physical memory ranges; allocate frames from the node closest to the requesting CPU to minimize cross-socket memory latency.

**Files:** `src/kernel/mm/pmm.c`, `include/kernel/mm/pmm.h`, `src/kernel/acpi.c`

- [ ] Parse `ACPI_SRAT` (System Resource Affinity Table) in `acpi_init()`: enumerate `SRAT_MEMORY_AFFINITY` entries; store `[phys_start, phys_end, node_id]` in a static `numa_node_t` table (max 8 nodes)
- [ ] Extend PMM frame descriptors with `node_id`; populate from SRAT ranges at `pmm_init()`
- [ ] Implement `pmm_alloc_node(node_id, count)` -- prefer frames from `node_id`; fall back to any node if local is exhausted
- [ ] Extend `struct task` with `preferred_numa_node`; scheduler sets this from the CPU's SRAT affinity
- [ ] `pmm_alloc_contiguous()` accepts optional `node_id` hint (pass `NUMA_NODE_ANY` to keep current behavior)
- [ ] Boot log: `[NUMA] %u nodes: node 0 [0x%llx–0x%llx], ...`; `[NUMA] not available (single node)` on non-NUMA systems
- [ ] Commit: `"mm: NUMA-aware PMM -- ACPI SRAT parse, pmm_alloc_node, task preferred node"`

## 10. Transparent Huge Pages Collapser `[Opus]`

A background kernel thread that promotes regions marked `MADV_HUGEPAGE` from 512 contiguous committed 4 KiB pages into a single 2 MiB page, reducing TLB miss rate for hot anonymous memory without requiring the application to use `MAP_HUGE` explicitly.

**Files:** `src/kernel/mm/thp.c` (new), `include/kernel/mm/thp.h` (new)

> [!IMPORTANT]
> The collapser must not collapse pages while the faulting CPU holds the page table lock. It runs in a dedicated kernel thread at low priority. Collapse is only safe when all 512 pages are present, committed, not swap-backed, and not COW-shared (refcount == 1). Check all preconditions under a VMA write lock before replacing 512 PTEs with one PD PS entry.

- [ ] Spawn `thp_kthread` at kernel init; runs every 200 ms, sleeps between passes
- [ ] Scan VMAs with the `VMR_HUGEPAGE` flag (set by `MADV_HUGEPAGE`): look for 2 MiB-aligned ranges of 512 consecutive PTEs where all pages are present, committed, not COW-shared, and not in the zRAM pool
- [ ] For qualifying ranges: acquire VMA write lock, call `pmm_alloc_huge()`, copy contents from 512 frames, install 2 MiB PD entry, release 512 old frames, release lock
- [ ] If `pmm_alloc_huge()` fails (no contiguous 2 MiB frame): skip range, retry next pass
- [ ] Emit `[THP] collapsed 0x%llx → 2 MiB` in serial log (debug builds only)
- [ ] Commit: `"mm: THP collapser -- background promotion of 512×4KiB to 2MiB for MADV_HUGEPAGE regions"`

## 11. Address Windowing Extensions (AWE) `[Opus]`

Windows AWE lets a process reserve physical pages it owns (`NtAllocateUserPhysicalPages`) and remap them into a fixed reserved virtual window (`NtMapUserPhysicalPages`) without the pagefile -- the mechanism large-memory / NUMA / large-page workloads use to control physical placement. It is the concrete owner for the three AWE SSDT stubs registered by 02-kernel-core/TODO-12 §9 (`nt_memory.c::NtAWE_stub`, SSDT 0x0059-0x005B), which return `STATUS_NOT_IMPLEMENTED` today. **Blocker:** AWE requires per-process physical-page ownership tracking and a per-process page-table window, neither of which exists while all user pages share the kernel's identity map (CLAUDE.md "User-mode pages need User bit at all 4 levels"; per-process isolation is planned for the Win32 PE loader / VirtualAlloc work). Do not implement until per-process page tables land.

**Files:** `src/kernel/nt/nt_memory.c`, `include/kernel/nt/nt_memory.h`, `src/kernel/mm/vmm.c`

- [ ] `NtAllocateUserPhysicalPages(0x0059)`: allocate NumberOfPages PMM frames owned by the process (per-task AWE frame list), return frame numbers in PageArray, enforce a per-process AWE quota
- [ ] `NtFreeUserPhysicalPages(0x005A)`: unmap any currently-windowed frames, return them to PMM, drop from the process AWE list; reject frames the process does not own
- [ ] `NtMapUserPhysicalPages(0x005B)`: remap the reserved AWE window at BaseAddress to owned frames (NULL entry unmaps that slot); needs an `MEM_PHYSICAL`-reserved window with per-process PTEs
- [ ] Replace `NtAWE_stub` in `nt_memory.c` with the three real handlers; flip 02-kernel-core/TODO-12 §9 items 0x0059-0x005B to `[x]` and correct the TODO-A master-table rows (`T05-mem §11`)
- [ ] Unit tests: allocate-window-map-unmap-free round trip; ownership rejection (map a frame not owned); quota exhaustion
- [ ] Commit: `"mm: AWE -- NtAllocate/Free/MapUserPhysicalPages over per-process physical page windows"`

**Test checkpoint:** `NtAllocateUserPhysicalPages(4)` returns 4 frame numbers; `NtMapUserPhysicalPages` maps them into the reserved window and a read/write round-trips; remap to NULL unmaps; `NtFreeUserPhysicalPages` returns the frames and a subsequent map of a freed frame is rejected.

**Inputs (XREFs):** -> XREF: 02-kernel-core/TODO-12-native-api-ssdt.md §9 (the three AWE SSDT stubs this section replaces); per-process page-table prerequisite tracked with the Win32 PE loader / VirtualAlloc isolation work.

---

## OS Comparison


| ⭐   | Feature                                  | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| ⭐   | COW `fork()` -- instant process clone    | ❌ No `fork()`; uses `CreateProcess` with | ✅ COW `fork()` -- standard POSIX,        | ⬜ §1 -- beats Windows -- both `fork()`   |
| 💎   | 2 MiB huge pages                         | ✅ `VirtualAlloc(MEM_LARGE_PAGES)`; requires privilege | ✅ `MAP_HUGETLB`; `hugepages=` kernel param | ⬜ §2 -- `MAP_HUGE` flag, `pmm_alloc_huge()` |
| 💎   | 1 GiB pages for MMIO / reserved ranges   | ✅ 1 GB large pages (Win8+,               | ✅ `PUD_SIZE` mappings for MMIO in        | ⬜ §3 -- kernel-only `vmm_map_1g()`, PDPTE PS bit |
| 💎   | `madvise` / `MEM_RESET` access hints     | ✅ `VirtualAlloc(MEM_RESET / MEM_RESET_UNDO)` | ✅ `madvise(2)` -- `DONTNEED`, `HUGEPAGE`, `SEQUENTIAL` | ⬜ §4 -- `vmm_advise()`, `MADV_*` + Win32 `MEM_RESET` |
| 💎   | Section Object / shared memory multi-view | ✅ `NtCreateSection` / `MapViewOfFile` -- core | ⚠️ `mmap(MAP_SHARED)` / POSIX `shm_open`; no | ⬜ §5 -- `NtCreateSection` + `NtMapViewOfSection` |
| 💎   | Per-process memory limits                | ✅ `CreateJobObject` / `ProcessMemoryLimit` | ⚠️ `cgroups` memory limit; no Win32      | ⬜ §6 -- `NtCreateJobObject` + committed-page enforcement |
| 💎   | Zero-copy DMA buffer pool                | ✅ `AllocateCommonBuffer` (WDM); HAL DMA API | ✅ `dma_alloc_coherent` / `dma_map_sg` (DMA-API) | ⬜ §7 -- unified `dma_alloc_coherent` / `dma_map_sg` |
| 💎   | Compressed memory                        | ✅ Memory Compression (Win10+, `MemCompressionProcess`) | ✅ zRAM (`CONFIG_ZRAM`); LZ4/LZO/zstd backends | ⬜ §8 -- LZ4 vmalloc pool, `MM_PRESSURE_HIGH` trigger |
| 💎   | NUMA-aware physical frame allocation     | ✅ NUMA node affinity in `MmAllocateContiguousMemory` | ✅ `alloc_pages_node()` + `numactl` / cpuset | ⬜ §9 -- ACPI SRAT parse, `pmm_alloc_node()` |
| ⭐   | Transparent huge page collapser          | ❌ No THP; large pages are                | ✅ THP daemon (`khugepaged`); `MADV_HUGEPAGE` regions | ⬜ §10 -- `thp_kthread` collapse pass every 200 |

> **After parity items:** Impossible OS matches Windows and Linux on huge pages, Section Objects, DMA pools, compressed memory, and NUMA. COW `fork()` adds a POSIX capability Windows lacks entirely. The THP collapser matches Linux `khugepaged`, giving transparent large-page performance to any anonymous mapping without application changes.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `fork()`: parent and child write different pages after fork -- no data mixing, no double-free on exit
- [ ] 2 MiB huge page: `mmap(NULL, 4*1024*1024, PROT_RW, MAP_HUGE)` → PD entry with PS=1 in page table dump; access succeeds
- [ ] `madvise(MADV_DONTNEED)`: pages reclaimed; next access returns zero-filled page
- [ ] Section Object: two processes map same section at different VAs -- write in one visible in other (shared view)
- [ ] Job Object: exceed `ProcessMemoryLimit` with `VirtualAlloc(MEM_COMMIT)` → returns `STATUS_COMMITMENT_LIMIT`
- [ ] DMA: AHCI PRD table allocated via `dma_alloc_coherent`; disk read completes successfully (regression test)
- [ ] Compressed memory: create 100 MiB of zero pages, trigger `MM_PRESSURE_HIGH` → serial log shows `[ZRAM] N pages compressed`
- [ ] NUMA: on multi-node QEMU config, `pmm_alloc_node(0)` returns frames from node 0 physical range
- [ ] THP: mmap 2 MiB anonymous region + `MADV_HUGEPAGE` → after 200 ms `thp_kthread` pass, PD entry shows PS=1
- [ ] Commit: `"mm: advanced virtual memory -- COW fork, huge pages, Section Objects, DMA, zRAM, NUMA, THP"`
