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
> **Base implementation spec:** `09-desktop-shell/TODO-12-utilities.md §3` covers the fixed
> 320×480 px window, 5×4 button grid, two-operand model, C/CE/⌫/±/1/x/x²/√, memory M±/R/C/S,
> and keyboard shortcuts. This TODO is the full-app companion that adds: (a) calculation
> history panel not specified in TODO-12; (b) full Scientific mode with degree/radian toggle;
> (c) full Programmer mode with BYTE/WORD/DWORD/QWORD word-size selector.
>
> `kmath_sin`, `kmath_tan`, `kmath_atan`, `kmath_log`, `kmath_exp` are **missing** from
> `kmath.h` -- add them in §4 (`kmath_sin` via Taylor series, `kmath_tan` = `sin/cos`,
> `kmath_log` via ln series, `kmath_exp` via Taylor).
> Noted in `09-desktop-shell/TODO-12-utilities.md` Important Notes.

---

## Inputs

- `09-desktop-shell/TODO-12-utilities.md §3` -- base calculator spec (window, button grid, two-operand model, memory ops)
- `include/kernel/kmath.h` -- `kmath_sqrt`, `kmath_pow`, `kmath_cos`, `kmath_acos`, `kmath_floor`, `kmath_fabs`, `kmath_fmod`; §4 adds `kmath_sin/tan/atan/log/exp`
- `include/gfx.h` -- `gfx_fill_rounded_rect(s, x, y, w, h, radius, color)`, `gfx_fill_rect()`, `gfx_draw_rect()`
- `include/font_mgr.h` -- `ttf_draw_string()`, `ttf_measure_width()`, `ttf_get(FONT_UI, px)`
- `include/desktop/wm.h` -- `wm_create_window(x, y, w, h, title, WM_FLAG_NO_RESIZE)`
- `include/kernel/clipboard.h` (→ XREF `09-desktop-shell/TODO-01 §1`) -- `clipboard_set(CLIP_TEXT, data, size)` -- §3 history copy
- `include/desktop/controls.h` -- `CTRL_SCROLLBAR_VERT` -- §3 history scroll

---

## Outcome

`calc.exe` opens a 320×480 fixed window (Standard), 560×480 with history panel (history visible), or 560×480 wide (Scientific/Programmer) modes. Standard mode: full two-operand arithmetic with memory bank. History panel: last 20 calculations, click-to-copy. Scientific mode: trig/log/exp with deg/rad toggle. Programmer mode: hex/bin/oct representation, bitwise ops, BYTE–QWORD word size.

---

## Implementation Order

| Step | Section                | 💎/⭐ | Dependency                               |
| ---- | ---------------------- | ----- | ---------------------------------------- |
| 1    | Standard Calculator UI | 💎    | `gfx_fill_rounded_rect`, `ttf_draw_string`, `wm_create_window` |
| 2    | Arithmetic Engine      | 💎    | §1 complete                              |
| 3    | Memory + History       | ⭐    | §2 complete, `clipboard_set`             |
| 4    | Scientific Mode        | 💎    | §2 complete, `kmath_sin/tan/log/exp` additions |
| 5    | Programmer Mode        | 💎    | §2 complete                              |

---

## 1. Standard Calculator UI `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-12-utilities.md §3` -- window size, button layout, display area, button rendering detail.

**Source file:** `src/apps/calc/calc.c`; header `include/apps/calc/calc.h`

- [ ] `wm_create_window(200, 150, 320, 480, "Calculator", WM_FLAG_NO_RESIZE)` -- fixed, non-resizable
- [ ] **Display area** (top 80 px, right-aligned):
  - [ ] Small operation preview line: `"{operand} {op}"` e.g. `"42 +"` (12 px font, gray)
  - [ ] Current number: right-aligned in display box, 28 px font; shrink font at 12+ digits
- [ ] **Button grid** (5 rows × 4 columns, 40×40 px each, 4 px gap):
  - [ ] Row 1 (memory): `MC` `MR` `M+` `M−` `MS`
  - [ ] Row 2 (clear): `%` `CE` `C` `⌫`
  - [ ] Row 3 (functions): `1/x` `x²` `√x` `÷`
  - [ ] Row 4–6 (number pad): `7 8 9 ×` / `4 5 6 −` / `1 2 3 +`
  - [ ] Row 7: `± 0 . =`
- [ ] **Button rendering**: `gfx_fill_rounded_rect(s, bx, by, 40, 40, 6, bg_color)` where `bg_color` cycles through 3 states: normal (`0xFF2D2D2D` dark), hover (`0xFF3C3C3C`), pressed (`0xFF1A1A1A`); operator buttons use accent color; `=` uses blue accent; memory row uses lighter gray
- [ ] **Mode toolbar** (above display): `[Standard]` `[Scientific]` `[Programmer]` tab buttons (24 px tall); active tab underlined; clicking switches mode and resizes window
- [ ] **History toggle** `[⌛]` button top-right → expand window to 560 px wide, show history panel on right (§3)

