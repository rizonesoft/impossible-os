# P0002 — Mouse & Input Drivers

> **Goal:** Build production-grade mouse input for Impossible OS — from PS/2 basics to
> USB HID, scroll wheels, configurable acceleration, touchpad support, and hypervisor
> integration (VBox, QEMU, Hyper-V). Every feature except Synaptics/ALPS touchpad can
> be developed and tested entirely inside VirtualBox before deploying to real hardware.

> [!IMPORTANT]
> **Known Gotcha — PMM vs kmalloc:** Mouse/input buffers and DMA descriptors must use
> `pmm_alloc_contiguous()`, not `kmalloc()`. The kernel heap is only 2 MiB. See rules.md.

> [!TIP]
> **Testing Strategy:** Use VirtualBox as the primary test platform. VBox emulates PS/2
> Intellimouse (scroll + 5 buttons), OHCI/EHCI USB controllers, and the VMMDev absolute
> mouse. Only Synaptics/ALPS touchpad drivers require real laptop hardware.

---

## 1. PS/2 Mouse — Basic Relative Mode ✅

### 1.1 PS/2 Mouse Driver

**Prompt:** This section is marked complete. Verify that `src/kernel/drivers/mouse.c` implements the PS/2 mouse driver with 3-byte packet protocol (status, Δx, Δy), IRQ 12 handler, delta clamping, and screen-bounds clamping. Confirm `mouse_init()`, `mouse_get_state()`, and `mouse_set_position()` are implemented. Run `bash scripts/build.sh clean` and verify mouse movement in QEMU/VBox. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to PS/2 mouse input. Add notes, gotchas, and design decisions directly in this TODO section covering the PS/2 mouse driver architecture, 3-byte packet format, and IRQ handling.

- [x] Create `src/kernel/drivers/mouse.c` and `include/kernel/drivers/mouse.h`
- [x] Implement PS/2 mouse initialization (reset, enable data reporting)
- [x] Implement 3-byte packet parsing (status byte, Δx, Δy)
- [x] IRQ 12 handler for mouse data
- [x] Delta clamping (±127 per axis)
- [x] Screen-bounds clamping (0 to fb_width/fb_height)
- [x] `mouse_get_state()` — return current x, y, buttons
- [x] `mouse_set_position()` — allow external drivers to override position

---

## 2. VirtualBox Absolute Mouse ✅

### 2.1 VBoxGuest VMMDev Driver

**Prompt:** This section is marked complete. Verify that `src/kernel/drivers/vbox_mouse.c` discovers PCI device `80EE:CAFE`, initializes VMMDev protocol v1.03, enables absolute mouse coordinates via `SetMouse` with `GUEST_CAN_ABSOLUTE | NEW_PROTOCOL | GUEST_NEEDS_HOST_CURSOR` flags, and receives position updates via IRQ handler. Confirm the 3-tier priority in `main.c`: VirtIO > VBox > PS/2. Verify buttons come from PS/2 (VMMDev does NOT provide buttons). Run `bash scripts/build.sh clean` and test in VirtualBox. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to VBox mouse integration. Add notes, gotchas, and design decisions directly in this TODO section covering the VMMDev protocol, PCI device discovery, and IRQ-based coordinate delivery.

- [x] Create `src/kernel/drivers/vbox_mouse.c` and `include/kernel/drivers/vbox_mouse.h`
- [x] PCI discovery: vendor `0x80EE`, device `0xCAFE`
- [x] BAR0 = I/O port, BAR1 = MMIO region (vmmdevmem)
- [x] Send `GuestInfo` packet (protocol v1.03, OS type = unknown)
- [x] Send `SetMouse` with absolute + new protocol + needs-host-cursor flags
- [x] IRQ handler: acknowledge events, poll `GetMouse`, scale 0–0xFFFF → screen pixels
- [x] `vbox_mouse_get_state()` returns IRQ-cached position (no polling)
- [x] `vbox_mouse_available()` returns 1 when VBox device is active
- [x] 3-tier mouse priority in `main.c`: VirtIO tablet → VBox VMMDev → PS/2
- [x] Buttons merged from PS/2 (VMMDev does not provide button state)
- [x] Commit: `"input: VBox absolute mouse + fix drag edge jump"` (`fd410bc`)
- [x] Commit: `"input: fix VBox mouse movement — IRQ-based coords"` (`3d5f08e`)

