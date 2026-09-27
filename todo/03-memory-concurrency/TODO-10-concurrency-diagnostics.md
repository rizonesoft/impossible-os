---
schema_version: 1
id: concurrency-diagnostics
domain: 03-memory-concurrency
status: active
title: "TODO-10 -- Concurrency & Memory Diagnostics"
---

# TODO-10 -- Concurrency & Memory Diagnostics

> **Goal:** Add every runtime and build-time diagnostic needed to catch concurrency bugs, memory corruption, and liveness failures before they reach production: thread stack guard pages, preemption count, heap canaries + SLAB red zones, lockdep, kernel watchdog, KCSAN data-race detector, graphical deadlock visualisation, named lock browser, KASAN kernel address sanitiser, and a unified `/sys/mem` memory observability interface.

> [!IMPORTANT]
> All sections gated on build flags (`LOCKDEP=1`, `KCSAN=1`, `KASAN=1`) must have **zero overhead in release builds** -- every data structure, hook, and counter must be inside `#ifdef` guards. Stack guard pages and `/sys/mem` are always-on. Preemption count is always-on; it is required by lockdep and KCSAN during their diagnostic work.

## Inputs

- [`include/kernel/sched/task.h`](../../include/kernel/sched/task.h)
- [`src/kernel/sched/sched.c`](../../src/kernel/sched/sched.c)
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c), [`src/kernel/mm/pmm.c`](../../src/kernel/mm/pmm.c)
- [`src/kernel/mm/heap.c`](../../src/kernel/mm/heap.c)
- [`include/kernel/sched/spinlock.h`](../../include/kernel/sched/spinlock.h), [`include/kernel/sched/mutex.h`](../../include/kernel/sched/mutex.h), [`include/kernel/sched/rwlock.h`](../../include/kernel/sched/rwlock.h)
- [`src/desktop/`](../../src/desktop/) -- GFX subsystem used by §7 deadlock visualiser
- → XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §1` -- SLAB allocator per-object layout; §5 SLAB red zones extend that structure
- → XREF: `03-memory-concurrency/TODO-07-smp-phase2.md §8` -- `READ_ONCE`/`WRITE_ONCE` macros; KCSAN (§7) uses them as suppressor annotations
- → XREF: `03-memory-concurrency/TODO-08-advanced-sync.md §4` -- `preempt_disable`/`preempt_enable` defined in TODO-08 §4; §4 here ensures that definition is in place and extends it with debug assertions
- → XREF: `02-kernel-core/TODO-08-time-filetime-management.md §3` -- `uptime_ns()` / tick counter for watchdog hold-time tracking in §3 and lock hold-time in §8

## Outcome

- Every thread stack has a `PROT_NONE` guard page at its base; a stack overflow triggers a clean `#PF` panic instead of silent memory corruption.
- `preempt_disable()` / `preempt_enable()` are active with debug-mode assertions in `schedule()`; lockdep and KCSAN call them to inhibit preemption during diagnostic checks.
- `kmalloc` blocks carry a magic-word canary validated on `kfree`; SLAB objects have a red-zone page beyond each allocation; violations are caught at free time.
- Lockdep builds a directed lock-class dependency graph; a DFS cycle detected at `mutex_lock` time fires immediately with the full held-locks stack -- before any actual deadlock.
- The kernel watchdog monitors per-CPU heartbeat counters every 5 s; a soft lockup triggers a serial log and a red framebuffer banner -- configurable to panic or reboot.
- KCSAN intercepts every memory read/write via `__tsan_read*`/`__tsan_write*` callbacks; concurrent accesses without a common lock are reported with access addresses and stack traces.
- On a lockdep cycle detection, the kernel draws a directed graph of thread boxes and lock-dependency arrows directly onto the framebuffer -- cycle edges red, non-cycle grey.
- `/sys/locks` VFS file and `locks` shell command expose every live lock: type, name, owner thread, waiter count, and hold time in milliseconds.
- KASAN maps one shadow byte per 8 kernel heap bytes; poisoned-byte accesses (use-after-free, overflow) fire with a precise stack trace; freed objects enter a quarantine queue before their shadow is cleared.
- `/sys/mem` VFS file and `memstats` shell command report `MemTotal`, `MemFree`, `Committed`, `SlabInuse`, `HugePages_*`, `CompressedPages`, and `PressureLevel`; Win32 `GlobalMemoryStatusEx` reads this data.

