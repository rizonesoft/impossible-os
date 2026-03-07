# Phase 08 — Hardware Drivers

> **Goal:** Extend hardware support beyond the basic PS/2 and RTL8139 drivers:
> add a full audio subsystem (sound card driver, mixer, codec libraries, system sounds),
> a USB host controller stack with device class drivers, and additional hardware
> support for a complete desktop experience.

---

## 1. Audio System — Sound Card Driver
> *Research: [01_audio_system.md](research/phase_08_hardware/01_audio_system.md)*

### 1.1 AC97 Sound Card Driver

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

- [ ] Create `src/kernel/audio.c` and `include/audio.h`
- [ ] Define `struct audio_device` (name, sample_rate, channels, bits_per_sample, play_fn, stop_fn, volume_fn)
- [ ] Implement `audio_init()` — detect sound card, register driver
- [ ] Implement `audio_play(pcm_data, samples, sample_rate)` — dispatch to driver
- [ ] Implement `audio_set_volume(volume)` — 0–100 scale
- [ ] Implement `audio_get_volume()` — read current volume
- [ ] Implement `audio_is_playing()` — check playback state
- [ ] Store volume in Codex: `System\Sound\Volume`, `System\Sound\Mute`
- [ ] Add `SYS_AUDIO_PLAY` and `SYS_AUDIO_VOLUME` syscalls
- [ ] Commit: `"kernel: audio abstraction layer"`

### 1.3 Audio Mixer

- [ ] Create `src/kernel/audio_mixer.c`
- [ ] Support multiple simultaneous audio streams (up to 8)
- [ ] Mix streams by summing PCM samples with per-stream volume
- [ ] Clamp mixed output to prevent clipping
- [ ] Master volume applied after mixing
- [ ] Mute support (Codex `System\Sound\Mute`)
- [ ] Commit: `"kernel: audio mixer"`

### 1.4 System Sounds

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
- [ ] Codex: `System\Sound\SystemSounds = 1` (enable/disable)
- [ ] Commit: `"kernel: system sounds"`

---

## 2. Audio Codec Libraries
> *Research: [02_audio_libraries.md](research/phase_08_hardware/02_audio_libraries.md)*

### 2.1 WAV Decoder

- [ ] Add `dr_wav.h` to `include/` (public domain, ~1500 lines)
- [ ] Redirect memory: `DRWAV_MALLOC → kmalloc`, `DRWAV_FREE → kfree`
- [ ] Implement `audio_load_wav(path)` — decode WAV file to PCM int16 buffer
- [ ] Support: 8-bit, 16-bit, mono, stereo, common sample rates (22050, 44100, 48000)
- [ ] Test: load and play a WAV file
- [ ] Commit: `"libs: dr_wav WAV decoder"`

### 2.2 MP3 Decoder

- [ ] Add `dr_mp3.h` to `include/` (public domain, ~3000 lines)
- [ ] Redirect memory to kmalloc/kfree
- [ ] Implement `audio_load_mp3(path)` — decode MP3 to PCM int16 buffer
- [ ] Handle: MPEG-1 Layer 3, 44100 Hz, stereo
- [ ] Resample if needed (driver expects specific rate)
- [ ] Test: decode and play an MP3 file
- [ ] Commit: `"libs: dr_mp3 MP3 decoder"`

### 2.3 OGG Vorbis Decoder

- [ ] Add `stb_vorbis.c` to `src/libs/` (public domain, ~5000 lines)
- [ ] Redirect memory, disable stdio
- [ ] Implement `audio_load_ogg(path)` — decode OGG to PCM
- [ ] Test: decode and play an OGG file
- [ ] Commit: `"libs: stb_vorbis OGG decoder"`

### 2.4 FLAC Decoder (Future)

- [ ] *(Stretch)* Add `dr_flac.h` to `include/` (public domain, ~4000 lines)
- [ ] *(Stretch)* Implement `audio_load_flac(path)` — lossless decode to PCM
- [ ] Commit: `"libs: dr_flac FLAC decoder"`

### 2.5 MIDI Synthesis (Future)

- [ ] *(Stretch)* Add **TinySoundFont** (MIT, single header) to `include/`
- [ ] *(Stretch)* Bundle a small SoundFont file (~5 MB)
- [ ] *(Stretch)* Implement `audio_play_midi(path)` — synthesize MIDI to PCM
- [ ] Commit: `"libs: TinySoundFont MIDI synthesis"`

### 2.6 Unified Audio Loader

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
> *Research: [03_usb_support.md](research/phase_08_hardware/03_usb_support.md)*