### 2.2 VirtIO Tablet (QEMU) ✅

**Prompt:** This section is marked complete. Verify that `src/kernel/drivers/virtio_input.c` implements the VirtIO input driver for QEMU's `-device virtio-tablet-pci`. Confirm absolute coordinates (0–32767) are scaled to screen resolution. Verify the Makefile includes `-device virtio-tablet-pci` in the QEMU run target. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to VirtIO input or QEMU mouse. Add notes, gotchas, and design decisions directly in this TODO section covering the VirtIO tablet driver and QEMU integration.

- [x] `virtio_input_init()` — discover VirtIO input PCI device
- [x] `virtio_input_get_state()` — return absolute x, y, buttons
- [x] `virtio_input_available()` — return 1 when device is active
- [x] Scale 0–32767 to framebuffer width/height
- [x] Makefile: `-device virtio-tablet-pci` in QEMU flags

---

## 3. Window Manager Drag Fix ✅

### 3.1 Edge Clamp Offset Recalculation

**Prompt:** This section is marked complete. Verify that `src/desktop/wm.c` recalculates the drag offset (`drag_offset_x/y`) after clamping the window to screen edges during drag operations. This prevents the window from jumping when the cursor reverses direction after hitting an edge. After verifying, mark all items as `[x]` and update this prompt to reflect the final verified state for future correctness checks. Update `README.md` if it contains stale or incorrect references to window dragging. Add notes, gotchas, and design decisions directly in this TODO section covering the window manager drag system and edge clamping behavior.

- [x] Recalculate `drag_offset_x = mx - new_x` after edge clamping in `wm_handle_mouse()`
- [x] Recalculate `drag_offset_y = my - new_y` after edge clamping
- [x] Commit: `"input: VBox absolute mouse + fix drag edge jump"` (`fd410bc`)

---

## 4. PS/2 Scroll Wheel (Intellimouse Protocol)

### 4.1 Enable Intellimouse 4-Byte Packets

**Prompt:** Upgrade the PS/2 mouse driver from 3-byte to 4-byte packet mode by sending the magic Intellimouse init sequence (set sample rate 200, 100, 80, then read device ID). If the mouse responds with ID 3, it supports scroll wheel. The 4th byte contains the scroll delta (signed 8-bit: positive = up, negative = down). Update the IRQ handler to parse 4-byte packets when Intellimouse is detected. Dispatch scroll events to the window manager. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, test scroll in VBox, and commit as `"input: PS/2 Intellimouse scroll wheel support"`. Update `README.md` if it contains stale or incorrect references to mouse capabilities. Add notes, gotchas, and design decisions directly in this TODO section covering Intellimouse protocol detection, 4-byte packet format, and scroll event dispatch.

- [ ] Send magic init sequence: set sample rate 200 → 100 → 80
- [ ] Read device ID: if `ID == 3`, Intellimouse detected (4-byte packets)
- [ ] Fall back to standard 3-byte mode if `ID != 3`
- [ ] Update IRQ 12 handler to parse 4th byte as `int8_t scroll_delta`
- [ ] Add `int8_t scroll` field to `struct mouse_state`
- [ ] Dispatch scroll events to WM (vertical scroll on focused window)
- [ ] Log: `"[OK] PS/2 mouse: Intellimouse scroll wheel enabled"`
- [ ] Commit: `"input: PS/2 Intellimouse scroll wheel support"`

### 4.2 Scroll Wheel in Window Manager

**Prompt:** Handle scroll events in the window manager. Dispatch scroll deltas to the focused window's control tree (e.g., scrollable text areas, list views). If no control handles the scroll, the window itself may scroll its client area. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"wm: scroll wheel event dispatch"`. Update `README.md` if it contains stale or incorrect references to scroll support. Add notes, gotchas, and design decisions directly in this TODO section covering scroll event handling in the window manager and control tree dispatch.

