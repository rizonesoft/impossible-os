---
schema_version: 1
id: advanced-allocator
domain: 03-memory-concurrency
status: active
title: "TODO-03 -- Advanced Kernel Allocator"
---

# TODO-03 -- Advanced Kernel Allocator

> **Goal:** Build the most self-defending kernel memory allocator in existence. Start with parity (growable heap, SLAB caches, vmalloc, NonPagedPool/PagedPool, tagged allocation, memory pressure, live stats) then go beyond both Windows and Linux with out-of-band metadata, type-isolated pools, probabilistic guard pages, production quarantine, per-page header encoding, zero-on-free, and lock-free SMP fast paths. Every allocation is tagged, every free is verified, every overflow is caught -- in production, not just in debug builds.
>
> When complete, Impossible OS has the only kernel allocator where:
> - Heap overflow cannot corrupt allocator metadata (out-of-band)
> - Type confusion after UAF has < 10% success rate (type isolation, inspired by Apple XNU kalloc_type)
> - Production systems detect UAF/overflow with < 1% overhead (probabilistic guard pages, inspired by GWP-ASan/KFENCE)
> - Every allocation carries a 4-byte tag verified on free (Windows parity + mismatch = panic)
> - Freed memory enters quarantine before reuse (catches UAF in production)

> [!IMPORTANT]
> **Memory rule:** `kmalloc` is only for structs <= 4 KB. `pmm_alloc_contiguous()` for buffers that can grow past 4 KB. Once the growable heap (section 1) is done that ceiling relaxes, but the `pmm_alloc_contiguous` rule for physically-contiguous DMA buffers still holds.

> [!CAUTION]
> **SMP critical:** The current heap has ZERO synchronization. Every section in this TODO must be SMP-safe from the start -- per-CPU freelists, spinlocks on shared state, atomic operations. No single-CPU assumptions. See CLAUDE.md "SMP From Day One" policy.

---

## Inputs

