---
schema_version: 1
id: pager-reclaim-working-set
domain: 03-memory-concurrency
status: active
title: "TODO-04 -- Pager, Reclaim, and Working Set Manager"
---

# TODO-04 -- Pager, Reclaim, and Working Set Manager

> **Goal:** Complete the memory-manager layer between basic `MEM_COMMIT` support and advanced VM features: pagefile ownership, working-set residency, background reclaim, dirty-page writeback, lazy file-backed faults, replacement policy, and user-visible trim/control APIs. When this TODO is done, Impossible OS has one canonical owner for how anonymous pages, mapped-file pages, transition pages, compressed pages, and pagefile-backed pages move through the system under memory pressure.

> [!IMPORTANT]
> **Current state:** `src/kernel/mm/swap.c` and `include/kernel/mm/swap.h` already implement a basic pagefile-backed clock pager with swap-encoded PTEs, but the implementation is global-array based, file-path specific, and not wired to per-process working sets, background reclaim, or modified-page writeback. `src/kernel/mm/mmap.c` eagerly loads file mappings because the current page-fault path cannot yet issue blocking VFS I/O safely from the fault context. `TODO-01`, `TODO-04`, and `TODO-09` already depend on swap, commit accounting, compressed memory, and `/sys/mem` stats, but no active TODO owns the full pager pipeline or the policy that decides whether a page stays resident, moves to transition/standby, is compressed, or is written to `pagefile.sys`.

## Inputs

- [`include/kernel/mm/swap.h`](../../include/kernel/mm/swap.h) -- existing swap-slot format, clock tracking, and public API
- [`include/kernel/mm/mmap.h`](../../include/kernel/mm/mmap.h) -- current mmap region metadata and fault hook
- [`include/kernel/mm/vmm.h`](../../include/kernel/mm/vmm.h) -- PTE helpers, page-fault entry points, and TLB helpers
- [`include/kernel/mm/pmm.h`](../../include/kernel/mm/pmm.h) -- frame ownership and physical-page accounting
- [`src/kernel/mm/swap.c`](../../src/kernel/mm/swap.c) -- current pagefile-backed swap implementation
- [`src/kernel/mm/mmap.c`](../../src/kernel/mm/mmap.c) -- eager file mapping baseline to be replaced by lazy faulting
- [`src/kernel/mm/vmm.c`](../../src/kernel/mm/vmm.c) -- page-fault routing, PTE updates, and region lookup
- [`src/kernel/sched/task.c`](../../src/kernel/sched/task.c) -- per-process working-set fields, counters, and lifecycle hooks
- -> XREF: `03-memory-concurrency/TODO-01-vmm-memory-protection.md §3,§14,§15,§16` -- demand paging, page locking, commit accounting, and process memory counters are upstream consumers and handoffs
- -> XREF: `03-memory-concurrency/TODO-03-advanced-allocator.md §9,§8` -- pressure notifications drive reclaim; `PagedPool` semantics depend on pageable residency rules here
- -> XREF: `03-memory-concurrency/TODO-05-advanced-virtual-memory.md §3,§8` -- `madvise` / `MEM_RESET` feed reclaim hints; compressed memory is layered on top of the pager handoff defined here
- -> XREF: `02-kernel-core/TODO-23-exception-dispatch-seh.md §2` -- page-fault triage must hand not-present faults to the pager without regressing kernel exception flow

## Outcome

- `pagefile.sys` is managed by a real pagefile manager with explicit size policy, slot accounting, locking, and failure states instead of fixed global arrays.
- Every process has a tracked working set with resident, transition, standby, modified, and locked-page accounting; trim decisions are explicit and observable.
- A background reclaim thread and modified-page writer keep free-memory watermarks healthy without forcing every allocation into ad hoc direct reclaim.
- File-backed mappings fault pages in lazily, support soft faults from standby/transition state, and no longer eagerly read entire files at `mmap()` time.
- Replacement policy is refault-aware: clean file-backed pages are reclaimed before anonymous pages, locked pages and DMA/nonpaged pages are never victims, and refaults promote hot pages back into the active set.
- Win32 and Linux-style control surfaces (`EmptyWorkingSet`, working-set min/max, `mincore`, trim telemetry, registry policy) are wired to one canonical pager implementation.

## Implementation Order