- [ ] Add `wm_handle_scroll(int32_t mx, int32_t my, int8_t delta)` to `wm.c`
- [ ] Dispatch to focused window's control handler
- [ ] Support vertical scroll for text areas and list controls
- [ ] Commit: `"wm: scroll wheel event dispatch"`

---

## 5. PS/2 5-Button Mouse (Intellimouse Explorer)

### 5.1 Enable Explorer 5-Byte Packets

**Prompt:** Extend Intellimouse support to detect the Explorer protocol (5 buttons). After enabling Intellimouse (ID 3), send the magic sequence again (sample rate 200, 200, 80, read ID). If the mouse responds with ID 4, it supports 5 buttons and the 4th byte includes both scroll + side button bits. Update the packet parser. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, test in VBox, and commit as `"input: PS/2 Intellimouse Explorer 5-button support"`. Update `README.md` if it contains stale or incorrect references to mouse button support. Add notes, gotchas, and design decisions directly in this TODO section covering Explorer protocol detection, 5-button packet parsing, and side button defines.

- [ ] After Intellimouse init (ID 3), send second magic: sample rate 200 → 200 → 80
- [ ] Read device ID: if `ID == 4`, Explorer mode (5 buttons in 4th byte)
- [ ] Parse 4th byte: bits [3:0] = scroll, bit 4 = button 4, bit 5 = button 5
- [ ] Add `MOUSE_BTN_SIDE` and `MOUSE_BTN_EXTRA` defines
- [ ] Log: `"[OK] PS/2 mouse: Explorer 5-button mode enabled"`
- [ ] Commit: `"input: PS/2 Intellimouse Explorer 5-button support"`

---

## 6. Packet Resynchronization

### 6.1 PS/2 Byte Sync Recovery

**Prompt:** If a PS/2 byte is lost (noisy line, slow IRQ), the driver reads garbage for the next 2 frames. Fix by verifying that byte 0 of each packet always has bit 3 set (PS/2 spec mandates this). If bit 3 is clear, discard bytes until a valid sync byte is found. Add a counter for sync-loss events for diagnostics. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"input: PS/2 packet resynchronization"`. Update `README.md` if it contains stale or incorrect references to mouse reliability. Add notes, gotchas, and design decisions directly in this TODO section covering the PS/2 packet sync recovery algorithm and diagnostic counters.

- [ ] Check bit 3 of byte 0 on every packet boundary
- [ ] If bit 3 is NOT set: discard byte, shift buffer, resume scanning
- [ ] Add `sync_loss_count` diagnostic counter
- [ ] Log: `"[WARN] PS/2 mouse: packet sync lost, recovering"` (first occurrence only)
- [ ] Commit: `"input: PS/2 packet resynchronization"`

---

## 7. Mouse Acceleration & Sensitivity

### 7.1 Acceleration Curves

**Prompt:** Implement mouse acceleration in the input processing layer (between raw deltas and cursor movement). For PS/2 relative mode: apply a non-linear acceleration curve so slow movements are precise and fast movements cover more distance. Use a simple polynomial: `accel_delta = delta * (1.0 + speed * acceleration_factor)` where `speed = sqrt(dx² + dy²)`. Implement using fixed-point integer math (no FPU). Store acceleration settings in the Registry. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"input: mouse acceleration curves"`. Update `README.md` if it contains stale or incorrect references to mouse movement or acceleration. Add notes, gotchas, and design decisions directly in this TODO section covering the acceleration curve algorithm, fixed-point math implementation, and Registry configuration.

- [ ] Create `src/kernel/input/mouse_accel.c` and `include/kernel/input/mouse_accel.h`
- [ ] Implement fixed-point acceleration: `accel = 1.0 + speed * factor` (16.16 fixed point)
- [ ] Apply acceleration to PS/2 deltas before cursor position update
- [ ] Skip acceleration for absolute sources (VirtIO, VBox — already pixel-accurate)
- [ ] Default acceleration factor: 1.5 (configurable via Registry)
- [ ] Commit: `"input: mouse acceleration curves"`

### 7.2 Configurable DPI / Sensitivity

