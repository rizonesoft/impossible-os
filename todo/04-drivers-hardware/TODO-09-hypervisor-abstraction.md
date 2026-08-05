---
schema_version: 1
id: hypervisor-abstraction
domain: 04-drivers-hardware
status: active
title: "TODO-09 -- Hypervisor Abstraction Layer"
---

# TODO-09 -- Hypervisor Abstraction Layer

> **Goal:** Build a unified cross-hypervisor abstraction layer so Impossible OS works identically in VirtualBox, QEMU/KVM, Hyper-V, and on bare metal -- completing guest-additions support (display resize, HGCM, shared folders, shared clipboard, VirtIO GPU/9P, Hyper-V synthetic HID/video) and adding a clean `hv.h` backend dispatch table that eliminates all hypervisor-specific `if/else` chains from the kernel core.

> [!IMPORTANT]
> **Already complete:** VBox absolute mouse (`vbox_mouse.c`), VirtIO tablet (`src/kernel/drivers/virtio/input.c`), VMBus core and storvsc. This TODO builds the remaining guest-additions stack (display, HGCM, shared folders, clipboard) and new hypervisor backends (VirtIO GPU, 9P, Hyper-V synthetic HID/video) plus the unified abstraction layer. The bare-metal path is the "null backend" -- every `hv_*` function must handle `HV_NONE` gracefully.

## Inputs

