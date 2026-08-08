---
schema_version: 1
id: gpu-display-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-17 -- GPU & Display Drivers"
---

# TODO-17 -- GPU & Display Drivers

> **Goal:** Deliver modesetting, 2D acceleration, and hardware cursor for all primary development targets (VMSVGA, VirtIO-GPU, Bochs/BGA) as loadable modules behind a clean `display_device_t` vtable, ensure the existing VBE/GOP framebuffer registers as a fallback, and add P3/P4 modesetting stubs for Intel HD/UHD iGPU and AMD APU Vega/RDNA to position the OS for real-hardware display support.

> [!IMPORTANT]
> **Already complete:** VBE/GOP linear framebuffer (`src/kernel/drivers/framebuffer.c`) -- pixels write to screen; VBE DISPI registers are already in use for mode-setting. This TODO extracts that code into a proper `display_device_t` backend, adds three accelerated GPU modules, and defines the vtable abstraction so the compositor (`src/desktop/`) calls `display_flush_rect()` instead of writing directly to a raw framebuffer pointer.

## Inputs

- [`src/kernel/drivers/framebuffer.c`](../../src/kernel/drivers/framebuffer.c) -- existing VBE DISPI + GOP framebuffer (extract Bochs/BGA logic into §3, register as §2 fallback)
- [`src/kernel/gfx/gfx_core.c`](../../src/kernel/gfx/gfx_core.c) -- compositor/rendering layer that will call `display_flush_rect()` after §1 lands
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- kernel module loader required for §3–§5 loadable modules
- → XREF: `04-drivers-hardware/TODO-09-hypervisor-abstraction.md §8` -- VirtIO GPU (§4 here) shares VirtIO transport; §1 `display_device_t` vtable is the same interface `hv_ops.display_resize` calls into
- → XREF: `07-graphics-ui` domain -- compositor calls `display_flush_rect(x,y,w,h)` and `display_set_cursor(image,hotspot)` / `display_move_cursor(x,y)` from the `display_device_t` vtable defined in §1

## Outcome

- VMSVGA 2D module: FIFO acceleration (`RECT_FILL`, `RECT_COPY`, `UPDATE`), hardware cursor, VirtualBox default display.
- VirtIO-GPU module: scanout resource page-flip pipeline, hardware cursor via cursorq, QEMU `-device virtio-vga`.
- Bochs/BGA module: VBE DISPI `0x01CE`/`0x01CF` mode-set + LFB + Y-offset page-flip; QEMU `-device bochs-display`.
- `display_device_t` vtable: single interface for all GPU backends; compositor picks best available at boot.
- VBE/GOP framebuffer registered as lowest-priority fallback so bare-metal and real hardware still reach the desktop.
- Intel iGPU (Gen9-12) and AMD APU (Vega/RDNA) modesetting stubs in place for future P3/P4 work.
- Multi-head support: `display_device_t` array; compositor spans monitors; `NtQueryDisplayConfig`/`NtSetDisplayConfig` stubs.

## Implementation Order

| ⭐  | Order | Deliverable                                                        | Depends On                              | Status |
| --- | :---: | ------------------------------------------------------------------ | --------------------------------------- | :----: |
| ⭐  |   1   | §1 `display_device_t` vtable + compositor hook                     | `gfx_core.c` interface point            |  [ ]   |
| 💎  |   2   | §2 VBE/GOP fallback registers as lowest-priority display device    | §1 (vtable defined)                     |  [ ]   |
| 💎  |   3   | §3 Bochs/BGA module -- extract from `framebuffer.c`, add page-flip | §1, TODO-05 module loader               |  [ ]   |
| 💎  |   4   | §4 VirtIO-GPU module -- resource pipeline, hardware cursor         | §1, TODO-05, VirtIO core                |  [ ]   |
| 💎  |   5   | §5 VMSVGA 2D module -- FIFO acceleration, hardware cursor          | §1, TODO-05                             |  [ ]   |
| 💎  |   6   | §6 Multi-head support -- `display_device_t[]`, compositor span     | §1 (vtable), compositor (§1 integrated) |  [ ]   |
| 💎  |   7   | §7 Intel HD/UHD iGPU modesetting stub (P3)                         | §1                                      |  [ ]   |
| 💎  |   8   | §8 AMD APU Vega/RDNA modesetting stub (P4)                         | §1                                      |  [ ]   |