**Prompt:** Add a sensitivity multiplier to the input layer. Sensitivity is a simple linear scale factor applied to mouse deltas: `effective_delta = raw_delta * sensitivity`. Default sensitivity is 1.0 (stored as integer 100 = 1.00×). Range: 0.1× (10) to 3.0× (300). Read from Registry path `HKLM\SYSTEM\Input\MouseSensitivity`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"input: configurable mouse sensitivity"`. Update `README.md` if it contains stale or incorrect references to mouse sensitivity. Add notes, gotchas, and design decisions directly in this TODO section covering the sensitivity multiplier, Registry integration, and valid range.

- [ ] Add sensitivity multiplier (16.16 fixed point)
- [ ] Read from Registry: `HKLM\SYSTEM\Input\MouseSensitivity` (default: 100 = 1.0×)
- [ ] Range: 10 (0.1×) to 300 (3.0×)
- [ ] Apply to both X and Y deltas independently
- [ ] Commit: `"input: configurable mouse sensitivity"`

---

## 8. USB HID Mouse (EHCI/xHCI)

### 8.1 USB Host Controller Driver (EHCI)

**Prompt:** Implement an EHCI (USB 2.0) host controller driver for USB mouse support. EHCI is the bare minimum for modern USB input — most mice use USB 2.0 full-speed. VirtualBox emulates Intel EHCI. Start with bus enumeration, device addressing, and interrupt transfer endpoints. This is a larger subsystem — implement incrementally. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, test with a USB mouse in VBox, and commit as `"usb: EHCI host controller driver"`. Update `README.md` if it contains stale or incorrect references to USB support. Add notes, gotchas, and design decisions directly in this TODO section covering the EHCI controller architecture, PCI discovery, and transfer scheduling.

- [ ] Create `src/kernel/drivers/usb/ehci.c` and headers
- [ ] PCI discovery: class 0x0C, subclass 0x03, prog IF 0x20 (EHCI)
- [ ] Map MMIO capability registers
- [ ] Implement port reset and device addressing
- [ ] Implement async schedule (control transfers)
- [ ] Implement periodic schedule (interrupt transfers)
- [ ] Commit: `"usb: EHCI host controller driver"`

### 8.2 USB HID Mouse Class Driver

**Prompt:** Implement a USB HID class driver for mice. After EHCI enumerates a USB device, check the interface descriptor for class 3 (HID), subclass 1 (boot interface), protocol 2 (mouse). Set the device to boot protocol mode (simpler than report protocol). Read 3-byte boot mouse reports via interrupt transfers: byte 0 = buttons, byte 1 = Δx, byte 2 = Δy. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, test in VBox, and commit as `"usb: HID mouse class driver"`. Update `README.md` if it contains stale or incorrect references to USB mouse support. Add notes, gotchas, and design decisions directly in this TODO section covering the USB HID boot protocol, mouse report parsing, and hot-plug support.

- [ ] Create `src/kernel/drivers/usb/hid_mouse.c`
- [ ] Detect HID boot mouse: class 3, subclass 1, protocol 2
- [ ] Send SET_PROTOCOL(0) to use boot protocol mode
- [ ] Set up interrupt IN transfer for periodic polling (8ms interval)
- [ ] Parse 3-byte boot report: buttons, Δx (signed), Δy (signed)
- [ ] Feed into `mouse_set_position()` (same as PS/2 path)
- [ ] Hot-plug support: detect device attach/detach
- [ ] Commit: `"usb: HID mouse class driver"`

### 8.3 xHCI Host Controller (USB 3.0) — Future

**Prompt:** xHCI (USB 3.0) is needed for modern laptops that don't have EHCI fallback. This is a significantly more complex controller than EHCI (ring-based command/event/transfer architecture). Defer until EHCI is stable. Note: VBox only partially emulates xHCI — test on real hardware or QEMU with `-device qemu-xhci`. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"usb: xHCI host controller driver"`. Update `README.md` if it contains stale or incorrect references to USB 3.0 support. Add notes, gotchas, and design decisions directly in this TODO section covering the xHCI ring architecture and differences from EHCI.

