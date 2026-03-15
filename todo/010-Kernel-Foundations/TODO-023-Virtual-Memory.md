# P0104 — Virtual Memory Enhancements

> **Goal:** Swap/pagefile and memory-mapped files — enabling more processes than
> physical RAM and efficient file I/O via pointer access.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Swap / Page File ✅

**Prompt:** This section is marked complete (disk-backed pagefile ✅). Verify the implementation is correct: confirm swap_init, swap_out, swap_in exist, the clock page replacement algorithm works, PTEs encode swap_id when Present=0, page faults trigger swap_in, and pagefile.sys is created at `C:\Impossible\System\pagefile.sys`. Verify Registry key `HKLM\SYSTEM\Memory\SwapSlots` controls size. Run `bash scripts/build.sh clean` and verify the boot log shows swap initialization. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to virtual memory or swap. Add notes, gotchas, and design decisions directly in this TODO section covering the swap system, pagefile.sys, clock replacement, and PTE encoding.

**Status: Disk-backed pagefile.** Swap out writes pages to `C:\Impossible\System\pagefile.sys`
via VFS. Swap in reads them back. Clock replacement, PTE encoding, and page fault → swap in
all work end-to-end with disk I/O. Swap size configurable via Registry `HKLM\SYSTEM\Memory\SwapSlots`.

- [x] Implement `swap_init(num_slots)` — initialize swap (currently RAM-backed)
- [x] Implement `swap_out(virt_addr)` — copy page to swap slot, free frame
- [x] Implement `swap_in(swap_id, virt_addr)` — read page back from swap slot
- [x] Implement Clock (second-chance) page replacement algorithm
- [x] Track swap state in page table entries (Present=0, swap_id encoded)
- [x] Handle page fault → check if page is swapped → `swap_in()` → retry
- [x] Commit: `"mm: swap / page file support"` (RAM-backed PoC)

**Disk-backed pagefile** ✅
- [x] Create pagefile: `C:\Impossible\System\pagefile.sys`
- [x] Replace `kmalloc` backing store with `vfs_write()`/`vfs_read()` to pagefile
- [x] Configurable swap size (default: 64 slots, stored in Registry `HKLM\SYSTEM\Memory\SwapSlots`)
- [x] Test: swap out → pagefile.sys → swap in → data integrity verified
- [x] Commit: `"mm: disk-backed swap via pagefile.sys"`

---

## 2. Memory-Mapped Files ✅

**Prompt:** This section is marked complete. Verify the implementation is correct: confirm `mmap`, `munmap`, `msync` exist, MAP_PRIVATE and MAP_SHARED work, `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls are registered. Verify the eager-load implementation reads file contents into mapped pages correctly. Run `bash scripts/build.sh clean`. Fix any inconsistencies in the TODO items below. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to memory mapping. Add notes, gotchas, and design decisions directly in this TODO section covering mmap semantics, MAP_PRIVATE/MAP_SHARED, and COW fault handling.

> **Note:** `SYS_MMAP=37` — verify this does not conflict with `SYS_SHMEM_UNMAP` in `TODO-022-IPC.md`. Check `syscall.h`.

- [x] Implement `mmap(addr, length, prot, flags, fd, offset)` — map file into address space
- [x] Implement `munmap(addr, length)` — unmap region
- [x] Implement `msync(addr, length)` — flush dirty pages to disk
- [x] Support `MAP_PRIVATE` (copy-on-write) and `MAP_SHARED` (shared writes)
- [x] Page fault handler handles COW for MAP_PRIVATE writes (eager-load for file data)
- [x] Add `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls
- [x] Test: mmap hello.txt, read first char as pointer → 'H' ✅
- [x] Commit: `"mm: memory-mapped files"`

---

## 3. Memory Protection (`mprotect`)

**Prompt:** `mprotect(addr, length, prot)` changes page permissions on an existing mapping — essential for W^X (write XOR execute) security policy, JIT compilers, and stack guards. The kernel updates the PTE flags for the specified range without remapping. `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` correspond to PTE R/W and NX bits. A guard page (PROT_NONE, one page below the stack) turns stack overflows into a page fault instead of silent corruption. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: mprotect and guard pages"`. Add notes directly in this TODO section about W^X enforcement and guard page placement.


- [ ] Implement `mprotect(addr, length, prot)` syscall — update PTE flags for range
- [ ] Define `PROT_NONE`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` constants
- [ ] Map PROT flags to PTE R/W bit and NX (No-Execute) bit
- [ ] Implement per-thread stack guard page (PROT_NONE page below stack bottom)
- [ ] Page fault on guard page → panic with "stack overflow" rather than silent corruption
- [ ] Add `SYS_MPROTECT` syscall
- [ ] Test: set region PROT_NONE → verify fault on access; set PROT_READ → verify read works
- [ ] Commit: `"mm: mprotect and guard pages"`

---

## 4. ASLR — Address Space Layout Randomization

**Prompt:** Without ASLR, the kernel, stack, heap, and mmap regions always load at fixed addresses — making exploit code trivially predictable. Both Windows and Linux enable ASLR by default. Randomize the base addresses of: kernel load address (already KASLR in Linux), user stack start, heap start, and mmap region start. Use the PIT tick counter XOR'd with a boot-time RDRAND value as the entropy source. Add a Registry key `HKLM\SYSTEM\Security\ASLR` (1=enabled, 0=disabled for debugging). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: ASLR — address space randomization"`. Add notes directly in this TODO section covering the entropy source, per-region randomization, and debug disable.

> **Beats:** Windows ASLR is opt-in per-binary. Impossible OS enables it for all user processes by default.


