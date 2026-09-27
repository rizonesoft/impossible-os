---
schema_version: 1
id: audio-system
domain: 10-platform-services
status: active
title: "TODO-01 -- Audio System & Media Player"
---

# TODO-01 -- Audio System & Media Player

> **Goal:** Build the complete software audio stack -- abstraction layer, multi-stream mixer, codec decoders (WAV/MP3/OGG/FLAC), unified loader, and the media player app + volume control UI on top. Hardware drivers live in `04-drivers-hardware/TODO-18-audio-drivers.md`.

> [!IMPORTANT]
> **Already exists**: AC97 hardware driver in `04-drivers-hardware/TODO-18-audio-drivers.md` (provides `ac97_play(pcm, samples)`, `ac97_stop()`, `ac97_set_volume(0–100)`, DMA buffer feeding). `pmm_alloc_contiguous()` for large PCM buffers. `registry_get/set()`. `CTRL_SLIDER` widget (TODO-05 forward dep). System sounds WAV hooks already wired in `09-desktop-shell/TODO-02` §4 (they call `audio_play()` once §1 exists). **Missing**: all of `src/kernel/audio.c`, `audio_mixer.c`, codec libs (`dr_wav.h`, `dr_mp3.h`, `stb_vorbis`), media player app, volume popup. **Syscalls**: `SYS_AUDIO_PLAY=60`, `SYS_AUDIO_VOLUME=61` (next after `SYS_PRIVILEGE_REQUEST=59`). **PMM rule**: all PCM buffers > 4 KB must use `pmm_alloc_contiguous()` -- `kmalloc` heap is only 2 MiB.

## Inputs

- `include/kernel/drivers/ac97.h` (TODO-10) -- `ac97_play/stop/set_volume()` -- §1 driver registration
- `include/kernel/mm/pmm.h` -- `pmm_alloc_contiguous()`, `pmm_free_contiguous()` -- §2 PCM DMA buffers, §3-6 decoded clip buffers
- `include/kernel/fs/vfs.h` -- `vfs_open/read/close()` -- §3-6 codec file loading
- `include/registry.h` -- `HKLM\SYSTEM\Sound\Volume`, `HKCU\Software\Impossible\MediaPlayer\*` -- §1 volume persist, §8 playlist persist
- `include/kernel/sched/syscall.h` -- `SYS_AUDIO_PLAY=60`, `SYS_AUDIO_VOLUME=61` (add)
- `include/kernel/timer.h` -- `sched_task_add()` -- §8 next-track, §9 OSD dismiss
- `include/desktop/controls.h` (TODO-05) -- `CTRL_SLIDER`, `CTRL_LISTVIEW`, `CTRL_TABSTRIP` -- §8 seek/vol slider, playlist, §10 mmsys.cpl
- `include/desktop/wm.h` -- `wm_create_window()` -- §8 media player window, §9 volume overlay pill
- `include/desktop/notification.h` (TODO-09) -- `notify_send()` -- §9 track-change toast
- `include/cpl.h` (TODO-11) -- `CPlApplet_t`, `NEWCPLINFO` -- §10 mmsys.cpl applet
- → XREF: `04-drivers-hardware/TODO-18-audio-drivers.md` -- AC97 + Intel HDA hardware drivers; §1 here depends on that
- → XREF: `09-desktop-shell/TODO-02 §4` -- system sound WAV hooks (`SOUND_STARTUP`, `SOUND_ERROR`, etc.) call `audio_play()` once §1 is live
- → XREF: `09-desktop-shell/TODO-11 §3` -- `mmsys.cpl` Control Panel entry; §10 here implements it
- → XREF: `08-graphics-ui/TODO-09-desktop-shell-features.md §8` -- quick settings volume slider that §9 binds to audio; `08-graphics-ui/TODO-11-startmenu-tray-notifications.md §4` -- system cluster volume glyph

## Outcome

- `audio_play/stop/set_volume/get_volume/is_playing()` kernel API; `SYS_AUDIO_PLAY=60` / `SYS_AUDIO_VOLUME=61`.
- 8-stream PCM mixer (INT32 accumulate, clamp, master volume, mute silence).
- `audio_load_wav/mp3/ogg/flac()` decoders; `audio_load(path)` unified dispatch.
- Media player app: transport controls, seek bar, volume slider, ID3 tags, playlist, repeat/shuffle.
- Volume in quick settings (slider, mute) and a 196 x 48 volume-key overlay pill above the taskbar (fades after 2 s).
- `mmsys.cpl` in Control Panel: master volume, output device selector, system sounds enable, test.

