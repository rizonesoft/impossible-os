---
schema_version: 1
id: animation-engine
domain: 08-graphics-ui
status: active
title: "TODO-04 -- Animation Engine"
---

# TODO-04 -- Animation Engine

> **Goal:** Build a time-based tween engine with 16.16 fixed-point easing functions, a global animation manager ticked inside `wm_composite()`, per-window transition animations (open/close/minimize/restore/maximize/snap/menu popup), Registry-driven speed and reduce-motion controls, and integer spring physics for natural elastic effects. Animations are the prerequisite for every animated desktop surface: Start Menu slide, notification slide-in, context menu pop, and window open/close all depend on this tick being live inside the compositor loop.

> [!IMPORTANT]
> `PIT_TARGET_FREQ = 100` Hz (from `include/kernel/drivers/pit.h`) → `delta_ms = (current_ticks - last_ticks) × 10`; cap at 33 ms per frame to prevent jump-cuts after preemption. `system_get_ticks()` from `include/kernel/timer.h` is the monotonic tick source. `wm_composite()` in `include/desktop/wm.h` is the compositor entry point -- `anim_mgr_tick()` is called at its top. `wm_mark_dirty()` forces a redraw. All tween arithmetic uses 16.16 fixed-point integers; no `float` or `double` anywhere. `theme_get()` (TODO-01) must be live before §4 window transitions so themed titlebar colors are available when windows animate open. Complete sections in order: tween → easing → manager → registry → window transitions → spring → compositor integration.

## Inputs

- `include/kernel/drivers/pit.h` -- `PIT_TARGET_FREQ = 100` (Hz); `pit_get_ticks()` for raw tick counter
- `include/kernel/timer.h` -- `system_get_ticks()` for monotonic counter used in delta_ms calculation
- `include/desktop/wm.h` -- `wm_composite()` (compositor entry point), `wm_mark_dirty()`, `wm_move_window()`, `wm_resize_window()`, `struct wm_window { int32_t x, y; uint32_t width, height; }` -- extended in §4 to hold `wm_anim_state_t`
- `include/registry.h` -- `RegGetValue()`, `HKCU` -- used in §5 to read `EnableAnimations` + `AnimationSpeed`
- `include/desktop/theme.h` (TODO-01) -- `theme_get()` must be available before §4 window transitions
- → XREF: `08-graphics-ui/TODO-03-theme-system.md` -- prerequisite; `theme_get()` must be live before animated windows can paint correctly
- Related (no stable XREF target): `09-desktop-shell/TODO-01-*` (Start Menu) -- depends on §3 animation manager being live; Start Menu slide-up uses `gfx_tween_start()`
- Related (no stable XREF target): `09-desktop-shell/TODO-02-*` (Notifications) -- slide-in notifications depend on §1 tween + §4 manager

## Outcome

- `include/kernel/gfx/gfx_animate.h` exports `gfx_tween_t`, `gfx_tween_start/update/value`, and all easing function pointers.
- `include/kernel/gfx/anim_mgr.h` exports `anim_mgr_add/cancel/tick`.
- `anim_mgr_tick(delta_ms)` called at the top of `wm_composite()` every frame; cap at 33 ms.
- Per-window `wm_anim_state_t` embedded in `struct wm_window`; window open/close/minimize/restore/maximize/snap play correct eased transitions.
- `EnableAnimations = 0` snaps all tweens to final value immediately.
- `spring_t` + `spring_update()` available for elastic scroll overshoot and window bounce.

## Implementation Order

