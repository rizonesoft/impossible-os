# P0111 — Memory Guardrails & Audit

> **Goal:** Audit all existing `kmalloc` usage across the kernel, migrate violations
> to `pmm_alloc_contiguous()`, update documentation, add a build-time lint check,
> and provide a debug-mode allocation tracker.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Audit kmalloc Usage (GFX, Desktop, Drivers, Net)

**Prompt:** Run a full audit of `kmalloc` calls across all kernel subsystems — not just GFX/desktop but also `src/kernel/net/`, `src/kernel/fs/`, and `src/kernel/drivers/`. For each result, determine whether the allocation could exceed 4 KB under any circumstances (image buffers, font data, file read buffers, network packet buffers, driver DMA regions). Migrate any violations to `pmm_alloc_contiguous()`. For any legitimate small-struct `kmalloc` calls that remain, add a `/* kmalloc OK: <reason> */` comment so the build-time lint check (§3) can whitelist them. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: kmalloc audit — full kernel"`. Add notes directly in this TODO section with the audit results summary.
- [ ] Run: `grep -rn 'kmalloc' src/ --include='*.c' | grep -v 'kmalloc OK'`
- [ ] For each hit in `src/kernel/gfx/`: verify allocation size; migrate buffers > 4 KB to `pmm_alloc_contiguous()`
- [ ] For each hit in `src/desktop/`: same — watch for icon/wallpaper/pixel buffers
- [ ] For each hit in `src/kernel/net/`: packet receive buffers (typically 1500 B — OK, but check reassembly buffers)
- [ ] For each hit in `src/kernel/fs/`: directory read buffers, inode caches — may exceed 4 KB
- [ ] For each hit in `src/kernel/drivers/`: DMA buffers must use PMM (require contiguous physical pages)
- [ ] Add `/* kmalloc OK: <reason> */` comment to every remaining legitimate call
- [ ] Commit: `"mm: kmalloc audit — full kernel"`

---

## 2. Update Guardrails Documentation

**Prompt:** Update inline comments, `rules.md`, and the `/add-asset` workflow to reflect the current correct practices. The goal is to make the rules obvious to any agent or developer working in this codebase in the future. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"docs: update kmalloc/PMM guardrails"`. Add notes directly in this TODO section.
- [ ] Update `gfx_text.c` header comment: remove any `TODO: Migrate` lines (migration is done)
- [ ] Update `rules.md` Known Gotchas: add the font PMM migration as a resolved example
- [ ] Update `.agents/workflows/add-asset.md`: add font system as a "good example" of correct PMM usage
- [ ] Review `gfx_text.c` `load_ttf_file()` error path: add comment that PMM pages are intentionally not freed (boot-time permanent allocation)
- [ ] Verify `stb_truetype_impl.c` still redirects `malloc`/`free` → `kmalloc`/`kfree` (correct — only for small temp buffers during glyph rasterization)
- [ ] Commit: `"docs: update kmalloc/PMM guardrails"`

---

## 3. Build-Time kmalloc Lint Check

**Prompt:** Create a shell script `scripts/lint-alloc.sh` that greps for bare `kmalloc` calls across ALL kernel source, skipping lines with a `/* kmalloc OK: */` whitelist comment. Integrate it into `scripts/build.sh` so it runs on every build and fails loudly if an un-whitelisted `kmalloc` is found. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: kmalloc lint checker"`. Add notes directly in this TODO section covering the lint script design and false-positive handling.
- [ ] Create `scripts/lint-alloc.sh`:
  - [ ] `grep -rn 'kmalloc' src/ --include='*.c'`
  - [ ] Skip lines containing `/* kmalloc OK:` (whitelist marker)
  - [ ] Print filename:line for each violation with a clear error message
  - [ ] Exit non-zero (fail build) if any un-whitelisted hits found
- [ ] Add `bash scripts/lint-alloc.sh` call to `scripts/build.sh` (before compilation stage)
- [ ] Test: add a bare `kmalloc` to a GFX file → confirm build fails with clear message
- [ ] Test: add `/* kmalloc OK: test */` to same line → confirm build passes
- [ ] Commit: `"build: kmalloc lint checker"`

---

## 4. Kernel Malloc Leak Detector (Debug Build)

**Prompt:** Debug-only feature with zero overhead in release builds, enabled by `-DKMALLOC_DEBUG`. Wrap `kmalloc` and `kfree` with tracking macros that record caller address (via `__builtin_return_address(0)`), size, file, and line. Store records in a linked list allocated from a dedicated debug pool (separate from the kernel heap to avoid corrupting leak detection state). On shutdown or via a `memleak` shell command, dump all un-freed allocations with their caller context. Also expose `kmalloc_stats()` returning current usage, peak usage, and allocation count — useful for the Task Manager (Phase 05). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: kmalloc leak detector (debug build)"`. Add notes, gotchas, and design decisions directly in this TODO section covering kmalloc debug mode, allocation tracking, and leak reporting.
- [ ] Define `KMALLOC_DEBUG` compile-time flag (set in debug build only)
- [ ] Wrap `kmalloc`/`kfree` with macros that capture `__FILE__`, `__LINE__`, `__builtin_return_address(0)`
- [ ] Store allocation records in a linked list (use a small static pool, not kmalloc itself)
- [ ] Implement `kmalloc_dump_leaks()` — print all un-freed allocations on shutdown
- [ ] Implement shell command `memleak` → calls `kmalloc_dump_leaks()`
- [ ] Implement `kmalloc_stats(stats_t *out)` — current usage, peak usage, allocation count
- [ ] Expose `kmalloc_stats()` to Task Manager via `SYS_MEMSTATS` syscall
- [ ] Zero overhead in release: all tracking code inside `#ifdef KMALLOC_DEBUG`
- [ ] Commit: `"mm: kmalloc leak detector (debug build)"`