## Implementation Order

| ⭐  | Order | Deliverable                                        | Depends On                             | Status |
| --- | :---: | -------------------------------------------------- | -------------------------------------- | :----: |
| 💎  |   1   | §1 Thread stack guard pages                        | `vmm_map_guard`, page fault handler    |  [ ]   |
| 💎  |   2   | §2 Preemption count (debug assertions + extension) | TODO-08 §4 definition                  |  [ ]   |
| 💎  |   3   | §3 Heap canaries + SLAB red zones                  | TODO-03 §1 SLAB layout                 |  [ ]   |
| 💎  |   4   | §8 Named lock browser (`/sys/locks` + `locks` cmd) | lock registry hook in `mutex_init`     |  [ ]   |
| 💎  |   5   | §4 Lockdep -- class graph + DFS cycle detection    | §2, §4 (`/sys/locks` for output)       |  [ ]   |
| 💎  |   6   | §5 Kernel watchdog thread                          | §2, tick counter                       |  [ ]   |
| 💎  |   7   | §9 KASAN -- 1:8 shadow memory + quarantine         | §3 (canary baseline), PMM hook         |  [ ]   |
| 💎  |   8   | §6 KCSAN -- `__tsan_*` callbacks + shadow cells    | §2, §7 (shadow memory pattern)         |  [ ]   |
| ⭐  |   9   | §7 Graphical deadlock visualisation                | §5 (lockdep cycle data), GFX subsystem |  [ ]   |
| ⭐  |  10   | §10 `/sys/mem` unified memory observability        | §3, §7, §9 (stat sources)              |  [ ]   |

> 💎 = parity -- stack guards, preempt count, lockdep, watchdog, KASAN, KCSAN, and memory stats all have Linux equivalents; stack guards and named sync objects have Windows equivalents.
> ⭐ = exclusive -- the graphical deadlock visualiser (framebuffer graph, not a text dump) and the unified `/sys/mem` cross-subsystem snapshot are differentiators over both Linux `dmesg` dumps and Windows bluescreen stop codes.

---

## 1. Thread Stack Guard Pages `[Sonnet]`

Map a `PROT_NONE` page immediately below each thread's stack at `thread_create()` time. The page fault handler checks whether the faulting address is in any thread's guard range; if so, it panics cleanly with thread name and stack pointer rather than silently corrupting adjacent memory.

**Files:** `src/kernel/sched/sched.c`, `src/kernel/mm/vmm.c`, `include/kernel/mm/vmm.h`

> [!CAUTION]
> Thread stacks must be allocated via `pmm_alloc_contiguous()` -- default stack is 64 KB, well over the 4 KB `kmalloc` limit. The guard page has no physical backing frame; it is a VMM-only mapping with `PROT_NONE`. Unmap it on `thread_exit()` / `thread_destroy()` before freeing the stack.

- [ ] Add `guard_page_va` (`uintptr_t`) field to `task_t`; set during `thread_create()`
- [ ] `vmm_map_guard(va, PAGE_SIZE)` -- map one page at `va` as `PROT_NONE` (no read, write, exec); no physical frame backing
- [ ] In `thread_create()`: call `vmm_map_guard(stack_base, PAGE_SIZE)` after stack alloc; set `task->guard_page_va = stack_base`
- [ ] Page fault handler: if `faulting_addr >= guard_page_va && faulting_addr < guard_page_va + PAGE_SIZE`, emit `[PANIC] Stack overflow: thread '%s' (tid=%u) at 0x%016llx; RSP=0x%016llx` and halt
- [ ] `thread_destroy()`: call `vmm_unmap_guard(task->guard_page_va, PAGE_SIZE)` before `pmm_free(stack)`
- [ ] Test: thread that recurses infinitely -- verify clean stack overflow panic, not silent corruption
- [ ] Commit: `"sched: thread stack guard pages -- vmm_map_guard, #PF overflow detection"`

