# TODO-03 — Advanced Kernel Allocator

> **Goal:** Eliminate the fixed 2 MiB heap ceiling and build a production-quality allocator tier: a growable heap, SLAB object caches with per-CPU free lists, a vmalloc virtual-contiguous allocator, NonPagedPool / PagedPool Win32 pool classes, tagged allocation for diagnostics, memory pressure notifications, and a live `/sys/slab` stats view.

> [!IMPORTANT]
> **Memory rule:** `kmalloc` is only for structs ≤ 4 KB. `pmm_alloc_contiguous()` for buffers that can grow past 4 KB. Once §4 (growable heap) is done that ceiling is removed, but the `pmm_alloc_contiguous` rule for physically-contiguous DMA buffers still holds. See [`freestanding-kernel-code.mdc`](../../.cursor/rules/freestanding-kernel-code.mdc).

## Inputs

- [`src/kernel/mm/heap.c`](../../src/kernel/mm/heap.c)
- [`include/kernel/mm/heap.h`](../../include/kernel/mm/heap.h)
- [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c)
- [`include/kernel/mm/pmm.h`](../../include/kernel/mm/pmm.h)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c)
- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h)
- → XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` — demand paging (MEM_COMMIT zero-fill) needed before vmalloc can map scattered frames
- → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md` — heap init timing; §4 growable heap must not regress the boot sequence
- → XREF: `02-kernel-core/TODO-13-registry-completion.md` — `/sys/pooltags` and NonPagedPool/PagedPool pool-tag registry integration
- → XREF: `05-storage-filesystems` domain — `/sys/slab` and `/sys/pooltags` VFS files require VFS to be initialised

## Outcome

- `kmalloc`/`kfree` backed by a growable heap that starts at 256 KB and expands in 256 KB PMM increments up to 16 MiB; the fixed 2 MiB ceiling is gone.
- SLAB caches provide O(1) object allocation for `vfs_node_t`, `task_t`, `reg_value_t` and other high-frequency types with a per-CPU free-list fast path.
- `vmalloc(size)` maps scattered PMM frames into a contiguous virtual range without needing physically contiguous pages.
- `kmalloc_tag(size, tag)` / `kfree_tag(ptr, tag)` mirror `ExAllocatePoolWithTag`; pool-tag mismatches panic on free.
- `kmalloc_nonpaged` and `kmalloc_paged` separate DMA-safe and swappable allocation paths, matching the Windows NonPagedPool / PagedPool contract.
- Three-level pressure notifications (`LOW` / `HIGH` / `CRITICAL`) allow caches to shed memory before the system runs out.
- `/sys/slab` and `/sys/pooltags` VFS files surface live allocator state; `slab` shell command prints a per-cache table.

## Implementation Order

| ⭐  | Order | Deliverable                                         | Depends On                    | Status |
| --- | :---: | --------------------------------------------------- | ----------------------------- | :----: |
| 💎  |   1   | §4 Growable kernel heap                             | PMM, VMM                      |  [ ]   |
| 💎  |   2   | §7 Memory pressure notifications                    | PMM stats (TODO-01 §8)        |  [ ]   |
| 💎  |   3   | §3 vmalloc                                          | §1, TODO-01 §3                |  [ ]   |
| 💎  |   4   | §1 SLAB allocator + per-CPU free list               | §1 (heap), §2 (pressure)      |  [ ]   |
| 💎  |   5   | §2 SLAB shrinker                                    | §4 (SLAB), §2 (pressure)      |  [ ]   |
| 💎  |   6   | §5 Tagged allocation API                            | §1 (growable heap)            |  [ ]   |
| 💎  |   7   | §6 NonPagedPool / PagedPool                         | §4 (SLAB), §3 (vmalloc)       |  [ ]   |
| 💎  |   8   | §8 `/sys/slab` VFS file + `slab` shell command      | §4 (SLAB), VFS ready          |  [ ]   |

