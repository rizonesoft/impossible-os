# P0003 — Guest Additions & Hypervisor Abstraction

> **Goal:** Build a hypervisor abstraction layer that auto-detects the host platform
> (VirtualBox, QEMU/KVM, Hyper-V, or bare metal) and activates the appropriate backend
> drivers for mouse integration, display resizing, shared folders, and clipboard sharing.
> This is the OS equivalent of "Guest Additions" — but ours works across all hypervisors
> through a unified kernel interface.

> [!IMPORTANT]
> **Design Philosophy:** Rather than shipping VBox-specific "Guest Additions" (vendor branding),
> we implement a **hypervisor abstraction layer** — one common interface with pluggable backends.
> This is exactly how Linux does it: `vboxguest.ko` for VBox, `virtio_*.ko` for QEMU,
> `hv_*.ko` for Hyper-V — all behind common input/display/filesystem interfaces.

> [!TIP]
> **Already Started:** The `vbox_mouse.c` driver (PCI `80EE:CAFE`, VMMDev protocol v1.03)
> IS the first piece of our guest additions. Display auto-resize uses the same device,
> same IRQ handler, just a different request type (`VBOX_REQUEST_GET_DISPLAY_CHANGE = 51`).

---

## Code Organisation

> [!IMPORTANT]
> **Hypervisor code lives in `src/kernel/hypervisor/`, NOT `src/kernel/drivers/`.**
> `drivers/` is for real hardware that exists on bare metal (PS/2, AHCI, RTL8139).
> `hypervisor/` is for virtual hardware that only exists inside VMs.
> This matches Linux: `drivers/virt/vboxguest/`, `drivers/virtio/`, `drivers/hv/`.

```
src/kernel/
├── drivers/              ← Hardware drivers (bare metal)
│   ├── mouse.c            ← PS/2 mouse (real hardware)
│   ├── keyboard.c
│   ├── ahci.c
│   ├── rtl8139.c
│   └── ...
│
├── hypervisor/           ← Guest additions / hypervisor abstraction
│   ├── detect.c           ← CPUID + PCI probe → VBox? KVM? Hyper-V? Bare metal?
│   ├── hv.c               ← Unified interface dispatch table
│   │
│   ├── vbox/              ← VirtualBox backend
│   │   ├── vbox_guest.c       ← VMMDev init, IRQ handler, event dispatch
│   │   ├── vbox_mouse.c       ← Absolute mouse (moved from drivers/)
│   │   ├── vbox_display.c     ← Display auto-resize
│   │   ├── vbox_hgcm.c        ← HGCM client connection
│   │   ├── vbox_sf.c          ← Shared folders via HGCM
│   │   └── vbox_clip.c        ← Shared clipboard via HGCM
│   │
│   ├── virtio/            ← QEMU/KVM backend
│   │   ├── virtio_input.c     ← Tablet (moved from drivers/)
│   │   ├── virtio_gpu.c       ← Display resize
│   │   └── virtio_9p.c        ← Shared folders
│   │
│   └── hyperv/            ← Hyper-V backend (future)
│       ├── vmbus.c
│       └── hv_mouse.c

include/kernel/
├── drivers/              ← Hardware driver headers
│   └── mouse.h
├── hypervisor/           ← Hypervisor headers
│   ├── detect.h
│   ├── hv.h              ← Unified interface
│   └── vbox/
│       ├── vbox_mouse.h
│       └── ...
```

> [!TIP]
> **Migration:** Existing `vbox_mouse.c` and `virtio_input.c` will move from `drivers/`
> to `hypervisor/vbox/` and `hypervisor/virtio/` respectively. The Makefile uses `find`
> for source discovery, so the move is seamless — no Makefile changes needed.

---

## 1. VirtualBox — VMMDev Mouse Integration ✅

### 1.1 VBoxGuest Absolute Mouse