- [`src/kernel/drivers/vbox_mouse.c`](../../src/kernel/drivers/vbox_mouse.c) -- existing VBox mouse port-based backend (reference for port I/O pattern)
- [`src/kernel/drivers/virtio/input.c`](../../src/kernel/drivers/virtio/input.c) -- VirtIO tablet (reference for VirtIO queue pattern)
- [`src/kernel/drivers/virtio/virtio.c`](../../src/kernel/drivers/virtio/virtio.c) -- VirtIO core init
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §1` -- PCI ECAM/MCFG needed for PCI `80EE:CAFE` VBox detection
- → XREF: `04-drivers-hardware/TODO-08-core-driver-enhancements.md §2` -- PCIe capability scanner used to enumerate VirtIO GPU and 9P PCI devices
- → XREF: `07-graphics-ui` domain -- `wm_display_resized(w,h)` is a compositor API notified by §4, §8, §11 when display resolution changes
- → XREF: `04-drivers-hardware/TODO-11-input-system.md §9` -- raw input grab API and `SYS_MOUSE_GRAB` must remain compatible with Hyper-V synthetic HID injecting into the same keyboard/mouse subsystem (§10)

## Outcome

- `hypervisor_type()` reliably identifies VBox, KVM, Hyper-V, or bare metal via CPUID leaf `0x40000000` (with PCI fallback for VBox); `hypervisor_init()` fills a backend dispatch table `hv_ops`.
- All hypervisor-specific logic is isolated behind `hv.h`; `kernel/main.c` calls `hv_mouse_available()`, `hv_display_resize()`, etc. -- no platform `if/else` chains in core code.
- VBox: display auto-resize, HGCM channel, shared folders at `H:\`, bidirectional clipboard.
- QEMU/KVM: VirtIO GPU page-flip pipeline, VirtIO 9P shared folders (9P2000.L), hardware cursor via cursor queue.
- Hyper-V: synthetic keyboard/mouse via VMBus HID VSP channels, synthetic video via VMBus Video VSP.
- Bare metal: null backend returns graceful errors -- existing PS/2, USB HID, and VBE framebuffer continue to work unchanged.

## Implementation Order

| ⭐   | Order | Deliverable                              | Depends On                               | Status |
| --- | :---: | ---------------------------------------- | ---------------------------------------- | :----: |
| 💎   |   1   | §1 Hypervisor detection -- CPUID `0x40000000`, PCI `80EE:CAFE` | PCI scan (existing)                      |  [ ]   |
| 💎   |   2   | §2 Unified `hv.h` interface + backend dispatch table | §1 (type known at boot)                  |  [ ]   |
| 💎   |   3   | §3 Bare-metal null backend               | §2 (dispatch table structure defined)    |  [ ]   |
| 💎   |   4   | §4 VBox display auto-resize              | §2, VBox IRQ handler (existing)          |  [ ]   |
| 💎   |   5   | §5 VBox HGCM client                      | §2 (VBox confirmed), VBox port I/O baseline |  [ ]   |
| 💎   |   6   | §6 VBox shared folders                   | §6 (HGCM), VFS mount point API           |  [ ]   |
| 💎   |   7   | §7 VBox shared clipboard                 | §6 (HGCM)                                |  [ ]   |
| 💎   |   8   | §8 VirtIO GPU display resize + page flip | §2, VirtIO core (existing)               |  [ ]   |
| 💎   |   9   | §9 VirtIO 9P shared folders              | §2, VirtIO core, VFS mount               |  [ ]   |
| 💎   |  10   | §10 Hyper-V synthetic HID (keyboard + mouse) | §2, VMBus core (existing)                |  [ ]   |
| 💎   |  11   | §11 Hyper-V synthetic video              | §2, VMBus, framebuffer infrastructure    |  [ ]   |
| 💎   |  12   | §12 VirtIO RNG guest entropy             | §2, VirtIO core (existing), D02T03 §5    |  [ ]   |

> All parity rows are 💎: VirtualBox, KVM/QEMU, and Hyper-V guest-additions are shipping features of those hypervisors. The ⭐ differentiator is the unified `hv.h` dispatch table -- Impossible OS treats all three hypervisors as first-class targets behind a single clean interface, whereas Linux's hypervisor drivers span dozens of kernel subsystems with no cross-hypervisor abstraction.

---

## 1. Hypervisor Detection `[Sonnet]`

Use CPUID leaf `0x40000000` (the hypervisor brand-string leaf) to identify the platform. Fall back to PCI device `80EE:CAFE` for VBox when CPUID is inconclusive. Expose `hypervisor_type()` and write a boot log entry.

**Files:** `src/kernel/hv/hv_detect.c` (new), `include/kernel/hv/hv.h` (new)

- [ ] Define `typedef enum { HV_NONE = 0, HV_VBOX, HV_KVM, HV_HYPERV } hypervisor_type_t`
- [ ] `hypervisor_detect()`: execute `CPUID(0x40000000)`; compare `EBX:ECX:EDX` brand string: `"KVMKVMKVM\0"` → `HV_KVM`; `"Microsoft Hv"` → `HV_HYPERV`; `"VBoxVBoxVBox"` → `HV_VBOX`
- [ ] VBox PCI fallback: if CPUID inconclusive, scan PCI for `vendor=0x80EE, device=0xCAFE`; if found, set `HV_VBOX`
- [ ] Cache result in `static hypervisor_type_t g_hv_type`; `hypervisor_type()` returns it
- [ ] Boot log: `[HV] Hypervisor: VirtualBox` / `KVM` / `Hyper-V` / `None (bare metal)`
- [ ] Commit: `"drivers: hypervisor detection -- CPUID 0x40000000 brand, PCI 80EE:CAFE fallback"`

## 2. Unified `hv.h` Interface + Backend Dispatch `[Sonnet]`

Define the `hv_ops_t` vtable with all cross-hypervisor operations. `hypervisor_init()` detects the platform (§1), selects the right backend, and fills `g_hv_ops`. Inline helpers in `hv.h` delegate to the vtable. Replace any three-tier `if/else` in `kernel/main.c`.

**Files:** `include/kernel/hv/hv.h`, `src/kernel/hv/hv.c` (new)

- [ ] Define `hv_ops_t { bool (*mouse_available)(void); void (*mouse_get_state)(mouse_state_t *); int (*display_resize)(uint32_t w, uint32_t h); int (*shared_folder_mount)(const char *name, const char *path); int (*clipboard_get)(char *buf, size_t len); int (*clipboard_set)(const char *text, size_t len); void (*init)(void); void (*shutdown)(void); }`
- [ ] Global `hv_ops_t g_hv_ops`; inline `hv_mouse_available()` etc. forward to `g_hv_ops.*`
- [ ] `hypervisor_init()`: call `hypervisor_detect()`; switch on type, assign backend `hv_ops_*` pointer, call `g_hv_ops.init()`
- [ ] Remove hypervisor `if/else` chains from `kernel/main.c`; replace with `hypervisor_init()` call + `hv_*` API usage
- [ ] Commit: `"kernel: hv_ops_t dispatch table -- unified hypervisor backend interface"`

## 3. Bare-Metal Null Backend `[Sonnet]`

Implement the `hv_ops_t` null backend for `HV_NONE`. Every function returns the correct "not supported" result so that non-hypervisor code paths (PS/2, USB HID, VBE framebuffer) continue unchanged.

**Files:** `src/kernel/hv/hv_none.c` (new)

- [ ] `hv_none_ops`: `mouse_available` → `false`; `mouse_get_state` → no-op; `display_resize` → call existing `vbe_set_mode(w, h)` (best-effort); `shared_folder_mount` → `STATUS_NOT_SUPPORTED`; `clipboard_get/set` → `STATUS_NOT_SUPPORTED`; `init/shutdown` → no-op
- [ ] Assign `g_hv_ops = hv_none_ops` when `hypervisor_detect()` returns `HV_NONE`
- [ ] Confirm that bare-metal boot still reaches desktop without modification after §1–2 land
- [ ] Boot log: no hypervisor line (or at `LOG_DEBUG` verbosity only)
- [ ] Commit: `"kernel: HV_NONE null backend -- bare-metal safe fallback for all hv_ops"`

---

## 4. VBox Display Auto-Resize `[Sonnet]`

Handle the VBox display-change IRQ to read the new resolution and reconfigure the VBE framebuffer. Notify the window manager. Set `VBOX_GUEST_CAP_GRAPHICS` so VBox knows the guest handles resize events.

**Files:** `src/kernel/drivers/vbox_display.c` (new), `include/kernel/drivers/vbox_display.h` (new)

- [ ] At init: issue `SetGuestCaps` request (request type 55) with `VBOX_GUEST_CAP_GRAPHICS (1<<2)` to announce resize support
- [ ] In existing VBox IRQ handler: detect `VBOX_NOTIFY_DISPLAY_CHANGE` event; issue `VBOX_REQUEST_GET_DISPLAY_CHANGE (51)`; read `xres`, `yres`, `bpp` from response
- [ ] Call VBE mode-set to reconfigure framebuffer to new resolution; update `framebuffer_t g_fb` dimensions
- [ ] Post `wm_display_resized(w, h)` to the compositor message queue; compositor reflows all windows
- [ ] Graceful handling if VBE mode-set fails (resolution unsupported): log warning, keep existing resolution
- [ ] Boot log: `[VBOX] Display auto-resize enabled`; on each resize: `[VBOX] Display resized to %ux%u`
- [ ] Commit: `"drivers: VBox display auto-resize -- SetGuestCaps, DISPLAY_CHANGE IRQ, VBE reconfigure"`

## 5. VBox HGCM Client `[Sonnet]`

Implement the Host-Guest Communication Manager (HGCM) channel layer. Provides `HGCM_CONNECT`, `HGCM_DISCONNECT`, and `HGCM_CALL` with parameter encoding for `uint32`, `uint64`, and pointer types. Used as the foundation for shared folders and clipboard.

**Files:** `src/kernel/drivers/vbox_hgcm.c` (new), `include/kernel/drivers/vbox_hgcm.h` (new)

- [ ] Define `hgcm_param_t { type: uint32/uint64/ptr; union { u32, u64, { ptr, size } }; }` and `hgcm_call_t { client_id, function, param_count, params[] }`
- [ ] `hgcm_connect(service_name, &client_id)` -- issue `VBOX_REQUEST_HGCM_CONNECT (60)`; service location type = `HGCM_LOC_NAMED`; return assigned `client_id`
- [ ] `hgcm_disconnect(client_id)` -- issue `VBOX_REQUEST_HGCM_DISCONNECT (61)`
- [ ] `hgcm_call(client_id, function, params, param_count)` -- issue `VBOX_REQUEST_HGCM_CALL (62)`; wait for completion event (VBox signals IRQ then sets `request.rc`)
- [ ] Handle async completion: HGCM calls may complete asynchronously (`VINF_HGCM_ASYNC_EXECUTE`); poll/wait on `request.rc` with timeout
- [ ] Commit: `"drivers: VBox HGCM client -- CONNECT/DISCONNECT/CALL, param encoding, async completion"`

## 6. VBox Shared Folders `[Sonnet]`

Use HGCM to connect to `"VBoxSharedFolders"`, enumerate mappings, map the first share, and expose it as a VFS drive at `H:\`. Implement the file operations: `CreateFile`, `ReadFile`, `WriteFile`, `ListDir`.

**Files:** `src/kernel/drivers/vbox_sf.c` (new), `include/kernel/drivers/vbox_sf.h` (new)

> [!NOTE]
> → XREF: `05-storage-filesystems` domain -- VFS mount-point API (`vfs_mount(letter, ops)`) must be used to register the `H:\` drive; this section wires HGCM calls to a `vfs_ops_t` backend.

- [ ] `vbox_sf_init()`: `hgcm_connect("VBoxSharedFolders", &client_id)`; call `QueryMappings` → get list of share names; call `QueryMapName` on first share; call `MapFolder` → receive `folder_handle`
- [ ] Implement `vfs_ops_t vbox_sf_ops { .open, .read, .write, .close, .readdir }` backed by HGCM `CreateFile`/`ReadFile`/`WriteFile`/`ListDir` function codes
- [ ] `vfs_mount('H', &vbox_sf_ops)` to register drive
- [ ] `ls H:\` in shell enumerates host directory; `type H:\readme.txt` reads file content
- [ ] Boot log: `[VBOX] Shared folder "%s" mounted as H:\`; if none available: `[VBOX] No shared folders configured`
- [ ] Commit: `"drivers: VBox shared folders -- HGCM QueryMappings/MapFolder, VFS H:\\ mount"`

## 7. VBox Shared Clipboard `[Sonnet]`

Connect to `"VBoxSharedClipboard"` via HGCM. Support bidirectional plain-text clipboard synchronization: guest reads host text and host can read guest text.

**Files:** `src/kernel/drivers/vbox_clipboard.c` (new), `include/kernel/drivers/vbox_clipboard.h` (new)

- [ ] `vbox_clipboard_init()`: `hgcm_connect("VBoxSharedClipboard", &client_id)`; send `GUEST_CONNECTED`; report supported formats (text `CF_UNICODETEXT`)
- [ ] `vbox_clipboard_set_text(text, len)`: announce `GUEST_REPORT_FORMATS(CF_UNICODETEXT)` to host; on `HOST_REQUEST_FORMATS`, respond with `GUEST_DATA(CF_UNICODETEXT, text)`
- [ ] `vbox_clipboard_get_text(buf, max_len)`: send `GUEST_REQUEST_FORMATS` to host; read `HOST_DATA(CF_UNICODETEXT, ...)`; copy to `buf`
- [ ] Register as clipboard backend: `clipboard_register_backend(&vbox_clipboard_ops)` (kernel clipboard API)
- [ ] Commit: `"drivers: VBox shared clipboard -- HGCM bidirectional text, CF_UNICODETEXT"`

## 8. VirtIO GPU Display Resize + Page Flip `[Sonnet]`

Drive the VirtIO GPU device (`PCI 1AF4:1050`) through its control queue: `GET_DISPLAY_INFO` on startup, `RESOURCE_CREATE_2D` → `SET_SCANOUT` → `TRANSFER_TO_HOST_2D` → `RESOURCE_FLUSH` for page flip. Handle resolution-change events. Provide hardware cursor via cursor queue.

**Files:** `src/kernel/drivers/virtio_gpu.c` (new), `include/kernel/drivers/virtio_gpu.h` (new)

> [!NOTE]
> VirtIO GPU uses two queues: controlq (control/display commands) and cursorq (cursor position/image updates). Both follow the standard VirtIO split-ring descriptor pattern from the existing `virtio.c` core.

- [ ] Detect `PCI 0x1AF4:0x1050`; initialize two VirtIO queues: `controlq` (index 0) and `cursorq` (index 1)
- [ ] `virtio_gpu_init()`: issue `GET_DISPLAY_INFO`; store `scanout_id`, `width`, `height`; call `wm_display_resized(w, h)`
- [ ] `virtio_gpu_create_fb(resource_id, width, height)`: `RESOURCE_CREATE_2D (type=BGRA8888)` then `RESOURCE_ATTACH_BACKING` with framebuffer physical pages
- [ ] `virtio_gpu_page_flip()`: `TRANSFER_TO_HOST_2D` (mark dirty region) + `RESOURCE_FLUSH` (push to screen); call at vsync tick
- [ ] `SET_SCANOUT`: wire the created resource to `scanout_id 0`
- [ ] Hardware cursor: `UPDATE_CURSOR` on cursorq with 64×64 ARGB image; `MOVE_CURSOR` on pointer movement
- [ ] Handle `VIRTIO_GPU_EVENT_DISPLAY` (bit 0 in `events_read` config field): re-issue `GET_DISPLAY_INFO`, reconfigure resource, notify WM
- [ ] Implement `hv_ops.display_resize` for VirtIO GPU: destroy old resource, create new, re-bind scanout
- [ ] Boot log: `[VIRTIO-GPU] Display %ux%u, scanout_id=%u`
- [ ] Commit: `"drivers: VirtIO GPU -- GET_DISPLAY_INFO, RESOURCE_CREATE/SET_SCANOUT/FLUSH, hardware cursor"`

## 9. VirtIO 9P Shared Folders `[Sonnet]`

Attach to the VirtIO 9P device (PCI transport, subsystem 9) and implement 9P2000.L: `version`/`attach`/`walk`/`open`/`read`/`write`/`stat`/`clunk`. Mount at `I:\` via VFS. Compatible with QEMU `-virtfs local,path=...`.

**Files:** `src/kernel/drivers/virtio_9p.c` (new), `include/kernel/drivers/virtio_9p.h` (new)

- [ ] Detect VirtIO device with subsystem ID 9 (filesystem); initialize one `requestq` VirtIO queue
- [ ] 9P message framing: `{ uint32_t size, uint8_t type, uint16_t tag, payload[] }`; implement `p9_send(msg)` / `p9_recv(tag)` over virtio queue
- [ ] `Tversion(msize=65536, version="9P2000.L")` → `Rversion`; negotiate max message size
- [ ] `Tattach(fid=0, afid=NOFID, uname="root", aname="")` → `Rattach` (get root qid)
- [ ] `Twalk(fid, newfid, wnames[])` → `Rwalk` for path traversal; `Topen` / `Tcreate`; `Tread` / `Twrite`; `Tstat` / `Twstat`; `Tclunk` on close
- [ ] Implement `vfs_ops_t virtio_9p_ops` wired to above; `vfs_mount('I', &virtio_9p_ops)`
- [ ] Boot log: `[9P] VirtIO-9P mounted as I:\\` if device present; skip silently if absent
- [ ] Commit: `"drivers: VirtIO 9P -- 9P2000.L version/attach/walk/read/write, VFS I:\\ mount"`

## 10. Hyper-V Synthetic HID `[Opus]`

Open two VMBus channels (Keyboard VSP and Mouse VSP GUIDs) and parse synthetic HID reports. Inject key and mouse events into the existing keyboard/mouse subsystem so all upper-layer input handling (layout, acceleration, raw grab) works transparently.

**Files:** `src/kernel/drivers/hyperv_hid.c` (new), `include/kernel/drivers/hyperv_hid.h` (new)

> [!IMPORTANT]
> VMBus core is already implemented. This section opens two named channels by GUID. Keyboard VSP GUID: `{F912AD6D-2B17-48EA-BD65-F927A61C7684}`. Mouse VSP GUID: `{CFA8B69E-5B4A-4CC0-B98B-8BA1A1F3F95A}`. Both use the standard VMBus `open_channel` / `read_packet` / `write_packet` interface.

- [ ] `hyperv_hid_init()`: enumerate VMBus channels; open Keyboard VSP channel (GUID match); open Mouse VSP channel (GUID match)
- [ ] Keyboard: negotiate protocol version (send `SYNTH_KBD_PROTOCOL_REQUEST`); receive `SYNTH_KBD_KEYSTROKE` packets; extract `make_code`, `flags` (E0/E1 prefix, break bit); translate to internal `key_event_t` and inject into `keyboard_process_event()`
- [ ] Mouse: negotiate protocol version (send `SYNTH_MOUSE_PROTOCOL_REQUEST`); receive `SYNTH_MOUSE_INPUT_REPORT`; extract `unit_id`, `button_data`, `relative_x`, `relative_y`; inject into `mouse_process_event()`
- [ ] Both channels: handle `VMBus_Channel_Message_Rescind` (hotplug remove); log and gracefully shut down
- [ ] Hyper-V `hv_ops.mouse_available()` returns true when Hyper-V mouse channel is open
- [ ] Boot log: `[HYPERV] Synthetic keyboard channel open`, `[HYPERV] Synthetic mouse channel open`
- [ ] Commit: `"drivers: Hyper-V synthetic HID -- VMBus kbd/mouse VSP channels, keystroke + mouse injection"`

## 11. Hyper-V Synthetic Video `[Opus]`

Open the VMBus Video VSP channel, negotiate a resolution, and map the synthetic framebuffer via shared memory. Send dirty-region notifications to trigger screen updates. Implement `hv_ops.display_resize` for the Hyper-V backend.

**Files:** `src/kernel/drivers/hyperv_video.c` (new), `include/kernel/drivers/hyperv_video.h` (new)

> [!NOTE]
> Hyper-V Video VSP GUID: `{DA0A7802-E377-4AAC-8E77-0558EB1073F8}`. The framebuffer is exposed as a GPA (Guest Physical Address) range in the channel's offer. The video VSP uses a simple request/response protocol for resolution negotiation.

- [ ] `hyperv_video_init()`: locate Video VSP channel by GUID; open channel; send `VIDSYN_NEGOTIATE_VERSION (v3.5)`
- [ ] Resolution negotiation: send `VIDSYN_LOCATION_OFFER`; receive VRAM GPA and size; map into kernel virtual address space via `vmm_map_mmio(gpa, size, PAGE_WRITE|PAGE_NO_CACHE)`
- [ ] Store mapped framebuffer address in `g_fb.base`; update `g_fb.width`, `g_fb.height`, `g_fb.stride`; notify WM: `wm_display_resized(w, h)`
- [ ] Dirty-region flush: `hyperv_video_flush(x, y, w, h)` sends `VIDSYN_POINTER_POSITION_UPDATE` or equivalent dirty-region message to Video VSP; called by compositor on each repaint
- [ ] `hv_ops.display_resize(w, h)`: send `VIDSYN_SCREEN_RESIZE`, renegotiate VRAM mapping
- [ ] Boot log: `[HYPERV] Synthetic video %ux%u @ GPA 0x%llx`
- [ ] Commit: `"drivers: Hyper-V synthetic video -- Video VSP, VRAM mapping, dirty-region flush"`

## 12. VirtIO RNG Guest Entropy

Discover the virtio-rng device (PCI device ID 0x1005), drain host-provided entropy through a single virtqueue, and feed it into the kernel CSPRNG's runtime reseed API. Filed from `01-boot-platform/TODO-12` gap audit: without this, QEMU/KVM guests lacking EFI RNG, TPM, and trusted RDRAND stay degraded even when the host exposes proper entropy.

**Files:** `src/kernel/drivers/virtio/rng.c` (new), reuses `src/kernel/drivers/virtio/virtio.c` core

- [ ] `virtio_rng_init()`: PCI probe for virtio device ID 0x1005; negotiate features; set up the single requestq
- [ ] Request path: post a bounded buffer (64-256 bytes), consume on completion, pass to `csprng_add_entropy(buf, len, quality)` (-> XREF `02-kernel-core/TODO-03-kernel-libraries.md §5`)
- [ ] Rate limit pulls (host fairness) and zero buffers after handoff
- [ ] Source classified as guest hwrng per `01-boot-platform/TODO-12` §1 quality model (post-PCI; never pre-KASLR)
- [ ] Boot log: `[VIRTIO] rng: N bytes seeded to CSPRNG`
- [ ] Commit: `"drivers: virtio-rng guest entropy -- requestq drain into csprng_add_entropy"`

**Test checkpoint:** QEMU with `-device virtio-rng-pci` logs the seed line and the entropy source mask gains the guest-hwrng bit; without the device, init degrades silently (no probe errors). QEMU WHPX, QEMU TCG; N/A on VirtualBox/bare metal (no device).

## OS Comparison


| ⭐   | Feature                                 | 🪟 Win11                                  | 🐧 Linux                                  | 🚀 Impossible OS                          |
| --- | --------------------------------------- | ---------------------------------------- | ---------------------------------------- | ---------------------------------------- |
| 💎   | Hypervisor detection                    | ✅ `hvinfo`; Hyper-V detection built into | ✅ `hypervisor` CPUID leaf; `arch/x86/kernel/cpu/hypervisor.c` | ⬜ §1 -- `HV_NONE/VBOX/KVM/HYPERV` enum, PCI `80EE:CAFE` fallback |
| ⭐   | Unified cross-hypervisor dispatch table | ❌ Per-hypervisor drivers in separate kernel | ❌ No unified abstraction; hypervisor drivers | ⬜ §2 -- single `hv_ops_t` vtable; one `hypervisor_init()` |
| 💎   | VirtualBox display auto-resize          | ✅ `VBoxVideoW8.sys` wddm driver          | ✅ `vboxvideo` kernel module; `drm_mode_set` | ⬜ §4 -- `SetGuestCaps`, DISPLAY_CHANGE IRQ, VBE reconfigure, |
| 💎   | VirtualBox HGCM channel                 | ✅ `VBoxSF.sys` (shared folders), `VBoxTray.exe` (clipboard) | ✅ `vboxsf` module; `vboxguest` IOCTL interface | ⬜ §5 -- HGCM_CONNECT/CALL, param encoding, async completion |
| 💎   | VirtualBox shared folders               | ✅ `\\\\vboxsvr\\share` UNC path; `net use` | ✅ `mount -t vboxsf name /mnt/share`      | ⬜ §6 -- HGCM QueryMappings/MapFolder, VFS `H:\` mount |
| 💎   | VirtualBox shared clipboard             | ✅ `VBoxTray.exe` clipboard integration   | ✅ `vboxclient --clipboard`               | ⬜ §7 -- HGCM `VBoxSharedClipboard`, `CF_UNICODETEXT` bidirectional |
| 💎   | VirtIO GPU display + page flip          | ✅ `viogpu.sys` WDDM display driver       | ✅ `virtio-gpu` DRM driver; `DRM_FORMAT_XRGB8888`, KMS | ⬜ §8 -- RESOURCE_CREATE/SET_SCANOUT/FLUSH, hardware cursor, display events |
| 💎   | VirtIO 9P shared folders                | ❌ Not supported (no VirtIO 9P            | ✅ `9p` kernel module; `mount -t          | ⬜ §9 -- 9P2000.L protocol, `I:\` VFS mount |
| 💎   | Hyper-V synthetic keyboard/mouse        | ✅ `hid-hyperv.sys`; `hyperv-keyboard`; full WM integration | ✅ `hv_kbd.c`, `hv_mouse.c`; evdev injection | ⬜ §10 -- VSP GUIDs, `SYNTH_KBD_KEYSTROKE`, mouse report |
| 💎   | Hyper-V synthetic video                 | ✅ `hypervideo.sys` (pre-Hyper-V Integration Services); `synth | ✅ `hyperv_fb.c`; `fbdev` interface; dirty-region flush | ⬜ §11 -- Video VSP GUID, VRAM GPA        |
| ⭐   | Bare-metal null backend                 | ❌ HAL hardcodes bare-metal vs. guest     | ❌ Hypervisor modules loaded/not loaded by | ⬜ §3 -- `hv_none_ops` struct; bare-metal boot is |

