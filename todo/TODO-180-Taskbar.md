# P0304 — Taskbar

> **Goal:** Taskbar window list, pinned apps, jump lists, taskbar button context menu,
> Aero Peek, progress badges, and multi-monitor support.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Taskbar Window List

**Prompt:** The taskbar shows a button for each open window. Clicking a window button focuses/raises it. Clicking the active window's button minimizes it (toggle). The active button gets an accent underline. Buttons display the window's icon + title. Long titles are truncated with ellipsis. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar window list"`. Add notes directly in this TODO section covering the window list data structure, active state tracking, and flash behavior.

- [ ] Define `struct taskbar_entry` (window ptr, title, icon, active, flashing)
- [ ] Create `src/desktop/taskbar_winlist.c`
- [ ] `taskbar_add_window(win)` / `taskbar_remove_window(win)` / `taskbar_set_active(win)`
- [ ] Draw window buttons between Start button and system tray
- [ ] Active button: accent underline highlight
- [ ] Click button → focus/raise; click active → minimize
- [ ] `taskbar_flash(win)` — blink button to attract attention
- [ ] Truncate long titles with ellipsis (…)
- [ ] Commit: `"desktop: taskbar window list"`

---

## 2. Taskbar Button Context Menu

**Prompt:** Right-clicking a taskbar button shows Close, Maximize/Restore, Minimize, and optionally "Move to Desktop ►" submenu for virtual desktops. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar button context menu"`. Add notes directly in this TODO section.

- [ ] Right-click button → context menu: Close, Maximize/Restore, Minimize
- [ ] *(Stretch)* "Move to Desktop ►" submenu (requires virtual desktops from TODO-170 §7)
- [ ] *(Stretch)* "Pin to taskbar" for frequently used apps
- [ ] Commit: `"desktop: taskbar button context menu"`

---

## 3. Window Peek (Aero Peek)

**Prompt:** Hovering a taskbar button for 500ms makes all other windows transparent (10% opacity), revealing the desktop behind them. The hovered window stays at 100% opacity. Mouse leaves → restore all to 100%. "Show Desktop" button at the far-right taskbar corner: hover = peek all, click = toggle minimize all. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: window peek (Aero Peek)"`. Add notes directly in this TODO section.

- [ ] Hover button 500ms → set all other windows opacity to 10%
- [ ] Mouse leaves → restore all to 100% opacity
- [ ] Far-right corner: hover = peek all, click = toggle minimize all
- [ ] Registry: `HKCU\Software\Impossible\Shell\EnablePeek` (default: true)
- [ ] Commit: `"desktop: window peek (Aero Peek)"`

---

## 4. Taskbar Progress Badges

**Prompt:** Apps can display a progress overlay on their taskbar button — a thin filled bar at the bottom of the button icon, shown in green (normal), yellow (paused), or red (error). Used by: file copy operations, downloads, disk format. The API: `taskbar_set_progress(win, pct, state)`. Apps call this via a syscall or through the WM message queue. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar progress badge"`. Add notes directly in this TODO section.

> **Beats:** Linux taskbars (GNOME Shell) support progress via `com.canonical.Unity.LaunchItem` D-Bus interface — complex. Windows has `ITaskbarList3`. Impossible OS: in-kernel, zero D-Bus, zero COM.

- [ ] `taskbar_set_progress(win, pct, state)` — set progress overlay on taskbar button
- [ ] States: `TASKBAR_PROGRESS_NORMAL` (green), `TASKBAR_PROGRESS_PAUSED` (yellow), `TASKBAR_PROGRESS_ERROR` (red), `TASKBAR_PROGRESS_NONE` (hidden)
- [ ] Draw thin progress bar at bottom of button icon
- [ ] Syscall: `SYS_TASKBAR_SET_PROGRESS` for user-mode apps
- [ ] Commit: `"desktop: taskbar progress badge"`

---

## 5. Pinned Apps

**Prompt:** Apps can be pinned to the taskbar for quick launch. Pinned apps show even when not running. Click pinned app icon → launch if not running, focus if running. Right-click → Unpin from taskbar. Pinned apps stored in Registry `HKCU\Software\Impossible\Shell\TaskbarPins` as comma-separated app paths. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar pinned apps"`. Add notes directly in this TODO section.

