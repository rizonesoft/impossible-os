---
schema_version: 1
id: audio-drivers
domain: 04-drivers-hardware
status: active
title: "TODO-18 -- Audio Drivers"
---

# TODO-18 -- Audio Drivers

> **Goal:** Deliver hardware-level PCM playback drivers behind a clean `audio_device_t` vtable that the audio mixing subsystem (a separate desktop-domain TODO) will build on top of -- covering AC97, Intel HDA, VirtIO Sound, and USB Audio Class 1.0 playback, plus hot-plug device switching and a path to convert all drivers to loadable `.kmod` modules.

> [!IMPORTANT]
> **No audio exists today.** This TODO is a greenfield driver layer. The `audio_device_t` vtable (§1) is the architectural contract: every driver section registers against it, and the desktop audio subsystem (→ XREF `09-desktop-shell`) calls only `audio_device_t` functions -- never driver internals. All drivers are initially built-in (to verify correctness), then converted to `.kmod` in §7 once the module system (→ XREF `04-drivers-hardware/TODO-05-kernel-module-system.md`) is complete.

## Inputs

- No existing audio code -- greenfield.
- [`src/kernel/drivers/virtio/virtio.c`](../../src/kernel/drivers/virtio/virtio.c) -- VirtIO transport reused by §4 (VirtIO Sound)
- → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- module loader required for §7 (kmod conversion); §7 is blocked on TODO-05
- → XREF: `04-drivers-hardware/TODO-10-usb-stack.md §3` -- xHCI isochronous endpoint support (`xhci_configure_isoch_ep`, `xhci_submit_isoch_transfer`) is now defined in TODO-10 §3; §5 here consumes it via `usb_submit_isoch()` from the USB core API (TODO-10 §1)
- → XREF: `09-desktop-shell` domain -- the desktop audio mixer/session manager calls `audio_device_t.write()` and `audio_device_t.set_volume()`; it is a consumer of the HAL defined in §1

## Outcome

- `audio_device_t` vtable in place; a single `audio_get_active()` call returns whichever driver registered at highest priority.
- AC97 driver plays PCM audio in QEMU (`-device AC97`); DMA Buffer Descriptor List refills via IRQ.
- Intel HDA driver enumerates codecs, parses widget tree, and plays PCM in QEMU (`-device intel-hda -device hda-duplex`).
- VirtIO Sound plays and records PCM in QEMU (`-device virtio-sound-pci`).
- USB Audio Class 1.0 plays PCM over an isochronous OUT endpoint; volume via `SET_CUR`.
- All four drivers convert to `.kmod` after the module system lands.
- Device hot-plug switches the active audio device and persists preference to `HKLM\SYSTEM\Audio\DefaultDevice`.

## Implementation Order

| ⭐  | Order | Deliverable                                                        | Depends On                               | Status |
| --- | :---: | ------------------------------------------------------------------ | ---------------------------------------- | :----: |
| ⭐  |   1   | §1 `audio_device_t` HAL vtable                                     | none                                     |  [ ]   |
| 💎  |   2   | §2 AC97 driver -- BDL DMA ring, IRQ refill, QEMU AC97              | §1 (vtable)                              |  [ ]   |
| 💎  |   3   | §3 Intel HDA driver -- CORB/RIRB, widget tree, DMA stream          | §1 (vtable)                              |  [ ]   |
| 💎  |   4   | §4 VirtIO Sound module -- control/tx/rx virtqueues, mic            | §1 (vtable), VirtIO core                 |  [ ]   |
| 💎  |   5   | §5 USB Audio UAC1 -- isochronous OUT, `SET_CUR` volume             | §1 (vtable), TODO-10 §3 (isoch endpoint) |  [ ]   |
| 💎  |   6   | §6 Audio device hot-plug -- active device switch, Registry persist | §2–5 (drivers registered)                |  [ ]   |
| 💎  |   7   | §7 Convert all drivers to `.kmod`                                  | §2–5, TODO-05 module loader              |  [ ]   |