### 3.1 USB Core

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

- [ ] *(Stretch)* Detect USB hub devices (class `0x09`)
- [ ] *(Stretch)* Enumerate downstream ports
- [ ] *(Stretch)* Handle hub port status changes → enumeration of cascaded devices
- [ ] Commit: `"drivers: USB hub support"`

---

## 4. Agent-Recommended Additions

> Items not in the research files but important for complete hardware support.

### 4.1 Intel HDA Sound Driver

- [ ] *(Stretch)* Create `src/kernel/drivers/hda.c`
- [ ] *(Stretch)* Detect Intel HDA via PCI (class `0x04`, subclass `0x03`)
- [ ] *(Stretch)* Map MMIO registers, initialize CORB/RIRB (command/response buffers)
- [ ] *(Stretch)* Enumerate codecs, parse widget tree, configure DAC path
- [ ] *(Stretch)* DMA stream setup for PCM playback
- [ ] *(Stretch)* QEMU: `-device intel-hda -device hda-duplex`
- [ ] Commit: `"drivers: Intel HDA audio"`

### 4.2 Media Player App

- [ ] Create `src/apps/mediaplayer/mediaplayer.c`
- [ ] Play audio files: WAV, MP3, OGG
- [ ] UI: play/pause/stop buttons, progress bar, volume slider
- [ ] Playlist: add files, next/previous track
- [ ] File association: double-click `.mp3` / `.wav` → opens in media player
- [ ] Album art display (from embedded image or folder `cover.jpg`)
- [ ] Commit: `"apps: Media Player"`

### 4.3 Volume Popup (System Tray)

- [ ] Click 🔊 tray icon → volume slider popup
- [ ] Drag slider → `audio_set_volume()` in real-time
- [ ] Mute toggle button
- [ ] Volume UP / DOWN keyboard keys → adjust volume
- [ ] Show volume OSD briefly when keys pressed
- [ ] Commit: `"desktop: volume control popup"`

### 4.4 Sound Settings Applet

- [ ] `sound.spl` in Settings Panel:
  - [ ] Master volume slider
  - [ ] Mute toggle
  - [ ] Output device selector (if multiple audio devices)
  - [ ] System sounds enable/disable
  - [ ] Test sound button (play a test tone)
- [ ] Commit: `"apps: sound settings applet"`

### 4.5 Hot-Plug Event System

- [ ] Kernel notification on USB device attach/detach
- [ ] Desktop toast: "USB drive detected — D:\ (8.0 GB, FAT32)"
- [ ] Desktop toast: "USB keyboard connected"
- [ ] Safe removal: system tray icon → "Safely eject D:\"
  - [ ] Flush writes, unmount, notify user
- [ ] Commit: `"kernel: USB hot-plug notifications"`

### 4.6 PS/2 ↔ USB Fallback

- [ ] If USB keyboard/mouse detected, prefer USB input over PS/2
- [ ] If no USB HID, fall back to PS/2 (current default)
- [ ] Seamlessly switch input source without application changes
- [ ] Commit: `"drivers: PS/2 ↔ USB input fallback"`

### 4.7 EHCI/UHCI Fallback (Legacy USB)

- [ ] *(Stretch)* Create `src/kernel/drivers/usb/ehci.c` — USB 2.0 host controller
- [ ] *(Stretch)* Detect via PCI (class `0x0C`, subclass `0x03`, prog_if `0x20`)
- [ ] *(Stretch)* Async + periodic schedule, QH/TD structures
- [ ] *(Stretch)* Create `src/kernel/drivers/usb/uhci.c` — USB 1.1 host controller
- [ ] *(Stretch)* Detect via PCI (prog_if `0x00`)
- [ ] *(Stretch)* Frame list + TD chain
- [ ] Commit: `"drivers: EHCI/UHCI legacy USB host controllers"`

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
| 🟢 P3 | 3.4 USB Mass Storage | USB flash drive support |
| 🟢 P3 | 4.5 Hot-Plug Events | USB attach/detach notifications |
| 🟢 P3 | 2.6 Unified Audio Loader | Format-agnostic loading |
| 🟢 P3 | 4.4 Sound Settings | Settings applet |
| 🔵 P4 | 2.4 FLAC Decoder | Lossless audio (niche) |
| 🔵 P4 | 2.5 MIDI Synthesis | Music creation |
| 🔵 P4 | 4.1 Intel HDA | Modern hardware |
| 🔵 P4 | 3.5 USB Hub | Cascaded USB devices |
| 🔵 P4 | 4.7 EHCI/UHCI | Legacy USB support |
