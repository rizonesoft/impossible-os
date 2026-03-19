# Display & Resolution

> **Goal:** VESA/VBE mode enumeration, resolution switching, and multi-monitor.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB. `kmalloc` is ONLY for small kernel structs (≤ 4 KB).

---
- [ ] `display_enum_modes()` — query VESA/VBE modes
- [ ] `display_get_mode()` — return current resolution
- [ ] *(Stretch)* `display_set_mode(w, h)` — runtime change (requires virtio-gpu)
- [ ] Commit: `"display: resolution management"`

### 21.2 Multi-Monitor (Stretch)

- [ ] *(Stretch)* `struct monitor` (id, resolution, position, DPI, framebuffer)
- [ ] *(Stretch)* Virtual desktop coordinate space

---

## 22. Software OpenGL

- [ ] Port TinyGL (~5000 lines, Zlib license) to Impossible OS framebuffer
- [ ] Basic OpenGL 1.1: `glBegin/glEnd`, vertices, colors, textures, z-buffer
- [ ] Test: rotating cube
- [ ] Commit: `"gfx: TinyGL software OpenGL 1.1"`