## Implementation Order

| ⭐  | Order | Deliverable                                                                               | Depends On                                                                                | Status |
| --- | :---: | ----------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | :----: |
| 💎  |   1   | §1 Audio abstraction -- `audio_device`, `audio_init/play/stop/volume`, Registry, syscalls | AC97 driver (TODO-10); `registry_set/get`; add `SYS_AUDIO_PLAY=60`, `SYS_AUDIO_VOLUME=61` |  [ ]   |
| ⭐  |   2   | §2 Audio mixer -- 8-stream INT32 accumulate + clamp, DMA feed, mute, per-stream handles   | §1 audio abstraction; `pmm_alloc_contiguous()` for mix buffer                             |  [ ]   |
| 💎  |   3   | §3 WAV decoder -- `dr_wav.h` vendor, `kmalloc/kfree` redirect, `audio_load_wav()`         | §1; `vfs_open/read()`                                                                     |  [ ]   |
| 💎  |   4   | §4 MP3 decoder -- `dr_mp3.h` vendor, `audio_load_mp3()`, linear resampler                 | §1; `vfs_open/read()`                                                                     |  [ ]   |
| 💎  |   5   | §5 OGG Vorbis -- `stb_vorbis.c` impl file, `audio_load_ogg()`                             | §1; `vfs_open/read()`                                                                     |  [ ]   |
| 💎  |   6   | §6 FLAC decoder (stretch) -- `dr_flac.h` vendor, `audio_load_flac()`                      | §1; `vfs_open/read()`                                                                     |  [ ]   |
| ⭐  |   7   | §7 Unified loader -- `audio_load(path)` extension dispatch → clip                         | §3 + §4 + §5 + §6                                                                         |  [ ]   |
| 💎  |   8   | §8 Media player app -- transport, seek bar, ID3v2 tags, playlist, file assoc              | §7; `CTRL_SLIDER`; `CTRL_LISTVIEW`; `dialog_file_open()` (TODO-05)                        |  [ ]   |
| 💎  |   9   | §9 Volume -- quick settings binding, volume-key overlay pill, glyph                       | §1; quick settings (D08 T09 §8); `sched_task_add()` for OSD dismiss                       |  [ ]   |
| 💎  |  10   | §10 `mmsys.cpl` -- master vol + mute + device selector + system sounds + test             | §1 + §9; `include/cpl.h` (TODO-11); `CTRL_SLIDER`                                         |  [ ]   |

---

## 1. Audio Abstraction Layer `[Sonnet]`

`struct audio_device` (name, sample_rate, channels, bits_per_sample, play_fn/stop_fn/volume_fn). `audio_init()` detects and registers sound hardware. `audio_play/stop/set_volume/get_volume/is_playing()`. Volume persisted in Registry. `SYS_AUDIO_PLAY=60`, `SYS_AUDIO_VOLUME=61`.

**Files:** `src/kernel/audio.c` (new), `include/audio.h` (new)

> [!NOTE]
> `audio_init()`: probe AC97 first via `ac97_probe()` → if found, register `g_audio_dev`; HDA probe stretch. `audio_play(const int16_t *pcm, uint32_t samples, uint32_t sample_rate)`: calls `g_audio_dev.play_fn()`; if `sample_rate != g_audio_dev.sample_rate`: resample via §4 linear interpolation stub. `audio_stop()`: `g_audio_dev.stop_fn()`. `audio_set_volume(0–100)`: clamp → `g_audio_dev.volume_fn(v)` + `registry_set("HKLM\\SYSTEM\\Sound\\Volume", REG_DWORD, &v, 4)`. `audio_get_volume()`: `registry_get(...)` → or cached `g_volume`. `audio_is_playing()`: atomic flag set/cleared in play_fn/stop_fn. **Syscalls**: `SYS_AUDIO_PLAY=60` (args: pcm_user_ptr, samples, sample_rate → copies via `vmm_copy_from_user`, calls `audio_play()`); `SYS_AUDIO_VOLUME=61` (args: volume 0–100 or -1 to query → set or get).