| ⭐  | Order | Deliverable                              | Depends On                 | Status |
| --- | :---: | ---------------------------------------- | -------------------------- | :----: |
| 💎  |   1   | §1 Pagefile manager + swap metadata foundation | --                         |  [ ]   |
| 💎  |   2   | §2 Working-set tracking + lock-aware residency rules | §1, T01 §14,§15            |  [ ]   |
| 💎  |   3   | §3 Background reclaim + modified-page writer | §1, §2, T03 §2             |  [ ]   |
| 💎  |   4   | §4 Lazy file-backed pager + soft/hard fault resolution | §1, §3, D02 T23 §2, T01 §3 |  [ ]   |
| 💎  |   5   | §5 Replacement policy + refault tracking | §2, §3, §4                 |  [ ]   |
| 💎  |   6   | §6 Control surface + observability handoff | §2-5                       |  [ ]   |

> 💎 = parity work: Windows and Linux both ship a first-class pager, pagefile/swap policy, working-set trim, and reclaim daemons. Impossible OS must reach that baseline before higher VM features feel real.

---

## 1. Pagefile Manager + Swap Metadata Foundation

Replace the fixed-slot, path-special-cased swap globals with a real pagefile manager that owns slot allocation, policy, and failure handling.

**Files:** `include/kernel/mm/swap.h`, `src/kernel/mm/swap.c`, `include/kernel/mm/pager.h` (new), `src/kernel/mm/pager.c` (new)

> [!IMPORTANT]
> `swap.c` is already named in `D05 T05 §11` as a raw `vfs_*` caller. Do not spread raw file I/O through the new pager code. Hide all pagefile reads and writes behind a narrow backend (`pagefile_read_slot`, `pagefile_write_slot`) so the future `CreateFile` migration changes one layer, not every reclaim path.

- [ ] Define `pagefile_mgr_t` with slot bitmap or extent map, `slot_count`, `used_count`, `io_lock`, `backend_state`, and policy fields (`pagefile_bytes`, `pagefile_path`, `allow_runtime_grow`)
- [ ] Replace `SWAP_MAX_SLOTS` static-array ownership with runtime-sized slot allocation; keep `SWAP_ENCODE_PTE()` stable so existing PTE encodings remain valid
- [ ] Add pagefile discovery and creation policy: use `C:\Impossible\System\pagefile.sys` by default, honor Registry size override, and surface a degraded mode when the system volume is unavailable instead of silent partial init
- [ ] Move `swap_init`, `swap_out`, `swap_in`, and `swap_stats` onto the manager object; protect slot allocation/free and reverse-map updates with explicit locking
- [ ] Serialize the `swap_out` + clock PTE transaction (verify/mark/free/publish) against preemption via an IRQ-safe address-space lock; §9 left it a preemptible TOCTOU → XREF: `02-kernel-core/TODO-33-higher-half-kernel-relocation.md` §9
- [ ] Add `pagefile_backend_read(slot, buf)` / `pagefile_backend_write(slot, buf)` wrappers so later `D05 T05 §11` migration is isolated to one backend
- [ ] Add `swap_stats()` / `pagefile_stats()` reporting: total slots, used slots, failed writes, failed reads, degraded-state reason
- [ ] Reject unswappable mappings early: NonPagedPool, DMA buffers, kernel stacks, guard pages, and explicitly locked pages never receive swap PTEs
- [ ] Commit: `"mm: pager foundation -- pagefile manager, slot bitmap, swap stats, backend wrapper"`

**Test checkpoint:** Serial log shows `[PAGEFILE] ready: path=C:\Impossible\System\pagefile.sys slots=<n>` or a single degraded-state reason. `swap_stats()` reports stable totals after repeated swap-out/swap-in cycles. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

**Regression risk:** This section changes the backing store for every swapped page. If metadata corruption appears, revert to the current fixed-slot path and keep the new backend wrapper disabled behind a compile-time guard.

## 2. Working-Set Tracking + Lock-Aware Residency Rules

Make residency an explicit per-process concept so reclaim, trimming, and counters have a real owner instead of scattered ad hoc checks.

**Files:** `include/kernel/mm/pager.h`, `src/kernel/mm/pager.c`, `include/kernel/sched/task.h`, `src/kernel/sched/task.c`

> [!IMPORTANT]
> → XREF: `TODO-01-vmm-memory-protection.md §14` -- locked pages must be marked unevictable here. `NtLockVirtualMemory` is not complete until the pager respects the lock during victim selection and trim.