- [ ] Plan xHCI ring architecture (command ring, event ring, transfer rings)
- [ ] PCI discovery: class 0x0C, subclass 0x03, prog IF 0x30 (xHCI)
- [ ] Implement device context management
- [ ] Port EHCI HID mouse driver to work over xHCI transfers
- [ ] Commit: `"usb: xHCI host controller driver"`

---

## 9. Synaptics/ALPS Touchpad — Future

### 9.1 Synaptics Touchpad Driver

**Prompt:** Synaptics touchpads communicate over the PS/2 bus using proprietary extensions. Detect by sending the Synaptics identify command (`0xE8 0x00, 0xE8 0x00, 0xE8 0x00, 0xE8 0x00, 0xE6, 0xE9`) and checking the info bytes. If detected, switch to absolute mode for multi-touch, palm detection, and scroll zones. **Cannot be tested in VBox** — requires a real laptop with Synaptics hardware. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"input: Synaptics touchpad driver"`. Update `README.md` if it contains stale or incorrect references to touchpad support. Add notes, gotchas, and design decisions directly in this TODO section covering the Synaptics PS/2 protocol, detect sequence, absolute mode, and gesture support.

- [ ] Detect Synaptics via PS/2 identify command sequence
- [ ] Switch to Synaptics absolute mode
- [ ] Parse 6-byte Synaptics packets (x, y, pressure, finger width)
- [ ] Implement tap-to-click, two-finger scroll, palm rejection
- [ ] Commit: `"input: Synaptics touchpad driver"`

### 9.2 ALPS Touchpad Driver

- [ ] Detect ALPS via vendor-specific PS/2 commands
- [ ] Parse ALPS packet format (varies by hardware version)
- [ ] Commit: `"input: ALPS touchpad driver"`

---

## 10. Guest Additions / Hypervisor Abstraction

### 10.1 VBoxGuest Extended Features

**Prompt:** Extend the VBoxGuest VMMDev integration beyond mouse. Add display auto-resize (VBox tells the guest the ideal resolution when the user resizes the VBox window), shared clipboard, and shared folders. All use the same PCI device (`80EE:CAFE`) and VMMDev packet protocol. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, run `bash scripts/build.sh clean`, and commit as `"vbox: display resize + shared clipboard"`. Update `README.md` if it contains stale or incorrect references to VBox integration. Add notes, gotchas, and design decisions directly in this TODO section covering VMMDev display resize, HGCM shared clipboard, and shared folder VFS mounting.

- [ ] VBox display auto-resize (`VBOX_REQUEST_GET_DISPLAY_CHANGE = 51`)
  - [ ] Handle display change events in IRQ handler
  - [ ] Switch framebuffer resolution via VBE/VGA mode set
  - [ ] Notify WM to recomposite at new resolution
- [ ] VBox shared clipboard (HGCM service calls)
- [ ] VBox shared folders (HGCM + VFS integration)
- [ ] Commit: `"vbox: display resize + shared clipboard"`

### 10.2 Hypervisor Abstraction Layer

**Prompt:** Create a unified hypervisor abstraction that detects which VM platform we're running on and activates the appropriate backend. The detection uses CPUID leaf 0x40000000 (hypervisor brand string) or PCI device probing. After completing all items, mark every item as `[x]`, update this prompt to a verification prompt that can be used for future correctness checks, and commit as `"hypervisor: abstraction layer with auto-detection"`. Update `README.md` if it contains stale or incorrect references to hypervisor support. Add notes, gotchas, and design decisions directly in this TODO section covering the hypervisor detection algorithm, backend dispatch, and supported platforms.

- [ ] Create `src/kernel/hypervisor/detect.c` — detect VBox / QEMU / Hyper-V / bare metal
- [ ] VirtualBox: VMMDev mouse, display resize, shared folders
- [ ] QEMU/KVM: VirtIO tablet, virtio-gpu, virtio-fs / 9p
- [ ] Hyper-V: VMBus synthetic mouse, synthetic video
- [ ] Bare metal: PS/2 mouse, USB HID, native GPU
- [ ] Common interface: `hv_get_mouse()`, `hv_resize_display()`, `hv_shared_folder_mount()`
- [ ] Commit: `"hypervisor: abstraction layer with auto-detection"`

---

## Priority Order

| Priority | Section               | Description                                        |
|----------|-----------------------|----------------------------------------------------|
| ✅ Done   | 1.1 PS/2 Basic        | 3-byte relative mouse, IRQ 12                      |
| ✅ Done   | 2.1 VBox Mouse        | VMMDev absolute mouse via PCI `80EE:CAFE`          |
| ✅ Done   | 2.2 VirtIO Tablet     | QEMU absolute mouse via virtio-tablet-pci          |
| ✅ Done   | 3.1 Drag Edge Fix     | Offset recalculation after screen edge clamp       |
| 🔴 P0     | 4.1 Scroll Wheel      | Intellimouse 4-byte packets — most-needed feature  |
| 🔴 P0     | 6.1 Packet Resync     | Prevent jitter from lost PS/2 bytes                |
| 🟠 P1     | 5.1 Explorer 5-Button | Side buttons for browser back/forward              |
| 🟠 P1     | 7.1 Acceleration      | Non-linear curves for precise + fast mouse         |
| 🟠 P1     | 7.2 Sensitivity       | Configurable DPI multiplier via Registry           |
| 🟡 P2     | 8.1 USB EHCI          | USB 2.0 host controller — needed for real hardware |
| 🟡 P2     | 8.2 USB HID Mouse     | USB mouse class driver over EHCI                   |
| 🟡 P2     | 10.1 VBox Extended    | Display resize, clipboard, shared folders          |
| 🟢 P3     | 8.3 xHCI              | USB 3.0 — modern laptops without EHCI              |
| 🟢 P3     | 10.2 Hypervisor Layer | Unified VBox/QEMU/Hyper-V abstraction              |
| 🔵 P4     | 9.1 Synaptics         | Laptop touchpad (real hardware only)               |
| 🔵 P4     | 9.2 ALPS              | Laptop touchpad (real hardware only)               |

---

## Feature Comparison vs Other OSes

| Feature                      | Impossible OS | Linux | SerenityOS |
|------------------------------|---------------|-------|------------|
| Basic PS/2 relative          | ✅             | ✅     | ✅          |
| Scroll wheel (4th byte)      | ❌             | ✅     | ✅          |
| 5-button (5th byte)          | ❌             | ✅     | ✅          |
| USB HID mouse (EHCI/xHCI)    | ❌             | ✅     | ✅          |
| Mouse acceleration curves    | ❌             | ✅     | ✅          |
| Configurable DPI/sensitivity | ❌             | ✅     | ✅          |
| Synaptics/ALPS touchpad      | ❌             | ✅     | ❌          |
| Packet resynchronization     | ❌             | ✅     | ✅          |

## VirtualBox Testability

| Feature                      | Testable in VBox? | Notes                                                                                       |
|------------------------------|-------------------|---------------------------------------------------------------------------------------------|
| Scroll wheel (4th byte)      | ✅ Yes             | VBox emulates Intellimouse. Magic init → 4-byte packets. Translates 1:1 to real hardware.   |
| 5-button (5th byte)          | ✅ Yes             | VBox emulates Explorer. Same magic sequence + ID check. Works identically on real hardware. |
| USB HID mouse (EHCI/xHCI)    | ⚠️ Partially      | VBox emulates OHCI/EHCI. USB HID driver works, but real xHCI (USB 3.0) has differences.     |
| Mouse acceleration curves    | ✅ Yes             | Pure software math — no hardware needed. Works everywhere.                                  |
| Configurable DPI/sensitivity | ✅ Yes             | Pure software — apply multiplier to deltas. Works everywhere.                               |
| Synaptics/ALPS touchpad      | ❌ No              | Proprietary PS/2 extensions. VBox doesn't emulate. Must test on real laptop.                |
| Packet resynchronization     | ✅ Yes             | Verify bit 3 of byte 0 (PS/2 spec). Can deliberately corrupt bytes to test recovery.        |

> **Bottom line:** 6 out of 7 features can be fully developed and tested in VBox.
> Only touchpad drivers need real hardware.
