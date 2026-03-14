# Phase 10 — Compatibility & Internationalization

> **Goal:** Provide international keyboard layouts, Unicode support, and
> optional Java bytecode execution. PE is the **native and only** binary format
> (see `TODO-P0105-Native.md` for PE loader, Win32 native API, and DLL system).
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

> [!NOTE]
> **Win32 compatibility** (PE loader, kernel32/user32/gdi32 stubs, IAT patching,
> calling convention bridge, PE resource parser, shell icon API, Win32 path
> translation) has been moved to **`TODO-P0105-Native.md`** for consolidated
> tracking.

---

## 1. Keyboard Layouts & Internationalization

### 1.1 Keyboard Layout System

**Prompt:** Replace hardcoded US QWERTY scancode table in `keyboard.c` with a layout system. Define `struct kbd_layout` with name, code ("en-US"), normal[128], shift[128], altgr[128] arrays. `kbd_set_layout(code)` switches active layout. Store in Registry `System\Input\KeyboardLayout`. After completing all items, create `docs/architecture/keyboard-layouts.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: keyboard layout system"`.


- [ ] Create `src/kernel/kbd_layout.c` and `include/kbd_layout.h`
- [ ] Define `struct kbd_layout` (name, code, normal[128], shift[128], altgr[128])
- [ ] Replace hardcoded US QWERTY scancode→ASCII table in `keyboard.c` with layout lookup
- [ ] `kbd_set_layout(code)` — switch active layout
- [ ] `kbd_get_layout()` — return current layout code
- [ ] Registry: `System\Input\KeyboardLayout = "en-US"`
- [ ] Commit: `"kernel: keyboard layout system"`

### 1.2 Built-in Layouts

**Prompt:** Define layout tables: US QWERTY (default), UK English (£ vs $), German QWERTZ (Z/Y swap, umlauts on AltGr), French AZERTY (A/Q, Z/W swap), Spanish (ñ, accents), Dvorak (alt layout). Store as static arrays in `resources/layouts/`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: built-in keyboard layouts"`.


- [ ] Create `resources/layouts/` directory with layout data
- [ ] **US English (QWERTY)** — `en-US` (default)
- [ ] **UK English** — `en-GB` (different symbols: £ vs $, @ position)
- [ ] **German (QWERTZ)** — `de-DE` (Z/Y swapped, umlauts on AltGr)
- [ ] **French (AZERTY)** — `fr-FR` (A/Q, Z/W swapped, accents)
- [ ] **Spanish** — `es-ES` (ñ, accents)
- [ ] **Dvorak** — `en-DV` (alternative layout)
- [ ] Commit: `"kernel: built-in keyboard layouts (6 layouts)"`

### 1.3 Layout Switching

**Prompt:** Win+Space cycles installed layouts. System tray shows 2-letter indicator ("EN", "FR", "DE"). Click indicator → layout picker popup. Settings applet for layout management. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"desktop: keyboard layout switching"`.


- [ ] Win+Space → cycle through installed layouts
- [ ] System tray indicator: show current layout code (`EN`, `FR`, `DE`)
- [ ] Click tray indicator → layout picker popup
- [ ] Input → keyboard settings applet for layout management
- [ ] Commit: `"desktop: keyboard layout switching (Win+Space)"`

### 1.4 Unicode / UTF-8 Support

**Prompt:** Store all text as UTF-8. Implement `utf8_encode(codepoint, buf)` and `utf8_decode(buf, codepoint_out)`. Keyboard outputs UTF-8. stb_truetype already supports Unicode codepoints. Stretch: Noto Sans fallback font for CJK. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: UTF-8 Unicode support"`.


- [ ] Store all text strings internally as UTF-8
- [ ] UTF-8 encode/decode helpers: `utf8_encode(codepoint, buf)`, `utf8_decode(buf, codepoint_out)`
- [ ] Keyboard input: convert layout output to UTF-8 codepoints
- [ ] Font rendering: `stb_truetype` already supports Unicode codepoints
- [ ] *(Stretch)* Noto Sans as fallback font (covers all Unicode scripts)
- [ ] Commit: `"kernel: UTF-8 Unicode support"`

### 1.5 Localization Framework *(Stretch)*

**Prompt:** Stretch: per-locale .ini files at `C:\Impossible\System\Locale\{code}.ini`. `locale_get(key)` returns localized string. All UI uses locale_get() instead of hardcoded English. Start with en-US, add fr-FR/de-DE/es-ES. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: localization framework"`.