> §1 `display_device_t` is `⭐` exclusive: Windows uses WDDM kernel-mode drivers with a fixed DDI; Linux uses DRM/KMS. Impossible OS defines a lean custom vtable that any module can register against -- simpler than DRM/KMS, more capable than a raw framebuffer pointer.

---

## 1. `display_device_t` Vtable + Compositor Hook `[Opus]`

Define the `display_device_t` abstraction. All GPU modules register against it; the compositor calls through it. Eliminates raw framebuffer pointer access from `src/desktop/` and `src/kernel/gfx/`.

**Files:** `include/kernel/drivers/display_device.h` (new), `src/kernel/drivers/display_device.c` (new), `src/kernel/gfx/gfx_core.c` (update)

> [!IMPORTANT]
> This is the architectural pivot point for all display work. It must land first (order 1 in the implementation table). After this section, no code outside `src/modules/*/` and `src/kernel/drivers/framebuffer.c` should access `g_fb.base` directly -- all pixel flushes go through `display_flush_rect()`.

- [ ] Define:
  ```c
  typedef struct {
      int     priority;
      int   (*set_mode)(uint32_t w, uint32_t h, uint32_t bpp);
      void  (*flush_rect)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
      void  (*set_cursor)(const uint32_t *argb64, uint32_t hotspot_x, uint32_t hotspot_y);
      void  (*move_cursor)(uint32_t x, uint32_t y);
      int   (*page_flip)(uint8_t page);
      void  (*get_info)(uint32_t *w, uint32_t *h, uint32_t *bpp, void **fb_base);
  } display_device_t;
  ```
- [ ] `display_register(display_device_t *dev)` -- add to sorted list by `priority`; call `dev->set_mode(g_fb.w, g_fb.h, 32)` to activate
- [ ] `display_get_active()` -- return highest-priority registered device
- [ ] `DISPLAY_PRIORITY_ACCEL = 100` (VMSVGA, VirtIO-GPU), `DISPLAY_PRIORITY_STANDARD = 50` (Bochs/BGA), `DISPLAY_PRIORITY_FALLBACK = 1` (VBE/GOP)
- [ ] Update `gfx_core.c`: replace `memcpy_to_fb(dst, src, len)` / `fb_swap()` with calls to `display_flush_rect(0, 0, w, h)` via `display_get_active()`
- [ ] Update compositor dirty-region tracker to call `display_flush_rect(dirty.x, dirty.y, dirty.w, dirty.h)` on each frame
- [ ] Commit: `"kernel: display_device_t vtable -- register/get_active, priority system, gfx_core hook"`

## 2. VBE/GOP Fallback Display Device `[Sonnet]`

Wrap the existing VBE/GOP linear framebuffer in a `display_device_t` at lowest priority. Ensures bare-metal systems and hypervisors without a supported GPU module still reach the desktop unchanged.

**Files:** `src/kernel/drivers/framebuffer.c` (update)

> [!NOTE]
> After §3 extracts the Bochs/BGA DISPI code, `framebuffer.c` retains only the GOP linear framebuffer setup (boot-time mode already set by UEFI) and becomes the `DISPLAY_PRIORITY_FALLBACK` backend. No mode-set calls needed -- GOP already configured the resolution at boot.

