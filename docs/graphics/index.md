# Graphics

The graphics and user interface stack above the kernel's framebuffer: 2D drawing, images, icons and cursors, text and fonts, the theme and animation engines, the widget and dialog libraries, accessibility and input methods, and the window manager. The visual specification these pages implement is the [design system](../design/index.md); the desktop that runs today is described under [Desktop](../desktop/index.md).

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