- `src/kernel/mm/heap.c` -- current 2 MiB fixed first-fit allocator (to be replaced)
- `include/kernel/mm/heap.h` -- kmalloc/kfree/krealloc API
- `src/kernel/mm/pmm.c` -- physical frame allocator (backing store)
- `include/kernel/mm/pmm.h` -- pmm_alloc_frame/pmm_alloc_contiguous API
- `src/kernel/mm/vmm.c` -- page table management
- `include/kernel/mm/vmm.h` -- vmm_map_page/vmm_unmap_page API
- -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3` -- demand paging needed before vmalloc
- -> XREF: `03-memory-concurrency/TODO-04-pager-reclaim-working-set.md §3,§5` -- pressure callbacks and pageable-pool victim rules are consumed by the pager and reclaim policy
- -> XREF: `03-memory-concurrency/TODO-10-concurrency-diagnostics.md §3,§4` -- SLAB red zones, KASAN (COMPLEMENT: diagnostics layer on top of allocator)
- -> XREF: `02-kernel-core/TODO-01-kernel-init-sequencing.md` -- heap init timing; growable heap must not regress boot
- -> XREF: `02-kernel-core/TODO-07-irql-model-dpcs.md §1` -- IRQL enforcement for PagedPool vs NonPagedPool
- -> XREF: `02-kernel-core/TODO-14-registry-completion.md` -- `/sys/pooltags` registry integration
- -> XREF: `02-kernel-core/TODO-31-kernel-bulletproofing.md` -- 5-layer defense pattern for allocator invariants
- -> XREF: `05-storage-filesystems` domain -- `/sys/slab` VFS files require VFS ready

---

## Outcome

- `kmalloc`/`kfree` backed by a growable heap with SMP spinlock protection -- the fixed 2 MiB ceiling is gone.
- SLAB caches provide O(1) object allocation with lock-free per-CPU fast path (cmpxchg-based, no interrupt disable needed).
- `vmalloc(size)` maps scattered PMM frames into a contiguous virtual range.
- `kmalloc_tag(size, tag)` / `kfree_tag(ptr, tag)` mirror `ExAllocatePoolWithTag`; tag mismatch = panic.
- `kmalloc_nonpaged` / `kmalloc_paged` separate DMA-safe and swappable pools (Windows NonPagedPool/PagedPool contract).
- Three-level pressure notifications allow caches to shed memory before OOM.
- `/sys/slab` and `/sys/pooltags` VFS files surface live allocator state.
- **Out-of-band metadata** eliminates the entire class of overflow-to-header attacks.
- **Type-isolated pools** reduce UAF exploitation success rate to < 10% (Apple kalloc_type model).
- **Probabilistic guard pages** catch overflow/UAF in production with < 1% overhead (KFENCE/GWP-ASan model).
- **Production quarantine** delays freed memory reuse, catching UAF without debug-mode slowdown.
- **Per-page encoding keys** make header forgery require per-page info leak, not per-heap.
- **Zero-on-free** eliminates info leaks from stale data without hot-path cost.

---

## Implementation Order

| Star | Order | Deliverable                              | Depends On        | Status |
| ---- | :---: | ---------------------------------------- | ----------------- | :----: |
| 💎    |   1   | §1 Growable kernel heap + SMP spinlock   | PMM, VMM          |  [ ]   |
| 💎    |   2   | §2 Memory pressure notifications         | PMM stats         |  [ ]   |
| 💎    |   3   | §3 vmalloc                               | §1, TODO-01 §3    |  [ ]   |
| 💎    |   4   | §4 SLAB allocator + lock-free per-CPU fast path | §1                |  [ ]   |
| 💎    |   5   | §5 SLAB shrinker                         | §4, §2            |  [ ]   |
| 💎    |   6   | §6 Tagged allocation API                 | §1                |  [ ]   |
| 💎    |   7   | §7 NonPagedPool / PagedPool              | §4, §3            |  [ ]   |
| ⭐    |   8   | §8 Out-of-band metadata                  | §4                |  [ ]   |
| ⭐    |   9   | §9 Type-isolated pools (kalloc_type model) | §4, §8            |  [ ]   |
| ⭐    |  10   | §10 Per-page header encoding keys        | §8                |  [ ]   |
| ⭐    |  11   | §11 Probabilistic guard pages (KFENCE model) | §4                |  [ ]   |
| ⭐    |  12   | §12 Production quarantine (delayed reuse) | §4, §11           |  [ ]   |
| ⭐    |  13   | §13 Zero-on-free                         | §1                |  [ ]   |
| ⭐    |  14   | §14 Bulk alloc/free API                  | §4                |  [ ]   |
| 💎    |  15   | §15 `/sys/slab` + `/sys/pooltags` + shell commands | §4, §6, VFS ready |  [ ]   |

> 💎 = parity (Windows + Linux both have equivalent). ⭐ = exclusive (neither OS does this in production, or Impossible OS does it better).

---

## 1. Growable Kernel Heap + SMP Spinlock

Replace the fixed 2 MiB first-fit allocator with a growable, SMP-safe heap. Starts at 256 KB; expands in 256 KB segments on demand up to 16 MiB. Protects all shared state with a spinlock.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

> [!IMPORTANT]
> The current heap has ZERO locking. On SMP, concurrent kmalloc from two CPUs corrupts the free list. The spinlock must be added FIRST, before any growth logic.

> [!NOTE]
> The irqsave heap spinlock + `block_header` `_Static_assert` shipped early as part of the kernel-heap-hardening work in `D02 T10 §11` (heap was lockless, an SMP-from-day-one gap). The growable-segment, PMM-lock, canary, and configurable-max items below are still open here.

- [x] Add `spinlock_t heap_lock` protecting `heap_start_block`, `total_heap_size`, `used_bytes` -- shipped as `s_heap_lock` in `D02 T10 §11`
- [x] Wrap `kmalloc`, `kfree`, `krealloc` with `spin_lock_irqsave` / `spin_unlock_irqrestore` -- shipped in `D02 T10 §11` (also `kmalloc_zeroed`/`kmalloc_tagged`/`kfree_tagged`/`heap_get_free`)
- [ ] **PMM bitmap SMP locking**: `pmm_alloc_frame()`, `pmm_alloc_contiguous()`, `pmm_free_frame()`, `pmm_mark_region_used()` in `src/kernel/mm/pmm.c` mutate the global frame bitmap and `used_frames` counter without synchronization. Concurrent callers from two CPUs (e.g. user-reachable syscall paths like the UEFI Variable Services SSDT handlers in `src/kernel/uefi_runtime.c` allocating value buffers above 4 KiB via `uefi_value_alloc()`) can race the bitmap and double-allocate / double-free / corrupt counters. Add `spinlock_t pmm_lock` (or per-zone if a zone allocator is introduced); wrap allocator/free entry points with `spin_lock_irqsave` / `spin_unlock_irqrestore`. The same `irqsave` variant requirement as the heap lock applies (PMM may be called from IRQ context for emergency frame allocation). Consumer waiting on this: page-backed quota-ledger chunk storage, which cannot lazily allocate frames on a live charge path until the bitmap is locked -> XREF: `02-kernel-core/TODO-25-kernel-resource-accounting-quotas.md §17`
- [ ] Close the timed-out-async-worker window: `boot_async_group` proceeds after its 10s timeout while that AP KEEPS RUNNING (`boot_init.c:396-416`), so it can race any later PMM allocation -> XREF: `02-kernel-core/TODO-33 §10`
- [ ] Replace static 512-page BSS array with a linked list of heap segments (`struct heap_segment { uintptr_t base; uint64_t size; struct heap_segment *next; }`)
- [ ] Initial segment: 256 KB from `pmm_alloc_contiguous(64)` at `heap_init()`
- [ ] On `kmalloc` failure: if below max, `pmm_alloc_contiguous(64)` for new 256 KB segment; append to segment list; retry
- [ ] Canary word at end of each segment (RDRAND-seeded); checked on kfree
- [ ] Configurable max: read `HKLM\SYSTEM\Memory\HeapMaxMiB` (default 16)
- [x] `_Static_assert` on `sizeof(struct block_header)` to catch layout changes -- shipped in `D02 T10 §11` (pins size==48, %16==0, and the size/next/redzone_front offsets)
- [ ] Commit: `"mm: growable SMP-safe kernel heap -- spinlock + dynamic 256 KB segments"`

**Test checkpoint:** Allocate objects until 256 KB exhausted -> serial shows `[HEAP] grew to 512 KiB`. Run `kmalloc` from two CPUs simultaneously via IPI -> no corruption. QEMU WHPX/TCG/VBox.

**Regression risk:** Touches every kmalloc caller. If spinlock deadlocks in interrupt context -> triple fault. Use `irqsave` variant. Rollback: revert to fixed 2 MiB + no lock (known working).

---

## 2. Memory Pressure Notifications

Poll PMM free-frame count at 1 Hz; broadcast pressure level to registered callbacks. Subsystems (glyph cache, disk buffer, SLAB shrinker) shed non-critical memory before OOM.

**Files:** `src/kernel/mm/pressure.c` (new), `include/kernel/mm/pressure.h` (new)

- [ ] Define levels: `MM_PRESSURE_NORMAL` (>= 64 MB free), `LOW` (< 64 MB), `HIGH` (< 16 MB), `CRITICAL` (< 4 MB)
- [ ] `mm_pressure_register(level, callback)` -- static callback table (max 32)
- [ ] Poll in scheduler tick (1 Hz): `pmm_get_free_frames()` -> compute level -> fire callbacks
- [ ] Hysteresis: 5 consecutive ticks below threshold to transition up; same to transition down
- [ ] Built-in: glyph cache eviction on `LOW`, disk buffer flush on `HIGH`, `slab_reap_all()` on `CRITICAL`
- [ ] Blocking allocation path (wait for reclaim instead of returning NULL) plus `quota_stall_task_stalled(QUOTA_STALL_MEM)` around the wait, which is the seam the PSI mem metric needs. -> XREF: `02-kernel-core/TODO-25 §12`
- [ ] Commit: `"mm: memory pressure notifications -- 4 levels, 1 Hz poll, shrinker callbacks"`

**Test checkpoint:** Artificially exhaust PMM -> serial shows `[MM] PRESSURE HIGH -- %u MiB free`. Restore frames -> returns to NORMAL after 5 ticks.

---

## 3. vmalloc -- Virtual Contiguous Allocator

Allocate virtually contiguous memory from scattered physical frames. For large kernel buffers that don't need DMA.

**Files:** `src/kernel/mm/vmalloc.c` (new), `include/kernel/mm/vmalloc.h` (new)

> [!IMPORTANT]
> vmalloc virtual range must not overlap identity map. Reserve `0xFFFF900000000000-0xFFFF980000000000` (32 TiB). Map with 4 KB pages; future: 2 MiB PMD mappings for large allocations.

- [ ] Define `VMALLOC_START` / `VMALLOC_END` constants
- [ ] Sorted interval list of live regions; protected by spinlock
- [ ] `vmalloc(size)` -- allocate individual PMM frames, find free virtual slot, map each with `vmm_map_page()`, return virtual base
- [ ] `vfree(ptr)` -- unmap all pages, free each PMM frame, remove from interval list
- [ ] NX bit on all vmalloc data pages
- [ ] Route exec image STAGING off the kmalloc heap once vmalloc exists, and retire the interim
      `EXEC_KMALLOC_STAGE_MAX` cap. Filed 2026-07-28 from TODO-21 §19's review.
      - **Three stagers read a whole executable into a `kmalloc` buffer** -- `SYS_EXEC` (`src/kernel/sched/syscall.c`), `shell_loader_func`, and `exec_loader_func` -- against a fixed 512-page / 2 MiB heap (`heap.c` `HEAP_INITIAL_PAGES`).
      - **An unprivileged exec of a 1-2 MiB file therefore holds most of the global heap** for the whole read-and-load and can starve concurrent kernel allocations, so those stagers are capped at `EXEC_KMALLOC_STAGE_MAX` (512 KiB, `include/kernel/exec.h`, derived as about a third of the boot-baseline free heap) purely to protect the arena.
      - **That cap is BELOW the format bound `EXEC_MAX_IMAGE_SIZE`** (16 MiB), so a raw image between the two is loadable only via a path that stages with `pmm_alloc_contiguous`.
      - **Shape**: switch the three stagers to `vmalloc`, raise them to the format bound, and add a heap-pressure / concurrent-exec regression.
      -> XREF: `02-kernel-core/TODO-21-process-model-extensions.md` §19 (item: "Caller staging-buffer release no longer races publication")
- [ ] Commit: `"mm: vmalloc -- virtual contiguous allocator, scattered PMM frames"`

**Test checkpoint:** `vmalloc(8 MiB)` succeeds; write pattern; read back; `vfree` -> PMM frames returned.

---

## 4. SLAB Allocator + Lock-Free Per-CPU Fast Path

Pre-sized object caches for high-frequency kernel types. Lock-free per-CPU freelist using `cmpxchg` (inspired by Linux SLUB's `cmpxchg_double` pattern). No interrupt disable needed on the fast path.

**Files:** `src/kernel/mm/slab.c` (new), `include/kernel/mm/slab.h` (new)

> [!TIP]
> **Better than Windows:** Windows LFH uses atomic bitmap ops (good) but activates only after 17 allocations of a size class. Our SLAB is always active for registered caches -- no warm-up delay.
>
> **Better than Linux:** Linux SLUB's `cmpxchg_double` requires `CMPXCHG16B` (128-bit atomic). We use a simpler `cmpxchg` on a single pointer + separate `tid` check, which works on all x86-64 CPUs.

- [ ] Define `struct slab_cache`: name, obj_size, align, slab page list, global free list (spinlock), per-CPU freelist array
- [ ] Per-CPU fast path: `struct slab_cpu { void *freelist; uint64_t tid; }` -- allocate via `cmpxchg(&cpu->freelist, old, next)` with tid ABA protection
- [ ] Slow path: per-CPU partial list -> per-node partial list (spinlock) -> allocate new slab page from PMM
- [ ] `slab_cache_create(name, obj_size, align)` -- register globally
- [ ] `slab_alloc(cache)` -- per-CPU fast path; fallback to slow path
- [ ] `slab_free(cache, ptr)` -- push to per-CPU freelist; if full, drain to global
- [ ] Size classes for `kmalloc`: 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048, 4096 (11 caches)
- [ ] Cache merging: if a new cache matches an existing cache's size+align+flags, share it
- [ ] Migrate `vfs_node_t`, `task_t`, `reg_key_t`, `reg_value_t` to dedicated SLAB caches
- [ ] Commit: `"mm: SLAB allocator with lock-free per-CPU fast path + kmalloc size classes"`

**Test checkpoint:** 10,000 `slab_alloc/slab_free` cycles from two CPUs via IPI -> no corruption, no lock contention on fast path. Serial shows per-cache stats.

---

## 5. SLAB Shrinker

Release fully-empty slab pages back to PMM on memory pressure.

**Files:** `src/kernel/mm/slab.c`, `include/kernel/mm/slab.h`

- [ ] Track per-slab-page occupancy count; page is "empty" when count == 0
- [ ] `slab_cache_reap(cache)` -- walk slab pages; free empty ones to PMM
- [ ] Auto-register `MM_PRESSURE_HIGH` callback at `slab_cache_create` time
- [ ] `slab_reap_all()` on `MM_PRESSURE_CRITICAL`
- [ ] Commit: `"mm: SLAB shrinker -- release empty slab pages on memory pressure"`

**Test checkpoint:** Allocate 1000 objects; free all; trigger pressure HIGH -> empty slab pages released to PMM.

---

## 6. Tagged Allocation API

Mirror `ExAllocatePoolWithTag` -- every allocation carries a 4-byte tag. Tag mismatch on free = immediate panic.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`