## 2. Preemption Count -- Debug Extensions `[Opus]`

Extend the `preempt_disable()` / `preempt_enable()` definition from `TODO-08 §4` with debug-mode assertions, a nesting depth cap, and documentation of the interaction with lockdep and KCSAN (both of which call `preempt_disable()` internally to prevent races during their shadow-memory checks).

**Files:** `include/kernel/sched/task.h`, `src/kernel/sched/sched.c`

> [!IMPORTANT]
> This section does **not** re-implement `preempt_disable`/`preempt_enable` -- those are defined in TODO-08 §2. It adds: (a) a debug assertion in `schedule()` that `preempt_count == 0`; (b) a `PREEMPT_MAX_DEPTH` cap with a `[BUG]` log on overflow; (c) a `preempt_count()` accessor macro used by lockdep and KCSAN to gate their checks.

- [ ] Add `PREEMPT_MAX_DEPTH 8` constant; in `preempt_disable()` (debug build): if `current->preempt_count >= PREEMPT_MAX_DEPTH`, emit `[BUG] preempt nesting overflow in thread '%s'` and continue (do not panic -- avoid recursive fault)
- [ ] `schedule()` debug assertion: `ASSERT(current->preempt_count == 0, "schedule() called with preempt_count=%u")`
- [ ] `preempt_count()` accessor macro: `(current->preempt_count)` -- used as a read-only probe by lockdep/KCSAN
- [ ] `preemptible()` macro: `(preempt_count() == 0 && !irqs_disabled())` -- used by lockdep to verify safe lock acquisition context
- [ ] Document in header: lockdep and KCSAN call `preempt_disable()` during their internal checks; this is safe and expected
- [ ] Commit: `"sched: preemption count debug extensions -- nesting cap, schedule assert, preemptible()"`

## 3. Heap Canaries + SLAB Red Zones `[Sonnet]`

Place a magic-word canary at the end of every `kmalloc` block; validate it on `kfree`. For SLAB caches, map a `PROT_NONE` red-zone page after each object's backing physical pages. Both mechanisms are gated on `KMALLOC_DEBUG` and catch out-of-bounds writes at free time rather than silently propagating corruption.

**Files:** `src/kernel/mm/heap.c`, `include/kernel/mm/heap.h`; SLAB: `src/kernel/mm/slab.c`

- [ ] `KMALLOC_DEBUG` build flag: when set, allocate `size + sizeof(uint64_t)` bytes; write `CANARY_MAGIC (0xDEADC0DEDEADC0DEULL)` at `ptr + size`
- [ ] `kfree()` (debug): read canary at `ptr + original_size`; if != `CANARY_MAGIC`, emit `[HEAP] Canary corruption in kfree at %p (expected 0x..., got 0x...)` and panic
- [ ] SLAB red zone: for each slab object, call `vmm_map_guard(object_end_page, PAGE_SIZE)` immediately after the object's last page
- [ ] SLAB free validation: `kmem_cache_free()` checks the last `SLAB_REDZONE_BYTES` of the object for the `SLAB_REDZONE_MAGIC` pattern before returning to the free list
- [ ] `SLAB_POISON` flag: fill freed SLAB objects with `0x6B` (`POISON_FREE`); fill on alloc with `0x5A` (`POISON_ALLOC`) to catch use-after-free
- [ ] Boot log (debug): `[HEAP] canary protection active (KMALLOC_DEBUG=1)`
- [ ] Commit: `"mm: heap canaries + SLAB red zones -- kmalloc canary, SLAB_POISON, guard page"`

## 4. Lock Dependency Validator (Lockdep) `[Opus]`

Build a directed lock-class dependency graph at runtime. On every `mutex_lock`, `rwlock_write_lock`, and `spin_lock`, record the transition from each already-held lock class to the acquiring class. A DFS cycle check on this graph fires immediately -- before the thread sleeps -- with the full held-locks stack printed to the serial log and `/sys/locks`.

