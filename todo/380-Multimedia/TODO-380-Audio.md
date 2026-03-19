# P0006 — Audio System

> **Goal:** Audio subsystem — abstraction layer, mixer, codec libraries
> (WAV/MP3/OGG/FLAC), system sounds, and unified audio API.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

**Related TODOs:**
- **080-Drivers** §5 — Sound card hardware drivers (AC97, Intel HDA)
- **P0302** §4 — System sound WAV files (startup chime, click, error, etc.)

---

## 1. Sound Card Drivers

> **Moved to [TODO-063-Drivers.md](../060-Hardware-Drivers/TODO-063-Drivers.md) §5** — AC97 sound
> card driver (§5.1), Intel HDA stretch (§5.2), and module conversion (§5.3).
> The hardware driver provides `ac97_play()`, `ac97_stop()`, `ac97_set_volume()`.

---

## 2. Audio Abstraction Layer

> **Depends on:** 080-Drivers §5.1 (AC97 driver)

**Prompt:** The audio abstraction layer provides a uniform API over different sound card drivers (AC97 now, Intel HDA later). `struct audio_device` holds the driver name, sample_rate, channels, bits_per_sample, and function pointers for play/stop/volume. `audio_init()` detects available sound hardware and registers the driver. `audio_play(pcm_data, samples, sample_rate)` dispatches to the currently registered driver. Volume is stored in Registry `HKLM\SYSTEM\Sound\Volume` (REG_DWORD, 0-100) and `HKLM\SYSTEM\Sound\Mute` (REG_DWORD). Add `SYS_AUDIO_PLAY` and `SYS_AUDIO_VOLUME` syscalls so user-mode apps can play audio. After completing all items,sh clean`, and commit as `"kernel: audio abstraction layer"`.


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

---

## 3. Audio Mixer

**Prompt:** The audio mixer allows multiple sounds to play simultaneously (up to 8 streams). Each stream has its own PCM buffer and per-stream volume. The mixer sums all active streams' samples, applies per-stream volume scaling, then applies master volume. Clamp the mixed output to INT16_MIN/INT16_MAX to prevent clipping distortion. Feed the mixed output to the sound driver's DMA buffer. Mute support: when `HKLM\SYSTEM\Sound\Mute` is set, output silence (zeros) without stopping the mixer. After completing all items,sh clean`, and commit as `"kernel: audio mixer"`.


- [ ] Create `src/kernel/audio_mixer.c`
- [ ] Support multiple simultaneous audio streams (up to 8)
- [ ] Mix streams by summing PCM samples with per-stream volume
- [ ] Clamp mixed output to prevent clipping (INT16_MIN/INT16_MAX)
- [ ] Master volume applied after mixing
- [ ] Mute support (Registry `HKLM\SYSTEM\Sound\Mute`)
- [ ] Commit: `"kernel: audio mixer"`

---

## 4. System Sounds

> **Moved to [TODO-240-Resources.md](../230-Core-Services/TODO-240-Resources.md) §4** — WAV system sounds
> (startup chime, click, error, notification, shutdown, recycle), `resources/sounds/`
> directory, install to IXFS.

---

## 5. Audio Codec Libraries

### 5.1 WAV Decoder

**Prompt:** `dr_wav.h` is a public domain single-header WAV decoder (~1500 lines). Redirect its memory allocation macros to `kmalloc`/`kfree`. `audio_load_wav(path)` reads the WAV file via VFS, passes it to `drwav_init_memory()`, then decodes to 16-bit PCM. Support common formats: 8-bit unsigned, 16-bit signed, mono and stereo, sample rates 22050/44100/48000. Return a struct with the PCM buffer pointer, sample count, sample rate, and channel count. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"libs: dr_wav WAV decoder"`.


- [ ] Add `dr_wav.h` to `include/` (public domain, ~1500 lines)
- [ ] Redirect memory: `DRWAV_MALLOC → kmalloc`, `DRWAV_FREE → kfree`
- [ ] Implement `audio_load_wav(path)` — decode WAV file to PCM int16 buffer
- [ ] Support: 8-bit, 16-bit, mono, stereo, common sample rates (22050, 44100, 48000)
- [ ] Test: load and play a WAV file
- [ ] Commit: `"libs: dr_wav WAV decoder"`

### 5.2 MP3 Decoder

**Prompt:** `dr_mp3.h` is a public domain single-header MP3 decoder (~3000 lines). Same integration pattern as WAV: redirect memory, decode to 16-bit PCM. MP3 files are typically MPEG-1 Layer 3 at 44100 Hz stereo. If the sound driver expects a different sample rate (e.g., 48000), resample by linear interpolation. `audio_load_mp3(path)` returns the same PCM struct as WAV. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"libs: dr_mp3 MP3 decoder"`.


- [ ] Add `dr_mp3.h` to `include/` (public domain, ~3000 lines)
- [ ] Redirect memory to kmalloc/kfree
- [ ] Implement `audio_load_mp3(path)` — decode MP3 to PCM int16 buffer
- [ ] Handle: MPEG-1 Layer 3, 44100 Hz, stereo
- [ ] Resample if needed (driver expects specific rate)
- [ ] Test: decode and play an MP3 file
- [ ] Commit: `"libs: dr_mp3 MP3 decoder"`

### 5.3 OGG Vorbis Decoder

**Prompt:** `stb_vorbis.c` is a public domain OGG Vorbis decoder (~5000 lines). Because it's a .c file (not header-only), create a wrapper `stb_vorbis_impl.c` similar to the stb_truetype integration. Redirect memory and disable stdio. Compile with SSE2 and `-ffreestanding`. `audio_load_ogg(path)` decodes the full OGG file to PCM. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"libs: stb_vorbis OGG decoder"`.


