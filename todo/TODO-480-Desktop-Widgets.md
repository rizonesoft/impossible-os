# Desktop Widgets

> **Goal:** Floating panels above wallpaper with real-time info widgets.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---


- [ ] Create `src/desktop/widgets.c`
- [ ] Widget API: `widget_fn(msg, surface, ctx)` — WGT_INIT/RENDER/TICK/CLOSE
- [ ] Widget manager: load, position, update
- [ ] Draggable positioning, semi-transparent background
- [ ] Built-in: Clock, CPU Meter, RAM Monitor, Calendar, Quick Notes
- [ ] Commit: `"desktop: widget framework + built-in widgets"`

---

## 21. Display & Resolution