> §1 `audio_device_t` is `⭐` exclusive by design: Windows WaveRT/WASAPI and Linux ALSA both have multi-layer audio stacks; Impossible OS exposes a single kernel-level HAL vtable that any driver can satisfy in under 300 lines. The desktop mixer sits on top -- clean separation, no in-kernel mixing.

---

## 1. Audio HAL -- `audio_device_t` Vtable `[Opus]`

Define the `audio_device_t` abstraction. All audio drivers register against it; the desktop audio subsystem calls only these functions. Priority-sorted registration mirrors `display_device_t` from `TODO-17`.

**Files:** `include/kernel/drivers/audio_device.h` (new), `src/kernel/drivers/audio_device.c` (new)

> [!NOTE]
> Keep the vtable minimal: the kernel audio layer is a PCM passthrough, not a mixer. Mixing, resampling, and effects are desktop-domain responsibilities. `write()` accepts interleaved 16-bit or 32-bit PCM at whatever rate/channels the device was initialized with; the caller is responsible for format conversion before calling.

- [ ] Define:
  ```c
  typedef struct {
      int     priority;
      const char *name;
      int   (*init)(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
      int   (*write)(const void *pcm, uint32_t frames);
      int   (*read)(void *pcm, uint32_t frames);   /* NULL if capture unsupported */
      void  (*set_volume)(uint8_t percent);         /* 0–100 */
      void  (*stop)(void);
  } audio_device_t;
  ```
- [ ] `audio_register(audio_device_t *dev)` -- insert into priority-sorted list; call `dev->init(44100, 2, 16)` to activate
- [ ] `audio_get_active()` -- return highest-priority registered device
- [ ] `AUDIO_PRIORITY_VIRTIO = 100`, `AUDIO_PRIORITY_HDA = 80`, `AUDIO_PRIORITY_AC97 = 50`, `AUDIO_PRIORITY_USB = 60`
- [ ] `audio_write(pcm, frames)` / `audio_set_volume(pct)` / `audio_stop()` -- inline helpers that delegate to `audio_get_active()`
- [ ] `audio_switch_device(audio_device_t *dev)` -- force-select a specific device (used by hot-plug §6 and user preference)
- [ ] Commit: `"kernel: audio_device_t HAL -- register/get_active, priority system, write/volume/stop"`

## 2. AC97 Sound Driver `[Opus]`

Implement the AC97 driver against the Intel ICH AC97 specification. Cold reset sequence, codec ready poll, NAM (mixer) register access for volume, and a 32-entry Buffer Descriptor List DMA ring on the NAB (bus master) PCM Out channel. IRQ completion handler refills the ring.

**Files:** `src/kernel/drivers/ac97.c` (new), `include/kernel/drivers/ac97.h` (new)

> [!NOTE]
> AC97 has two PCI BARs: BAR0 = Native Audio Mixer (NAM, port I/O, codec register access via `0x00`–`0x7E`); BAR1 = Native Audio Bus Master (NAB, port I/O, DMA registers). PCM Out DMA channel at NAB offset `0x10`: `BDBAR` (BDL base), `CIV` (current index), `LVI` (last valid index), `SR` (status/IRQ), `PICB` (position in current buffer), `PIV` (prefetched index), `CR` (control). BDL entry: `{ uint32_t addr, uint16_t samples, uint16_t flags }` where `flags bit15 = IOC` (interrupt on completion).

