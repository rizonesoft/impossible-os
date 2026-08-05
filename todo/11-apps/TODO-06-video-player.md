---
schema_version: 1
id: video-player
domain: 11-apps
status: active
title: "TODO-06 -- Video Player"
---

# TODO-06 -- Video Player

> **Goal:** Build `player.exe` -- a video player for Impossible OS backed by **pl_mpeg** (public
> domain, single-header MPEG-1/MPEG audio decoder). Decodes MPEG-1 video frames, converts YCbCr
> to RGB via an SSE2-optimised path, synchronises A/V streams against a wall-clock baseline,
> and presents a clean windowed UI with seek, zoom, fullscreen, playlist, and SRT subtitles.

> [!IMPORTANT]
> Audio subsystem (`audio_mixer_stream_add`, `audio_mixer_stream_stop`) from
> `10-platform-services/TODO-01-audio-system.md §2` **must** be complete before §3 A/V sync --
> `→ XREF: 10-platform-services/TODO-01 §2`.
> All video frame buffers and PCM decode buffers are large (frame = width × height × 4 bytes) --
> **always use `pmm_alloc_contiguous()` for these**; `kmalloc` heap is only 2 MiB.
> SSE2 is available (`-msse2` is already in the kernel build flags) but the kernel uses
> `-mno-sse` / `-mno-sse2` by default; the player app compiles separately as a user-mode EIF
> with SSE2 enabled.

---

## Inputs

- `10-platform-services/TODO-01-audio-system.md §2` -- `audio_mixer_stream_add(pcm, samples, vol)`, `audio_mixer_stream_stop(handle)` from `include/audio_mixer.h`
- `include/kernel/timer.h` -- `system_get_ticks()` (monotonic ms counter), `sleep_ms(ms)`
- `include/gfx.h` -- `gfx_surface_create()`, `gfx_fill_rect()`, `gfx_blit()`, `gfx_scale_blit()`
- `include/desktop/controls.h` -- `CTRL_BUTTON`, `CTRL_SCROLLBAR` (seek bar via `CTRL_SCROLLBAR_HORIZ`), `CTRL_TEXTBOX`
- `include/desktop/wm.h` -- `wm_create_window()`, `wm_set_fullscreen()`, `wm_mark_dirty()`
- `include/desktop/file_assoc.h` -- `file_assoc_set(ext, prog_id, path)` (→ XREF `09-desktop-shell/TODO-02 §1`)
- `include/registry.h` -- `reg_set_string`, `reg_get_string`, `reg_enum_keys`
- `include/kernel/vfs.h` -- `vfs_open`, `vfs_read`, `vfs_stat` -- file I/O for pl_mpeg + SRT

---

## Outcome

`player.exe file.mpg` plays back MPEG-1 video with synchronised audio. Full playback controls (seek bar, play/pause, volume), fullscreen mode with auto-hiding controls, playlist sidebar, and SRT subtitle overlay. `.mpg` / `.mpeg` / `.avi` file associations open in the player. Foundation is extensible to H.264 (Phase 2) via codec swap.

---

## Implementation Order

| Step | Section                    | 💎/⭐ | Dependency                                   |
| ---- | -------------------------- | ----- | -------------------------------------------- |
| 1    | pl_mpeg Integration        | 💎    | VFS file I/O                                 |
| 2    | YCbCr → RGB Conversion     | 💎    | §1 `plm_frame_t` output                      |
| 3    | A/V Sync Engine            | 💎    | §1 `plm_samples_t`, `audio_mixer` TODO-01 §2 |
| 4    | Player UI                  | 💎    | §2 page surface, §3 timing, controls.h, wm.h |
| 5    | File Operations            | 💎    | §4 app exists, `file_assoc_set`              |
| 6    | Playlist                   | ⭐    | §4 + §5 complete                             |
| 7    | Subtitle Support (Stretch) | ⭐    | §4 stable, `ttf_draw_string`                 |
| 8    | Format Support Roadmap     | ⭐    | §1 MPEG-1 proven                             |