- [ ] Implement `display_device_t vbe_gop_display`: `set_mode` = no-op (GOP mode fixed at boot); `flush_rect` = no-op (pixels already in LFB; no presentation needed for direct-write framebuffer); `page_flip` = no-op; `get_info` = returns `g_fb.*`
- [ ] Call `display_register(&vbe_gop_display)` from `framebuffer_init()` after `g_fb` is populated
- [ ] Verify compositor still reaches desktop on bare-metal after §1 switch -- `g_fb.base` pointer is still valid via `get_info()`
- [ ] Boot log: `[DISPLAY] VBE/GOP fallback registered (%ux%u)`
- [ ] Commit: `"drivers: VBE/GOP display_device_t fallback -- lowest-priority registration, bare-metal safe"`

## 3. Bochs/BGA Display Module `[Sonnet]`

Extract the existing VBE DISPI register code from `framebuffer.c` into a standalone loadable module. Add Y_OFFSET double-buffering page flip. Register as a `display_device_t` backend.

**Files:** `src/modules/bochs_display/bochs_display.c` (new), `include/kernel/drivers/bochs_display.h` (new), `src/kernel/drivers/framebuffer.c` (prune VBE DISPI code after extraction)

> [!NOTE]
> VBE DISPI I/O ports: `VBE_DISPI_IOPORT_INDEX = 0x01CE`, `VBE_DISPI_IOPORT_DATA = 0x01CF`. Registers: `XRES (1)`, `YRES (2)`, `BPP (3)`, `ENABLE (4)`, `BANK (5)`, `VIRT_WIDTH (6)`, `VIRT_HEIGHT (7)`, `X_OFFSET (8)`, `Y_OFFSET (9)`. QEMU device IDs: `{ 0x1234, 0x1111 }` (Bochs), `{ 0x1B36, 0x0100 }` (QEMU stdvga).

- [ ] PCI match: `{ 0x1234, 0x1111 }` (Bochs/QEMU stdvga), `{ 0x1B36, 0x0100 }` (QEMU VGA)
- [ ] Extract `vbe_set_mode(w, h, bpp)` from `framebuffer.c`: write `XRES`, `YRES`, `BPP`, `ENABLE=1`; map LFB from BAR0
- [ ] `VIRT_HEIGHT = h * 2`: allocate double-height virtual framebuffer (two page buffers stacked vertically)
- [ ] `display_page_flip(back_page)`: write `Y_OFFSET = h` (display second half) or `Y_OFFSET = 0` (display first half); compositor alternates pages for tear-free updates
- [ ] `display_flush_rect(x,y,w,h)`: for non-page-flip path (single buffer), no-op (pixels are already in LFB); update `dirty_rect` for callers that want it
- [ ] Register `display_device_t bochs_display`; priority = `DISPLAY_PRIORITY_STANDARD`
- [ ] Commit: `"modules: Bochs/BGA display -- VBE DISPI extract, Y_OFFSET page-flip, display_device_t"`

## 4. VirtIO-GPU Display Module `[Opus]`

Restore and re-land the VirtIO-GPU driver as a proper loadable `.kmod`. Control queue for resource lifecycle and scanout; cursor queue for hardware cursor updates. Replace the current `fb_swap()` call with a scanout resource flush.