- [ ] *(Stretch)* UI string files: `C:\Impossible\System\Locale\{code}.ini`
- [ ] *(Stretch)* `locale_get(key)` — return localized string for current locale
- [ ] *(Stretch)* Default: `en-US.ini`, additional: `fr-FR.ini`, `de-DE.ini`, `es-ES.ini`
- [ ] *(Stretch)* All UI elements use `locale_get()` instead of hardcoded strings
- [ ] Commit: `"kernel: localization framework"`

---

## 2. Java Runtime *(Optional)*

### 2.1 GraalVM Native Images (Approach B — Recommended First)

**Prompt:** Compile Java to native PE via GraalVM `native-image` on host. No JVM needed at runtime — runs through the PE loader. Requires PE output + syscalls (mmap, file I/O). Test with Hello World. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: GraalVM native image support"`.


- [ ] *(Stretch)* Compile Java programs to native PE on host using `native-image`
- [ ] *(Stretch)* Include compiled binary on C:\
- [ ] *(Stretch)* Execute like any other PE program — no JVM needed at runtime
- [ ] *(Stretch)* Prerequisite: working PE loader + enough syscalls (`mmap`, file I/O)

### 2.2 Mini-JVM Bytecode Interpreter (Approach C — Educational)

**Prompt:** Build minimal JVM (~2-4K lines). Parse .class format: magic 0xCAFEBABE, constant pool, methods, Code attribute. Implement ~40 opcodes: constants, arithmetic, variables, control flow, objects. Map System.out.println → sys_write. Shell: `java HelloWorld.class`. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: Mini-JVM"`.


- [ ] *(Stretch)* Create `src/apps/jvm/jvm.c` (~2000-4000 lines)
- [ ] *(Stretch)* Parse `.class` file format: magic, constant pool, methods, code attribute
- [ ] *(Stretch)* Implement operand stack (per method frame)
- [ ] *(Stretch)* Implement bytecode interpreter loop (~40 essential opcodes):
  - [ ] Constants: `iconst_0`–`iconst_5`, `ldc`
  - [ ] Arithmetic: `iadd`, `isub`, `imul`, `idiv`
  - [ ] Variables: `iload`, `istore`, `aload`, `astore`
  - [ ] Control: `if_icmpge`, `goto`, `ireturn`, `return`
  - [ ] Objects: `new`, `invokespecial`, `invokevirtual`
  - [ ] I/O: `getstatic` (System.out), `invokevirtual` (println)
- [ ] *(Stretch)* Implement `System.out.println(String)` → `sys_write()`
- [ ] *(Stretch)* Shell command: `java HelloWorld.class` → run
- [ ] *(Stretch)* Test: "Hello, World!" Java program

### 2.3 JamVM Port (Approach A — Full JVM)

**Prompt:** Port JamVM (~15K lines, GPL 2.0). Requires mmap, file I/O, threading, ZIP parsing (miniz). Most complete Java but most effort. Stretch goal — prioritize approaches B and C first. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: JamVM port"`.


- [ ] *(Stretch)* Port JamVM interpreter (~15K lines, GPL 2.0)
- [ ] *(Stretch)* Prerequisites: `mmap()`, file I/O, threading
- [ ] *(Stretch)* Covers: classes, methods, strings, arrays, exceptions
- [ ] *(Stretch)* Load `.jar` files (ZIP parsing via miniz)

---

## 3. PE Compatibility Test Suite

**Prompt:** Create `tests/compat/` with pre-compiled PE binaries: MinGW PE Hello World, MinGW file I/O, MinGW memory allocation. Run all and verify expected output. After all items, mark `[x]`, run `bash scripts/build.sh clean`, commit `"tests: PE compatibility test suite"`.


- [ ] Create `tests/compat/` directory
- [ ] MinGW "Hello World" console program → test PE load + WriteConsoleA
- [ ] MinGW file I/O program → test CreateFileA + ReadFile + WriteFile
- [ ] MinGW memory allocation program → test VirtualAlloc + HeapAlloc
- [ ] Commit: `"tests: PE compatibility test suite"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | **P0105** §3–5 | PE loader + console (see `TODO-P0105-Native.md`) |
| 🟠 P1 | §1.1–1.2 Keyboard Layout System | International input support |
| 🟠 P1 | §1.4 UTF-8 Unicode | Text support for all languages |
| 🟡 P2 | §1.3 Layout Switching | Win+Space, system tray indicator |
| 🟢 P3 | §3 PE Compatibility Tests | PE regression testing |
| 🟢 P3 | §1.5 Localization | Multi-language UI |
| 🔵 P4 | §2 Java Runtime | JVM support (educational/future) |
