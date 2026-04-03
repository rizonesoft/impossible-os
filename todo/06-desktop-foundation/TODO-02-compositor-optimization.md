# TODO-02 -- Compositor Optimization

> **Goal:** Replace the full-screen redraw compositor with dirty-rect tracking so only changed regions are redrawn. Target: 60fps compositing on 1280x720, <5ms per frame on idle desktop (only clock ticks).

> [!IMPORTANT]
> The existing compositor (`wm_composite()`) redraws the entire screen every frame. Infrastructure for dirty rects exists (`drag_dirty_*` fields) but isn't used. This TODO makes the compositor production-grade.

## Inputs

- [`src/desktop/wm.c`](../../src/desktop/wm.c) -- `wm_composite()`, `needs_redraw` flag
- [`src/kernel/gfx/`](../../src/kernel/gfx/) -- framebuffer blit, double buffering

## Outcome

- Compositor tracks per-window and per-region dirty rects
- Only dirty regions are reblitted to the screen framebuffer
- Idle desktop (no window movement): <1ms per frame (only clock update)
- Window drag: only old and new positions redrawn
- Window content change: only that window's rect redrawn

## Implementation Order

| ⭐  | Order | Deliverable                               | Depends On | Status |
| --- | :---: | ----------------------------------------- | ---------- | :----: |
| 💎  |   1   | Dirty rect tracking infrastructure        | --          |  [ ]   |
| 💎  |   2   | Partial wallpaper restore                 | §1         |  [ ]   |
| 💎  |   3   | Per-window damage and compositor loop     | §1, §2     |  [ ]   |
| ⭐  |   4   | Frame timing and VSync                    | §3         |  [ ]   |

---

## 1. Dirty Rect Tracking Infrastructure
Add a dirty rect list that accumulates regions needing redraw each frame.

**Files:** `src/desktop/wm.c`, `include/desktop/wm.h`

- [ ] `struct wm_dirty_rect { int16_t x, y, w, h; }` -- single dirty region
- [ ] `wm_dirty_rects[64]` -- per-frame dirty list (ring buffer)
- [ ] `wm_mark_dirty(x, y, w, h)` -- add region to dirty list
- [ ] `wm_dirty_merge()` -- merge overlapping rects to reduce overdraw
- [ ] Window move: mark old rect + new rect as dirty
- [ ] Window resize: mark old rect + new rect
- [ ] Window create/destroy: mark window rect
- [ ] Commit

**Test checkpoint:** Add logging: `compositor: N dirty rects this frame`. Idle desktop should show 0-1 rects (clock only). Dragging a window should show 2 rects.

## 2. Partial Wallpaper Restore
Redraw only the wallpaper pixels under dirty rects, not the entire screen.

**Files:** `src/desktop/desktop.c`, `src/desktop/wm.c`

- [ ] `desktop_draw_wallpaper_rect(x, y, w, h)` already exists -- verify it works for partial restore
- [ ] For each dirty rect: blit wallpaper region first, then overlay windows
- [ ] Handle rects that span taskbar area (48px bottom)
- [ ] Commit

**Test checkpoint:** Move a window -- wallpaper behind old position is restored without full-screen redraw.

## 3. Per-Window Damage and Compositor Loop
Replace full-screen `wm_composite()` with dirty-rect-driven compositing.

**Files:** `src/desktop/wm.c`

- [ ] For each dirty rect: restore wallpaper → draw intersecting windows (z-order) → draw taskbar if intersecting → draw start menu if intersecting
- [ ] Per-window `content_dirty` flag: set when app writes to window framebuffer
- [ ] Window content change only marks that window's screen rect as dirty
- [ ] Cursor: track old/new cursor position, mark both as dirty
- [ ] Commit

**Test checkpoint:** Idle desktop with clock: compositor redraws only the clock region (~60x20px). Open terminal, type -- only terminal rect redrawn.

## 4. Frame Timing and VSync
Measure frame times and optionally sync to display refresh.

**Files:** `src/desktop/wm.c`

- [ ] Frame time measurement: TSC delta between composite calls
- [ ] Log: `compositor: avg Xms/frame (Y fps)` on debug builds
- [ ] VSync: if Bochs VGA, wait for vblank before page flip
- [ ] Frame budget: skip composite if <16ms since last frame (60fps cap)
- [ ] Commit

**Test checkpoint:** Serial log shows frame timing. Idle: <1ms. Drag: <5ms. No visual tearing.

---

## OS Comparison

| ⭐ | Feature            | 🪟 Win11        | 🐧 Linux (Wayland)  | 🚀 Impossible OS        |
|----|--------------------|--------------|------------------|----------------------|
| 💎 | Dirty rect compose | ✅ DWM       | ✅ Compositor    | ⬜ §1-§3             |
| ⭐ | VSync              | ✅ D3D       | ✅ DRM           | ⬜ §4 Bochs VGA      |
| ⭐ | <16ms frames       | ✅ GPU accel | ✅ GPU accel     | ⬜ §3 CPU compositor |