**Prompt:** This section is marked complete. Verify that `src/kernel/drivers/vbox_mouse.c` discovers PCI device `80EE:CAFE`, initializes VMMDev protocol v1.03, enables absolute mouse with `GUEST_CAN_ABSOLUTE | NEW_PROTOCOL | GUEST_NEEDS_HOST_CURSOR`, and receives coordinates via IRQ handler. Buttons merge from PS/2. Verify 3-tier priority in `main.c`: VirtIO > VBox > PS/2. Run `bash scripts/build.sh clean` and test in VBox.

- [x] PCI discovery: vendor `0x80EE`, device `0xCAFE`
- [x] VMMDev protocol v1.03 initialization (`GuestInfo` packet)
- [x] Absolute mouse via `SetMouse` with capability flags
- [x] IRQ handler: acknowledge events → `GetMouse` → scale 0–0xFFFF → screen pixels
- [x] Cached position returned by `vbox_mouse_get_state()` (no polling)
- [x] Buttons from PS/2 (VMMDev does not provide button state)
- [x] 3-tier priority: VirtIO tablet → VBox VMMDev → PS/2 relative
- [x] Commit: `"input: VBox absolute mouse + fix drag edge jump"` (`fd410bc`)
- [x] Commit: `"input: fix VBox mouse movement — IRQ-based coords"` (`3d5f08e`)

---

## 2. VirtualBox — Display Auto-Resize

### 2.1 VMMDev Display Change Events

**Prompt:** When the user resizes the VirtualBox window, VBox fires a VMMDev event with the ideal guest resolution. Implement handling for `VBOX_REQUEST_GET_DISPLAY_CHANGE` (request type 51) in the existing VBox IRQ handler. On receiving a display change event, read the new resolution and BPP, then reconfigure the VBE/VGA framebuffer to match. Notify the window manager to recomposite at the new size. The plumbing already exists — same `80EE:CAFE` device, same IRQ line. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, test by resizing the VBox window, and commit as `"vbox: display auto-resize via VMMDev"`.

- [ ] Add `VBOX_REQUEST_GET_DISPLAY_CHANGE = 51` constant
- [ ] Add `VBOX_REQUEST_SET_GUEST_CAPS = 55` constant
- [ ] Send `SetGuestCaps` with `VBOX_GUEST_CAP_GRAPHICS` (bit 2) during init
- [ ] Allocate `vbox_display_change` packet (xres, yres, bpp, eventack fields)
- [ ] In IRQ handler: detect display change event, send `GetDisplayChange` request
- [ ] Read new xres, yres, bpp from response packet
- [ ] Reconfigure framebuffer via VBE mode set (or VGA register writes)
- [ ] Reallocate back buffer via `pmm_alloc_contiguous()` for new resolution
- [ ] Notify WM: `wm_display_resized(new_width, new_height)`
- [ ] Update wallpaper scaling for new dimensions
- [ ] Commit: `"vbox: display auto-resize via VMMDev"`

### 2.2 VBox Guest Capabilities Advertisement

**Prompt:** Tell VBox that we support auto-resize graphics by sending a `SetGuestCaps` packet with the graphics bit set. This enables the "Auto-resize Guest Display" option in VBox's View menu. Without this, VBox grays out the option. After completing all items, mark every item as `[x]`, and commit as `"vbox: advertise guest capabilities"`.

- [ ] Create `vbox_guest_caps` packet struct (header + uint32_t caps)
- [ ] Set bit 2 (`VBOX_GUEST_CAP_GRAPHICS`) for auto-resize support
- [ ] Send during `vbox_mouse_init()` (or rename to `vbox_guest_init()`)
- [ ] Verify VBox View menu shows "Auto-resize Guest Display" as active
- [ ] Commit: `"vbox: advertise guest capabilities"`

---

## 3. VirtualBox — Shared Folders (HGCM)

### 3.1 HGCM Client Connection

**Prompt:** VBox shared folders use the Host-Guest Communication Manager (HGCM) protocol over VMMDev. HGCM is a generic RPC system — the guest sends function call packets to named host services. The "VBoxSharedFolders" service provides file I/O on host directories. First, implement the HGCM connect/disconnect protocol. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vbox: HGCM client connection"`.