---

## 1. pl_mpeg Integration `[Sonnet]`

**Source file:** `src/apps/player/pl_mpeg_port.c`; copy `PL_MPEG_IMPLEMENTATION` header to `include/libs/pl_mpeg.h`

- [ ] Copy `pl_mpeg.h` (single-header, ~3500 lines, public domain) to `include/libs/pl_mpeg.h`
- [ ] Redirect allocator macros before `#define PL_MPEG_IMPLEMENTATION`:
  ```c
  #define PLMPEG_MALLOC(sz)      pmm_alloc_contiguous(PAGES(sz))
  #define PLMPEG_FREE(ptr, sz)   pmm_free_contiguous(ptr, PAGES(sz))
  #define PLMPEG_REALLOC(p,o,n)  plmpeg_realloc_shim(p, o, n)
  ```
  where `PAGES(sz) = (sz + 4095) / 4096`; implement `plmpeg_realloc_shim` as alloc-copy-free
- [ ] File I/O callbacks: pl_mpeg needs `load_buffer_cb(plm, buffer, len, user)` and `seek_cb(plm, pos, user)` -- wire to `vfs_read` / `vfs_seek` using the open fd passed as `user`
- [ ] `plm_t *plm = plm_create_with_callbacks(load_cb, seek_cb, buf_size=8192, FALSE, fd)`
- [ ] `plm_get_width(plm)`, `plm_get_height(plm)` → allocate output `gfx_surface_t` via `gfx_surface_create(w, h)`
- [ ] `plm_get_framerate(plm)` → compute `frame_period_ms = 1000.0 / fps`
- [ ] `plm_get_duration(plm)` → total seconds for seek bar range
- [ ] Allocate decode PCM buffer: `plm_samples_t` provides float PCM; convert float → INT16 in `audio_bridge_cb` (see §3)
- [ ] Verify: open `test.mpg`; serial log shows correct `width × height @ fps`, non-zero `duration`

---

## 2. YCbCr → RGB Conversion `[Opus]`

**Source file:** `src/apps/player/yuv_convert.c`

> YCbCr → RGB conversion is a tight inner loop executed at 25–30 fps for every pixel of the
> frame. Naive C is correct but too slow at HD resolutions; SSE2 SIMD processes 4 output pixels
> per iteration, keeping frame decode within the frame period. The SSE2 path is novel hardware
> usage in a non-kernel context and uses intrinsics unavailable elsewhere in the build.

- [ ] `void yuv420_to_rgb(const plm_frame_t *frame, gfx_surface_t *out)`:
  - [ ] For each 2×2 luma block (luma step = 1 pixel, chroma step = 1 per 2 pixels):
    - [ ] Fetch `Y0, Y1, Y2, Y3` from `frame->y.data` (4 luma pixels)
    - [ ] Fetch shared `Cb`, `Cr` from `frame->cb.data`, `frame->cr.data`
    - [ ] BT.601 fixed-point conversion (scale by 256 to avoid floats):
      - `R = Y + ((359 × (Cr − 128)) >> 8)`
      - `G = Y − ((88 × (Cb − 128) + 183 × (Cr − 128)) >> 8)`
      - `B = Y + ((454 × (Cb − 128)) >> 8)`
    - [ ] Clamp R, G, B to 0–255; write `0xFF000000 | (R<<16) | (G<<8) | B` to `out->pixels`
  - [ ] Correctness first: pure C scalar path; verify against reference frame from `pl_mpeg` sample
