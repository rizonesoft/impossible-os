# P0304 — Taskbar

> **Goal:** Taskbar window list, button context menu, and Aero Peek.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 6. Taskbar *(from Phase 04 §1)*

### 6.1 Taskbar Window List *(from Phase 04 §1.1)*

**Prompt:** The taskbar shows a button for each open window. Clicking a window button focuses/raises it. Clicking the active window's button minimizes it (toggle). The active button gets an accent underline. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar window list"`. Update `README.md` if it contains stale or incorrect references to the taskbar. Add notes, gotchas, and design decisions directly in this TODO section covering the taskbar window list, active button rendering, and flash behavior.


- [ ] Define `struct taskbar_entry` (window ptr, title, icon, active, flashing)
- [ ] Create `src/desktop/taskbar_winlist.c`
- [ ] `taskbar_add_window(win)` / `taskbar_remove_window(win)` / `taskbar_set_active(win)`
- [ ] Draw window buttons between start button and system tray
- [ ] Active button: accent underline highlight
- [ ] Click button → focus/raise; click active → minimize
- [ ] `taskbar_flash(win)` — blink button to attract attention
- [ ] Commit: `"desktop: taskbar window list"`

### 6.2 Taskbar Button Context Menu *(from Phase 04 §1.2)*

**Prompt:** Right-clicking a taskbar button shows Close, Maximize/Restore, Minimize. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar button context menu"`. Update `README.md` if it contains stale or incorrect references to taskbar menus. Add notes, gotchas, and design decisions directly in this TODO section covering the taskbar button context menu items.


- [ ] Right-click button → Close, Maximize/Restore, Minimize
- [ ] *(Stretch)* "Move to Desktop ►" submenu
- [ ] Commit: `"desktop: taskbar button context menu"`

### 6.3 Window Peek (Aero Peek) *(from Phase 04 §1.3)*

**Prompt:** Hovering a taskbar button for 500ms makes all other windows 10% opacity. "Show Desktop" button at far-right corner. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: window peek (Aero Peek)"`. Update `README.md` if it contains stale or incorrect references to Aero Peek. Add notes, gotchas, and design decisions directly in this TODO section covering the peek functionality, hover timing, and show desktop toggle.


- [ ] Hover button 500ms → all other windows 10% opacity
- [ ] Mouse leaves → restore all to 100%
- [ ] Far-right corner: hover = peek all, click = toggle minimize all
- [ ] Registry: `HKCU\Software\Impossible\Shell\EnablePeek`
- [ ] Commit: `"desktop: window peek (Aero Peek)"`

