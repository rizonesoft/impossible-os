---
schema_version: 1
id: gpu-compositor
domain: 18-future-research
status: active
title: "TODO-03 -- GPU-Accelerated Compositor"
---

# TODO-03 -- GPU-Accelerated Compositor

> **Goal:** Research spike to select a GPU acceleration strategy for the Impossible OS
> compositor, design the Vulkan kernel driver architecture, assess Mesa lavapipe
> feasibility, plan the GPU compositor path that replaces `wm_composite()` + `gfx_blit()`
> with texture-based GPU rendering, research AMD/Intel display engines, and produce a
> prioritised plan for reaching 4K 120 Hz desktop compositing.

> [!IMPORTANT]
> **Current compositor** is CPU-based: `wm_composite()` + `gfx_blit()` + dirty-rect
> tracking + PMM acrylic blur cache (`acrylic_cache` in `wm.h`). This TODO researches
> the GPU path; it does NOT modify the existing compositor.
>
> **TinyGL software OpenGL** (~5 K lines, zlib) is specced in
> `10-platform-services/TODO-12 §8`; §3 here assesses the *upgrade path* from TinyGL
> to Mesa lavapipe (CPU Vulkan) -- it does not re-specify the TinyGL port itself.
>
> **IOMMU driver** is a hard prerequisite for DMA-safe GPU memory (§3) and is documented
> as a blocker in `18-future-research/TODO-02 §6`; the VirtIO-GPU path (§1 Option A)
> avoids IOMMU by using the hypervisor as a DMA safety boundary -- the recommended
> first step precisely because it sidesteps this blocker.
>
> No production GPU code is written during the research phase. The §6 deliverable
> `gpu-compositor-plan.md` is the output. A VirtIO-GPU proof-of-concept triangle in
> QEMU validates the approach before any bare-metal work.

---

## Inputs