> **After §1–11:** Impossible OS targets all four environments (VirtualBox, QEMU/KVM, Hyper-V, bare metal) with a single kernel binary and no `#ifdef` or `if/else` sprawl. The unified `hv_ops_t` dispatch table (§2) and null backend (§3) are exclusive to Impossible OS -- neither Linux nor Windows exposes a clean cross-hypervisor abstraction behind a single vtable in the kernel core. Linux users on Hyper-V, VirtualBox, and KVM each configure separate kernel modules; Impossible OS makes all three first-class targets behind one interface.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU bare-metal (no `-machine type=q35 -accel kvm`): boot log shows `[HV] Hypervisor: None (bare metal)`; desktop reaches GUI
- [ ] QEMU + KVM: boot log shows `[HV] Hypervisor: KVM`; `hypervisor_type()` == `HV_KVM`
- [ ] VirtualBox: boot log shows `[HV] Hypervisor: VirtualBox`; resizing the VM window triggers `[VBOX] Display resized to WxH` and redraws at new resolution
- [ ] VirtualBox shared folders: configure a host folder in VM settings; boot log shows `[VBOX] Shared folder "... " mounted as H:\\`; `ls H:\` lists host directory contents
- [ ] VirtualBox clipboard: paste text from host → appears in guest terminal; copy text in guest → paste in host
- [ ] QEMU VirtIO GPU (`-device virtio-gpu-pci`): boot log shows `[VIRTIO-GPU] Display %ux%u`; resize via `qemu-monitor` triggers WM reflow
- [ ] QEMU VirtIO 9P (`-virtfs local,path=/tmp/share,...`): boot log shows `[9P] VirtIO-9P mounted as I:\\`; `ls I:\` lists `/tmp/share`
- [ ] Hyper-V Gen 2 VM: boot log shows `[HYPERV] Synthetic keyboard channel open` + `[HYPERV] Synthetic mouse channel open`; typing in terminal works; mouse moves correctly
- [ ] No hypervisor `if/else` chains remain in `kernel/main.c` (grep check: `rg "HV_VBOX\|HV_KVM\|HV_HYPERV" src/kernel/main.c` → 0 matches outside `hv/` directory)
- [ ] Commit: `"drivers: hypervisor abstraction -- hv_ops_t, VBox (resize/HGCM/SF/clipboard), VirtIO (GPU/9P), Hyper-V (HID/video), null backend"`