> 💎 = parity — Windows (NonPagedPool, look-aside lists, `ExAllocatePoolWithTag`) and Linux (SLUB, vmalloc, shrinkers) both implement these allocator tiers; Impossible OS must match across the board.

---

## 1. SLAB Allocator + Per-CPU Free List `[Opus]`

Pre-sized object caches for high-frequency kernel types. Each cache holds fixed-size slots backed by PMM slab pages. A per-CPU free list is the O(1) hot path — no lock needed for alloc/free on the common case; the shared free list is the cold-path fallback when the per-CPU list is empty or full.

**Files:** `src/kernel/mm/slab.c` (new), `include/kernel/mm/slab.h` (new), `src/kernel/fs/vfs.c`, `src/kernel/sched/task.c`

> [!IMPORTANT]
> The per-CPU free list must be accessed with interrupts disabled (or per-CPU context protection) to avoid a preemption race where another CPU pops the same slot. Use a per-CPU array of pointers, not a lock. Keep the per-CPU list bounded (e.g., ≤ 32 objects) to limit wasted memory.

- [ ] Define `slab_cache_t`: name, object size, alignment, PMM-backed slab page list, global free list, per-CPU free list array
- [ ] Implement `slab_cache_create(name, obj_size, align)` — initialise cache, register globally
- [ ] Implement `slab_alloc(cache)` — pop from per-CPU free list if non-empty (hot path, no lock); else pop from global free list; else allocate new slab page from PMM and carve into slots
- [ ] Implement `slab_free(cache, ptr)` — push to per-CPU free list if not full; else return to global free list; mark slot with `SLAB_FREE_MAGIC` for double-free detection
- [ ] Migrate `vfs_node_t` allocations off `kmalloc` → `slab_alloc(g_vfs_node_cache)`
- [ ] Migrate `task_t` allocations → `slab_alloc(g_task_cache)`
- [ ] Migrate `reg_value_t` / `reg_key_t` → `slab_alloc(g_reg_cache)`
- [ ] Boot log: `[SLAB] cache '%s': obj=%u B, align=%u` for each created cache
- [ ] Commit: `"mm: SLAB allocator with per-CPU free list + migrate vfs/task/reg to SLAB"`

## 2. SLAB Shrinker `[Sonnet]`

Release fully-empty SLAB slab pages back to PMM when memory pressure reaches `HIGH` — the SLAB cache's response to the pressure notification system (§7).

**Files:** `src/kernel/mm/slab.c`, `include/kernel/mm/slab.h`

- [ ] Track per-slab-page occupancy count in each slab page header; a page is "empty" when occupancy == 0
- [ ] Implement `slab_cache_reap(cache)` — walk the slab page list; free empty pages to PMM; update cache stats
- [ ] Each `slab_cache_create` call automatically registers a `MM_PRESSURE_HIGH` callback that calls `slab_cache_reap(cache)` (→ §7 `mm_pressure_register`)
- [ ] Implement `slab_reap_all()` — call `slab_cache_reap` on every registered cache; invoked directly on `MM_PRESSURE_CRITICAL`
- [ ] Log: `[SLAB] reap '%s': freed %u pages (%u KiB)` per cache that released pages
- [ ] Commit: `"mm: SLAB shrinker — release empty slab pages on memory pressure HIGH"`

## 3. vmalloc — Virtual Contiguous Allocator `[Opus]`

Allocate virtually contiguous memory from scattered physical frames — no requirement for physically contiguous pages. For large kernel buffers that don't need DMA (driver staging areas, large caches, module text).

**Files:** `src/kernel/mm/vmalloc.c` (new), `include/kernel/mm/vmalloc.h` (new)

> [!IMPORTANT]
> vmalloc requires scattering individual PMM frames and mapping them into a reserved virtual address range via the VMM page table. The virtual range must not overlap the identity map, kernel BSS, or the heap virtual range. Reserve a canonical vmalloc zone (e.g., `0xFFFF900000000000–0xFFFF980000000000`).

