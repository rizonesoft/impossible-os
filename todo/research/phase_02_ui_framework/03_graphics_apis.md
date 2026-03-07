# Graphics APIs — Research

## Overview

Graphics APIs provide a standard interface between applications and the GPU/framebuffer. For Impossible OS, we need to understand what's available, what's open source, and what we can realistically implement.

---

## DirectX

| Aspect | Details |
|--------|---------|
| **Developer** | Microsoft |
| **License** | ❌ **Proprietary** — cannot be redistributed or ported |
| **Components** | Direct3D (3D), Direct2D (2D), DirectWrite (text), DirectSound (audio), DirectInput |
| **Platform** | Windows only (by design) |
| **API style** | COM-based (Component Object Model) |

### Can we use it?

**No.** DirectX is proprietary, Windows-exclusive, and deeply tied to the Windows kernel (`dxgkrnl.sys`). However, there are open-source alternatives:

### Open-Source DirectX Alternatives

| Project | What it does | License | Redistributable |
|---------|-------------|---------|-----------------|
| **DXVK** | Translates Direct3D 9/10/11 → Vulkan | Zlib | ✅ Yes |
| **VKD3D-Proton** | Translates Direct3D 12 → Vulkan | LGPL 2.1 | ✅ Yes |
| **WineD3D** | Translates Direct3D → OpenGL (part of Wine) | LGPL 2.1 | ✅ Yes |
| **Mesa/Gallium** | DirectX state tracker (D3D9) | MIT | ✅ Yes |

> [!NOTE]
> These all require a working Vulkan or OpenGL driver underneath. For Impossible OS, we'd need to implement an OpenGL or Vulkan driver first (see below), then layer DirectX translation on top.

---

## OpenGL / OpenGL ES

| Aspect | Details |
|--------|---------|
| **Developer** | Khronos Group |
| **License** | ✅ **Open standard** — specification is freely available |
| **Current version** | OpenGL 4.6 (desktop), OpenGL ES 3.2 (embedded) |
| **API style** | C function calls, state machine |

### Open-Source Implementations

| Project | What it is | License | Redistributable | Notes |
|---------|-----------|---------|-----------------|-------|
| **Mesa 3D** | Full OpenGL/Vulkan implementation | MIT | ✅ Yes | The gold standard. Used by Linux, Android, ChromeOS |
| **TinyGL** | Minimal software OpenGL 1.1 | BSD/Zlib | ✅ Yes | ≤5000 lines, perfect for hobby OS |
| **SwiftShader** | Google's software Vulkan/GL renderer | Apache 2.0 | ✅ Yes | CPU-only, no GPU needed |

### Recommended approach for Impossible OS

| Stage | What to implement | Effort |
|-------|-------------------|--------|
| **Stage 1** | **Software rasterizer** — basic triangle/line/pixel drawing API, no GPU | 2-4 weeks |
| **Stage 2** | **Port TinyGL** — gives us OpenGL 1.x with software rendering | 1-2 weeks |
| **Stage 3** | **Port Mesa softpipe** — Mesa's software renderer, OpenGL 3.3 | Months |
| **Stage 4** | **GPU driver** — virtio-gpu for QEMU, then real hardware | Months-Years |

### TinyGL Details

```
Repository: https://github.com/nickg/tinygl  (or many forks)
Size:       ~5000 lines of C
License:    Zlib/BSD (fully redistributable)
Features:   OpenGL 1.1 subset (glBegin/glEnd, textures, lighting, zbuffer)
Depends on: Nothing — pure software, needs only a framebuffer to write pixels to
Perfect for: Hobby OS with framebuffer but no GPU driver
```

> [!TIP]
> **TinyGL is the best starting point.** It's tiny, has no dependencies, and we already have a framebuffer (`fb_put_pixel`, `fb_fill_rect`). Porting it would give Impossible OS basic 3D rendering immediately.

---

## Vulkan

| Aspect | Details |
|--------|---------|
| **Developer** | Khronos Group |
| **License** | ✅ **Open standard** |
| **API style** | Explicit, low-level GPU control |

### Open-Source Implementations

| Project | License | Redistributable | Notes |
|---------|---------|-----------------|-------|
| **Mesa (Lavapipe)** | MIT | ✅ Yes | Software Vulkan — good for testing without GPU |
| **SwiftShader** | Apache 2.0 | ✅ Yes | Google's software Vulkan |
| **RADV** (Mesa) | MIT | ✅ Yes | AMD GPU Vulkan driver |
| **ANV** (Mesa) | MIT | ✅ Yes | Intel GPU Vulkan driver |

> [!IMPORTANT]
> Vulkan is more complex than OpenGL but is the future direction. For Impossible OS, start with OpenGL (TinyGL), add Vulkan later when GPU drivers exist.

---

## Feasibility Summary for Impossible OS

| API | Can we implement? | Best path | Timeline |
|-----|------------------|-----------|----------|
| Custom 2D API | ✅ Already have basics | Extend `fb_*` functions | Done |
| OpenGL 1.x (software) | ✅ Port TinyGL | ~5000 lines, Zlib license | 1-2 weeks |
| OpenGL 3.3 (software) | 🟡 Port Mesa softpipe | Large effort | Months |
| Vulkan (software) | 🟡 Port Lavapipe | Requires Mesa + LLVM | Months |
| DirectX 9 (via translation) | 🔴 Needs OpenGL/Vulkan first | DXVK or WineD3D | After GL |
| Native GPU driver | 🔴 Hard | virtio-gpu → real HW | Long-term |