- [ ] Add `mm_aslr_enabled` flag (read from Registry at boot, default 1)
- [ ] Generate ASLR seed: `PIT ticks ^ RDRAND` at kernel init → store in `g_aslr_seed`
- [ ] Randomize user stack base: align to page, offset by 1–255 pages from stack top
- [ ] Randomize heap (`brk`) start: offset by 1–63 pages from BSS end
- [ ] Randomize mmap region start: offset per `mmap_base = DEFAULT_MMAP_BASE ^ (seed & MMAP_MASK)`
- [ ] Ensure all offsets are page-aligned
- [ ] Log chosen bases to serial at process spawn (debug builds only)
- [ ] Commit: `"mm: ASLR — address space randomization"`

---

## 5. Huge Pages (2 MiB pages for performance)

**Prompt:** The kernel already uses 2 MiB pages for its identity-mapped region (bootloader). User processes currently use 4 KiB pages everywhere. For large mappings (graphics buffers, file-backed regions > 4 MiB), using 2 MiB pages reduces TLB pressure and page-fault overhead dramatically. Linux calls these HugePages (`mmap(MAP_HUGETLB)`); Windows uses Large Page Support (`VirtualAlloc(MEM_LARGE_PAGES)`). Add a `MAP_HUGE` flag to `mmap` — if the region is 2 MiB aligned and of sufficient size, use PD entries directly instead of allocating PTs. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: huge page support (2 MiB pages)"`. Add notes directly in this TODO section.

> **Win:** Framebuffer back buffer (3.6 MiB) and `mmap` of large files benefit immediately.


- [ ] Add `MAP_HUGE` flag constant
- [ ] In `mmap`: if size ≥ 2 MiB and addr/size 2 MiB-aligned + `MAP_HUGE` set, use 2 MiB PD entries
- [ ] Physical allocator: `pmm_alloc_huge()` — allocate contiguous 2 MiB-aligned physical frame
- [ ] Page fault handler: recognize 2 MiB PD entry (PS bit) distinctly from 4 KiB PT entry
- [ ] `munmap` for huge pages: clear PD entry, mark physical 2 MiB frame free
- [ ] Use for: framebuffer back buffer mapping, large file mmaps
- [ ] Commit: `"mm: huge page support (2 MiB pages)"`

---

## 6. Copy-on-Write `fork()`

**Prompt:** `fork()` creates a child process that shares the parent's physical pages until either writes — at which point the written page is copied (COW). Without COW, `fork()` must deep-copy all pages upfront (~seconds for a large process). With COW, `fork()` is nearly instant — only a page table copy. Mark all user pages as read-only in both parent and child after fork; on write fault, copy the page and mark writable. This is how POSIX `fork()` works on Linux; Windows has no `fork()` equivalent. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: COW fork()"`. Add notes directly in this TODO section.

> **Beats:** Windows 11 — no `fork()`. Impossible OS gets both `fork()` + `exec()` (POSIX-style process creation).

> **Prerequisite:** `mprotect` (§3) must exist — fork uses PTE write-protect to trigger COW faults.


- [ ] `fork()`: duplicate page tables but mark all user pages read-only in parent + child
- [ ] Increment physical page reference count for each shared page
- [ ] On write page fault: if COW page (ref_count > 1), allocate new frame, copy, decrement old ref
- [ ] If ref_count == 1 after decrement: re-mark page writable (no copy needed)
- [ ] Add `SYS_FORK` syscall
- [ ] Test: fork a process, parent and child modify different memory — verify no corruption
- [ ] Commit: `"mm: COW fork()"`

---

## Priority Order

| Priority  | Section                          | Reason                                                       |
|-----------|----------------------------------|--------------------------------------------------------------|
| ✅ Done   | 1. Swap / Page File              | Verified complete — disk-backed pagefile                     |
| ✅ Done   | 2. Memory-Mapped Files           | Verified complete — mmap/munmap/msync                        |
| 🔴 P0     | 3. mprotect + guard pages        | W^X security + stack overflow detection                      |
| 🟠 P1     | 4. ASLR                          | Essential security — exploit mitigation                      |
| 🟠 P1     | 6. COW fork()                    | POSIX process creation; requires §3 mprotect first           |
| 🟡 P2     | 5. Huge Pages                    | Performance — framebuffer and large file mmap                |

---

## OS Comparison

| Feature                      | Windows 11              | Linux Kernel             | Impossible OS                      |
|------------------------------|-------------------------|--------------------------|------------------------------------|
| Swap / pagefile              | ✅ `pagefile.sys`       | ✅ swap partition/file   | ✅ §1 Done — `pagefile.sys`        |
| Memory-mapped files          | ✅ `MapViewOfFile`      | ✅ `mmap(2)`             | ✅ §2 Done                         |
| Memory protection (mprotect) | ✅ `VirtualProtect`     | ✅ `mprotect(2)`         | ⬜ §3 P0                            |
| ASLR                         | ✅ Opt-in per binary    | ✅ Default on            | ⬜ §4 P1 — **default on all procs** |
| Huge pages                   | ✅ `MEM_LARGE_PAGES`    | ✅ `MAP_HUGETLB`         | ⬜ §5 P2                            |
| COW fork()                   | ❌ No fork              | ✅ COW fork              | ⬜ §6 P1 — **beats Windows**        |
| Page replacement algorithm   | ✅ Modified clock       | ✅ LRU + clock           | ✅ Clock (second-chance)            |
| **ASLR default for all**     | ⚠️ Opt-in per PE binary | ✅ Default               | ⬜ **§4 — mandatory for all procs** |

> **After §3–6:** Impossible OS matches Linux's VM subsystem. ASLR mandatory-by-default exceeds Windows 11.