| ⭐  | Order | Deliverable                                                                                           | Depends On                                                                | Status |
| --- | :---: | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Core tween engine -- `gfx_tween_t`, `tween_start/update/value`, 16.16 fixed-point                 | Nothing; standalone                                                       |  [ ]   |
| 💎  |   2   | §2 Easing functions -- LINEAR, IN/OUT/IN_OUT QUAD+CUBIC, BOUNCE, BACK                                | §1 (easing_fn pointer type defined in `gfx_animate.h`)                    |  [ ]   |
| 💎  |   3   | §3 Global animation manager -- 64-slot table, `add/cancel/tick`, compositor wiring, delta cap        | §1 + §2 (manages `gfx_tween_t*`, dispatches easing fns)                   |  [ ]   |
| 💎  |   4   | §5 Registry controls -- `EnableAnimations` + `AnimationSpeed` DWORDs, reduce-motion path            | §3 (`anim_mgr_add` must check flag; `AnimationSpeed` scales duration)      |  [ ]   |
| 💎  |   5   | §4 Window transition animations -- `wm_anim_state_t`, open/close/minimize/restore/maximize/snap/menu | §3 manager + §4 registry (speed multiplier needed before wiring transitions) |  [ ]   |
| ⭐  |   6   | §6 Spring physics -- `spring_t`, Hooke's law integer ODE, `spring_settled()`                         | §3 (springs registered with manager; settled check drives `wm_mark_dirty`) |  [ ]   |
| 💎  |   7   | §7 Compositor integration checklist -- dirty-frame gating, VSync delta cap, reduce-motion audit      | §5 + §6 (all animation types must be wired before integration audit)       |  [ ]   |

---

## 1. Core Tween Engine `[Sonnet]`

`gfx_tween_t` struct with from/to/current, duration_ms, elapsed_ms, easing_fn pointer, on_complete callback, active flag. `gfx_tween_start()` initializes; `gfx_tween_update(delta_ms)` advances and writes interpolated value to `current`; `gfx_tween_value()` returns `current`. All arithmetic in 16.16 fixed-point.

**Files:** `src/kernel/gfx/gfx_animate.c` (new), `include/kernel/gfx/gfx_animate.h` (new)

> [!NOTE]
> 16.16 fixed-point: `1 << 16 = 65536` represents 1.0. `t` ranges from `0` to `65536` over the tween duration. Interpolated value: `current = from + (int32_t)(((int64_t)(to - from) * eased_t) >> 16)`. Use `int64_t` for the multiply to avoid overflow when `(to - from)` can be up to screen resolution (e.g., 1920). Easing function signature: `typedef int32_t (*gfx_ease_fn)(int32_t t)` where `t` is 16.16 (0→65536) and the return is also 16.16 (may exceed 0–65536 for BACK/BOUNCE). `on_complete` is a `void (*)(void *userdata)` callback; call it once when `elapsed >= duration` then set `active = 0`. Allow `on_complete = NULL` (no callback).

- [ ] `typedef int32_t (*gfx_ease_fn)(int32_t t);` in `include/kernel/gfx/gfx_animate.h`
- [ ] `typedef struct { int32_t from, to, current; uint32_t duration_ms, elapsed_ms; gfx_ease_fn easing; void (*on_complete)(void *ud); void *userdata; uint8_t active; } gfx_tween_t;`
- [ ] `void gfx_tween_start(gfx_tween_t *tw, int32_t from, int32_t to, uint32_t duration_ms, gfx_ease_fn easing)` -- initialize all fields; `active = 1`; `elapsed = 0`; `current = from`
- [ ] `void gfx_tween_update(gfx_tween_t *tw, uint32_t delta_ms)` -- if `!tw->active`: return; advance `elapsed` by `delta_ms`; clamp to `duration`; compute `t = (elapsed << 16) / duration`; `eased = tw->easing(t)`; `current = from + (int32_t)(((int64_t)(to-from) * eased) >> 16)`; if `elapsed >= duration`: `current = to`; call `on_complete` if non-NULL; `active = 0`
- [ ] `int32_t gfx_tween_value(const gfx_tween_t *tw)` → `tw->current`
- [ ] `int gfx_tween_active(const gfx_tween_t *tw)` → `tw->active`
- [ ] Commit: `"gfx/anim: gfx_tween_t -- start/update/value, 16.16 fixed-point, on_complete callback"`

## 2. Easing Functions `[Sonnet]`

9 easing functions, all with signature `int32_t fn(int32_t t)` where `t` is 16.16 (0→65536). LINEAR, IN_QUAD/OUT_QUAD/IN_OUT_QUAD, IN_CUBIC/OUT_CUBIC/IN_OUT_CUBIC, BOUNCE, BACK. Exported as named function pointers.

**Files:** `src/kernel/gfx/gfx_animate.c` (extend), `include/kernel/gfx/gfx_animate.h` (extend)