- `include/desktop/wm.h` -- `window_t.acrylic_cache`, `wm_composite()`, dirty-rect flags -- §4 GPU path replaces these
- `include/desktop/desktop.h` -- `desktop_get_wallpaper_surface()` -- §4 wallpaper texture upload
- `include/kernel/drivers/framebuffer.h` -- framebuffer flip / dirty-rect API -- §4 DMA flip target
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()` -- §3 GPU-visible memory allocator
- `include/kernel/mm/vmm.h` -- `vmm_map_page()` -- §3 IOMMU-safe GPU buffer mapping
- `include/kernel/sched/syscall.h` -- next free syscall number -- §3 `SYS_GPU_*` additions
- `10-platform-services/TODO-12-long-term-features.md §8` (→ XREF) -- TinyGL port; §3 here assesses upgrade to Mesa lavapipe from TinyGL baseline
- `18-future-research/TODO-02-hypervisor.md §5 §6` (→ XREF) -- virtio-gpu stretch mentioned there; IOMMU prerequisite documented there; §1 §4 here build on that analysis
- `TODO-06-android-app-compatibility.md` (→ XREF) -- TODO-06 section 5 guest framebuffer to host compositor; VirtIO-GPU scanout ties to sections 1 and 5 here
- `src/kernel/drivers/virtio/virtio.c` -- existing guest-side VirtIO transport; §1 VirtIO-GPU driver extends this
- `src/kernel/gfx/` -- `gfx_simd.c`, `arc_ring.c`, `gfx_text.c` -- current CPU compositor internals

---

## Outcome

A `docs/architecture/gpu-compositor-plan.md` document that recommends VirtIO-GPU as
the first GPU acceleration target (QEMU today, bare-metal later), defines the Vulkan
kernel driver syscall surface (`SYS_GPU_SUBMIT/WAIT/MAP`), assesses the Mesa lavapipe
porting cost, and provides a LOC-estimated phased roadmap from the current CPU
compositor to 4K 120 Hz.

---

## Implementation Order

| Step | Section                                          | 💎/⭐ | Dependency                                                 |
| ---- | ------------------------------------------------ | ----- | ---------------------------------------------------------- |
| 1    | GPU access strategy (option comparison)          | ⭐    | existing VirtIO transport; IOMMU analysis (TODO-02 §6)     |
| 2    | TinyGL → Mesa lavapipe upgrade research          | ⭐    | `D10T12 §8` TinyGL baseline                                |
| 3    | Display engine research (AMD DCN + Intel Arc)    | ⭐    | bare-metal GPU strategy from §1                            |
| 4    | Vulkan kernel driver architecture                | ⭐    | §1 option selection; `pmm_alloc_contiguous`; syscall table |
| 5    | Compositor GPU path design                       | ⭐    | §4 Vulkan API; `wm.h` compositor internals                 |
| 6    | Research deliverables (`gpu-compositor-plan.md`) | ⭐    | §1–§5 complete                                             |

---

## 1. GPU Access Strategy Options `[Sonnet]`

> Survey three access paths; recommend the one feasible within 12 months.

- [ ] **Option A -- VirtIO-GPU (recommended first step)**:
  - QEMU `virtio-gpu-gl` exposes host GPU's OpenGL/Vulkan via a virtio MMIO device; Impossible OS needs a guest-side `virtio-gpu` driver
  - VirtIO-GPU protocol: `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`, `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`, `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`, `VIRTIO_GPU_CMD_SET_SCANOUT`, `VIRTIO_GPU_CMD_RESOURCE_FLUSH`
  - Extend existing `src/kernel/drivers/virtio/` with `gpu.c` driver; uses same split-ring transport already implemented
  - **Feasibility**: ✅ works in QEMU today with `-device virtio-gpu-gl -display gtk,gl=on`; no IOMMU required; host handles DMA; OVMF firmware already used for x86 UEFI boot
  - **Limitation**: requires hypervisor (QEMU); no path to bare-metal from this driver alone

- [ ] **Option B -- Direct bare-metal GPU (AMD RDNA / Intel Arc)**:
  - Requires: full PCIe BAR enumeration + MMIO mapping, GPU command ring allocation + submission, display engine programming (CRTC/plane/encoder), firmware blob loading (for AMD GFX firmware, Intel DMC)
  - **Feasibility**: 12–18+ months; blocked by: IOMMU driver (DMA safety), PCIe hot-plug/reset, firmware blob delivery mechanism
  - **Recommended**: Phase 2 after VirtIO-GPU path validates the compositor GPU architecture

- [ ] **Option C -- DRM/KMS-style modesetting (Linux-inspired)**:
  - Implement a kernel display subsystem with CRTC objects, plane objects (primary/overlay/cursor), encoder + connector pipeline
  - `drm_mode_set()` equivalent; atomic KMS (`atomic_commit`); DPCD AUX channel for DisplayPort
  - **Feasibility**: architecturally sound but requires bare-metal GPU (same blockers as Option B)
  - **Value**: the abstraction layer is worth building even if bare-metal comes later -- defines `display_plane_ops_t` vtable that both VirtIO-GPU and bare-metal can implement

- [ ] **Option comparison table**:

| Option                    | Works in QEMU         | Bare-metal | LOC estimate           | Timeline     | IOMMU required |
| ------------------------- | --------------------- | ---------- | ---------------------- | ------------ | -------------- |
| A -- VirtIO-GPU           | ✅ now                | ❌         | ~2 K LOC driver        | 4–6 weeks    | No             |
| B -- Bare-metal AMD/Intel | ✅ (pass-through)     | ✅         | ~30–50 K LOC           | 12–18 months | Yes            |
| C -- DRM/KMS layer        | N/A                   | ✅         | ~10 K LOC (layer only) | 3–4 months   | Depends on GPU |
| **Recommended**           | **A first, then C+B** | --         | --                     | --           | --             |

---

## 2. TinyGL → Mesa lavapipe Upgrade Research `[Sonnet]`

> Assesses upgrade path from TinyGL (OpenGL 1.1) to Mesa lavapipe (CPU Vulkan, no GPU
> required). TinyGL baseline is specced in `TODO-12 §8`.

- [ ] **TinyGL baseline** (from `TODO-12 §8`): `~5 K LOC`, OpenGL 1.1, renders to `gfx_surface_t`; `glFlush()` → `gfx_blit()`; covers: `glBegin/glEnd`, matrices, z-buffer, texture mapping
- [ ] **Mesa lavapipe assessment**:
  - Mesa is ~10M LOC total; `lavapipe` (CPU Vulkan, `LLVMpipe` backend) is the isolated target
  - Key isolatable modules: `src/gallium/drivers/llvmpipe/` (~80 K LOC), `src/gallium/auxiliary/` (shared utils), `include/vulkan/vulkan.h` (Khronos headers)
  - **Porting blockers for Impossible OS**:
    - Mesa uses `stdlib`, `stdio`, `pthread`, `mmap` -- all need kernel shims or elimination
    - LLVM backend (~2 M LOC) needed for `llvmpipe` JIT; alternative: `softpipe` (non-JIT, pure C, ~40 K LOC) is more portable
    - `lavapipe` requires `dlfcn.h` for ICD loading -- needs `dlopen` from `TODO-03 §3` (`12-user-platform-sdk/TODO-03`)
  - **`softpipe` as intermediate target** (Mesa's non-JIT Gallium driver, ~40 K LOC):
    - Portable C, no LLVM dependency
    - Provides full Gallium3D state tracker → Vulkan via `zink` layer
    - Estimated porting effort: 3–5 person-months of stdlib shim work + Gallium surface backend
- [ ] **LOC estimates** for each path:

| Path                    | LOC to port          | Prerequisites                                     | Quality             |
| ----------------------- | -------------------- | ------------------------------------------------- | ------------------- |
| Keep TinyGL             | 0 (done)             | None                                              | OpenGL 1.1 only     |
| Mesa `softpipe`         | ~40 K + shims        | `dlopen`, libc shims (`TODO-03 §3`, `TODO-04 §1`) | Full OpenGL 4.x     |
| Mesa `lavapipe`         | ~80 K + LLVM         | All above + LLVM port                             | Vulkan 1.3          |
| Zink (OpenGL on Vulkan) | Needs lavapipe first | As above                                          | Bridges GL → Vulkan |

- [ ] **Recommended path**: TinyGL stays as the immediate baseline (already specced in `TODO-12 §8`); Mesa `softpipe` becomes the Phase 2 software renderer after libc shims and `dlopen` are complete; lavapipe deferred to Phase 3
- [ ] **`vkd3d-proton` compatibility layer assessment**: runs Direct3D 12 apps on Vulkan ICD; requires a working Vulkan ICD (lavapipe or VirtIO-GPU) as prerequisite; enables Windows game compatibility; defer to Phase 3

---

## 3. Display Engine Research (AMD DCN + Intel Arc) `[Sonnet]`

> Documentation-only section. Surveys required firmware, register access patterns, and
> open-source reference for eventual bare-metal GPU display programming.

- [ ] **AMD Display Core Next (DCN) -- RDNA 2+ (RX 6000+)**:
  - DCN version: DCN 3.x (RDNA 3/RX 7000), DCN 2.x (RDNA 2/RX 6000)
  - Required firmware blobs: `amdgpu/dcn_x_y_dmcu.bin` (Display Micro-Controller Unit); loaded to GPU SRAM via command ring; available at `linux-firmware` repo (GPL redistributable)
  - Key register accesses: DCE IPP (Input Pixel Processing), DCE OPP (Output Pixel Processing), DC_STREAM (display stream), HUBP (Hubpreq -- display engine DMA to scanout), DPPCLK/DISPCLK programming
  - DisplayPort: DPCD AUX channel via `AUX_CONTROL_REG`; read EDID via `EDID_AUX_READ`
  - Open reference: `amdgpu` Linux driver, `drivers/gpu/drm/amd/display/dc/` -- ~500 K LOC of display code; architecture well-documented in AMD public datasheet
  - **Blocking items**: PCIe BAR MMIO mapping (need full PCIe driver), GPU command ring initialization before any display engine access, SMU (System Management Unit) power-on sequence for display rails

- [ ] **Intel Arc (Xe DG2) Display Engine**:
  - Display engine: Xe-HPD display, part of the Gen12+ IP block
  - Required firmware: `i915/dmc_xe2hpd_*.bin` (Display Micro-Controller, loaded via command stream); `intel-ucode` for compute microcode
  - Key register groups: `PIPE_*` (display pipe), `TRANSCODER_*` (encoder), `DDI_*` (Digital Display Interface), `DPLL_*` (DisplayPort PLL), `DE_*` (display engine enable/disable)
  - HDMI: HDMI audio DP-AUX, `HDMI_PHY_CONTROL`, panel power sequencing via `PCH_PP_CONTROL`
  - Open reference: `i915` + `xe` Linux drivers, `drivers/gpu/drm/i915/display/` -- well-documented; IGT GPU tools test cases serve as behavioral spec
  - **Blocking items**: same as AMD -- PCIe MMIO mapping, GuC/HuC firmware loading, ring buffer initialization

- [ ] **Common display programming prerequisites** (shared between AMD and Intel):
  - PCIe driver that exposes BAR0 MMIO region via `vmm_map_page(bar0_phys, virt, VMM_PAGE_MMIO)`
  - Firmware blob loading: read from `C:\Impossible\System\Drivers\gpu-firmware\*.bin` via VFS; copy to GPU-accessible memory; signal via command ring
  - EDID parsing: 128-byte EDID block via DPCD AUX; extract preferred resolution (`Detailed Timing Descriptor`); feed to mode-setting
  - **Recommended research reference**: `linux-firmware` repo for blob availability; `amdgpu` + `i915` Linux source for register-level programming patterns

---

## 4. Vulkan Kernel Driver Architecture `[Opus]`

> Novel: first GPU memory management subsystem in Impossible OS. Defines the
> kernel-userspace GPU API boundary, GPU-visible memory allocator separate from PMM,
> IOMMU-safe DMA mapping, and fence/semaphore GPU synchronization. No prior precedent.

**Source:** `src/kernel/gpu/` (new directory, gated `#ifdef ENABLE_GPU`)

