# P2401 — Keyboard Layouts & Internationalization

> **Goal:** Provide international keyboard layouts, Unicode/UTF-8 support, and
> a localization framework for multi-language UI.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. Keyboard Layout System

**Prompt:** Replace hardcoded US QWERTY scancode table in `keyboard.c` with a layout system. Define `struct kbd_layout` with name, code ("en-US"), normal[128], shift[128], altgr[128] arrays. `kbd_set_layout(code)` switches active layout. Store in Registry `HKLM\SYSTEM\Input\KeyboardLayout`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"kernel: keyboard layout system"`. Add notes, gotchas, and design decisions directly in this TODO section covering the layout struct, scancode-to-char mapping, and Registry integration.

- [ ] Create `src/kernel/kbd_layout.c` and `include/kbd_layout.h`
- [ ] Define `struct kbd_layout` (name, code, normal[128], shift[128], altgr[128])
- [ ] Replace hardcoded US QWERTY scancode→ASCII table in `keyboard.c` with layout lookup
- [ ] `kbd_set_layout(code)` — switch active layout
- [ ] `kbd_get_layout()` — return current layout code
- [ ] Registry: `HKLM\SYSTEM\Input\KeyboardLayout = "en-US"`
- [ ] Commit: `"kernel: keyboard layout system"`

---

## 2. Built-in Layouts

**Prompt:** Define layout tables: US QWERTY (default), UK English (£ vs $), German QWERTZ (Z/Y swap, umlauts on AltGr), French AZERTY (A/Q, Z/W swap), Spanish (ñ, accents), Dvorak (alt layout). Store as static arrays in `resources/layouts/`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in keyboard layouts"`. Add notes directly in this TODO section.

- [ ] Create `resources/layouts/` directory with layout data
- [ ] **US English (QWERTY)** — `en-US` (default)
- [ ] **UK English** — `en-GB` (different symbols: £ vs $, @ position)
- [ ] **German (QWERTZ)** — `de-DE` (Z/Y swapped, umlauts on AltGr)
- [ ] **French (AZERTY)** — `fr-FR` (A/Q, Z/W swapped, accents)
- [ ] **Spanish** — `es-ES` (ñ, accents)
- [ ] **Dvorak** — `en-DV` (alternative layout)
- [ ] Commit: `"kernel: built-in keyboard layouts (6 layouts)"`

---

## 3. Layout Switching

**Prompt:** Win+Space cycles installed layouts. System tray shows 2-letter indicator ("EN", "FR", "DE"). Click indicator → layout picker popup. Settings applet for layout management. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard layout switching"`. Add notes directly in this TODO section.

- [ ] Win+Space → cycle through installed layouts
- [ ] System tray indicator: show current layout code (`EN`, `FR`, `DE`)
- [ ] Click tray indicator → layout picker popup
- [ ] Input → keyboard settings applet for layout management
- [ ] Commit: `"desktop: keyboard layout switching (Win+Space)"`

---

## 4. Unicode / UTF-8 Support

**Prompt:** Store all text as UTF-8. Implement `utf8_encode(codepoint, buf)` and `utf8_decode(buf, codepoint_out)`. Keyboard outputs UTF-8 codepoints. stb_truetype already supports Unicode codepoints. Stretch: Noto Sans fallback font for CJK. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: UTF-8 Unicode support"`. Add notes directly in this TODO section.

- [ ] Store all text strings internally as UTF-8
- [ ] UTF-8 encode/decode helpers: `utf8_encode(codepoint, buf)`, `utf8_decode(buf, codepoint_out)`
- [ ] Keyboard input: convert layout output to UTF-8 codepoints
- [ ] Font rendering: `stb_truetype` already supports Unicode codepoints
- [ ] *(Stretch)* Noto Sans as fallback font (covers all Unicode scripts)
- [ ] Commit: `"kernel: UTF-8 Unicode support"`

---