- [ ] PCI match: `{ 0x8086, 0x2415 }` (ICH), `{ 0x8086, 0x2425 }` (ICH0), `{ 0x8086, 0x2445 }` (ICH2), `{ 0x8086, 0x24C5 }` (ICH4); PCI class `0x04`, subclass `0x01`
- [ ] BAR0 (NAM) I/O port map; BAR1 (NAB) I/O port map
- [ ] Cold reset: assert `GLOB_CNT.ACLINK_OFF=0`; wait 100 µs; set `GLOB_CNT.COLD_RESET=1`; poll `GLOB_STS.CODECS_READY` until set (timeout 500 ms)
- [ ] Codec access: `nam_read16(reg)` / `nam_write16(reg, val)` via BAR0 I/O; poll `GLOB_STS.CODEC_ACCESS` busy bit
- [ ] Volume init: `nam_write16(NAM_MASTER_VOLUME, 0x0000)` (full volume, unmuted); `nam_write16(NAM_PCM_OUT_VOLUME, 0x0808)` (0 dB both channels)
- [ ] `ac97_set_rate(rate)`: write `rate` to `NAM_PCM_FRONT_DAC_RATE (0x2C)`; read back to verify
- [ ] BDL allocation: 32 × `{ phys_addr, sample_count, IOC=1 }` entries; `pmm_alloc_contiguous()` for descriptor table and audio buffers (each buffer = 2 × 4096 bytes for 16-bit stereo at 44100 Hz ≈ 21 ms)
- [ ] DMA start: write `BDBAR`; set `LVI=31`; set `CR.RPBM=1` (run/pause bus master)
- [ ] `ac97_write(pcm, frames)`: copy frames to next BDL buffer; advance `LVI`; ring wraps at 31
- [ ] IRQ handler: check `SR.BCIS` (buffer complete); ACK by writing `SR.BCIS=1`; call completion callback to request next audio buffer from desktop mixer
- [ ] `ac97_set_volume(pct)`: map 0–100 → `NAM_MASTER_VOLUME` attenuation (0 dB at 100, –46.5 dB at 0); write both channels
- [ ] Register `audio_device_t ac97_dev`; priority = `AUDIO_PRIORITY_AC97`
- [ ] Boot log: `[AC97] Codec ready, PCM Out DMA initialized, %u Hz`
- [ ] Commit: `"drivers: AC97 -- cold reset, NAM codec, 32-entry BDL DMA ring, IRQ refill, volume"`

## 3. Intel HDA Driver `[Opus]`

Implement the Intel High Definition Audio driver. CORB/RIRB for codec verb communication. Enumerate codecs on the HDA link; parse the audio function group widget tree to find the DAC → output pin path. Configure a DMA stream descriptor for PCM playback.

**Files:** `src/kernel/drivers/hda.c` (new), `include/kernel/drivers/hda.h` (new)

> [!NOTE]
> HDA MMIO registers (BAR0): `GCAP (0x00)`, `GCTL (0x08)`, `CORBBASE (0x40)`, `CORBWP (0x48)`, `CORBRP (0x4A)`, `CORBCTL (0x4C)`, `RIRBBASE (0x50)`, `RIRBWP (0x58)`, `RINTCNT (0x5A)`, `RIRBCTL (0x5C)`. Stream descriptor N at `0x80 + N*0x20`: `CTL (0x00)`, `STS (0x03)`, `LPIB (0x04)`, `CBL (0x08)`, `LVI (0x0C)`, `FIFOD (0x10)`, `FMT (0x12)`, `BDPL (0x18)`, `BDPU (0x1C)`.