- [ ] Implement `VBOX_REQUEST_HGCM_CONNECT = 60` — connect to named service
- [ ] Implement `VBOX_REQUEST_HGCM_DISCONNECT = 61` — disconnect
- [ ] Implement `VBOX_REQUEST_HGCM_CALL = 62` — call service function
- [ ] HGCM parameter types: uint32, uint64, pointer (guest → host / host → guest)
- [ ] Connect to `"VBoxSharedFolders"` service, store client ID
- [ ] Handle HGCM async completion (event + callback)
- [ ] Commit: `"vbox: HGCM client connection"`

### 3.2 Shared Folder Mounting

**Prompt:** Once connected to the VBoxSharedFolders service, implement folder queries and file operations. Map host shared folders as VFS mount points under `/mnt/host/` (or `H:\` drive letter). Support directory listing, file read, file write, and stat. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, test by reading a host file from the guest shell, and commit as `"vbox: shared folder VFS mount"`.

- [ ] Query available shared folder mappings (HGCM function 1: QueryMappings)
- [ ] Query mapping name for each mapping ID (HGCM function 2: QueryMapName)
- [ ] Implement MapFolder (HGCM function 3) — get root handle
- [ ] Implement CreateFile / OpenFile / ReadFile / WriteFile / CloseFile
- [ ] Implement ListDir (HGCM function for directory enumeration)
- [ ] Register as VFS filesystem: `vfs_mount("/mnt/host/<name>", vbox_sf_ops)`
- [ ] Shell integration: `ls H:\` or `cat /mnt/host/shared/readme.txt`
- [ ] Commit: `"vbox: shared folder VFS mount"`

---

## 4. VirtualBox — Shared Clipboard

### 4.1 Clipboard Service

**Prompt:** VBox clipboard sharing uses the HGCM service `"VBoxSharedClipboard"`. The guest advertises its clipboard capabilities, and VBox forwards clipboard data bidirectionally. Support plain text clipboard for copy/paste between host and guest. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"vbox: shared clipboard"`.

- [ ] Connect to `"VBoxSharedClipboard"` HGCM service
- [ ] Advertise text format capability
- [ ] Handle host → guest clipboard: read text data from HGCM, store in kernel clipboard buffer
- [ ] Handle guest → host clipboard: on Ctrl+C in guest, push text to HGCM service
- [ ] Kernel clipboard API: `clipboard_set_text()`, `clipboard_get_text()`
- [ ] Commit: `"vbox: shared clipboard"`

---

## 5. QEMU/KVM — VirtIO Integration

### 5.1 VirtIO Tablet ✅

**Prompt:** This section is marked complete. Verify that `src/kernel/drivers/virtio_input.c` provides absolute mouse coordinates via VirtIO input for QEMU's `-device virtio-tablet-pci`.

- [x] `virtio_input_init()` — discover VirtIO input PCI device
- [x] `virtio_input_get_state()` — return absolute x, y, buttons
- [x] Scale 0–32767 to framebuffer resolution
- [x] Makefile: `-device virtio-tablet-pci` in QEMU flags

### 5.2 VirtIO GPU (Display Resize)

**Prompt:** Implement a VirtIO GPU driver for QEMU display resizing. VirtIO GPU replaces the legacy VGA adapter and supports dynamic resolution changes, multi-head displays, and hardware-accelerated 2D. Start with basic scanout configuration (set resolution + framebuffer). After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"virtio: GPU driver for display resize"`.

- [ ] PCI discovery: VirtIO GPU device (device ID 16)
- [ ] Implement virtqueue setup (control queue + cursor queue)
- [ ] `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` — create framebuffer resource
- [ ] `VIRTIO_GPU_CMD_SET_SCANOUT` — attach resource to display
- [ ] `VIRTIO_GPU_CMD_RESOURCE_FLUSH` — push framebuffer to host
- [ ] `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` — query supported resolutions
- [ ] Handle display resize events
- [ ] Commit: `"virtio: GPU driver for display resize"`

### 5.3 VirtIO-FS / 9P Shared Folders

**Prompt:** Implement VirtIO-FS or 9P filesystem sharing for QEMU. This provides shared folder access between host and guest — QEMU's equivalent of VBox shared folders. VirtIO-FS uses FUSE over virtqueues. 9P is simpler (Plan 9 protocol). Start with 9P as it's more straightforward. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"virtio: 9P shared folder support"`.

- [ ] PCI discovery: VirtIO 9P device (device ID 9)
- [ ] Implement 9P protocol: version, attach, walk, open, read, write, stat, clunk
- [ ] Register as VFS mount point
- [ ] QEMU flag: `-virtfs local,path=/host/dir,mount_tag=shared,security_model=mapped`
- [ ] Commit: `"virtio: 9P shared folder support"`

---

## 6. Hyper-V — VMBus Integration (Future)

### 6.1 VMBus Discovery

**Prompt:** Hyper-V uses VMBus (Virtual Machine Bus) for host-guest communication instead of emulated PCI or VirtIO. VMBus is discovered via the Hyper-V CPUID leaf (0x40000000 returns "Microsoft Hv") and MSR-based hypercall interface. This is needed for production Hyper-V deployments. After completing all items, mark every item as `[x]`, and commit as `"hyperv: VMBus discovery"`.

- [ ] Detect Hyper-V via CPUID leaf 0x40000000 ("Microsoft Hv")
- [ ] Read Hyper-V feature MSRs (guest OS ID, hypercall page)
- [ ] Set up hypercall page for VMBus communication
- [ ] Implement VMBus channel offer/open/close protocol
- [ ] Commit: `"hyperv: VMBus discovery"`

### 6.2 Hyper-V Synthetic Mouse & Video

- [ ] Synthetic mouse: absolute coordinates via VMBus HID channel
- [ ] Synthetic video: dynamic resolution via VMBus video channel
- [ ] Commit: `"hyperv: synthetic mouse + video"`

---

## 7. Hypervisor Abstraction Layer

### 7.1 Hypervisor Detection

**Prompt:** Create a unified hypervisor detection module that identifies the host platform at boot. Use CPUID leaf 0x40000000 to read the hypervisor brand string, and fall back to PCI device probing. The detection result determines which backend drivers to load. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"hypervisor: auto-detection layer"`.

- [ ] Create `src/kernel/hypervisor/detect.c` and `include/kernel/hypervisor/detect.h`
- [ ] Detection methods:
  - [ ] CPUID 0x40000000: `"KVMKVMKVM\0\0\0"` → QEMU/KVM
  - [ ] CPUID 0x40000000: `"Microsoft Hv"` → Hyper-V
  - [ ] CPUID 0x40000000: `"VBoxVBoxVBox"` → VirtualBox
  - [ ] PCI `80EE:CAFE` present → VirtualBox (fallback)
  - [ ] None of the above → bare metal
- [ ] Expose `hypervisor_type()` → enum { HV_NONE, HV_VBOX, HV_KVM, HV_HYPERV }
- [ ] Log: `"[OK] Hypervisor: VirtualBox"` / `"QEMU/KVM"` / `"Hyper-V"` / `"Bare metal"`
- [ ] Commit: `"hypervisor: auto-detection layer"`

### 7.2 Unified Hypervisor Interface

**Prompt:** Create a common interface that abstracts per-hypervisor drivers behind a single API. The kernel and desktop call these functions without knowing which hypervisor is running. After completing all items, mark every item as `[x]`, and commit as `"hypervisor: unified abstraction interface"`.

- [ ] Create `include/kernel/hypervisor/hv.h` — common interface
- [ ] `hv_mouse_available()` → is absolute mouse active?
- [ ] `hv_mouse_get_state()` → return absolute x, y (delegates to vbox/virtio/vmbus)
- [ ] `hv_display_resize(w, h)` → request display resolution change
- [ ] `hv_shared_folder_mount(name, path)` → mount host folder
- [ ] `hv_clipboard_get()` / `hv_clipboard_set()` → clipboard access
- [ ] Backend dispatch table populated by `hypervisor_init()` based on detection
- [ ] Simplify `main.c` compositor: replace 3-tier if/else with single `hv_mouse_get_state()`
- [ ] Commit: `"hypervisor: unified abstraction interface"`

---

## 8. Bare Metal Fallback

### 8.1 Native Input (No Hypervisor)

**Prompt:** On bare metal (no hypervisor detected), all input comes from PS/2 or USB HID drivers. The hypervisor abstraction layer's mouse functions should gracefully fall back to these. Display resizing is handled by the native GPU driver (VBE/GOP mode switching). No shared folders or clipboard on bare metal.

- [ ] `hv_mouse_available()` returns 0 on bare metal → use PS/2 / USB HID directly
- [ ] `hv_display_resize()` delegates to VBE mode set on bare metal
- [ ] `hv_shared_folder_mount()` returns -1 (not supported)
- [ ] `hv_clipboard_get()` / `hv_clipboard_set()` → kernel-only clipboard (no host sharing)
- [ ] Commit: `"hypervisor: bare metal fallback"` 

---

## Priority Order

| Priority | Section | Description |
|----------|---------|-------------|
| ✅ Done | 1.1 VBox Mouse | VMMDev absolute mouse (PCI `80EE:CAFE`) |
| ✅ Done | 5.1 VirtIO Tablet | QEMU absolute mouse (virtio-tablet-pci) |
| 🔴 P0 | 2.1 VBox Display Resize | Same device, same IRQ — almost free to add |
| 🔴 P0 | 7.1 Hypervisor Detection | Foundation for all abstraction |
| 🟠 P1 | 3.1 HGCM Connection | Required for shared folders and clipboard |
| 🟠 P1 | 3.2 Shared Folders | Mount host directories in guest — huge dev UX win |
| 🟡 P2 | 4.1 Shared Clipboard | Copy/paste between host and guest |
| 🟡 P2 | 5.2 VirtIO GPU | QEMU display resize |
| 🟡 P2 | 7.2 Unified Interface | Clean up per-hypervisor code in main.c |
| 🟢 P3 | 2.2 Guest Caps | Enable VBox "Auto-resize" menu option |
| 🟢 P3 | 5.3 VirtIO-FS / 9P | QEMU shared folders |
| 🟢 P3 | 8.1 Bare Metal Fallback | Graceful degradation without hypervisor |
| 🔵 P4 | 6.1 VMBus Discovery | Hyper-V support |
| 🔵 P4 | 6.2 Hyper-V Devices | Synthetic mouse + video for Hyper-V |

---

## Guest Additions Feature Matrix

| Feature | What It Does | Effort | Value |
|---------|-------------|--------|-------|
| Absolute mouse | ✅ **Done** — `vbox_mouse.c` | — | High |
| Display auto-resize | VBox tells guest ideal resolution on window resize | Small (same VMMDev protocol) | High |
| Shared folders | Mount host folders inside guest | Medium (needs VFS + HGCM) | Very High |
| Shared clipboard | Copy/paste between host and guest | Medium (HGCM service) | High |
| Seamless mode | Guest windows appear on host desktop | Large | Low priority |

## Hypervisor Backend Map

```
Hypervisor Detection (CPUID 0x40000000 + PCI probe):
  VirtualBox? → vbox_mouse, vbox_display_resize, vbox_shared_folders
  QEMU/KVM?   → virtio_input, virtio_gpu, virtio_fs / 9p
  Hyper-V?    → vmbus + synthetic mouse + synthetic video
  Bare metal? → PS/2 mouse, USB HID, native GPU
```

> **This is how Linux does it:** `vboxguest.ko` for VBox, `virtio_*.ko` for QEMU,
> `hv_*.ko` for Hyper-V — all behind common input/display/filesystem interfaces.
> Our hypervisor abstraction layer follows the same proven architecture.