**Files:** `src/kernel/sched/lockdep.c` (new), `include/kernel/sched/lockdep.h` (new)

> [!IMPORTANT]
> Lock **class** -- not lock instance. Two `mutex_t` variables initialised from the same source location belong to the same class. Use `__FILE__:__LINE__` as the class key, hashed to a class index. The per-thread held-locks stack records class indices, not pointer addresses, so the graph captures patterns rather than specific lock instances.
> Gate everything on `#ifdef LOCKDEP`. Zero overhead in release builds -- no struct fields added to `mutex_t` in release; use a parallel array in debug.

- [ ] Define `lock_class_t { uint32_t id; const char *name; const char *file; int line; }` -- one per unique lock initialisation site
- [ ] Per-thread held-locks stack (debug only): fixed array of `lock_class_t *` up to `LOCKDEP_MAX_HELD (16)`
- [ ] `LOCK_CLASS_KEY` macro: `static lock_class_key_t __lock_key_##__LINE__ = { .file = __FILE__, .line = __LINE__ }`; use as class identifier
- [ ] On every `mutex_lock` / `rwlock_write_lock` / `spin_lock` (debug): push class to per-thread stack; for each already-held class H, add edge `H → new_class` to global dependency graph if not present; run DFS from `new_class` -- if a back-edge to any held class is found, fire
- [ ] Cycle report: `[LOCKDEP] Deadlock detected: <classA> → <classB> → <classA>` with each class's file/line and the full per-thread held-locks stack
- [ ] `LOCKDEP_INIT_MAP(lock, name)` macro wraps lock init and registers the class
- [ ] Apply to: `mutex_t`, `rwlock_t`, `spinlock_t`; skip `condvar` and `semaphore` (different semantics)
- [ ] All state inside `#ifdef LOCKDEP`; release builds compile to zero instructions
- [ ] Commit: `"debug: lockdep -- lock-class graph, DFS cycle detection, held-locks stack"`

## 5. Kernel Watchdog Thread `[Sonnet]`

A high-priority kernel thread checks per-CPU heartbeat counters every 5 seconds. If a CPU's counter has not advanced since the last check (indicating the PIT ISR has not fired -- a spinlock hold, infinite loop, or interrupt storm), the watchdog fires: logs to serial, draws a red banner on the framebuffer, and optionally panics or reboots per Registry configuration.

**Files:** `src/kernel/sched/watchdog.c` (new), `include/kernel/sched/watchdog.h` (new)

> [!NOTE]
> The watchdog complements lockdep -- lockdep catches *potential* deadlocks from lock ordering analysis; the watchdog catches *actual* hangs already in progress (infinite loops, runaway spinlocks that disabled interrupts). A machine that hung before lockdep could fire is only caught by the watchdog.

- [ ] Add per-CPU `cpu_heartbeat` (`atomic_uint64_t`) incremented in every PIT/LAPIC tick ISR
- [ ] Spawn `watchdog_thread` at `SCHED_FIFO` priority 99 (highest); loop with 5 s sleep
- [ ] In watchdog loop: snapshot `cpu_heartbeat[N]` for each online CPU; after sleep, re-check; if `delta == 0` for any CPU, fire
- [ ] Watchdog fire sequence: (1) `klog(LOG_CRIT, "WATCHDOG", "Soft lockup on CPU%u: stuck for %llu s")` to serial; (2) `watchdog_draw_banner(cpu_id)` -- red bar at framebuffer top with thread name and uptime; (3) execute `action`
- [ ] `action` from Registry `HKLM\SYSTEM\Watchdog\Action`: `"log"` (default), `"panic"`, `"reboot"`
- [ ] Timeout threshold from `HKLM\SYSTEM\Watchdog\TimeoutSeconds` (default: 10)
- [ ] Test: create a kernel thread that spins with IRQs disabled -- verify watchdog fires within threshold
- [ ] Commit: `"debug: kernel watchdog -- per-CPU heartbeat, soft lockup detection, framebuffer banner"`