- [ ] Define vmalloc virtual address zone constant (`VMALLOC_START` / `VMALLOC_END`) in `include/kernel/mm/vmalloc.h`
- [ ] Maintain a sorted interval list of live vmalloc regions (start address, size, frame list); protected by a spinlock
- [ ] Implement `vmalloc(size)` — round up to page, allocate individual PMM frames, find a free virtual slot in the vmalloc zone via interval list, map each frame with `vmm_map_page()`, return the virtual base
- [ ] Implement `vfree(ptr)` — look up region by pointer, unmap all pages, free each PMM frame, remove from interval list
- [ ] `vmalloc` pages carry the NX bit on data regions (→ XREF `03-memory-concurrency/TODO-02-memory-security.md §5`)
- [ ] Boot log: `[VMALLOC] zone 0x%llx–0x%llx reserved`
- [ ] Commit: `"mm: vmalloc — virtual contiguous allocator, scattered PMM frames"`

## 4. Growable Kernel Heap `[Opus]`

Replace the fixed-size `heap_pool` array. The heap starts at 256 KB; when `kmalloc` cannot satisfy a request, it expands the heap in 256 KB PMM increments up to a configurable maximum (default 16 MiB). Existing `kmalloc`/`kfree` callers are unaffected.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

> [!IMPORTANT]
> The current `heap_pool` is a statically allocated BSS array. Replacing it with a dynamic base pointer changes the kernel BSS size estimate — verify the BSS stays below `0x800000` after the change (`scripts/build.sh` checks this automatically). Any bug in the growth path produces a silent heap corruption, not a clean fault. Add a canary at the end of each heap segment.

- [ ] Replace static `heap_pool[HEAP_SIZE]` with a dynamically grown linked list of heap segments
- [ ] Initial segment: 256 KB, allocated from PMM at `heap_init()` and mapped into the heap virtual range
- [ ] On `kmalloc` failure: if heap is below max, call `pmm_alloc_contiguous()` for one 256 KB segment, map it, append to segment list, retry allocation
- [ ] Configurable maximum: read `HKLM\SYSTEM\Memory\HeapMaxMiB` at boot (default 16); `PANIC("kernel heap exhausted")` on overflow
- [ ] Boot log: `[HEAP] initial 256 KiB; max %u MiB` and `[HEAP] grew to %u KiB` on each expansion
- [ ] Commit: `"mm: growable kernel heap — dynamic 256 KiB segments, 16 MiB ceiling"`

## 5. Tagged Allocation API `[Sonnet]`

Mirror `ExAllocatePoolWithTag` — every allocation carries a 4-byte tag stored in the header immediately before the returned pointer. `kfree_tag` validates the tag on free, catching allocator/free mismatches early.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] `kmalloc_tag(size, tag)` — allocate `size + 8` bytes (4 B tag + 4 B padding for alignment), store `tag` at `ptr - 4`, return `ptr`
- [ ] `kfree_tag(ptr, tag)` — read stored tag at `ptr - 4`; if mismatch: `PANIC("pool tag mismatch: expected 0x%08x got 0x%08x at 0x%p")`; else free normally
- [ ] Define canonical kernel pool tags as 4-byte ASCII constants: `PTAG_VFS` (`'VFS '`), `PTAG_TASK` (`'TASK'`), `PTAG_NET` (`'NET '`), `PTAG_REG` (`'REG '`), etc.
- [ ] `/sys/pooltags` VFS file — enumerate live tagged allocations grouped by tag (count, total bytes)
- [ ] Commit: `"mm: tagged allocation — kmalloc_tag / kfree_tag + pool tag mismatch detection"`

## 6. NonPagedPool / PagedPool `[Sonnet]`

Two pool classes matching the Windows NonPagedPool / PagedPool contract: `kmalloc_nonpaged` is PMM-backed and always resident in memory (safe at any IRQL, DMA-usable); `kmalloc_paged` is vmalloc-backed and can be swapped under pressure.