- [ ] SSE2 optimisation (after scalar path verified correct):
  - [ ] Process 4 pixels per iteration using 128-bit XMM registers (`__m128i`)
  - [ ] Load 4× Y values into one XMM; broadcast one Cb and one Cr across lanes
  - [ ] Apply BT.601 multipliers via `_mm_mulhi_epi16` / `_mm_add_epi16`
  - [ ] Pack to `_mm_packus_epi16` (clamped uint8); interleave into RGBA with `_mm_shuffle_epi8`
  - [ ] Store 4 ARGB pixels with `_mm_storeu_si128`
  - [ ] Fallback: compile `#ifdef __SSE2__` guard; scalar path used if SSE2 unavailable
- [ ] Benchmark: log `yuv_convert: {w}×{h} frame in {elapsed_us} µs` at debug build; target < 1 frame period ms

---

## 3. A/V Sync Engine `[Opus]`

> A/V sync requires a 3-frame ring buffer, wall-clock PTS comparison, and drift correction.
> The audio feed path runs asynchronously through the mixer DMA ISR; the video decode loop
> must hold the correct frame on screen without busy-waiting. This involves subtle timing
> and per-stream state management.

**Source file:** `src/apps/player/av_sync.c`; header `include/apps/player/av_sync.h`

- [ ] `struct video_player` definition:
  ```c
  typedef struct {
      plm_t          *plm;
      int             audio_handle;     /* audio_mixer stream handle */
      uint64_t        start_ticks;      /* system_get_ticks() at first play */
      uint64_t        pause_offset_ms;  /* accumulated paused time */
      gfx_surface_t   ring[3];          /* decoded RGB frames */
      int             ring_head;        /* next slot to decode into */
      int             ring_tail;        /* slot currently displayed */
      double          frame_pts[3];     /* PTS for each ring slot */
      enum { VP_STOPPED, VP_PLAYING, VP_PAUSED } state;
  } video_player_t;
  ```
- [ ] `vp_create(fd)` → allocate struct + `gfx_surface_create` for all 3 ring slots
- [ ] `vp_play(vp)`: record `start_ticks = system_get_ticks() - pause_offset_ms`; set `VP_PLAYING`
- [ ] `vp_pause(vp)`: record `pause_start`; set `VP_PAUSED`; `pause_offset_ms += now - pause_start` on resume
- [ ] `vp_seek(vp, seconds)`: `plm_seek(vp->plm, seconds, TRUE)`; reset `start_ticks`; flush ring buffer
- [ ] **Decode loop** (runs in dedicated kernel thread `task_create("player_decode", ...)`)
  - [ ] While `VP_PLAYING`:
    - [ ] Compute `video_time = (system_get_ticks() - start_ticks) / 1000.0` (seconds)
    - [ ] `frame = plm_decode_video(vp->plm, video_time)` → `NULL` if nothing due yet
    - [ ] If `frame != NULL`: `yuv420_to_rgb(frame, &ring[ring_head])`; record `frame_pts[ring_head] = frame->time`; advance `ring_head = (ring_head + 1) % 3`
    - [ ] Check for display: if `frame_pts[ring_tail]` ≤ `video_time`: advance `ring_tail`; signal compositor to blit new frame
    - [ ] **Drift correction**: `audio_ahead = audio_pts - video_pts`
      - If `audio_ahead > 0.200 s` (audio ahead): skip display of current video frame (fast-forward video)
      - If `audio_ahead < −0.200 s` (video ahead): repeat last frame (`ring_tail` unchanged); extend sleep
    - [ ] Sleep: `frame_remaining_ms = frame_pts[ring_tail+1] - video_time) × 1000`; `sleep_ms(max(1, frame_remaining_ms - 2))`
- [ ] **Audio feed**: pl_mpeg audio callback `audio_cb(plm, samples, count, user)` (registered via `plm_set_audio_decode_callback`):
  - [ ] Convert `float samples[]` (range ±1.0) → `int16_t pcm[]` (× 32767); buffer in `pmm_alloc_contiguous`
  - [ ] `audio_mixer_stream_add(pcm, count, 220)` → store handle in `vp->audio_handle`
  - [ ] Old stream auto-stops when `STREAM_DONE`; ring of 2 audio buffers to avoid gaps

