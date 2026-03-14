# Phase 08 — Hardware Drivers

> **Goal:** Extend hardware support beyond the basic PS/2 and RTL8139 drivers:
> add a full audio subsystem (sound card driver, mixer, codec libraries, system sounds),
> a USB host controller stack with device class drivers, and additional hardware
> support for a complete desktop experience.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1–2. Audio System

> **Moved to [TODO-P0006-Audio.md](TODO-P0006-Audio.md)** — AC97 driver, audio
> abstraction layer, mixer, codec libraries (WAV/MP3/OGG/FLAC/MIDI), and
> unified audio loader.

---

## 3. USB Support

### 3.1 USB Core

**Prompt:** USB Core defines the data structures and enumeration logic shared by all USB host controllers. Define `struct usb_device` (address, speed, descriptors, endpoints, class driver), plus standard USB descriptor structs (device, config, interface, endpoint). Implement the USB enumeration sequence: reset device on port → assign address (SET_ADDRESS) → read device descriptor (GET_DESCRIPTOR) → read configuration descriptor → set configuration (SET_CONFIGURATION). After enumeration, match the device's class/subclass/protocol to a registered class driver (HID, Mass Storage, etc.). After completing all items, create `docs/architecture/usb.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: USB core and enumeration"`.


- [ ] Create `src/kernel/drivers/usb/usb_core.c` and `include/usb.h`
- [ ] Define USB data structures:
  - [ ] `struct usb_device` (address, speed, descriptor, endpoints, driver)
  - [ ] `struct usb_device_descriptor` (vendor/product ID, class, protocol, max_packet)
  - [ ] `struct usb_config_descriptor`, `struct usb_interface_descriptor`, `struct usb_endpoint_descriptor`
- [ ] Implement USB control transfer (SETUP + DATA + STATUS)
- [ ] Implement USB device enumeration:
  - [ ] Reset device on port
  - [ ] Assign address (SET_ADDRESS)
  - [ ] Read device descriptor (GET_DESCRIPTOR)
  - [ ] Read configuration descriptor
  - [ ] Set configuration (SET_CONFIGURATION)
- [ ] Match device class/subclass → load appropriate class driver
- [ ] Commit: `"drivers: USB core and enumeration"`

### 3.2 xHCI Host Controller Driver

**Prompt:** xHCI (USB 3.0) is the modern USB host controller found in all current hardware. Detect via PCI class 0x0C, subclass 0x03, prog_if 0x30. Map BAR0 for MMIO registers (capability, operational, runtime, doorbell arrays). Initialization: halt controller, reset, allocate DCBAA (Device Context Base Address Array), command ring, and event ring segments, set Max Slots Enabled, then start the controller. Handle port status change events for device attach/detach. Implement slot allocation, address device, and configure endpoint commands via the command ring. Transfers use per-endpoint Transfer Rings with TRBs (Transfer Request Blocks). Test with QEMU `-device qemu-xhci -device usb-kbd`. After completing all items, update `docs/architecture/usb.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: xHCI USB 3.0 host controller"`.


- [ ] Create `src/kernel/drivers/usb/xhci.c` and `include/xhci.h`
- [ ] Detect xHCI controller via PCI (class `0x0C`, subclass `0x03`, prog_if `0x30`)
- [ ] Map MMIO BAR0 registers (capability, operational, runtime, doorbell)
- [ ] Initialize:
  - [ ] Halt controller, reset
  - [ ] Allocate Device Context Base Address Array (DCBAA)
  - [ ] Allocate Command Ring, Event Ring
  - [ ] Set Max Slots Enabled
  - [ ] Start controller (run bit)
- [ ] Handle port status change events → device attach/detach
- [ ] Implement slot allocation, address device, configure endpoint
- [ ] Implement control, bulk, interrupt transfers via Transfer Rings
- [ ] Handle xHCI IRQ → process event ring
- [ ] QEMU flag: `-device qemu-xhci`
- [ ] Test: detect a USB device connected in QEMU
- [ ] Commit: `"drivers: xHCI USB 3.0 host controller"`