**Files:** `include/kernel/mm/pool.h` (new), `src/kernel/mm/pool.c` (new)

> [!IMPORTANT]
> → XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md` — interrupt handlers and code executing above `PASSIVE_LEVEL` must only use NonPagedPool. Using PagedPool at elevated IRQL produces a blue screen on Windows; Impossible OS must enforce the same contract and panic rather than silently succeed.

- [ ] Define `POOL_TYPE_NONPAGED` and `POOL_TYPE_PAGED` constants
- [ ] `kmalloc_nonpaged(size)` → `pmm_alloc_contiguous()` for the backing frame; always physically resident, safe at any IRQL
- [ ] `kfree_nonpaged(ptr)` → returns frame to PMM
- [ ] `kmalloc_paged(size)` → `vmalloc(size)` backing; may be reclaimed by the shrinker under pressure
- [ ] `kfree_paged(ptr)` → `vfree(ptr)`
- [ ] `ExAllocatePoolWithTag(pool_type, size, tag)` Win32 wrapper → dispatch to `kmalloc_nonpaged` or `kmalloc_paged` based on `pool_type`, then apply tag header (→ XREF `02-kernel-core/TODO-05-native-api-ssdt.md`)
- [ ] Debug: assert in `kmalloc_paged` that current IRQL is `PASSIVE_LEVEL` (to be wired once IRQL is implemented → XREF `02-kernel-core/TODO-06-irql-model-dpcs.md`)
- [ ] Commit: `"mm: NonPagedPool / PagedPool — PMM-backed and vmalloc-backed pool classes"`

## 7. Memory Pressure Notifications `[Sonnet]`

Poll PMM free-frame count at 1 Hz in the scheduler tick and broadcast a pressure level to all registered callbacks. Gives subsystems (glyph cache, disk buffer cache, SLAB shrinker) a chance to shed non-critical memory before the system runs out.

**Files:** `src/kernel/mm/pressure.c` (new), `include/kernel/mm/pressure.h` (new), `src/kernel/sched/tick.c`

- [ ] Define levels: `MM_PRESSURE_NORMAL` (≥ 64 MB free), `MM_PRESSURE_LOW` (< 64 MB), `MM_PRESSURE_HIGH` (< 16 MB), `MM_PRESSURE_CRITICAL` (< 4 MB)
- [ ] Implement `mm_pressure_register(level, callback)` — add to a static callback table (max 32 entries)
- [ ] Poll in scheduler tick (1 Hz): call `pmm_stats()`, compute current level, call all registered callbacks whose trigger level is ≤ current level
- [ ] Hysteresis: only transition up (normal → low → high → critical) when the threshold is crossed; transition back down requires 5 consecutive ticks at the lower level
- [ ] Built-in registrations: glyph cache eviction on `LOW`, disk buffer flush on `HIGH`, `slab_reap_all()` on `CRITICAL`
- [ ] Boot log: `[MM] pressure callbacks registered: %u`; `[MM] PRESSURE HIGH — %u MiB free` on first HIGH crossing
- [ ] Commit: `"mm: memory pressure notifications — 4 levels, 1 Hz poll, shrinker callbacks"`

## 8. `/sys/slab` VFS File + `slab` Shell Command `[Sonnet]`

Surface live per-SLAB-cache statistics through the VFS so diagnostic tools and the `slab` shell command can inspect allocator state at runtime.

**Files:** `src/kernel/fs/sysfs_slab.c` (new), `src/shell/cmd_slab.c` (new)

- [ ] Implement a `/sys/slab` VFS read callback: enumerate all registered caches and emit one line per cache: `name  obj_sz  active  total  slabpgs  wastepct`
- [ ] `slab` shell command: call `vfs_read("/sys/slab", ...)` and print a formatted table with totals row
- [ ] `/sys/pooltags` VFS file (from §5): emit one line per active tag: `tag  count  total_bytes`
- [ ] `pooltags` shell command: print formatted `/sys/pooltags` output
- [ ] Commit: `"mm: /sys/slab and /sys/pooltags VFS files + slab and pooltags shell commands"`

---

## OS Comparison


| ⭐ | Feature                                 | 🪟 Win11                                                   | 🐧 Linux                                       | 🚀 Impossible OS                                             |
|----|-----------------------------------------|---------------------------------------------------------|---------------------------------------------|-----------------------------------------------------------|
| 💎 | Object caches with per-CPU free list    | ✅ Look-aside lists (`ExInitializeNPagedLookasideList`) | ✅ SLUB allocator with per-CPU slabs        | ⬜ §1 — `slab_cache_t` + per-CPU hot path                 |
| 💎 | SLAB page release under memory pressure | ✅ Lookaside lists trimmed by Memory                    | ✅ SLUB shrinker + `kmem_cache_shrink`      | ⬜ §2 — `slab_cache_reap` on `MM_PRESSURE_HIGH`           |
| 💎 | Virtual contiguous allocator            | ✅ `MmAllocateMappingAddress` / `MmProbeAndLockPages`   | ✅ `vmalloc` / `vfree` with rb-tree         | ⬜ §3 — sorted interval-list zone, scattered PMM          |
| 💎 | Growable kernel heap                    | ✅ NonPagedPool grows automatically from free           | ✅ `kmalloc` backed by SLUB; grows          | ⬜ §4 — 256 KiB segments; 16 MiB                          |
| 💎 | Tagged pool allocation                  | ✅ `ExAllocatePoolWithTag` — 4-byte tag on              | ⚠️ `kmemleak` annotations; no mandatory tag | ⬜ §5 — `kmalloc_tag` / `kfree_tag`, tag-mismatch panic   |
| 💎 | NonPagedPool / PagedPool pool classes   | ✅ Core NT contract; IRQL-aware dispatch                | ⚠️ `GFP_KERNEL` vs `GFP_ATOMIC` flags; not  | ⬜ §6 — explicit `kmalloc_nonpaged` / `kmalloc_paged` API |
| 💎 | Memory pressure notifications           | ✅ `CreateMemoryResourceNotification` event             | ✅ Shrinker callbacks + OOM notifier        | ⬜ §7 — 4-level thresholds, 1 Hz poll,                    |
| 💎 | Live per-cache allocator stats          | ✅ `!poolused`, `!slab` (WinDbg); Poolmon.exe           | ✅ `/proc/slabinfo`; `slabtop` tool         | ⬜ §8 — `/sys/slab` + `/sys/pooltags`, `slab` shell       |

> **After parity items:** Impossible OS matches Windows and Linux on all eight allocator tiers. The per-CPU SLAB fast path is architecturally equivalent to SLUB and look-aside lists without the complexity of either. The tagged allocation model is a strict superset of `ExAllocatePoolWithTag` — tag mismatches panic at `kfree` time rather than being silently lost, making allocator bugs visible immediately in development.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Heap growth: allocate enough objects to exhaust 256 KB → serial log shows `[HEAP] grew to 512 KiB`; continue until 1 MiB without panic
- [ ] `slab_alloc` benchmark: 10 000 allocations from a `vfs_node_t` cache complete without PMM call after warm-up (per-CPU list hot path)
- [ ] vmalloc: allocate 8 MiB via `vmalloc` → verify virtually contiguous, physically scattered; `vfree` → PMM frames returned
- [ ] Tagged alloc: `kfree_tag(ptr, wrong_tag)` → kernel panics with tag-mismatch message
- [ ] Memory pressure: reduce free frame count below HIGH threshold → serial log shows `[MM] PRESSURE HIGH`; SLAB caches release empty pages
- [ ] `slab` shell command prints table with ≥ 3 rows (vfs_node, task, reg caches)
- [ ] `pooltags` command lists at least `VFS ` and `TASK` tags after boot
- [ ] Commit: `"mm: advanced allocator — SLAB, vmalloc, growable heap, tagged pools, pressure"`