- [ ] `kmalloc_tag(size, tag)` -- store tag in block header; return pointer
- [ ] `kfree_tag(ptr, tag)` -- verify tag matches; panic on mismatch with address + expected/actual tags
- [ ] Define canonical tags: `PTAG_VFS 'VFS '`, `PTAG_TASK 'TASK'`, `PTAG_NET 'NET '`, `PTAG_REG 'REG '`, `PTAG_OB 'OBJ '`, `PTAG_SEC 'SEC '`
- [ ] Pool tag tracking: per-tag count + total bytes (atomic counters)
- [ ] Accept an optional quota owner alongside the tag so a tagged allocation can be billed to a principal; the charge API and its cost budget already ship. -> XREF: `02-kernel-core/TODO-25 §5`
- [ ] Migrate lookaside backing to tagged pool: swap `backing_alloc`/`backing_free` in `ex_lookaside.c` to `kmalloc_tag`/`kfree_tag`, drop the interim local-IRQ mask once the pool is SMP+reentrancy-safe -> XREF: 02-kernel-core/TODO-06 §5
- [ ] Commit: `"mm: tagged allocation -- kmalloc_tag/kfree_tag + pool tag mismatch panic"`

**Test checkpoint:** `kfree_tag(ptr, wrong_tag)` -> kernel panics with tag-mismatch message. `/sys/pooltags` shows VFS and TASK tags after boot.