> [!NOTE]
> All formulas use 16.16 fixed-point. Use `int64_t` for intermediate products. `t_norm = t` (0→65536 = 0.0→1.0 in 16.16). Standard formulas adapted to fixed-point:
> - **LINEAR**: return `t`
> - **IN_QUAD**: `(t * t) >> 16`
> - **OUT_QUAD**: `t2 = 65536 - t; 65536 - ((t2 * t2) >> 16)`
> - **IN_OUT_QUAD**: if `t < 32768`: `(2 * t * t) >> 16`; else: `t2 = t - 32768; 65536 - ((2 * (65536-t) * (65536-t)) >> 16)`
> - **IN_CUBIC**: `(t * t >> 16) * t >> 16`
> - **OUT_CUBIC**: `t2 = t - 65536; -(t2*t2>>16)*t2>>16 + 65536` (note: result must clamp to 65536)
> - **IN_OUT_CUBIC**: same pattern as IN_OUT_QUAD but cubed
> - **BOUNCE** (ease-out): multi-segment parabola approximation: 4 bounce regions, constants `a=0x1.5625 × 65536`, `b=0.75 × 65536` etc.; implement via if-else integer segments
> - **BACK** (slight overshoot): `s = 1.70158 × 65536`; `t2 = t - 65536`; standard back-ease formula mapped to fixed-point; output can exceed 65536 briefly (overshoot is intentional -- tween clamps final value to `to` at completion)

- [ ] `int32_t gfx_ease_linear(int32_t t)` -- identity
- [ ] `int32_t gfx_ease_in_quad(int32_t t)`, `gfx_ease_out_quad(int32_t t)`, `gfx_ease_in_out_quad(int32_t t)`
- [ ] `int32_t gfx_ease_in_cubic(int32_t t)`, `gfx_ease_out_cubic(int32_t t)`, `gfx_ease_in_out_cubic(int32_t t)`
- [ ] `int32_t gfx_ease_bounce(int32_t t)` -- ease-out bounce; 4 parabolic segments; no float
- [ ] `int32_t gfx_ease_back(int32_t t)` -- ease-out with `s = 1.70158`; integer overshoot (output may briefly exceed 65536); clamp applied by caller at completion
- [ ] Named constants in `gfx_animate.h`: `#define GFX_EASE_LINEAR gfx_ease_linear` (and similarly for all 9)
- [ ] Unit-testable: `gfx_ease_out_cubic(0) == 0`, `gfx_ease_out_cubic(65536) == 65536`, `gfx_ease_bounce(65536) == 65536`; log these in a `gfx_anim_self_test()` function called once at init
- [ ] Commit: `"gfx/anim: 9 easing functions -- quad/cubic/bounce/back in 16.16 fixed-point"`

## 3. Global Animation Manager `[Sonnet]`

Fixed table of 64 `gfx_tween_t*` pointers. `anim_mgr_add(tw)` registers; `anim_mgr_cancel(tw)` removes; `anim_mgr_tick(delta_ms)` advances all and auto-removes completed. Wired into `wm_composite()` as first call. delta_ms derived from `system_get_ticks()` diff, capped at 33 ms.

**Files:** `src/kernel/gfx/anim_mgr.c` (new), `include/kernel/gfx/anim_mgr.h` (new)

> [!NOTE]
> `delta_ms = (current_ticks - last_ticks) * 1000 / PIT_TARGET_FREQ` where `PIT_TARGET_FREQ = 100`. At 100 Hz each tick is 10 ms; typical frame at 60 FPS ≈ 16.7 ms ≈ 1–2 ticks. Cap: if `delta_ms > 33`: `delta_ms = 33` (prevents a ≥3-tick gap -- e.g., after a long kernel operation -- from teleporting animations). Store `last_ticks` as a `static uint64_t` in `anim_mgr.c`. The manager does not allocate: it stores raw pointers; ownership stays with the caller (usually a `wm_anim_state_t` field). If table is full and `anim_mgr_add()` is called: log warning and return -1; caller may snap to final value directly.

