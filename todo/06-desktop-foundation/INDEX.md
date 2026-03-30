# 06 Desktop Foundation

This domain completes the core desktop infrastructure that everything else builds on. The basic window manager, compositor, controls, taskbar, and start menu exist as prototypes — this domain hardens them into production-grade systems before theming, animations, and advanced features (Domain 08) are layered on.

## Belongs Here

- Window manager completion: minimize, maximize, restore, snap, resize-by-edge
- Compositor optimization: dirty-rect tracking, partial repaints
- Input system: keyboard routing, global hotkeys (Alt+Tab, Alt+F4, Win+D), focus model
- Control library hardening: existing controls + checkbox, radio, combobox, listbox, progress bar
- Context menu engine (right-click menus)
- Desktop shell completion: desktop icons click, right-click menu, drag-and-drop
- Taskbar completion: window list sync, minimize/restore via taskbar click
- Start menu: functional program launch, search, settings/power actions

## Does Not Belong Here

- Theme system, animation engine — those go in 08-graphics-ui
- Advanced widgets (TreeView, ListView, Toolbar) — those go in 08-graphics-ui
- File manager, notepad, terminal improvements — those go in 09-desktop-shell
- Win32 GDI/USER32 stubs — those go in 08-graphics-ui

## Active TODOs

- [TODO-01 — Window Manager Completion](TODO-01-wm-completion.md) — Minimize, maximize, restore, snap, resize-by-edge, Alt+Tab, global hotkeys
- [TODO-02 — Compositor Optimization](TODO-02-compositor-optimization.md) — Dirty-rect tracking, partial repaints, 60fps target
- [TODO-03 — Input System](TODO-03-input-system.md) — Keyboard routing, modifier keys, focus model, Tab navigation, system hotkeys
- [TODO-04 — Control Library Completion](TODO-04-control-library.md) — Checkbox, radio, combobox, listbox, progress bar, context menu
- [TODO-05 — Desktop Shell Completion](TODO-05-desktop-shell.md) — Right-click menu, taskbar sync, start menu actions, power/settings
- [TODO-06 — Desktop Icon System](TODO-06-desktop-icons.md) — Dynamic icons, special folders, shortcuts, grid layout, drag, rename, file type icons