- [ ] `include/audio.h`: `struct audio_device`, `struct audio_clip { int16_t *pcm; uint32_t samples; uint32_t sample_rate; uint8_t channels; }`, API prototypes
- [ ] `src/kernel/audio.c`: `g_audio_dev`, `g_volume`, `g_is_playing` globals
- [ ] `audio_init()` -- AC97 probe + register; HDA stub
- [ ] `audio_play(pcm, samples, sample_rate)` -- dispatch; basic sample-rate mismatch passthrough (resampler stub until §4)
- [ ] `audio_stop()`, `audio_set_volume(v)`, `audio_get_volume()`, `audio_is_playing()`
- [ ] Registry persist: `HKLM\SYSTEM\Sound\Volume`, `HKLM\SYSTEM\Sound\Mute`
- [ ] `SYS_AUDIO_PLAY=60` in `syscall.h` + syscall handler: copy PCM from user, call `audio_play()`
- [ ] `SYS_AUDIO_VOLUME=61`: set or query; update Registry
- [ ] Commit: `"kernel: audio abstraction layer -- audio_device, play/stop/volume, SYS_AUDIO_PLAY=60"`

## 2. Audio Mixer `[Opus]`

8 simultaneous streams. `struct audio_stream` (PMM PCM buffer, sample count, position, per-stream volume, loop flag). Mixer thread sums active streams into INT32 accumulator, normalizes, clamps to INT16, applies master volume, feeds DMA ring buffer. `audio_mixer_stream_add()/stream_stop()`. Mute outputs silence without stopping mixer.

**Files:** `src/kernel/audio_mixer.c` (new), `include/audio_mixer.h` (new)

> [!NOTE]
> **PMM buffer**: mix buffer = `pmm_alloc_contiguous(1)` (4 KiB / 2048 INT16 samples). Ring buffer: front-half / back-half pattern; DMA plays front while mixer fills back; swap on DMA interrupt. **Stream table**: `g_streams[8]`; `audio_mixer_stream_add(pcm, samples, volume)` finds first `state==STREAM_FREE` slot; atomically sets `STREAM_PLAYING`. **Mix loop** (called from DMA completion ISR, or dedicated kernel thread at fixed period): for each output sample: `int32_t acc = 0`; for each active stream `i`: `acc += (g_streams[i].pcm[pos] * g_streams[i].vol) >> 8`; clamp `acc` to `[-32768, 32767]`; multiply by `g_master_vol / 100`. **Mute**: if `g_mute`: fill DMA buffer with zeros (streams keep advancing their positions -- no click artifact on un-mute). **Per-stream looping**: if `stream.loop` and `position >= sample_count`: `position = 0`. **Concurrency**: spinlock `g_mix_lock` protects stream table modifications; ISR increments atomic position.

- [ ] `include/audio_mixer.h`: `struct audio_stream`, `STREAM_FREE/PLAYING/DONE` states, `audio_mixer_stream_add/stream_stop/stream_is_done()` prototypes
- [ ] `src/kernel/audio_mixer.c`: `g_streams[8]`, `g_mix_lock`, `g_mute`, `g_master_vol`
- [ ] PMM mix buffer: `pmm_alloc_contiguous(1)` for INT32 accumulator + output INT16 buffer
- [ ] Mix loop: INT32 accumulate per sample → clamp → scale by `g_master_vol`
- [ ] DMA ring-buffer half: front/back swap on DMA completion callback from AC97 driver
- [ ] Per-stream `loop` flag support; `STREAM_DONE` state set on completion
- [ ] `g_mute` path: zero-fill DMA buffer; do NOT pause stream position advance
- [ ] Spinlock guard on `g_streams[]` table; atomic `g_is_playing` set when any stream active
- [ ] `audio_mixer_stream_add(pcm, samples, vol_0_255)` → handle (0–7); -1 on full
- [ ] `audio_mixer_stream_stop(handle)` → mark `STREAM_FREE`
- [ ] Commit: `"kernel: audio mixer -- 8-stream INT32 mix, DMA ring, mute, per-stream volume"`

## 3. WAV Decoder `[Sonnet]`

Vendor `dr_wav.h` (public domain, ~1500 lines) at `include/libs/dr_wav.h`. Redirect `DRWAV_MALLOC/FREE` → `kmalloc/kfree`. `audio_load_wav(path)` → `audio_clip`. Support 8/16-bit, mono/stereo, 22050/44100/48000 Hz.

**Files:** `include/libs/dr_wav.h` (new vendor), `src/kernel/audio_wav.c` (new)

