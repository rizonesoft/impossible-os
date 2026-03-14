# P0501 — Core Applications (Hub)

> **Goal:** Deliver a suite of essential GUI applications that make Impossible OS
> a usable daily environment: file manager, terminal emulator, settings panel,
> text editor, calculator, paint program, and a collection of utility apps —
> all built on a shared UI widget library.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. UI Widget Library (Shared)

> **Moved to [TODO-P0202-GUI.md §2](TODO-P0202-GUI.md)** — Extended Widget Toolkit
> (checkbox, radio, dropdown, slider, progress bar, tabs, list view, tree view,
> toolbar, menu bar, status bar, groupbox, tooltips, dialog system).
> Existing basic controls (Button, Label, TextBox, ScrollBar) are in `controls.c`.

---

## 2. File Manager

> **See [TODO-P0502-File-Manager.md](TODO-P0502-File-Manager.md)** — Sidebar, view modes,
> file operations, advanced features.

---

## 3. Terminal Emulator

> **See [TODO-P0503-Terminal.md](TODO-P0503-Terminal.md)** — Core grid, ANSI escape codes,
> scrollback, selection, copy/paste, configurable appearance.

---

## 4. Settings Panel

> **See [TODO-P0504-Settings-Panel.md](TODO-P0504-Settings-Panel.md)** — SPL applet framework,
> host app, 14 built-in applets (about, display, theme, wallpaper, etc.).

---

## 5. Notepad

> **See [TODO-P0505-Notepad.md](TODO-P0505-Notepad.md)** — Gap buffer, text rendering,
> file operations, editing features.

---

## 6. Calculator

> **See [TODO-P0506-Calculator.md](TODO-P0506-Calculator.md)** — Arithmetic engine,
> memory functions, scientific/programmer stretch modes.

---

## 7. Paint

> **See [TODO-P0507-Paint.md](TODO-P0507-Paint.md)** — Canvas, drawing tools,
> color system, undo/redo, file operations.

---

## 8. Task Manager & Device Manager

> **See [TODO-P0508-Task-Manager.md](TODO-P0508-Task-Manager.md)** — Process list,
> performance monitor, device tree.

---

## 9. Utility Apps & Shell Commands

> **See [TODO-P0509-Utility-Apps.md](TODO-P0509-Utility-Apps.md)** — Shell commands,
> Image Viewer, Screenshot, Archive Manager, Calendar, System Info,
> On-Screen Keyboard, Font Manager, Color Picker, Sticky Notes, app patterns.

---

## Priority Order

| Priority | Section | File |
|----------|---------|------|
| 🔴 P0 | UI Widget Library | `P0202-GUI.md` |
| 🔴 P0 | Terminal Emulator | `P0503-Terminal.md` |
| 🔴 P0 | File Manager | `P0502-File-Manager.md` |
| 🟠 P1 | Notepad | `P0505-Notepad.md` |
| 🟠 P1 | Calculator | `P0506-Calculator.md` |
| 🟠 P1 | Shell Commands | `P0509-Utility-Apps.md` |
| 🟡 P2 | Task Manager | `P0508-Task-Manager.md` |
| 🟡 P2 | Settings Panel | `P0504-Settings-Panel.md` |
| 🟡 P2 | Image Viewer | `P0509-Utility-Apps.md` |
| 🟡 P2 | Screenshot Tool | `P0509-Utility-Apps.md` |
| 🟢 P3 | Paint | `P0507-Paint.md` |
| 🟢 P3 | Device Manager | `P0508-Task-Manager.md` |
| 🔵 P4 | Calendar, OSK, etc. | `P0509-Utility-Apps.md` |
