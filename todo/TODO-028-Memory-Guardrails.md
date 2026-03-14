# P0111 — Memory Guardrails & Audit

> **Goal:** Audit all existing `kmalloc` usage across the kernel, migrate violations
> to `pmm_alloc_contiguous()`, update documentation, add a build-time lint check,
> and provide a debug-mode allocation tracker.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas.

---

## 1. Audit kmalloc Usage (GFX, Desktop, Drivers, Net)

**Prompt:** Run a full audit of `kmalloc` calls across all kernel subsystems — not just GFX/desktop but also `src/kernel/net/`, `src/kernel/fs/`, and `src/kernel/drivers/`. For each result, determine whether the allocation could exceed 4 KB under any circumstances (image buffers, font data, file read buffers, network packet buffers, driver DMA regions). Migrate any violations to `pmm_alloc_contiguous()`. For any legitimate small-struct `kmalloc` calls that remain, add a `/* kmalloc OK: <reason> */` comment so the build-time lint check (§3) can whitelist them. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: kmalloc audit — full kernel"`.


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

**Prompt:** Update inline comments, `rules.md`, and the `/add-asset` workflow to reflect the current correct practices. The goal is to make the rules obvious to any agent or developer working in this codebase in the future. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"docs: update kmalloc/PMM guardrails"`.


- [ ] Update `gfx_text.c` header comment: remove any `TODO: Migrate` lines (migration is done)
- [ ] Update `rules.md` Known Gotchas: add the font PMM migration as a resolved example
- [ ] Update `.agents/workflows/add-asset.md`: add font system as a "good example" of correct PMM usage
- [ ] Review `gfx_text.c` `load_ttf_file()` error path: add comment that PMM pages are intentionally not freed (boot-time permanent allocation)
- [ ] Verify `stb_truetype_impl.c` still redirects `malloc`/`free` → `kmalloc`/`kfree` (correct — only for small temp buffers during glyph rasterization)
- [ ] Commit: `"docs: update kmalloc/PMM guardrails"`

---

## 3. Build-Time kmalloc Lint Check

**Prompt:** Create a shell script `scripts/lint-alloc.sh` that greps for bare `kmalloc` calls across ALL kernel source, skipping lines with a `/* kmalloc OK: */` whitelist comment. Integrate it into `scripts/build.sh` so it runs on every build and fails loudly if an un-whitelisted `kmalloc` is found. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"build: kmalloc lint checker"`.


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

## Priority Order

| Priority | Section                      | Reason                                              |
|----------|------------------------------|-----------------------------------------------------|
| 🔴 P0     | 1. Full kmalloc Audit        | Find and fix existing heap violations in all subsys |
| 🟠 P1     | 3. Build-time Lint Check     | Prevent future violations — implement once, free    |
| 🟠 P1     | 2. Update documentation      | Keep rules current for future agents                |
| 🟡 P2     | 4. Leak Detector             | Useful debug tool; zero prod cost                   |
