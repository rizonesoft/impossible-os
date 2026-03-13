# Phase 08 — Hardware Drivers

> **Goal:** Extend hardware support beyond the basic PS/2 and RTL8139 drivers:
> add a full audio subsystem (sound card driver, mixer, codec libraries, system sounds),
> a USB host controller stack with device class drivers, and additional hardware
> support for a complete desktop experience.
> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.


---

## 1. Audio System — Sound Card Driver

### 1.1 AC97 Sound Card Driver

**Prompt:** AC97 is the simplest sound card to implement in QEMU. Detect the Intel ICH AC97 controller via PCI class 0x04/subclass 0x01. Map two I/O BARs: the Native Audio Mixer BAR (for codec registers like master volume, PCM out volume) and the Native Audio Bus Master BAR (for DMA control). The Bus Master uses a Buffer Descriptor List (BDL) — a ring of 32 entries, each pointing to a PCM data buffer with length and IOC (Interrupt On Completion) flags. Fill the BDL with PCM audio data, set the BDL base address register, and start playback by setting the run bit. Generate a test sine wave (440 Hz, 16-bit signed, 44100 Hz) to verify audio output. After completing all items, create `docs/architecture/audio.md`, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"drivers: AC97 sound card driver"`.


- [ ] Create `src/kernel/drivers/ac97.c` and `include/ac97.h`
- [ ] Detect AC97 controller via PCI (class `0x04`, subclass `0x01`, or Intel ICH vendor/device)
- [ ] Map I/O BAR (Native Audio Mixer BAR + Native Audio Bus Master BAR)
- [ ] Initialize AC97 codec:
  - [ ] Cold reset via Bus Master control register
  - [ ] Read codec ready status
  - [ ] Set master volume, PCM out volume
- [ ] Configure Bus Master for PCM out:
  - [ ] Allocate DMA buffer (ring of Buffer Descriptor List entries)
  - [ ] Each BDL entry: pointer to PCM data + length + flags (IOC)
  - [ ] Set BDL base address register
- [ ] Implement `ac97_play(pcm_data, samples, sample_rate)` — fill DMA buffers, start playback
- [ ] Implement `ac97_stop()` — halt DMA playback
- [ ] Implement `ac97_set_volume(volume)` — write mixer register (0–100%)
- [ ] Handle AC97 IRQ: buffer completion → refill with next chunk
- [ ] QEMU flag: `-device AC97` (or `-soundhw ac97`)
- [ ] Test: play a short PCM tone (sine wave) to verify audio output
- [ ] Commit: `"drivers: AC97 sound card driver"`

### 1.2 Audio Abstraction Layer

**Prompt:** The audio abstraction layer provides a uniform API over different sound card drivers (AC97 now, Intel HDA later). `struct audio_device` holds the driver name, sample_rate, channels, bits_per_sample, and function pointers for play/stop/volume. `audio_init()` detects available sound hardware and registers the driver. `audio_play(pcm_data, samples, sample_rate)` dispatches to the currently registered driver. Volume is stored in Registry `HKLM\SYSTEM\Sound\Volume` (REG_DWORD, 0-100) and `HKLM\SYSTEM\Sound\Mute` (REG_DWORD). Add `SYS_AUDIO_PLAY` and `SYS_AUDIO_VOLUME` syscalls so user-mode apps can play audio. After completing all items, update `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: audio abstraction layer"`.


- [ ] Create `src/kernel/audio.c` and `include/audio.h`
- [ ] Define `struct audio_device` (name, sample_rate, channels, bits_per_sample, play_fn, stop_fn, volume_fn)
- [ ] Implement `audio_init()` — detect sound card, register driver
- [ ] Implement `audio_play(pcm_data, samples, sample_rate)` — dispatch to driver
- [ ] Implement `audio_set_volume(volume)` — 0–100 scale
- [ ] Implement `audio_get_volume()` — read current volume
- [ ] Implement `audio_is_playing()` — check playback state
- [ ] Store volume in Registry: `HKLM\SYSTEM\Sound\Volume`, `HKLM\SYSTEM\Sound\Mute`
- [ ] Add `SYS_AUDIO_PLAY` and `SYS_AUDIO_VOLUME` syscalls
- [ ] Commit: `"kernel: audio abstraction layer"`

### 1.3 Audio Mixer

**Prompt:** The audio mixer allows multiple sounds to play simultaneously (up to 8 streams). Each stream has its own PCM buffer and per-stream volume. The mixer sums all active streams' samples, applies per-stream volume scaling, then applies master volume. Clamp the mixed output to INT16_MIN/INT16_MAX to prevent clipping distortion. Feed the mixed output to the sound driver's DMA buffer. Mute support: when `HKLM\SYSTEM\Sound\Mute` is set, output silence (zeros) without stopping the mixer. After completing all items, update `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: audio mixer"`.