- [ ] Add `working_set_t` or equivalent per-process residency state: resident pages, transition pages, standby references, modified count, locked count, min/max working-set targets
- [ ] Extend page metadata with residency class (`ACTIVE`, `STANDBY`, `MODIFIED`, `TRANSITION`, `LOCKED`, `SWAPPED`) and owning process or section reference
- [ ] Wire `NtLockVirtualMemory` / `mlock` metadata from `T01 §14` into the pager so locked pages are excluded from trim and direct reclaim
- [ ] Add working-set attach/detach hooks to process creation, `fork`/section-map paths, and process teardown so every mapped page enters and leaves one owner cleanly
- [ ] Add min/max working-set controls and trim eligibility bookkeeping compatible with `EmptyWorkingSet` / `SetProcessWorkingSetSizeEx`
- [ ] Update per-process counters consumed by `T01 §16`: resident set, peak working set, hard faults, soft faults, trimmed pages
- [ ] Define victim filters that categorically skip kernel text/data, guard pages, DMA/nonpaged pages, active I/O pages, and pages pinned by section or COW invariants
- [ ] Commit: `"mm: working set tracking -- residency classes, locked-page policy, trim bookkeeping"`

**Test checkpoint:** A process with locked pages keeps those pages resident during trim; unlocked pageable pages move out of the working set first. `GetProcessMemoryInfo` style counters show resident, peak, soft-fault, and hard-fault deltas after synthetic pressure. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

## 3. Background Reclaim + Modified-Page Writer

Add the reclaim threads and queues that keep memory healthy under pressure instead of waiting for every caller to fail first.

**Files:** `src/kernel/mm/pager.c`, `include/kernel/mm/pager.h`, `src/kernel/mm/swap.c`, `src/kernel/mm/pressure.c`

> [!WARNING]
> Reclaim must never issue blocking pagefile or mapped-file I/O from interrupt context. Direct reclaim is only allowed from sleepable allocation paths. IRQ, DPC, and page-table critical sections may trigger wakeups, not the I/O itself.

- [ ] Add `mm_reclaimd` background thread that wakes from `T03 §2` pressure notifications and maintains low/high free-page watermarks
- [ ] Add a modified-page writer thread that drains dirty anonymous pages to the pagefile and dirty file-backed pages to their mapped file, then moves them to standby or transition state
- [ ] Implement reclaim tiers: drop clean standby file-backed pages first, write modified file-backed pages next, then swap anonymous pages, preserving locked/unevictable pages throughout
- [ ] Add direct-reclaim fallback for sleepable allocators when `mm_reclaimd` cannot keep up; fail fast rather than stalling forever in non-sleepable contexts
- [ ] Introduce reclaim batching and hysteresis policy (`TrimBatchPages`, `ReclaimWakeFreePages`, `ModifiedWriterIntervalMs`) via Registry keys under `HKLM\SYSTEM\Memory`
- [ ] Emit structured `klog` lines for reclaim start/stop, pages scanned, pages reclaimed, dirty pages written, and direct-reclaim stalls
- [ ] Handoff contract: `T05 §8` compressed memory may intercept the anonymous-page write path before pagefile write, but this section remains the owner of when reclaim asks for that handoff
- [ ] Commit: `"mm: reclaim engine -- background reclaimd, modified-page writer, direct-reclaim policy"`

**Test checkpoint:** Under forced pressure, serial shows `[RECLAIM] pressure=HIGH scanned=<n> reclaimed=<n>` and `[PAGEWRITE] wrote=<n> dirty pages`. High-pressure allocations recover without panicking, while non-sleepable allocations fail deterministically instead of hanging. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

## 4. Lazy File-Backed Pager + Soft/Hard Fault Resolution

Replace eager `mmap()` loading with real lazy page-in and make the fault path distinguish demand-zero, soft-fault, standby, and hard pagefile/mapped-file faults.

**Files:** `src/kernel/mm/mmap.c`, `include/kernel/mm/mmap.h`, `src/kernel/mm/vmm.c`, `src/kernel/mm/pager.c`

> [!IMPORTANT]
> → XREF: `D02 T23 §2` -- page-fault triage remains the authoritative dispatcher. This section supplies `pager_resolve_fault()` and the sleepable page-in path it calls. Do not duplicate exception classification logic here.