> [!NOTE]
> Define `DRWAV_IMPLEMENTATION` in `audio_wav.c` only. `#define DRWAV_MALLOC(sz) kmalloc(sz)` / `DRWAV_FREE(p) kfree(p)` -- place before `#include "libs/dr_wav.h"`. `audio_load_wav(path)`: `vfs_open()` → `vfs_read()` full file → `drwav_init_memory(&wav, data, size, NULL)` → `drwav_read_pcm_frames_s16()` → fill `audio_clip`. PCM buffer alloc: `samples > 2048` → `pmm_alloc_contiguous(ceil(bytes / 4096))` else `kmalloc`. 8-bit unsigned → `dr_wav` converts to s16 internally via `drwav_read_pcm_frames_s16`. Channel count preserved (mono/stereo); `audio_play()` will handle mono duplication if driver requires stereo. `drwav_uninit()` after decode (dr_wav keeps no references after uninit). `audio_clip_free(clip)` frees PMM or kmalloc buffer based on size flag.

- [ ] Download / vendor `dr_wav.h` to `include/libs/dr_wav.h` (public domain -- David Reid)
- [ ] `src/kernel/audio_wav.c`: `#define DRWAV_IMPLEMENTATION`, `DRWAV_MALLOC/FREE` redirects
- [ ] `audio_load_wav(const char *path)` → `audio_clip *` (PMM or kmalloc by size)
- [ ] Support 8-bit unsigned + 16-bit signed input; mono + stereo; 22050/44100/48000 Hz
- [ ] `audio_clip_free(audio_clip *clip)` -- PMM or kmalloc free based on internal size flag
- [ ] Test: load `C:\Impossible\Media\startup.wav` → play via `audio_play()` → audible chime
- [ ] Commit: `"libs: dr_wav -- WAV decoder, freestanding kmalloc/kfree redirect, audio_load_wav"`

## 4. MP3 Decoder `[Sonnet]`

Vendor `dr_mp3.h` (public domain, ~3000 lines) at `include/libs/dr_mp3.h`. Same redirect pattern. `audio_load_mp3(path)` → `audio_clip`. Linear interpolation resampler if driver rate differs.

**Files:** `include/libs/dr_mp3.h` (new vendor), `src/kernel/audio_mp3.c` (new)

> [!NOTE]
> `DRMP3_MALLOC/REALLOC/FREE` → `kmalloc`/`kmalloc_resize` (or `kmalloc` new + `kfree` old)/`kfree`. `audio_load_mp3()`: VFS read full file → `drmp3_init_memory()` → `drmp3_read_pcm_frames_s16()` → `audio_clip`. MP3 is commonly 44100 Hz stereo MPEG-1 Layer 3; driver may expect 48000 Hz. **Linear resampler**: if `clip.sample_rate != g_audio_dev.sample_rate`: output_samples = `clip.sample_count * target_rate / clip.sample_rate`; for each output index `i`: `float src_pos = i * (float)clip.sample_rate / target_rate`; `int s0 = src_pos`, `frac = src_pos - s0`; `out[i] = pcm[s0] + frac * (pcm[s0+1] - pcm[s0])`. PMM alloc for resampled buffer if > 4 KiB.

- [ ] Vendor `dr_mp3.h` to `include/libs/dr_mp3.h`
- [ ] `src/kernel/audio_mp3.c`: `DRMP3_MALLOC/REALLOC/FREE` redirects + `DRMP3_IMPLEMENTATION`
- [ ] `audio_load_mp3(const char *path)` → `audio_clip *`
- [ ] Linear interpolation resampler: `audio_resample(clip, target_rate)` → resampled `audio_clip`
- [ ] `audio_resample()` called automatically in `audio_load_mp3()` if rate mismatch with `g_audio_dev`
- [ ] PMM alloc for resampled buffer when `output_bytes > 4096`
- [ ] Test: load `.mp3` track → play → audible music output
- [ ] Commit: `"libs: dr_mp3 -- MP3 decoder, freestanding redirects, audio_load_mp3, linear resampler"`

## 5. OGG Vorbis Decoder `[Sonnet]`

Vendor `stb_vorbis.c` as `src/libs/stb_vorbis_impl.c` (same pattern as `stb_truetype`). Redirect `malloc/realloc/free` → `kmalloc/kfree`. `audio_load_ogg(path)` → `audio_clip`.

**Files:** `src/libs/stb_vorbis_impl.c` (new), `include/libs/stb_vorbis.h` (new -- split from single-header)

