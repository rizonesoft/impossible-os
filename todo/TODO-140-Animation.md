# P0205 — Animation Engine

> **Goal:** Time-based tweening with easing functions for smooth UI transitions.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---

## 1. Tween Engine *(from Phase 02 §6)*

> Required by window transitions, menu popups, notification slides, and more.

### 1.1 Tween Engine *(from Phase 02 §6.1)*

**Prompt:** The animation engine provides time-based interpolation (tweening) for smooth UI transitions. A `gfx_tween_t` stores: start value, end value, current value, duration in ms, elapsed time, and an easing function pointer. `gfx_tween_update(delta_ms)` advances the tween by the frame delta time and recomputes the current value using the easing function. Easing functions take `t` (0.0→1.0) and return a shaped `t`: linear is identity, ease-out-cubic is `1 - (1-t)^3` (starts fast, decelerates), ease-in-quad is `t^2` (starts slow, accelerates). The compositor calls `gfx_tween_update` each frame with the frame delta. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"gfx: animation engine with easing"`. Update `README.md` if it contains stale or incorrect references to animations. Add notes, gotchas, and design decisions directly in this TODO section covering the tween engine, easing functions, and compositor integration.


- [ ] Create `src/kernel/gfx/gfx_animate.c`
- [ ] Define `gfx_tween_t` struct (from, to, current, duration_ms, elapsed_ms, easing, active)
- [ ] Implement `gfx_tween_start(tw, from, to, duration_ms, easing)`
- [ ] Implement `gfx_tween_update(tw, delta_ms)` — advance by delta time
- [ ] Implement `gfx_tween_value(tw)` — get interpolated current value
- [ ] Implement easing functions:
  - [ ] `GFX_EASE_LINEAR`
  - [ ] `GFX_EASE_IN_QUAD` / `GFX_EASE_OUT_QUAD` / `GFX_EASE_IN_OUT_QUAD`
  - [ ] `GFX_EASE_IN_CUBIC` / `GFX_EASE_OUT_CUBIC` / `GFX_EASE_IN_OUT_CUBIC`
  - [ ] `GFX_EASE_BOUNCE`
- [ ] Commit: `"gfx: animation engine with easing"`

### 1.2 Window Transition Animations *(from Phase 02 §6.2)*

**Prompt:** Each window state change should have a smooth animation: open (scale 90%→100% + fade in, 200ms ease-out-cubic), close (scale 100%→90% + fade out, 150ms), minimize (shrink toward the window's taskbar button position, 250ms), restore (reverse of minimize), maximize (expand to fill screen, 200ms). Use the tween engine from §3.1 — each animation creates tweens for the window's x, y, width, height, and opacity. The compositor must render animating windows at their interpolated position/size each frame. Add a Registry setting `HKCU\Software\Impossible\Theme\EnableAnimations` (default: true) and `HKCU\Software\Impossible\Theme\AnimationSpeed` (multiplier, default: 1.0). After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"desktop: window transition animations"`. Update `README.md` if it contains stale or incorrect references to animations. Add notes, gotchas, and design decisions directly in this TODO section covering window animation types, Registry settings, and reduce-motion option.


- [ ] Create `src/kernel/wm_anim.c`
- [ ] Window open: scale 90%→100% + fade in (200ms, ease-out-cubic)
- [ ] Window close: scale 100%→90% + fade out (150ms)
- [ ] Minimize: shrink toward taskbar button position (250ms)
- [ ] Restore: expand from taskbar button (250ms)
- [ ] Maximize: expand to fill screen (200ms)
- [ ] *(Stretch)* Snap left/right: slide + resize to half (200ms)
- [ ] *(Stretch)* Focus switch: subtle scale pulse (100ms)
- [ ] Menu popup: scale Y 0→100% from top (150ms)
- [ ] Registry: `HKCU\Software\Impossible\Theme\EnableAnimations`, `HKCU\Software\Impossible\Theme\AnimationSpeed`
- [ ] "Reduce motion" option disables all animations
- [ ] Commit: `"desktop: window transition animations"`