---

## 7. NonPagedPool / PagedPool

Two pool classes matching the Windows NT contract. NonPagedPool: always resident, DMA-safe, any IRQL. PagedPool: swappable, PASSIVE_LEVEL only.

**Files:** `include/kernel/mm/pool.h` (new), `src/kernel/mm/pool.c` (new)

- [ ] `POOL_TYPE_NONPAGED` and `POOL_TYPE_PAGED` constants
- [ ] `kmalloc_nonpaged(size)` -> PMM-backed, always resident
- [ ] `kmalloc_paged(size)` -> vmalloc-backed, can be swapped
- [ ] `ExAllocatePool2(pool_type, size, tag)` Win32 wrapper
- [ ] IRQL assert: `kmalloc_paged` at above PASSIVE_LEVEL -> panic
- [ ] Charge paged/nonpaged pool allocations through a quota provider hook, and refuse an owner-less user-triggered allocation rather than billing System. -> XREF: `02-kernel-core/TODO-25 §5`
- [ ] Attribute pre-registry allocations to System: `kmalloc` is live from Phase 0 but the System block appears at Phase 3, so the hook needs a deferred replay. -> XREF: `02-kernel-core/TODO-25 §5`
- [ ] Cache-line-aligned pool allocation AND a 32-multiple counter-array offset for quota records: alignment alone leaves exactly half straddling (24-byte block prefix, 32-byte records). -> XREF: `02-kernel-core/TODO-25 §5`
- [ ] Commit: `"mm: NonPagedPool/PagedPool -- PMM-backed and vmalloc-backed pool classes"`

