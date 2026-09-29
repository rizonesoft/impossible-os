<!-- docs: covers=todo/10-platform-services/TODO-01-audio-system.md sources=include/kernel/sched/syscall.h,include/registry.h,include/kernel/mm/pmm.h,scripts/machines/run-vbox.sh reviewed=2026-09-29 order=1 -->
# Audio System and Media Player

## What is it?

The audio system is the software half of sound on Impossible OS: one kernel audio API, a mixer that plays several sounds at once, decoders for WAV, MP3, Ogg Vorbis and FLAC, and the programs on top (a media player, the volume overlay and the Sound applet, `mmsys.cpl`). None of its ten sections has shipped, and there is no audio hardware driver yet either, so today the OS makes no sound at all.

## How does it work?

**Today.** Nothing in `src/` or `include/` plays audio. No sound card driver exists: the [Audio Drivers](../hardware/audio-drivers.md) roadmap that owns AC97, Intel HDA, VirtIO Sound and USB Audio is entirely open. The QEMU and VirtualBox launch scripts do not attach a sound device; [`run-vbox.sh`](../../scripts/machines/run-vbox.sh) passes `--audio-driver none`.

**Planned design.** The plan is layered so each piece can be tested on its own:

```mermaid
flowchart LR
    A[Media player, system sounds, apps] --> B["audio_load(path)"]
    B --> C[WAV / MP3 / OGG / FLAC decoders]
    C --> D[8-stream mixer]
    D --> E[audio_device_t driver vtable]
    E --> F[AC97, HDA, VirtIO Sound, USB Audio]
```

1. **Abstraction layer.** An `audio_play`, `audio_stop`, `audio_set_volume` and `audio_is_playing` API over whichever device the driver layer registers, with the master volume kept in the Registry.
2. **Mixer.** Up to eight PCM streams summed into 32-bit accumulators, clamped to 16 bits, scaled by master volume and fed to the device's DMA ring from its interrupt.
3. **Decoders.** Single-header libraries (`dr_wav`, `dr_mp3`, `stb_vorbis`, `dr_flac`) with their allocators redirected to the kernel heap for small clips and to `pmm_alloc_contiguous()` for anything over 4 KB, plus a linear resampler when a file's rate differs from the device's.
4. **Loader.** `audio_load(path)` picks the decoder from the extension, and `audio_play_file()` wraps load and play.
5. **Programs.** A media player (transport buttons, seek bar, ID3v2 title and artist, playlist, shuffle and repeat), volume in Quick Settings with a 196 by 48 pixel overlay pill for the volume keys, and `mmsys.cpl` for master volume, output device, system sounds and a test tone.

## What are its interfaces?

| Interface | Status |
| --- | --- |
| `audio_play()`, `audio_stop()`, `audio_set_volume()`, `audio_get_volume()`, `audio_is_playing()` | Planned |
| Mixer streams, `audio_load()`, `audio_play_file()` | Planned |
| Audio syscalls | Planned; numbers not assigned. The highest assigned `SYS_*` is 48 ([`syscall.h`](../../include/kernel/sched/syscall.h)) |
| `HKLM\SYSTEM\Sound\Volume`, `HKCU\Software\Impossible\MediaPlayer\*` | Planned Registry values, through the shipped `RegSetDword()` and `RegGetDword()` ([`registry.h`](../../include/registry.h)) |
| `pmm_alloc_contiguous()` for PCM buffers | Shipped ([`pmm.h`](../../include/kernel/mm/pmm.h)) |

## How do I use it?

There is nothing to run yet. When the driver and the first two sections land, the test will be a WAV file played from `cmd.exe` on QEMU with an `intel-hda` device attached, and the roadmap's Verification section lists the checks for each decoder.

## What is not implemented yet?

Everything in this roadmap, in dependency order:

- [Audio Abstraction Layer](../../todo/10-platform-services/TODO-01-audio-system.md#1-audio-abstraction-layer-sonnet), which needs a driver from the [Audio Drivers roadmap](../../todo/04-drivers-hardware/TODO-18-audio-drivers.md)
- [Audio Mixer](../../todo/10-platform-services/TODO-01-audio-system.md#2-audio-mixer-opus)
- [WAV](../../todo/10-platform-services/TODO-01-audio-system.md#3-wav-decoder-sonnet), [MP3](../../todo/10-platform-services/TODO-01-audio-system.md#4-mp3-decoder-sonnet), [Ogg Vorbis](../../todo/10-platform-services/TODO-01-audio-system.md#5-ogg-vorbis-decoder-sonnet) and [FLAC](../../todo/10-platform-services/TODO-01-audio-system.md#6-flac-decoder-stretch-sonnet) decoders, none of which is vendored yet
- [Unified Audio Loader](../../todo/10-platform-services/TODO-01-audio-system.md#7-unified-audio-loader-sonnet)
- [Media Player App](../../todo/10-platform-services/TODO-01-audio-system.md#8-media-player-app-sonnet), [Volume Control](../../todo/10-platform-services/TODO-01-audio-system.md#9-volume-control-popup-sonnet) and [`mmsys.cpl`](../../todo/10-platform-services/TODO-01-audio-system.md#10-mmsyscpl----sound-settings-applet-sonnet)

Two design questions are open in the roadmaps and need settling before code: the driver roadmap defines its own `audio_device_t` vtable and says the mixer lives on the desktop side, while this roadmap puts the mixer in the kernel; and `mmsys.cpl` is also specified, with different calls, in the [Control Panel roadmap](../../todo/09-desktop-shell/TODO-11-control-panel.md).

## How does it compare with Windows 11 and Linux?

Windows 11 layers WASAPI over WaveRT drivers with a shared-mode mixer, and decodes WAV, MP3 and FLAC through Media Foundation, but has no built-in Ogg Vorbis decoder. Linux uses ALSA drivers with PipeWire or PulseAudio mixing in a user-space daemon and decodes everything through libraries such as FFmpeg. The Impossible OS plan is an in-kernel stack with no daemon: one driver vtable, one fixed-point mixer and one `audio_load()` call for all four formats. None of it exists yet.

## See also

- [Audio System and Media Player roadmap](../../todo/10-platform-services/TODO-01-audio-system.md)
- [Audio Drivers](../hardware/audio-drivers.md)
- [Control Panel and Settings](../desktop/control-panel.md)
- [File Associations, Shortcuts and System Resources](../desktop/file-associations.md), which owns the system sounds
