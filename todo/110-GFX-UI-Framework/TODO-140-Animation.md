# P0205 — Animation Engine

> **Goal:** Time-based tweening with easing functions, a global animation manager,
> and spring physics for smooth, fluid UI transitions across all desktop elements.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Tween Engine

> Required by window transitions, menu popups, notification slides, and more.

### 1.1 Core Tween Engine

**Prompt:** The animation engine provides time-based interpolation (tweening) for smooth UI transitions. A `gfx_tween_t` stores: start value, end value, current value, duration in ms, elapsed time, and an easing function pointer. `gfx_tween_update(delta_ms)` advances the tween by the frame delta time and recomputes the current value using the easing function. Easing functions take `t` (0.0→1.0, represented as fixed-point 16.16) and return a shaped `t`. The compositor calls `gfx_tween_update()` each frame with the frame delta. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gfx: animation engine with easing"`. Add notes directly in this TODO section covering the fixed-point representation, easing function accuracy, and compositor integration.

> **Beats:** Linux Clutter uses GObject overhead. Windows DWM uses D3D interpolation. Impossible OS: pure integer fixed-point tweening — no floating point, no GObject, minimal overhead.

- [ ] Create `src/kernel/gfx/gfx_animate.c` and `include/kernel/gfx/gfx_animate.h`
- [ ] Define `gfx_tween_t` struct (from, to, current, duration_ms, elapsed_ms, easing_fn, active, on_complete callback)
- [ ] Implement `gfx_tween_start(tw, from, to, duration_ms, easing)` — initialize and activate
- [ ] Implement `gfx_tween_update(tw, delta_ms)` — advance by delta, write to current, call on_complete when done
- [ ] Implement `gfx_tween_value(tw)` — get interpolated current value
- [ ] Implement easing functions (16.16 fixed-point input/output):
  - [ ] `GFX_EASE_LINEAR` — identity
  - [ ] `GFX_EASE_IN_QUAD` / `GFX_EASE_OUT_QUAD` / `GFX_EASE_IN_OUT_QUAD`
  - [ ] `GFX_EASE_IN_CUBIC` / `GFX_EASE_OUT_CUBIC` / `GFX_EASE_IN_OUT_CUBIC`
  - [ ] `GFX_EASE_BOUNCE` — elastic bounce at end
  - [ ] `GFX_EASE_BACK` — slight overshoot then settle
- [ ] Commit: `"gfx: animation engine with easing"`

### 1.2 Window Transition Animations

