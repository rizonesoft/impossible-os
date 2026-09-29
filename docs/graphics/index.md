# Graphics

The graphics and user interface stack above the kernel's framebuffer: 2D drawing, images, icons and cursors, text and fonts, the theme and animation engines, the widget and dialog libraries, accessibility and input methods, the window manager, the shell surfaces (taskbar, Start menu, notifications and clock), the boot splash, and the Win32 GDI, USER32 and Win32k system call layers. The visual specification these pages implement is the [design system](../design/index.md); the desktop that runs today is described under [Desktop](../desktop/index.md).

## Roadmap Overviews

One page per graphics roadmap file, each following the [page contract](../contributing/docs-page-contract.md).

| Document | Topics |
| --- | --- |
| [2D Graphics and Visual Assets](graphics-assets.md) | Surfaces, effects, images, icon and cursor stores today; planned render targets, vector paths, SVG icons and the icon engine |
| [Text and Fonts](text-fonts.md) | The TrueType slot manager and glyph caches today; planned catalog, fallback, shaping with HarfBuzz and FreeType |
| [Theme System](theme-system.md) | Generated design tokens and seeded Registry settings today; planned `theme_t`, presets, accents and live reload |
| [Animation Engine](animation-engine.md) | Motion tokens today; planned fixed-point tweens, easing, a 64-slot manager, window transitions and springs |
| [Core Widgets](widget-library.md) | Planned check box, radio, slider, progress, drop-down, tabs, theming and overlay scroll bar |
| [Complex Controls and Dialogs](complex-controls-dialogs.md) | Planned list and tree views, bars, tooltips, info bar, `MessageBox` and file dialogs |
| [UI Accessibility, Automation and IME](accessibility-ime.md) | The pending WCAG sweep today; planned semantic tree, provider events, screen reader bridge and IME |
| [Window Manager Enhancements](window-manager.md) | Chrome and Alt+F4 today; planned minimize and maximize, snap layouts, hotkeys, Alt+Tab and drag and drop |
| [Desktop Shell Features](desktop-shell-features.md) | The startup wallpaper today; planned context menus, DPI scaling, screenshots, night light, Focus, Quick Settings and virtual desktops |
| [Taskbar](taskbar.md) | The basic bar, text window buttons and clock today; planned centred icons, button menus, Aero Peek, progress, pins, jump lists and auto-hide |
| [Start Menu, Tray and Notifications](start-menu-tray-notifications.md) | The static Start menu today; planned data loading, search, the system tray, toasts and the notification centre |
| [Kernel Time and Taskbar Clock](clock-time.md) | The FILETIME clock and basic taskbar clock today; planned formatting, named time zones, the calendar flyout and the Date and time page |
| [Boot Splash and F8 Recovery](boot-splash-recovery.md) | The splash, spinner and bootloader F8 today; planned progress bar, milestones and the recovery menu |
| [Win32 GDI and USER32 Desktop API](win32-gdi-user32.md) | Planned device contexts, drawing, text, windows, the message loop and dialogs over the native graphics stack |
| [Win32k Shadow SSDT](win32k-shadow-ssdt.md) | The empty, dispatched shadow table today; planned `NtGdi` and `NtUser` services in 27 waves |
| [Win32k Shadow Native API](win32k-shadow-router.md) | The table-selector routing and range checks today; planned headers, static checks, index generation and the callback review |
| [Win32k Shadow SSDT Master Table](win32k-shadow-master-table.md) | The 1,300-row service ledger, its capacity gap and its planned audit |