- [ ] **GPU-visible memory allocator** (`src/kernel/gpu/gpu_mem.c`):
  - GPU RAM is distinct from system RAM -- on discrete GPUs, video RAM is accessible only via PCIe BAR1 MMIO (VRAM); on integrated GPUs and VirtIO-GPU, system RAM is GPU-visible via IOMMU mapping
  - `gpu_alloc(size, flags)` → returns `gpu_buf_t {phys, virt, gpu_va, size, flags}`:
    - VirtIO-GPU path: `pmm_alloc_contiguous(pages)` + `vmm_map_page()` -- host maps the pages into GPU address space automatically
    - Bare-metal discrete GPU: allocate from VRAM BAR1 region (separate free list from PMM)
  - `gpu_free(gpu_buf_t *)` -- return pages/VRAM back to respective pool
  - **IOMMU mapping** (bare-metal path only): `iommu_map(domain, gpu_va, hpa, size)` -- create GPU DMA → host physical mapping; prevents GPU from accessing arbitrary host memory; blocked until IOMMU driver available
- [ ] **GPU command submission syscalls** (add to `include/kernel/sched/syscall.h`):
  ```c
  SYS_GPU_MAP       = 90    // map a GPU buffer into process address space
  SYS_GPU_SUBMIT    = 91    // submit a command buffer to GPU ring
  SYS_GPU_WAIT      = 92    // wait for GPU fence (blocking)
  SYS_GPU_QUERY     = 93    // query GPU capabilities / available VRAM
  ```