- [ ] Rewrite `mmap.c` so file-backed mappings reserve VMA metadata at map time and fault pages in lazily instead of reading the entire file during `mmap()`
- [ ] Add `pager_resolve_fault(addr, error_code, access_kind)` that distinguishes demand-zero, swap-backed hard faults, standby/transition soft faults, and mapped-file hard faults
- [ ] Introduce a sleepable page-in path for file-backed faults so the kernel never performs blocking VFS/AHCI I/O directly in the raw ISR fault branch; the faulting thread waits on pager I/O and retries cleanly
- [ ] Track dirty file-backed pages and `MAP_SHARED` writeback ownership so `msync()` and `munmap()` flush only modified pages rather than full eager regions
- [ ] Add clustered readahead / fault-around for sequential mapped-file faults and wire `MADV_SEQUENTIAL`, `MADV_RANDOM`, and `MADV_WILLNEED` hints from `T05 §4`
- [ ] Add soft-fault fast paths: if a page is already in standby, transition, or compressed backing, resolve without full filesystem I/O
- [ ] Preserve `MAP_PRIVATE` COW semantics during lazy faulting; writes to shared file-backed pages still split correctly
- [ ] Commit: `"mm: lazy file pager -- mmap reserve-only, pager_resolve_fault, clustered page-in"`

**Test checkpoint:** Mapping a large file no longer logs eager full-file reads at `mmap()` time. First touch logs `[PAGER] hard fault file-backed` once per cluster; second touch from standby logs `[PAGER] soft fault standby` with no disk I/O. `msync()` writes only dirty pages. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

**Regression risk:** This section changes page-fault resolution and mapped-file semantics. If file-backed faults loop or deadlock, revert to eager `mmap()` loading and keep `pager_resolve_fault()` disabled for file-backed VMAs only.

## 5. Replacement Policy + Refault Tracking

Upgrade victim selection from a flat clock array to a policy that can keep the real workingset hot and punish bad evictions instead of repeating them.

**Files:** `src/kernel/mm/pager.c`, `include/kernel/mm/pager.h`, `src/kernel/mm/swap.c`

> [!NOTE]
> Windows exposes transition, standby, and modified behavior; Linux exposes workingset/refault and swap-cache behavior. Impossible OS should unify the same ideas under one simpler pager policy, not keep the current global `clock_entries[]` forever.

- [ ] Replace the global `clock_entries[]` victim list with active/inactive tracking for anonymous and file-backed pages, or a Clock-Pro style equivalent that preserves hot vs cold distinction
- [ ] Add shadow-entry or refault-distance tracking so a recently evicted page that faults back quickly is promoted and not re-evicted immediately
- [ ] Add swap-cache or equivalent transition-cache ownership so swapped pages that refault quickly can avoid repeated disk reads when still resident in intermediate state
- [ ] Prefer reclaiming clean file-backed standby pages before anonymous dirty pages; anonymous pages become pagefile candidates only when cheaper reclaim sources are exhausted
- [ ] Add configurable `swappiness` or equivalent policy knob and document how it biases anon vs file-backed reclaim
- [ ] Integrate compressed-memory handoff from `T05 §8`: anonymous victims may be offered to compressed backing first, then pagefile, but the selection policy remains owned here
- [ ] Add reclaim-reason telemetry (`standby-hit`, `refault-promote`, `swap-write`, `trim-api`, `pressure-high`) for debugging and future tuning
- [ ] Commit: `"mm: pager policy -- active/inactive replacement, refault tracking, reclaim reasons"`

**Test checkpoint:** A sequential file scan under pressure evicts cold file-backed pages first; a hot anonymous working set that refaults immediately is promoted rather than thrashed. Serial log shows `[PAGER] refault promote` and `[PAGER] victim class=file-backed-clean` under the expected workloads. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

## 6. Control Surface + Observability Handoff

Wire the pager to the APIs and telemetry surfaces that make policy visible and debuggable from user mode, shell tools, and future diagnostics.

**Files:** `src/kernel/mm/pager.c`, `include/kernel/mm/pager.h`, `src/kernel/fs/sysfs_mem.c`, `src/kernel/sched/syscall.c`

> [!IMPORTANT]
> This section does not replace `T10 §10`. It supplies pager-owned counters and control points so `/sys/mem`, `GlobalMemoryStatusEx`, and process working-set APIs report real data instead of placeholders.