### 3.3 USB HID Driver (Keyboard + Mouse)

**Prompt:** USB HID (Human Interface Device) class 0x03 covers keyboards and mice. Match HID class devices during USB enumeration. Set up an interrupt IN endpoint for periodic reports. USB keyboard reports are 8 bytes: byte 0 = modifier keys (Ctrl/Shift/Alt/GUI), byte 1 = reserved, bytes 2-7 = up to 6 simultaneous keycodes. Convert USB HID keycodes (different from PS/2 scancodes) to the keyboard subsystem's scancode format. USB mouse reports contain button states + X/Y delta + wheel delta. Inject events into the existing keyboard/mouse subsystems so all apps work transparently. After completing all items, update `docs/architecture/usb.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: USB HID keyboard and mouse"`.


- [ ] Create `src/kernel/drivers/usb/usb_hid.c`
- [ ] Match HID class devices (class `0x03`)
- [ ] Parse HID report descriptor (simplified — handle standard keyboard/mouse)
- [ ] USB keyboard:
  - [ ] Set up interrupt IN endpoint for key reports
  - [ ] Parse 8-byte keyboard report: modifier keys + 6 key codes
  - [ ] Convert USB HID keycodes to scancodes → inject into keyboard subsystem
- [ ] USB mouse:
  - [ ] Parse mouse report: buttons + X/Y delta + wheel
  - [ ] Inject into mouse subsystem
- [ ] QEMU: `-device usb-kbd -device usb-mouse`
- [ ] Test: type and move mouse using USB HID devices
- [ ] Commit: `"drivers: USB HID keyboard and mouse"`

### 3.4 USB Mass Storage Driver

**Prompt:** USB Mass Storage class 0x08, subclass 0x06 (SCSI), protocol 0x50 (Bulk-Only Transport). Commands are wrapped in 31-byte CBW (Command Block Wrapper) structs sent via bulk OUT endpoint. Data transfers use bulk IN/OUT. Status is received as a 13-byte CSW (Command Status Wrapper) via bulk IN. SCSI commands: INQUIRY (0x12, identify device), READ CAPACITY (0x25, get size), READ(10) (0x28, read sectors), WRITE(10) (0x2A, write sectors). Register the USB drive as a block device, trigger partition scanning and auto-mount with a drive letter. Test with QEMU `-device usb-storage,drive=usb0`. After completing all items, update `docs/architecture/usb.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: USB mass storage (flash drives)"`.


- [ ] Create `src/kernel/drivers/usb/usb_msc.c`
- [ ] Match mass storage class (class `0x08`, subclass `0x06`, protocol `0x50` = Bulk-Only)
- [ ] Implement Bulk-Only Transport protocol:
  - [ ] CBW (Command Block Wrapper) → send SCSI command via bulk OUT
  - [ ] Data phase → bulk IN/OUT
  - [ ] CSW (Command Status Wrapper) → read status via bulk IN
- [ ] SCSI commands:
  - [ ] INQUIRY — identify device
  - [ ] READ CAPACITY — get size
  - [ ] READ(10) — read sectors
  - [ ] WRITE(10) — write sectors
- [ ] Register as block device → auto-mount with drive letter
- [ ] QEMU: `-drive file=usb.img,if=none,id=usb0 -device usb-storage,drive=usb0`
- [ ] Test: read files from USB drive image
- [ ] Commit: `"drivers: USB mass storage (flash drives)"`

### 3.5 USB Hub Support

**Prompt:** USB hubs (class 0x09) enumerate downstream ports and allow cascaded device connections. Read the hub descriptor for port count, then poll port status changes. When a new device is detected on a hub port, power it, wait for reset, then run the standard USB enumeration sequence. This is a stretch goal — most QEMU testing uses direct device connections without hubs. After completing all items, update `docs/architecture/usb.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: USB hub support"`.


- [ ] *(Stretch)* Detect USB hub devices (class `0x09`)
- [ ] *(Stretch)* Enumerate downstream ports
- [ ] *(Stretch)* Handle hub port status changes → enumeration of cascaded devices
- [ ] Commit: `"drivers: USB hub support"`