> [!NOTE]
> `stb_vorbis` is included in the project as `stb_vorbis.c` -- define `STB_VORBIS_IMPLEMENTATION` in `stb_vorbis_impl.c`, set `#define malloc kmalloc`, `#define free kfree`, `#define realloc(p,s) kmalloc(s)` (with kfree old pointer manually if needed -- or use `stb_vorbis_alloc` struct to pass a fixed PMM arena). Preferred: `stb_vorbis_open_memory()` with arena buffer from `pmm_alloc_contiguous()` → no `realloc` calls. `audio_load_ogg(path)`: VFS read → `stb_vorbis_open_memory(data, size, &err, &alloc)` → `stb_vorbis_get_samples_short_interleaved()` → `audio_clip`. Check `stb_vorbis_get_info()` for `sample_rate` + `channels`; apply `audio_resample()` if needed.

- [ ] `src/libs/stb_vorbis_impl.c`: `STB_VORBIS_IMPLEMENTATION`, PMM arena alloc struct
- [ ] `include/libs/stb_vorbis.h` split header (or use `#include "stb_vorbis.c"` approach matching existing `stb_truetype`)
- [ ] `audio_load_ogg(const char *path)` → `audio_clip *`; PMM alloc for decoded PCM
- [ ] `stb_vorbis_alloc` struct backed by `pmm_alloc_contiguous()` -- eliminate `realloc` dependency
- [ ] Apply `audio_resample()` if driver rate differs
- [ ] Test: play `.ogg` audio file
- [ ] Commit: `"libs: stb_vorbis -- OGG decoder, PMM arena alloc, audio_load_ogg"`

## 6. FLAC Decoder (Stretch) `[Sonnet]`

Vendor `dr_flac.h` (public domain, ~5000 lines). Same redirect pattern. `audio_load_flac(path)` → `audio_clip`. Lossless decode; may be large buffers -- PMM mandatory for output.

**Files:** `include/libs/dr_flac.h` (new vendor), `src/kernel/audio_flac.c` (new)

> [!NOTE]
> `DRFLAC_MALLOC/REALLOC/FREE` → `kmalloc`/`kmalloc_resize` (alloc new + copy + free old)/`kfree`. FLAC output is 16/24-bit; use `drflac_read_pcm_frames_s16()` for uniform 16-bit output. FLAC files are lossless and can be large (e.g., 50 MB for album track) -- **always PMM** for output buffer regardless of size: `pmm_alloc_contiguous(ceil(output_bytes / 4096))`. Log a warning via `klog_warn()` if `output_bytes > 64 MB`.

- [ ] Vendor `dr_flac.h` to `include/libs/dr_flac.h`
- [ ] `src/kernel/audio_flac.c`: `DRFLAC_MALLOC/REALLOC/FREE` redirects + `DRFLAC_IMPLEMENTATION`
- [ ] `audio_load_flac(const char *path)` → `audio_clip *`; always PMM alloc
- [ ] `drflac_read_pcm_frames_s16()` for uniform 16-bit output
- [ ] `klog_warn()` if decoded buffer > 64 MiB
- [ ] Apply `audio_resample()` if needed
- [ ] Test: play lossless `.flac` file
- [ ] Commit: `"libs: dr_flac -- FLAC decoder, freestanding redirects, audio_load_flac"`

## 7. Unified Audio Loader `[Sonnet]`

`audio_load(path)` detects format by extension (`.wav`/`.mp3`/`.ogg`/`.flac`) → dispatches to decoder → returns `audio_clip *`. Single entry point for all callers: media player, system sounds, any app via `SYS_AUDIO_PLAY`.

**Files:** `src/kernel/audio_load.c` (new, or extend `audio.c`)

> [!NOTE]
> `audio_load(path)`: `const char *ext = strrchr(path, '.')` → lowercase compare → dispatch. Unknown extension → `klog_err("audio_load: unsupported format: %s", ext)` → return NULL. Returned `audio_clip *` is PMM-backed (or kmalloc for small WAVs); caller frees with `audio_clip_free(clip)`. This function is used by `09-desktop-shell/TODO-02 §4` system sounds once live.

- [ ] `audio_clip *audio_load(const char *path)` -- extension dispatch
- [ ] `void audio_clip_free(audio_clip *clip)` -- PMM or kmalloc branch based on internal `clip->pmm_backed` flag
- [ ] `void audio_play_file(const char *path)` convenience: `audio_load()` → `audio_play()` → `audio_clip_free()` (fire and forget)
- [ ] Plug into `SYS_AUDIO_PLAY` handler: if args contain path string instead of raw PCM → route to `audio_load()` then `audio_play()`
- [ ] System sounds in `09-desktop-shell/TODO-02 §4` call `audio_play_file(SOUND_STARTUP_PATH)` -- confirm API match
- [ ] Test: `audio_load("test.ogg")` → non-NULL; `audio_load("test.xyz")` → NULL + klog
- [ ] Commit: `"kernel: audio_load -- unified format dispatch, audio_play_file, audio_clip_free"`