- [ ] Create `src/kernel/audio_mixer.c`
- [ ] Support multiple simultaneous audio streams (up to 8)
- [ ] Mix streams by summing PCM samples with per-stream volume
- [ ] Clamp mixed output to prevent clipping
- [ ] Master volume applied after mixing
- [ ] Mute support (Registry `HKLM\SYSTEM\Sound\Mute`)
- [ ] Commit: `"kernel: audio mixer"`

### 1.4 System Sounds

**Prompt:** Create WAV system sounds (22050 Hz, mono, 16-bit — small file sizes): startup chime (played after boot splash), button click (tactile feedback), error alert (for error dialogs), notification toast sound, shutdown sound, and recycle bin empty sound. Store in `resources/sounds/` in the source tree, install to `C:\Impossible\Sounds\` on IXFS. Play startup chime after boot splash finishes. Play error sound with error dialogs from Phase 05 §1.3 Message Dialog. Play notification sound with toast notifications from Phase 04 §6.3. Control via Registry `HKLM\SYSTEM\Sound\SystemSounds` (enable/disable). After completing all items, update `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: system sounds"`.


- [ ] Create `resources/sounds/` directory
- [ ] Generate or source system sounds (WAV format, 22050 Hz, mono):
  - [ ] `startup.wav` — OS boot chime
  - [ ] `click.wav` — button click feedback
  - [ ] `error.wav` — error alert
  - [ ] `notify.wav` — notification toast
  - [ ] `shutdown.wav` — shutdown sound
  - [ ] `recycle.wav` — empty recycle bin
- [ ] Install to `C:\Impossible\Sounds\` on IXFS
- [ ] Play startup chime after boot splash finishes
- [ ] Play error sound with error dialogs
- [ ] Play notification sound with toast notifications
- [ ] Registry: `HKLM\SYSTEM\Sound\SystemSounds = 1` (enable/disable)
- [ ] Commit: `"kernel: system sounds"`

---

## 2. Audio Codec Libraries

### 2.1 WAV Decoder

**Prompt:** `dr_wav.h` is a public domain single-header WAV decoder (~1500 lines). Redirect its memory allocation macros to `kmalloc`/`kfree`. `audio_load_wav(path)` reads the WAV file via VFS, passes it to `drwav_init_memory()`, then decodes to 16-bit PCM. Support common formats: 8-bit unsigned, 16-bit signed, mono and stereo, sample rates 22050/44100/48000. Return a struct with the PCM buffer pointer, sample count, sample rate, and channel count. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: dr_wav WAV decoder"`.


- [ ] Add `dr_wav.h` to `include/` (public domain, ~1500 lines)
- [ ] Redirect memory: `DRWAV_MALLOC → kmalloc`, `DRWAV_FREE → kfree`
- [ ] Implement `audio_load_wav(path)` — decode WAV file to PCM int16 buffer
- [ ] Support: 8-bit, 16-bit, mono, stereo, common sample rates (22050, 44100, 48000)
- [ ] Test: load and play a WAV file
- [ ] Commit: `"libs: dr_wav WAV decoder"`

### 2.2 MP3 Decoder

**Prompt:** `dr_mp3.h` is a public domain single-header MP3 decoder (~3000 lines). Same integration pattern as WAV: redirect memory, decode to 16-bit PCM. MP3 files are typically MPEG-1 Layer 3 at 44100 Hz stereo. If the sound driver expects a different sample rate (e.g., 48000), resample by linear interpolation. `audio_load_mp3(path)` returns the same PCM struct as WAV. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: dr_mp3 MP3 decoder"`.


- [ ] Add `dr_mp3.h` to `include/` (public domain, ~3000 lines)
- [ ] Redirect memory to kmalloc/kfree
- [ ] Implement `audio_load_mp3(path)` — decode MP3 to PCM int16 buffer
- [ ] Handle: MPEG-1 Layer 3, 44100 Hz, stereo
- [ ] Resample if needed (driver expects specific rate)
- [ ] Test: decode and play an MP3 file
- [ ] Commit: `"libs: dr_mp3 MP3 decoder"`

### 2.3 OGG Vorbis Decoder

**Prompt:** `stb_vorbis.c` is a public domain OGG Vorbis decoder (~5000 lines). Because it's a .c file (not header-only), create a wrapper `stb_vorbis_impl.c` similar to the stb_truetype integration from Phase 02. Redirect memory and disable stdio. Compile with SSE2 and `-ffreestanding`. `audio_load_ogg(path)` decodes the full OGG file to PCM. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: stb_vorbis OGG decoder"`.