---

## 5. PMM Allocator Statistics & Shell Command

**Prompt:** The physical memory manager (`pmm_alloc_contiguous`) has no visibility — you can't see how much physical memory is free, how many allocations succeeded, or how much is wasted on fragmentation. Add a `pmm_stats()` function that returns: total physical pages, free pages, largest contiguous free block (for diagnosing fragmentation), and allocation count. Expose via a `meminfo` shell command and `SYS_PMMSTATS` syscall for the Task Manager. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: PMM statistics and meminfo command"`. Add notes directly in this TODO section.

> **Beats:** Linux `/proc/meminfo` is well known. Windows Task Manager shows similar info. Impossible OS shell `meminfo` matches both, with PMM-level breakdown that neither exposes directly.
- [ ] Implement `pmm_stats(pmm_stats_t *out)` — total/free pages, largest free block, alloc count
- [ ] Add `meminfo` shell command: print human-readable memory stats
  ```
  Physical Memory:  128 MiB total,  93 MiB free,  35 MiB used
  Largest block:    84 MiB contiguous free
  Kernel heap:       1.4 MiB used of 2 MiB (kmalloc)
  PMM allocations:  47 regions (framebuffer, font data, ...)
  ```
- [ ] Expose via `SYS_PMMSTATS` syscall for Task Manager memory tab
- [ ] Log PMM stats to serial at boot (`[OK] PMM: X MiB free of Y MiB`)
- [ ] Commit: `"mm: PMM statistics and meminfo command"`

---

## 6. Heap Overflow Detection (Stack Canaries)

**Prompt:** Heap and stack overflows are silent in the current kernel — a buffer overrun corrupts adjacent memory without detection. Add stack canaries to kernel functions: a random value placed below the return address at function entry, checked at exit (GCC `-fstack-protector-strong`). For the kernel heap, add a canary word at the end of each `kmalloc` allocation; `kfree` verifies it before freeing. Double-free detection: mark freed blocks with a magic pattern and check on free. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"mm: heap canaries and stack protector"`. Add notes directly in this TODO section.

> **Beats:** Neither Windows nor Linux enable heap canaries in their kernel by default. Impossible OS makes them the default in debug builds — safer development.
- [ ] Enable `-fstack-protector-strong` in kernel Makefile (check if `x86_64-elf-gcc` supports it)
- [ ] Initialize stack canary value from RDRAND at kernel boot (stored in `g_stack_canary`)
- [ ] Add heap tail canary to `kmalloc`: write `HEAP_CANARY` word after last byte of allocation
- [ ] `kfree`: verify tail canary before freeing; panic with `"heap corruption detected"` if wrong
- [ ] Double-free detection: write `FREE_MAGIC (0xDEADBEEFDEADBEEF)` pattern to first 8 bytes on free; `kfree` checks for this pattern and panics on `"double free detected"`
- [ ] Commit: `"mm: heap canaries and stack protector"`

---

## Priority Order

| Priority | Section                        | Reason                                                  |
|----------|--------------------------------|---------------------------------------------------------|
| 🔴 P0    | 1. Full kmalloc Audit          | Find and fix existing heap violations in all subsystems |
| 🟠 P1    | 3. Build-time Lint Check       | Prevent future violations — implement once, always runs |
| 🟠 P1    | 2. Update documentation        | Keep rules current for future agents                    |
| 🟡 P2    | 4. Leak Detector               | Useful debug tool; zero prod overhead                   |
| 🟡 P2    | 5. PMM Statistics / meminfo    | Visibility into physical memory usage                   |
| 🟡 P2    | 6. Heap Canaries               | Safety net for heap/stack corruption — debug default    |

---

## OS Comparison

| Feature                         | 🪟 Windows Kernel            | 🐧 Linux Kernel              | 🚀 Impossible OS                        |
| ------------------------------- | --------------------------- | --------------------------- | -------------------------------------- |
| Compile-time alloc lint         | ❌ (Driver Verifier runtime) | ✅ `sparse` / `smatch`       | ⬜ §3 P1 — **build fails on violation** |
| Heap leak detection             | ✅ Driver Verifier LEAK      | ✅ `kmemleak` (debug)        | ⬜ §4 P2 — `KMALLOC_DEBUG`              |
| PMM statistics / meminfo        | ✅ `!poolused` (WinDbg)      | ✅ `/proc/meminfo`           | ⬜ §5 P2 — `meminfo` shell cmd          |
| Stack protector                 | ✅ `/GS` (MSVC)              | ✅ `-fstack-protector`       | ⬜ §6 P2                                |
| Heap canaries                   | ✅ Debug heap (user only)    | ⚠️ SLUB debug (kernel only) | ⬜ §6 P2                                |
| **Build fails on bare kmalloc** | ❌                           | ❌                           | ⬜ **§3 — Impossible OS only**          |
| **Heap canary in kernel debug** | ❌ (user-mode only)          | ⚠️ SLUB allocator only      | ⬜ **§6 — kmalloc-level canaries**      |