**Prompt:** Each window state change should have a smooth animation: open (scale 90%→100% + fade in, 200ms ease-out-cubic), close (scale 100%→90% + fade out, 150ms), minimize (shrink toward the window's taskbar button position, 250ms), restore (expand from taskbar button, 250ms), maximize (expand to fill screen, 200ms). Use the tween engine from §1.1. The compositor renders animating windows at their interpolated position/size each frame. Add Registry settings `HKCU\Software\Impossible\Theme\EnableAnimations` (default: true) and `AnimationSpeed` (multiplier, default: 100 = 1×). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"desktop: window transition animations"`. Add notes directly in this TODO section covering the per-window animation state, minimize-to-taskbar trajectory calculation, and reduce-motion option.

- [ ] Create `src/kernel/wm_anim.c` and `include/kernel/wm_anim.h`
- [ ] Per-window `wm_anim_state_t`: tweens for x, y, w, h, opacity, scale_x, scale_y
- [ ] Window open: scale 90%→100% + opacity 0→255 (200ms, ease-out-cubic)
- [ ] Window close: scale 100%→90% + opacity 255→0 (150ms, ease-in-quad)
- [ ] Minimize: shrink position toward taskbar button rect (250ms, ease-in-quad)
- [ ] Restore: expand from taskbar button rect (250ms, ease-out-cubic)
- [ ] Maximize: expand to fill screen (200ms, ease-out-cubic)
- [ ] *(Stretch)* Snap left/right: slide + resize to half (200ms)
- [ ] *(Stretch)* Focus switch: subtle scale pulse (100ms)
- [ ] Menu popup: scale Y 0→100% from top (150ms, ease-out-quad)
- [ ] Registry: `HKCU\Software\Impossible\Theme\EnableAnimations`, `AnimationSpeed`
- [ ] "Reduce motion" option: set `EnableAnimations = 0`, instant transitions
- [ ] Commit: `"desktop: window transition animations"`

---

## 2. Global Animation Manager

**Prompt:** Individual tweens are fine, but the desktop needs a centralized animation scheduler — a list of all active tweens across all windows. The compositor calls `anim_mgr_tick(delta_ms)` once per frame to advance all running animations. `anim_mgr_add(tween)` registers a new animation. Completed animations are removed automatically. This allows the compositor to drive all animations from a single tick, rather than each subsystem managing its own timing. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt, run `bash scripts/build.sh clean`, and commit as `"gfx: global animation manager"`.

> **Beats:** Windows DWM has a similar internal animation scheduler. Linux Mutter uses GNOME Shell JS animation scheduler. Impossible OS: in-kernel, zero-overhead animation list — no JS, no GObject.

- [ ] Create `src/kernel/gfx/anim_mgr.c` and `include/kernel/gfx/anim_mgr.h`
- [ ] Fixed-size animation table: `ANIM_MAX = 64` running tweens at once
- [ ] `anim_mgr_add(tween_ptr)` — add to table (return -1 if full)
- [ ] `anim_mgr_cancel(tween_ptr)` — remove from table
- [ ] `anim_mgr_tick(delta_ms)` — advance all active tweens, auto-remove completed
- [ ] Wire into compositor main loop: call `anim_mgr_tick()` before compositing frame
- [ ] Boot log: `[OK] Animation manager: N active tweens` (debug only)
- [ ] Commit: `"gfx: global animation manager"`

---

## 3. Spring Physics Animations *(Stretch)*

**Prompt:** Spring physics produces more natural-feeling animations than easing curves. A spring has stiffness (k), damping (d), and target value. Each frame, `spring_update()` computes velocity and position using Hooke's law: `force = -k * (pos - target) - d * velocity`. Integer approximation of the ODE. This is how iOS `UISpringTimingParameters` and Android's spring interpolators work — the animation naturally settles without a fixed duration. Used for: window bounce on max/restore, elastic scroll overshoot, drag-release snap-back. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"gfx: spring physics animation"`.

> **Beats:** iOS uses spring animations natively. Android has `SpringForce`. Windows does not have built-in spring physics in DWM (only CSS springs in WinUI3). Linux Mutter uses fixed easing, not springs. Impossible OS can be ahead of Windows here.

- [ ] *(Stretch)* Create `src/kernel/gfx/gfx_spring.c`
- [ ] *(Stretch)* `spring_t` struct: position, velocity, target, stiffness, damping
- [ ] *(Stretch)* `spring_update(sp, delta_ms)` — Hooke's law integer step
- [ ] *(Stretch)* `spring_settled(sp)` — true if |velocity| < threshold and |pos - target| < 1
- [ ] *(Stretch)* Apply: window restore bounce, elastic scroll overshoot, drag-release snap
- [ ] *(Stretch)* Commit: `"gfx: spring physics animation"`

---

## 4. Compositing Integration Checklist

**Prompt:** Ensure all animation systems are correctly integrated into the compositor loop. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`.

- [ ] Compositor main loop calls `anim_mgr_tick(delta_ms)` before compositing
- [ ] `delta_ms` computed from PIT tick counter: `(current_ticks - last_ticks) * 1000 / PIT_HZ`
- [ ] Windows with active animations force a compositor redraw each frame
- [ ] Non-animating frames: compositor skips redraw if no dirty rects (uses existing dirty rect system)
- [ ] VSync-aware: animation tick delta capped at 33ms (30fps minimum) to avoid huge jumps
- [ ] `EnableAnimations = 0`: all `anim_mgr_add()` calls immediately complete (instant snap)

---

## Priority Order

| Priority | Section                        | Reason                                         |
|----------|--------------------------------|------------------------------------------------|
| 🔴 P0    | §1.1 Tween Engine              | Foundation — all other sections depend on this |
| 🟠 P1    | §1.2 Window Transitions        | Open/close/minimize/maximize feel              |
| 🟠 P1    | §2 Global Animation Manager    | Ensures all tweens tick from one place         |
| 🟡 P2    | §4 Compositor Integration      | Wiring delta_ms and dirty rect skipping        |
| 🟢 P3    | §3 Spring Physics              | Stretch — iOS-quality natural feel             |

---

## Key Files

| File                                  | Purpose                               |
|---------------------------------------|---------------------------------------|
| `src/kernel/gfx/gfx_animate.c`       | [NEW] Core tween engine + easing fns  |
| `include/kernel/gfx/gfx_animate.h`   | [NEW] Tween API header                |
| `src/kernel/gfx/anim_mgr.c`          | [NEW] Global animation scheduler      |
| `include/kernel/gfx/anim_mgr.h`      | [NEW] Animation manager header        |
| `src/kernel/wm_anim.c`               | [NEW] Per-window animation state      |
| `include/kernel/wm_anim.h`           | [NEW] Window animation header         |
| `src/kernel/gfx/gfx_spring.c`        | [NEW] Spring physics (stretch)        |

---

## OS Comparison

| Feature                           | 🪟 Windows 11 (DWM)                 | 🐧 Linux (Mutter/KWin)            | 🚀 Impossible OS                       |
| --------------------------------- | ---------------------------------- | -------------------------------- | ------------------------------------- |
| Window open/close animations      | ✅ DWM scale+fade                   | ✅ Mutter (GNOME) / KWin          | ⬜ §1.2 P1                             |
| Easing functions                  | ✅ CubicBezier in UWP               | ✅ CSS easing in Clutter          | ⬜ §1.1 P0 — 8 easing types            |
| Global animation scheduler        | ✅ DWM internal                     | ✅ Clutter stage                  | ⬜ §2 P1 — in-kernel table             |
| Animation enable/disable          | ✅ System → Accessibility           | ✅ `gtk-enable-animations`        | ⬜ §1.2 — Registry `EnableAnimations`  |
| Reduce motion (accessibility)     | ✅ Settings → Accessibility         | ✅ GTK prefer-reduced-motion      | ⬜ §1.2 — `EnableAnimations = 0`       |
| Spring physics                    | ❌ DWM only (CSS springs in WinUI3) | ❌ Fixed easing only in Mutter    | ⬜ §3 P3 — **native spring physics**   |
| Animate without GPU               | ❌ DWM requires D3D11               | ❌ Mutter/KWin require GPU        | ✅ **CPU framebuffer — works in QEMU** |
| In-kernel (no animation daemon)   | ❌ DWM separate process             | ❌ Mutter/KWin separate processes | ✅ **Single in-kernel tick**           |
| **Spring physics ahead of Win11** | ❌ No DWM spring support            | ❌                                | ⬜ **§3 — ahead of Windows DWM**       |
