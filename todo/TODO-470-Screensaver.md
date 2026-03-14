# P0306 — Screensaver & Lock Screen

> **Goal:** Idle detection, screensaver framework, and password-protected lock screen.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 19. Screensaver & Lock Screen *(from Phase 04 §11)*

### 19.1 Screensaver System *(from Phase 04 §11.1–11.2)*

**Prompt:** Idle detection + screensaver API + 5 built-in screensavers. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: screensaver system"`. Update `README.md` if it contains stale or incorrect references to screensavers. Add notes, gotchas, and design decisions directly in this TODO section covering the screensaver API, idle detection, and built-in screensavers.


- [ ] Create `src/desktop/screensaver.c`
- [ ] Screensaver API: `scr_entry_fn(msg, surface)` — SCR_INIT/FRAME/CLOSE
- [ ] Idle detection, configurable timeout
- [ ] Dismiss on any input
- [ ] Built-in: Blank, Starfield, Matrix, Bouncing Logo, Clock
- [ ] Registry: `HKCU\Software\Impossible\Screensaver\IdleTimeout`, `HKCU\Software\Impossible\Screensaver\Type`
- [ ] Commit: `"desktop: screensaver system"`

### 19.2 Lock Screen *(from Phase 04 §11.3)*

**Prompt:** Full-screen lock with blurred wallpaper, clock, password input. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: lock screen"`. Update `README.md` if it contains stale or incorrect references to lock screen. Add notes, gotchas, and design decisions directly in this TODO section covering the lock screen, password input, and auto-lock behavior.


- [ ] Create `src/desktop/lockscreen.c`
- [ ] Blurred wallpaper background
- [ ] Large clock + date, user avatar + name
- [ ] Password input field + [Unlock →] button
- [ ] Win+L shortcut, auto-lock after screensaver
- [ ] Registry: `HKCU\Software\Impossible\Screensaver\RequirePassword`
- [ ] Commit: `"desktop: lock screen"`