- [ ] **GPU fence synchronization**:
  - `struct gpu_fence { uint64_t seqno; volatile uint64_t *completed_seqno_ptr; spinlock_t lock; waitqueue_t waiters; }`
  - `gpu_fence_signal(fence)`: increment `*completed_seqno_ptr`; wake all `waiters`
  - `gpu_fence_wait(fence, timeout_ms)`: if `*completed_seqno_ptr >= fence.seqno`: return immediately; else: `sched_sleep_until(timeout)` on `fence.waiters`
  - For VirtIO-GPU: the fence token is embedded in the virtio command; GPU signals by writing seqno to a host-mapped buffer
- [ ] **VirtIO-GPU command ring** (`src/kernel/drivers/virtio/gpu.c`):
  - `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`: allocate a host-side GPU texture (`resource_id`, `width`, `height`, format)
  - `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`: upload dirty rect from guest memory to host GPU texture
  - `VIRTIO_GPU_CMD_SET_SCANOUT`: bind a resource to a display scanout
  - `VIRTIO_GPU_CMD_RESOURCE_FLUSH`: present (flip) the scanout to the display
  - `VIRTIO_GPU_CMD_GET_CAPSET_INFO` / `VIRTIO_GPU_CMD_GET_CAPSET`: query Vulkan/OpenGL capability set for `virtio-gpu-gl`