- [ ] Implement `EmptyWorkingSet`, `SetProcessWorkingSetSizeEx`, and `QueryWorkingSetEx` compatible kernel-side plumbing, plus Linux-compat `mincore` residency queries where appropriate
- [ ] Export pager counters to `T10 §10`: pagefile total/free, standby pages, modified pages, transition pages, hard faults, soft faults, refault promotions, reclaim stalls
- [ ] Expose per-process working-set telemetry to `T01 §16` and `NtQueryInformationProcess(ProcessVmCounters)` so RSS/peak/trim/fault numbers come from one source of truth
- [ ] Add Registry-backed pager tuning keys (`PagefileMiB`, `Swappiness`, `TrimBatchPages`, `FaultClusterPages`, `ModifiedWriterIntervalMs`) with bounds checking and defaults
- [ ] Add shell or debug commands for `pager` / `ws` summaries, or document the exact `/sys/mem` extensions that replace them if a new command is unnecessary
- [ ] Surface explicit degraded modes: pagefile unavailable, reclaim disabled in current context, compressed store unavailable, mapped-file pager backend offline
- [ ] Commit: `"mm: pager control surface -- working-set APIs, telemetry export, registry policy"`

**Test checkpoint:** `EmptyWorkingSet` trims pageable pages and logs `[WS] trim pid=<n> removed=<n>`. `/sys/mem` shows standby, modified, transition, and swap counts that change under pressure. Invalid Registry tuning values are clamped and reported once at boot. Test on: QEMU WHPX + TCG; VirtualBox; bare metal.

---

## OS Comparison

| ⭐  | Feature                               | 🪟 Win11                        | 🐧 Linux                         | 🚀 Impossible OS |
| --- | ------------------------------------- | ------------------------------- | -------------------------------- | ---------------- |
| 💎  | Pagefile / swap manager               | ✅ `pagefile.sys` managed       | ✅ swapfile or partition         | ⬜ Planned -- §1 |
| 💎  | Working-set trim and residency        | ✅ min/max + trim APIs          | ⚠️ reclaim + `mlock` + `mincore`  | ⬜ Planned -- §2 |
| 💎  | Background reclaim + page writer      | ✅ WS manager + modified writer | ✅ kswapd + writeback            | ⬜ Planned -- §3 |
| 💎  | Lazy mapped-file faults + readahead   | ✅ demand-paged sections        | ✅ lazy file pager               | ⬜ Planned -- §4 |
| 💎  | Refault-aware replacement policy      | ✅ standby or transition reuse  | ✅ workingset refault            | ⬜ Planned -- §5 |
| 💎  | Working-set and pager control surface | ✅ PSAPI + trim controls        | ⚠️ split across `/proc` + sysctls | ⬜ Planned -- §6 |

> After §1-§3, Impossible OS has a credible pagefile and reclaim core instead of isolated swap primitives.
> After §4-§5, mapped files and refault behavior reach the level expected from a real desktop OS.
> After §6, policy and telemetry are visible enough to debug and tune without grepping the kernel.

## Unit Tests

> Wire into `test_runner_init()` via `test_register_pager()` -- register in `src/kernel/test/test_runner.c`.
> Use `TEST_CAT_MM`. Keep unit tests focused on pure helpers and metadata transitions; do not call live pagefile I/O or full fault paths directly from a kernel test.

- [ ] Create `src/kernel/test/test_pager.c` with:
  - `SWAP_ENCODE_PTE(slot)` / `SWAP_DECODE_PTE(pte)` round-trip for representative slot IDs
  - Pagefile slot allocator returns distinct slots, then reuses freed slots safely
  - Victim-selection helper never returns locked, guard, DMA, or NonPagedPool pages
  - Working-set trim accounting decrements resident count and increments standby or swapped count exactly once
  - Refault-distance helper promotes a recent refault and rejects a stale one
  - Registry clamp helper bounds invalid `PagefileMiB` / `Swappiness` values to defaults
- [ ] Register in `test_runner_init()`: `test_register_pager()`
- [ ] Commit: `"test: add pager and working-set test suite"`

## Verification

- [ ] `bash scripts/build.sh clean` -> `tail -1 build/build.log` -> `=== BUILD OK ===`
- [ ] Boot log shows `[PAGEFILE] ready:` or one explicit degraded-mode reason
- [ ] Pressure run shows `[RECLAIM] pressure=HIGH` and `[PAGEWRITE] wrote=` without hangs
- [ ] File-backed mapping test shows first-touch hard faults followed by standby or soft faults on reuse
- [ ] `EmptyWorkingSet` or equivalent trim path logs `[WS] trim pid=` and updates `/sys/mem`
- [ ] `bash scripts/test.sh SUITE=mm QUIET=1` includes the pager suite with no failures
- [ ] Verify on: QEMU WHPX (2 CPUs), QEMU TCG, VirtualBox, bare metal