## 6. KCSAN -- Kernel Concurrency Sanitiser `[Opus]`

Instrument every kernel memory read and write with `__tsan_read*` / `__tsan_write*` callbacks (via `-fsanitize=thread` adapted for freestanding). Each access checks a per-location shadow cell; a concurrent write without a common lock is reported with both access addresses, sizes, and call stacks. `READ_ONCE`, `WRITE_ONCE`, and `data_race()` annotations suppress known-benign races.

**Files:** `src/kernel/mm/kcsan.c` (new), `include/kernel/mm/kcsan.h` (new)

> [!IMPORTANT]
> KCSAN shadow memory is separate from KASAN shadow -- KCSAN tracks *timing* (which CPU last accessed an address); KASAN tracks *validity* (is this address poisoned). Both can coexist. KCSAN is gated on `KCSAN=1`; compile flag `-fsanitize=thread` emits `__tsan_*` callbacks that the kernel intercepts. Clang-19's `--target=x86_64-elf -fsanitize=thread` must not pull in the compiler-rt TSan runtime -- provide a freestanding `__tsan_init` stub.

- [ ] Define shadow cell: one `uint64_t` per aligned 8-byte memory region; encodes `{ cpu_id, access_type (R/W), access_size, clock }`
- [ ] `__tsan_read1/2/4/8(addr)` / `__tsan_write1/2/4/8(addr)`: read shadow cell; if another CPU wrote without a common lock since last read → report race
- [ ] Race report: `[KCSAN] DATA RACE: CPU%u write at %p (size=%u) races with CPU%u read at %p` + call stacks of both accesses
- [ ] `READ_ONCE(x)` / `WRITE_ONCE(x, v)` suppress KCSAN for that specific access (mark as intentionally racy)
- [ ] `data_race(x)` macro: `({ __kcsan_disable(); (x); __kcsan_enable(); })` -- suppress a complex expression
- [ ] `__tsan_init()` / `__tsan_func_entry()` / `__tsan_func_exit()` -- freestanding no-op stubs (keep the TSan runtime out)
- [ ] All KCSAN state inside `#ifdef KCSAN`; `KCSAN=1` added to `Makefile` for debug builds only
- [ ] Commit: `"debug: KCSAN -- __tsan_read/write callbacks, shadow cells, data_race() suppressor"`

## 7. Graphical Deadlock Visualisation `[Opus]`

**Design:** n/a -- a LOCKDEP-only debug overlay drawn straight to the framebuffer before the compositor; never shipped in a user build

When lockdep detects a cycle (§4), instead of a serial text dump, draw the dependency graph directly onto the framebuffer: thread boxes connected by directed arrows, cycle edges highlighted in red, non-cycle edges grey, lock class names labelling each edge. This makes the deadlock immediately readable without a serial log reader.

**Files:** `src/kernel/sched/lockdep.c`, `src/desktop/gfx_deadlock.c` (new), `include/desktop/gfx_deadlock.h` (new)

> [!NOTE]
> This is a genuine exclusive feature -- Linux prints a wall of text to `dmesg`; Windows shows a generic stop code. Impossible OS draws a directed graph on the live framebuffer, legible at a glance on any monitor attached to the machine.

- [ ] Define `deadlock_graph_t { node_t nodes[LOCKDEP_MAX_HELD]; edge_t edges[LOCKDEP_MAX_EDGES]; int cycle_mask; }` -- built from the lockdep cycle detection output
- [ ] `lockdep_fire_cycle()` populates `deadlock_graph_t` and calls `gfx_deadlock_draw(&graph)`
- [ ] `gfx_deadlock_draw()`: clear a 640×480 overlay region; lay out thread nodes in a circle; draw `gfx_fill_rect` box per node, thread name label via `gfx_draw_text`
- [ ] Draw edges: `gfx_draw_line(src_centre, dst_centre)` with an arrowhead; cycle edges 2 px in `status_critical` and labelled "cycle" with the critical status glyph (never colour alone); non-cycle edges 1 px in `control_strong_stroke`
- [ ] Label each edge with the lock class name and `FILE:LINE` source location
- [ ] Call `fb_swap()` to flush immediately -- visible before any shell/log output
- [ ] Gate on `#ifdef LOCKDEP`; no GFX calls in release builds
- [ ] Commit: `"debug: graphical deadlock visualisation -- framebuffer graph, cycle edges red, lock labels"`

