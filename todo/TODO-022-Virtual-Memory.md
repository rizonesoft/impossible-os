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

> **Note:** `SYS_MMAP=37` — verify this does not conflict with `SYS_SHMEM_UNMAP` in `TODO-021-IPC.md`. Check `syscall.h`.

- [x] Implement `mmap(addr, length, prot, flags, fd, offset)` — map file into address space
- [x] Implement `munmap(addr, length)` — unmap region
- [x] Implement `msync(addr, length)` — flush dirty pages to disk
- [x] Support `MAP_PRIVATE` (copy-on-write) and `MAP_SHARED` (shared writes)
- [x] Page fault handler handles COW for MAP_PRIVATE writes (eager-load for file data)
- [x] Add `SYS_MMAP` (37) and `SYS_MUNMAP` (38) syscalls
- [x] Test: mmap hello.txt, read first char as pointer → 'H' ✅
- [x] Commit: `"mm: memory-mapped files"`

---

## 3. Memory Protection

**Prompt:** `mprotect(addr, length, prot)` changes page permissions on an existing mapping — essential for W^X (write XOR execute) security policy, JIT compilers, and stack guards. The kernel updates the PTE flags for the specified range without remapping. `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` correspond to PTE R/W and NX bits. A guard page (PROT_NONE, one page below the stack) turns stack overflows into a page fault instead of silent corruption. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: mprotect and guard pages"`.


- [ ] Implement `mprotect(addr, length, prot)` syscall — update PTE flags for range
- [ ] Define `PROT_NONE`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC` constants
- [ ] Map PROT flags to PTE R/W bit and NX (No-Execute) bit
- [ ] Implement per-thread stack guard page (PROT_NONE page below stack bottom)
- [ ] Page fault on guard page → panic with "stack overflow" rather than silent corruption
- [ ] Add `SYS_MPROTECT` syscall
- [ ] Test: set region PROT_NONE → verify fault on access; set PROT_READ → verify read works
- [ ] Commit: `"mm: mprotect and guard pages"`

---

## Priority Order

| Priority | Section                   | Reason                                         |
|----------|---------------------------|------------------------------------------------|
| ✅ Done   | 1. Swap / Page File       | Verified complete — disk-backed pagefile       |
| ✅ Done   | 2. Memory-Mapped Files    | Verified complete — mmap/munmap/msync          |
| 🟠 P1     | 3. Memory Protection      | `mprotect` + guard pages for W^X security      |