- [ ] Add `stb_vorbis.c` to `src/libs/` (public domain, ~5000 lines)
- [ ] Redirect memory, disable stdio
- [ ] Implement `audio_load_ogg(path)` — decode OGG to PCM
- [ ] Test: decode and play an OGG file
- [ ] Commit: `"libs: stb_vorbis OGG decoder"`

### 2.4 FLAC Decoder (Future)

**Prompt:** `dr_flac.h` is a public domain single-header FLAC decoder (~4000 lines). FLAC is lossless audio — larger files but perfect quality. Same integration pattern as the other dr_* libraries. This is a stretch goal since WAV, MP3, and OGG cover most use cases. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: dr_flac FLAC decoder"`.


- [ ] *(Stretch)* Add `dr_flac.h` to `include/` (public domain, ~4000 lines)
- [ ] *(Stretch)* Implement `audio_load_flac(path)` — lossless decode to PCM
- [ ] Commit: `"libs: dr_flac FLAC decoder"`

### 2.5 MIDI Synthesis (Future)

**Prompt:** TinySoundFont (MIT, single header) synthesizes MIDI audio using SoundFont (.sf2) instrument samples. Bundle a small General MIDI SoundFont (~5 MB). `audio_play_midi(path)` loads the MIDI file, synthesizes it to PCM via TinySoundFont, and plays via the audio system. This is a stretch goal for music creation and retro game audio. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"libs: TinySoundFont MIDI synthesis"`.


- [ ] *(Stretch)* Add **TinySoundFont** (MIT, single header) to `include/`
- [ ] *(Stretch)* Bundle a small SoundFont file (~5 MB)
- [ ] *(Stretch)* Implement `audio_play_midi(path)` — synthesize MIDI to PCM
- [ ] Commit: `"libs: TinySoundFont MIDI synthesis"`

### 2.6 Unified Audio Loader

**Prompt:** `audio_load(path)` detects the audio format by file extension (.wav/.mp3/.ogg/.flac) and dispatches to the appropriate decoder. Returns a unified `struct audio_clip` with PCM buffer, sample_rate, channels, and total_samples. This is the single entry point for all audio loading — used by the media player, system sounds, and any future audio playback. After completing all items, mark every item as `[x]`, run `make clean && make all && make run`, and commit as `"kernel: unified audio file loader"`.


- [ ] Implement `audio_load(path)` — detect format by extension, dispatch to decoder:
  - [ ] `.wav` → `audio_load_wav()`
  - [ ] `.mp3` → `audio_load_mp3()`
  - [ ] `.ogg` → `audio_load_ogg()`
  - [ ] `.flac` → `audio_load_flac()`
- [ ] Return: PCM buffer + sample_rate + channels + total_samples
- [ ] Used by: media player app, system sounds, games
- [ ] Commit: `"kernel: unified audio file loader"`

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
| 🔴 P0 | 1.1 AC97 Sound Card | Audio hardware foundation |
| 🔴 P0 | 1.2 Audio Abstraction | Unified API for all audio |
| 🔴 P0 | 2.1 WAV Decoder | Simplest format — system sounds |
| 🟠 P1 | 1.3 Audio Mixer | Multiple simultaneous sounds |
| 🟠 P1 | 1.4 System Sounds | UX — startup chime, notifications |
| 🟠 P1 | 2.2 MP3 Decoder | Most common music format |
| 🟠 P1 | 3.1 USB Core | Foundation for all USB devices |
| 🟡 P2 | 3.2 xHCI Controller | USB 3.0 host — enables all USB devices |
| 🟡 P2 | 3.3 USB HID | USB keyboard + mouse |
| 🟡 P2 | 2.3 OGG Vorbis | Open audio format |
| 🟡 P2 | 4.2 Media Player | Audio playback app |
| 🟡 P2 | 4.3 Volume Popup | Essential UX |
| 🟡 P2 | 5.1 SLAB Allocator | Eliminates heap pressure for kernel objects |
| 🟢 P3 | 3.4 USB Mass Storage | USB flash drive support |
| 🟢 P3 | 4.5 Hot-Plug Events | USB attach/detach notifications |
| 🟢 P3 | 2.6 Unified Audio Loader | Format-agnostic loading |
| 🟢 P3 | 4.4 Sound Settings | Settings applet |
| 🟢 P3 | 5.2 vmalloc | Large kernel buffers without physical contiguity |
| 🟢 P3 | 5.3 Growable Heap | Eliminates fixed heap size ceiling |
| 🔵 P4 | 2.4 FLAC Decoder | Lossless audio (niche) |
| 🔵 P4 | 2.5 MIDI Synthesis | Music creation |
| 🔵 P4 | 4.1 Intel HDA | Modern hardware |
| 🔵 P4 | 3.5 USB Hub | Cascaded USB devices |
| 🔵 P4 | 4.7 EHCI/UHCI | Legacy USB support |