- [ ] PCI match: `{ 0x8086, 0x2668 }` (ICH6), `{ 0x8086, 0x27D8 }` (ICH7), `{ 0x8086, 0x293E }` (ICH9), and Intel generic HDA class `0x0403`; also `{ 0x1002, 0xAAB0 }` (AMD HDMI), `{ 0x10DE, 0x0E0F }` (NVIDIA HDA)
- [ ] BAR0 MMIO map; controller reset: `GCTL.CRST=0` (reset); wait 100 µs; `GCTL.CRST=1`; wait for `GCTL.CRST` to read back 1 (codec init done, ~521 µs typical)
- [ ] CORB init: allocate 256-entry ring (each entry = 32-bit verb); write `CORBBASE`; set `CORBSIZE=2` (256 entries); `CORBCTL.CORBRUN=1`
- [ ] RIRB init: allocate 256-entry ring (each entry = 64-bit: 32-bit response + 32-bit response extended); write `RIRBBASE`; `RIRBCTL.RIRBDMAEN=1`; `RINTCNT=0xFF` (interrupt every response)
- [ ] `hda_send_verb(codec, node, verb, payload)` → 32-bit CORB entry `(codec<<28)|(node<<20)|(verb<<8)|payload`; advance CORBWP; wait for RIRB response
- [ ] Codec enumeration: `GET_PARAMETER(AFG, AUDIO_FUNCTION_GROUP_TYPE)` for codecs 0–15; for each AFG found: `GET_PARAMETER(AFG, SUBORDINATE_NODE_COUNT)` to get widget range
- [ ] Widget tree walk: for each widget in AFG, `GET_PARAMETER(widget, AUDIO_WIDGET_CAPABILITIES)` → type: `0x0`=AUDIO_OUTPUT, `0x1`=AUDIO_INPUT, `0x4`=PIN_COMPLEX, `0x3`=MIXER; find DAC widget + output PIN widget; configure connection path from DAC to PIN
- [ ] Output PIN enable: `SET_PIN_WIDGET_CONTROL(PIN, 0xC0)` (output enable + HP drive); `SET_EAPD_BTLENABLE(PIN, 0x02)` (EAPD enable)
- [ ] DMA stream (stream 0): configure `FMT` register for 44100 Hz stereo 16-bit (`0x4011`); allocate 4-entry BDL; write `BDPL`/`BDPU`; `CBL = total_bytes`; `LVI = 3`; `CTL.RUN=1`
- [ ] `hda_write(pcm, frames)`: copy to next BDL buffer; advance LVI pointer
- [ ] `hda_set_volume(pct)`: send `SET_AMPLIFIER_GAIN_MUTE(DAC, OUTPUT, pct)` verb; map 0–100 → 0–0x7F gain steps
- [ ] Register `audio_device_t hda_dev`; priority = `AUDIO_PRIORITY_HDA`
- [ ] Boot log: `[HDA] Codec %u: widget[%u..%u], DAC node %u → PIN node %u, stream 0 armed`
- [ ] Commit: `"drivers: Intel HDA -- CORB/RIRB, widget tree walk, DMA stream descriptor, PCM playback"`

## 4. VirtIO Sound Module `[Sonnet]`

Implement VirtIO Sound (`PCI 1AF4:1059`) using the VirtIO transport from `virtio.c`. Three virtqueues: `controlq` for PCM set-params/start/stop commands, `txq` (PCM Out), `rxq` (PCM In / microphone). Implements both playback and capture via the `audio_device_t` vtable.

**Files:** `src/kernel/drivers/virtio_sound.c` (new), `include/kernel/drivers/virtio_sound.h` (new)

> [!NOTE]
> VirtIO Sound spec (v1.2): `controlq` (index 0), `eventq` (index 1), `txq` (index 2), `rxq` (index 3). Streams are identified by `stream_id`. Control commands: `VIRTIO_SND_R_PCM_INFO`, `VIRTIO_SND_R_PCM_SET_PARAMS`, `VIRTIO_SND_R_PCM_PREPARE`, `VIRTIO_SND_R_PCM_START`, `VIRTIO_SND_R_PCM_STOP`, `VIRTIO_SND_R_PCM_RELEASE`. PCM transfer (txq): `{ virtio_snd_pcm_xfer header; uint8_t data[]; }` in descriptor chain.