## 5. Dead Keys & Compose Sequences

**Prompt:** Many European layouts use dead keys — e.g., pressing `^` then `e` produces `ê`. Dead keys work by buffering the accent character and combining it with the next keypress. Define a dead-key table in `struct kbd_layout`: `deadkeys[32]` entries, each a `(dead_char, base_char, composed_codepoint)` triple. When a key mapped to a dead accent is pressed, set a `pending_dead` flag in the keyboard state; on the next keypress, look up the combination. If no match found, emit the dead char followed by the new char. This is P1 because French (AZERTY) and German (QWERTZ) both use dead keys. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: dead key compose sequences"`. Add notes directly in this TODO section.

> **Beats:** Linux implements dead keys in xkb (user-space). Windows handles in Win32 keyboard stack. Impossible OS handles in the kernel layout table — simpler, no xkb complexity.

- [ ] Add `deadkeys[]` table to `struct kbd_layout`:
  - [ ] `(dead_char, base_char, composed_codepoint)` triples
  - [ ] Max 32 entries per layout
- [ ] Keyboard state: add `pending_dead` codepoint field
- [ ] On keypress: if `pending_dead`, look up combination → emit composed codepoint or `dead + new`
- [ ] Populate dead-key tables for: `de-DE` (umlauts: `¨ + a = ä`, `¨ + o = ö`, `¨ + u = ü`), `fr-FR` (circumflex, grave, tilde)
- [ ] Test: type `^ + e` on `fr-FR` layout → `ê` appears in terminal
- [ ] Commit: `"kernel: dead key compose sequences"`

---

## 6. Sticky Keys & Accessibility

**Prompt:** Sticky Keys makes modifier key usage one-handed: pressing Shift, Ctrl, or Alt latches it active for the next keypress, then releases automatically. Five taps of Shift in rapid succession enables Sticky Keys (matches Windows behavior). Once enabled, a visual indicator appears in the system tray (e.g., a lock icon with the modifier name). This is an accessibility feature that matches Windows Sticky Keys precisely. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: sticky keys accessibility"`. Add notes directly in this TODO section.

> **Beats:** Linux requires X11/libXt for sticky keys (not in kernel). Windows has sticky keys in the kernel-level keyboard filter. Impossible OS: in-kernel, no X11 needed.

- [ ] Track sticky state per modifier: `STICKY_SHIFT`, `STICKY_CTRL`, `STICKY_ALT`
- [ ] 5 rapid Shift taps → enable Sticky Keys (store in Registry `HKLM\SYSTEM\Input\StickyKeys`)
- [ ] When Sticky Keys enabled: pressing Shift latches it until next non-modifier keypress
- [ ] System tray indicator: show active sticky modifier
- [ ] Disable Sticky Keys: press two modifiers simultaneously
- [ ] Commit: `"kernel: sticky keys accessibility"`

---

## 7. Key Repeat & Typematic Rate

**Prompt:** When a key is held down, the keyboard controller sends repeat scancodes. The first repeat occurs after a delay (typematic delay), then at a repeating rate (typematic rate). Both are configurable via PS/2 commands (command 0xF3). Default: 500ms delay, 30 chars/sec rate. Store in Registry `HKLM\SYSTEM\Input\TypematicDelay` and `TypematicRate`. The keyboard settings applet lets the user adjust both. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: keyboard typematic rate"`. Add notes directly in this TODO section.

- [ ] Send PS/2 command 0xF3 with encoded typematic rate/delay at init
- [ ] Read default values from Registry: `HKLM\SYSTEM\Input\TypematicDelay` (default: 500ms), `TypematicRate` (default: 30 cps)
- [ ] Keyboard settings applet: delay slider (250ms/500ms/750ms/1000ms), rate slider (2–30 cps)
- [ ] Commit: `"kernel: keyboard typematic rate"`

---

## 8. Localization Framework *(Stretch)*