- [ ] `#define ANIM_MAX 64` in `include/kernel/gfx/anim_mgr.h`
- [ ] `static gfx_tween_t* anim_table[ANIM_MAX]` and `static int anim_count` in `anim_mgr.c`
- [ ] `static uint64_t anim_last_ticks` in `anim_mgr.c`
- [ ] `void anim_mgr_init(void)` -- zero table; `anim_last_ticks = system_get_ticks()`; call `gfx_anim_self_test()`; called from `desktop_init()`
- [ ] `int anim_mgr_add(gfx_tween_t *tw)` → 0 or -1: scan for NULL slot; store pointer; increment count
- [ ] `void anim_mgr_cancel(gfx_tween_t *tw)` -- find and NULL the slot; `tw->active = 0`; decrement count
- [ ] `void anim_mgr_tick(void)` -- compute `delta_ms`; cap at 33; iterate table; call `gfx_tween_update(tw, delta_ms)` for each non-NULL active entry; if `!tw->active` after update: NULL the slot; decrement count
- [ ] `int anim_mgr_any_active(void)` → `anim_count > 0` -- used by compositor to decide if redraw is needed
- [ ] Wire in `wm_composite()`: call `anim_mgr_tick()` as the first statement; after tick: if `anim_mgr_any_active()`: `wm_mark_dirty()`
- [ ] Log: `[anim] init; self-test passed` at startup; `[anim] table full -- drop tween` if add fails
- [ ] Commit: `"gfx/anim: anim_mgr -- 64-slot table, tick/add/cancel, wired into wm_composite()"`

## 4. Registry Controls `[Sonnet]`

`HKCU\Software\Impossible\Theme\EnableAnimations` DWORD (default 1). `AnimationSpeed` DWORD (default 100 = 1×, range 50–200). When `EnableAnimations = 0`: `anim_mgr_add()` snaps tween to final value immediately instead of registering it. `AnimationSpeed` scales the `duration_ms` passed to `gfx_tween_start()`.

**Files:** `src/kernel/gfx/anim_mgr.c` (extend), `include/kernel/gfx/anim_mgr.h` (extend)