**Test checkpoint:** `kmalloc_nonpaged(4096)` succeeds at DISPATCH_LEVEL. `kmalloc_paged(4096)` at DISPATCH_LEVEL -> panic with IRQL message. A pool allocation billed to a quota owner charges that owner's paged/nonpaged usage and the free returns it; an owner-less user-triggered allocation is refused rather than billed to System.

---

## 8. Out-of-Band Metadata

> [!TIP]
> **Neither Windows nor Linux does this.** Both store block headers immediately before user data (in-band). A heap overflow corrupts the next block's header, giving attackers control of the allocator. Out-of-band metadata stores headers in separate metadata pages -- overflow writes to user data only, never to allocator control structures.

Move SLAB and heap block headers to dedicated metadata pages, separate from user data pages.

**Files:** `src/kernel/mm/slab.c`, `src/kernel/mm/heap.c`

- [ ] For SLAB: store per-object metadata (allocated/free, tag, size class, cache pointer) in a separate metadata page per slab page. Each slab page has a companion metadata page.
- [ ] Metadata page contains: `struct slab_obj_meta { uint32_t tag; uint16_t flags; uint16_t cache_idx; }` array (one per object slot)
- [ ] User data pages contain ONLY user data -- no freelist pointers, no headers
- [ ] Free objects tracked via a bitmap in the metadata page (like Windows LFH), not an in-object freelist
- [ ] For heap (non-SLAB path): block headers stored in a separate header region at the start of each segment, indexed by offset
- [ ] `_Static_assert` on metadata struct sizes
- [ ] Commit: `"mm: out-of-band metadata -- separate metadata pages for SLAB and heap"`

**Test checkpoint:** Deliberately overflow a SLAB allocation by 16 bytes -> no metadata corruption; object freed successfully. Overflow into guard page -> #PF. Compare: with in-band metadata, same overflow would corrupt next block's header.

**Regression risk:** Changes fundamental allocation layout. All pointer arithmetic in slab_alloc/slab_free changes. Test thoroughly on all platforms before proceeding.

---

## 9. Type-Isolated Pools (kalloc_type Model)

> [!TIP]
> **Inspired by Apple XNU kalloc_type.** Apple demonstrated that type isolation reduces UAF exploitation success rate from ~100% to ~8% by preventing cross-type reallocation. Windows has no type isolation at all. Linux has per-cache separation but allows cache merging, weakening isolation. Our implementation: strict type isolation with NO merging for security-critical types.

Assign security-critical kernel types to dedicated SLAB caches that are never merged. After UAF, the freed slot can only be reallocated by the SAME type -- cross-type confusion is architecturally impossible.

**Files:** `src/kernel/mm/slab.c`, `include/kernel/mm/slab.h`