---

## 4. Player UI `[Sonnet]`

**Source file:** `src/apps/player/player_ui.c`

- [ ] **Window**: `wm_create_window("Impossible Player -- {filename}", 800, 520)` (resizable)
- [ ] **Video canvas** (fills window above controls bar):
  - [ ] Compute aspect-ratio-correct letterbox: `scale = min(canvas_w / vid_w, canvas_h / vid_h)`; `dst_w = vid_w × scale`, `dst_h = vid_h × scale`; offset `x = (canvas_w - dst_w)/2`, `y = (canvas_h - dst_h)/2`
  - [ ] Black `gfx_fill_rect` for letterbox bars
  - [ ] `gfx_scale_blit(canvas, x, y, dst_w, dst_h, ring[ring_tail].pixels, vid_w, vid_h)` on each new frame
- [ ] **Controls bar** (fixed 48 px, shows on hover in fullscreen):
  - [ ] `[⏮]` rewind 10 s; `[⏯]` play/pause toggle; `[⏭]` forward 10 s; `[⏹]` stop
  - [ ] Seek bar: `CTRL_SCROLLBAR_HORIZ`, range 0–`plm_get_duration()`, step = 1 s; click/drag → `vp_seek()`; updates as video plays
  - [ ] Time display `CTRL_TEXTBOX` (read-only): `{mm:ss} / {mm:ss}`, updated each frame
  - [ ] Volume slider: `CTRL_SCROLLBAR_HORIZ` (range 0–100); change → `audio_mixer_set_stream_volume(handle, val × 255 / 100)`
  - [ ] 🔊 mute toggle button
- [ ] **Fullscreen** (F11 or double-click canvas):
  - [ ] `wm_set_fullscreen(win, TRUE)` -- hides title bar
  - [ ] Controls bar hidden; auto-show on mouse move; auto-hide after 3 s of no mouse activity (`hide_timer_ms`)
  - [ ] Cursor hidden after 3 s in fullscreen; restored on mouse move
  - [ ] F11 or Escape exits fullscreen
- [ ] **OSD (On-Screen Display)**: brief semi-transparent overlay in top-left showing action text (`"▶ Playing"`, `"⏸ Paused"`, `"⏩ +10s"`) for 1.5 s after each control action

---

## 5. File Operations `[Sonnet]`

**Source file:** `src/apps/player/player_files.c`

- [ ] **CLI**: `player.exe C:\path\video.mpg` -- open and begin playback immediately
- [ ] **Without args**: show open-file dialog (`dialog_file_open()`) filtered to `*.mpg;*.mpeg;*.avi`
- [ ] **File→Open**: `CTRL_TEXTBOX`-based open dialog → load and play
- [ ] **Drag file onto window**: `WM_DROPFILES` message → extract path → `vp_destroy` current + `vp_create` + `vp_play`
- [ ] **Recent files**: `HKCU\Software\Impossible\Player\RecentFiles\0..7` (8 entries, MRU order, path strings); `File → Recent Files` submenu
- [ ] **File associations** (registered at app init):
  - [ ] `file_assoc_set(".mpg",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")`
  - [ ] `file_assoc_set(".mpeg", "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")`
  - [ ] `file_assoc_set(".avi",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")` (stub; plays if container has MPEG-1 video track)
  - [ ] `file_assoc_set(".mp4",  "ImpossibleOS.VideoPlayer", "C:\\Impossible\\System32\\player.exe")` (stub; "Format not yet supported" message until H.264 phase)

---

## 6. Playlist `[Sonnet]`

**Source file:** `src/apps/player/playlist.c`