---

## 4. Agent-Recommended Additions

> Items not in the research files but important for complete hardware support.

### 4.1 Intel HDA Sound Driver

**Prompt:** Intel HDA (High Definition Audio) is the modern audio standard, more complex than AC97. Detect via PCI class 0x04, subclass 0x03. Map MMIO registers, initialize CORB (Command Output Ring Buffer) and RIRB (Response Input Ring Buffer) for codec communication. Enumerate codecs on the HDA link, parse the widget tree (AFG → mixer → DAC → output pin) to find the audio output path. Set up a DMA stream descriptor for PCM playback. QEMU: `-device intel-hda -device hda-duplex`. This is a stretch goal since AC97 covers QEMU testing. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: Intel HDA audio"`.


- [ ] *(Stretch)* Create `src/kernel/drivers/hda.c`
- [ ] *(Stretch)* Detect Intel HDA via PCI (class `0x04`, subclass `0x03`)
- [ ] *(Stretch)* Map MMIO registers, initialize CORB/RIRB (command/response buffers)
- [ ] *(Stretch)* Enumerate codecs, parse widget tree, configure DAC path
- [ ] *(Stretch)* DMA stream setup for PCM playback
- [ ] *(Stretch)* QEMU: `-device intel-hda -device hda-duplex`
- [ ] Commit: `"drivers: Intel HDA audio"`

### 4.2 Media Player App

**Prompt:** The Media Player is the primary audio playback app. Load files via `audio_load()` (unified loader from §2.6). Transport controls: Play/Pause toggle, Stop, Previous/Next track using the button widget from Phase 05 §1.1. A seek bar (slider widget from Phase 05 §1.4) shows playback progress and allows seeking. Volume slider with mute toggle. Display song title from filename (or ID3 metadata if parser is implemented). Register file associations for .mp3/.wav/.ogg so double-clicking opens in the media player. After completing all items, create `docs/user/media-player.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"apps: Media Player"`.


- [ ] Create `src/apps/mediaplayer/mediaplayer.c`
- [ ] Play audio files: WAV, MP3, OGG
- [ ] UI: play/pause/stop buttons, progress bar, volume slider
- [ ] Playlist: add files, next/previous track
- [ ] File association: double-click `.mp3` / `.wav` → opens in media player
- [ ] Album art display (from embedded image or folder `cover.jpg`)
- [ ] Commit: `"apps: Media Player"`

### 4.3 Volume Popup (System Tray)

**Prompt:** Click the 🔊 speaker icon in the system tray (Phase 04 §3.2) to show a volume slider popup. Dragging the slider calls `audio_set_volume()` in real-time. Include a mute toggle button. Hardware volume keys (Volume Up/Down on keyboard) adjust volume by 5% increments and briefly show a volume OSD (On-Screen Display) — a small overlay near the system tray that fades out after 2 seconds. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"desktop: volume control popup"`.


- [ ] Click 🔊 tray icon → volume slider popup
- [ ] Drag slider → `audio_set_volume()` in real-time
- [ ] Mute toggle button
- [ ] Volume UP / DOWN keyboard keys → adjust volume
- [ ] Show volume OSD briefly when keys pressed
- [ ] Commit: `"desktop: volume control popup"`

### 4.4 Sound Settings Applet

**Prompt:** The `sound.spl` applet in the Settings Panel (Phase 05 §4 SPL framework) provides persistent audio configuration. Master volume slider (writes `HKLM\SYSTEM\Sound\Volume` to Registry), mute toggle, output device selector (dropdown, if multiple audio devices are detected), system sounds enable/disable toggle, and a "Test Sound" button that plays a short test tone. This applet uses the same audio abstraction API as the media player. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"apps: sound settings applet"`.


- [ ] `sound.spl` in Settings Panel:
  - [ ] Master volume slider
  - [ ] Mute toggle
  - [ ] Output device selector (if multiple audio devices)
  - [ ] System sounds enable/disable
  - [ ] Test sound button (play a test tone)
- [ ] Commit: `"apps: sound settings applet"`