---

## 2. Arithmetic Engine `[Sonnet]`

> → XREF: `09-desktop-shell/TODO-12-utilities.md §3` -- two-operand model, division-by-zero, keyboard shortcuts.

- [ ] **State**: `typedef struct { char display[32]; double operand; char op; int after_op; int has_error; double memory; int mode; int deg_mode; } calc_t;`
- [ ] **Two-operand model**: `operand1` → press `op` → `operand2` → `=` → `result`
  - [ ] On operator press: if previous result → use as `operand`; set `g_op`; set `after_op = 1`; update preview line
  - [ ] On `=`: compute `result = apply(operand, op, parse(display))`; show result; set `after_op = 1` for next entry; record in history (§3)
  - [ ] Chaining: press `+` after `=` → continue from result
- [ ] **Operations**: `+`, `−`, `×`, `÷`, `=`; also `%` (percent of operand), `±` (negate), `1/x`, `x²`, `√x` (immediate, no `=` required)
- [ ] **Clear**: `C` → `g_display=""`, `g_operand=0`, `g_op=0`, `g_after_op=0`, preview cleared; `CE` → `g_display=""` only; `⌫` → remove last char (`strlen(display)--`)
- [ ] **Division by zero**: `÷ 0 =` → `g_display = "Error"`, `g_has_error = 1`; any digit press clears error
- [ ] **Decimal**: `.` appended only if no `.` already in display; leading `0.` auto-added
- [ ] **Keyboard input**: digit keys `0–9` + numpad; `.` decimal; `+` `-` `*` `/`; `Enter` = `=`; `Backspace` = `⌫`; `Delete` = `CE`; `Escape` = `C`

---

## 3. Memory + History `[Sonnet]`

- [ ] **Memory register** (single `double g_memory`):
  - [ ] `MS` (Memory Store): `g_memory = parse(display)`; light `M` indicator in display top-left when non-zero
  - [ ] `MR` (Memory Recall): `g_display = format(g_memory)`
  - [ ] `M+`: `g_memory += parse(display)`
  - [ ] `M−`: `g_memory -= parse(display)`
  - [ ] `MC` (Memory Clear): `g_memory = 0`; clear `M` indicator
- [ ] **History panel** (right 240 px when visible, `CTRL_SCROLLBAR_VERT` on right edge):
  - [ ] Ring buffer of 20 `struct hist_entry { char expr[48], result[32] }` entries; oldest overwritten
  - [ ] Each entry renders: expression (gray, 11 px) above result (white, 16 px); separator line
  - [ ] Scroll: mouse wheel or scrollbar
  - [ ] **Click history entry** → `clipboard_set(CLIP_TEXT, entry.result, strlen(entry.result))` + flash accent color for 300 ms as confirmation
  - [ ] History persists to `HKCU\Software\Impossible\Calculator\History\{0..19}` on window close; restored on open
- [ ] **History recording**: after every successful `=` (no error), prepend `"{display_before_op} {op} {display_op2} = {result}"` to ring buffer

---

## 4. Scientific Mode `[Sonnet]`

- [ ] **Window resize**: switching to Scientific → resize to 560×480; adds 2 extra button columns on left (total layout becomes: function columns | existing numpad); mode toolbar updates active tab
- [ ] **Missing kmath additions** (add to `include/kernel/kmath.h`):
  - [ ] `kmath_sin(x)` -- Taylor series: `Σ (−1)^n × x^(2n+1) / (2n+1)!`, converge to 1e-12; reduce angle mod `2π` first
  - [ ] `kmath_tan(x)` = `kmath_sin(x) / kmath_cos(x)`; guard for cos = 0 → return `∞` (large sentinel)
  - [ ] `kmath_atan(x)` -- Taylor for `|x| < 1`; use identity `atan(x) = π/2 - atan(1/x)` for `|x| > 1`
  - [ ] `kmath_log(x)` -- natural log via `kmath_pow(e, y) = x` convergence or series; `#define KMATH_E 2.718281828459045`
  - [ ] `kmath_exp(x)` -- `Σ x^n / n!`; converge to 1e-12
  - [ ] `#define KMATH_PI 3.141592653589793`
- [ ] **Degree/Radian toggle**: `[DEG]` `[RAD]` button (toggles `g_deg_mode`); when `DEG`: multiply input by `π/180` before trig calls; display shows current mode
- [ ] **Scientific button layout** (2 extra columns × 5 rows):
  - [ ] Col A: `sin` `cos` `tan` `log` `ln`
  - [ ] Col B: `asin` `acos` `atan` `10^x` `e^x`
  - [ ] Below existing: `π` `e` `x^y` `n!` `mod`
