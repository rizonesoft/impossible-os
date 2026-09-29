# Desktop

The desktop foundation: the window manager, the compositor that paints the screen, keyboard and mouse routing, the basic control library, and the desktop itself (wallpaper, icons, taskbar and Start menu). Most of this roadmap domain is superseded by the richer plans in [Graphics](../graphics/index.md), so these pages describe what runs today and point at the owning plan for everything else. The authoritative visual specification is the [design system](../design/index.md).

## Roadmap Overviews

One page per desktop foundation roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [Window Management Basics](window-management.md) | Window table, dragging, caption buttons and Alt+F4 today; minimize, resize, snap and hotkeys owned elsewhere |
| [Desktop Compositor](compositor.md) | The full-repaint compositor loop, partial presents, frame timing; planned damage tracking and VSync |
| [Keyboard and Mouse Input](input-system.md) | Today's direct key routing and merged pointer sources; planned modifiers, key events, Tab focus and hotkeys |
| [Control Library](control-library.md) | Buttons, labels, text boxes, scroll bars and the Control Gallery; new controls owned by the widget roadmaps |
| [Desktop Shell Today](desktop-shell.md) | Wallpaper, taskbar, clock and the current Start menu; Windows 11 shell owned by the graphics roadmaps |
| [Desktop Icons](desktop-icons.md) | Three static icons and the icon store today; planned grid, special folders, shortcuts and file-type icons |