**Files:** `src/modules/virtio_gpu/virtio_gpu.c` (new), `include/kernel/drivers/virtio_gpu.h` (new)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-09-hypervisor-abstraction.md §8` -- §8 there covers VirtIO GPU as the QEMU hypervisor backend. This section is the standalone `.kmod` version of the same driver. The two sections share the same PCI ID and protocol; whichever ships first should be treated as the reference implementation -- consolidate if both are being worked simultaneously.

- [ ] PCI match: `{ 0x1AF4, 0x1050 }` (VirtIO GPU); initialize controlq (VQ0) and cursorq (VQ1) via `virtio.c`
- [ ] `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` at init: read `rect.width/height` for scanout 0; call `wm_display_resized(w,h)`
- [ ] Resource lifecycle: `RESOURCE_CREATE_2D` (format `BGRA8888`, resource_id=1); `RESOURCE_ATTACH_BACKING` (physical pages of kernel framebuffer)
- [ ] `SET_SCANOUT`: bind resource 1 to scanout 0
- [ ] Page flip / flush: `display_flush_rect(x,y,w,h)` issues `TRANSFER_TO_HOST_2D` (dirty region) then `RESOURCE_FLUSH` (present to screen)
- [ ] Hardware cursor (cursorq): `UPDATE_CURSOR` with 64×64 ARGB image at init; `MOVE_CURSOR` on every pointer movement tick
- [ ] Handle `VIRTIO_GPU_EVENT_DISPLAY` (events_read bit 0): re-issue `GET_DISPLAY_INFO`, recreate resource at new size, notify WM
- [ ] Register `display_device_t virtio_gpu_display`; priority = `DISPLAY_PRIORITY_ACCEL`
- [ ] Boot log: `[virtio-gpu] Scanout %ux%u, resource_id=1`
- [ ] Commit: `"modules: VirtIO-GPU -- scanout resource pipeline, cursorq, display event resize, flush"`

## 5. VMSVGA 2D Display Module `[Opus]`

Port the VMware SVGA II driver (SerenityOS BSD-2 reference) as a loadable `.kmod`. Negotiate SVGA protocol version, set display mode, issue FIFO 2D acceleration commands (`RECT_FILL`, `RECT_COPY`, `UPDATE`), and enable hardware cursor via SVGA register writes.

**Files:** `src/modules/vmsvga/vmsvga.c` (new), `include/kernel/drivers/vmsvga.h` (new)

> [!NOTE]
> SVGA_INDEX_PORT = `0x3CE` (BAR0 I/O), SVGA_VALUE_PORT = `0x3CF`. BAR1 = framebuffer MMIO. BAR2 = FIFO MMIO. Version negotiation: write `SVGA_ID_2` to `SVGA_REG_ID`; read back and confirm. VirtualBox exposes this as `PCI {0x15AD, 0x0405}`.

- [ ] PCI match: `{ 0x15AD, 0x0405 }` (VMware SVGA II / VirtualBox VMSVGA)
- [ ] BAR0 I/O: `SVGA_INDEX_PORT = bar0`, `SVGA_VALUE_PORT = bar0+1`; `svga_write(reg, val)` / `svga_read(reg)`
- [ ] BAR1 MMIO map: framebuffer; BAR2 MMIO map: FIFO; sizes from `SVGA_REG_FB_SIZE` / `SVGA_REG_MEM_SIZE`
- [ ] Version negotiation: write `SVGA_ID_2 (0x90000002)` → `SVGA_REG_ID`; read back; verify match
- [ ] Mode set: `SVGA_REG_WIDTH`, `SVGA_REG_HEIGHT`, `SVGA_REG_BITS_PER_PIXEL`, `SVGA_REG_ENABLE=1`
- [ ] FIFO init: `FIFO_MIN`, `FIFO_MAX`, `FIFO_NEXT_CMD`, `FIFO_STOP` pointers; `SVGA_REG_CONFIG_DONE=1`
- [ ] `svga_fifo_write(cmd, ...)`: check `FIFO_NEXT_CMD + sizeof(cmd) < FIFO_MAX`; wrap; write words; advance `FIFO_NEXT_CMD`
- [ ] Implement `SVGA_CMD_RECT_FILL`, `SVGA_CMD_RECT_COPY`, `SVGA_CMD_UPDATE` (triggers screen refresh)
- [ ] Hardware cursor: `SVGA_REG_CURSOR_ID`, `SVGA_REG_CURSOR_X/Y`, `SVGA_REG_CURSOR_ON`; 32×32 AND+XOR masks in FIFO `DEFINE_CURSOR` command
- [ ] Register `display_device_t vmsvga_display` (§1 vtable); priority = `DISPLAY_PRIORITY_ACCEL`
- [ ] Add to `NOTICE.md`: SerenityOS `Kernel/Devices/GPU/VMWare/VMWareFramebufferDevice.cpp` (BSD-2)
- [ ] Boot log: `[VMSVGA] SVGA II %ux%u FIFO %u KiB`
- [ ] Commit: `"modules: VMSVGA -- FIFO RECT_FILL/COPY/UPDATE, hardware cursor, VirtualBox display"`

## 6. Multi-Head Support `[Opus]`

Extend `display_device_t` registration to a per-head array. Compositor spans or mirrors across multiple monitors. Add `NtQueryDisplayConfig`/`NtSetDisplayConfig` syscall stubs for Win32 compatibility.

**Files:** `src/kernel/drivers/display_device.c`, `include/kernel/drivers/display_device.h`, `src/desktop/` (compositor span)

> [!NOTE]
> → XREF: `07-graphics-ui` domain -- compositor layout across monitors is a window manager concern; this section provides the kernel API (`display_get_count()`, `display_get_head(n)`) and syscall stubs that the WM calls.

- [ ] `display_register_head(n, display_device_t *dev)` -- register device at head index `n` (0 = primary); replace single-device `g_display_active` with `g_display_heads[MAX_HEADS]`
- [ ] Boot-time head registration from `boot_info.gop_handles[]` (`gop_handle_count` entries, published by D01 T27 §4): register each `fb_valid` entry as a head, the `is_primary` one at head 0. -> XREF: `01-boot-platform/TODO-27-uefi-advanced.md §4`
- [ ] `display_get_count()` -- number of registered heads
- [ ] `display_get_head(n)` -- return device at head `n`; `NULL` if unpopulated
- [ ] VMSVGA / VirtIO-GPU: detect multi-scanout capability; register additional heads if device reports >1 scanout
- [ ] Compositor: query `display_get_count()`; if >1, allocate separate back-buffer per head; flush each head independently
- [ ] `NtQueryDisplayConfig(paths, modes)` stub: fills one `DISPLAYCONFIG_PATH_INFO` per head with width/height/refresh (60 Hz fixed); returns `STATUS_SUCCESS`
- [ ] `NtSetDisplayConfig(paths, modes)` stub: calls `display_get_head(n)->set_mode(w, h, bpp)` for changed heads; returns `STATUS_SUCCESS`
- [ ] Boot log: `[DISPLAY] %u head(s) registered`
- [ ] Commit: `"kernel: multi-head display -- display_get_head array, compositor span, NtQueryDisplayConfig stub"`

---

## 7. Intel HD/UHD iGPU Modesetting Stub (P3) `[Opus]`

Detect Intel Gen9–12 integrated GPUs by PCI device ID. Map MMIO. Read EDID via DDC. Set a display mode through the Display Engine (pipe, plane, transcoder, DPLL). Use Intel PRM open documentation exclusively -- no GPL driver code.

**Files:** `src/modules/intel_display/intel_display.c` (new), `include/kernel/drivers/intel_display.h` (new)

> [!IMPORTANT]
> **P3 stretch goal.** Intel PRMs are publicly available at [01.org/linuxgraphics](https://01.org/linuxgraphics). The implementation must be a clean-room reimplementation from the PRM -- no code from `drivers/gpu/drm/i915/` (GPL-2.0). This section is a modesetting stub: detect, EDID read, one mode-set -- no 3D, no power management, no display port training yet.

- [ ] PCI match table: common Gen9 (Skylake `0x1916/1912`), Gen10 (Kaby Lake `0x5916`), Gen11 (Ice Lake `0x8A51`), Gen12 (Tiger Lake `0x9A49`); device class `0x0300` (VGA) or `0x0380` (display)
- [ ] MMIO map BAR0 (GTTMMADR, 16 MiB); `intel_mmio_read/write32(offset)`
- [ ] EDID: implement DDC I2C using GMBUS registers (`GMBUS0–5`); `intel_gmbus_read_edid(port)` → 128-byte EDID; parse preferred mode from `Detailed Timing Descriptor 1`
- [ ] Mode-set sequence (abbreviated PRM path): configure DPLL via `DPLL_CTRL1`/`DPLL_CFGCR0/1`; configure transcoder timing (`TRANS_HTOTAL`, `TRANS_VTOTAL`, `TRANS_HBLANK`, etc.); configure plane (`PLANE_CTL`, `PLANE_SURF`, `PLANE_STRIDE`); enable pipe; enable port (`intel_dp_enable()` or `intel_hdmi_enable()` stub that writes PHY registers)
- [ ] Register `display_device_t intel_display`; priority = `DISPLAY_PRIORITY_ACCEL + 10` (prefer over VMSVGA/VirtIO if on real hardware)
- [ ] Boot log: `[intel-display] Gen%d iGPU PCI %04x, EDID preferred mode %ux%u`
- [ ] Commit: `"modules: Intel iGPU display stub -- PRM-based modesetting, GMBUS EDID, Gen9-12 (P3)"`

## 8. AMD APU Vega/RDNA Display Stub (P4) `[Opus]`

Detect AMD APU (Vega iGPU on Ryzen 2000–5000, RDNA on Ryzen 6000+) by PCI display device ID. Map MMIO. Set display mode via DCE/DCN display engine using AMD open register guide (GPUOpen). No 3D.

**Files:** `src/modules/amd_display/amd_display.c` (new), `include/kernel/drivers/amd_display.h` (new)

> [!IMPORTANT]
> **P4 stretch goal.** AMD publishes GPU register specifications on [GPUOpen](https://gpuopen.com/documentation/). Use the Vega (GFX9) and RDNA2 (GFX10.3) DCN register headers -- no code from `drivers/gpu/drm/amd/` (GPL/MIT). This section is a stub: PCI detection + one mode-set attempt via DCN.

- [ ] PCI match: Vega iGPU (`{ 0x1002, 0x687F }` Vega10, `{ 0x1002, 0x15D8 }` Picasso), RDNA APU (`{ 0x1002, 0x1638 }` Renoir, `{ 0x1002, 0x164C }` Rembrandt); device class `0x0300/0x0380`
- [ ] MMIO map BAR5 (MMIO registers, 512 KiB–2 MiB); `amd_reg_read/write32(reg_offset)`
- [ ] DCN display init: read `HUBBUB_SDPIF_FB_BASE` to confirm framebuffer address; configure one `OTG` (output timing generator), one `DPP` (display pixel pipe), one `OPTC` (output pixel timing controller)
- [ ] EDID via DDC using AUX or i2c channel: `amd_aux_read_edid(connector)` → preferred mode
- [ ] Mode-set: configure DCN pipe timing registers (`OTG_H_TOTAL`, `OTG_V_TOTAL`, `OTG_H_SYNC_*`, `OTG_V_SYNC_*`); configure plane address + pitch; enable OTG
- [ ] Register `display_device_t amd_display`; priority = `DISPLAY_PRIORITY_ACCEL + 10`
- [ ] Boot log: `[amd-display] Vega/RDNA APU PCI %04x, preferred mode %ux%u`
- [ ] Commit: `"modules: AMD APU display stub -- DCN mode-set, EDID, Vega/RDNA GPUOpen (P4)"`

## OS Comparison


| ⭐  | Feature                                                | 🪟 Win11                                                            | 🐧 Linux                                             | 🚀 Impossible OS                                                                 |
| --- | ------------------------------------------------------ | ------------------------------------------------------------------- | ---------------------------------------------------- | -------------------------------------------------------------------------------- |
| ⭐  | Unified `display_device_t` vtable for all GPU backends | ❌ WDDM DDI is ABI-stable but                                       | ❌ DRM/KMS has a similar role                        | ⬜ §1 -- lean 7-function vtable; modules register                                |
| 💎  | VMware/VMSVGA 2D FIFO acceleration + hardware cursor   | ✅ `vmswitch.sys` / `vm3dmp.sys` SVGA driver                        | ✅ `vmwgfx` DRM driver; FIFO commands;               | ⬜ §5 `RECT_FILL`/`RECT_COPY`/`UPDATE` FIFO, `DEFINE_CURSOR`, VirtualBox display |
| 💎  | VirtIO-GPU scanout pipeline + hardware cursor          | ✅ `viogpu.sys` WDDM miniport                                       | ✅ `virtio-gpu` DRM driver; scanout resources;       | ⬜ §4 -- RESOURCE_CREATE/SET_SCANOUT/FLUSH, cursorq, display event resize        |
| 💎  | Bochs/BGA VBE DISPI mode-set + Y_OFFSET page-flip      | ✅ `vgapnp.sys` VGA compatible; no Y_OFFSET                         | ✅ `bochs-drm` DRM driver; Y_OFFSET double-buffering | ⬜ §3 -- extract from `framebuffer.c`, double-height VIRT_HEIGHT,                |
| 💎  | VBE/GOP bare-metal fallback                            | ✅ UEFI GOP fallback for pre-driver                                 | ✅ `efifb` / `vesafb` framebuffer fallback           | ✅ §2 -- Done -- (register existing `framebuffer.c`                              |
| 💎  | Intel HD/UHD iGPU modesetting                          | ✅ `igdkmd64.sys` WDDM driver; full display                         | ✅ `i915` DRM driver; KMS modesetting;               | ⬜ §7 -- PRM-based clean-room, GMBUS EDID, pipe/transcoder/plane                 |
| 💎  | AMD APU Vega/RDNA display modesetting                  | ✅ `amdkmpfd.sys` WDDM; DCN display engine                          | ✅ `amdgpu` DRM; DCN modesetting; GPUOpen            | ⬜ §8 -- GPUOpen clean-room, DCN OTG+DPP, EDID                                   |
| 💎  | Multi-head: compositor spans/mirrors monitors          | ✅ `NtQueryDisplayConfig` / `NtSetDisplayConfig`; multi-monitor DWM | ✅ `xrandr`; DRM connector enumeration; compositor   | ⬜ §6 `display_get_head[]`, compositor per-head buffer, `NtQueryDisplayConfig`   |

> **After §1–8:** Impossible OS has accelerated display across all three development environments (VirtualBox VMSVGA, QEMU VirtIO-GPU, QEMU Bochs) and bare-metal fallback -- the same binary runs everywhere. The `display_device_t` vtable (`⭐`) is Impossible OS's architectural advantage: it is smaller and simpler than Linux DRM/KMS while still supporting runtime backend selection, hardware cursor, page-flip, and multi-head -- all exposed through a 7-function interface any `.kmod` can satisfy in ~200 lines.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU Bochs (`-device bochs-display`): boot log `[DISPLAY] VBE DISPI Bochs ...`; desktop renders; compositor calls `display_flush_rect()` (not raw `g_fb.base` writes)
- [ ] QEMU VirtIO-GPU (`-device virtio-vga`): boot log `[virtio-gpu] Scanout %ux%u`; hardware cursor moves smoothly; resize QEMU window → `wm_display_resized()` fires
- [ ] VirtualBox VMSVGA: boot log `[VMSVGA] SVGA II %ux%u FIFO`; hardware cursor visible; resize VM window → display adapts
- [ ] Bare metal (no GPU module loaded): `[DISPLAY] VBE/GOP fallback registered`; desktop reaches GUI unchanged
- [ ] `display_get_active()` returns highest-priority registered device; priority order: VMSVGA=ACCEL(100) > VirtIO-GPU=ACCEL(100) > Bochs=STANDARD(50) > VBE/GOP=FALLBACK(1)
- [ ] `rg "g_fb\.base" src/desktop/ src/kernel/gfx/` → 0 matches (all flushed via vtable after §1)
- [ ] Multi-head: QEMU `-device virtio-vga -device virtio-vga` → `[DISPLAY] 2 head(s) registered`; compositor allocates two back-buffers
- [ ] `NtQueryDisplayConfig` syscall returns `STATUS_SUCCESS` with correct width/height for each head
- [ ] Commit: `"drivers: GPU display -- display_device_t vtable, VMSVGA, VirtIO-GPU, Bochs, multi-head"`
