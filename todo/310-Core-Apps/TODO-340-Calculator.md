# P0506 — Calculator

> **Goal:** Calculator app with basic arithmetic, memory functions,
> and stretch goals for scientific/programmer modes.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Calculator

### 1.1 Calculator Core

**Prompt:** The Calculator app is a compact fixed-size window with a display area and a 5×4 button grid. Buttons rendered with `gfx_fill_rounded_rect()` with hover/press state colors from the theme. The display shows the current number (large font, right-aligned) and an operation preview (smaller, showing the pending expression like "42 +"). Use floating-point arithmetic via the kernel math shims (Phase 02 `kmath.h`). After completing all items,sh clean`, and commit as `"apps: Calculator layout"`.
- [ ] Create `src/apps/calculator/calculator.c`
- [ ] Define `struct calculator` (display_value, operand, memory, op, flags)
- [ ] Button grid: 5 rows × 4 columns (numbers, operators, functions)
- [ ] Buttons rendered with `gfx_fill_rounded_rect()` + hover/press state colors
- [ ] Display area: current number (large font) + operation preview (small)
- [ ] Commit: `"apps: Calculator layout"`

### 1.2 Arithmetic Engine

**Prompt:** The arithmetic engine handles operator precedence simply by using a two-operand model: the user enters operand1, presses an operator, enters operand2, presses = to compute. Store the pending operation and operand. C (clear) resets everything, CE (clear entry) clears only the current display without losing the pending operation. Handle division by zero gracefully (display "Error"). Accept keyboard input: numpad digits, +, -, *, /, Enter for =. After completing all items,sh clean`, and commit as `"apps: Calculator arithmetic"`.
- [ ] Basic operations: +, −, ×, ÷
- [ ] = key: compute result, display
- [ ] C: clear all, CE: clear entry only
- [ ] ⌫: backspace one digit
- [ ] Decimal point input
- [ ] Percentage (%)
- [ ] Sign toggle (±)
- [ ] 1/x, x², √x
- [ ] Division by zero → display "Error"
- [ ] Keyboard input: numpad and regular number keys
- [ ] Commit: `"apps: Calculator arithmetic"`

### 1.3 Memory & History

**Prompt:** Memory buttons (M+, M−, MR, MC, MS) use a separate stored value that persists across calculations. Stretch goals: Scientific mode adds trig functions (sin/cos/tan — using `kmath.h` shims), log, sqrt, power, π. Programmer mode shows hex/binary/octal representations with bitwise operators (AND, OR, XOR, NOT, shifts). History lists previous calculations. After completing all items,sh clean`, and commit as `"apps: Calculator memory and advanced modes"`.
- [ ] Memory buttons: M+, M−, MR, MC, MS
- [ ] *(Stretch)* Scientific mode: sin, cos, tan, log, sqrt, pow, π
- [ ] *(Stretch)* Programmer mode: hex, binary, octal, bitwise ops
- [ ] *(Stretch)* History: list of previous calculations
- [ ] Commit: `"apps: Calculator memory and advanced modes"`