- [ ] Load pinned apps from Registry `HKCU\Software\Impossible\Shell\TaskbarPins`
- [ ] Render pinned app icons at left side of taskbar (before window list)
- [ ] Running + pinned: show both in same button (combined)
- [ ] Not running: show icon-only button, click → launch
- [ ] Right-click pinned button → "Unpin from taskbar"
- [ ] Right-click window button → "Pin to taskbar"
- [ ] Commit: `"desktop: taskbar pinned apps"`

---

## 6. Taskbar Auto-Hide *(Stretch)*

**Prompt:** When auto-hide is enabled, the taskbar slides off-screen when the mouse isn't near the bottom. It slides back in when the mouse moves to the bottom edge. Use the animation engine from TODO-140 for the slide. Registry: `HKCU\Software\Impossible\Shell\TaskbarAutoHide`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: taskbar auto-hide"`.

- [ ] *(Stretch)* Registry: `HKCU\Software\Impossible\Shell\TaskbarAutoHide` (default: false)
- [ ] *(Stretch)* Mouse at bottom edge → slide taskbar in (150ms ease-out)
- [ ] *(Stretch)* Mouse leaves → 2 second delay → slide out (150ms ease-in)
- [ ] *(Stretch)* Commit: `"desktop: taskbar auto-hide"`

---

## Priority Order

| Priority | Section                      | Reason                                             |
|----------|------------------------------|----------------------------------------------------|
| 🔴 P0    | §1 Taskbar Window List       | Core taskbar — every open window needs a button    |
| 🟠 P1    | §2 Button Context Menu       | Right-click on taskbar buttons                     |
| 🟠 P1    | §5 Pinned Apps               | Quick-launch from taskbar                          |
| 🟡 P2    | §3 Window Peek (Aero Peek)   | Polish — hover to peek behind windows              |
| 🟡 P2    | §4 Progress Badges           | Visual feedback for long operations                |
| 🔵 P4    | §6 Auto-Hide                 | Power user feature                                 |

---

## Key Files

| File                                | Purpose                                  |
|-------------------------------------|------------------------------------------|
| `src/desktop/taskbar_winlist.c`     | [NEW] Window button list                 |
| `include/desktop/taskbar.h`         | [NEW] Taskbar API header                 |
| `src/desktop/taskbar.c`             | [NEW] Main taskbar renderer              |

---

## OS Comparison

| Feature                         | Windows 11 (taskbar)                  | Linux (GNOME Shell / KWin panel)       | Impossible OS                           |
|---------------------------------|---------------------------------------|----------------------------------------|-----------------------------------------|
| Window list (buttons per app)   | ✅ Taskbar buttons                    | ✅ GNOME Top Bar / KDE panel            | ⬜ §1 P0                               |
| Button context menu             | ✅ Right-click → window options       | ✅ Right-click menu                     | ⬜ §2 P1                               |
| Aero Peek (hover to peek)       | ✅ ITaskbarList3 thumbnail preview    | ❌ GNOME no Peek / KWin with extension  | ⬜ §3 P2                               |
| Progress overlay on button      | ✅ ITaskbarList3::SetProgressValue    | ✅ Unity Launcher progress (D-Bus)      | ⬜ §4 P2                               |
| Pinned apps                     | ✅ TaskBar pins in Registry           | ✅ GNOME / KDE favorites                | ⬜ §5 P1                               |
| Auto-hide                       | ✅ Taskbar auto-hide setting          | ✅ gnome-panel autohide / KDE           | ⬜ §6 P4 (stretch)                     |
| Jump lists (right-click)        | ✅ `ICustomDestinationList`           | ❌ GNOME no jump lists                  | 🔵 Future                              |
| Multi-monitor taskbar           | ✅ Show on all monitors               | ✅ KDE multi-monitor panel              | 🔵 Future                              |
| **In-kernel (no D-Bus)**        | ❌ COM-based ITaskbarList3            | ❌ D-Bus protocol for progress          | ✅ **Syscall-based — zero IPC overhead** |
| **Progress badge simplicity**   | ❌ Complex COM interface              | ❌ D-Bus required                       | ⬜ **§4 — simple `taskbar_set_progress()` syscall** |
