# P0006 — Audio System

> **Goal:** Full audio subsystem — sound card driver, abstraction layer, mixer,
> codec libraries (WAV/MP3/OGG/FLAC), system sounds, and unified audio API.

> [!CAUTION]
> **Memory Rule:** Use `pmm_alloc_contiguous()` for ALL buffers > 4 KB (fonts, images, file data). `kmalloc` is ONLY for small kernel structs (≤ 4 KB). Violating this crashes the 2 MiB heap silently. See `rules.md` Known Gotchas and `/add-asset` workflow.

**Related TODOs:**
- **P0302** §4 — System sound WAV files (startup chime, click, error, etc.)
- **P1601** — Drivers (Phase-92 covers USB, Intel HDA as stretch)

---

## 1. AC97 Sound Card Driver

**Prompt:** AC97 is the simplest sound card to implement in QEMU. Detect the Intel ICH AC97 controller via PCI class 0x04/subclass 0x01. Map two I/O BARs: the Native Audio Mixer BAR (for codec registers like master volume, PCM out volume) and the Native Audio Bus Master BAR (for DMA control). The Bus Master uses a Buffer Descriptor List (BDL) — a ring of 32 entries, each pointing to a PCM data buffer with length and IOC (Interrupt On Completion) flags. Fill the BDL with PCM audio data, set the BDL base address register, and start playback by setting the run bit. Generate a test sine wave (440 Hz, 16-bit signed, 44100 Hz) to verify audio output. After completing all items, create `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"drivers: AC97 sound card driver"`.


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
- [ ] Test: play a short PCM tone (sine wave 440 Hz) to verify audio output
- [ ] Commit: `"drivers: AC97 sound card driver"`

---

## 2. Audio Abstraction Layer

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

---

## 3. Audio Mixer

**Prompt:** The audio mixer allows multiple sounds to play simultaneously (up to 8 streams). Each stream has its own PCM buffer and per-stream volume. The mixer sums all active streams' samples, applies per-stream volume scaling, then applies master volume. Clamp the mixed output to INT16_MIN/INT16_MAX to prevent clipping distortion. Feed the mixed output to the sound driver's DMA buffer. Mute support: when `HKLM\SYSTEM\Sound\Mute` is set, output silence (zeros) without stopping the mixer. After completing all items, update `docs/architecture/audio.md`, mark every item as `[x]`, run `bash scripts/build.sh clean`, and commit as `"kernel: audio mixer"`.


- [ ] Create `src/kernel/audio_mixer.c`
- [ ] Support multiple simultaneous audio streams (up to 8)
- [ ] Mix streams by summing PCM samples with per-stream volume
- [ ] Clamp mixed output to prevent clipping (INT16_MIN/INT16_MAX)
- [ ] Master volume applied after mixing
- [ ] Mute support (Registry `HKLM\SYSTEM\Sound\Mute`)
- [ ] Commit: `"kernel: audio mixer"`

---

## 4. System Sounds

> **Moved to [TODO-P0302-Resources.md](TODO-P0302-Resources.md) §4** — WAV system sounds
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

## Priority Order

| Priority | Section | Reason |
|----------|---------|--------|
| 🔴 P0 | §1 AC97 Sound Card | Audio hardware foundation |
| 🔴 P0 | §2 Audio Abstraction | Unified API for all audio |
| 🔴 P0 | §5.1 WAV Decoder | Simplest format — system sounds |
| 🟠 P1 | §3 Audio Mixer | Multiple simultaneous sounds |
| 🟠 P1 | §5.2 MP3 Decoder | Most common music format |
| 🟡 P2 | §5.3 OGG Vorbis | Open audio format |
| 🟡 P2 | §5.6 Unified Audio Loader | Format-agnostic loading |
| 🟢 P3 | §5.4 FLAC Decoder | Lossless audio (niche) |
| 🔵 P4 | §5.5 MIDI Synthesis | Music creation |

---

## Key Files

| File | Purpose |
|------|---------|
| `src/kernel/drivers/ac97.c` | [NEW] AC97 sound card driver |
| `include/ac97.h` | [NEW] AC97 register definitions |
| `src/kernel/audio.c` | [NEW] Audio abstraction layer |
| `include/audio.h` | [NEW] Audio API header |
| `src/kernel/audio_mixer.c` | [NEW] Multi-stream PCM mixer |
| `include/dr_wav.h` | [NEW] WAV decoder (public domain) |
| `include/dr_mp3.h` | [NEW] MP3 decoder (public domain) |
| `src/libs/stb_vorbis_impl.c` | [NEW] OGG Vorbis decoder wrapper |
| `docs/architecture/audio.md` | [NEW] Audio system documentation |

---

## Effort Estimates

| Component | Effort | Dependencies |
|-----------|--------|-------------|
| AC97 driver | Weeks | PCI, DMA, IRQ |
| Audio abstraction | Days | AC97 driver |
| Audio mixer | Days | Audio abstraction |
| WAV decoder | Days | VFS file I/O |
| MP3 decoder | Days | VFS file I/O |
| OGG decoder | Days | VFS file I/O |
| Unified loader | Days | All decoders |
| **Total (core)** | **~2–3 weeks** | |
