<!-- docs: covers=todo/08-graphics-ui/TODO-03-theme-system.md order=1 -->
# Design System

The Impossible OS desktop follows Windows 11 as closely as a clean-room implementation can: the same layout, spacing, type ramp, corner radii and control shapes, so that anyone who uses Windows 11 is at home immediately. It departs in one deliberate place. The glass is a little frostier: shell surfaces blur the wallpaper a little wider and tint it a little less, with a faint grain, so colour reads through as soft light rather than shapes.

**See it live:** the [interactive desktop mockup](https://impossibleos.co/design/) renders the taskbar, Start menu, quick settings, calendar, context menu and a File Explorer window from the same tokens the C code uses. Switch between light and dark in the top-right corner.

## Where the design lives

| File | Role |
| --- | --- |
| [`tokens.json`](tokens.json) | The canonical tokens: colours for dark and light, acrylic and mica materials, sizes, radii, spacing, type ramp, elevation and motion. |
| [`include/desktop/theme_tokens.h`](../../include/desktop/theme_tokens.h) | Generated from `tokens.json` by `scripts/site/gen_theme_header.py`. Never edited by hand. |
| [`shell.md`](shell.md) | The shell specification: desktop, taskbar, Start, flyouts, menus and window chrome, with every measurement. |
| [`icons.md`](icons.md) | The icon system: grid, light, palette, small-size rules and the full icon list. |
| [`gh-pages/design/index.html`](../../gh-pages/design/index.html) | The interactive mockup, published at [impossibleos.co/design](https://impossibleos.co/design/). Its CSS variables are generated from `tokens.json` at build time. |
| [`resources/backgrounds/src/`](../../resources/backgrounds/src/bloom-dark.svg) | The default "impossible bloom" wallpapers, dark and light, as SVG sources. |
| [`resources/brand/`](../../resources/brand/logo.svg) | The logo mark, the README wordmarks and the desktop preview. |

## How is drift prevented?

Every published surface is derived from one source, and a check fails the commit when a derived copy disagrees:

- `scripts/site/build.py --check` (lint Check 30) regenerates `theme_tokens.h` in memory and fails if the committed header differs from `tokens.json`.
- The mockup never hard-codes a colour, size or duration: its `:root` block is generated from `tokens.json` on every Pages build, so the web reference and the C header cannot disagree.
- `scripts/site/render-brand.sh` re-renders the wordmarks and the README desktop screenshot from their sources after a design change.

## How do I change the design?

1. Edit [`tokens.json`](tokens.json). Colours are `#RRGGBB` or `#AARRGGBB`, alpha first, matching the framebuffer's `0xAARRGGBB` layout.
2. Run `python3 scripts/site/gen_theme_header.py` to regenerate the C header.
3. Run `python3 scripts/site/build.py` and open `build/site/design/index.html` to see the change.
4. If the change is visible in the README hero, run `bash scripts/site/render-brand.sh`.
5. Commit the tokens, the header and any re-rendered images together.

## What principles guide it?

- **Windows 11 first.** When in doubt, match what Windows 11 does. Users should not have to learn a new desktop.
- **Frosted, not foggy.** Acrylic blurs the wallpaper wider than Windows 11 and tints it less, but text on glass must meet WCAG AA against the worst-case wallpaper region. When transparency is turned off (`EnableTransparency=0`), every material falls back to its tint colour at full opacity.
- **One key light.** Icons, shadows and highlights all assume a single light from above.
- **Software rendering is the budget.** The compositor runs on the CPU at 60 Hz (16.67 ms per frame). Blurred surfaces are cached and recomputed only when what is behind them changes, never per frame.
- **Tokens, not literals.** Shell code reads `THEME_*` constants (and, once the theme system lands, `theme_get()`), never a hex literal.

## How does this relate to the theme system?

The [theme system roadmap](../../todo/08-graphics-ui/TODO-03-theme-system.md) owns the runtime side: the `theme_t` structure, `theme_get()`, Registry-driven light and dark switching, and hot reload. `tokens.json` is its design input. Its planned Fluent token corpus generator will write the `color.themes` block of `tokens.json` from Microsoft's WinUI resource dictionaries, so the colours match Windows 11 byte for byte, while the materials, shell sizes, elevation and motion stay hand-authored here because they are Impossible OS design decisions.