- [ ] **Playlist sidebar** (toggle with `P` key or `View → Playlist`): 220 px wide panel on right side; `CTRL_LISTVIEW` showing filenames (no path); double-click → jump to that item; highlighted = currently playing
- [ ] **File→Open Multiple**: open-file dialog allowing multi-select; add all selected files to playlist
- [ ] **Add / Remove**: right-click context menu on playlist item: `Remove`, `Move Up`, `Move Down`; drag-to-reorder (stretch)
- [ ] **Next / Previous**: `[⏭]`/ `[⏮]` toolbar buttons advance/rewind playlist; also `Media → Next Track` / `Prev Track` menu items; end of file → auto-advance to next playlist item
- [ ] **Loop modes** (cycle with `L` key):
  - `NO_LOOP` -- stop at end of playlist
  - `LOOP_TRACK` -- replay current item
  - `LOOP_PLAYLIST` -- wrap around to first item after last
  - Loop icon in controls bar changes per mode: `➡` / `🔁` / `🔂`
- [ ] **Shuffle** (`Z` key toggle): randomise playback order (Fisher-Yates shuffle of index array); re-shuffle on full cycle
- [ ] **Persist**: on exit, save playlist to `HKCU\Software\Impossible\Player\Playlist\{n}` (up to 256 entries); restore on next launch; clear on `File → Clear Playlist`

---

## 7. Subtitle Support (Stretch) `[Sonnet]`

**Source file:** `src/apps/player/subtitles.c`

- [ ] **Auto-detect**: on file open, check for `{video_basename}.srt` in same directory via `vfs_stat(srt_path, &st)` → auto-load if found; `Subtitles → Load…` menu for manual selection
- [ ] **SRT parser** (`srt_load(path, &subs[], &count)`):
  - [ ] Parse blocks: sequence number, timestamp line `HH:MM:SS,mmm --> HH:MM:SS,mmm`, text lines until blank line
  - [ ] Store `struct subtitle { start_ms, end_ms, text[256] }[MAX_SUBS=2048]`
- [ ] **Display**: each frame, call `srt_get_active(current_ms)` → returns text or NULL; if text: render with `ttf_draw_string(canvas, font, x_center, canvas_h - 60, text, GFX_COLOR_WHITE)` + 1 px black shadow (`ttf_draw_string` offset by 1 px in each direction with black)
- [ ] **Toggle**: `S` key or `Subtitles → {filename} / Off` toggle
- [ ] **Encoding**: assume UTF-8; pass directly to `ttf_draw_string` (already UTF-8 capable)

---

## 8. Format Support Roadmap `[Sonnet]`

> Planning section -- no new code; defines the codec extension path.

- [ ] **Phase 1 (this TODO):** MPEG-1 video + MPEG audio Layers 1/2 via `pl_mpeg` → `.mpg`, `.mpeg` playback
- [ ] **Phase 2 (stretch):** H.264 / AVC baseline via **h264bsd** (BSD, ~25K lines of C89):
  - [ ] Port h264bsd to freestanding (`malloc`/`free` → `pmm_alloc_contiguous`; no stdio)
  - [ ] `H264SwDecInit`, `H264SwDecDecode`, `H264SwDecNextPicture` → replace `plm_decode_video` call
  - [ ] Output is already YCbCr420 → existing `yuv420_to_rgb` works unchanged
  - [ ] Enables `.mp4` (H.264 + AAC in MP4 container) playback
- [ ] **Phase 3 (long-term):** AVI / MKV container parser:
  - [ ] AVI: RIFF chunk walk (`LIST movi` → `00dc` video chunks, `01wb` audio chunks); feed to §1 or Phase 2 decoder
  - [ ] MKV: EBML element parser → Segment → Tracks → Cluster → Block; demux video/audio to decoder
  - [ ] Enables `.avi` (legacy) and `.mkv` (modern) containers with MPEG-1 or H.264 video streams
- [ ] **Phase 4 (very long-term):** Software audio decode: AAC (FDK-AAC or faaad2 port) for MP4 audio; Vorbis (stb_vorbis, already in build flags) for MKV/OGG audio

---

## OS Comparison


