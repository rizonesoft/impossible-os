<!-- docs: covers=todo/10-platform-services/TODO-06-accessibility.md sources=include/cursor.h,include/kernel/drivers/keyboard.h,include/kernel/drivers/mouse.h,src/kernel/drivers/mouse.c,include/kernel/drivers/framebuffer.h,include/desktop/theme_tokens.h,include/kernel/test/wcag.h,docs/design/shell.md reviewed=2026-09-29 order=6 -->
# Accessibility Features

## What is it?

This roadmap delivers the accessibility settings a user turns on: high contrast, larger text through a DPI override, sticky keys, a larger cursor, a screen magnifier, mouse keys, reduced motion with a colour-blind filter, and one Ease of Access applet (`ease.cpl`) that holds them all. None of its eight sections has shipped. The lower layers it sits on (themes, DPI, animation, and the accessibility tree that screen readers use) are owned by the graphics roadmaps, and so is the magnifier's visual design.

## How does it work?

**Today.** No accessibility setting can be turned on. The pieces the features will reuse:

- **Cursor.** [`cursor.h`](../../include/cursor.h) loads cursor images from `C:\Impossible\System\Cursors\` into a `cursor_image_t` of up to 8 sizes, each at most 48 pixels (`CURSOR_MAX_SIZE`), and draws them with `cursor_draw()`. The planned 64 and 96 pixel cursors need that limit raised.
- **Keyboard.** The keyboard driver already latches Shift, Ctrl and Alt, and `keyboard_inject_hid_key()` feeds a key from USB HID into the same path ([`keyboard.h`](../../include/kernel/drivers/keyboard.h)). Sticky keys will extend these latches.
- **Pointer.** `mouse_inject_state(x, y, buttons)` sets the cursor position and buttons directly; today only the desktop tests and their input record and replay call it ([`mouse.h`](../../include/kernel/drivers/mouse.h)). Mouse keys will rename it `mouse_event_inject()` and drive it from the numeric keypad, and the [desktop test framework](../infrastructure/desktop-ui-test-framework.md) will keep using the same primitive.
- **Magnifier design.** The spec is fixed in [Shell design: Magnifier](../design/shell.md#magnifier): a 400 by 300 lens that follows the pointer (`THEME_SIZE_MAGNIFIER_LENS_WIDTH` and `..._HEIGHT` in [`theme_tokens.h`](../../include/desktop/theme_tokens.h)), with full-screen and docked views.
- **Contrast checks.** A WCAG contrast helper used by the tests ([`wcag.h`](../../include/kernel/test/wcag.h)), against which a high contrast theme can be checked.

**Planned design.** A global accessibility state is restored from `HKLM\SYSTEM\Accessibility\*` at boot, and each feature applies instantly:

1. **High contrast.** Switch to the Fluent high contrast theme and broadcast a theme change, through the [Theme System](../graphics/theme-system.md).
2. **Large text.** A 125 to 300 percent scale override passed to the single DPI owner in [Desktop Shell Features](../graphics/desktop-shell-features.md).
3. **Sticky keys.** Five presses of Shift toggle it; a pressed modifier latches until the next key, with a tray indicator.
4. **Large cursor.** `cursor_set_size()` at 32, 48, 64 or 96 pixels, and a white, black or inverting colour.
5. **Magnifier.** Win+Plus and Win+Minus zoom 2x to 8x over the compositor's back buffer, following the pointer.
6. **Mouse keys.** The numeric keypad moves the pointer, toggled with Alt+Shift+Num Lock, with speed and acceleration.
7. **Reduced motion and colour-blind filter.** Animations turned off through the [Animation Engine](../graphics/animation-engine.md), and a 3 by 3 fixed-point colour matrix applied as the compositor presents each frame.
8. **`ease.cpl`.** One page with every toggle and slider, applied without an Apply button.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `cursor_draw()`, `cursor_image_t`, `CURSOR_MAX_SIZE` (48) | Shipped |
| Keyboard modifier latches, `keyboard_inject_hid_key()` | Shipped |
| `mouse_inject_state()` | Shipped; to be renamed `mouse_event_inject()` |
| `accessibility_set_high_contrast()`, `accessibility_set_dpi()`, `cursor_set_size()`, `cursor_set_color()` | Planned |
| Magnifier, mouse keys, `fb_set_color_matrix()`, `ease.cpl` | Planned |
| New syscalls | None planned |

## How do I use it?

Nothing can be switched on yet. Contributors can preview the magnifier and the high contrast visuals in the [design spec](../design/shell.md#magnifier) and the live mockup on the project site.

## What is not implemented yet?

- [High Contrast Mode](../../todo/10-platform-services/TODO-06-accessibility.md#1-high-contrast-mode-sonnet) and [Large Text and DPI Override](../../todo/10-platform-services/TODO-06-accessibility.md#2-large-text--dpi-override-sonnet)
- [Sticky Keys](../../todo/10-platform-services/TODO-06-accessibility.md#3-sticky-keys-sonnet) and [Mouse Keys](../../todo/10-platform-services/TODO-06-accessibility.md#6-mouse-keys-sonnet)
- [Large Cursor](../../todo/10-platform-services/TODO-06-accessibility.md#4-large-cursor-sonnet), which also needs `CURSOR_MAX_SIZE` raised above 48
- [Screen Magnifier](../../todo/10-platform-services/TODO-06-accessibility.md#5-screen-magnifier-sonnet)
- [Reduced Motion and Color Blind Mode](../../todo/10-platform-services/TODO-06-accessibility.md#7-reduced-motion--color-blind-mode-sonnet)
- [`ease.cpl`](../../todo/10-platform-services/TODO-06-accessibility.md#8-easecpl----accessibility-settings-applet-sonnet)

Screen reader support, the accessibility tree and UI Automation are not part of this roadmap; they belong to [UI Accessibility, Automation and IME](../graphics/accessibility-ime.md).

## How does it compare with Windows 11 and Linux?

Windows 11 has every feature on this list in Settings > Accessibility, including colour filters, plus Narrator. GNOME and KDE offer high contrast, large text, sticky and mouse keys, zoom and reduced animation, with Orca as the screen reader. The Impossible OS plan applies magnification and colour correction inside its own compositor, the correction as a fixed-point matrix on the final frame, and puts every setting on one page. Nothing is usable yet.

## See also

- [Accessibility Features roadmap](../../todo/10-platform-services/TODO-06-accessibility.md)
- [UI Accessibility, Automation and IME](../graphics/accessibility-ime.md)
- [Theme System](../graphics/theme-system.md)
- [Keyboard and Mouse Input](../desktop/input-system.md)
- [Shell design: Magnifier](../design/shell.md#magnifier)
