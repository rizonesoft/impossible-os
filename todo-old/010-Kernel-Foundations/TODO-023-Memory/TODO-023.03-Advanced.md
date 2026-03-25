# TODO-023.03-Advanced — Advanced Memory Management

> **Goal:** Modernize the kernel allocator to eliminate the fixed 2 MiB heap ceiling:
> SLAB allocator for kernel objects, vmalloc for large kernel buffers, growable heap.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. SLAB Allocator

**Prompt:** The SLAB allocator creates pre-sized object caches for frequently-allocated kernel structures. Each cache holds fixed-size slots (e.g., 128-byte `vfs_node_t`, 256-byte `task_t`, 64-byte `reg_value_t`). New slabs are allocated from PMM on demand. Allocation is O(1) — pop from a free list. Benefits: zero internal fragmentation, cache-line friendly, no general-purpose heap overhead. Create `slab_cache_create(name, obj_size)`, `slab_alloc(cache)`, `slab_free(cache, ptr)`. Migrate VFS nodes, task structs, and Registry values off the general heap. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: SLAB allocator"`. Add notes, gotchas, and design decisions directly in this TODO section covering the SLAB cache struct, overflow behavior, and debug stats.

> **Beats:** Linux SLUB allocator uses per-CPU caches. Windows uses look-aside lists. Impossible OS can combine both: per-CPU free lists without the SLUB complexity.

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

**Prompt:** `vmalloc(size)` allocates virtually contiguous memory from scattered physical pages. Unlike `pmm_alloc_contiguous()` (which needs physically contiguous frames), vmalloc maps arbitrary free frames into a reserved virtual address range. This is ideal for large kernel buffers that don't need DMA. Requires the page table manager to map individual frames. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: vmalloc virtual allocator"`. Add notes directly in this TODO section covering the virtual address range reservation and page table mapping.

> **Beats:** Linux vmalloc is complex (rb-tree VMA tracking). Windows MmAllocateContiguousMemory still needs physically contiguous pages. Impossible OS vmalloc can be simpler — sorted interval list, fits our scale perfectly.

- [ ] Create `src/kernel/mm/vmalloc.c` and `include/kernel/mm/vmalloc.h`
- [ ] Reserve virtual address range for vmalloc mappings (above kernel, below 0xFFFF800000000000)
- [ ] Implement `vmalloc(size)` — allocate scattered PMM frames, map contiguously in virtual space
- [ ] Implement `vfree(ptr)` — unmap pages, free frames back to PMM
- [ ] Track vmalloc regions (start addr → size + frame list) using sorted interval list
- [ ] Commit: `"mm: vmalloc virtual allocator"`

---

## 3. Growable Heap

**Prompt:** Replace the fixed 2 MiB `heap_pool` array with a dynamically growable heap. Start with a small initial allocation (e.g., 256 KB). When `kmalloc` cannot satisfy a request, map additional PMM frames into the heap's virtual address range and extend the free list. This eliminates the hard 2 MiB ceiling while keeping the familiar `kmalloc`/`kfree` API. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: growable kernel heap"`. Add notes directly in this TODO section covering the growth strategy and maximum size guard.

- [ ] Modify `src/kernel/mm/heap.c` to support dynamic growth
- [ ] Start with 256 KB initial heap, grow on demand in 256 KB increments
- [ ] On `kmalloc` failure: request new pages from PMM, map into heap range, extend free list
- [ ] Configurable max heap size guard (default: 16 MiB) to prevent runaway growth
- [ ] Boot log: report initial and current heap size
- [ ] Commit: `"mm: growable kernel heap"`

---

## 4. Memory Pressure Notifications

**Prompt:** When PMM free frames drop below a configurable threshold (e.g., 16 MB), the kernel should broadcast a memory pressure event to all registered consumers. Consumers respond by freeing caches or reducing buffer sizes. Register with `mm_pressure_register(callback)`. The glyph cache, disk buffer cache, and SLAB caches should all register. This prevents OOM crashes by giving subsystems a chance to release non-critical memory before the system runs out. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: memory pressure notifications"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Beats:** Linux uses shrinker callbacks (complex infrastructure). Windows uses memory notification events. Impossible OS: simple callback list — simpler than Linux shrinkers, just as functional.

- [ ] Define memory pressure levels: `MM_PRESSURE_LOW` (< 64 MB free), `MM_PRESSURE_HIGH` (< 16 MB free), `MM_PRESSURE_CRITICAL` (< 4 MB free)
- [ ] Implement `mm_pressure_register(level, callback)` — subscribe to pressure events
- [ ] Poll current free frame count in scheduler tick (every 1 second)
- [ ] On threshold crossing: call all registered callbacks with pressure level
- [ ] Glyph cache: register handler → evict non-ASCII cached glyphs on HIGH
- [ ] Disk buffer cache: register handler → flush + shrink on HIGH
- [ ] SLAB caches: register handler → release empty slabs on HIGH
- [ ] Boot log / shell: `meminfo` command shows pressure level
- [ ] Commit: `"mm: memory pressure notifications"`

---

## 5. NUMA-Awareneses (Future)