| ⭐  | Feature                                  | 🪟 Win11                    | 🐧 Linux              | 🚀 Impossible OS                                     |
| --- | ---------------------------------------- | --------------------------- | --------------------- | ---------------------------------------------------- |
| 💎  | MPEG-1 video decode                      | ✅ Windows Media Player     | ✅ VLC / mpv          | ⬜ §1 -- pl_mpeg single-header port                  |
| 💎  | YCbCr → RGB                              | ✅ WMP (GPU)                | ✅ VLC (libyuv)       | ⬜ §2 -- SSE2 4-pixel-at-a-time                      |
| 💎  | A/V synchronisation + drift correction   | ✅ WMP                      | ✅ mpv (audio-driven) | ⬜ §3 -- 3-frame ring buffer, ±200 ms                |
| 💎  | Seek bar + time display + volume         | ✅ WMP                      | ✅ VLC                | ⬜ §4 -- CTRL_SCROLLBAR seek, OSD                    |
| ⭐  | Fullscreen with 3 s auto-hiding controls | ✅ WMP / films app          | ✅ VLC / mpv          | ⬜ §4 -- `wm_set_fullscreen`, hide_timer_ms          |
| 💎  | Playlist with loop + shuffle             | ✅ WMP                      | ✅ VLC                | ⬜ §6 -- CTRL_LISTVIEW sidebar, Fisher-Yates shuffle |
| ⭐  | SRT subtitle overlay with TTF text       | ✅ WMP (limited) / films ✅ | ✅ VLC (built-in)     | ⬜ §7 -- (Stretch) -- ; `ttf_draw_string` +          |
| 💎  | File associations                        | ✅ WMP default              | ✅ `xdg-open`         | ⬜ §5 -- `file_assoc_set`                            |
| 💎  | H.264 / MP4 support                      | ✅ WMP / HEVC codec         | ✅ VLC / mpv          | ⬜ §8 -- (Phase 2) -- ; h264bsd                      |

Impossible OS ships a fully native, zero-dependency video player backed by a public-domain
single-header codec -- no COM, no DirectShow, no GStreamer pipeline. The SSE2 YCbCr converter
and 3-frame ring buffer with drift correction give smooth playback on commodity hardware from
day one.

---

## Verification

Run `bash scripts/build.sh run` for each verification step.

- [ ] **pl_mpeg port:** `vp_create(fd)` on `test.mpg`; serial log shows width/height/fps/duration; no allocation panics
- [ ] **YCbCr scalar:** call `yuv420_to_rgb` on first frame; dump pixel `[0,0]` -- must be within ±3 of reference value; no visible colour-channel swap
- [ ] **YCbCr SSE2:** pixel values identical to scalar path for same frame; serial log shows `yuv_convert: {w}×{h} in < {frame_period_ms} ms`
- [ ] **A/V sync:** play 30 s of `test.mpg` with audio; no visible audio/video drift; serial log shows drift < 50 ms throughout
- [ ] **Seek:** drag seek bar to 50%; playback resumes from correct position; audio restarts in sync
- [ ] **Pause/Resume:** `[⏯]` pauses; `[⏯]` again resumes; `pause_offset_ms` accounts for paused duration (no jump on resume)
- [ ] **Controls UI:** seek bar advances in real-time; time display updates each second; volume slider adjusts audio level
- [ ] **Fullscreen:** F11 → title bar hidden; canvas fills display; controls hide after 3 s; mouse move → controls reappear
- [ ] **Playlist:** add 3 files; play first; auto-advances to second at end; loop-playlist wraps back to first
- [ ] **File assoc:** `.mpg` double-click in File Manager → `player.exe` opens and begins playback
- [ ] **Subtitles (stretch):** place `test.srt` next to `test.mpg`; open video → subtitles auto-load; correct text overlaid at correct timestamps; `S` toggles off/on
- [ ] Commit: `"apps: video player -- pl_mpeg, YCbCr/SSE2, A/V sync, UI, playlist, subtitles"`