## 8. Named Lock Browser (`/sys/locks`) `[Sonnet]`

Register every initialised synchronisation primitive in a global lock registry. Expose the registry as a `/sys/locks` VFS virtual file showing type, name, owner thread, waiter count, and hold time. A `locks` shell command reads and formats it; the Task Manager lock viewer panel queries the same file.

**Files:** `src/kernel/sched/lock_registry.c` (new), `include/kernel/sched/lock_registry.h` (new), `src/kernel/fs/sysfs_locks.c` (new), `src/shell/cmd_locks.c` (new)

- [ ] Define `lock_entry_t { lock_type_t type; const char *name; uint64_t owner_tid; uint32_t waiter_count; uint64_t acquire_tick; }` -- stored in a global singly-linked list
- [ ] `lock_registry_add(entry)` called from `mutex_init()`, `rwlock_init()`, `spinlock_init()`, `semaphore_init()`, `ticket_lock_init()` -- inserts into registry under a `ticket_lock_t`
- [ ] `lock_registry_remove(entry)` called from `mutex_destroy()` etc.
- [ ] Update `owner_tid` and `acquire_tick` on `mutex_lock` / `ticket_lock` acquire; clear on release; increment `waiter_count` when thread blocks
- [ ] `/sys/locks` VFS read callback: iterate registry, format one row per entry: `TYPE  NAME              OWNER   WAITERS  HELD_MS`
- [ ] `locks` shell command: reads `/sys/locks`, pretty-prints with column alignment
- [ ] Wire lock registry to Task Manager lock viewer panel (→ XREF: `09-desktop-shell` domain)
- [ ] Commit: `"kernel: /sys/locks named lock browser -- registry, VFS file, locks shell command"`

## 9. KASAN -- Kernel Address Sanitiser `[Opus]`

Map one shadow byte per 8 kernel heap bytes. `kmalloc` poisons shadow bytes as `KASAN_FREE` on free and `KASAN_ALLOC` on alloc. Any load or store to a poisoned address fires with a precise stack trace. Freed objects enter a per-CPU quarantine queue before their shadow bytes are cleared, catching use-after-free across the allocation/free boundary.

**Files:** `src/kernel/mm/kasan.c` (new), `include/kernel/mm/kasan.h` (new)

> [!IMPORTANT]
> Shadow memory must be placed in a dedicated kernel virtual address region -- typically at `KASAN_SHADOW_BASE` = `kernel_base - (heap_size / 8)`. Map shadow pages lazily (on first heap page alloc) to avoid allocating shadow for the entire address space at boot. Shadow memory pages are allocated from the PMM directly, not via `kmalloc` (avoid recursion).

- [ ] Reserve `KASAN_SHADOW_REGION` in the kernel virtual address map: `[KASAN_SHADOW_BASE, KASAN_SHADOW_BASE + heap_size/8)`
- [ ] `kasan_poison(addr, size, tag)` -- write `tag` byte to `shadow[(addr - heap_base) / 8]` through `shadow[(addr - heap_base + size) / 8]`
- [ ] `kasan_unpoison(addr, size)` -- write `KASAN_VALID (0x00)` to shadow range
- [ ] Intercept `kmalloc(size)`: on alloc, `kasan_unpoison(ptr, size)`; on `kfree`, `kasan_poison(ptr, original_size, KASAN_FREE_TAG 0xFF)`
- [ ] Intercept memory accesses via compiler `-fsanitize=kernel-address` (clang-19 freestanding); `__asan_load*`/`__asan_store*` callbacks check shadow byte; if poisoned, emit `[KASAN] Use-after-free at %p (size=%u, shadow=0x%02x)` with stack trace
- [ ] Per-CPU quarantine queue: `kfree` enqueues into quarantine (keeps shadow poisoned); `kasan_drain_quarantine()` runs from `schedule()` idle to return blocks to the PMM and clear shadow
- [ ] Quarantine drain threshold: 1 MiB or 100 entries, whichever is hit first
- [ ] `KASAN=1` Makefile flag; all KASAN state inside `#ifdef KASAN`; zero overhead in release
- [ ] Boot log (debug): `[KASAN] shadow memory at 0x%016llx–0x%016llx (%zu KB shadow for %zu MB heap)`
- [ ] Commit: `"mm: KASAN -- 1:8 shadow memory, kmalloc intercept, quarantine, use-after-free detection"`