- [ ] Add `stb_vorbis.c` to `src/libs/` (public domain, ~5000 lines)
- [ ] Redirect memory, disable stdio
- [ ] Implement `audio_load_ogg(path)` — decode OGG to PCM
- [ ] Test: decode and play an OGG file
- [ ] Commit: `"libs: stb_vorbis OGG decoder"`

### 5.4 FLAC Decoder *(Stretch)*

- [ ] *(Stretch)* Add `dr_flac.h` to `include/` (public domain, ~4000 lines)
- [ ] *(Stretch)* Implement `audio_load_flac(path)` — lossless decode to PCM
- [ ] Commit: `"libs: dr_flac FLAC decoder"`

### 5.5 MIDI Synthesis *(Stretch)*

- [ ] *(Stretch)* Add **TinySoundFont** (MIT, single header) to `include/`
- [ ] *(Stretch)* Bundle a small SoundFont file (~5 MB)
- [ ] *(Stretch)* Implement `audio_play_midi(path)` — synthesize MIDI to PCM
- [ ] Commit: `"libs: TinySoundFont MIDI synthesis"`

### 5.6 Unified Audio Loader

**Prompt:** `audio_load(path)` detects the audio format by file extension (.wav/.mp3/.ogg/.flac) and dispatches to the appropriate decoder. Returns a unified `struct audio_clip` with PCM buffer, sample_rate, channels, and total_samples. This is the single entry point for all audio loading — used by the media player, system sounds, and any future audio playback. After completing all items, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: unified audio file loader"`.


- [ ] Implement `audio_load(path)` — detect format by extension, dispatch to decoder:
  - [ ] `.wav` → `audio_load_wav()`
  - [ ] `.mp3` → `audio_load_mp3()`
  - [ ] `.ogg` → `audio_load_ogg()`
  - [ ] `.flac` → `audio_load_flac()`
- [ ] Return: PCM buffer + sample_rate + channels + total_samples
- [ ] Used by: media player app, system sounds, games
- [ ] Commit: `"kernel: unified audio file loader"`

---

## 6. Media Player App

> *Moved from [TODO-063-Drivers.md](../060-Hardware-Drivers/TODO-063-Drivers.md) §4.2 and [TODO-560-Long-Term.md](../510-Long-Term-Stretch/TODO-560-Long-Term.md) §3*

- [ ] Create `src/apps/mediaplayer/mediaplayer.c`
- [ ] Audio playback via `audio_play()`, load via `audio_load()` (WAV, MP3, OGG, FLAC)
- [ ] Transport: Play/Pause, Stop, Previous/Next track
- [ ] Seek bar, volume slider with mute toggle
- [ ] Display song title/artist from filename or ID3 tags
- [ ] Playlist panel: add files, double-click to play, repeat/shuffle
- [ ] File association: `.mp3`, `.wav`, `.ogg`, `.flac` → Media Player
- [ ] Commit: `"apps: Media Player"`

---

## 7. Volume Popup (System Tray)

- [ ] Click 🔊 tray icon → volume slider popup
- [ ] Drag slider → `audio_set_volume()` in real-time
- [ ] Mute toggle button
- [ ] Volume UP / DOWN keyboard keys → adjust volume
- [ ] Show volume OSD briefly when keys pressed
- [ ] Commit: `"desktop: volume control popup"`

---

## 8. Sound Control Panel Applet

- [ ] `mmsys.cpl` in Control Panel:
  - [ ] Master volume slider
  - [ ] Mute toggle
  - [ ] Output device selector (if multiple audio devices)
  - [ ] System sounds enable/disable
  - [ ] Test sound button
- [ ] Commit: `"apps: sound control panel applet"`

---

## Priority Order

| Priority | Section                          | Reason                                      |
|----------|----------------------------------|---------------------------------------------|
| 🔴 P0     | **080-Drivers §5.1** AC97 Driver | Audio hardware foundation (see 080-Drivers) |
| 🔴 P0     | §2 Audio Abstraction             | Unified API over drivers                    |
| 🔴 P0     | §5.1 WAV Decoder                 | Simplest format — system sounds             |
| 🟠 P1     | §3 Audio Mixer                   | Multiple simultaneous sounds                |
| 🟠 P1     | §5.2 MP3 Decoder                 | Most common music format                    |
| 🟡 P2     | §5.3 OGG Vorbis                  | Open audio format                           |
| 🟡 P2     | §5.6 Unified Audio Loader        | Format-agnostic loading                     |
| 🟢 P3     | §5.4 FLAC Decoder                | Lossless audio (niche)                      |
| 🔵 P4     | §5.5 MIDI Synthesis              | Music creation                              |

---

## Key Files

| File                         | Purpose                            |
|------------------------------|------------------------------------|
| `src/kernel/drivers/ac97.c`  | AC97 driver (see 080-Drivers §5.1) |
| `src/kernel/audio.c`         | [NEW] Audio abstraction layer      |
| `include/audio.h`            | [NEW] Audio API header             |
| `src/kernel/audio_mixer.c`   | [NEW] Multi-stream PCM mixer       |
| `include/dr_wav.h`           | [NEW] WAV decoder (public domain)  |
| `include/dr_mp3.h`           | [NEW] MP3 decoder (public domain)  |
| `src/libs/stb_vorbis_impl.c` | [NEW] OGG Vorbis decoder wrapper   |
