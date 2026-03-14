# P0012 — Advanced Memory Management

> **Goal:** Modernize the kernel allocator to eliminate the fixed 2 MiB heap ceiling:
> SLAB allocator for kernel objects, vmalloc for large kernel buffers, growable heap.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. SLAB Allocator

**Prompt:** The SLAB allocator creates pre-sized object caches for frequently-allocated kernel structures. Each cache holds fixed-size slots (e.g., 128-byte `vfs_node_t`, 256-byte `task_t`, 64-byte `reg_value_t`). New slabs are allocated from PMM on demand. Allocation is O(1) — pop from a free list. Benefits: zero internal fragmentation, cache-line friendly, no general-purpose heap overhead. Create `slab_cache_create(name, obj_size)`, `slab_alloc(cache)`, `slab_free(cache, ptr)`. Migrate VFS nodes, task structs, and Registry values off the general heap. After completing all items, create `docs/architecture/slab-allocator.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: SLAB allocator"`.


- [ ] Create `src/kernel/mm/slab.c` and `include/kernel/mm/slab.h`
- [ ] Implement `slab_cache_create(name, obj_size, align)` — create a new object cache
- [ ] Implement `slab_alloc(cache)` — O(1) allocation from free list
- [ ] Implement `slab_free(cache, ptr)` — return object to free list
- [ ] Auto-grow: allocate new slab pages from PMM when cache is exhausted
- [ ] Migrate `vfs_node_t` allocations to SLAB cache
- [ ] Migrate `task_t` allocations to SLAB cache
- [ ] Migrate `reg_value_t` / `reg_key_t` to SLAB cache
- [ ] Debug: `/proc/slabinfo`-style stats (cache name, active, total, slab pages)
- [ ] Commit: `"mm: SLAB allocator"`

---

## 2. vmalloc — Virtual Contiguous Allocator

**Prompt:** `vmalloc(size)` allocates virtually contiguous memory from scattered physical pages. Unlike `pmm_alloc_contiguous()` (which needs physically contiguous frames), vmalloc maps arbitrary free frames into a reserved virtual address range. This is ideal for large kernel buffers that don't need DMA. Requires the page table manager to map individual frames. After completing all items, update memory architecture docs, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: vmalloc virtual allocator"`.


- [ ] Create `src/kernel/mm/vmalloc.c` and `include/kernel/mm/vmalloc.h`
- [ ] Reserve virtual address range for vmalloc mappings
- [ ] Implement `vmalloc(size)` — allocate scattered PMM frames, map contiguously
- [ ] Implement `vfree(ptr)` — unmap pages, free frames back to PMM
- [ ] Track vmalloc regions (start addr → size + frame list)
- [ ] Commit: `"mm: vmalloc virtual allocator"`

---

## 3. Growable Heap

**Prompt:** Replace the fixed 2 MiB `heap_pool` array with a dynamically growable heap. Start with a small initial allocation (e.g., 256 KB). When `kmalloc` cannot satisfy a request, map additional PMM frames into the heap's virtual address range and extend the free list. This eliminates the hard 2 MiB ceiling while keeping the familiar `kmalloc`/`kfree` API. After completing all items, update memory architecture docs, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: growable kernel heap"`.


- [ ] Modify `src/kernel/mm/heap.c` to support dynamic growth
- [ ] Start with 256 KB initial heap, grow on demand
- [ ] On `kmalloc` failure: request new pages from PMM, map into heap range
- [ ] Configurable max heap size (default: 16 MiB) to prevent runaway growth
- [ ] Boot log: report initial and current heap size
- [ ] Commit: `"mm: growable kernel heap"`

---

## Priority Order

| Priority | Section           | Reason                                           |
|----------|-------------------|--------------------------------------------------|
| 🟡 P2     | §1 SLAB Allocator | Eliminates heap pressure for kernel objects      |
| 🟢 P3     | §2 vmalloc        | Large kernel buffers without physical contiguity |
| 🟢 P3     | §3 Growable Heap  | Eliminates fixed heap size ceiling               |