- [ ] `slab_cache_create_isolated(name, obj_size, align, SLAB_FLAG_NO_MERGE)` -- create a cache that is NEVER merged with other caches
- [ ] Mandatory isolated caches for security-critical types: `ACCESS_TOKEN`, `SECURITY_DESCRIPTOR`, `HANDLE_TABLE_ENTRY`, `EPROCESS`, `ETHREAD`, `FILE_OBJECT`, `OB_HEADER`
- [ ] Type signature generation: at cache creation, record a 64-bit hash of the type name + size + alignment. Stored in cache metadata for diagnostics.
- [ ] Pointer-data layout tracking (future): separate caches for "contains pointers" vs "data only" types within the same size class
- [ ] Boot log: `[SLAB] isolated cache '%s': obj=%u B (no merge)`
- [ ] Commit: `"mm: type-isolated pools -- dedicated caches for security-critical types, no merge"`

**Test checkpoint:** Allocate `ACCESS_TOKEN` from isolated cache; free it; allocate `FILE_OBJECT` of same size -> different cache, different address range. Verify via `/sys/slab` that isolated caches show separate stats.

---

## 10. Per-Page Header Encoding Keys

> [!TIP]
> **Better than Windows.** Windows XOR-encodes all block headers with a SINGLE per-heap key. One info leak anywhere in pool = all headers forgeable. Our approach: derive the encoding key from `page_address XOR boot_random_seed`. Each page has a unique key. An attacker must leak per-page info, not just a global secret.

Encode SLAB metadata entries with a key derived from the page address, preventing header forgery.

**Files:** `src/kernel/mm/slab.c`

- [ ] At boot: generate 64-bit `g_slab_random_seed` from RDRAND
- [ ] Encoding key for page at address `P`: `key = g_slab_random_seed ^ (P >> 12) ^ swab64(P)`
- [ ] Encode metadata on `slab_free`: `encoded = raw_meta ^ key`
- [ ] Decode + verify on `slab_alloc`: `raw_meta = encoded ^ key`; validate tag + flags; panic on corruption with page address + expected key
- [ ] Commit: `"mm: per-page encoding keys -- RDRAND seed + page-derived XOR for SLAB metadata"`

**Test checkpoint:** Corrupt a metadata entry directly -> panic with `[SLAB] metadata corruption at page 0x%x`. Valid alloc/free cycle -> no false positives.

---

## 11. Probabilistic Guard Pages (KFENCE Model)

> [!TIP]
> **Better than both.** Linux KFENCE samples 255 objects into a fixed pool -- too small to catch rare bugs. Windows Special Pool uses 2 pages per allocation -- too expensive for production. Our approach: integrate guard pages WITHIN the SLAB allocator. 1-2% of slab page boundaries become guard pages (not-present PTE). Near-zero average overhead because most allocations are unaffected. On a system with millions of allocations, even 1% sampling catches bugs within hours.

Insert not-present guard pages at random slab page boundaries. Any out-of-bounds access that crosses a page boundary triggers an immediate #PF.

**Files:** `src/kernel/mm/slab.c`

- [ ] At slab page allocation: with probability 1/64 (configurable), allocate an extra guard page (not-present PTE) after the slab page
- [ ] Guard page insertion is random per-page (RDRAND bit) -- attacker cannot predict which pages are guarded
- [ ] Object alignment within guarded pages: right-align objects so overflow hits the guard page immediately
- [ ] On #PF in guard page: log `[SLAB] GUARD PAGE HIT at 0x%x -- probable overflow from cache '%s'` with allocation backtrace (from stack depot)
- [ ] Configurable: `HKLM\SYSTEM\Memory\GuardPageRate` (default 64 = 1/64 pages)
- [ ] Commit: `"mm: probabilistic guard pages -- 1/64 slab pages get guard page canary"`

**Test checkpoint:** Allocate 1000 objects of 64 bytes; write 65th byte on each; ~15 trigger #PF (1/64 rate). Zero overhead on the other 985. Verify on QEMU WHPX/TCG.

---

## 12. Production Quarantine (Delayed Reuse)

> [!TIP]
> **Better than both.** Windows only has quarantine in Driver Verifier (debug mode, 5-10x slowdown). Linux only has quarantine in KASAN (3x slowdown). Our approach: a small FIFO quarantine (256 KB default) that is ALWAYS active. Freed objects sit in quarantine before reuse. If any code touches a quarantined object, it reads `0xDEADDEAD` poison bytes -- detectable. Cost: 256 KB memory + one pointer indirection on free.

Hold freed SLAB objects in a per-cache FIFO quarantine before returning them to the freelist. Catches use-after-free in production.

**Files:** `src/kernel/mm/slab.c`