- [ ] PCI match: `{ 0x1AF4, 0x1059 }` (VirtIO Sound); initialize `controlq`, `eventq`, `txq`, `rxq` via `virtio.c`
- [ ] Query available PCM streams: `VIRTIO_SND_R_PCM_INFO` for stream_id 0 (playback) and 1 (capture); verify stream directions
- [ ] `virtio_snd_set_params(stream_id, rate, channels, format)`: issue `VIRTIO_SND_R_PCM_SET_PARAMS` on controlq; wait for response; verify `VIRTIO_SND_S_OK`
- [ ] `VIRTIO_SND_R_PCM_PREPARE` → `VIRTIO_SND_R_PCM_START` on stream 0 before writing PCM
- [ ] `virtio_snd_write(pcm, frames)`: build descriptor chain `{ xfer_header(stream_id=0), pcm_data }` on txq; kick queue; completion updates write pointer
- [ ] Capture: pre-fill rxq with empty buffers; on completion, copy `data[]` to caller's record buffer; `audio_device_t.read()` implementation
- [ ] `VIRTIO_SND_R_PCM_STOP` → `VIRTIO_SND_R_PCM_RELEASE` on `audio_device_t.stop()`
- [ ] Volume: issue `VIRTIO_SND_R_CHMAP_INFO` + `SET_VOLUME` CTL command (if feature negotiated); fallback to software volume scaling
- [ ] Register `audio_device_t virtio_snd_dev`; priority = `AUDIO_PRIORITY_VIRTIO`; `read` = capture path
- [ ] Boot log: `[virtio-snd] PCM streams: %u playback, %u capture`
- [ ] Commit: `"drivers: VirtIO Sound -- controlq PCM params/start/stop, txq write, rxq capture"`

## 5. USB Audio Class 1.0 (UAC1) Playback `[Opus]`

Detect USB Audio Class 1.0 devices (interface class `0x01`). Configure an isochronous OUT endpoint for PCM playback using the USB core isochronous API (→ XREF `04-drivers-hardware/TODO-10-usb-stack.md §3`). Issue `SET_CUR` for volume and mute on the feature unit.

**Files:** `src/kernel/drivers/usb_audio.c` (new), `include/kernel/drivers/usb_audio.h` (new)

> [!NOTE]
> Isochronous endpoint infrastructure (`xhci_configure_isoch_ep`, `xhci_submit_isoch_transfer`, `usb_submit_isoch()`) is defined in `04-drivers-hardware/TODO-10-usb-stack.md §3`. This section uses that API. For 44100 Hz stereo 16-bit at 1 ms frames: 44 frames x 4 bytes = 176 bytes per isochronous packet, plus 1 byte every ~11 frames to handle the fractional rate (`44100/1000 = 44.1`).

- [ ] Parse USB AudioControl interface (class `0x01`, subclass `0x01`): find `Feature Unit` descriptor; record feature unit ID, channel config, and supported controls bitmap (mute=bit0, volume=bit1)
- [ ] Parse USB AudioStreaming interface (class `0x01`, subclass `0x02`): find isochronous OUT endpoint; read `wMaxPacketSize` and `bInterval`
- [ ] Configure isochronous endpoint via `usb_submit_isoch()` from USB core API (TODO-10 §1 + §3)
- [ ] `SET_INTERFACE(alt_setting=1)` control transfer to activate streaming interface (alt 0 = zero bandwidth, alt 1 = PCM streaming)
- [ ] `usb_audio_set_volume(feature_unit, channel, db_hundredths)`: `SET_CUR(VOLUME_CONTROL)` class request (bmRequestType=`0x21`, bRequest=`0x01`, value=`0x0100|channel`, index=`feature_unit<<8|interface`)
- [ ] `usb_audio_write(pcm, frames)`: split frames into isochronous packets; handle 44.1/48 kHz fractional packet sizes; submit Isoch TRBs; refill on completion
- [ ] Register `audio_device_t usb_audio_dev`; priority = `AUDIO_PRIORITY_USB`
- [ ] Boot log: `[USB-AUDIO] UAC1 device, isochronous OUT, %u Hz %u-bit %u-ch`
- [ ] Commit: `"drivers: USB Audio UAC1 -- xHCI isoch endpoint, SET_CUR volume, fractional packet fill"`