**Prompt:** Stretch: per-locale .ini files at `C:\Impossible\System\Locale\{code}.ini`. `locale_get(key)` returns localized string. All UI uses locale_get() instead of hardcoded English. Start with en-US, add fr-FR/de-DE/es-ES. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: localization framework"`. Add notes directly in this TODO section.

- [ ] *(Stretch)* UI string files: `C:\Impossible\System\Locale\{code}.ini`
- [ ] *(Stretch)* `locale_get(key)` — return localized string for current locale
- [ ] *(Stretch)* Default: `en-US.ini`, additional: `fr-FR.ini`, `de-DE.ini`, `es-ES.ini`
- [ ] *(Stretch)* All UI elements use `locale_get()` instead of hardcoded strings
- [ ] Commit: `"kernel: localization framework"`

---

## Priority Order

| Priority | Section                     | Reason                                       |
|----------|-----------------------------|----------------------------------------------|
| 🟠 P1    | §1 Keyboard Layout System   | Foundation for i18n input                    |
| 🟠 P1    | §2 Built-in Layouts         | Ship 6 layouts                               |
| 🟠 P1    | §4 Unicode / UTF-8          | Text support for all languages               |
| 🟠 P1    | §5 Dead Keys                | French/German layouts require dead keys      |
| 🟡 P2    | §3 Layout Switching         | Win+Space, system tray indicator             |
| 🟡 P2    | §6 Sticky Keys              | Accessibility — matches Windows behavior     |
| 🟡 P2    | §7 Typematic Rate           | User comfort — configurable repeat           |
| 🟢 P3    | §8 Localization             | Multi-language UI strings                    |

---

## Key Files

| File                                    | Purpose                              |
|-----------------------------------------|--------------------------------------|
| `src/kernel/kbd_layout.c`               | [NEW] Keyboard layout system         |
| `include/kbd_layout.h`                  | [NEW] Layout API header              |
| `resources/layouts/`                    | [NEW] Layout data tables             |
| `src/kernel/drivers/keyboard.c`         | [MODIFY] Replace hardcoded US QWERTY |

---

## OS Comparison

| Feature                         | Windows 11                        | Linux (X11/Wayland)               | Impossible OS                              |
|---------------------------------|-----------------------------------|-----------------------------------|--------------------------------------------|
| Keyboard layout system          | ✅ Win32 keyboard subsystem        | ✅ xkb (X11), evdev                | ⬜ §1 P1                                   |
| Built-in layout count           | ✅ ~200 layouts                   | ✅ ~200 layouts                    | ⬜ §2 P1 — start with 6                   |
| Layout switching (hotkey)       | ✅ Win+Space                      | ✅ Win+Space / Super+Space         | ⬜ §3 P2                                  |
| UTF-8 / Unicode input           | ✅ UTF-16 internally, UTF-8 API   | ✅ UTF-8 native                    | ⬜ §4 P1                                  |
| Dead key compose sequences      | ✅ In Win32 keyboard stack         | ✅ xkb compose table               | ⬜ §5 P1 — **in-kernel, simpler than xkb** |
| Sticky Keys (accessibility)     | ✅ Kernel-level keyboard filter   | ⚠️ X11/AT-SPI (user-space)        | ⬜ §6 P2 — **in-kernel like Windows**      |
| Typematic rate configuration    | ✅ Control Panel / Registry        | ✅ `kbdrate` / `setleds`           | ⬜ §7 P2                                  |
| Localization framework          | ✅ MUI resource DLLs              | ✅ gettext / .po files             | ⬜ §8 P3 — ini-file approach              |
| **In-kernel dead keys**         | ✅ (kernel filter level)          | ❌ (xkb is user-space)            | ⬜ **§5 — in-kernel, no xkb complexity**  |
| **In-kernel sticky keys**       | ✅ (kernel filter level)          | ❌ (X11 user-space)               | ⬜ **§6 — in-kernel, beats Linux**        |