- [ ] Per-cache quarantine: circular buffer of `QUARANTINE_DEPTH` freed object pointers (default 64 per cache)
- [ ] On `slab_free`: poison object bytes with `0xDEADDEAD` pattern; push to quarantine; if quarantine full, evict oldest entry to real freelist
- [ ] On eviction from quarantine: verify poison bytes still intact; if corrupted -> `[SLAB] USE-AFTER-FREE detected in cache '%s' at 0x%x` with backtrace
- [ ] Quarantine depth configurable: `HKLM\SYSTEM\Memory\QuarantineDepth` (default 64, 0 = disabled)
- [ ] Total quarantine overhead: ~256 KB across all caches (64 objects x 64 bytes avg x ~64 caches)
- [ ] Commit: `"mm: production quarantine -- FIFO delayed reuse with poison verification"`

**Test checkpoint:** Allocate object; free it; write to freed pointer -> quarantine poison detect on next eviction, logs UAF. Normal alloc/free cycle -> no false positives.

---

## 13. Zero-on-Free

> [!TIP]
> **Better than Windows.** Microsoft is moving to zero-on-alloc (hot path cost -- every allocation pays). Linux has CONFIG_INIT_ON_FREE but it's opt-in. Our approach: zero freed memory at FREE time, not ALLOC time. The cost is amortized -- free typically happens at lower priority than alloc. For quarantined objects, zeroing happens at quarantine eviction (even lower priority). Info leaks from stale data are eliminated without slowing down the allocation fast path.

Zero freed memory to eliminate information leaks from stale data.

**Files:** `src/kernel/mm/slab.c`, `src/kernel/mm/heap.c`

- [ ] On `slab_free` (after quarantine eviction): `memset(obj, 0, cache->obj_size)` before returning to freelist
- [ ] On `kfree` (heap path): zero the freed block data region
- [ ] For large PMM allocations: zero pages at `pmm_free_frame` time (future: DMA engine offload)
- [ ] Skip zeroing if object will be immediately reused from per-CPU freelist (optimization: lazy zero flag)
- [ ] Commit: `"mm: zero-on-free -- eliminate info leaks from stale heap/SLAB data"`

**Test checkpoint:** Allocate 256 bytes; write `0xAA` pattern; free; reallocate same size -> all bytes are 0x00. No info leak.

---

## 14. Bulk Alloc/Free API

Amortize per-object overhead across batches. Critical for networking (sk_buff-like structures) and VFS (inode batches).

**Files:** `src/kernel/mm/slab.c`, `include/kernel/mm/slab.h`

- [ ] `int slab_alloc_bulk(slab_cache_t *cache, uint32_t count, void **out)` -- allocate `count` objects into `out` array; return actual count allocated
- [ ] `void slab_free_bulk(slab_cache_t *cache, uint32_t count, void **ptrs)` -- free all objects in one call
- [ ] Bulk fast path: drain per-CPU freelist in a single locked region; refill from partial list once
- [ ] Commit: `"mm: bulk alloc/free API -- slab_alloc_bulk/slab_free_bulk for batched operations"`

**Test checkpoint:** `slab_alloc_bulk(cache, 64, ptrs)` returns 64; all pointers valid and distinct. `slab_free_bulk` frees all.

---

## 15. `/sys/slab` + `/sys/pooltags` + Shell Commands

Surface live allocator state through VFS for diagnostics.

**Files:** `src/kernel/fs/sysfs_slab.c` (new), `src/shell/cmd_slab.c` (new)

- [ ] `/sys/slab` read callback: one line per cache: `name  obj_sz  active  total  slabpgs  wastepct  isolated  guarded`
- [ ] `slab` shell command: formatted table with totals row
- [ ] `/sys/pooltags`: one line per active tag: `tag  count  total_bytes`
- [ ] `pooltags` shell command: formatted output
- [ ] Include quarantine stats: `quarantine_depth  quarantine_uaf_detected`
- [ ] Commit: `"mm: /sys/slab and /sys/pooltags + shell commands + quarantine stats"`

**Test checkpoint:** `slab` command prints >= 3 caches. `pooltags` shows VFS and TASK tags. Quarantine column shows 0 UAF detected on clean run.

---

## OS Comparison