> [!NOTE]
> Read both values once in `anim_mgr_init()` and cache in `static int g_anim_enabled` and `static uint32_t g_anim_speed`. Scaled duration: `effective_duration = (duration_ms * g_anim_speed) / 100`; clamp `g_anim_speed` to `[50, 200]`. Snap path: if `!g_anim_enabled`: in `anim_mgr_add()`, immediately set `tw->current = tw->to; tw->active = 0`; call `tw->on_complete` if set; return 0 (don't add to table). Re-read settings on `WM_THEME_CHANGED` (TODO-01 §8) so a settings change takes effect immediately without restart.

- [ ] `static int g_anim_enabled = 1` and `static uint32_t g_anim_speed = 100` in `anim_mgr.c`
- [ ] `void anim_mgr_load_settings(void)`: `RegGetValue(HKCU, key, "EnableAnimations", RRF_RT_DWORD, …)` → `g_anim_enabled`; same for `AnimationSpeed`; clamp speed to [50, 200]; fallback to defaults on error
- [ ] Call `anim_mgr_load_settings()` from `anim_mgr_init()` and on `WM_THEME_CHANGED`
- [ ] `anim_mgr_add()`: if `!g_anim_enabled`: snap + return without table insert
- [ ] `gfx_tween_start()`: if `g_anim_speed != 100`: apply speed multiplier to `duration_ms` before storing
- [ ] `int anim_mgr_enabled(void)` → `g_anim_enabled` -- for callers that want to skip tween setup entirely
- [ ] Log: `[anim] settings: enabled=%d speed=%u%%`
- [ ] Commit: `"gfx/anim: registry controls -- EnableAnimations + AnimationSpeed, snap reduce-motion path"`

## 5. Window Transition Animations `[Opus]`

Per-window `wm_anim_state_t` with tweens for x, y, w, h, opacity, scale (in 16.16 percent, 65536=100%). Open: scale 90→100% + opacity 0→255 (200 ms ease-out-cubic). Close: scale 100→90% + opacity 255→0 (150 ms ease-in-quad). Minimize/restore: slide to/from taskbar button rect (250 ms). Maximize/snap: expand/slide+resize (200 ms). Menu popup: scale-Y 0→100% (150 ms ease-out-quad).

**Files:** `src/desktop/wm_anim.c` (new), `include/desktop/wm_anim.h` (new), `include/desktop/wm.h` (extend)

> [!NOTE]
> This is `[Opus]` -- window animations are novel for Impossible OS: they require the compositor to render windows at interpolated geometry and opacity each frame, which is not currently implemented. **Scale rendering**: the compositor must read `win->anim.scale` (16.16 percent) and call the blitter with a scaled source; when `scale == 65536` (100%): use the normal blit path. **Opacity**: `win->anim.opacity` (0–255); when 255: normal blit; otherwise: blend with alpha `opacity/255`. **Minimize target**: the taskbar button rect for this window must be known at minimize time; store as `wm_anim_state_t.taskbar_rect`; if unknown (taskbar not yet implemented): animate to screen bottom-center. **Close animation**: when the close tween completes: the `on_complete` callback calls `wm_destroy_window(handle)`. **Compositor changes**: in `wm_composite()`, for each window: if `win->anim.scale != 65536`: render scaled; if `win->anim.opacity != 255`: render with alpha blend. Both branches fall back to fast path when at nominal values.

- [ ] `typedef struct { gfx_tween_t x, y, w, h; gfx_tween_t opacity; gfx_tween_t scale; int32_t taskbar_rx, taskbar_ry, taskbar_rw, taskbar_rh; uint8_t closing; } wm_anim_state_t;` in `include/desktop/wm_anim.h`
- [ ] Embed `wm_anim_state_t anim` field into `struct wm_window` in `include/desktop/wm.h`
- [ ] `void wm_anim_open(int handle)`: start `scale` tween 58982→65536 (90→100%), opacity 0→255; both 200 ms ease-out-cubic; register both in anim_mgr
- [ ] `void wm_anim_close(int handle)`: start `scale` 65536→58982, opacity 255→0; 150 ms ease-in-quad; `anim.closing = 1`; `on_complete` → `wm_destroy_window(handle)`
- [ ] `void wm_anim_minimize(int handle, int32_t tx, int32_t ty, uint32_t tw, uint32_t th)`: tween x/y/w/h toward taskbar rect; 250 ms ease-in-quad; `on_complete` → hide window (`WM_FLAG_VISIBLE = 0`)
- [ ] `void wm_anim_restore(int handle)`: store pre-minimize position; tween from taskbar rect back to stored geometry; 250 ms ease-out-cubic; set visible first
- [ ] `void wm_anim_maximize(int handle)`: tween x/y/w/h to `{0, 0, screen_w, screen_h}`; 200 ms ease-out-cubic
- [ ] `void wm_anim_snap(int handle, int side)`: `side=LEFT`: tween to `{0, 0, screen_w/2, screen_h}` 200 ms; `side=RIGHT`: `{screen_w/2, 0, screen_w/2, screen_h}`
- [ ] `void wm_anim_menu_open(int handle)`: tween `scale` in Y axis 0→65536; 150 ms ease-out-quad (scale-Y stub: use `h` tween from 0 to natural height; compositor clips to current `h.current`)
- [ ] Compositor in `wm.c`: after `anim_mgr_tick()`: for each window: apply `win->anim.x.current`, `y.current`, `w.current`, `h.current`; if `anim.scale.active || anim.opacity.active`: use alpha-blend blit path; else: fast blit
- [ ] Call `wm_anim_open()` from `wm_create_window()` (after buffer allocation, before first composite)
- [ ] Commit: `"desktop/wm_anim: window open/close/minimize/restore/maximize/snap/menu animations"`

## 6. Spring Physics `[Opus]`

`spring_t` struct with position, velocity, target, stiffness (k), damping (d). `spring_update(sp, delta_ms)` applies Hooke's law as an integer Euler step. `spring_settled(sp)` returns true when velocity and displacement are below threshold. Applied to window bounce on restore and elastic scroll overshoot.

**Files:** `src/kernel/gfx/gfx_spring.c` (new), `include/kernel/gfx/gfx_spring.h` (new)

> [!NOTE]
> This is `[Opus]` -- integer spring ODE requires careful scaling to avoid overflow and instability. All values in 16.16 fixed-point. Hooke's law Euler step: `force = -k × (pos - target) - d × velocity` (all 16.16); `velocity += force × delta_ms / 1000`; `pos += velocity × delta_ms / 1000`; use `int64_t` intermediates for multiply. Stability constraint: for `k=65536` (1.0) and `d=98304` (1.5): the system is critically damped; default recommended values. Delta_ms cap: use 16 ms max in spring step to prevent instability (subcycle: if `delta_ms > 16`: split into 16 ms substeps). `spring_settled`: `|velocity| < 128` (< 0.002 in 16.16) AND `|pos - target| < 256` (< 0.004 px in 16.16). Spring is not managed by `anim_mgr` (different update loop -- call `spring_update()` directly from the owning subsystem); however, owning code must call `wm_mark_dirty()` while `!spring_settled()`.

- [ ] `typedef struct { int32_t pos, vel, target, k, d; } spring_t;` in `include/kernel/gfx/gfx_spring.h`
- [ ] `void spring_init(spring_t *sp, int32_t initial_pos, int32_t target, int32_t k, int32_t d)` -- default k=65536, d=98304 (critically damped in 16.16)
- [ ] `void spring_update(spring_t *sp, uint32_t delta_ms)` -- if `delta_ms > 16`: iterate in 16 ms substeps; each step: `force = -(sp->k * (sp->pos - sp->target) >> 16) - (sp->d * sp->vel >> 16)`; `sp->vel += force × 16 >> 10`; `sp->pos += sp->vel × 16 >> 10` (division by 1000 approximated as `>> 10` for 16 ms: `16/1024 ≈ 0.0156 ≈ 16/1000`; document approximation)
- [ ] `int spring_settled(const spring_t *sp)` → `abs(sp->vel) < 128 && abs(sp->pos - sp->target) < 256`
- [ ] `void spring_set_target(spring_t *sp, int32_t new_target)` -- change target mid-flight without resetting velocity (natural direction change)
- [ ] Apply to: scroll overshoot in scroll view (future `controls.c` scrollbar); window restore bounce: after `wm_anim_restore()` completes, run a spring with k=131072 (2.0), d=65536 (1.0) for a subtle bounce overshoot on y
- [ ] Commit: `"gfx/spring: integer Hooke's law spring -- critically-damped default, spring_settled, substep"`

## 7. Compositor Integration `[Sonnet]`

Non-animating frames skip compositor redraw when no dirty rects. Animating windows force redraw each frame via `anim_mgr_any_active()`. VSync-aware delta capping (33 ms). `EnableAnimations=0` reduce-motion audit confirming instant transitions everywhere.

**Files:** `src/desktop/wm.c` (extend)

> [!NOTE]
> The existing dirty rect system (`wm_mark_dirty()` + dirty flag in compositor) already gates redraws. The integration requirement is: when `anim_mgr_any_active()` returns true, call `wm_mark_dirty()` before the dirty-rect check so the compositor does not skip animating frames. This is already specified in §3; this section is the integration audit and any remaining wiring. VSync delta cap: `delta_ms = min(delta_ms, 33)` in `anim_mgr_tick()` (already in §3 design). Reduce-motion audit: grep for every `wm_anim_*` and `anim_mgr_add()` call site and confirm each goes through the `g_anim_enabled` snap path -- no animation runs when reduce-motion is active.

- [ ] Confirm `anim_mgr_tick()` is the first call in `wm_composite()` -- before dirty rect check
- [ ] `if (anim_mgr_any_active()) wm_mark_dirty();` placed immediately after `anim_mgr_tick()` in `wm_composite()`
- [ ] Delta cap: `if (delta_ms > 33) delta_ms = 33;` in `anim_mgr_tick()` (verify present from §3)
- [ ] Reduce-motion audit: all `wm_anim_open/close/minimize/restore/maximize/snap/menu_open` call `anim_mgr_add()` which checks `g_anim_enabled`; no animation code bypasses this check
- [ ] Non-animating idle: `wm_composite()` early-returns if `!dirty && !anim_mgr_any_active()` -- confirm this path works correctly with QEMU (idle desktop should show near-zero CPU in `top`)
- [ ] Boot log: `[wm] compositor: animation tick integrated, reduce-motion=%s` at startup
- [ ] Commit: `"desktop/wm: compositor integration -- anim tick, dirty gating, reduce-motion audit"`

---

## OS Comparison


| ⭐  | Feature                                | 🪟 Win11                                                              | 🐧 Linux                                                                         | 🚀 Impossible OS                                                                       |
| --- | -------------------------------------- | --------------------------------------------------------------------- | -------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------- |
| 💎  | Core tween engine                      | ✅ DWM storyboard animations; WinUI3 `AnimationInterpolation`         | ✅ GTK `GskRenderNode` + CSS transitions;                                        | ⬜ §1 -- 16.16 fixed-point; no float; `int64_t`                                        |
| 💎  | Easing functions                       | ✅ WinUI3 `EasingFunctionBase` hierarchy; bounce/back in              | ✅ GTK CSS `cubic-bezier`; GNOME Shell                                           | ⬜ §2 -- 9 functions; all integer; `gfx_ease_bounce`                                   |
| 💎  | Global animation manager               | ✅ DWM internal scheduler; WinUI3 `CompositionAnimationGroup`         | ✅ Mutter animation scheduler; GNOME Shell                                       | ⬜ §3 -- in-kernel 64-slot table; zero GObject/JS                                      |
| 💎  | Window transitions                     | ✅ DWM animates all window state                                      | ✅ Mutter/KWin window animation plugin system                                    | ⬜ §5 -- `wm_anim_open/close/minimize/restore` with correct easing per                 |
| 💎  | Reduce-motion / Registry speed control | ✅ Settings → Accessibility → Visual                                  | ✅ GNOME `org.gnome.desktop.interface.enable-animations`; KDE disable animations | ⬜ §4 -- `HKCU\Software\Impossible\Theme\EnableAnimations + AnimationSpeed`; snap path |
| ⭐  | Spring physics                         | ⚠️ WinUI3 `SpringVector3NaturalMotionAnimation` (high-level only; DWM | ❌ Mutter/GTK use easing curves only;                                            | ⬜ §6 -- `⭐` kernel-level integer spring; `spring_set_target()`                       |
| 💎  | Compositor dirty-frame gating          | ✅ DWM skips redraws on idle;                                         | ✅ Mutter/KWin damage tracking                                                   | ⬜ §7 -- `anim_mgr_any_active()` drives `wm_mark_dirty()`; idle desktop                |

> **After §1–§7:** Impossible OS has a fully kernel-native animation engine -- no GObject, no JS engine, no D3D dependency. The `⭐` spring physics differentiator goes beyond WinUI3's high-level spring API: the kernel-level `spring_set_target()` allows mid-flight direction changes with preserved velocity, which means dragging a window and releasing it snaps back with physically correct momentum rather than resetting. GTK/Mutter have no built-in spring physics at all.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] `gfx_anim_self_test()` log at boot: `[anim] init; self-test passed`
- [ ] Easing: `gfx_ease_linear(32768) == 32768`; `gfx_ease_in_quad(32768) == 16384`; `gfx_ease_out_cubic(65536) == 65536`; `gfx_ease_bounce(65536) == 65536` -- verify via serial log
- [ ] `anim_mgr_tick()` called first in `wm_composite()`; serial log confirms on first compositor frame
- [ ] Window open animation visible in QEMU: new window scales 90→100% + fades in over ~200 ms
- [ ] Window close animation: scales down + fades out before buffer freed; window destroyed in `on_complete`
- [ ] Minimize/restore: window slides to/from bottom of screen (taskbar area) with correct easing
- [ ] `EnableAnimations = 0` in Registry: window open/close snap instantly; zero tweens registered in `anim_mgr`
- [ ] `AnimationSpeed = 200`: all animations run at 2× duration (200 ms open → 400 ms); `AnimationSpeed = 50`: half duration
- [ ] Spring: `spring_settled()` returns true after ~500 ms of decay from `pos=65536*10, target=0, k=65536, d=98304`; verified via serial log in a test init call
- [ ] Idle CPU: desktop with no open windows and no animations running → compositor early-returns dirty check; CPU usage near baseline
- [ ] Commit: `"gfx/anim: complete animation engine -- tween, easing, manager, window transitions, spring, compositor"`