---

## 5. Compositor GPU Path Design `[Opus]`

> Novel architectural redesign: replace CPU `wm_composite()` + `gfx_blit()` with GPU
> texture management + single-pass GPU composite. Requires careful dirty tracking to
> avoid unnecessary texture uploads. Complex algorithm.

**Source:** design only; production code gated on §4 Vulkan API availability

- [ ] **Reconcile with `display_device_t`**: `04-drivers-hardware/TODO-17` §1/§4 already plans the VirtIO-GPU module behind `display_device_t`; define `display_plane_ops_t` as an extension of it, not a second display abstraction
- [ ] **Window surface → GPU texture model**:
  - Each window gets a `gpu_buf_t window_tex` (2D resource on GPU); allocated once on `wm_create_window()`
  - On each frame: if `window->dirty_rect` is non-zero: `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` for dirty region only; clear `dirty_rect`; else: reuse existing GPU texture (zero CPU copy)
  - CPU `gfx_surface_t` backing buffer (`window->surface`) is still maintained for app rendering; GPU texture is a shadow copy uploaded on demand
- [ ] **Compositor frame pipeline** (replaces `wm_composite()` + `fb_swap()`/`fb_swap_rect()`):
  1. **Dirty scan**: iterate all windows; for each with non-empty `dirty_rect`: upload changed region via `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`
  2. **Wallpaper texture**: `desktop_get_wallpaper_surface()` → upload to GPU resource `wallpaper_tex` once (re-upload only on wallpaper change)
  3. **Acrylic/blur**: GPU blur pass on `wallpaper_tex` → `blur_tex`; replaces current CPU PMM blur in `wm.h`; GPU blur via fragment shader or `VIRTIO_GPU_BLOB_MEM_HOST3D` command
  4. **Composite pass**: GPU renders all window quads from back to front (z-order) in a single draw: wallpaper → blur layer → window textures → cursor; single `VIRTIO_GPU_CMD_RESOURCE_FLUSH` to present
  5. **DMA flip**: `VIRTIO_GPU_CMD_SET_SCANOUT` + `RESOURCE_FLUSH` -- no CPU memcpy to framebuffer; host GPU writes directly to display scanout
- [ ] **Expected performance gains**:

| Operation                       | CPU compositor          | GPU compositor target |
| ------------------------------- | ----------------------- | --------------------- |
| Acrylic/Mica blur (full screen) | ~8 ms (CPU PMM)         | ~0.5 ms (GPU shader)  |
| Composite 20 windows            | ~4 ms (`gfx_blit` × 20) | ~0.3 ms (GPU batched) |
| 4K (3840×2160) frame            | ~80 ms (not viable)     | ~4 ms (GPU viable)    |
| Max framerate (1080p)           | ~30 FPS                 | ~120 FPS target       |