## 10. `/sys/mem` Unified Memory Observability `[Sonnet]`

Aggregate statistics from PMM, SLAB, huge pages, compressed memory, and swap into a single `/sys/mem` VFS file readable by any process. A `memstats` shell command formats it. Win32 `GlobalMemoryStatusEx` and `NtQuerySystemInformation(SystemPerformanceInformation)` read from the same data source.

**Files:** `src/kernel/fs/sysfs_mem.c` (new), `src/shell/cmd_memstats.c` (new)

- [ ] Define `mem_stats_t`: `total_pages`, `free_pages`, `committed_pages`, `swap_total`, `swap_free`, `slab_inuse_bytes`, `hugepages_2m_total`, `hugepages_2m_free`, `hugepages_1g_total`, `compressed_pages`, `pressure_level` (0–3)
- [ ] `mem_stats_collect()` -- aggregate from: `pmm_free_pages()`, `slab_total_inuse()` (→ XREF TODO-03 §10), `vmm_huge_page_stats()` (→ XREF TODO-05 §4–§2), `zram_compressed_pages()` (→ XREF TODO-05 §8), `swap_stats()` and pager counters (→ XREF TODO-04 §2,§8)
- [ ] `/sys/mem` VFS read callback: call `mem_stats_collect()`, format as key=value lines: `MemTotal=%llu kB`, `MemFree=%llu kB`, `Committed=%llu kB`, etc.
- [ ] `memstats` shell command: reads `/sys/mem`, prints formatted table with human-readable units (MiB)
- [ ] `GlobalMemoryStatusEx(&ms)` Win32 stub: calls `mem_stats_collect()`, fills `MEMORYSTATUSEX` struct
- [ ] `NtQuerySystemInformation(SystemPerformanceInformation, &perf, ...)` → fills `SYSTEM_PERFORMANCE_INFORMATION` from `mem_stats_t`
- [ ] `pressure_level` updated by memory pressure notifier (→ XREF TODO-03 §8) -- 0 = normal, 1 = moderate, 2 = high, 3 = critical
- [ ] Commit: `"kernel: /sys/mem -- unified memory stats, memstats shell command, GlobalMemoryStatusEx"`

---

## OS Comparison