**Prompt:** On NUMA (Non-Uniform Memory Access) systems, memory accesses to local nodes are faster than remote nodes. The NUMA topology is described in the ACPI SRAT (System Resource Affinity Table). Read SRAT at boot to identify NUMA nodes and their associated physical memory ranges. When allocating frames for a CPU, prefer frames local to that CPU's NUMA node. This is a stretch goal — single-node systems (all current QEMU/VBox configs) are not affected. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: NUMA-aware PMM allocation"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

> **Competitive target:** Matches Linux NUMA subsystem for multi-socket server support.
> Windows uses NUMA node affinity in the NT kernel. Even basic NUMA awareness positions
> Impossible OS alongside production server operating systems.

- [ ] Parse ACPI SRAT to identify NUMA nodes and memory ranges
- [ ] Extend PMM to track which node each memory range belongs to
- [ ] `pmm_alloc_node(node_id)` — prefer local-node frames
- [ ] Scheduler: set preferred NUMA node per task
- [ ] Commit: `"mm: NUMA-aware PMM allocation"`

---

## 6. Memory Architecture Documentation

> *Incorporated from parking-lot P10*

**Prompt:** Create comprehensive memory architecture documentation consolidating the entire memory subsystem design. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"docs: memory architecture documentation"`. Add notes directly in this TODO section. After implementation, save any gotchas, solutions, and important information to MCP memory (`mcp_memory_create_entities` / `mcp_memory_add_observations`).

- [ ] Document the two-tier allocation model (kmalloc vs PMM) with diagram
- [ ] Document identity-mapped physical memory layout
- [ ] List all PMM consumers with sizes: framebuffer back buffer, font files, glyph pool, window framebuffers
- [ ] List all kmalloc consumers with typical sizes: VFS nodes, task structs, Registry values
- [ ] Document the glyph pool bump allocator: how it works, capacity, fragmentation
- [ ] Add a "capacity planning" section: current usage vs limits, warning thresholds
- [ ] Add a Mermaid diagram showing memory regions at runtime
- [ ] Commit: `"docs: memory architecture documentation"`

---

## Priority Order

| Priority | Section                       | Reason                                                  |
|----------|-------------------------------|---------------------------------------------------------|
| 🟡 P2    | §1 SLAB Allocator             | Eliminates heap pressure for kernel objects             |
| 🟡 P2    | §4 Memory Pressure            | Prevents OOM crashes — safety net for all caches       |
| 🟢 P3    | §2 vmalloc                    | Large kernel buffers without physical contiguity        |
| 🟢 P3    | §3 Growable Heap              | Eliminates fixed 2 MiB heap ceiling                     |
| 🟢 P3    | §6 Architecture Docs          | Consolidates memory design knowledge                    |
| 🔵 P4    | §5 NUMA Awareness             | Advanced — only matters for multi-socket servers        |

---

## Key Files

| File                              | Purpose                              |
|-----------------------------------|--------------------------------------|
| `src/kernel/mm/slab.c`            | [NEW] SLAB object caches             |
| `include/kernel/mm/slab.h`        | [NEW] SLAB API header                |
| `src/kernel/mm/vmalloc.c`         | [NEW] Virtual contiguous allocator   |
| `include/kernel/mm/vmalloc.h`     | [NEW] vmalloc API header             |
| `src/kernel/mm/heap.c`            | [MODIFY] Make growable               |
| `src/kernel/mm/pmm.c`             | [MODIFY] Add pressure hooks          |

---

## OS Comparison

| Feature                          | 🪟 Windows 11                     | 🐧 Linux                          | 🚀 Impossible OS                            |
| -------------------------------- | -------------------------------- | -------------------------------- | ------------------------------------------ |
| Kernel heap                      | ✅ Non-paged pool (growable)      | ✅ kmalloc (SLUB)                 | ⬜ §3 P3 — currently fixed 2 MiB            |
| SLAB/look-aside list allocator   | ✅ Look-aside lists (ExAllocate*) | ✅ SLUB (fast-path, per-CPU)      | ⬜ §1 P2                                    |
| Virtually contiguous allocator   | ✅ MmMapIoSpace / MmAllocate*     | ✅ vmalloc                        | ⬜ §2 P3                                    |
| Memory pressure callbacks        | ✅ Memory notification events     | ✅ Shrinker callbacks             | ⬜ §4 P2                                    |
| Identity-mapped physical RAM     | ✅ KSEG / Direct Map              | ✅ Direct Map (kernel linear map) | ✅ Done — full physical range mapped        |
| NUMA-aware allocation            | ✅ NUMA node affinity             | ✅ numactl, cpuset                | ⬜ §5 P4 (stretch)                          |
| PMM (physical frame allocator)   | ✅ MmAllocateContiguousMemory     | ✅ get_free_pages / alloc_pages   | ✅ Done — `pmm_alloc_contiguous()`          |
| **No fragmentation SLAB caches** | ✅ (look-aside lists)             | ✅ (SLUB per-CPU)                 | ⬜ **§1 P2 — O(1), zero fragmentation**     |
| **Pressure → cache eviction**    | ✅                                | ✅ (shrinkers)                    | ⬜ **§4 P2 — simpler than Linux shrinkers** |