- [ ] **Fallback path**: if GPU not available (`ENABLE_GPU` not set, or VirtIO-GPU not detected): keep existing `wm_composite()` CPU path unchanged; GPU path is purely additive
- [ ] **DRM/KMS abstraction layer** (`include/kernel/gpu/display.h`):
  ```c
  typedef struct display_plane_ops {
      int (*create_fb)(uint32_t w, uint32_t h, uint32_t fmt, uintptr_t *fb_id);
      int (*flip)(uintptr_t fb_id);              // present (DMA flip)
      int (*update_region)(uintptr_t fb_id, rect_t dirty); // partial upload
  } display_plane_ops_t;
  ```
  VirtIO-GPU implements this vtable; bare-metal AMD/Intel will implement the same vtable -- compositor calls only through `display_plane_ops_t`, never VirtIO-specific functions directly

---

## 6. Research Deliverables `[Sonnet]`

**Source:** `docs/architecture/gpu-compositor-plan.md`

- [ ] **`docs/architecture/gpu-compositor-plan.md`** -- sections:
  - **Option comparison table** (from §1): VirtIO-GPU vs. bare-metal AMD vs. Intel vs. DRM/KMS layer; LOC, timeline, IOMMU requirement, feasibility verdict
  - **Recommended path and rationale**: VirtIO-GPU (Phase 1), Mesa softpipe (Phase 2), bare-metal DRM/KMS + AMD/Intel (Phase 3), lavapipe + vkd3d-proton (Phase 4)
  - **Vulkan kernel driver API** (from §4): `SYS_GPU_*` syscall table; `gpu_buf_t` struct; fence API; `display_plane_ops_t` vtable
  - **Compositor GPU path diagram** (from §5): dirty-scan → upload → blur pass → composite → DMA flip; CPU fallback path
  - **Mesa porting cost assessment** (from §2): TinyGL (done) → softpipe (Phase 2, ~40 K LOC + shims) → lavapipe (Phase 3, ~80 K LOC + LLVM) → vkd3d-proton (Phase 4)
  - **AMD DCN + Intel Arc prerequisite list** (from §3): firmware blobs, PCIe BAR mapping, register access patterns, blocking items
  - **Dependency tree**: VirtIO-GPU → `gpu.c` VirtIO transport; softpipe → `dlopen` (`TODO-03 §3`) + libc shims (`TODO-04 §1`); bare-metal → IOMMU driver + PCIe driver; vkd3d-proton → Vulkan ICD
  - **LOC estimate per phase**:
    - Phase 1 (VirtIO-GPU driver + GPU compositor): ~5 K LOC
    - Phase 2 (Mesa softpipe port): ~40–50 K LOC
    - Phase 3 (bare-metal AMD/Intel + DRM/KMS layer): ~50–80 K LOC
    - Phase 4 (lavapipe + vkd3d-proton): ~100 K+ LOC
- [ ] **VirtIO-GPU proof-of-concept prototype** (in `gpu/virtio-gpu-spike` branch):
  - Boot QEMU with `-device virtio-gpu-gl -display gtk,gl=on`
  - `virtio_gpu_init()` → `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` → resource create → transfer solid color → set scanout → flush
  - **Success criterion**: QEMU window shows a 100×100 colored rectangle drawn entirely through VirtIO-GPU protocol, not via the CPU framebuffer path
- [ ] **Prototype branch**: `gpu/virtio-gpu-spike`; README with QEMU launch command and expected output; tracking GitHub Issue "GPU-Accelerated Compositor" linking to `gpu-compositor-plan.md`

---

## OS Comparison