## 6. Audio Device Hot-Plug `[Sonnet]`

Notify the audio subsystem when a new audio device appears or disappears. Switch the active device to the highest-priority newly available device. Persist user preference to Registry.

**Files:** `src/kernel/drivers/audio_device.c`, `src/kernel/drivers/usb_audio.c`, `src/kernel/drivers/virtio_sound.c`

> [!NOTE]
> → XREF: `09-desktop-shell` domain -- the desktop audio mixer needs a `WM_AUDIO_DEVICE_CHANGED` message (or similar compositor notification) to update its device selector UI when hot-plug occurs. This section posts that message; the shell handles the UI response.

- [ ] `audio_notify_attach(audio_device_t *dev)`: call `audio_register(dev)`; if new device has higher priority than current active, call `audio_switch_device(dev)`; post `WM_AUDIO_DEVICE_CHANGED(new_dev_name)` to compositor
- [ ] `audio_notify_detach(audio_device_t *dev)`: remove from registered list; if detached device was active, select next highest-priority device; call `audio_switch_device(next)` or stop audio if none; post `WM_AUDIO_DEVICE_CHANGED(NULL)` to compositor
- [ ] USB Audio (§5): call `audio_notify_attach()` from `usb_audio_probe()`; call `audio_notify_detach()` from `usb_audio_disconnect()`
- [ ] Registry: `audio_switch_device()` writes `HKLM\SYSTEM\Audio\DefaultDevice` (`REG_SZ`, device name); on boot, `audio_register()` checks if new device name matches Registry preference and forces it active if so
- [ ] Boot log: `[AUDIO] Active device: %s (priority %d)`; on hot-plug: `[AUDIO] Device %s attached/detached, new active: %s`
- [ ] Commit: `"drivers: audio hot-plug -- notify_attach/detach, priority switch, DefaultDevice Registry"`

---

## 7. Convert All Audio Drivers to `.kmod` `[Sonnet]`

Move AC97, HDA, VirtIO Sound, and USB Audio out of the built-in kernel and into `src/modules/` as loadable `.kmod` files. Depends on the module loader from `04-drivers-hardware/TODO-05-kernel-module-system.md`.

**Files:** `src/modules/ac97/ac97.c`, `src/modules/hda/hda.c`, `src/modules/virtio_sound/virtio_sound.c`, `src/modules/usb_audio/usb_audio.c` (all moved)

> [!NOTE]
> → XREF: `04-drivers-hardware/TODO-05-kernel-module-system.md` -- module loader, `EXPORT_SYMBOL`, and driver model HAL vtables must be complete before this section. The `audio_device_t` struct pointer and `audio_register()` function must be exported via `EXPORT_SYMBOL` so module code can call them.