## 8. Media Player App `[Sonnet]`

**Design:** [`shell.md#window-chrome`](../../docs/design/shell.md#window-chrome), [`controls.md#which-rules-apply-to-every-control`](../../docs/design/controls.md#which-rules-apply-to-every-control)

`src/apps/mediaplayer/mediaplayer.c`. Transport: Play/Pause, Stop, Prev/Next. Seek bar + volume slider + mute. Song title/artist from ID3v2 tags. Playlist panel. File associations. Shuffle/repeat.

**Files:** `src/apps/mediaplayer/mediaplayer.c` (new), `include/apps/mediaplayer.h` (new)

> [!NOTE]
> Window: 700×440 px. **Layout**: top 80 px = album art stub (gray rect) + title/artist; below that: seek bar `CTRL_SLIDER(0, total_samples, pos)` + time labels; transport row (|◀ ▶/⏸ ■ ▶|); volume slider (0–100) + mute 🔊; bottom 280 px = playlist `CTRL_LISTVIEW` (Track#, Title, Artist, Duration, Size). **Playback**: `audio_mixer_stream_add(clip.pcm, clip.samples, 255)` returns stream handle; timer via `sched_task_add("mp_tick", mediaplayer_tick, 1, 1)` updates seek position (`audio_mixer_stream_get_pos(handle) * 100 / total_samples`). **ID3v2 tags**: read first 512 bytes of file; check for `"ID3"` at offset 0; scan frames for `TIT2` (title) and `TPE1` (artist); parse 4-byte size (BE) + 2-byte flags + text encoding byte + text data. Skip if no ID3 header -- use filename as title. **Playlist**: `CTRL_LISTVIEW`; "Add Files" → `dialog_file_open(multi, "Audio files\0*.mp3;*.wav;*.ogg;*.flac\0")` → append rows; double-click → `mediaplayer_play_index(i)`; delete → remove row. **Prev/Next**: index into playlist. **Shuffle**: `kmath_rand()` next unplayed index. **Repeat**: all / one / off toggle. **File assoc**: `.mp3`/`.wav`/`.ogg`/`.flac` → `mediaplayer.exe` (register in TODO-02 §1).

- [ ] `void mediaplayer_open(const char *initial_path)` -- window + layout; load if `initial_path != NULL`
- [ ] `void mediaplayer_play(int playlist_idx)` -- `audio_load(path)` → `audio_mixer_stream_add()`; store handle
- [ ] `void mediaplayer_tick(void)` -- update seek `CTRL_SLIDER` position; detect stream done → auto-next
- [ ] Pause: save stream position; `audio_mixer_stream_stop()`; resume: `audio_mixer_stream_add()` from offset
- [ ] Seek: `CTRL_SLIDER` drag → `stream_set_position()` stub (or stop + restart from offset)
- [ ] ID3v2 parser: `mediaplayer_read_id3v2(path, title, artist, buflen)` -- read 512 bytes; scan TIT2/TPE1
- [ ] Volume slider → `audio_set_volume(v)` in real-time; mute toggle → `audio_set_mute()`
- [ ] Playlist add/remove/double-click; shuffle: `kmath_rand()` index; repeat modes
- [ ] Register `.mp3/.wav/.ogg/.flac` file associations (TODO-02 §1)
- [ ] Commit: `"apps: media player -- transport, seek, ID3v2 tags, playlist, shuffle/repeat, file assoc"`

## 9. Volume Control Popup `[Sonnet]`

**Design:** [`shell.md#volume-and-brightness-overlay`](../../docs/design/shell.md#volume-and-brightness-overlay)

Volume has no popup of its own: the volume slider and mute live in the quick settings flyout (`08-graphics-ui/TODO-09 §8`), opened from the taskbar system cluster (`08-graphics-ui/TODO-11 §4`). This section wires audio into those surfaces and adds the keyboard overlay: volume keys → ±5 and the volume overlay pill per `docs/design/shell.md#volume-and-brightness-overlay`.

**Files:** `src/desktop/volume_osd.c` (new), `include/desktop/volume_osd.h` (new)

> [!NOTE]
> **Quick settings binding**: the quick settings volume slider calls `audio_set_volume(v)` on each drag tick and reads `audio_get_volume()` when the flyout opens; its speaker glyph toggles `audio_set_mute()`. **System cluster glyph**: on every volume or mute change call `systray_update_volume_glyph()` (muted / low / mid / high). **Volume overlay**: on `VK_VOLUME_UP` / `VK_VOLUME_DOWN` / `VK_VOLUME_MUTE` adjust ±5 or toggle mute, then show a `THEME_SIZE_OSD_WIDTH` x `THEME_SIZE_OSD_HEIGHT` (196 x 48) flyout-acrylic pill centred horizontally 24 px above the taskbar (`z_order=20000`): speaker glyph, a thumbless slider track per `docs/design/controls.md#slider`, and the value; it fades out 2 s after the last change (`sched_task_add("vol_osd_dismiss", vol_osd_close, 2, 0)`, re-armed on each key).

- [ ] Quick settings volume slider → `audio_set_volume(v)` real-time; flyout open reads `audio_get_volume()`
- [ ] Quick settings speaker glyph → `audio_set_mute(!mute)`; slider visually disabled while muted
- [ ] `VK_VOLUME_UP/DOWN` global hotkeys → ±5 volume adjust; `VK_VOLUME_MUTE` → `audio_set_mute(toggle)`
- [ ] Volume overlay: 196 x 48 flyout-acrylic pill, centred 24 px above the taskbar; glyph + thumbless accent track + value; fades 2 s after the last key
- [ ] `systray_update_volume_glyph()` on every level or mute change (muted / low / mid / high)
- [ ] Commit: `"desktop: volume -- quick settings binding, volume key overlay, system cluster glyph"`

## 10. `mmsys.cpl` -- Sound Settings Applet `[Sonnet]`

**Design:** [`shell.md#settings-and-control-panel-frame`](../../docs/design/shell.md#settings-and-control-panel-frame), [`controls.md#cards-and-settings-rows`](../../docs/design/controls.md#cards-and-settings-rows)

Control Panel Sound applet: master volume slider + mute + output device selector + system sounds enable/disable + test sound button. Extends `09-desktop-shell/TODO-11 §3`.

**Files:** `src/apps/control/applets/mmsys.c` (new)

> [!NOTE]
> → XREF: `09-desktop-shell/TODO-11 §3` -- `mmsys.cpl` stub is registered there; §10 here implements its content. **Layout**: the page renders inside the Settings and Control Panel frame (`docs/design/shell.md#settings-and-control-panel-frame`) as settings rows (`docs/design/controls.md#cards-and-settings-rows`), grouped under "Output" and "System sounds" headings. Output: device row with a combo box (from `audio_get_device_list()` -- initially "AC97" or "Intel HDA"), volume row with a slider + current %, mute row with a toggle switch. System sounds: enable row with a toggle switch, "Test sound" row with a standard button → `audio_play_file(SOUND_STARTUP_PATH)`. Output quality: a read-only row showing the sample rate (`g_audio_dev.sample_rate`). Every change applies immediately and is written to the Registry; there is no Apply or Cancel button.

- [ ] `src/apps/control/applets/mmsys.c` implementing `CPlApplet()` messages `CPL_INIT/INQUIRE/DBLCLK/STOP`
- [ ] Settings rows: device combo box, volume slider, mute toggle switch (controls per `docs/design/controls.md`)
- [ ] `audio_get_device_list(names, max)` stub in `audio.c` -- returns registered device name(s)
- [ ] System sounds toggle switch → `HKLM\SYSTEM\Sound\SystemSoundsEnabled`
- [ ] "Test sound" button → `audio_play_file("C:\\Impossible\\Media\\test.wav")`
- [ ] Sample rate read-only row from `g_audio_dev.sample_rate`
- [ ] Slider drag → immediate `audio_set_volume()`; the quick settings slider reflects it next time it opens
- [ ] Changes apply immediately and persist to the Registry (no Apply/Cancel)
- [ ] Commit: `"mmsys.cpl: sound settings -- volume slider, mute, device selector, system sounds, test"`

---

## OS Comparison


| ⭐  | Feature                                        | 🪟 Win11                                           | 🐧 Linux                                             | 🚀 Impossible OS                                                           |
| --- | ---------------------------------------------- | -------------------------------------------------- | ---------------------------------------------------- | -------------------------------------------------------------------------- |
| 💎  | Audio abstraction layer -- driver-agnostic API | ✅ WDM kernel streaming; WASAPI; Core              | ✅ ALSA kernel API; PulseAudio/PipeWire userspace;   | ⬜ §1 -- `struct audio_device` fn-ptr dispatch; `audio_play/stop/volume()` |
| ⭐  | Audio mixer                                    | ✅ KMixer (kernel); WASAPI exclusive/shared; HDA   | ✅ PulseAudio/PipeWire: userspace daemon; ALSA dmix: | ⬜ §2 -- `⭐` INT32 accumulate + clamp                                     |
| 💎  | WAV decoder                                    | ✅ MFMediaSource; DirectShow; built-in WAV support | ✅ libsndfile; FFmpeg; GStreamer; ALSA `aplay`       | ⬜ §3 -- `dr_wav.h` + `kmalloc/kfree` redirect; PMM                        |
| 💎  | MP3 decoder -- `dr_mp3.h` + linear resampler   | ✅ Media Foundation MP3 decoder; WMP;              | ✅ FFmpeg; mpg123; GStreamer bad plugins             | ⬜ §4 -- `dr_mp3.h`; linear interpolation resampler for                    |
| 💎  | OGG Vorbis decoder -- `stb_vorbis`, PMM arena  | ⚠️ No native OGG support; requires                 | ✅ libvorbis; FFmpeg; GStreamer; native in           | ⬜ §5 -- `stb_vorbis` PMM arena (`realloc`-free); `audio_load_ogg()`       |
| 💎  | FLAC decoder (stretch) -- `dr_flac.h` lossless | ⚠️ No native FLAC; requires Windows                | ✅ libFLAC; FFmpeg; GStreamer; native in             | ⬜ §6 -- (stretch) -- ; `dr_flac.h`; always                                |
| ⭐  | Unified loader                                 | ❌ No single API; requires per-format              | ⚠️ FFmpeg `avformat_open_input` is unified but       | ⬜ §7 -- `⭐` single `audio_load(path)` → `audio_clip                      |
| 💎  | Media player                                   | ✅ Windows Media Player / Groove                   | ✅ Rhythmbox; Banshee; Clementine; mpv; VLC;         | ⬜ §8 -- `⭐` in-kernel ID3v2 parser (no                                   |
| 💎  | Volume slider, mute, keyboard OSD              | ✅ System tray volume flyout; OSD                  | ✅ PulseAudio / PipeWire tray icon                   | ⬜ §9 -- in-kernel compositor OSD (no notify                               |
| 💎  | Sound Control Panel (`mmsys.cpl`)              | ✅ Sound control panel; per-app volume             | ✅ PulseAudio Volume Control; PipeWire settings;     | ⬜ §10 -- CPL applet (TODO-11 framework); `CTRL_DROPDOWN`                  |

> **After §1–§10:** Impossible OS has a self-contained in-kernel audio stack with no userspace daemon (unlike PulseAudio/PipeWire). The `⭐` advantages: the mixer runs INT32 accumulate + clamp directly in the DMA completion ISR (zero-copy, no scheduling latency); `audio_load()` is the only OS to provide a single in-kernel format-agnostic loader with no library dependencies; the volume OSD dismisses via a PIT-ticked scheduler task rather than a userspace notification daemon.

## Verification

- [ ] `bash scripts/build.sh clean` → `tail -1 build/build.log` → `=== BUILD OK ===`
- [ ] QEMU: boot → startup chime plays (system sounds WAV via `audio_play_file()`)
- [ ] `audio_set_volume(50)` in shell (or `SYS_AUDIO_VOLUME=61`) → volume halved; `audio_get_volume()` → 50
- [ ] Play 8-stream test: 8 simultaneous WAV clips → all audible; no clipping distortion
- [ ] Load `.mp3` → `audio_load_mp3()` → non-NULL clip; play → audible; rate mismatch → resampler triggered
- [ ] Load `.ogg` → `audio_load_ogg()` → non-NULL; play → audible
- [ ] `audio_load("test.xyz")` → NULL + `klog` warning
- [ ] Open Media Player → "Add Files" `.mp3` → double-click → plays; seek bar moves; Pause/Resume works
- [ ] ID3v2: MP3 with TIT2/TPE1 tags → title/artist shown in player window (not filename)
- [ ] Open quick settings from the system cluster → drag the volume slider → volume changes in real time; the cluster glyph follows
- [ ] VK_VOLUME_UP keyboard → volume +5; a 196 x 48 pill appears 24 px above the taskbar and fades 2 s after the last key
- [ ] Control Panel → Sound (`mmsys.cpl`) → volume slider synced with system; "Test" → plays test WAV
- [ ] Commit: `"audio: abstraction, mixer, WAV/MP3/OGG/FLAC codecs, media player, volume popup, mmsys.cpl -- all complete"`