| ⭐  | Feature                                   | 🪟 Win11                                  | 🐧 Linux                                        | 🚀 Impossible OS                                              |
| --- | ----------------------------------------- | ----------------------------------------- | ----------------------------------------------- | ------------------------------------------------------------- |
| 💎  | GPU-accelerated compositor                | ✅ DWM (DirectCompose; D3D11; GPU flip    | ✅ Mutter/KWin (OpenGL/Vulkan; KMS; GPU planes; | ⬜ §5 -- VirtIO-GPU Phase 1; DMA flip                         |
| 💎  | CPU Vulkan                                | ✅ WARPDevice (D3D12 software rasterizer) | ✅ Mesa lavapipe (CPU Vulkan 1.3)               | ⬜ §2 -- Mesa softpipe Phase 2; lavapipe                      |
| 💎  | Kernel-mode GPU memory + fence API        | ✅ D3DKMT / `dxgkrnl.sys` (`SYS_*` GPU    | ✅ DRM GEM/TTM + syncobj fences                 | ⬜ §4 -- `SYS_GPU_MAP/SUBMIT/WAIT/QUERY`; GPU fence waitqueue |
| 💎  | DRM/KMS-style display plane abstraction   | ✅ Windows DDI (DXGK display miniport;    | ✅ Linux DRM atomic KMS (CRTC                   | ⬜ §1 -- §5; `display_plane_ops_t` vtable; VirtIO-GPU +       |
| 💎  | DirectX / Vulkan on GPU                   | ✅ WDDM 3.x; D3D12; Vulkan via            | ✅ Mesa AMDGPU/RADV/ANV; AMDKFD; i915/xe kernel | ⬜ §3 -- §4; blocked by IOMMU +                               |
| ⭐  | Zero-CPU-copy 4K 120 Hz compositor target | ✅ Windows 11 DWM: GPU flip               | ✅ KWin/Mutter: DRM page-flip, atomic commit    | ⬜ §5 -- VirtIO-GPU `RESOURCE_FLUSH` DMA flip; 4K             |

Impossible OS's `⭐` advantage: the `display_plane_ops_t` abstraction layer means VirtIO-GPU
in QEMU and bare-metal AMD/Intel share the same compositor call path from day one --
switching from VirtIO-GPU to native AMD RDNA is a driver swap, not a compositor
rewrite. The GPU path is purely additive (full CPU fallback preserved), so the research
spike produces zero regression risk while delivering the architectural foundation for
4K 120 Hz compositing.

---

## Verification

- [ ] **VirtIO-GPU probe**: boot QEMU with `-device virtio-gpu-gl -display gtk,gl=on -device virtio-gpu`; serial shows `"[GPU] VirtIO-GPU detected: {width}×{height}"`; fallback to CPU compositor if not present (no crash)
- [ ] **VirtIO-GPU colored rectangle POC**: `virtio_gpu_test_rect(100, 100, 200, 200, 0xFF0000FF)` via serial command → QEMU window shows red 100×100 rect; no CPU framebuffer involved
- [ ] **GPU buffer allocation**: `gpu_alloc(4096 * 1024, GPU_FLAG_SHARED)` → returns non-null `gpu_buf_t`; `gpu_free()` returns memory; PMM free page count restored
- [ ] **GPU fence**: submit command + `gpu_fence_wait(fence, 100)` → returns before 100 ms; verify `*completed_seqno_ptr` incremented
- [ ] **Compositor GPU path (design validation)**: enable GPU path; compositor renders 10 windows via VirtIO-GPU; measure frame time vs. CPU path; verify GPU frame time < CPU frame time at 1080p
- [ ] **DMA flip (no CPU memcpy)**: enable GPU compositor; use performance counter to verify `memcpy` call count per frame drops to 0 during compositing (only dirty-region `TRANSFER_TO_HOST` for window content)
- [ ] **CPU fallback**: boot without `-device virtio-gpu` → compositor falls back to `wm_composite()` CPU path; no crash; full desktop functional
- [ ] **`gpu-compositor-plan.md` deliverable**: document exists; contains option comparison table, LOC estimates for all 4 phases, dependency tree, `display_plane_ops_t` vtable definition, Mesa porting assessment
- [ ] Commit: `"research: GPU compositor spike -- VirtIO-GPU driver, Vulkan kernel API, compositor GPU path design, Mesa lavapipe assessment, display engine research"`