### 4.5 Hot-Plug Event System

**Prompt:** When xHCI detects a port status change (device attach/detach), send a kernel notification event. The desktop toasts system (Phase 04 §6.3) shows "USB drive detected — D:\\ (8.0 GB, FAT32)" or "USB keyboard connected". Safe removal: add a system tray eject icon. Clicking "Safely eject D:\\" flushes all dirty buffers to the device, unmounts it, then shows "Safe to remove". This requires the USB Mass Storage driver from §3.4 and the block device layer from Phase 06. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: USB hot-plug notifications"`.


- [ ] Kernel notification on USB device attach/detach
- [ ] Desktop toast: "USB drive detected — D:\ (8.0 GB, FAT32)"
- [ ] Desktop toast: "USB keyboard connected"
- [ ] Safe removal: system tray icon → "Safely eject D:\"
  - [ ] Flush writes, unmount, notify user
- [ ] Commit: `"kernel: USB hot-plug notifications"`

### 4.6 PS/2 ↔ USB Fallback

**Prompt:** If USB HID keyboard/mouse are detected, prefer them over PS/2 input. If no USB HID devices are found, fall back to the current PS/2 drivers. The keyboard and mouse subsystems should abstract the input source so applications don't need to know whether input comes from PS/2 or USB. This is a seamless transition handler. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: PS/2 ↔ USB input fallback"`.


- [ ] If USB keyboard/mouse detected, prefer USB input over PS/2
- [ ] If no USB HID, fall back to PS/2 (current default)
- [ ] Seamlessly switch input source without application changes
- [ ] Commit: `"drivers: PS/2 ↔ USB input fallback"`

### 4.7 EHCI/UHCI Fallback (Legacy USB)

**Prompt:** EHCI (USB 2.0, PCI prog_if 0x20) and UHCI (USB 1.1, PCI prog_if 0x00) are legacy USB host controllers. EHCI uses async and periodic schedules with Queue Head/Transfer Descriptor structures. UHCI uses a 1024-entry frame list with Transfer Descriptor chains. These are stretch goals for compatibility with older hardware — xHCI covers all modern systems and QEMU. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: EHCI/UHCI legacy USB host controllers"`.


- [ ] *(Stretch)* Create `src/kernel/drivers/usb/ehci.c` — USB 2.0 host controller
- [ ] *(Stretch)* Detect via PCI (class `0x0C`, subclass `0x03`, prog_if `0x20`)
- [ ] *(Stretch)* Async + periodic schedule, QH/TD structures
- [ ] *(Stretch)* Create `src/kernel/drivers/usb/uhci.c` — USB 1.1 host controller
- [ ] *(Stretch)* Detect via PCI (prog_if `0x00`)
- [ ] *(Stretch)* Frame list + TD chain
- [ ] Commit: `"drivers: EHCI/UHCI legacy USB host controllers"`

---

## 5. Advanced Memory Management

> Current system: 2 MiB fixed heap (`kmalloc`) + PMM (`pmm_alloc_contiguous`) for everything else.
> These items modernize the kernel allocator to eliminate the heap size limitation.

### 5.1 SLAB Allocator

**Prompt:** The SLAB allocator creates pre-sized object caches for frequently-allocated kernel structures. Each cache holds fixed-size slots (e.g., 128-byte `vfs_node_t`, 256-byte `task_t`, 64-byte `reg_value_t`). New slabs are allocated from PMM on demand. Allocation is O(1) — pop from a free list. Benefits: zero internal fragmentation, cache-line friendly, no general-purpose heap overhead. Linux uses SLUB (a simplified SLAB variant). Create `slab_cache_create(name, obj_size)`, `slab_alloc(cache)`, `slab_free(cache, ptr)`. Migrate VFS nodes, task structs, and Registry values off the general heap. After completing all items, create `docs/architecture/slab-allocator.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"mm: SLAB allocator"`.


