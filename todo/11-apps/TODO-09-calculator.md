---
schema_version: 1
id: calculator
domain: 11-apps
status: active
title: "TODO-09 -- Calculator"
---

# TODO-09 -- Calculator

> **Goal:** Build `calc.exe` -- a three-mode calculator (Standard, Scientific, Programmer) with a
> gap-free history side-panel, memory registers, keyboard input, and clipboard integration.

> [!IMPORTANT]
> **Base implementation spec:** `§6` covers the fixed
> 320×480 px window, 5×4 button grid, two-operand model, C/CE/⌫/±/1/x/x²/√, memory M±/R/C/S,
> and keyboard shortcuts. This TODO is the full-app companion that adds: (a) calculation
> history panel not specified in TODO-12; (b) full Scientific mode with degree/radian toggle;
> (c) full Programmer mode with BYTE/WORD/DWORD/QWORD word-size selector.
>
> `kmath_sin`, `kmath_tan`, `kmath_atan`, `kmath_asin`, `kmath_log`, `kmath_log10` and `kmath_exp` already ship in
> `include/libc/math.h` (which includes `kmath.h`); §4 uses them, it does not add them.
> Noted in the base section §6 of this file.

---

## Inputs

- `§6` -- base calculator spec (window, button grid, two-operand model, memory ops)
- `include/kernel/kmath.h` -- `kmath_sqrt`, `kmath_pow`, `kmath_cos`, `kmath_acos`, `kmath_floor`, `kmath_fabs`, `kmath_fmod`; `include/libc/math.h` adds `kmath_sin/tan/atan/asin/log/log10/exp` and `KM_PI`
- `include/gfx.h` -- `gfx_fill_rounded_rect(s, x, y, w, h, radius, color)`, `gfx_fill_rect()`, `gfx_draw_rect()`
- `include/font_mgr.h` -- `ttf_draw_string()`, `ttf_measure_width()`, `ttf_get(FONT_UI, px)`
- `include/desktop/wm.h` -- `wm_create_window(title, x, y, w, h, flags)`; a fixed window omits `WM_FLAG_RESIZABLE`
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, data, size)` -- §3 history copy
- `include/desktop/controls.h` -- `CTRL_SCROLLBAR_VERT` -- §3 history scroll

---

## Outcome

`calc.exe` opens a 320×480 fixed window (Standard), 560×480 with history panel (history visible), or 560×480 wide (Scientific/Programmer) modes. Standard mode: full two-operand arithmetic with memory bank. History panel: last 20 calculations, click-to-copy. Scientific mode: trig/log/exp with deg/rad toggle. Programmer mode: hex/bin/oct representation, bitwise ops, BYTE–QWORD word size.

---

## Implementation Order

| Step | Section                   | 💎/⭐ | Dependency                                                                             | Status |
| ---- | ------------------------- | ----- | -------------------------------------------------------------------------------------- | :----: |
| 1    | §1 Standard Calculator UI | 💎    | §7                                                                                     |  [ ]   |
| 2    | §2 Arithmetic Engine      | 💎    | §7                                                                                     |  [ ]   |
| 3    | §3 Memory + History       | ⭐    | §7                                                                                     |  [ ]   |
| 4    | §4 Scientific Mode        | 💎    | §7                                                                                     |  [ ]   |
| 5    | §5 Programmer Mode        | 💎    | §7                                                                                     |  [ ]   |
| 6    | §6 Calculator             | 💎    | §7                                                                                     |  [ ]   |
| 7    | §7 Import Calculator      | 💎    | `10-platform-services/TODO-07` §7 (PE programs run), `10-platform-services/TODO-13` §2 |  [ ]   |

---

## 1. Standard Calculator UI

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 2. Arithmetic Engine

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 3. Memory + History

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 4. Scientific Mode

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 5. Programmer Mode

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 6. Calculator

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

> **Spawned-by:** root

> **Moved here 2026-11-14 from `09-desktop-shell/TODO-12-utilities.md` §2** (operator decision: `11-apps` owns the apps; the source section is now a pointer).

**Moved:** calculator

Built in Calculator, where the items of this section are kept in `docs/os-roadmap`. The section keeps its place here and stays open until §7 has brought it and it is shown to work.

- [ ] Shown to work here on the imported copy, as §7 brings it
- [ ] Commit: `"imports: calculator brings this section"`

## 7. Import Calculator

> **Spawned-by:** root

**Import:** calculator

Calculator is developed in its own repository and proved on Windows. This section brings its program and tests here and runs them on the OS.

- [ ] Copy the pinned program with `python3 scripts/imports.py --pull calculator` into `src/apps/calculator/`
- [ ] Build it in the OS build and run its engine tests there
- [ ] Open it on the desktop and walk its Standard, Scientific and Programmer windows
- [ ] Propose what had to change here with `python3 scripts/imports.py --propose calculator`
- [ ] Commit: `"apps: Calculator imported"`

---

## OS Comparison


| ⭐  | Feature                                                       | 🪟 Win11                                   | 🐧 Linux                        | 🚀 Impossible OS                                        |
| --- | ------------------------------------------------------------- | ------------------------------------------ | ------------------------------- | ------------------------------------------------------- |
| 💎  | Standard two-operand arithmetic + rounded-rect button grid    | ✅ Windows Calculator                      | ✅ GNOME Calculator / KCalc     | ⬜ §1–§2 -- `gfx_fill_rounded_rect`, hover/press states |
| 💎  | Memory register                                               | ✅ Windows Calculator                      | ✅ GNOME Calculator             | ⬜ §3 -- single double register, M indicator            |
| ⭐  | History panel with click-to-copy-to-clipboard                 | ✅ Windows Calculator (history sidebar)    | ⚠️ GNOME: history list only; no | ⬜ §3 -- 20-entry ring, `clipboard_set` on click,       |
| 💎  | Scientific mode                                               | ✅ Windows Calculator Scientific           | ✅ GNOME Calculator Scientific  | ⬜ §4 -- shipped `kmath_*` trig/log, deg/rad toggle     |
| 💎  | Programmer mode                                               | ✅ Windows Calculator Programmer           | ✅ KCalc Numeral System         | ⬜ §5 -- simultaneous multi-base display, bit-flip row  |
| ⭐  | BYTE/WORD/DWORD/QWORD word-size selector in Programmer mode   | ✅ Windows Calculator (word-size selector) | ⚠️ KCalc: no per-type clipping  | ⬜ §5 -- `uint64_t` clamp to selected width             |
| ⭐  | All 4 bases (HEX/DEC/OCT/BIN) shown at once, clickable switch | ✅ Windows Calculator (shows all 4         | ⚠️ KCalc: one base at a         | ⬜ §5 -- 4 always-visible rows, active-row highlight    |

Impossible OS Calculator ships all three modes (Standard/Scientific/Programmer) with a simultaneous four-base display in Programmer mode and a one-click clipboard history panel -- matching Windows Calculator feature-for-feature with no external dependencies.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **Standard UI:** `calc.exe` opens 320×480 fixed window; all buttons render with rounded corners; hover changes shade; press darkens; number display right-aligns; operation preview shows `"42 +"` after entering 42 + 
- [ ] **Arithmetic:** `3 + 4 =` → `7`; `10 ÷ 0 =` → `"Error"`; any digit press clears error; `9 × 9 = =` → second `=` re-applies (99); `CE` clears display without losing operand
- [ ] **Keyboard:** type `1 2 + 3 4 Enter` via keyboard → `46`; `Backspace` removes last digit; `Escape` clears all; numpad keys work
- [ ] **Memory:** enter `100`; `MS`; compute `200 + 300 =`; `MR` → display shows `100`; `M+` → memory = `600`; `MC` → M indicator disappears
- [ ] **History:** after 3 calculations, `[⌛]` opens history panel; panel shows all 3; click latest result → clipboard contains that value; oldest entry visible after 21+ calculations
- [ ] **Scientific:** switch to Scientific; window widens; `sin(30 DEG)` → `0.5`; `cos(0)` → `1`; `tan(45 DEG)` → `1`; switch to RAD; `sin(π/6)` → `0.5`; `log(100)` → `2`; `10^3` → `1000`; `5 n!` → `120`
- [ ] **Programmer:** switch to Programmer; enter `255`; all 4 bases show `FF / 255 / 377 / 1111 1111`; select BYTE → value clamped to 0–255; enter `A` in HEX → decimal shows `10`; `15 AND 6 =` → `6`; `15 XOR 9 =` → `6`; click a bit in bit-flip display → toggles all base displays
- [ ] Commit: `"apps: Calculator -- standard/scientific/programmer modes, memory, history, kmath additions"`