| ⭐  | Feature                                                  | 🪟 Win11                                           | 🐧 Linux                                                        | 🚀 Impossible OS                                             |
| --- | -------------------------------------------------------- | -------------------------------------------------- | --------------------------------------------------------------- | ------------------------------------------------------------ |
| 💎  | Thread stack guard pages                                 | ✅ Guard page per thread stack;                    | ✅ `MAP_STACK` + `SIGSEGV` on overflow;                         | ⬜ §1 -- `vmm_map_guard`, `#PF` panic with thread            |
| 💎  | Preemption count + debug assertions                      | ✅ `IRQL` preemption depth in `KTHREAD`            | ✅ `preempt_count()` per-thread; `WARN_ON_ONCE` in `schedule()` | ⬜ §2 -- nesting cap, `schedule()` assert, `preemptible()`   |
| 💎  | Heap canaries + SLAB red zones                           | ✅ Driver Verifier special pool; `POOL_HEADER`     | ✅ `SLUB_DEBUG` -- red zones, poison,                           | ⬜ §3 -- `CANARY_MAGIC` at end of kmalloc,                   |
| 💎  | Lock dependency validator                                | ✅ Driver Verifier deadlock detection (limited)    | ✅ `CONFIG_LOCKDEP` -- lock-class graph, DFS                    | ⬜ §4 -- lock-class graph, DFS, held-locks stack,            |
| 💎  | Kernel watchdog                                          | ✅ Watchdog timer + `KeBugCheckEx` (hidden         | ✅ `CONFIG_LOCKUP_DETECTOR` -- soft/hard lockup detector        | ⬜ §5 -- `SCHED_FIFO` thread, heartbeat counter, framebuffer |
| 💎  | KCSAN data-race detector                                 | ❌ No equivalent in-kernel race detector           | ✅ `CONFIG_KCSAN` (v5.8+) -- `__tsan_*` callbacks,              | ⬜ §6 -- `__tsan_*` callbacks, shadow cells, `data_race()`   |
| ⭐  | Graphical deadlock visualisation on framebuffer          | ❌ Blue screen stop code only                      | ❌ Text `dmesg` dump only                                       | ⬜ §7 -- directed graph on framebuffer, red                  |
| ⭐  | Unified `/sys/locks` named lock browser                  | ❌ No readable kernel lock registry                | ❌ `/proc/locks` for file locks only                            | ⬜ §8 -- all sync types, owner +                             |
| 💎  | KASAN -- kernel address sanitiser                        | ✅ KASAN in WDK test mode                          | ✅ `CONFIG_KASAN` -- 1:8 shadow, quarantine,                    | ⬜ §9 -- 1:8 shadow, quarantine queue, `KASAN=1`             |
| ⭐  | Unified `/sys/mem` + Win32 `GlobalMemoryStatusEx` parity | ✅ `GlobalMemoryStatusEx`; no single readable file | ✅ `/proc/meminfo` -- many fields, no                           | ⬜ §10 -- one file, both Win32 +                             |

> **After §1–6, §8–9:** Impossible OS reaches full parity with Linux's best-in-class kernel diagnostics and exceeds Windows's limited Driver Verifier coverage. Two exclusive differentiators stand out: the graphical deadlock visualiser (§7) replaces text dumps with a directed graph directly on the framebuffer -- actionable at a glance on bare metal. The unified `/sys/mem` file (§10) serves both Linux-style `/proc/meminfo` readers and Win32 `GlobalMemoryStatusEx` callers from one data source, eliminating the per-subsystem stat divergence both existing OSes suffer from.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] Stack guard: thread recurses infinitely -- serial log shows `[PANIC] Stack overflow: thread '...' (tid=N)`, not silent corruption
- [ ] Heap canary: `kmalloc(16)` + write to `ptr[16]` + `kfree(ptr)` -- serial log shows `[HEAP] Canary corruption` (debug build)
- [ ] Lockdep: two locks acquired in `A→B` order in one thread, `B→A` in another -- lockdep fires `[LOCKDEP] Deadlock detected` before either thread actually deadlocks
- [ ] Watchdog: thread calls `cli` + infinite loop -- watchdog fires within `TimeoutSeconds`, red banner appears on framebuffer
- [ ] KCSAN: two threads read/write same global without a lock -- `[KCSAN] DATA RACE` report in serial log (debug build)
- [ ] Deadlock viz: trigger lockdep cycle -- framebuffer shows directed graph with red cycle edges within 1 s
- [ ] `/sys/locks`: `locks` shell command output shows at least `heap_lock`, `slab_lock`, `rq_lock` with owner and hold time
- [ ] KASAN: `kmalloc(8)` + `kfree(ptr)` + read `ptr[0]` -- `[KASAN] Use-after-free` report (debug build)
- [ ] `/sys/mem`: `memstats` shows `MemTotal`, `MemFree`, `SlabInuse`; Win32 `GlobalMemoryStatusEx` returns non-zero `dwTotalPhys`
- [ ] Commit: `"debug: concurrency and memory diagnostics -- guard pages, canaries, lockdep, watchdog, KCSAN, KASAN, /sys/mem"`