| ⭐   | Feature                       | 🪟 Win11                      | 🐧 Linux                    | 🚀 Impossible OS                       |
| --- | ----------------------------- | ---------------------------- | -------------------------- | ------------------------------------- |
| 💎   | Growable kernel heap          | ✅ Segment Heap auto-grows    | ✅ SLUB backed by buddy     | ⬜ §1 -- 256 KB segments, 16 MiB max   |
| 💎   | Memory pressure notifications | ✅ Memory resource events     | ✅ Shrinker + OOM notifier  | ⬜ §2 -- 4-level, 1 Hz poll            |
| 💎   | Virtual contiguous allocator  | ✅ MmAllocateMapping          | ✅ vmalloc/vfree            | ⬜ §3 -- scattered PMM, NX             |
| 💎   | SLAB/slab caches              | ✅ Lookaside lists + LFH      | ✅ SLUB per-CPU slabs       | ⬜ §4 -- lock-free cmpxchg fast path   |
| 💎   | SLAB shrinker                 | ✅ Lookaside depth tuning     | ✅ kmem_cache_shrink        | ⬜ §5 -- pressure-driven reap          |
| 💎   | Tagged allocation             | ✅ ExAllocatePoolWithTag      | ⚠️ kmemleak only           | ⬜ §6 -- tag mismatch = panic          |
| 💎   | NonPagedPool / PagedPool      | ✅ Core NT contract           | ⚠️ GFP flags, not explicit | ⬜ §7 -- IRQL-enforced pool classes    |
| ⭐   | Out-of-band metadata          | ❌ In-band headers            | ❌ In-band freelist ptrs    | ⬜ §8 -- separate metadata pages       |
| ⭐   | Type-isolated pools           | ❌ No type isolation          | ⚠️ Cache merge weakens it  | ⬜ §9 -- no-merge for critical types   |
| ⭐   | Per-page encoding keys        | ⚠️ Single key per heap       | ⚠️ Per-cache key           | ⬜ §10 -- page-addr derived key        |
| ⭐   | Probabilistic guard pages     | ❌ Special Pool too expensive | ⚠️ KFENCE 255 obj limit    | ⬜ §11 -- 1/64 slab pages, integrated  |
| ⭐   | Production quarantine         | ❌ Debug mode only            | ❌ KASAN only (3x slow)     | ⬜ §12 -- always-on FIFO, 256 KB cost  |
| ⭐   | Zero-on-free                  | ⚠️ Moving to zero-on-alloc   | ⚠️ Opt-in CONFIG           | ⬜ §13 -- zero at free time, not alloc |
| ⭐   | Bulk alloc/free               | ❌ No bulk API                | ✅ kmem_cache_alloc_bulk    | ⬜ §14 -- slab_alloc_bulk/free_bulk    |
| 💎   | Live allocator stats          | ✅ !poolused, Poolmon         | ✅ /proc/slabinfo           | ⬜ §15 -- /sys/slab + /sys/pooltags    |

> **After all sections:** Impossible OS has the only kernel allocator where heap overflow cannot corrupt metadata (§8), type confusion succeeds < 10% of the time (§9), and production systems detect UAF/overflow with < 1% overhead (§11-§12). The parity features (§1-§7, §15) match Windows and Linux; the ⭐ features (§8-§14) surpass both.

---

## Unit Tests

> Tests register via `test_register_allocator()` and run when `test=1` or `test_suite=mm` in boot.conf.

- [ ] Create `src/kernel/test/test_allocator.c` with:
  - Heap growth: exhaust initial segment -> verify growth logged + new alloc succeeds
  - SMP safety: IPI-based concurrent kmalloc/kfree from 2 CPUs -> no corruption
  - SLAB fast path: 1000 alloc/free from per-CPU path -> verify O(1) (no PMM calls after warm-up)
  - SLAB slow path: exhaust per-CPU list -> verify fallback to partial -> verify fallback to new slab page
  - Tagged alloc: correct tag -> success; wrong tag -> panic (test with deliberate mismatch)
  - Type isolation: two isolated caches of same size -> allocations from different address ranges
  - Guard page: overflow past object end -> #PF on guarded page (probabilistic; run 100 iterations)
  - Quarantine: free + immediate write -> poison corruption detected on quarantine eviction
  - Zero-on-free: alloc + write pattern + free + realloc -> all zeros
  - Bulk: `slab_alloc_bulk(64)` returns 64 distinct pointers; `slab_free_bulk` frees all
  - Pressure: mock PMM free count below HIGH threshold -> callbacks fired
- [ ] Register in `test_runner_init()`: `test_register_allocator()`
- [ ] Commit: `"test: add advanced allocator test suite"`

---

## Verification

- [ ] `bash scripts/build.sh clean` -> `=== BUILD OK ===`
- [ ] Heap growth: serial shows `[HEAP] grew to 512 KiB` under load
- [ ] SLAB 10,000 alloc/free: no PMM call after warm-up (per-CPU hot path)
- [ ] vmalloc 8 MiB: write + readback + vfree -> PMM frames returned
- [ ] Tag mismatch: `kfree_tag(ptr, wrong)` -> panic
- [ ] Guard page: overflow -> #PF on guarded page
- [ ] Quarantine: UAF write -> detected on eviction
- [ ] Zero-on-free: stale data eliminated
- [ ] `/sys/slab` and `pooltags` commands show live stats
- [ ] All tests pass on QEMU WHPX, TCG, VBox, bare metal
