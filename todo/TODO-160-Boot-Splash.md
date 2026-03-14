# P0307 — Boot Splash Screen

> **Goal:** Graphical boot splash with centered logo and smooth progress bar.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 18. Boot Splash Screen *(from Phase 04 §10)*

**Prompt:** Graphical boot splash with logo and progress bar. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"kernel: graphical boot splash"`. Update `README.md` if it contains stale or incorrect references to boot splash. Add notes, gotchas, and design decisions directly in this TODO section covering the boot splash API, progress milestones, and F8 boot menu.


- [ ] Create `src/kernel/boot_splash.c`
- [ ] `boot_splash_init()`, `boot_splash_progress(pct)`, `boot_splash_status(msg)`, `boot_splash_finish()`
- [ ] Progress milestones: 10% PMM → 20% drivers → 40% FS → 60% network → 80% desktop → 100%
- [ ] Centered logo, gradient background, smooth progress bar
- [ ] *(Stretch)* F8 boot menu: Normal, Safe mode, Recovery, Last known good
- [ ] Commit: `"kernel: graphical boot splash"`

