# P2401 — Keyboard Layouts & Internationalization

> **Goal:** Provide international keyboard layouts, Unicode/UTF-8 support, and
> a localization framework for multi-language UI.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

---

## 1. Keyboard Layout System

**Prompt:** Replace hardcoded US QWERTY scancode table in `keyboard.c` with a layout system. Define `struct kbd_layout` with name, code ("en-US"), normal[128], shift[128], altgr[128] arrays. `kbd_set_layout(code)` switches active layout. Store in Registry `System\Input\KeyboardLayout`. After completing all items, create `docs/architecture/keyboard-layouts.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: keyboard layout system"`.


- [ ] Create `src/kernel/kbd_layout.c` and `include/kbd_layout.h`
- [ ] Define `struct kbd_layout` (name, code, normal[128], shift[128], altgr[128])
- [ ] Replace hardcoded US QWERTY scancode→ASCII table in `keyboard.c` with layout lookup
- [ ] `kbd_set_layout(code)` — switch active layout
- [ ] `kbd_get_layout()` — return current layout code
- [ ] Registry: `System\Input\KeyboardLayout = "en-US"`
- [ ] Commit: `"kernel: keyboard layout system"`

---

## 2. Built-in Layouts

**Prompt:** Define layout tables: US QWERTY (default), UK English (£ vs $), German QWERTZ (Z/Y swap, umlauts on AltGr), French AZERTY (A/Q, Z/W swap), Spanish (ñ, accents), Dvorak (alt layout). Store as static arrays in `resources/layouts/`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in keyboard layouts"`.


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

**Prompt:** Win+Space cycles installed layouts. System tray shows 2-letter indicator ("EN", "FR", "DE"). Click indicator → layout picker popup. Settings applet for layout management. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard layout switching"`.


- [ ] Win+Space → cycle through installed layouts
- [ ] System tray indicator: show current layout code (`EN`, `FR`, `DE`)
- [ ] Click tray indicator → layout picker popup
- [ ] Input → keyboard settings applet for layout management
- [ ] Commit: `"desktop: keyboard layout switching (Win+Space)"`

---

## 4. Unicode / UTF-8 Support

**Prompt:** Store all text as UTF-8. Implement `utf8_encode(codepoint, buf)` and `utf8_decode(buf, codepoint_out)`. Keyboard outputs UTF-8. stb_truetype already supports Unicode codepoints. Stretch: Noto Sans fallback font for CJK. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: UTF-8 Unicode support"`.


- [ ] Store all text strings internally as UTF-8
- [ ] UTF-8 encode/decode helpers: `utf8_encode(codepoint, buf)`, `utf8_decode(buf, codepoint_out)`
- [ ] Keyboard input: convert layout output to UTF-8 codepoints
- [ ] Font rendering: `stb_truetype` already supports Unicode codepoints
- [ ] *(Stretch)* Noto Sans as fallback font (covers all Unicode scripts)
- [ ] Commit: `"kernel: UTF-8 Unicode support"`

---

## 5. Localization Framework *(Stretch)*

**Prompt:** Stretch: per-locale .ini files at `C:\Impossible\System\Locale\{code}.ini`. `locale_get(key)` returns localized string. All UI uses locale_get() instead of hardcoded English. Start with en-US, add fr-FR/de-DE/es-ES. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: localization framework"`.


- [ ] *(Stretch)* UI string files: `C:\Impossible\System\Locale\{code}.ini`
- [ ] *(Stretch)* `locale_get(key)` — return localized string for current locale
- [ ] *(Stretch)* Default: `en-US.ini`, additional: `fr-FR.ini`, `de-DE.ini`, `es-ES.ini`
- [ ] *(Stretch)* All UI elements use `locale_get()` instead of hardcoded strings
- [ ] Commit: `"kernel: localization framework"`

---

## Priority Order

| Priority | Section                   | Reason                           |
|----------|---------------------------|----------------------------------|
| 🟠 P1     | §1 Keyboard Layout System | Foundation for i18n input        |
| 🟠 P1     | §2 Built-in Layouts       | Ship 6 layouts                   |
| 🟠 P1     | §4 Unicode / UTF-8        | Text support for all languages   |
| 🟡 P2     | §3 Layout Switching       | Win+Space, system tray indicator |
| 🟢 P3     | §5 Localization           | Multi-language UI strings        |

---

## Key Files

| File                                    | Purpose                              |
|-----------------------------------------|--------------------------------------|
| `src/kernel/kbd_layout.c`               | [NEW] Keyboard layout system         |
| `include/kbd_layout.h`                  | [NEW] Layout API header              |
| `resources/layouts/`                    | [NEW] Layout data tables             |
| `src/kernel/drivers/keyboard.c`         | [MODIFY] Replace hardcoded US QWERTY |
| `docs/architecture/keyboard-layouts.md` | [NEW] Layout system documentation    |