- [ ] `sin`/`cos`/`tan`: call kmath with deg→rad conversion if `g_deg_mode`; result replaces display (immediate, no `=`)
- [ ] `asin`/`acos`/`atan`: inverse; result in degrees if `g_deg_mode` (multiply by `180/π`)
- [ ] `log`: log base 10 = `kmath_log(x) / kmath_log(10)`
- [ ] `ln`: `kmath_log(x)` directly
- [ ] `10^x`: `kmath_pow(10.0, parse(display))`; `e^x`: `kmath_exp(parse(display))`
- [ ] `x^y`: two-operand form -- press `x^y`, enter y, press `=`; uses `kmath_pow(operand, y)`
- [ ] `n!`: integer factorial; guard overflow (display "Error" for n > 20)
- [ ] `mod`: two-operand modulo via `kmath_fmod`
- [ ] `π` / `e`: insert constant into display

---

## 5. Programmer Mode `[Sonnet]`

- [ ] **Window resize**: switching to Programmer → resize to 360×560; replaces number rows with extended layout
- [ ] **Word-size selector** (radio buttons at top): `QWORD` `DWORD` `WORD` `BYTE` -- clips value to 64/32/16/8-bit unsigned integer; stored as `uint64_t g_int_val`; truncate on switch
- [ ] **Multi-base display** (below word-size): always shows all 4 representations simultaneously:
  - [ ] `HEX  {value in hex, uppercase, leading-zero-padded to word size}`
  - [ ] `DEC  {value in decimal}`
  - [ ] `OCT  {value in octal}`
  - [ ] `BIN  {binary, grouped by nibble e.g. 0100 1101}`
  - [ ] Highlighted row = active input base (click a row to switch input base)
- [ ] **Hex digit buttons** `A`–`F` (active only in HEX input base; grayed out in DEC/OCT/BIN)
- [ ] **Bit-flip display**: horizontal row of 8/16/32/64 bit boxes (based on word size); click bit → toggle; updates all base displays
- [ ] **Bitwise operators**: `AND` `OR` `XOR` `NOT` `LSH` (left shift) `RSH` (right shift); two-operand model same as standard; `NOT` is immediate (bitwise invert within word size); `LSH`/`RSH` take shift count as second operand
- [ ] **Additional ops**: `NAND` `NOR` (stretch, computed via NOT(AND) / NOT(OR))
- [ ] **Keyboard in HEX mode**: `A`–`F` keys active; digit keys 0–9 always active; `&` → AND; `|` → OR; `^` → XOR; `~` → NOT; `<` → LSH; `>` → RSH

---

## OS Comparison


| ⭐ | Feature                                                                           | 🪟 Win11                                      | 🐧 Linux                           | 🚀 Impossible OS                                          |
|----|-----------------------------------------------------------------------------------|--------------------------------------------|---------------------------------|--------------------------------------------------------|
| 💎 | Standard two-operand arithmetic + rounded-rect button grid                        | ✅ Windows Calculator                      | ✅ GNOME Calculator / KCalc     | ⬜ §1–§2 -- `gfx_fill_rounded_rect`, hover/press states |
| 💎 | Memory register                                                                   | ✅ Windows Calculator                      | ✅ GNOME Calculator             | ⬜ §3 -- single double register, M indicator            |
| ⭐ | History panel with click-to-copy-to-clipboard                                     | ✅ Windows Calculator (history sidebar)    | ⚠️ GNOME: history list only; no | ⬜ §3 -- 20-entry ring, `clipboard_set` on click,       |
| 💎 | Scientific mode                                                                   | ✅ Windows Calculator Scientific           | ✅ GNOME Calculator Scientific  | ⬜ §4 -- add `kmath_sin/tan/log/exp`, deg/rad toggle    |
| 💎 | Programmer mode                                                                   | ✅ Windows Calculator Programmer           | ✅ KCalc Numeral System         | ⬜ §5 -- simultaneous multi-base display, bit-flip row  |
| ⭐ | BYTE/WORD/DWORD/QWORD word-size selector in Programmer mode                       | ✅ Windows Calculator (word-size selector) | ⚠️ KCalc: no per-type clipping  | ⬜ §5 -- `uint64_t` clamp to selected width             |
| ⭐ | All 4 bases (HEX/DEC/OCT/BIN) displayed simultaneously with clickable base switch | ✅ Windows Calculator (shows all 4         | ⚠️ KCalc: one base at a         | ⬜ §5 -- 4 always-visible rows, active-row highlight    |

Impossible OS Calculator ships all three modes (Standard/Scientific/Programmer) with a
simultaneous four-base display in Programmer mode and a one-click clipboard history panel --
matching Windows Calculator feature-for-feature with no external dependencies.

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