- [ ] Create `src/kernel/mm/slab.c` and `include/kernel/mm/slab.h`
- [ ] Implement `slab_cache_create(name, obj_size, align)` — create a new object cache
- [ ] Implement `slab_alloc(cache)` — O(1) allocation from free list
- [ ] Implement `slab_free(cache, ptr)` — return object to free list
- [ ] Auto-grow: allocate new slab pages from PMM when cache is exhausted
- [ ] Migrate `vfs_node_t` allocations to SLAB cache
- [ ] Migrate `task_t` allocations to SLAB cache
- [ ] Migrate `reg_value_t` / `reg_key_t` to SLAB cache
- [ ] Debug: `/proc/slabinfo`-style stats (cache name, active, total, slab pages)
- [ ] Commit: `"mm: SLAB allocator"`

### 5.2 vmalloc — Virtual Contiguous Allocator

**Prompt:** `vmalloc(size)` allocates virtually contiguous memory from scattered physical pages. Unlike `pmm_alloc_contiguous()` (which needs physically contiguous frames), vmalloc maps arbitrary free frames into a reserved virtual address range (e.g., `0xFFFF_C000_0000_0000` to `0xFFFF_C000_FFFF_FFFF`). This is ideal for large kernel buffers that don't need DMA (which requires physical contiguity). Requires the page table manager to map individual frames. Linux uses vmalloc for module loading, large hash tables, and iptables rules. After completing all items, update memory architecture docs, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"mm: vmalloc virtual allocator"`.


- [ ] Create `src/kernel/mm/vmalloc.c` and `include/kernel/mm/vmalloc.h`
- [ ] Reserve virtual address range for vmalloc mappings
- [ ] Implement `vmalloc(size)` — allocate scattered PMM frames, map contiguously
- [ ] Implement `vfree(ptr)` — unmap pages, free frames back to PMM
- [ ] Track vmalloc regions (start addr → size + frame list)
- [ ] Commit: `"mm: vmalloc virtual allocator"`

### 5.3 Growable Heap

**Prompt:** Replace the fixed 2 MiB `heap_pool` array with a dynamically growable heap. Start with a small initial allocation (e.g., 256 KB). When `kmalloc` cannot satisfy a request, map additional PMM frames into the heap's virtual address range and extend the free list. This eliminates the hard 2 MiB ceiling while keeping the familiar `kmalloc`/`kfree` API. Requires vmalloc (§5.2) or direct page table manipulation. Guard against unbounded growth with a configurable max heap size. After completing all items, update memory architecture docs, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"mm: growable kernel heap"`.


- [ ] Modify `src/kernel/mm/heap.c` to support dynamic growth
- [ ] Start with 256 KB initial heap, grow on demand
- [ ] On `kmalloc` failure: request new pages from PMM, map into heap range
- [ ] Configurable max heap size (default: 16 MiB) to prevent runaway growth
- [ ] Boot log: report initial and current heap size
- [ ] Commit: `"mm: growable kernel heap"`

---

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | **P0006** §1–5 | Audio system (see `TODO-P0006-Audio.md`) |
| 🟠 P1 | §3.1 USB Core | Foundation for all USB devices |
| 🟡 P2 | §3.2 xHCI Controller | USB 3.0 host — enables all USB devices |
| 🟡 P2 | §3.3 USB HID | USB keyboard + mouse |
| 🟡 P2 | §4.2 Media Player | Audio playback app |
| 🟡 P2 | §4.3 Volume Popup | Essential UX |
| 🟡 P2 | §5.1 SLAB Allocator | Eliminates heap pressure for kernel objects |
| 🟢 P3 | §3.4 USB Mass Storage | USB flash drive support |
| 🟢 P3 | §4.5 Hot-Plug Events | USB attach/detach notifications |
| 🟢 P3 | §4.4 Sound Settings | Settings applet |
| 🟢 P3 | §5.2 vmalloc | Large kernel buffers without physical contiguity |
| 🟢 P3 | §5.3 Growable Heap | Eliminates fixed heap size ceiling |
| 🔵 P4 | §4.1 Intel HDA | Modern hardware |
| 🔵 P4 | §3.5 USB Hub | Cascaded USB devices |
| 🔵 P4 | §4.7 EHCI/UHCI | Legacy USB support |