- [ ] `EXPORT_SYMBOL(audio_register)` and `EXPORT_SYMBOL(audio_get_active)` in `audio_device.c`
- [ ] Move each driver source to `src/modules/<name>/`; add `module_init()` / `module_exit()` entry points calling `audio_register()` / cleanup
- [ ] Add `Makefile` rules under `src/modules/<name>/` following the module build system pattern from TODO-05
- [ ] Remove the four drivers from the built-in kernel `Makefile` object list; confirm boot still reaches audio with modules auto-loaded from `C:\Impossible\System\Drivers\`
- [ ] Update `NOTICE.md` if any ported code was moved
- [ ] Commit: `"modules: audio drivers as .kmod -- ac97, hda, virtio_sound, usb_audio module_init/exit"`

## OS Comparison


| ⭐  | Feature                                      | 🪟 Win11                                              | 🐧 Linux                                            | 🚀 Impossible OS                                                                |
| --- | -------------------------------------------- | ----------------------------------------------------- | --------------------------------------------------- | ------------------------------------------------------------------------------- |
| ⭐  | Single `audio_device_t` HAL vtable           | ❌ WaveRT→WASAPI multi-layer stack; KMixer in         | ❌ ALSA PCM + mixer layers;                         | ⬜ §1 -- 6-function vtable, passthrough only, desktop                           |
| 💎  | AC97 PCM playback                            | ✅ `msac97.sys` inbox AC97 WDM audio                  | ✅ `snd_intel8x0` ALSA driver; BDL DMA;             | ⬜ §2 -- cold reset, NAM/NAB I/O, 32-entry                                      |
| 💎  | Intel HDA                                    | ✅ `hdaudio.sys` + `HDAudBus.sys`; codec enumeration; | ✅ `snd_hda_intel`; CORB/RIRB; codec parser; widget | ⬜ §3 -- CORB/RIRB verbs, AFG widget walk,                                      |
| 💎  | VirtIO Sound PCM playback + capture          | ✅ VirtIO drivers for Windows (Red                    | ✅ `snd_virtio` ALSA driver; v1.2 spec;             | ⬜ §4 -- controlq PCM params, txq write,                                        |
| 💎  | USB Audio Class 1.0 (UAC1) PCM playback      | ✅ `usbaudio.sys` inbox UAC1/UAC2 driver              | ✅ `snd_usb_audio`; UAC1 + UAC2; isochronous;       | ⬜ §5 -- xHCI isoch endpoint, fractional packet                                 |
| 💎  | Audio drivers as loadable `.kmod` modules    | ✅ All WDM audio drivers are                          | ✅ All ALSA drivers are loadable                    | ⬜ §7 -- `module_init/exit`, `src/modules/ac97/` etc., after TODO-05            |
| 💎  | Audio device hot-plug + active device switch | ✅ Windows Audio Session API; device                  | ✅ `udev` + PulseAudio/PipeWire; `PA_SINK_ADDED` /  | ⬜ §6 -- `audio_notify_attach/detach`, `WM_AUDIO_DEVICE_CHANGED`, Registry pref |

> **After §1–7:** Impossible OS has PCM playback across all four common audio surfaces (AC97, HDA, VirtIO, USB). The `audio_device_t` vtable (`⭐`) is the OS's architectural differentiator: Windows ships `KMixer` in the kernel and a 5-layer audio stack; Linux ships `dmix` as an ALSA plugin. Impossible OS keeps all mixing, resampling, and effects above the kernel boundary -- the driver layer is a thin, auditable PCM pipe that any device can implement in under 300 lines.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU AC97 (`-device AC97`): boot log `[AC97] Codec ready ... Hz`; `audio_get_active()->name == "AC97"`; play 440 Hz sine wave: heard via QEMU audio output
- [ ] QEMU Intel HDA (`-device intel-hda -device hda-duplex`): boot log `[HDA] Codec 0: DAC node → PIN node`; 440 Hz sine wave plays; `set_volume(50)` audibly reduces level
- [ ] QEMU VirtIO Sound (`-device virtio-sound-pci`): boot log `[virtio-snd] PCM streams: 1 playback, 1 capture`; PCM playback works; record capture buffer fills
- [ ] USB Audio: physical UAC1 USB speaker or QEMU USB audio emulation → `[USB-AUDIO] UAC1 ... Hz`; audio plays; volume `SET_CUR` applies
- [ ] Priority: attach VirtIO + AC97 simultaneously → `audio_get_active()` returns VirtIO (priority 100 > 50)
- [ ] Hot-plug: disconnect USB Audio device → `[AUDIO] Device USB-Audio detached, new active: HDA`; compositor receives `WM_AUDIO_DEVICE_CHANGED`; Registry `DefaultDevice` updated
- [ ] Module conversion (§7): confirm AC97 is not in built-in kernel; `[MODULE] Loaded ac97.kmod`; audio still works
- [ ] Commit: `"drivers: audio -- AC97, HDA, VirtIO Sound, USB UAC1, hot-plug, kmod conversion"`
